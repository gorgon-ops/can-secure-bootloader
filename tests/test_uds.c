/* The point of this file is security access: no unlock from the default
 * session, no replay, no grinding one challenge, and power-cycling does not
 * clear the failed-attempt count. */
#include "test_util.h"

#include "uds.h"

#include <string.h>

/* Survives a simulated reset because the test owns it, not the server. */
typedef struct
{
    uds_sec_record_t rec;
    bool             present;
    int              stores;
} fake_flash_t;

static bool flash_load(void *ctx, uds_sec_record_t *out)
{
    fake_flash_t *f = (fake_flash_t *)ctx;
    if (!f->present) {
        return false;
    }
    *out = f->rec;
    return true;
}

static bool flash_store(void *ctx, const uds_sec_record_t *rec)
{
    fake_flash_t *f = (fake_flash_t *)ctx;
    f->rec = *rec;
    f->present = true;
    f->stores++;
    return true;
}

/* Accepts the challenge reversed: enough to prove the server only accepts a
 * response bound to the challenge it issued. */
typedef struct
{
    int calls;
    bool accept_anything;
} fake_verifier_t;

static bool verify_reversed(void *ctx, const uint8_t *challenge, uint16_t clen,
                            const uint8_t *response, uint16_t rlen)
{
    fake_verifier_t *v = (fake_verifier_t *)ctx;
    uint16_t i;

    v->calls++;
    if (v->accept_anything) {
        return true;
    }
    if (clen != rlen) {
        return false;
    }
    for (i = 0; i < clen; i++) {
        if (response[i] != challenge[clen - 1 - i]) {
            return false;
        }
    }
    return true;
}

static uint16_t read_did(void *ctx, uint16_t did, uint8_t *out, uint16_t cap)
{
    (void)ctx;
    if ((did == 0xF190U) && (cap >= 4U)) {
        memcpy(out, "BFMC", 4);
        return 4;
    }
    return 0;
}

typedef struct
{
    int      begins;
    int      transfers;
    int      finishes;
    int      aborts;
    uint32_t last_addr;
    uint32_t last_size;
    uint32_t total_bytes;
    uint8_t  begin_nrc;
    uint8_t  transfer_nrc;
    uint8_t  finish_nrc;
    uint32_t max_block;
} fake_download_t;

static uint8_t dl_begin(void *ctx, uint32_t addr, uint32_t size,
                        uint32_t *max_block)
{
    fake_download_t *d = (fake_download_t *)ctx;
    d->begins++;
    d->last_addr = addr;
    d->last_size = size;
    *max_block = d->max_block;
    return d->begin_nrc;
}

static uint8_t dl_transfer(void *ctx, const uint8_t *data, uint16_t len)
{
    fake_download_t *d = (fake_download_t *)ctx;
    (void)data;
    d->transfers++;
    if (d->transfer_nrc == 0) {
        d->total_bytes += len;
    }
    return d->transfer_nrc;
}

static uint8_t dl_finish(void *ctx)
{
    fake_download_t *d = (fake_download_t *)ctx;
    d->finishes++;
    return d->finish_nrc;
}

static void dl_abort(void *ctx)
{
    fake_download_t *d = (fake_download_t *)ctx;
    d->aborts++;
}

typedef struct
{
    uds_server_t    s;
    fake_flash_t    flash;
    fake_verifier_t ver;
    fake_download_t dl;
    uint8_t         resp[128];
    uint16_t        rlen;
} fix_t;

static const uint8_t k_uid[UDS_UID_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C
};

static void boot_clean(fix_t *f, uint32_t now)
{
    memset(f, 0, sizeof(*f));
    Uds_SetSecurityStore(&f->s, flash_load, flash_store, &f->flash);
    Uds_SetVerifier(&f->s, verify_reversed, &f->ver);
    Uds_SetDidReader(&f->s, read_did, NULL);
    Uds_Init(&f->s, k_uid, now);
}

/* Power cycle: the server restarts, the flash contents do not. */
static void reboot(fix_t *f, uint32_t now)
{
    memset(&f->s, 0, sizeof(f->s));
    memset(f->resp, 0, sizeof(f->resp));
    Uds_SetSecurityStore(&f->s, flash_load, flash_store, &f->flash);
    Uds_SetVerifier(&f->s, verify_reversed, &f->ver);
    Uds_SetDidReader(&f->s, read_did, NULL);
    Uds_Init(&f->s, k_uid, now);
}

static void request(fix_t *f, const uint8_t *req, uint16_t len, uint32_t now)
{
    f->rlen = Uds_HandleRequest(&f->s, req, len, f->resp, sizeof(f->resp), now);
}

