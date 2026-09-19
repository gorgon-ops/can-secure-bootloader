/**
 * Host-side tests for the boot-time verification layer.
 *
 * Two halves. SHA-256 is checked against the published FIPS vectors, because a
 * hash is only worth having if it is the same hash everyone else computes. The
 * image verifier is checked for the properties the threat model claims, with
 * particular attention to the *order* the checks run in — the length bound has
 * to hold even when the signature would have rejected the header anyway.
 *
 * Builds with plain gcc. No HAL, no hardware.
 */
#include "test_util.h"

#include "image.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* SHA-256 against the published vectors                               */
/* ------------------------------------------------------------------ */

static void hex(const uint8_t *d, size_t n, char *out)
{
    static const char *H = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2]     = H[d[i] >> 4];
        out[i * 2 + 1] = H[d[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

static void check_digest(const char *msg, const char *expect)
{
    uint8_t d[SHA256_DIGEST_LEN];
    char got[SHA256_DIGEST_LEN * 2 + 1];

    Sha256((const uint8_t *)msg, strlen(msg), d);
    hex(d, SHA256_DIGEST_LEN, got);
    CHECK(strcmp(got, expect) == 0);
    if (strcmp(got, expect) != 0) {
        printf("       message %.20s...\n       got    %s\n       want   %s\n",
               msg, got, expect);
    }
}

static void test_sha256_known_vectors(void)
{
    check_digest("",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_digest("abc",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check_digest("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                 "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

static void test_sha256_one_million_a(void)
{
    sha256_t ctx;
    uint8_t d[SHA256_DIGEST_LEN];
    char got[SHA256_DIGEST_LEN * 2 + 1];
    uint8_t block[1000];
    int i;

    /* The long vector, fed in chunks. This is the one that catches a broken
       length counter or a mishandled block boundary, which short messages
       happily pass. */
    memset(block, 'a', sizeof(block));
    Sha256_Init(&ctx);
    for (i = 0; i < 1000; i++) {
        Sha256_Update(&ctx, block, sizeof(block));
    }
    Sha256_Final(&ctx, d);

    hex(d, SHA256_DIGEST_LEN, got);
    CHECK(strcmp(got,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
}

static void test_sha256_streaming_matches_one_shot(void)
{
    uint8_t data[300];
    uint8_t a[SHA256_DIGEST_LEN], b[SHA256_DIGEST_LEN];
    sha256_t ctx;
    int i;
    unsigned split;

    for (i = 0; i < 300; i++) {
        data[i] = (uint8_t)(i * 7);
    }
    Sha256(data, sizeof(data), a);

    /* Every split point must give the same answer. Block-boundary handling is
       where streaming implementations usually go wrong, and a payload arriving
       in flash-page-sized pieces hits those boundaries constantly. */
    for (split = 1; split < sizeof(data); split++) {
        Sha256_Init(&ctx);
        Sha256_Update(&ctx, data, split);
        Sha256_Update(&ctx, &data[split], sizeof(data) - split);
        Sha256_Final(&ctx, b);
        CHECK(memcmp(a, b, SHA256_DIGEST_LEN) == 0);
    }
}

/**
 * Checks that the comparison returns the right answer. It does **not** verify
 * the constant-time property, and no functional test can: a short-circuiting
 * implementation returns exactly the same booleans. Timing it on a host is
 * unreliable too — the optimiser and the scheduler dominate the measurement.
 *
 * That property is therefore held by code review, not by this suite. Saying so
 * is better than implying the tests cover it; on the target it would be
 * demonstrated with a cycle counter, not asserted here.
 */
static void test_digest_compare(void)
{
    uint8_t a[SHA256_DIGEST_LEN], b[SHA256_DIGEST_LEN];

    memset(a, 0x5A, sizeof(a));
    memcpy(b, a, sizeof(b));
    CHECK(Sha256_Equal(a, b));

    b[SHA256_DIGEST_LEN - 1] ^= 0x01;   /* differs only in the last byte */
    CHECK(!Sha256_Equal(a, b));

    memcpy(b, a, sizeof(b));
    b[0] ^= 0x80;                       /* and only in the first */
    CHECK(!Sha256_Equal(a, b));
}

/* ------------------------------------------------------------------ */
/* Image verification                                                  */
/* ------------------------------------------------------------------ */

#define SLOT_SIZE 4096U

typedef struct
{
    uint8_t  data[SLOT_SIZE];
    uint32_t reads;
    uint32_t highest_offset;   /*!< furthest byte the verifier asked for */
    bool     fail_reads;
} fake_slot_t;

/** Reads are bounded here the way flash would be, and the furthest offset
    requested is recorded so a test can prove nothing past the slot was read. */
static bool slot_read(void *ctx, uint32_t offset, uint8_t *out, uint32_t len)
{
    fake_slot_t *s = (fake_slot_t *)ctx;

    s->reads++;
    if (s->fail_reads) {
        return false;
    }
    if (((uint64_t)offset + len) > SLOT_SIZE) {
        return false;   /* a real flash driver would fault or return garbage */
    }
    if ((offset + len) > s->highest_offset) {
        s->highest_offset = offset + len;
    }
    memcpy(out, &s->data[offset], len);
    return true;
}

/** Stands in for a real signature: accepts when the message hashes to a value
    the test planted. Enough to prove the verifier only accepts a header it was
    given a matching signature for, without implementing any curve arithmetic. */
typedef struct
{
    uint8_t expected[IMAGE_SIGNATURE_LEN];
    int     calls;
    bool    accept_anything;
} fake_signer_t;

static void sign(fake_signer_t *k, const uint8_t *msg, uint32_t len)
{
    uint8_t d[SHA256_DIGEST_LEN];
    Sha256(msg, len, d);
    memset(k->expected, 0, sizeof(k->expected));
    memcpy(k->expected, d, SHA256_DIGEST_LEN);
}

static bool fake_verify(void *ctx, const uint8_t *msg, uint32_t msg_len,
                        const uint8_t sig[IMAGE_SIGNATURE_LEN])
{
    fake_signer_t *k = (fake_signer_t *)ctx;
    uint8_t d[SHA256_DIGEST_LEN];

    k->calls++;
    if (k->accept_anything) {
        return true;
    }
    Sha256(msg, msg_len, d);
    return (memcmp(sig, d, SHA256_DIGEST_LEN) == 0);
}

typedef struct
{
    image_verifier_t v;
    fake_slot_t      slot;
    fake_signer_t    key;
} img_fix_t;

/**
 * Lay a valid image into the slot: payload, correct hash in the header, and a
 * signature over the signed region.
 */
static void build_image(img_fix_t *f, uint32_t version, uint32_t payload_len,
                        uint8_t fill)
{
    image_header_t h;
    uint8_t raw[IMAGE_HEADER_LEN];
    uint32_t i;

    memset(f, 0, sizeof(*f));

    for (i = 0; i < payload_len; i++) {
        f->slot.data[IMAGE_HEADER_LEN + i] = (uint8_t)(fill + (uint8_t)i);
    }

    memset(&h, 0, sizeof(h));
    h.magic          = IMAGE_MAGIC;
    h.header_version = IMAGE_HEADER_VERSION;
    h.image_version  = version;
    h.payload_length = payload_len;
    h.entry_offset   = IMAGE_HEADER_LEN;
    Sha256(&f->slot.data[IMAGE_HEADER_LEN], payload_len, h.payload_sha256);

    CHECK(ImageHeader_Serialise(&h, raw, sizeof(raw)));

    /* Sign the signed region, then place the signature. */
    sign(&f->key, raw, IMAGE_SIGNED_LEN);
    memcpy(&raw[IMAGE_SIGNED_LEN], f->key.expected, IMAGE_SIGNATURE_LEN);
    memcpy(f->slot.data, raw, IMAGE_HEADER_LEN);

    ImageVerify_SetReader(&f->v, slot_read, &f->slot);
    ImageVerify_SetVerifier(&f->v, fake_verify, &f->key);
    ImageVerify_Init(&f->v, SLOT_SIZE, 0U);
}

/** Overwrite a 32-bit field in the stored header without re-signing it. */
static void tamper32(img_fix_t *f, uint32_t offset, uint32_t value)
{
    f->slot.data[offset]     = (uint8_t)(value);
    f->slot.data[offset + 1] = (uint8_t)(value >> 8);
    f->slot.data[offset + 2] = (uint8_t)(value >> 16);
    f->slot.data[offset + 3] = (uint8_t)(value >> 24);
}

static void test_valid_image_is_accepted(void)
{
    img_fix_t f;
    build_image(&f, 5, 1000, 0x11);

    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_OK);
    CHECK_EQ(f.v.last_version, 5);
    CHECK_EQ(f.v.last_length, 1000);
}

static void test_header_round_trips(void)
{
    image_header_t a, b;
    uint8_t raw[IMAGE_HEADER_LEN];

    memset(&a, 0, sizeof(a));
    a.magic          = IMAGE_MAGIC;
    a.header_version = IMAGE_HEADER_VERSION;
    a.image_version  = 0xDEADBEEF;
    a.payload_length = 0x1234;
    a.entry_offset   = 0x80;
    memset(a.payload_sha256, 0xAB, sizeof(a.payload_sha256));
    memset(a.signature, 0xCD, sizeof(a.signature));

    CHECK(ImageHeader_Serialise(&a, raw, sizeof(raw)));
    CHECK(ImageHeader_Parse(raw, sizeof(raw), &b));

    CHECK_EQ(b.magic, a.magic);
    CHECK_EQ(b.image_version, a.image_version);
    CHECK_EQ(b.payload_length, a.payload_length);
    CHECK_EQ(b.entry_offset, a.entry_offset);
    CHECK(memcmp(b.payload_sha256, a.payload_sha256, SHA256_DIGEST_LEN) == 0);
    CHECK(memcmp(b.signature, a.signature, IMAGE_SIGNATURE_LEN) == 0);
}

static void test_truncated_header_is_rejected(void)
{
    image_header_t h;
    uint8_t raw[IMAGE_HEADER_LEN];

    CHECK(!ImageHeader_Parse(raw, IMAGE_HEADER_LEN - 1, &h));
    CHECK(!ImageHeader_Parse(NULL, IMAGE_HEADER_LEN, &h));
}

static void test_bad_magic_and_header_version(void)
{
    img_fix_t f;

    build_image(&f, 1, 256, 0x22);
    tamper32(&f, 0, 0x11223344);
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_BAD_MAGIC);

    build_image(&f, 1, 256, 0x22);
    tamper32(&f, 4, IMAGE_HEADER_VERSION + 99);
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_BAD_HEADER_VERSION);
}

/**
 * The ordering test, and the reason the length check comes first.
 *
 * A header claiming a payload far larger than the slot is rejected on the
 * length alone. The signature over that tampered header would have failed a
 * moment later, but by then the payload hash would already have read past the
 * end of the slot. The assertion that matters is not just the return code — it
 * is that nothing beyond the slot was ever requested, and that the verifier was
 * never even called.
 */
static void test_oversized_length_is_caught_before_anything_reads_it(void)
{
    img_fix_t f;

    build_image(&f, 1, 256, 0x33);
    tamper32(&f, 12, 0xFFFFFF00UL);   /* payload_length */

    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_LENGTH_OUT_OF_RANGE);
    CHECK(f.slot.highest_offset <= SLOT_SIZE);
    CHECK_EQ(f.slot.highest_offset, IMAGE_HEADER_LEN); /* only the header */
    CHECK_EQ(f.key.calls, 0);                          /* verifier untouched */
}

static void test_length_exactly_filling_the_slot_is_accepted(void)
{
    img_fix_t f;

    /* The boundary has to work, or legitimate full-slot images are refused. */
    build_image(&f, 1, SLOT_SIZE - IMAGE_HEADER_LEN, 0x44);
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_OK);
}

static void test_length_one_byte_too_large_is_refused(void)
{
    img_fix_t f;

    build_image(&f, 1, 256, 0x44);
    tamper32(&f, 12, SLOT_SIZE - IMAGE_HEADER_LEN + 1U);
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_LENGTH_OUT_OF_RANGE);
}

static void test_zero_length_is_refused(void)
{
    img_fix_t f;

    build_image(&f, 1, 256, 0x44);
    tamper32(&f, 12, 0U);
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_LENGTH_OUT_OF_RANGE);
}

static void test_tampered_header_fails_the_signature(void)
{
    img_fix_t f;

    /* Raise the version without re-signing: the classic attempt to smuggle a
       change past a check that only looks at some fields. */
    build_image(&f, 3, 256, 0x55);
    tamper32(&f, 8, 999);
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_BAD_SIGNATURE);
    CHECK_EQ(f.key.calls, 1);
}

