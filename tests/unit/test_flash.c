/* SPDX-License-Identifier: MIT */
/*
 * The flash interface (lib/flash/flash.h) on the NOR model (lib/flash/nor.h), in the order of
 * the headers: the geometry and set-up; every rule of flash.h (the range, the alignment and the
 * page, write-once, AND, the erase, the order of the checks); the counters and the wear; the
 * time of each operation; and the power cuts: which operation is cut, every position within a
 * program and within an erase, the seeded bits (the same seed gives the same result, across
 * seeds every outcome the rule allows occurs and none it forbids, and the result is the one
 * the procedure written in nor.h gives, recomputed here from that text), and the loss of
 * supply until nor_power_on().
 *
 * Most tests run on a small device whose sizes all differ, so that a size used in the place of
 * another shows: 4 sectors of 64 bytes, pages of 16 bytes, units of 4 bytes (256 bytes in
 * all), with a page program of 400 us and a sector erase of 45 ms, the figures the host uses.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "flash/flash.h"
#include "flash/nor.h"
#include "unity.h"

#define SIZE    256U
#define SECTOR  64U
#define PAGE    16U
#define UNIT    4U
#define SECTORS (SIZE / SECTOR)
#define T_PP    400000U
#define T_SE    45000000U

static const flash_geometry_t GEOMETRY = {
    .size = SIZE, .sector_size = SECTOR, .page_size = PAGE, .unit_size = UNIT};
static const nor_timing_t TIMING = {.page_program_ns = T_PP, .sector_erase_ns = T_SE};

static nor_t s_nor;
static uint8_t s_mem[SIZE];
static uint32_t s_counts[SECTORS];
static uint8_t s_before[SIZE]; /* the array as a test saw it before a call */

void setUp(void)
{
    (void)memset(s_mem, 0xFF, sizeof(s_mem));
    /* Nonsense first: nor_init() must need nothing from the memory it is given. */
    (void)memset(&s_nor, 0xA5, sizeof(s_nor));
    (void)memset(s_counts, 0xA5, sizeof(s_counts));
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------------------- */

static const char *at(const char *what, unsigned long value)
{
    static char text[96];

    (void)snprintf(text, sizeof(text), "%s %lu", what, value);
    return text;
}

static nor_config_t config_of(uint8_t *memory, uint32_t *counts)
{
    const nor_config_t config = {
        .geometry = GEOMETRY, .timing = TIMING, .memory = memory, .erase_counts = counts};

    return config;
}

static void start(void)
{
    const nor_config_t config = config_of(s_mem, s_counts);

    TEST_ASSERT_TRUE(nor_init(&s_nor, &config));
}

static void snapshot(void)
{
    (void)memcpy(s_before, s_mem, sizeof(s_mem));
}

static void assert_unchanged(void)
{
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_before, s_mem, SIZE);
}

static nor_stats_t stats(void)
{
    nor_stats_t out;

    (void)memset(&out, 0, sizeof(out));
    nor_get_stats(&s_nor, &out);
    return out;
}

static nor_cut_t last_cut(void)
{
    nor_cut_t cut;

    (void)memset(&cut, 0x5A, sizeof(cut));
    TEST_ASSERT_TRUE(nor_last_cut(&s_nor, &cut));
    return cut;
}

static uint32_t popcount8(uint32_t byte)
{
    uint32_t count = 0U;

    for (uint32_t bit = 0U; bit < 8U; bit++) {
        count += (byte >> bit) & 1U;
    }

    return count;
}

/* A page of data whose every unit clears some bits and keeps others. */
static void pattern(uint8_t *out, uint32_t len, uint32_t salt)
{
    for (uint32_t i = 0U; i < len; i++) {
        out[i] = (uint8_t)(((i * 37U) + (salt * 101U) + 0x15U) & 0xFFU);
    }
}

/* ------------------------------------------------------------------------------------------
 * Geometry and set-up
 * ---------------------------------------------------------------------------------------- */

static void test_the_geometries_flash_h_allows_are_valid_and_no_other(void)
{
    const struct {
        flash_geometry_t geometry;
        bool valid;
    } cases[] = {
        {{256U, 64U, 16U, 4U}, true},
        {{0x202000U, 4096U, 256U, 8U}, true}, /* the host's flash */
        {{4096U, 4096U, 4096U, 4096U}, true}, /* one sector, one page, one unit */
        {{1U, 1U, 1U, 1U}, true},
        {{0x10000000U, 0x10000000U, 256U, 1U}, true}, /* the largest sector */
        {{0U, 64U, 16U, 4U}, false},                  /* no bytes */
        {{320U, 64U, 16U, 4U}, true},                 /* size: any multiple of the sector */
        {{288U, 64U, 16U, 4U}, false},                /* not a multiple of the sector */
        {{256U, 48U, 16U, 4U}, false},                /* sector not a power of two */
        {{256U, 64U, 12U, 4U}, false},                /* page not a power of two */
        {{256U, 64U, 16U, 3U}, false},                /* unit not a power of two */
        {{256U, 64U, 16U, 0U}, false},
        {{256U, 64U, 0U, 4U}, false},
        {{256U, 0U, 16U, 4U}, false},
        {{256U, 64U, 128U, 4U}, false},               /* page larger than the sector */
        {{256U, 64U, 16U, 32U}, false},               /* unit larger than the page */
        {{0x20000000U, 0x20000000U, 256U, 1U}, false} /* sector too large for a bit count */
    };

    for (uint32_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].valid, nor_geometry_valid(&cases[i].geometry),
                                  at("case", i));
    }

    TEST_ASSERT_FALSE(nor_geometry_valid(NULL));
}

static void test_init_keeps_the_memory_and_zeroes_every_count(void)
{
    pattern(s_mem, SIZE, 7U);
    snapshot();
    start();

    assert_unchanged();
    TEST_ASSERT_TRUE(nor_powered(&s_nor));
    TEST_ASSERT_EQUAL_UINT32(0U, nor_cut_countdown(&s_nor));
    TEST_ASSERT_EQUAL_UINT32(0U, nor_last_duration_ns(&s_nor));

    for (uint32_t sector = 0U; sector < SECTORS; sector++) {
        TEST_ASSERT_EQUAL_UINT32(0U, s_counts[sector]);
        TEST_ASSERT_EQUAL_UINT32(0U, nor_sector_erases(&s_nor, sector));
    }

    const nor_stats_t counted = stats();

    TEST_ASSERT_EQUAL_UINT64(0U, counted.reads + counted.bytes_read + counted.programs +
                                     counted.units_programmed + counted.erases + counted.refused +
                                     counted.unpowered + counted.busy_ns);
    TEST_ASSERT_EQUAL_UINT32(0U, counted.cuts);

    nor_cut_t cut;

    TEST_ASSERT_FALSE(nor_last_cut(&s_nor, &cut));
    TEST_ASSERT_EQUAL_INT(NOR_OP_NONE, cut.op);
}

/* A refused nor_init() leaves a model without supply that no power-on brings back. */
static void assert_unusable(void)
{
    uint8_t byte = 0U;

    TEST_ASSERT_FALSE(nor_powered(&s_nor));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_read(&s_nor, 0U, &byte, 1U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(&s_nor, 0U, s_mem, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, 0U));
    nor_power_on(&s_nor);
    TEST_ASSERT_FALSE(nor_powered(&s_nor));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_read(&s_nor, 0U, &byte, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, nor_sector_erases(&s_nor, 0U));
}