static bool is_negative(const fix_t *f, uint8_t sid, uint8_t nrc)
{
    return (f->rlen == 3U) && (f->resp[0] == UDS_NEGATIVE_RESPONSE) &&
           (f->resp[1] == sid) && (f->resp[2] == nrc);
}

static void enter_session(fix_t *f, uint8_t session, uint32_t now)
{
    uint8_t req[2] = { UDS_SID_SESSION_CONTROL, 0 };
    req[1] = session;
    request(f, req, 2, now);
}

static void unlock(fix_t *f, uint32_t now)
{
    uint8_t req[2 + UDS_CHALLENGE_LEN];
    uint8_t key[2 + UDS_CHALLENGE_LEN];
    uint16_t i;

    req[0] = UDS_SID_SECURITY_ACCESS;
    req[1] = UDS_SEC_LEVEL_PROGRAMMING;
    request(f, req, 2, now);

    key[0] = UDS_SID_SECURITY_ACCESS;
    key[1] = UDS_SEC_LEVEL_PROGRAMMING + 1;
    for (i = 0; i < UDS_CHALLENGE_LEN; i++) {
        key[2 + i] = f->resp[2 + UDS_CHALLENGE_LEN - 1 - i];
    }
    request(f, key, (uint16_t)(2 + UDS_CHALLENGE_LEN), now);
}

static void bad_key(fix_t *f, uint32_t now)
{
    uint8_t req[2] = { UDS_SID_SECURITY_ACCESS, UDS_SEC_LEVEL_PROGRAMMING };
    uint8_t key[2 + UDS_CHALLENGE_LEN];

    request(f, req, 2, now);
    key[0] = UDS_SID_SECURITY_ACCESS;
    key[1] = UDS_SEC_LEVEL_PROGRAMMING + 1;
    memset(&key[2], 0xEE, UDS_CHALLENGE_LEN);
    request(f, key, (uint16_t)(2 + UDS_CHALLENGE_LEN), now);
}

static void test_starts_in_default_session_locked(void)
{
    fix_t f;
    boot_clean(&f, 0);

    CHECK_EQ(Uds_Session(&f.s), UDS_SESSION_DEFAULT);
    CHECK(!Uds_IsUnlocked(&f.s));
}

static void test_unknown_service_is_rejected(void)
{
    fix_t f;
    uint8_t req[2] = { 0xAB, 0x00 };

    boot_clean(&f, 0);
    request(&f, req, 2, 0);
    CHECK(is_negative(&f, 0xAB, UDS_NRC_SERVICE_NOT_SUPPORTED));
}

static void test_session_change_and_positive_response(void)
{
    fix_t f;
    boot_clean(&f, 0);

    enter_session(&f, UDS_SESSION_EXTENDED, 0);
    CHECK_EQ(f.resp[0], UDS_POSITIVE_RESPONSE(UDS_SID_SESSION_CONTROL));
    CHECK_EQ(f.resp[1], UDS_SESSION_EXTENDED);
    CHECK_EQ(f.rlen, 6);
    CHECK_EQ(Uds_Session(&f.s), UDS_SESSION_EXTENDED);
}

static void test_unknown_session_is_rejected(void)
{
    fix_t f;
    uint8_t req[2] = { UDS_SID_SESSION_CONTROL, 0x7A };

    boot_clean(&f, 0);
    request(&f, req, 2, 0);
    CHECK(is_negative(&f, UDS_SID_SESSION_CONTROL,
                      UDS_NRC_SUBFUNCTION_NOT_SUPPORTED));
}

static void test_wrong_length_is_rejected(void)
{
    fix_t f;
    uint8_t req[3] = { UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED, 0x00 };

    boot_clean(&f, 0);
    request(&f, req, 3, 0);
    CHECK(is_negative(&f, UDS_SID_SESSION_CONTROL, UDS_NRC_INVALID_LENGTH));
}

static void test_suppress_positive_response_bit(void)
{
    fix_t f;
    uint8_t req[2] = { UDS_SID_TESTER_PRESENT, 0x80 };

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_EXTENDED, 0);

    /* Suppressed positive response: keeps the session alive silently. */
    request(&f, req, 2, 100);
    CHECK_EQ(f.rlen, 0);
    CHECK_EQ(Uds_Session(&f.s), UDS_SESSION_EXTENDED);
}

static void test_session_times_out_to_default(void)
{
    fix_t f;
    boot_clean(&f, 0);

    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    unlock(&f, 10);
    CHECK(Uds_IsUnlocked(&f.s));

    Uds_Poll(&f.s, 10 + UDS_S3_TIMEOUT_MS - 1);
    CHECK_EQ(Uds_Session(&f.s), UDS_SESSION_PROGRAMMING);

    Uds_Poll(&f.s, 10 + UDS_S3_TIMEOUT_MS);
    CHECK_EQ(Uds_Session(&f.s), UDS_SESSION_DEFAULT);
    CHECK(!Uds_IsUnlocked(&f.s));
}

