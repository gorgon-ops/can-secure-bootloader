#include "can_if.h"
#include "can_config.h"

#include <string.h>

extern CAN_HandleTypeDef hcan1;
extern CAN_HandleTypeDef hcan2;

volatile can_if_stats_t g_can1_stats;
volatile can_if_stats_t g_can2_stats;

static can_if_rx_fn s_on_frame;

/* Banks [0, SLAVE_START) are CAN1's and the rest CAN2's. The split must be
 * programmed explicitly or CAN2 silently matches nothing. */
#define CAN_IF_SLAVE_START_BANK 14U

#define CAN_IF_IT_FLAGS (CAN_IT_RX_FIFO0_MSG_PENDING | \
                         CAN_IT_RX_FIFO0_OVERRUN     | \
                         CAN_IT_ERROR_WARNING        | \
                         CAN_IT_ERROR_PASSIVE        | \
                         CAN_IT_BUSOFF               | \
                         CAN_IT_LAST_ERROR_CODE      | \
                         CAN_IT_ERROR)

#define CAN_IF_RECOVERY_INTERVAL_MS 500U

/* From the CAN specification. */
#define CAN_IF_EWARN_LIMIT    96U
#define CAN_IF_EPASSIVE_LIMIT 128U

static can_if_health_t s_health[CAN_IF_PORT_COUNT];
static uint32_t s_last_recovery_ms[CAN_IF_PORT_COUNT];

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
    return port == CAN_IF_PORT_1;
#endif
}

/* From the shared pool of 28. */
#define CAN_IF_PORT1_FIRST_BANK 0U
#define CAN_IF_PORT1_BANK_COUNT CAN_IF_SLAVE_START_BANK          /* 0..13  */
#define CAN_IF_PORT2_FIRST_BANK CAN_IF_SLAVE_START_BANK
#define CAN_IF_PORT2_BANK_COUNT (28U - CAN_IF_SLAVE_START_BANK)  /* 14..27 */

/* Standard ID in bits [15:5]; IDE and RTR are zero for a standard data frame. */
#define CAN_IF_STDID_TO_FILTER16(id) ((uint16_t)((uint16_t)(id) << 5))

static void CanIf_ConfigAcceptAll(CAN_HandleTypeDef *hcan, uint32_t bank)
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

/* Four 11-bit IDs per bank in 16-bit list mode. */
static void CanIf_ConfigIdList(CAN_HandleTypeDef *hcan,
                               uint32_t first_bank,
                               uint32_t bank_budget,
                               const can_filter_plan_t *plan)
{
    CAN_FilterTypeDef filter;
    uint32_t bank = first_bank;
    uint8_t i = 0U;

    if ((plan->count == 0U) ||
        (CanFilterPlan_BanksRequired(plan) > bank_budget))
    {
        /* A plan that does not fit would silently drop IDs. Refuse to start. */
        Error_Handler();
    }

    filter.FilterActivation     = CAN_FILTER_ENABLE;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterMode           = CAN_FILTERMODE_IDLIST;
    filter.FilterScale          = CAN_FILTERSCALE_16BIT;
    filter.SlaveStartFilterBank = CAN_IF_SLAVE_START_BANK;

    while (i < plan->count)
    {
        uint16_t slot[4];
        uint8_t n;

        for (n = 0U; (n < 4U) && (i < plan->count); n++, i++)
        {
            slot[n] = CAN_IF_STDID_TO_FILTER16(plan->ids[i]);
        }

        /* Pad with the last ID: a zero slot would subscribe the node to 0x000. */
        for (; n < 4U; n++)
        {
            slot[n] = CAN_IF_STDID_TO_FILTER16(plan->ids[plan->count - 1U]);
        }

        filter.FilterBank       = bank;
        filter.FilterIdLow      = slot[0];
        filter.FilterIdHigh     = slot[1];
        filter.FilterMaskIdLow  = slot[2];
        filter.FilterMaskIdHigh = slot[3];

        if (HAL_CAN_ConfigFilter(hcan, &filter) != HAL_OK)
        {
            Error_Handler();
        }
        bank++;
    }
}

static void CanIf_StartPort(CAN_HandleTypeDef *hcan,
                            uint32_t first_bank,
                            uint32_t bank_budget,
                            const can_filter_plan_t *plan)
{
    if (plan == NULL)
    {
        CanIf_ConfigAcceptAll(hcan, first_bank);
    }
    else
    {
        CanIf_ConfigIdList(hcan, first_bank, bank_budget, plan);
    }

    if (HAL_CAN_Start(hcan) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_CAN_ActivateNotification(hcan, CAN_IF_IT_FLAGS) != HAL_OK)
    {
        Error_Handler();
    }
}

