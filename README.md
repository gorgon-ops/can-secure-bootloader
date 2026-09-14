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

```
dbc/bfmc.dbc              single source of truth for every signal on the bus
docs/architecture.md      node responsibilities, failure behaviour, layering
docs/message-matrix.md    ID scheme, frame table, bus load analysis
docs/bit-timing.md        bit timing derivation for each MCU
docs/hardware.md          bill of materials, wiring, bring-up order
firmware/common/          protocol + application logic (no HAL, host-testable)
firmware/gateway|motion|sensor/   per-node targets
tools/validate_dbc.py     CI check: database integrity + bus load budget
tests/                    host-side unit tests
```

## Design decisions

**The DBC is the source of truth, not the C headers.** Every node's pack/unpack
code and all Python tooling is generated from `dbc/bfmc.dbc`. Two ECUs cannot
disagree about where a signal sits in a payload, because neither one hand-writes
the layout. This is how automotive teams actually work, and it eliminates the
single most common multi-node integration bug.

**CAN IDs are assigned by urgency, not by sender.** Arbitration is won by the
lowest ID, so the ID map *is* the priority map. Emergency stop is `0x010`;
battery telemetry is `0x340`.

**Absence of a message is information.** Every periodic frame has a consumer-side
timeout. A `MOTION_CMD` that stops arriving for 60 ms ramps the vehicle to a
stop. A command that keeps arriving with a frozen rolling counter does the same
— which catches a wedged sender that a plain heartbeat would miss.

**Most of the code builds without hardware.** Protocol and application layers
have no STM32 HAL dependency, so they run under host unit tests in CI. The
project stays verifiable when the car is not on the desk.

## Verify the protocol without any hardware

```bash
pip install cantools
python tools/validate_dbc.py
```

Checks database integrity (overlapping signals, signals past DLC, duplicate IDs)
and recomputes worst-case bus load against a 30% budget.

## Roadmap

| Phase | Scope | State |
|-------|-------|-------|
| 1 | Architecture, message matrix, DBC | **done** |
| 2 | Single-node bring-up, scope-verified bit timing | next |
| 3 | Multi-node bus, full message set, heartbeat timeouts | |
| 4 | Gateway integration with the BFMC Pi stack | |
| 5 | Fault injection, bus-off recovery, UDS-style diagnostics | |
| 6 | Latency measurement, demo video, documentation | |

## Documentation

- [Architecture](docs/architecture.md)
- [Message matrix and bus load](docs/message-matrix.md)
- [Bit timing derivation](docs/bit-timing.md)
- [Hardware and bring-up](docs/hardware.md)
