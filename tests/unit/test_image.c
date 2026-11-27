/* SPDX-License-Identifier: MIT */
/*
 * The image format (lib/image), in the order of its header: the version encoding; every field
 * of the header at its offset and back; the validation of a slot, accepted and refused for
 * every reason, in the order of the checks; every single flipped bit in header and payload;
 * the validation in steps against the one in a call; failed reads; refused arguments.
 *
 * The slots live in a flash device of this file (s_dev): RAM with the host geometry of the
 * project (4 KiB sectors, 256-byte pages, 8-byte program units) behind the read operation of
 * flash_dev_t. The validation only reads, so the device has no program or erase operation;
 * images are put into its memory directly, over "erased" bytes of 0xFF. The read operation
 * counts what it is asked for, so that the tests can check what the validation reads, and can
 * be made to fail at the n-th read, the way a device fails when the supply goes.
 *
 * Most tests use a small slot (SLOT_SIZE at SLOT_ADDR) to stay fast; one uses a slot of the
 * host layout's size (1 MiB).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crc/crc.h"
#include "image/image.h"
#include "sha256/sha256.h"
#include "unity.h"

#define SECTOR_SIZE 4096U
#define PAGE_SIZE   256U
#define UNIT_SIZE   8U

/* Two small slots of 4 sectors after 2 sectors left free, then a slot of 1 MiB. Written as
 * literals: an index computed as a product of 32-bit constants would be widened to size_t. */
#define SLOT_ADDR  0x2000U
#define SLOT_SIZE  0x4000U
#define SLOT2_ADDR 0x6000U
#define BIG_ADDR   0xA000U
#define BIG_SIZE   0x100000U
#define DEV_SIZE   (BIG_ADDR + BIG_SIZE)

#define MAX_PAYLOAD (SLOT_SIZE - IMAGE_HEADER_SIZE)
#define NO_FAILURE  UINT32_MAX

_Static_assert(SLOT_ADDR == (2U * SECTOR_SIZE), "two sectors before the first slot");
_Static_assert(SLOT_SIZE == (4U * SECTOR_SIZE), "a small slot is four sectors");
_Static_assert(SLOT2_ADDR == (SLOT_ADDR + SLOT_SIZE), "the second slot follows the first");
_Static_assert(BIG_ADDR == (SLOT2_ADDR + SLOT_SIZE), "the large slot follows the second");
_Static_assert(BIG_SIZE == (1024U * 1024U), "the slot size of the host layout");

typedef struct {
    uint8_t mem[DEV_SIZE];
    uint32_t reads;        /* read calls since the last reset_counters() */
    uint32_t largest_read; /* their largest length */
    uint32_t bytes_read;   /* the sum of their lengths */
    uint32_t next_addr;    /* the address after the last read */
    bool contiguous;       /* every read began where the one before ended */
    uint32_t first_addr;   /* the address of the first read */
    uint32_t fail_after;   /* reads that succeed before every further one fails */
    flash_status_t fail_with;
} ram_flash_t;

static ram_flash_t s_flash;

static flash_status_t ram_read(void *ctx, uint32_t addr, uint8_t *dst, uint32_t len)
{
    ram_flash_t *flash = (ram_flash_t *)ctx;
    flash_status_t status = FLASH_OK;

    if ((flash->reads > 0U) && (addr != flash->next_addr)) {
        flash->contiguous = false;
    }

    if (flash->reads == 0U) {
        flash->first_addr = addr;
    }

    flash->reads++;
    flash->largest_read = (len > flash->largest_read) ? len : flash->largest_read;
    flash->bytes_read += len;
    flash->next_addr = addr + len;

    if ((len == 0U) || (addr >= DEV_SIZE) || (len > (DEV_SIZE - addr))) {
        status = FLASH_E_RANGE;
    } else if (flash->fail_after == 0U) {
        status = flash->fail_with;
    } else {
        (void)memcpy(dst, &flash->mem[addr], len);

        if (flash->fail_after != NO_FAILURE) {
            flash->fail_after--;
        }
    }

    return status;
}

static const flash_dev_t s_dev = {
    .geometry = {.size = DEV_SIZE,
                 .sector_size = SECTOR_SIZE,
                 .page_size = PAGE_SIZE,
                 .unit_size = UNIT_SIZE},
    .ctx = &s_flash,
    .read = ram_read,
    .program = NULL,
    .erase = NULL,
};

static uint8_t s_payload[BIG_SIZE];
static uint8_t s_bytes[IMAGE_HEADER_SIZE];
static image_header_t s_header;
static image_validator_t s_validator;
static image_validator_t s_validator2;

/* xorshift32, reset before every test. */
static uint32_t s_random_state;

static uint32_t random_u32(void)
{
    s_random_state ^= s_random_state << 13U;
    s_random_state ^= s_random_state >> 17U;
    s_random_state ^= s_random_state << 5U;
    return s_random_state;
}

static void random_fill(uint8_t *buffer, size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        buffer[i] = (uint8_t)(random_u32() >> 24U);
    }
}

static const char *at(const char *what, unsigned long value)
{
    static char text[64];

    (void)snprintf(text, sizeof(text), "%s %lu", what, value);
    return text;
}

static void reset_counters(void)
{
    s_flash.reads = 0U;
    s_flash.largest_read = 0U;
    s_flash.bytes_read = 0U;
    s_flash.next_addr = 0U;
    s_flash.contiguous = true;
    s_flash.first_addr = 0U;
    s_flash.fail_after = NO_FAILURE;
    s_flash.fail_with = FLASH_E_POWER_LOST;
}

