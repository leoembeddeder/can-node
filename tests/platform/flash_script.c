/* SPDX-License-Identifier: MIT */
/* The shared sequence of flash operations; see flash_script.h. */
#include "flash_script.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "flash/nor.h"

typedef enum { STEP_ERASE, STEP_PROGRAM, STEP_READ } step_kind_t;

typedef struct {
    step_kind_t kind;
    uint32_t addr;
    uint32_t len; /* program, read */
} step_t;

/* Metadata at 0x0000 and 0x1000, slot A at 0x2000, slot B at 0x102000. */
static const step_t STEPS[] = {
    {STEP_ERASE, 0x000000U, 0U},     /* write 1 */
    {STEP_PROGRAM, 0x000000U, 8U},   /* write 2: one unit */
    {STEP_PROGRAM, 0x000008U, 8U},   /* write 3: the next unit */
    {STEP_READ, 0x000000U, 16U},     /* a read, not counted */
    {STEP_ERASE, 0x002000U, 0U},     /* write 4 */
    {STEP_PROGRAM, 0x002000U, 256U}, /* write 5: a whole page */
    {STEP_PROGRAM, 0x002120U, 64U},  /* write 6: units within a page */
    {STEP_READ, 0x002000U, 512U},    /* a read across a page */
    {STEP_ERASE, 0x102000U, 0U},     /* write 7 */
    {STEP_PROGRAM, 0x102F00U, 256U}, /* write 8: the last page of the sector */
    {STEP_ERASE, 0x001000U, 0U},     /* write 9 */
    {STEP_PROGRAM, 0x001FF0U, 16U},  /* write 10: the last two units of the metadata */
    {STEP_READ, 0x001F00U, 256U},    /* a read */
};

#define STEP_COUNT (sizeof(STEPS) / sizeof(STEPS[0]))

/* Data of a program: different for every address, with 0 and 1 bits in every unit. */
static void data_for(uint32_t addr, uint8_t *out, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++) {
        out[i] = (uint8_t)((((addr + i) * 29U) ^ ((addr + i) >> 8U) ^ 0x5AU) & 0xFFU);
    }
}

uint32_t flash_script_run(const flash_dev_t *dev, flash_status_t *status_out)
{
    uint8_t buffer[512];
    flash_status_t status = FLASH_OK;
    uint32_t writes = 0U;

    for (size_t i = 0U; (i < STEP_COUNT) && (status == FLASH_OK); i++) {
        const step_t *step = &STEPS[i];

        switch (step->kind) {
        case STEP_ERASE:
            status = dev->erase(dev->ctx, step->addr);
            break;
        case STEP_PROGRAM:
            data_for(step->addr, buffer, step->len);
            status = dev->program(dev->ctx, step->addr, buffer, step->len);
            break;
        case STEP_READ:
        default:
            status = dev->read(dev->ctx, step->addr, buffer, step->len);
            break;
        }

        if ((status == FLASH_OK) && (step->kind != STEP_READ)) {
            writes++;
        }
    }

    if (status_out != NULL) {
        *status_out = status;
    }

    return writes;
}

/* The replay's wear counts, which nor_init() zeroes and nobody reads. */
static uint32_t s_erase_counts[FLASH_SCRIPT_DEVICE_SIZE / 4096U];

void flash_script_replay(const uint8_t *start, uint8_t *result, uint32_t cut, uint32_t seed)
{
    const nor_config_t config = {
        .geometry = {.size = FLASH_SCRIPT_DEVICE_SIZE,
                     .sector_size = 4096U,
                     .page_size = 256U,
                     .unit_size = 8U},
        .timing = {.page_program_ns = 0U, .sector_erase_ns = 0U},
        .memory = result,
        .erase_counts = s_erase_counts,
    };
    nor_t nor;
    flash_dev_t dev;

    (void)memcpy(result, start, FLASH_SCRIPT_DEVICE_SIZE);

    if (nor_init(&nor, &config)) {
        nor_device(&nor, &dev);

        if (cut != 0U) {
            nor_arm_cut(&nor, cut, NOR_CUT_SEEDED_POSITION, seed);
        }

        (void)flash_script_run(&dev, NULL);
    }
}
