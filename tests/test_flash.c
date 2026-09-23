/**
 * Host-side tests for the flash map and write sequencer.
 *
 * The property that matters most is negative: an update must never erase or
 * program the bootloader or the metadata sector. The bootloader is what
 * recovers from a bad image, so it has to survive one; metadata holds the keys
 * and counters an image is judged against, so an update that could rewrite them
 * could authorise itself.
 *
 * The fake hardware below records every erase and program, which lets the tests
 * assert what was *not* touched rather than only what was.
 *
 * Builds with plain gcc. No HAL, no hardware.
 */
#include "test_util.h"

#include "flash_map.h"
#include "flash_writer.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Flash geometry                                                      */
/* ------------------------------------------------------------------ */

static void test_sector_table_covers_the_device_exactly(void)
{
    uint32_t total = 0;
    uint32_t i;

    /* An error here misplaces every region boundary, so it is worth checking
       the table sums to the part rather than trusting it. */
    for (i = 0; i < FLASH_SECTOR_COUNT; i++) {
        CHECK(FlashMap_SectorSize(i) > 0);
        total += FlashMap_SectorSize(i);
    }
    CHECK_EQ(total, FLASH_TOTAL_SIZE);

    /* And that bases are contiguous. */
    for (i = 1; i < FLASH_SECTOR_COUNT; i++) {
        CHECK_EQ(FlashMap_SectorBase(i),
                 FlashMap_SectorBase(i - 1) + FlashMap_SectorSize(i - 1));
    }
}

static void test_irregular_sector_sizes(void)
{
    /* The three sizes are the reason this module exists: assuming uniform
       sectors puts every boundary after sector 3 in the wrong place. */
    CHECK_EQ(FlashMap_SectorSize(0), 16 * 1024);
    CHECK_EQ(FlashMap_SectorSize(3), 16 * 1024);
    CHECK_EQ(FlashMap_SectorSize(4), 64 * 1024);
    CHECK_EQ(FlashMap_SectorSize(5), 128 * 1024);
    CHECK_EQ(FlashMap_SectorSize(7), 128 * 1024);

    CHECK_EQ(FlashMap_SectorBase(4), FLASH_METADATA_BASE);
    CHECK_EQ(FlashMap_SectorBase(5), FLASH_SLOT_A_BASE);
    CHECK_EQ(FlashMap_SectorBase(6), FLASH_SLOT_B_BASE);
    CHECK_EQ(FlashMap_SectorBase(7), FLASH_SPARE_BASE);
}

static void test_address_to_sector(void)
{
    CHECK_EQ(FlashMap_SectorOf(FLASH_BASE_ADDR), 0);
    CHECK_EQ(FlashMap_SectorOf(FLASH_BASE_ADDR + 0x3FFF), 0);
    CHECK_EQ(FlashMap_SectorOf(FLASH_BASE_ADDR + 0x4000), 1);
    CHECK_EQ(FlashMap_SectorOf(FLASH_SLOT_A_BASE), 5);
    CHECK_EQ(FlashMap_SectorOf(FLASH_SLOT_A_BASE + FLASH_SLOT_SIZE - 1), 5);
    CHECK_EQ(FlashMap_SectorOf(FLASH_SLOT_B_BASE), 6);

    /* Outside the device entirely. */
    CHECK_EQ(FlashMap_SectorOf(FLASH_BASE_ADDR - 1), FLASH_SECTOR_INVALID);
    CHECK_EQ(FlashMap_SectorOf(FLASH_BASE_ADDR + FLASH_TOTAL_SIZE),
             FLASH_SECTOR_INVALID);
    CHECK_EQ(FlashMap_SectorOf(0x20000000), FLASH_SECTOR_INVALID); /* RAM */
}

static void test_slot_occupies_exactly_one_sector(void)
{
    uint32_t first = 0, count = 0;

    /* If a slot spanned two sectors the erase would be twice as slow and twice
       as easy to get wrong. The layout is chosen so it does not. */
    CHECK(FlashMap_SectorRange(FLASH_SLOT_A_BASE, FLASH_SLOT_SIZE, &first, &count));
    CHECK_EQ(first, 5);
    CHECK_EQ(count, 1);

    CHECK(FlashMap_SectorRange(FLASH_SLOT_B_BASE, FLASH_SLOT_SIZE, &first, &count));
    CHECK_EQ(first, 6);
    CHECK_EQ(count, 1);
}

