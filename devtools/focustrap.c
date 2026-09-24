// focustrap.c — X11 focus-steal probe for the stress gate.
//
// The EWMH regression this guards: X11 clients cannot authenticate a user
// gesture. Wine/Proton apps (the GOG installer, games) send
// _NET_ACTIVE_WINDOW on every internal SetForegroundWindow and map
// FlashWindowEx to _NET_WM_STATE_DEMANDS_ATTENTION. Under
// misc:focus_on_activate the compositor used to take focus for each ping —
// the XWM now maps both to urgency only. The probe maps a window, waits
// (the gate moves keyboard focus to a Wayland window), then sends one ping
// of the requested kind and holds, so the gate can assert the focus stayed
// and the socket2 `urgent` event named this window.
//
// usage: focustrap <map|attention|activate> [delay-s] [hold-s]
//   map      — map only, no ping (baseline: map-time focus is allowed)
//   attention — _NET_WM_STATE ADD _NET_WM_STATE_DEMANDS_ATTENTION
//   activate  — _NET_ACTIVE_WINDOW (source = application)

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    const char* mode   = argc > 1 ? argv[1] : "attention";
    int         delay  = argc > 2 ? atoi(argv[2]) : 6;
    int         hold   = argc > 3 ? atoi(argv[3]) : 18;

    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "focustrap: no X display\n");
        return 1;
    }

    int  scr       = DefaultScreen(dpy);
    Atom netWmState = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom demands    = XInternAtom(dpy, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
    Atom netActive  = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);

    Window w = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 900, 500, 90, 60, 1, 0xff305070, 0xffb0d0ff);
    XStoreName(dpy, w, "focustrap");
    XClassHint ch;
    ch.res_name  = (char*)"focustrap";
    ch.res_class = (char*)"focustrap";
    XSetClassHint(dpy, w, &ch);
    XMapWindow(dpy, w);
    XFlush(dpy);
    fprintf(stderr, "focustrap: mapped 0x%lx pid %d\n", (unsigned long)w, getpid());

    for (int i = 0; i < 100; i++) {
        if (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == MapNotify && ev.xmap.window == w)
                break;
        } else
            usleep(50 * 1000);
    }

    for (int i = 0; i < delay * 20; i++) {
        if (XPending(dpy))
            XNextEvent(dpy, &(XEvent){0});
        else
            usleep(50 * 1000);
    }

    if (strcmp(mode, "map") != 0) {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type       = ClientMessage;
        ev.xclient.window     = w; // the target XWM reads, as on a real EWMH ping
        ev.xclient.format     = 32;
        if (strcmp(mode, "activate") == 0) {
            ev.xclient.message_type = netActive;
            ev.xclient.data.l[0]    = 2; // source: application
            ev.xclient.data.l[1]    = CurrentTime;
        } else {
            ev.xclient.message_type = netWmState;
            ev.xclient.data.l[0]    = 1; // _NET_WM_STATE_ADD
            ev.xclient.data.l[1]    = demands;
            ev.xclient.data.l[3]    = CurrentTime;
        }
        XSendEvent(dpy, RootWindow(dpy, scr), False, SubstructureNotifyMask | SubstructureRedirectMask, &ev);
        XFlush(dpy);
        fprintf(stderr, "focustrap: sent %s\n", mode);
    }

    for (int i = 0; i < hold * 20; i++) {
        if (XPending(dpy))
            XNextEvent(dpy, &(XEvent){0});
        else
            usleep(50 * 1000);
    }

    XCloseDisplay(dpy);
    return 0;
}
