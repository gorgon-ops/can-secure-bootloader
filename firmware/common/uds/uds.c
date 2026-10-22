#include "uds.h"

#include <string.h>

static bool Uds_Expired(uint32_t past, uint32_t now, uint32_t limit_ms)
{
    return (uint32_t)(now - past) >= limit_ms;
}

/* ------------------------------------------------------------------ */
/* Response helpers                                                    */
/* ------------------------------------------------------------------ */

static uint16_t Uds_Negative(uds_server_t *s, uint8_t sid, uint8_t nrc,
                             uint8_t *resp, uint16_t cap)
{
    s->stat_negative++;

    if (cap < 3U)
    {
        return 0U;
    }
    resp[0] = UDS_NEGATIVE_RESPONSE;
    resp[1] = sid;
    resp[2] = nrc;
    return 3U;
}

/* ------------------------------------------------------------------ */
/* Security state                                                      */
/* ------------------------------------------------------------------ */

static void Uds_PersistSecurity(uds_server_t *s)
{
    if (s->sec_store != NULL)
    {
        (void)s->sec_store(s->sec_ctx, &s->sec);
    }
}

/* Also called at start-up when the persisted count is at the limit: a reset
 * must not shorten the penalty. */
static void Uds_StartLockout(uds_server_t *s, uint32_t now_ms)
{
    s->lockout_active     = true;
    s->lockout_started_ms = now_ms;
    s->stat_lockouts++;
}

static bool Uds_LockedOut(uds_server_t *s, uint32_t now_ms)
{
    if (!s->lockout_active)
    {
        return false;
    }

    if (!Uds_Expired(s->lockout_started_ms, now_ms, UDS_SEC_LOCKOUT_MS))
    {
        return true;
    }

    /* Rate-limits guessing rather than barring a mistyping tester forever. */
    s->lockout_active      = false;
    s->sec.failed_attempts = 0U;
    Uds_PersistSecurity(s);
    return false;
}

static void Uds_AbortDownload(uds_server_t *s)
{
    if (s->dl_active)
    {
        if (s->download.abort != NULL)
        {
            s->download.abort(s->download.ctx);
        }
        s->dl_active = false;
    }
}

static void Uds_Lock(uds_server_t *s)
{
    s->unlocked    = false;
    s->seed_issued = false;
    memset(s->challenge, 0, sizeof(s->challenge));

    /* Or a half-written slot would stay open to whoever comes next. */
    Uds_AbortDownload(s);
}

static void Uds_BuildChallenge(uds_server_t *s, uint8_t level)
{
    uint32_t c = s->sec.challenge_counter;

    s->challenge[0] = (uint8_t)(c >> 24);
    s->challenge[1] = (uint8_t)(c >> 16);
    s->challenge[2] = (uint8_t)(c >> 8);
    s->challenge[3] = (uint8_t)(c);
    memcpy(&s->challenge[4], s->uid, UDS_UID_LEN);
    s->challenge[4U + UDS_UID_LEN] = level;
}

/* ------------------------------------------------------------------ */
/* Services                                                            */
/* ------------------------------------------------------------------ */

static uint16_t Uds_SessionControl(uds_server_t *s, const uint8_t *req,
                                   uint16_t len, uint8_t *resp, uint16_t cap,
                                   uint32_t now_ms)
{
    uint8_t sub;

    if (len != 2U)
    {
        return Uds_Negative(s, UDS_SID_SESSION_CONTROL, UDS_NRC_INVALID_LENGTH,
                            resp, cap);
    }

    sub = req[1] & (uint8_t)~UDS_SUPPRESS_POS_RSP;

    if ((sub != UDS_SESSION_DEFAULT) &&
        (sub != UDS_SESSION_PROGRAMMING) &&
        (sub != UDS_SESSION_EXTENDED))
    {
        return Uds_Negative(s, UDS_SID_SESSION_CONTROL,
                            UDS_NRC_SUBFUNCTION_NOT_SUPPORTED, resp, cap);
    }

    /* An unlock must not carry across a session change. */
    if (sub != s->session)
    {
        Uds_Lock(s);
    }

    s->session         = sub;
    s->session_last_ms = now_ms;

    if ((req[1] & UDS_SUPPRESS_POS_RSP) != 0U)
    {
        return 0U;
    }

    if (cap < 6U)
    {
        return 0U;
    }
    resp[0] = UDS_POSITIVE_RESPONSE(UDS_SID_SESSION_CONTROL);
    resp[1] = sub;
    resp[2] = 0x00U; /* P2 server max, high byte */
    resp[3] = 0x32U; /* 50 ms                    */
    resp[4] = 0x01U; /* P2* enhanced, high byte  */
    resp[5] = 0xF4U; /* 5000 ms in 10 ms units   */
    return 6U;
}