void CanIf_Init(can_if_rx_fn on_frame,
                const can_filter_plan_t *plan1,
                const can_filter_plan_t *plan2)
{
    s_on_frame = on_frame;

    memset((void *)&g_can1_stats, 0, sizeof(g_can1_stats));
    memset((void *)&g_can2_stats, 0, sizeof(g_can2_stats));
    memset(s_health, 0, sizeof(s_health));
    memset(s_last_recovery_ms, 0, sizeof(s_last_recovery_ms));

#if CAN_BRINGUP_MODE == CAN_BRINGUP_LOOPBACK
    /* Silent keeps TX recessive, so the board is valid with nothing attached. */
    hcan1.Init.Mode = CAN_MODE_SILENT_LOOPBACK;
    if (HAL_CAN_Init(&hcan1) != HAL_OK)
    {
        Error_Handler();
    }
#endif

    CanIf_StartPort(&hcan1, CAN_IF_PORT1_FIRST_BANK,
                    CAN_IF_PORT1_BANK_COUNT, plan1);

#if CAN_BRINGUP_MODE == CAN_BRINGUP_DUAL
    CanIf_StartPort(&hcan2, CAN_IF_PORT2_FIRST_BANK,
                    CAN_IF_PORT2_BANK_COUNT, plan2);
#else
    (void)plan2;
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

    if (dlc > 8U)
    {
        /* The HAL would mask the length and put an undecodable frame on the bus. */
        stats->tx_err++;
        return false;
    }

    header.StdId              = id;
    header.ExtId              = 0U;
    header.IDE                = CAN_ID_STD;
    header.RTR                = CAN_RTR_DATA;
    header.DLC                = dlc;
    header.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_AddTxMessage(CanIf_Handle(port), &header,
                             (uint8_t *)data, &mailbox) == HAL_OK)
    {
        stats->tx_ok++;
        return true;
    }

    stats->tx_err++;
    return false;
}

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

    uint32_t esr = hcan->Instance->ESR;

    stats->err_count++;
    stats->last_error = hcan->ErrorCode;
    stats->last_esr   = esr;

    /* LEC 3: nobody acknowledged. TEC climbs by 8 per failed transmission and 255
     * is bus-off, which with AutoBusOff disabled does not recover on its own. */
    stats->last_lec = (uint8_t)((esr >> 4) & 0x7U);
    stats->tec      = (uint8_t)((esr >> 16) & 0xFFU);
    stats->rec      = (uint8_t)((esr >> 24) & 0xFFU);
    stats->bus_off  = ((esr & CAN_ESR_BOFF) != 0U) ? 1U : 0U;

    /* The FIFO is three deep; an overrun is lost traffic, invisible unless
     * counted. */
    if ((hcan->ErrorCode & HAL_CAN_ERROR_RX_FOV0) != 0U)
    {
        stats->rx_overrun++;
    }
}

static can_if_health_t CanIf_Classify(const volatile can_if_stats_t *stats)
{
    if (stats->bus_off)
    {
        return CAN_IF_HEALTH_BUS_OFF;
    }
    if ((stats->tec >= CAN_IF_EPASSIVE_LIMIT) || (stats->rec >= CAN_IF_EPASSIVE_LIMIT))
    {
        return CAN_IF_HEALTH_PASSIVE;
    }

    /* ACK error with TEC > 0: alone on the bus. */
    if ((stats->last_lec == CAN_IF_LEC_ACK) && (stats->tec > 0U))
    {
        return CAN_IF_HEALTH_NO_PEER;
    }

    if ((stats->tec >= CAN_IF_EWARN_LIMIT) || (stats->rec >= CAN_IF_EWARN_LIMIT))
    {
        return CAN_IF_HEALTH_WARNING;
    }

    return CAN_IF_HEALTH_OK;
}

static void CanIf_PollPort(can_if_port_t port, uint32_t now_ms)
{
    CAN_HandleTypeDef *hcan = CanIf_Handle(port);
    volatile can_if_stats_t *stats = CanIf_Stats(port);
    uint32_t esr = hcan->Instance->ESR;

    /* Polled too: the interrupt only fires on a transition, so a recovered bus
     * would otherwise still read as broken. */
    stats->last_esr = esr;
    stats->last_lec = (uint8_t)((esr >> 4) & 0x7U);
    stats->tec      = (uint8_t)((esr >> 16) & 0xFFU);
    stats->rec      = (uint8_t)((esr >> 24) & 0xFFU);
    stats->bus_off  = ((esr & CAN_ESR_BOFF) != 0U) ? 1U : 0U;

    s_health[port] = CanIf_Classify(stats);

    if (!stats->bus_off)
    {
        return;
    }

    /* Cycle through init mode; the hardware then waits 128 x 11 recessive bits.
     * Rate-limited so a dead bus is not hammered. */
    if ((uint32_t)(now_ms - s_last_recovery_ms[port]) < CAN_IF_RECOVERY_INTERVAL_MS)
    {
        return;
    }
    s_last_recovery_ms[port] = now_ms;
    stats->recoveries++;

    if (HAL_CAN_Stop(hcan) == HAL_OK)
    {
        if (HAL_CAN_Start(hcan) == HAL_OK)
        {
            (void)HAL_CAN_ActivateNotification(hcan, CAN_IF_IT_FLAGS);
        }
    }
}

void CanIf_Poll(uint32_t now_ms)
{
    CanIf_PollPort(CAN_IF_PORT_1, now_ms);

    if (CanIf_PortEnabled(CAN_IF_PORT_2))
    {
        CanIf_PollPort(CAN_IF_PORT_2, now_ms);
    }
}

can_if_health_t CanIf_Health(can_if_port_t port)
{
    return s_health[port];
}

can_if_health_t CanIf_WorstHealth(void)
{
    can_if_health_t worst = s_health[CAN_IF_PORT_1];

    if (CanIf_PortEnabled(CAN_IF_PORT_2) && (s_health[CAN_IF_PORT_2] > worst))
    {
        worst = s_health[CAN_IF_PORT_2];
    }

    return worst;
}

void CanIf_Silence(void)
{
    /* Ignored: on the failure path the handles may never have been initialised. */
    (void)HAL_CAN_Stop(&hcan1);
    (void)HAL_CAN_Stop(&hcan2);
}

const char *CanIf_HealthName(can_if_health_t health)
{
    switch (health)
    {
        case CAN_IF_HEALTH_OK:      return "OK";
        case CAN_IF_HEALTH_NO_PEER: return "NO_PEER";
        case CAN_IF_HEALTH_WARNING: return "WARNING";
        case CAN_IF_HEALTH_PASSIVE: return "PASSIVE";
        case CAN_IF_HEALTH_BUS_OFF: return "BUS_OFF";
        default:                    return "?";
    }
}