static void test_sector_range_rejects_overflow(void)
{
    uint32_t first = 0, count = 0;

    /* A length that wraps 32 bits would, with naive arithmetic, look like a
       tiny range comfortably inside flash. */
    CHECK(!FlashMap_SectorRange(FLASH_SLOT_A_BASE, 0xFFFFFFFFUL, &first, &count));
    CHECK(!FlashMap_SectorRange(FLASH_SLOT_A_BASE, 0, &first, &count));
    CHECK(!FlashMap_SectorRange(FLASH_BASE_ADDR + FLASH_TOTAL_SIZE, 4,
                                &first, &count));
}

static void test_regions(void)
{
    CHECK_EQ(FlashMap_RegionOf(0x08000000), FLASH_REGION_BOOTLOADER);
    CHECK_EQ(FlashMap_RegionOf(0x0800FFFF), FLASH_REGION_BOOTLOADER);
    CHECK_EQ(FlashMap_RegionOf(0x08010000), FLASH_REGION_METADATA);
    CHECK_EQ(FlashMap_RegionOf(0x0801FFFF), FLASH_REGION_METADATA);
    CHECK_EQ(FlashMap_RegionOf(0x08020000), FLASH_REGION_SLOT_A);
    CHECK_EQ(FlashMap_RegionOf(0x08040000), FLASH_REGION_SLOT_B);
    CHECK_EQ(FlashMap_RegionOf(0x08060000), FLASH_REGION_SPARE);
    CHECK_EQ(FlashMap_RegionOf(0x20000000), FLASH_REGION_NONE);
}

/**
 * The central safety property of the whole module.
 */
static void test_bootloader_and_metadata_are_never_updatable(void)
{
    /* The slots and the spare are fair game. */
    CHECK(FlashMap_IsUpdatable(FLASH_SLOT_A_BASE, FLASH_SLOT_SIZE));
    CHECK(FlashMap_IsUpdatable(FLASH_SLOT_B_BASE, FLASH_SLOT_SIZE));
    CHECK(FlashMap_IsUpdatable(FLASH_SPARE_BASE, 4));

    /* The bootloader is not, at any address within it. */
    CHECK(!FlashMap_IsUpdatable(FLASH_BOOTLOADER_BASE, 4));
    CHECK(!FlashMap_IsUpdatable(FLASH_BOOTLOADER_BASE + 0x8000, 4));
    CHECK(!FlashMap_IsUpdatable(FLASH_BOOTLOADER_BASE, FLASH_BOOTLOADER_SIZE));

    /* Nor is metadata: an update able to rewrite the keys and the rollback
       floor could authorise itself. */
    CHECK(!FlashMap_IsUpdatable(FLASH_METADATA_BASE, 4));
    CHECK(!FlashMap_IsUpdatable(FLASH_METADATA_BASE + 0x100, 64));

    /* A range that begins in a slot but runs past its end into the next region
       is refused rather than clipped. */
    CHECK(!FlashMap_IsUpdatable(FLASH_SLOT_A_BASE, FLASH_SLOT_SIZE + 1));
    CHECK(!FlashMap_IsUpdatable(FLASH_METADATA_BASE + FLASH_METADATA_SIZE - 4, 8));

    /* And one that starts below the slot and reaches into it. */
    CHECK(!FlashMap_IsUpdatable(FLASH_METADATA_BASE, FLASH_METADATA_SIZE + 4));

    CHECK(!FlashMap_IsUpdatable(FLASH_SLOT_A_BASE, 0));
    CHECK(!FlashMap_IsUpdatable(0x20000000, 4));
}

/* ------------------------------------------------------------------ */
/* Fake hardware                                                       */
/* ------------------------------------------------------------------ */

#define FAKE_MAX_OPS 64

typedef struct
{
    uint32_t erased[FAKE_MAX_OPS];
    int      erase_count;

    uint32_t prog_addr[FAKE_MAX_OPS];
    uint32_t prog_len[FAKE_MAX_OPS];
    uint8_t  prog_data[FAKE_MAX_OPS][8];
    int      prog_count;

    bool fail_erase;
    bool fail_program;
} fake_flash_t;

static bool fake_erase(void *ctx, uint32_t sector)
{
    fake_flash_t *f = (fake_flash_t *)ctx;
    if (f->fail_erase) {
        return false;
    }
    if (f->erase_count < FAKE_MAX_OPS) {
        f->erased[f->erase_count] = sector;
    }
    f->erase_count++;
    return true;
}

