/* SPDX-License-Identifier: MIT */
/*
 * Boot control and the slot writer (lib/boot), in the order of the header: the flash model the
 * tests stand on; initialisation and the layouts it refuses; the factory state; the record, its
 * bytes and what makes it count; every operation in every state against the reference model;
 * select, confirm and the slot writer one rule at a time; the log and its wear; the description;
 * and a long random run against the reference model on three geometries.
 *
 * The power-cut campaign is test_boot_cuts.c. Both use boot_test_support.h: a bench of one
 * flash device (the model of boot_test_flash.h, by default boot_test_nor.c) with a tracer in
 * front of it, test images whose id is their 32-byte header, and the reference model written
 * from the state machine of boot.h.
 *
 * Most tests use the host geometry (4096-byte sectors, 256-byte pages, 8-byte units, two
 * metadata sectors) with slots of three sectors, so that an entry is 128 bytes: a 96-byte
 * record, an 8-byte commit field and three 8-byte attempt fields, 32 entries per sector.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "boot/boot.h"
#include "boot_test_flash.h"
#include "boot_test_support.h"
#include "crc/crc.h"
#include "unity.h"

/* Images used across the tests. X1 is the factory image. */
enum { X1, X2, X3, X4, IMAGES };

static uint8_t s_image[IMAGES][8192];
static uint32_t s_len[IMAGES];

/* A spare buffer for an image changed in transfer. */
static uint8_t s_scratch[16384];

static char s_msg[256];
static char s_text_a[96];
static char s_text_b[96];

/* xorshift32, reset before every test. */
static uint32_t s_random_state;

static uint32_t random_u32(void)
{
    s_random_state ^= s_random_state << 13U;
    s_random_state ^= s_random_state >> 17U;
    s_random_state ^= s_random_state << 5U;
    return s_random_state;
}

static uint32_t random_below(uint32_t bound)
{
    return random_u32() % bound;
}

static const char *at(const char *what, unsigned long value)
{
    (void)snprintf(s_msg, sizeof(s_msg), "%s %lu", what, value);
    return s_msg;
}

void setUp(void)
{
    s_random_state = 0xB007C0DEU;
    s_len[X1] = bt_image_build(s_image[X1], sizeof(s_image[X1]), 0x01000000U, 11U, 1500U);
    s_len[X2] = bt_image_build(s_image[X2], sizeof(s_image[X2]), 0x01010000U, 22U, 3000U);
    s_len[X3] = bt_image_build(s_image[X3], sizeof(s_image[X3]), 0x01020000U, 33U, 700U);
    s_len[X4] = bt_image_build(s_image[X4], sizeof(s_image[X4]), 0x02000000U, 44U, 5000U);
    bt_setup(&bt_shape_host_small);
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------ helpers */

static void factory(uint8_t slot, int image)
{
    bt_raw_write(bt_slot_addr(slot), s_image[image], s_len[image]);
}

/* The expected abstract state equals what bt_boot reads now. */
static void expect_state(const bt_ref_t *expected, const char *where)
{
    bt_ref_t now;

    bt_ref_now(&now);
    if (!bt_ref_equal(expected, &now)) {
        (void)snprintf(s_msg, sizeof(s_msg), "%s: expected %s, flash says %s", where,
                       bt_ref_text(expected, s_text_a, sizeof(s_text_a)),
                       bt_ref_text(&now, s_text_b, sizeof(s_text_b)));
        TEST_FAIL_MESSAGE(s_msg);
    }
}

static boot_selection_t select_expect(boot_status_t status)
{
    boot_selection_t sel;

    TEST_ASSERT_EQUAL_INT(status, boot_select(&bt_boot, &sel));
    return sel;
}

static void expect_boot(const boot_selection_t *sel, uint8_t slot, uint8_t attempt)
{
    TEST_ASSERT_EQUAL_UINT8(slot, sel->slot);
    TEST_ASSERT_EQUAL(attempt != 0U, sel->trial);
    TEST_ASSERT_EQUAL_UINT8(attempt, sel->attempt);
    TEST_ASSERT_EQUAL_UINT32(bt_slot_addr(slot), sel->addr);
    TEST_ASSERT_EQUAL_UINT32(bt_slot_size(slot), sel->size);
}

static boot_description_t describe(void)
{
    boot_description_t d;

    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_describe(&bt_boot, &d));
    return d;
}

/* A test image changed in transfer: one payload byte differs. */
static const uint8_t *corrupted(int image)
{
    (void)memcpy(s_scratch, s_image[image], s_len[image]);
    s_scratch[BT_IMAGE_HEADER + 5U] ^= 0x10U;
    return s_scratch;
}

/* Factory image X1 in slot A, adopted: A CONFIRMED, B EMPTY. */
static void adopted(void)
{
    factory(0U, X1);
    const boot_selection_t sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
}

/* Adopted, then X2 committed into B: TRIAL with no attempt. */
static void trial_in_b(void)
{
    adopted();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 1U, s_image[X2], s_len[X2], 0U));
}

/* ------------------------------------------------------------------ the flash model */

/* The model of boot_test_flash.h on a small device of its own: 4 sectors of 512 bytes, pages
 * of 64, units of 8. These tests go through the three functions only, so they hold for any
 * model the boot tests are run on. */
static uint8_t s_model_mem[2048];
static boot_test_flash_t s_model;

static void model_setup(void)
{
    const flash_geometry_t g = {
        .size = 2048U, .sector_size = 512U, .page_size = 64U, .unit_size = 8U};

    (void)memset(s_model_mem, 0xFF, sizeof(s_model_mem));
    boot_test_flash_init(&s_model, s_model_mem, &g);
}

static flash_status_t model_program(uint32_t addr, const uint8_t *src, uint32_t len)
{
    return s_model.dev.program(s_model.dev.ctx, addr, src, len);
}

static flash_status_t model_erase(uint32_t addr)
{
    return s_model.dev.erase(s_model.dev.ctx, addr);
}

static flash_status_t model_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
    return s_model.dev.read(s_model.dev.ctx, addr, dst, len);
}

static void test_the_flash_model_follows_the_rules_of_flash_h(void)
{
    uint8_t data[128];
    uint8_t back[128];
    uint8_t before[2048];

    (void)printf("flash model: %s\n", boot_test_flash_name());
    model_setup();
    for (uint32_t i = 0U; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 37U);
    }

    /* Erased bytes read 0xFF; a program writes whole units within a page. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, model_read(0U, back, 64U));
    TEST_ASSERT_EACH_EQUAL_UINT8(0xFFU, back, 64U);
    TEST_ASSERT_EQUAL_INT(FLASH_OK, model_program(64U, data, 64U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, model_read(64U, back, 64U));
    TEST_ASSERT_EQUAL_MEMORY(data, back, 64U);

    /* Refused, changing nothing: out of range, length 0, part of a unit, across a page, an
     * erase off a sector start, and a program over a unit that is not erased (here the second
     * unit of a call whose first unit is erased). */
    (void)memcpy(before, s_model_mem, sizeof(before));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, model_program(2048U, data, 8U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, model_program(2040U, data, 16U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, model_read(0U, back, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, model_erase(2048U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, model_program(4U, data, 8U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, model_program(0U, data, 12U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, model_program(56U, data, 16U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, model_erase(256U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, model_program(120U, data, 8U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, model_program(184U, data, 8U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, model_program(176U, data, 16U));
    (void)memcpy(&before[184], data, 8U); /* the one program that was allowed */
    TEST_ASSERT_EQUAL_MEMORY(before, s_model_mem, sizeof(before));

    /* Write-once holds even for a value that would change nothing (all 0xFF over all 0xFF
     * is a program of an erased unit; 0xFF over a written unit is not allowed), for the same
     * value again, and for a value that only clears more bits (allowed on a plain NOR part, not
     * on one with a code per unit): the unit keeps what it holds. */
    (void)memset(back, 0xFF, 8U);
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, model_program(64U, back, 8U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, model_program(64U, data, 8U));
    (void)memset(back, 0x00, 8U);
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, model_program(64U, back, 8U));
    TEST_ASSERT_EQUAL_MEMORY(data, &s_model_mem[64], 8U);

    /* Erase sets the whole sector, and only it, to 0xFF. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, model_program(512U, data, 64U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, model_erase(0U));
    TEST_ASSERT_EACH_EQUAL_UINT8(0xFFU, s_model_mem, 512U);
    TEST_ASSERT_EQUAL_MEMORY(data, &s_model_mem[512], 64U);
}

static void test_the_flash_model_tears_a_program_as_decision_2_says(void)
{
    uint8_t data[64];
    uint8_t outcome[64];
    bool partial_seen = false;

    for (uint32_t i = 0U; i < sizeof(data); i++) {
        data[i] = (uint8_t)(0x5AU ^ (i * 29U));
    }

    for (uint32_t position = 0U; position < 8U; position++) {
        for (uint32_t seed = 0U; seed < 20U; seed++) {
            uint8_t dummy[8];

            model_setup();
            s_model.arm_next(s_model.model, position, seed);
            TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, model_program(64U, data, 64U));
            /* Units before the position written, the unit at it a subset of its transitions
             * (every 1 of the data is still 1), the rest untouched. */
            if (position > 0U) {
                TEST_ASSERT_EQUAL_MEMORY_MESSAGE(data, &s_model_mem[64], position * 8U,
                                                 at("units before position", position));
            }
            for (uint32_t i = position * 8U; i < ((position + 1U) * 8U); i++) {
                TEST_ASSERT_EQUAL_HEX8_MESSAGE(s_model_mem[64U + i],
                                               (uint8_t)(s_model_mem[64U + i] | data[i]),
                                               at("torn unit byte", i));
                partial_seen = partial_seen || ((s_model_mem[64U + i] != data[i]) &&
                                                (s_model_mem[64U + i] != 0xFFU));
            }
            if (position < 7U) {
                TEST_ASSERT_EACH_EQUAL_UINT8(0xFFU, &s_model_mem[64U + ((position + 1U) * 8U)],
                                             64U - ((position + 1U) * 8U));
            }
            TEST_ASSERT_EACH_EQUAL_UINT8(0xFFU, s_model_mem, 64U);
            TEST_ASSERT_EACH_EQUAL_UINT8(0xFFU, &s_model_mem[128], sizeof(s_model_mem) - 128U);

            /* Power lost, reads included, until power-on; then the torn state stays. */
            (void)memcpy(outcome, &s_model_mem[64], 64U);
            TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, model_read(0U, dummy, 8U));
            TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, model_program(1024U, data, 8U));
            TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, model_erase(1536U));
            s_model.power_on(s_model.model);
            TEST_ASSERT_EQUAL_INT(FLASH_OK, model_read(64U, dummy, 8U));
            TEST_ASSERT_EQUAL_MEMORY(outcome, &s_model_mem[64], 64U);
            TEST_ASSERT_EQUAL_INT(FLASH_OK, model_program(1024U, data, 8U));

            /* The same seed tears the same way. */
            model_setup();
            s_model.arm_next(s_model.model, position, seed);
            (void)model_program(64U, data, 64U);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(outcome, &s_model_mem[64], 64U, at("seed", seed));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(partial_seen, "no seed left a unit half programmed");
}

static void test_the_flash_model_tears_an_erase_as_decision_2_says(void)
{
    uint8_t data[64];
    bool partial_seen = false;

    for (uint32_t i = 0U; i < sizeof(data); i++) {
        data[i] = (uint8_t)(0xA5U ^ (i * 13U));
    }

    for (uint32_t seed = 0U; seed < 20U; seed++) {
        uint8_t before[512];

        model_setup();
        for (uint32_t page = 0U; page < 8U; page++) {
            TEST_ASSERT_EQUAL_INT(FLASH_OK, model_program(512U + (page * 64U), data, 64U));
        }
        (void)memcpy(before, &s_model_mem[512], sizeof(before));
        s_model.arm_next(s_model.model, 0U, seed);
        TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, model_erase(512U));
        /* Only 0 bits can have become 1; nothing outside the sector changed. */
        for (uint32_t i = 0U; i < sizeof(before); i++) {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(before[i], (uint8_t)(s_model_mem[512U + i] & before[i]),
                                           at("torn erase byte", i));
        }
        partial_seen = partial_seen || ((memcmp(before, &s_model_mem[512], 512U) != 0) &&
                                        (s_model_mem[512] != 0xFFU || s_model_mem[513] != 0xFFU));
        TEST_ASSERT_EACH_EQUAL_UINT8(0xFFU, s_model_mem, 512U);
        TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, model_erase(0U));
        s_model.power_on(s_model.model);
        TEST_ASSERT_EQUAL_INT(FLASH_OK, model_erase(512U));
        TEST_ASSERT_EACH_EQUAL_UINT8(0xFFU, &s_model_mem[512], 512U);
    }
    TEST_ASSERT_TRUE_MESSAGE(partial_seen, "no seed left an erase half done");
}

/* ------------------------------------------------------------------ init */

static void test_init_accepts_the_host_layout_and_reads_nothing(void)
{
    const bt_shape_t host = {.sector_size = 4096U,
                             .page_size = 256U,
                             .unit_size = 8U,
                             .meta_sectors = 2U,
                             .slot_sectors = 256U,
                             .spare_sectors = 0U};
    bt_counts_t counts;
    boot_description_t d;

    bt_setup(&host); /* boot_init() inside */
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(0U, counts.reads + counts.programs + counts.erases);
    TEST_ASSERT_EQUAL_UINT32(8192U, bt_slot_addr(0U));
    TEST_ASSERT_EQUAL_UINT32(1048576U, bt_slot_size(1U));

    d = describe();
    TEST_ASSERT_FALSE(d.have_record);
    TEST_ASSERT_EQUAL_UINT32(128U, d.entry_stride);
    TEST_ASSERT_EQUAL_UINT32(32U, d.entries_per_sector);
    TEST_ASSERT_EQUAL_UINT32(64U, d.entries_total);
    TEST_ASSERT_EQUAL_UINT32(8U, d.field_size);
    TEST_ASSERT_EQUAL_UINT32(96U, d.record_body);
    TEST_ASSERT_EQUAL_UINT32(0U, d.entries_used);
    TEST_ASSERT_FALSE(d.next_write_erases);
    TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, d.active_slot);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_EMPTY, d.slot[0].state);

    /* A whole image of 1 MiB fits the slot; one byte more does not. */
    factory(0U, X1);
    (void)select_expect(BOOT_OK);
    TEST_ASSERT_EQUAL_INT(BOOT_E_SIZE, boot_writer_begin(&bt_writer, &bt_boot, 1U, 1048577U));
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, 1048576U));
    {
        boot_writer_progress_t p;

        boot_writer_progress(&bt_writer, &p);
        TEST_ASSERT_EQUAL_UINT32(256U, p.sectors_total);
    }
}

