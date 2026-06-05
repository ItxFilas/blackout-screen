# Output lifecycle + per-surface state + tests — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the blackout daemon survive monitor unplug (P1), own all per-output resources per-surface so nothing dangles (P2), and add a hermetic unit-test layer plus an opt-in real-KWin smoke script.

**Architecture:** Extract output bookkeeping into a tiny pure module (`outputs.c`/`outputs.h`) that tracks outputs by their `wl_registry` name and is unit-testable with no compositor. `blackout.c` uses that module, implements `reg_global_remove` to drop a removed output and its surface, moves `locked_pointer` from a file-global into `struct surface`, and routes all teardown through the existing `destroy_surface()` helper. Semantics are **snapshot-at-toggle** (outputs enumerated on show; removal handled safely; additions appear on next toggle).

**Tech Stack:** C11, libwayland-client (wlr-layer-shell, pointer-constraints, keyboard-shortcuts-inhibit), GNU make, wayland-scanner. Tests: plain C asserts (Layer 1) + bash with `spectacle`/`identify` (Layer 2).

**Spec:** `docs/superpowers/specs/2026-06-05-output-lifecycle-and-tests-design.md`

**Working directory:** `src/blackout-overlay-c/` (all paths below are relative to it unless noted).

**Per-task verification note:** This daemon talks to a live compositor; most glue can't be unit-tested. After each code task: `make`, `install -m755 blackout-overlay ~/.local/bin/blackout-overlay`, `systemctl --user restart blackout-overlay`, then the signal sanity check (shown → `0,0,0`, hidden → non-zero). Real monitor-removal is verified manually in Task 5's checklist (single-monitor dev box can't script it).

---

### Task 1: Pure `outputs` module + Layer 1 unit tests (TDD)

**Files:**
- Create: `src/blackout-overlay-c/outputs.h`
- Create: `src/blackout-overlay-c/outputs.c`
- Create: `src/blackout-overlay-c/test_outputs.c`
- Modify: `src/blackout-overlay-c/Makefile`

- [ ] **Step 1: Write the header**

Create `outputs.h`:

```c
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
```

- [ ] **Step 2: Write the failing test**

Create `test_outputs.c`:

```c
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
```

- [ ] **Step 3: Add the `test` target to the Makefile**

In `Makefile`, after the `install:` rule and before `clean:`, add:

```makefile
# --- Layer 1 unit tests (pure logic; no compositor / display needed) ---
TEST_BIN = test-outputs
test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): test_outputs.c outputs.c outputs.h
	$(CC) $(CFLAGS) -o $@ test_outputs.c outputs.c $(LIBS)
```

And extend the existing `clean:` rule to also remove the test binary — change:

```makefile
	rm -f blackout-overlay $(GEN_H) $(GEN_C) $(GEN_H2) $(GEN_C2) $(GEN_H3) $(GEN_C3)
```

to:

```makefile
	rm -f blackout-overlay $(TEST_BIN) $(GEN_H) $(GEN_C) $(GEN_H2) $(GEN_C2) $(GEN_H3) $(GEN_C3)
```

- [ ] **Step 4: Run the test to verify it fails**

Run: `make test`
Expected: link failure — `undefined reference to 'output_track'` (and `output_find`, `output_drop`), because `outputs.c` doesn't exist yet.

- [ ] **Step 5: Write the implementation**

Create `outputs.c`:

```c
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
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `make test`
Expected: builds, runs, prints `test_outputs: all assertions passed`, exit 0.

- [ ] **Step 7: Commit**

```bash
git add src/blackout-overlay-c/outputs.h src/blackout-overlay-c/outputs.c \
        src/blackout-overlay-c/test_outputs.c src/blackout-overlay-c/Makefile
git commit -m "test: add pure outputs module with unit tests (Layer 1)"
```

---

### Task 2: Use the `outputs` module in the daemon

No behavior change yet — just replace the inline `struct output` and start capturing the registry name. This is the seam the P1 fix needs.

**Files:**
- Modify: `src/blackout-overlay-c/blackout.c`
- Modify: `src/blackout-overlay-c/Makefile`

- [ ] **Step 1: Include the module and delete the inline struct**

In `blackout.c`, add the include after the existing protocol headers (right after the `keyboard-shortcuts-inhibit` include near the top):

```c
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "outputs.h"
```

Then delete the now-duplicate definition in the `/* ---- data structures ---- */` block (it lives in `outputs.h` now):

```c
struct output {
    struct wl_output *wl_output;
    struct wl_list    link;
};
```

(Leave `struct surface { ... }` exactly as it is.)

- [ ] **Step 2: Track outputs by registry name in `reg_global`**

In `reg_global`, replace the `wl_output` branch:

```c
    } else if (!strcmp(iface, wl_output_interface.name)) {
        struct output *o = calloc(1, sizeof(*o));
        o->wl_output = wl_registry_bind(reg, name, &wl_output_interface, 3);
        wl_list_insert(&outputs, &o->link);
    }