static void test_tester_present_keeps_session_alive(void)
{
    fix_t f;
    uint8_t tp[2] = { UDS_SID_TESTER_PRESENT, 0x80 };
    uint32_t t;

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_EXTENDED, 0);

    for (t = 1000; t <= 20000; t += 1000) {
        request(&f, tp, 2, t);
        Uds_Poll(&f.s, t);
    }
    CHECK_EQ(Uds_Session(&f.s), UDS_SESSION_EXTENDED);
}

static void test_read_data_by_identifier(void)
{
    fix_t f;
    uint8_t req[3] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    uint8_t bad[3] = { UDS_SID_READ_DATA_BY_ID, 0xDE, 0xAD };

    boot_clean(&f, 0);

    request(&f, req, 3, 0);
    CHECK_EQ(f.resp[0], UDS_POSITIVE_RESPONSE(UDS_SID_READ_DATA_BY_ID));
    CHECK_EQ(f.rlen, 7);
    CHECK_EQ(f.resp[3], 'B');

    request(&f, bad, 3, 0);
    CHECK(is_negative(&f, UDS_SID_READ_DATA_BY_ID,
                      UDS_NRC_REQUEST_OUT_OF_RANGE));
}

static void test_security_access_unreachable_in_default_session(void)
{
    fix_t f;
    uint8_t req[2] = { UDS_SID_SECURITY_ACCESS, UDS_SEC_LEVEL_PROGRAMMING };

    boot_clean(&f, 0);

    /* Anyone can put a frame on this bus. */
    request(&f, req, 2, 0);
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS,
                      UDS_NRC_SERVICE_NOT_IN_SESSION));
    CHECK_EQ(f.s.stat_auth_failures, 0);
}

static void test_programming_services_denied_while_locked(void)
{
    fix_t f;
    uint8_t dl[4] = { UDS_SID_REQUEST_DOWNLOAD, 0x00, 0x44, 0x00 };

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);

    request(&f, dl, 4, 0);
    CHECK(is_negative(&f, UDS_SID_REQUEST_DOWNLOAD,
                      UDS_NRC_SECURITY_ACCESS_DENIED));
}

static void test_successful_unlock(void)
{
    fix_t f;
    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);

    unlock(&f, 10);
    CHECK_EQ(f.rlen, 2);
    CHECK_EQ(f.resp[0], UDS_POSITIVE_RESPONSE(UDS_SID_SECURITY_ACCESS));
    CHECK(Uds_IsUnlocked(&f.s));
    CHECK_EQ(f.s.stat_unlocks, 1);
    CHECK_EQ(f.s.sec.failed_attempts, 0);
}

static void test_challenge_is_bound_to_this_device(void)
{
    fix_t f;
    uint8_t req[2] = { UDS_SID_SECURITY_ACCESS, UDS_SEC_LEVEL_PROGRAMMING };

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    request(&f, req, 2, 0);

    /* The UID is in the challenge, so a response cannot be replayed on another
     * ECU. */
    CHECK_EQ(f.rlen, 2 + UDS_CHALLENGE_LEN);
    CHECK_EQ(memcmp(&f.resp[2 + 4], k_uid, UDS_UID_LEN), 0);

    CHECK_EQ(f.resp[2 + 4 + UDS_UID_LEN], UDS_SEC_LEVEL_PROGRAMMING);
}

static void test_challenge_never_repeats(void)
{
    fix_t f;
    uint8_t req[2] = { UDS_SID_SECURITY_ACCESS, UDS_SEC_LEVEL_PROGRAMMING };
    uint8_t first[UDS_CHALLENGE_LEN];

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);

    request(&f, req, 2, 0);
    memcpy(first, &f.resp[2], UDS_CHALLENGE_LEN);

    unlock(&f, 10);
    CHECK(Uds_IsUnlocked(&f.s));

    /* Without the counter advancing, a captured response would work forever. */
    enter_session(&f, UDS_SESSION_DEFAULT, 20);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 30);
    request(&f, req, 2, 30);

    CHECK(memcmp(first, &f.resp[2], UDS_CHALLENGE_LEN) != 0);
}

