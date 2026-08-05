# vptr / vkbd — virtual-pointer and virtual-keyboard injectors for driving
# input paths in the nested Hyprland dev instance. NOT plugins (not in
# hyprpm.toml); local dev tooling. Generates the protocol glue from the
# fork's vendored XML, then compiles. Only the .c files + this Makefile + the
# README are tracked; the binaries and the generated *-proto.{c,h} are build
# artifacts (see .gitignore).
HL      ?= $(HOME)/repo/Hyprland
XML     := $(HL)/protocols/wlr-virtual-pointer-unstable-v1.xml
KXML    := $(HL)/protocols/virtual-keyboard-unstable-v1.xml
CXML    := $(HL)/subprojects/hyprland-protocols/protocols/hyprland-input-capture-v1.xml
CFLAGS  += -O2 -Wall $(shell pkg-config --cflags wayland-client)
LDLIBS  += $(shell pkg-config --libs wayland-client)
KLIBS   := $(shell pkg-config --libs xkbcommon)
EICFLAGS := $(shell pkg-config --cflags libei-1.0)
EILIBS   := $(shell pkg-config --libs libei-1.0)
HYPR_CFLAGS := $(shell pkg-config --cflags hyprland)

all: vptr vkbd input-capture

icon-resolver-test: icon_resolver_test.cpp ../common/icons.hpp
	$(CXX) -std=c++26 -O2 -Wall -Wextra -Wpedantic -Werror -I.. $< -o $@

test-icon-resolver: icon-resolver-test
	./icon-resolver-test

battery-state-test: battery_state_test.cpp ../hyprbar/battery_state.hpp
	$(CXX) -std=c++26 -O2 -Wall -Wextra -Wpedantic -Werror -I.. $< -o $@

test-battery-state: battery-state-test
	./battery-state-test

desktop-exec-test: desktop_exec_test.cpp ../hyprbar/desktop_exec.hpp ../common/fileindex.hpp ../common/icons.hpp
	$(CXX) -std=c++26 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -I.. $< -o $@

test-desktop-exec: desktop-exec-test
	./desktop-exec-test

hyprosd-readback-test: hyprosd_readback_test.cpp ../hyprosd/wpctl.hpp
	$(CXX) -std=c++26 -O2 -Wall -Wextra -Wpedantic -Werror -I.. $< -o $@

test-hyprosd: hyprosd-readback-test
	./hyprosd-readback-test

persist-test: persist_test.cpp ../common/persist.hpp ../common/lifecycle.hpp
	$(CXX) -std=c++26 -O2 -Wall -Wextra -Wpedantic -Werror -I.. $(HYPR_CFLAGS) $< -o $@

test-persist: persist-test
	./persist-test

hyprsnap-geometry-test: hyprsnap_geometry_test.cpp ../hyprsnap/geometry.hpp
	$(CXX) -std=c++26 -O2 -Wall -Wextra -Wpedantic -Werror -I.. $(HYPR_CFLAGS) $< -o $@

test-hyprsnap-geometry: hyprsnap-geometry-test
	./hyprsnap-geometry-test

hyprmax-geometry-test: hyprmax_geometry_test.cpp ../hyprmax/geometry.hpp
	$(CXX) -std=c++26 -O2 -Wall -Wextra -Wpedantic -Werror -I.. $(HYPR_CFLAGS) $< -o $@

test-hyprmax-geometry: hyprmax-geometry-test
	./hyprmax-geometry-test

vptr: vptr.c vptr-proto.c vptr-proto.h
	$(CC) $(CFLAGS) vptr.c vptr-proto.c -o $@ $(LDLIBS)

vkbd: vkbd.c vkbd-proto.c vkbd-proto.h
	$(CC) $(CFLAGS) vkbd.c vkbd-proto.c -o $@ $(LDLIBS) $(KLIBS)

input-capture: input-capture.c input-capture-proto.c input-capture-proto.h
	$(CC) $(CFLAGS) $(EICFLAGS) input-capture.c input-capture-proto.c -o $@ $(LDLIBS) $(EILIBS)

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

clean:
	rm -f vptr vkbd input-capture icon-resolver-test battery-state-test desktop-exec-test hyprosd-readback-test persist-test hyprsnap-geometry-test hyprmax-geometry-test vptr-proto.h vptr-proto.c vkbd-proto.h vkbd-proto.c input-capture-proto.h input-capture-proto.c

.PHONY: all clean test-icon-resolver test-battery-state test-desktop-exec test-hyprosd test-persist test-hyprsnap-geometry test-hyprmax-geometry
