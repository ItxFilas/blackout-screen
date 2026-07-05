# Self-Extracting .run Installer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce `blackout-screen-install.run`, a self-extracting shell installer that builds the daemon from source, installs it and its systemd unit/desktop file, starts it, and hands the user off to KDE's Shortcuts settings to bind whatever key they choose.

**Architecture:** `packaging/install.sh` and `packaging/uninstall.sh` hold all the real install/uninstall logic and are directly runnable from the repo root for testing. `packaging/stub.sh` is a shell self-extraction header with no project-specific logic. A new top-level `Makefile`'s `dist` target stages `src/`, `systemd/`, `bin/blackout-toggle`, and the two packaging scripts into a `tar.gz`, then concatenates `stub.sh` + that archive into the final `.run` file.

**Tech Stack:** POSIX/bash shell, `kdialog` (KDE-native dialogs, already present on every KDE system), `tar`/`gzip`, GNU Make, `systemd --user`.

**Spec:** `docs/superpowers/specs/2026-07-05-run-installer-design.md`

## Global Constraints

- Repo: `~/Mods/blackout-screen`.
- This tool only supports KDE Plasma on Wayland with KWin — pre-flight checks must fail with a specific `kdialog --error` message naming what's wrong, not a generic failure.
- No partial installs: the build must succeed before any files are installed.
- No package-manager auto-install (`dnf`/`apt`) — missing tools are named in the error message, not installed automatically.
- Key binding is never written to `kglobalshortcutsrc` by this installer — the `.desktop` file is installed unbound, and KDE's own Shortcuts settings (`systemsettings6 kcm_keys` / `kcmshell6 kcm_keys`) is what the user binds a key through.
- Audience is small-scale/public sharing, not hardened mass-market — clear specific errors are enough; exhaustive edge-case handling is not required.

---

### Task 1: install.sh and uninstall.sh

**Files:**
- Create: `packaging/install.sh`
- Create: `packaging/uninstall.sh`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: two scripts that Task 2 copies verbatim into the staged archive as siblings of `src/`, `systemd/`, `bin/`. `install.sh` locates `uninstall.sh` via `$(dirname "$0")/uninstall.sh` (script-relative, so it works both when run directly from `packaging/` in this repo and when both files are copied to the same directory in the packaged archive) and locates `src/blackout-overlay-c`, `bin/blackout-toggle`, `systemd/blackout-overlay.service` relative to the current working directory (so it works both run from the repo root directly, and from the extracted package root, which has the same relative layout).

- [ ] **Step 1: Write install.sh**

Create `packaging/install.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

fail() {
    kdialog --error "$1"
    exit 1
}

INSTALL_DIR="$(cd "$(dirname "$0")" && pwd)"

# --- Pre-flight: KDE Plasma on Wayland via KWin ---
if [ "${XDG_SESSION_TYPE:-}" != "wayland" ] || ! pgrep -x kwin_wayland >/dev/null 2>&1; then
    fail "This tool requires KDE Plasma running on Wayland (KWin).
Your session: XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-unset}, XDG_CURRENT_DESKTOP=${XDG_CURRENT_DESKTOP:-unset}"
fi

# --- Pre-flight: build tools ---
for tool in gcc pkg-config wayland-scanner; do
    command -v "$tool" >/dev/null 2>&1 || fail "Missing required build tool: $tool.
Install a C compiler and wayland-scanner (from your distro's wayland-utils/wayland-devel package) and re-run this installer."
done
pkg-config --exists wayland-client || fail "Missing wayland-client development headers.
Install your distro's wayland-client development package (e.g. wayland-devel, libwayland-dev) and re-run this installer."

# --- Pre-flight: runtime tools ---
for tool in busctl kdialog systemctl; do
    command -v "$tool" >/dev/null 2>&1 || fail "Missing required tool: $tool. This should already be present on any KDE Plasma system - is this really a KDE Plasma session?"
done

# --- Build ---
BUILD_LOG="$(mktemp)"
if ! make -C src/blackout-overlay-c >"$BUILD_LOG" 2>&1; then
    fail "Build failed. Last 20 lines of output:

$(tail -n 20 "$BUILD_LOG")"
fi
rm -f "$BUILD_LOG"

# --- Install ---
install -Dm755 src/blackout-overlay-c/blackout-overlay "$HOME/.local/bin/blackout-overlay"
install -Dm755 bin/blackout-toggle "$HOME/.local/bin/blackout-toggle"
install -Dm644 systemd/blackout-overlay.service "$HOME/.config/systemd/user/blackout-overlay.service"

DESKTOP_FILE="$HOME/.local/share/applications/net.local.blackout-toggle.desktop"
mkdir -p "$(dirname "$DESKTOP_FILE")"
cat > "$DESKTOP_FILE" <<'EOF'
[Desktop Entry]
Exec=%h/.local/bin/blackout-toggle
Name=blackout-toggle
NoDisplay=true
StartupNotify=false
Type=Application
X-KDE-GlobalAccel-CommandShortcut=true
EOF

install -Dm755 "$INSTALL_DIR/uninstall.sh" "$HOME/.local/share/blackout-screen/uninstall.sh"

# --- Activate (enable + restart covers both fresh install and upgrade) ---
systemctl --user daemon-reload
systemctl --user enable blackout-overlay
if ! systemctl --user restart blackout-overlay; then
    fail "Failed to start the blackout-overlay service. Check: systemctl --user status blackout-overlay"
fi

# --- Done: hand off key binding to KDE's own Shortcuts settings ---
kdialog --msgbox "blackout-screen is installed and running.

Next: choose a key to toggle it. The Shortcuts settings page will open now - search for \"blackout-toggle\" and press your chosen key."

if command -v systemsettings6 >/dev/null 2>&1; then
    systemsettings6 kcm_keys &
elif command -v kcmshell6 >/dev/null 2>&1; then
    kcmshell6 kcm_keys &
else
    kdialog --msgbox "Could not find System Settings automatically. Open System Settings > Shortcuts yourself and search for \"blackout-toggle\"."
fi
```

