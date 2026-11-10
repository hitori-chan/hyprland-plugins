# vclick — click or scroll inside a window without the user's pointer.
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
CXXFLAGS  += -std=c++26 -Wall -fPIC -fno-gnu-unique $(HL_CFLAGS)
LDFLAGS   += -shared -Wl,--version-script=plugin.ver

# The build's target: compiler, flags (they carry the header root) and the
# headers' version.h. The stamp is rewritten only when that changes, so
# another header set or an installed fork rebuilds by itself.
HL_VERSION_H := $(firstword $(wildcard $(patsubst -I%,%/hyprland/src/version.h,$(filter -I%,$(HL_CFLAGS)))))
STAMP        := .build/target.stamp
STAMP_TEXT   := $(CXX) $(CXXFLAGS) $(LDFLAGS) $(if $(HL_VERSION_H),$(shell cksum < $(HL_VERSION_H)))

all: $(NAME).so

$(STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo '$(STAMP_TEXT)' | cmp -s - $@ || echo '$(STAMP_TEXT)' > $@

# the linker unlinks its output first: a loaded .so keeps its old inode
$(NAME).so: main.cpp plugin.ver $(STAMP)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) main.cpp -o $@

clean:
	rm -rf $(NAME).so .build

FORCE:
.PHONY: all clean FORCE
