/* SPDX-License-Identifier: MIT */
#include "image/image.h"

#include <stddef.h>
#include <string.h>

#include "crc/crc.h"

/*
 * The format and the order of the checks are described in image.h. The header is turned into
 * bytes and back one byte at a time: header_byte() gives the byte at any offset of a header,
 * reader_take() puts a byte at any offset into the fields. Both are written as the table of
 * image.h, field by field in offset order, so the two directions can be compared line by line.
 *
 * The validation reads the header through the same reader in chunks of IMAGE_READ_CHUNK
 * bytes, so a header is decoded the same way whether it comes from a buffer
 * (image_header_decode) or from the flash (image_validate_step).
 */

/* The layout of image.h: every field starts where the one before ends, and the unused bytes
 * run up to the CRC, the last word of the header. */
_Static_assert(IMAGE_OFFSET_MAGIC == 0U, "the magic opens the header");
_Static_assert(IMAGE_OFFSET_HEADER_VERSION == (IMAGE_OFFSET_MAGIC + 4U), "magic: 4 bytes");
_Static_assert(IMAGE_OFFSET_HEADER_SIZE == (IMAGE_OFFSET_HEADER_VERSION + 2U),
               "header_version: 2 bytes");
_Static_assert(IMAGE_OFFSET_TARGET == (IMAGE_OFFSET_HEADER_SIZE + 2U), "header_size: 2 bytes");
_Static_assert(IMAGE_OFFSET_IMAGE_VERSION == (IMAGE_OFFSET_TARGET + 4U), "target: 4 bytes");
_Static_assert(IMAGE_OFFSET_PAYLOAD_SIZE == (IMAGE_OFFSET_IMAGE_VERSION + 4U),
               "image_version: 4 bytes");
_Static_assert(IMAGE_OFFSET_FLAGS == (IMAGE_OFFSET_PAYLOAD_SIZE + 4U), "payload_size: 4 bytes");
_Static_assert(IMAGE_OFFSET_PAYLOAD_SHA256 == (IMAGE_OFFSET_FLAGS + 4U), "flags: 4 bytes");
_Static_assert(IMAGE_OFFSET_UNUSED == (IMAGE_OFFSET_PAYLOAD_SHA256 + SHA256_DIGEST_SIZE),
               "payload_sha256: one digest");
_Static_assert(IMAGE_OFFSET_HEADER_CRC == (IMAGE_HEADER_SIZE - 4U), "the CRC is the last word");
_Static_assert(IMAGE_OFFSET_UNUSED < IMAGE_OFFSET_HEADER_CRC, "the fields fit into the header");

/* The steps of image.h: the header is a whole number of chunks, so the payload's chunks are
 * aligned to its start, and a chunk is one SHA-256 block. */
_Static_assert((IMAGE_HEADER_SIZE % IMAGE_READ_CHUNK) == 0U, "the header is whole chunks");
_Static_assert(IMAGE_READ_CHUNK == SHA256_BLOCK_SIZE, "a chunk is one SHA-256 block");

/* Byte `index` (0 = least significant) of a little-endian field. */
static uint8_t le_byte(uint32_t value, uint32_t index)
{
    return (uint8_t)(value >> (8U * index));
}

/* The byte at offset `at` (below IMAGE_OFFSET_HEADER_CRC) of the encoded `header`. */
static uint8_t header_byte(const image_header_t *header, uint32_t at)
{
    uint8_t byte;

    if (at < IMAGE_OFFSET_HEADER_VERSION) {
        byte = le_byte(header->magic, at - IMAGE_OFFSET_MAGIC);
    } else if (at < IMAGE_OFFSET_HEADER_SIZE) {
        byte = le_byte(header->header_version, at - IMAGE_OFFSET_HEADER_VERSION);
    } else if (at < IMAGE_OFFSET_TARGET) {
        byte = le_byte(header->header_size, at - IMAGE_OFFSET_HEADER_SIZE);
    } else if (at < IMAGE_OFFSET_IMAGE_VERSION) {
        byte = le_byte(header->target, at - IMAGE_OFFSET_TARGET);
    } else if (at < IMAGE_OFFSET_PAYLOAD_SIZE) {
        byte = le_byte(header->image_version, at - IMAGE_OFFSET_IMAGE_VERSION);
    } else if (at < IMAGE_OFFSET_FLAGS) {
        byte = le_byte(header->payload_size, at - IMAGE_OFFSET_PAYLOAD_SIZE);
    } else if (at < IMAGE_OFFSET_PAYLOAD_SHA256) {
        byte = le_byte(header->flags, at - IMAGE_OFFSET_FLAGS);
    } else if (at < IMAGE_OFFSET_UNUSED) {
        byte = header->payload_sha256[at - IMAGE_OFFSET_PAYLOAD_SHA256];
    } else {
        byte = IMAGE_UNUSED_BYTE;
    }

    return byte;
}

