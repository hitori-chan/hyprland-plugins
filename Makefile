# Root convenience target — the plugins each own their build (common.mk);
# this is the "one command" entry for the dev loop and the gate.
#
# `make` builds the plugins one behind another; `make -j` builds them in
# parallel (the -j propagates to each plugin's TU farm through MAKEFLAGS).

PLUGINS := hyprbar hyprnotify hyprmax hyprsnap hyprclick hyprplace hyprpad hyprosd

all: $(PLUGINS)

$(PLUGINS):
	$(MAKE) -C $@

test:
	$(MAKE) -C devtools
	@for t in devtools/*-test; do $$t || exit 1; done

clean:
	@for p in $(PLUGINS); do $(MAKE) -C $$p clean; done
	$(MAKE) -C devtools clean

gate:
	bash devtools/stress.sh

.PHONY: all test clean gate $(PLUGINS)