static void expect_init(boot_status_t expected, const boot_config_t *config, const char *what)
{
    boot_t b;
    boot_selection_t sel;
    boot_description_t d;
    uint8_t slot = 0U;

    (void)memset(&b, 0xA5, sizeof(b));
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, boot_init(&b, config), what);
    if (expected != BOOT_OK) {
        /* A refused context is unusable. */
        TEST_ASSERT_EQUAL_INT_MESSAGE(BOOT_E_ARG, boot_select(&b, &sel), what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(BOOT_E_ARG, boot_confirm(&b, 0U), what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(BOOT_E_ARG, boot_describe(&b, &d), what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(BOOT_E_ARG, boot_slot_by_address(&b, 0U, &slot), what);
    }
}

static void test_init_refuses_every_layout_it_cannot_use(void)
{
    const boot_config_t good = bt_config;
    flash_dev_t dev;
    boot_config_t c = good;

    expect_init(BOOT_OK, &good, "the bench layout");
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_init(NULL, &good));
    expect_init(BOOT_E_ARG, NULL, "no configuration");
    c.dev = NULL;
    expect_init(BOOT_E_ARG, &c, "no device");

#define REFUSED(status, what, change)                                                              \
    do {                                                                                           \
        c = good;                                                                                  \
        dev = *good.dev;                                                                           \
        c.dev = &dev;                                                                              \
        change;                                                                                    \
        expect_init(status, &c, what);                                                             \
    } while (0)

    REFUSED(BOOT_E_ARG, "no read", dev.read = NULL);
    REFUSED(BOOT_E_ARG, "no program", dev.program = NULL);
    REFUSED(BOOT_E_ARG, "no erase", dev.erase = NULL);
    REFUSED(BOOT_E_ARG, "half a validator", c.validator.step = NULL);
    REFUSED(BOOT_E_ARG, "the other half", c.validator.start = NULL);
    REFUSED(BOOT_E_LAYOUT, "sector not a power of two", dev.geometry.sector_size = 3000U);
    REFUSED(BOOT_E_LAYOUT, "page not a power of two", dev.geometry.page_size = 200U);
    REFUSED(BOOT_E_LAYOUT, "unit not a power of two", dev.geometry.unit_size = 6U);
    REFUSED(BOOT_E_LAYOUT, "unit beyond the page", dev.geometry.unit_size = 512U);
    REFUSED(BOOT_E_LAYOUT, "page beyond the sector", dev.geometry.page_size = 8192U);
    REFUSED(BOOT_E_LAYOUT, "size not whole sectors", dev.geometry.size += 512U);
    REFUSED(BOOT_E_LAYOUT, "size 0", dev.geometry.size = 0U);
    REFUSED(BOOT_E_LAYOUT, "unit beyond BOOT_UNIT_MAX", dev.geometry.unit_size = 128U);
    REFUSED(BOOT_E_LAYOUT, "page beyond BOOT_PAGE_MAX", dev.geometry.page_size = 1024U);
    REFUSED(BOOT_E_LAYOUT, "page smaller than a field",
            (dev.geometry.page_size = 4U, dev.geometry.unit_size = 4U));
    REFUSED(BOOT_E_LAYOUT, "one metadata sector", c.meta_sectors = 1U);
    REFUSED(BOOT_E_LAYOUT, "nine metadata sectors", c.meta_sectors = 9U);
    REFUSED(BOOT_E_LAYOUT, "metadata off a sector start", c.meta_addr = 256U);
    REFUSED(BOOT_E_LAYOUT, "metadata beyond the device", c.meta_addr = bt_device_size());
    REFUSED(BOOT_E_LAYOUT, "metadata over slot A", c.meta_sectors = 3U);
    REFUSED(BOOT_E_LAYOUT, "slot A off a sector start", c.slot[0].addr += 8U);
    REFUSED(BOOT_E_LAYOUT, "slot A not whole sectors", c.slot[0].size -= 8U);
    REFUSED(BOOT_E_LAYOUT, "slot A empty", c.slot[0].size = 0U);
    REFUSED(BOOT_E_LAYOUT, "slot B past the end", c.slot[1].size += 4096U);
    REFUSED(BOOT_E_LAYOUT, "slot B starts past the end", c.slot[1].addr = bt_device_size());
    REFUSED(BOOT_E_LAYOUT, "slot B wraps around", c.slot[1].size = 0xFFFFF000U);
    REFUSED(BOOT_E_LAYOUT, "slots overlap", c.slot[1].addr = c.slot[0].addr + 4096U);
    REFUSED(BOOT_E_LAYOUT, "slot B over the metadata", c.slot[1].addr = 0U);
    REFUSED(BOOT_E_LAYOUT, "slot A over the metadata", c.slot[0].addr = 4096U);
    REFUSED(BOOT_E_LAYOUT, "the device holds fewer sectors than the metadata",
            (dev.geometry.size = 4096U, c.meta_sectors = 2U));
    /* An entry of 384 bytes (64-byte units) does not fit a 256-byte sector. */
    REFUSED(BOOT_E_LAYOUT, "no entry fits a sector",
            (dev.geometry.sector_size = 256U, dev.geometry.page_size = 256U,
             dev.geometry.unit_size = 64U, c.meta_addr = 0U, c.slot[0].addr = 512U,
             c.slot[0].size = 256U, c.slot[1].addr = 768U, c.slot[1].size = 256U));

    /* Accepted: no validator at all (a node that only confirms), the regions in another order,
     * a gap between them. */
    c = good;
    c.validator.start = NULL;
    c.validator.step = NULL;
    expect_init(BOOT_OK, &c, "no validator");
    c = good;
    c.slot[0].addr = 0U;
    c.slot[1].addr = 12288U;
    c.meta_addr = 24576U;
    expect_init(BOOT_OK, &c, "metadata at the end");
    c = good;
    c.slot[0].size = 4096U;
    expect_init(BOOT_OK, &c, "a gap after slot A");
#undef REFUSED
}

static void test_the_entry_layout_follows_the_unit_and_page(void)
{
    /* unit, page, sector -> field, body, stride (boot.h, THE METADATA LOG) */
    static const uint32_t cases[][6] = {
        {1U, 256U, 4096U, 8U, 96U, 128U},   {4U, 256U, 4096U, 8U, 96U, 128U},
        {8U, 256U, 4096U, 8U, 96U, 128U},   {8U, 64U, 512U, 8U, 96U, 128U},
        {8U, 8U, 512U, 8U, 96U, 128U},      {16U, 256U, 4096U, 16U, 96U, 256U},
        {16U, 64U, 512U, 16U, 96U, 192U},   {32U, 256U, 4096U, 32U, 96U, 256U},
        {32U, 32U, 1024U, 32U, 96U, 224U},  {64U, 512U, 4096U, 64U, 128U, 512U},
        {64U, 128U, 4096U, 64U, 128U, 384U}};

    for (uint32_t i = 0U; i < (uint32_t)(sizeof(cases) / sizeof(cases[0])); i++) {
        const bt_shape_t shape = {.sector_size = cases[i][2],
                                  .page_size = cases[i][1],
                                  .unit_size = cases[i][0],
                                  .meta_sectors = 2U,
                                  .slot_sectors = 2U,
                                  .spare_sectors = 0U};
        boot_description_t d;

        bt_setup(&shape);
        d = describe();
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cases[i][3], d.field_size, at("field, case", i));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cases[i][4], d.record_body, at("body, case", i));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cases[i][5], d.entry_stride, at("stride, case", i));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cases[i][2] / cases[i][5], d.entries_per_sector,
                                         at("entries, case", i));
    }
}

static void test_an_uninitialised_context_or_writer_is_refused(void)
{
    static boot_t zeroed; /* static: all zero */
    static boot_writer_t idle;
    boot_selection_t sel;
    boot_description_t d;
    boot_writer_progress_t p;
    uint8_t slot = 7U;
    const uint8_t byte = 0U;

    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_select(&zeroed, &sel));
    TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, sel.slot);
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_select(NULL, &sel));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_select(&bt_boot, NULL));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_confirm(&zeroed, 0U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_confirm(&bt_boot, 2U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_describe(&zeroed, &d));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_describe(&bt_boot, NULL));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_slot_by_address(&bt_boot, bt_slot_addr(1U), NULL));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_slot_by_address(&zeroed, bt_slot_addr(1U), &slot));
    TEST_ASSERT_EQUAL_UINT8(7U, slot);
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_writer_begin(NULL, &bt_boot, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_writer_begin(&idle, &zeroed, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_writer_begin(&idle, &bt_boot, 2U, 100U));
    boot_writer_progress(&idle, &p);
    TEST_ASSERT_EQUAL_INT(BOOT_WRITER_IDLE, p.phase);
    TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, p.slot);
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_writer_erase_step(&idle));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_writer_erase_step(NULL));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_writer_write(&idle, 0U, &byte, 1U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_writer_write(NULL, 0U, &byte, 1U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_writer_finish_step(&idle));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_writer_finish_step(NULL));
    boot_writer_progress(NULL, &p);
    boot_writer_progress(&idle, NULL);

    /* No validator: select and the writer refuse, confirm and describe work. */
    {
        boot_config_t c = bt_config;
        boot_t b;

        c.validator.start = NULL;
        c.validator.step = NULL;
        TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_init(&b, &c));
        TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_select(&b, &sel));
        TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_writer_begin(&idle, &b, 1U, 100U));
        TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_confirm(&b, 1U));
        TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_describe(&b, &d));
    }
}

/* ------------------------------------------------------------------ the factory state */

static void test_select_adopts_the_factory_image_in_slot_a(void)
{
    uint8_t id[BOOT_IMAGE_ID_SIZE];
    boot_selection_t sel;
    boot_description_t d;
    bt_counts_t counts;

    factory(0U, X1);
    factory(1U, X2); /* both valid: A is adopted */
    bt_counts_reset();
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT32(0x01000000U, sel.version);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_ADOPT, sel.event);
    TEST_ASSERT_EQUAL_UINT8(0U, sel.event_slot);
    TEST_ASSERT_EQUAL_INT(BOOT_OK, sel.write_status);
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(2U, counts.programs); /* record and commit field */
    TEST_ASSERT_EQUAL_UINT32(0U, counts.erases);   /* the log was blank */

    d = describe();
    TEST_ASSERT_TRUE(d.have_record);
    TEST_ASSERT_EQUAL_UINT32(1U, d.seq);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_ADOPT, d.event);
    TEST_ASSERT_EQUAL_UINT8(0U, d.active_slot);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_CONFIRMED, d.slot[0].state);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_EMPTY, d.slot[1].state);
    TEST_ASSERT_TRUE(bt_image_check_mem(bt_slot_addr(0U), bt_slot_size(0U), id, NULL));
    TEST_ASSERT_EQUAL_MEMORY(id, d.slot[0].id, BOOT_IMAGE_ID_SIZE);
    TEST_ASSERT_EQUAL_UINT32(0x01000000U, d.slot[0].version);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFU, d.slot[1].version);

    /* The next select boots A and writes nothing. */
    bt_counts_reset();
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_NONE, sel.event);
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(0U, counts.programs + counts.erases);
}

static void test_select_adopts_slot_b_when_a_does_not_validate(void)
{
    boot_selection_t sel;

    factory(0U, X1);
    bt_flip_bit(bt_slot_addr(0U) + 100U, 3U);
    factory(1U, X2);
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 1U, 0U);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_ADOPT, sel.event);
    TEST_ASSERT_EQUAL_UINT8(1U, sel.event_slot);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_EMPTY, describe().slot[0].state);
    TEST_ASSERT_EQUAL_UINT8(1U, describe().active_slot);
}

static void test_with_no_valid_image_nothing_is_bootable_and_nothing_is_written(void)
{
    boot_selection_t sel;
    bt_counts_t counts;

    bt_counts_reset();
    sel = select_expect(BOOT_E_NOTHING_BOOTABLE);
    TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, sel.slot);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_NONE, sel.event);
    factory(0U, X1);
    bt_flip_bit(bt_slot_addr(0U) + 40U, 0U);
    sel = select_expect(BOOT_E_NOTHING_BOOTABLE);
    TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, sel.slot);
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(0U, counts.programs + counts.erases);
    TEST_ASSERT_FALSE(describe().have_record);
    /* Begin and confirm are refused in the factory state. */
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_writer_begin(&bt_writer, &bt_boot, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_confirm(&bt_boot, 0U));
}

static void test_garbage_in_the_metadata_is_not_a_record(void)
{
    boot_description_t d;
    boot_selection_t sel;

    /* Sector 0 full of noise, sector 1 noise in its first two entries. */
    for (uint32_t i = 0U; i < 4096U; i++) {
        bt_mem[i] = (uint8_t)random_u32();
    }
    for (uint32_t i = 4096U; i < (4096U + 256U); i++) {
        bt_mem[i] = (uint8_t)random_u32();
    }
    d = describe();
    TEST_ASSERT_FALSE(d.have_record);
    TEST_ASSERT_EQUAL_UINT32(0U, d.records_valid);
    TEST_ASSERT_EQUAL_UINT32(34U, d.entries_used);
    TEST_ASSERT_TRUE(d.next_write_erases); /* sector 0 is full: sector 1 is erased */
    TEST_ASSERT_EQUAL_UINT32(1U, d.next_sector);

    factory(0U, X1);
    sel = select_expect(BOOT_OK);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_ADOPT, sel.event);
    TEST_ASSERT_EQUAL_INT(BOOT_OK, sel.write_status);
    d = describe();
    TEST_ASSERT_TRUE(d.have_record);
    TEST_ASSERT_EQUAL_UINT32(1U, d.record_sector);
    TEST_ASSERT_EQUAL_UINT32(0U, d.record_index);
    TEST_ASSERT_EACH_EQUAL_UINT8(0xFFU, &bt_mem[4096U + 128U], 4096U - 128U);
}

