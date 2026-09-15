# Host-side build and verification. Nothing here needs an STM32 attached.
#
#   make            run every check
#   make generate   regenerate the C protocol layer from dbc/bfmc.dbc
#
# The firmware itself is built by STM32CubeIDE from firmware/gateway/, or
# headlessly by tools/build_firmware.sh (which is what CI uses).

CC      ?= gcc
PYTHON  ?= python
BUILD   := build/host
GEN     := firmware/common/protocol/generated
APP     := firmware/common/app

CFLAGS  := -std=c99 -Wall -Wextra -Werror
LDLIBS  := -lm

APP_SRC := $(wildcard $(APP)/*.c)
APP_HDR := $(wildcard $(APP)/*.h)

.PHONY: all validate check-generated test test-protocol test-app cross-check generate clean

all: validate check-generated test cross-check
	@echo "all checks passed"

## Message database integrity, node-state contract, bus load budget.
validate:
	$(PYTHON) tools/validate_dbc.py

## Fail if the committed generated code no longer matches the database.
check-generated:
	$(PYTHON) tools/generate_protocol.py --check

test: test-protocol test-app

## Round-trip, scaling, range and frame-ID tests against the generated code.
test-protocol: $(BUILD)/test_protocol
	$(BUILD)/test_protocol

## Timeout, heartbeat and fail-safe state machines against a synthetic clock.
test-app: $(BUILD)/test_app
	$(BUILD)/test_app

## Compare the generated C encoder against the cantools Python encoder.
cross-check: $(GEN)/bfmc.c
	$(PYTHON) tests/cross_check.py --cc $(CC)

$(BUILD)/test_protocol: tests/test_protocol.c tests/test_util.h $(GEN)/bfmc.c $(GEN)/bfmc.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(GEN) -Itests tests/test_protocol.c $(GEN)/bfmc.c $(LDLIBS) -o $@

# -I$(GEN) because the filter plan is built from the generated frame IDs, which
# keeps the DBC as the single source of truth for what each node subscribes to.
$(BUILD)/test_app: tests/test_app.c tests/test_util.h $(APP_SRC) $(APP_HDR) $(GEN)/bfmc.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(APP) -I$(GEN) -Itests tests/test_app.c $(APP_SRC) -o $@

generate:
	$(PYTHON) tools/generate_protocol.py

clean:
	rm -rf $(BUILD)