```

with:

```c
    } else if (!strcmp(iface, wl_output_interface.name)) {
        struct wl_output *wo = wl_registry_bind(reg, name, &wl_output_interface, 3);
        output_track(&outputs, wo, name);
    }
```

- [ ] **Step 3: Link `outputs.c` into the daemon**

In `Makefile`, change the build rule from:

```makefile
blackout-overlay: blackout.c $(GEN_H) $(GEN_C) $(GEN_H2) $(GEN_C2) $(GEN_H3) $(GEN_C3)
	$(CC) $(CFLAGS) -o $@ blackout.c $(GEN_C) $(GEN_C2) $(GEN_C3) $(LIBS)
```

to:

```makefile
blackout-overlay: blackout.c outputs.c outputs.h $(GEN_H) $(GEN_C) $(GEN_H2) $(GEN_C2) $(GEN_H3) $(GEN_C3)
	$(CC) $(CFLAGS) -o $@ blackout.c outputs.c $(GEN_C) $(GEN_C2) $(GEN_C3) $(LIBS)
```

- [ ] **Step 4: Build, deploy, sanity-check**

Run:
```bash
make && make test \
  && install -m755 blackout-overlay ~/.local/bin/blackout-overlay \
  && systemctl --user restart blackout-overlay
PID=$(systemctl --user show -p MainPID --value blackout-overlay)
kill -USR1 $PID; sleep 0.4; spectacle -b -n -f -o /tmp/t2on.png 2>/dev/null
identify -format '%[fx:mean.r],%[fx:mean.g],%[fx:mean.b]\n' /tmp/t2on.png
kill -USR2 $PID; sleep 0.4; spectacle -b -n -f -o /tmp/t2off.png 2>/dev/null
identify -format '%[fx:mean.r],%[fx:mean.g],%[fx:mean.b]\n' /tmp/t2off.png
```
Expected: clean build, `test_outputs` passes, shown screenshot `0,0,0`, hidden screenshot non-zero, no errors in `journalctl --user -u blackout-overlay -n 5`.

- [ ] **Step 5: Commit**

```bash
git add src/blackout-overlay-c/blackout.c src/blackout-overlay-c/Makefile
git commit -m "refactor: track outputs via the outputs module (capture registry name)"
```

---

### Task 3: Handle output removal — fix the unplug crash (P1)

**Files:**
- Modify: `src/blackout-overlay-c/blackout.c`

- [ ] **Step 1: Give each surface a back-pointer to its output**

In `blackout.c`, add an `output` field to `struct surface`:

```c
struct surface {
    struct wl_surface             *wl_surface;
    struct zwlr_layer_surface_v1  *layer_surface;
    struct wl_buffer              *buffer;
    struct zwp_keyboard_shortcuts_inhibitor_v1 *ksi;  /* suppresses KWin global shortcuts while focused */
    struct output                 *output;            /* which output this covers */
    struct wl_list                 link;
};
```

- [ ] **Step 2: Record the output in `show()`**

In `show()`, inside the `wl_list_for_each(out, &outputs, link)` loop, set the back-pointer right after the surface is allocated. Change:

```c
        struct surface *s = calloc(1, sizeof(*s));
        s->wl_surface    = wl_compositor_create_surface(compositor);