static void test_tampered_payload_fails_the_hash(void)
{
    img_fix_t f;

    /* Header genuinely signed, payload altered afterwards. This is what an
       interrupted or substituted transfer looks like. */
    build_image(&f, 1, 512, 0x66);
    f.slot.data[IMAGE_HEADER_LEN + 100] ^= 0xFF;

    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_BAD_PAYLOAD_HASH);
}

static void test_truncated_payload_fails_the_hash(void)
{
    img_fix_t f;

    /* A transfer that stopped part-way: the tail is still erased flash. The
       image must not run, which is precisely what a CRC in an unsigned header
       would fail to guarantee. */
    build_image(&f, 1, 512, 0x77);
    memset(&f.slot.data[IMAGE_HEADER_LEN + 256], 0xFF, 256);

    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_BAD_PAYLOAD_HASH);
}

static void test_rollback_is_refused_despite_a_valid_signature(void)
{
    img_fix_t f;

    build_image(&f, 4, 256, 0x88);
    ImageVerify_Init(&f.v, SLOT_SIZE, 7U);   /* floor above this version */

    /* Everything about this image is authentic. It is refused anyway, because
       the threat is a genuinely signed older build with a defect that has since
       been fixed. */
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_ROLLBACK);
    CHECK_EQ(f.v.last_version, 4);
}