static void test_init_refuses_what_it_cannot_model(void)
{
    nor_config_t config = config_of(s_mem, s_counts);

    TEST_ASSERT_FALSE(nor_init(NULL, &config));
    TEST_ASSERT_FALSE(nor_init(&s_nor, NULL));
    assert_unusable();

    config.memory = NULL;
    TEST_ASSERT_FALSE(nor_init(&s_nor, &config));
    assert_unusable();

    config = config_of(s_mem, NULL);
    TEST_ASSERT_FALSE(nor_init(&s_nor, &config));
    assert_unusable();

    config = config_of(s_mem, s_counts);
    config.geometry.page_size = 12U;
    TEST_ASSERT_FALSE(nor_init(&s_nor, &config));
    assert_unusable();
}

static void test_the_device_carries_the_geometry_the_model_and_its_operations(void)
{
    flash_dev_t dev;
    uint8_t data[UNIT] = {0x12U, 0x34U, 0x56U, 0x78U};
    uint8_t back[UNIT] = {0U};

    start();
    (void)memset(&dev, 0, sizeof(dev));
    nor_device(&s_nor, &dev);

    TEST_ASSERT_EQUAL_UINT32(SIZE, dev.geometry.size);
    TEST_ASSERT_EQUAL_UINT32(SECTOR, dev.geometry.sector_size);
    TEST_ASSERT_EQUAL_UINT32(PAGE, dev.geometry.page_size);
    TEST_ASSERT_EQUAL_UINT32(UNIT, dev.geometry.unit_size);
    TEST_ASSERT_EQUAL_PTR(&s_nor, dev.ctx);

    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev.program(dev.ctx, 2U * PAGE, data, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev.read(dev.ctx, 2U * PAGE, back, UNIT));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, back, UNIT);
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev.erase(dev.ctx, 0U));
    TEST_ASSERT_EQUAL_HEX8(0xFFU, s_mem[(size_t)2U * PAGE]);
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, dev.erase(dev.ctx, 1U));

    /* NULL arguments change nothing and do not crash. */
    nor_device(NULL, &dev);
    nor_device(&s_nor, NULL);
    TEST_ASSERT_EQUAL_PTR(&s_nor, dev.ctx);
}

/* ------------------------------------------------------------------------------------------
 * The rules of flash.h
 * ---------------------------------------------------------------------------------------- */

static void test_erased_bytes_read_ff_and_any_range_inside_reads(void)
{
    uint8_t back[SIZE];

    start();
    (void)memset(back, 0, sizeof(back));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_read(&s_nor, 0U, back, SIZE));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xFFU, back, SIZE);

    pattern(s_mem, SIZE, 3U);

    /* Reads need no alignment: odd addresses and lengths, across pages and sectors. */
    for (uint32_t addr = 0U; addr < SIZE; addr += 13U) {
        for (uint32_t len = 1U; len <= (SIZE - addr); len += 29U) {
            (void)memset(back, 0, sizeof(back));
            TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_read(&s_nor, addr, back, len));
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(&s_mem[addr], back, len, at("address", addr));
        }
    }

    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_read(&s_nor, SIZE - 1U, back, 1U));
    TEST_ASSERT_EQUAL_HEX8(s_mem[SIZE - 1U], back[0]);
}

static void test_a_call_outside_the_device_or_of_no_bytes_is_refused_and_changes_nothing(void)
{
    uint8_t back[8] = {0U};
    const uint8_t data[UNIT] = {0U};

    start();
    snapshot();

    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_read(&s_nor, 0U, back, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_read(&s_nor, SIZE, back, 1U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_read(&s_nor, SIZE - 2U, back, 3U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_read(&s_nor, SIZE - 1U, back, UINT32_MAX));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_read(&s_nor, UINT32_MAX, back, 2U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_read(&s_nor, 0U, NULL, 1U));

    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_program(&s_nor, 0U, data, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_program(&s_nor, SIZE, data, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_program(&s_nor, SIZE - UNIT, data, 2U * UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_program(&s_nor, UINT32_MAX - 3U, data, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_program(&s_nor, 0U, NULL, UNIT));

    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_erase(&s_nor, SIZE));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_erase(&s_nor, UINT32_MAX));

    assert_unchanged();
    TEST_ASSERT_EQUAL_UINT64(13U, stats().refused);
    TEST_ASSERT_EQUAL_UINT64(0U, stats().reads + stats().programs + stats().erases);
}

static void test_a_program_writes_whole_units_within_one_page_and_nothing_else(void)
{
    uint8_t data[PAGE];

    start();
    pattern(data, PAGE, 1U);
    snapshot();

    /* Not at a unit's start, not whole units, across the end of a page. */
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_program(&s_nor, 1U, data, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_program(&s_nor, 2U, data, 2U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_program(&s_nor, 0U, data, UNIT + 1U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_program(&s_nor, 0U, data, 1U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_program(&s_nor, PAGE - UNIT, data, 2U * UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_program(&s_nor, UNIT, data, PAGE));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_program(&s_nor, 0U, data, 2U * PAGE));
    /* Across the end of a sector as well. */
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_program(&s_nor, SECTOR - UNIT, data, 2U * UNIT));
    assert_unchanged();
    TEST_ASSERT_EQUAL_UINT64(8U, stats().refused);

    /* Every whole page, and every run of units inside one, is accepted. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, PAGE, data, PAGE));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, &s_mem[PAGE], PAGE);
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, (2U * PAGE) + UNIT, data, 2U * UNIT));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, &s_mem[(2U * PAGE) + UNIT], 2U * UNIT);
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, SIZE - UNIT, data, UNIT));

    /* Only the bytes of the calls changed. */
    for (uint32_t i = 0U; i < SIZE; i++) {
        const bool written = ((i >= PAGE) && (i < (2U * PAGE))) ||
                             ((i >= ((2U * PAGE) + UNIT)) && (i < ((2U * PAGE) + (3U * UNIT)))) ||
                             (i >= (SIZE - UNIT));

        if (!written) {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFFU, s_mem[i], at("byte", i));
        }
    }
}

static void test_a_unit_is_programmed_once_between_two_erases(void)
{
    const uint8_t first[UNIT] = {0xF0U, 0xFFU, 0x0FU, 0xA5U};
    const uint8_t more_zeros[UNIT] = {0x00U, 0x00U, 0x00U, 0x00U};
    uint8_t page[PAGE];

    start();
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, UNIT, first, UNIT));
    snapshot();

    /* Not again, not with the same data, not with bits that would only clear more. */
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, nor_program(&s_nor, UNIT, first, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, nor_program(&s_nor, UNIT, more_zeros, UNIT));

    /* A call with erased units and one written unit changes none of them. */
    (void)memset(page, 0x00, sizeof(page));
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, nor_program(&s_nor, 0U, page, PAGE));
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, nor_program(&s_nor, UNIT, page, 3U * UNIT));
    assert_unchanged();
    TEST_ASSERT_EQUAL_UINT64(4U, stats().refused);

    /* The other units of the page are still erased and can be written. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, 0U, page, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, 2U * UNIT, page, 2U * UNIT));

    /* After an erase of its sector the unit can be programmed again. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, UNIT, more_zeros, UNIT));
    TEST_ASSERT_EACH_EQUAL_HEX8(0x00U, &s_mem[UNIT], UNIT);
}

/* The limit nor.h states: no error-correcting code, so a unit that reads erased is erased. */
static void test_a_unit_programmed_with_ff_reads_erased_and_is_accepted_again(void)
{
    const uint8_t ones[UNIT] = {0xFFU, 0xFFU, 0xFFU, 0xFFU};
    const uint8_t data[UNIT] = {0x01U, 0x02U, 0x03U, 0x04U};

    start();
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, 0U, ones, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, 0U, data, UNIT));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, s_mem, UNIT);
}

