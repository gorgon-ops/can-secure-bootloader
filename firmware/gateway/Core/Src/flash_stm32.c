/* Single flash bank: while a sector erases, every fetch from flash stalls, so
 * the core freezes for the ~1-2 s of a 128 KB erase - but the IWDG keeps
 * counting on the LSI. Erasing a slot with the application's 170-250 ms
 * watchdog running is a guaranteed reset part-way through. Programming belongs
 * in a bootloader with the IWDG off or at its ~22 s maximum window. */
#include "flash_map.h"
#include "flash_writer.h"

#include "main.h"

#include <string.h>

static uint32_t FlashStm32_HalSector(uint32_t sector)
{
    /* FLASH_SECTOR_0..7 are consecutive on this family. Checked rather than
     * assumed: being wrong means erasing the wrong sector. */
    return sector;
}

static bool FlashStm32_Erase(void *ctx, uint32_t sector)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t bad_sector = 0U;
    HAL_StatusTypeDef st;

    (void)ctx;

    if (sector >= FLASH_SECTOR_COUNT)
    {
        return false;
    }

    /* Checked again at the hardware boundary, in case a future caller skips the
     * map. */
    if (!FlashMap_IsUpdatable(FlashMap_SectorBase(sector),
                              FlashMap_SectorSize(sector)))
    {
        return false;
    }

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return false;
    }

    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Banks        = FLASH_BANK_1;
    erase.Sector       = FlashStm32_HalSector(sector);
    erase.NbSectors    = 1U;
    /* RANGE_3 (2.7-3.6 V) permits word-wide programming. */
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    st = HAL_FLASHEx_Erase(&erase, &bad_sector);

    (void)HAL_FLASH_Lock();

    return (st == HAL_OK) && (bad_sector == 0xFFFFFFFFUL);
}

static bool FlashStm32_Program(void *ctx, uint32_t addr, const uint8_t *data,
                               uint32_t len)
{
    uint32_t i;

    (void)ctx;

    if ((data == NULL) || (len == 0U) || ((len % 4U) != 0U))
    {
        return false;
    }
    if ((addr % 4U) != 0U)
    {
        return false;
    }
    if (!FlashMap_IsUpdatable(addr, len))
    {
        return false;
    }

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return false;
    }

    for (i = 0U; i < len; i += 4U)
    {
        uint32_t word;

        /* Byte-wise: the source may be unaligned within a received block, and an
         * unaligned 32-bit load faults on this core. */
        word = (uint32_t)data[i] |
               ((uint32_t)data[i + 1U] << 8) |
               ((uint32_t)data[i + 2U] << 16) |
               ((uint32_t)data[i + 3U] << 24);

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i,
                              (uint64_t)word) != HAL_OK)
        {
            (void)HAL_FLASH_Lock();
            return false;
        }

        /* Read back: catches a worn or partially erased cell now rather than as a
         * failed image hash much later. */
        if (*(volatile uint32_t *)(addr + i) != word)
        {
            (void)HAL_FLASH_Lock();
            return false;
        }
    }

    (void)HAL_FLASH_Lock();
    return true;
}

void FlashStm32_GetOps(flash_ops_t *ops)
{
    ops->erase_sector = FlashStm32_Erase;
    ops->program      = FlashStm32_Program;
    ops->ctx          = NULL;
}
