#include "can_filter_plan.h"

#include "bfmc.h" /* frame IDs, generated from dbc/bfmc.dbc */

#include <string.h>

/**
 * What the Gateway ECU consumes: feedback from the actuators, the sensor
 * stream, peer liveness, and diagnostic responses. It does not listen to its
 * own commands.
 */
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
    BFMC_DIAG_RESP_FRAME_ID,
};

/**
 * What the Motion ECU consumes: commands, the vehicle safety state, peer
 * liveness, and diagnostic requests. Deliberately not the sensor stream — it
 * acts on commands, not on raw sensor data, and subscribing to traffic a node
 * does not use is exactly the cost this filtering exists to avoid.
 */
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
            return; /* already present: a role overlap, not an error */
        }
    }

    if (plan->count >= CAN_FILTER_PLAN_MAX_IDS)
    {
        /* Dropping an ID here would silently stop delivering a frame the node
           needs, so the caller must treat an incomplete plan as a fault. That
           is what CanFilterPlan_BanksRequired plus the bank-budget check in the
           transport is for. */
        return;
    }

    /* Insertion sort: the lists are a handful of entries and this keeps the
       programmed banks deterministic between builds. */
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
    /* Four 11-bit identifiers per bank in 16-bit list mode. */
    return (uint8_t)((plan->count + 3U) / 4U);
}
