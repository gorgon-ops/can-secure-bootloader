/* Eight sectors of three different sizes, and erase is per sector, so every
 * region boundary has to fall on one:
 *
 *   0-3   16 KB each   0x08000000   bootloader
 *   4     64 KB        0x08010000   metadata: keys, counters, rollback floor
 *   5    128 KB        0x08020000   application slot A
 *   6    128 KB        0x08040000   application slot B
 *   7    128 KB        0x08060000   spare / staging */
#ifndef FLASH_MAP_H
#define FLASH_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLASH_BASE_ADDR    0x08000000UL
#define FLASH_TOTAL_SIZE   (512UL * 1024UL)
#define FLASH_SECTOR_COUNT 8U

#define FLASH_SECTOR_INVALID 0xFFFFFFFFUL

#define FLASH_BOOTLOADER_BASE 0x08000000UL
#define FLASH_BOOTLOADER_SIZE (64UL * 1024UL)

#define FLASH_METADATA_BASE 0x08010000UL
#define FLASH_METADATA_SIZE (64UL * 1024UL)

#define FLASH_SLOT_A_BASE 0x08020000UL
#define FLASH_SLOT_B_BASE 0x08040000UL
#define FLASH_SLOT_SIZE   (128UL * 1024UL)

#define FLASH_SPARE_BASE 0x08060000UL

#define FLASH_SLOT_A 0U
#define FLASH_SLOT_B 1U

typedef enum
{
    FLASH_REGION_NONE = 0,
    FLASH_REGION_BOOTLOADER,
    FLASH_REGION_METADATA,
    FLASH_REGION_SLOT_A,
    FLASH_REGION_SLOT_B,
    FLASH_REGION_SPARE
} flash_region_t;

uint32_t FlashMap_SectorBase(uint32_t index);
uint32_t FlashMap_SectorSize(uint32_t index);
uint32_t FlashMap_SectorOf(uint32_t addr);

bool FlashMap_SectorRange(uint32_t addr, uint32_t len,
                          uint32_t *first, uint32_t *count);

flash_region_t FlashMap_RegionOf(uint32_t addr);
bool FlashMap_SlotBounds(uint8_t slot, uint32_t *base, uint32_t *size);

/* False for anything touching the bootloader or metadata, or straddling a
 * region boundary: the bootloader is what recovers from a bad image, so it must
 * survive one. */
bool FlashMap_IsUpdatable(uint32_t addr, uint32_t len);

const char *FlashMap_RegionName(flash_region_t r);

#endif