- [ ] **Step 2: Write uninstall.sh**

Create `packaging/uninstall.sh`:

```bash
#!/usr/bin/env bash
set -uo pipefail

systemctl --user disable --now blackout-overlay 2>/dev/null || true
rm -f "$HOME/.local/bin/blackout-overlay"
rm -f "$HOME/.local/bin/blackout-toggle"
rm -f "$HOME/.config/systemd/user/blackout-overlay.service"
rm -f "$HOME/.local/share/applications/net.local.blackout-toggle.desktop"
systemctl --user daemon-reload

kdialog --msgbox "blackout-screen has been uninstalled.

If you had bound a key to it, that binding is still listed in System Settings > Shortcuts under a stale entry - open Shortcuts and remove it manually; there is no safe way to do that from a script."

rm -f "$HOME/.local/share/blackout-screen/uninstall.sh"
rmdir "$HOME/.local/share/blackout-screen" 2>/dev/null || true
```

- [ ] **Step 3: Make both scripts executable and syntax-check them**

Run: `chmod +x packaging/install.sh packaging/uninstall.sh && bash -n packaging/install.sh && bash -n packaging/uninstall.sh`
Expected: no output (both parse cleanly).

- [ ] **Step 4: Run install.sh directly from the repo root (real reinstall test)**

This machine already satisfies every pre-flight check and already has the daemon installed, so running this is a safe reinstall/upgrade test — every file it touches gets overwritten with identical or updated content, and the service gets restarted with the freshly-built binary.

Run: `cd ~/Mods/blackout-screen && ./packaging/install.sh`
Expected: no `kdialog --error` dialog appears; a `kdialog --msgbox` appears confirming install; the Shortcuts settings window opens. Then verify:

```bash
systemctl --user is-active blackout-overlay
test -x ~/.local/bin/blackout-overlay && echo "binary present"
test -x ~/.local/share/blackout-screen/uninstall.sh && echo "uninstall.sh present"
```
Expected: `active`, `binary present`, `uninstall.sh present`.

- [ ] **Step 5: Run uninstall.sh and verify clean removal**

Run: `~/.local/share/blackout-screen/uninstall.sh`
Expected: a `kdialog --msgbox` confirms uninstall. Then verify:

```bash
systemctl --user is-active blackout-overlay || echo "service stopped (expected)"
test ! -e ~/.local/bin/blackout-overlay && echo "binary removed"
test ! -e ~/.local/share/blackout-screen && echo "uninstall dir removed"
```
Expected: `service stopped (expected)`, `binary removed`, `uninstall dir removed`.

- [ ] **Step 6: Reinstall to leave the daemon running again**

Since Step 5 just uninstalled the daemon this user relies on day-to-day, restore it:

Run: `cd ~/Mods/blackout-screen && ./packaging/install.sh`
Expected: same as Step 4 — daemon reinstalled and running.

- [ ] **Step 7: Commit**

```bash
cd ~/Mods/blackout-screen
git add packaging/install.sh packaging/uninstall.sh
git commit -m "feat: add install.sh and uninstall.sh for the .run installer

Both scripts are directly runnable from the repo root for testing (they
resolve src/bin/systemd paths relative to the working directory, matching
both the repo layout and the packaged archive's layout). install.sh's only
job beyond building/installing is registering the .desktop file unbound
and handing off to KDE's own Shortcuts settings for key binding — this
installer never writes kglobalshortcutsrc directly."
```

