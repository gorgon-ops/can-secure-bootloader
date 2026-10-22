#include "isotp.h"

#include <string.h>

static bool IsoTp_Expired(uint32_t past, uint32_t now, uint32_t limit_ms)
{
    return (uint32_t)(now - past) >= limit_ms;
}

static void IsoTp_AbortRx(isotp_t *tp, isotp_error_t why)
{
    tp->state       = ISOTP_IDLE;
    tp->last_error  = why;
    tp->rx_expected = 0U;
    tp->rx_received = 0U;
    tp->rx_complete = false;
}

void IsoTp_Init(isotp_t *tp, isotp_send_fn send, void *send_ctx)
{
    memset(tp, 0, sizeof(*tp));
    tp->send     = send;
    tp->send_ctx = send_ctx;
    tp->state    = ISOTP_IDLE;
}

void IsoTp_Reset(isotp_t *tp)
{
    tp->state       = ISOTP_IDLE;
    tp->rx_expected = 0U;
    tp->rx_received = 0U;
    tp->rx_complete = false;
    tp->tx_data     = NULL;
    tp->tx_len      = 0U;
    tp->tx_sent     = 0U;
}

static bool IsoTp_Emit(isotp_t *tp, const uint8_t *data, uint8_t dlc)
{
    if (tp->send == NULL)
    {
        return false;
    }
    return tp->send(tp->send_ctx, data, dlc);
}

static void IsoTp_SendFlowControl(isotp_t *tp, uint8_t flow_status)
{
    uint8_t fc[3];

    fc[0] = (uint8_t)((ISOTP_PCI_FLOW_CONTROL << 4) | (flow_status & 0x0FU));
    fc[1] = 0U; /* block size 0: no further flow control */
    fc[2] = 0U; /* STmin 0 */

    (void)IsoTp_Emit(tp, fc, sizeof(fc));
}

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */

static bool IsoTp_OnSingleFrame(isotp_t *tp, const uint8_t *data, uint8_t dlc)
{
    uint8_t len = data[0] & 0x0FU;

    /* A truncated frame claiming seven bytes would otherwise read past what was
     * received. */
    if ((len == 0U) || (len > 7U) || ((uint8_t)(len + 1U) > dlc))
    {
        tp->stat_malformed++;
        tp->last_error = ISOTP_ERR_MALFORMED;
        return false;
    }

    memcpy(tp->rx_buf, &data[1], len);
    tp->rx_expected = len;
    tp->rx_received = len;
    tp->rx_complete = true;
    tp->state       = ISOTP_IDLE;
    tp->last_error  = ISOTP_ERR_NONE;
    tp->stat_rx_messages++;
    return true;
}

static bool IsoTp_OnFirstFrame(isotp_t *tp, const uint8_t *data, uint8_t dlc,
                               uint32_t now_ms)
{
    uint16_t declared;
    uint8_t  copy;

    if (dlc < 8U)
    {
        /* A first frame always fills the frame; anything shorter is malformed. */
        tp->stat_malformed++;
        tp->last_error = ISOTP_ERR_MALFORMED;
        return false;
    }

    declared = (uint16_t)(((uint16_t)(data[0] & 0x0FU) << 8) | data[1]);

    /* <= 7 bytes should have been a single frame. */
    if (declared <= 7U)
    {
        tp->stat_malformed++;
        tp->last_error = ISOTP_ERR_MALFORMED;
        return false;
    }

    /* Attacker-controlled: checked before a single byte is copied, and refused
     * outright rather than truncated into a message the sender never described. */
    if (declared > (uint16_t)ISOTP_MAX_MESSAGE)
    {
        tp->stat_rx_overflow++;
        IsoTp_SendFlowControl(tp, ISOTP_FS_OVERFLOW);
        IsoTp_AbortRx(tp, ISOTP_ERR_TOO_LONG);
        return false;
    }

    /* Pre-empts the old message. Counted: on a quiet bus it should not happen. */
    if (tp->state == ISOTP_RECEIVING)
    {
        tp->last_error = ISOTP_ERR_ABORTED;
    }

    copy = 6U;
    memcpy(tp->rx_buf, &data[2], copy);

    tp->rx_expected = declared;
    tp->rx_received = copy;
    tp->rx_next_sn  = 1U;
    tp->rx_last_ms  = now_ms;
    tp->rx_complete = false;
    tp->state       = ISOTP_RECEIVING;

    IsoTp_SendFlowControl(tp, ISOTP_FS_CONTINUE);
    return false;
}

