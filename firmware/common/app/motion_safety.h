/**
 * Fail-safe actuator output for the Motion ECU.
 *
 * Turns a command-supervision verdict into the values actually sent to the
 * motor and steering servo. While commands are healthy the request passes
 * through unchanged; the moment supervision reports anything else, speed is
 * ramped to zero at a bounded rate and steering is frozen at its last value.
 *
 * Steering is held rather than centred on purpose. Snapping the wheels straight
 * mid-corner is its own hazard, and a vehicle that is already decelerating will
 * stop in under half a second: holding the current line is the more
 * predictable outcome.
 *
 * Pure module: no globals, no I/O, no HAL.
 */
#ifndef MOTION_SAFETY_H
#define MOTION_SAFETY_H

#include "cmd_monitor.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    int32_t decel_mm_s2;      /*!< ramp rate, e.g. APP_FAILSAFE_DECEL_MM_S2 */

    int16_t speed_mm_s;       /*!< value to send to the motor controller    */
    int16_t steer_deg_x10;    /*!< value to send to the steering servo      */
    bool    failsafe_active;  /*!< true whenever the request is not passed through */
} motion_safety_t;

/** @brief Reset outputs to standstill and set the ramp rate. */
void MotionSafety_Init(motion_safety_t *ms, int32_t decel_mm_s2);

/**
 * @brief Advance the actuator outputs by one control cycle.
 *
 * @param state      verdict from CmdMonitor_Poll()
 * @param req_speed  requested speed, mm/s, from the last MOTION_CMD
 * @param req_steer  requested steering, 0.1 deg, from the last MOTION_CMD
 * @param dt_ms      elapsed time since the previous call
 *
 * In CMD_MONITOR_OK the requested values pass through. In every other state the
 * request is ignored: speed ramps toward zero and steering holds. Note that
 * TIMEOUT and STALE are treated identically here even though they are reported
 * separately -- the distinction matters for diagnostics, not for what the
 * actuators should do.
 */
void MotionSafety_Update(motion_safety_t *ms,
                         cmd_monitor_state_t state,
                         int16_t req_speed,
                         int16_t req_steer,
                         uint32_t dt_ms);

/** @brief True once the fail-safe ramp has brought the vehicle to standstill. */
bool MotionSafety_IsStopped(const motion_safety_t *ms);

#endif /* MOTION_SAFETY_H */
