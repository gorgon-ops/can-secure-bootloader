#include "can_bringup.h"
#include "can_if.h"
#include "fault_record.h"

#define CAN_BRINGUP_TX_PERIOD_MS 500U

static uint32_t s_last_tx_tick;
static uint8_t  s_tx_seq;
static uint32_t s_init_ms;
static bool     s_noted_healthy;

#define CAN_BRINGUP_HEALTHY_AFTER_MS 1000U

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
    data[7] = s_tx_seq;  /* frozen means a stuck sender */

    (void)CanIf_Send(port, id, data, sizeof(data));
}

void CanBringup_Init(void)
{
    /* NULL plans accept everything: bring-up shows whatever is on the wire. */
    CanIf_Init(NULL, NULL, NULL);
    s_last_tx_tick = HAL_GetTick();
    s_init_ms = s_last_tx_tick;
    s_noted_healthy = false;
    s_tx_seq = 0U;
}

void CanBringup_Task(void)
{
    uint32_t now = HAL_GetTick();

    /* Init succeeded, so any earlier fault was transient. */
    if (!s_noted_healthy &&
        ((uint32_t)(now - s_init_ms) >= CAN_BRINGUP_HEALTHY_AFTER_MS))
    {
        FaultRecord_NoteHealthy();
        s_noted_healthy = true;
    }

    if ((uint32_t)(now - s_last_tx_tick) < CAN_BRINGUP_TX_PERIOD_MS)
    {
        return;
    }
    s_last_tx_tick = now;
    s_tx_seq++;

    CanBringup_Send(CAN_IF_PORT_1, CAN_BRINGUP_ID_NODE1);

    if (CanIf_PortEnabled(CAN_IF_PORT_2))
    {
        /* Queued right after PORT_1 so the two contend. The lower ID must win. */
        CanBringup_Send(CAN_IF_PORT_2, CAN_BRINGUP_ID_NODE2);
    }
}