static bool IsoTp_OnConsecutiveFrame(isotp_t *tp, const uint8_t *data,
                                     uint8_t dlc, uint32_t now_ms)
{
    uint8_t  sn;
    uint16_t remaining;
    uint16_t copy;

    if (tp->state != ISOTP_RECEIVING)
    {
        /* Somebody else's traffic on a shared bus: counted, then ignored. */
        tp->stat_rx_orphans++;
        tp->last_error = ISOTP_ERR_ORPHAN_CF;
        return false;
    }

    sn = data[0] & 0x0FU;
    if (sn != tp->rx_next_sn)
    {
        /* Lost or injected frames: abandon rather than splice two transfers. */
        tp->stat_rx_seq_errors++;
        IsoTp_AbortRx(tp, ISOTP_ERR_BAD_SEQUENCE);
        return false;
    }

    remaining = (uint16_t)(tp->rx_expected - tp->rx_received);
    copy      = (remaining < 7U) ? remaining : 7U;

    if ((uint16_t)(copy + 1U) > (uint16_t)dlc)
    {
        copy = (uint16_t)(dlc - 1U);
    }

    if (copy > 0U)
    {
        memcpy(&tp->rx_buf[tp->rx_received], &data[1], copy);
        tp->rx_received = (uint16_t)(tp->rx_received + copy);
    }

    tp->rx_next_sn = (uint8_t)((tp->rx_next_sn + 1U) & 0x0FU);
    tp->rx_last_ms = now_ms;

    if (tp->rx_received >= tp->rx_expected)
    {
        tp->rx_complete = true;
        tp->state       = ISOTP_IDLE;
        tp->last_error  = ISOTP_ERR_NONE;
        tp->stat_rx_messages++;
        return true;
    }

    return false;
}

static void IsoTp_OnFlowControl(isotp_t *tp, const uint8_t *data, uint8_t dlc,
                                uint32_t now_ms)
{
    uint8_t fs;

    if ((tp->state != ISOTP_TRANSMITTING) || (dlc < 3U))
    {
        return;
    }

    fs = data[0] & 0x0FU;

    if (fs == ISOTP_FS_OVERFLOW)
    {
        tp->state      = ISOTP_IDLE;
        tp->last_error = ISOTP_ERR_TOO_LONG;
        tp->tx_data    = NULL;
        return;
    }

    if (fs == ISOTP_FS_WAIT)
    {
        tp->tx_last_ms = now_ms;
        return;
    }

    tp->tx_block_size      = data[1];
    tp->tx_st_min_ms       = (data[2] <= 0x7FU) ? data[2] : 0U;
    tp->tx_block_remaining = tp->tx_block_size;
    tp->tx_last_ms         = now_ms;
}

bool IsoTp_OnFrame(isotp_t *tp, const uint8_t *data, uint8_t dlc, uint32_t now_ms)
{
    uint8_t pci;

    if ((data == NULL) || (dlc == 0U))
    {
        tp->stat_malformed++;
        tp->last_error = ISOTP_ERR_MALFORMED;
        return false;
    }

    pci = (uint8_t)(data[0] >> 4);

    switch (pci)
    {
        case ISOTP_PCI_SINGLE:
            return IsoTp_OnSingleFrame(tp, data, dlc);

        case ISOTP_PCI_FIRST:
            return IsoTp_OnFirstFrame(tp, data, dlc, now_ms);

        case ISOTP_PCI_CONSECUTIVE:
            return IsoTp_OnConsecutiveFrame(tp, data, dlc, now_ms);

        case ISOTP_PCI_FLOW_CONTROL:
            IsoTp_OnFlowControl(tp, data, dlc, now_ms);
            return false;

        default:
            tp->stat_malformed++;
            tp->last_error = ISOTP_ERR_MALFORMED;
            return false;
    }
}

/* ------------------------------------------------------------------ */
/* Transmit                                                            */
/* ------------------------------------------------------------------ */

