# Idle Inhibition via Wayland idle-inhibit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While the blackout overlay is shown, block idle-triggered sleep/screen-lock/dim natively via `zwp_idle_inhibit_manager_v1`, restoring prior state automatically on hide; delete the broken busctl-based inhibit code.

**Architecture:** One `zwp_idle_inhibitor_v1` per overlay surface, created in `show()` and destroyed in `destroy_surface()` (the single teardown path used by hide, compositor close, and output unplug). The compositor scopes the inhibition to surface visibility, so no state can leak even on daemon crash. The user's manual applet toggle is separate plasmashell state and is never touched.

**Tech Stack:** C, wayland-client, wayland-scanner, system wayland-protocols package (idle-inhibit XML confirmed present at `/usr/share/wayland-protocols/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml`), GNU Make, systemd user service `blackout-overlay`.

**Spec:** `docs/superpowers/specs/2026-07-04-idle-inhibit-design.md`

## Global Constraints

- Repo: `~/Mods/blackout-screen`; all code in `src/blackout-overlay-c/`.
- Follow the existing degradation pattern: a missing Wayland global (here `zwp_idle_inhibit_manager_v1`) must not break the overlay — skip the feature silently.
- Generated `*-client-protocol.{c,h}` files stay untracked (build products), matching the three existing protocols.
- The final live verification requires the user at the machine — STOP and ask before/after steps that need them to observe the screen (standing user preference).
- KWin exposes `zwp_idle_inhibit_manager_v1` version 1 on this system (verified 2026-07-04).

---

### Task 1: Makefile — generate idle-inhibit protocol code

**Files:**
- Modify: `src/blackout-overlay-c/Makefile`

**Interfaces:**
- Consumes: system XML `$(WP_DIR)/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml`.
- Produces: build products `idle-inhibit-unstable-v1-client-protocol.h` / `.c` (referred to as `$(GEN_H4)` / `$(GEN_C4)`), linked into `blackout-overlay`. Task 2's `#include` and symbols (`zwp_idle_inhibit_manager_v1_interface`, `zwp_idle_inhibit_manager_v1_create_inhibitor`, `zwp_idle_inhibitor_v1_destroy`) come from these files.

- [ ] **Step 1: Add the protocol block to the Makefile**

In `src/blackout-overlay-c/Makefile`, after the keyboard-shortcuts-inhibit block (lines 17–20), add:

```makefile
# idle-inhibit (blocks idle-triggered dim/lock/sleep while the overlay is shown)
II_XML  = $(WP_DIR)/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml
GEN_H4  = idle-inhibit-unstable-v1-client-protocol.h
GEN_C4  = idle-inhibit-unstable-v1-client-protocol.c
```

After the `$(GEN_C3)` rule, add:

```makefile
$(GEN_H4):
	$(SCANNER) client-header $(II_XML) $@

$(GEN_C4):
	$(SCANNER) private-code $(II_XML) $@
```

Change the `blackout-overlay` rule (both dependency line and link line) to:

```makefile
blackout-overlay: blackout.c outputs.c outputs.h $(GEN_H) $(GEN_C) $(GEN_H2) $(GEN_C2) $(GEN_H3) $(GEN_C3) $(GEN_H4) $(GEN_C4)
	$(CC) $(CFLAGS) -o $@ blackout.c outputs.c $(GEN_C) $(GEN_C2) $(GEN_C3) $(GEN_C4) $(LIBS)
```

Change the `clean` rule to:

```makefile
clean:
	rm -f blackout-overlay $(TEST_BIN) $(GEN_H) $(GEN_C) $(GEN_H2) $(GEN_C2) $(GEN_H3) $(GEN_C3) $(GEN_H4) $(GEN_C4)
```

- [ ] **Step 2: Verify generation works**

Run: `cd ~/Mods/blackout-screen/src/blackout-overlay-c && make idle-inhibit-unstable-v1-client-protocol.h idle-inhibit-unstable-v1-client-protocol.c && grep -c zwp_idle_inhibit_manager_v1 idle-inhibit-unstable-v1-client-protocol.h`
Expected: both scanner commands echo, grep prints a count ≥ 5.

- [ ] **Step 3: Verify full build still succeeds (code unchanged so far)**

Run: `make clean && make`
Expected: compiles `blackout-overlay` with all four protocols, no warnings from the generated files.

- [ ] **Step 4: Commit**

```bash
cd ~/Mods/blackout-screen
git add src/blackout-overlay-c/Makefile
git commit -m "build: generate idle-inhibit protocol client code"
```

---

### Task 2: blackout.c — idle inhibitor per surface, delete busctl block

**Files:**
- Modify: `src/blackout-overlay-c/blackout.c`