/* ------------------------------------------------------------------ the record */

/* An encoder written from the record table of boot.h, independent of boot.c. */
typedef struct {
    uint32_t magic;
    uint8_t format;
    uint8_t event;
    uint8_t event_slot;
    uint8_t reserved;
    uint32_t seq;
    uint8_t state[2];
    uint8_t slot_reserved[2];
    uint32_t version[2];
    uint8_t id[2][BOOT_IMAGE_ID_SIZE];
} raw_record_t;

static void raw_put32(uint8_t *p, uint32_t v)
{
    for (uint32_t i = 0U; i < 4U; i++) {
        p[i] = (uint8_t)(v >> (8U * i));
    }
}

static void raw_encode(const raw_record_t *r, uint8_t body[96])
{
    (void)memset(body, 0xFF, 96U);
    raw_put32(&body[0], r->magic);
    body[4] = r->format;
    body[5] = r->event;
    body[6] = r->event_slot;
    body[7] = r->reserved;
    raw_put32(&body[8], r->seq);
    for (uint32_t s = 0U; s < 2U; s++) {
        uint8_t *slot = &body[12U + (40U * s)];

        slot[0] = r->state[s];
        slot[1] = r->slot_reserved[s];
        raw_put32(&slot[4], r->version[s]);
        (void)memcpy(&slot[8], r->id[s], BOOT_IMAGE_ID_SIZE);
    }
    raw_put32(&body[92], crc32_compute(body, 92U));
}

/* The commit field of an 8-byte field geometry: zero bits, counted bit by bit, and inverted. */
static void raw_commit(const uint8_t body[96], uint8_t field[8])
{
    uint32_t zeros = 0U;

    for (uint32_t i = 0U; i < 96U; i++) {
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            zeros += (((uint32_t)body[i] >> bit) & 1U) ^ 1U;
        }
    }
    raw_put32(&field[0], zeros);
    raw_put32(&field[4], ~zeros);
}

static raw_record_t raw_record(uint8_t event, uint8_t event_slot, uint32_t seq)
{
    raw_record_t r;

    (void)memset(&r, 0xFF, sizeof(r));
    r.magic = BOOT_RECORD_MAGIC;
    r.format = 1U;
    r.event = event;
    r.event_slot = event_slot;
    r.seq = seq;
    return r;
}

static void raw_slot(raw_record_t *r, uint8_t slot, boot_slot_state_t state, int image)
{
    r->state[slot] = (uint8_t)state;
    if (image >= 0) {
        (void)memcpy(r->id[slot], s_image[image], BOOT_IMAGE_ID_SIZE);
        r->version[slot] = (uint32_t)s_image[image][4] | ((uint32_t)s_image[image][5] << 8U) |
                           ((uint32_t)s_image[image][6] << 16U) |
                           ((uint32_t)s_image[image][7] << 24U);
    }
}

/* Write an entry (record and commit field) straight into memory, host-small geometry. */
static void raw_put_entry(uint32_t sector, uint32_t index, const raw_record_t *r)
{
    uint8_t body[96];
    uint8_t field[8];
    const uint32_t addr = (sector * 4096U) + (index * 128U);

    raw_encode(r, body);
    raw_commit(body, field);
    (void)memcpy(&bt_mem[addr], body, 96U);
    (void)memcpy(&bt_mem[addr + 96U], field, 8U);
}

static void expect_entry(uint32_t sector, uint32_t index, const raw_record_t *r, uint32_t attempts,
                         const char *what)
{
    uint8_t body[96];
    uint8_t field[8];
    const uint32_t addr = (sector * 4096U) + (index * 128U);

    raw_encode(r, body);
    raw_commit(body, field);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(body, &bt_mem[addr], 96U, what);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(field, &bt_mem[addr + 96U], 8U, what);
    for (uint32_t k = 0U; k < 3U; k++) {
        TEST_ASSERT_EACH_EQUAL_UINT8_MESSAGE((k < attempts) ? 0x00U : 0xFFU,
                                             &bt_mem[addr + 104U + (8U * k)], 8U, what);
    }
}

static void test_the_record_bytes_are_as_documented(void)
{
    raw_record_t r;

    adopted();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 1U, s_image[X2], s_len[X2], 0U));
    (void)select_expect(BOOT_OK); /* attempt 1 of X2 */

    r = raw_record((uint8_t)BOOT_EVENT_ADOPT, 0U, 1U);
    raw_slot(&r, 0U, BOOT_SLOT_CONFIRMED, X1);
    raw_slot(&r, 1U, BOOT_SLOT_EMPTY, -1);
    expect_entry(0U, 0U, &r, 0U, "entry 0: ADOPT");

    r = raw_record((uint8_t)BOOT_EVENT_BEGIN, 1U, 2U);
    raw_slot(&r, 0U, BOOT_SLOT_CONFIRMED, X1);
    raw_slot(&r, 1U, BOOT_SLOT_WRITING, -1);
    expect_entry(0U, 1U, &r, 0U, "entry 1: BEGIN");

    r = raw_record((uint8_t)BOOT_EVENT_COMMIT, 1U, 3U);
    raw_slot(&r, 0U, BOOT_SLOT_CONFIRMED, X1);
    raw_slot(&r, 1U, BOOT_SLOT_TRIAL, X2);
    expect_entry(0U, 2U, &r, 1U, "entry 2: COMMIT with one attempt");

    /* Nothing after entry 2 (byte 384 on) is written. */
    TEST_ASSERT_EACH_EQUAL_UINT8(0xFFU, &bt_mem[384], 8192U - 384U);

    /* The values the documentation gives. */
    TEST_ASSERT_EQUAL_HEX8('F', bt_mem[0]);
    TEST_ASSERT_EQUAL_HEX8('C', bt_mem[1]);
    TEST_ASSERT_EQUAL_HEX8('N', bt_mem[2]);
    TEST_ASSERT_EQUAL_HEX8('B', bt_mem[3]);
    TEST_ASSERT_EQUAL_INT(0, BOOT_SLOT_EMPTY);
    TEST_ASSERT_EQUAL_INT(1, BOOT_SLOT_WRITING);
    TEST_ASSERT_EQUAL_INT(2, BOOT_SLOT_TRIAL);
    TEST_ASSERT_EQUAL_INT(3, BOOT_SLOT_CONFIRMED);
    TEST_ASSERT_EQUAL_INT(4, BOOT_SLOT_PREVIOUS);
    TEST_ASSERT_EQUAL_INT(5, BOOT_SLOT_REJECTED);
    TEST_ASSERT_EQUAL_INT(1, BOOT_EVENT_ADOPT);
    TEST_ASSERT_EQUAL_INT(7, BOOT_EVENT_FALLBACK);
}

/* After adopt and begin: entry 1 (seq 2) is the newest; damage it and see seq 1 win. */
static void expect_entry1_rejected(const char *what)
{
    const boot_description_t d = describe();

    TEST_ASSERT_TRUE_MESSAGE(d.have_record, what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, d.seq, what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, d.records_valid, what);
}

static void test_every_damage_that_sets_bits_rejects_the_record(void)
{
    static uint8_t saved[8192];
    uint32_t zero_bits = 0U;

    adopted();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, 100U));
    TEST_ASSERT_EQUAL_UINT32(2U, describe().seq);
    (void)memcpy(saved, bt_mem, sizeof(saved));

    /* Every single 0 bit of the record and its commit field set to 1 (what a torn program or
     * a torn erase can do), and every 1 bit cleared (outside the tear model). */
    for (uint32_t i = 128U; i < (128U + 104U); i++) {
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            zero_bits += (((uint32_t)saved[i] >> bit) & 1U) ^ 1U;
            bt_flip_bit(i, bit);
            expect_entry1_rejected(at("bit flipped at byte", i));
            bt_flip_bit(i, bit);
        }
    }
    TEST_ASSERT_GREATER_THAN_UINT32(300U, zero_bits);

    /* 3000 random sets of 0 bits raised to 1, of every density. */
    for (uint32_t trial = 0U; trial < 3000U; trial++) {
        const uint32_t density = 1U + random_below(16U);
        bool changed = false;

        for (uint32_t i = 128U; i < (128U + 104U); i++) {
            const uint8_t zeros = (uint8_t)~saved[i];
            uint8_t raise = 0U;

            for (uint32_t bit = 0U; bit < 8U; bit++) {
                if (((((uint32_t)zeros >> bit) & 1U) != 0U) && (random_below(16U) < density)) {
                    raise = (uint8_t)(raise | (1U << bit));
                }
            }
            bt_mem[i] = (uint8_t)(saved[i] | raise);
            changed = changed || (raise != 0U);
        }
        if (changed) {
            expect_entry1_rejected(at("random damage", trial));
        }
        (void)memcpy(&bt_mem[128], &saved[128], 104U);
    }
    TEST_ASSERT_EQUAL_UINT32(2U, describe().seq);
}

/* Damage that keeps the number of 0 bits (outside the tear model, which only leaves bits at 1)
 * passes the commit field; the CRC is what catches it. */
static void test_damage_the_commit_field_cannot_see_is_caught_by_the_crc(void)
{
    static uint8_t saved[8192];
    const uint32_t entry = 128U; /* entry 1: the BEGIN record */
    const uint32_t id_a = entry + 12U + 8U;
    const uint32_t version_a = entry + 12U + 4U;

    adopted();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, 100U));
    (void)memcpy(saved, bt_mem, sizeof(saved));

    /* Two different bytes of slot A's id swapped. */
    TEST_ASSERT_NOT_EQUAL(bt_mem[id_a], bt_mem[id_a + 1U]);
    bt_mem[id_a] = saved[id_a + 1U];
    bt_mem[id_a + 1U] = saved[id_a];
    expect_entry1_rejected("two id bytes swapped");
    (void)memcpy(bt_mem, saved, sizeof(saved));

    /* One bit of slot A's version from 1 to 0 and another from 0 to 1 (version 0x01000000:
     * bit 0 of its top byte is 1, bit 1 is 0). */
    TEST_ASSERT_EQUAL_HEX8(0x01U, bt_mem[version_a + 3U]);
    bt_mem[version_a + 3U] = 0x02U;
    expect_entry1_rejected("one bit cleared, one set");
    (void)memcpy(bt_mem, saved, sizeof(saved));
    TEST_ASSERT_EQUAL_UINT32(2U, describe().seq);
}

static void test_a_record_without_its_commit_field_does_not_count(void)
{
    raw_record_t r;

    adopted();
    r = raw_record((uint8_t)BOOT_EVENT_BEGIN, 1U, 2U);
    raw_slot(&r, 0U, BOOT_SLOT_CONFIRMED, X1);
    raw_slot(&r, 1U, BOOT_SLOT_WRITING, -1);
    raw_put_entry(0U, 1U, &r);
    TEST_ASSERT_EQUAL_UINT32(2U, describe().seq);

    /* The same record with its commit field erased, or with one of another record. */
    (void)memset(&bt_mem[128U + 96U], 0xFF, 8U);
    TEST_ASSERT_EQUAL_UINT32(1U, describe().seq);
    TEST_ASSERT_EQUAL_UINT32(2U, describe().entries_used);
    (void)memcpy(&bt_mem[128U + 96U], &bt_mem[96], 8U);
    TEST_ASSERT_EQUAL_UINT32(1U, describe().seq);

    /* The next record goes after the dead entry, never into it. */
    TEST_ASSERT_EQUAL_UINT32(2U, describe().next_index);
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, 100U));
    TEST_ASSERT_EQUAL_UINT32(2U, describe().record_index);
    TEST_ASSERT_EQUAL_UINT32(2U, describe().seq);
}

static void test_a_record_must_be_well_formed(void)
{
    raw_record_t good = raw_record((uint8_t)BOOT_EVENT_BEGIN, 1U, 2U);
    raw_record_t r;

    raw_slot(&good, 0U, BOOT_SLOT_CONFIRMED, X1);
    raw_slot(&good, 1U, BOOT_SLOT_WRITING, -1);
    adopted();

#define MALFORMED(what, change)                                                                    \
    do {                                                                                           \
        r = good;                                                                                  \
        change;                                                                                    \
        (void)memset(&bt_mem[128], 0xFF, 128U);                                                    \
        raw_put_entry(0U, 1U, &r);                                                                 \
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, describe().seq, what);                                \
    } while (0)

    /* Each has a matching CRC and commit field, and is still not a record. */
    MALFORMED("magic", r.magic = 0x424E4347U);
    MALFORMED("format version 2", r.format = 2U);
    MALFORMED("reserved byte", r.reserved = 0x00U);
    MALFORMED("event NONE", r.event = 0U);
    MALFORMED("event 8", r.event = 8U);
    MALFORMED("event slot 2", r.event_slot = 2U);
    MALFORMED("sequence number 0", r.seq = 0U);
    MALFORMED("sequence number 0xFFFFFFFF", r.seq = 0xFFFFFFFFU);
    MALFORMED("state 6", r.state[1] = 6U);
    MALFORMED("slot reserved byte", r.slot_reserved[0] = 0x7FU);
    MALFORMED("two CONFIRMED", r.state[1] = (uint8_t)BOOT_SLOT_CONFIRMED);
    MALFORMED("no CONFIRMED", r.state[0] = (uint8_t)BOOT_SLOT_PREVIOUS);
    MALFORMED("two TRIAL",
              (r.state[0] = (uint8_t)BOOT_SLOT_TRIAL, r.state[1] = (uint8_t)BOOT_SLOT_TRIAL));
    MALFORMED("WRITING with a version", r.version[1] = 5U);
    MALFORMED("WRITING with an id", r.id[1][31] = 0x00U);
    MALFORMED("EMPTY with an id", (r.state[1] = (uint8_t)BOOT_SLOT_EMPTY, r.id[1][0] = 0U));

    /* And the good one is. */
    r = good;
    (void)memset(&bt_mem[128], 0xFF, 128U);
    raw_put_entry(0U, 1U, &r);
    TEST_ASSERT_EQUAL_UINT32(2U, describe().seq);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_WRITING, describe().slot[1].state);
