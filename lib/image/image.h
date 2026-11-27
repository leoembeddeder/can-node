/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The firmware image: the header that describes a payload, its encoding and decoding, and the
 * validation of a slot of flash that should hold an image. docs/update.md describes the format
 * for readers outside the code; tools/fcn_tools/image.py implements it a second time, from that
 * text, and tests/python/test_image.py holds the two against each other.
 *
 * AN IMAGE is a 256-byte header at the start of a slot and the payload right after it, at slot
 * offset 256 (IMAGE_PAYLOAD_OFFSET). On a Cortex-M target the payload begins with the vector
 * table, which VTOR can only point at a suitably aligned address; a slot starts at a sector
 * boundary, so the payload is 256-byte aligned. On the host the payload is the node's
 * executable.
 *
 * THE HEADER, version 1. Every field is little-endian (least significant byte at the lowest
 * offset) and is written and read one byte at a time, never by laying a struct over the
 * bytes, so the layout depends neither on the compiler nor on the byte order of the machine.
 *
 *   offset  size  field           meaning
 *        0     4  magic           the bytes 'F' 'C' 'N' 'I' (IMAGE_MAGIC as a little-endian word)
 *        4     2  header_version  IMAGE_HEADER_VERSION (1): the layout described here
 *        6     2  header_size     IMAGE_HEADER_SIZE (256): where the payload starts
 *        8     4  target          the build the payload is for: IMAGE_TARGET_HOST or
 *                                 IMAGE_TARGET_MPS2_AN385
 *       12     4  image_version   the version of the image (VERSION below)
 *       16     4  payload_size    bytes of payload, 1 .. slot size - 256
 *       20     4  flags           reserved: 0
 *       24    32  payload_sha256  SHA-256 (FIPS 180-4) of the payload, in the digest's byte
 *                                 order (lib/sha256)
 *       56   196  (unused)        0xFF each, the value of erased flash
 *      252     4  header_crc      CRC-32/ISO-HDLC (lib/crc, equal to zlib.crc32) of bytes 0..251
 *
 * VERSION. image_version = major << 24 | minor << 16 | patch, with major and minor 0..255 and
 * patch 0..65535 (image_version_make()), written major.minor.patch. Comparing two values as
 * unsigned integers orders them as the triples are ordered. Every 32-bit value is a version;
 * the validation below does not look at it (see WHAT IT DOES NOT CHECK).
 *
 * VALIDATION (image_validate(), or image_validate_begin() and image_validate_step()) reads a
 * slot through a flash_dev_t (lib/flash/flash.h) in chunks of at most IMAGE_READ_CHUNK bytes:
 * the header first, then payload_size bytes of payload. The result is the first of these that
 * fails, or IMAGE_OK:
 *
 *    1. the slot lies inside the device and has room for the header and one byte of payload,
 *       and the pointers are not NULL                                        IMAGE_E_ARG
 *    2. every read of the device succeeds (at any point below)               IMAGE_E_FLASH
 *    3. magic == IMAGE_MAGIC                                                 IMAGE_E_MAGIC
 *    4. header_version == IMAGE_HEADER_VERSION                               IMAGE_E_HEADER_VERSION
 *    5. header_size == IMAGE_HEADER_SIZE                                     IMAGE_E_HEADER_SIZE
 *    6. header_crc == the CRC-32 of header bytes 0..251                      IMAGE_E_HEADER_CRC
 *    7. flags == 0 and every unused byte is 0xFF                             IMAGE_E_RESERVED
 *    8. target == the target the caller expects                              IMAGE_E_TARGET
 *    9. 1 <= payload_size <= slot size - IMAGE_HEADER_SIZE                   IMAGE_E_PAYLOAD_SIZE
 *   10. the SHA-256 of the payload == payload_sha256                         IMAGE_E_DIGEST
 *
 * Checks 3 to 5 come before the CRC so that a header of another format (a later header
 * version with another layout or size) is reported as that and not as a damaged header. The
 * CRC then guards every field that checks 7 to 10 rely on. An erased slot reads 0xFF and fails
 * check 3. One flipped bit anywhere in the header fails one of checks 3 to 6 (a CRC detects
 * every single-bit error), one in the payload fails check 10.
 *
 * WHAT IT DOES NOT CHECK. Authenticity: anybody can compute a CRC and a SHA-256, so a valid
 * image proves that the slot holds what its packer wrote, not who the packer was; a product
 * would sign the digest and check the signature here (as MCUboot does with ECDSA P-256 or
 * Ed25519). Rollback: an older image_version is as valid as a newer one; there is no
 * anti-rollback counter. The bytes of the slot after the payload: they are not read. That the
 * payload is a program for the target: the target field is the packer's statement.
 *
 * STEPS. image_validate_begin() reads nothing. Each image_validate_step() reads one chunk of
 * at most IMAGE_READ_CHUNK bytes and hashes it (at most two SHA-256 compressions, in the step
 * that reads the last chunk and finishes the hash), so its time is bounded by one read of the
 * device and two compressions. A slot whose header is accepted takes 4 + ceil(payload_size /
 * 64) steps; one whose header is refused takes 4 (the whole header is read before any check);
 * a failed read ends the validation in the step in which it happens. A caller that must keep
 * its timing runs as many steps as fit into its time slice and comes back; image_validate()
 * runs them all. The two give the same result for the same slot. A validator keeps all its
 * state in its own memory, so several may run at once on different slots.
 *
 * CHUNKS. IMAGE_READ_CHUNK is 64 bytes, one SHA-256 block: the chunks of the payload are
 * 64-byte aligned to its start, so each one is exactly one block for the hash and none is
 * copied twice, and a validator with its SHA-256 context and the chunk buffer on the step's
 * stack fits into the RAM of a small bootloader.
 *
 * CONTEXT. No function keeps state of its own; a validator is caller memory and must not be
 * used by two tasks at once. The validation uses the device's read operation only, from the
 * calling task, and takes as long as the device takes to read. Any task; not from an interrupt
 * handler, since a device read may wait.
 */
