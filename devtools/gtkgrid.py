#!/usr/bin/env python3
# gtkgrid — a real GTK3 client as a render-vs-input oracle: a fixed-size
# (floating) CSD window of solid color blocks and a menu button whose
# GtkMenu is an xdg_popup of colored items. Every press prints what GTK
# thinks it hit, so a click on what is DRAWN must name the same block.
# usage: gtkgrid.py <log> [wayland|x11] [wayland-debug-log]
#   log lines: READY | HIT <color> | MENU <color> | MENUBUTTON True|False
import os
import sys

LOG = sys.argv[1]
os.dup2(os.open(LOG, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644), 1)
os.environ["GDK_BACKEND"] = sys.argv[2] if len(sys.argv) > 2 else "wayland"
if len(sys.argv) > 3:  # protocol trace: the popup's configures
    os.dup2(os.open(sys.argv[3], os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644), 2)
    os.environ["WAYLAND_DEBUG"] = "client"

import gi  # noqa: E402

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

GLib.set_prgname("gtkgrid")
BLOCKS = [("red", (1, 0, 0)), ("green", (0, 1, 0)), ("blue", (0, 0, 1)),
          ("yellow", (1, 1, 0)), ("magenta", (1, 0, 1)), ("cyan", (0, 1, 1))]
ITEMS = [("orange", (1, .5, 0)), ("violet", (.5, 0, 1)), ("mint", (0, 1, .5))]


def say(*a):
    print(*a, flush=True)


def swatch(name, rgb, w, h, press=True):
    da = Gtk.DrawingArea()
    da.set_size_request(w, h)
    da.connect("draw", lambda _w, cr: (cr.set_source_rgb(*rgb), cr.paint(), False)[-1])
    if press:
        da.add_events(Gdk.EventMask.BUTTON_PRESS_MASK)
        da.connect("button-press-event", lambda *_: (say("HIT", name), True)[-1])
    return da


win = Gtk.Window(title="gtkgrid")
win.set_resizable(False)
win.set_titlebar(Gtk.HeaderBar(title="gtkgrid", show_close_button=True))
box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
grid = Gtk.Grid(column_spacing=6, row_spacing=6)
for i, (n, c) in enumerate(BLOCKS):
    grid.attach(swatch(n, c, 120, 80), i % 3, i // 3, 1, 1)
box.pack_start(grid, False, False, 0)
menu = Gtk.Menu()
for n, c in ITEMS:
    item = Gtk.MenuItem()
    item.add(swatch(n, c, 140, 30, press=False))
    item.connect("activate", lambda _w, n=n: say("MENU", n))
    menu.append(item)
menu.show_all()
button = Gtk.MenuButton(popup=menu)
button.remove(button.get_child())
button.add(swatch("button", (.5, .25, 0), 120, 30, press=False))
button.connect("toggled", lambda b: say("MENUBUTTON", b.get_active()))
box.pack_start(button, False, False, 0)
win.add(box)
win.connect("destroy", Gtk.main_quit)
win.show_all()
say("READY")
Gtk.main()
