# Hardware

## What you already have

| Item | Status |
|------|--------|
| NUCLEO-F446RE | confirmed — CAN1 on PA11/PA12, CAN2 on PB12/PB13 |
| Arduino + MCP2515 modules | confirmed, ×2 sketches exist |
| Working 500 kbit/s bxCAN config | confirmed in `stm32sndworkspace/can comunication bfmc` |

## What the architecture needs

Three bxCAN nodes plus a bus analyser. One Nucleo is not enough.

| Qty | Part | Purpose | Approx |
|-----|------|---------|--------|
| 2 | STM32F103C8T6 "Blue Pill" | Motion + Sensor ECUs | €4 ea |
| 1 | ST-Link V2 clone | Flash the Blue Pills | €3 |
| 4 | **SN65HVD230** transceiver breakout | One per node | €2 ea |
| 1 | CANable / CANtact USB-CAN | Logger + PC tooling | €15–30 |
| — | Twisted pair, 2× 120 Ω resistors | Bus medium and termination | €2 |

Total roughly €40–60.

## Two hardware traps to avoid

**Do not buy MCP2515 modules for the STM32 nodes.** The MCP2515 is a *CAN
controller* with an SPI interface. The STM32F1/F4 already has a controller
(bxCAN) built in — it only needs a *transceiver*, the analog line driver. Using
an MCP2515 module would mean ignoring bxCAN entirely and talking SPI instead,
which is a different (and less impressive) project. Buy bare transceiver
breakouts.

**Prefer SN65HVD230 over TJA1050/MCP2551.** The SN65HVD230 is 3.3 V native,
which is what the STM32 I/O runs at. TJA1050 and MCP2551 are 5 V parts: their
receiver output can drive ~5 V into the STM32 CAN_RX pin. PA11 is 5 V-tolerant
on the F446 so it often survives, but it is out of spec and a common source of
intermittent faults. The MCP2515 modules you already own carry a 5 V TJA1050 —
fine for the Arduino nodes, not for direct STM32 wiring.

## Bus wiring

```
  Node1        Node2        Node3        Node4
   | |          | |          | |          | |
 CANH CANL    CANH CANL    CANH CANL    CANH CANL
   | |          | |          | |          | |
 --+-+----------+-+----------+-+----------+-+--
  [120R]                                [120R]
   ^                                      ^
   termination at the two physical ends only
```

Termination goes at the **two ends of the bus**, not at every node and not at
the electrical middle. Stubs from the trunk to each node should stay under
~30 cm at 500 kbit/s. With the bus unpowered, measuring CAN_H to CAN_L should
read ~60 Ω — two 120 Ω resistors in parallel. If you read 120 Ω you have one
terminator; if you read 40 Ω you have three. This measurement catches most
first-day wiring problems in ten seconds.

## Bring-up order

1. One node, `CAN_MODE_LOOPBACK`. Proves clock, pins, and filter config with no
   bus at all.
2. One node + scope on CAN_H. Measure the bit time: 2.0 µs at 500 kbit/s.
3. Two nodes, one in `CAN_MODE_SILENT`. Proves RX timing without ACK coupling.
4. Two nodes both `CAN_MODE_NORMAL`. First real arbitration.
5. Add remaining nodes one at a time, re-checking bus resistance each time.

Skipping to step 4 is the usual reason a bus "doesn't work" with no clue why:
a missing ACK looks identical to bad bit timing, wrong pins, or an unpowered
transceiver.
