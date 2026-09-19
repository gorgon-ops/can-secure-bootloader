/**
 * @file    sha256.h
 * @brief   SHA-256 (FIPS 180-4).
 *
 * Streaming, so a firmware image can be hashed straight out of flash without
 * ever holding it in RAM — which matters on a part with 128 KB of RAM and
 * 128 KB application slots.
 *
 * Implementing a hash is defensible in a way that implementing a signature
 * scheme is not: SHA-256 is a fixed, published function with official test
 * vectors, so a correct implementation is *verifiable*. The tests check it
 * against the FIPS vectors including the one-million-character case. The
 * asymmetric primitive is a different matter and is deliberately left behind
 * an interface — see image.h.
 */
#ifndef SHA256_H
#define SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32U
#define SHA256_BLOCK_LEN  64U

typedef struct
{
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[SHA256_BLOCK_LEN];
    uint8_t  buflen;
} sha256_t;

void Sha256_Init(sha256_t *ctx);
void Sha256_Update(sha256_t *ctx, const uint8_t *data, size_t len);
void Sha256_Final(sha256_t *ctx, uint8_t out[SHA256_DIGEST_LEN]);

/** @brief One-shot convenience wrapper. */
void Sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

/**
 * @brief Compare two digests without leaking where they first differ.
 *
 * A plain memcmp returns as soon as it finds a mismatch, so how long it takes
 * depends on how many leading bytes matched. That is a timing oracle: an
 * attacker who can measure it recovers the expected digest one byte at a time.
 * This always reads both buffers in full.
 *
 * @return true if equal.
 */
bool Sha256_Equal(const uint8_t a[SHA256_DIGEST_LEN],
                  const uint8_t b[SHA256_DIGEST_LEN]);

#endif /* SHA256_H */