**Interfaces:**
- Consumes: `zwp_idle_inhibit_manager_v1_interface`, `zwp_idle_inhibit_manager_v1_create_inhibitor()`, `zwp_idle_inhibitor_v1_destroy()` from Task 1's generated header.
- Produces: daemon behavior — overlay shown ⇒ idle inhibited; overlay hidden ⇒ inhibitor gone. No API consumed by later tasks.

- [ ] **Step 1: Add the include**

After the existing `#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"` (line 14), add:

```c
#include "idle-inhibit-unstable-v1-client-protocol.h"
```

- [ ] **Step 2: Delete the dead busctl inhibit block**

Delete lines 30–73 entirely — the comment block starting `/* ---- sleep-inhibition via org.freedesktop.PowerManagement.Inhibit ----` through the closing brace of `uninhibit_sleep()`, including the `inhibit_by_us` / `inhibit_cookie` globals and the functions `sleep_inhibit_active()`, `inhibit_sleep()`, `uninhibit_sleep()`.

Then remove the four call sites (a stale one left behind breaks the build, which is the check in Step 6):
- `inhibit_sleep();` in `show()` (line 190)
- `uninhibit_sleep();` in `hide()` (line 232)
- `uninhibit_sleep();` in `lsurf_closed()` (line 169)
- `uninhibit_sleep();` in `reg_global_remove()` (line 384)

The `showing = false;` statements next to them stay.

- [ ] **Step 3: Add the manager global and surface member**

Next to the existing `ksi_manager` global (line 86), add:

```c
static struct zwp_idle_inhibit_manager_v1 *idle_manager;
```

In `struct surface`, after the `locked_pointer` member (line 98), add:

```c
    struct zwp_idle_inhibitor_v1  *idle_inhibitor;    /* blocks idle dim/lock/sleep while this surface is visible */
```

- [ ] **Step 4: Create the inhibitor in show(), destroy it in destroy_surface()**

In `show()`, immediately after the keyboard-shortcuts-inhibitor `if` block (after line 213), add:

```c
        /* Block idle-triggered dim/lock/sleep for as long as this surface is
           visible. The compositor scopes the inhibition to surface lifetime,
           so hide()/crash/unplug all restore power management by themselves —
           and the user's manual "Block Sleep" applet toggle is never touched. */
        if (idle_manager)
            s->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
                idle_manager, s->wl_surface);
```

In `destroy_surface()`, before the `ksi` line (line 157), add:

```c
    if (s->idle_inhibitor) zwp_idle_inhibitor_v1_destroy(s->idle_inhibitor);
```

- [ ] **Step 5: Bind the manager in the registry**

In `reg_global()`, after the `zwp_keyboard_shortcuts_inhibit_manager_v1` branch (line 362), add:

```c
    } else if (!strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name)) {
        idle_manager = wl_registry_bind(
            reg, name, &zwp_idle_inhibit_manager_v1_interface, 1);
```

- [ ] **Step 6: Build clean and run unit tests**

Run: `cd ~/Mods/blackout-screen/src/blackout-overlay-c && make clean && make && make test`
Expected: zero warnings (`-Wall -Wextra` would flag leftover busctl remnants or unused variables); `test-outputs` prints its pass summary unchanged.

- [ ] **Step 7: Grep for dead references**

