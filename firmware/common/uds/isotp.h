/* Reassembly is a security boundary: the declared length in a first frame is
 * attacker-chosen, so it is checked against the buffer before anything is
 * copied, and an oversize transfer is refused rather than truncated. */
#ifndef ISOTP_H
#define ISOTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef ISOTP_MAX_MESSAGE
#define ISOTP_MAX_MESSAGE 512U
#endif

#define ISOTP_MAX_DECLARED 4095U

/* N_Cr: receiver gives up on a missing consecutive frame. */
#ifndef ISOTP_N_CR_TIMEOUT_MS
#define ISOTP_N_CR_TIMEOUT_MS 1000U
#endif

/* N_Bs: sender gives up on missing flow control. */
#ifndef ISOTP_N_BS_TIMEOUT_MS
#define ISOTP_N_BS_TIMEOUT_MS 1000U
#endif

#define ISOTP_PCI_SINGLE       0x0U
#define ISOTP_PCI_FIRST        0x1U
#define ISOTP_PCI_CONSECUTIVE  0x2U
#define ISOTP_PCI_FLOW_CONTROL 0x3U

#define ISOTP_FS_CONTINUE 0x0U
#define ISOTP_FS_WAIT     0x1U
#define ISOTP_FS_OVERFLOW 0x2U

typedef enum
{
    ISOTP_IDLE = 0,
    ISOTP_RECEIVING,
    ISOTP_TRANSMITTING
} isotp_state_t;

typedef enum
{
    ISOTP_ERR_NONE = 0,
    ISOTP_ERR_TOO_LONG,
    ISOTP_ERR_BAD_SEQUENCE,
    ISOTP_ERR_ORPHAN_CF,
    ISOTP_ERR_MALFORMED,
    ISOTP_ERR_TIMEOUT,        /* peer stopped part-way through */
    ISOTP_ERR_ABORTED
} isotp_error_t;

typedef bool (*isotp_send_fn)(void *ctx, const uint8_t *data, uint8_t dlc);

typedef struct
{
    isotp_send_fn send;
    void         *send_ctx;

    isotp_state_t state;
    isotp_error_t last_error;

    uint8_t  rx_buf[ISOTP_MAX_MESSAGE];
    uint16_t rx_expected;
    uint16_t rx_received;
    uint8_t  rx_next_sn;    /* sequence number the next frame must carry */
    uint32_t rx_last_ms;
    bool     rx_complete;

    const uint8_t *tx_data;
    uint16_t tx_len;
    uint16_t tx_sent;
    uint8_t  tx_next_sn;
    uint32_t tx_last_ms;
    uint8_t  tx_block_remaining;
    uint8_t  tx_block_size;
    uint8_t  tx_st_min_ms;

    /* Any of these moving on a healthy link is worth investigating. */
    uint32_t stat_rx_messages;
    uint32_t stat_rx_overflow;   /* refused for declared length */
    uint32_t stat_rx_seq_errors;
    uint32_t stat_rx_orphans;
    uint32_t stat_rx_timeouts;
    uint32_t stat_malformed;
} isotp_t;

void IsoTp_Init(isotp_t *tp, isotp_send_fn send, void *send_ctx);

/* Frames that are not ours (an orphaned consecutive frame, say) are ignored,
 * not fatal: on a shared bus that traffic is normal. */
bool IsoTp_OnFrame(isotp_t *tp, const uint8_t *data, uint8_t dlc, uint32_t now_ms);

/* Above seven bytes, data is BORROWED, not copied: the tail is read from this
 * pointer during later IsoTp_Poll() calls, so it must outlive the transfer. */
bool IsoTp_Send(isotp_t *tp, const uint8_t *data, uint16_t len, uint32_t now_ms);

/* Without the timeout, a peer stopping mid-transfer would hold the buffer
 * forever - a denial of service needing no privilege at all. */
void IsoTp_Poll(isotp_t *tp, uint32_t now_ms);

const uint8_t *IsoTp_Message(const isotp_t *tp, uint16_t *len);
void IsoTp_Reset(isotp_t *tp);
const char *IsoTp_ErrorName(isotp_error_t err);

#endif