void setUp(void)
{
    s_random_state = 0x1A6E0004U;
    (void)memset(s_flash.mem, 0xFF, sizeof(s_flash.mem));
    reset_counters();
    random_fill(s_payload, sizeof(s_payload));
    /* Nonsense first: nothing may need the memory to be prepared. */
    (void)memset(&s_header, 0xA5, sizeof(s_header));
    (void)memset(&s_validator, 0xA5, sizeof(s_validator));
    (void)memset(&s_validator2, 0xA5, sizeof(s_validator2));
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------------------- */

/* A correct header for the first `len` bytes of s_payload, for the host, version 1.2.3. */
static void make_header(image_header_t *header, uint32_t len)
{
    uint8_t digest[SHA256_DIGEST_SIZE];

    TEST_ASSERT_TRUE(sha256_compute(s_payload, len, digest));
    TEST_ASSERT_TRUE(
        image_header_init(header, IMAGE_TARGET_HOST, image_version_make(1U, 2U, 3U), len, digest));
}

/* Erase the slot, then write the encoded `header` and `len` bytes of s_payload into it. */
static void put_image(uint32_t slot_addr, uint32_t slot_size, const image_header_t *header,
                      uint32_t len)
{
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(slot_size, IMAGE_HEADER_SIZE + len);
    (void)memset(&s_flash.mem[slot_addr], 0xFF, slot_size);
    TEST_ASSERT_TRUE(image_header_encode(header, &s_flash.mem[slot_addr]));
    (void)memcpy(&s_flash.mem[slot_addr + IMAGE_HEADER_SIZE], s_payload, len);
}

/* A correct image of `len` payload bytes in the small slot. */
static void put_valid(uint32_t len)
{
    image_header_t header;

    make_header(&header, len);
    put_image(SLOT_ADDR, SLOT_SIZE, &header, len);
}

static image_status_t validate(void)
{
    return image_validate(&s_dev, SLOT_ADDR, SLOT_SIZE, IMAGE_TARGET_HOST, NULL);
}

/* The validation of a slot in steps. Every step must read at most once and at most one chunk;
 * the number of steps is returned in *steps. */
static image_status_t validate_in_steps(image_validator_t *validator, uint32_t slot_addr,
                                        uint32_t slot_size, uint32_t target, uint32_t *steps)
{
    image_status_t status = image_validate_begin(validator, &s_dev, slot_addr, slot_size, target);

    *steps = 0U;

    while (status == IMAGE_PENDING) {
        const uint32_t reads = s_flash.reads;

        status = image_validate_step(validator);
        (*steps)++;
        TEST_ASSERT_EQUAL_UINT32(reads + 1U, s_flash.reads);
    }

    TEST_ASSERT_LESS_OR_EQUAL_UINT32(IMAGE_READ_CHUNK, s_flash.largest_read);
    return status;
}

/* Write `header`, whose fields a test has changed, over the small slot's header, with a CRC
 * that is correct for the changed fields. */
static void reencode(const image_header_t *header)
{
    TEST_ASSERT_TRUE(image_header_encode(header, &s_flash.mem[SLOT_ADDR]));
}

static uint32_t get_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void expect_same_header(const image_header_t *expected, const image_header_t *actual)
{
    TEST_ASSERT_EQUAL_HEX32(expected->magic, actual->magic);
    TEST_ASSERT_EQUAL_HEX16(expected->header_version, actual->header_version);
    TEST_ASSERT_EQUAL_HEX16(expected->header_size, actual->header_size);
    TEST_ASSERT_EQUAL_HEX32(expected->target, actual->target);
    TEST_ASSERT_EQUAL_HEX32(expected->image_version, actual->image_version);
    TEST_ASSERT_EQUAL_HEX32(expected->payload_size, actual->payload_size);
    TEST_ASSERT_EQUAL_HEX32(expected->flags, actual->flags);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected->payload_sha256, actual->payload_sha256,
                                 SHA256_DIGEST_SIZE);
}

/* ------------------------------------------------------------------------------------------
 * Version encoding
 * ---------------------------------------------------------------------------------------- */

static void test_version_is_major_minor_patch_in_8_8_16_bits(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x01020003U, image_version_make(1U, 2U, 3U));
    TEST_ASSERT_EQUAL_HEX32(0x00000000U, image_version_make(0U, 0U, 0U));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFU, image_version_make(255U, 255U, 65535U));
    TEST_ASSERT_EQUAL_HEX32(0x0A000000U, image_version_make(10U, 0U, 0U));
    TEST_ASSERT_EQUAL_HEX32(0x00140000U, image_version_make(0U, 20U, 0U));
    TEST_ASSERT_EQUAL_HEX32(0x00001234U, image_version_make(0U, 0U, 0x1234U));

    for (uint32_t trial = 0U; trial < 1000U; trial++) {
        const uint32_t version = random_u32();

        TEST_ASSERT_EQUAL_HEX32(version, image_version_make(image_version_major(version),
                                                            image_version_minor(version),
                                                            image_version_patch(version)));
    }

    TEST_ASSERT_EQUAL_UINT8(1U, image_version_major(0x01020003U));
    TEST_ASSERT_EQUAL_UINT8(2U, image_version_minor(0x01020003U));
    TEST_ASSERT_EQUAL_UINT16(3U, image_version_patch(0x01020003U));
}

static int compare(uint32_t a, uint32_t b)
{
    int order = 0;

    if (a < b) {
        order = -1;
    } else if (a > b) {
        order = 1;
    } else {
        order = 0;
    }

    return order;
}

/* Comparing the numbers orders the versions as their triples are ordered. */
static void test_version_numbers_order_like_the_triples(void)
{
    for (uint32_t trial = 0U; trial < 5000U; trial++) {
        /* Small parts, so that equal ones are common; the patches reach the top of 16 bits. */
        const uint8_t a[2] = {(uint8_t)(random_u32() % 3U), (uint8_t)(random_u32() % 3U)};
        const uint8_t b[2] = {(uint8_t)(random_u32() % 3U), (uint8_t)(random_u32() % 3U)};
        const uint16_t pa = (uint16_t)((random_u32() % 3U) * 0x7FFFU);
        const uint16_t pb = (uint16_t)((random_u32() % 3U) * 0x7FFFU);
        int by_triple = compare(a[0], b[0]);

        if (by_triple == 0) {
            by_triple = compare(a[1], b[1]);
        }

        if (by_triple == 0) {
            by_triple = compare(pa, pb);
        }

        const int by_number =
            compare(image_version_make(a[0], a[1], pa), image_version_make(b[0], b[1], pb));

        TEST_ASSERT_EQUAL_INT_MESSAGE(by_triple, by_number, at("trial", trial));
    }
}

/* ------------------------------------------------------------------------------------------
 * The header: every field at its offset, and back
 * ---------------------------------------------------------------------------------------- */

/* The table of image.h, byte by byte, with a value in every field whose bytes all differ, so
 * that a field at the wrong offset or in the wrong byte order shows. */
static void test_encode_writes_every_field_at_its_offset_little_endian(void)
{
    image_header_t header;

    header.magic = IMAGE_MAGIC;
    header.header_version = 0x1234U;
    header.header_size = 0x5678U;
    header.target = 0xA1B2C3D4U;
    header.image_version = 0x01020003U;
    header.payload_size = 0x00ABCDEFU;
    header.flags = 0x0F1E2D3CU;

    for (uint32_t i = 0U; i < SHA256_DIGEST_SIZE; i++) {
        header.payload_sha256[i] = (uint8_t)(0x40U + i);
    }

    header.header_crc = 0xDEADBEEFU; /* not read by the encoder */

    TEST_ASSERT_TRUE(image_header_encode(&header, s_bytes));

    static const uint8_t fields[24] = {
        'F',   'C',   'N',   'I',   /* 0 magic */
        0x34U, 0x12U,               /* 4 header_version */
        0x78U, 0x56U,               /* 6 header_size */
        0xD4U, 0xC3U, 0xB2U, 0xA1U, /* 8 target */
        0x03U, 0x00U, 0x02U, 0x01U, /* 12 image_version */
        0xEFU, 0xCDU, 0xABU, 0x00U, /* 16 payload_size */
        0x3CU, 0x2DU, 0x1EU, 0x0FU, /* 20 flags */
    };

    TEST_ASSERT_EQUAL_HEX8_ARRAY(fields, s_bytes, sizeof(fields));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(header.payload_sha256, &s_bytes[IMAGE_OFFSET_PAYLOAD_SHA256],
                                 SHA256_DIGEST_SIZE);

    for (uint32_t i = IMAGE_OFFSET_UNUSED; i < IMAGE_OFFSET_HEADER_CRC; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFFU, s_bytes[i], at("unused byte", i));
    }

    TEST_ASSERT_EQUAL_HEX32(crc32_compute(s_bytes, IMAGE_OFFSET_HEADER_CRC),
                            get_le32(&s_bytes[IMAGE_OFFSET_HEADER_CRC]));
}

