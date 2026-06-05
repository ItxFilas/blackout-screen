# Blackout Input-Lock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While the blackout overlay is up, grab the keyboard and lock (freeze) the pointer so the cursor can't crawl to the bottom screen edge — killing Bug A (KWin's blue edge-glow) and preventing stray input from reaching apps underneath.

**Architecture:** Extend the existing C/wlr-layer-shell daemon (`blackout.c`). Add the `zwp_pointer_constraints_v1` Wayland protocol: on `show()`, request `EXCLUSIVE` keyboard interactivity on the layer surface and, when the pointer enters the fullscreen overlay, create a persistent `lock_pointer` constraint that freezes pointer motion. On `hide()`, destroy the lock (the keyboard grab releases automatically when the surfaces are destroyed). No cursor centering — the pointer freezes in place. `SIGUSR2` (explicit off) remains the escape hatch.

**Tech Stack:** C, libwayland-client, wayland-scanner, wlr-layer-shell + pointer-constraints-unstable-v1 protocols, GNU make. Verification is manual (build → deploy → `spectacle` capture), matching this project's existing untested-daemon pattern — there is no automated test harness for the live daemon (the Python tests under `tests/` cover dead code and are out of scope).

**Working directory:** `/home/filas/Mods/blackout-screen` (canonical repo as of 2026-06-05; the old `~/projects/blackout-screen` copy was deleted). All daemon source lives in `src/blackout-overlay-c/`.

**Environment for verification:** KWin 6.6.5, Wayland, single output eDP-1 1920×1080 @ scale 1.25, OLED. The deploy loop must run in the user's graphical session (it talks to the live compositor).

---

## File Structure

- `src/blackout-overlay-c/Makefile` — add a second protocol (pointer-constraints) to the scanner/build rules.
- `src/blackout-overlay-c/blackout.c` — bind the constraints global, grab keyboard in `show()`, lock pointer in `ptr_enter`, unlock in `hide()`.
- Generated (git-ignored build artifacts, produced by `make`): `pointer-constraints-unstable-v1-client-protocol.{h,c}`.

---

## Task 1: Add the pointer-constraints protocol to the build

**Files:**
- Modify: `src/blackout-overlay-c/Makefile`

- [ ] **Step 1: Confirm the protocol XML and scanner are present**

Run:
```bash
pkg-config --variable=pkgdatadir wayland-protocols
ls "$(pkg-config --variable=pkgdatadir wayland-protocols)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml"
which wayland-scanner
```
Expected: prints a dir (e.g. `/usr/share/wayland-protocols`), the XML path exists, and `/usr/bin/wayland-scanner`. If the XML is missing, install `wayland-protocols` (`sudo dnf install wayland-protocols-devel`).

- [ ] **Step 2: Edit the Makefile**

Replace the entire contents of `src/blackout-overlay-c/Makefile` with:

```makefile
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags wayland-client)
LIBS    = $(shell pkg-config --libs wayland-client)
SCANNER = wayland-scanner

# wlr-layer-shell (vendored XML in this dir)
PROTO   = wlr-layer-shell.xml
GEN_H   = wlr-layer-shell-unstable-v1-client-protocol.h
GEN_C   = wlr-layer-shell-unstable-v1-client-protocol.c

# pointer-constraints (from the system wayland-protocols package)
WP_DIR  = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
PC_XML  = $(WP_DIR)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
GEN_H2  = pointer-constraints-unstable-v1-client-protocol.h
GEN_C2  = pointer-constraints-unstable-v1-client-protocol.c

all: blackout-overlay

$(GEN_H): $(PROTO)
	$(SCANNER) client-header $< $@

$(GEN_C): $(PROTO)
	$(SCANNER) private-code $< $@

$(GEN_H2):
	$(SCANNER) client-header $(PC_XML) $@

$(GEN_C2):
	$(SCANNER) private-code $(PC_XML) $@

blackout-overlay: blackout.c $(GEN_H) $(GEN_C) $(GEN_H2) $(GEN_C2)
	$(CC) $(CFLAGS) -o $@ blackout.c $(GEN_C) $(GEN_C2) $(LIBS)

install: blackout-overlay
	install -m755 blackout-overlay ~/.local/bin/blackout-overlay

clean:
	rm -f blackout-overlay $(GEN_H) $(GEN_C) $(GEN_H2) $(GEN_C2)
```

- [ ] **Step 3: Verify the generated header appears**

Run:
```bash
cd /home/filas/projects/blackout-screen/src/blackout-overlay-c
make clean && make GEN_H2 2>/dev/null; make pointer-constraints-unstable-v1-client-protocol.h
ls -l pointer-constraints-unstable-v1-client-protocol.h
grep -c 'zwp_pointer_constraints_v1' pointer-constraints-unstable-v1-client-protocol.h
```
Expected: the `.h` exists and the grep count is `> 0`. (The full `make` will fail until `blackout.c` is edited in later tasks — that is expected; this step only checks the scanner rule works.)

- [ ] **Step 4: Commit**

```bash
cd /home/filas/projects/blackout-screen
git add src/blackout-overlay-c/Makefile
git commit -m "build: add pointer-constraints-v1 protocol to overlay build"
```

