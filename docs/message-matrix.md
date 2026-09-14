# Message Matrix

The authoritative definition is [`dbc/bfmc.dbc`](../dbc/bfmc.dbc). This document
explains the reasoning; the DBC is what code and tools actually consume.

## ID allocation scheme

CAN arbitration is won by the **numerically lowest ID**, so the ID map *is* the
priority map. Ranges are assigned by urgency, not by sender:

| Range | Class | Rationale |
|-------|-------|-----------|
| `0x000–0x0FF` | Safety / emergency | Must win arbitration against all traffic |
| `0x100–0x1FF` | Actuator commands | Late command = vehicle drives on stale input |
| `0x200–0x2FF` | Motion feedback | Closed-loop control depends on it |
| `0x300–0x3FF` | Sensor data | Tolerant of one missed cycle |
| `0x400–0x4FF` | Heartbeats | Liveness only, low rate |
| `0x600–0x7FF` | Diagnostics | Never time-critical, must not crowd the bus |

## Frames

| ID | Name | Sender | Cycle | DLC | Contents |
|----|------|--------|-------|-----|----------|
| `0x010` | `EMERGENCY_STOP` | any | event | 2 | source id, reason code |
| `0x080` | `SAFETY_STATE` | Gateway | 20 ms | 2 | arm state, fault bitfield |
| `0x100` | `MOTION_CMD` | Gateway | 20 ms | 5 | target speed, steering angle, rolling counter |
| `0x200` | `MOTION_STATUS` | Motion | 20 ms | 8 | measured speed, measured angle, motor current, flags |
| `0x210` | `WHEEL_ENCODER` | Motion | 20 ms | 8 | cumulative ticks, distance |
| `0x300` | `IMU_ACCEL` | Sensor | 20 ms | 6 | ax, ay, az |
| `0x310` | `IMU_GYRO` | Sensor | 20 ms | 6 | gx, gy, gz |
| `0x320` | `IMU_ATTITUDE` | Sensor | 20 ms | 4 | yaw, yaw rate |
| `0x330` | `RANGE_FRONT` | Sensor | 50 ms | 4 | distance, sensor status |
| `0x340` | `BATTERY` | Sensor | 500 ms | 6 | pack voltage, current, state of charge |
| `0x400` | `HB_GATEWAY` | Gateway | 100 ms | 2 | counter, node state |
| `0x401` | `HB_MOTION` | Motion | 100 ms | 2 | counter, node state |
| `0x402` | `HB_SENSOR` | Sensor | 100 ms | 2 | counter, node state |
| `0x600` | `DIAG_REQ` | Gateway | event | 8 | target node, service id, payload |
| `0x700` | `DIAG_RESP` | any | event | 8 | source node, service id, payload |

## Design decisions worth defending in an interview

**`MOTION_CMD` carries a rolling counter.** The Motion ECU rejects a command
whose counter has not advanced. This catches a stuck Gateway that is still
transmitting — a failure a plain heartbeat cannot see, because the heartbeat
task may be alive while the control task is wedged.

**IMU is split across three frames rather than packed into one.** Accel and gyro
are 3 x int16 = 6 bytes each; they do not fit in one 8-byte payload. Splitting on
a physical-quantity boundary also lets a consumer subscribe to only what it
needs via hardware filters.

**Heartbeats are separate IDs per node, not one shared ID.** One ID per sender
means a receiver can hardware-filter for exactly the nodes it depends on, and a
missing heartbeat is unambiguous. A shared ID would require inspecting the
payload to know who is missing.

**Nothing is faster than 20 ms.** 50 Hz is sufficient for this vehicle's control
loop and keeps the bus load low enough that a burst of diagnostic traffic cannot
starve the control path.

## Bus load

Worst-case bits per standard frame, including stuffing and the 3-bit interframe
space, approximated as `55 + 10 * DLC`:

| Group | Frames | Bits/cycle | Cycle | bit/s |
|-------|--------|-----------|-------|-------|
| 20 ms | `0x080,0x100,0x200,0x210,0x300,0x310,0x320` | 775 | 20 ms | 38 750 |
| 50 ms | `0x330` | 95 | 50 ms | 1 900 |
| 100 ms | `0x400,0x401,0x402` | 225 | 100 ms | 2 250 |
| 500 ms | `0x340` | 115 | 500 ms | 230 |
| | | | **Total** | **≈ 43 130** |

At 500 kbit/s that is **≈ 8.6% bus load**. Automotive practice keeps steady-state
load under 30–40% so that priority inversion stays bounded under burst; there is
ample headroom here for diagnostics, CAN-FD experiments, and the stretch goals.

Worst-case response time for `0x100 MOTION_CMD`: blocking is limited to one
in-progress lower-priority frame (max 135 bits = 270 µs at 500 kbit/s) plus
higher-priority traffic from `0x010` and `0x080`. Well inside the 20 ms budget.
