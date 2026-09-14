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

## Level 2 — host unit tests ⚠️ partly implemented

`firmware/common/protocol/generated/` has no STM32 HAL dependency and compiles
for the host with plain gcc under `-Wall -Wextra -Werror`.

**Implemented** — `tests/test_protocol.c`, 68 assertions via `make test`:

- pack/unpack round-trips across signed extremes, including `-2000..2000` speed
  and `-250..250` steering raw values
- physical scaling: `12.3 deg` must encode to raw `123` under a 0.1 scale
- range predicates reject out-of-range values at the boundary
- an undersized buffer is rejected rather than silently truncated onto the bus
- frame IDs match the message matrix, and the priority ordering holds

**Implemented** — `tests/cross_check.py` via `make cross-check`. Encodes fixed
vectors through the generated C and through the cantools Python encoder, and
compares the bytes. This catches what a round-trip cannot: if a signal sat at
the wrong bit offset, pack and unpack would be wrong in the same way and the
round-trip would still pass, while every other node read garbage. Two
independent implementations can only agree if the layout is genuinely right.

**Not yet written** — these need the application layer, which does not exist:

- timeout state machines: feed a synthetic clock and assert fail-safe after
  60 ms of silence, and after 60 ms of frames with a frozen `CmdCounter`
- heartbeat supervision: assert `DEGRADED` on a missing `HB_SENSOR`

These are the behaviours that matter most for safety and they are exactly the
ones that are painful to trigger on real hardware. They land with Phase 3.

## Level 3 — virtual bus integration ❌ not started

`python-can` provides a `virtual` bus and Linux provides `vcan`; both let
several processes exchange real CAN frames with no transceiver. The plan is to
run node logic compiled for the host against a virtual bus to test the Gateway
fan-out, multi-node startup ordering, and fault injection.

None of this exists yet. It is Phase 3–5 work.

## CI ✅ implemented

`.github/workflows/ci.yml` runs on every push and pull request:

| Job | Contents |
|-----|----------|
| `protocol` | `make all` — levels 1 and 2 above |
| `firmware` | builds `gateway.elf` for **both** bring-up modes with `arm-none-eabi-gcc`, uploads the ELFs |

The firmware job matters because CubeIDE generates its own makefiles at build
time, which is useless in CI. `tools/build_firmware.sh` reproduces the compile
and link with a plain toolchain, so a change that breaks only the dual-CAN path
cannot merge unnoticed.

## What still requires hardware

Stated plainly rather than implied away:

- bit timing and sample point — needs a scope
- transceiver behaviour, termination, signal integrity
- real arbitration under electrical contention
- bus-off entry and recovery timing
- end-to-end latency versus the original UART link

Those are Phase 2 and Phase 5 deliverables and belong in a test report with
scope captures, not in CI.
