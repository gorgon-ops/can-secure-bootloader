# Architecture

## Problem

The stock BFMC vehicle runs a single Nucleo doing motor control, steering, and
sensor acquisition, talking to a Raspberry Pi over one UART link. That is a
star topology with a single point of failure and no way to add a node without
touching the one firmware image.

Real vehicles distribute function across ECUs on a shared multi-master bus. This
project rebuilds the vehicle's internals that way.

## Node topology

```
           +-------------------+
           |  Raspberry Pi 4   |   high-level perception & planning
           |  (BFMC stack)     |
           +---------+---------+
                     | USB CDC / SPI
           +---------+---------+
           |   Gateway ECU     |  node 1
           |   STM32F446RE     |  translates Pi API <-> CAN frames
           +---------+---------+
                     |
  ===================+=========+===============+========  CAN bus, 500 kbit/s
    120R             |         |               |     120R
                     |         |               |
           +---------+--+  +---+--------+  +---+---------+
           | Motion ECU |  | Sensor ECU |  | Logger      |  node 4
           | STM32F103  |  | STM32F103  |  | USB-CAN->PC |  passive
           | node 2     |  | node 3     |  |             |
           +------------+  +------------+  +-------------+
             motor PWM       IMU (I2C)       SavvyCAN /
             servo PWM       ToF / ultrasonic  python-can
             encoder         battery ADC
```

## Mapping logical nodes onto real hardware

The four logical nodes do not need four boards. The F446 has two independent CAN
controllers, so one Nucleo hosts two nodes:

| Logical node | Tier 0 (no parts) | Tier 1 (2 transceivers) | Tier 2 (+ Arduino) |
|---|---|---|---|
| Gateway | CAN1, silent-loopback | CAN1 on the bus | CAN1 on the bus |
| Motion | — | **CAN2 on the bus** | CAN2 on the bus |
| Sensor | — | — | **Arduino + MCP2515** |
| Logger | — | — | USB-CAN, listen-only |

Because the node logic is written against the protocol layer rather than against
a specific controller, moving a node from "another task on this MCU" to "another
chip on the bus" is a wiring change, not a rewrite. See
[docs/hardware.md](hardware.md) for what each tier costs and proves.

## Responsibilities

| Node | Owns | Must not |
|------|------|----------|
| **Gateway** | Pi link, arm/disarm, fault aggregation, diagnostics master | Touch actuators directly |
| **Motion** | Closed-loop speed control, steering servo, encoder | Trust a command whose counter has not advanced |
| **Sensor** | IMU, range, battery; publishes at fixed rate | Depend on any other node to run |
| **Logger** | Passive capture, fault injection during test | Ever ACK-influence the bus in normal operation |

The Logger runs in `CAN_MODE_SILENT` so it cannot perturb the system it is
measuring — the standard reason a bus analyser is listen-only by default.

## Failure behaviour

This is the part that distinguishes the project from a demo that only works when
everything is plugged in.

| Failure | Detected by | Response |
|---------|-------------|----------|
| Gateway stops sending `MOTION_CMD` | Motion: 60 ms timeout (3 cycles) | Ramp to zero speed, hold steering, set `CMD_TIMEOUT` |
| Gateway wedged but still transmitting | Motion: `CmdCounter` not advancing | Same as timeout, set `CMD_STALE` |
| Sensor ECU dies | Gateway + Motion: missing `HB_SENSOR` for 300 ms | Gateway enters `DEGRADED`, forbids arming |
| Node goes bus-off | bxCAN error counters | Enter `BUS_OFF`, attempt recovery, report via `DIAG_RESP` |
| Bus flooded | Gateway: TX mailbox never drains | Emergency stop, `DEGRADED` |
| Physical disconnect | Everyone: all heartbeats gone | Fail-safe stop |

The rule: **absence of a message is itself information.** Every periodic frame
has a consumer-side timeout. A node that hears nothing assumes the worst.

## Interrupts and concurrency

### Priorities

CubeMX configures `NVIC_PRIORITYGROUP_0`, which allocates **zero bits to
pre-emption** and four to sub-priority. The consequence is worth stating plainly
because it removes a whole class of bug: **no interrupt can pre-empt another.**
Every ISR runs to completion before the next begins, so there is no nesting
between the eight CAN vectors, and none between CAN and SysTick.

`TICK_INT_PRIORITY` is also 0. When SysTick and a CAN interrupt are pending
together the tie is broken by exception number, and SysTick (exception 15) wins
against CAN (exception 36 and above). A burst of CAN interrupts therefore
delays the tick by the length of one ISR, but cannot starve it. That matters:
every timeout in the application is measured against `HAL_GetTick()`, so a
starved tick would freeze the supervision logic precisely when the bus is in
trouble.

