# CAN Bus Network for a BFMC Autonomous Vehicle

Replacing the single Raspberry Pi ↔ Nucleo UART link in a Bosch Future Mobility
Challenge vehicle with a distributed multi-node CAN bus, the way a production
car distributes control across ECUs.

**Status:** Phase 3 — two node roles running DBC-defined frames through the
safety logic on one board. Hardware bring-up next.

---

## What this is

The stock vehicle runs one microcontroller doing motor control, steering and
sensing, on one serial link. This project splits it into four nodes on a shared
500 kbit/s CAN bus with a defined message database, priority-ordered IDs,
heartbeat monitoring and fail-safe behaviour on node loss.

```
Raspberry Pi ── Gateway ECU ──┬── Motion ECU    (motor, steering, encoder)
                              ├── Sensor ECU    (IMU, range, battery)
                              └── Logger        (passive capture, fault injection)
```

| Metric | Value |
|--------|-------|
| Bit rate | 500 kbit/s |
| Sample point | 85.7% (F446) / 88.9% (F103) — [why](docs/bit-timing.md) |
| Messages | 15 |
| Steady-state bus load | **8.6%** — verified by `tools/validate_dbc.py` |
| Control loop | 50 Hz |
| Command timeout → fail-safe | 60 ms |

## Repository layout

Everything listed here exists. Planned work is in the roadmap below, not here.

```
dbc/bfmc.dbc                       single source of truth for every signal
docs/                              architecture, message matrix, bit timing,
                                   hardware, testing
firmware/common/protocol/generated/ bfmc.c/.h, generated from the DBC, committed
firmware/gateway/                  Gateway ECU, CubeIDE project (NUCLEO-F446RE)
  Core/Src/can_if.c                bxCAN transport: filters, startup, callbacks
  Core/Src/can_bringup.c           raw harness: is the bus alive at all?
  Core/Src/bfmc_app.c              Gateway + Motion roles on the safety logic
  Core/Inc/can_config.h            bus topology and run mode switches
tools/validate_dbc.py              database integrity + bus load budget
tools/generate_protocol.py         regenerate the C layer; --check detects drift
tools/build_firmware.sh            build the firmware without CubeIDE, for CI
firmware/common/app/               timeout, heartbeat and fail-safe logic,
                                   no HAL dependency, host-tested
tests/test_protocol.c              68 assertions against the generated code
tests/test_app.c                   439 assertions against the safety logic
tests/cross_check.py               generated C encoder vs the cantools encoder
Makefile                           make all runs every check above
.github/workflows/ci.yml           host checks plus all four firmware builds
```

Not yet written: dedicated Motion and Sensor node firmware on their own boards,
the Raspberry Pi link, and the virtual-bus integration tests.

## Design decisions

**The DBC is the source of truth, not the C headers.** The pack/unpack layer in
`firmware/common/protocol/generated/` is generated from `dbc/bfmc.dbc` by
`cantools`, and the Python tooling reads the same file. Two ECUs cannot disagree
about where a signal sits in a payload, because neither one hand-writes the
layout. The generated code is committed so the firmware builds without Python,
and CI runs `tools/generate_protocol.py --check` so it cannot drift from the
database.

**CAN IDs are assigned by urgency, not by sender.** Arbitration is won by the
lowest ID, so the ID map *is* the priority map. Emergency stop is `0x010`;
battery telemetry is `0x340`.

**Absence of a message is information.** Every periodic frame has a
consumer-side timeout. A `MOTION_CMD` that stops arriving for 60 ms ramps the
vehicle to a stop; one that keeps arriving with a frozen rolling counter does
the same — which catches a wedged sender that a plain heartbeat would miss,
because the heartbeat task can still be running while the control task is
stuck. Both paths are implemented in `firmware/common/app/` and asserted at the
exact millisecond boundary in `tests/test_app.c`.

**A watchdog that checks the work, not the loop.** The IWDG is refreshed only
when every periodic task has checked in within its deadline, so a control task
that stops running resets the board instead of leaving the vehicle on stale
actuator values — which a watchdog kicked from the main loop would never catch.
The reason it fired survives the reset in a `.noinit` RAM section, so an
intermittent hang leaves a trace rather than looking like a power cycle.

**The logic that matters builds and is tested without hardware.** Neither the
protocol nor the application layer touches the STM32 HAL; both compile with
plain `gcc` and run under host tests in CI on every push, and both are also
cross-compiled for Cortex-M4 there. The safety modules take the clock as a
parameter instead of calling `HAL_GetTick()`, so a test can advance a synthetic
clock rather than waiting in real time.

## Running it on one board

The F446 has **two independent CAN controllers**, so a single NUCLEO-F446RE is
enough to build a real multi-node bus. Two independent switches in
`Core/Inc/can_config.h` decide what gets built. Bus topology:

