# Bring-Up Checklist

For a NUCLEO-F446RE and two standalone MCP2551 transceivers. Work through it in
order: each step isolates one thing, and the counters tell you which.

## Wiring

MCP2551 is DIP-8:

| Pin | Name | Connect to |
|-----|------|-----------|
| 1 | TXD | STM32 CAN_TX **and** a 1 kΩ resistor to +5 V |
| 2 | VSS | GND, shared with the Nucleo |
| 3 | VDD | +5 V |
| 4 | RXD | STM32 CAN_RX |
| 5 | VREF | leave unconnected (it is an output) |
| 6 | CANL | bus CAN_L |
| 7 | CANH | bus CAN_H |
| 8 | Rs | **GND** for high-speed mode |

Per controller:

| Signal | MCU pin | Goes to |
|--------|---------|---------|
| CAN1_TX | **PA12** | transceiver A, pin 1 (+ pull-up to +5 V) |
| CAN1_RX | **PA11** | transceiver A, pin 4 |
| CAN2_TX | **PB13** | transceiver B, pin 1 (+ pull-up to +5 V) |
| CAN2_RX | **PB12** | transceiver B, pin 4 |

Those four are on the morpho headers. Find their positions on the board
silkscreen or in ST's UM1724 rather than from memory — a mis-identified pin
here means 5 V somewhere it should not be.

Both transceivers' CANH join one wire, both CANL join the other, with a 120 Ω
resistor across the pair at each physical end.

Pin 8 floating is a common and confusing mistake: slope control is then
undefined and the bus works erratically rather than not at all.

## Before applying power

1. **Continuity:** every GND is common — Nucleo, both transceivers, both supplies.
2. **No shorts:** CANH to CANL must not be a dead short; +5 V to GND must not be
   either.
3. **Termination:** with the board unpowered, measure CANH to CANL. It should
   read **≈ 60 Ω** — two 120 Ω resistors in parallel. 120 Ω means only one
   terminator is fitted; 40 Ω means you have three.
4. **Pull-ups present** on both TXD lines if you build with
   `CAN_TX_OPEN_DRAIN=1`. Without them the pin can never go high, TXD sits
   permanently dominant, and the node jams the bus for everyone.

## Step 0 — loopback, nothing connected

```bash
tools/build_firmware.sh loopback bringup
```

Unplug the transceivers entirely. Watch `g_can1_stats`:

| Field | Expected |
|-------|----------|
| `tx_ok` | climbing, ~2/s |
| `rx_count` | climbing, matching `tx_ok` |
| `err_count` | **0** |
| `last_rx_id` | `0x100` |

The controller receives its own frames internally. This proves the clock tree,
bit timing arithmetic, filter banks, mailboxes and the RX interrupt path with no
analog involved at all. If this fails, nothing downstream can work — and the
cause is firmware or clock configuration, never wiring.

## Step 1 — one transceiver, scope on the bus

Connect transceiver A only. Build `dual bringup` (add `CAN_TX_OPEN_DRAIN=1`).
Scope CANH against GND and measure one bit: it must be **2.0 µs ±1%** at
500 kbit/s. Anything else means the bit timing or the clock tree is wrong, and
no amount of rewiring will fix it.

Expect the node to report errors at this point — it has no partner. That is
Step 2.

## Step 2 — the lone-node signature

With one node transmitting and nothing to answer it:

| Field | Expected |
|-------|----------|
| `last_lec` | **3** — acknowledgment error |
| `last_tec` | climbing in steps of 8 |
| `bus_off` | eventually 1 |

`last_lec == 3` is the single most informative reading in this whole document.
It means the frame left the controller correctly and nobody acknowledged it.
Transmit works; there is simply no second node yet.

**Expect bus-off within about 16 seconds.** Each unacknowledged transmission
adds 8 to the transmit error counter, bus-off is at 255, and bring-up transmits
every 500 ms — roughly 32 frames. Because `AutoBusOff` is deliberately disabled,
the node stays off and the counters stop moving. That is correct behaviour for
this build, not a fault. Reset the board after connecting the second node.

If instead you see `last_lec == 0` and `tx_ok` climbing with no errors while
nothing else is attached, something is looping your own frames back — check you
did not build `loopback`.

## Step 3 — two nodes

Connect transceiver B. Same `dual bringup` build.

| Field | Expected |
|-------|----------|
| `g_can1_stats.rx_count` | climbing — CAN1 is hearing CAN2 |
| `g_can2_stats.rx_count` | climbing — CAN2 is hearing CAN1 |
| `g_can1_stats.last_rx_id` | `0x200` (the frame from CAN2) |
| `g_can2_stats.last_rx_id` | `0x100` (the frame from CAN1) |
| `err_count` both | stops climbing |
| `bus_off` both | 0 |

Each port receives the *other* one's ID, never its own — a node does not
receive its own transmissions on a real bus. Seeing your own ID come back means
you are still in loopback.

## Step 4 — the application

```bash
CAN_TX_OPEN_DRAIN=1 tools/build_firmware.sh dual app
```

Watch `g_bfmc_app`:

| Field | Expected |
|-------|----------|
| `mo_cmd_state` | `1` (`CMD_MONITOR_OK`) |
| `mo_failsafe` | `0` |
| `cmd_rx_count` | climbing at ~50/s |
| `decode_errors` | **0** |
| `gw_state`, `mo_state` | `1` (`NODE_STATE_RUNNING`) |
| `mo_out_speed` | sweeping 0 → 1000 → 0 over 4 s |

`decode_errors` climbing while `cmd_rx_count` does not means frames are arriving
but failing to unpack — a DBC and firmware mismatch, so rerun
`make check-generated`.

## Step 5 — prove the fail-safe

This is the demonstration worth recording for the writeup.

**Pull the CAN_H wire** while watching `g_bfmc_app`:

1. `mo_cmd_state` goes to `2` (`CMD_MONITOR_TIMEOUT`) within 60 ms
2. `mo_failsafe` goes to `1`
3. `mo_out_speed` ramps to 0 within ~375 ms rather than dropping instantly
4. `mo_out_steer` holds its last value
5. reconnect: `mo_cmd_state` returns to `1` and the sweep resumes

Steps 1–4 are the exact behaviour asserted in `tests/test_app.c` against a
synthetic clock. Seeing it on hardware confirms the wiring of that logic, not
the logic itself — which is the right division of labour.

## Symptom table

| Symptom | Most likely cause |
|---------|------------------|
| `last_lec == 3`, TEC climbing | no other live node; or the other node is not ACKing |
| Nothing transmits, bus stuck dominant | `CAN_TX_OPEN_DRAIN=1` with no pull-up fitted |
| Receives fine, transmits never acknowledged | TXD threshold — the 5 V problem; enable open-drain |
| Bit time wrong on the scope | clock tree or prescaler, not wiring |
| `last_lec` 1, 2 or 6 (stuff/form/CRC) | signal integrity: termination, stub length, ground |
| Erratic, works sometimes | Rs (pin 8) floating; or missing common ground |
| 120 Ω across the bus | only one terminator fitted |
| `bus_off == 1`, counters frozen | expected after ~16 s alone; reset after connecting the peer |
| Own ID comes back in `last_rx_id` | built `loopback` when you meant `dual` |

## What this cannot tell you

The counters prove the bus works. They say nothing about whether the sample
point is well placed — that needs the scope measurement in Step 1, and it is
worth doing even when everything appears to work, because a marginal sample
point fails later under temperature rather than now on the bench.
