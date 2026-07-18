# sdbus-c++: the cards' session bus + logind's system bus.
CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++26 -Wall -shared -fPIC -fno-gnu-unique \
            $(shell pkg-config --cflags hyprland pixman-1 libdrm pangocairo sdbus-c++)
LDLIBS   := $(shell pkg-config --libs sdbus-c++)

all: hyprosd.so

hyprosd.so: $(wildcard *.cpp) $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDLIBS)

clean:
	rm -f hyprosd.so

.PHONY: all clean