#undef MALFORMED
}

/* Entry `index` of sector `sector` used, but no record: a torn one. */
static void raw_put_torn(uint32_t sector, uint32_t index)
{
    (void)memset(&bt_mem[(sector * 4096U) + (index * 128U)], 0x00, 40U);
}

static void test_the_newest_valid_record_wins_wherever_it_is(void)
{
    raw_record_t r = raw_record((uint8_t)BOOT_EVENT_ADOPT, 0U, 7U);
    boot_description_t d;

    bt_counts_t counts;

    /* Sector 1: torn entries 0..4, seq 7 at 5 and seq 8 at 6 (B EMPTY), then a torn entry.
     * Sector 0: torn entries 0..2, seq 9 at 3 (B WRITING). */
    for (uint32_t i = 0U; i < 5U; i++) {
        raw_put_torn(1U, i);
    }
    raw_slot(&r, 0U, BOOT_SLOT_CONFIRMED, X1);
    raw_slot(&r, 1U, BOOT_SLOT_EMPTY, -1);
    raw_put_entry(1U, 5U, &r);
    r.seq = 8U;
    raw_put_entry(1U, 6U, &r);
    raw_put_torn(1U, 7U);
    r.seq = 9U;
    r.event = (uint8_t)BOOT_EVENT_BEGIN;
    r.event_slot = 1U;
    r.state[1] = (uint8_t)BOOT_SLOT_WRITING;
    raw_put_entry(0U, 3U, &r);
    for (uint32_t i = 0U; i < 3U; i++) {
        raw_put_torn(0U, i);
    }
    d = describe();
    TEST_ASSERT_EQUAL_UINT32(9U, d.seq);
    TEST_ASSERT_EQUAL_UINT32(0U, d.record_sector);
    TEST_ASSERT_EQUAL_UINT32(3U, d.record_index);
    TEST_ASSERT_EQUAL_UINT32(3U, d.records_valid);
    TEST_ASSERT_EQUAL_UINT32(12U, d.entries_used);
    TEST_ASSERT_EQUAL_UINT32(4U, d.next_index); /* the end of the used part of sector 0 */
    TEST_ASSERT_EQUAL_UINT32(0U, d.next_sector);
    TEST_ASSERT_FALSE(d.next_write_erases);
    /* The scan of the operations (which decodes only the last valid record of each used part)
     * finds the same one: with seq 9 the newest, B is WRITING, and a begin of B writes nothing. */
    factory(0U, X1);
    bt_counts_reset();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, 100U));
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(0U, counts.programs + counts.erases);

    /* The used part of a sector ends at its first erased entry: a record after it is not read
     * (no sequence of operations writes one there; a torn erase of a sector that holds only old
     * records can leave one, and that sector is erased before it is written again). */
    r.seq = 10U;
    raw_put_entry(0U, 5U, &r);
    d = describe();
    TEST_ASSERT_EQUAL_UINT32(9U, d.seq);
    TEST_ASSERT_EQUAL_UINT32(3U, d.records_valid);
    TEST_ASSERT_EQUAL_UINT32(4U, d.next_index);
}

/* ------------------------------------------------------------------ every operation in every
 * state, against the reference model */

enum {
    S_FACTORY,
    S_FACTORY_B, /* the factory image in B only */
    S_ADOPTED,
    S_WRITING,
    S_TRIAL0,
    S_TRIAL1,
    S_TRIAL2,
    S_TRIAL3,
    S_PREVIOUS,
    S_REJECTED,
    S_STATES
};

enum { D_NONE, D_ACTIVE, D_OTHER, D_DAMAGES };

enum {
    OP_SELECT,
    OP_BEGIN_A,
    OP_BEGIN_B,
    OP_CONFIRM_A,
    OP_CONFIRM_B,
    OP_UPDATE,
    OP_UPDATE_BAD,
    OPS
};

/* Bring the bench to state `which` from the factory, checking every step against `ref`. */
static void reach_state(int which, bt_ref_t *ref)
{
    bt_truth_t truth;
    bt_ref_choice_t choice;

    bt_setup(&bt_shape_host_small);
    bt_ref_factory(ref);
    if (which == S_FACTORY_B) {
        factory(1U, X1);
        return;
    }
    factory(0U, X1);
    if (which == S_FACTORY) {
        return;
    }
    bt_truth(&truth);
    bt_ref_select(ref, &truth, &choice);
    (void)select_expect(BOOT_OK);
    if (which == S_ADOPTED) {
        return;
    }
    if (which == S_WRITING) {
        TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, s_len[X4]));
        TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_ref_begin(ref, 1U, s_len[X4], bt_slot_size(1U)));
        TEST_ASSERT_EQUAL_INT(BOOT_BUSY, boot_writer_erase_step(&bt_writer)); /* half erased */
        return;
    }
    TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 1U, s_image[X2], s_len[X2], 3U));
    (void)bt_ref_begin(ref, 1U, s_len[X2], bt_slot_size(1U));
    bt_ref_commit(ref, 1U, s_image[X2]);

    /* Selects: k attempts for TRIAL(k), one before the confirm for PREVIOUS, three attempts and
     * the rollback for REJECTED. */
    int selects = which - S_TRIAL0;

    if (which == S_PREVIOUS) {
        selects = 1;
    } else if (which == S_REJECTED) {
        selects = 4;
    } else {
        /* TRIAL(k): k selects */
    }

    for (int k = 0; k < selects; k++) {
        boot_selection_t sel;

        bt_truth(&truth);
        bt_ref_select(ref, &truth, &choice);
        sel = select_expect(BOOT_OK);
        TEST_ASSERT_EQUAL_UINT8(choice.slot, sel.slot);
        TEST_ASSERT_EQUAL_UINT8(choice.attempt, sel.attempt);
    }
    if (which == S_PREVIOUS) {
        TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_confirm(&bt_boot, 1U));
        TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_ref_confirm(ref, 1U));
    }
}

static void damage(const bt_ref_t *ref, int which)
{
    const uint8_t c = ref->have ? bt_ref_confirmed(ref) : 0U;
    const uint8_t slot = (which == D_ACTIVE) ? c : (uint8_t)(1U - c);

    if (which != D_NONE) {
        bt_flip_bit(bt_slot_addr(slot) + BT_IMAGE_HEADER + 9U, 6U);
    }
}

static void test_every_operation_in_every_state_matches_the_reference_model(void)
{
    uint32_t cases = 0U;
    uint32_t refused = 0U;

    for (int state = 0; state < S_STATES; state++) {
        for (int dmg = 0; dmg < D_DAMAGES; dmg++) {
            for (int op = 0; op < OPS; op++) {
                bt_ref_t ref;
                bt_ref_t before;
                bt_truth_t truth;
                boot_status_t want = BOOT_OK;
                boot_status_t got = BOOT_OK;
                char where[128];

                reach_state(state, &ref);
                damage(&ref, dmg);
                bt_reboot();
                expect_state(&ref, "state reached");
                before = ref;
                (void)snprintf(where, sizeof(where), "state %d, damage %d, operation %d", state,
                               dmg, op);
                bt_truth(&truth);
                const uint8_t o = ref.have ? (uint8_t)(1U - bt_ref_confirmed(&ref)) : 1U;

                switch (op) {
                case OP_SELECT: {
                    bt_ref_choice_t choice;
                    boot_selection_t sel;

                    bt_ref_select(&ref, &truth, &choice);
                    want = choice.status;
                    got = boot_select(&bt_boot, &sel);
                    TEST_ASSERT_EQUAL_UINT8_MESSAGE(choice.slot, sel.slot, where);
                    TEST_ASSERT_EQUAL_MESSAGE(choice.trial, sel.trial, where);
                    TEST_ASSERT_EQUAL_UINT8_MESSAGE(choice.attempt, sel.attempt, where);
                    TEST_ASSERT_EQUAL_INT_MESSAGE(choice.event, sel.event, where);
                    TEST_ASSERT_EQUAL_UINT8_MESSAGE(choice.event_slot, sel.event_slot, where);
                    TEST_ASSERT_EQUAL_INT_MESSAGE(BOOT_OK, sel.write_status, where);
                    break;
                }
                case OP_BEGIN_A:
                case OP_BEGIN_B: {
                    const uint8_t slot = (uint8_t)((op == OP_BEGIN_A) ? 0U : 1U);

                    want = bt_ref_begin(&ref, slot, s_len[X3], bt_slot_size(slot));
                    got = boot_writer_begin(&bt_writer, &bt_boot, slot, s_len[X3]);
                    break;
                }
                case OP_CONFIRM_A:
                case OP_CONFIRM_B: {
                    const uint8_t slot = (uint8_t)((op == OP_CONFIRM_A) ? 0U : 1U);

                    want = bt_ref_confirm(&ref, slot);
                    got = boot_confirm(&bt_boot, slot);
                    break;
                }
                case OP_UPDATE:
                case OP_UPDATE_BAD:
                default: {
                    const uint8_t *image = (op == OP_UPDATE) ? s_image[X3] : corrupted(X3);

                    want = bt_ref_begin(&ref, o, s_len[X3], bt_slot_size(o));
                    if ((want == BOOT_OK) && (op == OP_UPDATE)) {
                        bt_ref_commit(&ref, o, s_image[X3]);
                    } else if (want == BOOT_OK) {
                        want = BOOT_E_INVALID;
                    } else {
                        /* refused at begin */
                    }
                    got = bt_update(&bt_boot, o, image, s_len[X3], (uint32_t)op);
                    break;
                }
                }

                TEST_ASSERT_EQUAL_INT_MESSAGE(want, got, where);
                expect_state(&ref, where);
                bt_reboot();
                expect_state(&ref, where);
                cases++;
                refused += bt_ref_equal(&before, &ref) ? 1U : 0U;
            }
        }
    }
    (void)printf("%lu cases of state x damage x operation, %lu of them changed nothing\n",
                 (unsigned long)cases, (unsigned long)refused);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(S_STATES * D_DAMAGES * OPS), cases);
}

/* ------------------------------------------------------------------ select and confirm */

static void test_a_trial_gets_three_attempts_then_is_rolled_back(void)
{
    boot_selection_t sel;
    boot_description_t d;
    bt_counts_t counts;
    const uint32_t entry = bt_entry_addr(0U, 2U); /* the COMMIT record */

    trial_in_b();
    d = describe();
    TEST_ASSERT_EQUAL_UINT8(1U, d.trial_slot);
    TEST_ASSERT_EQUAL_UINT8(0U, d.attempts_used);
    TEST_ASSERT_EQUAL_UINT8(3U, d.attempts_left);

    for (uint8_t attempt = 1U; attempt <= 3U; attempt++) {
        bt_counts_reset();
        sel = select_expect(BOOT_OK);
        expect_boot(&sel, 1U, attempt);
        TEST_ASSERT_EQUAL_UINT32(0x01010000U, sel.version);
        TEST_ASSERT_EQUAL_INT(BOOT_EVENT_NONE, sel.event);
        bt_counts(&counts);
        /* One program of one unit, all zero, in the record's attempt field. */
        TEST_ASSERT_EQUAL_UINT32(1U, counts.programs);
        TEST_ASSERT_EQUAL_UINT32(1U, counts.units);
        TEST_ASSERT_EQUAL_UINT32(0U, counts.erases);
        TEST_ASSERT_EACH_EQUAL_UINT8(0x00U, &bt_mem[entry + 96U + (8U * attempt)], 8U);
        d = describe();
        TEST_ASSERT_EQUAL_UINT8(attempt, d.attempts_used);
        TEST_ASSERT_EQUAL_UINT8(3U - attempt, d.attempts_left);
    }

    /* The fourth select rolls back without validating B again, and boots A. */
    bt_checker.starts = 0U;
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_ROLLBACK, sel.event);
    TEST_ASSERT_EQUAL_UINT8(1U, sel.event_slot);
    TEST_ASSERT_EQUAL_UINT32(1U, bt_checker.starts);
    d = describe();
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_REJECTED, d.slot[1].state);
    TEST_ASSERT_EQUAL_MEMORY(s_image[X2], d.slot[1].id, BOOT_IMAGE_ID_SIZE);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_ROLLBACK, d.event);
    TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, d.trial_slot);
    TEST_ASSERT_EQUAL_UINT8(0U, d.attempts_left);

    /* From then on A, with nothing written. */
    bt_counts_reset();
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_NONE, sel.event);
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(0U, counts.programs + counts.erases);
}

static void test_a_partly_written_attempt_counts_against_the_trial(void)
{
    const uint32_t entry = bt_entry_addr(0U, 0U);
    boot_selection_t sel;

    trial_in_b();
    /* Any non-erased byte in an attempt field counts, wherever it is. */
    bt_mem[bt_entry_addr(0U, 2U) + 96U + 8U + 5U] = 0xFEU;
    TEST_ASSERT_EQUAL_UINT8(1U, describe().attempts_used);
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 1U, 2U);
    /* Attempt fields of an older entry do not count. */
    (void)memset(&bt_mem[entry + 104U], 0x00, 24U);
    TEST_ASSERT_EQUAL_UINT8(2U, describe().attempts_used);
    /* One bit of the third field is enough to use it up. */
    bt_mem[bt_entry_addr(0U, 2U) + 96U + 24U] = 0x7FU;
    TEST_ASSERT_EQUAL_UINT8(3U, describe().attempts_used);
    sel = select_expect(BOOT_OK);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_ROLLBACK, sel.event);
}

