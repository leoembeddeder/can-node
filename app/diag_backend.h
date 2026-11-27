/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The node's backend of lib/uds (uds_backend_t): what each service asks of the node, and how the
 * diagnostic task gets it from the task that owns it. Pure C: the node's side is a table of
 * functions (diag_node_t) that the diagnostic task supplies (app.c, THE DIAGNOSTIC TASK).
 *
 *   uds_backend_t     what it does                                           answers
 *   ecu_reset         accepts hardReset in every session and state: lib/boot  DONE at once
 *                     leaves an unfinished slot WRITING, and the reset comes
 *                     after the response or at its deadline, whichever is
 *                     first (diag.h, ECURESET)
 *   boot_info         the hand-off and the confirm state, from the node       DONE at once
 *                     (diag_node_t.boot_info)
 *   dtc_snapshot      a copy of the DTCs the health task published after its  DONE at once
 *                     last period (diag_node_t.dtc_snapshot)
 *   dtc_clear         asks the health task for a clear (clear_post) and waits  PENDING until the
 *                     until it has been made (clear_done)                     clear is made
 *   erase, write, transfer_exit, check, confirm
 *                     a job of the flash task (flash_job.h), posted at the     PENDING until the
 *                     start (post_job), its result at the end (job_done)       job is done
 *
 * The functions keep no data of their own but the number of the clear asked for and the
 * counts below. A job that cannot be posted (the flash task still has one, which lib/uds never
 * lets happen: it runs one operation at a time) ends at once with BOOT_E_FLASH, so lib/uds sends
 * generalProgrammingFailure (0x72) instead of waiting for nothing; counted in post_refused.
 *
 * CONTEXT. The diagnostic task: lib/uds calls these inside uds_request() and uds_poll().
 */
#ifndef FCN_DIAG_BACKEND_H
#define FCN_DIAG_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#include "dtc/dtc.h"
#include "flash_job.h"
#include "uds/uds.h"

/** What the backend needs from the node; every function must be set. `ctx` is passed to each. */
typedef struct {
    void *ctx;
    /** Hand `job` to the flash task: true if it was taken (the flash task had no job). */
    bool (*post_job)(void *ctx, const flash_job_t *job);
    /** The result of the job posted last, once it is done: true with `*result`, false while it
     *  runs. */
    bool (*job_done)(void *ctx, flash_job_result_t *result);
    /** The boot information now (uds_boot_info_t). */
    void (*boot_info)(void *ctx, uds_boot_info_t *info);
    /** The DTCs as the health task published them, in memory of the diagnostic task's that stays
     *  unchanged until the next call; NULL if there is none. */
    const dtc_state_t *(*dtc_snapshot)(void *ctx);
    /** Ask the health task to clear the DTCs; returns the number the clear is counted with. */
    uint32_t (*clear_post)(void *ctx);
    /** Whether the clear counted with `number` has been made. */
    bool (*clear_done)(void *ctx, uint32_t number);
} diag_node_t;

/** The backend's counts, per job kind indexed by flash_job_kind_t. */
typedef struct {
    uint32_t jobs[FLASH_JOB_KINDS];             /**< jobs posted */
    uint32_t jobs_failed[FLASH_JOB_KINDS];      /**< of those, ended with a status but BOOT_OK, or
                                                  BOOT_BUSY at the exit */
    uint32_t last_steps[FLASH_JOB_KINDS];       /**< lib/boot calls of the last job of the kind */
    boot_status_t last_status[FLASH_JOB_KINDS]; /**< its result */
    uint32_t post_refused;                      /**< jobs the flash task did not take */
    uint32_t snapshots;                         /**< DTC snapshots read */
    uint32_t clears;                            /**< clears asked for */
    uint32_t clears_done;                       /**< of those, made */
    uint32_t resets;                            /**< ECUResets accepted */
} diag_backend_stats_t;

/** The backend. Set up with diag_backend_init(); read `stats`, write nothing. */
typedef struct {
    diag_node_t node;
    flash_job_kind_t pending; /* the kind of the job lib/uds waits for, or FLASH_JOB_NONE */
    uint32_t clear_number;    /* the clear lib/uds waits for */
    diag_backend_stats_t stats;
} diag_backend_t;

/**
 * Prepare `b` for `node` (copied) and fill `out` with the functions lib/uds is to call (its ctx
 * is `b`, which must outlive the server). Returns false, filling nothing, for a NULL pointer or a
 * `node` with a function missing. The diagnostic task, before uds_init().
 */
bool diag_backend_init(diag_backend_t *b, const diag_node_t *node, uds_backend_t *out);

#endif /* FCN_DIAG_BACKEND_H */
