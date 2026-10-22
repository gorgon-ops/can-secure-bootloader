/* Gateway and Motion roles as cooperative tasks on one board. In LOOPBACK both
 * share CAN1, so the whole stack runs with nothing attached; in DUAL they sit
 * on CAN1 and CAN2 of one real bus. */
#ifndef BFMC_APP_H
#define BFMC_APP_H

#include "can_if.h"
#include "fault_record.h"
#include "cmd_monitor.h"
#include "heartbeat.h"
#include "motion_safety.h"
#include "node_state.h"

#include <stdint.h>

/* Matching the NodeAddress attribute in dbc/bfmc.dbc. */
#define BFMC_NODE_GATEWAY 1U
#define BFMC_NODE_MOTION  2U
#define BFMC_NODE_SENSOR  3U

typedef struct
{
    uint8_t      gw_cmd_counter;
    int16_t      gw_target_speed;  /* mm/s */
    int16_t      gw_target_steer;  /* 0.1 deg */
    node_state_t gw_state;

    /* Motion keeps its own copy of the request: what it received is not the same
     * thing as what the Gateway believes it sent. */
    int16_t      mo_req_speed;     /* mm/s */
    int16_t      mo_req_steer;     /* 0.1 deg */
    cmd_monitor_state_t mo_cmd_state;
    int16_t      mo_out_speed;
    int16_t      mo_out_steer;
    uint8_t      mo_failsafe;
    node_state_t mo_state;

    /* Should all stay at zero on a healthy bus. */
    uint32_t     cmd_rx_count;
    uint32_t     decode_errors;
    uint32_t     cmd_overruns;     /* commands dropped: task too slow */
    uint32_t     hb_overruns;      /* heartbeats dropped: task too slow */

    can_if_health_t can_health;

    /* reset_was_watchdog == 1 means the previous run hung; prev_overdue_mask
     * names the task that stopped checking in. */
    uint8_t  reset_was_watchdog;
    uint32_t prev_overdue_mask;
    uint32_t overdue_mask;

    /* prev_fault_pc is a flash address: "info symbol <addr>" in gdb names it. */
    fault_kind_t prev_fault_kind;
    uint32_t     prev_fault_pc;
    uint32_t     prev_fault_consecutive;
} bfmc_app_state_t;

#define BFMC_WDT_TASK_CONTROL   0U  /* 20 ms */
#define BFMC_WDT_TASK_GATEWAY   1U  /* 20 ms */
#define BFMC_WDT_TASK_HEARTBEAT 2U  /* 100 ms */

extern volatile bfmc_app_state_t g_bfmc_app;

void BfmcApp_Init(void);
void BfmcApp_Task(void);

#endif