/* A unit that does not read 0xFF everywhere counts as written, whichever byte it is. */
static void test_one_cleared_bit_anywhere_in_a_unit_makes_it_written(void)
{
    const uint8_t data[UNIT] = {0x00U, 0x00U, 0x00U, 0x00U};

    start();

    for (uint32_t byte = 0U; byte < UNIT; byte++) {
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            (void)memset(s_mem, 0xFF, sizeof(s_mem));
            s_mem[(3U * PAGE) + byte] = (uint8_t)(0xFFU & ~(1U << bit));
            snapshot();
            TEST_ASSERT_EQUAL_INT_MESSAGE(FLASH_E_NOT_ERASED,
                                          nor_program(&s_nor, 3U * PAGE, data, UNIT),
                                          at("byte and bit", (byte * 8U) + bit));
            assert_unchanged();
        }
    }
}

/* new = old AND src. With write-once, old is 0xFF in every program that is accepted, so the
 * rule shows as: the bytes given, exactly, also those that keep bits at 1. A torn program,
 * further down, shows that a program never sets a bit. */
static void test_an_accepted_program_leaves_the_bytes_given(void)
{
    uint8_t data[PAGE];

    start();

    for (uint32_t value = 0U; value < 256U; value++) {
        const uint32_t addr = (value % (SIZE / UNIT)) * UNIT;

        if ((value % (SIZE / UNIT)) == 0U) {
            (void)memset(s_mem, 0xFF, sizeof(s_mem));
        }

        (void)memset(data, (int)value, UNIT);
        TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, addr, data, UNIT));
        TEST_ASSERT_EACH_EQUAL_HEX8_MESSAGE((uint8_t)value, &s_mem[addr], UNIT, at("value", value));
    }
}

static void test_an_erase_sets_one_whole_sector_to_ff_and_nothing_else(void)
{
    uint8_t back[SIZE];

    start();
    (void)memset(s_mem, 0x00, sizeof(s_mem));

    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 2U * SECTOR));

    for (uint32_t i = 0U; i < SIZE; i++) {
        const bool in_sector = (i >= (2U * SECTOR)) && (i < (3U * SECTOR));

        TEST_ASSERT_EQUAL_HEX8_MESSAGE(in_sector ? 0xFFU : 0x00U, s_mem[i], at("byte", i));
    }

    /* Only a sector's start: any other address is refused, however close. */
    snapshot();

    for (uint32_t addr = 1U; addr < SECTOR; addr++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(FLASH_E_ALIGN, nor_erase(&s_nor, SECTOR + addr),
                                      at("address", SECTOR + addr));
    }

    assert_unchanged();

    /* The last sector, and an erase of an erased sector. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, SIZE - SECTOR));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, SIZE - SECTOR));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_read(&s_nor, 0U, back, SIZE));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xFFU, &back[SIZE - SECTOR], SECTOR);
}

/* The order of the checks: the supply, the range, the alignment, write-once. */
static void test_the_checks_come_in_the_order_flash_h_gives(void)
{
    uint8_t data[2U * PAGE];
    uint8_t back[4];

    start();
    (void)memset(data, 0x00, sizeof(data));
    (void)memset(s_mem, 0x00, sizeof(s_mem)); /* every unit written */

    /* Out of range and misaligned: the range. */
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_program(&s_nor, SIZE - 2U, data, 3U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_erase(&s_nor, SIZE + 1U));
    /* Misaligned and written: the alignment. */
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_program(&s_nor, 1U, data, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_program(&s_nor, 0U, data, 2U * PAGE));
    /* Aligned and written: write-once. */
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, nor_program(&s_nor, 0U, data, UNIT));

    /* Without supply, whatever else is wrong: the supply. */
    nor_arm_cut(&s_nor, 1U, 0U, 1U);
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_read(&s_nor, SIZE, back, 1U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_read(&s_nor, 0U, back, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(&s_nor, SIZE - 2U, data, 3U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(&s_nor, 1U, data, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(&s_nor, 0U, data, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, 1U));

    const nor_stats_t counted = stats();

    TEST_ASSERT_EQUAL_UINT64(5U, counted.refused);
    TEST_ASSERT_EQUAL_UINT64(6U, counted.unpowered);
    TEST_ASSERT_EQUAL_UINT64(1U, counted.erases);
}

/* ------------------------------------------------------------------------------------------
 * Counters, wear and time
 * ---------------------------------------------------------------------------------------- */

static void test_every_call_is_counted_once_in_the_first_counter_that_applies(void)
{
    uint8_t data[PAGE];
    uint8_t back[PAGE];

    start();
    pattern(data, PAGE, 2U);

    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_read(&s_nor, 3U, back, 10U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_read(&s_nor, 0U, back, 1U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, 0U, data, PAGE));     /* 4 units */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, SECTOR, data, UNIT)); /* 1 unit */
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, nor_program(&s_nor, 0U, data, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_program(&s_nor, 2U, data, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_read(&s_nor, SIZE, back, 1U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, SECTOR));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, SECTOR));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 3U * SECTOR));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_erase(&s_nor, 3U));

    nor_stats_t counted = stats();

    TEST_ASSERT_EQUAL_UINT64(2U, counted.reads);
    TEST_ASSERT_EQUAL_UINT64(11U, counted.bytes_read);
    TEST_ASSERT_EQUAL_UINT64(2U, counted.programs);
    TEST_ASSERT_EQUAL_UINT64(5U, counted.units_programmed);
    TEST_ASSERT_EQUAL_UINT64(3U, counted.erases);
    TEST_ASSERT_EQUAL_UINT64(4U, counted.refused);
    TEST_ASSERT_EQUAL_UINT64(0U, counted.unpowered);
    TEST_ASSERT_EQUAL_UINT64((2U * (uint64_t)T_PP) + (3U * (uint64_t)T_SE), counted.busy_ns);
    TEST_ASSERT_EQUAL_UINT32(0U, counted.cuts);

    /* The wear, per sector, in the model and in the caller's array. */
    const uint32_t expected[SECTORS] = {0U, 2U, 0U, 1U};

    TEST_ASSERT_EQUAL_UINT32_ARRAY(expected, s_counts, SECTORS);

    for (uint32_t sector = 0U; sector < SECTORS; sector++) {
        TEST_ASSERT_EQUAL_UINT32(expected[sector], nor_sector_erases(&s_nor, sector));
    }

    TEST_ASSERT_EQUAL_UINT32(0U, nor_sector_erases(&s_nor, SECTORS));
    TEST_ASSERT_EQUAL_UINT32(0U, nor_sector_erases(NULL, 0U));

    /* A torn program counts its units up to the torn one, a torn erase as wear. */
    nor_arm_cut(&s_nor, 1U, 2U, 9U);
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(&s_nor, 2U * PAGE, data, PAGE));
    nor_power_on(&s_nor);
    nor_arm_cut(&s_nor, 1U, 5U, 9U);
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, 3U * SECTOR));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_read(&s_nor, 0U, back, 1U));

    counted = stats();
    TEST_ASSERT_EQUAL_UINT64(3U, counted.programs);
    TEST_ASSERT_EQUAL_UINT64(5U + 3U, counted.units_programmed);
    TEST_ASSERT_EQUAL_UINT64(4U, counted.erases);
    TEST_ASSERT_EQUAL_UINT64(1U, counted.unpowered);
    TEST_ASSERT_EQUAL_UINT32(2U, counted.cuts);
    TEST_ASSERT_EQUAL_UINT32(2U, nor_sector_erases(&s_nor, 3U));
}

