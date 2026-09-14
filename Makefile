# Host-side build and verification. Nothing here needs an STM32 attached.
#
#   make            run every check
#   make generate   regenerate the C protocol layer from dbc/bfmc.dbc
#
# The firmware itself is built by STM32CubeIDE from firmware/gateway/.

CC      ?= gcc
PYTHON  ?= python
BUILD   := build/host
GEN     := firmware/common/protocol/generated

CFLAGS  := -std=c99 -Wall -Wextra -Werror -I$(GEN)
LDLIBS  := -lm

.PHONY: all validate check-generated test cross-check generate clean

all: validate check-generated test cross-check
	@echo "all checks passed"

## Message database integrity and bus load budget.
validate:
	$(PYTHON) tools/validate_dbc.py

## Fail if the committed generated code no longer matches the database.
check-generated:
	$(PYTHON) tools/generate_protocol.py --check

## Round-trip, scaling, range and frame-ID tests against the generated code.
test: $(BUILD)/test_protocol
	$(BUILD)/test_protocol

## Compare the generated C encoder against the cantools Python encoder.
cross-check: $(GEN)/bfmc.c
	$(PYTHON) tests/cross_check.py --cc $(CC)

$(BUILD)/test_protocol: tests/test_protocol.c $(GEN)/bfmc.c $(GEN)/bfmc.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) tests/test_protocol.c $(GEN)/bfmc.c $(LDLIBS) -o $@

generate:
	$(PYTHON) tools/generate_protocol.py

clean:
	rm -rf $(BUILD)