static void test_a_trial_that_does_not_validate_is_rejected(void)
{
    boot_selection_t sel;
    bt_counts_t counts;

    trial_in_b();
    (void)select_expect(BOOT_OK); /* attempt 1 */
    bt_flip_bit(bt_slot_addr(1U) + 2000U, 1U);
    bt_counts_reset();
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_REJECT, sel.event);
    TEST_ASSERT_EQUAL_UINT8(1U, sel.event_slot);
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(2U, counts.programs); /* the record; no attempt field */
    TEST_ASSERT_EQUAL_UINT32(4U, describe().records_valid);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_REJECTED, describe().slot[1].state);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_REJECT, describe().event);

    /* Begin over the rejected slot, and the next update works. */
    TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 1U, s_image[X3], s_len[X3], 1U));
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 1U, 1U);
}

static void test_confirm_makes_the_trial_active_and_booting_it_writes_nothing(void)
{
    boot_selection_t sel;
    boot_description_t d;
    bt_counts_t counts;

    trial_in_b();
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_confirm(&bt_boot, 1U)); /* never booted */
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 1U, 1U);
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_confirm(&bt_boot, 0U)); /* not the trial */
    bt_counts_reset();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_confirm(&bt_boot, 1U));
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(2U, counts.programs);
    TEST_ASSERT_EQUAL_UINT32(0U, counts.erases);
    d = describe();
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_CONFIRMED, d.slot[1].state);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_PREVIOUS, d.slot[0].state);
    TEST_ASSERT_EQUAL_MEMORY(s_image[X1], d.slot[0].id, BOOT_IMAGE_ID_SIZE);
    TEST_ASSERT_EQUAL_UINT8(1U, d.active_slot);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_CONFIRM, d.event);
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_confirm(&bt_boot, 1U)); /* confirmed already */

    for (int i = 0; i < 5; i++) {
        bt_counts_reset();
        sel = select_expect(BOOT_OK);
        expect_boot(&sel, 1U, 0U);
        bt_counts(&counts);
        TEST_ASSERT_EQUAL_UINT32(0U, counts.programs + counts.erases);
    }

    /* The third attempt may still confirm. */
    TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 0U, s_image[X3], s_len[X3], 2U));
    for (uint8_t a = 1U; a <= 3U; a++) {
        sel = select_expect(BOOT_OK);
        expect_boot(&sel, 0U, a);
    }
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_confirm(&bt_boot, 0U));
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
}

static void test_an_active_image_that_stops_validating_falls_back_to_the_previous_one(void)
{
    boot_selection_t sel;
    boot_description_t d;

    trial_in_b();
    (void)select_expect(BOOT_OK);
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_confirm(&bt_boot, 1U));
    bt_flip_bit(bt_slot_addr(1U) + BT_IMAGE_HEADER + 100U, 7U);

    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_FALLBACK, sel.event);
    TEST_ASSERT_EQUAL_UINT8(0U, sel.event_slot);
    TEST_ASSERT_EQUAL_UINT32(0x01000000U, sel.version);
    d = describe();
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_CONFIRMED, d.slot[0].state);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_REJECTED, d.slot[1].state);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_FALLBACK, d.event);

    /* And if A stops validating too, nothing is bootable: a REJECTED image is never booted,
     * even one that validates again. */
    bt_flip_bit(bt_slot_addr(1U) + BT_IMAGE_HEADER + 100U, 7U);
    bt_flip_bit(bt_slot_addr(0U) + BT_IMAGE_HEADER + 1U, 0U);
    sel = select_expect(BOOT_E_NOTHING_BOOTABLE);
    TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, sel.slot);
}

static void test_an_image_rewritten_behind_the_metadata_is_never_taken_for_the_recorded_one(void)
{
    boot_selection_t sel;

    /* A valid image of another id where the CONFIRMED one was, with a PREVIOUS image: the
     * fallback is booted. */
    trial_in_b();
    (void)select_expect(BOOT_OK);
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_confirm(&bt_boot, 1U));
    factory(1U, X4);
    TEST_ASSERT_TRUE(bt_image_check_mem(bt_slot_addr(1U), bt_slot_size(1U), NULL, NULL));
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_FALLBACK, sel.event);

    /* Without one: nothing bootable, although slot A validates. */
    bt_setup(&bt_shape_host_small);
    adopted();
    factory(0U, X3);
    (void)select_expect(BOOT_E_NOTHING_BOOTABLE);

    /* A TRIAL replaced by another valid image is rejected, not booted. */
    bt_setup(&bt_shape_host_small);
    trial_in_b();
    factory(1U, X3);
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_REJECT, sel.event);

    /* A PREVIOUS image replaced by another one is not a fallback. */
    bt_setup(&bt_shape_host_small);
    trial_in_b();
    (void)select_expect(BOOT_OK);
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_confirm(&bt_boot, 1U));
    factory(0U, X3);
    bt_flip_bit(bt_slot_addr(1U) + BT_IMAGE_HEADER, 0U);
    (void)select_expect(BOOT_E_NOTHING_BOOTABLE);
}

static void test_a_failed_write_never_boots_a_trial(void)
{
    boot_selection_t sel;

    /* The attempt cannot be written: no boot of the trial, a rollback instead. */
    trial_in_b();
    bt_fail_next(FLASH_E_NOT_ERASED);
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(BOOT_E_FLASH, sel.write_status);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_ROLLBACK, sel.event);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_REJECTED, describe().slot[1].state);

    /* The attempt and then the rollback record cannot be written: A is still booted, and the
     * trial, with no attempt counted, is not. */
    bt_setup(&bt_shape_host_small);
    trial_in_b();
    bt_fail_next(FLASH_E_ALIGN);
    bt_fail_next(FLASH_E_ALIGN);
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(BOOT_E_FLASH, sel.write_status);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_TRIAL, describe().slot[1].state);
    TEST_ASSERT_EQUAL_UINT8(0U, describe().attempts_used);

    /* The adoption record cannot be written: the only image is booted anyway. */
    bt_setup(&bt_shape_host_small);
    factory(0U, X1);
    bt_fail_next(FLASH_E_RANGE);
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_ADOPT, sel.event);
    TEST_ASSERT_EQUAL_INT(BOOT_E_FLASH, sel.write_status);
    TEST_ASSERT_FALSE(describe().have_record);

    /* The fallback record cannot be written: the previous image is booted anyway. */
    bt_setup(&bt_shape_host_small);
    trial_in_b();
    (void)select_expect(BOOT_OK);
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_confirm(&bt_boot, 1U));
    bt_flip_bit(bt_slot_addr(1U) + 50U, 2U);
    bt_fail_next(FLASH_E_NOT_ERASED);
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_FALLBACK, sel.event);
    TEST_ASSERT_EQUAL_INT(BOOT_E_FLASH, sel.write_status);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_CONFIRMED, describe().slot[1].state);

    /* A power loss in any of these writes boots nothing. */
    bt_setup(&bt_shape_host_small);
    trial_in_b();
    bt_fail_next(FLASH_E_POWER_LOST);
    sel = select_expect(BOOT_E_POWER_LOST);
    TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, sel.slot);
}

static void test_select_stops_on_validator_errors_and_bounds_a_stalled_validator(void)
{
    boot_selection_t sel;
    bt_counts_t counts;

    trial_in_b();
    bt_checker.fail_reads = true;
    bt_counts_reset();
    sel = select_expect(BOOT_E_FLASH);
    TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, sel.slot);
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(0U, counts.programs + counts.erases);
    bt_checker.fail_reads = false;

    /* The same in the check of every other row: the factory state, the CONFIRMED slot, and the
     * PREVIOUS slot when the CONFIRMED one does not validate. Nothing is booted or written. */
    for (int row = 0; row < 3; row++) {
        bt_setup(&bt_shape_host_small);
        factory(0U, X1);
        if (row > 0) {
            (void)select_expect(BOOT_OK);
        }
        if (row == 2) {
            TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 1U, s_image[X2], s_len[X2], 0U));
            (void)select_expect(BOOT_OK);
            TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_confirm(&bt_boot, 1U));
            bt_flip_bit(bt_slot_addr(1U) + BT_IMAGE_HEADER + 3U, 2U);
        }
        bt_checker.starts = 0U;
        bt_checker.fail_at_start = (row == 2) ? 2U : 1U; /* row 2: B is checked, then A fails */
        bt_counts_reset();
        sel = select_expect(BOOT_E_FLASH);
        TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, sel.slot);
        TEST_ASSERT_EQUAL_INT(BOOT_EVENT_NONE, sel.event);
        bt_counts(&counts);
        TEST_ASSERT_EQUAL_UINT32(0U, counts.programs + counts.erases);
        TEST_ASSERT_EQUAL_UINT32((row == 2) ? 2U : 1U, bt_checker.starts);
        bt_checker.fail_at_start = 0U;
    }
    bt_setup(&bt_shape_host_small);
    trial_in_b();

    bt_checker.bogus = true;
    (void)select_expect(BOOT_E_FLASH);
    bt_checker.bogus = false;

    /* A validator that never finishes is stopped after slot-size steps, as INVALID: the trial
     * is rejected, and then A does not validate either. */
    bt_checker.stall = true;
    bt_checker.steps = 0U;
    sel = select_expect(BOOT_E_NOTHING_BOOTABLE);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_REJECT, sel.event);
    TEST_ASSERT_EQUAL_UINT32(2U * (bt_slot_size(0U) + 1U), bt_checker.steps);
    bt_checker.stall = false;

    /* Reads of the metadata that fail: nothing is decided, nothing written. */
    bt_setup(&bt_shape_host_small);
    trial_in_b();
    (void)select_expect(BOOT_OK);
    bt_counts_reset();
    for (int round = 0; round < 2; round++) {
        const boot_status_t expected = (round == 0) ? BOOT_E_POWER_LOST : BOOT_E_FLASH;
        boot_description_t d;

        if (round == 0) {
            bt_power_off();
        } else {
            bt_fail_reads(FLASH_E_RANGE);
        }
        TEST_ASSERT_EQUAL_INT(expected, boot_confirm(&bt_boot, 1U));
        TEST_ASSERT_EQUAL_INT(expected, boot_writer_begin(&bt_writer, &bt_boot, 0U, 10U));
        TEST_ASSERT_EQUAL_INT(expected, boot_select(&bt_boot, &sel));
        TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, sel.slot);
        TEST_ASSERT_EQUAL_INT(expected, boot_describe(&bt_boot, &d));
        TEST_ASSERT_FALSE(d.have_record);
        TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, d.active_slot);
        bt_power_on();
    }
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(0U, counts.programs + counts.erases);
    TEST_ASSERT_EQUAL_UINT8(1U, describe().attempts_used);
    TEST_ASSERT_EQUAL_UINT8(0U, describe().active_slot);
}

/* The sequence numbers end at 0xFFFFFFFE: the record with that number is the last one written
 * (at three records per update, after more than 1.4 billion updates); the state it names stays
 * usable for booting. */
static void test_no_record_follows_the_last_sequence_number(void)
{
    raw_record_t r = raw_record((uint8_t)BOOT_EVENT_CONFIRM, 0U, 0xFFFFFFFEU);
    boot_selection_t sel;
    bt_counts_t counts;

    raw_slot(&r, 0U, BOOT_SLOT_CONFIRMED, X1);
    raw_slot(&r, 1U, BOOT_SLOT_EMPTY, -1);
    raw_put_entry(0U, 0U, &r);
    factory(0U, X1);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFEU, describe().seq);
    sel = select_expect(BOOT_OK);
    expect_boot(&sel, 0U, 0U);
    bt_counts_reset();
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_writer_begin(&bt_writer, &bt_boot, 1U, 100U));
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(0U, counts.programs + counts.erases);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFEU, describe().seq);
}

static void test_a_record_that_does_not_read_back_is_an_error(void)
{
    adopted();
    bt_fail_next(FLASH_OK); /* the record program is dropped; the commit field lands */
    TEST_ASSERT_EQUAL_INT(BOOT_E_FLASH, boot_writer_begin(&bt_writer, &bt_boot, 1U, 100U));
    TEST_ASSERT_EQUAL_UINT32(1U, describe().seq);
    {
        boot_writer_progress_t p;

        boot_writer_progress(&bt_writer, &p);
        TEST_ASSERT_EQUAL_INT(BOOT_WRITER_FAILED, p.phase);
    }
    /* The next try goes after the dead entry and works. */
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, 100U));
    TEST_ASSERT_EQUAL_UINT32(2U, describe().record_index);
}

/* ------------------------------------------------------------------ the slot writer */

