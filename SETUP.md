# Setup notes

Global shortcut: keycode F12 (physically Fn+Esc) → /home/filas/.local/bin/blackout-toggle
Bound via System Settings → Keyboard → Shortcuts → Add New → Command or Script.
Stored in ~/.config/kglobalshortcutsrc.

Reinstall wrapper after code changes:
  install -m 755 ~/Mods/blackout-screen/bin/blackout-toggle ~/.local/bin/blackout-toggle
