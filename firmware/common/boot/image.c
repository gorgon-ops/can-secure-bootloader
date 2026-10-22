#include "image.h"

#include <string.h>

#define IMAGE_HASH_CHUNK 128U

static uint32_t Image_Load32(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void Image_Store32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool ImageHeader_Parse(const uint8_t *raw, uint32_t len, image_header_t *out)
{
    if ((raw == NULL) || (out == NULL) || (len < (uint32_t)IMAGE_HEADER_LEN))
    {
        return false;
    }

    out->magic          = Image_Load32(&raw[0]);
    out->header_version = Image_Load32(&raw[4]);
    out->image_version  = Image_Load32(&raw[8]);
    out->payload_length = Image_Load32(&raw[12]);
    memcpy(out->payload_sha256, &raw[16], SHA256_DIGEST_LEN);
    out->entry_offset   = Image_Load32(&raw[48]);
    out->reserved[0]    = Image_Load32(&raw[52]);
    out->reserved[1]    = Image_Load32(&raw[56]);
    out->reserved[2]    = Image_Load32(&raw[60]);
    memcpy(out->signature, &raw[IMAGE_SIGNED_LEN], IMAGE_SIGNATURE_LEN);
    return true;
}

bool ImageHeader_Serialise(const image_header_t *h, uint8_t *out, uint32_t cap)
{
    if ((h == NULL) || (out == NULL) || (cap < (uint32_t)IMAGE_HEADER_LEN))
    {
        return false;
    }

    memset(out, 0, IMAGE_HEADER_LEN);
    Image_Store32(&out[0], h->magic);
    Image_Store32(&out[4], h->header_version);
    Image_Store32(&out[8], h->image_version);
    Image_Store32(&out[12], h->payload_length);
    memcpy(&out[16], h->payload_sha256, SHA256_DIGEST_LEN);
    Image_Store32(&out[48], h->entry_offset);
    Image_Store32(&out[52], h->reserved[0]);
    Image_Store32(&out[56], h->reserved[1]);
    Image_Store32(&out[60], h->reserved[2]);
    memcpy(&out[IMAGE_SIGNED_LEN], h->signature, IMAGE_SIGNATURE_LEN);
    return true;
}

void ImageVerify_Init(image_verifier_t *v, uint32_t slot_size,
                      uint32_t rollback_floor)
{
    image_read_fn   read  = v->read;
    void           *rctx  = v->read_ctx;
    image_verify_fn ver   = v->verify;
    void           *vctx  = v->verify_ctx;

    memset(v, 0, sizeof(*v));
    v->read           = read;
    v->read_ctx       = rctx;
    v->verify         = ver;
    v->verify_ctx     = vctx;
    v->slot_size      = slot_size;
    v->rollback_floor = rollback_floor;
}

void ImageVerify_SetReader(image_verifier_t *v, image_read_fn read, void *ctx)
{
    v->read     = read;
    v->read_ctx = ctx;
}

void ImageVerify_SetVerifier(image_verifier_t *v, image_verify_fn verify,
                             void *ctx)
{
    v->verify     = verify;
    v->verify_ctx = ctx;
}

static bool Image_HashPayload(image_verifier_t *v, uint32_t length,
                              uint8_t out[SHA256_DIGEST_LEN])
{
    sha256_t ctx;
    uint8_t  chunk[IMAGE_HASH_CHUNK];
    uint32_t done = 0U;

    Sha256_Init(&ctx);

    while (done < length)
    {
        uint32_t take = length - done;

        if (take > (uint32_t)IMAGE_HASH_CHUNK)
        {
            take = (uint32_t)IMAGE_HASH_CHUNK;
        }

        if (!v->read(v->read_ctx, (uint32_t)IMAGE_HEADER_LEN + done, chunk, take))
        {
            return false;
        }

        Sha256_Update(&ctx, chunk, (size_t)take);
        done += take;
    }

    Sha256_Final(&ctx, out);
    return true;
}

image_result_t ImageVerify_Run(image_verifier_t *v)
{
    uint8_t        raw[IMAGE_HEADER_LEN];
    image_header_t h;
    uint8_t        digest[SHA256_DIGEST_LEN];

    v->last_version = 0U;
    v->last_length  = 0U;

    if ((v->read == NULL) ||
        !v->read(v->read_ctx, 0U, raw, (uint32_t)IMAGE_HEADER_LEN))
    {
        return IMAGE_ERR_READ_FAILED;
    }

    if (!ImageHeader_Parse(raw, (uint32_t)IMAGE_HEADER_LEN, &h))
    {
        return IMAGE_ERR_READ_FAILED;
    }

    /* --- 1. Cheap sanity, and the bound ------------------------------- */

    if (h.magic != (uint32_t)IMAGE_MAGIC)
    {
        return IMAGE_ERR_BAD_MAGIC;
    }
    if (h.header_version != (uint32_t)IMAGE_HEADER_VERSION)
    {
        return IMAGE_ERR_BAD_HEADER_VERSION;
    }

    v->last_version = h.image_version;
    v->last_length  = h.payload_length;

    /* Bounded before use even though the signature covers it: until verified the
     * header is attacker-controlled, and an unchecked length reads past the slot. */
    if ((h.payload_length == 0U) ||
        (h.payload_length > (v->slot_size - (uint32_t)IMAGE_HEADER_LEN)))
    {
        return IMAGE_ERR_LENGTH_OUT_OF_RANGE;
    }

    /* --- 2. Authenticity of the header -------------------------------- */

    if (v->verify == NULL)
    {
        /* No key provisioned: refuse everything. */
        return IMAGE_ERR_NO_VERIFIER;
    }

    if (!v->verify(v->verify_ctx, raw, (uint32_t)IMAGE_SIGNED_LEN, h.signature))
    {
        return IMAGE_ERR_BAD_SIGNATURE;
    }

    /* --- 3. The payload matches the authenticated header --------------- */

    if (!Image_HashPayload(v, h.payload_length, digest))
    {
        return IMAGE_ERR_READ_FAILED;
    }

    /* Catches an interrupted transfer. Unlike a CRC, the expected hash is inside
     * the signed header, so an attacker cannot recompute it. */
    if (!Sha256_Equal(digest, h.payload_sha256))
    {
        return IMAGE_ERR_BAD_PAYLOAD_HASH;
    }

    /* --- 4. Not a rollback -------------------------------------------- */

    /* A genuinely signed older image can still carry a since-fixed defect. */
    if (h.image_version < v->rollback_floor)
    {
        return IMAGE_ERR_ROLLBACK;
    }

    return IMAGE_OK;
}

const char *ImageVerify_ResultName(image_result_t r)
{
    switch (r)
    {
        case IMAGE_OK:                     return "OK";
        case IMAGE_ERR_BAD_MAGIC:          return "BAD_MAGIC";
        case IMAGE_ERR_BAD_HEADER_VERSION: return "BAD_HEADER_VERSION";
        case IMAGE_ERR_LENGTH_OUT_OF_RANGE:return "LENGTH_OUT_OF_RANGE";
        case IMAGE_ERR_NO_VERIFIER:        return "NO_VERIFIER";
        case IMAGE_ERR_BAD_SIGNATURE:      return "BAD_SIGNATURE";
        case IMAGE_ERR_BAD_PAYLOAD_HASH:   return "BAD_PAYLOAD_HASH";
        case IMAGE_ERR_ROLLBACK:           return "ROLLBACK";
        case IMAGE_ERR_READ_FAILED:        return "READ_FAILED";
        default:                           return "?";
    }
}