static void test_the_writer_refuses_calls_out_of_order(void)
{
    const uint8_t *img = s_image[X4]; /* 5032 bytes: two sectors */
    const uint32_t len = s_len[X4];
    boot_writer_progress_t p;

    adopted();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, len));
    boot_writer_progress(&bt_writer, &p);
    TEST_ASSERT_EQUAL_INT(BOOT_WRITER_ERASING, p.phase);
    TEST_ASSERT_EQUAL_UINT8(1U, p.slot);
    TEST_ASSERT_EQUAL_UINT32(2U, p.sectors_total);
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_write(&bt_writer, 0U, img, 10U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_finish_step(&bt_writer));
    TEST_ASSERT_EQUAL_INT(BOOT_BUSY, boot_writer_erase_step(&bt_writer));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_write(&bt_writer, 0U, img, 10U));
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_erase_step(&bt_writer));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_erase_step(&bt_writer));
    boot_writer_progress(&bt_writer, &p);
    TEST_ASSERT_EQUAL_INT(BOOT_WRITER_WRITING, p.phase);
    TEST_ASSERT_EQUAL_UINT32(2U, p.sectors_erased);

    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_writer_write(&bt_writer, 0U, NULL, 10U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_writer_write(&bt_writer, 0U, img, 0U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_write(&bt_writer, 1U, img, 10U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SIZE, boot_writer_write(&bt_writer, 0U, img, len + 1U));
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_write(&bt_writer, 0U, img, 300U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_write(&bt_writer, 0U, img, 10U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_write(&bt_writer, 301U, &img[301], 10U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_finish_step(&bt_writer)); /* not all */
    TEST_ASSERT_EQUAL_INT(BOOT_E_SIZE, boot_writer_write(&bt_writer, 300U, &img[300], len));
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_write(&bt_writer, 300U, &img[300], len - 300U - 1U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SIZE, boot_writer_write(&bt_writer, len - 1U, img, 2U));
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_write(&bt_writer, len - 1U, &img[len - 1U], 1U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SIZE, boot_writer_write(&bt_writer, len, img, 1U));
    boot_writer_progress(&bt_writer, &p);
    TEST_ASSERT_EQUAL_UINT32(len, p.written);

    TEST_ASSERT_EQUAL_INT(BOOT_BUSY, boot_writer_finish_step(&bt_writer));
    boot_writer_progress(&bt_writer, &p);
    TEST_ASSERT_EQUAL_INT(BOOT_WRITER_VALIDATING, p.phase);
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_write(&bt_writer, len, img, 1U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_erase_step(&bt_writer));
    while (boot_writer_finish_step(&bt_writer) == BOOT_BUSY) {
    }
    boot_writer_progress(&bt_writer, &p);
    TEST_ASSERT_EQUAL_INT(BOOT_WRITER_DONE, p.phase);
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_finish_step(&bt_writer));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_write(&bt_writer, len, img, 1U));
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_TRIAL, describe().slot[1].state);
    TEST_ASSERT_EQUAL_MEMORY(img, &bt_mem[bt_slot_addr(1U)], len);
}

static void test_begin_refuses_the_states_and_sizes_of_the_state_machine(void)
{
    boot_writer_progress_t p;
    bt_counts_t counts;

    adopted();
    bt_counts_reset();
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_writer_begin(&bt_writer, &bt_boot, 0U, 100U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SIZE, boot_writer_begin(&bt_writer, &bt_boot, 1U, 0U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_SIZE,
                          boot_writer_begin(&bt_writer, &bt_boot, 1U, bt_slot_size(1U) + 1U));
    boot_writer_progress(&bt_writer, &p);
    TEST_ASSERT_EQUAL_INT(BOOT_WRITER_IDLE, p.phase);
    TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, p.slot);
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(0U, counts.programs + counts.erases);

    /* The whole slot is allowed; a begin of a slot that is WRITING writes no record. */
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, bt_slot_size(1U)));
    bt_counts_reset();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, 10U));
    bt_counts(&counts);
    TEST_ASSERT_EQUAL_UINT32(0U, counts.programs + counts.erases);
    TEST_ASSERT_EQUAL_UINT32(2U, describe().seq);

    /* While a trial is committed, or running, neither slot may be begun. */
    TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 1U, s_image[X2], s_len[X2], 0U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_writer_begin(&bt_writer, &bt_boot, 0U, 100U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_writer_begin(&bt_writer, &bt_boot, 1U, 100U));
    (void)select_expect(BOOT_OK);
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_writer_begin(&bt_writer, &bt_boot, 0U, 100U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_writer_begin(&bt_writer, &bt_boot, 1U, 100U));
    /* After the confirm, A (PREVIOUS) may be begun, B (CONFIRMED) not. */
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_confirm(&bt_boot, 1U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_writer_begin(&bt_writer, &bt_boot, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 0U, 100U));
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_WRITING, describe().slot[0].state);
}

static void test_the_writer_erases_only_the_sectors_the_image_needs(void)
{
    static const uint32_t sizes[] = {1U, 4095U, 4096U, 4097U, 8192U, 12288U};
    bt_op_t log[64];

    for (uint32_t i = 0U; i < (uint32_t)(sizeof(sizes) / sizeof(sizes[0])); i++) {
        const uint32_t sectors = (sizes[i] + 4095U) / 4096U;
        boot_status_t status = BOOT_BUSY;
        uint32_t steps = 0U;

        bt_setup(&bt_shape_host_small);
        adopted();
        (void)memset(&bt_mem[bt_slot_addr(1U)], 0x3C, bt_slot_size(1U)); /* old content */
        TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, sizes[i]));
        bt_trace(log, 64U);
        while (status == BOOT_BUSY) {
            status = boot_writer_erase_step(&bt_writer);
            steps++;
        }
        TEST_ASSERT_EQUAL_INT(BOOT_OK, status);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(sectors, steps, at("erase steps for size", sizes[i]));
        TEST_ASSERT_EQUAL_UINT32(sectors, bt_trace_len());
        for (uint32_t s = 0U; s < sectors; s++) {
            TEST_ASSERT_TRUE(log[s].erase);
            TEST_ASSERT_EQUAL_UINT32(bt_slot_addr(1U) + (s * 4096U), log[s].addr);
        }
        TEST_ASSERT_EACH_EQUAL_UINT8(0xFFU, &bt_mem[bt_slot_addr(1U)], sectors * 4096U);
        if (sectors < 3U) {
            TEST_ASSERT_EACH_EQUAL_UINT8(0x3CU, &bt_mem[bt_slot_addr(1U) + (sectors * 4096U)],
                                         (3U - sectors) * 4096U);
        }
        bt_trace(NULL, 0U);
    }

    /* A sector that reads erased is erased all the same (see LIMITS in boot.h). */
    bt_setup(&bt_shape_host_small);
    adopted();
    {
        bt_counts_t counts;

        TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, 9000U));
        bt_counts_reset();
        while (boot_writer_erase_step(&bt_writer) == BOOT_BUSY) {
        }
        bt_counts(&counts);
        TEST_ASSERT_EQUAL_UINT32(3U, counts.erases);
    }
}

static void test_the_writer_programs_whole_pages_whatever_the_chunks(void)
{
    static const uint32_t chunk_sizes[] = {1U, 7U, 8U, 255U, 256U, 257U, 1000U, 5032U};

    for (uint32_t c = 0U; c <= (uint32_t)(sizeof(chunk_sizes) / sizeof(chunk_sizes[0])); c++) {
        const uint32_t len = s_len[X4];
        bt_op_t log[64];
        uint32_t offset = 0U;
        uint32_t programs = 0U;

        bt_setup(&bt_shape_host_small);
        adopted();
        TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, len));
        while (boot_writer_erase_step(&bt_writer) == BOOT_BUSY) {
        }
        bt_trace(log, 64U);
        while (offset < len) {
            /* the last round takes random chunk sizes */
            uint32_t n = (c < 8U) ? chunk_sizes[c] : (1U + random_below(600U));

            if (n > (len - offset)) {
                n = len - offset;
            }
            TEST_ASSERT_EQUAL_INT(BOOT_OK,
                                  boot_writer_write(&bt_writer, offset, &s_image[X4][offset], n));
            offset += n;
        }
        /* 5032 bytes: 19 whole pages while writing, the rest (168 bytes, 21 units) at the
         * first finish step. */
        programs = bt_trace_len();
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(19U, programs, at("chunk size case", c));
        for (uint32_t i = 0U; i < programs; i++) {
            TEST_ASSERT_FALSE(log[i].erase);
            TEST_ASSERT_EQUAL_UINT32(bt_slot_addr(1U) + (i * 256U), log[i].addr);
            TEST_ASSERT_EQUAL_UINT32(256U, log[i].len);
        }
        TEST_ASSERT_EQUAL_INT(BOOT_BUSY, boot_writer_finish_step(&bt_writer));
        TEST_ASSERT_EQUAL_UINT32(20U, bt_trace_len());
        TEST_ASSERT_EQUAL_UINT32(bt_slot_addr(1U) + (19U * 256U), log[19].addr);
        TEST_ASSERT_EQUAL_UINT32(168U, log[19].len);
        bt_trace(NULL, 0U);
        while (boot_writer_finish_step(&bt_writer) == BOOT_BUSY) {
        }
        TEST_ASSERT_EQUAL_MEMORY(s_image[X4], &bt_mem[bt_slot_addr(1U)], len);
        TEST_ASSERT_EACH_EQUAL_UINT8(0xFFU, &bt_mem[bt_slot_addr(1U) + len], 8192U - len);
        TEST_ASSERT_EQUAL_INT(BOOT_SLOT_TRIAL, describe().slot[1].state);
    }

    /* An image that is not whole units: the last unit is padded with 0xFF. */
    bt_setup(&bt_shape_host_small);
    adopted();
    {
        uint8_t odd[45];
        const uint32_t len = bt_image_build(odd, sizeof(odd), 9U, 99U, 13U);

        TEST_ASSERT_EQUAL_UINT32(45U, len);
        TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 1U, odd, len, 0U));
        TEST_ASSERT_EQUAL_MEMORY(odd, &bt_mem[bt_slot_addr(1U)], len);
        TEST_ASSERT_EACH_EQUAL_UINT8(0xFFU, &bt_mem[bt_slot_addr(1U) + len], 3U);
    }
}

static void test_finish_validates_in_steps_of_one_flash_operation_at_most(void)
{
    const uint32_t len = s_len[X4]; /* 32-byte header, 5000-byte payload */
    uint32_t busy = 0U;
    boot_status_t status = BOOT_BUSY;

    adopted();
    bt_checker.chunk = 100U;
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, len));
    while (boot_writer_erase_step(&bt_writer) == BOOT_BUSY) {
    }
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_write(&bt_writer, 0U, s_image[X4], len));
    bt_checker.starts = 0U;
    bt_checker.steps = 0U;
    while (status == BOOT_BUSY) {
        bt_counts_t counts;

        bt_counts_reset();
        status = boot_writer_finish_step(&bt_writer);
        bt_counts(&counts);
        /* At most one program, or the record's two. Never an erase here (log not full). */
        TEST_ASSERT_TRUE((counts.programs <= 1U) || (status == BOOT_OK));
        TEST_ASSERT_EQUAL_UINT32(0U, counts.erases);
        busy += (status == BOOT_BUSY) ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_INT(BOOT_OK, status);
    /* flush, 1 header step + 50 payload steps (the last one VALID), then the commit */
    TEST_ASSERT_EQUAL_UINT32(1U, bt_checker.starts);
    TEST_ASSERT_EQUAL_UINT32(51U, bt_checker.steps);
    TEST_ASSERT_EQUAL_UINT32(52U, busy);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_TRIAL, describe().slot[1].state);
    TEST_ASSERT_EQUAL_UINT32(0x02000000U, describe().slot[1].version);
}

static void test_an_image_that_does_not_validate_is_not_committed(void)
{
    boot_writer_progress_t p;
    bt_counts_t counts;

    adopted();
    TEST_ASSERT_EQUAL_INT(BOOT_E_INVALID, bt_update(&bt_boot, 1U, corrupted(X2), s_len[X2], 0U));
    boot_writer_progress(&bt_writer, &p);
    TEST_ASSERT_EQUAL_INT(BOOT_WRITER_FAILED, p.phase);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_WRITING, describe().slot[1].state);
    TEST_ASSERT_EQUAL_INT(BOOT_E_SEQUENCE, boot_writer_finish_step(&bt_writer));
    (void)select_expect(BOOT_OK);

    /* A validator that stalls fails the writer after image-size steps. */
    bt_checker.stall = true;
    TEST_ASSERT_EQUAL_INT(BOOT_E_INVALID, bt_update(&bt_boot, 1U, s_image[X3], 40U, 0U));
    bt_checker.stall = false;
    bt_checker.fail_reads = true;
    TEST_ASSERT_EQUAL_INT(BOOT_E_FLASH, bt_update(&bt_boot, 1U, s_image[X3], s_len[X3], 0U));
    bt_checker.fail_reads = false;

    /* The next begin writes no record (WRITING already) and the update succeeds. */
    bt_counts_reset();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 1U, s_image[X2], s_len[X2], 5U));
    bt_counts(&counts);
    /* 3032 bytes: 11 whole pages, the last 216 bytes at the flush, then the record. */
    TEST_ASSERT_EQUAL_UINT32(11U + 1U + 2U, counts.programs);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_TRIAL, describe().slot[1].state);
}

static void test_an_image_must_lie_within_the_announced_size(void)
{
    /* X4 (5032 bytes, two sectors) committed in B, then rolled back: B still holds all of it. */
    trial_in_b();
    for (int i = 0; i < 3; i++) {
        (void)select_expect(BOOT_OK);
    }
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_ROLLBACK, select_expect(BOOT_OK).event);
    TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 1U, s_image[X4], s_len[X4], 0U));
    for (int i = 0; i < 4; i++) {
        (void)select_expect(BOOT_OK);
    }
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_REJECTED, describe().slot[1].state);

    /* An update of 4096 bytes erases one sector and writes X4's first 4096 bytes: with the
     * second sector left from before, the slot holds a valid X4, but not within the 4096 bytes
     * announced, so it is not committed. */
    TEST_ASSERT_EQUAL_INT(BOOT_E_INVALID, bt_update(&bt_boot, 1U, s_image[X4], 4096U, 0U));
    TEST_ASSERT_TRUE(bt_image_check_mem(bt_slot_addr(1U), bt_slot_size(1U), NULL, NULL));
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_WRITING, describe().slot[1].state);
    TEST_ASSERT_EQUAL_UINT8(0U, select_expect(BOOT_OK).slot);
}