static void test_the_wear_count_stops_at_its_largest_value(void)
{
    start();
    s_counts[1] = UINT32_MAX - 1U;

    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, SECTOR));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, nor_sector_erases(&s_nor, 1U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, SECTOR));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, nor_sector_erases(&s_nor, 1U));
    TEST_ASSERT_EQUAL_UINT64(2U, stats().erases);
}

static void test_each_operation_takes_the_time_of_the_modelled_part(void)
{
    uint8_t data[PAGE];
    uint8_t back[SIZE];

    start();
    pattern(data, PAGE, 4U);

    /* A program takes a page program, whatever its length within the page. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, 0U, data, UNIT));
    TEST_ASSERT_EQUAL_UINT32(T_PP, nor_last_duration_ns(&s_nor));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, PAGE, data, PAGE));
    TEST_ASSERT_EQUAL_UINT32(T_PP, nor_last_duration_ns(&s_nor));
    /* A read takes no time, however long. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_read(&s_nor, 0U, back, SIZE));
    TEST_ASSERT_EQUAL_UINT32(0U, nor_last_duration_ns(&s_nor));
    /* An erase takes a sector erase. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 0U));
    TEST_ASSERT_EQUAL_UINT32(T_SE, nor_last_duration_ns(&s_nor));
    /* A refused call takes none. */
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_erase(&s_nor, 2U));
    TEST_ASSERT_EQUAL_UINT32(0U, nor_last_duration_ns(&s_nor));
    TEST_ASSERT_EQUAL_UINT64((2U * (uint64_t)T_PP) + T_SE, stats().busy_ns);

    /* The figures are the configuration's, not constants of the model. */
    nor_config_t config = config_of(s_mem, s_counts);

    config.timing.page_program_ns = 7U;
    config.timing.sector_erase_ns = 1234567U;
    TEST_ASSERT_TRUE(nor_init(&s_nor, &config));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, 2U * PAGE, data, UNIT));
    TEST_ASSERT_EQUAL_UINT32(7U, nor_last_duration_ns(&s_nor));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, SECTOR));
    TEST_ASSERT_EQUAL_UINT32(1234567U, nor_last_duration_ns(&s_nor));
    TEST_ASSERT_EQUAL_UINT64(1234567U + 7U, stats().busy_ns);
    TEST_ASSERT_EQUAL_UINT32(0U, nor_last_duration_ns(NULL));
}

/* A torn operation took the time up to its cut: within the share of its position. */
static void test_a_torn_operation_takes_the_time_up_to_its_cut(void)
{
    uint8_t data[PAGE];
    uint64_t busy = 0U;

    pattern(data, PAGE, 5U);

    for (uint32_t seed = 0U; seed < 64U; seed++) {
        for (uint32_t position = 0U; position < (PAGE / UNIT); position++) {
            (void)memset(s_mem, 0xFF, sizeof(s_mem));
            start();
            nor_arm_cut(&s_nor, 1U, position, seed);
            TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(&s_nor, 0U, data, PAGE));

            const uint32_t took = nor_last_duration_ns(&s_nor);
            const uint32_t share = T_PP / (PAGE / UNIT);

            TEST_ASSERT_UINT32_WITHIN_MESSAGE(share / 2U, (position * share) + (share / 2U), took,
                                              at("program position", position));
            TEST_ASSERT_EQUAL_UINT32(took, last_cut().duration_ns);
            TEST_ASSERT_EQUAL_UINT64(took, stats().busy_ns);
        }

        for (uint32_t position = 0U; position < NOR_ERASE_POSITIONS; position++) {
            const uint32_t share = T_SE / NOR_ERASE_POSITIONS;

            (void)memset(s_mem, 0x00, sizeof(s_mem));
            start();
            nor_arm_cut(&s_nor, 1U, position, seed);
            TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, SECTOR));

            const uint32_t took = nor_last_duration_ns(&s_nor);

            TEST_ASSERT_UINT32_WITHIN_MESSAGE(share / 2U, (position * share) + (share / 2U), took,
                                              at("erase position", position));
            TEST_ASSERT_TRUE(took < T_SE);
            busy += took;
        }
    }

    TEST_ASSERT_TRUE(busy > 0U);
}

/* ------------------------------------------------------------------------------------------
 * Power cuts: which operation, and the loss of supply
 * ---------------------------------------------------------------------------------------- */

static void test_the_cut_falls_on_the_kth_program_or_erase_that_passes_its_checks(void)
{
    const uint8_t data[UNIT] = {0x00U, 0x11U, 0x22U, 0x33U};
    uint8_t back[UNIT];
    uint8_t page[PAGE];

    start();
    pattern(page, PAGE, 13U);
    nor_arm_cut(&s_nor, 4U, 0U, 1U);
    TEST_ASSERT_EQUAL_UINT32(4U, nor_cut_countdown(&s_nor));

    /* Reads, refused calls and calls without effect on the count do not move it. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_read(&s_nor, 0U, back, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, nor_program(&s_nor, 1U, data, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, nor_erase(&s_nor, SIZE));
    TEST_ASSERT_EQUAL_UINT32(4U, nor_cut_countdown(&s_nor));

    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, 0U, data, UNIT)); /* 1 */
    TEST_ASSERT_EQUAL_UINT32(3U, nor_cut_countdown(&s_nor));
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, nor_program(&s_nor, 0U, data, UNIT));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 2U * SECTOR));        /* 2 */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, UNIT, data, UNIT)); /* 3 */
    TEST_ASSERT_EQUAL_UINT32(1U, nor_cut_countdown(&s_nor));
    TEST_ASSERT_TRUE(nor_powered(&s_nor));

    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, 3U * SECTOR)); /* 4: cut */
    TEST_ASSERT_FALSE(nor_powered(&s_nor));
    TEST_ASSERT_EQUAL_UINT32(0U, nor_cut_countdown(&s_nor));

    const nor_cut_t cut = last_cut();

    TEST_ASSERT_EQUAL_INT(NOR_OP_ERASE, cut.op);
    TEST_ASSERT_EQUAL_UINT64(4U, cut.operation);
    TEST_ASSERT_EQUAL_UINT32(3U * SECTOR, cut.addr);
    TEST_ASSERT_EQUAL_UINT32(SECTOR, cut.len);
    TEST_ASSERT_EQUAL_UINT32(0U, cut.position);
    TEST_ASSERT_EQUAL_UINT32(NOR_ERASE_POSITIONS, cut.positions);
    TEST_ASSERT_EQUAL_UINT32(1U, cut.seed);
    TEST_ASSERT_EQUAL_UINT32(0U, cut.transitions); /* the sector was erased already */
    TEST_ASSERT_EQUAL_UINT32(0U, cut.done);

    /* The operation number counts from nor_init(), the programs and erases before the arm
     * included: armed after 1 more erase, the 2nd from then is the 6th. */
    nor_power_on(&s_nor);
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 0U)); /* 5 */
    nor_arm_cut(&s_nor, 2U, 1U, 77U);
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 0U));                           /* 6 */
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(&s_nor, PAGE, page, PAGE)); /* 7 */
    TEST_ASSERT_EQUAL_INT(NOR_OP_PROGRAM, last_cut().op);
    TEST_ASSERT_EQUAL_UINT64(7U, last_cut().operation);
    TEST_ASSERT_EQUAL_UINT32(PAGE, last_cut().addr);
    TEST_ASSERT_EQUAL_UINT32(PAGE, last_cut().len);
    TEST_ASSERT_EQUAL_UINT32(1U, last_cut().position);
    TEST_ASSERT_EQUAL_UINT32(PAGE / UNIT, last_cut().positions);
    TEST_ASSERT_EQUAL_UINT32(77U, last_cut().seed);
}

