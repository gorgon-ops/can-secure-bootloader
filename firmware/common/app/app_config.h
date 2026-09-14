/**
 * Timing budgets for the application layer.
 *
 * Every value here is derived from the cycle times in docs/message-matrix.md.
 * The rule is three missed cycles before declaring loss: one missed frame is
 * noise on a shared bus, three in a row is a fault. Tightening this to a single
 * cycle would make the vehicle trip on ordinary arbitration delay.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/** MOTION_CMD is published every 20 ms by the Gateway. */
#define APP_MOTION_CMD_PERIOD_MS 20U

/** No MOTION_CMD at all for this long: the sender is gone or the bus is down. */
#define APP_MOTION_CMD_TIMEOUT_MS (3U * APP_MOTION_CMD_PERIOD_MS)  /* 60 ms */

/**
 * Frames still arriving but CmdCounter frozen for this long.
 *
 * This is the failure a heartbeat cannot see: the sender is alive enough to
 * keep transmitting, but its control task is wedged and the payload is stale.
 * Same budget as a hard timeout, because the consequence is identical.
 */
#define APP_MOTION_CMD_STALE_MS APP_MOTION_CMD_TIMEOUT_MS           /* 60 ms */

/** Heartbeats are published every 100 ms by each node. */
#define APP_HEARTBEAT_PERIOD_MS 100U

/** Three missed heartbeats before a node counts as lost. */
#define APP_HEARTBEAT_TIMEOUT_MS (3U * APP_HEARTBEAT_PERIOD_MS)     /* 300 ms */

/**
 * Deceleration applied when the command source is lost, in mm/s per second.
 *
 * Cutting the motor outright would be abrupt enough to skid or tip a light
 * vehicle, and an uncontrolled slide is not a safe state. A bounded ramp stops
 * the car quickly while keeping it steerable and predictable.
 */
#define APP_FAILSAFE_DECEL_MM_S2 4000

#endif /* APP_CONFIG_H */
