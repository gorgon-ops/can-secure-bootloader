# Hardware

The plan is staged so that each tier produces a working, demonstrable result on
its own. Tier 0 needs nothing you do not already own.

## What you have

| Item | Status |
|------|--------|
| NUCLEO-F446RE | **two independent CAN controllers** — CAN1 on PA11/PA12, CAN2 on PB12/PB13 |
| Arduino + MCP2515 modules | ×2, each with an onboard 5 V transceiver |
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

## Tier 1 — two transceivers

You have **MCP2551**, which is a 5 V part. It works with a 3.3 V STM32, but only
with one extra resistor per node. Read the next section before wiring anything.

| Qty | Part | Purpose |
|-----|------|---------|
| 2 | **MCP2551** | one for CAN1, one for CAN2 |
| 2 | 1 kΩ – 4.7 kΩ resistor | TXD pull-up to +5 V — **not optional** |
| 2 | 120 Ω resistor | bus termination, one at each end |
| — | twisted pair | the bus |

```
              +5V                        +5V
               |                          |
             [1k]                       [1k]           <- TXD pull-ups
               |                          |
   PA12 CAN1_TX +-> TXD |         | CANH -+------+--------- CAN_H
   PA11 CAN1_RX <--- RXD | MCP2551| CANL -|--+   |
                     Rs--+ (GND)  |       |  |  [120R]
                         +--------+       |  |   |
              +5V                         |  |   |
               |                          |  |   |
             [1k]                         |  |   |
               |                          |  |   |
   PB13 CAN2_TX +-> TXD |         | CANH -+  |   |
   PB12 CAN2_RX <--- RXD | MCP2551| CANL ----+---+--------- CAN_L
                     Rs--+ (GND)  |        [120R]
                         +--------+
```

Both transceivers take **+5 V** on VDD (pin 3) and share ground with the Nucleo.
Tie **Rs (pin 8) to GND** for high-speed mode — floating it leaves the slope
control undefined. VREF (pin 5) is an output; leave it unconnected.

Build the firmware with the matching option:

```bash
CAN_TX_OPEN_DRAIN=1 tools/build_firmware.sh dual app
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

**The MCP2551 problem is the transmit direction, not receive.** Almost everyone
worries about the wrong one.

*Receive* is fine. RXD swings to about 5 V, but PA11 and PB12 are five-volt
tolerant inputs on the F446, so that direction is within spec as-is.

*Transmit* is not. The MCP2551 specifies its TXD logic-high threshold as
0.7 × VDD, which is **3.5 V at a 5.0 V supply**. An STM32 output pin reaches
about 3.2 V. The high level is below the threshold, so the transceiver may read
a recessive bit as dominant, intermittently, depending on the exact chip and
temperature.

That asymmetry is what makes it so confusing to debug: the node receives every
frame on the bus perfectly while its own transmissions are ignored, corrupted,
or — worst case — hold the bus dominant. It looks like a transmit bug in
firmware, and it is not.

**Fix: drive CAN_TX open-drain with a pull-up to +5 V.** One resistor per node,
no level-shifter IC. The STM32 pin only ever pulls low; the resistor supplies
the high level, so TXD sees a true 5 V recessive state. This fits CAN unusually
well, because the protocol is already dominant-low and recessive-high —
open-drain is arguably the more natural configuration for a CAN TX pin
regardless of transceiver.

Set `CAN_TX_OPEN_DRAIN=1`. The firmware then reconfigures PA12 and PB13 as
`GPIO_MODE_AF_OD` after the CubeMX GPIO block, so regenerating the project
cannot silently drop it.

**The pull-up is mandatory once that option is on.** With open-drain and no
pull-up, TXD can never go high, the transceiver sits permanently dominant, and
the node jams the whole bus for everyone. That is why the option defaults to
off: the configuration that needs no external parts is the safe one.

At 1 kΩ the rise time is tens of nanoseconds against a 2 µs bit time at
500 kbit/s, so there is no speed concern. 4.7 kΩ is also fine; 10 kΩ starts to
matter if you later move to 1 Mbit/s.

If you ever buy a 3.3 V transceiver such as the SN65HVD230, leave the option off
and use push-pull — no resistor needed.

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
