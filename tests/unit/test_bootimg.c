/* SPDX-License-Identifier: MIT */
/*
 * lib/bootimg: lib/image's validation as lib/boot's validator (bootimg.h). The slots are in a
 * flash device over the test's RAM that counts its reads, so that the step bound (one read of at
 * most 64 bytes per step) is checked, and in lib/flash's NOR model with the host's layout for
 * the tests with lib/boot itself.
 */
#include <string.h>

#include "bootimg/bootimg.h"
#include "flash/nor.h"
#include "unity.h"

/* ------------------------------------------------------------------- a flash in the test's RAM */

#define RAM_SIZE  (64U * 1024U)
#define SLOT_SIZE (32U * 1024U)

typedef struct {
    uint8_t mem[RAM_SIZE];
    uint32_t reads;
    uint32_t largest_read;
    uint32_t fail_at; /* a read that covers this address fails; UINT32_MAX: none */
} ram_t;

static ram_t s_ram;
static flash_dev_t s_dev;

static flash_status_t ram_read(void *ctx, uint32_t addr, uint8_t *dst, uint32_t len)
{
    ram_t *ram = ctx;
    flash_status_t status = FLASH_E_RANGE;

    if ((len != 0U) && (addr < RAM_SIZE) && (len <= (RAM_SIZE - addr))) {
        ram->reads++;
        ram->largest_read = (len > ram->largest_read) ? len : ram->largest_read;

        if ((ram->fail_at >= addr) && (ram->fail_at < (addr + len))) {
            status = FLASH_E_POWER_LOST;
        } else {
            (void)memcpy(dst, &ram->mem[addr], len);
            status = FLASH_OK;
        }
    }

    return status;
}

/* An image of `payload_size` bytes (a pattern of `seed`) and `version` at `addr`; returns its
 * header. */
static image_header_t put_image(uint8_t *mem, uint32_t payload_size, uint32_t version, uint8_t seed)
{
    image_header_t header;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t *payload = &mem[IMAGE_PAYLOAD_OFFSET];

    for (uint32_t i = 0U; i < payload_size; i++) {
        payload[i] = (uint8_t)((i * 31U) + seed);
    }

    TEST_ASSERT_TRUE(sha256_compute(payload, payload_size, digest));
    TEST_ASSERT_TRUE(image_header_init(&header, IMAGE_TARGET_HOST, version, payload_size, digest));
    TEST_ASSERT_TRUE(image_header_encode(&header, mem));
    return header;
}

void setUp(void)
{
    (void)memset(&s_ram, 0xFF, sizeof(s_ram.mem));
    s_ram.reads = 0U;
    s_ram.largest_read = 0U;
    s_ram.fail_at = UINT32_MAX;
    (void)memset(&s_dev, 0, sizeof(s_dev));
    s_dev.geometry = (flash_geometry_t){
        .size = RAM_SIZE, .sector_size = 4096U, .page_size = 256U, .unit_size = 8U};
    s_dev.ctx = &s_ram;
    s_dev.read = ram_read;
}

void tearDown(void)
{
}

/* Run a check of [addr, addr + SLOT_SIZE) to its end, at most one read per step. */
static boot_check_t check(boot_validator_t *v, uint32_t addr, boot_image_info_t *info,
                          uint32_t *steps)
{
    boot_check_t result = BOOT_CHECK_BUSY;

    *steps = 0U;
    v->start(v->ctx, &s_dev, addr, SLOT_SIZE);

    while (result == BOOT_CHECK_BUSY) {
        const uint32_t before = s_ram.reads;

        result = v->step(v->ctx, info);
        (*steps)++;
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(1U, s_ram.reads - before);
        TEST_ASSERT_LESS_THAN_UINT32(100000U, *steps);
    }

    return result;
}

/* ------------------------------------------------------------------------------- the tests */

