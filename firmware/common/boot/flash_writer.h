/* Flash can clear bits but not set them, so writing an address twice without
 * an erase silently ANDs the values - hence append-only rather than addressed.
 * Partial words are buffered and the tail is padded with 0xFF, which is what an
 * erased cell already holds. */
#ifndef FLASH_WRITER_H
#define FLASH_WRITER_H

#include "flash_map.h"

#include <stdbool.h>
#include <stdint.h>

#define FLASH_WRITE_ALIGN 4U

typedef enum
{
    FLASH_WRITE_OK = 0,
    FLASH_WRITE_ERR_BAD_SLOT,
    FLASH_WRITE_ERR_TOO_LARGE,
    FLASH_WRITE_ERR_ACTIVE_SLOT,   /* refusing to erase the running image */
    FLASH_WRITE_ERR_NOT_OPEN,
    FLASH_WRITE_ERR_OVERRUN,
    FLASH_WRITE_ERR_INCOMPLETE,
    FLASH_WRITE_ERR_PROTECTED,     /* range covers bootloader or metadata */
    FLASH_WRITE_ERR_ERASE_FAILED,
    FLASH_WRITE_ERR_PROGRAM_FAILED
} flash_write_result_t;

typedef struct
{
    bool (*erase_sector)(void *ctx, uint32_t sector);
    bool (*program)(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len);
    void *ctx;
} flash_ops_t;

typedef struct
{
    flash_ops_t ops;

    uint8_t  active_slot;   /* never erased */
    bool     open;
    uint8_t  slot;
    uint32_t base;
    uint32_t size;
    uint32_t declared;
    uint32_t written;

    uint8_t  pending[FLASH_WRITE_ALIGN];
    uint8_t  pending_len;

    uint32_t stat_sectors_erased;
    uint32_t stat_bytes_programmed;
    uint32_t stat_rejected;
} flash_writer_t;

/* Begin refuses active_slot: erasing the code you are executing does not end
 * well. */
void FlashWriter_Init(flash_writer_t *w, uint8_t active_slot);

void FlashWriter_SetOps(flash_writer_t *w, const flash_ops_t *ops);

/* Erases the whole slot once, here. The core stalls on flash fetches for the
 * ~1 s per 128 KB sector - see docs/secure-bootloader.md for the watchdog. */
flash_write_result_t FlashWriter_Begin(flash_writer_t *w, uint8_t slot,
                                       uint32_t total_len);

flash_write_result_t FlashWriter_Write(flash_writer_t *w,
                                       const uint8_t *data, uint32_t len);

flash_write_result_t FlashWriter_Finish(flash_writer_t *w);

/* Leaves the slot erased and unbootable. */
void FlashWriter_Abort(flash_writer_t *w);

uint32_t FlashWriter_Written(const flash_writer_t *w);
const char *FlashWriter_ResultName(flash_write_result_t r);

#endif
