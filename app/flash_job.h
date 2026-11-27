/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The flash work of a UDS download and of the confirm, as jobs of the flash task: what one job
 * calls of lib/boot, and the alive indications it gives on the way (app.c, THE FLASH TASK). Pure
 * C over lib/boot: no operating system, no clock; the job's caller says how an alive indication
 * is given.
 *
 * ONE JOB PER STEP OF A SERVICE, not per call of lib/boot (lib/uds's backend, uds_backend_t). A
 * job loops lib/boot's steps itself, so that the diagnostic task posts one job for an erase of
 * any number of sectors and one for the validation of any image, and waits for it while lib/uds
 * answers responsePending:
 *
 *   job       service                          lib/boot                                  done
 *   ERASE     RoutineControl 0xFF00            boot_slot_by_address(), boot_writer_begin(), with the last
 *             eraseMemory                      then boot_writer_erase_step() until it is   erase step
 *                                              not BOOT_BUSY
 *   WRITE     TransferData 0x36                boot_writer_write() of the block            at once
 *   EXIT      RequestTransferExit 0x37         the first boot_writer_finish_step(): the    at once
 *                                              last partial page and the start of the
 *                                              validation (BOOT_BUSY when it succeeds)
 *   CHECK     RoutineControl 0xFF01            boot_writer_finish_step() until it is not   with the
 *             checkProgrammingDependencies     BOOT_BUSY: the validation, one step of 64    COMMIT
 *                                              bytes per call, and the COMMIT record        record
 *   CONFIRM   RoutineControl 0xF000, the       boot_confirm() of the slot the hand-off     at once
 *             confirm routine                  names, if the node runs as a trial
 *
 * The status of the last lib/boot call is the job's result; lib/uds maps it to a negative
 * response (uds_nrc_for_boot()). A job without a flash (no boot_t: plat_flash_device() NULL, or
 * lib/boot refused the layout) and a CONFIRM of a node that runs no trial end with BOOT_E_STATE,
 * which lib/uds answers with conditionsNotCorrect (0x22): the job cannot be done in this state of
 * the node. lib/boot itself refuses an erase while a trial is pending, and of the slot that runs
 * (BOOT_E_STATE), so that the fallback of a trial is never overwritten.
 *
 * ALIVE INDICATIONS. The flash task is a supervised entity of the watchdog manager (app.c,
 * SUPERVISION). flash_job_run() calls `alive` after every call of lib/boot it makes, so that the
 * longest time between two indications of a flash task that works is that of the longest single
 * call: a record write, one sector erase and three page programs at the part's maximum times
 * (400 ms and 3 ms, W25Q128JV, plat_flash.h), and a read of the log. A call that never returns (a
 * part whose busy flag never clears) gives no more indications, and the supervision resets the
 * node.
 *
 * THE CONFIRM, FIRST COME FIRST SERVED. A trial is confirmed either by itself, after 5 s of good
 * health (update.h; flash_jobs_self_confirm()), or by the tester's confirm routine (a CONFIRM
 * job); both run in the flash task, one after the other, and the first one that succeeds writes
 * the CONFIRM record. After it, the routine is refused by lib/boot (the slot is no longer TRIAL:
 * BOOT_E_STATE, so 0x22), and the self-confirm finds the trial confirmed and writes nothing, with
 * BOOT_OK: the trial did not fail its confirm, the tester made it.
 *
 * CONTEXT. A flash_jobs_t belongs to the flash task, the owner of the boot_t; every call is made
 * there. The data of a WRITE job is read during flash_job_run() only.
 */
#ifndef FCN_FLASH_JOB_H
#define FCN_FLASH_JOB_H

#include <stdbool.h>
#include <stdint.h>

#include "boot/boot.h"
#include "plat_retained.h"

/** The kinds of job (see the table above). */
typedef enum {
    FLASH_JOB_NONE = 0,
    FLASH_JOB_ERASE,
    FLASH_JOB_WRITE,
    FLASH_JOB_EXIT,
    FLASH_JOB_CHECK,
    FLASH_JOB_CONFIRM,
    FLASH_JOB_KINDS /**< the number of kinds, NONE included; not a kind */
} flash_job_kind_t;

/** One job. Which members a kind uses: ERASE `address` (the start of a slot in the device's
 *  address space) and `size` (bytes of the image); WRITE `offset`, `data` and `len`; the others
 *  none. */
typedef struct {
    flash_job_kind_t kind;
    uint32_t address;
    uint32_t size;
    uint32_t offset;
    const uint8_t *data;
    uint32_t len;
} flash_job_t;

/** What a job did. */
typedef struct {
    boot_status_t status; /**< the result of its last lib/boot call (see the table) */
    uint32_t steps;       /**< lib/boot calls it made (an ERASE: the begin and every erase step;
                               a CHECK: every finish step) */
} flash_job_result_t;

/** How a job gives an alive indication (see ALIVE INDICATIONS). */
typedef void (*flash_job_alive_fn)(void *ctx);

/** The flash task's state of the jobs. Set up with flash_jobs_init(); read the members for
 *  diagnosis, write none. */
typedef struct {
    boot_t *boot;          /**< lib/boot's state, or NULL without a flash */
    boot_writer_t writer;  /**< the slot writer of the download */
    bool trial;            /**< the node runs as a trial (the hand-off's mode) */
    uint8_t trial_slot;    /**< the slot it runs from, if trial */
    bool confirmed;        /**< the trial was confirmed in this run, by either way */
    bool by_routine;       /**< ... by the confirm routine (a CONFIRM job) */
    uint32_t self_skipped; /**< self-confirms that found the trial confirmed and wrote nothing */
} flash_jobs_t;

/**
 * Set up `j` for this run: `boot` is lib/boot's state after boot_init() succeeded, or NULL for a
 * run without a usable flash; `handoff` the bootloader's hand-off, or NULL without one (a trial
 * only if its mode is PLAT_BOOT_TRIAL). Returns false, setting up nothing, for a NULL `j`. The
 * flash task, before its first job.
 */
bool flash_jobs_init(flash_jobs_t *j, boot_t *boot, const plat_boot_handoff_t *handoff);

/**
 * Run `job` (see the table), calling `alive(ctx)` after every lib/boot call (`alive` may be
 * NULL), and fill `out`. A job of an unknown kind ends with BOOT_E_ARG and no step. A NULL `j`,
 * `job` or `out` does nothing. The flash task; the job takes the time of its flash operations.
 */
void flash_job_run(flash_jobs_t *j, const flash_job_t *job, flash_job_alive_fn alive, void *ctx,
                   flash_job_result_t *out);

/**
 * The trial confirms itself (update.h): boot_confirm() of the hand-off's slot (*written true),
 * unless the confirm routine has confirmed it in this run already, which is BOOT_OK without a
 * call of lib/boot (*written false, counted in self_skipped). BOOT_E_STATE, with nothing called,
 * when the node runs no trial; BOOT_E_ARG when it has no usable flash (as the confirm of phase 4
 * answered) or for a NULL `j` or `written`. The flash task.
 */
boot_status_t flash_jobs_self_confirm(flash_jobs_t *j, bool *written);

/** The name of a kind ("erase", ...), "none" for FLASH_JOB_NONE, "?" for a value that is no
 *  kind. Any context. */
const char *flash_job_kind_name(flash_job_kind_t kind);

#endif /* FCN_FLASH_JOB_H */
