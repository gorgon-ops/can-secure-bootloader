/**
 * @file    can_filter_plan.h
 * @brief   Which CAN IDs each node role needs to receive.
 *
 * Accept-all filters are right for bring-up, where the point is to see whatever
 * is on the wire. They are wrong for the vehicle: every node wakes for every
 * frame, so the receive interrupt rate is the whole bus load rather than the
 * part that node cares about, and the three-deep hardware FIFO is far more
 * likely to overrun under a burst.
 *
 * The risk in narrowing them is that a filter which drops a frame the
 * application needs fails **silently** — the frame simply never arrives, and
 * the consumer sees a timeout it will blame on the sender or the wiring. So the
 * ID set is built here as plain data, independent of any bxCAN register layout,
 * and tested on a host against the IDs the application actually dispatches on.
 *
 * Pure module: no HAL, no registers, no globals.
 */
#ifndef CAN_FILTER_PLAN_H
#define CAN_FILTER_PLAN_H

#include <stdbool.h>
#include <stdint.h>

/** Roles are a bitmask: one controller may host more than one. */
#define BFMC_ROLE_GATEWAY (1U << 0)
#define BFMC_ROLE_MOTION  (1U << 1)

/** Union of both roles needs 15 entries; 16 leaves a little headroom. */
#define CAN_FILTER_PLAN_MAX_IDS 16U

typedef struct
{
    uint16_t ids[CAN_FILTER_PLAN_MAX_IDS];
    uint8_t  count;
} can_filter_plan_t;

/**
 * @brief Build the receive ID set for one or more roles.
 * @param role_mask BFMC_ROLE_GATEWAY, BFMC_ROLE_MOTION, or both ORed together
 *
 * Duplicates are removed, so a controller hosting both roles gets the union
 * once rather than the same ID twice. The result is sorted ascending, which
 * makes a debugger dump readable and the hardware programming deterministic.
 */
void CanFilterPlan_Build(can_filter_plan_t *plan, uint32_t role_mask);

/** @brief Would this plan accept that standard ID? */
bool CanFilterPlan_Accepts(const can_filter_plan_t *plan, uint16_t std_id);

/**
 * @brief Filter banks needed to express the plan in 16-bit list mode.
 *
 * bxCAN packs four 11-bit identifiers into one bank at that scale, so this is
 * the count rounded up. Used to check the plan fits the banks allocated to a
 * controller before any of it is programmed.
 */
uint8_t CanFilterPlan_BanksRequired(const can_filter_plan_t *plan);

#endif /* CAN_FILTER_PLAN_H */
