# awesome — the unified shell plugin.
#
# Standalone build: same header discipline — an inherited PKG_CONFIG_PATH
# wins
# (hyprpm's header cache, the gate's scratch headers), and with no fork
# installed this falls through to the installed prefix, which must be the
# fork's (/usr/local).
#
# Dependencies: Hyprland itself, plus sdbus-c++ (the fd.o notification
# daemon), librsvg (icons), libudev
# (battery uevents), and pangocairo (text) which Hyprland's .pc does not
# carry.
NAME       := awesome
PKGS       := hyprland pixman-1 libdrm
# libeis: the input-capture protocol header includes it, Hyprland's .pc
# does not carry it
PKGS_EXTRA := librsvg-2.0 libudev pangocairo libeis-1.0
LIBS_EXTRA := sdbus-c++ librsvg-2.0 libudev pangocairo

GATE_PKG_CONFIG_PATH := $(PKG_CONFIG_PATH)
PKG_CONFIG_PATH := $(if $(PKG_CONFIG_PATH),$(PKG_CONFIG_PATH):)/usr/local/share/pkgconfig
export PKG_CONFIG_PATH

CXX       ?= g++
CXXFLAGS  ?= -O2
HL_CFLAGS := $(shell pkg-config --cflags $(PKGS) $(PKGS_EXTRA))
CXXFLAGS  += -std=c++26 -Wall -fPIC -fno-gnu-unique -MMD -MP -I. $(HL_CFLAGS)
LDFLAGS   += -shared -Wl,--version-script=plugin.ver
LDLIBS    += $(shell pkg-config --libs $(LIBS_EXTRA))

# GATE=1 is the gate's variant (awesome-gate.so, loaded by the nested): its
# test seams compile in under AWESOME_GATE, from objects of its own. The
# release build — hyprpm's, and the gate's deploy rehearsal — never has
# them.
ifeq ($(GATE),1)
CXXFLAGS += -DAWESOME_GATE
OBJDIR   := .build-gate
TARGET   := $(NAME)-gate.so
else
OBJDIR   := .build
TARGET   := $(NAME).so
endif
# one unit per file; subdirectory units (windows/, notify/, shell/,
# system/) build to the same tree so their object names stay unique
OBJS   := $(patsubst %.cpp,$(OBJDIR)/%.o,$(shell find . -name '*.cpp' -not -path './.build*' -not -path './test/*'))

# The build's target: compiler and flags (they carry the header roots).
# Every object depends on this stamp, rewritten only when it changes, so
# another header root (the gate's staged set vs the installed one)
# rebuilds everything. Under a system include dir (/usr/local/include) a
# fork installed over the same root is invisible to -MMD — GCC never lists
# system headers in the .d files — so there the fork's version.h joins the
# stamp. A staged set is tracked header by header (cmake --install keeps
# the sources' mtimes): a fork change that touches no header rebuilds only
# what includes version.h.
HL_VERSION_H := $(firstword $(wildcard $(patsubst -I%,%/hyprland/src/version.h,$(filter -I%,$(HL_CFLAGS)))))
STAMP        := $(OBJDIR)/target.stamp
STAMP_TEXT   := $(CXX) $(CXXFLAGS) $(if $(filter /usr/%,$(HL_VERSION_H)),$(shell cksum < $(HL_VERSION_H)))

all: $(TARGET)

$(STAMP): FORCE | $(OBJDIR)
	@echo '$(STAMP_TEXT)' | cmp -s - $@ || echo '$(STAMP_TEXT)' > $@

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)

# one object per unit; -MMD tracks the plugin's own headers, the stamp
# the target's
$(OBJDIR)/%.o: %.cpp Makefile plugin.ver $(STAMP) | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR):
	@mkdir -p $@

-include $(shell find $(OBJDIR) -name '*.d' 2>/dev/null)

# the headless unit harness (pure core; no fork headers, no compositor),
# incremental: its own objects, its own dir
TEST_SRCS := $(wildcard test/*.cpp) core/store.cpp core/state.cpp core/schema.cpp
TEST_OBJS := $(patsubst %.cpp,.build-test/%.o,$(TEST_SRCS))

.build-test/%.o: %.cpp Makefile
	@mkdir -p $(dir $@)
	$(CXX) -std=c++26 -Wall -MMD -MP -I.. -I. -c $< -o $@

.build-test/awesome-test: $(TEST_OBJS)
	$(CXX) $(TEST_OBJS) -o $@

test: .build-test/awesome-test
	.build-test/awesome-test

-include $(shell find .build-test -name '*.d' 2>/dev/null)

# the gate's preflight resolves the header root through this
print-hl-cflags:
	@echo $(HL_CFLAGS)

# the integration gate (nested compositor; see gate/gate.sh): the plugin's
# behavioral contract. Unit tests are `make test`; the gate is the rest.
# ARGS passes through to gate.sh (-s fork dir, bin, -b/-k selection). The
# gate builds the plugin itself, against the caller's exact header set:
# hand it the caller's PKG_CONFIG_PATH, not this file's installed-prefix
# fallback (the preflight refuses a mixed path).
gate:
	env PKG_CONFIG_PATH="$(GATE_PKG_CONFIG_PATH)" bash gate/gate.sh $(ARGS)

clean:
	rm -rf .build .build-gate .build-test $(NAME).so $(NAME)-gate.so

.PHONY: all test gate clean print-hl-cflags FORCE
