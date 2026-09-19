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

/**
 * Re-impose the lockout from the current instant.
 *
 * Called both when the limit is first reached and at start-up when the
 * persisted count is already at the limit. The second case is the whole point:
 * a reset must not shorten the penalty, or power-cycling between guesses
 * becomes free.
 */
static void Uds_StartLockout(uds_server_t *s, uint32_t now_ms)
{
    s->lockout_active     = true;
    s->lockout_started_ms = now_ms;
    s->stat_lockouts++;
}

/** @return true while the server is refusing authorisation attempts. */
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

    /* The delay has been served. Clear the count so a legitimate tester that
       mistyped can proceed; the effect is to rate-limit guessing rather than
       to bar it forever, which is what the standard intends. */
    s->lockout_active      = false;
    s->sec.failed_attempts = 0U;
    Uds_PersistSecurity(s);
    return false;
}

static void Uds_Lock(uds_server_t *s)
{
    s->unlocked    = false;
    s->seed_issued = false;
    memset(s->challenge, 0, sizeof(s->challenge));
}

/** Build the challenge: counter, device identifier, requested level. */
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

    /* Any session change drops authorisation. Carrying an unlock across a
       transition would let a tester authorise in one context and spend it in
       another. */
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
        return 0U; /* the usual form: keeps the session alive, stays silent */
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

    /* Already unlocked: the standard says answer with a zero seed rather than
       issuing a fresh challenge. */
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

    /* A key with no outstanding challenge is either a confused tester or an
       attempt to replay one captured earlier. Either way there is nothing to
       verify it against. */
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
        /* Consume the challenge on failure as well as on success. Leaving it
           outstanding would let an attacker grind many guesses against one
           challenge, which is the opposite of what the counter is for. */
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

    /* Success. Advance the counter so this exchange can never be replayed,
       and clear the failure history. */
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

    /* Odd sub-functions request a seed, even ones deliver the key. Only one
       level is implemented. */
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
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/** @return true if the service may be used in the session that is active. */
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
            /* Never in the default session. Leaving it reachable there would
               expose the authorisation surface to anyone on the bus without
               even the small step of requesting a session. */
            return s->session != UDS_SESSION_DEFAULT;

        case UDS_SID_REQUEST_DOWNLOAD:
        case UDS_SID_TRANSFER_DATA:
        case UDS_SID_TRANSFER_EXIT:
            return s->session == UDS_SESSION_PROGRAMMING;

        default:
            return false;
    }
}

/** @return true if the service additionally requires an unlocked server. */
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

    /* Evaluate the session timeout before the request, so a request arriving
       after the window has lapsed is judged against the default session rather
       than the one the tester believes is still open. */
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

    /* Any accepted request keeps the session alive. */
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

        default:
            /* Reachable only for the transfer services, which are authorised
               above but not implemented until the flash driver exists. Saying
               so honestly beats pretending to accept an image. */
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

    memset(s, 0, sizeof(*s));

    s->sec_load   = load;
    s->sec_store  = store;
    s->sec_ctx    = sctx;
    s->verify     = ver;
    s->verify_ctx = vctx;
    s->read_did   = rd;
    s->did_ctx    = dctx;

    s->session         = UDS_SESSION_DEFAULT;
    s->session_last_ms = now_ms;

    if (uid != NULL)
    {
        memcpy(s->uid, uid, UDS_UID_LEN);
    }

    if ((s->sec_load != NULL) && s->sec_load(s->sec_ctx, &s->sec))
    {
        /* The count survived the reset. If it was already at the limit, serve
           the delay again starting now -- otherwise a power cycle would be a
           free reset of the penalty, which is the classic bypass. */
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
