/**
 * @file    can_bringup.c
 * @brief   Raw bxCAN bring-up harness. See can_bringup.h.
 */
#include "can_bringup.h"
#include "can_if.h"

#define CAN_BRINGUP_TX_PERIOD_MS 500U

static uint32_t s_last_tx_tick;
static uint8_t  s_tx_seq;

static void CanBringup_Send(can_if_port_t port, uint32_t id)
{
    uint8_t data[8];

    data[0] = 0xDEU;
    data[1] = 0xADU;
    data[2] = 0xBEU;
    data[3] = 0xEFU;
    data[4] = (uint8_t)(id >> 8);
    data[5] = (uint8_t)(id);
    data[6] = 0x00U;
    data[7] = s_tx_seq;  /* advances every cycle: frozen means a stuck sender */

    (void)CanIf_Send(port, id, data, sizeof(data));
}

void CanBringup_Init(void)
{
    /* No receive callback: the harness only needs the frame counters that
       can_if maintains, not the contents. */
    CanIf_Init(NULL);
    s_last_tx_tick = HAL_GetTick();
    s_tx_seq = 0U;
}

void CanBringup_Task(void)
{
    uint32_t now = HAL_GetTick();

    if ((uint32_t)(now - s_last_tx_tick) < CAN_BRINGUP_TX_PERIOD_MS)
    {
        return;
    }
    s_last_tx_tick = now;
    s_tx_seq++;

    CanBringup_Send(CAN_IF_PORT_1, CAN_BRINGUP_ID_NODE1);

    if (CanIf_PortEnabled(CAN_IF_PORT_2))
    {
        /* Queued immediately after PORT_1 so the two controllers contend for
           the bus. The lower ID must win. */
        CanBringup_Send(CAN_IF_PORT_2, CAN_BRINGUP_ID_NODE2);
    }
}
