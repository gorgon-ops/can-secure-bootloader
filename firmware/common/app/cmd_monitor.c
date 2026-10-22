#include "cmd_monitor.h"
#include "app_time.h"

#include <string.h>

void CmdMonitor_Init(cmd_monitor_t *mon, uint32_t timeout_ms, uint32_t stale_ms)
{
    memset(mon, 0, sizeof(*mon));
    mon->timeout_ms = timeout_ms;
    mon->stale_ms   = stale_ms;
    mon->state      = CMD_MONITOR_INIT;
}

void CmdMonitor_OnFrame(cmd_monitor_t *mon, uint8_t counter, uint32_t now_ms)
{
    bool advanced;

    if (!mon->seeded)
    {
        /* The first frame can only be a baseline, so it counts as fresh. */
        mon->seeded       = true;
        mon->last_counter = counter;
        mon->last_rx_ms   = now_ms;
        mon->last_fresh_ms = now_ms;
        return;
    }

    /* Any change counts as progress, not exactly +1: a strict check would trip
     * fail-safe on one frame lost to arbitration. Real loss is the timeout's job. */
    advanced = (counter != mon->last_counter);

    mon->last_rx_ms   = now_ms;
    mon->last_counter = counter;

    if (advanced)
    {
        mon->last_fresh_ms = now_ms;
    }
}

cmd_monitor_state_t CmdMonitor_Poll(cmd_monitor_t *mon, uint32_t now_ms)
{
    if (!mon->seeded)
    {
        /* Not TIMEOUT: startup is not a fault, and INIT is unusable anyway. */
        mon->state = CMD_MONITOR_INIT;
    }
    else if (AppTime_Expired(mon->last_rx_ms, now_ms, mon->timeout_ms))
    {
        mon->state = CMD_MONITOR_TIMEOUT;
    }
    else if (AppTime_Expired(mon->last_fresh_ms, now_ms, mon->stale_ms))
    {
        /* After TIMEOUT, so a dead link reports the more specific cause. */
        mon->state = CMD_MONITOR_STALE;
    }
    else
    {
        mon->state = CMD_MONITOR_OK;
    }

    return mon->state;
}

bool CmdMonitor_CommandIsUsable(const cmd_monitor_t *mon)
{
    return mon->state == CMD_MONITOR_OK;
}

const char *CmdMonitor_StateName(cmd_monitor_state_t state)
{
    switch (state)
    {
        case CMD_MONITOR_INIT:    return "INIT";
        case CMD_MONITOR_OK:      return "OK";
        case CMD_MONITOR_TIMEOUT: return "TIMEOUT";
        case CMD_MONITOR_STALE:   return "STALE";
        default:                  return "?";
    }
}
