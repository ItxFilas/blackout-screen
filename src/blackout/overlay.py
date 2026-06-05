#!/usr/bin/env python3
import os
import signal
import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, GLibUnix, Gtk

_PID_FILE = f"/run/user/{os.getuid()}/blackout-overlay.pid"
_windows = []  # [(win, monitor), ...]


def _on_draw(area, cr, width, height):
    cr.set_source_rgb(0, 0, 0)
    cr.paint()


def _show(*_):
    for win, monitor in _windows:
        win.fullscreen_on_monitor(monitor)
        win.present()
    return GLib.SOURCE_CONTINUE


def _hide(*_):
    for win, _ in _windows:
        win.hide()
    return GLib.SOURCE_CONTINUE


def _on_activate(app):
    display = Gdk.Display.get_default()
    monitors = display.get_monitors()
    for i in range(monitors.get_n_items()):
        monitor = monitors.get_item(i)
        win = Gtk.ApplicationWindow(application=app)
        win.set_decorated(False)
        win.set_cursor(Gdk.Cursor.new_from_name("none", None))
        area = Gtk.DrawingArea()
        area.set_draw_func(_on_draw)
        win.set_child(area)
        _windows.append((win, monitor))

    app.hold()
    GLibUnix.signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR1, _show)
    GLibUnix.signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR2, _hide)

    with open(_PID_FILE, "w") as f:
        f.write(str(os.getpid()))


app = Gtk.Application(application_id="com.local.blackout")
app.connect("activate", _on_activate)
try:
    app.run([])
finally:
    try:
        os.unlink(_PID_FILE)
    except FileNotFoundError:
        pass