### What runs in interrupt context

Only two callbacks, both in `can_if.c`, both dispatching on `hcan->Instance`:

- `HAL_CAN_RxFifo0MsgPendingCallback` — read the frame out of the FIFO, unpack
  it, drop it into a single-slot mailbox, set a flag. No supervision, no
  actuator maths, no transmission.
- `HAL_CAN_ErrorCallback` — record the error code and the decoded `CAN_ESR`.

The receive ISR is deliberately short because the hardware FIFO is only three
frames deep. Anything slow there loses traffic.

### Handing data to the main loop

Each frame type has a one-slot mailbox written by the ISR and drained by
`BfmcApp_Task()`. The task copies the slot inside a critical section, because
without one an arriving frame could overwrite the payload between the task
reading the counter and reading the speed — producing a torn value that belongs
to neither frame.

The critical section saves and restores `PRIMASK` rather than calling
`__disable_irq()` / `__enable_irq()`. The naive pair is wrong the moment the
code is called from somewhere that already had interrupts masked: the
unconditional enable silently breaks the caller's critical section. See
`critical_section.h`.

A one-slot mailbox drops frames if the task falls behind, so both drops are
counted (`cmd_overruns`, `hb_overruns`). At 50 Hz against a main loop running
orders of magnitude faster these should stay at zero; either climbing means the
loop has stalled, and that is worth knowing rather than silently absorbing.

### Faults the transport reports

`CanIf_Poll()` runs every loop and refreshes the error state by reading
`CAN_ESR` directly. Polling as well as taking the interrupt matters because the
error interrupt fires on a *transition*: between transitions the counters would
go stale, and a bus that had recovered would still read as broken.

| Signal | Meaning |
|---|---|
| `can_health` | one-word verdict: `OK`, `NO_PEER`, `WARNING`, `PASSIVE`, `BUS_OFF` |
| `last_lec == 3` | acknowledgment error — transmit works, nothing is answering |
| `tec` / `rec` | error counters; 96 is warning, 128 error-passive, 255 bus-off |
| `rx_overrun` | the hardware FIFO overflowed and frames were lost |
| `recoveries` | bus-off recovery attempts started |
| `decode_errors` | a frame arrived but failed to unpack — DBC and firmware disagree |

`AutoBusOff` is disabled, so the controller will not rejoin the bus by itself.
`CanIf_Poll()` performs the recovery instead: it cycles the controller through
initialisation mode, rate-limited to one attempt every 500 ms so a genuinely
dead bus is not hammered, and counts the attempts so an intermittent fault is
still visible after the bus comes back.

### Known gaps

Stated rather than implied away:

- **No watchdog.** A wedged main loop currently stops the fail-safe logic along
  with everything else. An IWDG would catch it; it is not fitted yet.
- **`Error_Handler()` halts forever** with interrupts disabled. Correct for
  bring-up, where a silent misconfiguration is worse than a stopped board, and
  wrong for a vehicle.
- **Accept-all receive filters.** Every node currently sees every frame. The
  vehicle build should narrow each node in hardware to the IDs it consumes.

## Software layering

Kept strictly separated so most of the codebase can be tested without hardware:

```
firmware/common/protocol/    generated pack/unpack from bfmc.dbc  built + tested
firmware/common/app/         state machines, timeout logic        built + tested
firmware/<node>/             HAL init, ISRs, board wiring         gateway only
```

Neither the protocol nor the application layer has an STM32 HAL dependency, so
both compile with plain `gcc` and run under host tests in CI today. That is what
lets the failure table above be verified without a car on the desk: the modules
take the clock as a parameter instead of calling `HAL_GetTick()`, so a test can
advance a synthetic clock and assert the exact millisecond of every transition.

Both layers are cross-compiled for Cortex-M4 in CI and are now called from the
Gateway build: `firmware/gateway/Core/Src/bfmc_app.c` runs the Gateway and
Motion roles against them. That file is deliberately thin -- it gives the tested
logic a clock, a transport and somewhere to put its output, and contains no
supervision logic of its own.

The transport underneath it, `can_if.c`, exists because the HAL dispatches receive
and error interrupts to exactly one callback symbol per event. That dispatch has
to live in one place; the raw bring-up harness and the application both sit on
top of it rather than defining competing callbacks.

This is why the generated-code approach matters: signal layout is defined once
in the DBC, and every node plus the Python tooling derives from that one file.
Two nodes cannot disagree about where a signal sits, because neither of them
hand-writes it. See [`docs/testing.md`](testing.md) for what is verified now and
what is not.