/* `byte` shifted into place `index` (0 = least significant) of a little-endian field. */
static uint32_t le_part(uint8_t byte, uint32_t index)
{
    return (uint32_t)byte << (8U * index);
}

static void reader_init(image_header_reader_t *reader)
{
    (void)memset(&reader->header, 0, sizeof(reader->header));
    reader->crc = CRC32_INIT;
    reader->offset = 0U;
    reader->unused_erased = true;
}

/* Take the next header byte. The fields start at 0 (reader_init) and every byte is ORed into
 * its place, so a field is complete once its last byte has been taken. A reader takes
 * IMAGE_HEADER_SIZE bytes in all: both callers give it exactly the header. */
static void reader_take(image_header_reader_t *reader, uint8_t byte)
{
    const uint32_t at = reader->offset;
    image_header_t *header = &reader->header;

    if (at < IMAGE_OFFSET_HEADER_CRC) {
        reader->crc = crc32_update(reader->crc, &byte, 1U);
    }

    if (at < IMAGE_OFFSET_HEADER_VERSION) {
        header->magic |= le_part(byte, at - IMAGE_OFFSET_MAGIC);
    } else if (at < IMAGE_OFFSET_HEADER_SIZE) {
        header->header_version =
            (uint16_t)(header->header_version | le_part(byte, at - IMAGE_OFFSET_HEADER_VERSION));
    } else if (at < IMAGE_OFFSET_TARGET) {
        header->header_size =
            (uint16_t)(header->header_size | le_part(byte, at - IMAGE_OFFSET_HEADER_SIZE));
    } else if (at < IMAGE_OFFSET_IMAGE_VERSION) {
        header->target |= le_part(byte, at - IMAGE_OFFSET_TARGET);
    } else if (at < IMAGE_OFFSET_PAYLOAD_SIZE) {
        header->image_version |= le_part(byte, at - IMAGE_OFFSET_IMAGE_VERSION);
    } else if (at < IMAGE_OFFSET_FLAGS) {
        header->payload_size |= le_part(byte, at - IMAGE_OFFSET_PAYLOAD_SIZE);
    } else if (at < IMAGE_OFFSET_PAYLOAD_SHA256) {
        header->flags |= le_part(byte, at - IMAGE_OFFSET_FLAGS);
    } else if (at < IMAGE_OFFSET_UNUSED) {
        header->payload_sha256[at - IMAGE_OFFSET_PAYLOAD_SHA256] = byte;
    } else if (at < IMAGE_OFFSET_HEADER_CRC) {
        if (byte != IMAGE_UNUSED_BYTE) {
            reader->unused_erased = false;
        }
    } else {
        header->header_crc |= le_part(byte, at - IMAGE_OFFSET_HEADER_CRC);
    }

    reader->offset = at + 1U;
}

static void reader_take_all(image_header_reader_t *reader, const uint8_t *bytes, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++) {
        reader_take(reader, bytes[i]);
    }
}

/* Checks 3 to 7 of image.h, on a header that has been read completely. */
static image_status_t reader_check(const image_header_reader_t *reader)
{
    const image_header_t *header = &reader->header;
    image_status_t status = IMAGE_OK;

    if (header->magic != IMAGE_MAGIC) {
        status = IMAGE_E_MAGIC;
    } else if (header->header_version != IMAGE_HEADER_VERSION) {
        status = IMAGE_E_HEADER_VERSION;
    } else if (header->header_size != IMAGE_HEADER_SIZE) {
        status = IMAGE_E_HEADER_SIZE;
    } else if (header->header_crc != (reader->crc ^ CRC32_XOROUT)) {
        status = IMAGE_E_HEADER_CRC;
    } else if ((header->flags != 0U) || !reader->unused_erased) {
        status = IMAGE_E_RESERVED;
    } else {
        /* the header is of this format and intact */
    }

    return status;
}

