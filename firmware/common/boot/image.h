/* The length is checked against the slot before anything else, even though
 * the signature covers it: until the signature has passed it is
 * attacker-controlled, and hashing with it would read past the slot. */
#ifndef IMAGE_H
#define IMAGE_H

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* "BFM1" */
#define IMAGE_MAGIC 0x42464D31UL

#define IMAGE_HEADER_VERSION 1U

/* Ed25519 and P-256 both produce 64 bytes. */
#define IMAGE_SIGNATURE_LEN 64U

#define IMAGE_SIGNED_LEN 64U

#define IMAGE_HEADER_LEN (IMAGE_SIGNED_LEN + IMAGE_SIGNATURE_LEN)

typedef enum
{
    IMAGE_OK = 0,
    IMAGE_ERR_BAD_MAGIC,
    IMAGE_ERR_BAD_HEADER_VERSION,
    IMAGE_ERR_LENGTH_OUT_OF_RANGE,
    IMAGE_ERR_NO_VERIFIER,         /* nothing provisioned: fail closed */
    IMAGE_ERR_BAD_SIGNATURE,
    IMAGE_ERR_BAD_PAYLOAD_HASH,
    IMAGE_ERR_ROLLBACK,            /* authentic, but older than the floor */
    IMAGE_ERR_READ_FAILED
} image_result_t;

/* Everything verification depends on, reserved bytes included, sits inside the
 * first IMAGE_SIGNED_LEN bytes the signature covers. */
typedef struct
{
    uint32_t magic;
    uint32_t header_version;
    uint32_t image_version;
    uint32_t payload_length;
    uint8_t  payload_sha256[SHA256_DIGEST_LEN];
    uint32_t entry_offset;
    uint32_t reserved[3];
    uint8_t  signature[IMAGE_SIGNATURE_LEN];
} image_header_t;

typedef bool (*image_read_fn)(void *ctx, uint32_t offset,
                              uint8_t *out, uint32_t len);

/* Deliberately not implemented here: the curve arithmetic is easy to get subtly
 * and catastrophically wrong. The intended fill is a reviewed library such as
 * TweetNaCl or micro-ecc. */
typedef bool (*image_verify_fn)(void *ctx,
                                const uint8_t *msg, uint32_t msg_len,
                                const uint8_t signature[IMAGE_SIGNATURE_LEN]);

typedef struct
{
    image_read_fn   read;
    void           *read_ctx;
    image_verify_fn verify;
    void           *verify_ctx;

    uint32_t slot_size;
    uint32_t rollback_floor;

    /* Populated by the last ImageVerify_Run(). */
    uint32_t last_version;
    uint32_t last_length;
} image_verifier_t;

void ImageVerify_Init(image_verifier_t *v, uint32_t slot_size,
                      uint32_t rollback_floor);

void ImageVerify_SetReader(image_verifier_t *v, image_read_fn read, void *ctx);
void ImageVerify_SetVerifier(image_verifier_t *v, image_verify_fn verify,
                             void *ctx);

bool ImageHeader_Parse(const uint8_t *raw, uint32_t len, image_header_t *out);
bool ImageHeader_Serialise(const image_header_t *h, uint8_t *out, uint32_t cap);
image_result_t ImageVerify_Run(image_verifier_t *v);
const char *ImageVerify_ResultName(image_result_t r);

#endif