static uint16_t Uds_TesterPresent(uds_server_t *s, const uint8_t *req,
                                  uint16_t len, uint8_t *resp, uint16_t cap)
{
    if (len != 2U)
    {
        return Uds_Negative(s, UDS_SID_TESTER_PRESENT, UDS_NRC_INVALID_LENGTH,
                            resp, cap);
    }
    if ((req[1] & (uint8_t)~UDS_SUPPRESS_POS_RSP) != 0x00U)
    {
        return Uds_Negative(s, UDS_SID_TESTER_PRESENT,
                            UDS_NRC_SUBFUNCTION_NOT_SUPPORTED, resp, cap);
    }

    if ((req[1] & UDS_SUPPRESS_POS_RSP) != 0U)
    {
        return 0U;
    }

    if (cap < 2U)
    {
        return 0U;
    }
    resp[0] = UDS_POSITIVE_RESPONSE(UDS_SID_TESTER_PRESENT);
    resp[1] = 0x00U;
    return 2U;
}

static uint16_t Uds_RequestSeed(uds_server_t *s, uint8_t level,
                                uint8_t *resp, uint16_t cap, uint32_t now_ms)
{
    uint16_t i;

    if (Uds_LockedOut(s, now_ms))
    {
        return Uds_Negative(s, UDS_SID_SECURITY_ACCESS,
                            UDS_NRC_TIME_DELAY_NOT_EXPIRED, resp, cap);
    }

    if (cap < (uint16_t)(2U + UDS_CHALLENGE_LEN))
    {
        return Uds_Negative(s, UDS_SID_SECURITY_ACCESS,
                            UDS_NRC_CONDITIONS_NOT_CORRECT, resp, cap);
    }

    /* Already unlocked: the standard says answer with a zero seed. */
    if (s->unlocked)
    {
        resp[0] = UDS_POSITIVE_RESPONSE(UDS_SID_SECURITY_ACCESS);
        resp[1] = level;
        for (i = 0U; i < UDS_CHALLENGE_LEN; i++)
        {
            resp[2U + i] = 0x00U;
        }
        return (uint16_t)(2U + UDS_CHALLENGE_LEN);
    }

    Uds_BuildChallenge(s, level);
    s->seed_issued = true;

    resp[0] = UDS_POSITIVE_RESPONSE(UDS_SID_SECURITY_ACCESS);
    resp[1] = level;
    memcpy(&resp[2], s->challenge, UDS_CHALLENGE_LEN);
    return (uint16_t)(2U + UDS_CHALLENGE_LEN);
}

