# Root convenience target — the plugins each own their build (common.mk);
# this is the "one command" entry for the dev loop and the gate.

PLUGINS := hyprbar hyprnotify hyprmax hyprsnap hyprclick hyprplace hyprpad hyprosd

all:
	@for p in $(PLUGINS); do make -C $$p -j$(JOBS) || exit 1; done

test:
	$(MAKE) -C devtools
	@for t in devtools/*-test; do $$t || exit 1; done

clean:
	@for p in $(PLUGINS); do make -C $$p clean; done
	$(MAKE) -C devtools clean

gate:
	bash devtools/stress.sh

JOBS ?= $(shell nproc)

.PHONY: all test clean gate
