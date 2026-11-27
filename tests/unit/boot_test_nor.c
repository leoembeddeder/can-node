/* SPDX-License-Identifier: MIT */
/*
 * A small NOR flash model for the boot control tests (boot_test_flash.h), written for them and
 * independently of lib/flash, from decisions 1 and 2 of phase 4:
 *
 *   - the device of flash/flash.h over the caller's memory, with the checks in the order the
 *     header gives (supply, range, alignment, erased state) and strict write-once per unit;
 *   - a cut armed for the next program or erase. A torn program writes the units before the
 *     cut position completely, does a seeded subset of the 1-to-0 transitions of the unit at the
 *     position, and leaves the rest untouched; a torn erase sets a seeded subset of the
 *     sector's 0 bits to 1. The fraction is (seed mod 9) / 8: seed 0 (mod 9) does none of the
 *     work, seed 8 (mod 9) all of it, the seeds in between each bit with that probability,
 *     drawn from xorshift32 seeded from the seed (no rand()). A torn bit is a stable 0 or 1.
 *   - after a cut, every operation returns FLASH_E_POWER_LOST until power_on().
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "boot_test_flash.h"

typedef struct {
    uint8_t *mem;
    flash_geometry_t geometry;
    bool powered;
    bool armed;
    uint32_t position;
    uint32_t random;   /* xorshift32 state of the cut being made */
    uint32_t fraction; /* eighths of the transitions a cut does: 0..8 */
} nor_t;

static nor_t s_nor;

static uint32_t next_random(nor_t *nor)
{
    nor->random ^= nor->random << 13U;
    nor->random ^= nor->random >> 17U;
    nor->random ^= nor->random << 5U;
    return nor->random;
}

/* Whether the next transition of a cut is done. */
static bool transition_done(nor_t *nor)
{
    return (next_random(nor) % 8U) < nor->fraction;
}

static bool in_range(const nor_t *nor, uint32_t addr, uint32_t len)
{
    return (len != 0U) && (addr < nor->geometry.size) && (len <= (nor->geometry.size - addr));
}

static flash_status_t nor_read(void *ctx, uint32_t addr, uint8_t *dst, uint32_t len)
{
    const nor_t *nor = ctx;
    flash_status_t status = FLASH_OK;

    if (!nor->powered) {
        status = FLASH_E_POWER_LOST;
    } else if (!in_range(nor, addr, len)) {
        status = FLASH_E_RANGE;
    } else {
        (void)memcpy(dst, &nor->mem[addr], len);
    }

    return status;
}

/* The unit at `addr` of a program from `src`, torn: a subset of its 1-to-0 transitions. */
static void tear_unit(nor_t *nor, uint32_t addr, const uint8_t *src)
{
    for (uint32_t i = 0U; i < nor->geometry.unit_size; i++) {
        uint8_t byte = nor->mem[addr + i];

        for (uint32_t bit = 0U; bit < 8U; bit++) {
            const uint8_t mask = (uint8_t)(1U << bit);

            if (((src[i] & mask) == 0U) && ((byte & mask) != 0U) && transition_done(nor)) {
                byte = (uint8_t)(byte & (uint8_t)~mask);
            }
        }
        nor->mem[addr + i] = byte;
    }
}

static flash_status_t nor_program(void *ctx, uint32_t addr, const uint8_t *src, uint32_t len)
{
    nor_t *nor = ctx;
    const uint32_t unit = nor->geometry.unit_size;
    const uint32_t page = nor->geometry.page_size;
    flash_status_t status = FLASH_OK;

    if (!nor->powered) {
        status = FLASH_E_POWER_LOST;
    } else if (!in_range(nor, addr, len)) {
        status = FLASH_E_RANGE;
    } else if (((addr % unit) != 0U) || ((len % unit) != 0U) ||
               ((addr / page) != ((addr + len - 1U) / page))) {
        status = FLASH_E_ALIGN;
    } else {
        for (uint32_t i = 0U; i < len; i++) {
            if (nor->mem[addr + i] != 0xFFU) {
                status = FLASH_E_NOT_ERASED; /* nothing has been changed yet */
            }
        }
    }

    if ((status == FLASH_OK) && nor->armed) {
        const uint32_t units = len / unit;
        const uint32_t cut = (nor->position < units) ? nor->position : (units - 1U);
        const uint32_t before = cut * unit; /* bytes of the units before the cut */

        for (uint32_t i = 0U; i < before; i++) {
            nor->mem[addr + i] = (uint8_t)(nor->mem[addr + i] & src[i]);
        }
        tear_unit(nor, addr + before, &src[before]);
        nor->armed = false;
        nor->powered = false;
        status = FLASH_E_POWER_LOST;
    } else if (status == FLASH_OK) {
        for (uint32_t i = 0U; i < len; i++) {
            nor->mem[addr + i] = (uint8_t)(nor->mem[addr + i] & src[i]);
        }
    } else {
        /* refused: nothing changed */
    }

    return status;
}

static flash_status_t nor_erase(void *ctx, uint32_t addr)
{
    nor_t *nor = ctx;
    const uint32_t sector = nor->geometry.sector_size;
    flash_status_t status = FLASH_OK;

    if (!nor->powered) {
        status = FLASH_E_POWER_LOST;
    } else if (addr >= nor->geometry.size) {
        status = FLASH_E_RANGE;
    } else if ((addr % sector) != 0U) {
        status = FLASH_E_ALIGN;
    } else if (nor->armed) {
        for (uint32_t i = 0U; i < sector; i++) {
            uint8_t byte = nor->mem[addr + i];

            for (uint32_t bit = 0U; bit < 8U; bit++) {
                const uint8_t mask = (uint8_t)(1U << bit);

                if (((byte & mask) == 0U) && transition_done(nor)) {
                    byte = (uint8_t)(byte | mask);
                }
            }
            nor->mem[addr + i] = byte;
        }
        nor->armed = false;
        nor->powered = false;
        status = FLASH_E_POWER_LOST;
    } else {
        (void)memset(&nor->mem[addr], 0xFF, sector);
    }

    return status;
}

static void nor_arm_next(void *model, uint32_t position, uint32_t seed)
{
    nor_t *nor = model;

    nor->armed = true;
    nor->position = position;
    nor->fraction = seed % 9U;
    nor->random = (seed * 2654435761U) ^ 0x9E3779B9U;
    if (nor->random == 0U) {
        nor->random = 1U;
    }
}

static void nor_power_on(void *model)
{
    nor_t *nor = model;

    nor->powered = true;
    nor->armed = false;
}

void boot_test_flash_init(boot_test_flash_t *flash, uint8_t *mem, const flash_geometry_t *geometry)
{
    (void)memset(&s_nor, 0, sizeof(s_nor));
    s_nor.mem = mem;
    s_nor.geometry = *geometry;
    s_nor.powered = true;

    flash->dev.geometry = *geometry;
    flash->dev.ctx = &s_nor;
    flash->dev.read = nor_read;
    flash->dev.program = nor_program;
    flash->dev.erase = nor_erase;
    flash->model = &s_nor;
    flash->arm_next = nor_arm_next;
    flash->power_on = nor_power_on;
}

const char *boot_test_flash_name(void)
{
    return "tests/unit/boot_test_nor.c (the boot tests' own NOR model)";
}
