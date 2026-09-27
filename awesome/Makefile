# awesome — build the single Rust plugin into awesome/awesome.so (a cdylib
# the fork dlopen()s). bindgen parses the cabi C ABI header; the .so only
# references the fork's exported hl_* symbols (resolved at load), so no
# link-time dependency on Hyprland is needed.
#
# HYPR_CABI_HEADER defaults to the fork source; the gate/stage points it at
# the staged header matching the running binary. The runtime
# cabiAbiVersion() handshake is the real guard (a stale header ejects).

CARGO    ?= cargo
CABI_HDR ?= $(HOME)/repo/Hyprland/src/plugins/cabi/cabi.h

# the .so has no prerequisites of its own (cargo decides staleness); a find
# over the source files makes `make` re-check the cargo build after any edit
SRCS := $(shell find src build.rs Cargo.toml Cargo.lock -type f 2>/dev/null)

all: awesome.so

awesome.so: $(SRCS)
	@HYPR_CABI_HEADER="$(CABI_HDR)" $(CARGO) build --release --manifest-path Cargo.toml
	@cp target/release/libawesome.so awesome.so

clean:
	rm -f awesome.so
	@$(CARGO) clean

.PHONY: all clean