static void test_same_version_as_floor_is_allowed(void)
{
    img_fix_t f;

    /* Reinstalling the current version is legitimate — recovering a corrupted
       slot, for instance — so the floor is a minimum, not a strict increase. */
    build_image(&f, 7, 256, 0x99);
    ImageVerify_Init(&f.v, SLOT_SIZE, 7U);
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_OK);
}

static void test_missing_verifier_fails_closed(void)
{
    img_fix_t f;

    build_image(&f, 1, 256, 0xAA);
    ImageVerify_SetVerifier(&f.v, NULL, NULL);

    /* A build that forgot to provision a key must refuse every image rather
       than accept every image. */
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_NO_VERIFIER);
}

static void test_read_failure_is_reported(void)
{
    img_fix_t f;

    build_image(&f, 1, 256, 0xBB);
    f.slot.fail_reads = true;
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_READ_FAILED);
}

static void test_payload_is_streamed_not_buffered(void)
{
    img_fix_t f;

    /* A payload far larger than any reasonable RAM buffer still verifies,
       because it is hashed in chunks straight out of the slot. On the target
       the slot is 128 KB against 128 KB of total RAM, so buffering is not an
       option even in principle. */
    build_image(&f, 1, SLOT_SIZE - IMAGE_HEADER_LEN, 0xCC);
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_OK);
    CHECK(f.slot.reads > 4);   /* many small reads, not one big one */
}

int main(void)
{
    test_sha256_known_vectors();
    test_sha256_one_million_a();
    test_sha256_streaming_matches_one_shot();
    test_digest_compare();

    test_valid_image_is_accepted();
    test_header_round_trips();
    test_truncated_header_is_rejected();
    test_bad_magic_and_header_version();
    test_oversized_length_is_caught_before_anything_reads_it();
    test_length_exactly_filling_the_slot_is_accepted();
    test_length_one_byte_too_large_is_refused();
    test_zero_length_is_refused();
    test_tampered_header_fails_the_signature();
    test_tampered_payload_fails_the_hash();
    test_truncated_payload_fails_the_hash();
    test_rollback_is_refused_despite_a_valid_signature();
    test_same_version_as_floor_is_allowed();
    test_missing_verifier_fails_closed();
    test_read_failure_is_reported();
    test_payload_is_streamed_not_buffered();

    return test_report();
}
