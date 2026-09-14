# Testing Without the Car

The weakness of most embedded portfolio projects is that nothing can be verified
unless the hardware is plugged in. That makes regressions invisible, makes the
repo unreviewable by anyone who does not own the boards, and leaves no evidence
the thing ever worked.

This project is structured so the majority of its logic is testable on a host
machine, in CI, with no STM32 present.

## Three levels

### 1. Database validation — no code, no hardware

`tools/validate_dbc.py` parses `dbc/bfmc.dbc` and enforces:

- no overlapping signals, no signal past its DLC (via cantools strict mode)
- no duplicate frame IDs
- steady-state bus load within budget (currently 8.6% of 30%)

Runs in under a second. Catches the "someone added a frame by hand" class of bug
before it reaches a board.

### 2. Host unit tests — logic, no hardware

`firmware/common/` has no STM32 HAL dependency, so it compiles for the host with
plain gcc. What gets tested there:

- **Pack/unpack round-trips.** Encode a signal, decode it, assert equality
  across the full range including negatives and saturation limits. Cross-check
  the C encoder against `cantools` encoding the same value from Python — if the
  two disagree, the C is wrong.
- **Timeout state machines.** Feed the Motion ECU's command handler a synthetic
  clock and a sequence of frames; assert it enters fail-safe after 60 ms of
  silence, and after 60 ms of frames with a frozen `CmdCounter`.
- **Heartbeat supervision.** Assert `DEGRADED` on a missing `HB_SENSOR`.

These are the behaviours that matter for safety and they are exactly the ones
that are painful to trigger on real hardware.

### 3. Virtual bus integration — full network, no hardware

`python-can` provides a `virtual` bus; Linux provides `vcan`. Both let several
processes exchange real CAN frames with no transceiver. Run the node logic
compiled for host against a virtual bus and you can test:

- the Gateway's Pi-API → CAN fan-out end to end
- multi-node startup ordering
- fault injection: stop publishing a heartbeat, flood the bus, send a frozen
  counter, and assert every consumer reacts as documented

```bash
# Linux
sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0
candump vcan0
```

On Windows, `python-can`'s `virtual` interface works in-process without the
kernel module.

## What still requires hardware

Be honest about this in the README rather than implying full coverage:

- bit timing and sample point (needs a scope)
- transceiver behaviour, termination, signal integrity
- real arbitration under electrical contention
- bus-off entry and recovery timing
- actual end-to-end latency versus the original UART link

Those are Phase 2 and Phase 5 deliverables and they belong in a test report with
scope captures, not in CI.

## CI

A GitHub Actions workflow running levels 1 and 2 on every push costs nothing and
means the repo carries a green badge that reflects something real. For a
portfolio piece this is disproportionately valuable: it signals that you think
about verification, not just about getting a demo to work once.
