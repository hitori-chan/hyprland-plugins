# vptr / vkbd — virtual-pointer and virtual-keyboard injectors for driving
# input paths in the nested Hyprland dev instance. NOT plugins (not in
# hyprpm.toml); local dev tooling. Generates the protocol glue from the
# fork's vendored XML, then compiles. Only the .c files + this Makefile + the
# README are tracked; the binaries and the generated *-proto.{c,h} are build
# artifacts (see .gitignore).
HL      ?= $(HOME)/repo/Hyprland
XML     := $(HL)/protocols/wlr-virtual-pointer-unstable-v1.xml
KXML    := $(HL)/protocols/virtual-keyboard-unstable-v1.xml
CFLAGS  += -O2 -Wall $(shell pkg-config --cflags wayland-client)
LDLIBS  += $(shell pkg-config --libs wayland-client)
KLIBS   := $(shell pkg-config --libs xkbcommon)

all: vptr vkbd

vptr: vptr.c vptr-proto.c vptr-proto.h
	$(CC) $(CFLAGS) vptr.c vptr-proto.c -o $@ $(LDLIBS)

vkbd: vkbd.c vkbd-proto.c vkbd-proto.h
	$(CC) $(CFLAGS) vkbd.c vkbd-proto.c -o $@ $(LDLIBS) $(KLIBS)

vptr-proto.h: $(XML)
	wayland-scanner client-header $< $@
vptr-proto.c: $(XML)
	wayland-scanner private-code $< $@
vkbd-proto.h: $(KXML)
	wayland-scanner client-header $< $@
vkbd-proto.c: $(KXML)
	wayland-scanner private-code $< $@

clean:
	rm -f vptr vkbd vptr-proto.h vptr-proto.c vkbd-proto.h vkbd-proto.c

.PHONY: all clean
