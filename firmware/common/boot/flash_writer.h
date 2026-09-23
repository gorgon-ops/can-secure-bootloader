/**
 * @file    flash_writer.h
 * @brief   Sequenced writing of an image into an application slot.
 *
 * Receives an image in arbitrary chunks — a UDS TransferData block is whatever
 * size the tester chose — and turns it into erase and program operations that
 * the hardware will actually accept. Three constraints shape it:
 *
 * **Erase is per sector.** You cannot rewrite one byte; the whole sector goes
 * first. So a slot is erased once at the start, not incrementally.
 *
 * **Flash clears bits but cannot set them.** An erased cell is 0xFF and
 * programming only drives ones to zeros. Writing the same address twice
 * without an erase silently ANDs the two values together, which is why writes
 * here are strictly append-only rather than addressed.
 *
 * **Programming is word-aligned.** Incoming chunks are not, so partial words
 * are buffered until they fill. The tail is padded with 0xFF, which is what an
 * erased cell already holds and therefore changes nothing.
 *
 * Pure module: the hardware arrives through `flash_ops_t`, so the sequencing
 * is tested on a host against an array that records exactly what would have
 * been erased and programmed.
 */
#ifndef FLASH_WRITER_H
#define FLASH_WRITER_H

#include "flash_map.h"

#include <stdbool.h>
#include <stdint.h>

/** Programming granularity. Word writes need a 4-byte aligned address. */
#define FLASH_WRITE_ALIGN 4U

typedef enum
{
    FLASH_WRITE_OK = 0,
    FLASH_WRITE_ERR_BAD_SLOT,
    FLASH_WRITE_ERR_TOO_LARGE,     /*!< declared image exceeds the slot     */
    FLASH_WRITE_ERR_ACTIVE_SLOT,   /*!< refusing to erase the running image */
    FLASH_WRITE_ERR_NOT_OPEN,      /*!< write or finish without a begin     */
    FLASH_WRITE_ERR_OVERRUN,       /*!< more data than was declared         */
    FLASH_WRITE_ERR_INCOMPLETE,    /*!< finish before the declared length    */
    FLASH_WRITE_ERR_PROTECTED,     /*!< range covers bootloader or metadata */
    FLASH_WRITE_ERR_ERASE_FAILED,
    FLASH_WRITE_ERR_PROGRAM_FAILED
} flash_write_result_t;

/** Hardware operations, injected. */
typedef struct
{
    bool (*erase_sector)(void *ctx, uint32_t sector);
    bool (*program)(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len);
    void *ctx;
} flash_ops_t;

typedef struct
{
    flash_ops_t ops;

    uint8_t  active_slot;   /*!< the slot currently executing; never erased */
    bool     open;
    uint8_t  slot;
    uint32_t base;
    uint32_t size;
    uint32_t declared;      /*!< total image length promised at begin       */
    uint32_t written;       /*!< bytes accepted so far                      */

    uint8_t  pending[FLASH_WRITE_ALIGN];
    uint8_t  pending_len;

    /* Diagnostics. */
    uint32_t stat_sectors_erased;
    uint32_t stat_bytes_programmed;
    uint32_t stat_rejected;
} flash_writer_t;

/**
 * @brief Reset the writer.
 * @param active_slot the slot the running image occupies. Begin refuses it,
 *        because erasing the code you are executing does not end well.
 */
void FlashWriter_Init(flash_writer_t *w, uint8_t active_slot);

void FlashWriter_SetOps(flash_writer_t *w, const flash_ops_t *ops);

/**
 * @brief Open a slot for writing and erase it.
 * @param total_len full image length, header included.
 *
 * The erase happens here, once, and covers exactly the slot's sectors. On this
 * part that takes on the order of a second for a 128 KB sector, during which
 * the core stalls on any flash fetch — see docs/secure-bootloader.md for what
 * that means for the watchdog.
 */
flash_write_result_t FlashWriter_Begin(flash_writer_t *w, uint8_t slot,
                                       uint32_t total_len);

/** @brief Append the next chunk. Order is implied; there is no address. */
flash_write_result_t FlashWriter_Write(flash_writer_t *w,
                                       const uint8_t *data, uint32_t len);

/** @brief Flush the final partial word and close. Fails if short of the length. */
flash_write_result_t FlashWriter_Finish(flash_writer_t *w);

/** @brief Abandon the transfer. The slot is left erased and unbootable. */
void FlashWriter_Abort(flash_writer_t *w);

/** @brief Bytes accepted so far. */
uint32_t FlashWriter_Written(const flash_writer_t *w);

/** @brief Human-readable result, for diagnostics and test output. */
const char *FlashWriter_ResultName(flash_write_result_t r);

#endif /* FLASH_WRITER_H */