static void test_a_writer_fails_when_its_slot_stops_being_written(void)
{
    static boot_writer_t other;
    boot_writer_progress_t p;

    adopted();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, s_len[X3]));
    /* Another writer completes an update of the same slot meanwhile. */
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&other, &bt_boot, 1U, s_len[X2]));
    while (boot_writer_erase_step(&other) == BOOT_BUSY) {
    }
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_write(&other, 0U, s_image[X2], s_len[X2]));
    while (boot_writer_finish_step(&other) == BOOT_BUSY) {
    }
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_writer_erase_step(&bt_writer));
    boot_writer_progress(&bt_writer, &p);
    TEST_ASSERT_EQUAL_INT(BOOT_WRITER_FAILED, p.phase);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_TRIAL, describe().slot[1].state);

    /* The same in the other phases: writing, the flush, the commit. */
    bt_setup(&bt_shape_host_small);
    adopted();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&other, &bt_boot, 1U, s_len[X3]));
    while (boot_writer_erase_step(&other) == BOOT_BUSY) {
    }
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_write(&other, 0U, s_image[X3], 10U));
    TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 1U, s_image[X2], s_len[X2], 0U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_writer_write(&other, 10U, &s_image[X3][10], 10U));

    bt_setup(&bt_shape_host_small);
    adopted();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&other, &bt_boot, 1U, s_len[X3]));
    while (boot_writer_erase_step(&other) == BOOT_BUSY) {
    }
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_write(&other, 0U, s_image[X3], s_len[X3]));
    TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 1U, s_image[X2], s_len[X2], 0U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_writer_finish_step(&other));

    bt_setup(&bt_shape_host_small);
    adopted();
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&other, &bt_boot, 1U, s_len[X3]));
    while (boot_writer_erase_step(&other) == BOOT_BUSY) {
    }
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_write(&other, 0U, s_image[X3], s_len[X3]));
    TEST_ASSERT_EQUAL_INT(BOOT_BUSY, boot_writer_finish_step(&other)); /* flushed */
    while (boot_writer_finish_step(&other) == BOOT_BUSY) {
        boot_writer_progress(&other, &p);
        if (p.phase == BOOT_WRITER_COMMITTING) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, 1U, s_image[X2], s_len[X2], 0U));
    TEST_ASSERT_EQUAL_INT(BOOT_E_STATE, boot_writer_finish_step(&other));
    TEST_ASSERT_EQUAL_MEMORY(s_image[X2], describe().slot[1].id, BOOT_IMAGE_ID_SIZE);
}

/* ------------------------------------------------------------------ the log and its wear */

static void test_the_log_fills_every_sector_of_the_ring_in_turn(void)
{
    /* 512-byte sectors of 4 entries, three of them in the ring. */
    const bt_shape_t ring = {.sector_size = 512U,
                             .page_size = 64U,
                             .unit_size = 8U,
                             .meta_sectors = 3U,
                             .slot_sectors = 4U,
                             .spare_sectors = 0U};
    uint8_t img[2][600];
    uint32_t len[2];
    uint32_t records = 0U;
    uint32_t meta_erases = 0U;

    len[0] = bt_image_build(img[0], sizeof(img[0]), 1U, 501U, 500U);
    len[1] = bt_image_build(img[1], sizeof(img[1]), 2U, 502U, 520U);
    bt_setup(&ring);
    bt_raw_write(bt_slot_addr(0U), img[0], len[0]);

    for (uint32_t round = 0U; round < 40U; round++) {
        bt_op_t log[64];
        boot_description_t d;
        const uint8_t target = (uint8_t)(round % 2U);

        bt_trace(log, 64U);
        if (round == 0U) {
            TEST_ASSERT_EQUAL_INT(BOOT_EVENT_ADOPT, select_expect(BOOT_OK).event);
        } else {
            boot_selection_t sel;

            TEST_ASSERT_EQUAL_INT(BOOT_OK,
                                  bt_update(&bt_boot, target, img[target], len[target], round));
            sel = select_expect(BOOT_OK);
            expect_boot(&sel, target, 1U);
            TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_confirm(&bt_boot, target));
        }
        for (uint32_t i = 0U; i < bt_trace_len(); i++) {
            if (log[i].erase && (log[i].addr < 1536U)) {
                meta_erases++;
                /* an erase of the metadata only ever comes before an entry 0 */
                TEST_ASSERT_TRUE(i + 1U < bt_trace_len());
                TEST_ASSERT_EQUAL_UINT32(log[i].addr, log[i + 1U].addr);
            }
        }
        bt_trace(NULL, 0U);
        records += (round == 0U) ? 1U : 3U;
        d = describe();
        /* record r (from 0) sits at entry r mod 4 of sector (r / 4) mod 3 */
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(records, d.seq, at("round", round));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(((records - 1U) / 4U) % 3U, d.record_sector,
                                         at("round", round));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((records - 1U) % 4U, d.record_index, at("round", round));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((records - 1U) / 4U, meta_erases, at("round", round));
        TEST_ASSERT_EQUAL_UINT8(target, d.active_slot);
    }
    (void)printf("ring of 3 sectors x 4 entries: %lu records, %lu metadata erases\n",
                 (unsigned long)records, (unsigned long)meta_erases);
}

static void test_programs_and_erases_per_operation(void)
{
    const bt_shape_t host = {.sector_size = 4096U,
                             .page_size = 256U,
                             .unit_size = 8U,
                             .meta_sectors = 2U,
                             .slot_sectors = 64U,
                             .spare_sectors = 0U};
    static uint8_t big[2][200000];
    uint32_t len[2];
    bt_counts_t c;
    uint32_t meta_erases = 0U;
    uint32_t slot_erases = 0U;
    uint32_t programs = 0U;
    const uint32_t updates = 100U;

    len[0] = bt_image_build(big[0], sizeof(big[0]), 1U, 7001U, 196608U - 32U); /* 192 KiB */
    len[1] = bt_image_build(big[1], sizeof(big[1]), 2U, 7002U, 196608U - 32U);
    bt_setup(&host);
    bt_raw_write(bt_slot_addr(0U), big[0], len[0]);

    (void)printf("operation                         programs  units  erases   (host geometry, "
                 "192 KiB image)\n");
#define MEASURE(what, call)                                                                        \
    do {                                                                                           \
        bt_counts_reset();                                                                         \
        call;                                                                                      \
        bt_counts(&c);                                                                             \
        (void)printf("%-32s %9lu %6lu %7lu\n", what, (unsigned long)c.programs,                    \
                     (unsigned long)c.units, (unsigned long)c.erases);                             \
    } while (0)

    MEASURE("select, factory: ADOPT", (void)select_expect(BOOT_OK));
    TEST_ASSERT_EQUAL_UINT32(2U, c.programs);
    TEST_ASSERT_EQUAL_UINT32(13U, c.units);
    TEST_ASSERT_EQUAL_UINT32(0U, c.erases);
    MEASURE("writer begin: BEGIN",
            TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_writer_begin(&bt_writer, &bt_boot, 1U, len[1])));
    TEST_ASSERT_EQUAL_UINT32(2U, c.programs);
    MEASURE("writer erase steps", while (boot_writer_erase_step(&bt_writer) == BOOT_BUSY){});
    TEST_ASSERT_EQUAL_UINT32(48U, c.erases);
    MEASURE(
        "writer write (4 KiB blocks)", for (uint32_t o = 0U; o < len[1]; o += 4096U) {
            TEST_ASSERT_EQUAL_INT(BOOT_OK,
                                  boot_writer_write(&bt_writer, o, &big[1][o],
                                                    (len[1] - o) < 4096U ? (len[1] - o) : 4096U));
        });
    TEST_ASSERT_EQUAL_UINT32(768U, c.programs);
    TEST_ASSERT_EQUAL_UINT32(0U, c.erases);
    MEASURE("writer finish: COMMIT", while (boot_writer_finish_step(&bt_writer) == BOOT_BUSY){});
    TEST_ASSERT_EQUAL_UINT32(2U, c.programs); /* no partial page: 192 KiB is whole pages */
    MEASURE("select, trial: attempt", (void)select_expect(BOOT_OK));
    TEST_ASSERT_EQUAL_UINT32(1U, c.programs);
    TEST_ASSERT_EQUAL_UINT32(1U, c.units);
    MEASURE("confirm: CONFIRM", TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_confirm(&bt_boot, 1U)));
    TEST_ASSERT_EQUAL_UINT32(2U, c.programs);
    MEASURE("select, confirmed", (void)select_expect(BOOT_OK));
    TEST_ASSERT_EQUAL_UINT32(0U, c.programs + c.erases);
#undef MEASURE

    /* Then `updates` complete updates, alternating slots, counted by region. */
    for (uint32_t u = 0U; u < updates; u++) {
        const uint8_t target = (uint8_t)(u % 2U);
        static bt_op_t log[2048];

        bt_trace(log, 2048U);
        TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_update(&bt_boot, target, big[target], len[target], u));
        (void)select_expect(BOOT_OK);
        TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_confirm(&bt_boot, target));
        (void)select_expect(BOOT_OK);
        TEST_ASSERT_TRUE(bt_trace_len() < 2048U);
        for (uint32_t i = 0U; i < bt_trace_len(); i++) {
            if (log[i].erase) {
                meta_erases += (log[i].addr < 8192U) ? 1U : 0U;
                slot_erases += (log[i].addr >= 8192U) ? 1U : 0U;
            } else {
                programs++;
            }
        }
        bt_trace(NULL, 0U);
    }
    /* 4 records before, 300 more: the log crosses a sector boundary at records 33, 65, ...,
     * 289: 9 times. */
    (void)printf("%lu complete updates of 192 KiB: %lu metadata erases, %lu slot erases, %lu "
                 "programs\n",
                 (unsigned long)updates, (unsigned long)meta_erases, (unsigned long)slot_erases,
                 (unsigned long)programs);
    TEST_ASSERT_EQUAL_UINT32(9U, meta_erases);
    TEST_ASSERT_EQUAL_UINT32(updates * 48U, slot_erases);
    TEST_ASSERT_EQUAL_UINT32(updates * (2U + 768U + 2U + 1U + 2U), programs);
}

/* ------------------------------------------------------------------ the rest */

static void test_describe_reports_the_state(void)
{
    boot_description_t d;

    trial_in_b();
    (void)select_expect(BOOT_OK);
    (void)select_expect(BOOT_OK);
    d = describe();
    TEST_ASSERT_TRUE(d.have_record);
    TEST_ASSERT_EQUAL_UINT32(3U, d.seq);
    TEST_ASSERT_EQUAL_INT(BOOT_EVENT_COMMIT, d.event);
    TEST_ASSERT_EQUAL_UINT8(1U, d.event_slot);
    TEST_ASSERT_EQUAL_UINT32(0U, d.record_sector);
    TEST_ASSERT_EQUAL_UINT32(2U, d.record_index);
    TEST_ASSERT_EQUAL_UINT8(0U, d.active_slot);
    TEST_ASSERT_EQUAL_UINT8(1U, d.trial_slot);
    TEST_ASSERT_EQUAL_UINT8(2U, d.attempts_used);
    TEST_ASSERT_EQUAL_UINT8(1U, d.attempts_left);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_CONFIRMED, d.slot[0].state);
    TEST_ASSERT_EQUAL_INT(BOOT_SLOT_TRIAL, d.slot[1].state);
    TEST_ASSERT_EQUAL_UINT32(0x01010000U, d.slot[1].version);
    TEST_ASSERT_EQUAL_MEMORY(s_image[X2], d.slot[1].id, BOOT_IMAGE_ID_SIZE);
    TEST_ASSERT_EQUAL_UINT32(bt_slot_addr(1U), d.slot[1].addr);
    TEST_ASSERT_EQUAL_UINT32(bt_slot_size(1U), d.slot[1].size);
    TEST_ASSERT_EQUAL_UINT32(3U, d.records_valid);
    TEST_ASSERT_EQUAL_UINT32(3U, d.entries_used);
    TEST_ASSERT_EQUAL_UINT32(64U, d.entries_total);
    TEST_ASSERT_FALSE(d.next_write_erases);
    TEST_ASSERT_EQUAL_UINT32(0U, d.next_sector);
    TEST_ASSERT_EQUAL_UINT32(3U, d.next_index);
}

static void test_slot_by_address(void)
{
    uint8_t slot = 9U;

    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_slot_by_address(&bt_boot, bt_slot_addr(0U), &slot));
    TEST_ASSERT_EQUAL_UINT8(0U, slot);
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_slot_by_address(&bt_boot, bt_slot_addr(1U), &slot));
    TEST_ASSERT_EQUAL_UINT8(1U, slot);
    slot = 9U;
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG,
                          boot_slot_by_address(&bt_boot, bt_slot_addr(1U) + 4096U, &slot));
    TEST_ASSERT_EQUAL_INT(BOOT_E_ARG, boot_slot_by_address(&bt_boot, 0U, &slot));
    TEST_ASSERT_EQUAL_UINT8(9U, slot);
}

