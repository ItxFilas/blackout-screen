#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "outputs.h"

/* evdev keycode of the toggle key: KEY_PROG1 (the Asus WMI hotkey that KDE
   registers as "Launch (1)"/Launch1, physically Fn+Esc). wl_keyboard.key
   delivers raw evdev codes. While the overlay is up we inhibit KWin's global
   shortcuts so Alt+Tab et al. can't escape — but that also inhibits this key,
   so KWin delivers it to us instead and we catch it here to turn off. */
#define KEY_TOGGLE 148

#define ANCHOR_ALL ( \
    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP    | \
    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | \
    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   | \
    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)

/* ---- sleep-inhibition via org.freedesktop.PowerManagement.Inhibit ----
   On show: if no inhibitor is already registered, call Inhibit and remember the
   cookie.  On hide: release the cookie only if we created it — so a user's
   manual "Block Sleep" toggle in the KDE applet is left exactly as found. */
static bool     inhibit_by_us  = false;
static uint32_t inhibit_cookie = 0;

static bool sleep_inhibit_active(void) {
    FILE *f = popen("busctl --user call org.freedesktop.PowerManagement"
                    " /org/freedesktop/PowerManagement/Inhibit"
                    " org.freedesktop.PowerManagement.Inhibit HasInhibit 2>/dev/null", "r");
    if (!f) return false;
    char buf[64]; bool result = false;
    if (fgets(buf, sizeof(buf), f) && strstr(buf, "true"))
        result = true;
    pclose(f);
    return result;
}

static void inhibit_sleep(void) {
    if (sleep_inhibit_active()) { inhibit_by_us = false; return; }
    FILE *f = popen("busctl --user call org.freedesktop.PowerManagement"
                    " /org/freedesktop/PowerManagement/Inhibit"
                    " org.freedesktop.PowerManagement.Inhibit Inhibit"
                    " ss blackout-overlay 'Screen blacked out' 2>/dev/null", "r");
    if (!f) { inhibit_by_us = false; return; }
    char buf[64]; unsigned int v = 0;
    if (fgets(buf, sizeof(buf), f)) sscanf(buf, "u %u", &v);
    pclose(f);
    if (v) { inhibit_cookie = v; inhibit_by_us = true; }
    else     inhibit_by_us = false;
}

static void uninhibit_sleep(void) {
    if (!inhibit_by_us) return;
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "busctl --user call org.freedesktop.PowerManagement"
        " /org/freedesktop/PowerManagement/Inhibit"
        " org.freedesktop.PowerManagement.Inhibit UnInhibit u %u 2>/dev/null",
        inhibit_cookie);
    system(cmd);
    inhibit_by_us = false; inhibit_cookie = 0;
}

/* ---- globals ---- */
static struct wl_display        *display;
static struct wl_compositor     *compositor;
static struct wl_shm            *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_seat           *seat;
static struct wl_pointer        *pointer;
static struct wl_keyboard       *keyboard;
static struct wl_surface        *cursor_surface;  /* 1x1 transparent, hides pointer */
static struct wl_buffer         *cursor_buffer;
static struct zwp_pointer_constraints_v1 *pointer_constraints;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1 *ksi_manager;
static struct wl_list outputs;
static struct wl_list surfaces;
static bool showing = false;

/* ---- data structures ---- */
struct surface {
    struct wl_surface             *wl_surface;
    struct zwlr_layer_surface_v1  *layer_surface;
    struct wl_buffer              *buffer;
    struct zwp_keyboard_shortcuts_inhibitor_v1 *ksi;  /* suppresses KWin global shortcuts while focused */
    struct output                 *output;            /* which output this covers */
    struct zwp_locked_pointer_v1  *locked_pointer;    /* non-NULL while this surface's pointer is frozen */
    struct wl_list                 link;
};

/* ---- SHM black buffer ---- */
static struct wl_buffer *make_black_buffer(int w, int h) {
    int stride = w * 4, size = stride * h;
    char path[] = "/tmp/blackout-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    unlink(path);
    /* ftruncate zero-fills the new bytes (POSIX), and black XRGB8888 is all
       zero — so the freshly sized fd is already a black buffer; no mmap/memset
       needed. */
    if (ftruncate(fd, size) < 0) { close(fd); return NULL; }
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

/* Tear down one overlay surface and unlink+free it. Shared by hide() (we drop
   the overlay) and lsurf_closed() (the compositor dropped it). */
static void destroy_surface(struct surface *s) {
    if (s->locked_pointer) zwp_locked_pointer_v1_destroy(s->locked_pointer);
    if (s->ksi)            zwp_keyboard_shortcuts_inhibitor_v1_destroy(s->ksi);
    if (s->buffer)         wl_buffer_destroy(s->buffer);
    zwlr_layer_surface_v1_destroy(s->layer_surface);
    wl_surface_destroy(s->wl_surface);
    wl_list_remove(&s->link);
    free(s);
}

static void lsurf_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    destroy_surface(data);
    if (wl_list_empty(&surfaces)) {
        showing = false;
        uninhibit_sleep();
    }
}

/* inhibitor active/inactive are informational; while active KWin stops firing
   its global shortcuts (Alt+Tab, ...) and delivers those keys to us instead. */
