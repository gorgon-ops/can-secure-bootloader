/* On anything but healthy commands, speed ramps to zero and steering holds its
 * last value. Held rather than centred: snapping the wheels straight mid-corner
 * is its own hazard. */
#ifndef MOTION_SAFETY_H
#define MOTION_SAFETY_H

#include "cmd_monitor.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    int32_t decel_mm_s2;      /* e.g. APP_FAILSAFE_DECEL_MM_S2 */

    int16_t speed_mm_s;
    int16_t steer_deg_x10;
    bool    failsafe_active;
} motion_safety_t;

void MotionSafety_Init(motion_safety_t *ms, int32_t decel_mm_s2);

/* req_speed in mm/s, req_steer in 0.1 deg. TIMEOUT and STALE are handled the
 * same here: the distinction is for diagnostics, not for the actuators. */
void MotionSafety_Update(motion_safety_t *ms,
                         cmd_monitor_state_t state,
                         int16_t req_speed,
                         int16_t req_steer,
                         uint32_t dt_ms);

bool MotionSafety_IsStopped(const motion_safety_t *ms);

#endif
