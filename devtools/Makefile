# Native Wayland fixtures and standalone C++ regressions. Protocol glue and
# executables are generated locally and ignored by Git.
HL      ?= $(HOME)/repo/Hyprland
CXML    := $(HL)/subprojects/hyprland-protocols/protocols/hyprland-input-capture-v1.xml
XXML    := /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
CFLAGS  += -O2 -Wall $(shell pkg-config --cflags wayland-client)
LDLIBS  += $(shell pkg-config --libs wayland-client)
EICFLAGS := $(shell pkg-config --cflags libei-1.0)
EILIBS   := $(shell pkg-config --libs libei-1.0)
HYPR_CFLAGS := $(shell pkg-config --cflags hyprland)
SERVER_CFLAGS := $(shell pkg-config --cflags wayland-server)
SERVER_LIBS   := $(shell pkg-config --libs wayland-server)
TEST_CXXFLAGS := -std=c++26 -O2 -Wall -Wextra -Wpedantic -Werror -I..
TEST_BINS := icon-resolver-test battery-state-test desktop-exec-test \
	hyprosd-readback-test persist-test fileindex-test \
	hyprsnap-geometry-test hyprmax-geometry-test pixel-model-test
TEST_TARGETS := test-icon-resolver test-battery-state test-desktop-exec \
	test-hyprosd test-persist test-fileindex \
	test-hyprsnap-geometry test-hyprmax-geometry test-pixel-model

# Rust fixture clients (awesome-rust plan, Track D): same names, same CLI.
# As a client is ported it moves from the C rules to RUST_BINS.
RUST_BINS := vptr focustrap vkbd cliphold
.PHONY: rust

all: $(RUST_BINS) input-capture fixwin splashwin fake-sni

rust:
	cargo build --release

$(RUST_BINS): rust
	cp target/release/$@ $@

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

persist-test: persist_test.cpp test.hpp ../common/persist.hpp ../common/lifecycle.hpp
	$(CXX) $(TEST_CXXFLAGS) $(HYPR_CFLAGS) $< -o $@

test-persist: persist-test
	./persist-test

pixel-model-test: pixel_model_test.cpp test.hpp ../hyprnotify/pixel_model.hpp
	$(CXX) $(TEST_CXXFLAGS) $< -o $@

test-pixel-model: pixel-model-test
	./pixel-model-test

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

input-capture: input-capture.c input-capture-proto.c input-capture-proto.h
	$(CC) $(CFLAGS) $(EICFLAGS) input-capture.c input-capture-proto.c -o $@ $(LDLIBS) $(EILIBS)

splashwin: splashwin.c fixwin-protocol.h fixwin-proto.c
	$(CC) $(CFLAGS) splashwin.c fixwin-proto.c -o $@ $(LDLIBS)

fixwin: fixwin.c fixwin-protocol.h fixwin-proto.c
	$(CC) $(CFLAGS) fixwin.c fixwin-proto.c -o $@ $(LDLIBS)

# SNI + dbusmenu fixture over libdbus (not a compositor client): the tray
# menu battery drives it against the nested instance's private session bus.
fake-sni: fake-sni.c
	$(CC) -O2 -Wall $< -o $@ $(shell pkg-config --cflags --libs dbus-1)

input-capture-proto.h: $(CXML)
	wayland-scanner client-header $< $@
input-capture-proto.c: $(CXML)
	wayland-scanner private-code $< $@
fixwin-protocol.h: $(XXML)
	wayland-scanner client-header $< $@
fixwin-proto.c: $(XXML)
	wayland-scanner private-code $< $@

clean:
	rm -f $(RUST_BINS) input-capture fixwin splashwin fake-sni $(TEST_BINS) input-capture-proto.h input-capture-proto.c fixwin-protocol.h fixwin-proto.c

.PHONY: all clean test $(TEST_TARGETS)