bool IsoTp_Send(isotp_t *tp, const uint8_t *data, uint16_t len, uint32_t now_ms)
{
    uint8_t frame[8];

    if ((tp->state != ISOTP_IDLE) || (len == 0U) || (data == NULL))
    {
        return false;
    }
    if (len > (uint16_t)ISOTP_MAX_DECLARED)
    {
        return false;
    }

    if (len <= 7U)
    {
        frame[0] = (uint8_t)((ISOTP_PCI_SINGLE << 4) | (uint8_t)len);
        memcpy(&frame[1], data, len);
        return IsoTp_Emit(tp, frame, (uint8_t)(len + 1U));
    }

    frame[0] = (uint8_t)((ISOTP_PCI_FIRST << 4) | (uint8_t)((len >> 8) & 0x0FU));
    frame[1] = (uint8_t)(len & 0xFFU);
    memcpy(&frame[2], data, 6U);

    if (!IsoTp_Emit(tp, frame, 8U))
    {
        return false;
    }

    tp->tx_data            = data;
    tp->tx_len             = len;
    tp->tx_sent            = 6U;
    tp->tx_next_sn         = 1U;
    tp->tx_last_ms         = now_ms;
    tp->tx_block_remaining = 0U;
    tp->state              = ISOTP_TRANSMITTING;
    return true;
}

static void IsoTp_PumpTransmit(isotp_t *tp, uint32_t now_ms)
{
    uint8_t frame[8];

    while (tp->tx_sent < tp->tx_len)
    {
        uint16_t remaining = (uint16_t)(tp->tx_len - tp->tx_sent);
        uint8_t  chunk     = (remaining < 7U) ? (uint8_t)remaining : 7U;

        frame[0] = (uint8_t)((ISOTP_PCI_CONSECUTIVE << 4) | tp->tx_next_sn);
        memcpy(&frame[1], &tp->tx_data[tp->tx_sent], chunk);

        if (!IsoTp_Emit(tp, frame, (uint8_t)(chunk + 1U)))
        {
            return; /* mailboxes full; try again next poll */
        }

        tp->tx_sent    = (uint16_t)(tp->tx_sent + chunk);
        tp->tx_next_sn = (uint8_t)((tp->tx_next_sn + 1U) & 0x0FU);
        tp->tx_last_ms = now_ms;

        if (tp->tx_block_size != 0U)
        {
            tp->tx_block_remaining--;
            if (tp->tx_block_remaining == 0U)
            {
                return; /* wait for the next flow control */
            }
        }
    }

    if (tp->tx_sent >= tp->tx_len)
    {
        tp->state   = ISOTP_IDLE;
        tp->tx_data = NULL;
    }
}

void IsoTp_Poll(isotp_t *tp, uint32_t now_ms)
{
    if (tp->state == ISOTP_RECEIVING)
    {
        /* Otherwise a peer that stops after a first frame holds the buffer forever. */
        if (IsoTp_Expired(tp->rx_last_ms, now_ms, ISOTP_N_CR_TIMEOUT_MS))
        {
            tp->stat_rx_timeouts++;
            IsoTp_AbortRx(tp, ISOTP_ERR_TIMEOUT);
        }
        return;
    }

    if (tp->state == ISOTP_TRANSMITTING)
    {
        if (IsoTp_Expired(tp->tx_last_ms, now_ms, ISOTP_N_BS_TIMEOUT_MS))
        {
            tp->state      = ISOTP_IDLE;
            tp->last_error = ISOTP_ERR_TIMEOUT;
            tp->tx_data    = NULL;
            return;
        }
        IsoTp_PumpTransmit(tp, now_ms);
    }
}

const uint8_t *IsoTp_Message(const isotp_t *tp, uint16_t *len)
{
    if (!tp->rx_complete)
    {
        if (len != NULL)
        {
            *len = 0U;
        }
        return NULL;
    }

    if (len != NULL)
    {
        *len = tp->rx_received;
    }
    return tp->rx_buf;
}

const char *IsoTp_ErrorName(isotp_error_t err)
{
    switch (err)
    {
        case ISOTP_ERR_NONE:         return "NONE";
        case ISOTP_ERR_TOO_LONG:     return "TOO_LONG";
        case ISOTP_ERR_BAD_SEQUENCE: return "BAD_SEQUENCE";
        case ISOTP_ERR_ORPHAN_CF:    return "ORPHAN_CF";
        case ISOTP_ERR_MALFORMED:    return "MALFORMED";
        case ISOTP_ERR_TIMEOUT:      return "TIMEOUT";
        case ISOTP_ERR_ABORTED:      return "ABORTED";
        default:                     return "?";
    }
}
