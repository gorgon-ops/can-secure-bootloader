#include "flash_writer.h"

#include <string.h>

void FlashWriter_Init(flash_writer_t *w, uint8_t active_slot)
{
    flash_ops_t ops = w->ops;

    memset(w, 0, sizeof(*w));
    w->ops         = ops;
    w->active_slot = active_slot;
}

void FlashWriter_SetOps(flash_writer_t *w, const flash_ops_t *ops)
{
    w->ops = *ops;
}

uint32_t FlashWriter_Written(const flash_writer_t *w)
{
    return w->written;
}

void FlashWriter_Abort(flash_writer_t *w)
{
    w->open        = false;
    w->written     = 0U;
    w->declared    = 0U;
    w->pending_len = 0U;
}

flash_write_result_t FlashWriter_Begin(flash_writer_t *w, uint8_t slot,
                                       uint32_t total_len)
{
    uint32_t base;
    uint32_t size;
    uint32_t first;
    uint32_t count;
    uint32_t i;

    if (!FlashMap_SlotBounds(slot, &base, &size))
    {
        w->stat_rejected++;
        return FLASH_WRITE_ERR_BAD_SLOT;
    }

    /* Erasing the slot the processor is executing from would destroy the
       running image mid-instruction. The two-slot layout exists precisely so
       this never has to happen. */
    if (slot == w->active_slot)
    {
        w->stat_rejected++;
        return FLASH_WRITE_ERR_ACTIVE_SLOT;
    }

    if ((total_len == 0U) || (total_len > size))
    {
        w->stat_rejected++;
        return FLASH_WRITE_ERR_TOO_LARGE;
    }

    /* Belt and braces against the map: the slot bounds already say this is a
       slot, but asking the region check as well means a future layout change
       that accidentally overlaps the bootloader fails here rather than
       silently erasing it. */
    if (!FlashMap_IsUpdatable(base, size))
    {
        w->stat_rejected++;
        return FLASH_WRITE_ERR_PROTECTED;
    }

    if (!FlashMap_SectorRange(base, size, &first, &count))
    {
        w->stat_rejected++;
        return FLASH_WRITE_ERR_PROTECTED;
    }

    if (w->ops.erase_sector == NULL)
    {
        return FLASH_WRITE_ERR_ERASE_FAILED;
    }

    for (i = 0U; i < count; i++)
    {
        if (!w->ops.erase_sector(w->ops.ctx, first + i))
        {
            w->open = false;
            return FLASH_WRITE_ERR_ERASE_FAILED;
        }
        w->stat_sectors_erased++;
    }

    w->open        = true;
    w->slot        = slot;
    w->base        = base;
    w->size        = size;
    w->declared    = total_len;
    w->written     = 0U;
    w->pending_len = 0U;
    return FLASH_WRITE_OK;
}

/** Program one aligned word at the current offset. */
static bool FlashWriter_FlushWord(flash_writer_t *w, uint32_t offset)
{
    if (w->ops.program == NULL)
    {
        return false;
    }
    if (!w->ops.program(w->ops.ctx, w->base + offset, w->pending,
                        FLASH_WRITE_ALIGN))
    {
        return false;
    }
    w->stat_bytes_programmed += FLASH_WRITE_ALIGN;
    w->pending_len = 0U;
    return true;
}

flash_write_result_t FlashWriter_Write(flash_writer_t *w,
                                       const uint8_t *data, uint32_t len)
{
    uint32_t consumed = 0U;

    if (!w->open)
    {
        w->stat_rejected++;
        return FLASH_WRITE_ERR_NOT_OPEN;
    }
    if ((data == NULL) || (len == 0U))
    {
        return FLASH_WRITE_OK;
    }

    /* More than was promised. Accepting it would write past the declared image
       and, with a large enough overrun, past the slot itself. */
    if (((uint64_t)w->written + (uint64_t)len) > (uint64_t)w->declared)
    {
        w->stat_rejected++;
        return FLASH_WRITE_ERR_OVERRUN;
    }

    while (consumed < len)
    {
        uint32_t offset = (w->written + consumed) - w->pending_len;

        /* Fill the partial word first, then flush it. Chunks arriving from a
           tester are whatever size it chose, so the aligned case is the
           exception rather than the rule. */
        while ((w->pending_len < FLASH_WRITE_ALIGN) && (consumed < len))
        {
            w->pending[w->pending_len] = data[consumed];
            w->pending_len++;
            consumed++;
        }

        if (w->pending_len == FLASH_WRITE_ALIGN)
        {
            if (!FlashWriter_FlushWord(w, offset))
            {
                return FLASH_WRITE_ERR_PROGRAM_FAILED;
            }
        }
    }

    w->written += len;
    return FLASH_WRITE_OK;
}

flash_write_result_t FlashWriter_Finish(flash_writer_t *w)
{
    if (!w->open)
    {
        w->stat_rejected++;
        return FLASH_WRITE_ERR_NOT_OPEN;
    }

    /* Short of the declared length means the transfer was interrupted. The
       image would fail verification anyway — its hash covers the full payload —
       but refusing here reports the real cause instead of a hash mismatch. */
    if (w->written < w->declared)
    {
        w->stat_rejected++;
        w->open = false;
        return FLASH_WRITE_ERR_INCOMPLETE;
    }

    if (w->pending_len > 0U)
    {
        uint32_t offset = w->written - w->pending_len;

        /* Pad to a word with 0xFF: that is what an erased cell already holds,
           so the padding programs nothing and leaves the bytes untouched. */
        while (w->pending_len < FLASH_WRITE_ALIGN)
        {
            w->pending[w->pending_len] = 0xFFU;
            w->pending_len++;
        }

        if (!FlashWriter_FlushWord(w, offset))
        {
            w->open = false;
            return FLASH_WRITE_ERR_PROGRAM_FAILED;
        }
    }

    w->open = false;
    return FLASH_WRITE_OK;
}

const char *FlashWriter_ResultName(flash_write_result_t r)
{
    switch (r)
    {
        case FLASH_WRITE_OK:                 return "OK";
        case FLASH_WRITE_ERR_BAD_SLOT:       return "BAD_SLOT";
        case FLASH_WRITE_ERR_TOO_LARGE:      return "TOO_LARGE";
        case FLASH_WRITE_ERR_ACTIVE_SLOT:    return "ACTIVE_SLOT";
        case FLASH_WRITE_ERR_NOT_OPEN:       return "NOT_OPEN";
        case FLASH_WRITE_ERR_OVERRUN:        return "OVERRUN";
        case FLASH_WRITE_ERR_INCOMPLETE:     return "INCOMPLETE";
        case FLASH_WRITE_ERR_PROTECTED:      return "PROTECTED";
        case FLASH_WRITE_ERR_ERASE_FAILED:   return "ERASE_FAILED";
        case FLASH_WRITE_ERR_PROGRAM_FAILED: return "PROGRAM_FAILED";
        default:                             return "?";
    }
}
