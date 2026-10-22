/* A filter that drops a frame the application needs fails silently - the
 * consumer just sees a timeout. So the ID set is plain data, tested on the host
 * against the IDs the application actually dispatches on. */
#ifndef CAN_FILTER_PLAN_H
#define CAN_FILTER_PLAN_H

#include <stdbool.h>
#include <stdint.h>

#define BFMC_ROLE_GATEWAY (1U << 0)
#define BFMC_ROLE_MOTION  (1U << 1)

/* Union of both roles needs 15. */
#define CAN_FILTER_PLAN_MAX_IDS 16U

typedef struct
{
    uint16_t ids[CAN_FILTER_PLAN_MAX_IDS];
    uint8_t  count;
} can_filter_plan_t;

/* Deduplicated and sorted ascending. */
void CanFilterPlan_Build(can_filter_plan_t *plan, uint32_t role_mask);

bool CanFilterPlan_Accepts(const can_filter_plan_t *plan, uint16_t std_id);

/* bxCAN fits four 11-bit IDs per bank in 16-bit list mode. */
uint8_t CanFilterPlan_BanksRequired(const can_filter_plan_t *plan);

#endif
