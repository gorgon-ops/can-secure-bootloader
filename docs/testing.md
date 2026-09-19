# Testing Without the Car

The weakness of most embedded portfolio projects is that nothing can be verified
unless the hardware is plugged in. That makes regressions invisible, makes the
repo unreviewable by anyone who does not own the boards, and leaves no evidence
the thing ever worked.

This project is structured so the majority of its logic is testable on a host
machine, in CI, with no STM32 present.

**Status is marked explicitly below.** Nothing here describes intent as though
it were already built.

## Level 1 — database validation ✅ implemented

`tools/validate_dbc.py`, run by `make validate`:

- no overlapping signals, no signal past its DLC (via cantools strict mode)
- no duplicate frame IDs
- steady-state bus load within budget — currently 8.6% against a 30% ceiling

Runs in under a second. Catches the "someone added a frame by hand" class of bug
before it reaches a board.

`tools/generate_protocol.py --check` (`make check-generated`) additionally
regenerates the C layer into a temporary directory and fails if it differs from
what is committed. That is what keeps "the DBC is the source of truth" true
rather than merely stated. The generator stamps a timestamp into its own file
header, so the comparison normalises that line out — otherwise the check could
never pass.

## Level 2 — host unit tests ✅ implemented

`firmware/common/protocol/generated/` has no STM32 HAL dependency and compiles
for the host with plain gcc under `-Wall -Wextra -Werror`.

**Implemented** — `tests/test_protocol.c`, 68 assertions via `make test`:

- pack/unpack round-trips across signed extremes, including `-2000..2000` speed
  and `-250..250` steering raw values
- physical scaling: `12.3 deg` must encode to raw `123` under a 0.1 scale
- range predicates reject out-of-range values at the boundary
- an undersized buffer is rejected rather than silently truncated onto the bus
- frame IDs match the message matrix, and the priority ordering holds

**Implemented** — `tests/test_isotp.c`, 129 assertions via `make test-isotp`,
against the ISO-TP transport in `firmware/common/uds/`. Weighted deliberately
towards malformed and hostile input, because reassembly is the first thing an
attacker reaches through the diagnostic interface:

- a first frame may declare up to 4095 bytes; the declared length is checked
  against the buffer **before any copy** and refused with a flow-control
  overflow rather than truncated
- the boundary is asserted on both sides, so an off-by-one cannot quietly
  reject legitimate maximum-length messages
- out-of-order sequence numbers abandon the message rather than assembling one
  from pieces of two transfers; orphaned consecutive frames are ignored
- a stalled transfer times out instead of holding the reassembly buffer
  indefinitely, which would be denial of service requiring no privilege
- the reassembly index is asserted to stay inside the buffer under a sustained
  overflow attempt

**Implemented** — `tests/cross_check.py` via `make cross-check`. Encodes fixed
vectors through the generated C and through the cantools Python encoder, and
compares the bytes. This catches what a round-trip cannot: if a signal sat at
the wrong bit offset, pack and unpack would be wrong in the same way and the
round-trip would still pass, while every other node read garbage. Two
independent implementations can only agree if the layout is genuinely right.

**Implemented** — `tests/test_app.c`, 473 assertions via `make test-app`, against
the application layer in `firmware/common/app/`:

- command timeout: healthy at 59 ms, fail-safe at exactly 60 ms. Both sides of
  the boundary are asserted, so an off-by-one cannot pass
- frozen `CmdCounter`: frames arriving perfectly on schedule for 60 ms with an
  unchanged counter reports `STALE`, which a timeout alone would never catch
- heartbeat supervision: a Sensor ECU that stops publishing reports `LOST` and
  drives the consumer to `NODE_STATE_DEGRADED` at 300 ms
- a *passive* node going quiet must not degrade anything — the Logger is
  registered as optional and its absence is asserted to be harmless