static bool fake_program(void *ctx, uint32_t addr, const uint8_t *data,
                         uint32_t len)
{
    fake_flash_t *f = (fake_flash_t *)ctx;
    if (f->fail_program) {
        return false;
    }
    if (f->prog_count < FAKE_MAX_OPS) {
        f->prog_addr[f->prog_count] = addr;
        f->prog_len[f->prog_count]  = len;
        memcpy(f->prog_data[f->prog_count], data, (len > 8) ? 8 : len);
    }
    f->prog_count++;
    return true;
}

typedef struct
{
    flash_writer_t w;
    fake_flash_t   hw;
} wfix_t;

static void wsetup(wfix_t *f, uint8_t active_slot)
{
    flash_ops_t ops;

    memset(f, 0, sizeof(*f));
    ops.erase_sector = fake_erase;
    ops.program      = fake_program;
    ops.ctx          = &f->hw;

    FlashWriter_SetOps(&f->w, &ops);
    FlashWriter_Init(&f->w, active_slot);
    FlashWriter_SetOps(&f->w, &ops);
}

static bool erased_sector(const fake_flash_t *hw, uint32_t sector)
{
    int i;
    for (i = 0; i < hw->erase_count; i++) {
        if (hw->erased[i] == sector) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Write sequencing                                                    */
/* ------------------------------------------------------------------ */

static void test_begin_erases_only_the_target_slot(void)
{
    wfix_t f;
    wsetup(&f, FLASH_SLOT_A);

    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, 1024), FLASH_WRITE_OK);

    /* Exactly one sector, and it is slot B's. Nothing else was touched --
       which is the assertion that matters, not that the right one was. */
    CHECK_EQ(f.hw.erase_count, 1);
    CHECK(erased_sector(&f.hw, 6));
    CHECK(!erased_sector(&f.hw, 0));
    CHECK(!erased_sector(&f.hw, 1));
    CHECK(!erased_sector(&f.hw, 2));
    CHECK(!erased_sector(&f.hw, 3));
    CHECK(!erased_sector(&f.hw, 4));   /* metadata */
    CHECK(!erased_sector(&f.hw, 5));   /* the running slot */
    CHECK(!erased_sector(&f.hw, 7));
}

static void test_refuses_to_erase_the_running_slot(void)
{
    wfix_t f;
    wsetup(&f, FLASH_SLOT_A);

    /* Erasing the code currently executing does not end well. The two-slot
       layout exists so this never has to happen. */
    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_A, 1024),
             FLASH_WRITE_ERR_ACTIVE_SLOT);
    CHECK_EQ(f.hw.erase_count, 0);
}

static void test_refuses_an_image_larger_than_the_slot(void)
{
    wfix_t f;
    wsetup(&f, FLASH_SLOT_A);

    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, FLASH_SLOT_SIZE + 1),
             FLASH_WRITE_ERR_TOO_LARGE);
    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, 0),
             FLASH_WRITE_ERR_TOO_LARGE);

    /* Nothing erased on a rejected begin: a refused update must leave the
       previous image intact. */
    CHECK_EQ(f.hw.erase_count, 0);

    /* The exact slot size is legitimate. */
    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, FLASH_SLOT_SIZE),
             FLASH_WRITE_OK);
}

static void test_unknown_slot_is_refused(void)
{
    wfix_t f;
    wsetup(&f, FLASH_SLOT_A);

    CHECK_EQ(FlashWriter_Begin(&f.w, 7, 1024), FLASH_WRITE_ERR_BAD_SLOT);
    CHECK_EQ(f.hw.erase_count, 0);
}

static void test_aligned_writes_program_words(void)
{
    wfix_t f;
    uint8_t data[16];
    int i;

    for (i = 0; i < 16; i++) {
        data[i] = (uint8_t)i;
    }

    wsetup(&f, FLASH_SLOT_A);
    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, 16), FLASH_WRITE_OK);
    CHECK_EQ(FlashWriter_Write(&f.w, data, 16), FLASH_WRITE_OK);
    CHECK_EQ(FlashWriter_Finish(&f.w), FLASH_WRITE_OK);

    CHECK_EQ(f.hw.prog_count, 4);
    CHECK_EQ(f.hw.prog_addr[0], FLASH_SLOT_B_BASE);
    CHECK_EQ(f.hw.prog_addr[1], FLASH_SLOT_B_BASE + 4);
    CHECK_EQ(f.hw.prog_addr[3], FLASH_SLOT_B_BASE + 12);
    CHECK_EQ(f.hw.prog_len[0], FLASH_WRITE_ALIGN);
}

