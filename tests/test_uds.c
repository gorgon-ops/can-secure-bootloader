/**
 * Host-side tests for the UDS server.
 *
 * The security access tests are the point of this file. The conventional
 * implementation of that service fails in specific, well-known ways, and the
 * tests below are written to prove this one does not: authorisation cannot be
 * reached from the default session, a captured exchange cannot be replayed, a
 * key cannot be ground against one challenge, and — the classic bypass —
 * power-cycling does not clear the failed-attempt count.
 *
 * Builds with plain gcc. No HAL, no hardware, no crypto: the verifier is
 * injected, so the state machine is tested independently of the algorithm.
 */
#include "test_util.h"

#include "uds.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Test doubles                                                        */
/* ------------------------------------------------------------------ */

/** Stands in for the flash sector holding the security record. Survives a
    simulated reset because the test owns it, not the server. */
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

/** A verifier that accepts one specific "signature": the challenge reversed.
    Standing in for a real signature check, it is enough to prove the state
    machine only accepts a response bound to the challenge it issued. */
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

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    uds_server_t    s;
    fake_flash_t    flash;
    fake_verifier_t ver;
    uint8_t         resp[128];
    uint16_t        rlen;
} fix_t;

static const uint8_t k_uid[UDS_UID_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C
};

/** Fresh board: wipes the simulated flash too. */
static void boot_clean(fix_t *f, uint32_t now)
{
    memset(f, 0, sizeof(*f));
    Uds_SetSecurityStore(&f->s, flash_load, flash_store, &f->flash);
    Uds_SetVerifier(&f->s, verify_reversed, &f->ver);
    Uds_SetDidReader(&f->s, read_did, NULL);
    Uds_Init(&f->s, k_uid, now);
}

/** Simulated power cycle: the server restarts, the flash contents do not. */
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

/** Request a seed and answer it correctly. */
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

/** Send a deliberately wrong key. */
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

/* ------------------------------------------------------------------ */
/* Sessions and dispatch                                               */
/* ------------------------------------------------------------------ */

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

    /* The usual form of TesterPresent: keeps the session alive and says
       nothing, so it does not consume bus bandwidth every few seconds. */
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

    /* A tester that walks away must not leave the ECU authorised. */
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

/* ------------------------------------------------------------------ */
/* Security access                                                     */
/* ------------------------------------------------------------------ */

static void test_security_access_unreachable_in_default_session(void)
{
    fix_t f;
    uint8_t req[2] = { UDS_SID_SECURITY_ACCESS, UDS_SEC_LEVEL_PROGRAMMING };

    boot_clean(&f, 0);

    /* Anyone can put a frame on this bus. Leaving the authorisation surface
       reachable without even a session request widens it for no benefit. */
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

    /* The device identifier appears in the challenge, so a response captured
       from one ECU cannot be replayed against a different one. */
    CHECK_EQ(f.rlen, 2 + UDS_CHALLENGE_LEN);
    CHECK_EQ(memcmp(&f.resp[2 + 4], k_uid, UDS_UID_LEN), 0);

    /* And the level being requested is covered too. */
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

    /* A fresh session, a fresh challenge. Without the counter advancing, a
       captured response would unlock the ECU forever. */
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

    /* Capture a genuine, successful exchange. */
    request(&f, req, 2, 0);
    captured[0] = UDS_SID_SECURITY_ACCESS;
    captured[1] = UDS_SEC_LEVEL_PROGRAMMING + 1;
    for (i = 0; i < UDS_CHALLENGE_LEN; i++) {
        captured[2 + i] = f.resp[2 + UDS_CHALLENGE_LEN - 1 - i];
    }
    request(&f, captured, (uint16_t)(2 + UDS_CHALLENGE_LEN), 0);
    CHECK(Uds_IsUnlocked(&f.s));

    /* Now replay it on a new session. The counter has moved, so the challenge
       it answers no longer exists. */
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

    /* The challenge is consumed by the failure. Leaving it open would let an
       attacker grind guesses against a single challenge, which is exactly what
       the counter exists to prevent. */
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

    /* Even a seed request is refused while the delay runs. */
    request(&f, req, 2, 100);
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS,
                      UDS_NRC_TIME_DELAY_NOT_EXPIRED));
}

/**
 * The headline test.
 *
 * The classic bypass of UDS attempt limiting is to power-cycle between
 * guesses, because the counter usually lives in RAM. Here it is persisted, and
 * the delay is re-imposed from boot whenever the stored count is already at
 * the limit -- so a reset buys the attacker nothing at all.
 */
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

    /* Power cycle. The tick restarts at zero, which is precisely why a
       persisted deadline would have been useless and the count is persisted
       instead. */
    reboot(&f, 0);
    CHECK_EQ(f.s.sec.failed_attempts, UDS_SEC_MAX_ATTEMPTS);

    enter_session(&f, UDS_SESSION_PROGRAMMING, 0);
    request(&f, req, 2, 0);
    CHECK(is_negative(&f, UDS_SID_SECURITY_ACCESS,
                      UDS_NRC_TIME_DELAY_NOT_EXPIRED));

    /* Still refused most of the way through the delay, measured from boot.
       The session has to be re-established first: the lockout deliberately
       outlives the S3 session timeout, so waiting one out always means
       re-entering the session. */
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

    /* After serving the delay a legitimate tester that mistyped must be able
       to continue: the intent is rate limiting, not permanent denial. The
       session lapsed while waiting -- the lockout is longer than S3 by design
       -- so it is re-entered first. */
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

    /* Failing closed matters: a build that forgot to provision a verifier must
       refuse everyone rather than admit everyone. */
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
    CHECK_EQ(f.ver.calls, 0);   /* never reached the verifier */
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
    CHECK(f.flash.stores > 0);   /* a failure must reach storage immediately */

    /* Storing only on success would let an attacker interrupt power after each
       failure and never accumulate a count. */
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

    /* Otherwise a failed attempt would leave the same challenge available for
       the next guess. */
    CHECK(f.s.sec.challenge_counter > before);
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

    return test_report();
}
