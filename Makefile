# Host-side build and verification. Nothing here needs an STM32 attached.
#
#   make            run every check
#   make generate   regenerate the C protocol layer from dbc/bfmc.dbc
#
# The firmware itself is built by STM32CubeIDE from firmware/gateway/, or
# headlessly by tools/build_firmware.sh (which is what CI uses).

# Override make's built-in default (cc) but honour an explicit env/CLI CC.
# A bare `?=` would not win here: make's default CC=cc already counts as set,
# so on a toolchain without a `cc` alias (e.g. MinGW) the build would break.
ifeq ($(origin CC),default)
CC      := gcc
endif
PYTHON  ?= python
BUILD   := build/host
GEN     := firmware/common/protocol/generated
APP     := firmware/common/app
UDS     := firmware/common/uds
BOOT    := firmware/common/boot

CFLAGS  := -std=c99 -Wall -Wextra -Werror
LDLIBS  := -lm

APP_SRC := $(wildcard $(APP)/*.c)
UDS_SRC := $(wildcard $(UDS)/*.c)
UDS_HDR := $(wildcard $(UDS)/*.h)
BOOT_SRC := $(wildcard $(BOOT)/*.c)
BOOT_HDR := $(wildcard $(BOOT)/*.h)
APP_HDR := $(wildcard $(APP)/*.h)

.PHONY: all validate check-generated test test-protocol test-app test-isotp test-uds test-boot cross-check generate clean

all: validate check-generated test cross-check
	@echo "all checks passed"

## Message database integrity, node-state contract, bus load budget.
validate:
	$(PYTHON) tools/validate_dbc.py

## Fail if the committed generated code no longer matches the database.
check-generated:
	$(PYTHON) tools/generate_protocol.py --check

test: test-protocol test-app test-isotp test-uds test-boot

## Round-trip, scaling, range and frame-ID tests against the generated code.
test-protocol: $(BUILD)/test_protocol
	$(BUILD)/test_protocol

## Timeout, heartbeat and fail-safe state machines against a synthetic clock.
test-app: $(BUILD)/test_app
	$(BUILD)/test_app

## ISO-TP transport, weighted towards malformed and hostile input.
test-isotp: $(BUILD)/test_isotp
	$(BUILD)/test_isotp

## UDS server: sessions, dispatch, and the security access state machine.
test-uds: $(BUILD)/test_uds
	$(BUILD)/test_uds

## SHA-256 against the FIPS vectors, and image verification ordering.
test-boot: $(BUILD)/test_boot
	$(BUILD)/test_boot

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

$(BUILD)/test_isotp: tests/test_isotp.c tests/test_util.h $(UDS_SRC) $(UDS_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(UDS) -Itests tests/test_isotp.c $(UDS_SRC) -o $@

$(BUILD)/test_uds: tests/test_uds.c tests/test_util.h $(UDS_SRC) $(UDS_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(UDS) -Itests tests/test_uds.c $(UDS)/uds.c -o $@

$(BUILD)/test_boot: tests/test_boot.c tests/test_util.h $(BOOT_SRC) $(BOOT_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(BOOT) -Itests tests/test_boot.c $(BOOT_SRC) -o $@

generate:
	$(PYTHON) tools/generate_protocol.py

clean:
	rm -rf $(BUILD)
