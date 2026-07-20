# vptr — wlr-virtual-pointer injector for driving mouse/drag paths in the
# nested Hyprland dev instance. NOT a plugin (not in hyprpm.toml); local dev
# tooling. Generates the protocol glue from the fork's vendored XML, then
# compiles. Only vptr.c + this Makefile + the README are tracked; the binary
# and the generated vptr-proto.{c,h} are build artifacts (see .gitignore).
HL      ?= $(HOME)/repo/Hyprland
XML     := $(HL)/protocols/wlr-virtual-pointer-unstable-v1.xml
CFLAGS  += -O2 -Wall $(shell pkg-config --cflags wayland-client)
LDLIBS  += $(shell pkg-config --libs wayland-client)

vptr: vptr.c vptr-proto.c vptr-proto.h
	$(CC) $(CFLAGS) vptr.c vptr-proto.c -o $@ $(LDLIBS)

vptr-proto.h: $(XML)
	wayland-scanner client-header $< $@
vptr-proto.c: $(XML)
	wayland-scanner private-code $< $@

clean:
	rm -f vptr vptr-proto.h vptr-proto.c

.PHONY: clean
