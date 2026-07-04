# Idle inhibition via Wayland idle-inhibit protocol

**Date:** 2026-07-04
**Status:** Approved

## Problem

While the blackout overlay is shown, KDE still treats the session as idle:
after the configured timeouts it dims, locks the screen (the lock screen maps
*above* the overlay and lights the panel back up), and eventually suspends.
The user currently works around this by manually enabling the battery applet's
"Manually Block Sleep and Screen Locking" toggle before blacking out, and
turning it back off after.

Desired behavior: blackout ON ⇒ sleep/screen-locking blocked; blackout OFF ⇒
power-management state exactly as it was before (the user's manual toggle, if
set, stays set; if unset, stays unset).

## Prior art in this repo (broken)

Commit `5008c54` added sleep inhibition through
`org.freedesktop.PowerManagement.Inhibit`, shelling out to `busctl` via
`popen()`. It is a silent no-op with two independent defects:

1. **Connection lifetime.** PowerDevil ties inhibition cookies to the
   caller's D-Bus connection and auto-releases them when the connection
   closes. `busctl call` connects, calls, and exits — the cookie is dead
   milliseconds after creation.
2. **Wrong scope.** `org.freedesktop.PowerManagement.Inhibit` blocks sleep
   only, not screen locking/dimming, so even a held cookie would cover only
   half of what the applet toggle covers.

Additionally, its `HasInhibit` guard ("skip if anything already inhibits")
makes the code do nothing whenever *any* app holds an inhibition — including
the user's own manual toggle.

## Decision

Replace the D-Bus code with the native Wayland **`zwp_idle_inhibit_manager_v1`**
protocol (confirmed exposed by KWin on this system, v1). An idle inhibitor is
attached to each overlay surface; the compositor keeps it effective exactly
while the surface is visible. KWin feeds this into PowerDevil, which blocks
idle-triggered dimming, screen locking, and suspend.

Why this beats the alternatives considered:

- **vs. persistent D-Bus connection (sd-bus):** no new library dependency, no
  cookie bookkeeping, and no way to leak an inhibition if the daemon is
  SIGKILLed mid-blackout — the compositor destroys the inhibitor with the
  surface.
- **vs. spawning `kde-inhibit` as a child:** no held subprocess, no PID
  management, no KDE-specific binary dependency.

Restore-as-found semantics are structural: the daemon never touches the
applet's manual toggle (separate plasmashell-internal state), so whatever the
user set stays set. This matches the applet toggle's own scope: idle-triggered
actions are blocked; explicit actions (power button, lid close, Meta+L manual
lock) still work.

## Changes

All in `src/blackout-overlay-c/`:

1. **Protocol generation (Makefile).** Generate
   `idle-inhibit-unstable-v1-client-protocol.{c,h}` from
   `/usr/share/wayland-protocols/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml`
   with `wayland-scanner`, following the existing pattern used for
   wlr-layer-shell, pointer-constraints, and keyboard-shortcuts-inhibit.
2. **Registry bind (`blackout.c`, `reg_global`).** Bind
   `zwp_idle_inhibit_manager_v1` (v1) into a new global `idle_manager`.
   Optional: if the compositor lacks it, everything else works as today.
3. **`struct surface`.** New member
   `struct zwp_idle_inhibitor_v1 *idle_inhibitor;`.
4. **`show()`.** After creating each per-output surface (alongside the
   existing keyboard-shortcuts inhibitor):
   `s->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(idle_manager, s->wl_surface);`
   guarded on `idle_manager` being non-NULL. One inhibitor per surface, so
   output unplug during blackout keeps the remaining outputs' inhibition.
5. **`destroy_surface()`.** Destroy `idle_inhibitor` if set. This single
   teardown path already covers hide(), compositor-initiated close, output
   removal, and daemon exit.
6. **Delete the dead D-Bus block.** Remove `sleep_inhibit_active()`,
   `inhibit_sleep()`, `uninhibit_sleep()`, the `inhibit_by_us`/
   `inhibit_cookie` globals, and their call sites in `show()`, `hide()`,
   `lsurf_closed()`, and `reg_global_remove()`.

No changes to the toggle script, systemd unit, or the Python tree.

## Error handling

- `zwp_idle_inhibit_manager_v1` missing from the registry → `idle_manager`
  stays NULL, inhibitor creation is skipped, overlay works as before (same
  degradation pattern as `ksi_manager`/`pointer_constraints`).
- Daemon killed while shown → compositor destroys the client's surfaces and
  with them the inhibitors; nothing leaks.
- The protocol has no failure events; `create_inhibitor` cannot fail
  observably.

## Testing

- **Build/unit:** existing `test_outputs` unit tests unaffected; `make` must
  produce the new protocol files and compile clean.
- **End-to-end (manual, needs the user at the machine):**
  1. With the applet toggle OFF: toggle blackout on → applet popup shows
     "an application is blocking sleep and screen locking"; KWin D-Bus
     inhibition state reports inhibited. Toggle off → back to normal.
  2. With the applet toggle ON: blackout on/off → toggle remains ON
     throughout.
  3. (Optional, slow) Set screen-lock timeout to 1 min, blackout on, wait —
     no lock screen appears.
- **Automated check:** probing during design showed PowerDevil's
  `ListInhibitions`/`ActiveInhibitions` do not list foreign inhibitors, so the
  exact scriptable probe is to be settled during implementation (candidates:
  PowerDevil `HasInhibition u 4`, logind `ListInhibitors`, applet tooltip
  text). If none proves reliable, the end-to-end checks above are the
  verification, appended to the manual checklist used by
  `integration-test.sh`.

## Out of scope

- Blocking lid-close / power-button sleep (the applet toggle doesn't either).
- Visually flipping the applet's toggle switch (plasmashell-internal state,
  no API; user confirmed the effect is sufficient).
- Any other daemon changes — the efficiency review found the rest already
  optimal (0% idle CPU in poll(), zero-fill SHM black buffers, event-driven).