static void test_init_fills_the_fields_of_this_format(void)
{
    uint8_t digest[SHA256_DIGEST_SIZE];

    random_fill(digest, sizeof(digest));
    TEST_ASSERT_TRUE(
        image_header_init(&s_header, IMAGE_TARGET_MPS2_AN385, 0x07080009U, 4321U, digest));
    TEST_ASSERT_EQUAL_HEX32(IMAGE_MAGIC, s_header.magic);
    TEST_ASSERT_EQUAL_HEX16(IMAGE_HEADER_VERSION, s_header.header_version);
    TEST_ASSERT_EQUAL_HEX16(IMAGE_HEADER_SIZE, s_header.header_size);
    TEST_ASSERT_EQUAL_HEX32(IMAGE_TARGET_MPS2_AN385, s_header.target);
    TEST_ASSERT_EQUAL_HEX32(0x07080009U, s_header.image_version);
    TEST_ASSERT_EQUAL_UINT32(4321U, s_header.payload_size);
    TEST_ASSERT_EQUAL_HEX32(0U, s_header.flags);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(digest, s_header.payload_sha256, SHA256_DIGEST_SIZE);
    TEST_ASSERT_EQUAL_HEX32(0U, s_header.header_crc);

    /* The magic is the ASCII text "FCNI" in the first four bytes. */
    TEST_ASSERT_TRUE(image_header_encode(&s_header, s_bytes));
    TEST_ASSERT_EQUAL_MEMORY("FCNI", s_bytes, 4U);
}

/* Random values in every field, through encode and decode: each comes back. */
static void test_decode_reads_back_every_field(void)
{
    for (uint32_t trial = 0U; trial < 500U; trial++) {
        image_header_t header;
        image_header_t decoded;

        header.magic = random_u32();
        header.header_version = (uint16_t)random_u32();
        header.header_size = (uint16_t)random_u32();
        header.target = random_u32();
        header.image_version = random_u32();
        header.payload_size = random_u32();
        header.flags = random_u32();
        random_fill(header.payload_sha256, SHA256_DIGEST_SIZE);

        TEST_ASSERT_TRUE(image_header_encode(&header, s_bytes));
        (void)memset(&decoded, 0x5A, sizeof(decoded));
        (void)image_header_decode(s_bytes, &decoded);
        expect_same_header(&header, &decoded);
        TEST_ASSERT_EQUAL_HEX32(crc32_compute(s_bytes, IMAGE_OFFSET_HEADER_CRC),
                                decoded.header_crc);
    }
}

/* image_header_decode() makes checks 3 to 7 and no others. */
static void test_decode_checks_the_format_but_not_target_or_size(void)
{
    image_header_t header;
    image_header_t decoded;

    make_header(&header, 100U);
    header.target = 0U;
    header.payload_size = 0U; /* both refused by a validation, not by a decode */
    TEST_ASSERT_TRUE(image_header_encode(&header, s_bytes));
    TEST_ASSERT_EQUAL(IMAGE_OK, image_header_decode(s_bytes, &decoded));

    s_bytes[0] ^= 0x01U;
    TEST_ASSERT_EQUAL(IMAGE_E_MAGIC, image_header_decode(s_bytes, &decoded));
    s_bytes[0] ^= 0x01U;
    s_bytes[IMAGE_OFFSET_HEADER_VERSION] ^= 0x02U;
    TEST_ASSERT_EQUAL(IMAGE_E_HEADER_VERSION, image_header_decode(s_bytes, &decoded));
    s_bytes[IMAGE_OFFSET_HEADER_VERSION] ^= 0x02U;
    s_bytes[IMAGE_OFFSET_HEADER_SIZE + 1U] ^= 0x80U;
    TEST_ASSERT_EQUAL(IMAGE_E_HEADER_SIZE, image_header_decode(s_bytes, &decoded));
    s_bytes[IMAGE_OFFSET_HEADER_SIZE + 1U] ^= 0x80U;
    s_bytes[IMAGE_OFFSET_HEADER_CRC + 3U] ^= 0x10U;
    TEST_ASSERT_EQUAL(IMAGE_E_HEADER_CRC, image_header_decode(s_bytes, &decoded));
    s_bytes[IMAGE_OFFSET_HEADER_CRC + 3U] ^= 0x10U;
    TEST_ASSERT_EQUAL(IMAGE_OK, image_header_decode(s_bytes, &decoded));

    header.flags = 0x00000100U;
    TEST_ASSERT_TRUE(image_header_encode(&header, s_bytes));
    TEST_ASSERT_EQUAL(IMAGE_E_RESERVED, image_header_decode(s_bytes, &decoded));
}

/* ------------------------------------------------------------------------------------------
 * Accepted
 * ---------------------------------------------------------------------------------------- */

static void test_valid_images_are_accepted_with_their_header(void)
{
    static const uint32_t lengths[] = {1U,   2U,   63U,   64U,   65U,        127U,
                                       128U, 300U, 4096U, 4097U, MAX_PAYLOAD};

    for (size_t i = 0U; i < (sizeof(lengths) / sizeof(lengths[0])); i++) {
        image_header_t expected;
        image_header_t got;

        make_header(&expected, lengths[i]);
        put_image(SLOT_ADDR, SLOT_SIZE, &expected, lengths[i]);
        (void)memset(&got, 0x5A, sizeof(got));
        TEST_ASSERT_EQUAL_MESSAGE(
            IMAGE_OK, image_validate(&s_dev, SLOT_ADDR, SLOT_SIZE, IMAGE_TARGET_HOST, &got),
            at("payload", lengths[i]));
        expect_same_header(&expected, &got);
        TEST_ASSERT_EQUAL_HEX32(get_le32(&s_flash.mem[SLOT_ADDR + IMAGE_OFFSET_HEADER_CRC]),
                                got.header_crc);
    }
}

/* The other target and another version number are as valid for a validation that expects
 * them. The version is not checked at all. */