```

to:

```c
        struct surface *s = calloc(1, sizeof(*s));
        s->output        = out;
        s->wl_surface    = wl_compositor_create_surface(compositor);
```

- [ ] **Step 3: Implement `reg_global_remove`**

Replace the empty handler:

```c
static void reg_global_remove(void *data, struct wl_registry *reg, uint32_t name) {}
```

with:

```c
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
    if (wl_list_empty(&surfaces))
        showing = false;
}
```

- [ ] **Step 4: Reset `showing` when the compositor closes the last surface**

Update `lsurf_closed` (compositor-initiated close path) to keep `showing` consistent. Change:

```c
static void lsurf_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    destroy_surface(data);
}
```

to:

```c
static void lsurf_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    destroy_surface(data);
    if (wl_list_empty(&surfaces))
        showing = false;
}
```

- [ ] **Step 5: Build, deploy, sanity-check**

Run:
```bash
make && make test \
  && install -m755 blackout-overlay ~/.local/bin/blackout-overlay \
  && systemctl --user restart blackout-overlay
PID=$(systemctl --user show -p MainPID --value blackout-overlay)
kill -USR1 $PID; sleep 0.4; spectacle -b -n -f -o /tmp/t3on.png 2>/dev/null
identify -format '%[fx:mean.r],%[fx:mean.g],%[fx:mean.b]\n' /tmp/t3on.png   # expect 0,0,0
kill -USR2 $PID; sleep 0.4
```
Expected: clean build, unit tests pass, normal toggle still blacks/restores the screen, no journal errors. (Actual unplug behavior is verified manually in Task 5 — it can't be scripted on a single-monitor box.)

- [ ] **Step 6: Commit**

```bash
git add src/blackout-overlay-c/blackout.c
git commit -m "fix: handle output removal so monitor unplug can't crash show() (P1)"
```

---

### Task 4: Move `locked_pointer` into the surface (P2)

**Files:**
- Modify: `src/blackout-overlay-c/blackout.c`

- [ ] **Step 1: Remove the global, add a per-surface field**

In `blackout.c`, delete the file-global:

```c
static struct zwp_locked_pointer_v1      *locked_pointer;  /* non-NULL while pointer is frozen */
```

Add the field to `struct surface` (alongside `output` from Task 3):

```c
    struct output                 *output;            /* which output this covers */
    struct zwp_locked_pointer_v1  *locked_pointer;    /* non-NULL while this surface's pointer is frozen */
    struct wl_list                 link;
```

- [ ] **Step 2: Destroy the per-surface lock in `destroy_surface`**

Update the teardown helper. Change:

```c
static void destroy_surface(struct surface *s) {
    if (s->ksi)    zwp_keyboard_shortcuts_inhibitor_v1_destroy(s->ksi);
    if (s->buffer) wl_buffer_destroy(s->buffer);
    zwlr_layer_surface_v1_destroy(s->layer_surface);
    wl_surface_destroy(s->wl_surface);
    wl_list_remove(&s->link);
    free(s);
}
```

to:

```c
static void destroy_surface(struct surface *s) {
    if (s->locked_pointer) zwp_locked_pointer_v1_destroy(s->locked_pointer);
    if (s->ksi)            zwp_keyboard_shortcuts_inhibitor_v1_destroy(s->ksi);
    if (s->buffer)         wl_buffer_destroy(s->buffer);
    zwlr_layer_surface_v1_destroy(s->layer_surface);
    wl_surface_destroy(s->wl_surface);
    wl_list_remove(&s->link);
    free(s);
}
```

- [ ] **Step 3: Drop the global-lock teardown from `hide()`**

`destroy_surface()` now owns the locked-pointer teardown, so remove the global block from `hide()`. Change:

```c
static void hide(void) {
    if (!showing) return;
    showing = false;
    if (locked_pointer) {
        zwp_locked_pointer_v1_destroy(locked_pointer);
        locked_pointer = NULL;
    }
    struct surface *s, *tmp;
    wl_list_for_each_safe(s, tmp, &surfaces, link)
        destroy_surface(s);
    wl_display_flush(display);
}
```

to:

```c
static void hide(void) {
    if (!showing) return;
    showing = false;
    struct surface *s, *tmp;
    wl_list_for_each_safe(s, tmp, &surfaces, link)
        destroy_surface(s);
    wl_display_flush(display);
}
```

- [ ] **Step 4: Add a `wl_surface` → `struct surface` lookup**

Immediately above `ptr_enter` (after the `lp_listener` definition), add:

```c
/* Map a compositor wl_surface back to our surface struct (small N: one per
   output). Returns NULL if it isn't one of ours. */
