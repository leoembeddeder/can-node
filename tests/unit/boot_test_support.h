/* SPDX-License-Identifier: MIT */
/*
 * What test_boot.c and test_boot_cuts.c share:
 *
 *   - THE BENCH: one flash device (the model of boot_test_flash.h over bt_mem) in a layout of
 *     metadata sectors followed by slot A and slot B, and a boot_t on it, as after a reset;
 *   - THE TRACER: a flash_dev_t between lib/boot and the model. It counts the operations that
 *     reach the device, logs programs and erases, cuts the supply just before a chosen program
 *     or erase or lets the model tear it, and can make one operation fail or be dropped;
 *   - TEST IMAGES and a stepwise validator for them, plus an independent check of a slot read
 *     straight from memory (the truth the reference model is given);
 *   - THE REFERENCE MODEL of boot control, written from the state machine in lib/boot/boot.h and
 *     docs/update.md, not from boot.c.
 *
 * Programs and erases are numbered from 0 since the last bt_counts_reset(); that number is what
 * a cut names.
 */
#ifndef FCN_BOOT_TEST_SUPPORT_H
#define FCN_BOOT_TEST_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot/boot.h"
#include "flash/flash.h"

/* ------------------------------------------------------------------ the bench */

/* Room for the host layout: 2 metadata sectors and two 1 MiB slots of 4 KiB sectors. */
#define BT_MEM_MAX ((2U * 1024U * 1024U) + (64U * 1024U))

/* Device memory: exactly what the device holds. */
extern uint8_t bt_mem[BT_MEM_MAX];

/* A layout: `meta_sectors` metadata sectors at 0, then slot A and slot B of `slot_sectors`
 * sectors each, then `spare_sectors` more (outside every region). */
typedef struct {
    uint32_t sector_size;
    uint32_t page_size;
    uint32_t unit_size;
    uint32_t meta_sectors;
    uint32_t slot_sectors;
    uint32_t spare_sectors;
} bt_shape_t;

/* The host geometry of phase 4 with small slots (three sectors each). */
extern const bt_shape_t bt_shape_host_small;

/* The configuration and the boot_t of the bench. */
extern boot_config_t bt_config;
extern boot_t bt_boot;

/* Erase all of bt_mem's device (0xFF), set up the model, the tracer and bt_config for `shape`
 * with the test validator, and initialise bt_boot. Fails the test if boot_init() refuses. */
void bt_setup(const bt_shape_t *shape);

/* bt_boot as after a reset: boot_init() again on the same flash. */
void bt_reboot(void);

/* The device size of the bench. */
uint32_t bt_device_size(void);

/* ------------------------------------------------------------------ the tracer */

typedef struct {
    uint32_t reads;
    uint32_t programs;
    uint32_t erases;
    uint32_t units; /* units programmed */
    uint32_t ops;   /* programs + erases: the number the next one gets */
} bt_counts_t;

typedef struct {
    bool erase;
    uint32_t addr;
    uint32_t len;
} bt_op_t;

void bt_counts(bt_counts_t *counts);
void bt_counts_reset(void);

/* Log every program and erase (from now, up to `cap`) into `log`; NULL stops logging. */
void bt_trace(bt_op_t *log, uint32_t cap);
uint32_t bt_trace_len(void);

/* Cut the supply just before program or erase number `op`: it does not reach the device and
 * returns FLASH_E_POWER_LOST, as does every later operation until bt_power_on(). */
void bt_cut_before(uint32_t op);

/* Let the model tear program or erase number `op` at `position` with `seed`. */
void bt_cut_tear(uint32_t op, uint32_t position, uint32_t seed);

/* Whether the armed cut has happened. */
bool bt_cut_fired(void);

/* The supply is back: the model powered, no cut armed, nothing failing. */
void bt_power_on(void);

/* The supply is gone from now on: every operation returns FLASH_E_POWER_LOST. */
void bt_power_off(void);

/* The next program or erase returns `status` without reaching the device; with `status`
 * FLASH_OK it is dropped: it returns FLASH_OK and changes nothing (a device that lies). Each
 * call adds one more failing operation, all with the status of the last call. */
void bt_fail_next(flash_status_t status);

/* Every read returns `status` until bt_power_on() or bt_fail_reads(FLASH_OK). */
void bt_fail_reads(flash_status_t status);

/* ------------------------------------------------------------------ test images */

#define BT_IMAGE_HEADER 32U
#define BT_IMAGE_MAGIC  0x474D4954U /* "TIMG" */

/* Build a test image into `out`: a 32-byte header (magic, version, payload size, CRC-32 of the
 * payload, seed, filler, CRC-32 of the header) and `payload` bytes drawn from `seed`. Returns
 * its size. Its id is its header. */
