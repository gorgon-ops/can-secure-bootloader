#ifndef CMD_MONITOR_H
#define CMD_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    CMD_MONITOR_INIT = 0,  /* nothing received yet */
    CMD_MONITOR_OK,        /* fresh commands on schedule */
    CMD_MONITOR_TIMEOUT,   /* no frame within the budget */
    CMD_MONITOR_STALE      /* frames arriving, counter frozen: sender's control task is wedged */
} cmd_monitor_state_t;

typedef struct
{
    uint32_t timeout_ms;
    uint32_t stale_ms;

    uint32_t last_rx_ms;      /* any frame, fresh or not */
    uint32_t last_fresh_ms;   /* frame whose counter had advanced */
    uint8_t  last_counter;
    bool     seeded;

    cmd_monitor_state_t state;
} cmd_monitor_t;

void CmdMonitor_Init(cmd_monitor_t *mon, uint32_t timeout_ms, uint32_t stale_ms);

/* A repeated counter still counts as traffic (no timeout) but not as progress
 * (the stale budget keeps running). */
void CmdMonitor_OnFrame(cmd_monitor_t *mon, uint8_t counter, uint32_t now_ms);

/* Polled rather than frame-driven: the failures that matter are the ones where
 * nothing arrives. */
cmd_monitor_state_t CmdMonitor_Poll(cmd_monitor_t *mon, uint32_t now_ms);

bool CmdMonitor_CommandIsUsable(const cmd_monitor_t *mon);
const char *CmdMonitor_StateName(cmd_monitor_state_t state);

#endif
