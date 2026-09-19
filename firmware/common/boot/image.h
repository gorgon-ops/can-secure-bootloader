/**
 * @file    image.h
 * @brief   Firmware image header, authenticity check and rollback protection.
 *
 * Verifying an image is four questions, and the order they are asked in is the
 * security-relevant part:
 *
 *   1. Does the header claim a length that fits the slot?
 *   2. Is the header signed by the holder of the private key?
 *   3. Does the payload hash to what the header says?
 *   4. Is the version at least the rollback floor?
 *
 * Question 1 comes first even though the signature covers the length, because
 * the length is attacker-controlled until question 2 has passed. Hashing a
 * payload using a length taken from an unverified header reads past the end of
 * the slot, and the fact that the signature would have failed a moment later is
 * no consolation — the out-of-bounds read has already happened. The same
 * reasoning as the transport layer: bound the input before you use it.
 *
 * ## On the signature primitive
 *
 * It is deliberately **not implemented here**. The elliptic-curve arithmetic
 * behind Ed25519 or ECDSA is exactly the kind of code that is easy to write,
 * hard to write correctly, and catastrophic when subtly wrong — a
 * non-constant-time comparison or a missing point validation leaks the key.
 * Writing it to demonstrate competence would demonstrate the opposite.
 *
 * So verification arrives through `image_verify_fn`, and the intended
 * production fill is a reviewed implementation such as TweetNaCl or micro-ecc.
 * Everything around it — framing, ordering, bounds, hashing, rollback — is here
 * and tested.
 */
#ifndef IMAGE_H
#define IMAGE_H

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** "BFM1" — identifies a header this build understands. */
#define IMAGE_MAGIC 0x42464D31UL

/** Bumped when the header layout changes incompatibly. */
#define IMAGE_HEADER_VERSION 1U

/** Ed25519 and P-256 both produce 64 bytes. */
#define IMAGE_SIGNATURE_LEN 64U

/** Bytes of the header covered by the signature. */
#define IMAGE_SIGNED_LEN 64U

/** Total header size, signed part plus signature. */
#define IMAGE_HEADER_LEN (IMAGE_SIGNED_LEN + IMAGE_SIGNATURE_LEN)

typedef enum
{
    IMAGE_OK = 0,
    IMAGE_ERR_BAD_MAGIC,
    IMAGE_ERR_BAD_HEADER_VERSION,
    IMAGE_ERR_LENGTH_OUT_OF_RANGE, /*!< claims more than the slot holds     */
    IMAGE_ERR_NO_VERIFIER,         /*!< nothing provisioned: fail closed    */
    IMAGE_ERR_BAD_SIGNATURE,
    IMAGE_ERR_BAD_PAYLOAD_HASH,    /*!< signed header, but payload differs  */
    IMAGE_ERR_ROLLBACK,            /*!< authentic, but older than the floor */
    IMAGE_ERR_READ_FAILED
} image_result_t;

/**
 * Image header, little-endian on the wire and in flash.
 *
 * The first IMAGE_SIGNED_LEN bytes are what the signature covers, so every
 * field that verification depends on — length, version, payload hash — is
 * authenticated. Reserved bytes are inside the signed region so they cannot be
 * used as an unauthenticated side channel later.
 */
typedef struct
{
    uint32_t magic;
    uint32_t header_version;
    uint32_t image_version;   /*!< monotonic; compared against the floor */
    uint32_t payload_length;
    uint8_t  payload_sha256[SHA256_DIGEST_LEN];
    uint32_t entry_offset;    /*!< vector table offset within the slot   */
    uint32_t reserved[3];
    uint8_t  signature[IMAGE_SIGNATURE_LEN];
} image_header_t;

/**
 * @brief Read @p len bytes at @p offset within the slot.
 * @return true on success.
 *
 * Injected so the same verification runs against flash on the target and
 * against a buffer under test.
 */
typedef bool (*image_read_fn)(void *ctx, uint32_t offset,
                              uint8_t *out, uint32_t len);

/**
 * @brief Verify a signature over @p msg.
 * @return true only if produced by the holder of the expected private key.
 *
 * See the note at the top of this file: this is where a reviewed
 * implementation goes, and why none is written here.
 */
typedef bool (*image_verify_fn)(void *ctx,
                                const uint8_t *msg, uint32_t msg_len,
                                const uint8_t signature[IMAGE_SIGNATURE_LEN]);

typedef struct
{
    image_read_fn   read;
    void           *read_ctx;
    image_verify_fn verify;
    void           *verify_ctx;

    uint32_t slot_size;       /*!< bytes available for header plus payload */
    uint32_t rollback_floor;  /*!< lowest version this ECU will accept     */

    /* Diagnostics. Populated by the last call to ImageVerify_Run(). */
    uint32_t last_version;
    uint32_t last_length;
} image_verifier_t;

void ImageVerify_Init(image_verifier_t *v, uint32_t slot_size,
                      uint32_t rollback_floor);

void ImageVerify_SetReader(image_verifier_t *v, image_read_fn read, void *ctx);
void ImageVerify_SetVerifier(image_verifier_t *v, image_verify_fn verify,
                             void *ctx);

/**
 * @brief Parse a header out of a raw buffer.
 * @return false if the buffer is too short.
 *
 * Does not validate anything beyond the length: parsing and judging are kept
 * separate so the judging can be tested on its own.
 */
bool ImageHeader_Parse(const uint8_t *raw, uint32_t len, image_header_t *out);

/** @brief Serialise a header, for the tests and for the signing tool. */
bool ImageHeader_Serialise(const image_header_t *h, uint8_t *out, uint32_t cap);

/**
 * @brief Verify the image in the slot, in the order described above.
 * @return IMAGE_OK only if it is authentic, intact and not a rollback.
 */
image_result_t ImageVerify_Run(image_verifier_t *v);

/** @brief Human-readable result, for diagnostics and test output. */
const char *ImageVerify_ResultName(image_result_t r);

#endif /* IMAGE_H */
