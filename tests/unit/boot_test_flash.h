/* SPDX-License-Identifier: MIT */
/*
 * The flash model the boot control tests run on, behind three functions, so that the same tests
 * run on any model of flash/flash.h with a power cut (decisions 1 and 2 of phase 4).
 *
 * boot_test_nor.c is this project's own small model, written for these tests independently of
 * lib/flash. A file that implements the same three functions over lib/flash's model lets the
 * tests run on that one too: list it in place of boot_test_nor.c in tests/unit/CMakeLists.txt.
 *
 * What the tests rely on (and test_boot.c checks, through these functions only):
 *   - the device of decision 1 over the caller's memory: erased bytes read 0xFF, strict
 *     write-once per unit, program within a page, erase of a whole sector, nothing changed by a
 *     refused operation, and the memory holding exactly the device's content at all times (the
 *     tests read it, and write it to stage a factory image or to damage a slot);
 *   - arm_next(position, seed): the NEXT program or erase is cut. A torn program leaves the
 *     units before unit `position` of the call written, unit `position` with a seed-chosen
 *     subset of its 1-to-0 transitions done, and the rest untouched; a torn erase leaves a
 *     seed-chosen subset of the sector's 0 bits set to 1 (`position` is then the moment of the
 *     cut for a model that has one, as lib/flash's sixteenths of the erase; the campaign spreads
 *     it over 0..15 with the seed, and boot_test_nor.c ignores it). The torn
 *     operation returns FLASH_E_POWER_LOST, and so does every later operation (reads included)
 *     until power_on(). The same seed gives the same outcome. Reads are never cut: the tests
 *     arm a cut immediately before the operation they mean, so a model that counts reads as
 *     operations arms "the next operation" just the same;
 *   - power_on(): the supply is back, no cut is armed, the memory is unchanged.
 */
#ifndef FCN_BOOT_TEST_FLASH_H
#define FCN_BOOT_TEST_FLASH_H

#include <stdint.h>

#include "flash/flash.h"

typedef struct {
    flash_dev_t dev; /* the model's device over the memory given to boot_test_flash_init() */
    void *model;     /* passed to the two functions below */
    void (*arm_next)(void *model, uint32_t position, uint32_t seed);
    void (*power_on)(void *model);
} boot_test_flash_t;

/* A model of `geometry` over `mem` (geometry->size bytes, filled by the caller), powered and
 * with no cut armed. A second call replaces the first model (one model at a time). */
void boot_test_flash_init(boot_test_flash_t *flash, uint8_t *mem, const flash_geometry_t *geometry);

/* The name of the model, printed by the tests. */
const char *boot_test_flash_name(void);

#endif /* FCN_BOOT_TEST_FLASH_H */
