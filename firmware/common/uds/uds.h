/**
 * @file    uds.h
 * @brief   UDS (ISO 14229) server: sessions, service dispatch, security access.
 *
 * Sits above ISO-TP. A reassembled request goes in, a response comes out, and
 * the caller moves the bytes. No transport, no HAL, no globals, and the clock
 * arrives as a parameter, so the whole state machine runs under host tests.
 *
 * The security access implementation deliberately departs from the usual one.
 * See docs/secure-bootloader.md for the reasoning; in short:
 *
 *   - The conventional seed-key function is symmetric and shared across every
 *     ECU of a type and every tester that services them, so recovering it once
 *     unlocks the fleet. Here the ECU holds no secret at all: it issues a
 *     challenge and verifies a signature against a **public** key.
 *
 *   - The challenge is a monotonic counter rather than a random nonce, because
 *     this part has no entropy source and a challenge derived from a timer
 *     would reproduce exactly the weakness being avoided. Freshness does not
 *     require randomness, only that a value is never accepted twice.
 *
 *   - The failed-attempt count is **persisted**, because the classic bypass is
 *     to power-cycle between guesses when it lives in RAM.
 */
#ifndef UDS_H
#define UDS_H

#include <stdbool.h>
#include <stdint.h>

/* --- Service identifiers ------------------------------------------------- */
#define UDS_SID_SESSION_CONTROL   0x10U
#define UDS_SID_ECU_RESET         0x11U
#define UDS_SID_READ_DATA_BY_ID   0x22U
#define UDS_SID_SECURITY_ACCESS   0x27U
#define UDS_SID_REQUEST_DOWNLOAD  0x34U
#define UDS_SID_TRANSFER_DATA     0x36U
#define UDS_SID_TRANSFER_EXIT     0x37U
#define UDS_SID_TESTER_PRESENT    0x3EU

/** A positive response echoes the service identifier with bit 6 set. */
#define UDS_POSITIVE_RESPONSE(sid) ((uint8_t)((sid) + 0x40U))

/** First byte of a negative response. */
#define UDS_NEGATIVE_RESPONSE 0x7FU

/** Set in a sub-function byte to ask for no positive response. */
#define UDS_SUPPRESS_POS_RSP 0x80U

/* --- Negative response codes --------------------------------------------- */
#define UDS_NRC_GENERAL_REJECT             0x10U
#define UDS_NRC_SERVICE_NOT_SUPPORTED      0x11U
#define UDS_NRC_SUBFUNCTION_NOT_SUPPORTED  0x12U
#define UDS_NRC_INVALID_LENGTH             0x13U
#define UDS_NRC_CONDITIONS_NOT_CORRECT     0x22U
#define UDS_NRC_REQUEST_SEQUENCE_ERROR     0x24U
#define UDS_NRC_REQUEST_OUT_OF_RANGE       0x31U
#define UDS_NRC_SECURITY_ACCESS_DENIED     0x33U
#define UDS_NRC_INVALID_KEY                0x35U
#define UDS_NRC_EXCEEDED_ATTEMPTS          0x36U
#define UDS_NRC_TIME_DELAY_NOT_EXPIRED     0x37U
#define UDS_NRC_UPLOAD_DOWNLOAD_DENIED     0x70U
#define UDS_NRC_TRANSFER_SUSPENDED         0x71U
#define UDS_NRC_PROGRAMMING_FAILURE        0x72U
#define UDS_NRC_WRONG_BLOCK_COUNTER        0x73U
#define UDS_NRC_SERVICE_NOT_IN_SESSION     0x7FU

/* --- Sessions ------------------------------------------------------------ */
#define UDS_SESSION_DEFAULT     0x01U
#define UDS_SESSION_PROGRAMMING 0x02U
#define UDS_SESSION_EXTENDED    0x03U

/** A non-default session lapses back to default after this much silence. */
#ifndef UDS_S3_TIMEOUT_MS
#define UDS_S3_TIMEOUT_MS 5000U
#endif

/* --- Security access ----------------------------------------------------- */

/** Security level implemented here: unlocks programming. */
#define UDS_SEC_LEVEL_PROGRAMMING 0x01U

/** Consecutive failures before the server refuses to listen. */
#ifndef UDS_SEC_MAX_ATTEMPTS
#define UDS_SEC_MAX_ATTEMPTS 3U
#endif

/** How long the refusal lasts. */
#ifndef UDS_SEC_LOCKOUT_MS
#define UDS_SEC_LOCKOUT_MS 10000U
#endif

/** Device identifier length mixed into every challenge. */
#define UDS_UID_LEN 12U

/** counter (4) + device id (12) + level (1). */
#define UDS_CHALLENGE_LEN (4U + UDS_UID_LEN + 1U)

/** Largest signature this build will accept from a tester. */
#ifndef UDS_MAX_KEY_LEN
#define UDS_MAX_KEY_LEN 72U
#endif

/**
 * The part of the security state that must outlive a reset.
 *
 * Deliberately does **not** hold a lockout deadline. A deadline is a tick
 * value, and the tick restarts at zero on reboot, so a persisted one would be
 * meaningless — and reading it back as "already expired" would hand an attacker
 * exactly the power-cycle bypass this is meant to close. The count is
 * persisted; the delay is re-imposed from boot whenever the count is at the
 * limit.
 */
