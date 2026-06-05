# Blackout overlay: output lifecycle + per-surface state + layered tests

**Date:** 2026-06-05
**Component:** `src/blackout-overlay-c/blackout.c` (the C wlr-layer-shell daemon)
**Status:** Approved design, ready for implementation plan

## Problem

A code review of the daemon surfaced two pre-existing correctness bugs and a
testing gap:

- **P1 — output-unplug crash.** `reg_global_remove` is a no-op. Unplugging a
  monitor leaves a dangling `wl_output` in the `outputs` list. The next
  `show()` iterates that list and creates a layer surface on a freed output →
  protocol error / crash. Realistic trigger: dock/undock, external-display
  sleep.
- **P2 — global per-output state.** `locked_pointer` is a file-global singleton
  (assumes one screen). `lsurf_closed()` (compositor-initiated close) doesn't
  reset `showing` or destroy `locked_pointer`, so state can desync and the
  global can dangle when its surface is torn down independently.
- **Testing gap.** The live C daemon + bash wrapper are untested; only the dead
  Python tree (`src/blackout/*.py`) has tests.

## Goals

1. Unplugging/replugging a monitor never crashes the daemon.
2. Per-output resources (surface, buffer, ksi, locked pointer) are owned by the
   surface and destroyed exactly once, on every path.
3. Two test layers: hermetic automated lifecycle tests + an opt-in real-KWin
   integration smoke script.

## Non-goals

- **Live hotplug coverage while shown.** Chosen behavior is **snapshot at
  toggle**: outputs are enumerated when toggling ON; a monitor plugged in while
  the overlay is already shown is *not* blacked until the next toggle. Removal
  while shown is still handled safely (no crash).
- No change to the `show()` synchronous `wl_display_roundtrip` (intentional
  anti-flash; black must land on the next compositor frame).
- No change to the hardcoded `wl_compositor` v4 bind (real minimum — we call
  `wl_surface.damage_buffer`, a v4 request) or the toggle keycode
  (`KEY_TOGGLE` 148).
- The mandatory empty Wayland listener callbacks stay (API boilerplate).

## Design

### Approach (chosen: B — push per-output state into the structs)

Generalize the surface lifecycle so P1 and P2 fall out of one rule — *a surface
owns all its resources; destroying it is always `destroy_surface()`* — rather
than special-casing the crash. The `destroy_surface()` helper already exists (a
prior cleanup commit unified `hide()` and `lsurf_closed()` teardown).

### 1. Output lifecycle (fixes P1)

- Add `uint32_t name;` to `struct output`, set from the registry `name` in
  `reg_global` when the `wl_output` is bound.
- Implement `reg_global_remove(name)`:
  - Walk `outputs`; find the `struct output` whose `name == name`.
  - If a `struct surface` exists for that output (only possible while
    `showing`), `destroy_surface()` it.
  - `wl_output_destroy()` the output proxy, unlink from `outputs`, free.
- Because `show()` iterates the pruned `outputs` list, a removed monitor is
  simply not covered. No dangling `wl_output`.
- **Open implementation detail for the plan:** a `struct surface` does not
  currently back-reference its `struct output`. The plan must choose how
  `reg_global_remove` finds the surface to destroy — e.g. store
  `struct output *output;` (or its `wl_output *`) on `struct surface` and match,
  or store the surface pointer on the output. Prefer a single back-pointer on
  `struct surface` set in `show()`.

### 2. Per-surface state (fixes P2)

- Move `locked_pointer` from a file-global into `struct surface`.
- `ptr_enter` maps the incoming `wl_surface` → its `struct surface` (walk
  `surfaces`) and locks *that* surface's pointer (created
  `LIFETIME_PERSISTENT`, as today). The per-surface guard replaces the global
  `!locked_pointer` guard.
- `destroy_surface()` destroys `s->locked_pointer` if set — so the locked
  pointer can never dangle, on any teardown path.
- When a surface is torn down via the compositor/output path
  (`lsurf_closed` / `reg_global_remove`) and `wl_list_empty(&surfaces)` becomes
  true, reset `showing = false` so the next toggle re-shows.

### 3. Tests (layered)

> **Tooling note (2026-06-05):** No headless compositor (`sway`/`cage`/
> `weston`) or capture tool (`grim`) is installed, and the GCC sanitizer
> runtime is missing. So Layer 1 is implemented as install-free unit tests on
> extracted logic rather than a nested-compositor harness.

**Layer 1 — automated, hermetic, install-free (`make test`)**
- Extract the output bookkeeping into a small pure module
  (`outputs.h`/`outputs.c`): `output_track`, `output_find`, `output_drop`,
  operating on a `wl_list` of `struct output`. These touch only the list and
  the stored registry `name` — never the `wl_output` proxy — so they link
  against `libwayland-client` (for the `wl_list` helpers) with **no display
  connection**.
- A plain C test (`test_outputs.c`, run by `make test`) asserts:
  `output_find` returns the node matching a registry name, returns NULL for an
  absent name, returns the right node among several; `output_drop` removes
  exactly that node and shrinks the list; the list empties after dropping all.
- This pins the core of the P1 fix (find/remove the right output by name)
  deterministically. It does **not** exercise real Wayland protocol or
  KWin-specific behavior — that's Layer 2.

**Layer 2 — manual, real KWin (`integration-test.sh`, opt-in)**
- Run inside the user's graphical session. Scripts the signal +
  `spectacle -b -n -f` + `identify` mean-pixel checks:
  - toggle ON → screenshot mean == `0,0,0`; toggle OFF → mean != 0.
- Prints a manual checklist for human-only confirmations: Alt+Tab is sealed
  while shown, the toggle key turns it off, dock/undock doesn't crash.
- Clearly marked as non-CI (needs a live session, flashes the screen).

## Affected files

- `src/blackout-overlay-c/outputs.h` (new) — `struct output` + pure
  track/find/drop declarations.
- `src/blackout-overlay-c/outputs.c` (new) — pure implementations.
- `src/blackout-overlay-c/test_outputs.c` (new) — Layer 1 unit tests.
- `src/blackout-overlay-c/blackout.c` — use the outputs module; per-surface
  `output` back-pointer + `locked_pointer`; `reg_global`, `reg_global_remove`,
  `ptr_enter`, `destroy_surface`, `show`/`hide` glue.
- `src/blackout-overlay-c/Makefile` — link `outputs.c`; add `test` target.
- `src/blackout-overlay-c/integration-test.sh` (new) — Layer 2 smoke script.

## Risks / verification

- Layer 1 fidelity: a nested wlroots compositor is not KWin; it validates
  protocol/lifecycle, not KWin quirks. Accepted — Layer 2 covers real KWin.
- Multi-output paths can't be fully verified on the single-monitor laptop
  except via Layer 1's virtual outputs; Layer 2's dock/undock check is manual.
- Regression guard: after changes, re-run the existing manual sanity
  (USR1 → `0,0,0`, USR2 → non-zero) and confirm Alt+Tab seal + toggle-off still
  work on real KWin.
