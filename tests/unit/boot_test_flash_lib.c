/* SPDX-License-Identifier: MIT */
/*
 * The boot control tests on lib/flash's NOR model (flash/nor.h) instead of their own
 * (boot_test_nor.c): the three functions of boot_test_flash.h over nor_t. To use it, list this
 * file in place of boot_test_nor.c in tests/unit/CMakeLists.txt and link fcn_flash.
 *
 * arm_next(position, seed) arms a cut of the next program or erase (nor_arm_cut with k = 1);
 * power_on() disarms any cut and powers the model.
 */
#include <stdint.h>
#include <string.h>

#include "boot_test_flash.h"
#include "flash/nor.h"
#include "unity.h"

static nor_t s_nor;
/* One count per sector of the largest bench (boot_test_support.h, BT_MEM_MAX) in sectors of
 * 512 bytes, the smallest sector the tests use. */
static uint32_t s_erase_counts[((2U * 1024U * 1024U) + (64U * 1024U)) / 512U];

static void lib_arm_next(void *model, uint32_t position, uint32_t seed)
{
    nor_arm_cut(model, 1U, position, seed);
}

static void lib_power_on(void *model)
{
    nor_disarm_cut(model);
    nor_power_on(model);
}

void boot_test_flash_init(boot_test_flash_t *flash, uint8_t *mem, const flash_geometry_t *geometry)
{
    nor_config_t config;

    TEST_ASSERT_TRUE_MESSAGE((geometry->size / geometry->sector_size) <=
                                 (uint32_t)(sizeof(s_erase_counts) / sizeof(s_erase_counts[0])),
                             "more sectors than erase counters");
    (void)memset(&config, 0, sizeof(config));
    config.geometry = *geometry;
    config.memory = mem;
    config.erase_counts = s_erase_counts;
    TEST_ASSERT_TRUE_MESSAGE(nor_init(&s_nor, &config), "nor_init refused the bench geometry");
    nor_device(&s_nor, &flash->dev);
    flash->model = &s_nor;
    flash->arm_next = lib_arm_next;
    flash->power_on = lib_power_on;
}

const char *boot_test_flash_name(void)
{
    return "lib/flash/nor.c (lib/flash's NOR model)";
}