/* Checks 8 and 9 of image.h. slot_size is above IMAGE_HEADER_SIZE (image_validate_begin). */
static image_status_t slot_check(const image_header_t *header, uint32_t slot_size, uint32_t target)
{
    image_status_t status = IMAGE_OK;

    if (header->target != target) {
        status = IMAGE_E_TARGET;
    } else if ((header->payload_size == 0U) ||
               (header->payload_size > (slot_size - IMAGE_HEADER_SIZE))) {
        status = IMAGE_E_PAYLOAD_SIZE;
    } else {
        /* the payload fits */
    }

    return status;
}

uint32_t image_version_make(uint8_t major, uint8_t minor, uint16_t patch)
{
    return ((uint32_t)major << 24U) | ((uint32_t)minor << 16U) | (uint32_t)patch;
}

uint8_t image_version_major(uint32_t version)
{
    return (uint8_t)(version >> 24U);
}

uint8_t image_version_minor(uint32_t version)
{
    return (uint8_t)(version >> 16U);
}

uint16_t image_version_patch(uint32_t version)
{
    return (uint16_t)version;
}

bool image_header_init(image_header_t *header, uint32_t target, uint32_t image_version,
                       uint32_t payload_size, const uint8_t payload_sha256[SHA256_DIGEST_SIZE])
{
    const bool ok = (header != NULL) && (payload_sha256 != NULL);

    if (ok) {
        header->magic = IMAGE_MAGIC;
        header->header_version = (uint16_t)IMAGE_HEADER_VERSION;
        header->header_size = (uint16_t)IMAGE_HEADER_SIZE;
        header->target = target;
        header->image_version = image_version;
        header->payload_size = payload_size;
        header->flags = 0U;
        (void)memcpy(header->payload_sha256, payload_sha256, SHA256_DIGEST_SIZE);
        header->header_crc = 0U;
    }

    return ok;
}

bool image_header_encode(const image_header_t *header, uint8_t bytes[IMAGE_HEADER_SIZE])
{
    const bool ok = (header != NULL) && (bytes != NULL);

    if (ok) {
        for (uint32_t at = 0U; at < IMAGE_OFFSET_HEADER_CRC; at++) {
            bytes[at] = header_byte(header, at);
        }

        const uint32_t crc = crc32_compute(bytes, IMAGE_OFFSET_HEADER_CRC);

        for (uint32_t i = 0U; i < 4U; i++) {
            bytes[IMAGE_OFFSET_HEADER_CRC + i] = le_byte(crc, i);
        }
    }

    return ok;
}

image_status_t image_header_decode(const uint8_t bytes[IMAGE_HEADER_SIZE], image_header_t *header)
{
    image_status_t status = IMAGE_E_ARG;

    if ((bytes != NULL) && (header != NULL)) {
        image_header_reader_t reader;

        reader_init(&reader);
        reader_take_all(&reader, bytes, IMAGE_HEADER_SIZE);
        *header = reader.header;
        status = reader_check(&reader);
    }

    return status;
}

image_status_t image_validate_begin(image_validator_t *validator, const flash_dev_t *dev,
                                    uint32_t slot_addr, uint32_t slot_size, uint32_t target)
{
    image_status_t status = IMAGE_E_ARG;

    if (validator != NULL) {
        /* Check 1. The second comparison cannot wrap: it is made only when slot_addr is
         * inside the device. */
        const bool usable = (dev != NULL) && (dev->read != NULL) &&
                            (slot_size > IMAGE_HEADER_SIZE) && (slot_addr < dev->geometry.size) &&
                            (slot_size <= (dev->geometry.size - slot_addr));

        status = usable ? IMAGE_PENDING : IMAGE_E_ARG;
        validator->dev = usable ? dev : NULL;
        validator->slot_addr = slot_addr;
        validator->slot_size = slot_size;
        validator->target = target;
        validator->offset = 0U;
        validator->status = status;
        validator->flash_status = FLASH_OK;
        reader_init(&validator->reader);
        (void)sha256_init(&validator->sha);
    }

    return status;
}

/* Read `len` bytes at slot offset validator->offset into `chunk`. A failed read ends the
 * validation with IMAGE_E_FLASH (check 2). */
static bool read_chunk(image_validator_t *validator, uint8_t *chunk, uint32_t len)
{
    const flash_dev_t *dev = validator->dev;
    const flash_status_t read =
        dev->read(dev->ctx, validator->slot_addr + validator->offset, chunk, len);

    if (read != FLASH_OK) {
        validator->flash_status = read;
        validator->status = IMAGE_E_FLASH;
    }

    return read == FLASH_OK;
}