static void test_replaying_a_captured_key_fails(void)
{
    fix_t f;
    uint8_t req[2] = { UDS_SID_SECURITY_ACCESS, UDS_SEC_LEVEL_PROGRAMMING };
    uint8_t captured[2 + UDS_CHALLENGE_LEN];
    uint16_t i;

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);

    request(&f, req, 2, 0);
    captured[0] = UDS_SID_SECURITY_ACCESS;
    captured[1] = UDS_SEC_LEVEL_PROGRAMMING + 1;
    for (i = 0; i < UDS_CHALLENGE_LEN; i++) {
        captured[2 + i] = f.resp[2 + UDS_CHALLENGE_LEN - 1 - i];
    }
    request(&f, captured, (uint16_t)(2 + UDS_CHALLENGE_LEN), 0);
    CHECK(Uds_IsUnlocked(&f.s));

    /* Replay on a new session: the challenge it answers no longer exists. */
    enter_session(&f, UDS_SESSION_DEFAULT, 100);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 110);
    request(&f, req, 2, 110);
    request(&f, captured, (uint16_t)(2 + UDS_CHALLENGE_LEN), 110);

    CHECK(!Uds_IsUnlocked(&f.s));
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_KEY));
}

static void test_key_without_seed_is_a_sequence_error(void)
{
    fix_t f;
    uint8_t key[2 + UDS_CHALLENGE_LEN];

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);

    key[0] = UDS_SID_SECURITY_ACCESS;
    key[1] = UDS_SEC_LEVEL_PROGRAMMING + 1;
    memset(&key[2], 0x11, UDS_CHALLENGE_LEN);

    request(&f, key, (uint16_t)(2 + UDS_CHALLENGE_LEN), 0);
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS,
                      UDS_NRC_REQUEST_SEQUENCE_ERROR));
    CHECK_EQ(f.s.stat_auth_failures, 0);
}

static void test_one_challenge_permits_only_one_guess(void)
{
    fix_t f;
    uint8_t req[2] = { UDS_SID_SECURITY_ACCESS, UDS_SEC_LEVEL_PROGRAMMING };
    uint8_t key[2 + UDS_CHALLENGE_LEN];

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    request(&f, req, 2, 0);

    key[0] = UDS_SID_SECURITY_ACCESS;
    key[1] = UDS_SEC_LEVEL_PROGRAMMING + 1;
    memset(&key[2], 0xEE, UDS_CHALLENGE_LEN);

    request(&f, key, (uint16_t)(2 + UDS_CHALLENGE_LEN), 0);
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_KEY));

    /* Consumed by the failure, so guesses cannot be ground against it. */
    request(&f, key, (uint16_t)(2 + UDS_CHALLENGE_LEN), 0);
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS,
                      UDS_NRC_REQUEST_SEQUENCE_ERROR));
}

static void test_attempts_are_limited(void)
{
    fix_t f;
    unsigned i;

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);

    for (i = 0; i < UDS_SEC_MAX_ATTEMPTS - 1U; i++) {
        bad_key(&f, 0);
        CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_KEY));
    }

    bad_key(&f, 0);
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS, UDS_NRC_EXCEEDED_ATTEMPTS));
    CHECK_EQ(f.s.stat_lockouts, 1);
}

static void test_lockout_refuses_further_attempts(void)
{
    fix_t f;
    uint8_t req[2] = { UDS_SID_SECURITY_ACCESS, UDS_SEC_LEVEL_PROGRAMMING };
    unsigned i;

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    for (i = 0; i < UDS_SEC_MAX_ATTEMPTS; i++) {
        bad_key(&f, 0);
    }

    request(&f, req, 2, 100);
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS,
                      UDS_NRC_TIME_DELAY_NOT_EXPIRED));
}

static void test_power_cycle_does_not_clear_the_attempt_count(void)
{
    fix_t f;
    uint8_t req[2] = { UDS_SID_SECURITY_ACCESS, UDS_SEC_LEVEL_PROGRAMMING };
    unsigned i;

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    for (i = 0; i < UDS_SEC_MAX_ATTEMPTS; i++) {
        bad_key(&f, 0);
    }
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS, UDS_NRC_EXCEEDED_ATTEMPTS));

    /* The tick restarts at zero, which is why the count is persisted, not a
     * deadline. */
    reboot(&f, 0);
    CHECK_EQ(f.s.sec.failed_attempts, UDS_SEC_MAX_ATTEMPTS);

    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    request(&f, req, 2, 0);
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS,
                      UDS_NRC_TIME_DELAY_NOT_EXPIRED));

    /* The lockout outlives the S3 timeout by design, so the session is
     * re-entered first. */
    enter_session(&f, UDS_SESSION_PROGRAMMING, UDS_SEC_LOCKOUT_MS - 1);
    request(&f, req, 2, UDS_SEC_LOCKOUT_MS - 1);
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS,
                      UDS_NRC_TIME_DELAY_NOT_EXPIRED));
}

