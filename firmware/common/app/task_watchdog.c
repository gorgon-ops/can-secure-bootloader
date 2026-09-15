#include "task_watchdog.h"
#include "app_time.h"

#include <string.h>

void TaskWatchdog_Init(task_watchdog_t *wdt, uint32_t now_ms, uint32_t grace_ms)
{
    memset(wdt, 0, sizeof(*wdt));
    wdt->start_ms = now_ms;
    wdt->grace_ms = grace_ms;
}

void TaskWatchdog_Register(task_watchdog_t *wdt, uint8_t task_id, uint32_t deadline_ms)
{
    if (task_id >= TASK_WATCHDOG_MAX_TASKS)
    {
        return;
    }

    wdt->tasks[task_id].registered  = true;
    wdt->tasks[task_id].deadline_ms = deadline_ms;
    wdt->tasks[task_id].seen        = false;
}

void TaskWatchdog_Checkin(task_watchdog_t *wdt, uint8_t task_id, uint32_t now_ms)
{
    if (task_id >= TASK_WATCHDOG_MAX_TASKS)
    {
        return;
    }
    if (!wdt->tasks[task_id].registered)
    {
        return;
    }

    wdt->tasks[task_id].last_checkin_ms = now_ms;
    wdt->tasks[task_id].seen            = true;
}

bool TaskWatchdog_MayRefresh(task_watchdog_t *wdt, uint32_t now_ms)
{
    bool within_grace;
    uint8_t i;

    /* Latched: once a deadline has been missed the answer stays no. A task that
       checks in late has still left the system running on stale data for longer
       than the design allows, and the correct recovery is the reset. */
    if (wdt->overdue_mask != 0U)
    {
        return false;
    }

    within_grace = !AppTime_Expired(wdt->start_ms, now_ms, wdt->grace_ms);

    for (i = 0U; i < TASK_WATCHDOG_MAX_TASKS; i++)
    {
        const task_watchdog_slot_t *task = &wdt->tasks[i];

        if (!task->registered)
        {
            continue;
        }

        if (!task->seen)
        {
            /* Never run yet. Tolerated during startup, a fault afterwards:
               a task that never starts at all is exactly as dangerous as one
               that stops. */
            if (!within_grace)
            {
                wdt->overdue_mask |= (1UL << i);
            }
            continue;
        }

        if (AppTime_Expired(task->last_checkin_ms, now_ms, task->deadline_ms))
        {
            wdt->overdue_mask |= (1UL << i);
        }
    }

    return wdt->overdue_mask == 0U;
}

uint32_t TaskWatchdog_OverdueMask(const task_watchdog_t *wdt)
{
    return wdt->overdue_mask;
}
