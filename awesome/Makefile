# awesome/ — the unified plugin.
#
# Two targets:
#   make -C awesome test   — the headless unit tests (pure core, no fork
#                            headers: the regression net for the platform
#                            layer)
#   make -C awesome        — the .so (lands with the supervisor; it needs
#                            the fork headers through pkg-config, like the
#                            old plugins)

CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++26 -Wall -fno-gnu-unique -I.

BUILD := .build

TEST_BIN  := $(BUILD)/awesome-test
CORE_SRCS := $(wildcard core/*.cpp)
TEST_SRCS := $(wildcard test/*.cpp)

all: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRCS) $(CORE_SRCS) $(wildcard core/*.hpp) $(wildcard test/*.hpp)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(TEST_SRCS) $(CORE_SRCS) -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
