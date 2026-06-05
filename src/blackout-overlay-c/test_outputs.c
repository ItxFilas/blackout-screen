/* Layer 1 unit tests for the pure output-tracking module. No compositor / no
   display connection: wl_list helpers come from libwayland-client, and the
   module never dereferences the wl_output proxy, so dummy pointers are safe. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <wayland-util.h>
#include "outputs.h"

#define WO(n) ((struct wl_output *)(uintptr_t)(n))  /* dummy, never dereferenced */

int main(void) {
    struct wl_list outputs;
    wl_list_init(&outputs);

    struct output *a = output_track(&outputs, WO(0xA), 10);
    struct output *b = output_track(&outputs, WO(0xB), 20);
    struct output *c = output_track(&outputs, WO(0xC), 30);
    assert(a && b && c);
    assert(wl_list_length(&outputs) == 3);

    /* find returns the right node by registry name */
    assert(output_find(&outputs, 10) == a);
    assert(output_find(&outputs, 20) == b);
    assert(output_find(&outputs, 30) == c);

    /* absent name -> NULL */
    assert(output_find(&outputs, 99) == NULL);

    /* drop removes exactly that node; the others stay findable */
    output_drop(b);
    assert(wl_list_length(&outputs) == 2);
    assert(output_find(&outputs, 20) == NULL);
    assert(output_find(&outputs, 10) == a);
    assert(output_find(&outputs, 30) == c);

    /* drop the rest -> list empties */
    output_drop(a);
    output_drop(c);
    assert(wl_list_empty(&outputs));

    printf("test_outputs: all assertions passed\n");
    return 0;
}
