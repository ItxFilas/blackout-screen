# Disable touchpad while blackout is shown

**Date:** 2026-07-05
**Status:** Approved

## Problem

The idle-inhibit fix (see `2026-07-04-idle-inhibit-design.md`) sealed sleep/lock
leaks, but a separate leak remains: KWin's 4-finger touchpad gesture for
switching virtual desktops still works while the overlay is shown, letting
the user see other desktops' contents through the swipe animation. Neither
`zwp_keyboard_shortcuts_inhibit_manager_v1` (keyboard shortcuts only) nor
`zwp_pointer_constraints_v1` (pointer motion/position only) covers
compositor-level touchpad gestures — there is no Wayland client protocol that
lets a client inhibit gesture recognition. The practical fix is to disable
the touchpad hardware itself while blacked out.

## Decision

Call KDE's own `org.kde.touchpad` D-Bus interface (confirmed present at
`/modules/kded_touchpad`, methods `disable()`/`enable()`/`toggle()`, all
`Q_NOREPLY`) from `show()` and `hide()`, via a backgrounded `system()` shell-out
to `busctl`. No new library dependency; no persistent D-Bus connection needed
since these are one-shot, stateless commands — unlike the deleted
`PowerManagement.Inhibit` cookie, there is nothing here whose lifetime could
be tied to (and killed by) a transient connection.

**Restore semantics:** `org.kde.touchpad` exposes no query/getter for current
enabled state, so true "restore to whatever it was before" (as idle-inhibit
achieves structurally) is not possible here. `hide()` unconditionally calls
`enable()`. Confirmed acceptable with the user: manually disabling the
touchpad immediately before triggering a blackout is not a real usage
pattern worth adding complexity for.

**Failure mode:** best-effort, fire-and-forget. If `kded_touchpad` isn't
loaded or `busctl` is missing, the call silently no-ops — the overlay still
shows/hides correctly, just without touchpad blocking. This matches the
existing degradation pattern for optional Wayland globals (`ksi_manager`,
`pointer_constraints`, `idle_manager`).

## Changes

All in `src/blackout-overlay-c/blackout.c`:

1. **`show()`.** After entering (position independent of the per-output
   surface loop — this call has nothing to do with Wayland surfaces), add:

   ```c
   system("busctl --user call org.kde.touchpad /modules/kded_touchpad"
          " org.kde.touchpad disable >/dev/null 2>&1 &");
   ```

2. **`hide()`.** Symmetric call with `enable` in place of `disable`.

3. No new globals, no new struct members, no new protocol/header. This is
   pure shell-out, not a Wayland object.

The trailing `&` inside the shell command backgrounds the `busctl` process so
a slow or hung D-Bus call cannot delay the black frame from appearing (or
delay hide() returning). `system()` still blocks briefly to fork the shell,
but not for the D-Bus round-trip itself.

## Error handling

- `busctl`/`kded_touchpad` absent → command fails silently (stderr
  redirected), overlay unaffected.
- `system()`'s return value is intentionally ignored — there is no
  recovery action to take on failure, and the command is fire-and-forget by
  design (same rationale as the deleted `uninhibit_sleep()`'s old `system()`
  call, minus that call's now-removed cookie-tracking bug).

## Testing

- No automated D-Bus check is meaningful (same finding as idle-inhibit:
  `kded_touchpad` exposes no queryable state to assert against).
- Human checklist addition to `integration-test.sh`'s existing `CHECKLIST`
  heredoc: toggle blackout on, attempt a touchpad gesture/click/scroll and
  confirm no response; toggle off, confirm touchpad works normally again.

## Out of scope

- True restore-as-found (querying prior touchpad state) — no D-Bus property
  exists to query; user confirmed unconditional enable-on-hide is fine.
- External USB/Bluetooth mice — `org.kde.touchpad` only affects the
  touchpad; this is intentional, matching the "gesture leak" problem being
  fixed (touchpad-specific).
- Any change to the idle-inhibit feature — this is purely additive.
