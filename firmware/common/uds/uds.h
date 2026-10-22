/* Security access departs from the usual seed-key scheme (see
 * docs/secure-bootloader.md): the ECU holds no secret and verifies a signature
 * against a public key, the challenge is a monotonic counter because the part
 * has no entropy source, and the failed-attempt count is persisted so a power
 * cycle between guesses buys nothing. */
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

#define UDS_POSITIVE_RESPONSE(sid) ((uint8_t)((sid) + 0x40U))

#define UDS_NEGATIVE_RESPONSE 0x7FU

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

/* A non-default session lapses back to default after this much silence. */
#ifndef UDS_S3_TIMEOUT_MS
#define UDS_S3_TIMEOUT_MS 5000U
#endif

/* --- Security access ----------------------------------------------------- */
#define UDS_SEC_LEVEL_PROGRAMMING 0x01U

/* Consecutive failures before a lockout of UDS_SEC_LOCKOUT_MS. */
#ifndef UDS_SEC_MAX_ATTEMPTS
#define UDS_SEC_MAX_ATTEMPTS 3U
#endif

#ifndef UDS_SEC_LOCKOUT_MS
#define UDS_SEC_LOCKOUT_MS 10000U
#endif

#define UDS_UID_LEN 12U

/* counter (4) + device id (12) + level (1) */
#define UDS_CHALLENGE_LEN (4U + UDS_UID_LEN + 1U)

#ifndef UDS_MAX_KEY_LEN
#define UDS_MAX_KEY_LEN 72U
#endif

/* No lockout deadline here: the tick restarts at zero on reboot, so a persisted
 * one would read as expired. The count is persisted and the delay re-imposed
 * from boot whenever it is at the limit. */
typedef struct
{
    uint32_t challenge_counter; /* strictly increasing, never reused */
    uint8_t  failed_attempts;
} uds_sec_record_t;

typedef bool (*uds_sec_load_fn)(void *ctx, uds_sec_record_t *out);
typedef bool (*uds_sec_store_fn)(void *ctx, const uds_sec_record_t *rec);

/* True only for a valid signature by the holder of the provisioned key. */
typedef bool (*uds_verify_fn)(void *ctx,
                              const uint8_t *challenge, uint16_t challenge_len,
                              const uint8_t *response, uint16_t response_len);

typedef uint16_t (*uds_read_did_fn)(void *ctx, uint16_t did,
                                    uint8_t *out, uint16_t out_cap);

/* Each call returns an NRC, or zero for success, so the backend can say why it
 * refused. */
typedef struct
{
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
    uint32_t session_last_ms;

    bool     unlocked;
    bool     seed_issued;
    uint8_t  challenge[UDS_CHALLENGE_LEN];

    uds_sec_record_t sec;
    uint32_t lockout_started_ms;
    bool     lockout_active;

    uds_sec_load_fn  sec_load;
    uds_sec_store_fn sec_store;
    void            *sec_ctx;
    uds_verify_fn    verify;
    void            *verify_ctx;
    uds_read_did_fn  read_did;
    void            *did_ctx;

    uint8_t uid[UDS_UID_LEN];

    uds_download_t download;
    bool     dl_active;
    uint8_t  dl_next_block;   /* sequence counter the next block must carry */
    uint8_t  dl_last_block;   /* to spot a retransmission */
    uint32_t dl_max_block;

    uint32_t stat_requests;
    uint32_t stat_negative;
    uint32_t stat_auth_failures;
    uint32_t stat_lockouts;
    uint32_t stat_unlocks;
    uint32_t stat_blocks;
    uint32_t stat_block_repeats; /* retransmissions absorbed, not rewritten */
} uds_server_t;

/* uid is mixed into every challenge so a response captured from one ECU cannot
 * be replayed against another. A persisted count at the limit re-imposes the
 * lockout from now. */
void Uds_Init(uds_server_t *s, const uint8_t uid[UDS_UID_LEN], uint32_t now_ms);

void Uds_SetSecurityStore(uds_server_t *s, uds_sec_load_fn load,
                          uds_sec_store_fn store, void *ctx);

/* Without a verifier no unlock can succeed. */
void Uds_SetVerifier(uds_server_t *s, uds_verify_fn verify, void *ctx);

void Uds_SetDidReader(uds_server_t *s, uds_read_did_fn read, void *ctx);

/* Without one the transfer services answer uploadDownloadNotAccepted. */
void Uds_SetDownload(uds_server_t *s, const uds_download_t *dl);

bool Uds_DownloadActive(const uds_server_t *s);

uint16_t Uds_HandleRequest(uds_server_t *s,
                           const uint8_t *req, uint16_t req_len,
                           uint8_t *resp, uint16_t resp_cap,
                           uint32_t now_ms);

void Uds_Poll(uds_server_t *s, uint32_t now_ms);
bool Uds_IsUnlocked(const uds_server_t *s);
uint8_t Uds_Session(const uds_server_t *s);

#endif
