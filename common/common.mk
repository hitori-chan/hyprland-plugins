# common/common.mk — the shared plugin build. A plugin's Makefile sets NAME
# (and PKGS_EXTRA / LIBS_EXTRA for what it links beyond Hyprland) and
# includes this file.
#
# plugin.ver localizes the plugin's own NHypr* namespace symbols — the
# dlopen collision surface — while leaving everything else default-visible:
# Hyprland's globals are inline variables in headers, and the plugin's copy
# must stay exported to unify with the compositor's at dlopen (see
# plugin.ver; blanket -fvisibility=hidden nulls them and SEGVs).
CXX      ?= g++
CXXFLAGS ?= -O2
PKGS     := hyprland pixman-1 libdrm pangocairo $(PKGS_EXTRA)
CXXFLAGS += -std=c++26 -Wall -shared -fPIC -fno-gnu-unique \
            -Wl,--version-script=../common/plugin.ver \
            -I../ $(shell pkg-config --cflags $(PKGS))
LDLIBS   := $(if $(LIBS_EXTRA),$(shell pkg-config --libs $(LIBS_EXTRA)))

all: $(NAME).so

$(NAME).so: $(wildcard *.cpp) $(wildcard *.hpp) $(wildcard ../common/*.hpp)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDLIBS)

clean:
	rm -f $(NAME).so

.PHONY: all clean
