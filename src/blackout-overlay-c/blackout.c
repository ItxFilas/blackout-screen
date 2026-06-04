#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"

#define ANCHOR_ALL ( \
    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP    | \
    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | \
    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   | \
    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)

/* ---- globals ---- */
static struct wl_display        *display;
static struct wl_compositor     *compositor;
static struct wl_shm            *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_seat           *seat;
static struct wl_pointer        *pointer;
static struct wl_surface        *cursor_surface;  /* 1x1 transparent, hides pointer */
static struct wl_buffer         *cursor_buffer;
static struct zwp_pointer_constraints_v1 *pointer_constraints;
static struct zwp_locked_pointer_v1      *locked_pointer;  /* non-NULL while pointer is frozen */
static struct wl_list outputs;
static struct wl_list surfaces;
static bool showing = false;

/* ---- data structures ---- */
struct output {
    struct wl_output *wl_output;
    struct wl_list    link;
};

struct surface {
    struct wl_surface             *wl_surface;
    struct zwlr_layer_surface_v1  *layer_surface;
    struct wl_buffer              *buffer;
    struct wl_list                 link;
};

/* ---- SHM black buffer ---- */
static struct wl_buffer *make_black_buffer(int w, int h) {
    int stride = w * 4, size = stride * h;
    char path[] = "/tmp/blackout-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    unlink(path);
    if (ftruncate(fd, size) < 0) { close(fd); return NULL; }
    void *data = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
    if (data != MAP_FAILED) { memset(data, 0, size); munmap(data, size); }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    close(fd);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    return buf;
}

/* 1x1 fully transparent buffer for the cursor surface. ftruncate zero-fills,
   so ARGB8888 is 0x00000000 = transparent. Used to hide the pointer: a plain
   NULL cursor gets overridden by KWin's "shake cursor" effect, but scaling a
   transparent pixel stays invisible. */
static struct wl_buffer *make_transparent_buffer(void) {
    char path[] = "/tmp/blackout-cur-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    unlink(path);
    if (ftruncate(fd, 4) < 0) { close(fd); return NULL; }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, 4);
    close(fd);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    return buf;
}

/* ---- layer surface events ---- */
static void lsurf_configure(void *data,
    struct zwlr_layer_surface_v1 *ls,
    uint32_t serial, uint32_t w, uint32_t h)
{
    struct surface *s = data;
    if (s->buffer) wl_buffer_destroy(s->buffer);
    s->buffer = make_black_buffer(w, h);
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    wl_surface_attach(s->wl_surface, s->buffer, 0, 0);
    wl_surface_damage_buffer(s->wl_surface, 0, 0, w, h);
    wl_surface_commit(s->wl_surface);
}

static void lsurf_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    struct surface *s = data;
    if (s->buffer)       wl_buffer_destroy(s->buffer);
    zwlr_layer_surface_v1_destroy(s->layer_surface);
    wl_surface_destroy(s->wl_surface);
    wl_list_remove(&s->link);
    free(s);
}

static const struct zwlr_layer_surface_v1_listener lsurf_listener = {
    .configure = lsurf_configure,
    .closed    = lsurf_closed,
};

/* ---- show / hide ---- */
static void show(void) {
    if (showing) return;
    showing = true;
    struct output *out;
    wl_list_for_each(out, &outputs, link) {
        struct surface *s = calloc(1, sizeof(*s));
        s->wl_surface    = wl_compositor_create_surface(compositor);
        s->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            layer_shell, s->wl_surface, out->wl_output,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "blackout");
        zwlr_layer_surface_v1_set_size(s->layer_surface, 0, 0);
        zwlr_layer_surface_v1_set_anchor(s->layer_surface, ANCHOR_ALL);
        zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            s->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        zwlr_layer_surface_v1_add_listener(s->layer_surface, &lsurf_listener, s);
        wl_surface_commit(s->wl_surface);   /* request the initial configure */
        wl_list_insert(&surfaces, &s->link);
    }
    /* Drive the configure handshake to completion now, synchronously, so the
       black buffer is attached and committed before we return — rather than a
       poll iteration later. This lets the compositor paint black on its very
       next repaint instead of the one after. */
    wl_display_roundtrip(display);
    wl_display_flush(display);
}

