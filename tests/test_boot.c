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

    /* The long vector, in chunks: catches a broken length counter or block
     * boundary that short messages pass. */
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

    /* Every split point must agree: block boundaries are where streaming goes
     * wrong. */
    for (split = 1; split < sizeof(data); split++) {
        Sha256_Init(&ctx);
        Sha256_Update(&ctx, data, split);
        Sha256_Update(&ctx, &data[split], sizeof(data) - split);
        Sha256_Final(&ctx, b);
        CHECK(memcmp(a, b, SHA256_DIGEST_LEN) == 0);
    }
}

/* Correctness only. No functional test can show the constant-time property -
 * a short-circuiting compare returns the same booleans. That is held by
 * review, and on target would be measured with a cycle counter. */
static void test_digest_compare(void)
{
    uint8_t a[SHA256_DIGEST_LEN], b[SHA256_DIGEST_LEN];

    memset(a, 0x5A, sizeof(a));
    memcpy(b, a, sizeof(b));
    CHECK(Sha256_Equal(a, b));

    b[SHA256_DIGEST_LEN - 1] ^= 0x01;   /* last byte only */
    CHECK(!Sha256_Equal(a, b));

    memcpy(b, a, sizeof(b));
    b[0] ^= 0x80;
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
    uint32_t highest_offset;
    bool     fail_reads;
} fake_slot_t;

/* Records the furthest offset requested, so a test can prove nothing past the
 * slot was read. */
static bool slot_read(void *ctx, uint32_t offset, uint8_t *out, uint32_t len)
{
    fake_slot_t *s = (fake_slot_t *)ctx;

    s->reads++;
    if (s->fail_reads) {
        return false;
    }
    if (((uint64_t)offset + len) > SLOT_SIZE) {
        return false;
    }
    if ((offset + len) > s->highest_offset) {
        s->highest_offset = offset + len;
    }
    memcpy(out, &s->data[offset], len);
    return true;
}

/* Stand-in signature: accepts when the message hashes to a planted value. */
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

    sign(&f->key, raw, IMAGE_SIGNED_LEN);
    memcpy(&raw[IMAGE_SIGNED_LEN], f->key.expected, IMAGE_SIGNATURE_LEN);
    memcpy(f->slot.data, raw, IMAGE_HEADER_LEN);

    ImageVerify_SetReader(&f->v, slot_read, &f->slot);
    ImageVerify_SetVerifier(&f->v, fake_verify, &f->key);
    ImageVerify_Init(&f->v, SLOT_SIZE, 0U);
}

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

/* The length must be rejected before the payload hash reads past the slot,
 * and before the verifier is even called. */
static void test_oversized_length_is_caught_before_anything_reads_it(void)
{
    img_fix_t f;

    build_image(&f, 1, 256, 0x33);
    tamper32(&f, 12, 0xFFFFFF00UL);

    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_LENGTH_OUT_OF_RANGE);
    CHECK(f.slot.highest_offset <= SLOT_SIZE);
    CHECK_EQ(f.slot.highest_offset, IMAGE_HEADER_LEN); /* only the header */
    CHECK_EQ(f.key.calls, 0);                          /* verifier untouched */
}

static void test_length_exactly_filling_the_slot_is_accepted(void)
{
    img_fix_t f;

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

    /* Version raised without re-signing. */
    build_image(&f, 3, 256, 0x55);
    tamper32(&f, 8, 999);
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_BAD_SIGNATURE);
    CHECK_EQ(f.key.calls, 1);
}

static void test_tampered_payload_fails_the_hash(void)
{
    img_fix_t f;

    /* Genuinely signed header, payload altered afterwards. */
    build_image(&f, 1, 512, 0x66);
    f.slot.data[IMAGE_HEADER_LEN + 100] ^= 0xFF;

    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_BAD_PAYLOAD_HASH);
}

static void test_truncated_payload_fails_the_hash(void)
{
    img_fix_t f;

    /* Tail still erased: a CRC in an unsigned header would not stop this. */
    build_image(&f, 1, 512, 0x77);
    memset(&f.slot.data[IMAGE_HEADER_LEN + 256], 0xFF, 256);

    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_BAD_PAYLOAD_HASH);
}

static void test_rollback_is_refused_despite_a_valid_signature(void)
{
    img_fix_t f;

    build_image(&f, 4, 256, 0x88);
    ImageVerify_Init(&f.v, SLOT_SIZE, 7U);   /* floor above this version */

    /* Authentic, refused anyway: a signed older build with a since-fixed defect. */
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_ERR_ROLLBACK);
    CHECK_EQ(f.v.last_version, 4);
}

static void test_same_version_as_floor_is_allowed(void)
{
    img_fix_t f;

    /* Reinstalling the current version is legitimate: the floor is a minimum. */
    build_image(&f, 7, 256, 0x99);
    ImageVerify_Init(&f.v, SLOT_SIZE, 7U);
    CHECK_EQ(ImageVerify_Run(&f.v), IMAGE_OK);
}

static void test_missing_verifier_fails_closed(void)
{
    img_fix_t f;

    build_image(&f, 1, 256, 0xAA);
    ImageVerify_SetVerifier(&f.v, NULL, NULL);

    /* No key provisioned: refuse everything. */
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

    /* Hashed in chunks straight out of the slot - on target the slot is as big as
     * all of RAM. */
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