/* A valid image: VALID after 8 + ceil(payload / 64) steps, none reading more than one chunk;
 * the id is the SHA-256 of the slot's first 256 bytes, and the version the header's. */
static void test_a_valid_image_validates_with_the_digest_of_its_header_as_id(void)
{
    static const uint32_t sizes[] = {1U, 63U, 64U, 65U, 1000U, SLOT_SIZE - IMAGE_HEADER_SIZE};

    for (size_t i = 0U; i < (sizeof(sizes) / sizeof(sizes[0])); i++) {
        bootimg_t v;
        boot_validator_t validator;
        boot_image_info_t info;
        uint8_t expect[SHA256_DIGEST_SIZE];
        uint32_t steps = 0U;

        setUp();
        const image_header_t header = put_image(s_ram.mem, sizes[i], 0x01020003U, (uint8_t)i);

        TEST_ASSERT_TRUE(bootimg_init(&v, IMAGE_TARGET_HOST, &validator));
        TEST_ASSERT_EQUAL_PTR(&v, validator.ctx);
        (void)memset(&info, 0, sizeof(info));
        TEST_ASSERT_EQUAL(BOOT_CHECK_VALID, check(&validator, 0U, &info, &steps));
        TEST_ASSERT_EQUAL_UINT32(8U + ((sizes[i] + 63U) / 64U), steps);
        TEST_ASSERT_EQUAL_UINT32(steps, bootimg_steps(&v));
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(IMAGE_READ_CHUNK, s_ram.largest_read);
        TEST_ASSERT_TRUE(sha256_compute(s_ram.mem, IMAGE_HEADER_SIZE, expect));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, info.id, SHA256_DIGEST_SIZE);
        TEST_ASSERT_EQUAL_HEX32(0x01020003U, info.version);
        TEST_ASSERT_EQUAL(IMAGE_OK, bootimg_status(&v));

        /* The one-call id is the same, and the header is the one written. */
        uint8_t id[BOOT_IMAGE_ID_SIZE];
        image_header_t back;

        TEST_ASSERT_TRUE(bootimg_image_id(&header, id));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, id, sizeof(id));
        TEST_ASSERT_TRUE(bootimg_header(&v, &back));
        TEST_ASSERT_EQUAL_UINT32(sizes[i], back.payload_size);
        TEST_ASSERT_EQUAL(FLASH_OK, bootimg_flash_status(&v));
    }
}

/* What lib/image refuses is INVALID: an erased slot and a damaged header after the header's 4
 * steps, a damaged payload after the payload's; another target too. */
static void test_what_lib_image_refuses_is_invalid(void)
{
    bootimg_t v;
    boot_validator_t validator;
    boot_image_info_t info;
    uint32_t steps = 0U;

    TEST_ASSERT_TRUE(bootimg_init(&v, IMAGE_TARGET_HOST, &validator));
    TEST_ASSERT_EQUAL(BOOT_CHECK_INVALID, check(&validator, 0U, &info, &steps));
    TEST_ASSERT_EQUAL_UINT32(4U, steps);
    TEST_ASSERT_EQUAL(IMAGE_E_MAGIC, bootimg_status(&v));

    (void)put_image(s_ram.mem, 1000U, 1U, 7U);
    s_ram.mem[IMAGE_PAYLOAD_OFFSET + 999U] ^= 0x01U;
    TEST_ASSERT_EQUAL(BOOT_CHECK_INVALID, check(&validator, 0U, &info, &steps));
    TEST_ASSERT_EQUAL_UINT32(4U + 16U, steps);
    TEST_ASSERT_EQUAL(IMAGE_E_DIGEST, bootimg_status(&v));

    (void)put_image(s_ram.mem, 1000U, 1U, 7U);
    s_ram.mem[IMAGE_OFFSET_IMAGE_VERSION] ^= 0x80U;
    TEST_ASSERT_EQUAL(BOOT_CHECK_INVALID, check(&validator, 0U, &info, &steps));
    TEST_ASSERT_EQUAL_UINT32(4U, steps);
    TEST_ASSERT_EQUAL(IMAGE_E_HEADER_CRC, bootimg_status(&v));
    /* The header of a refused check is still there to look at. */
    image_header_t back;

    TEST_ASSERT_TRUE(bootimg_header(&v, &back));

    (void)put_image(s_ram.mem, 1000U, 1U, 7U);
    TEST_ASSERT_TRUE(bootimg_init(&v, IMAGE_TARGET_MPS2_AN385, &validator));
    TEST_ASSERT_EQUAL(BOOT_CHECK_INVALID, check(&validator, 0U, &info, &steps));
    TEST_ASSERT_EQUAL(IMAGE_E_TARGET, bootimg_status(&v));
}