static uint16_t Uds_SendKey(uds_server_t *s, uint8_t level,
                            const uint8_t *key, uint16_t key_len,
                            uint8_t *resp, uint16_t cap, uint32_t now_ms)
{
    if (Uds_LockedOut(s, now_ms))
    {
        return Uds_Negative(s, UDS_SID_SECURITY_ACCESS,
                            UDS_NRC_TIME_DELAY_NOT_EXPIRED, resp, cap);
    }

    /* Nothing to verify against: a confused tester or a replay. */
    if (!s->seed_issued)
    {
        return Uds_Negative(s, UDS_SID_SECURITY_ACCESS,
                            UDS_NRC_REQUEST_SEQUENCE_ERROR, resp, cap);
    }

    if ((key_len == 0U) || (key_len > (uint16_t)UDS_MAX_KEY_LEN))
    {
        return Uds_Negative(s, UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_LENGTH,
                            resp, cap);
    }

    if ((s->verify == NULL) ||
        !s->verify(s->verify_ctx, s->challenge, UDS_CHALLENGE_LEN, key, key_len))
    {
        /* Consumed on failure too, or one challenge could be ground with many
         * guesses. */
        s->seed_issued = false;
        s->stat_auth_failures++;

        s->sec.challenge_counter++;
        if (s->sec.failed_attempts < 0xFFU)
        {
            s->sec.failed_attempts++;
        }
        Uds_PersistSecurity(s);

        if (s->sec.failed_attempts >= (uint8_t)UDS_SEC_MAX_ATTEMPTS)
        {
            Uds_StartLockout(s, now_ms);
            return Uds_Negative(s, UDS_SID_SECURITY_ACCESS,
                                UDS_NRC_EXCEEDED_ATTEMPTS, resp, cap);
        }

        return Uds_Negative(s, UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_KEY,
                            resp, cap);
    }

    /* Never replayable. */
    s->sec.challenge_counter++;
    s->sec.failed_attempts = 0U;
    Uds_PersistSecurity(s);

    s->unlocked    = true;
    s->seed_issued = false;
    s->lockout_active = false;
    s->stat_unlocks++;

    if (cap < 2U)
    {
        return 0U;
    }
    resp[0] = UDS_POSITIVE_RESPONSE(UDS_SID_SECURITY_ACCESS);
    resp[1] = level;
    return 2U;
}

static uint16_t Uds_SecurityAccess(uds_server_t *s, const uint8_t *req,
                                   uint16_t len, uint8_t *resp, uint16_t cap,
                                   uint32_t now_ms)
{
    uint8_t sub;

    if (len < 2U)
    {
        return Uds_Negative(s, UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_LENGTH,
                            resp, cap);
    }

    sub = req[1] & (uint8_t)~UDS_SUPPRESS_POS_RSP;

    /* Odd sub-functions request a seed, even ones deliver the key. */
    if (sub == UDS_SEC_LEVEL_PROGRAMMING)
    {
        if (len != 2U)
        {
            return Uds_Negative(s, UDS_SID_SECURITY_ACCESS,
                                UDS_NRC_INVALID_LENGTH, resp, cap);
        }
        return Uds_RequestSeed(s, sub, resp, cap, now_ms);
    }

    if (sub == (uint8_t)(UDS_SEC_LEVEL_PROGRAMMING + 1U))
    {
        return Uds_SendKey(s, sub, &req[2], (uint16_t)(len - 2U), resp, cap,
                           now_ms);
    }

    return Uds_Negative(s, UDS_SID_SECURITY_ACCESS,
                        UDS_NRC_SUBFUNCTION_NOT_SUPPORTED, resp, cap);
}

static uint16_t Uds_ReadDataByIdentifier(uds_server_t *s, const uint8_t *req,
                                         uint16_t len, uint8_t *resp,
                                         uint16_t cap)
{
    uint16_t did;
    uint16_t written;

    if (len != 3U)
    {
        return Uds_Negative(s, UDS_SID_READ_DATA_BY_ID, UDS_NRC_INVALID_LENGTH,
                            resp, cap);
    }

    did = (uint16_t)(((uint16_t)req[1] << 8) | req[2]);

    if ((s->read_did == NULL) || (cap < 4U))
    {
        return Uds_Negative(s, UDS_SID_READ_DATA_BY_ID,
                            UDS_NRC_REQUEST_OUT_OF_RANGE, resp, cap);
    }

    written = s->read_did(s->did_ctx, did, &resp[3], (uint16_t)(cap - 3U));
    if (written == 0U)
    {
        return Uds_Negative(s, UDS_SID_READ_DATA_BY_ID,
                            UDS_NRC_REQUEST_OUT_OF_RANGE, resp, cap);
    }

    resp[0] = UDS_POSITIVE_RESPONSE(UDS_SID_READ_DATA_BY_ID);
    resp[1] = (uint8_t)(did >> 8);
    resp[2] = (uint8_t)(did);
    return (uint16_t)(3U + written);
}