| `CAN_BRINGUP_MODE` | Hardware needed | What is real |
|---|---|---|
| `CAN_BRINGUP_LOOPBACK` (default) | **none** | filters, mailboxes, RX interrupts, message set |
| `CAN_BRINGUP_DUAL` | 2 × MCP2551 + 2 resistors | arbitration, ACK, error frames, bit timing |

And what the main loop runs:

| `BFMC_RUN_MODE` | Behaviour |
|---|---|
| `BFMC_RUN_BRINGUP` (default) | fixed payload on a fixed ID, no protocol — is the bus alive at all? |
| `BFMC_RUN_APPLICATION` | Gateway and Motion roles exchanging real DBC frames through the safety logic |

Bring-up comes first on purpose: a silent bus and a wedged application look
identical from outside, so that question has to be settled before the
application is worth running.

A third switch, `CAN_TX_OPEN_DRAIN`, exists because the MCP2551 is a 5 V part:
its TXD logic-high threshold is 3.5 V, which a 3.3 V STM32 pin cannot reach.
Receive is unaffected, so the node hears the bus perfectly while its own
transmissions fail intermittently — a confusing failure that looks like a
firmware bug. Driving CAN_TX open-drain with a pull-up to +5 V fixes it with one
resistor per node. The pull-up is mandatory when the option is on, or the pin
sits permanently dominant and jams the bus; see
[docs/hardware.md](docs/hardware.md).

In `BFMC_RUN_APPLICATION` the Gateway publishes `MOTION_CMD` at 50 Hz with an
advancing `CmdCounter` and `HB_GATEWAY` at 10 Hz; the Motion role consumes them
through `cmd_monitor`, drives its outputs through `motion_safety`, and publishes
`MOTION_STATUS` and `HB_MOTION`. In loopback the two roles share CAN1 and the
controller hears itself, so the entire stack — pack, transmit, receive, unpack,
supervise, fail-safe — runs with nothing attached to the board. In dual mode the
Gateway is on CAN1 and Motion on CAN2, contending on one real bus.

In `BFMC_RUN_BRINGUP` the frames are raw instead: `0x100` from CAN1 and `0x200`
from CAN2, queued back-to-back so the two contend. CAN1 holds the lower ID and
must win, which is visible on a capture. Adding your existing Arduino + MCP2515
gives a third node on an independent oscillator at no extra cost. Details in
[docs/hardware.md](docs/hardware.md).

Watch `g_bfmc_app` as a Live Expression for the application state: the
supervision verdict, the actuator outputs, and whether the fail-safe ramp is
active. Watch `g_can1_stats` / `g_can2_stats` for the transport: TX/RX counts, plus
`last_error` and an `ESR` snapshot carrying the transmit and receive error
counters and the last error code. An ACK error (LEC `0b011`) is what a lone node
with no partner reports — which is the single most common bring-up symptom.

Two settings are deliberate bring-up choices that must be revisited before the
multi-node phase: `AutoBusOff` and `AutoRetransmission` are both disabled, which
isolates wiring faults but is not correct vehicle behaviour.

## Verify it without any hardware

```bash
pip install cantools && make all
```

Four checks, all run in CI on every push:

| Target | What it proves |
|---|---|
| `validate` | no overlapping signals or duplicate IDs; bus load within 30% budget |
| `check-generated` | the committed C still matches `bfmc.dbc` |
| `test` | 507 assertions: protocol round-trips and the safety state machines |
| `cross-check` | the generated C encoder agrees byte-for-byte with cantools |

The cross-check earns its place: a round-trip only proves pack and unpack agree
with *each other*. If a signal sat at the wrong bit offset, both directions
would be wrong identically and the round-trip would still pass, while every
other node on the bus read garbage. Encoding the same vectors through two
independent implementations catches that.

CI also builds the firmware itself for both bring-up modes with
`arm-none-eabi-gcc`, so a change that breaks only the dual-CAN path cannot reach
`main` unnoticed.

## Roadmap

| Phase | Scope | State |
|-------|-------|-------|
| 1 | Architecture, message matrix, DBC | **done** |
| 2 | Single-node bring-up, scope-verified bit timing | **in progress** |
| 3 | Multi-node bus, full message set, heartbeat timeouts | **in progress** |
| 4 | Gateway integration with the BFMC Pi stack | |
| 5 | Fault injection, bus-off recovery, UDS-style diagnostics | |
| 6 | Latency measurement, demo video, documentation | |

## Documentation

- [Architecture](docs/architecture.md)
- [Message matrix and bus load](docs/message-matrix.md)
- [Bit timing derivation](docs/bit-timing.md)
- [Hardware and bring-up](docs/hardware.md)
- [Bring-up checklist](docs/bringup-checklist.md) — wiring, expected counters, symptom table
- [Testing without the car](docs/testing.md)
