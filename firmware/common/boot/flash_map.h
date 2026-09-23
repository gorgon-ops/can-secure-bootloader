/**
 * @file    flash_map.h
 * @brief   Flash geometry and the memory map, as plain data.
 *
 * The STM32F446RE has 512 KB of flash in **eight sectors of three different
 * sizes**, and that irregularity drives the whole layout: erase granularity is
 * a whole sector, so every region boundary has to fall on one. A layout that
 * ignores this ends up with a slot whose erase also wipes the bootloader.
 *
 *   0-3   16 KB each   0x08000000   bootloader
 *   4     64 KB        0x08010000   metadata: keys, counters, rollback floor
 *   5    128 KB        0x08020000   application slot A
 *   6    128 KB        0x08040000   application slot B
 *   7    128 KB        0x08060000   spare / staging
 *
 * Two slots, so an update never overwrites the only working image: a failed or
 * interrupted transfer leaves the previous version bootable.
 *
 * Pure module. No HAL, no register access, no side effects — it answers
 * questions about addresses, which makes the answers testable on a host where
 * getting them wrong costs nothing.
 */
#ifndef FLASH_MAP_H
#define FLASH_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLASH_BASE_ADDR    0x08000000UL
#define FLASH_TOTAL_SIZE   (512UL * 1024UL)
#define FLASH_SECTOR_COUNT 8U

/** Returned when an address does not lie in flash at all. */
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
    FLASH_REGION_NONE = 0, /*!< not in flash                        */
    FLASH_REGION_BOOTLOADER,
    FLASH_REGION_METADATA,
    FLASH_REGION_SLOT_A,
    FLASH_REGION_SLOT_B,
    FLASH_REGION_SPARE
} flash_region_t;

/** @brief Base address of a sector, or 0 if the index is out of range. */
uint32_t FlashMap_SectorBase(uint32_t index);

/** @brief Size of a sector in bytes, or 0 if the index is out of range. */
uint32_t FlashMap_SectorSize(uint32_t index);

/** @brief Sector containing @p addr, or FLASH_SECTOR_INVALID. */
uint32_t FlashMap_SectorOf(uint32_t addr);

/**
 * @brief Sectors spanned by a range.
 * @return false if the range leaves flash or has zero length.
 *
 * Used before an erase, so the caller can see exactly how much it is about to
 * destroy rather than discovering it afterwards.
 */
bool FlashMap_SectorRange(uint32_t addr, uint32_t len,
                          uint32_t *first, uint32_t *count);

/** @brief Which region an address belongs to. */
flash_region_t FlashMap_RegionOf(uint32_t addr);

/** @brief Base and size of an application slot. */
bool FlashMap_SlotBounds(uint8_t slot, uint32_t *base, uint32_t *size);

/**
 * @brief May this range be erased or programmed by an update?
 *
 * False for anything touching the bootloader or the metadata sector, and for
 * any range straddling a region boundary. This is the check that stops a
 * malformed or hostile length turning an application update into a bricked
 * board: the bootloader is the one thing that must survive a bad image, since
 * it is what recovers from one.
 */
bool FlashMap_IsUpdatable(uint32_t addr, uint32_t len);

/** @brief Human-readable region, for diagnostics and test output. */
const char *FlashMap_RegionName(flash_region_t r);

#endif /* FLASH_MAP_H */