#ifndef FCN_IMAGE_H
#define FCN_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "flash/flash.h"
#include "sha256/sha256.h"

/** Bytes of the header, and the slot offset of the payload. */
#define IMAGE_HEADER_SIZE    256U
#define IMAGE_PAYLOAD_OFFSET IMAGE_HEADER_SIZE

/** The bytes 'F' 'C' 'N' 'I' read as a little-endian word. */
#define IMAGE_MAGIC 0x494E4346U

/** The header layout this module reads and writes. */
#define IMAGE_HEADER_VERSION 1U

/** Target identifiers. 0 and 0xFFFFFFFF name no target (a zeroed or an erased word). */
#define IMAGE_TARGET_HOST       1U /**< the node on the FreeRTOS POSIX port (Linux, x86-64) */
#define IMAGE_TARGET_MPS2_AN385 2U /**< Cortex-M3 on the Arm MPS2 AN385 (QEMU), phase 7 */

/** The most bytes one validation step reads. */
#define IMAGE_READ_CHUNK 64U

/** Offsets of the header fields (the table above). */
#define IMAGE_OFFSET_MAGIC          0U
#define IMAGE_OFFSET_HEADER_VERSION 4U
#define IMAGE_OFFSET_HEADER_SIZE    6U
#define IMAGE_OFFSET_TARGET         8U
#define IMAGE_OFFSET_IMAGE_VERSION  12U
#define IMAGE_OFFSET_PAYLOAD_SIZE   16U
#define IMAGE_OFFSET_FLAGS          20U
#define IMAGE_OFFSET_PAYLOAD_SHA256 24U
#define IMAGE_OFFSET_UNUSED         56U
#define IMAGE_OFFSET_HEADER_CRC     252U

