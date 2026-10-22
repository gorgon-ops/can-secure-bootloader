#include "motion_safety.h"

#include <string.h>

void MotionSafety_Init(motion_safety_t *ms, int32_t decel_mm_s2)
{
    memset(ms, 0, sizeof(*ms));
    ms->decel_mm_s2 = decel_mm_s2;
}

static int32_t MotionSafety_Step(const motion_safety_t *ms, uint32_t dt_ms)
{
    int32_t step = (ms->decel_mm_s2 * (int32_t)dt_ms) / 1000;

    /* Integer division can round the step to zero, leaving the vehicle coasting
     * forever. At least 1 mm/s per call so the ramp always terminates. */
    if ((step == 0) && (dt_ms > 0U))
    {
        step = 1;
    }

    return step;
}

void MotionSafety_Update(motion_safety_t *ms,
                         cmd_monitor_state_t state,
                         int16_t req_speed,
                         int16_t req_steer,
                         uint32_t dt_ms)
{
    int32_t step;
    int32_t speed;

    if (state == CMD_MONITOR_OK)
    {
        ms->failsafe_active = false;
        ms->speed_mm_s      = req_speed;
        ms->steer_deg_x10   = req_steer;
        return;
    }

    ms->failsafe_active = true;

    /* Steering holds: safer than snapping to centre mid-corner. */

    step  = MotionSafety_Step(ms, dt_ms);
    speed = ms->speed_mm_s;

    if (speed > 0)
    {
        speed -= step;
        if (speed < 0)
        {
            speed = 0;
        }
    }
    else if (speed < 0)
    {
        speed += step;
        if (speed > 0)
        {
            speed = 0;
        }
    }

    ms->speed_mm_s = (int16_t)speed;
}

bool MotionSafety_IsStopped(const motion_safety_t *ms)
{
    return ms->speed_mm_s == 0;
}