- tick wraparound: `HAL_GetTick()` rolls over every ~49.7 days, and a naive
  comparison would read that as a huge elapsed time and trip fail-safe on a
  perfectly healthy vehicle
- fail-safe ramp: speed decreases monotonically to zero without overshooting
  into reverse, steering holds its last value, and the vehicle stops within
  500 ms from 1500 mm/s
- task watchdog: a stalled task is caught **while the main loop keeps turning
  and every other task keeps checking in** — the case a loop-kicked watchdog
  cannot see; plus the boot grace window, the never-started-task fault,
  latching, the deadline boundary on both sides, and tick wraparound
- receive filters: every ID the application dispatches on is accepted, in both
  topologies. A filter that drops a needed frame reports nothing at all, so the
  dispatched list is restated in the test rather than shared with the firmware,
  forcing the two to agree instead of being wrong together. Also asserts a role
  does *not* accept traffic it has no use for, and never accepts ID `0x000` —
  the value a mis-padded hardware bank would subscribe to

These are the behaviours that matter most for safety and the ones that are
painful to trigger on hardware — you would have to physically unplug a node and
catch the reaction. Because every module takes the clock as a parameter, the
tests drive a synthetic clock and assert the exact millisecond of each
transition.

### The tests are mutation-checked

A suite that always passes is worthless, so the failure modes were verified by
deliberately breaking the code and confirming the tests catch it:

| Mutation | Result |
|---|---|
| wrap-safe elapsed replaced with a naive signed comparison | 75 failures |
| frozen-counter detection disabled | 2 failures |
| minimum ramp step removed (vehicle coasts forever) | 1 failure |
| optional nodes treated as required | 16 failures |
| Motion unsubscribed from `MOTION_CMD` | 2 failures |
| filter de-duplication removed | 3 failures |
| filter widened back to accept-all | 9 failures |
| ISO-TP declared-length bounds check removed | 8 failures |
| bounds check off by one (`>=` for `>`) | 4 failures |
| ISO-TP sequence number check removed | 3 failures |
| orphaned consecutive frames accepted | 1 failure |
| reassembly timeout removed | 5 failures |
| watchdog verdict un-latched (a late check-in rescues a missed deadline) | 1 failure |
| a task that never starts tolerated forever | 2 failures |
| deadline comparison off by one (`>` instead of `>=`) | 17 failures |

## Level 3 — virtual bus integration ❌ not started

`python-can` provides a `virtual` bus and Linux provides `vcan`; both let
several processes exchange real CAN frames with no transceiver. The plan is to
run node logic compiled for the host against a virtual bus to test the Gateway
fan-out, multi-node startup ordering, and fault injection.

None of this exists yet. It is Phase 5 work.

## CI ✅ implemented

`.github/workflows/ci.yml` runs on every push and pull request:

| Job | Contents |
|-----|----------|
| `protocol` | `make all` — levels 1 and 2 above |
| `firmware` | builds `gateway.elf` for **all four** combinations of bus topology and run mode with `arm-none-eabi-gcc`, uploads the ELFs |

The firmware job matters because CubeIDE generates its own makefiles at build
time, which is useless in CI. `tools/build_firmware.sh` reproduces the compile
and link with a plain toolchain, so a change that breaks only the dual-CAN path,
or only the application build, cannot merge unnoticed.

The application build is also where the two layers stop being theoretical: a
symbol check confirms `CmdMonitor_*`, `Heartbeat_*`, `MotionSafety_*` and the
generated `bfmc_*` pack/unpack functions are linked into `gateway.elf`, and
absent from the bring-up build where `--gc-sections` correctly drops them.

## What still requires hardware

Stated plainly rather than implied away:

- bit timing and sample point — needs a scope
- transceiver behaviour, termination, signal integrity
- real arbitration under electrical contention
- bus-off entry and recovery timing
- end-to-end latency versus the original UART link

Those are Phase 2 and Phase 5 deliverables and belong in a test report with
scope captures, not in CI.