/**
 * The case that actually happens. A tester chooses its own block size, so
 * chunks are almost never multiples of four.
 */
static void test_unaligned_chunks_are_buffered(void)
{
    wfix_t f;
    uint8_t data[16];
    uint32_t addr;
    int i;

    for (i = 0; i < 16; i++) {
        data[i] = (uint8_t)(0xA0 + i);
    }

    wsetup(&f, FLASH_SLOT_A);
    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, 16), FLASH_WRITE_OK);

    /* Deliberately awkward: 3, then 7, then 6. */
    CHECK_EQ(FlashWriter_Write(&f.w, &data[0], 3), FLASH_WRITE_OK);
    CHECK_EQ(FlashWriter_Write(&f.w, &data[3], 7), FLASH_WRITE_OK);
    CHECK_EQ(FlashWriter_Write(&f.w, &data[10], 6), FLASH_WRITE_OK);
    CHECK_EQ(FlashWriter_Finish(&f.w), FLASH_WRITE_OK);

    /* Every program must still land on a word boundary, in order, with no
       gaps and no repeats -- flash cannot set bits back, so a rewritten
       address silently corrupts rather than failing. */
    CHECK_EQ(f.hw.prog_count, 4);
    for (i = 0; i < f.hw.prog_count; i++) {
        addr = FLASH_SLOT_B_BASE + (uint32_t)(i * 4);
        CHECK_EQ(f.hw.prog_addr[i], addr);
        CHECK_EQ(f.hw.prog_len[i], FLASH_WRITE_ALIGN);
        CHECK_EQ(f.hw.prog_data[i][0], data[i * 4]);
        CHECK_EQ(f.hw.prog_data[i][3], data[(i * 4) + 3]);
    }
}

static void test_tail_is_padded_with_erased_value(void)
{
    wfix_t f;
    uint8_t data[6] = { 1, 2, 3, 4, 5, 6 };

    wsetup(&f, FLASH_SLOT_A);
    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, 6), FLASH_WRITE_OK);
    CHECK_EQ(FlashWriter_Write(&f.w, data, 6), FLASH_WRITE_OK);

    /* Two bytes are still pending; nothing has programmed them yet. */
    CHECK_EQ(f.hw.prog_count, 1);

    CHECK_EQ(FlashWriter_Finish(&f.w), FLASH_WRITE_OK);
    CHECK_EQ(f.hw.prog_count, 2);

    /* Padding is 0xFF, which is what an erased cell already holds, so it
       programs nothing and cannot disturb a later write. */
    CHECK_EQ(f.hw.prog_data[1][0], 5);
    CHECK_EQ(f.hw.prog_data[1][1], 6);
    CHECK_EQ(f.hw.prog_data[1][2], 0xFF);
    CHECK_EQ(f.hw.prog_data[1][3], 0xFF);
}

static void test_write_beyond_declared_length_is_refused(void)
{
    wfix_t f;
    uint8_t data[32];

    memset(data, 0x5A, sizeof(data));
    wsetup(&f, FLASH_SLOT_A);
    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, 16), FLASH_WRITE_OK);

    CHECK_EQ(FlashWriter_Write(&f.w, data, 16), FLASH_WRITE_OK);

    /* One byte more than promised. Accepting it would write past the declared
       image and, with a large enough overrun, past the slot. */
    CHECK_EQ(FlashWriter_Write(&f.w, data, 1), FLASH_WRITE_ERR_OVERRUN);
    CHECK_EQ(FlashWriter_Written(&f.w), 16);
}

static void test_a_single_oversized_chunk_is_refused(void)
{
    wfix_t f;
    static uint8_t big[FLASH_SLOT_SIZE];

    wsetup(&f, FLASH_SLOT_A);
    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, 64), FLASH_WRITE_OK);
    CHECK_EQ(FlashWriter_Write(&f.w, big, FLASH_SLOT_SIZE),
             FLASH_WRITE_ERR_OVERRUN);
    CHECK_EQ(f.hw.prog_count, 0);
}

static void test_write_without_begin_is_refused(void)
{
    wfix_t f;
    uint8_t data[4] = { 1, 2, 3, 4 };

    wsetup(&f, FLASH_SLOT_A);
    CHECK_EQ(FlashWriter_Write(&f.w, data, 4), FLASH_WRITE_ERR_NOT_OPEN);
    CHECK_EQ(FlashWriter_Finish(&f.w), FLASH_WRITE_ERR_NOT_OPEN);
    CHECK_EQ(f.hw.prog_count, 0);
}

