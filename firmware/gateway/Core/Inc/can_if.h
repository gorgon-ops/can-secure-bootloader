/**
 * @file    can_if.h
 * @brief   Thin CAN transport for the NUCLEO-F446RE.
 *
 * Owns everything that touches bxCAN: filter banks, controller startup,
 * interrupt configuration, and the HAL receive and error callbacks. Consumers
 * get a send function and a receive callback, and never see a HAL type.
 *
 * The HAL dispatches RX and error interrupts to exactly one callback symbol per
 * event, so that dispatch has to live in a single place. This is it. The raw
 * bring-up harness and the vehicle application both sit on top rather than
 * defining competing callbacks.
 *
 * Bus topology follows CAN_BRINGUP_MODE (see can_bringup.h):
 *   LOOPBACK  only PORT_1 exists, in silent-loopback; frames sent on it come
 *             back to the same port. Needs no transceiver and no wiring.
 *   DUAL      PORT_1 is CAN1 and PORT_2 is CAN2, both on one physical bus.
 */
#ifndef CAN_IF_H
#define CAN_IF_H

#include "main.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    CAN_IF_PORT_1 = 0, /*!< CAN1, PA11/PA12 */
    CAN_IF_PORT_2 = 1, /*!< CAN2, PB12/PB13 */
    CAN_IF_PORT_COUNT
} can_if_port_t;

/** Diagnostic counters. Watch these as Live Expressions in the debugger. */
typedef struct
{
    uint32_t tx_ok;        /*!< frames accepted by a mailbox                 */
    uint32_t tx_err;       /*!< rejected: every mailbox busy                 */
    uint32_t rx_count;     /*!< frames pulled out of FIFO0                   */
    uint32_t err_count;    /*!< error-interrupt callbacks                    */
    uint32_t last_rx_id;
    uint8_t  last_rx_data[8];
    uint8_t  last_rx_dlc;
    uint32_t last_error;   /*!< most recent hcan->ErrorCode                  */
    uint32_t last_esr;     /*!< raw CAN_ESR                                  */

    /* CAN_ESR decoded, so the debugger shows the diagnosis rather than a word
       of hex to decode by hand. */
    uint8_t last_lec;      /*!< last error code, ESR[6:4]. 3 = no ACK        */
    uint8_t last_tec;      /*!< transmit error counter, ESR[23:16]           */
    uint8_t last_rec;      /*!< receive error counter, ESR[31:24]            */
    uint8_t bus_off;       /*!< 1 once the controller has gone bus-off       */
} can_if_stats_t;

/** Values of can_if_stats_t::last_lec, from the bxCAN last-error-code field. */
#define CAN_IF_LEC_NONE      0U
#define CAN_IF_LEC_STUFF     1U
#define CAN_IF_LEC_FORM      2U
#define CAN_IF_LEC_ACK       3U /*!< nobody acknowledged: no other live node */
#define CAN_IF_LEC_BIT_REC   4U
#define CAN_IF_LEC_BIT_DOM   5U
#define CAN_IF_LEC_CRC       6U

extern volatile can_if_stats_t g_can1_stats;
extern volatile can_if_stats_t g_can2_stats;

/**
 * @brief Receive callback, invoked from interrupt context.
 * @note  Keep it short: it runs in the CAN RX ISR. The application copies the
 *        frame into node state and does the real work in its periodic task.
 */
typedef void (*can_if_rx_fn)(can_if_port_t port,
                             uint32_t id,
                             const uint8_t *data,
                             uint8_t dlc);

/**
 * @brief Configure filters, start the controllers, enable interrupts.
 * @param on_frame receive callback, or NULL to drop received frames
 *
 * Halts via Error_Handler() on any HAL failure, so a silent misconfiguration
 * cannot slip through into a running vehicle.
 */
void CanIf_Init(can_if_rx_fn on_frame);

/**
 * @brief Queue a standard-ID data frame.
 * @return true if a mailbox accepted it.
 *
 * False means all three mailboxes are full, which on a real bus means nothing
 * is ACKing. With AutoRetransmission disabled the frame is dropped rather than
 * retried, so the caller decides whether to care.
 */
bool CanIf_Send(can_if_port_t port, uint32_t id, const uint8_t *data, uint8_t dlc);

/** @brief True if this port is active in the current bus topology. */
bool CanIf_PortEnabled(can_if_port_t port);

/** @brief Diagnostic counters for one port. */
volatile can_if_stats_t *CanIf_Stats(can_if_port_t port);

#endif /* CAN_IF_H */