static void test_target_must_match_and_version_is_not_checked(void)
{
    image_header_t header;

    make_header(&header, 200U);
    header.target = IMAGE_TARGET_MPS2_AN385;
    header.image_version = 0U;
    put_image(SLOT_ADDR, SLOT_SIZE, &header, 200U);
    TEST_ASSERT_EQUAL(IMAGE_OK,
                      image_validate(&s_dev, SLOT_ADDR, SLOT_SIZE, IMAGE_TARGET_MPS2_AN385, NULL));
    TEST_ASSERT_EQUAL(IMAGE_E_TARGET, validate());

    header.image_version = 0xFFFFFFFFU;
    put_image(SLOT_ADDR, SLOT_SIZE, &header, 200U);
    TEST_ASSERT_EQUAL(IMAGE_OK,
                      image_validate(&s_dev, SLOT_ADDR, SLOT_SIZE, IMAGE_TARGET_MPS2_AN385, NULL));
}

/* A slot of the host layout's size, filled to the last byte, and one byte more. */
static void test_payload_filling_a_1_mib_slot_is_accepted(void)
{
    const uint32_t max = BIG_SIZE - IMAGE_HEADER_SIZE;
    image_header_t header;
    uint32_t steps = 0U;

    make_header(&header, max);
    put_image(BIG_ADDR, BIG_SIZE, &header, max);
    TEST_ASSERT_EQUAL(
        IMAGE_OK, validate_in_steps(&s_validator, BIG_ADDR, BIG_SIZE, IMAGE_TARGET_HOST, &steps));
    TEST_ASSERT_EQUAL_UINT32(4U + (max / IMAGE_READ_CHUNK), steps);
    TEST_ASSERT_EQUAL_UINT32(BIG_SIZE, s_flash.bytes_read);

    header.payload_size = max + 1U;
    TEST_ASSERT_TRUE(image_header_encode(&header, &s_flash.mem[BIG_ADDR]));
    TEST_ASSERT_EQUAL(IMAGE_E_PAYLOAD_SIZE,
                      image_validate(&s_dev, BIG_ADDR, BIG_SIZE, IMAGE_TARGET_HOST, NULL));
}

/* The validation reads the header and the payload, in order, and nothing else: not the slot
 * bytes after the payload, whatever they hold. */
static void test_validation_reads_header_and_payload_only(void)
{
    for (uint32_t len = 1U; len <= 300U; len++) {
        put_valid(len);
        /* Whatever an earlier image left after this one's payload. */
        random_fill(&s_flash.mem[SLOT_ADDR + IMAGE_HEADER_SIZE + len], 64U);
        reset_counters();
        TEST_ASSERT_EQUAL_MESSAGE(IMAGE_OK, validate(), at("payload", len));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(IMAGE_HEADER_SIZE + len, s_flash.bytes_read,
                                         at("payload", len));
        TEST_ASSERT_EQUAL_UINT32(SLOT_ADDR, s_flash.first_addr);
        TEST_ASSERT_TRUE(s_flash.contiguous);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(IMAGE_READ_CHUNK, s_flash.largest_read);
    }

    /* A refused header: the header and nothing more. */
    s_flash.mem[SLOT_ADDR] = 0x00U;
    reset_counters();
    TEST_ASSERT_EQUAL(IMAGE_E_MAGIC, validate());
    TEST_ASSERT_EQUAL_UINT32(IMAGE_HEADER_SIZE, s_flash.bytes_read);
    TEST_ASSERT_EQUAL_UINT32(IMAGE_HEADER_SIZE / IMAGE_READ_CHUNK, s_flash.reads);
}

/* ------------------------------------------------------------------------------------------
 * Refused, for every reason
 * ---------------------------------------------------------------------------------------- */

static void test_erased_or_zeroed_slot_has_no_magic(void)
{
    TEST_ASSERT_EQUAL(IMAGE_E_MAGIC, validate());
    (void)memset(&s_flash.mem[SLOT_ADDR], 0x00, SLOT_SIZE);
    TEST_ASSERT_EQUAL(IMAGE_E_MAGIC, validate());
}

static void test_bad_magic_is_refused(void)
{
    static const uint32_t magics[] = {0U, 0xFFFFFFFFU, IMAGE_MAGIC ^ 1U, IMAGE_MAGIC ^ 0x80000000U,
                                      0x46434E49U /* "FCNI" read big-endian */};
    image_header_t header;

    make_header(&header, 100U);

    for (size_t i = 0U; i < (sizeof(magics) / sizeof(magics[0])); i++) {
        put_image(SLOT_ADDR, SLOT_SIZE, &header, 100U);
        header.magic = magics[i];
        reencode(&header);
        TEST_ASSERT_EQUAL_MESSAGE(IMAGE_E_MAGIC, validate(), at("magic", magics[i]));
    }
}

static void test_bad_header_version_is_refused(void)
{
    static const uint16_t versions[] = {0U, 2U, 0x0101U, 0xFFFFU};
    image_header_t header;

    make_header(&header, 100U);
    put_image(SLOT_ADDR, SLOT_SIZE, &header, 100U);

    for (size_t i = 0U; i < (sizeof(versions) / sizeof(versions[0])); i++) {
        header.header_version = versions[i];
        reencode(&header);
        TEST_ASSERT_EQUAL_MESSAGE(IMAGE_E_HEADER_VERSION, validate(), at("version", versions[i]));
    }
}

static void test_bad_header_size_is_refused(void)
{
    static const uint16_t sizes[] = {0U, 255U, 257U, 512U, 0xFFFFU};
    image_header_t header;

    make_header(&header, 100U);
    put_image(SLOT_ADDR, SLOT_SIZE, &header, 100U);

    for (size_t i = 0U; i < (sizeof(sizes) / sizeof(sizes[0])); i++) {
        header.header_size = sizes[i];
        reencode(&header);
        TEST_ASSERT_EQUAL_MESSAGE(IMAGE_E_HEADER_SIZE, validate(), at("size", sizes[i]));
    }
}

/* Each bit of the stored CRC, and a CRC computed without the final XOR or over one byte too
 * few. */
