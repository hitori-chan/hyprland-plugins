# Native Wayland fixtures and standalone C++ regressions. Protocol glue and
# executables are generated locally and ignored by Git.
HL      ?= $(HOME)/repo/Hyprland
XML     := $(HL)/protocols/wlr-virtual-pointer-unstable-v1.xml
KXML    := $(HL)/protocols/virtual-keyboard-unstable-v1.xml
CXML    := $(HL)/subprojects/hyprland-protocols/protocols/hyprland-input-capture-v1.xml
DXML    := $(HL)/protocols/wlr-data-control-unstable-v1.xml
CFLAGS  += -O2 -Wall $(shell pkg-config --cflags wayland-client)
LDLIBS  += $(shell pkg-config --libs wayland-client)
KLIBS   := $(shell pkg-config --libs xkbcommon)
EICFLAGS := $(shell pkg-config --cflags libei-1.0)
EILIBS   := $(shell pkg-config --libs libei-1.0)
HYPR_CFLAGS := $(shell pkg-config --cflags hyprland)
SERVER_CFLAGS := $(shell pkg-config --cflags wayland-server)
SERVER_LIBS   := $(shell pkg-config --libs wayland-server)
TEST_CXXFLAGS := -std=c++26 -O2 -Wall -Wextra -Wpedantic -Werror -I..
TEST_BINS := icon-resolver-test battery-state-test desktop-exec-test \
	hyprosd-readback-test pixel-model-test persist-test fileindex-test \
	hyprsnap-geometry-test hyprmax-geometry-test
TEST_TARGETS := test-icon-resolver test-battery-state test-desktop-exec \
	test-hyprosd test-pixel-model test-persist test-fileindex \
	test-hyprsnap-geometry test-hyprmax-geometry

all: vptr vkbd input-capture cliphold

test: $(TEST_TARGETS)

icon-resolver-test: icon_resolver_test.cpp test.hpp ../common/icons.hpp
	$(CXX) $(TEST_CXXFLAGS) $< -o $@ $(shell pkg-config --libs hyprgraphics)

test-icon-resolver: icon-resolver-test
	./icon-resolver-test

battery-state-test: battery_state_test.cpp test.hpp ../hyprbar/battery_state.hpp
	$(CXX) $(TEST_CXXFLAGS) $< -o $@

test-battery-state: battery-state-test
	./battery-state-test

desktop-exec-test: desktop_exec_test.cpp test.hpp ../hyprbar/desktop_exec.hpp ../common/fileindex.hpp ../common/icons.hpp
	$(CXX) $(TEST_CXXFLAGS) $(SERVER_CFLAGS) $< -o $@ $(SERVER_LIBS)

test-desktop-exec: desktop-exec-test
	./desktop-exec-test

hyprosd-readback-test: hyprosd_readback_test.cpp test.hpp ../hyprosd/wpctl.hpp
	$(CXX) $(TEST_CXXFLAGS) $< -o $@

test-hyprosd: hyprosd-readback-test
	./hyprosd-readback-test

pixel-model-test: pixel_model_test.cpp test.hpp ../hyprnotify/pixel_model.hpp
	$(CXX) $(TEST_CXXFLAGS) $< -o $@

test-pixel-model: pixel-model-test
	./pixel-model-test

persist-test: persist_test.cpp test.hpp ../common/persist.hpp ../common/lifecycle.hpp
	$(CXX) $(TEST_CXXFLAGS) $(HYPR_CFLAGS) $< -o $@

test-persist: persist-test
	./persist-test

fileindex-test: fileindex_test.cpp test.hpp ../common/fileindex.hpp
	$(CXX) $(TEST_CXXFLAGS) $(SERVER_CFLAGS) $< -o $@ $(SERVER_LIBS)

test-fileindex: fileindex-test
	./fileindex-test

hyprsnap-geometry-test: hyprsnap_geometry_test.cpp test.hpp ../hyprsnap/geometry.hpp
	$(CXX) $(TEST_CXXFLAGS) $(HYPR_CFLAGS) $< -o $@

test-hyprsnap-geometry: hyprsnap-geometry-test
	./hyprsnap-geometry-test

hyprmax-geometry-test: hyprmax_geometry_test.cpp test.hpp ../hyprmax/geometry.hpp
	$(CXX) $(TEST_CXXFLAGS) $(HYPR_CFLAGS) $< -o $@

test-hyprmax-geometry: hyprmax-geometry-test
	./hyprmax-geometry-test

vptr: vptr.c vptr-proto.c vptr-proto.h
	$(CC) $(CFLAGS) vptr.c vptr-proto.c -o $@ $(LDLIBS)

vkbd: vkbd.c vkbd-proto.c vkbd-proto.h
	$(CC) $(CFLAGS) vkbd.c vkbd-proto.c -o $@ $(LDLIBS) $(KLIBS)

input-capture: input-capture.c input-capture-proto.c input-capture-proto.h
	$(CC) $(CFLAGS) $(EICFLAGS) input-capture.c input-capture-proto.c -o $@ $(LDLIBS) $(EILIBS)

cliphold: cliphold.c cliphold-proto.c cliphold-proto.h
	$(CC) $(CFLAGS) cliphold.c cliphold-proto.c -o $@ $(LDLIBS)

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

clean:
	rm -f vptr vkbd input-capture cliphold $(TEST_BINS) vptr-proto.h vptr-proto.c vkbd-proto.h vkbd-proto.c input-capture-proto.h input-capture-proto.c cliphold-proto.h cliphold-proto.c

.PHONY: all clean test $(TEST_TARGETS)
