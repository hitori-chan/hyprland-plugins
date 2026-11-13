# vclick — click, scroll, drag or pinch inside a window without the user's pointer.
#
# Standalone build: an inherited PKG_CONFIG_PATH wins (hyprpm's header
# cache), else the installed prefix, which must be the fork's (/usr/local).
NAME := vclick
PKGS := hyprland pixman-1 libdrm

PKG_CONFIG_PATH := $(if $(PKG_CONFIG_PATH),$(PKG_CONFIG_PATH):)/usr/local/share/pkgconfig
export PKG_CONFIG_PATH

CXX       ?= g++
CXXFLAGS  ?= -O2
HL_CFLAGS := $(shell pkg-config --cflags $(PKGS))
CXXFLAGS  += -std=c++26 -Wall -fPIC -fno-gnu-unique -MMD -MP $(HL_CFLAGS)
LDFLAGS   += -shared -Wl,--version-script=plugin.ver

OBJDIR := .build
OBJS   := $(patsubst %.cpp,$(OBJDIR)/%.o,$(wildcard *.cpp))

# The build's target: compiler, flags (they carry the header root) and the
# headers' version.h. The stamp is rewritten only when that changes, so
# another header set or an installed fork rebuilds by itself (-MMD never
# lists a system header such as /usr/local/include's).
HL_VERSION_H := $(firstword $(wildcard $(patsubst -I%,%/hyprland/src/version.h,$(filter -I%,$(HL_CFLAGS)))))
STAMP        := $(OBJDIR)/target.stamp
STAMP_TEXT   := $(CXX) $(CXXFLAGS) $(if $(HL_VERSION_H),$(shell cksum < $(HL_VERSION_H)))

all: $(NAME).so

$(OBJDIR):
	@mkdir -p $@

$(STAMP): FORCE | $(OBJDIR)
	@echo '$(STAMP_TEXT)' | cmp -s - $@ || echo '$(STAMP_TEXT)' > $@

$(OBJDIR)/%.o: %.cpp $(STAMP) | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# the linker unlinks its output first: a loaded .so keeps its old inode
$(NAME).so: $(OBJS) plugin.ver
	$(CXX) $(LDFLAGS) $(OBJS) -o $@

# the units without a compositor, headless: the path's math (only
# hyprutils) and the cursor theme reader
TESTDIR := .build-test
test: $(TESTDIR)/path $(TESTDIR)/theme
	$(TESTDIR)/path
	$(TESTDIR)/theme

$(TESTDIR)/path: test/path.cpp path.cpp path.hpp
	@mkdir -p $(TESTDIR)
	$(CXX) -std=c++26 -O2 -Wall -I. test/path.cpp path.cpp -o $@ $(shell pkg-config --libs hyprutils)

$(TESTDIR)/theme: test/theme.cpp theme.cpp theme.hpp
	@mkdir -p $(TESTDIR)
	$(CXX) -std=c++26 -O2 -Wall -I. test/theme.cpp theme.cpp -o $@

clean:
	rm -rf $(NAME).so $(OBJDIR) $(TESTDIR)

-include $(OBJS:.o=.d)

FORCE:
.PHONY: all test clean FORCE
