# sdbus-c++: the feedback cards' bus link; libinput: device bus types.
CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++26 -Wall -shared -fPIC -fno-gnu-unique \
            $(shell pkg-config --cflags hyprland pixman-1 libdrm pangocairo sdbus-c++ libinput)
LDLIBS   := $(shell pkg-config --libs sdbus-c++ libinput)

all: hyprpad.so

hyprpad.so: $(wildcard *.cpp) $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDLIBS)

clean:
	rm -f hyprpad.so

.PHONY: all clean
