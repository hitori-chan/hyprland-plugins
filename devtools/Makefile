# Shared input/window fixtures for the plugin's nested gate (awesome/gate).
# Protocol glue and executables are generated locally and ignored by Git.
HL     ?= $(HOME)/repo/Hyprland
XML    := $(HL)/protocols/wlr-virtual-pointer-unstable-v1.xml
KXML   := $(HL)/protocols/virtual-keyboard-unstable-v1.xml
CXML   := $(HL)/subprojects/hyprland-protocols/protocols/hyprland-input-capture-v1.xml
DXML   := $(HL)/protocols/wlr-data-control-unstable-v1.xml
XXML   := /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
CFLAGS += -O2 -Wall $(shell pkg-config --cflags wayland-client)
LDLIBS += $(shell pkg-config --libs wayland-client)
KLIBS  := $(shell pkg-config --libs xkbcommon)
EICFLAGS := $(shell pkg-config --cflags libei-1.0)
EILIBS   := $(shell pkg-config --libs libei-1.0)

all: vptr vkbd input-capture cliphold fixwin splashwin fake-sni

vptr: vptr.c vptr-proto.c vptr-proto.h
	$(CC) $(CFLAGS) vptr.c vptr-proto.c -o $@ $(LDLIBS)

vkbd: vkbd.c vkbd-proto.c vkbd-proto.h
	$(CC) $(CFLAGS) vkbd.c vkbd-proto.c -o $@ $(LDLIBS) $(KLIBS)

input-capture: input-capture.c input-capture-proto.c input-capture-proto.h
	$(CC) $(CFLAGS) $(EICFLAGS) input-capture.c input-capture-proto.c -o $@ $(LDLIBS) $(EILIBS)

cliphold: cliphold.c cliphold-proto.c cliphold-proto.h
	$(CC) $(CFLAGS) cliphold.c cliphold-proto.c -o $@ $(LDLIBS)

splashwin: splashwin.c fixwin-protocol.h fixwin-proto.c
	$(CC) $(CFLAGS) splashwin.c fixwin-proto.c -o $@ $(LDLIBS)

fixwin: fixwin.c fixwin-protocol.h fixwin-proto.c
	$(CC) $(CFLAGS) fixwin.c fixwin-proto.c -o $@ $(LDLIBS)

# SNI + dbusmenu fixture over libdbus (not a compositor client): the tray
# menu battery drives it against the nested instance's private session bus.
fake-sni: fake-sni.c
	$(CC) -O2 -Wall $< -o $@ $(shell pkg-config --cflags --libs dbus-1)

vptr-proto.h: $(XML)
	wayland-scanner client-header $< $@
vptr-proto.c: $(XML)
	wayland-scanner private-code $< $@
vkbd-proto.h: $(KXML)
	wayland-scanner client-header $< $@
vkbd-proto.c: $(KXML)
	wayland-scanner private-code $< $@
input-capture-proto.h: $(CXML)
	wayland-scanner client-header $< $@
input-capture-proto.c: $(CXML)
	wayland-scanner private-code $< $@
cliphold-proto.h: $(DXML)
	wayland-scanner client-header $< $@
cliphold-proto.c: $(DXML)
	wayland-scanner private-code $< $@
fixwin-protocol.h: $(XXML)
	wayland-scanner client-header $< $@
fixwin-proto.c: $(XXML)
	wayland-scanner private-code $< $@

clean:
	rm -f vptr vkbd input-capture cliphold fixwin splashwin fake-sni vptr-proto.h vptr-proto.c vkbd-proto.h vkbd-proto.c input-capture-proto.h input-capture-proto.c cliphold-proto.h cliphold-proto.c fixwin-protocol.h fixwin-proto.c

.PHONY: all clean
