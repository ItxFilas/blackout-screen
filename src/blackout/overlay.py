#!/usr/bin/env python3
"""Disables the display output for AMOLED pixel-off blackout. Re-enables on SIGTERM."""
import signal
import subprocess
import sys


def _discover_output():
    r = subprocess.run(["kscreen-doctor", "-o"], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        if "eDP" in line and "Output" in line:
            for word in line.split():
                if word.startswith("eDP"):
                    return word
    return "eDP-1"


_OUTPUT = _discover_output()
subprocess.run(["kscreen-doctor", f"output.{_OUTPUT}.disable"], check=False)


def _on_term(sig, frame):
    subprocess.run(["kscreen-doctor", f"output.{_OUTPUT}.enable"], check=False)
    sys.exit(0)


signal.signal(signal.SIGTERM, _on_term)
signal.pause()
