#include <stdlib.h>
#include "outputs.h"

struct output *output_track(struct wl_list *list, struct wl_output *wl_output,
                            uint32_t name) {
    struct output *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->wl_output = wl_output;
    o->name = name;
    wl_list_insert(list, &o->link);
    return o;
}

struct output *output_find(struct wl_list *list, uint32_t name) {
    struct output *o;
    wl_list_for_each(o, list, link)
        if (o->name == name) return o;
    return NULL;
}

void output_drop(struct output *o) {
    wl_list_remove(&o->link);
    free(o);
}