static uint16_t Uds_EcuReset(uds_server_t *s, const uint8_t *req, uint16_t len,
                             uint8_t *resp, uint16_t cap)
{
    uint8_t sub;

    if (len != 2U)
    {
        return Uds_Negative(s, UDS_SID_ECU_RESET, UDS_NRC_INVALID_LENGTH,
                            resp, cap);
    }

    sub = req[1] & (uint8_t)~UDS_SUPPRESS_POS_RSP;
    if ((sub != 0x01U) && (sub != 0x03U))
    {
        return Uds_Negative(s, UDS_SID_ECU_RESET,
                            UDS_NRC_SUBFUNCTION_NOT_SUPPORTED, resp, cap);
    }

    /* The reset itself belongs to the caller; the server only answers. */
    if (cap < 2U)
    {
        return 0U;
    }
    resp[0] = UDS_POSITIVE_RESPONSE(UDS_SID_ECU_RESET);
    resp[1] = sub;
    return 2U;
}

/* ------------------------------------------------------------------ */
/* Transfer services                                                   */
/* ------------------------------------------------------------------ */

static uint32_t Uds_ReadBE(const uint8_t *p, uint8_t n)
{
    uint32_t v = 0U;
    uint8_t  i;

    for (i = 0U; i < n; i++)
    {
        v = (v << 8) | p[i];
    }
    return v;
}

static uint16_t Uds_RequestDownload(uds_server_t *s, const uint8_t *req,
                                    uint16_t len, uint8_t *resp, uint16_t cap)
{
    uint8_t  dfi;
    uint8_t  alfid;
    uint8_t  addr_len;
    uint8_t  size_len;
    uint32_t addr;
    uint32_t size;
    uint32_t max_block = 0U;
    uint8_t  nrc;

    if (s->download.begin == NULL)
    {
        /* Rather than accepting a transfer it would silently discard. */
        return Uds_Negative(s, UDS_SID_REQUEST_DOWNLOAD,
                            UDS_NRC_UPLOAD_DOWNLOAD_DENIED, resp, cap);
    }

    if (len < 3U)
    {
        return Uds_Negative(s, UDS_SID_REQUEST_DOWNLOAD,
                            UDS_NRC_INVALID_LENGTH, resp, cap);
    }

    dfi   = req[1];
    alfid = req[2];

    /* No compression or encryption: compressed bytes in a slot would fail
     * verification for no discoverable reason. */
    if (dfi != 0x00U)
    {
        return Uds_Negative(s, UDS_SID_REQUEST_DOWNLOAD,
                            UDS_NRC_REQUEST_OUT_OF_RANGE, resp, cap);
    }

    addr_len = (uint8_t)(alfid & 0x0FU);
    size_len = (uint8_t)((alfid >> 4) & 0x0FU);

    /* Sender-chosen nibbles that say how much to read: validated before indexing
     * anything. */
    if ((addr_len == 0U) || (addr_len > 4U) ||
        (size_len == 0U) || (size_len > 4U))
    {
        return Uds_Negative(s, UDS_SID_REQUEST_DOWNLOAD,
                            UDS_NRC_REQUEST_OUT_OF_RANGE, resp, cap);
    }

    /* And the request must actually carry the bytes the nibbles promise. */
    if (len != (uint16_t)(3U + addr_len + size_len))
    {
        return Uds_Negative(s, UDS_SID_REQUEST_DOWNLOAD,
                            UDS_NRC_INVALID_LENGTH, resp, cap);
    }

    addr = Uds_ReadBE(&req[3], addr_len);
    size = Uds_ReadBE(&req[3U + addr_len], size_len);

    Uds_AbortDownload(s); /* a second request supersedes the first */

    nrc = s->download.begin(s->download.ctx, addr, size, &max_block);
    if (nrc != 0U)
    {
        return Uds_Negative(s, UDS_SID_REQUEST_DOWNLOAD, nrc, resp, cap);
    }

    s->dl_active     = true;
    s->dl_next_block = 1U;  /* the first block carries sequence 1 */
    s->dl_last_block = 0U;
    s->dl_max_block  = max_block;

    if (cap < 4U)
    {
        return 0U;
    }

    /* lengthFormatIdentifier: two bytes of maxNumberOfBlockLength follow. */
    resp[0] = UDS_POSITIVE_RESPONSE(UDS_SID_REQUEST_DOWNLOAD);
    resp[1] = 0x20U;
    resp[2] = (uint8_t)(max_block >> 8);
    resp[3] = (uint8_t)(max_block);
    return 4U;
}