static void test_after_a_cut_every_call_is_refused_until_the_power_comes_back(void)
{
    uint8_t data[PAGE];
    uint8_t back[PAGE];

    start();
    pattern(data, PAGE, 6U);
    nor_arm_cut(&s_nor, 1U, 1U, 5U);
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(&s_nor, 0U, data, PAGE));
    snapshot();

    for (uint32_t i = 0U; i < 10U; i++) {
        TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_read(&s_nor, 0U, back, PAGE));
        TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(&s_nor, PAGE, data, PAGE));
        TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, 0U));
        TEST_ASSERT_EQUAL_UINT32(0U, nor_last_duration_ns(&s_nor));
    }

    assert_unchanged();
    TEST_ASSERT_EQUAL_UINT64(30U, stats().unpowered);
    TEST_ASSERT_EQUAL_UINT64(1U, stats().programs);
    TEST_ASSERT_EQUAL_UINT32(0U, nor_sector_erases(&s_nor, 0U));

    /* Back: the array is what the cut left, and everything works. */
    nor_power_on(&s_nor);
    TEST_ASSERT_TRUE(nor_powered(&s_nor));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_read(&s_nor, 0U, back, PAGE));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_before, back, PAGE);
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_program(&s_nor, PAGE, data, PAGE));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 0U));

    /* A cut fires once: nothing is cut any more. */
    for (uint32_t i = 0U; i < 8U; i++) {
        TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, SECTOR));
    }

    TEST_ASSERT_EQUAL_UINT32(1U, stats().cuts);
    nor_power_on(&s_nor); /* while powered: no change */
    TEST_ASSERT_TRUE(nor_powered(&s_nor));
}

static void test_a_cut_can_be_replaced_disarmed_and_waits_while_the_supply_is_gone(void)
{
    const uint8_t data[UNIT] = {0U};

    start();
    nor_arm_cut(&s_nor, 2U, 0U, 1U);
    nor_arm_cut(&s_nor, 3U, 0U, 1U); /* replaces */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, nor_cut_countdown(&s_nor));
    nor_disarm_cut(&s_nor);
    TEST_ASSERT_EQUAL_UINT32(0U, nor_cut_countdown(&s_nor));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 0U));

    nor_arm_cut(&s_nor, 1U, 0U, 1U);
    nor_arm_cut(&s_nor, 0U, 0U, 1U); /* 0 disarms */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, stats().cuts);

    /* Armed while the supply is gone, the count waits for it. */
    nor_arm_cut(&s_nor, 1U, 0U, 1U);
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, 0U));
    nor_arm_cut(&s_nor, 2U, 0U, 3U);
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(&s_nor, 0U, data, UNIT));
    TEST_ASSERT_EQUAL_UINT32(2U, nor_cut_countdown(&s_nor));
    nor_power_on(&s_nor);
    TEST_ASSERT_EQUAL_INT(FLASH_OK, nor_erase(&s_nor, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, SECTOR));
    TEST_ASSERT_EQUAL_UINT32(2U, stats().cuts);
    TEST_ASSERT_EQUAL_UINT32(3U, last_cut().seed);
}

static void test_null_arguments_change_nothing_and_answer_as_a_missing_device(void)
{
    uint8_t byte = 0U;
    nor_cut_t cut;
    nor_stats_t counted;

    (void)memset(&counted, 0x77, sizeof(counted));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_read(NULL, 0U, &byte, 1U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(NULL, 0U, &byte, 1U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(NULL, 0U));
    nor_arm_cut(NULL, 1U, 0U, 0U);
    nor_disarm_cut(NULL);
    nor_power_on(NULL);
    nor_get_stats(NULL, &counted);
    TEST_ASSERT_EQUAL_HEX8(0x77U, ((const uint8_t *)&counted)[0]);
    TEST_ASSERT_FALSE(nor_powered(NULL));
    TEST_ASSERT_FALSE(nor_last_cut(NULL, &cut));
    TEST_ASSERT_EQUAL_UINT32(0U, nor_cut_countdown(NULL));

    start();
    nor_get_stats(&s_nor, NULL);
    TEST_ASSERT_FALSE(nor_last_cut(&s_nor, NULL));
}

/* ------------------------------------------------------------------------------------------
 * Power cuts: what a cut leaves
 * ---------------------------------------------------------------------------------------- */

/* Cut a program of `len` bytes of `data` at `addr` of a fresh, erased device at `position`
 * with `seed`; the device keeps what the cut left. */
static void cut_program(uint32_t addr, const uint8_t *data, uint32_t len, uint32_t position,
                        uint32_t seed)
{
    (void)memset(s_mem, 0xFF, sizeof(s_mem));
    start();
    nor_arm_cut(&s_nor, 1U, position, seed);
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(&s_nor, addr, data, len));
}

/* What a torn program may leave, and nothing else: the units before the cut position written,
 * the unit at it with only some of its 1-to-0 transitions done, the units after it and every
 * other byte erased; the record counts what changed. Returns the bits done. */
static uint32_t check_torn_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    const nor_cut_t cut = last_cut();
    const uint32_t start_of_torn = addr + (cut.position * UNIT);
    uint32_t transitions = 0U;
    uint32_t done = 0U;

    TEST_ASSERT_EQUAL_INT(NOR_OP_PROGRAM, cut.op);
    TEST_ASSERT_EQUAL_UINT32(len / UNIT, cut.positions);
    TEST_ASSERT_TRUE(cut.position < cut.positions);

    for (uint32_t i = 0U; i < SIZE; i++) {
        const uint32_t got = s_mem[i];

        if ((i < addr) || (i >= (addr + len))) {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFFU, got, at("outside the program, byte", i));
        } else if (i < start_of_torn) {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(data[i - addr], got, at("written unit, byte", i));
        } else if (i < (start_of_torn + UNIT)) {
            const uint32_t want = data[i - addr];

            /* Only transitions: every 0 bit is 0 in the data (no bit outside them cleared),
             * and no bit is set (a program never sets one; the old byte was 0xFF). */
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0U, ~got & want & 0xFFU, at("torn unit, byte", i));
            transitions += popcount8(~want & 0xFFU);
            done += popcount8(~got & 0xFFU);
        } else {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFFU, got, at("unit after the cut, byte", i));
        }
    }

    TEST_ASSERT_EQUAL_UINT32(transitions, cut.transitions);
    TEST_ASSERT_EQUAL_UINT32(done, cut.done);
    return done;
}

