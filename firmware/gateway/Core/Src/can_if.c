#include "can_if.h"
#include "can_config.h"   /* CAN_BRINGUP_MODE selects the bus topology */

#include <string.h>

extern CAN_HandleTypeDef hcan1;
extern CAN_HandleTypeDef hcan2;

volatile can_if_stats_t g_can1_stats;
volatile can_if_stats_t g_can2_stats;

static can_if_rx_fn s_on_frame;

/**
 * bxCAN on the F446 has 28 filter banks shared between the two controllers.
 * Banks [0, SLAVE_START) belong to CAN1 and [SLAVE_START, 28) to CAN2. The
 * split must be programmed explicitly or CAN2 matches nothing at all -- it is
 * the classic dual-CAN trap on this family, and it fails silently.
 */
#define CAN_IF_SLAVE_START_BANK 14U

#define CAN_IF_IT_FLAGS (CAN_IT_RX_FIFO0_MSG_PENDING | \
                         CAN_IT_ERROR_WARNING        | \
                         CAN_IT_ERROR_PASSIVE        | \
                         CAN_IT_BUSOFF               | \
                         CAN_IT_LAST_ERROR_CODE      | \
                         CAN_IT_ERROR)

static CAN_HandleTypeDef *CanIf_Handle(can_if_port_t port)
{
    return (port == CAN_IF_PORT_1) ? &hcan1 : &hcan2;
}

volatile can_if_stats_t *CanIf_Stats(can_if_port_t port)
{
    return (port == CAN_IF_PORT_1) ? &g_can1_stats : &g_can2_stats;
}

bool CanIf_PortEnabled(can_if_port_t port)
{
#if CAN_BRINGUP_MODE == CAN_BRINGUP_DUAL
    return (port == CAN_IF_PORT_1) || (port == CAN_IF_PORT_2);
#else
    /* Loopback exercises a single controller with no bus attached. */
    return port == CAN_IF_PORT_1;
#endif
}

/**
 * Accept-everything filter. Correct for bring-up, where the point is to see
 * whatever is on the wire; the vehicle build should narrow each node to the
 * IDs it actually consumes so the hardware discards the rest.
 */
static void CanIf_ConfigFilter(CAN_HandleTypeDef *hcan, uint32_t bank)
{
    CAN_FilterTypeDef filter;

    filter.FilterActivation     = CAN_FILTER_ENABLE;
    filter.FilterBank           = bank;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterMode           = CAN_FILTERMODE_IDMASK;
    filter.FilterScale          = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh         = 0x0000U;
    filter.FilterIdLow          = 0x0000U;
    filter.FilterMaskIdHigh     = 0x0000U;
    filter.FilterMaskIdLow      = 0x0000U;
    filter.SlaveStartFilterBank = CAN_IF_SLAVE_START_BANK;

    if (HAL_CAN_ConfigFilter(hcan, &filter) != HAL_OK)
    {
        Error_Handler();
    }
}

static void CanIf_StartPort(CAN_HandleTypeDef *hcan, uint32_t bank)
{
    CanIf_ConfigFilter(hcan, bank);

    if (HAL_CAN_Start(hcan) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_CAN_ActivateNotification(hcan, CAN_IF_IT_FLAGS) != HAL_OK)
    {
        Error_Handler();
    }
}

void CanIf_Init(can_if_rx_fn on_frame)
{
    s_on_frame = on_frame;

    memset((void *)&g_can1_stats, 0, sizeof(g_can1_stats));
    memset((void *)&g_can2_stats, 0, sizeof(g_can2_stats));

#if CAN_BRINGUP_MODE == CAN_BRINGUP_LOOPBACK
    /* Silent keeps the TX pin recessive, so the board is valid with nothing
       attached. MX_CAN1_Init() has already run; re-init only changes the mode
       and skips MspInit. */
    hcan1.Init.Mode = CAN_MODE_SILENT_LOOPBACK;
    if (HAL_CAN_Init(&hcan1) != HAL_OK)
    {
        Error_Handler();
    }
#endif

    CanIf_StartPort(&hcan1, 0U);

#if CAN_BRINGUP_MODE == CAN_BRINGUP_DUAL
    CanIf_StartPort(&hcan2, CAN_IF_SLAVE_START_BANK);
#endif
}

bool CanIf_Send(can_if_port_t port, uint32_t id, const uint8_t *data, uint8_t dlc)
{
    CAN_TxHeaderTypeDef header;
    volatile can_if_stats_t *stats = CanIf_Stats(port);
    uint32_t mailbox;

    if (!CanIf_PortEnabled(port))
    {
        return false;
    }

    header.StdId              = id;
    header.ExtId              = 0U;
    header.IDE                = CAN_ID_STD;
    header.RTR                = CAN_RTR_DATA;
    header.DLC                = dlc;
    header.TransmitGlobalTime = DISABLE;

    /* HAL takes a non-const pointer but does not modify the payload. */
    if (HAL_CAN_AddTxMessage(CanIf_Handle(port), &header,
                             (uint8_t *)data, &mailbox) == HAL_OK)
    {
        stats->tx_ok++;
        return true;
    }

    stats->tx_err++;
    return false;
}

/* ---------------------------------------------------------------------------
   HAL callbacks. Both controllers share these; dispatch on hcan->Instance.
   --------------------------------------------------------------------------- */

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CAN_RxHeaderTypeDef header;
    uint8_t data[8];
    can_if_port_t port;
    volatile can_if_stats_t *stats;

    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, data) != HAL_OK)
    {
        return;
    }

    port  = (hcan->Instance == CAN1) ? CAN_IF_PORT_1 : CAN_IF_PORT_2;
    stats = CanIf_Stats(port);

    stats->last_rx_id  = (header.IDE == CAN_ID_STD) ? header.StdId : header.ExtId;
    stats->last_rx_dlc = (uint8_t)header.DLC;
    memcpy((void *)stats->last_rx_data, data, sizeof(data));
    stats->rx_count++;

    if (s_on_frame != NULL)
    {
        s_on_frame(port, stats->last_rx_id, data, stats->last_rx_dlc);
    }
}

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
    can_if_port_t port = (hcan->Instance == CAN1) ? CAN_IF_PORT_1 : CAN_IF_PORT_2;
    volatile can_if_stats_t *stats = CanIf_Stats(port);

    stats->err_count++;
    stats->last_error = hcan->ErrorCode;

    /* ESR carries the transmit and receive error counters plus the last error
       code, so it says *why* the bus failed rather than only that it did.
       LEC 0b011 is an ACK error: what a single node with no partner reports. */
    stats->last_esr = hcan->Instance->ESR;
}