static void test_names(void)
{
    TEST_ASSERT_EQUAL_STRING("ok", boot_status_name(BOOT_OK));
    TEST_ASSERT_EQUAL_STRING("busy", boot_status_name(BOOT_BUSY));
    TEST_ASSERT_EQUAL_STRING("argument", boot_status_name(BOOT_E_ARG));
    TEST_ASSERT_EQUAL_STRING("layout", boot_status_name(BOOT_E_LAYOUT));
    TEST_ASSERT_EQUAL_STRING("state", boot_status_name(BOOT_E_STATE));
    TEST_ASSERT_EQUAL_STRING("sequence", boot_status_name(BOOT_E_SEQUENCE));
    TEST_ASSERT_EQUAL_STRING("size", boot_status_name(BOOT_E_SIZE));
    TEST_ASSERT_EQUAL_STRING("invalid", boot_status_name(BOOT_E_INVALID));
    TEST_ASSERT_EQUAL_STRING("nothing_bootable", boot_status_name(BOOT_E_NOTHING_BOOTABLE));
    TEST_ASSERT_EQUAL_STRING("flash", boot_status_name(BOOT_E_FLASH));
    TEST_ASSERT_EQUAL_STRING("power_lost", boot_status_name(BOOT_E_POWER_LOST));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): outside it on purpose */
    TEST_ASSERT_EQUAL_STRING("?", boot_status_name((boot_status_t)99));
    TEST_ASSERT_EQUAL_STRING("empty", boot_slot_state_name(BOOT_SLOT_EMPTY));
    TEST_ASSERT_EQUAL_STRING("writing", boot_slot_state_name(BOOT_SLOT_WRITING));
    TEST_ASSERT_EQUAL_STRING("trial", boot_slot_state_name(BOOT_SLOT_TRIAL));
    TEST_ASSERT_EQUAL_STRING("confirmed", boot_slot_state_name(BOOT_SLOT_CONFIRMED));
    TEST_ASSERT_EQUAL_STRING("previous", boot_slot_state_name(BOOT_SLOT_PREVIOUS));
    TEST_ASSERT_EQUAL_STRING("rejected", boot_slot_state_name(BOOT_SLOT_REJECTED));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): outside it on purpose */
    TEST_ASSERT_EQUAL_STRING("?", boot_slot_state_name((boot_slot_state_t)6));
    TEST_ASSERT_EQUAL_STRING("none", boot_event_name(BOOT_EVENT_NONE));
    TEST_ASSERT_EQUAL_STRING("adopt", boot_event_name(BOOT_EVENT_ADOPT));
    TEST_ASSERT_EQUAL_STRING("begin", boot_event_name(BOOT_EVENT_BEGIN));
    TEST_ASSERT_EQUAL_STRING("commit", boot_event_name(BOOT_EVENT_COMMIT));
    TEST_ASSERT_EQUAL_STRING("confirm", boot_event_name(BOOT_EVENT_CONFIRM));
    TEST_ASSERT_EQUAL_STRING("rollback", boot_event_name(BOOT_EVENT_ROLLBACK));
    TEST_ASSERT_EQUAL_STRING("reject", boot_event_name(BOOT_EVENT_REJECT));
    TEST_ASSERT_EQUAL_STRING("fallback", boot_event_name(BOOT_EVENT_FALLBACK));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): outside it on purpose */
    TEST_ASSERT_EQUAL_STRING("?", boot_event_name((boot_event_t)8));
    TEST_ASSERT_EQUAL_STRING("idle", boot_writer_phase_name(BOOT_WRITER_IDLE));
    TEST_ASSERT_EQUAL_STRING("erasing", boot_writer_phase_name(BOOT_WRITER_ERASING));
    TEST_ASSERT_EQUAL_STRING("writing", boot_writer_phase_name(BOOT_WRITER_WRITING));
    TEST_ASSERT_EQUAL_STRING("validating", boot_writer_phase_name(BOOT_WRITER_VALIDATING));
    TEST_ASSERT_EQUAL_STRING("committing", boot_writer_phase_name(BOOT_WRITER_COMMITTING));
    TEST_ASSERT_EQUAL_STRING("done", boot_writer_phase_name(BOOT_WRITER_DONE));
    TEST_ASSERT_EQUAL_STRING("failed", boot_writer_phase_name(BOOT_WRITER_FAILED));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): outside it on purpose */
    TEST_ASSERT_EQUAL_STRING("?", boot_writer_phase_name((boot_writer_phase_t)7));
}

/* ------------------------------------------------------------------ random runs */

typedef struct {
    uint32_t selects, trial_boots, rollbacks, rejects, fallbacks, confirms, updates, refusals,
        invalid, abandoned, damages, rewrites, nothing;
} visits_t;

static void random_run(const bt_shape_t *shape, uint32_t steps, visits_t *v)
{
    static uint8_t pool[6][6000];
    uint32_t pool_len[6];
    const uint32_t capacity = shape->slot_sectors * shape->sector_size;
    bt_ref_t ref;

    for (uint32_t i = 0U; i < 6U; i++) {
        const uint32_t payload = 1U + random_below((capacity * 4U / 5U) - BT_IMAGE_HEADER);

        pool_len[i] = bt_image_build(pool[i], sizeof(pool[i]), 100U + i, 900U + i,
                                     payload < (6000U - 32U) ? payload : (6000U - 32U));
    }
    bt_setup(shape);
    bt_raw_write(bt_slot_addr(0U), pool[0], pool_len[0]);
    bt_ref_factory(&ref);

    for (uint32_t step = 0U; step < steps; step++) {
        const uint32_t dice = random_below(100U);
        const uint8_t slot = (uint8_t)random_below(2U);
        const uint32_t pick = random_below(6U);
        const uint8_t o = ref.have ? (uint8_t)(1U - bt_ref_confirmed(&ref)) : 1U;
        bt_truth_t truth;
        char where[96];

        (void)snprintf(where, sizeof(where), "random step %lu (dice %lu)", (unsigned long)step,
                       (unsigned long)dice);
        bt_truth(&truth);
        if (dice < 40U) {
            bt_ref_choice_t choice;
            boot_selection_t sel;

            bt_ref_select(&ref, &truth, &choice);
            TEST_ASSERT_EQUAL_INT_MESSAGE(choice.status, boot_select(&bt_boot, &sel), where);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(choice.slot, sel.slot, where);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(choice.attempt, sel.attempt, where);
            TEST_ASSERT_EQUAL_INT_MESSAGE(choice.event, sel.event, where);
            v->selects++;
            v->trial_boots += sel.trial ? 1U : 0U;
            v->rollbacks += (sel.event == BOOT_EVENT_ROLLBACK) ? 1U : 0U;
            v->rejects += (sel.event == BOOT_EVENT_REJECT) ? 1U : 0U;
            v->fallbacks += (sel.event == BOOT_EVENT_FALLBACK) ? 1U : 0U;
            v->nothing += (choice.status != BOOT_OK) ? 1U : 0U;
        } else if (dice < 55U) {
            const boot_status_t want = bt_ref_confirm(&ref, slot);

            TEST_ASSERT_EQUAL_INT_MESSAGE(want, boot_confirm(&bt_boot, slot), where);
            v->confirms += (want == BOOT_OK) ? 1U : 0U;
            v->refusals += (want != BOOT_OK) ? 1U : 0U;
        } else if (dice < 80U) {
            /* an update, usually of the right slot, sometimes changed in transfer */
            const uint8_t target = (random_below(4U) == 0U) ? slot : o;
            const bool bad = (random_below(5U) == 0U);
            boot_status_t want = bt_ref_begin(&ref, target, pool_len[pick], capacity);
            const uint8_t *image = pool[pick];

            if (bad) {
                (void)memcpy(s_scratch, pool[pick], pool_len[pick]);
                s_scratch[random_below(pool_len[pick])] ^= (uint8_t)(1U + random_below(255U));
                image = s_scratch;
            }
            if ((want == BOOT_OK) && bad) {
                want = BOOT_E_INVALID;
                v->invalid++;
            } else if (want == BOOT_OK) {
                bt_ref_commit(&ref, target, pool[pick]);
                v->updates++;
            } else {
                v->refusals++;
            }
            TEST_ASSERT_EQUAL_INT_MESSAGE(
                want, bt_update(&bt_boot, target, image, pool_len[pick], step), where);
        } else if (dice < 88U) {
            /* an update abandoned after its begin and part of the erase */
            const boot_status_t want = bt_ref_begin(&ref, o, pool_len[pick], capacity);

            TEST_ASSERT_EQUAL_INT_MESSAGE(
                want, boot_writer_begin(&bt_writer, &bt_boot, o, pool_len[pick]), where);
            if (want == BOOT_OK) {
                (void)boot_writer_erase_step(&bt_writer);
                v->abandoned++;
            }
        } else if (dice < 92U) {
            bt_reboot();
        } else if (dice < 94U) {
            /* damage: one bit of a slot's image area */
            bt_flip_bit(bt_slot_addr(slot) + random_below(pool_len[pick]), random_below(8U));
            v->damages++;
        } else {
            /* A bench rewrites a slot: half the time with the image the record names for it
             * (a repair, which brings a dead state back), else with any valid image. */
            uint32_t image = pick;

            for (uint32_t i = 0U; i < 6U; i++) {
                if ((random_below(2U) == 0U) &&
                    (memcmp(pool[i], ref.id[slot], BOOT_IMAGE_ID_SIZE) == 0)) {
                    image = i;
                }
            }
            (void)memset(&bt_mem[bt_slot_addr(slot)], 0xFF, capacity);
            bt_raw_write(bt_slot_addr(slot), pool[image], pool_len[image]);
            v->rewrites++;
        }
        expect_state(&ref, where);
    }
}

static void test_random_operations_match_the_reference_model(void)
{
    const bt_shape_t shapes[3] = {
        {.sector_size = 4096U,
         .page_size = 256U,
         .unit_size = 8U,
         .meta_sectors = 2U,
         .slot_sectors = 2U,
         .spare_sectors = 1U},
        {.sector_size = 1024U,
         .page_size = 256U,
         .unit_size = 8U,
         .meta_sectors = 2U,
         .slot_sectors = 6U,
         .spare_sectors = 0U},
        {.sector_size = 512U,
         .page_size = 64U,
         .unit_size = 16U,
         .meta_sectors = 3U,
         .slot_sectors = 8U,
         .spare_sectors = 0U},
    };
    visits_t v;

    (void)memset(&v, 0, sizeof(v));
    for (uint32_t i = 0U; i < 3U; i++) {
        random_run(&shapes[i], 2500U, &v);
    }
    (void)printf("random runs: %lu selects (%lu trial boots, %lu rollbacks, %lu rejects, %lu "
                 "fallbacks, %lu nothing bootable), %lu confirms, %lu updates, %lu invalid "
                 "images, %lu abandoned, %lu refusals, %lu damages, %lu rewrites\n",
                 (unsigned long)v.selects, (unsigned long)v.trial_boots, (unsigned long)v.rollbacks,
                 (unsigned long)v.rejects, (unsigned long)v.fallbacks, (unsigned long)v.nothing,
                 (unsigned long)v.confirms, (unsigned long)v.updates, (unsigned long)v.invalid,
                 (unsigned long)v.abandoned, (unsigned long)v.refusals, (unsigned long)v.damages,
                 (unsigned long)v.rewrites);
    TEST_ASSERT_GREATER_THAN_UINT32(200U, v.trial_boots);
    TEST_ASSERT_GREATER_THAN_UINT32(20U, v.rollbacks);
    TEST_ASSERT_GREATER_THAN_UINT32(20U, v.rejects);
    TEST_ASSERT_GREATER_THAN_UINT32(5U, v.fallbacks);
    TEST_ASSERT_GREATER_THAN_UINT32(100U, v.confirms);
    TEST_ASSERT_GREATER_THAN_UINT32(200U, v.updates);
    TEST_ASSERT_GREATER_THAN_UINT32(20U, v.invalid);
    TEST_ASSERT_GREATER_THAN_UINT32(500U, v.refusals);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_flash_model_follows_the_rules_of_flash_h);
    RUN_TEST(test_the_flash_model_tears_a_program_as_decision_2_says);
    RUN_TEST(test_the_flash_model_tears_an_erase_as_decision_2_says);
    RUN_TEST(test_init_accepts_the_host_layout_and_reads_nothing);
    RUN_TEST(test_init_refuses_every_layout_it_cannot_use);
    RUN_TEST(test_the_entry_layout_follows_the_unit_and_page);
    RUN_TEST(test_an_uninitialised_context_or_writer_is_refused);
    RUN_TEST(test_select_adopts_the_factory_image_in_slot_a);
    RUN_TEST(test_select_adopts_slot_b_when_a_does_not_validate);
    RUN_TEST(test_with_no_valid_image_nothing_is_bootable_and_nothing_is_written);
    RUN_TEST(test_garbage_in_the_metadata_is_not_a_record);
    RUN_TEST(test_the_record_bytes_are_as_documented);
    RUN_TEST(test_every_damage_that_sets_bits_rejects_the_record);
    RUN_TEST(test_damage_the_commit_field_cannot_see_is_caught_by_the_crc);
    RUN_TEST(test_a_record_without_its_commit_field_does_not_count);
    RUN_TEST(test_a_record_must_be_well_formed);
    RUN_TEST(test_the_newest_valid_record_wins_wherever_it_is);
    RUN_TEST(test_every_operation_in_every_state_matches_the_reference_model);
    RUN_TEST(test_a_trial_gets_three_attempts_then_is_rolled_back);
    RUN_TEST(test_a_partly_written_attempt_counts_against_the_trial);
    RUN_TEST(test_a_trial_that_does_not_validate_is_rejected);
    RUN_TEST(test_confirm_makes_the_trial_active_and_booting_it_writes_nothing);
    RUN_TEST(test_an_active_image_that_stops_validating_falls_back_to_the_previous_one);
    RUN_TEST(test_an_image_rewritten_behind_the_metadata_is_never_taken_for_the_recorded_one);
    RUN_TEST(test_a_failed_write_never_boots_a_trial);
    RUN_TEST(test_select_stops_on_validator_errors_and_bounds_a_stalled_validator);
    RUN_TEST(test_no_record_follows_the_last_sequence_number);
    RUN_TEST(test_a_record_that_does_not_read_back_is_an_error);
    RUN_TEST(test_the_writer_refuses_calls_out_of_order);
    RUN_TEST(test_begin_refuses_the_states_and_sizes_of_the_state_machine);
    RUN_TEST(test_the_writer_erases_only_the_sectors_the_image_needs);
    RUN_TEST(test_the_writer_programs_whole_pages_whatever_the_chunks);
    RUN_TEST(test_finish_validates_in_steps_of_one_flash_operation_at_most);
    RUN_TEST(test_an_image_that_does_not_validate_is_not_committed);
    RUN_TEST(test_an_image_must_lie_within_the_announced_size);
    RUN_TEST(test_a_writer_fails_when_its_slot_stops_being_written);
    RUN_TEST(test_the_log_fills_every_sector_of_the_ring_in_turn);
    RUN_TEST(test_programs_and_erases_per_operation);
    RUN_TEST(test_describe_reports_the_state);
    RUN_TEST(test_slot_by_address);
    RUN_TEST(test_names);
    RUN_TEST(test_random_operations_match_the_reference_model);
    return UNITY_END();
}