static struct surface *surface_from_wl(struct wl_surface *wl_surface) {
    struct surface *s;
    wl_list_for_each(s, &surfaces, link)
        if (s->wl_surface == wl_surface) return s;
    return NULL;
}
```

- [ ] **Step 5: Lock the entered surface's own pointer in `ptr_enter`**

Replace the global-singleton lock block at the end of `ptr_enter`. Change:

```c
    if (showing && pointer_constraints && !locked_pointer) {
        locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
            pointer_constraints, surface, wl_pointer, NULL,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_locked_pointer_v1_add_listener(locked_pointer, &lp_listener, NULL);
    }
```

to:

```c
    struct surface *s = surface_from_wl(surface);
    if (showing && pointer_constraints && s && !s->locked_pointer) {
        s->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
            pointer_constraints, surface, wl_pointer, NULL,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_locked_pointer_v1_add_listener(s->locked_pointer, &lp_listener, NULL);
    }
```

- [ ] **Step 6: Build, deploy, sanity-check**

Run:
```bash
make && make test \
  && install -m755 blackout-overlay ~/.local/bin/blackout-overlay \
  && systemctl --user restart blackout-overlay
PID=$(systemctl --user show -p MainPID --value blackout-overlay)
kill -USR1 $PID; sleep 0.4; spectacle -b -n -f -o /tmp/t4on.png 2>/dev/null
identify -format '%[fx:mean.r],%[fx:mean.g],%[fx:mean.b]\n' /tmp/t4on.png   # expect 0,0,0
kill -USR2 $PID; sleep 0.4
```
Then manually: toggle ON, shove the mouse toward a screen edge — cursor stays hidden, no edge glow; toggle OFF. No journal errors.

- [ ] **Step 7: Commit**

```bash
git add src/blackout-overlay-c/blackout.c
git commit -m "fix: own locked_pointer per-surface so it can't dangle on teardown (P2)"
```

---

### Task 5: Layer 2 real-KWin smoke script

**Files:**
- Create: `src/blackout-overlay-c/integration-test.sh`
- Modify: `src/blackout-overlay-c/Makefile`

- [ ] **Step 1: Write the smoke script**

Create `integration-test.sh`:

```bash
#!/usr/bin/env bash
# Layer 2 smoke test — run INSIDE a live KWin/Wayland session.
# Drives the running blackout-overlay daemon and checks the screen really goes
# black, then prints a checklist for human-only confirmations.
# NOT for CI: needs a graphical session and briefly flashes the screen black.
set -euo pipefail

PIDFILE="/run/user/$(id -u)/blackout-overlay.pid"
[ -r "$PIDFILE" ] || { echo "daemon not running (no $PIDFILE)"; exit 1; }
PID=$(cat "$PIDFILE")

shot() { spectacle -b -n -f -o "$1" >/dev/null 2>&1; }
mean() { identify -format '%[fx:mean.r],%[fx:mean.g],%[fx:mean.b]' "$1"; }

echo "== toggle ON =="
kill -USR1 "$PID"; sleep 0.4
shot /tmp/bo-on.png;  on=$(mean /tmp/bo-on.png)
echo "screen mean while shown: $on (expect 0,0,0)"

