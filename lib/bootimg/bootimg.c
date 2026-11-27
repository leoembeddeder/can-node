/* SPDX-License-Identifier: MIT */
#include "bootimg/bootimg.h"

#include <stddef.h>
#include <string.h>

/* Where a check stands (bootimg_t.phase). */
#define PHASE_IDLE  0U /* start() not called yet: steps answer INVALID */
#define PHASE_IMAGE 1U /* lib/image's steps */
#define PHASE_ID    2U /* hashing the header for the id */
#define PHASE_DONE  3U /* the result is known */

/* The header of the slot as lib/image read it, encoded again: the slot's first 256 bytes for
 * a header that validated (bootimg.h, THE IMAGE ID). */
static void header_bytes(const bootimg_t *v, uint8_t bytes[IMAGE_HEADER_SIZE],
                         image_header_t *header)
{
    (void)image_validator_header(&v->image, header);
    (void)image_header_encode(header, bytes);
}

static void finish(bootimg_t *v, boot_check_t result)
{
    v->result = result;
    v->phase = (uint8_t)PHASE_DONE;
}

/* lib/image's step, and what its result means for lib/boot. */
static boot_check_t step_image(bootimg_t *v)
{
    const image_status_t status = image_validate_step(&v->image);
    boot_check_t result = BOOT_CHECK_BUSY;

    v->status = status;

    if (status == IMAGE_OK) {
        /* The slot is valid; its id follows in BOOTIMG_ID_STEPS more steps. */
        (void)sha256_init(&v->id_hash);
        v->id_chunks = 0U;
        v->phase = (uint8_t)PHASE_ID;
    } else if (status == IMAGE_E_FLASH) {
        result = BOOT_CHECK_ERROR;
        finish(v, result);
    } else if (status != IMAGE_PENDING) {
        result = BOOT_CHECK_INVALID;
        finish(v, result);
    } else {
        /* more chunks to read */
    }

    return result;
}

/* One chunk of the header into the id; the last one finishes it. */
static boot_check_t step_id(bootimg_t *v, boot_image_info_t *info)
{
    uint8_t bytes[IMAGE_HEADER_SIZE];
    image_header_t header;
    boot_check_t result = BOOT_CHECK_BUSY;

    const size_t at = (size_t)v->id_chunks * (size_t)IMAGE_READ_CHUNK;

    header_bytes(v, bytes, &header);
    (void)sha256_update(&v->id_hash, &bytes[at], IMAGE_READ_CHUNK);
    v->id_chunks++;

    if (v->id_chunks == (uint8_t)BOOTIMG_ID_STEPS) {
        (void)sha256_final(&v->id_hash, v->id);
        v->version = header.image_version;
        (void)memcpy(info->id, v->id, sizeof(v->id));
        info->version = v->version;
        result = BOOT_CHECK_VALID;
        finish(v, result);
    }

    return result;
}

static void validator_start(void *ctx, const flash_dev_t *dev, uint32_t addr, uint32_t len)
{
    bootimg_t *const v = (bootimg_t *)ctx;

    v->steps = 0U;
    v->id_chunks = 0U;
    v->status = image_validate_begin(&v->image, dev, addr, len, v->target);
    v->phase = (uint8_t)PHASE_IMAGE;

    if (v->status != IMAGE_PENDING) {
        /* A slot lib/image refuses to look at (outside the device, too small): not bootable. */
        finish(v, BOOT_CHECK_INVALID);
    }
}

static boot_check_t validator_step(void *ctx, boot_image_info_t *info)
{
    bootimg_t *const v = (bootimg_t *)ctx;
    boot_check_t result = BOOT_CHECK_INVALID;

    if (v->phase == (uint8_t)PHASE_IMAGE) {
        v->steps++;
        result = step_image(v);
    } else if (v->phase == (uint8_t)PHASE_ID) {
        v->steps++;
        result = step_id(v, info);
    } else if (v->phase == (uint8_t)PHASE_DONE) {
        /* The result again, without reading; a valid slot's info again with it. */
        result = v->result;

        if (result == BOOT_CHECK_VALID) {
            (void)memcpy(info->id, v->id, sizeof(v->id));
            info->version = v->version;
        }
    } else {
        /* never started */
    }

    return result;
}

bool bootimg_init(bootimg_t *v, uint32_t target, boot_validator_t *out)
{
    const bool ok = (v != NULL) && (out != NULL);

    if (ok) {
        (void)memset(v, 0, sizeof(*v));
        v->target = target;
        v->status = IMAGE_PENDING;
        v->result = BOOT_CHECK_INVALID;
        v->phase = (uint8_t)PHASE_IDLE;
        out->ctx = v;
        out->start = validator_start;
        out->step = validator_step;
    }

    return ok;
}

image_status_t bootimg_status(const bootimg_t *v)
{
    return (v != NULL) ? v->status : IMAGE_E_ARG;
}

bool bootimg_header(const bootimg_t *v, image_header_t *header)
{
    return (v != NULL) && (v->phase != (uint8_t)PHASE_IDLE) &&
           image_validator_header(&v->image, header);
}

uint32_t bootimg_steps(const bootimg_t *v)
{
    return (v != NULL) ? v->steps : 0U;
}

flash_status_t bootimg_flash_status(const bootimg_t *v)
{
    flash_status_t status = FLASH_OK;

    if ((v != NULL) && (v->phase == (uint8_t)PHASE_DONE) && (v->result == BOOT_CHECK_ERROR)) {
        status = image_validator_flash_status(&v->image);
    }

    return status;
}

bool bootimg_image_id(const image_header_t *header, uint8_t id[BOOT_IMAGE_ID_SIZE])
{
    bool ok = (header != NULL) && (id != NULL);

    if (ok) {
        uint8_t bytes[IMAGE_HEADER_SIZE];

        ok = image_header_encode(header, bytes) && sha256_compute(bytes, sizeof(bytes), id);
    }

    return ok;
}
