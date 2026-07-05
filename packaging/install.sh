#!/usr/bin/env bash
set -euo pipefail

fail() {
    kdialog --error "$1"
    exit 1
}

INSTALL_DIR="$(cd "$(dirname "$0")" && pwd)"

# --- Pre-flight: kdialog availability (before any fail() calls) ---
if ! command -v kdialog >/dev/null 2>&1; then
    echo "Missing required tool: kdialog. This should already be present on any KDE Plasma system - is this really a KDE Plasma session?" >&2
    exit 1
fi

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
for tool in busctl systemctl; do
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