static void ksi_active(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i) {}
static void ksi_inactive(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i) {}
static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener ksi_listener = {
    .active = ksi_active, .inactive = ksi_inactive,
};

static const struct zwlr_layer_surface_v1_listener lsurf_listener = {
    .configure = lsurf_configure,
    .closed    = lsurf_closed,
};

/* ---- show / hide ---- */
static void show(void) {
    if (showing) return;
    showing = true;
    inhibit_sleep();
    struct output *out;
    wl_list_for_each(out, &outputs, link) {
        struct surface *s = calloc(1, sizeof(*s));
        s->output        = out;
        s->wl_surface    = wl_compositor_create_surface(compositor);
        s->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            layer_shell, s->wl_surface, out->wl_output,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "blackout");
        zwlr_layer_surface_v1_set_size(s->layer_surface, 0, 0);
        zwlr_layer_surface_v1_set_anchor(s->layer_surface, ANCHOR_ALL);
        zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            s->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
        zwlr_layer_surface_v1_add_listener(s->layer_surface, &lsurf_listener, s);
        /* Seal the blackout: inhibit KWin's global shortcuts while this surface
           holds keyboard focus, so Alt+Tab et al. can't escape it. The toggle
           key is itself a global shortcut, so KWin then delivers it to us
           instead of firing it — kb_key() catches it to turn the overlay off. */
        if (ksi_manager && seat) {
            s->ksi = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
                ksi_manager, s->wl_surface, seat);
            zwp_keyboard_shortcuts_inhibitor_v1_add_listener(s->ksi, &ksi_listener, NULL);
        }
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
    wl_list_for_each_safe(s, tmp, &surfaces, link)
        destroy_surface(s);
    wl_display_flush(display);
    uninhibit_sleep();
}

/* ---- locked pointer: events are informational; we just need the freeze ---- */
static void lp_locked(void *d, struct zwp_locked_pointer_v1 *lp) {}
static void lp_unlocked(void *d, struct zwp_locked_pointer_v1 *lp) {}
static const struct zwp_locked_pointer_v1_listener lp_listener = {
    .locked   = lp_locked,
    .unlocked = lp_unlocked,
};

/* Map a compositor wl_surface back to our surface struct (small N: one per
   output). Returns NULL if it isn't one of ours. */
static struct surface *surface_from_wl(struct wl_surface *wl_surface) {
    struct surface *s;
    wl_list_for_each(s, &surfaces, link)
        if (s->wl_surface == wl_surface) return s;
    return NULL;
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
    /* Freeze the pointer for as long as the overlay is up: a locked pointer
       stops emitting motion, so the cursor cannot crawl to a screen edge and
       trigger KWin's edge-glow (Bug A). Created once; lives until hide(). */
    struct surface *s = surface_from_wl(surface);
    if (showing && pointer_constraints && s && !s->locked_pointer) {
        s->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
            pointer_constraints, surface, wl_pointer, NULL,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_locked_pointer_v1_add_listener(s->locked_pointer, &lp_listener, NULL);
    }
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

/* ---- keyboard ----
   We bind a wl_keyboard so that, while the overlay (EXCLUSIVE keyboard
   interactivity) is mapped, KWin has a focus target and routes the keyboard to
   us — apps below get nothing. Binding is what makes the grab actually swallow:
   with the interactivity flag set but no keyboard bound, KWin leaves focus on
   the window below and keys pass through. We swallow everything except the
   toggle key: because we inhibit shortcuts while shown, KWin delivers KEY_TOGGLE
   to us instead of firing the global shortcut, so we treat it as the off switch.
   The keymap fd is closed so we don't leak one. SIGUSR2 is the escape hatch. */
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size) { close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t st) {
    /* The toggle key reaches us (not KWin) because shortcuts are inhibited while
       shown. Act on press; the matching release lands after hide(), harmless. */
    if (st == WL_KEYBOARD_KEY_STATE_PRESSED && key == KEY_TOGGLE)
        hide();
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t md, uint32_t ml, uint32_t lk, uint32_t grp) {}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {}
static const struct wl_keyboard_listener kb_listener = {
    .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
    .key = kb_key, .modifiers = kb_mods, .repeat_info = kb_repeat,
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
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &kb_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard) {
        wl_keyboard_release(keyboard);
        keyboard = NULL;
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
    } else if (!strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name)) {
        ksi_manager = wl_registry_bind(
            reg, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        struct wl_output *wo = wl_registry_bind(reg, name, &wl_output_interface, 3);
        output_track(&outputs, wo, name);
    }
}

static void reg_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    /* Only outputs are tracked by name; ignore anything else going away. */
    struct output *o = output_find(&outputs, name);
    if (!o) return;
    /* Tear down the blackout surface covering this output, if we have one
       (only while shown), then drop the output so show() never touches a
       freed wl_output again. */
    struct surface *s, *tmp;
    wl_list_for_each_safe(s, tmp, &surfaces, link)
        if (s->output == o)
            destroy_surface(s);
    wl_output_destroy(o->wl_output);
    output_drop(o);
    /* If the compositor pulled our last surface, we are no longer showing. */
    if (wl_list_empty(&surfaces)) {
        showing = false;
        uninhibit_sleep();
    }
}

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
