#!/usr/bin/env python3
import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gtk  # noqa: E402


def _on_draw(area, cr, width, height):
    cr.set_source_rgb(0, 0, 0)
    cr.paint()


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
        win.fullscreen_on_monitor(monitor)
        win.present()


app = Gtk.Application(application_id="com.local.blackout")
app.connect("activate", _on_activate)
app.run([])