echo "== toggle OFF =="
kill -USR2 "$PID"; sleep 0.4
shot /tmp/bo-off.png; off=$(mean /tmp/bo-off.png)
echo "screen mean after hide: $off (expect non-zero)"

[ "$on"  = "0,0,0" ] || { echo "FAIL: screen was not black while shown"; exit 1; }
[ "$off" != "0,0,0" ] || { echo "FAIL: screen still black after hide"; exit 1; }
echo "PASS: automated pixel checks"

cat <<'CHECKLIST'

== Manual checklist (only a human can confirm) ==
  [ ] Blackout ON, press your toggle key            -> turns OFF
  [ ] Blackout ON, Alt+Tab                           -> does NOT switch windows
  [ ] Blackout ON, shove mouse to a screen edge      -> no edge glow
  [ ] Plug an external monitor, toggle ON            -> both screens black
  [ ] Blackout OFF, unplug a monitor, toggle a few x -> no crash
       (verify: systemctl --user is-active blackout-overlay)
CHECKLIST
```

- [ ] **Step 2: Make it executable and add a Makefile target**

Run: `chmod +x integration-test.sh`

In `Makefile`, after the `test:` target block, add:

```makefile
# --- Layer 2 smoke (run inside a live KWin session; flashes the screen) ---
smoke:
	./integration-test.sh
```

- [ ] **Step 3: Run it against the live daemon**

Run: `make smoke`
Expected: prints `PASS: automated pixel checks` then the manual checklist. Walk the checklist by hand once.

- [ ] **Step 4: Commit**

```bash
git add src/blackout-overlay-c/integration-test.sh src/blackout-overlay-c/Makefile
git commit -m "test: add opt-in real-KWin smoke script + checklist (Layer 2)"
```

---

### Task 6: Update project memory

Not a code change — keep the saved daemon notes accurate for the next session.

- [ ] **Step 1: Update the memory file**

Edit `/home/filas/.claude/projects/-home-filas/memory/blackout-screen-state.md`:
- Under "Known issues found in review (unfixed)", remove the `reg_global_remove` no-op line (now fixed) and note the single-output `locked_pointer` assumption is resolved (per-surface now).
- In the Architecture section, note `outputs.c`/`outputs.h` (output bookkeeping, unit-tested) and that `locked_pointer` is per-`struct surface`.
- Add to the deploy/test loop: `make test` (Layer 1 unit tests) and `make smoke` (Layer 2, live KWin).

- [ ] **Step 2: (no commit needed — memory lives outside the repo)**

---

## Self-Review

**Spec coverage:**
- P1 output-unplug crash → Task 3 (`reg_global_remove`, name tracking via Task 1/2, surface back-pointer). ✓
- P2 per-output state → Task 4 (`locked_pointer` per-surface) + Task 3 (`showing` reset in `lsurf_closed`/`reg_global_remove`). ✓
- Snapshot-at-toggle semantics → preserved; `show()` enumerates outputs, additions need a re-toggle; no live-add code added. ✓
- Layer 1 hermetic tests → Task 1 (`outputs` module + `test_outputs.c` + `make test`). ✓
- Layer 2 real-KWin smoke → Task 5. ✓
- Non-goals respected: `show()` roundtrip untouched, `wl_compositor` v4 untouched, keycode untouched, empty listeners kept. ✓

**Placeholder scan:** No TBD/TODO; every code step shows full code; every command has an expected result. ✓

**Type consistency:** `struct output { wl_output, name, link }` defined in Task 1, used unchanged in Tasks 2–3. `output_track/output_find/output_drop` signatures match between `outputs.h`, `outputs.c`, `test_outputs.c`, and the `reg_global`/`reg_global_remove` call sites. `struct surface` gains `output` (Task 3) then `locked_pointer` (Task 4); both referenced consistently in `destroy_surface`, `show`, `ptr_enter`. `surface_from_wl` defined before its only caller `ptr_enter`. ✓

**Ordering note:** Task 3 adds `struct surface.output`; Task 4 adds `struct surface.locked_pointer` next to it — apply in order so the struct edits don't collide.