static void test_bad_header_crc_is_refused(void)
{
    uint8_t *crc = &s_flash.mem[SLOT_ADDR + IMAGE_OFFSET_HEADER_CRC];

    put_valid(100U);

    for (uint32_t bit = 0U; bit < 32U; bit++) {
        crc[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
        TEST_ASSERT_EQUAL_MESSAGE(IMAGE_E_HEADER_CRC, validate(), at("bit", bit));
        crc[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
    }

    TEST_ASSERT_EQUAL(IMAGE_OK, validate());

    const uint32_t right = get_le32(crc);
    const uint32_t wrong[] = {
        right ^ CRC32_XOROUT,
        crc32_compute(&s_flash.mem[SLOT_ADDR], IMAGE_OFFSET_HEADER_CRC - 1U),
    };

    for (size_t i = 0U; i < (sizeof(wrong) / sizeof(wrong[0])); i++) {
        for (uint32_t b = 0U; b < 4U; b++) {
            crc[b] = (uint8_t)(wrong[i] >> (8U * b));
        }

        TEST_ASSERT_EQUAL_MESSAGE(IMAGE_E_HEADER_CRC, validate(), at("variant", i));
    }
}

static void test_flags_or_unused_bytes_not_erased_are_refused(void)
{
    static const uint32_t flags[] = {1U, 0x80000000U, 0xFFFFFFFFU};
    static const uint32_t unused_at[] = {IMAGE_OFFSET_UNUSED, 100U, IMAGE_OFFSET_HEADER_CRC - 1U};
    image_header_t header;

    make_header(&header, 100U);
    put_image(SLOT_ADDR, SLOT_SIZE, &header, 100U);

    for (size_t i = 0U; i < (sizeof(flags) / sizeof(flags[0])); i++) {
        header.flags = flags[i];
        reencode(&header);
        TEST_ASSERT_EQUAL_MESSAGE(IMAGE_E_RESERVED, validate(), at("flags", flags[i]));
    }

    header.flags = 0U;

    for (size_t i = 0U; i < (sizeof(unused_at) / sizeof(unused_at[0])); i++) {
        uint8_t *bytes = &s_flash.mem[SLOT_ADDR];

        reencode(&header);
        TEST_ASSERT_EQUAL(IMAGE_OK, validate());

        /* One unused byte changed, with a CRC that matches the change. */
        bytes[unused_at[i]] = 0xFEU;
        const uint32_t crc = crc32_compute(bytes, IMAGE_OFFSET_HEADER_CRC);

        for (uint32_t b = 0U; b < 4U; b++) {
            bytes[IMAGE_OFFSET_HEADER_CRC + b] = (uint8_t)(crc >> (8U * b));
        }

        TEST_ASSERT_EQUAL_MESSAGE(IMAGE_E_RESERVED, validate(), at("unused byte", unused_at[i]));
    }
}

static void test_other_target_is_refused(void)
{
    static const uint32_t targets[] = {0U, IMAGE_TARGET_MPS2_AN385, 3U, 0x00000101U, 0xFFFFFFFFU};
    image_header_t header;

    make_header(&header, 100U);
    put_image(SLOT_ADDR, SLOT_SIZE, &header, 100U);

    for (size_t i = 0U; i < (sizeof(targets) / sizeof(targets[0])); i++) {
        header.target = targets[i];
        reencode(&header);
        TEST_ASSERT_EQUAL_MESSAGE(IMAGE_E_TARGET, validate(), at("target", targets[i]));
    }
}

static void test_payload_size_zero_or_beyond_the_slot_is_refused(void)
{
    static const uint32_t sizes[] = {0U,          MAX_PAYLOAD + 1U, SLOT_SIZE,
                                     0x7FFFFFFFU, 0xFFFFFF00U,      0xFFFFFFFFU};
    image_header_t header;

    make_header(&header, MAX_PAYLOAD);
    put_image(SLOT_ADDR, SLOT_SIZE, &header, MAX_PAYLOAD);
    TEST_ASSERT_EQUAL(IMAGE_OK, validate());

    for (size_t i = 0U; i < (sizeof(sizes) / sizeof(sizes[0])); i++) {
        header.payload_size = sizes[i];
        reencode(&header);
        reset_counters();
        TEST_ASSERT_EQUAL_MESSAGE(IMAGE_E_PAYLOAD_SIZE, validate(), at("size", sizes[i]));
        /* Refused from the header: no payload byte is read. */
        TEST_ASSERT_EQUAL_UINT32(IMAGE_HEADER_SIZE, s_flash.bytes_read);
    }
}

static void test_payload_that_does_not_match_its_digest_is_refused(void)
{
    image_header_t header;

    /* The digest field changed, with a matching CRC. */
    make_header(&header, 300U);
    put_image(SLOT_ADDR, SLOT_SIZE, &header, 300U);
    header.payload_sha256[31] ^= 0x01U;
    reencode(&header);
    TEST_ASSERT_EQUAL(IMAGE_E_DIGEST, validate());

    /* The header names one byte fewer, or one byte more (an erased 0xFF follows the payload),
     * than the digest is of. */
    make_header(&header, 300U);
    header.payload_size = 299U;
    reencode(&header);
    TEST_ASSERT_EQUAL(IMAGE_E_DIGEST, validate());
    header.payload_size = 301U;
    reencode(&header);
    TEST_ASSERT_EQUAL(IMAGE_E_DIGEST, validate());

    /* The digest of an empty message. */
    TEST_ASSERT_TRUE(sha256_compute(NULL, 0U, header.payload_sha256));
    header.payload_size = 300U;
    reencode(&header);
    TEST_ASSERT_EQUAL(IMAGE_E_DIGEST, validate());
}

/* When several checks would fail, the first in the order of image.h is reported. Each case
 * breaks two neighbours in that order. */
static void test_the_first_failing_check_is_reported(void)
{
    image_header_t header;

    make_header(&header, 100U);
    put_image(SLOT_ADDR, SLOT_SIZE, &header, 100U);

    header.magic = 0U;
    header.header_version = 2U;
    reencode(&header);
    TEST_ASSERT_EQUAL(IMAGE_E_MAGIC, validate());

    make_header(&header, 100U);
    header.header_version = 2U;
    header.header_size = 512U;
    reencode(&header);
    TEST_ASSERT_EQUAL(IMAGE_E_HEADER_VERSION, validate());

    /* A bad size, then a CRC that does not match either. */
    make_header(&header, 100U);
    header.header_size = 512U;
    reencode(&header);
    s_flash.mem[SLOT_ADDR + IMAGE_OFFSET_HEADER_CRC] ^= 0x01U;
    TEST_ASSERT_EQUAL(IMAGE_E_HEADER_SIZE, validate());

    /* A flags word changed after the CRC was computed. */
    make_header(&header, 100U);
    reencode(&header);
    s_flash.mem[SLOT_ADDR + IMAGE_OFFSET_FLAGS] = 0x01U;
    TEST_ASSERT_EQUAL(IMAGE_E_HEADER_CRC, validate());

    make_header(&header, 100U);
    header.flags = 1U;
    header.target = 0U;
    reencode(&header);
    TEST_ASSERT_EQUAL(IMAGE_E_RESERVED, validate());

    make_header(&header, 100U);
    header.target = 0U;
    header.payload_size = 0U;
    reencode(&header);
    TEST_ASSERT_EQUAL(IMAGE_E_TARGET, validate());

    make_header(&header, 100U);
    header.payload_size = 0U;
    header.payload_sha256[0] ^= 0x01U;
    reencode(&header);
    TEST_ASSERT_EQUAL(IMAGE_E_PAYLOAD_SIZE, validate());
}

/*
 * One flipped bit, anywhere in the 256 + 300 bytes of an image, is refused, and with the
 * reason that the order of the checks gives: a flip in the magic, the header version or the
 * header size fails that check before the CRC is looked at; any other flip in the header fails
 * the CRC; a flip in the payload fails the digest. A flip in the byte after the payload is not
 * seen: that byte is not part of the image.
 */
static void test_every_single_bit_flip_in_header_or_payload_is_refused(void)
{
    const uint32_t len = 300U;
    uint8_t *image = &s_flash.mem[SLOT_ADDR];

    put_valid(len);

    for (uint32_t byte = 0U; byte < (IMAGE_HEADER_SIZE + len); byte++) {
        image_status_t expected;

        if (byte < IMAGE_OFFSET_HEADER_VERSION) {
            expected = IMAGE_E_MAGIC;
        } else if (byte < IMAGE_OFFSET_HEADER_SIZE) {
            expected = IMAGE_E_HEADER_VERSION;
        } else if (byte < IMAGE_OFFSET_TARGET) {
            expected = IMAGE_E_HEADER_SIZE;
        } else if (byte < IMAGE_HEADER_SIZE) {
            expected = IMAGE_E_HEADER_CRC;
        } else {
            expected = IMAGE_E_DIGEST;
        }

        for (uint32_t bit = 0U; bit < 8U; bit++) {
            image[byte] ^= (uint8_t)(1U << bit);
            TEST_ASSERT_EQUAL_MESSAGE(expected, validate(), at("bit", (8U * byte) + bit));
            image[byte] ^= (uint8_t)(1U << bit);
        }
    }

    for (uint32_t bit = 0U; bit < 8U; bit++) {
        image[IMAGE_HEADER_SIZE + len] ^= (uint8_t)(1U << bit);
        TEST_ASSERT_EQUAL(IMAGE_OK, validate());
    }
}

/* ------------------------------------------------------------------------------------------
 * In steps
 * ---------------------------------------------------------------------------------------- */

/* The slot contents of the comparison below: valid images of many lengths, and one of each
 * kind of refusal. */
static void put_case(uint32_t which)
{
    image_header_t header;
    const uint32_t len = 1U + ((which * 37U) % 700U);

    make_header(&header, len);
    put_image(SLOT_ADDR, SLOT_SIZE, &header, len);

    switch (which % 10U) {
    case 1U:
        header.magic ^= 0x00010000U;
        reencode(&header);
        break;
    case 2U:
        header.header_version = 3U;
        reencode(&header);
        break;
    case 3U:
        header.header_size = 128U;
        reencode(&header);
        break;
    case 4U:
        s_flash.mem[SLOT_ADDR + 200U] ^= 0x04U; /* an unused byte, CRC unchanged */
        break;
    case 5U:
        header.flags = 2U;
        reencode(&header);
        break;
    case 6U:
        header.target = IMAGE_TARGET_MPS2_AN385;
        reencode(&header);
        break;
    case 7U:
        header.payload_size = MAX_PAYLOAD + which;
        reencode(&header);
        break;
    case 8U:
        s_flash.mem[SLOT_ADDR + IMAGE_HEADER_SIZE + (len / 2U)] ^= 0x40U;
        break;
    default:
        break; /* valid */
    }
}

/* image_validate() and the steps give the same result and header for the same slot; the steps
 * read at most one chunk each, and their number is 4 for a refused header and 4 + ceil(payload
 * / 64) for an accepted one. */
static void test_steps_give_the_result_of_the_one_call(void)
{
    for (uint32_t which = 0U; which < 400U; which++) {
        image_header_t from_call;
        image_header_t from_steps;
        uint32_t steps = 0U;

        put_case(which);
        (void)memset(&from_call, 0x11, sizeof(from_call));
        (void)memset(&from_steps, 0x22, sizeof(from_steps));

        const image_status_t call =
            image_validate(&s_dev, SLOT_ADDR, SLOT_SIZE, IMAGE_TARGET_HOST, &from_call);

        reset_counters();
        const image_status_t stepped =
            validate_in_steps(&s_validator, SLOT_ADDR, SLOT_SIZE, IMAGE_TARGET_HOST, &steps);

        TEST_ASSERT_EQUAL_MESSAGE(call, stepped, at("case", which));
        TEST_ASSERT_TRUE(image_validator_header(&s_validator, &from_steps));

        if (call == IMAGE_OK) {
            const uint32_t len = from_steps.payload_size;

            expect_same_header(&from_call, &from_steps);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(
                4U + ((len + IMAGE_READ_CHUNK - 1U) / IMAGE_READ_CHUNK), steps, at("case", which));
        } else if (call == IMAGE_E_DIGEST) {
            const uint32_t len = from_steps.payload_size;

            TEST_ASSERT_EQUAL_UINT32_MESSAGE(
                4U + ((len + IMAGE_READ_CHUNK - 1U) / IMAGE_READ_CHUNK), steps, at("case", which));
        } else {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(4U, steps, at("case", which));
        }

        /* Once the result is known, further steps return it and read nothing. */
        const uint32_t reads = s_flash.reads;

        TEST_ASSERT_EQUAL(stepped, image_validate_step(&s_validator));
        TEST_ASSERT_EQUAL(stepped, image_validate_step(&s_validator));
        TEST_ASSERT_EQUAL_UINT32(reads, s_flash.reads);
    }
}

/* The header is available from the step that read its last byte on, and not before. */
static void test_header_is_available_once_read(void)
{
    image_header_t expected;
    image_header_t got;

    make_header(&expected, 500U);
    put_image(SLOT_ADDR, SLOT_SIZE, &expected, 500U);
    TEST_ASSERT_EQUAL(IMAGE_PENDING, image_validate_begin(&s_validator, &s_dev, SLOT_ADDR,
                                                          SLOT_SIZE, IMAGE_TARGET_HOST));

    for (uint32_t step = 0U; step < 3U; step++) {
        TEST_ASSERT_FALSE(image_validator_header(&s_validator, &got));
        TEST_ASSERT_EQUAL(IMAGE_PENDING, image_validate_step(&s_validator));
    }

    TEST_ASSERT_FALSE(image_validator_header(&s_validator, &got));
    TEST_ASSERT_EQUAL(IMAGE_PENDING, image_validate_step(&s_validator));
    TEST_ASSERT_TRUE(image_validator_header(&s_validator, &got));
    expect_same_header(&expected, &got);
}

/* Two validations on two slots, one step each in turn: each gets the result it gets alone. A
 * validator keeps its whole state in its own memory. */
static void test_interleaved_validations_do_not_disturb_each_other(void)
{
    for (uint32_t trial = 0U; trial < 40U; trial++) {
        image_header_t first;
        image_header_t second;
        const uint32_t len1 = 1U + (random_u32() % 2000U);
        const uint32_t len2 = 1U + (random_u32() % 2000U);

        make_header(&first, len1);
        put_image(SLOT_ADDR, SLOT_SIZE, &first, len1);
        make_header(&second, len2);
        put_image(SLOT2_ADDR, SLOT_SIZE, &second, len2);

        if ((trial % 2U) == 1U) {
            s_flash.mem[SLOT2_ADDR + IMAGE_HEADER_SIZE + (random_u32() % len2)] ^= 0x10U;
        }

        const image_status_t alone1 =
            image_validate(&s_dev, SLOT_ADDR, SLOT_SIZE, IMAGE_TARGET_HOST, NULL);
        const image_status_t alone2 =
            image_validate(&s_dev, SLOT2_ADDR, SLOT_SIZE, IMAGE_TARGET_HOST, NULL);
        image_status_t s1 =
            image_validate_begin(&s_validator, &s_dev, SLOT_ADDR, SLOT_SIZE, IMAGE_TARGET_HOST);
        image_status_t s2 =
            image_validate_begin(&s_validator2, &s_dev, SLOT2_ADDR, SLOT_SIZE, IMAGE_TARGET_HOST);

        while ((s1 == IMAGE_PENDING) || (s2 == IMAGE_PENDING)) {
            s1 = image_validate_step(&s_validator);
            s2 = image_validate_step(&s_validator2);
        }

        TEST_ASSERT_EQUAL(IMAGE_OK, alone1);
        TEST_ASSERT_EQUAL(((trial % 2U) == 1U) ? IMAGE_E_DIGEST : IMAGE_OK, alone2);
        TEST_ASSERT_EQUAL_MESSAGE(alone1, s1, at("trial", trial));
        TEST_ASSERT_EQUAL_MESSAGE(alone2, s2, at("trial", trial));
    }
}

/* ------------------------------------------------------------------------------------------
 * Failed reads
 * ---------------------------------------------------------------------------------------- */

/* The device fails at the n-th read, for every n of a validation: in the header and in the
 * payload. The validation ends in the step of the failed read with IMAGE_E_FLASH and the
 * device's status, reads nothing more, and has the header only if it was read before. */
static void test_a_failed_read_ends_the_validation(void)
{
    const uint32_t len = 1000U;
    const uint32_t reads = 4U + ((len + IMAGE_READ_CHUNK - 1U) / IMAGE_READ_CHUNK);
    static const flash_status_t failures[] = {FLASH_E_POWER_LOST, FLASH_E_RANGE};

    put_valid(len);

    for (size_t f = 0U; f < (sizeof(failures) / sizeof(failures[0])); f++) {
        for (uint32_t n = 0U; n < reads; n++) {
            image_header_t header;
            uint32_t steps = 0U;

            reset_counters();
            s_flash.fail_after = n;
            s_flash.fail_with = failures[f];
            TEST_ASSERT_EQUAL_MESSAGE(
                IMAGE_E_FLASH,
                validate_in_steps(&s_validator, SLOT_ADDR, SLOT_SIZE, IMAGE_TARGET_HOST, &steps),
                at("failing read", n));
            TEST_ASSERT_EQUAL_UINT32(n + 1U, steps);
            TEST_ASSERT_EQUAL(failures[f], image_validator_flash_status(&s_validator));
            TEST_ASSERT_EQUAL(n >= 4U, image_validator_header(&s_validator, &header));

            TEST_ASSERT_EQUAL(IMAGE_E_FLASH, image_validate_step(&s_validator));
            TEST_ASSERT_EQUAL_UINT32(n + 1U, s_flash.reads);

            reset_counters();
            s_flash.fail_after = n;
            s_flash.fail_with = failures[f];
            TEST_ASSERT_EQUAL(IMAGE_E_FLASH, validate());
        }
    }

    /* Without a failure the same slot is valid, and a new begin forgets the failed read. */
    reset_counters();
    TEST_ASSERT_EQUAL(IMAGE_OK, validate());
    TEST_ASSERT_EQUAL_UINT32(reads, s_flash.reads);
    TEST_ASSERT_EQUAL(IMAGE_PENDING, image_validate_begin(&s_validator, &s_dev, SLOT_ADDR,
                                                          SLOT_SIZE, IMAGE_TARGET_HOST));
    TEST_ASSERT_EQUAL(FLASH_OK, image_validator_flash_status(&s_validator));
}

/* ------------------------------------------------------------------------------------------
 * Arguments
 * ---------------------------------------------------------------------------------------- */

static void test_slots_outside_the_device_or_too_small_are_refused(void)
{
    flash_dev_t no_read = s_dev;
    flash_dev_t small = s_dev;

    no_read.read = NULL;
    small.geometry.size = 0xFFFFF000U; /* a device near the end of the 32-bit range */

    put_valid(100U);

    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_validate(NULL, SLOT_ADDR, SLOT_SIZE, 1U, NULL));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_validate(&no_read, SLOT_ADDR, SLOT_SIZE, 1U, NULL));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_validate(&s_dev, SLOT_ADDR, 0U, 1U, NULL));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_validate(&s_dev, SLOT_ADDR, IMAGE_HEADER_SIZE, 1U, NULL));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_validate(&s_dev, DEV_SIZE, SLOT_SIZE, 1U, NULL));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG,
                      image_validate(&s_dev, DEV_SIZE - SECTOR_SIZE, 2U * SECTOR_SIZE, 1U, NULL));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_validate(&s_dev, 0U, DEV_SIZE + 1U, 1U, NULL));
    /* slot_addr + slot_size would wrap to a small number. */
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_validate(&small, 0xFFFFE000U, 0x00002000U, 1U, NULL));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_validate(&small, 0x00001000U, 0xFFFFF001U, 1U, NULL));

    /* The smallest slot, and a slot that ends at the end of the device, are fine. */
    put_valid(1U);
    TEST_ASSERT_EQUAL(IMAGE_OK, image_validate(&s_dev, SLOT_ADDR, IMAGE_HEADER_SIZE + 1U,
                                               IMAGE_TARGET_HOST, NULL));
    TEST_ASSERT_EQUAL(IMAGE_E_MAGIC, image_validate(&s_dev, DEV_SIZE - SECTOR_SIZE, SECTOR_SIZE,
                                                    IMAGE_TARGET_HOST, NULL));
    TEST_ASSERT_EQUAL(IMAGE_E_MAGIC, image_validate(&s_dev, 0U, DEV_SIZE, IMAGE_TARGET_HOST, NULL));
}

