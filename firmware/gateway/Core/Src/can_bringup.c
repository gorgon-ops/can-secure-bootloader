/**
  ******************************************************************************
  * @file    can_bringup.c
  * @brief   Dual-bxCAN bring-up harness. See can_bringup.h.
  ******************************************************************************
  */
#include "can_bringup.h"
#include <string.h>

extern CAN_HandleTypeDef hcan1;
extern CAN_HandleTypeDef hcan2;

volatile can_bringup_stats_t g_can1_stats;
volatile can_bringup_stats_t g_can2_stats;

#define CAN_BRINGUP_TX_PERIOD_MS 500U

static uint32_t s_last_tx_tick;
static uint8_t  s_tx_seq;

/* Interrupts we care about during bring-up. The error sources matter as much as
   the RX path: a bus with no ACK partner reports itself here rather than just
   failing silently. */
#define CAN_BRINGUP_IT_FLAGS (CAN_IT_RX_FIFO0_MSG_PENDING | \
                              CAN_IT_ERROR_WARNING        | \
                              CAN_IT_ERROR_PASSIVE        | \
                              CAN_IT_BUSOFF               | \
                              CAN_IT_LAST_ERROR_CODE      | \
                              CAN_IT_ERROR)

/**
  * @brief Accept-everything filter for one controller.
  *
  * A zero mask matches every ID. That is correct for bring-up — you want to see
  * whatever is on the wire — and wrong for the vehicle, where each node should
  * filter in hardware for only the IDs it consumes.
  */
static void CanBringup_ConfigFilter(CAN_HandleTypeDef *hcan, uint32_t bank)
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

  /* Programmed identically on both handles: it is a single global split of the
     shared bank pool, not a per-controller setting. */
  filter.SlaveStartFilterBank = CAN_BRINGUP_SLAVE_START_BANK;

  if (HAL_CAN_ConfigFilter(hcan, &filter) != HAL_OK)
  {
    Error_Handler();
  }
}

void CanBringup_Init(void)
{
  memset((void *)&g_can1_stats, 0, sizeof(g_can1_stats));
  memset((void *)&g_can2_stats, 0, sizeof(g_can2_stats));

#if CAN_BRINGUP_MODE == CAN_BRINGUP_LOOPBACK
  /* Re-init CAN1 in silent-loopback. Silent keeps the TX pin recessive, so the
     test is valid with nothing at all attached to the board. MX_CAN1_Init() has
     already run, so MspInit is skipped and only the mode changes. */
  hcan1.Init.Mode = CAN_MODE_SILENT_LOOPBACK;
  if (HAL_CAN_Init(&hcan1) != HAL_OK)
  {
    Error_Handler();
  }
#endif

  CanBringup_ConfigFilter(&hcan1, 0U);

  if (HAL_CAN_Start(&hcan1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_CAN_ActivateNotification(&hcan1, CAN_BRINGUP_IT_FLAGS) != HAL_OK)
  {
    Error_Handler();
  }

#if CAN_BRINGUP_MODE == CAN_BRINGUP_DUAL
  /* CAN2's banks start at SLAVE_START; using bank 0 here would collide with
     CAN1 and the filter would never match. */
  CanBringup_ConfigFilter(&hcan2, CAN_BRINGUP_SLAVE_START_BANK);

  if (HAL_CAN_Start(&hcan2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_CAN_ActivateNotification(&hcan2, CAN_BRINGUP_IT_FLAGS) != HAL_OK)
  {
    Error_Handler();
  }
#endif

  s_last_tx_tick = HAL_GetTick();
}

static void CanBringup_Send(CAN_HandleTypeDef *hcan,
                            uint32_t std_id,
                            volatile can_bringup_stats_t *stats)
{
  CAN_TxHeaderTypeDef header;
  uint32_t mailbox;
  uint8_t data[8];

  header.StdId              = std_id;
  header.ExtId              = 0U;
  header.IDE                = CAN_ID_STD;
  header.RTR                = CAN_RTR_DATA;
  header.DLC                = 8U;
  header.TransmitGlobalTime = DISABLE;

  data[0] = 0xDEU;
  data[1] = 0xADU;
  data[2] = 0xBEU;
  data[3] = 0xEFU;
  data[4] = (uint8_t)(std_id >> 8);
  data[5] = (uint8_t)(std_id);
  data[6] = 0x00U;
  data[7] = s_tx_seq;  /* advances every cycle: a frozen value means a stuck sender */

  if (HAL_CAN_AddTxMessage(hcan, &header, data, &mailbox) == HAL_OK)
  {
    stats->tx_ok++;
  }
  else
  {
    /* Every mailbox busy. On a real bus this means nothing is ACKing: with
       AutoRetransmission disabled the frame is dropped rather than retried. */
    stats->tx_err++;
  }
}

void CanBringup_Task(void)
{
  uint32_t now = HAL_GetTick();

  if ((now - s_last_tx_tick) < CAN_BRINGUP_TX_PERIOD_MS)
  {
    return;
  }
  s_last_tx_tick = now;
  s_tx_seq++;

  CanBringup_Send(&hcan1, CAN_BRINGUP_ID_NODE1, &g_can1_stats);

#if CAN_BRINGUP_MODE == CAN_BRINGUP_DUAL
  /* Queued back-to-back with CAN1's frame so the two controllers contend for
     the bus. CAN1 holds the lower ID and must win. */
  CanBringup_Send(&hcan2, CAN_BRINGUP_ID_NODE2, &g_can2_stats);
#endif
}

/* ---------------------------------------------------------------------------
   HAL callbacks. Shared by both controllers; dispatch on hcan->Instance.
   --------------------------------------------------------------------------- */

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
  CAN_RxHeaderTypeDef header;
  uint8_t data[8];
  volatile can_bringup_stats_t *stats;

  if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, data) != HAL_OK)
  {
    return;
  }

  stats = (hcan->Instance == CAN1) ? &g_can1_stats : &g_can2_stats;

  stats->last_rx_id  = (header.IDE == CAN_ID_STD) ? header.StdId : header.ExtId;
  stats->last_rx_dlc = (uint8_t)header.DLC;
  memcpy((void *)stats->last_rx_data, data, sizeof(data));
  stats->rx_count++;
}

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
  volatile can_bringup_stats_t *stats =
      (hcan->Instance == CAN1) ? &g_can1_stats : &g_can2_stats;

  stats->err_count++;
  stats->last_error = hcan->ErrorCode;

  /* ESR carries the transmit and receive error counters plus the last error
     code. Reading it tells you *why* the bus failed rather than only that it
     did: LEC 0b011 is an ACK error, which is what a single node with no partner
     reports. */
  stats->last_esr = hcan->Instance->ESR;
}
