/* A watchdog fed from the main loop only proves the loop is spinning. Here the
 * hardware watchdog is refreshed only when every registered task has checked in
 * within its own deadline. */
#ifndef TASK_WATCHDOG_H
#define TASK_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#define TASK_WATCHDOG_MAX_TASKS 8U

typedef struct
{
    bool     registered;
    uint32_t deadline_ms;
    uint32_t last_checkin_ms;
    bool     seen;
} task_watchdog_slot_t;

typedef struct
{
    task_watchdog_slot_t tasks[TASK_WATCHDOG_MAX_TASKS];
    uint32_t start_ms;
    uint32_t grace_ms;
    uint32_t overdue_mask;
} task_watchdog_t;

/* grace_ms: refresh is always allowed during startup, before tasks have had a
 * chance to check in. */
void TaskWatchdog_Init(task_watchdog_t *wdt, uint32_t now_ms, uint32_t grace_ms);

/* Keep deadline_ms a generous multiple of the period: jitter resetting a
 * healthy vehicle is worse than the failure being guarded against. */
void TaskWatchdog_Register(task_watchdog_t *wdt, uint8_t task_id, uint32_t deadline_ms);

void TaskWatchdog_Checkin(task_watchdog_t *wdt, uint8_t task_id, uint32_t now_ms);

/* Latches false. A task that missed its deadline cannot recover by checking in
 * late - the recovery is the reset. */
bool TaskWatchdog_MayRefresh(task_watchdog_t *wdt, uint32_t now_ms);

uint32_t TaskWatchdog_OverdueMask(const task_watchdog_t *wdt);

#endif
