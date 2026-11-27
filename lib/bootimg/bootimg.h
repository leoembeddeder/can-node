/* SPDX-License-Identifier: MIT */
/**
 * @file
 * lib/image's validation of a slot as the validator of lib/boot (boot_validator_t), so that
 * neither module depends on the other: lib/boot knows nothing about the image format, lib/image
 * nothing about boot control. The bootloader (fcn_boot), the flash tool and, from phase 5, the
 * node's slot writer give lib/boot one of these.
 *
 * THE IMAGE ID. lib/boot binds every state of a slot but EMPTY and WRITING to a 32-byte id
 * that the validator returns, and takes a slot for the image a record names only if the
 * validator accepts the slot and returns the recorded id (boot.h, THE VALIDATOR). The id here
 * is the SHA-256 (FIPS 180-4) of the 256 header bytes of the image (lib/image, the header): the
 * header holds the payload's digest, its size, the image version and the target, and its CRC
 * covers all of them, so two valid images that differ in any byte of header or payload have
 * different ids (but for a collision of SHA-256), and a slot rewritten with the same payload
 * under another version is another image. The header bytes hashed are those of the header
 * lib/image has decoded, encoded again with image_header_encode(): for a header that
 * validated, that is byte for byte what the slot holds (every field, the unused bytes 0xFF,
 * flags 0, the CRC over the rest), so the id is the digest of the slot's first 256 bytes.
 *
 * STEPS. One check is image_validate_begin() at start() and one image_validate_step() per
 * step(); after lib/image accepts the slot, four more steps hash the header, 64 bytes each, the
 * fourth also finishing the hash. So every step reads at most one chunk of IMAGE_READ_CHUNK
 * (64) bytes from the device and makes at most two SHA-256 compressions, as lib/image's own
 * steps do. A slot lib/image accepts takes 8 + ceil(payload_size / 64) steps, the last of them
 * returning BOOT_CHECK_VALID; one whose header it refuses takes 4 (lib/image reads the whole
 * header first), one whose digest it refuses 4 + ceil(payload_size / 64).
 *
 * RESULTS. lib/image's IMAGE_OK is BOOT_CHECK_VALID (the info filled: the id and the header's
 * image version). IMAGE_E_FLASH, a read of the device that failed, is BOOT_CHECK_ERROR, so that
 * lib/boot boots nothing from a device it cannot read (boot_select() returns BOOT_E_FLASH).
 * Every other refusal of lib/image (an erased slot, a damaged header or payload, another
 * target, a payload larger than the slot) is BOOT_CHECK_INVALID. A step after the result returns
 * the result again, without reading. A step of a validator whose start() was never called
 * returns BOOT_CHECK_INVALID.
 *
 * CONTEXT. No globals: the state is the caller's bootimg_t, which must outlive the
 * boot_validator_t it fills in. One check at a time per bootimg_t, from the task that owns the
 * boot_t (lib/boot calls start() and step() itself). No allocation; a step uses about 400 bytes
 * of stack (the header, encoded, and lib/image's chunk).
 */
#ifndef FCN_BOOTIMG_H
#define FCN_BOOTIMG_H

#include <stdbool.h>
#include <stdint.h>

#include "boot/boot.h"
#include "image/image.h"
#include "sha256/sha256.h"

_Static_assert(BOOT_IMAGE_ID_SIZE == SHA256_DIGEST_SIZE, "the id is one SHA-256 digest");

/** Steps after lib/image's that hash the header: IMAGE_HEADER_SIZE in chunks of
 *  IMAGE_READ_CHUNK. */
#define BOOTIMG_ID_STEPS (IMAGE_HEADER_SIZE / IMAGE_READ_CHUNK)

_Static_assert((BOOTIMG_ID_STEPS * IMAGE_READ_CHUNK) == IMAGE_HEADER_SIZE,
               "the header is whole chunks");

/* Private to bootimg.c; in the header so that a validator can be allocated statically. Read
 * it through the functions below. */
typedef struct {
    image_validator_t image;        /* lib/image's validation of the slot */
    sha256_ctx_t id_hash;           /* the id, as far as the header has been hashed */
    uint32_t target;                /* the target lib/image is asked to expect */
    uint32_t steps;                 /* step() calls since start() */
    image_status_t status;          /* lib/image's last result; IMAGE_PENDING before the end */
    boot_check_t result;            /* the result, once it is known */
    uint8_t phase;                  /* see bootimg.c */
    uint8_t id_chunks;              /* header chunks hashed so far */
    uint8_t id[BOOT_IMAGE_ID_SIZE]; /* the id of a slot that validated */
    uint32_t version;               /* and its image version */
} bootimg_t;

/**
 * Prepare `v` to validate slots for images of `target` (lib/image's IMAGE_TARGET_HOST on the
 * host) and fill `out` with the validator lib/boot calls (its ctx is `v`). Returns false,
 * writing nothing, for a NULL pointer. Any context; no flash access.
 */
bool bootimg_init(bootimg_t *v, uint32_t target, boot_validator_t *out);

/** lib/image's result of the last check: IMAGE_PENDING while it runs and before the first,
 *  IMAGE_OK for a slot that validated, otherwise the refusal (image_status_name()); IMAGE_E_ARG
 *  for a NULL `v`. The context that owns `v`. */
image_status_t bootimg_status(const bootimg_t *v);

/** The header of the last check, once lib/image has read all 256 bytes of it, whether the
 *  check then accepted it or not (image_validator_header()); false otherwise or for a NULL
 *  argument. The context that owns `v`. */
bool bootimg_header(const bootimg_t *v, image_header_t *header);

/** step() calls since the last start(); 0 for a NULL `v`. The context that owns `v`. */
uint32_t bootimg_steps(const bootimg_t *v);

/** The device's status of the read that failed when the result was BOOT_CHECK_ERROR
 *  (image_validator_flash_status()), FLASH_OK otherwise or for a NULL `v`. */
flash_status_t bootimg_flash_status(const bootimg_t *v);

/**
 * The id of the image whose header is `header` (THE IMAGE ID), in one call: the SHA-256 of the
 * 256 bytes image_header_encode() makes of it. For tools and tests that compare the id a record
 * holds with an image; the validator computes the same in its last four steps. Returns false,
 * writing nothing, for a NULL argument or a header image_header_encode() refuses. Any context.
 */
bool bootimg_image_id(const image_header_t *header, uint8_t id[BOOT_IMAGE_ID_SIZE]);

#endif /* FCN_BOOTIMG_H */
