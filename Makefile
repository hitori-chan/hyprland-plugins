# `make` builds both plugins, `make test` runs both plugins' unit tests,
# `make gate` runs awesome's nested gate (ARGS passes through), `make
# vclick-check` vclick's nested check.

all:
	$(MAKE) -C awesome
	$(MAKE) -C vclick

test:
	$(MAKE) -C awesome test
	$(MAKE) -C vclick test

gate:
	$(MAKE) -C awesome gate ARGS='$(ARGS)'

vclick-check:
	$(MAKE) -C vclick
	bash vclick/check.sh

clean:
	$(MAKE) -C awesome clean
	$(MAKE) -C vclick clean

.PHONY: all test gate vclick-check clean
