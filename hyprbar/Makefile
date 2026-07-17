# sdbus-c++ (DBus tray) + librsvg (SVG icons): hyprbar links what Hyprland doesn't.
CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++26 -Wall -shared -fPIC -fno-gnu-unique \
            $(shell pkg-config --cflags hyprland pixman-1 libdrm pangocairo librsvg-2.0)
LDLIBS   := $(shell pkg-config --libs sdbus-c++ librsvg-2.0)

all: hyprbar.so

hyprbar.so: $(wildcard *.cpp) $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDLIBS)

clean:
	rm -f hyprbar.so

.PHONY: all clean