/** The value of every unused header byte. */
#define IMAGE_UNUSED_BYTE 0xFFU

/** Result of a validation or of a header decode. */
typedef enum {
    IMAGE_OK = 0,           /**< valid: every check passed */
    IMAGE_PENDING,          /**< stepwise validation not finished: call image_validate_step() */
    IMAGE_E_ARG,            /**< a NULL pointer, a slot outside the device or too small, or a
                                 validator that image_validate_begin() did not prepare */
    IMAGE_E_FLASH,          /**< a read of the device failed (image_validator_flash_status()) */
    IMAGE_E_MAGIC,          /**< magic is not IMAGE_MAGIC: no image (erased flash, say) */
    IMAGE_E_HEADER_VERSION, /**< header_version is not IMAGE_HEADER_VERSION */
    IMAGE_E_HEADER_SIZE,    /**< header_size is not IMAGE_HEADER_SIZE */
    IMAGE_E_HEADER_CRC,     /**< header_crc does not match the header bytes */
    IMAGE_E_RESERVED,       /**< flags not 0, or an unused byte not 0xFF */
    IMAGE_E_TARGET,         /**< built for another target */
    IMAGE_E_PAYLOAD_SIZE,   /**< payload_size is 0 or does not fit into the slot */
    IMAGE_E_DIGEST          /**< the payload's SHA-256 is not payload_sha256 */
} image_status_t;

/** The fields of a header, as numbers. */
typedef struct {
    uint32_t magic;
    uint16_t header_version;
    uint16_t header_size;
    uint32_t target;
    uint32_t image_version;
    uint32_t payload_size;
    uint32_t flags;
    uint8_t payload_sha256[SHA256_DIGEST_SIZE];
    uint32_t header_crc; /**< as stored in bytes 252..255 */
} image_header_t;

/* Private to image.c; in the header only so that a validator can be allocated statically.
 *
 * The header as far as it has been read: the fields put together from the bytes so far, the
 * CRC register over bytes 0..251 so far, and whether every unused byte so far was 0xFF. */
typedef struct {
    image_header_t header;
    uint32_t crc;
    uint32_t offset; /* header bytes taken so far, 0..IMAGE_HEADER_SIZE */
    bool unused_erased;
} image_header_reader_t;

/* Private to image.c, see above. One validation of one slot. */
typedef struct {
    const flash_dev_t *dev; /* NULL: not prepared, or refused by image_validate_begin() */
    uint32_t slot_addr;
    uint32_t slot_size;
    uint32_t target;
    uint32_t offset; /* slot bytes read so far: header, then payload */
    image_status_t status;
    flash_status_t flash_status;
    image_header_reader_t reader;
    sha256_ctx_t sha;
} image_validator_t;

/**
 * The image version major.minor.patch (see VERSION). Any context.
 */
uint32_t image_version_make(uint8_t major, uint8_t minor, uint16_t patch);

/** The parts of an image version (see VERSION). Any context. */
uint8_t image_version_major(uint32_t version);
uint8_t image_version_minor(uint32_t version);
uint16_t image_version_patch(uint32_t version);

/**
 * Fill `header` for a payload of `payload_size` bytes whose SHA-256 is `payload_sha256`:
 * magic, header version and header size of this format, flags 0, header_crc 0 (the CRC is
 * computed by image_header_encode()). Nothing is checked against a slot; a payload_size of 0 is
 * written as given and refused by the validation. Returns false, changing nothing, if a
 * pointer is NULL. Any context.
 */
bool image_header_init(image_header_t *header, uint32_t target, uint32_t image_version,
                       uint32_t payload_size, const uint8_t payload_sha256[SHA256_DIGEST_SIZE]);

