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
                         CAN_IT_RX_FIFO0_OVERRUN     | \
                         CAN_IT_ERROR_WARNING        | \
                         CAN_IT_ERROR_PASSIVE        | \
                         CAN_IT_BUSOFF               | \
                         CAN_IT_LAST_ERROR_CODE      | \
                         CAN_IT_ERROR)

/** Wait between bus-off recovery attempts, so a dead bus is not hammered. */
#define CAN_IF_RECOVERY_INTERVAL_MS 500U

/** bxCAN error-counter thresholds, from the CAN specification. */
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
    memset(s_health, 0, sizeof(s_health));
    memset(s_last_recovery_ms, 0, sizeof(s_last_recovery_ms));

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

    if (dlc > 8U)
    {
        /* A classic CAN frame carries at most 8 bytes. Rejected here rather
           than handed to the HAL, which would mask the value and put a frame
           on the bus that no receiver can decode. */
        stats->tx_err++;
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

    uint32_t esr = hcan->Instance->ESR;

    stats->err_count++;
    stats->last_error = hcan->ErrorCode;
    stats->last_esr   = esr;

    /* ESR says *why* the bus failed rather than only that it did. Decoding it
       here means the debugger shows the diagnosis directly:
         LEC 3   nobody acknowledged -- there is no other live node on the bus
         TEC     climbs by 8 per failed transmission; 255 means bus-off
         BOFF    the controller has dropped off and, with AutoBusOff disabled,
                 will not rejoin on its own */
    stats->last_lec = (uint8_t)((esr >> 4) & 0x7U);
    stats->tec      = (uint8_t)((esr >> 16) & 0xFFU);
    stats->rec      = (uint8_t)((esr >> 24) & 0xFFU);
    stats->bus_off  = ((esr & CAN_ESR_BOFF) != 0U) ? 1U : 0U;

    /* The hardware FIFO is three frames deep. Overrun means the receive
       interrupt was starved long enough to lose traffic -- a real fault, and
       invisible unless counted, because the frames simply never arrive. */
    if ((hcan->ErrorCode & HAL_CAN_ERROR_RX_FOV0) != 0U)
    {
        stats->rx_overrun++;
    }
}

/* ---------------------------------------------------------------------------
   Error-state polling and bus-off recovery
   --------------------------------------------------------------------------- */

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

    /* An acknowledgment error with a non-zero transmit counter is the specific
       and very common case of being alone on the bus. Naming it separately
       saves the reader inferring it from two numbers. */
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

    /* Polled as well as interrupt-driven: the error interrupt fires on a
       transition, so between transitions these would go stale and a bus that
       had recovered would still read as broken. */
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

    /* AutoBusOff is disabled, so the controller will not rejoin by itself.
       Recovery means cycling it through initialisation mode, after which the
       hardware waits for 128 sequences of 11 recessive bits before it may
       transmit again. Rate-limited so a genuinely dead bus is not hammered,
       and counted so an intermittent fault is still visible afterwards. */
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
    /* Return values ignored deliberately: this runs on the failure path, where
       the handles may never have been initialised. HAL_CAN_Stop rejects that
       cleanly, and there is nothing useful to do about it here anyway. */
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
