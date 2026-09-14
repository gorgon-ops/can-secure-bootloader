/**
 * Supervision of the MOTION_CMD stream, as consumed by the Motion ECU.
 *
 * Two distinct failures have to be caught, and only one of them is a timeout:
 *
 *   TIMEOUT  No frame arrived for APP_MOTION_CMD_TIMEOUT_MS. The Gateway is
 *            gone, the bus is broken, or the node is off.
 *
 *   STALE    Frames keep arriving on time, but CmdCounter has stopped
 *            advancing. The sender is alive enough to transmit while its
 *            control task is wedged. A heartbeat would look perfectly healthy
 *            here, which is exactly why MOTION_CMD carries its own counter.
 *
 * The module is pure: no globals, no I/O, no HAL. Time comes in as a parameter.
 */
#ifndef CMD_MONITOR_H
#define CMD_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    CMD_MONITOR_INIT = 0,  /*!< nothing received yet since startup      */
    CMD_MONITOR_OK,        /*!< fresh commands arriving on schedule     */
    CMD_MONITOR_TIMEOUT,   /*!< no frame within the timeout budget      */
    CMD_MONITOR_STALE      /*!< frames arriving, counter not advancing  */
} cmd_monitor_state_t;

typedef struct
{
    uint32_t timeout_ms;      /*!< budget for "no frame at all"            */
    uint32_t stale_ms;        /*!< budget for "no counter change"          */

    uint32_t last_rx_ms;      /*!< any frame, fresh or not                 */
    uint32_t last_fresh_ms;   /*!< frame whose counter had advanced        */
    uint8_t  last_counter;
    bool     seeded;          /*!< at least one frame seen                 */

    cmd_monitor_state_t state;
} cmd_monitor_t;

/**
 * @brief Reset the monitor and set its budgets.
 * @param timeout_ms  no-frame budget, e.g. APP_MOTION_CMD_TIMEOUT_MS
 * @param stale_ms    frozen-counter budget, e.g. APP_MOTION_CMD_STALE_MS
 */
void CmdMonitor_Init(cmd_monitor_t *mon, uint32_t timeout_ms, uint32_t stale_ms);

/**
 * @brief Feed a received MOTION_CMD.
 * @param counter  the CmdCounter signal from the frame
 * @param now_ms   receive timestamp
 *
 * A counter differing from the previous one marks the frame fresh. Equality
 * means the payload is a repeat: the frame still counts as traffic (so the
 * timeout does not fire) but not as progress (so the stale budget keeps
 * running).
 */
void CmdMonitor_OnFrame(cmd_monitor_t *mon, uint8_t counter, uint32_t now_ms);

/**
 * @brief Re-evaluate against the clock. Call every control cycle.
 * @return the current state, also stored in the monitor.
 *
 * This must be polled rather than driven purely by arriving frames: the
 * interesting failures are all cases where nothing arrives, and a callback that
 * never fires cannot raise an alarm.
 */
cmd_monitor_state_t CmdMonitor_Poll(cmd_monitor_t *mon, uint32_t now_ms);

/** @brief True only in CMD_MONITOR_OK: the last command may drive actuators. */
bool CmdMonitor_CommandIsUsable(const cmd_monitor_t *mon);

/** @brief Human-readable state, for diagnostics and test output. */
const char *CmdMonitor_StateName(cmd_monitor_state_t state);

#endif /* CMD_MONITOR_H */