uint32_t bt_image_build(uint8_t *out, uint32_t cap, uint32_t version, uint32_t seed,
                        uint32_t payload);

/* The truth: whether [addr, addr + len) of bt_mem holds a valid test image, read straight from
 * memory. Fills id (BOOT_IMAGE_ID_SIZE bytes) and version if it does and they are not NULL. */
bool bt_image_check_mem(uint32_t addr, uint32_t len, uint8_t *id, uint32_t *version);

/* The stepwise validator of test images that bt_config uses: one header step, then `chunk`
 * payload bytes per step. Counters and faults for the tests. */
typedef struct {
    uint32_t chunk;         /* payload bytes per step, 1..256 */
    uint32_t starts;        /* start() calls */
    uint32_t steps;         /* step() calls */
    bool fail_reads;        /* step() returns BOOT_CHECK_ERROR */
    uint32_t fail_at_start; /* from the check that start() call number fail_at_start begins (1:
                               the first since `starts` was last set to 0) on, step() returns
                               BOOT_CHECK_ERROR; 0: never */
    bool stall;             /* step() returns BOOT_CHECK_BUSY forever */
    bool bogus;             /* step() returns a value outside boot_check_t */
} bt_checker_control_t;

extern bt_checker_control_t bt_checker;

/* ------------------------------------------------------------------ helpers */

/* The writer of the last bt_update(). */
extern boot_writer_t bt_writer;

/* A whole update of `slot` with `image` through the slot writer: begin, every erase step, the
 * image in chunks (sizes from a pattern rotated by `chunking`), every finish step. Returns
 * BOOT_OK or the first other result that is not BOOT_BUSY. */
boot_status_t bt_update(boot_t *b, uint8_t slot, const uint8_t *image, uint32_t len,
                        uint32_t chunking);

/* Bench writes into device memory, past the device's rules: a factory programmer, or damage. */
void bt_raw_write(uint32_t addr, const uint8_t *data, uint32_t len);
void bt_flip_bit(uint32_t addr, uint32_t bit);

/* The slot regions of the bench. */
uint32_t bt_slot_addr(uint8_t slot);
uint32_t bt_slot_size(uint8_t slot);

/* Where entry `index` of metadata sector `sector` is, from bt_boot's description. */
uint32_t bt_entry_addr(uint32_t sector, uint32_t index);

/* ------------------------------------------------------------------ the reference model */

typedef struct {
    bool have; /* a record exists (false: the factory state) */
    boot_slot_state_t state[BOOT_SLOTS];
    uint8_t id[BOOT_SLOTS][BOOT_IMAGE_ID_SIZE]; /* 0xFF for EMPTY and WRITING */
    uint8_t attempts;                           /* of the TRIAL; 0 without one */
} bt_ref_t;

typedef struct {
    boot_status_t status; /* BOOT_OK or BOOT_E_NOTHING_BOOTABLE */
    uint8_t slot;
    bool trial;
    uint8_t attempt;
    boot_event_t event;
    uint8_t event_slot;
} bt_ref_choice_t;

/* What each slot of bt_mem holds now: valid[s] and, if valid, its id. */
typedef struct {
    bool valid[BOOT_SLOTS];
    uint8_t id[BOOT_SLOTS][BOOT_IMAGE_ID_SIZE];
} bt_truth_t;

void bt_truth(bt_truth_t *truth);

void bt_ref_factory(bt_ref_t *r);
void bt_ref_select(bt_ref_t *r, const bt_truth_t *truth, bt_ref_choice_t *choice);
boot_status_t bt_ref_begin(bt_ref_t *r, uint8_t slot, uint32_t size, uint32_t slot_size);
void bt_ref_commit(bt_ref_t *r, uint8_t slot, const uint8_t *id);
boot_status_t bt_ref_confirm(bt_ref_t *r, uint8_t slot);

/* The abstract state of a description, and of bt_boot as it reads the flash now. */
void bt_ref_of(const boot_description_t *d, bt_ref_t *r);
void bt_ref_now(bt_ref_t *r);
bool bt_ref_equal(const bt_ref_t *a, const bt_ref_t *b);

/* "A:confirmed(1a2b) B:trial(3c4d) k=1" into `buf`, for failure messages. */
const char *bt_ref_text(const bt_ref_t *r, char *buf, size_t len);

/* The CONFIRMED slot of a model state with a record. */
uint8_t bt_ref_confirmed(const bt_ref_t *r);

#endif /* FCN_BOOT_TEST_SUPPORT_H */
