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
PKGS_EXTRA := librsvg-2.0 libudev pangocairo
LIBS_EXTRA := sdbus-c++ librsvg-2.0 libudev pangocairo

PKG_CONFIG_PATH := $(if $(PKG_CONFIG_PATH),$(PKG_CONFIG_PATH):)/usr/local/share/pkgconfig
export PKG_CONFIG_PATH

CXX       ?= g++
CXXFLAGS  ?= -O2
HL_CFLAGS := $(shell pkg-config --cflags $(PKGS) $(PKGS_EXTRA))
CXXFLAGS  += -std=c++26 -Wall -fPIC -fno-gnu-unique -MMD -MP -I. $(HL_CFLAGS)
LDFLAGS   += -shared -Wl,--version-script=plugin.ver
LDLIBS    += $(shell pkg-config --libs $(LIBS_EXTRA))

OBJDIR := .build
# one unit per file; subdirectory units (windows/, notify/, shell/,
# system/) build to the same tree so their object names stay unique
OBJS   := $(patsubst %.cpp,$(OBJDIR)/%.o,$(shell find . -name '*.cpp' -not -path './.build/*' -not -path './test/*'))

all: $(NAME).so

$(NAME).so: $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)

# one object per unit; -MMD keeps the incremental build honest (the same
# GCC-16 installed-header gap as the old build: `make -B` after a fork
# install)
$(OBJDIR)/%.o: %.cpp Makefile plugin.ver | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR):
	@mkdir -p $@

-include $(shell find $(OBJDIR) -name '*.d' 2>/dev/null)

# the headless unit harness (pure core; no fork headers, no compositor)
test:
	g++ -std=c++26 -Wall -I.. -I. test/main.cpp test/test_bounded.cpp test/test_box.cpp test/test_store.cpp test/test_schema.cpp test/test_wpctl.cpp test/test_desktop_exec.cpp test/stubs.cpp core/bounded.hpp core/store.cpp core/state.cpp core/schema.cpp -o .build/test
	.build/test

# the gate's preflight resolves the header root through this
print-hl-cflags:
	@echo $(HL_CFLAGS)

# the integration gate (nested compositor; see gate/gate.sh): the plugin's
# behavioral contract. Unit tests are `make test`; the gate is the rest.
# ARGS passes through to gate.sh (bin, -b/-k selection).
gate: all
	bash gate/gate.sh $(ARGS)

clean:
	rm -rf $(OBJDIR) $(NAME).so

.PHONY: all test gate clean print-hl-cflags
