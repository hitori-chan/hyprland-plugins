# Root convenience entry for the dev loop and the gate.
#
# `make` builds the plugin; `make test` runs its headless harness;
# `make gate` runs the nested integration gate (ARGS passes through).

all:
	$(MAKE) -C awesome

test:
	$(MAKE) -C awesome test

gate:
	$(MAKE) -C awesome gate ARGS=$(ARGS)

clean:
	$(MAKE) -C awesome clean
