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
| Motion | � | **CAN2 on the bus** | CAN2 on the bus |
| Sensor | � | � | **Arduino + MCP2515** |
| Logger | � | � | USB-CAN, listen-only |

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

## Software layering

Kept strictly separated so most of the codebase can be tested without hardware:

```
firmware/common/protocol/    generated pack/unpack from bfmc.dbc  built + tested
firmware/common/app/         state machines, timeout logic        not yet written
firmware/<node>/             HAL init, ISRs, board wiring         gateway only
```

The protocol layer has no STM32 HAL dependency, so it compiles with plain `gcc`
and runs under host tests in CI today. The application layer is being written to
the same constraint, which is what will let the timeout and heartbeat logic in
the table above be tested without a car on the desk.

This is why the generated-code approach matters: signal layout is defined once
in the DBC, and every node plus the Python tooling derives from that one file.
Two nodes cannot disagree about where a signal sits, because neither of them
hand-writes it. See [`docs/testing.md`](testing.md) for what is verified now and
what is not.