/* A read that fails is ERROR, at any point: in the header, in the payload. The device's status
 * is kept. */
static void test_a_failed_read_is_an_error(void)
{
    static const uint32_t places[] = {0U, 200U, IMAGE_PAYLOAD_OFFSET, 900U};
    bootimg_t v;
    boot_validator_t validator;
    boot_image_info_t info;
    uint32_t steps = 0U;

    (void)put_image(s_ram.mem, 1000U, 1U, 3U);
    TEST_ASSERT_TRUE(bootimg_init(&v, IMAGE_TARGET_HOST, &validator));

    for (size_t i = 0U; i < (sizeof(places) / sizeof(places[0])); i++) {
        s_ram.fail_at = places[i];
        TEST_ASSERT_EQUAL(BOOT_CHECK_ERROR, check(&validator, 0U, &info, &steps));
        TEST_ASSERT_EQUAL(IMAGE_E_FLASH, bootimg_status(&v));
        TEST_ASSERT_EQUAL(FLASH_E_POWER_LOST, bootimg_flash_status(&v));
    }
}

/* After the result a step returns it again and reads nothing; a VALID one fills the info again.
 * A validator never started answers INVALID. A slot lib/image refuses to look at is INVALID at
 * the first step. */
static void test_after_the_result_and_before_a_start(void)
{
    bootimg_t v;
    boot_validator_t validator;
    boot_image_info_t info;
    boot_image_info_t again;
    uint32_t steps = 0U;

    TEST_ASSERT_TRUE(bootimg_init(&v, IMAGE_TARGET_HOST, &validator));
    TEST_ASSERT_EQUAL(BOOT_CHECK_INVALID, validator.step(validator.ctx, &info));
    TEST_ASSERT_EQUAL_UINT32(0U, s_ram.reads);
    TEST_ASSERT_EQUAL(IMAGE_PENDING, bootimg_status(&v));
    TEST_ASSERT_FALSE(bootimg_header(&v, NULL));

    (void)put_image(s_ram.mem, 100U, 9U, 1U);
    TEST_ASSERT_EQUAL(BOOT_CHECK_VALID, check(&validator, 0U, &info, &steps));
    const uint32_t reads = s_ram.reads;

    (void)memset(&again, 0, sizeof(again));
    TEST_ASSERT_EQUAL(BOOT_CHECK_VALID, validator.step(validator.ctx, &again));
    TEST_ASSERT_EQUAL_UINT32(reads, s_ram.reads);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(info.id, again.id, sizeof(info.id));
    TEST_ASSERT_EQUAL_UINT32(9U, again.version);

    validator.start(validator.ctx, &s_dev, RAM_SIZE - 128U, SLOT_SIZE); /* beyond the device */
    TEST_ASSERT_EQUAL(BOOT_CHECK_INVALID, validator.step(validator.ctx, &info));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, bootimg_status(&v));
    TEST_ASSERT_EQUAL_UINT32(reads, s_ram.reads);
}