static void hide(void) {
    if (!showing) return;
    showing = false;
    struct surface *s, *tmp;
    wl_list_for_each_safe(s, tmp, &surfaces, link) {
        if (s->buffer) wl_buffer_destroy(s->buffer);
        zwlr_layer_surface_v1_destroy(s->layer_surface);
        wl_surface_destroy(s->wl_surface);
        wl_list_remove(&s->link);
        free(s);
    }
    wl_display_flush(display);
}

/* ---- pointer: hide the cursor while it is over the overlay ---- */
static void ptr_enter(void *data, struct wl_pointer *wl_pointer,
    uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
    /* Point the cursor at our 1x1 transparent surface for as long as it stays
       over the overlay (fullscreen = whole screen). Using a transparent buffer
       rather than a NULL cursor keeps it invisible even when KWin's shake
       effect scales the cursor up. */
    wl_pointer_set_cursor(wl_pointer, serial, cursor_surface, 0, 0);
    wl_surface_attach(cursor_surface, cursor_buffer, 0, 0);
    wl_surface_commit(cursor_surface);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
    struct wl_surface *s) {}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t,
    wl_fixed_t sx, wl_fixed_t sy) {}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
    uint32_t t, uint32_t button, uint32_t state) {}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t,
    uint32_t axis, wl_fixed_t value) {}

static const struct wl_pointer_listener ptr_listener = {
    .enter  = ptr_enter,
    .leave  = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis   = ptr_axis,
};

/* ---- seat ---- */
static void seat_caps(void *data, struct wl_seat *s, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &ptr_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
        wl_pointer_release(pointer);
        pointer = NULL;
    }
}
static void seat_name(void *data, struct wl_seat *s, const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name         = seat_name,
};

/* ---- registry ---- */
static void reg_global(void *data, struct wl_registry *reg,
    uint32_t name, const char *iface, uint32_t ver)
{
    if (!strcmp(iface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface,
            ver < 4 ? ver : 4);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        /* Cap at v4: enough for wl_pointer.release (v3) while staying below the
           v5 pointer-frame events we don't handle. */
        seat = wl_registry_bind(reg, name, &wl_seat_interface, ver < 4 ? ver : 4);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name)) {
        pointer_constraints = wl_registry_bind(
            reg, name, &zwp_pointer_constraints_v1_interface, 1);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        struct output *o = calloc(1, sizeof(*o));
        o->wl_output = wl_registry_bind(reg, name, &wl_output_interface, 3);
        wl_list_insert(&outputs, &o->link);
    }
}

static void reg_global_remove(void *data, struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener reg_listener = {
    .global        = reg_global,
    .global_remove = reg_global_remove,
};

/* ---- main ---- */
int main(void) {
    wl_list_init(&outputs);
    wl_list_init(&surfaces);

    display = wl_display_connect(NULL);
    if (!display) { fputs("Cannot connect to Wayland\n", stderr); return 1; }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &reg_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !layer_shell) {
        fputs("Missing required Wayland protocols\n", stderr);
        return 1;
    }

    /* transparent cursor surface, reused by ptr_enter to hide the pointer */
    cursor_surface = wl_compositor_create_surface(compositor);
    cursor_buffer  = make_transparent_buffer();

    /* write PID file */
    char pid_path[64];
    snprintf(pid_path, sizeof(pid_path), "/run/user/%d/blackout-overlay.pid", getuid());
    FILE *pf = fopen(pid_path, "w");
    if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }

    /* block signals and open signalfd */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

    struct pollfd pfds[2] = {
        {.fd = wl_display_get_fd(display), .events = POLLIN},
        {.fd = sfd,                        .events = POLLIN},
    };

    int running = 1;
    while (running) {
    retry:
        if (wl_display_prepare_read(display) < 0) {
            wl_display_dispatch_pending(display);
            goto retry;
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            break;
        }
        if (poll(pfds, 2, -1) < 0) {
            wl_display_cancel_read(display);
            break;
        }
        if (pfds[0].revents & POLLIN)
            wl_display_read_events(display);
        else
            wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);

        if (pfds[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof(si)) == sizeof(si)) {
                if      (si.ssi_signo == SIGUSR1) showing ? hide() : show();
                else if (si.ssi_signo == SIGUSR2) hide();
                else { running = 0; break; }
            }
        }
    }

    hide();
    unlink(pid_path);
    wl_display_disconnect(display);
    return 0;
}
