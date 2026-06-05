#ifndef BLACKOUT_OUTPUTS_H
#define BLACKOUT_OUTPUTS_H

#include <stdint.h>
#include <wayland-util.h>

struct wl_output;  /* opaque here; this module never dereferences the proxy */

/* One connected output we may paint a blackout surface on. Tracked by its
   wl_registry global name so reg_global_remove() can drop the right one when
   a monitor is unplugged. */
struct output {
    struct wl_output *wl_output;
    uint32_t          name;       /* wl_registry global name */
    struct wl_list    link;
};

/* Allocate an output, store its proxy + registry name, insert at the head of
   `list`. Returns NULL on allocation failure. Does not touch the proxy. */
struct output *output_track(struct wl_list *list, struct wl_output *wl_output,
                            uint32_t name);

/* Return the tracked output with this registry name, or NULL if none match. */
struct output *output_find(struct wl_list *list, uint32_t name);

/* Unlink `o` from its list and free it. The caller must already have destroyed
   o->wl_output (and any surface bound to it). */
void output_drop(struct output *o);

#endif /* BLACKOUT_OUTPUTS_H */