---

### Task 2: Self-extracting stub and `make dist`

**Files:**
- Create: `packaging/stub.sh`
- Create: `Makefile` (new, top-level)

**Interfaces:**
- Consumes: `packaging/install.sh`, `packaging/uninstall.sh` from Task 1 (copied byte-for-byte into the staged archive).
- Produces: `blackout-screen-install.run` in the repo root, runnable standalone with no dependency on the git checkout.

- [ ] **Step 1: Write the self-extracting stub**

Create `packaging/stub.sh`:

```sh
#!/bin/sh
# Self-extracting installer stub for blackout-screen. Everything below the
# __ARCHIVE_BELOW__ marker is a tar.gz payload appended by `make dist`; this
# part only extracts it to a temp dir and runs the real installer.
set -eu
MARKER_LINE=$(awk '/^__ARCHIVE_BELOW__$/ { print NR + 1; exit }' "$0")
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
tail -n +"$MARKER_LINE" "$0" | tar xz -C "$TMPDIR"
cd "$TMPDIR/blackout-screen"
sh install.sh
exit 0
__ARCHIVE_BELOW__
```

- [ ] **Step 2: Write the top-level Makefile**

Create `Makefile` at the repo root:

```makefile
DIST_NAME = blackout-screen-install.run
STAGE := $(shell mktemp -d)

dist:
	mkdir -p $(STAGE)/blackout-screen
	cp -r src systemd bin $(STAGE)/blackout-screen/
	cp packaging/install.sh packaging/uninstall.sh $(STAGE)/blackout-screen/
	chmod +x $(STAGE)/blackout-screen/install.sh $(STAGE)/blackout-screen/uninstall.sh
	tar czf $(STAGE)/payload.tar.gz -C $(STAGE) blackout-screen
	cat packaging/stub.sh $(STAGE)/payload.tar.gz > $(DIST_NAME)
	chmod +x $(DIST_NAME)
	rm -rf $(STAGE)

clean-dist:
	rm -f $(DIST_NAME)

.PHONY: dist clean-dist
```

- [ ] **Step 3: Build the .run file**

Run: `cd ~/Mods/blackout-screen && make dist`
Expected: `blackout-screen-install.run` appears in the repo root, executable (`ls -l blackout-screen-install.run` shows `-rwxr-xr-x` or similar).

- [ ] **Step 4: Verify the stub's marker/extraction logic in isolation**

Before running the full installer, confirm the self-extraction mechanism itself is sound:

Run:
```bash
cd ~/Mods/blackout-screen
TMPDIR=$(mktemp -d)
MARKER_LINE=$(awk '/^__ARCHIVE_BELOW__$/ { print NR + 1; exit }' blackout-screen-install.run)
tail -n +"$MARKER_LINE" blackout-screen-install.run | tar tz | head -5
rm -rf "$TMPDIR"
```
Expected: a file listing starting with `blackout-screen/`, `blackout-screen/src/`, `blackout-screen/install.sh`, etc. — confirms the marker line and tar payload are correctly aligned.

- [ ] **Step 5: Run the full .run file end-to-end (real reinstall test)**

Same reasoning as Task 1 Step 4 — this is a safe reinstall on the same machine, since every file it touches already exists with matching content.

Run: `./blackout-screen-install.run`
Expected: no `kdialog --error`; the same install confirmation dialog and Shortcuts settings window as Task 1 Step 4. Then verify:

```bash
systemctl --user is-active blackout-overlay
test -x ~/.local/share/blackout-screen/uninstall.sh && echo "uninstall.sh present"
```
Expected: `active`, `uninstall.sh present`.

- [ ] **Step 6: Commit**

```bash
cd ~/Mods/blackout-screen
git add packaging/stub.sh Makefile
git commit -m "feat: add self-extracting stub and make dist target

Produces blackout-screen-install.run: a shell stub with a tar.gz of
src/systemd/bin/install.sh/uninstall.sh appended after an
__ARCHIVE_BELOW__ marker line. No external packaging tool (e.g. makeself)
needed - the stub is ~10 lines using awk to find the marker and tail to
extract everything after it."
```

- [ ] **Step 7: Add the built .run file to .gitignore**

`blackout-screen-install.run` is a build artifact, not source — it should not be committed itself.

Add to `.gitignore`:
```
/blackout-screen-install.run
```

Run: `cd ~/Mods/blackout-screen && git status --short blackout-screen-install.run`
Expected: no output (file is ignored, already present from Step 3/5's build but not tracked).

Commit:
```bash
cd ~/Mods/blackout-screen
git add .gitignore
git commit -m "build: gitignore the built .run installer artifact"
```