/* A refused begin leaves a validator whose steps refuse too; a zeroed or NULL validator is
 * refused; nothing is read. */
static void test_unprepared_validators_are_refused(void)
{
    image_header_t header;

    put_valid(100U);
    TEST_ASSERT_EQUAL(IMAGE_E_ARG,
                      image_validate_begin(NULL, &s_dev, SLOT_ADDR, SLOT_SIZE, IMAGE_TARGET_HOST));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_validate_step(NULL));

    TEST_ASSERT_EQUAL(IMAGE_E_ARG,
                      image_validate_begin(&s_validator, &s_dev, SLOT_ADDR, 0U, IMAGE_TARGET_HOST));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_validate_step(&s_validator));
    TEST_ASSERT_FALSE(image_validator_header(&s_validator, &header));

    (void)memset(&s_validator, 0, sizeof(s_validator));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_validate_step(&s_validator));
    TEST_ASSERT_EQUAL_UINT32(0U, s_flash.reads);

    TEST_ASSERT_FALSE(image_validator_header(NULL, &header));
    TEST_ASSERT_EQUAL(FLASH_OK, image_validator_flash_status(NULL));

    TEST_ASSERT_EQUAL(IMAGE_PENDING, image_validate_begin(&s_validator, &s_dev, SLOT_ADDR,
                                                          SLOT_SIZE, IMAGE_TARGET_HOST));
    for (uint32_t i = 0U; i < 4U; i++) {
        TEST_ASSERT_EQUAL(IMAGE_PENDING, image_validate_step(&s_validator));
    }
    TEST_ASSERT_FALSE(image_validator_header(&s_validator, NULL));
}

