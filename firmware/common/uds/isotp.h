/**
 * @file    isotp.h
 * @brief   ISO 15765-2 transport layer: segmentation and reassembly over CAN.
 *
 * UDS messages are longer than the eight bytes a classic CAN frame carries, so
 * they are split. A message of up to seven bytes travels as a single frame;
 * anything longer starts with a first frame declaring the total length, then
 * consecutive frames paced by flow control from the receiver.
 *
 * Reassembly is a security boundary, not just plumbing. The declared length in
 * a first frame is chosen by whoever sent it and may claim up to 4095 bytes.
 * This implementation checks it against the available buffer **before copying
 * anything**, and refuses with a flow-control overflow rather than truncating
 * or writing past the end. Sequence numbers are checked on every consecutive
 * frame, orphaned consecutive frames are ignored, and a stalled transfer times
 * out instead of holding the buffer forever.
 *
 * Pure module: no HAL, no globals, no I/O. Frames come in and go out through
 * the caller, and time is a parameter, so the whole thing runs under host tests.
 */
#ifndef ISOTP_H
#define ISOTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Largest message this build will reassemble. UDS programming requests fit. */
#ifndef ISOTP_MAX_MESSAGE
#define ISOTP_MAX_MESSAGE 512U
#endif

/** Protocol ceiling on a first frame's declared length (12-bit field). */
#define ISOTP_MAX_DECLARED 4095U

/** Receiver gives up if a consecutive frame does not arrive within this. */
#ifndef ISOTP_N_CR_TIMEOUT_MS
#define ISOTP_N_CR_TIMEOUT_MS 1000U
#endif

/** Sender gives up if flow control does not arrive within this. */
#ifndef ISOTP_N_BS_TIMEOUT_MS
#define ISOTP_N_BS_TIMEOUT_MS 1000U
#endif

/* Protocol control information: the high nibble of byte 0 names the frame. */
#define ISOTP_PCI_SINGLE       0x0U
#define ISOTP_PCI_FIRST        0x1U
#define ISOTP_PCI_CONSECUTIVE  0x2U
#define ISOTP_PCI_FLOW_CONTROL 0x3U

/* Flow status, the low nibble of a flow control frame. */
#define ISOTP_FS_CONTINUE 0x0U
#define ISOTP_FS_WAIT     0x1U
#define ISOTP_FS_OVERFLOW 0x2U

typedef enum
{
    ISOTP_IDLE = 0,
    ISOTP_RECEIVING,   /*!< first frame seen, awaiting consecutive frames */
    ISOTP_TRANSMITTING /*!< first frame sent, awaiting flow control       */
} isotp_state_t;

/** Why a transfer was abandoned. Kept for diagnostics and for the tests. */
typedef enum
{
    ISOTP_ERR_NONE = 0,
    ISOTP_ERR_TOO_LONG,       /*!< declared length exceeds our buffer      */
    ISOTP_ERR_BAD_SEQUENCE,   /*!< consecutive frame out of order          */
    ISOTP_ERR_ORPHAN_CF,      /*!< consecutive frame with no first frame   */
    ISOTP_ERR_MALFORMED,      /*!< length field impossible for the type    */
    ISOTP_ERR_TIMEOUT,        /*!< peer stopped part-way through           */
    ISOTP_ERR_ABORTED         /*!< a new first frame pre-empted this one   */
} isotp_error_t;

/**
 * @brief Emit one CAN frame. Supplied by the caller.
 * @return true if the frame was accepted for transmission.
 */
typedef bool (*isotp_send_fn)(void *ctx, const uint8_t *data, uint8_t dlc);

typedef struct
{
    isotp_send_fn send;
    void         *send_ctx;

    isotp_state_t state;
    isotp_error_t last_error;

    /* Reassembly */
    uint8_t  rx_buf[ISOTP_MAX_MESSAGE];
    uint16_t rx_expected;   /*!< declared total length                    */
    uint16_t rx_received;   /*!< bytes copied so far                      */
    uint8_t  rx_next_sn;    /*!< sequence number the next frame must carry */
    uint32_t rx_last_ms;
    bool     rx_complete;

    /* Segmented transmit */
    const uint8_t *tx_data;
    uint16_t tx_len;
    uint16_t tx_sent;
    uint8_t  tx_next_sn;
    uint32_t tx_last_ms;
    uint8_t  tx_block_remaining; /*!< frames left before the next flow control */
    uint8_t  tx_block_size;
    uint8_t  tx_st_min_ms;

    /* Counters. Any of these moving on a healthy link is worth investigating. */
    uint32_t stat_rx_messages;
    uint32_t stat_rx_overflow;   /*!< refused for declared length            */
    uint32_t stat_rx_seq_errors;
    uint32_t stat_rx_orphans;
    uint32_t stat_rx_timeouts;
    uint32_t stat_malformed;
} isotp_t;

/**
 * @brief Reset the context and attach the frame sink.
 * @param send may be NULL for a receive-only instance.
 */
void IsoTp_Init(isotp_t *tp, isotp_send_fn send, void *send_ctx);

/**
 * @brief Feed one received CAN frame.
 * @param dlc  frame length as reported by the controller
 * @return true if a complete message is now available.
 *
 * Silently ignores frames that are not addressed to this state machine
 * (an orphaned consecutive frame, for instance) rather than treating them as
 * fatal: on a shared bus, traffic that is not ours is normal.
 */
bool IsoTp_OnFrame(isotp_t *tp, const uint8_t *data, uint8_t dlc, uint32_t now_ms);

/**
 * @brief Begin sending a message, segmenting it if required.
 * @return false if a transfer is already running or the message is too long.
 */
bool IsoTp_Send(isotp_t *tp, const uint8_t *data, uint16_t len, uint32_t now_ms);

/**
 * @brief Drive timeouts and paced transmission. Call regularly.
 *
 * Without this a peer that stops mid-transfer would hold the reassembly buffer
 * indefinitely, which is a denial of service requiring no privilege at all.
 */
void IsoTp_Poll(isotp_t *tp, uint32_t now_ms);

/** @brief Completed message, valid until the next frame is fed in. */
const uint8_t *IsoTp_Message(const isotp_t *tp, uint16_t *len);

/** @brief Discard any message in progress and return to idle. */
void IsoTp_Reset(isotp_t *tp);

/** @brief Human-readable error, for diagnostics and test output. */
const char *IsoTp_ErrorName(isotp_error_t err);

#endif /* ISOTP_H */