typedef struct
{
    uint32_t challenge_counter; /*!< strictly increasing, never reused */
    uint8_t  failed_attempts;
} uds_sec_record_t;

/** Persistence, injected so it is flash on target and a double under test. */
typedef bool (*uds_sec_load_fn)(void *ctx, uds_sec_record_t *out);
typedef bool (*uds_sec_store_fn)(void *ctx, const uds_sec_record_t *rec);

/**
 * @brief Verify a tester's response against the challenge.
 * @return true only if the response is a valid signature by the holder of the
 *         private key matching the public key this ECU was provisioned with.
 *
 * Injected rather than called directly, so the state machine is testable
 * without a crypto implementation and the algorithm can change without
 * touching the protocol logic.
 */
typedef bool (*uds_verify_fn)(void *ctx,
                              const uint8_t *challenge, uint16_t challenge_len,
                              const uint8_t *response, uint16_t response_len);

/** @brief Read a data identifier. Return bytes written, or 0 if unknown. */
typedef uint16_t (*uds_read_did_fn)(void *ctx, uint16_t did,
                                    uint8_t *out, uint16_t out_cap);

/**
 * Download backend, injected like everything else that touches hardware.
 *
 * Each call returns a negative response code, or zero for success, so the
 * backend can say *why* it refused — out of range is a different fault from a
 * programming failure, and collapsing both into a boolean would lose the
 * distinction exactly where a tester needs it.
 */
typedef struct
{
    /** Open a download. @p max_block is the largest block the ECU will accept. */
    uint8_t (*begin)(void *ctx, uint32_t addr, uint32_t size,
                     uint32_t *max_block);
    uint8_t (*transfer)(void *ctx, const uint8_t *data, uint16_t len);
    uint8_t (*finish)(void *ctx);
    void    (*abort)(void *ctx);
    void   *ctx;
} uds_download_t;

typedef struct
{
    uint8_t  session;
    uint32_t session_last_ms;   /*!< last activity, for the S3 timeout      */

    bool     unlocked;
    bool     seed_issued;       /*!< a challenge is outstanding             */
    uint8_t  challenge[UDS_CHALLENGE_LEN];

    uds_sec_record_t sec;
    uint32_t lockout_started_ms;
    bool     lockout_active;

    /* Injected collaborators */
    uds_sec_load_fn  sec_load;
    uds_sec_store_fn sec_store;
    void            *sec_ctx;
    uds_verify_fn    verify;
    void            *verify_ctx;
    uds_read_did_fn  read_did;
    void            *did_ctx;

    uint8_t uid[UDS_UID_LEN];

    /* Download state. */
    uds_download_t download;
    bool     dl_active;
    uint8_t  dl_next_block;   /*!< sequence counter the next block must carry */
    uint8_t  dl_last_block;   /*!< the previous one, to spot a retransmission */
    uint32_t dl_max_block;

    /* Counters. Anything here moving on a healthy vehicle is worth a look. */
    uint32_t stat_requests;
    uint32_t stat_negative;
    uint32_t stat_auth_failures;
    uint32_t stat_lockouts;
    uint32_t stat_unlocks;
    uint32_t stat_blocks;
    uint32_t stat_block_repeats; /*!< retransmissions absorbed, not rewritten */
} uds_server_t;

/**
 * @brief Reset the server to the default session and load persisted state.
 * @param uid device identifier, mixed into every challenge so a response
 *        captured from one ECU cannot be replayed against another.
 *
 * If the persisted attempt count is already at the limit, the lockout is
 * re-imposed starting now. That is what makes power-cycling useless.
 */
void Uds_Init(uds_server_t *s, const uint8_t uid[UDS_UID_LEN], uint32_t now_ms);

/** @brief Attach the persistence backing for the security record. */
void Uds_SetSecurityStore(uds_server_t *s, uds_sec_load_fn load,
                          uds_sec_store_fn store, void *ctx);

/** @brief Attach the signature verifier. Without one, no unlock can succeed. */
void Uds_SetVerifier(uds_server_t *s, uds_verify_fn verify, void *ctx);

/** @brief Attach the data-identifier reader. Optional. */
void Uds_SetDidReader(uds_server_t *s, uds_read_did_fn read, void *ctx);

/**
 * @brief Attach the download backend.
 *
 * Without one the transfer services answer uploadDownloadNotAccepted, which is
 * the honest response for a build that cannot program anything.
 */
void Uds_SetDownload(uds_server_t *s, const uds_download_t *dl);

/** @brief True while a download is open. */
bool Uds_DownloadActive(const uds_server_t *s);

/**
 * @brief Handle one reassembled request.
 * @return bytes written to @p resp, or 0 when no response is to be sent.
 */
uint16_t Uds_HandleRequest(uds_server_t *s,
                           const uint8_t *req, uint16_t req_len,
                           uint8_t *resp, uint16_t resp_cap,
                           uint32_t now_ms);

/** @brief Drive the session timeout. Call regularly. */
void Uds_Poll(uds_server_t *s, uint32_t now_ms);

/** @brief True while programming services would be permitted. */
bool Uds_IsUnlocked(const uds_server_t *s);

/** @brief Current diagnostic session. */
uint8_t Uds_Session(const uds_server_t *s);

#endif /* UDS_H */
