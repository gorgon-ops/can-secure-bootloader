#include "can_filter_plan.h"

#include "bfmc.h"

#include <string.h>

/* The Gateway does not listen to its own commands. */
static const uint16_t s_gateway_ids[] = {
    BFMC_EMERGENCY_STOP_FRAME_ID,
    BFMC_MOTION_STATUS_FRAME_ID,
    BFMC_WHEEL_ENCODER_FRAME_ID,
    BFMC_IMU_ACCEL_FRAME_ID,
    BFMC_IMU_GYRO_FRAME_ID,
    BFMC_IMU_ATTITUDE_FRAME_ID,
    BFMC_RANGE_FRONT_FRAME_ID,
    BFMC_BATTERY_FRAME_ID,
    BFMC_HB_MOTION_FRAME_ID,
    BFMC_HB_SENSOR_FRAME_ID,
    BFMC_DIAG_REQ_FRAME_ID,
    BFMC_DIAG_RESP_FRAME_ID,
};

/* Not the sensor stream: Motion acts on commands, not raw sensor data. */
static const uint16_t s_motion_ids[] = {
    BFMC_EMERGENCY_STOP_FRAME_ID,
    BFMC_SAFETY_STATE_FRAME_ID,
    BFMC_MOTION_CMD_FRAME_ID,
    BFMC_HB_GATEWAY_FRAME_ID,
    BFMC_HB_SENSOR_FRAME_ID,
    BFMC_DIAG_REQ_FRAME_ID,
};

static void CanFilterPlan_AddSorted(can_filter_plan_t *plan, uint16_t id)
{
    uint8_t i;
    uint8_t pos;

    for (i = 0U; i < plan->count; i++)
    {
        if (plan->ids[i] == id)
        {
            return;
        }
    }

    if (plan->count >= CAN_FILTER_PLAN_MAX_IDS)
    {
        /* Unreachable with a correct plan; the bank-budget check in the transport
         * turns an incomplete one into a fault. */
        return;
    }

    /* Sorted, so the programmed banks are deterministic between builds. */
    pos = plan->count;
    while ((pos > 0U) && (plan->ids[pos - 1U] > id))
    {
        plan->ids[pos] = plan->ids[pos - 1U];
        pos--;
    }
    plan->ids[pos] = id;
    plan->count++;
}

void CanFilterPlan_Build(can_filter_plan_t *plan, uint32_t role_mask)
{
    size_t i;

    memset(plan, 0, sizeof(*plan));

    if ((role_mask & BFMC_ROLE_GATEWAY) != 0U)
    {
        for (i = 0U; i < (sizeof(s_gateway_ids) / sizeof(s_gateway_ids[0])); i++)
        {
            CanFilterPlan_AddSorted(plan, s_gateway_ids[i]);
        }
    }

    if ((role_mask & BFMC_ROLE_MOTION) != 0U)
    {
        for (i = 0U; i < (sizeof(s_motion_ids) / sizeof(s_motion_ids[0])); i++)
        {
            CanFilterPlan_AddSorted(plan, s_motion_ids[i]);
        }
    }
}

bool CanFilterPlan_Accepts(const can_filter_plan_t *plan, uint16_t std_id)
{
    uint8_t i;

    for (i = 0U; i < plan->count; i++)
    {
        if (plan->ids[i] == std_id)
        {
            return true;
        }
    }

    return false;
}

uint8_t CanFilterPlan_BanksRequired(const can_filter_plan_t *plan)
{
    return (uint8_t)((plan->count + 3U) / 4U);
}
