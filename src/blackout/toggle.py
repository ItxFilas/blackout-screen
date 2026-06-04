import os
import signal
import subprocess
import sys
import time

from . import brightness, state

OVERLAY_PATH = os.path.join(os.path.dirname(__file__), "overlay.py")


def _kill(pid):
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass


def activate():
    saved = brightness.get_brightness()
    proc = subprocess.Popen([sys.executable, OVERLAY_PATH])
    brightness.set_brightness(0)
    state.write_state(proc.pid, saved)


def deactivate():
    s = state.read_state()
    if s:
        _kill(s["overlay_pid"])
        time.sleep(0.5)  # let overlay's SIGTERM handler re-enable the output
        brightness.set_brightness(s["saved_brightness"])
    state.clear_state()


def toggle():
    if state.is_active():
        deactivate()
    else:
        activate()
