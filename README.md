# CAN Bus Network for a BFMC Autonomous Vehicle

Replacing the single Raspberry Pi ↔ Nucleo UART link in a Bosch Future Mobility
Challenge vehicle with a distributed multi-node CAN bus, the way a production car
distributes control across ECUs.

**Status:** the protocol, safety and diagnostic layers are built and tested, and
two node roles run real DBC-defined frames against each other on one board.
Hardware bring-up on the physical bus is next.

**It runs today with no hardware beyond the Nucleo** — see
[Try it in five minutes](#try-it-in-five-minutes).

---

## The problem

The stock vehicle runs one microcontroller doing motor control, steering and
sensing, talking to a Raspberry Pi over one serial link. That is a star topology
with a single point of failure, and no way to add a node without touching the one
firmware image.

Real vehicles distribute function across ECUs on a shared multi-master bus. This
rebuilds the vehicle's internals that way.

```
Raspberry Pi ── Gateway ECU ──┬── Motion ECU    (motor, steering, encoder)
                              ├── Sensor ECU    (IMU, range, battery)
                              └── Logger        (passive capture, fault injection)
```

| | |
|---|---|
| Bit rate | 500 kbit/s, sample point 85.7% — [derivation](docs/bit-timing.md) |
| Messages | 15, priority-ordered by urgency |
| Steady-state bus load | **8.6%** of the link, enforced against a 30% budget in CI |
| Control loop | 50 Hz |
| Command loss → fail-safe | 60 ms |
| Peer loss → `DEGRADED` | 300 ms |
| Task stall → reset | 100 ms deadline, then a 170–250 ms watchdog |
| Firmware size | 11,512 B flash, 2,540 B RAM (application build) |
| Host assertions | **541**, mutation-checked, run in CI on every push |

## Try it in five minutes

The F446 has **two independent CAN controllers**, so one NUCLEO-F446RE is enough
to build a real multi-node bus. And in loopback the controller hears itself, so
the whole stack runs with nothing attached to the board at all.

```bash
# Verify the protocol and safety logic on your machine — no board needed
pip install cantools && make all

# Build the firmware for the board (needs arm-none-eabi-gcc)
tools/build_firmware.sh loopback app
```

Flash it, then watch `g_bfmc_app` as a Live Expression. `mo_cmd_state` should sit
at `1` (`OK`), `mo_failsafe` at `0`, and `mo_out_speed` sweeping 0 → 1000 → 0 over
four seconds — a command packed from the DBC, transmitted, received, unpacked,
supervised and turned into an actuator value, with no transceiver in sight.

## What gets built

Two independent switches in [`can_config.h`](firmware/gateway/Core/Inc/can_config.h).
Bus topology:

| `CAN_BRINGUP_MODE` | Hardware | What becomes real |
|---|---|---|
| `CAN_BRINGUP_LOOPBACK` *(default)* | **none** | clock tree, bit timing arithmetic, filter banks, mailboxes, RX interrupt path |
| `CAN_BRINGUP_DUAL` | 2 × MCP2551 + 2 resistors | arbitration, ACK, error frames, scope-verifiable bit timing |

And what the main loop runs:

| `BFMC_RUN_MODE` | Behaviour |
|---|---|
| `BFMC_RUN_BRINGUP` *(default)* | fixed payload on a fixed ID, no protocol — *is the bus alive at all?* |
| `BFMC_RUN_APPLICATION` | Gateway and Motion roles exchanging DBC frames through the safety logic |

Bring-up comes first on purpose: a silent bus and a wedged application look
identical from outside, so that question has to be settled before the application
is worth running. `docs/bringup-checklist.md` walks it step by step with the
counter values to expect.

## Design decisions

**The DBC is the source of truth, not the C headers.** The pack/unpack layer is
generated from `dbc/bfmc.dbc` by `cantools`, and the Python tooling reads the same
file. Two ECUs cannot disagree about where a signal sits, because neither
hand-writes the layout. The generated code is committed so the firmware builds
without Python, and CI runs `generate_protocol.py --check` so it cannot drift.

**CAN IDs are assigned by urgency, not by sender.** Arbitration is won by the
lowest ID, so the ID map *is* the priority map. Emergency stop is `0x010`; battery
telemetry is `0x340`.

**Absence of a message is information.** Every periodic frame has a consumer-side
timeout. A `MOTION_CMD` that stops arriving for 60 ms ramps the vehicle to a stop
— and one that *keeps arriving* with a frozen rolling counter does the same. That
second case is a sender whose timer interrupt still publishes while its control
task is wedged, which a heartbeat reports as perfectly healthy.

**Fail-safe means ramping, not cutting.** Losing the command stream decelerates at
a bounded rate and *holds* steering rather than centring it. Cutting the motor
outright can skid a light vehicle, and snapping the wheels straight mid-corner is
its own hazard; an uncontrolled slide is not a safe state.

**A watchdog that supervises the work, not the loop.** The IWDG is refreshed only
when every periodic task has checked in within its deadline — and each checks in
*after* doing its work, never on entry. A watchdog kicked from the main loop keeps
being fed by a control task that has stopped running, which is the failure
actually worth catching.

**Failures leave a trace.** A node that fails takes itself off the bus *first*, so
peers fail-safe on their own schedule instead of trusting a node that is no longer
running. It then records the failing call site and resets — or, after three faults
with no healthy run between, parks itself bus-silent and debuggable rather than
rebooting in a loop nobody can attach to.

**Nodes subscribe only to what they use.** The application build programs an
explicit identifier list per controller, so frames a node does not consume are
discarded by hardware and never raise an interrupt.

**The logic that matters is tested without hardware.** Neither the protocol nor
the application layer touches the STM32 HAL; both compile with plain `gcc`, run
under host tests in CI, and are cross-compiled for Cortex-M4 there. The safety
modules take the clock as a parameter instead of calling `HAL_GetTick()`, so tests
advance a synthetic clock and assert the exact millisecond of every transition.

## When something is wrong, it says so

The point of the diagnostic work is that a failure names itself instead of
presenting as "the board stopped".

| Watch | Tells you |
|---|---|
| `can_health` | one word: `OK`, `NO_PEER`, `WARNING`, `PASSIVE`, `BUS_OFF` |
| `last_lec == 3` | acknowledgment error — **transmit works, nothing is answering** |
| `tec` / `rec` | error counters: 96 warning, 128 error-passive, 255 bus-off |
| `rx_overrun` | the hardware FIFO overflowed and frames were lost |
| `recoveries` | bus-off recovery attempts started |
| `cmd_overruns` / `hb_overruns` | the main loop stalled and a mailbox was overwritten |
| `decode_errors` | a frame arrived but failed to unpack — DBC and firmware disagree |
| `mo_cmd_state` | `OK` / `TIMEOUT` / `STALE` — and `STALE` is a *different* fault |
| `reset_was_watchdog` | the last reset was a hang, not a power cycle |
| `prev_overdue_mask` | which task stopped checking in before that reset |
| `prev_fault_pc` | which call site failed — look it up in the `.map` |

`last_lec == 3` is the single most useful reading during bring-up: it means the
frame left the controller correctly and nobody acknowledged it. Without that
decode it is indistinguishable from a transmit fault.

## Verification

```bash
make all
```

| Target | What it proves |
|---|---|
| `validate` | no overlapping signals or duplicate IDs, node-state tables match the firmware enum, bus load inside budget |
| `check-generated` | the committed C still matches `bfmc.dbc` |
| `test` | 541 assertions: protocol round-trips and the safety state machines |
| `cross-check` | the generated C encoder agrees byte-for-byte with cantools |

**The cross-check earns its place.** A round-trip proves only that pack and unpack
agree with *each other*. If a signal sat at the wrong bit offset both directions
would be wrong identically, the test would still pass, and every other node on the
bus would read garbage. Encoding the same vectors through two independent
implementations catches that.

**The tests are mutation-checked.** A suite that always passes is worthless, so
the failure modes were verified by deliberately breaking the code — wrap-safe
arithmetic, stale-command detection, the fail-safe ramp, watchdog latching,
filter coverage — and confirming the tests catch each one. The table is in
[docs/testing.md](docs/testing.md).

CI additionally builds the firmware for **all four** combinations of topology and
run mode with `arm-none-eabi-gcc`, then asserts by symbol that the application
build actually links the safety layers and the bring-up build does not — and that
the `.noinit` section holding the crash record really does sit outside `bss`,
since a linker edit could otherwise wipe it every boot and the feature would fail
silently.

## Repository layout

Everything listed here exists.

```
dbc/bfmc.dbc                        single source of truth for every signal
firmware/common/protocol/generated/ bfmc.c/.h generated from the DBC, committed
firmware/common/app/                pure logic, no HAL, host-tested
  cmd_monitor.c                       command timeout and frozen-counter detection
  heartbeat.c                         peer liveness supervision
  motion_safety.c                     fail-safe actuator ramp
  task_watchdog.c                     per-task deadline supervision
  can_filter_plan.c                   which IDs each role subscribes to
firmware/gateway/                   Gateway ECU, CubeIDE project (NUCLEO-F446RE)
  Core/Src/can_if.c                   bxCAN transport, filters, ISRs, bus-off recovery
  Core/Src/bfmc_app.c                 Gateway + Motion roles on the safety logic
  Core/Src/can_bringup.c              raw harness: is the bus alive at all?
  Core/Src/watchdog.c                 IWDG at register level, reset-cause capture
  Core/Src/fault_record.c             crash record that survives the reset
  Core/Inc/can_config.h               every compile-time switch
tools/validate_dbc.py               database integrity + bus load budget
tools/generate_protocol.py          regenerate the C layer; --check detects drift
tools/build_firmware.sh             build the firmware without CubeIDE, for CI
tests/test_protocol.c               68 assertions against the generated code
tests/test_app.c                    473 assertions against the safety logic
tests/cross_check.py                generated C encoder vs the cantools encoder
Makefile                            make all runs every check
.github/workflows/ci.yml            host checks, four firmware builds, link guards
```

**Not yet written:** dedicated Motion and Sensor firmware on their own boards, the
Raspberry Pi link, and the virtual-bus integration tests. The Sensor node is also
not registered with either heartbeat supervisor — registering a node that was
never built would hold the vehicle in `DEGRADED` forever.

## Hardware

One NUCLEO-F446RE, two MCP2551 transceivers, two 120 Ω terminators, and two
pull-up resistors. The MCP2551 is a 5 V part whose TXD threshold is 3.5 V, above
what a 3.3 V STM32 pin can drive; `CAN_TX_OPEN_DRAIN=1` plus one pull-up per node
fixes that without a level shifter. Receive is unaffected, which is what makes the
failure so confusing — the node hears the whole bus while its own transmissions
are ignored. Full detail, including why *not* to use the MCP2515 modules for this,
is in [docs/hardware.md](docs/hardware.md).

## Roadmap

| Phase | Scope | State |
|---|---|---|
| 1 | Architecture, message matrix, DBC | **done** |
| 2 | Single-node bring-up, scope-verified bit timing | firmware ready, awaiting the bench |
| 3 | Multi-node bus, full message set, heartbeat timeouts | running in loopback, awaiting transceivers |
| 4 | Gateway integration with the BFMC Pi stack | |
| 5 | Fault injection, bus-off recovery, UDS-style diagnostics | recovery done, injection pending |
| 6 | Latency measurement, demo video, documentation | |

## Documentation

- [Architecture](docs/architecture.md) — nodes, failure behaviour, interrupts, filtering
- [Message matrix and bus load](docs/message-matrix.md) — ID scheme and the 8.6% calculation
- [Bit timing derivation](docs/bit-timing.md) — why 85.7% and not 87.5%
- [Hardware](docs/hardware.md) — transceivers, the 5 V problem, wiring
- [Bring-up checklist](docs/bringup-checklist.md) — expected counters and a symptom table
- [Testing without the car](docs/testing.md) — what is verified, what is not, and the mutation results
