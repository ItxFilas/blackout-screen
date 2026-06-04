#!/usr/bin/env python3
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gdk, Gtk  # noqa: E402

CSS = b"window { background-color: #000000; }"


def _apply_black_css():
    provider = Gtk.CssProvider()
    provider.load_from_data(CSS)
    Gtk.StyleContext.add_provider_for_screen(
        Gdk.Screen.get_default(),
        provider,
        Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
    )


def _hide_cursor(win):
    gdk_win = win.get_window()
    if gdk_win is not None:
        blank = Gdk.Cursor.new_for_display(
            win.get_display(), Gdk.CursorType.BLANK_CURSOR
        )
        gdk_win.set_cursor(blank)


def _make_window(screen, monitor_index):
    win = Gtk.Window(type=Gtk.WindowType.TOPLEVEL)
    win.set_decorated(False)
    win.set_app_paintable(True)
    win.connect("destroy", Gtk.main_quit)
    win.connect("realize", lambda w: _hide_cursor(w))
    win.show_all()
    win.fullscreen_on_monitor(screen, monitor_index)
    return win


def main():
    _apply_black_css()
    screen = Gdk.Screen.get_default()
    display = Gdk.Display.get_default()
    count = display.get_n_monitors()
    windows = [_make_window(screen, i) for i in range(count)]  # noqa: F841
    Gtk.main()


if __name__ == "__main__":
    main()
