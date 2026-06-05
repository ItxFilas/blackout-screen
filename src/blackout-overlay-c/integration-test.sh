#!/usr/bin/env bash
# Layer 2 smoke test — run INSIDE a live KWin/Wayland session.
# Drives the running blackout-overlay daemon and checks the screen really goes
# black, then prints a checklist for human-only confirmations.
# NOT for CI: needs a graphical session and briefly flashes the screen black.
set -euo pipefail

PIDFILE="/run/user/$(id -u)/blackout-overlay.pid"
[ -r "$PIDFILE" ] || { echo "daemon not running (no $PIDFILE)"; exit 1; }
PID=$(cat "$PIDFILE")

shot() { spectacle -b -n -f -o "$1" >/dev/null 2>&1; }
mean() { identify -format '%[fx:mean.r],%[fx:mean.g],%[fx:mean.b]' "$1"; }

echo "== toggle ON =="
kill -USR1 "$PID"; sleep 0.4
shot /tmp/bo-on.png;  on=$(mean /tmp/bo-on.png)
echo "screen mean while shown: $on (expect 0,0,0)"

echo "== toggle OFF =="
kill -USR2 "$PID"; sleep 0.4
shot /tmp/bo-off.png; off=$(mean /tmp/bo-off.png)
echo "screen mean after hide: $off (expect non-zero)"

[ "$on"  = "0,0,0" ] || { echo "FAIL: screen was not black while shown"; exit 1; }
[ "$off" != "0,0,0" ] || { echo "FAIL: screen still black after hide"; exit 1; }
echo "PASS: automated pixel checks"

cat <<'CHECKLIST'

== Manual checklist (only a human can confirm) ==
  [ ] Blackout ON, press your toggle key            -> turns OFF
  [ ] Blackout ON, Alt+Tab                           -> does NOT switch windows
  [ ] Blackout ON, shove mouse to a screen edge      -> no edge glow
  [ ] Plug an external monitor, toggle ON            -> both screens black
  [ ] Blackout OFF, unplug a monitor, toggle a few x -> no crash
       (verify: systemctl --user is-active blackout-overlay)
CHECKLIST
