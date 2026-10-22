/* Three missed cycles before declaring loss: one missed frame is noise on a
 * shared bus, three in a row is a fault. */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define APP_MOTION_CMD_PERIOD_MS 20U

#define APP_MOTION_CMD_TIMEOUT_MS (3U * APP_MOTION_CMD_PERIOD_MS)

/* Frames still arriving but CmdCounter frozen: the sender is alive but its
 * control task is wedged. */
#define APP_MOTION_CMD_STALE_MS APP_MOTION_CMD_TIMEOUT_MS

#define APP_HEARTBEAT_PERIOD_MS 100U

#define APP_HEARTBEAT_TIMEOUT_MS (3U * APP_HEARTBEAT_PERIOD_MS)

/* mm/s per second. A ramp rather than cutting the motor, which could skid or
 * tip a light vehicle. */
#define APP_FAILSAFE_DECEL_MM_S2 4000

#endif
