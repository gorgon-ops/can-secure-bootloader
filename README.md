# CAN Bus Network for a BFMC Autonomous Vehicle

Replacing the single Raspberry Pi ↔ Nucleo UART link in a Bosch Future Mobility
Challenge vehicle with a distributed multi-node CAN bus, the way a production
car distributes control across ECUs.

**Status:** Phase 1 — architecture and protocol defined. Hardware bring-up next.

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
  Core/Src/can_bringup.c           dual-bxCAN harness, loopback and dual modes
tools/validate_dbc.py              database integrity + bus load budget
tools/generate_protocol.py         regenerate the C layer; --check detects drift
tools/build_firmware.sh            build the firmware without CubeIDE, for CI
tests/test_protocol.c              68 host assertions against the generated code
tests/cross_check.py               generated C encoder vs the cantools encoder
Makefile                           make all runs every check above
.github/workflows/ci.yml           runs all of it, plus both firmware builds
```

Not yet written: the application layer (`firmware/common/app/`), the Motion and
Sensor node firmware, and the virtual-bus integration tests.

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

**Absence of a message is information** *(designed, not yet implemented)*. Every
periodic frame is specified with a consumer-side timeout: a `MOTION_CMD` that
stops arriving for 60 ms ramps the vehicle to a stop, and one that keeps
arriving with a frozen rolling counter does the same — which catches a wedged
sender that a plain heartbeat would miss. The rolling counter exists in the DBC
today; the timeout logic lands with the application layer in Phase 3.

**The protocol layer builds and is tested without hardware.** It has no STM32
HAL dependency, so it compiles with plain `gcc` and runs under host tests in CI
on every push. The application layer is being written to the same constraint.

## Running it on one board

The F446 has **two independent CAN controllers**, so a single NUCLEO-F446RE is
enough to build a real multi-node bus. `firmware/gateway/` builds in two modes:

| `CAN_BRINGUP_MODE` | Hardware needed | What is real |
|---|---|---|
| `CAN_BRINGUP_LOOPBACK` (default) | **none** | filters, mailboxes, RX interrupts, message set |
| `CAN_BRINGUP_DUAL` | 2 � SN65HVD230, ~�4 | arbitration, ACK, error frames, bit timing |

In loopback the board receives its own frames internally with nothing attached.
In dual mode CAN1 transmits `0x100` and CAN2 transmits `0x200` onto one physical
bus � CAN1 holds the lower ID and must win arbitration, which is visible on a
capture. Adding your existing Arduino + MCP2515 gives a third node on an
independent oscillator at no extra cost. Details in
[docs/hardware.md](docs/hardware.md).

Watch `g_can1_stats` / `g_can2_stats` as Live Expressions: TX/RX counts, plus
`last_error` and an `ESR` snapshot carrying the transmit and receive error
counters and the last error code. An ACK error (LEC `0b011`) is what a lone node
with no partner reports � which is the single most common bring-up symptom.

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
| `test` | 68 assertions: round-trips, signed extremes, scaling, ranges, frame IDs |
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
| 3 | Multi-node bus, full message set, heartbeat timeouts | |
| 4 | Gateway integration with the BFMC Pi stack | |
| 5 | Fault injection, bus-off recovery, UDS-style diagnostics | |
| 6 | Latency measurement, demo video, documentation | |

## Documentation

- [Architecture](docs/architecture.md)
- [Message matrix and bus load](docs/message-matrix.md)
- [Bit timing derivation](docs/bit-timing.md)
- [Hardware and bring-up](docs/hardware.md)
