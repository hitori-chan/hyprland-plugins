# common/common.mk — the shared plugin build. A plugin's Makefile sets NAME
# (and PKGS_EXTRA / LIBS_EXTRA for what it links beyond Hyprland) and
# includes this file.
#
# plugin.ver localizes the plugin's own NHypr* namespace symbols — the
# dlopen collision surface — while leaving everything else default-visible:
# Hyprland's globals are inline variables in headers, and the plugin's copy
# must stay exported to unify with the compositor's at dlopen (see
# plugin.ver; blanket -fvisibility=hidden nulls them and SEGVs).

# WHICH Hyprland's headers. The fork installs under /usr/local; a distro
# hyprland package puts a DIFFERENT compositor's headers in /usr. With both
# present the two trees mix in one translation unit — `#include
# <hyprland/...>` resolves through /usr/local/include (a default system dir,
# searched before /usr/include) while the stock .pc's -I/usr/include/hyprland
# answers the relative includes inside those same headers. That compiles only
# for as long as the two trees are byte-identical, because GCC's `#pragma
# once` dedups files by CONTENT; the moment the distro moves ahead, every
# shared class is redefined. So put the fork's prefix on the search path: it
# owns both halves of the resolution. An inherited PKG_CONFIG_PATH still wins
# (hyprpm's own header cache, the gate's scratch headers), and with no fork
# installed this falls through to the distro's .pc.
PKG_CONFIG_PATH := $(if $(PKG_CONFIG_PATH),$(PKG_CONFIG_PATH):)/usr/local/share/pkgconfig
export PKG_CONFIG_PATH

CXX       ?= g++
CXXFLAGS  ?= -O2
PKGS      := hyprland pixman-1 libdrm pangocairo $(PKGS_EXTRA)
# resolved once, not per translation unit
HL_CFLAGS := $(shell pkg-config --cflags $(PKGS))
CXXFLAGS  += -std=c++26 -Wall -fPIC -fno-gnu-unique -MMD -MP -I../ $(HL_CFLAGS)
LDFLAGS   += -shared -Wl,--version-script=../common/plugin.ver
LDLIBS    := $(if $(LIBS_EXTRA),$(shell pkg-config --libs $(LIBS_EXTRA)))

# One object per unit so an edit rebuilds its own file and `make -j` can use
# the other cores — a full plugin is ~14 TUs of Hyprland headers, and doing
# them one behind another was the whole of the edit/gate loop's wait.
OBJDIR := .build
OBJS   := $(patsubst %.cpp,$(OBJDIR)/%.o,$(wildcard *.cpp))

all: $(NAME).so

$(NAME).so: $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)

# -MMD writes each unit's header list beside its object, so a common/ or
# third-party header change rebuilds exactly what included it; the makefiles
# are a dependency too, since flags decide the codegen just as much.
# GCC 16's -MMD omits headers under /usr/local/include (the installed
# fork headers) even though they are -I'd, so installed-header changes never
# trigger an incremental rebuild — after installing a new fork use `make -B`
# (the gate does).
$(OBJDIR)/%.o: %.cpp Makefile ../common/common.mk | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR):
	@mkdir -p $@

-include $(wildcard $(OBJDIR)/*.d)

clean:
	rm -rf $(NAME).so $(OBJDIR)

# devtools/stress.sh's preflight checks the headers against the binary it is
# about to gate. It asks here rather than resolving its own, so it can never
# vouch for a tree the plugins were not compiled against.
print-hl-cflags:
	@echo '$(HL_CFLAGS)'

.PHONY: all clean print-hl-cflags
