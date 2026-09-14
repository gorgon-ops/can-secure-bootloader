# CAN Bit Timing

Every node on the bus must agree on bit rate **and** land its sample point in
roughly the same place. Getting this wrong is the #1 cause of a bus that looks
wired correctly but never ACKs a frame.

## The arithmetic

A CAN bit is divided into time quanta (tq):

```
nominal bit time = SYNC_SEG (always 1 tq) + BS1 + BS2
bitrate          = f_APB1 / (Prescaler * (1 + BS1 + BS2))
sample point     = (1 + BS1) / (1 + BS1 + BS2)
```

bxCAN hardware limits: `BS1 = 1..16 tq`, `BS2 = 1..8 tq`, so the total is
capped at **25 tq**. CiA recommends a sample point of **87.5%**; anything in
the 80–90% band interoperates fine in practice.

## Target: 500 kbit/s on every node

### STM32F446RE @ APB1 = 42 MHz

`Prescaler * n_tq` must equal `42e6 / 500e3 = 84`.

| Prescaler | n_tq | BS1 | BS2 | Sample point | Legal? |
|-----------|------|-----|-----|--------------|--------|
| **6**     | **14** | **11** | **2** | **85.7%** | **yes — best available** |
| 7         | 12   | 9   | 2   | 83.3%        | yes |
| 7         | 12   | 10  | 1   | 91.7%        | yes |
| 4         | 21   | 17  | 3   | 85.7%        | no — BS1 > 16 |
| 4         | 21   | 16  | 4   | 81.0%        | yes, worse |
| 3         | 28   | —   | —   | —            | no — n_tq > 25 |

**Result: Prescaler 6 / BS1 11 tq / BS2 2 tq / SJW 1 tq.** 85.7% is not 87.5%,
and it cannot be: no legal combination at 42 MHz hits 87.5% exactly for
500 kbit/s. 85.7% is comfortably inside the interoperable band.

> This is already what `stm32sndworkspace/can comunication bfmc` uses. That
> configuration is correct and should be carried forward unchanged.

### STM32F103 (Blue Pill) @ APB1 = 36 MHz

`Prescaler * n_tq` must equal `36e6 / 500e3 = 72`.

| Prescaler | n_tq | BS1 | BS2 | Sample point | Notes |
|-----------|------|-----|-----|--------------|-------|
| **4**     | **18** | **15** | **2** | **88.9%** | **recommended** |
| 9         | 8    | 6   | 1   | 87.5%        | exact, but only 8 tq — coarse resync |
| 6         | 12   | 9   | 2   | 83.3%        | usable |

**Result: Prescaler 4 / BS1 15 tq / BS2 2 tq / SJW 1 tq.**

Prescaler 9 hits 87.5% exactly but leaves only 8 tq per bit, so a single tq of
SJW is 12.5% of the bit — much less tolerance for oscillator drift. The 18 tq
option is the better engineering trade even though its sample point is 1.4
points further from nominal.

### MCP2515 nodes @ 8 MHz crystal

`CAN_500KBPS, MCP_8MHZ` in the Arduino library. Confirm the module actually has
an 8 MHz can — **16 MHz crystals are common** and silently produce 250 kbit/s if
you pass `MCP_8MHZ`. Read the marking on the crystal, don't trust the listing.

## Verification before trusting any of it

1. Scope the CAN_H/CAN_L pair and measure one bit time: must be 2.0 µs ±1%.
2. Put one node in `CAN_MODE_SILENT` and confirm it receives the other's frames
   without ACKing (proves RX timing independently of TX).
3. Only then go to `CAN_MODE_NORMAL` on both.
