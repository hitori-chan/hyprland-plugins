# Root convenience entry for the dev loop and the gate.
#
# `make` builds the plugins; `make test` runs awesome's headless harness;
# `make gate` runs awesome's nested integration gate (ARGS passes
# through); `make vclick-check` runs vclick's nested check.

all:
	$(MAKE) -C awesome
	$(MAKE) -C vclick

test:
	$(MAKE) -C awesome test

gate:
	$(MAKE) -C awesome gate ARGS='$(ARGS)'

vclick-check:
	$(MAKE) -C vclick
	bash vclick/check.sh

clean:
	$(MAKE) -C awesome clean
	$(MAKE) -C vclick clean

.PHONY: all test gate vclick-check clean
