#include "sha256.h"

#include <string.h>

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32U - (n))))

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

/** First 32 bits of the fractional parts of the cube roots of the first 64 primes. */
static const uint32_t K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void Sha256_Compress(sha256_t *ctx, const uint8_t block[SHA256_BLOCK_LEN])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned i;

    for (i = 0U; i < 16U; i++)
    {
        w[i] = ((uint32_t)block[i * 4U] << 24) |
               ((uint32_t)block[(i * 4U) + 1U] << 16) |
               ((uint32_t)block[(i * 4U) + 2U] << 8) |
               ((uint32_t)block[(i * 4U) + 3U]);
    }
    for (i = 16U; i < 64U; i++)
    {
        w[i] = SSIG1(w[i - 2U]) + w[i - 7U] + SSIG0(w[i - 15U]) + w[i - 16U];
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0U; i < 64U; i++)
    {
        uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);

        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void Sha256_Init(sha256_t *ctx)
{
    /* First 32 bits of the fractional parts of the square roots of the first
       eight primes. */
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
    ctx->bitlen   = 0U;
    ctx->buflen   = 0U;
}

void Sha256_Update(sha256_t *ctx, const uint8_t *data, size_t len)
{
    size_t i = 0U;

    if ((data == NULL) || (len == 0U))
    {
        return;
    }

    ctx->bitlen += (uint64_t)len * 8U;

    /* Top up a partial block first. */
    if (ctx->buflen > 0U)
    {
        size_t need = (size_t)(SHA256_BLOCK_LEN - ctx->buflen);
        size_t take = (len < need) ? len : need;

        memcpy(&ctx->buf[ctx->buflen], data, take);
        ctx->buflen = (uint8_t)(ctx->buflen + take);
        i = take;

        if (ctx->buflen < SHA256_BLOCK_LEN)
        {
            return;
        }
        Sha256_Compress(ctx, ctx->buf);
        ctx->buflen = 0U;
    }

    while ((len - i) >= SHA256_BLOCK_LEN)
    {
        Sha256_Compress(ctx, &data[i]);
        i += SHA256_BLOCK_LEN;
    }

    if ((len - i) > 0U)
    {
        memcpy(ctx->buf, &data[i], len - i);
        ctx->buflen = (uint8_t)(len - i);
    }
}

void Sha256_Final(sha256_t *ctx, uint8_t out[SHA256_DIGEST_LEN])
{
    uint64_t bits = ctx->bitlen;
    unsigned i;

    /* Append 0x80, pad with zeroes, then the length as a 64-bit big-endian
       count of bits. If the length will not fit in this block, close it out
       and pad a further one. */
    ctx->buf[ctx->buflen] = 0x80U;
    ctx->buflen++;

    if (ctx->buflen > (SHA256_BLOCK_LEN - 8U))
    {
        memset(&ctx->buf[ctx->buflen], 0, SHA256_BLOCK_LEN - ctx->buflen);
        Sha256_Compress(ctx, ctx->buf);
        ctx->buflen = 0U;
        memset(ctx->buf, 0, SHA256_BLOCK_LEN);
    }
    else
    {
        memset(&ctx->buf[ctx->buflen], 0, SHA256_BLOCK_LEN - ctx->buflen);
    }

    for (i = 0U; i < 8U; i++)
    {
        ctx->buf[SHA256_BLOCK_LEN - 1U - i] = (uint8_t)(bits >> (8U * i));
    }
    Sha256_Compress(ctx, ctx->buf);

    for (i = 0U; i < 8U; i++)
    {
        out[i * 4U]        = (uint8_t)(ctx->state[i] >> 24);
        out[(i * 4U) + 1U] = (uint8_t)(ctx->state[i] >> 16);
        out[(i * 4U) + 2U] = (uint8_t)(ctx->state[i] >> 8);
        out[(i * 4U) + 3U] = (uint8_t)(ctx->state[i]);
    }

    /* The state is not secret, but leaving a stale digest in a reused context
       invites a caller to read it as a fresh one. */
    memset(ctx, 0, sizeof(*ctx));
}

void Sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_LEN])
{
    sha256_t ctx;

    Sha256_Init(&ctx);
    Sha256_Update(&ctx, data, len);
    Sha256_Final(&ctx, out);
}

bool Sha256_Equal(const uint8_t a[SHA256_DIGEST_LEN],
                  const uint8_t b[SHA256_DIGEST_LEN])
{
    uint8_t diff = 0U;
    unsigned i;

    /* Accumulate rather than returning early: the time taken must not depend
       on how many leading bytes happened to match. */
    for (i = 0U; i < SHA256_DIGEST_LEN; i++)
    {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }

    return diff == 0U;
}