static uint16_t Uds_TransferData(uds_server_t *s, const uint8_t *req,
                                 uint16_t len, uint8_t *resp, uint16_t cap)
{
    uint8_t  block;
    uint16_t data_len;
    uint8_t  nrc;

    if (len < 2U)
    {
        return Uds_Negative(s, UDS_SID_TRANSFER_DATA, UDS_NRC_INVALID_LENGTH,
                            resp, cap);
    }

    if (!s->dl_active)
    {
        return Uds_Negative(s, UDS_SID_TRANSFER_DATA,
                            UDS_NRC_REQUEST_SEQUENCE_ERROR, resp, cap);
    }

    block    = req[1];
    data_len = (uint16_t)(len - 2U);

    /* A repeat of the last block is a retransmission (our response was lost).
     * Answer positively and write nothing: programming the same addresses twice
     * corrupts silently. */
    if ((s->dl_last_block != 0U) && (block == s->dl_last_block))
    {
        s->stat_block_repeats++;
        if (cap < 2U)
        {
            return 0U;
        }
        resp[0] = UDS_POSITIVE_RESPONSE(UDS_SID_TRANSFER_DATA);
        resp[1] = block;
        return 2U;
    }

    if (block != s->dl_next_block)
    {
        /* A lost block would leave a hole in the image. */
        return Uds_Negative(s, UDS_SID_TRANSFER_DATA,
                            UDS_NRC_WRONG_BLOCK_COUNTER, resp, cap);
    }

    if ((s->dl_max_block != 0U) && ((uint32_t)data_len > s->dl_max_block))
    {
        return Uds_Negative(s, UDS_SID_TRANSFER_DATA,
                            UDS_NRC_INVALID_LENGTH, resp, cap);
    }

    if (s->download.transfer == NULL)
    {
        return Uds_Negative(s, UDS_SID_TRANSFER_DATA,
                            UDS_NRC_UPLOAD_DOWNLOAD_DENIED, resp, cap);
    }

    nrc = s->download.transfer(s->download.ctx, &req[2], data_len);
    if (nrc != 0U)
    {
        Uds_AbortDownload(s);
        return Uds_Negative(s, UDS_SID_TRANSFER_DATA, nrc, resp, cap);
    }

    s->dl_last_block = block;
    /* Wraps through zero on a long image; 0x00 is a valid sequence number. */
    s->dl_next_block = (uint8_t)(block + 1U);
    s->stat_blocks++;

    if (cap < 2U)
    {
        return 0U;
    }
    resp[0] = UDS_POSITIVE_RESPONSE(UDS_SID_TRANSFER_DATA);
    resp[1] = block;
    return 2U;
}