static void test_lockout_clears_after_the_delay(void)
{
    fix_t f;
    unsigned i;

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    for (i = 0; i < UDS_SEC_MAX_ATTEMPTS; i++) {
        bad_key(&f, 0);
    }

    /* After the delay a mistyping tester can continue: rate limiting, not
     * permanent denial. */
    enter_session(&f, UDS_SESSION_PROGRAMMING, UDS_SEC_LOCKOUT_MS);
    unlock(&f, UDS_SEC_LOCKOUT_MS);
    CHECK(Uds_IsUnlocked(&f.s));
    CHECK_EQ(f.s.sec.failed_attempts, 0);
}

static void test_unlock_is_dropped_on_session_change(void)
{
    fix_t f;
    boot_clean(&f, 0);

    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    unlock(&f, 10);
    CHECK(Uds_IsUnlocked(&f.s));

    /* Authorisation earned in one session must not be spent in another. */
    enter_session(&f, UDS_SESSION_EXTENDED, 20);
    CHECK(!Uds_IsUnlocked(&f.s));
}

static void test_unlock_survives_within_the_same_session(void)
{
    fix_t f;
    uint8_t tp[2] = { UDS_SID_TESTER_PRESENT, 0x80 };

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    unlock(&f, 10);

    request(&f, tp, 2, 20);
    CHECK(Uds_IsUnlocked(&f.s));
}

static void test_no_verifier_means_no_unlock(void)
{
    fix_t f;

    boot_clean(&f, 0);
    Uds_SetVerifier(&f.s, NULL, NULL);

    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    unlock(&f, 10);

    /* No verifier: refuse everyone. */
    CHECK(!Uds_IsUnlocked(&f.s));
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_KEY));
}

static void test_oversized_key_is_rejected_before_verification(void)
{
    fix_t f;
    uint8_t req[2] = { UDS_SID_SECURITY_ACCESS, UDS_SEC_LEVEL_PROGRAMMING };
    static uint8_t key[2 + UDS_MAX_KEY_LEN + 8];

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    request(&f, req, 2, 0);

    key[0] = UDS_SID_SECURITY_ACCESS;
    key[1] = UDS_SEC_LEVEL_PROGRAMMING + 1;
    memset(&key[2], 0x5A, UDS_MAX_KEY_LEN + 8);

    request(&f, key, (uint16_t)(2 + UDS_MAX_KEY_LEN + 8), 0);
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_LENGTH));
    CHECK_EQ(f.ver.calls, 0);
}

static void test_unsupported_security_level_is_rejected(void)
{
    fix_t f;
    uint8_t req[2] = { UDS_SID_SECURITY_ACCESS, 0x09 };

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    request(&f, req, 2, 0);
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS,
                      UDS_NRC_SUBFUNCTION_NOT_SUPPORTED));
}

static void test_security_state_is_persisted_on_every_change(void)
{
    fix_t f;

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);

    CHECK_EQ(f.flash.stores, 0);
    bad_key(&f, 0);
    CHECK(f.flash.stores > 0);

    /* Stored on failure, or cutting power after each guess never accumulates a
     * count. */
    CHECK_EQ(f.flash.rec.failed_attempts, 1);
}

static void test_counter_advances_on_failure_too(void)
{
    fix_t f;
    uint32_t before;

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);

    before = f.s.sec.challenge_counter;
    bad_key(&f, 0);

    /* Or the same challenge would be available for the next guess. */
    CHECK(f.s.sec.challenge_counter > before);
}


static void ready_to_program(fix_t *f)
{
    uds_download_t dl;

    boot_clean(f, 0);

    dl.begin    = dl_begin;
    dl.transfer = dl_transfer;
    dl.finish   = dl_finish;
    dl.abort    = dl_abort;
    dl.ctx      = &f->dl;
    Uds_SetDownload(&f->s, &dl);

    f->dl.max_block = 64;

    enter_session(f, UDS_SESSION_PROGRAMMING, 0);
    unlock(f, 10);
    CHECK(Uds_IsUnlocked(&f->s));
}

static void request_download(fix_t *f, uint32_t addr, uint32_t size, uint32_t now)
{
    uint8_t req[11];

    req[0]  = UDS_SID_REQUEST_DOWNLOAD;
    req[1]  = 0x00;          /* no compression, no encryption */
    req[2]  = 0x44;
    req[3]  = (uint8_t)(addr >> 24); req[4] = (uint8_t)(addr >> 16);
    req[5]  = (uint8_t)(addr >> 8);  req[6] = (uint8_t)(addr);
    req[7]  = (uint8_t)(size >> 24); req[8] = (uint8_t)(size >> 16);
    req[9]  = (uint8_t)(size >> 8);  req[10] = (uint8_t)(size);
    request(f, req, 11, now);
}

