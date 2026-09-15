/**
 * @file    task_watchdog.h
 * @brief   Deadline supervision for periodic tasks.
 *
 * A watchdog refreshed straight from the main loop proves only that the loop is
 * spinning. It says nothing about whether the loop is doing any *work*: if the
 * control task stops running but the loop keeps turning, a plain watchdog keeps
 * being fed and the vehicle carries on with stale actuator values. That is the
 * failure worth catching, and it is the one the naive version misses.
 *
 * So the hardware watchdog is refreshed conditionally. Each supervised task
 * checks in when it runs; the refresh only happens when every registered task
 * has checked in within its own deadline. If one stalls, refreshing stops, the
 * hardware watchdog expires, and the board resets into a safe state.
 *
 * Pure module: no globals, no I/O, no HAL. Time comes in as a parameter, so the
 * deadline logic is testable on a host without waiting in real time.
 */
#ifndef TASK_WATCHDOG_H
#define TASK_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#define TASK_WATCHDOG_MAX_TASKS 8U

typedef struct
{
    bool     registered;
    uint32_t deadline_ms;     /*!< longest tolerable gap between check-ins   */
    uint32_t last_checkin_ms;
    bool     seen;            /*!< has checked in at least once              */
} task_watchdog_slot_t;

typedef struct
{
    task_watchdog_slot_t tasks[TASK_WATCHDOG_MAX_TASKS];
    uint32_t start_ms;
    uint32_t grace_ms;        /*!< startup window before deadlines apply     */
    uint32_t overdue_mask;    /*!< bit n set if task n missed its deadline   */
} task_watchdog_t;

/**
 * @brief Reset the supervisor.
 * @param grace_ms window after startup during which refresh is always allowed,
 *        since tasks have not had a chance to check in yet. Without it the
 *        board would reset itself immediately on every boot.
 */
void TaskWatchdog_Init(task_watchdog_t *wdt, uint32_t now_ms, uint32_t grace_ms);

/**
 * @brief Supervise a task.
 * @param deadline_ms should be a generous multiple of the task period. Too
 *        tight and normal jitter resets a healthy vehicle, which is a worse
 *        failure than the one being guarded against.
 */
void TaskWatchdog_Register(task_watchdog_t *wdt, uint8_t task_id, uint32_t deadline_ms);

/** @brief Record that a task just ran. */
void TaskWatchdog_Checkin(task_watchdog_t *wdt, uint8_t task_id, uint32_t now_ms);

/**
 * @brief May the hardware watchdog be refreshed?
 * @return false as soon as any registered task is past its deadline.
 *
 * Once this returns false it keeps returning false: a task that has already
 * missed its deadline cannot argue its way back by checking in late, because by
 * then the system has already been running on stale data. Recovery is the
 * reset, not a resumption.
 */
bool TaskWatchdog_MayRefresh(task_watchdog_t *wdt, uint32_t now_ms);

/** @brief Bitmask of tasks that have missed a deadline, bit n for task n. */
uint32_t TaskWatchdog_OverdueMask(const task_watchdog_t *wdt);

#endif /* TASK_WATCHDOG_H */