/* Two images that differ only in the version, or only in a payload byte, have different ids. */
static void test_the_id_tells_images_apart(void)
{
    uint8_t a[IMAGE_HEADER_SIZE + 64U];
    uint8_t b[IMAGE_HEADER_SIZE + 64U];
    uint8_t c[IMAGE_HEADER_SIZE + 64U];
    uint8_t id_a[BOOT_IMAGE_ID_SIZE];
    uint8_t id_b[BOOT_IMAGE_ID_SIZE];
    uint8_t id_c[BOOT_IMAGE_ID_SIZE];

    const image_header_t ha = put_image(a, 64U, 0x01000000U, 5U);
    const image_header_t hb = put_image(b, 64U, 0x01000001U, 5U);
    const image_header_t hc = put_image(c, 64U, 0x01000000U, 6U);

    TEST_ASSERT_TRUE(bootimg_image_id(&ha, id_a));
    TEST_ASSERT_TRUE(bootimg_image_id(&hb, id_b));
    TEST_ASSERT_TRUE(bootimg_image_id(&hc, id_c));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(id_a, id_b, sizeof(id_a)));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(id_a, id_c, sizeof(id_a)));
    TEST_ASSERT_FALSE(bootimg_image_id(NULL, id_a));
    TEST_ASSERT_FALSE(bootimg_image_id(&ha, NULL));
}

static void test_null_arguments(void)
{
    bootimg_t v;
    boot_validator_t validator;

    TEST_ASSERT_FALSE(bootimg_init(NULL, IMAGE_TARGET_HOST, &validator));
    TEST_ASSERT_FALSE(bootimg_init(&v, IMAGE_TARGET_HOST, NULL));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, bootimg_status(NULL));
    TEST_ASSERT_FALSE(bootimg_header(NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, bootimg_steps(NULL));
    TEST_ASSERT_EQUAL(FLASH_OK, bootimg_flash_status(NULL));
}

/* ------------------------------------------------------------------- with lib/boot, host layout */

#define HOST_SIZE   0x202000U
#define HOST_SECTOR 4096U

static uint8_t s_host[HOST_SIZE];
static uint32_t s_erase_counts[HOST_SIZE / HOST_SECTOR];
static nor_t s_nor;
static flash_dev_t s_host_dev;
static bootimg_t s_validator;
static boot_t s_boot;

static void host_flash(void)
{
    const nor_config_t config = {.geometry = {.size = HOST_SIZE,
                                              .sector_size = HOST_SECTOR,
                                              .page_size = 256U,
                                              .unit_size = 8U},
                                 .timing = {.page_program_ns = 0U, .sector_erase_ns = 0U},
                                 .memory = s_host,
                                 .erase_counts = s_erase_counts};

    (void)memset(s_host, 0xFF, sizeof(s_host));
    TEST_ASSERT_TRUE(nor_init(&s_nor, &config));
    nor_device(&s_nor, &s_host_dev);
}

static void host_boot(void)
{
    boot_config_t config;

    (void)memset(&config, 0, sizeof(config));
    config.dev = &s_host_dev;
    config.meta_addr = 0U;
    config.meta_sectors = 2U;
    config.slot[0] = (boot_region_t){.addr = 0x2000U, .size = 0x100000U};
    config.slot[1] = (boot_region_t){.addr = 0x102000U, .size = 0x100000U};
    TEST_ASSERT_TRUE(bootimg_init(&s_validator, IMAGE_TARGET_HOST, &config.validator));
    TEST_ASSERT_EQUAL(BOOT_OK, boot_init(&s_boot, &config));
}

/* Through lib/boot: the factory image in slot A is adopted with its id, the bootloader boots it
 * again with nothing written, and a slot A rewritten behind the metadata with another valid image
 * is not the confirmed image any more: nothing is bootable. */