---

## Task 2: Bind the pointer-constraints global

**Files:**
- Modify: `src/blackout-overlay-c/blackout.c` (includes, globals, `reg_global`)

- [ ] **Step 1: Add the protocol header include**

In `blackout.c`, immediately after the existing line:
```c
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
```
add:
```c
#include "pointer-constraints-unstable-v1-client-protocol.h"
```

- [ ] **Step 2: Add globals for the constraints manager and the active lock**

In the `/* ---- globals ---- */` block, after the line:
```c
static struct wl_buffer         *cursor_buffer;
```
add:
```c
static struct zwp_pointer_constraints_v1 *pointer_constraints;
static struct zwp_locked_pointer_v1      *locked_pointer;  /* non-NULL while pointer is frozen */
```

- [ ] **Step 3: Bind the global in `reg_global`**

In `reg_global`, after the `wl_seat` branch (the `else if (!strcmp(iface, wl_seat_interface.name))` block, which ends with `wl_seat_add_listener(...)`), and before the `wl_output` branch, insert a new branch:
```c
    } else if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name)) {
        pointer_constraints = wl_registry_bind(
            reg, name, &zwp_pointer_constraints_v1_interface, 1);
```

- [ ] **Step 4: Build to verify it compiles and links**

Run:
```bash
cd /home/filas/projects/blackout-screen/src/blackout-overlay-c
make clean && make
```
Expected: `blackout-overlay` builds with no errors or warnings. (The binding is unused so far, but `pointer_constraints` is referenced by the registry, so no unused-variable warning. If you get an "unused" warning, that's fine to ignore until Task 4 wires it in.)

- [ ] **Step 5: Commit**

```bash
cd /home/filas/projects/blackout-screen
git add src/blackout-overlay-c/blackout.c
git commit -m "feat: bind zwp_pointer_constraints_v1 global"
```

---

## Task 3: Grab the keyboard while blacked out

**Files:**
- Modify: `src/blackout-overlay-c/blackout.c:124-125` (the `set_keyboard_interactivity` call inside `show()`)

- [ ] **Step 1: Change keyboard interactivity from NONE to EXCLUSIVE**

In `show()`, replace:
```c
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            s->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
```
with:
```c
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            s->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
```

- [ ] **Step 2: Build**

Run:
```bash
cd /home/filas/projects/blackout-screen/src/blackout-overlay-c
make
```
Expected: builds clean.

- [ ] **Step 3: Deploy and verify F12 still toggles off through the grab**

Run:
```bash
cd /home/filas/projects/blackout-screen/src/blackout-overlay-c
make && install -m755 blackout-overlay ~/.local/bin/blackout-overlay
systemctl --user restart blackout-overlay
```
Then, in the live session: press **F12** to black out, then press **F12** again.
Expected: the screen blacks out and then comes back. This confirms KWin still routes the F12 *global* shortcut to the daemon even with an EXCLUSIVE keyboard grab.

If F12 does NOT toggle back off (you appear stuck on a black screen), use the escape hatch:
```bash
kill -USR2 "$(cat /run/user/$(id -u)/blackout-overlay.pid)"
```
If the grab blocks the toggle, the fallback is to revert this task (keep keyboard `NONE`) and rely on the pointer lock alone for Bug A — note this in the commit/PR and stop to discuss before continuing.

- [ ] **Step 4: Commit**

```bash
cd /home/filas/projects/blackout-screen
git add src/blackout-overlay-c/blackout.c
git commit -m "feat: grab keyboard (EXCLUSIVE) while overlay is shown"
```

---

## Task 4: Lock (freeze) the pointer on enter, unlock on hide

**Files:**
- Modify: `src/blackout-overlay-c/blackout.c` (add lock listener, edit `ptr_enter`, edit `hide()`)

- [ ] **Step 1: Add a no-op listener for the locked pointer**

Directly ABOVE the `/* ---- pointer: hide the cursor while it is over the overlay ---- */` comment, add:
```c
/* ---- locked pointer: events are informational; we just need the freeze ---- */
static void lp_locked(void *d, struct zwp_locked_pointer_v1 *lp) {}
static void lp_unlocked(void *d, struct zwp_locked_pointer_v1 *lp) {}
static const struct zwp_locked_pointer_v1_listener lp_listener = {
    .locked   = lp_locked,
    .unlocked = lp_unlocked,
};
```

- [ ] **Step 2: Create the lock in `ptr_enter`**

In `ptr_enter`, after the existing body (after the `wl_surface_commit(cursor_surface);` line, still inside the function), add:
```c
    /* Freeze the pointer for as long as the overlay is up: a locked pointer
       stops emitting motion, so the cursor cannot crawl to a screen edge and
       trigger KWin's edge-glow (Bug A). Created once; lives until hide(). */
    if (showing && pointer_constraints && !locked_pointer) {
        locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
            pointer_constraints, surface, wl_pointer, NULL,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_locked_pointer_v1_add_listener(locked_pointer, &lp_listener, NULL);
    }
```
Note: `surface` and `wl_pointer` are the existing parameters of `ptr_enter`; `surface` is the fullscreen overlay surface that just gained pointer focus, which is exactly what we lock to.

- [ ] **Step 3: Destroy the lock in `hide()`**

In `hide()`, at the very top of the function body, immediately after:
```c
    if (!showing) return;
    showing = false;
```
add:
```c
    if (locked_pointer) {
        zwp_locked_pointer_v1_destroy(locked_pointer);
        locked_pointer = NULL;
    }
```

- [ ] **Step 4: Build**

Run:
```bash
cd /home/filas/projects/blackout-screen/src/blackout-overlay-c
make clean && make
```
Expected: builds clean, no warnings.

- [ ] **Step 5: Commit**

```bash
cd /home/filas/projects/blackout-screen
git add src/blackout-overlay-c/blackout.c
git commit -m "feat: lock pointer while overlay is shown to kill edge-glow (Bug A)"
```

---

## Task 5: Deploy and verify the fix (Bug A repro)

**Files:** none (verification only)

- [ ] **Step 1: Deploy the new daemon**

Run:
```bash
cd /home/filas/projects/blackout-screen/src/blackout-overlay-c
make && install -m755 blackout-overlay ~/.local/bin/blackout-overlay
systemctl --user restart blackout-overlay
systemctl --user --no-pager status blackout-overlay | head -5
```
Expected: service is `active (running)`.

- [ ] **Step 2: Verify the pointer is frozen while blacked out**

In the live session: press **F12** to black out, then move the mouse hard toward the bottom of the screen, then press **F12** to come back.
Expected while black: no blue seam/glow appears at the bottom edge no matter how the mouse is pushed. After F12-off: the cursor reappears wherever it was frozen (not centered — by design).

- [ ] **Step 3: Capture the original Bug A repro to confirm full black**

Run (the delay gives you time to black out and shove the cursor to the bottom edge):
```bash
spectacle -b -n -f -d 6000 -o /tmp/blackout_bug.png
```
During the 6s countdown: press F12 to black out and hold/push the mouse at the very bottom edge. After capture:
```bash
identify -format '%[fx:mean.r],%[fx:mean.g],%[fx:mean.b]\n' /tmp/blackout_bug.png
# Inspect the bottom rows specifically:
identify -format '%[fx:mean.b]\n' /tmp/blackout_bug.png[1920x4+0+1076]
```
Expected: overall mean `0,0,0` and the bottom-4-rows blue mean is `0` (no blue seam). Then press F12 to restore.

If a blue edge STILL appears: the lock isn't activating (check that `ptr_enter` fires — add a temporary `fprintf(stderr,...)` and watch `journalctl --user -u blackout-overlay -f`) OR the glow is a KWin electric-border element rendering above the OVERLAY layer independent of cursor position. In the latter case, stop and reassess — the lock approach assumes the glow is cursor-motion-triggered.

- [ ] **Step 4: Verify the escape hatch still works**

Run:
```bash
kill -USR1 "$(cat /run/user/$(id -u)/blackout-overlay.pid)"   # black out
kill -USR2 "$(cat /run/user/$(id -u)/blackout-overlay.pid)"   # force off
```
Expected: first command blacks out, second restores. (Confirms SIGUSR2 unlocks the pointer + tears down the grab.)

- [ ] **Step 5: No commit** (verification only). If all checks pass, the feature is complete.

---

## Task 6: Update project memory

**Files:** none in repo — updates Claude's auto-memory.

- [ ] **Step 1: Update the blackout-screen-state memory**

Edit `/home/filas/.claude/projects/-home-filas/memory/blackout-screen-state.md`:
- Mark **Bug B** as FIXED (the user confirmed it; remove from "in flight").
- Replace **Bug A** with a resolved note: fixed by locking pointer + EXCLUSIVE keyboard grab while the overlay is shown (`lock_pointer` freezes motion so the cursor can't reach the edge); SIGUSR2 remains the escape hatch. Reference the new commits.
- Note that `set_keyboard_interactivity` is now `EXCLUSIVE` and the daemon depends on `zwp_pointer_constraints_v1`.

No `MEMORY.md` index change needed (the pointer line already exists).

---

## Self-Review Notes

- **Spec coverage:** keyboard grab (Task 3), pointer freeze (Task 4), no centering (explicit in Task 4/5), escape hatch preserved (Task 3/5), build wiring (Task 1/2), Bug A verification (Task 5) — all covered.
- **Type consistency:** `pointer_constraints` (global), `locked_pointer` (global), `lp_listener`/`lp_locked`/`lp_unlocked` used consistently across Tasks 2 and 4. Registry binds version `1` of `zwp_pointer_constraints_v1`. `ptr_enter` params `surface`/`wl_pointer` match the existing signature in `blackout.c`.
- **No automated tests:** intentional — the live C daemon has no test harness; verification is the build + spectacle deploy loop, consistent with the project's existing practice. The dead Python tests are out of scope.
- **Risk flagged:** if the EXCLUSIVE keyboard grab swallows the F12 global shortcut (Task 3, Step 3), revert to `NONE` and rely on the pointer lock alone — stop to discuss.
