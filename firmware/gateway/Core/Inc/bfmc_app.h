/**
 * @file    bfmc_app.h
 * @brief   The vehicle application: two node roles on one NUCLEO-F446RE.
 *
 * Runs the Gateway and Motion roles as cooperative tasks, exchanging real
 * DBC-defined frames through the generated protocol layer and the supervision
 * logic in firmware/common/app/.
 *
 *   Gateway  publishes MOTION_CMD at 50 Hz with an advancing CmdCounter, and
 *            HB_GATEWAY at 10 Hz. Supervises the Motion heartbeat.
 *
 *   Motion   consumes MOTION_CMD through cmd_monitor, drives the actuator
 *            outputs through motion_safety, publishes MOTION_STATUS at 50 Hz
 *            and HB_MOTION at 10 Hz. Supervises the Gateway heartbeat.
 *
 * Which controller each role uses follows the bus topology:
 *
 *   LOOPBACK  both roles share CAN1. Frames the Gateway sends come straight
 *             back to the same controller, so the Motion role receives them
 *             and the whole stack -- pack, transmit, receive, unpack,
 *             supervise, fail-safe -- runs with nothing attached to the board.
 *
 *   DUAL      Gateway on CAN1, Motion on CAN2, contending on one real bus.
 *
 * The supervision logic itself is the host-tested code in
 * firmware/common/app/; this file is only the wiring that gives it a clock,
 * a transport and somewhere to put its output.
 */
#ifndef BFMC_APP_H
#define BFMC_APP_H

#include "cmd_monitor.h"
#include "heartbeat.h"
#include "motion_safety.h"
#include "node_state.h"

#include <stdint.h>

/** Node addresses, matching the NodeAddress attribute in dbc/bfmc.dbc. */
#define BFMC_NODE_GATEWAY 1U
#define BFMC_NODE_MOTION  2U
#define BFMC_NODE_SENSOR  3U

/** Observable application state. Watch as Live Expressions in the debugger. */
typedef struct
{
    /* Gateway role */
    uint8_t      gw_cmd_counter;   /*!< CmdCounter last transmitted          */
    int16_t      gw_target_speed;  /*!< speed being commanded, mm/s          */
    int16_t      gw_target_steer;  /*!< steering being commanded, 0.1 deg    */
    node_state_t gw_state;         /*!< RUNNING or DEGRADED, from peer health */

    /* Motion role */
    cmd_monitor_state_t mo_cmd_state; /*!< OK / TIMEOUT / STALE / INIT       */
    int16_t      mo_out_speed;     /*!< value that would reach the motor     */
    int16_t      mo_out_steer;     /*!< value that would reach the servo     */
    uint8_t      mo_failsafe;      /*!< 1 while the fail-safe ramp is active */
    node_state_t mo_state;

    uint32_t     cmd_rx_count;     /*!< MOTION_CMD frames accepted           */
    uint32_t     decode_errors;    /*!< frames that failed to unpack         */
} bfmc_app_state_t;

extern volatile bfmc_app_state_t g_bfmc_app;

/** @brief Bring up the bus and reset both roles. */
void BfmcApp_Init(void);

/** @brief Non-blocking scheduler tick. Call from the main loop. */
void BfmcApp_Task(void);

#endif /* BFMC_APP_H */