static void test_lib_boot_binds_its_records_to_the_id(void)
{
    boot_selection_t sel;
    boot_description_t d;
    uint8_t id[BOOT_IMAGE_ID_SIZE];

    host_flash();
    const image_header_t header = put_image(&s_host[0x2000U], 5000U, 0x01000000U, 1U);

    host_boot();
    TEST_ASSERT_EQUAL(BOOT_OK, boot_select(&s_boot, &sel));
    TEST_ASSERT_EQUAL(BOOT_EVENT_ADOPT, sel.event);
    TEST_ASSERT_EQUAL_UINT8(0U, sel.slot);
    TEST_ASSERT_EQUAL_HEX32(0x01000000U, sel.version);
    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    TEST_ASSERT_TRUE(bootimg_image_id(&header, id));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(id, d.slot[0].id, sizeof(id));

    TEST_ASSERT_EQUAL(BOOT_OK, boot_select(&s_boot, &sel));
    TEST_ASSERT_EQUAL(BOOT_EVENT_NONE, sel.event);
    TEST_ASSERT_EQUAL_UINT8(0U, sel.slot);

    /* Another image of the same size and version, written into the slot without lib/boot. */
    (void)put_image(&s_host[0x2000U], 5000U, 0x01000000U, 2U);
    TEST_ASSERT_EQUAL(BOOT_E_NOTHING_BOOTABLE, boot_select(&s_boot, &sel));
    TEST_ASSERT_EQUAL_UINT8(BOOT_NO_SLOT, sel.slot);
}

/* The slot writer with this validator commits an image as the trial with its id, and the next
 * select boots it as attempt 1. */
static void test_the_slot_writer_commits_with_the_id(void)
{
    static uint8_t image[IMAGE_HEADER_SIZE + 3000U];
    static boot_writer_t writer;
    boot_selection_t sel;
    boot_description_t d;
    uint8_t id[BOOT_IMAGE_ID_SIZE];
    boot_status_t status;

    host_flash();
    (void)put_image(&s_host[0x2000U], 2000U, 0x01000000U, 1U);
    host_boot();
    TEST_ASSERT_EQUAL(BOOT_OK, boot_select(&s_boot, &sel));

    const image_header_t header = put_image(image, 3000U, 0x01010000U, 9U);

    TEST_ASSERT_EQUAL(BOOT_OK, boot_writer_begin(&writer, &s_boot, 1U, (uint32_t)sizeof(image)));

    do {
        status = boot_writer_erase_step(&writer);
    } while (status == BOOT_BUSY);

    TEST_ASSERT_EQUAL(BOOT_OK, status);
    TEST_ASSERT_EQUAL(BOOT_OK, boot_writer_write(&writer, 0U, image, (uint32_t)sizeof(image)));

    do {
        status = boot_writer_finish_step(&writer);
    } while (status == BOOT_BUSY);

    TEST_ASSERT_EQUAL(BOOT_OK, status);
    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    TEST_ASSERT_EQUAL(BOOT_SLOT_TRIAL, d.slot[1].state);
    TEST_ASSERT_TRUE(bootimg_image_id(&header, id));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(id, d.slot[1].id, sizeof(id));

    TEST_ASSERT_EQUAL(BOOT_OK, boot_select(&s_boot, &sel));
    TEST_ASSERT_TRUE(sel.trial);
    TEST_ASSERT_EQUAL_UINT8(1U, sel.slot);
    TEST_ASSERT_EQUAL_UINT8(1U, sel.attempt);
    TEST_ASSERT_EQUAL_HEX32(0x01010000U, sel.version);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_valid_image_validates_with_the_digest_of_its_header_as_id);
    RUN_TEST(test_what_lib_image_refuses_is_invalid);
    RUN_TEST(test_a_failed_read_is_an_error);
    RUN_TEST(test_after_the_result_and_before_a_start);
    RUN_TEST(test_the_id_tells_images_apart);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_lib_boot_binds_its_records_to_the_id);
    RUN_TEST(test_the_slot_writer_commits_with_the_id);
    return UNITY_END();
}