static void transfer_block(fix_t *f, uint8_t counter, uint16_t n, uint32_t now)
{
    uint8_t req[2 + 64];
    uint16_t i;

    req[0] = UDS_SID_TRANSFER_DATA;
    req[1] = counter;
    for (i = 0; i < n; i++) {
        req[2 + i] = (uint8_t)i;
    }
    request(f, req, (uint16_t)(2 + n), now);
}

static void transfer_exit(fix_t *f, uint32_t now)
{
    uint8_t req[1] = { UDS_SID_TRANSFER_EXIT };
    request(f, req, 1, now);
}

static void test_transfer_services_need_security(void)
{
    fix_t f;
    uds_download_t dl;

    boot_clean(&f, 0);
    dl.begin = dl_begin; dl.transfer = dl_transfer;
    dl.finish = dl_finish; dl.abort = dl_abort; dl.ctx = &f.dl;
    Uds_SetDownload(&f.s, &dl);

    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    request_download(&f, 0x08020000, 1024, 0);

    CHECK(is_negative(&f, UDS_SID_REQUEST_DOWNLOAD,
                      UDS_NRC_SECURITY_ACCESS_DENIED));
    CHECK_EQ(f.dl.begins, 0);
}

static void test_full_download_sequence(void)
{
    fix_t f;

    ready_to_program(&f);

    request_download(&f, 0x08040000, 128, 20);
    CHECK_EQ(f.resp[0], UDS_POSITIVE_RESPONSE(UDS_SID_REQUEST_DOWNLOAD));
    CHECK_EQ(f.rlen, 4);
    CHECK_EQ(f.resp[1], 0x20);
    CHECK_EQ((f.resp[2] << 8) | f.resp[3], 64);
    CHECK_EQ(f.dl.begins, 1);
    CHECK_EQ(f.dl.last_addr, 0x08040000);
    CHECK_EQ(f.dl.last_size, 128);
    CHECK(Uds_DownloadActive(&f.s));

    transfer_block(&f, 1, 64, 30);
    CHECK_EQ(f.resp[0], UDS_POSITIVE_RESPONSE(UDS_SID_TRANSFER_DATA));
    CHECK_EQ(f.resp[1], 1);
    transfer_block(&f, 2, 64, 40);
    CHECK_EQ(f.resp[1], 2);
    CHECK_EQ(f.dl.total_bytes, 128);

    transfer_exit(&f, 50);
    CHECK_EQ(f.resp[0], UDS_POSITIVE_RESPONSE(UDS_SID_TRANSFER_EXIT));
    CHECK_EQ(f.rlen, 1);
    CHECK_EQ(f.dl.finishes, 1);
    CHECK(!Uds_DownloadActive(&f.s));
}

static void test_length_format_nibbles_are_validated(void)
{
    fix_t f;
    uint8_t req[11];

    ready_to_program(&f);

    /* 15 address bytes and 15 size bytes claimed in an 11-byte request. */
    memset(req, 0, sizeof(req));
    req[0] = UDS_SID_REQUEST_DOWNLOAD;
    req[1] = 0x00;
    req[2] = 0xFF;
    request(&f, req, 11, 20);
    CHECK(is_negative(&f, UDS_SID_REQUEST_DOWNLOAD,
                      UDS_NRC_REQUEST_OUT_OF_RANGE));
    CHECK_EQ(f.dl.begins, 0);

    req[2] = 0x00;
    request(&f, req, 11, 20);
    CHECK(is_negative(&f, UDS_SID_REQUEST_DOWNLOAD,
                      UDS_NRC_REQUEST_OUT_OF_RANGE));
    CHECK_EQ(f.dl.begins, 0);
}

static void test_declared_field_widths_must_match_the_request(void)
{
    fix_t f;
    uint8_t req[11];

    ready_to_program(&f);

    /* Four address and four size bytes declared, six sent. */
    memset(req, 0, sizeof(req));
    req[0] = UDS_SID_REQUEST_DOWNLOAD;
    req[1] = 0x00;
    req[2] = 0x44;
    request(&f, req, 7, 20);
    CHECK(is_negative(&f, UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_INVALID_LENGTH));
    CHECK_EQ(f.dl.begins, 0);
}

static void test_compression_and_encryption_are_refused(void)
{
    fix_t f;
    uint8_t req[11];

    ready_to_program(&f);

    memset(req, 0, sizeof(req));
    req[0] = UDS_SID_REQUEST_DOWNLOAD;
    req[1] = 0x11;
    req[2] = 0x44;
    request(&f, req, 11, 20);

    /* Compressed bytes in a slot would fail verification for no visible reason. */
    CHECK(is_negative(&f, UDS_SID_REQUEST_DOWNLOAD,
                      UDS_NRC_REQUEST_OUT_OF_RANGE));
    CHECK_EQ(f.dl.begins, 0);
}

