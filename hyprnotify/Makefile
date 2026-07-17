# sdbus-c++ (the notification daemon) + hyprgraphics (image decode):
# hyprnotify links what Hyprland doesn't.
CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++26 -Wall -shared -fPIC -fno-gnu-unique \
            $(shell pkg-config --cflags hyprland pixman-1 libdrm pangocairo hyprgraphics)
LDLIBS   := $(shell pkg-config --libs sdbus-c++ hyprgraphics)

all: hyprnotify.so

hyprnotify.so: $(wildcard *.cpp) $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDLIBS)

clean:
	rm -f hyprnotify.so

.PHONY: all clean
