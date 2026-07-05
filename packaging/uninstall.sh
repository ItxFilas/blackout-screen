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