static void test_backend_refusal_is_reported_verbatim(void)
{
    fix_t f;

    ready_to_program(&f);

    /* The backend's reason is passed through, not flattened. */
    f.dl.begin_nrc = UDS_NRC_REQUEST_OUT_OF_RANGE;
    request_download(&f, 0x08000000, 1024, 20);
    CHECK(is_negative(&f, UDS_SID_REQUEST_DOWNLOAD,
                      UDS_NRC_REQUEST_OUT_OF_RANGE));
    CHECK(!Uds_DownloadActive(&f.s));
}

static void test_transfer_without_request_download(void)
{
    fix_t f;

    ready_to_program(&f);
    transfer_block(&f, 1, 8, 20);
    CHECK(is_negative(&f, UDS_SID_TRANSFER_DATA,
                      UDS_NRC_REQUEST_SEQUENCE_ERROR));
    CHECK_EQ(f.dl.transfers, 0);

    transfer_exit(&f, 20);
    CHECK(is_negative(&f, UDS_SID_TRANSFER_EXIT,
                      UDS_NRC_REQUEST_SEQUENCE_ERROR));
}

static void test_out_of_order_block_is_refused(void)
{
    fix_t f;

    ready_to_program(&f);
    request_download(&f, 0x08040000, 256, 20);

    transfer_block(&f, 1, 32, 30);
    CHECK_EQ(f.resp[0], UDS_POSITIVE_RESPONSE(UDS_SID_TRANSFER_DATA));

    /* A lost block would leave a hole in the image. */
    transfer_block(&f, 3, 32, 40);
    CHECK(is_negative(&f, UDS_SID_TRANSFER_DATA,
                      UDS_NRC_WRONG_BLOCK_COUNTER));
    CHECK_EQ(f.dl.transfers, 1);
}

/* A retransmission (our response was lost): answered positively, not written
 * again. */
static void test_repeated_block_is_absorbed_not_rewritten(void)
{
    fix_t f;

    ready_to_program(&f);
    request_download(&f, 0x08040000, 256, 20);

    transfer_block(&f, 1, 32, 30);
    CHECK_EQ(f.dl.transfers, 1);
    CHECK_EQ(f.dl.total_bytes, 32);

    transfer_block(&f, 1, 32, 40);
    CHECK_EQ(f.resp[0], UDS_POSITIVE_RESPONSE(UDS_SID_TRANSFER_DATA));
    CHECK_EQ(f.resp[1], 1);
    CHECK_EQ(f.dl.transfers, 1);
    CHECK_EQ(f.dl.total_bytes, 32);
    CHECK_EQ(f.s.stat_block_repeats, 1);

    transfer_block(&f, 2, 32, 50);
    CHECK_EQ(f.dl.transfers, 2);
}

static void test_block_counter_wraps_through_zero(void)
{
    fix_t f;
    unsigned i;

    ready_to_program(&f);
    request_download(&f, 0x08040000, 64 * 300, 20);

    /* 0x00 follows 0xFF; images over 255 blocks are ordinary. */
    for (i = 1; i <= 300; i++) {
        uint8_t counter = (uint8_t)(i & 0xFF);
        transfer_block(&f, counter, 8, (uint32_t)(30 + i));
        CHECK_EQ(f.resp[0], UDS_POSITIVE_RESPONSE(UDS_SID_TRANSFER_DATA));
    }
    CHECK_EQ(f.dl.transfers, 300);
}

static void test_block_larger_than_advertised_is_refused(void)
{
    fix_t f;
    uint8_t req[2 + 100];

    ready_to_program(&f);
    request_download(&f, 0x08040000, 1024, 20);

    memset(req, 0xAA, sizeof(req));
    req[0] = UDS_SID_TRANSFER_DATA;
    req[1] = 1;

    /* The ECU advertised 64 bytes. */
    request(&f, req, 2 + 100, 30);
    CHECK(is_negative(&f, UDS_SID_TRANSFER_DATA, UDS_NRC_INVALID_LENGTH));
    CHECK_EQ(f.dl.transfers, 0);
}

static void test_transfer_failure_aborts_the_download(void)
{
    fix_t f;

    ready_to_program(&f);
    request_download(&f, 0x08040000, 256, 20);

    f.dl.transfer_nrc = UDS_NRC_PROGRAMMING_FAILURE;
    transfer_block(&f, 1, 32, 30);
    CHECK(is_negative(&f, UDS_SID_TRANSFER_DATA,
                      UDS_NRC_PROGRAMMING_FAILURE));

    /* A failed write closes the transfer. */
    CHECK(!Uds_DownloadActive(&f.s));
    CHECK_EQ(f.dl.aborts, 1);
}

