#include "flash_map.h"

/** The irregular sector table is the whole reason this module exists. */
static const uint32_t k_sector_size[FLASH_SECTOR_COUNT] = {
    16UL * 1024UL,  /* 0 */
    16UL * 1024UL,  /* 1 */
    16UL * 1024UL,  /* 2 */
    16UL * 1024UL,  /* 3 */
    64UL * 1024UL,  /* 4 */
    128UL * 1024UL, /* 5 */
    128UL * 1024UL, /* 6 */
    128UL * 1024UL  /* 7 */
};

uint32_t FlashMap_SectorBase(uint32_t index)
{
    uint32_t base = FLASH_BASE_ADDR;
    uint32_t i;

    if (index >= FLASH_SECTOR_COUNT)
    {
        return 0UL;
    }

    for (i = 0U; i < index; i++)
    {
        base += k_sector_size[i];
    }
    return base;
}

uint32_t FlashMap_SectorSize(uint32_t index)
{
    if (index >= FLASH_SECTOR_COUNT)
    {
        return 0UL;
    }
    return k_sector_size[index];
}

uint32_t FlashMap_SectorOf(uint32_t addr)
{
    uint32_t base = FLASH_BASE_ADDR;
    uint32_t i;

    if ((addr < FLASH_BASE_ADDR) ||
        (addr >= (FLASH_BASE_ADDR + FLASH_TOTAL_SIZE)))
    {
        return FLASH_SECTOR_INVALID;
    }

    for (i = 0U; i < FLASH_SECTOR_COUNT; i++)
    {
        if (addr < (base + k_sector_size[i]))
        {
            return i;
        }
        base += k_sector_size[i];
    }

    return FLASH_SECTOR_INVALID;
}

bool FlashMap_SectorRange(uint32_t addr, uint32_t len,
                          uint32_t *first, uint32_t *count)
{
    uint32_t last_addr;
    uint32_t s_first;
    uint32_t s_last;

    if ((len == 0U) || (first == NULL) || (count == NULL))
    {
        return false;
    }

    /* 64-bit intermediate: addr + len can wrap a 32-bit value, and a wrapped
       comparison would report a range far past the end of flash as being
       comfortably inside it. */
    if (((uint64_t)addr + (uint64_t)len) >
        ((uint64_t)FLASH_BASE_ADDR + (uint64_t)FLASH_TOTAL_SIZE))
    {
        return false;
    }

    last_addr = addr + len - 1U;
    s_first   = FlashMap_SectorOf(addr);
    s_last    = FlashMap_SectorOf(last_addr);

    if ((s_first == FLASH_SECTOR_INVALID) || (s_last == FLASH_SECTOR_INVALID))
    {
        return false;
    }

    *first = s_first;
    *count = (s_last - s_first) + 1U;
    return true;
}

flash_region_t FlashMap_RegionOf(uint32_t addr)
{
    if ((addr < FLASH_BASE_ADDR) ||
        (addr >= (FLASH_BASE_ADDR + FLASH_TOTAL_SIZE)))
    {
        return FLASH_REGION_NONE;
    }
    if (addr < (FLASH_BOOTLOADER_BASE + FLASH_BOOTLOADER_SIZE))
    {
        return FLASH_REGION_BOOTLOADER;
    }
    if (addr < (FLASH_METADATA_BASE + FLASH_METADATA_SIZE))
    {
        return FLASH_REGION_METADATA;
    }
    if (addr < (FLASH_SLOT_A_BASE + FLASH_SLOT_SIZE))
    {
        return FLASH_REGION_SLOT_A;
    }
    if (addr < (FLASH_SLOT_B_BASE + FLASH_SLOT_SIZE))
    {
        return FLASH_REGION_SLOT_B;
    }
    return FLASH_REGION_SPARE;
}

bool FlashMap_SlotBounds(uint8_t slot, uint32_t *base, uint32_t *size)
{
    if ((base == NULL) || (size == NULL))
    {
        return false;
    }

    if (slot == (uint8_t)FLASH_SLOT_A)
    {
        *base = FLASH_SLOT_A_BASE;
    }
    else if (slot == (uint8_t)FLASH_SLOT_B)
    {
        *base = FLASH_SLOT_B_BASE;
    }
    else
    {
        return false;
    }

    *size = FLASH_SLOT_SIZE;
    return true;
}

bool FlashMap_IsUpdatable(uint32_t addr, uint32_t len)
{
    flash_region_t start;
    flash_region_t end;

    if (len == 0U)
    {
        return false;
    }

    if (((uint64_t)addr + (uint64_t)len) >
        ((uint64_t)FLASH_BASE_ADDR + (uint64_t)FLASH_TOTAL_SIZE))
    {
        return false;
    }

    start = FlashMap_RegionOf(addr);
    end   = FlashMap_RegionOf(addr + len - 1U);

    /* A range that begins in a slot and ends in another region would erase
       across the boundary. Requiring both ends in the same region is stricter
       than checking only the start, and it is the cheap way to make a
       straddling length impossible rather than merely unlikely. */
    if (start != end)
    {
        return false;
    }

    /* The bootloader is what recovers from a bad image, so it must survive one.
       Metadata holds the keys and counters an update is verified against, and
       letting an update rewrite them would let a bad image authorise itself. */
    return (start == FLASH_REGION_SLOT_A) ||
           (start == FLASH_REGION_SLOT_B) ||
           (start == FLASH_REGION_SPARE);
}

const char *FlashMap_RegionName(flash_region_t r)
{
    switch (r)
    {
        case FLASH_REGION_BOOTLOADER: return "BOOTLOADER";
        case FLASH_REGION_METADATA:   return "METADATA";
        case FLASH_REGION_SLOT_A:     return "SLOT_A";
        case FLASH_REGION_SLOT_B:     return "SLOT_B";
        case FLASH_REGION_SPARE:      return "SPARE";
        default:                      return "NONE";
    }
}