/* image_validate() copies the header only for a valid image. */
static void test_one_call_leaves_the_header_alone_when_refused(void)
{
    image_header_t header;
    image_header_t untouched;

    put_valid(100U);
    s_flash.mem[SLOT_ADDR + IMAGE_HEADER_SIZE] ^= 0x01U;
    (void)memset(&header, 0x3C, sizeof(header));
    (void)memset(&untouched, 0x3C, sizeof(untouched));
    TEST_ASSERT_EQUAL(IMAGE_E_DIGEST,
                      image_validate(&s_dev, SLOT_ADDR, SLOT_SIZE, IMAGE_TARGET_HOST, &header));
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &header, sizeof(header));
}

static void test_header_functions_refuse_null_pointers(void)
{
    uint8_t digest[SHA256_DIGEST_SIZE] = {0U};
    image_header_t header;

    (void)memset(s_bytes, 0x77, sizeof(s_bytes));
    TEST_ASSERT_FALSE(image_header_init(NULL, 1U, 1U, 1U, digest));
    TEST_ASSERT_FALSE(image_header_init(&header, 1U, 1U, 1U, NULL));
    TEST_ASSERT_TRUE(image_header_init(&header, 1U, 1U, 1U, digest));
    TEST_ASSERT_FALSE(image_header_encode(NULL, s_bytes));
    TEST_ASSERT_FALSE(image_header_encode(&header, NULL));
    TEST_ASSERT_EACH_EQUAL_HEX8(0x77U, s_bytes, sizeof(s_bytes));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_header_decode(NULL, &header));
    TEST_ASSERT_EQUAL(IMAGE_E_ARG, image_header_decode(s_bytes, NULL));
}

