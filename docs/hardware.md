# Hardware

The plan is staged so that each tier produces a working, demonstrable result on
its own. Tier 0 needs nothing you do not already own.

## What you have

| Item | Status |
|------|--------|
| NUCLEO-F446RE | **two independent CAN controllers** — CAN1 on PA11/PA12, CAN2 on PB12/PB13 |
| Arduino + MCP2515 modules | ×2, each with an onboard transceiver |
| Verified 500 kbit/s bit timing | prescaler 6 / BS1 11 / BS2 2, sample point 85.7% |

The F446 having **two** bxCAN peripherals is what makes this project feasible on
one board. They are separate peripherals with their own mailboxes, filter banks,
error counters and interrupt vectors. Wired to a bus through two transceivers,
they are two nodes in every sense the protocol cares about.

## Tier 0 — nothing to buy

`CAN_BRINGUP_MODE = CAN_BRINGUP_LOOPBACK` puts CAN1 into silent-loopback: the
controller receives its own transmissions internally and never drives the bus.
Nothing needs to be attached to the board.

What this genuinely validates:

- clock tree and prescaler arithmetic
- filter bank configuration and FIFO assignment
- the TX mailbox and RX interrupt paths
- the entire message set, pack/unpack, state machines, timeouts and heartbeats

What it cannot validate: anything analog. No transceiver, no arbitration, no
ACK, no bit timing against another oscillator.

This tier is worth taking seriously rather than rushing past. Most of the
software risk in the project can be retired here, before any part arrives.

## Tier 1 — two transceivers, about €4

| Qty | Part | Purpose |
|-----|------|---------|
| 2 | **SN65HVD230** breakout | one for CAN1, one for CAN2 |
| 2 | 120 Ω resistor | termination at each end |
| — | twisted pair | the bus |

```
   PA12 CAN1_TX --> D |            | H ----+-------------+---- CAN_H
   PA11 CAN1_RX <-- R |  SN65HVD230| L --+ |             |
                      +------------+     | |           [120R]
                                         | |             |
   PB13 CAN2_TX --> D |            | H --|-+             |
   PB12 CAN2_RX <-- R |  SN65HVD230| L --+---------------+---- CAN_L
                      +------------+   [120R]
```

`CAN_BRINGUP_MODE = CAN_BRINGUP_DUAL` then gives a real two-node bus. CAN1
transmits `0x100`, CAN2 transmits `0x200`, each receives the other. Now
arbitration, ACK and error signalling are real, and the bit timing can be
verified on a scope.

One honest caveat to state in the writeup: both controllers run from the same
crystal, so there is no relative oscillator drift between them. Resynchronisation
is therefore exercised less hard than it would be between separate boards. That
is what Tier 2 fixes.

## Tier 2 — third node, nothing to buy

Your Arduino plus an MCP2515 module joins the same bus as a third node. The
module carries its own controller *and* transceiver, so it needs no extra parts.

This tier is valuable out of proportion to its cost: the Arduino has its own
crystal, so the bus now carries traffic between genuinely independent clock
domains. Different silicon, different vendor, different oscillator — which is
what a real vehicle bus looks like.

**Check the MCP2515 module's crystal before trusting the bit rate.** The sketches
pass `MCP_8MHZ`; 16 MHz crystals are common on these modules and would silently
give 250 kbit/s against the STM32's 500. Read the marking on the can.

## Two traps worth knowing

**Do not buy MCP2515 modules for the STM32 side.** The MCP2515 is a CAN
*controller* with an SPI interface. The F446 already has two controllers built
in; it needs only a *transceiver*, the analog line driver. Wiring an MCP2515 to
the STM32 would mean ignoring bxCAN entirely and talking SPI instead — a
different and considerably less interesting project.

**Prefer SN65HVD230 over TJA1050/MCP2551 for the STM32.** The SN65HVD230 is
3.3 V native. TJA1050 and MCP2551 are 5 V parts whose receiver output can drive
~5 V into the CAN_RX pin. PA11 is 5 V-tolerant so it usually survives, but it is
out of spec and a classic source of intermittent faults. The transceiver on your
existing MCP2515 modules is a 5 V TJA1050 — fine for the Arduino node, not for
direct STM32 wiring.

## Pin availability

CAN1 (PA11/PA12) and CAN2 (PB12/PB13) are all on the CN10 morpho header of the
NUCLEO-64. Confirm exact pin positions against the board's morpho map before
wiring — do not go from memory.

Note that the ported project also enables SPI1 on PA5/PA6/PA7, left over from an
earlier MCP2515 experiment. It is unused in this design, and PA5 is the LD2 user
LED. Removing SPI1 in CubeMX frees the LED for status indication.

## Bring-up order

1. **Tier 0 loopback.** `rx_count` climbs with nothing attached. Proves clocks,
   filters and the interrupt path.
2. **One transceiver, scope on CAN_H.** Measure one bit: 2.0 µs at 500 kbit/s.
3. **Tier 1, CAN2 in `CAN_MODE_SILENT`.** Proves RX timing without ACK coupling.
4. **Tier 1, both normal.** First real arbitration. `0x100` must beat `0x200`.
5. **Tier 2, add the Arduino.** Re-check bus resistance after every node added.

Skipping to step 4 is the usual reason a bus "doesn't work" with no clue why —
a missing ACK looks identical to bad bit timing, wrong pins, or an unpowered
transceiver. With the bus unpowered, CAN_H to CAN_L should measure ~60 Ω: two
120 Ω terminators in parallel. 120 Ω means you have one; 40 Ω means three.
