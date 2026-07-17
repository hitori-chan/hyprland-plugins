CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++26 -Wall -shared -fPIC -fno-gnu-unique \
            $(shell pkg-config --cflags hyprland pixman-1 libdrm pangocairo)

all: hyprsnap.so

hyprsnap.so: $(wildcard *.cpp) $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

clean:
	rm -f hyprsnap.so

.PHONY: all clean