/* One chunk of the header; after the last one, checks 3 to 9. */
static void step_header(image_validator_t *validator)
{
    uint8_t chunk[IMAGE_READ_CHUNK];

    if (read_chunk(validator, chunk, IMAGE_READ_CHUNK)) {
        reader_take_all(&validator->reader, chunk, IMAGE_READ_CHUNK);
        validator->offset += IMAGE_READ_CHUNK;

        if (validator->offset == IMAGE_HEADER_SIZE) {
            image_status_t status = reader_check(&validator->reader);

            if (status == IMAGE_OK) {
                status =
                    slot_check(&validator->reader.header, validator->slot_size, validator->target);
            }

            validator->status = (status == IMAGE_OK) ? IMAGE_PENDING : status;
        }
    }
}

/* One chunk of the payload; after the last one, check 10. The end cannot wrap: the payload
 * fits into the slot (check 9), and the slot into the device's 32-bit address range. */
static void step_payload(image_validator_t *validator)
{
    const uint32_t end = IMAGE_HEADER_SIZE + validator->reader.header.payload_size;
    uint8_t chunk[IMAGE_READ_CHUNK];
    uint32_t len = end - validator->offset;

    if (len > IMAGE_READ_CHUNK) {
        len = IMAGE_READ_CHUNK;
    }

    if (read_chunk(validator, chunk, len)) {
        /* The context was prepared by image_validate_begin() and the chunk is not NULL: the
         * hash cannot refuse. */
        (void)sha256_update(&validator->sha, chunk, len);
        validator->offset += len;

        if (validator->offset == end) {
            uint8_t digest[SHA256_DIGEST_SIZE];

            (void)sha256_final(&validator->sha, digest);

            const bool match =
                (memcmp(digest, validator->reader.header.payload_sha256, SHA256_DIGEST_SIZE) == 0);

            validator->status = match ? IMAGE_OK : IMAGE_E_DIGEST;
        }
    }
}

image_status_t image_validate_step(image_validator_t *validator)
{
    image_status_t status = IMAGE_E_ARG;

    if ((validator != NULL) && (validator->dev != NULL)) {
        if (validator->status == IMAGE_PENDING) {
            if (validator->offset < IMAGE_HEADER_SIZE) {
                step_header(validator);
            } else {
                step_payload(validator);
            }
        }

        status = validator->status;
    }

    return status;
}

bool image_validator_header(const image_validator_t *validator, image_header_t *header)
{
    const bool ok =
        (validator != NULL) && (header != NULL) && (validator->reader.offset == IMAGE_HEADER_SIZE);

    if (ok) {
        *header = validator->reader.header;
    }

    return ok;
}

flash_status_t image_validator_flash_status(const image_validator_t *validator)
{
    flash_status_t status = FLASH_OK;

    if (validator != NULL) {
        status = validator->flash_status;
    }

    return status;
}

image_status_t image_validate(const flash_dev_t *dev, uint32_t slot_addr, uint32_t slot_size,
                              uint32_t target, image_header_t *header)
{
    image_validator_t validator;
    image_status_t status = image_validate_begin(&validator, dev, slot_addr, slot_size, target);

    while (status == IMAGE_PENDING) {
        status = image_validate_step(&validator);
    }

    if ((status == IMAGE_OK) && (header != NULL)) {
        *header = validator.reader.header;
    }

    return status;
}

const char *image_status_name(image_status_t status)
{
    const char *name;

    switch (status) {
    case IMAGE_OK:
        name = "ok";
        break;
    case IMAGE_PENDING:
        name = "pending";
        break;
    case IMAGE_E_ARG:
        name = "bad_argument";
        break;
    case IMAGE_E_FLASH:
        name = "flash_error";
        break;
    case IMAGE_E_MAGIC:
        name = "bad_magic";
        break;
    case IMAGE_E_HEADER_VERSION:
        name = "bad_header_version";
        break;
    case IMAGE_E_HEADER_SIZE:
        name = "bad_header_size";
        break;
    case IMAGE_E_HEADER_CRC:
        name = "bad_header_crc";
        break;
    case IMAGE_E_RESERVED:
        name = "bad_reserved";
        break;
    case IMAGE_E_TARGET:
        name = "bad_target";
        break;
    case IMAGE_E_PAYLOAD_SIZE:
        name = "bad_payload_size";
        break;
    case IMAGE_E_DIGEST:
        name = "bad_digest";
        break;
    default:
        name = "unknown";
        break;
    }

    return name;
}