static void test_a_program_can_be_cut_at_every_position_and_leaves_only_what_the_rule_allows(void)
{
    uint8_t data[PAGE];
    uint32_t none = 0U;
    uint32_t all = 0U;
    uint32_t some = 0U;

    pattern(data, PAGE, 8U);

    for (uint32_t position = 0U; position < (PAGE / UNIT); position++) {
        for (uint32_t seed = 0U; seed < 300U; seed++) {
            cut_program(PAGE, data, PAGE, position, seed);
            TEST_ASSERT_EQUAL_UINT32(position, last_cut().position);

            const uint32_t done = check_torn_program(PAGE, data, PAGE);

            if (done == 0U) {
                none++;
            } else if (done == last_cut().transitions) {
                all++;
            } else {
                some++;
            }
        }
    }

    /* nor.h: none and all each in about a third of the seeds. */
    TEST_ASSERT_UINT32_WITHIN(150U, 400U, none);
    TEST_ASSERT_UINT32_WITHIN(150U, 400U, all);
    TEST_ASSERT_TRUE(some > 200U);

    /* A program of one unit has one position; one past the end of a program is its last. */
    for (uint32_t seed = 0U; seed < 50U; seed++) {
        cut_program(SIZE - UNIT, data, UNIT, 0U, seed);
        (void)check_torn_program(SIZE - UNIT, data, UNIT);
        cut_program(0U, data, 2U * UNIT, 2U, seed);
        TEST_ASSERT_EQUAL_UINT32(1U, last_cut().position);
        (void)check_torn_program(0U, data, 2U * UNIT);
        cut_program(0U, data, 2U * UNIT, 1000U, seed);
        TEST_ASSERT_EQUAL_UINT32(1U, last_cut().position);
    }
}

/* A sector of 0x00 bytes erased and cut: the bits set to 1 are some of the 0 bits; no other
 * sector changes. Returns the bits set. */
static uint32_t check_torn_erase(uint32_t sector)
{
    const nor_cut_t cut = last_cut();
    uint32_t done = 0U;
    uint32_t zeros = 0U;

    TEST_ASSERT_EQUAL_INT(NOR_OP_ERASE, cut.op);
    TEST_ASSERT_EQUAL_UINT32(NOR_ERASE_POSITIONS, cut.positions);

    for (uint32_t i = 0U; i < SIZE; i++) {
        const uint32_t got = s_mem[i];
        const uint32_t was = s_before[i];

        if ((i / SECTOR) != sector) {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(was, got, at("outside the sector, byte", i));
        } else {
            /* Only 0 -> 1: every bit that was 1 is still 1. */
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(was, got & was, at("torn sector, byte", i));
            zeros += popcount8(~was & 0xFFU);
            done += popcount8(got & ~was & 0xFFU);
        }
    }

    TEST_ASSERT_EQUAL_UINT32(zeros, cut.transitions);
    TEST_ASSERT_EQUAL_UINT32(done, cut.done);
    return done;
}

static void prepare_erase(uint32_t salt)
{
    pattern(s_mem, SIZE, salt);
    s_mem[SECTOR] = 0x00U; /* the sector cut below has 0 bits in any case */
    start();
    snapshot();
}

static void test_an_erase_can_be_cut_at_every_position_and_only_sets_some_0_bits(void)
{
    uint32_t mean_before = 0U;

    for (uint32_t position = 0U; position < NOR_ERASE_POSITIONS; position++) {
        uint32_t total = 0U;
        uint32_t zeros = 0U;

        for (uint32_t seed = 0U; seed < 200U; seed++) {
            prepare_erase(seed);
            nor_arm_cut(&s_nor, 1U, position, seed);
            TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, SECTOR));
            TEST_ASSERT_EQUAL_UINT32(position, last_cut().position);
            total += check_torn_erase(1U);
            zeros += last_cut().transitions;
        }

        /* nor.h: later positions leave more bits set on average. */
        const uint32_t mean = (total * 1000U) / zeros;

        TEST_ASSERT_TRUE_MESSAGE(mean > mean_before, at("mean per mille at position", position));
        mean_before = mean;
    }

    /* Past the end: the last position. */
    prepare_erase(1U);
    nor_arm_cut(&s_nor, 1U, NOR_ERASE_POSITIONS, 1U);
    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, SECTOR));
    TEST_ASSERT_EQUAL_UINT32(NOR_ERASE_POSITIONS - 1U, last_cut().position);
}

/* For one seed, a later cut of the same erase leaves every bit an earlier one left, and more:
 * the moments drawn are the same, only the cut moment moves on. */
static void test_for_one_seed_a_later_erase_cut_leaves_a_superset(void)
{
    uint8_t earlier[SECTOR];

    for (uint32_t seed = 0U; seed < 100U; seed++) {
        (void)memset(earlier, 0x00, sizeof(earlier));

        for (uint32_t position = 0U; position < NOR_ERASE_POSITIONS; position++) {
            prepare_erase(3U);
            nor_arm_cut(&s_nor, 1U, position, seed);
            TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, SECTOR));

            for (uint32_t i = 0U; i < SECTOR; i++) {
                const uint32_t now = s_mem[SECTOR + i];

                TEST_ASSERT_EQUAL_HEX8_MESSAGE(earlier[i], earlier[i] & now, at("seed", seed));
                earlier[i] = (uint8_t)now;
            }
        }
    }
}

static void test_the_same_seed_leaves_the_same_result_and_other_seeds_others(void)
{
    uint8_t data[PAGE];
    uint8_t first[SIZE];
    uint32_t differ = 0U;

    pattern(data, PAGE, 9U);

    for (uint32_t seed = 0U; seed < 40U; seed++) {
        cut_program(0U, data, PAGE, NOR_CUT_SEEDED_POSITION, seed);
        (void)memcpy(first, s_mem, sizeof(first));
        nor_cut_t first_cut = last_cut();

        cut_program(0U, data, PAGE, NOR_CUT_SEEDED_POSITION, seed);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(first, s_mem, SIZE);
        TEST_ASSERT_EQUAL_UINT32(first_cut.position, last_cut().position);
        TEST_ASSERT_EQUAL_UINT32(first_cut.done, last_cut().done);
        TEST_ASSERT_EQUAL_UINT32(first_cut.duration_ns, last_cut().duration_ns);

        cut_program(0U, data, PAGE, NOR_CUT_SEEDED_POSITION, seed + 1000U);
        differ += (memcmp(first, s_mem, sizeof(first)) != 0) ? 1U : 0U;

        prepare_erase(4U);
        nor_arm_cut(&s_nor, 1U, NOR_CUT_SEEDED_POSITION, seed);
        TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, SECTOR));
        (void)memcpy(first, s_mem, sizeof(first));
        first_cut = last_cut();

        prepare_erase(4U);
        nor_arm_cut(&s_nor, 1U, NOR_CUT_SEEDED_POSITION, seed);
        TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, SECTOR));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(first, s_mem, SIZE);
        TEST_ASSERT_EQUAL_UINT32(first_cut.position, last_cut().position);
        TEST_ASSERT_EQUAL_UINT32(first_cut.duration_ns, last_cut().duration_ns);

        prepare_erase(4U);
        nor_arm_cut(&s_nor, 1U, NOR_CUT_SEEDED_POSITION, seed + 1000U);
        TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, SECTOR));
        differ += (memcmp(first, s_mem, sizeof(first)) != 0) ? 1U : 0U;
    }

    /* Two seeds can leave the same result (both none, both all), but not most of the time. */
    TEST_ASSERT_TRUE(differ > 40U);
}

