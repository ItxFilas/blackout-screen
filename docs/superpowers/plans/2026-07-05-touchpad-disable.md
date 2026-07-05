# Touchpad Disable While Blackout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While the blackout overlay is shown, disable the touchpad via `org.kde.touchpad`'s D-Bus `disable()`, so 4-finger desktop-swipe gestures (and all other touchpad input) can no longer reveal other desktops through the overlay; re-enable on hide.

**Architecture:** Two fire-and-forget `system()` shell-outs to `busctl --user call org.kde.touchpad ...`, one in `show()` calling `disable`, one in `hide()` calling `enable`. No new globals, no new struct members, no Wayland protocol involved — this is a plain D-Bus side effect independent of the per-output surface machinery.

**Tech Stack:** C, `system()` (already used elsewhere in the codebase's Python tree; this is the first C-side use), `busctl` (already relied on by the project — confirmed present), KDE's `org.kde.touchpad` D-Bus service at `/modules/kded_touchpad` (confirmed present on this system with `disable()`/`enable()`/`toggle()`, all `Q_NOREPLY`).

**Spec:** `docs/superpowers/specs/2026-07-05-touchpad-disable-design.md`

## Global Constraints

- Repo: `~/Mods/blackout-screen`; all code in `src/blackout-overlay-c/blackout.c`.
- No query/getter exists for touchpad state — restore-on-hide is unconditional `enable()`, not "restore to whatever it was before." Confirmed acceptable with the user.
- Best-effort only: if `busctl` or `kded_touchpad` is missing, the call silently no-ops (`>/dev/null 2>&1`); the overlay must still show/hide correctly regardless of whether the D-Bus call succeeds.
- The shell command must background the `busctl` process (trailing `&` inside the string passed to `system()`) so a slow/hung D-Bus call cannot delay the black frame appearing or delay `hide()` returning.

---

### Task 1: Add touchpad disable/enable to show()/hide()

**Files:**
- Modify: `src/blackout-overlay-c/blackout.c:145-196` (the `show()`/`hide()` functions)
- Modify: `src/blackout-overlay-c/integration-test.sh:29-41` (the `CHECKLIST` heredoc)

**Interfaces:**
- Consumes: nothing from other tasks (this plan has one task).
- Produces: nothing consumed elsewhere — this is a leaf change, no new symbols.

- [ ] **Step 1: Add the disable call to show()**

In `blackout.c`, `show()` currently starts:

```c
static void show(void) {
    if (showing) return;
    showing = true;
    struct output *out;
```

Change it to:

```c
static void show(void) {
    if (showing) return;
    showing = true;
    /* KWin's touchpad gestures (e.g. 4-finger desktop swipe) aren't reachable
       by any Wayland client-inhibition protocol, so the only way to seal that
       leak is to disable the touchpad hardware itself. Backgrounded so a
       slow/hung D-Bus call can't delay the black frame; best-effort, since
       there is no recovery action if kded_touchpad or busctl is missing. */
    system("busctl --user call org.kde.touchpad /modules/kded_touchpad"
           " org.kde.touchpad disable >/dev/null 2>&1 &");
    struct output *out;
```

- [ ] **Step 2: Add the enable call to hide()**

`hide()` currently is:

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

Change it to:

```c
static void hide(void) {
    if (!showing) return;
    showing = false;
    /* Unconditional re-enable: org.kde.touchpad has no query for prior state,
       so this cannot restore "whatever it was before" the way idle-inhibit
       does. Confirmed acceptable — manually disabling the touchpad right
       before triggering blackout isn't a real usage pattern. */
    system("busctl --user call org.kde.touchpad /modules/kded_touchpad"
           " org.kde.touchpad enable >/dev/null 2>&1 &");
    struct surface *s, *tmp;
    wl_list_for_each_safe(s, tmp, &surfaces, link)
        destroy_surface(s);
    wl_display_flush(display);
}
```

- [ ] **Step 3: Build and run the existing unit tests**

Run: `cd ~/Mods/blackout-screen/src/blackout-overlay-c && make clean && make && make test`
Expected: builds with zero warnings (`-Wall -Wextra`); `test_outputs: all assertions passed` (these tests don't touch `show()`/`hide()`, so this only confirms nothing else broke).

- [ ] **Step 4: Manual functional check**

Install and restart the daemon, then exercise it live:

```bash
cd ~/Mods/blackout-screen/src/blackout-overlay-c
make install
systemctl --user restart blackout-overlay
sleep 1
PID=$(cat /run/user/$(id -u)/blackout-overlay.pid)
kill -USR1 $PID   # toggle ON — screen goes black
```

Then, at the keyboard/touchpad: try a touchpad gesture, click, or scroll — nothing should respond (mouse pointer, if you have a separate USB/BT one, should still work; only the touchpad is affected).

```bash
kill -USR2 $PID   # toggle OFF — screen returns
```

Then confirm the touchpad works normally again (move the pointer, scroll, click).

Expected: touchpad dead while blacked out, responsive again after.

- [ ] **Step 5: Add the checklist line to integration-test.sh**

In `integration-test.sh`, the `CHECKLIST` heredoc currently ends:

```
  [ ] With the manual applet toggle OFF and lock timeout at 1 min: blackout on,
      wait past the timeout, blackout off -> desktop returns UNLOCKED and undimmed.
  [ ] Battery applet shows no leftover "blocking sleep" line after blackout off.
CHECKLIST
```

Add one line before `CHECKLIST`, so it reads:

```
  [ ] With the manual applet toggle OFF and lock timeout at 1 min: blackout on,
      wait past the timeout, blackout off -> desktop returns UNLOCKED and undimmed.
  [ ] Battery applet shows no leftover "blocking sleep" line after blackout off.
  [ ] Blackout ON, try a touchpad gesture/scroll/click -> no response; blackout
      OFF -> touchpad responsive again.
CHECKLIST
```

- [ ] **Step 6: Run the smoke test**

Run: `cd ~/Mods/blackout-screen/src/blackout-overlay-c && make smoke`
Expected: existing automated pixel checks PASS (screen flashes black twice), followed by the printed checklist including the new touchpad line.

- [ ] **Step 7: Commit**

```bash
cd ~/Mods/blackout-screen
git add src/blackout-overlay-c/blackout.c src/blackout-overlay-c/integration-test.sh
git commit -m "feat: disable touchpad while blackout is shown

KWin's touchpad gestures (4-finger desktop swipe) bypass every Wayland
client-inhibition protocol already in use here, letting a swipe reveal
other desktops through the overlay. org.kde.touchpad's disable()/enable()
seals that leak directly; best-effort since there's no query for prior
touchpad state to restore."
```
