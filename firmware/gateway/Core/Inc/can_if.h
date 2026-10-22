/* The only owner of bxCAN and the HAL RX/error callbacks: the HAL dispatches
 * each event to exactly one symbol, so bring-up and the application both sit on
 * top of this rather than defining competing callbacks. */
#ifndef CAN_IF_H
#define CAN_IF_H

#include "can_filter_plan.h"
#include "main.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    CAN_IF_PORT_1 = 0, /* PA11/PA12 */
    CAN_IF_PORT_2 = 1, /* PB12/PB13 */
    CAN_IF_PORT_COUNT
} can_if_port_t;

typedef struct
{
    uint32_t tx_ok;
    uint32_t tx_err;
    uint32_t rx_count;
    uint32_t err_count;
    uint32_t last_rx_id;
    uint8_t  last_rx_data[8];
    uint8_t  last_rx_dlc;
    uint32_t last_error;
    uint32_t last_esr;

    /* CAN_ESR decoded, refreshed by CanIf_Poll(). */
    uint8_t last_lec;      /* ESR[6:4], 3 = no ACK */
    uint8_t tec;
    uint8_t rec;
    uint8_t bus_off;

    uint32_t rx_overrun;   /* hardware FIFO overflowed: frames were lost */
    uint32_t recoveries;
} can_if_stats_t;

typedef enum
{
    CAN_IF_HEALTH_OK = 0,
    CAN_IF_HEALTH_NO_PEER,     /* transmitting fine, nothing is acknowledging */
    CAN_IF_HEALTH_WARNING,     /* error counters past 96 */
    CAN_IF_HEALTH_PASSIVE,     /* past 128, still trying */
    CAN_IF_HEALTH_BUS_OFF
} can_if_health_t;

#define CAN_IF_LEC_NONE      0U
#define CAN_IF_LEC_STUFF     1U
#define CAN_IF_LEC_FORM      2U
#define CAN_IF_LEC_ACK       3U /* nobody acknowledged: no other live node */
#define CAN_IF_LEC_BIT_REC   4U
#define CAN_IF_LEC_BIT_DOM   5U
#define CAN_IF_LEC_CRC       6U

extern volatile can_if_stats_t g_can1_stats;
extern volatile can_if_stats_t g_can2_stats;

/* Runs in the RX ISR: copy the frame out and do the work in the periodic task. */
typedef void (*can_if_rx_fn)(can_if_port_t port,
                             uint32_t id,
                             const uint8_t *data,
                             uint8_t dlc);

/* A NULL plan accepts everything (bring-up). Halts via Error_Handler() on any
 * HAL failure or a plan that does not fit its bank budget. */
void CanIf_Init(can_if_rx_fn on_frame,
                const can_filter_plan_t *plan1,
                const can_filter_plan_t *plan2);

bool CanIf_Send(can_if_port_t port, uint32_t id, const uint8_t *data, uint8_t dlc);
bool CanIf_PortEnabled(can_if_port_t port);
volatile can_if_stats_t *CanIf_Stats(can_if_port_t port);

/* Call every loop. The error interrupt only fires on a change, and with
 * AutoBusOff disabled bus-off recovery has to be driven from here. */
void CanIf_Poll(uint32_t now_ms);

can_if_health_t CanIf_Health(can_if_port_t port);
can_if_health_t CanIf_WorstHealth(void);

/* For the failure path: a node that has stopped transmitting lets its peers
 * notice and fail safe on their own schedule. Safe before the controllers have
 * been started. */
void CanIf_Silence(void);

const char *CanIf_HealthName(can_if_health_t health);

#endif