static void test_a_seeded_position_takes_every_position_of_the_operation(void)
{
    uint8_t data[PAGE];
    uint32_t program_seen = 0U;
    uint32_t erase_seen = 0U;

    pattern(data, PAGE, 10U);

    for (uint32_t seed = 0U; seed < 400U; seed++) {
        cut_program(2U * PAGE, data, PAGE, NOR_CUT_SEEDED_POSITION, seed);
        (void)check_torn_program(2U * PAGE, data, PAGE);
        program_seen |= (uint32_t)1U << last_cut().position;

        prepare_erase(5U);
        nor_arm_cut(&s_nor, 1U, NOR_CUT_SEEDED_POSITION, seed);
        TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, SECTOR));
        (void)check_torn_erase(1U);
        erase_seen |= (uint32_t)1U << last_cut().position;
    }

    TEST_ASSERT_EQUAL_HEX32((1U << (PAGE / UNIT)) - 1U, program_seen);
    TEST_ASSERT_EQUAL_HEX32((1U << NOR_ERASE_POSITIONS) - 1U, erase_seen);
}

/* Across seeds every outcome the rule allows occurs. A unit, or a sector, with six transitions
 * has 64 outcomes; each one must turn up. */
#define SIX_BITS 0xC0U /* a byte whose bits 0 to 5 are 0 */

static void test_across_seeds_every_subset_of_a_torn_units_transitions_occurs(void)
{
    const uint8_t data[UNIT] = {0xFFU, SIX_BITS, 0xFFU, 0xFFU};
    uint64_t seen = 0U;

    for (uint32_t seed = 0U; seed < 3000U; seed++) {
        cut_program(PAGE + UNIT, data, UNIT, 0U, seed);
        (void)check_torn_program(PAGE + UNIT, data, UNIT);

        /* The six low bits of the byte: those that read 0 are the transitions done. */
        seen |= 1ULL << (~(uint32_t)s_mem[PAGE + UNIT + 1U] & 0x3FU);
    }

    TEST_ASSERT_EQUAL_HEX64(UINT64_MAX, seen);
}

static void test_across_seeds_every_subset_of_a_torn_sectors_0_bits_occurs(void)
{
    for (uint32_t position = 4U; position < 12U; position += 3U) {
        uint64_t seen = 0U;

        for (uint32_t seed = 0U; seed < 3000U; seed++) {
            (void)memset(s_mem, 0xFF, sizeof(s_mem));
            s_mem[(2U * SECTOR) + 37U] = SIX_BITS; /* the only 0 bits of the sector */
            start();
            snapshot();
            nor_arm_cut(&s_nor, 1U, position, seed);
            TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, 2U * SECTOR));
            (void)check_torn_erase(2U);

            /* The six low bits: those that read 1 are the transitions done. */
            seen |= 1ULL << ((uint32_t)s_mem[(2U * SECTOR) + 37U] & 0x3FU);
        }

        TEST_ASSERT_EQUAL_HEX64_MESSAGE(UINT64_MAX, seen, at("position", position));
    }
}

/* ------------------------------------------------------------------------------------------
 * The procedure of nor.h, recomputed from its text
 * ---------------------------------------------------------------------------------------- */

/* SplitMix64 as published: the state advances by the golden gamma, the output is the state
 * through the mixing function. */
static uint64_t s_ref_state;

static uint64_t ref_next(void)
{
    uint64_t z;

    s_ref_state += 0x9E3779B97F4A7C15ULL;
    z = s_ref_state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

/* nor.c draws a value below `bound` from the high half of an output, multiplied and shifted. */
static uint32_t ref_below(uint32_t bound)
{
    return (uint32_t)(((ref_next() >> 32U) * (uint64_t)bound) >> 32U);
}

static void test_the_generator_is_splitmix64(void)
{
    /* The first output from seed 0: the value Java's SplittableRandom(0).nextLong() returns,
     * -2152535657050944081, which uses the same generator. */
    s_ref_state = 0U;
    TEST_ASSERT_EQUAL_HEX64(0xE220A8397B1DCDAFULL, ref_next());
}

/* The bit transitions of a cut: [a, b] from two draws, then one moment per transition. */
static bool ref_done(uint32_t a, uint32_t b, uint32_t t)
{
    return (a + ref_below((b - a) + 1U)) < t;
}

static void test_a_torn_program_is_the_one_the_procedure_in_nor_h_gives(void)
{
    uint8_t data[PAGE];
    uint8_t expect[SIZE];

    pattern(data, PAGE, 11U);

    for (uint32_t seed = 0U; seed < 200U; seed += 7U) {
        cut_program(PAGE, data, PAGE, NOR_CUT_SEEDED_POSITION, seed);

        /* The position, the cut moment, the window, then the transitions bit by bit. */
        s_ref_state = seed;
        const uint32_t position = ref_below(PAGE / UNIT);
        const uint32_t t = ref_below(NOR_CUT_RESOLUTION + 1U);
        const uint32_t first = ref_below(NOR_CUT_RESOLUTION + 1U);
        const uint32_t second = ref_below(NOR_CUT_RESOLUTION + 1U);
        const uint32_t a = (first < second) ? first : second;
        const uint32_t b = (first < second) ? second : first;

        (void)memset(expect, 0xFF, sizeof(expect));
        (void)memcpy(&expect[PAGE], data, (size_t)position * UNIT);

        for (uint32_t i = position * UNIT; i < ((position + 1U) * UNIT); i++) {
            uint32_t byte = 0xFFU;

            for (uint32_t bit = 0U; bit < 8U; bit++) {
                if ((((uint32_t)data[i] >> bit) & 1U) == 0U) {
                    if (ref_done(a, b, t)) {
                        byte &= ~((uint32_t)1U << bit);
                    }
                }
            }

            expect[PAGE + i] = (uint8_t)byte;
        }

        TEST_ASSERT_EQUAL_UINT32_MESSAGE(position, last_cut().position, at("seed", seed));
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(expect, s_mem, SIZE, at("seed", seed));

        /* The time: (position + t / R) / units of a page program. */
        const uint64_t elapsed = ((uint64_t)position * NOR_CUT_RESOLUTION) + t;
        const uint64_t fraction = (elapsed << 16U) / ((PAGE / UNIT) * (uint64_t)NOR_CUT_RESOLUTION);

        TEST_ASSERT_EQUAL_UINT32((uint32_t)((T_PP * fraction) >> 16U), last_cut().duration_ns);
    }
}

static void test_a_torn_erase_is_the_one_the_procedure_in_nor_h_gives(void)
{
    uint8_t expect[SIZE];
    const uint32_t steps = NOR_ERASE_POSITIONS * NOR_CUT_RESOLUTION;

    for (uint32_t seed = 3U; seed < 300U; seed += 11U) {
        prepare_erase(seed);
        nor_arm_cut(&s_nor, 1U, NOR_CUT_SEEDED_POSITION, seed);
        TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&s_nor, SECTOR));

        s_ref_state = seed;
        const uint32_t position = ref_below(NOR_ERASE_POSITIONS);
        const uint32_t t = (position * NOR_CUT_RESOLUTION) + ref_below(NOR_CUT_RESOLUTION);
        const uint32_t first = ref_below(steps + 1U);
        const uint32_t second = ref_below(steps + 1U);
        const uint32_t a = (first < second) ? first : second;
        const uint32_t b = (first < second) ? second : first;

        (void)memcpy(expect, s_before, sizeof(expect));

        for (uint32_t i = SECTOR; i < (2U * SECTOR); i++) {
            uint32_t byte = s_before[i];

            for (uint32_t bit = 0U; bit < 8U; bit++) {
                if ((((uint32_t)s_before[i] >> bit) & 1U) == 0U) {
                    if (ref_done(a, b, t)) {
                        byte |= (uint32_t)1U << bit;
                    }
                }
            }

            expect[i] = (uint8_t)byte;
        }

        TEST_ASSERT_EQUAL_UINT32_MESSAGE(position, last_cut().position, at("seed", seed));
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(expect, s_mem, SIZE, at("seed", seed));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(((uint64_t)T_SE * t) / steps), last_cut().duration_ns);
    }
}