/**
 * Write the IMAGE_HEADER_SIZE bytes of `header` to `bytes`: every field as it stands in
 * `header` (magic, header_version and header_size included, so that a test can encode a
 * header of another format), the unused bytes 0xFF, and in bytes 252..255 the CRC-32 of bytes
 * 0..251, computed here: header->header_crc is not read. Returns false, writing nothing, if a
 * pointer is NULL. Any context.
 */
bool image_header_encode(const image_header_t *header, uint8_t bytes[IMAGE_HEADER_SIZE]);

/**
 * Read the IMAGE_HEADER_SIZE bytes at `bytes` into `header`: every field as stored,
 * header_crc included, whatever the result. Returns the first of the checks 3 to 7 of
 * VALIDATION that fails, or IMAGE_OK; the target and the payload size are not checked, since
 * they depend on the build and the slot. Returns IMAGE_E_ARG, writing nothing, if a pointer is
 * NULL. Any context.
 */
image_status_t image_header_decode(const uint8_t bytes[IMAGE_HEADER_SIZE], image_header_t *header);

/**
 * Prepare `validator` to validate the slot of `slot_size` bytes at device address `slot_addr`
 * for `target`. Reads nothing. The memory of `validator` may hold anything before, and the
 * device must stay valid until the validation has ended.
 *
 * Returns IMAGE_PENDING when the validator is ready for image_validate_step(); IMAGE_E_ARG if
 * `validator` is NULL, or if `dev` or its read operation is NULL, the slot does not lie inside
 * the device or slot_size is not above IMAGE_HEADER_SIZE (check 1 of VALIDATION). In the
 * second case every image_validate_step() then returns IMAGE_E_ARG. Any task.
 */
image_status_t image_validate_begin(image_validator_t *validator, const flash_dev_t *dev,
                                    uint32_t slot_addr, uint32_t slot_size, uint32_t target);

/**
 * One step of the validation (see STEPS): read the next chunk of the slot and take it in.
 * Returns IMAGE_PENDING while there is more to do, then the result of the validation (see
 * VALIDATION). Once the result is known, further calls return it again without reading.
 * Returns IMAGE_E_ARG if `validator` is NULL or was not prepared by image_validate_begin() (a
 * zeroed validator is refused). The task that called image_validate_begin().
 */
image_status_t image_validate_step(image_validator_t *validator);

/**
 * The header as read by a validation, once all of it has been read: copies it to `header` and
 * returns true. The fields are as stored whatever the result, so that a caller can report what
 * a refused header says; they describe an image only if the result is IMAGE_OK. Returns false,
 * leaving `header` alone, if a pointer is NULL or the header has not been read completely
 * (yet). Any task that owns the validator.
 */
bool image_validator_header(const image_validator_t *validator, image_header_t *header);

/**
 * The status of the read that ended the validation with IMAGE_E_FLASH; FLASH_OK in every other
 * case, and if `validator` is NULL. Any task that owns the validator.
 */
flash_status_t image_validator_flash_status(const image_validator_t *validator);

/**
 * The whole validation in one call: image_validate_begin() and image_validate_step() until the
 * result is known, on a validator on the stack. When the result is IMAGE_OK and `header` is
 * not NULL, the header is copied to `header`; otherwise `header` is left alone. Any task (the
 * call takes as long as reading header and payload; see STEPS for a caller that must keep its
 * timing).
 */
image_status_t image_validate(const flash_dev_t *dev, uint32_t slot_addr, uint32_t slot_size,
                              uint32_t target, image_header_t *header);

/**
 * A name for `status`, for logs and tools: "ok", "pending", "bad_argument", "flash_error",
 * "bad_magic", "bad_header_version", "bad_header_size", "bad_header_crc", "bad_reserved",
 * "bad_target", "bad_payload_size", "bad_digest"; "unknown" for a value outside the
 * enumeration. tools/fcn_tools/image.py uses the same names. Any context.
 */
const char *image_status_name(image_status_t status);

#endif /* FCN_IMAGE_H */
