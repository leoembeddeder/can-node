/* SPDX-License-Identifier: MIT */
/*
 * Private to lib/boot: what boot_writer.c uses from boot.c. Not part of the interface; nothing
 * outside lib/boot includes this file.
 */
#ifndef FCN_BOOT_INTERNAL_H
#define FCN_BOOT_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "boot/boot.h"
#include "flash/flash.h"

/* Whether `b` went through a successful boot_init() (and is not NULL). */
bool boot_ready(const boot_t *b);

/* Whether the configuration has a validator. */
bool boot_has_validator(const boot_t *b);

/* A flash status as a boot status: OK, POWER_LOST, every other error BOOT_E_FLASH. */
boot_status_t boot_from_flash(flash_status_t status);

/* Read the whole metadata log into b's scan fields (newest record, attempts, where the next
 * record goes). BOOT_OK, or the read error. */
boot_status_t boot_scan(boot_t *b);

/* Program `len` bytes at `addr`, one program operation per page the range touches. `addr` and
 * `len` are whole units. */
boot_status_t boot_program(const boot_t *b, uint32_t addr, const uint8_t *src, uint32_t len);

/* Write `record` (its seq is ignored: the next one is used) as the next entry of the log and
 * read the log back. BOOT_OK only if the new record is then the newest. Uses the scan fields,
 * so a scan must have been made since the last flash change. */
boot_status_t boot_write_record(boot_t *b, const boot_record_t *record);

/* Set slot `slot` of `record` to `state` with no image: version 0xFFFFFFFF, id 0xFF. */
void boot_record_clear_slot(boot_record_t *record, uint8_t slot, boot_slot_state_t state);

#endif /* FCN_BOOT_INTERNAL_H */