/* ------------------------------------------------------------------------------------------
 * The host's geometry
 * ---------------------------------------------------------------------------------------- */

/* The geometry of the host's flash (plat_flash.h): 2 MiB and 8 KiB, sectors of 4 KiB, pages of
 * 256 bytes, units of 8 bytes. A whole page is 32 positions; every one can be cut. */
static uint8_t s_big[0x202000U];
static uint32_t s_big_counts[0x202000U / 4096U];

static void test_on_the_hosts_geometry_every_position_of_a_page_and_an_erase_can_be_cut(void)
{
    const nor_config_t config = {.geometry = {0x202000U, 4096U, 256U, 8U},
                                 .timing = TIMING,
                                 .memory = s_big,
                                 .erase_counts = s_big_counts};
    nor_t nor;
    uint8_t page[256];

    pattern(page, sizeof(page), 12U);

    for (uint32_t position = 0U; position < 32U; position++) {
        const uint32_t addr = 0x102000U + (position * 256U);

        (void)memset(s_big, 0xFF, sizeof(s_big));
        TEST_ASSERT_TRUE(nor_init(&nor, &config));
        nor_arm_cut(&nor, 1U, position, position);
        TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_program(&nor, addr, page, 256U));
        TEST_ASSERT_EQUAL_UINT32(position, nor.last_cut.position);
        TEST_ASSERT_EQUAL_UINT32(32U, nor.last_cut.positions);

        /* Written before the torn unit, erased after it (Unity compares no empty arrays). */
        if (position > 0U) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(page, &s_big[addr], position * 8U);
        }

        if (position < 31U) {
            TEST_ASSERT_EACH_EQUAL_HEX8(0xFFU, &s_big[addr + ((position + 1U) * 8U)],
                                        256U - ((position + 1U) * 8U));
        }

        TEST_ASSERT_EACH_EQUAL_HEX8(0xFFU, s_big, addr);
    }

    for (uint32_t position = 0U; position < NOR_ERASE_POSITIONS; position++) {
        (void)memset(s_big, 0x00, sizeof(s_big));
        TEST_ASSERT_TRUE(nor_init(&nor, &config));
        nor_arm_cut(&nor, 1U, position, position);
        TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, nor_erase(&nor, 0x201000U));
        TEST_ASSERT_EACH_EQUAL_HEX8(0x00U, s_big, 0x201000U);
        TEST_ASSERT_EQUAL_UINT32(4096U * 8U, nor.last_cut.transitions);
        TEST_ASSERT_EQUAL_UINT32(1U, nor_sector_erases(&nor, 0x201U));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_geometries_flash_h_allows_are_valid_and_no_other);
    RUN_TEST(test_init_keeps_the_memory_and_zeroes_every_count);
    RUN_TEST(test_init_refuses_what_it_cannot_model);
    RUN_TEST(test_the_device_carries_the_geometry_the_model_and_its_operations);
    RUN_TEST(test_erased_bytes_read_ff_and_any_range_inside_reads);
    RUN_TEST(test_a_call_outside_the_device_or_of_no_bytes_is_refused_and_changes_nothing);
    RUN_TEST(test_a_program_writes_whole_units_within_one_page_and_nothing_else);
    RUN_TEST(test_a_unit_is_programmed_once_between_two_erases);
    RUN_TEST(test_a_unit_programmed_with_ff_reads_erased_and_is_accepted_again);
    RUN_TEST(test_one_cleared_bit_anywhere_in_a_unit_makes_it_written);
    RUN_TEST(test_an_accepted_program_leaves_the_bytes_given);
    RUN_TEST(test_an_erase_sets_one_whole_sector_to_ff_and_nothing_else);
    RUN_TEST(test_the_checks_come_in_the_order_flash_h_gives);
    RUN_TEST(test_every_call_is_counted_once_in_the_first_counter_that_applies);
    RUN_TEST(test_the_wear_count_stops_at_its_largest_value);
    RUN_TEST(test_each_operation_takes_the_time_of_the_modelled_part);
    RUN_TEST(test_a_torn_operation_takes_the_time_up_to_its_cut);
    RUN_TEST(test_the_cut_falls_on_the_kth_program_or_erase_that_passes_its_checks);
    RUN_TEST(test_after_a_cut_every_call_is_refused_until_the_power_comes_back);
    RUN_TEST(test_a_cut_can_be_replaced_disarmed_and_waits_while_the_supply_is_gone);
    RUN_TEST(test_null_arguments_change_nothing_and_answer_as_a_missing_device);
    RUN_TEST(test_a_program_can_be_cut_at_every_position_and_leaves_only_what_the_rule_allows);
    RUN_TEST(test_an_erase_can_be_cut_at_every_position_and_only_sets_some_0_bits);
    RUN_TEST(test_for_one_seed_a_later_erase_cut_leaves_a_superset);
    RUN_TEST(test_the_same_seed_leaves_the_same_result_and_other_seeds_others);
    RUN_TEST(test_a_seeded_position_takes_every_position_of_the_operation);
    RUN_TEST(test_across_seeds_every_subset_of_a_torn_units_transitions_occurs);
    RUN_TEST(test_across_seeds_every_subset_of_a_torn_sectors_0_bits_occurs);
    RUN_TEST(test_the_generator_is_splitmix64);
    RUN_TEST(test_a_torn_program_is_the_one_the_procedure_in_nor_h_gives);
    RUN_TEST(test_a_torn_erase_is_the_one_the_procedure_in_nor_h_gives);
    RUN_TEST(test_on_the_hosts_geometry_every_position_of_a_page_and_an_erase_can_be_cut);
    return UNITY_END();
}