static void test_short_transfer_is_reported_as_incomplete(void)
{
    wfix_t f;
    uint8_t data[8];

    memset(data, 0x33, sizeof(data));
    wsetup(&f, FLASH_SLOT_A);
    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, 64), FLASH_WRITE_OK);
    CHECK_EQ(FlashWriter_Write(&f.w, data, 8), FLASH_WRITE_OK);

    /* The image would fail verification anyway, since the hash covers the
       whole payload. Reporting it here names the real cause rather than
       leaving it to look like a corrupted image. */
    CHECK_EQ(FlashWriter_Finish(&f.w), FLASH_WRITE_ERR_INCOMPLETE);
}

static void test_erase_failure_leaves_the_writer_closed(void)
{
    wfix_t f;
    uint8_t data[4] = { 1, 2, 3, 4 };

    wsetup(&f, FLASH_SLOT_A);
    f.hw.fail_erase = true;

    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, 64),
             FLASH_WRITE_ERR_ERASE_FAILED);

    /* A failed erase must not leave a half-open writer that then programs into
       a sector still holding the old image. */
    CHECK_EQ(FlashWriter_Write(&f.w, data, 4), FLASH_WRITE_ERR_NOT_OPEN);
}

static void test_program_failure_is_reported(void)
{
    wfix_t f;
    uint8_t data[8];

    memset(data, 0x77, sizeof(data));
    wsetup(&f, FLASH_SLOT_A);
    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, 8), FLASH_WRITE_OK);

    f.hw.fail_program = true;
    CHECK_EQ(FlashWriter_Write(&f.w, data, 8), FLASH_WRITE_ERR_PROGRAM_FAILED);
}

static void test_abort_prevents_further_writes(void)
{
    wfix_t f;
    uint8_t data[4] = { 9, 9, 9, 9 };

    wsetup(&f, FLASH_SLOT_A);
    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, 64), FLASH_WRITE_OK);
    FlashWriter_Abort(&f.w);

    CHECK_EQ(FlashWriter_Write(&f.w, data, 4), FLASH_WRITE_ERR_NOT_OPEN);
    CHECK_EQ(FlashWriter_Written(&f.w), 0);
}

static void test_full_slot_image_programs_within_bounds(void)
{
    wfix_t f;
    static uint8_t chunk[256];
    uint32_t total = 0;
    int i;

    memset(chunk, 0xC3, sizeof(chunk));
    wsetup(&f, FLASH_SLOT_A);
    CHECK_EQ(FlashWriter_Begin(&f.w, FLASH_SLOT_B, 4096), FLASH_WRITE_OK);

    for (i = 0; i < 16; i++) {
        CHECK_EQ(FlashWriter_Write(&f.w, chunk, 256), FLASH_WRITE_OK);
        total += 256;
    }
    CHECK_EQ(FlashWriter_Finish(&f.w), FLASH_WRITE_OK);
    CHECK_EQ(FlashWriter_Written(&f.w), total);

    /* Every recorded program must lie inside slot B. */
    for (i = 0; i < f.hw.prog_count && i < FAKE_MAX_OPS; i++) {
        CHECK(f.hw.prog_addr[i] >= FLASH_SLOT_B_BASE);
        CHECK((f.hw.prog_addr[i] + f.hw.prog_len[i]) <=
              (FLASH_SLOT_B_BASE + FLASH_SLOT_SIZE));
    }
}

int main(void)
{
    test_sector_table_covers_the_device_exactly();
    test_irregular_sector_sizes();
    test_address_to_sector();
    test_slot_occupies_exactly_one_sector();
    test_sector_range_rejects_overflow();
    test_regions();
    test_bootloader_and_metadata_are_never_updatable();

    test_begin_erases_only_the_target_slot();
    test_refuses_to_erase_the_running_slot();
    test_refuses_an_image_larger_than_the_slot();
    test_unknown_slot_is_refused();
    test_aligned_writes_program_words();
    test_unaligned_chunks_are_buffered();
    test_tail_is_padded_with_erased_value();
    test_write_beyond_declared_length_is_refused();
    test_a_single_oversized_chunk_is_refused();
    test_write_without_begin_is_refused();
    test_short_transfer_is_reported_as_incomplete();
    test_erase_failure_leaves_the_writer_closed();
    test_program_failure_is_reported();
    test_abort_prevents_further_writes();
    test_full_slot_image_programs_within_bounds();

    return test_report();
}
