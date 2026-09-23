/**
 * @file    flash_stm32.c
 * @brief   The hardware behind flash_writer's injected operations.
 *
 * Everything that decides *what* to erase and program lives in
 * firmware/common/boot and is tested on a host. This file only performs it, so
 * the part that cannot be tested without a board is as small as possible.
 *
 * ## The constraint that shapes the caller
 *
 * This part has a single flash bank. While a sector is erasing, the flash
 * interface stalls every access to it — including instruction fetches. Code
 * executing from flash therefore *freezes* for the duration: no interrupts, no
 * SysTick, so `HAL_GetTick()` does not advance either.
 *
 * A 128 KB sector erase takes on the order of a second, up to around two in the
 * worst case. The application's watchdog window is 170 to 250 ms, and the IWDG
 * is clocked from the LSI, which keeps counting while the core is stalled. So:
 *
 *   **erasing an application slot with the application's watchdog running is a
 *   guaranteed reset, part-way through the erase.**
 *
 * That is why programming belongs in a bootloader that either leaves the IWDG
 * off or configures it with a far longer window — the maximum prescaler and
 * reload give roughly 22 seconds even at the fast end of the LSI tolerance,
 * which covers the worst-case erase with room to spare. Attempting an update
 * from the running application would need the erase routine relocated to RAM so
 * the core keeps fetching, which is a larger change than moving the operation
 * to where it belongs.
 *
 * None of this is visible from the host tests, so it is written down here.
 */
#include "flash_map.h"
#include "flash_writer.h"

#include "main.h"

#include <string.h>

/** Map a linear sector index onto the HAL's sector constant. */
static uint32_t FlashStm32_HalSector(uint32_t sector)
{
    /* FLASH_SECTOR_0..7 are consecutive on this family, so the index is the
       constant. Written as an explicit check rather than assumed, because the
       consequence of being wrong is erasing the wrong sector. */
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

    /* Refuse at the hardware boundary as well as in the map. A single check is
       enough when the code is correct; two are what stop a future caller
       reaching the erase with a sector the map would have rejected. */
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
    /* Voltage range sets the programming parallelism. RANGE_3 is 2.7-3.6 V,
       which is what a Nucleo supplies, and permits word-wide operations. */
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

        /* Assembled byte-wise rather than cast: the source may be an unaligned
           position inside a received block, and an unaligned 32-bit load faults
           on this core. */
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

        /* Read back. The HAL reports the controller's view; this confirms the
           cell actually took the value, which catches a worn or partially
           erased sector that would otherwise surface much later as a failed
           image hash. */
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
