/* SPDX-License-Identifier: MIT */
/**
 * @file
 * A fixed sequence of flash operations on the host's layout (plat_flash.h), shared by the tests
 * of the host's flash (test_posix_flash.c without a kernel, flash_selftest.c under FreeRTOS).
 * The test runs it on the device under test, and on lib/flash's model over a copy of the
 * device's earlier content (flash_script_replay()), and compares the two byte for byte.
 *
 * The script erases each sector before it programs it, so it can run again on its own result:
 * a run on that result tears sectors that hold data, which is where a cut shows. It uses every
 * area of the layout and every length the update code uses (one unit, units within a page, a
 * whole page), with reads in between, which a cut does not count.
 */
#ifndef FCN_TEST_FLASH_SCRIPT_H
#define FCN_TEST_FLASH_SCRIPT_H

#include <stdint.h>

#include "flash/flash.h"

/** Programs and erases in the script: a cut can fall on the 1st to the last of them. */
#define FLASH_SCRIPT_WRITES 10U

/** The size of the host's device, the size of a buffer for flash_script_replay(). */
#define FLASH_SCRIPT_DEVICE_SIZE 0x202000U

/**
 * Run the script on `dev`. Returns the programs and erases that returned FLASH_OK, all of
 * them (FLASH_SCRIPT_WRITES) unless one failed: then it stops there and *status_out (if not
 * NULL) is that call's result; otherwise FLASH_OK. A failing read stops it as well.
 */
uint32_t flash_script_run(const flash_dev_t *dev, flash_status_t *status_out);

/**
 * What the device holds after the script ran on content `start` (FLASH_SCRIPT_DEVICE_SIZE
 * bytes) with a cut of its `cut`-th program or erase (0: no cut) torn with `seed`, as the
 * host's flash arms it (NOR_CUT_SEEDED_POSITION): the script on lib/flash's model over
 * `result`.
 */
void flash_script_replay(const uint8_t *start, uint8_t *result, uint32_t cut, uint32_t seed);

#endif /* FCN_TEST_FLASH_SCRIPT_H */
