/**
  ******************************************************************************
  * @file    can_bringup.h
  * @brief   Dual-bxCAN bring-up harness for the NUCLEO-F446RE.
  *
  * The F446 has two independent CAN controllers, so a single Nucleo can act as
  * two nodes on one physical bus. This module drives both and records enough
  * state to diagnose a bus that is not working.
  *
  * Two modes, selected at compile time:
  *
  *   CAN_BRINGUP_LOOPBACK  CAN1 in silent-loopback. Needs no transceiver, no
  *                         wiring and no second node. Proves clock config,
  *                         filter banks, mailboxes and the RX interrupt path.
  *
  *   CAN_BRINGUP_DUAL      CAN1 and CAN2 on a real bus, each behind its own
  *                         transceiver. Real arbitration, real ACK, real error
  *                         frames. Requires two transceivers and termination.
  *
  * Define CAN_BRINGUP_MODE in the project symbols to override the default.
  ******************************************************************************
  */
#ifndef CAN_BRINGUP_H
#define CAN_BRINGUP_H

#include "main.h"
#include <stdint.h>

#define CAN_BRINGUP_LOOPBACK 0
#define CAN_BRINGUP_DUAL     1

#ifndef CAN_BRINGUP_MODE
#define CAN_BRINGUP_MODE CAN_BRINGUP_LOOPBACK
#endif

/* Test frame IDs. CAN1 wins arbitration against CAN2 by design, so a dual-mode
   capture shows 0x100 ahead of 0x200 whenever they contend. */
#define CAN_BRINGUP_ID_NODE1 0x100U
#define CAN_BRINGUP_ID_NODE2 0x200U

/* bxCAN on the F446 has 28 filter banks shared between the two controllers.
   Banks [0, SLAVE_START) belong to CAN1, [SLAVE_START, 28) to CAN2. This split
   must be programmed or CAN2 receives nothing. */
#define CAN_BRINGUP_SLAVE_START_BANK 14U

/** Per-controller counters. Watch these as Live Expressions in the debugger. */
typedef struct
{
  uint32_t tx_ok;        /*!< frames handed to a mailbox successfully        */
  uint32_t tx_err;       /*!< HAL_CAN_AddTxMessage rejections (no free mailbox) */
  uint32_t rx_count;     /*!< frames pulled out of FIFO0                     */
  uint32_t err_count;    /*!< error-interrupt callbacks                      */
  uint32_t last_rx_id;   /*!< ID of the most recent received frame           */
  uint8_t  last_rx_data[8];
  uint8_t  last_rx_dlc;
  uint32_t last_error;   /*!< most recent hcan->ErrorCode                    */
  uint32_t last_esr;     /*!< CAN_ESR snapshot: TEC, REC and last error code */
} can_bringup_stats_t;

extern volatile can_bringup_stats_t g_can1_stats;
extern volatile can_bringup_stats_t g_can2_stats;

/**
  * @brief Configure filters, start the controller(s) and enable interrupts.
  * @note  Call after MX_CAN1_Init() / MX_CAN2_Init(). Halts via Error_Handler()
  *        on any HAL failure, so a silent misconfiguration cannot slip through.
  */
void CanBringup_Init(void);

/**
  * @brief Non-blocking periodic transmit. Call from the main loop.
  */
void CanBringup_Task(void);

#endif /* CAN_BRINGUP_H */
