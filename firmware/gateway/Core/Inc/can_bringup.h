/**
 * @file    can_bringup.h
 * @brief   Raw bxCAN bring-up harness.
 *
 * Answers one question: is the bus alive at all? Transmits a fixed payload on a
 * fixed ID and counts what comes back, with no protocol, no database and no
 * application logic in the way. When this does not work, nothing above it can.
 *
 * Bus topology comes from CAN_BRINGUP_MODE in can_config.h. Diagnostic counters
 * live in can_if.h as g_can1_stats / g_can2_stats.
 *
 * For the vehicle application instead, build with BFMC_RUN_MODE set to
 * BFMC_RUN_APPLICATION; see bfmc_app.h.
 */
#ifndef CAN_BRINGUP_H
#define CAN_BRINGUP_H

#include "can_config.h"

/* Test frame IDs. PORT_1 holds the lower ID and must win arbitration whenever
   the two contend, which is visible on a bus capture in dual mode. */
#define CAN_BRINGUP_ID_NODE1 0x100U
#define CAN_BRINGUP_ID_NODE2 0x200U

/** @brief Bring the bus up and reset counters. */
void CanBringup_Init(void);

/** @brief Non-blocking periodic transmit. Call from the main loop. */
void CanBringup_Task(void);

#endif /* CAN_BRINGUP_H */