static void test_finish_failure_is_reported(void)
{
    fix_t f;

    ready_to_program(&f);
    request_download(&f, 0x08040000, 64, 20);
    transfer_block(&f, 1, 32, 30);

    f.dl.finish_nrc = UDS_NRC_PROGRAMMING_FAILURE;
    transfer_exit(&f, 40);
    CHECK(is_negative(&f, UDS_SID_TRANSFER_EXIT,
                      UDS_NRC_PROGRAMMING_FAILURE));
    CHECK(!Uds_DownloadActive(&f.s));
}

static void test_session_change_aborts_a_download(void)
{
    fix_t f;

    ready_to_program(&f);
    request_download(&f, 0x08040000, 256, 20);
    transfer_block(&f, 1, 32, 30);
    CHECK(Uds_DownloadActive(&f.s));

    /* Losing authorisation closes the transfer it authorised. */
    enter_session(&f, UDS_SESSION_EXTENDED, 40);
    CHECK(!Uds_DownloadActive(&f.s));
    CHECK_EQ(f.dl.aborts, 1);
}

static void test_session_timeout_aborts_a_download(void)
{
    fix_t f;

    ready_to_program(&f);
    request_download(&f, 0x08040000, 256, 20);
    CHECK(Uds_DownloadActive(&f.s));

    Uds_Poll(&f.s, 20 + UDS_S3_TIMEOUT_MS);
    CHECK(!Uds_DownloadActive(&f.s));
    CHECK_EQ(f.dl.aborts, 1);
}

static void test_no_backend_means_no_download(void)
{
    fix_t f;

    boot_clean(&f, 0);
    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    unlock(&f, 10);

    /* Rather than accepting a transfer it would silently discard. */
    request_download(&f, 0x08040000, 256, 20);
    CHECK(is_negative(&f, UDS_SID_REQUEST_DOWNLOAD,
                      UDS_NRC_UPLOAD_DOWNLOAD_DENIED));
}

static void test_second_request_download_supersedes_the_first(void)
{
    fix_t f;

    ready_to_program(&f);
    request_download(&f, 0x08040000, 256, 20);
    transfer_block(&f, 1, 32, 30);

    request_download(&f, 0x08040000, 512, 40);
    CHECK_EQ(f.resp[0], UDS_POSITIVE_RESPONSE(UDS_SID_REQUEST_DOWNLOAD));
    CHECK_EQ(f.dl.aborts, 1);
    CHECK_EQ(f.dl.begins, 2);

    /* The counter restarts, so a stale block number is refused. */
    transfer_block(&f, 2, 32, 50);
    CHECK(is_negative(&f, UDS_SID_TRANSFER_DATA,
                      UDS_NRC_WRONG_BLOCK_COUNTER));
}

int main(void)
{
    test_starts_in_default_session_locked();
    test_unknown_service_is_rejected();
    test_session_change_and_positive_response();
    test_unknown_session_is_rejected();
    test_wrong_length_is_rejected();
    test_suppress_positive_response_bit();
    test_session_times_out_to_default();
    test_tester_present_keeps_session_alive();
    test_read_data_by_identifier();

    test_security_access_unreachable_in_default_session();
    test_programming_services_denied_while_locked();
    test_successful_unlock();
    test_challenge_is_bound_to_this_device();
    test_challenge_never_repeats();
    test_replaying_a_captured_key_fails();
    test_key_without_seed_is_a_sequence_error();
    test_one_challenge_permits_only_one_guess();
    test_attempts_are_limited();
    test_lockout_refuses_further_attempts();
    test_power_cycle_does_not_clear_the_attempt_count();
    test_lockout_clears_after_the_delay();
    test_unlock_is_dropped_on_session_change();
    test_unlock_survives_within_the_same_session();
    test_no_verifier_means_no_unlock();
    test_oversized_key_is_rejected_before_verification();
    test_unsupported_security_level_is_rejected();
    test_security_state_is_persisted_on_every_change();
    test_counter_advances_on_failure_too();

    test_transfer_services_need_security();
    test_full_download_sequence();
    test_length_format_nibbles_are_validated();
    test_declared_field_widths_must_match_the_request();
    test_compression_and_encryption_are_refused();
    test_backend_refusal_is_reported_verbatim();
    test_transfer_without_request_download();
    test_out_of_order_block_is_refused();
    test_repeated_block_is_absorbed_not_rewritten();
    test_block_counter_wraps_through_zero();
    test_block_larger_than_advertised_is_refused();
    test_transfer_failure_aborts_the_download();
    test_finish_failure_is_reported();
    test_session_change_aborts_a_download();
    test_session_timeout_aborts_a_download();
    test_no_backend_means_no_download();
    test_second_request_download_supersedes_the_first();

    return test_report();
}