static void test_every_status_has_its_name(void)
{
    TEST_ASSERT_EQUAL_STRING("ok", image_status_name(IMAGE_OK));
    TEST_ASSERT_EQUAL_STRING("pending", image_status_name(IMAGE_PENDING));
    TEST_ASSERT_EQUAL_STRING("bad_argument", image_status_name(IMAGE_E_ARG));
    TEST_ASSERT_EQUAL_STRING("flash_error", image_status_name(IMAGE_E_FLASH));
    TEST_ASSERT_EQUAL_STRING("bad_magic", image_status_name(IMAGE_E_MAGIC));
    TEST_ASSERT_EQUAL_STRING("bad_header_version", image_status_name(IMAGE_E_HEADER_VERSION));
    TEST_ASSERT_EQUAL_STRING("bad_header_size", image_status_name(IMAGE_E_HEADER_SIZE));
    TEST_ASSERT_EQUAL_STRING("bad_header_crc", image_status_name(IMAGE_E_HEADER_CRC));
    TEST_ASSERT_EQUAL_STRING("bad_reserved", image_status_name(IMAGE_E_RESERVED));
    TEST_ASSERT_EQUAL_STRING("bad_target", image_status_name(IMAGE_E_TARGET));
    TEST_ASSERT_EQUAL_STRING("bad_payload_size", image_status_name(IMAGE_E_PAYLOAD_SIZE));
    TEST_ASSERT_EQUAL_STRING("bad_digest", image_status_name(IMAGE_E_DIGEST));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): outside it on purpose */
    const image_status_t beyond = (image_status_t)((int)IMAGE_E_DIGEST + 1);

    TEST_ASSERT_EQUAL_STRING("unknown", image_status_name(beyond));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_version_is_major_minor_patch_in_8_8_16_bits);
    RUN_TEST(test_version_numbers_order_like_the_triples);
    RUN_TEST(test_encode_writes_every_field_at_its_offset_little_endian);
    RUN_TEST(test_init_fills_the_fields_of_this_format);
    RUN_TEST(test_decode_reads_back_every_field);
    RUN_TEST(test_decode_checks_the_format_but_not_target_or_size);
    RUN_TEST(test_valid_images_are_accepted_with_their_header);
    RUN_TEST(test_target_must_match_and_version_is_not_checked);
    RUN_TEST(test_payload_filling_a_1_mib_slot_is_accepted);
    RUN_TEST(test_validation_reads_header_and_payload_only);
    RUN_TEST(test_erased_or_zeroed_slot_has_no_magic);
    RUN_TEST(test_bad_magic_is_refused);
    RUN_TEST(test_bad_header_version_is_refused);
    RUN_TEST(test_bad_header_size_is_refused);
    RUN_TEST(test_bad_header_crc_is_refused);
    RUN_TEST(test_flags_or_unused_bytes_not_erased_are_refused);
    RUN_TEST(test_other_target_is_refused);
    RUN_TEST(test_payload_size_zero_or_beyond_the_slot_is_refused);
    RUN_TEST(test_payload_that_does_not_match_its_digest_is_refused);
    RUN_TEST(test_the_first_failing_check_is_reported);
    RUN_TEST(test_every_single_bit_flip_in_header_or_payload_is_refused);
    RUN_TEST(test_steps_give_the_result_of_the_one_call);
    RUN_TEST(test_header_is_available_once_read);
    RUN_TEST(test_interleaved_validations_do_not_disturb_each_other);
    RUN_TEST(test_a_failed_read_ends_the_validation);
    RUN_TEST(test_slots_outside_the_device_or_too_small_are_refused);
    RUN_TEST(test_unprepared_validators_are_refused);
    RUN_TEST(test_one_call_leaves_the_header_alone_when_refused);
    RUN_TEST(test_header_functions_refuse_null_pointers);
    RUN_TEST(test_every_status_has_its_name);
    return UNITY_END();
}