Run: `grep -n "inhibit_sleep\|inhibit_by_us\|inhibit_cookie\|busctl\|popen\|HasInhibit" blackout.c`
Expected: no output (also proves `<stdio.h>`'s popen use is gone; `stdio.h` itself stays — still used for the PID file).

- [ ] **Step 8: Commit**

```bash
cd ~/Mods/blackout-screen
git add src/blackout-overlay-c/blackout.c
git commit -m "feat: block idle sleep/lock via zwp_idle_inhibit while shown

Replaces the busctl PowerManagement inhibit, which was a silent no-op:
PowerDevil releases inhibition cookies when the caller's bus connection
closes, and busctl's closes immediately. The Wayland inhibitor is scoped
by the compositor to surface visibility, so restore-on-hide is structural
and the user's manual applet toggle is left exactly as found."
```

---

### Task 3: Deploy and verify live (needs the user)

**Files:**
- Modify: none (install + service restart; `~/.local/bin/blackout-overlay` is replaced by `make install`)

**Interfaces:**
- Consumes: `blackout-overlay` binary from Task 2; systemd user unit `blackout-overlay.service`; toggle script sends SIGUSR1/SIGUSR2 to the PID in `/run/user/$UID/blackout-overlay.pid`.
- Produces: running daemon with idle inhibition; empirical answer to which D-Bus probe (if any) observes Wayland idle inhibitors, feeding Task 4.

- [ ] **Step 1: Install and restart the daemon**

Run: `cd ~/Mods/blackout-screen/src/blackout-overlay-c && make install && systemctl --user restart blackout-overlay && sleep 1 && systemctl --user is-active blackout-overlay && cat /run/user/$(id -u)/blackout-overlay.pid`
Expected: `active` and a fresh PID.

- [ ] **Step 2: Automated probe sweep while blacked out**

Run (screen goes black for ~3 s; commands keep executing underneath):

```bash
PID=$(cat /run/user/$(id -u)/blackout-overlay.pid)
kill -USR1 $PID; sleep 1
echo "== PM HasInhibit =="; busctl --user call org.freedesktop.PowerManagement /org/freedesktop/PowerManagement/Inhibit org.freedesktop.PowerManagement.Inhibit HasInhibit
echo "== PolicyAgent HasInhibition(screen=4) =="; busctl --user call org.kde.Solid.PowerManagement /org/kde/Solid/PowerManagement/PolicyAgent org.kde.Solid.PowerManagement.PolicyAgent HasInhibition u 4
echo "== PolicyAgent ListInhibitions =="; qdbus --literal org.kde.Solid.PowerManagement /org/kde/Solid/PowerManagement/PolicyAgent org.kde.Solid.PowerManagement.PolicyAgent.ListInhibitions
echo "== logind inhibitors =="; busctl call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager ListInhibitors | tr '"' '\n' | grep -i -A2 idle | head
kill -USR2 $PID
```

Record which probes flip between overlay-on and overlay-off (re-run the flipping ones after hide to confirm they clear). Note: the user's manual applet toggle is currently ON, which makes `HasInhibit`/`HasInhibition` return true regardless — interpret accordingly, or ask the user to turn it off first for a clean reading.

Expected: at least one probe distinguishes on/off; if none does, that's a finding, not a failure — functional verification is Step 3.

- [ ] **Step 3: STOP — ask the user for the functional check**

Ask the user (do not proceed without them; do not assume they acted):

1. Turn the applet's manual block toggle OFF.
2. Set screen locking timeout temporarily to 1 minute (System Settings → Screen Locking), or confirm current timeout and willingness to wait it out.
3. Press the blackout key, wait past the lock timeout, press it again.
4. Report: did the lock screen appear (FAIL) or did the desktop come back unlocked (PASS)? Also: with blackout ON, does the battery applet list something like "blackout-overlay is blocking sleep and screen locking"? (Screen is black — check by memory of Step 2's probe output or briefly toggling.)
5. Confirm their manual toggle, when turned back ON before a blackout, is still ON after blackout off (restore-as-found).

- [ ] **Step 4: Commit nothing — record results**

No repo changes in this task. Paste the probe results into the Task 4 decision.

---

### Task 4: Integration test — encode whatever probe works

**Files:**
- Modify: `src/blackout-overlay-c/integration-test.sh`

**Interfaces:**
- Consumes: Task 3's empirical finding of a reliable D-Bus probe (or the finding that none exists).
- Produces: `make smoke` coverage for idle inhibition — automated if a probe works, human checklist item otherwise.

- [ ] **Step 1: Add the check to integration-test.sh**

If a probe from Task 3 Step 2 reliably flips with overlay state, add after the existing pixel checks (adjust the probe command to the one that worked; this template assumes `HasInhibition u 4` worked and the manual toggle is off during the test):

```bash
echo "== idle inhibition =="
kill -USR1 "$PID"; sleep 0.6
inh_on=$(busctl --user call org.kde.Solid.PowerManagement /org/kde/Solid/PowerManagement/PolicyAgent org.kde.Solid.PowerManagement.PolicyAgent HasInhibition u 4)
kill -USR2 "$PID"; sleep 0.6
inh_off=$(busctl --user call org.kde.Solid.PowerManagement /org/kde/Solid/PowerManagement/PolicyAgent org.kde.Solid.PowerManagement.PolicyAgent HasInhibition u 4)
echo "inhibited while shown: $inh_on (expect b true), after hide: $inh_off (expect b false — requires manual applet toggle OFF)"
[ "$inh_on" = "b true" ] || { echo "FAIL: no idle inhibition while shown"; exit 1; }
[ "$inh_off" = "b false" ] || { echo "WARN: inhibition still present after hide (manual toggle on, or a leak)"; }
```

If no probe worked, instead append to the human `CHECKLIST` heredoc:

```
[ ] With the manual applet toggle OFF and lock timeout at 1 min: blackout on,
    wait past the timeout, blackout off -> desktop returns UNLOCKED and undimmed.
[ ] Battery applet shows no leftover "blocking sleep" line after blackout off.
```

- [ ] **Step 2: Run the smoke test**

Run: `cd ~/Mods/blackout-screen/src/blackout-overlay-c && make smoke`
Expected: existing pixel checks PASS plus the new check's expected output (screen flashes black twice).

- [ ] **Step 3: Commit**

```bash
cd ~/Mods/blackout-screen
git add src/blackout-overlay-c/integration-test.sh
git commit -m "test: cover idle inhibition in smoke test"
```
