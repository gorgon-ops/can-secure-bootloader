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
        /* First frame establishes the baseline. It cannot be judged fresh or
           stale against a counter we have never seen, so treat it as fresh and
           start both budgets from here. */
        mon->seeded       = true;
        mon->last_counter = counter;
        mon->last_rx_ms   = now_ms;
        mon->last_fresh_ms = now_ms;
        return;
    }

    /* Any difference counts as progress, rather than requiring exactly +1.
       A strict increment check would trip fail-safe on a single frame lost to
       arbitration, which is normal on a shared bus; genuine loss is already
       covered by the timeout. The counter is uint8 so its wrap is free. */
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
        /* Nothing has ever arrived. Deliberately not reported as TIMEOUT:
           startup is not a fault, and INIT is already unusable, so the vehicle
           stays safe either way. */
        mon->state = CMD_MONITOR_INIT;
    }
    else if (AppTime_Expired(mon->last_rx_ms, now_ms, mon->timeout_ms))
    {
        mon->state = CMD_MONITOR_TIMEOUT;
    }
    else if (AppTime_Expired(mon->last_fresh_ms, now_ms, mon->stale_ms))
    {
        /* Checked after TIMEOUT so a total loss reports the more specific
           cause: a dead link is also, technically, a frozen counter. */
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
