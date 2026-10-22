/* Streaming, so an image can be hashed straight out of flash without holding
 * it in RAM. */
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
void Sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

/* Constant time: memcmp returns at the first mismatch, which leaks the
 * expected digest one byte at a time. */
bool Sha256_Equal(const uint8_t a[SHA256_DIGEST_LEN],
                  const uint8_t b[SHA256_DIGEST_LEN]);

#endif