static uint16_t Uds_TransferExit(uds_server_t *s, const uint8_t *req,
                                 uint16_t len, uint8_t *resp, uint16_t cap)
{
    uint8_t nrc;

    (void)req;

    if (len != 1U)
    {
        return Uds_Negative(s, UDS_SID_TRANSFER_EXIT, UDS_NRC_INVALID_LENGTH,
                            resp, cap);
    }
    if (!s->dl_active)
    {
        return Uds_Negative(s, UDS_SID_TRANSFER_EXIT,
                            UDS_NRC_REQUEST_SEQUENCE_ERROR, resp, cap);
    }

    nrc = (s->download.finish != NULL)
              ? s->download.finish(s->download.ctx)
              : UDS_NRC_UPLOAD_DOWNLOAD_DENIED;

    s->dl_active = false;

    if (nrc != 0U)
    {
        return Uds_Negative(s, UDS_SID_TRANSFER_EXIT, nrc, resp, cap);
    }

    if (cap < 1U)
    {
        return 0U;
    }
    resp[0] = UDS_POSITIVE_RESPONSE(UDS_SID_TRANSFER_EXIT);
    return 1U;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static bool Uds_ServiceAllowedInSession(const uds_server_t *s, uint8_t sid)
{
    switch (sid)
    {
        case UDS_SID_SESSION_CONTROL:
        case UDS_SID_TESTER_PRESENT:
        case UDS_SID_READ_DATA_BY_ID:
        case UDS_SID_ECU_RESET:
            return true;

        case UDS_SID_SECURITY_ACCESS:
            /* Never in the default session, or the authorisation surface is open to
             * anyone on the bus. */
            return s->session != UDS_SESSION_DEFAULT;

        case UDS_SID_REQUEST_DOWNLOAD:
        case UDS_SID_TRANSFER_DATA:
        case UDS_SID_TRANSFER_EXIT:
            return s->session == UDS_SESSION_PROGRAMMING;

        default:
            return false;
    }
}

static bool Uds_ServiceRequiresSecurity(uint8_t sid)
{
    return (sid == UDS_SID_REQUEST_DOWNLOAD) ||
           (sid == UDS_SID_TRANSFER_DATA) ||
           (sid == UDS_SID_TRANSFER_EXIT);
}

static bool Uds_ServiceKnown(uint8_t sid)
{
    switch (sid)
    {
        case UDS_SID_SESSION_CONTROL:
        case UDS_SID_ECU_RESET:
        case UDS_SID_READ_DATA_BY_ID:
        case UDS_SID_SECURITY_ACCESS:
        case UDS_SID_REQUEST_DOWNLOAD:
        case UDS_SID_TRANSFER_DATA:
        case UDS_SID_TRANSFER_EXIT:
        case UDS_SID_TESTER_PRESENT:
            return true;
        default:
            return false;
    }
}

uint16_t Uds_HandleRequest(uds_server_t *s, const uint8_t *req, uint16_t req_len,
                           uint8_t *resp, uint16_t resp_cap, uint32_t now_ms)
{
    uint8_t sid;

    if ((req == NULL) || (req_len == 0U) || (resp == NULL))
    {
        return 0U;
    }

    s->stat_requests++;

    /* Before the request, so a request after the window is judged against the
     * default session. */
    Uds_Poll(s, now_ms);

    sid = req[0];

    if (!Uds_ServiceKnown(sid))
    {
        return Uds_Negative(s, sid, UDS_NRC_SERVICE_NOT_SUPPORTED, resp,
                            resp_cap);
    }

    if (!Uds_ServiceAllowedInSession(s, sid))
    {
        return Uds_Negative(s, sid, UDS_NRC_SERVICE_NOT_IN_SESSION, resp,
                            resp_cap);
    }

    if (Uds_ServiceRequiresSecurity(sid) && !s->unlocked)
    {
        return Uds_Negative(s, sid, UDS_NRC_SECURITY_ACCESS_DENIED, resp,
                            resp_cap);
    }

    s->session_last_ms = now_ms;

    switch (sid)
    {
        case UDS_SID_SESSION_CONTROL:
            return Uds_SessionControl(s, req, req_len, resp, resp_cap, now_ms);

        case UDS_SID_TESTER_PRESENT:
            return Uds_TesterPresent(s, req, req_len, resp, resp_cap);

        case UDS_SID_SECURITY_ACCESS:
            return Uds_SecurityAccess(s, req, req_len, resp, resp_cap, now_ms);

        case UDS_SID_READ_DATA_BY_ID:
            return Uds_ReadDataByIdentifier(s, req, req_len, resp, resp_cap);

        case UDS_SID_ECU_RESET:
            return Uds_EcuReset(s, req, req_len, resp, resp_cap);

        case UDS_SID_REQUEST_DOWNLOAD:
            return Uds_RequestDownload(s, req, req_len, resp, resp_cap);

        case UDS_SID_TRANSFER_DATA:
            return Uds_TransferData(s, req, req_len, resp, resp_cap);

        case UDS_SID_TRANSFER_EXIT:
            return Uds_TransferExit(s, req, req_len, resp, resp_cap);

        default:
            return Uds_Negative(s, sid, UDS_NRC_SERVICE_NOT_SUPPORTED, resp,
                                resp_cap);
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void Uds_Init(uds_server_t *s, const uint8_t uid[UDS_UID_LEN], uint32_t now_ms)
{
    uds_sec_load_fn  load  = s->sec_load;
    uds_sec_store_fn store = s->sec_store;
    void            *sctx  = s->sec_ctx;
    uds_verify_fn    ver   = s->verify;
    void            *vctx  = s->verify_ctx;
    uds_read_did_fn  rd    = s->read_did;
    void            *dctx  = s->did_ctx;
    uds_download_t   dl    = s->download;

    memset(s, 0, sizeof(*s));

    s->sec_load   = load;
    s->sec_store  = store;
    s->sec_ctx    = sctx;
    s->verify     = ver;
    s->verify_ctx = vctx;
    s->read_did   = rd;
    s->did_ctx    = dctx;
    s->download   = dl;

    s->session         = UDS_SESSION_DEFAULT;
    s->session_last_ms = now_ms;

    if (uid != NULL)
    {
        memcpy(s->uid, uid, UDS_UID_LEN);
    }

    if ((s->sec_load != NULL) && s->sec_load(s->sec_ctx, &s->sec))
    {
        /* At the limit across a reset: serve the delay again from now, or a power
         * cycle resets the penalty. */
        if (s->sec.failed_attempts >= (uint8_t)UDS_SEC_MAX_ATTEMPTS)
        {
            Uds_StartLockout(s, now_ms);
        }
    }
}

void Uds_SetSecurityStore(uds_server_t *s, uds_sec_load_fn load,
                          uds_sec_store_fn store, void *ctx)
{
    s->sec_load  = load;
    s->sec_store = store;
    s->sec_ctx   = ctx;
}

void Uds_SetVerifier(uds_server_t *s, uds_verify_fn verify, void *ctx)
{
    s->verify     = verify;
    s->verify_ctx = ctx;
}

void Uds_SetDidReader(uds_server_t *s, uds_read_did_fn read, void *ctx)
{
    s->read_did = read;
    s->did_ctx  = ctx;
}

void Uds_SetDownload(uds_server_t *s, const uds_download_t *dl)
{
    s->download = *dl;
}

bool Uds_DownloadActive(const uds_server_t *s)
{
    return s->dl_active;
}

void Uds_Poll(uds_server_t *s, uint32_t now_ms)
{
    if (s->session == UDS_SESSION_DEFAULT)
    {
        return;
    }

    if (Uds_Expired(s->session_last_ms, now_ms, UDS_S3_TIMEOUT_MS))
    {
        /* A tester that walked away must not leave the ECU authorised. */
        s->session = UDS_SESSION_DEFAULT;
        Uds_Lock(s);
    }
}

bool Uds_IsUnlocked(const uds_server_t *s)
{
    return s->unlocked;
}

uint8_t Uds_Session(const uds_server_t *s)
{
    return s->session;
}
