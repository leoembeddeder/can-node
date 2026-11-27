/* SPDX-License-Identifier: MIT */
/**
 * @file
 * What the host's bootloader (fcn_boot, boot_main.c) and flash tool (fcn_flash, flash_tool.c)
 * share: lib/boot's configuration from the platform's flash layout, the hand-off from a
 * selection, the text of an image version, and a log that writes lib/log's lines straight to a
 * file. Neither program links the FreeRTOS kernel: this file and what it uses (posix_flash.c,
 * posix_retained.c, lib/) need none.
 *
 * Main thread of a program without a kernel; nothing here is thread-safe.
 */
#ifndef FCN_BOOT_HOST_H
#define FCN_BOOT_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot/boot.h"
#include "bootimg/bootimg.h"
#include "log/log.h"
#include "plat_flash.h"
#include "plat_retained.h"

/** fcn_boot's own exit statuses, beside 1 (a usage or start-up error) and 12
 *  (PLAT_RESET_POWER_LOSS: a power cut armed with --flash-cut fired in the bootloader). None
 *  of them is a status of the node (0, 1, 10 to 22, 70, 74): the test harness tells from the
 *  status alone whether the bootloader or the image ended the run. */
#define FCN_BOOT_EXIT_NOTHING_BOOTABLE 40 /**< boot_select(): no slot may be started */
#define FCN_BOOT_EXIT_FLASH            41 /**< the flash could not be read or written */
#define FCN_BOOT_EXIT_JUMP             42 /**< the image could not be started (memfd, exec) */

/** Room for the text of an image version: "255.255.65535" and the terminating zero. */
#define BOOT_HOST_VERSION_TEXT 14U

/**
 * lib/boot's configuration for this platform: the device plat_flash_device() (NULL if the run
 * has no flash, which boot_init() refuses), the metadata area and the two slots of
 * plat_flash_layout(), and `validator` (NULL: none, for a caller that only describes or
 * confirms). Fills `out` completely.
 */
void boot_host_config(boot_config_t *out, const boot_validator_t *validator);

/** The hand-off (plat_retained.h, THE HAND-OFF) that tells the image what `sel`, the result of
 *  a boot_select() that returned BOOT_OK, decided. */
void boot_host_handoff(const boot_selection_t *sel, plat_boot_handoff_t *out);

/** "major.minor.patch" of an image version (lib/image's encoding) into `text` (at least
 *  BOOT_HOST_VERSION_TEXT bytes); returns `text`. */
const char *boot_host_version_text(uint32_t version, char *text);

/** The name of a slot index: "A", "B", or "none". */
const char *boot_host_slot_name(uint8_t slot);

/**
 * Send lib/log's lines at `level` and above to `path`, appended (created if missing), or to
 * stderr for a NULL `path`, each written with one write() as it is made, stamped with
 * CLOCK_MONOTONIC as the node's lines are. Returns false with a message in `error` if the file
 * cannot be opened. The file is closed on exec (O_CLOEXEC).
 */
bool boot_host_log_open(const char *path, log_level_t level, char *error, size_t error_len);

/** CLOCK_MONOTONIC in nanoseconds. */
uint64_t boot_host_time_ns(void);

#endif /* FCN_BOOT_HOST_H */
