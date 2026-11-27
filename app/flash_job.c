/* SPDX-License-Identifier: MIT */
#include "flash_job.h"

#include <stddef.h>
#include <string.h>

/* The kinds' names, by kind. */
static const char *const KIND_NAMES[] = {"none", "erase", "write", "exit", "check", "confirm"};

_Static_assert((sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0])) == (size_t)FLASH_JOB_KINDS,
               "a name for every kind");

static void give_alive(flash_job_alive_fn alive, void *ctx)
{
    if (alive != NULL) {
        alive(ctx);
    }
}

bool flash_jobs_init(flash_jobs_t *j, boot_t *boot, const plat_boot_handoff_t *handoff)
{
    const bool ok = (j != NULL);

    if (ok) {
        (void)memset(j, 0, sizeof(*j));
        j->boot = boot;
        j->trial_slot = BOOT_NO_SLOT;

        if ((handoff != NULL) && (handoff->mode == PLAT_BOOT_TRIAL)) {
            j->trial = true;
            j->trial_slot = handoff->slot;
        }
    }

    return ok;
}

/* ERASE: the slot named by its address, the BEGIN record, then one sector per step. */
static void run_erase(flash_jobs_t *j, const flash_job_t *job, flash_job_alive_fn alive, void *ctx,
                      flash_job_result_t *out)
{
    uint8_t slot = BOOT_NO_SLOT;

    out->status = boot_slot_by_address(j->boot, job->address, &slot);

    if (out->status == BOOT_OK) {
        out->status = boot_writer_begin(&j->writer, j->boot, slot, job->size);
        out->steps++;
        give_alive(alive, ctx);
    }

    if (out->status == BOOT_OK) {
        out->status = BOOT_BUSY;

        while (out->status == BOOT_BUSY) {
            out->status = boot_writer_erase_step(&j->writer);
            out->steps++;
            give_alive(alive, ctx);
        }
    }
}

/* CHECK: the finish steps until the end: the validation, then the COMMIT record. */
static void run_check(flash_jobs_t *j, flash_job_alive_fn alive, void *ctx, flash_job_result_t *out)
{
    out->status = BOOT_BUSY;

    while (out->status == BOOT_BUSY) {
        out->status = boot_writer_finish_step(&j->writer);
        out->steps++;
        give_alive(alive, ctx);
    }
}

/* CONFIRM: the confirm routine. */
static void run_confirm(flash_jobs_t *j, flash_job_alive_fn alive, void *ctx,
                        flash_job_result_t *out)
{
    if (!j->trial) {
        out->status = BOOT_E_STATE; /* no trial to confirm */
    } else {
        out->status = boot_confirm(j->boot, j->trial_slot);
        out->steps++;
        give_alive(alive, ctx);

        if (out->status == BOOT_OK) {
            j->confirmed = true;
            j->by_routine = true;
        }
    }
}

void flash_job_run(flash_jobs_t *j, const flash_job_t *job, flash_job_alive_fn alive, void *ctx,
                   flash_job_result_t *out)
{
    if ((j != NULL) && (job != NULL) && (out != NULL)) {
        out->status = BOOT_E_ARG;
        out->steps = 0U;

        if ((job->kind == FLASH_JOB_NONE) || (job->kind >= FLASH_JOB_KINDS)) {
            /* no kind: BOOT_E_ARG, nothing done */
        } else if (j->boot == NULL) {
            out->status = BOOT_E_STATE; /* no flash in this run */
        } else {
            switch (job->kind) {
            case FLASH_JOB_ERASE:
                run_erase(j, job, alive, ctx, out);
                break;
            case FLASH_JOB_WRITE:
                out->status = boot_writer_write(&j->writer, job->offset, job->data, job->len);
                out->steps++;
                give_alive(alive, ctx);
                break;
            case FLASH_JOB_EXIT:
                out->status = boot_writer_finish_step(&j->writer);
                out->steps++;
                give_alive(alive, ctx);
                break;
            case FLASH_JOB_CHECK:
                run_check(j, alive, ctx, out);
                break;
            default:
                run_confirm(j, alive, ctx, out);
                break;
            }
        }
    }
}

boot_status_t flash_jobs_self_confirm(flash_jobs_t *j, bool *written)
{
    boot_status_t status = BOOT_E_ARG;

    if ((j != NULL) && (written != NULL)) {
        *written = false;

        if (!j->trial) {
            status = BOOT_E_STATE;
        } else if (j->boot == NULL) {
            status = BOOT_E_ARG; /* no flash, or lib/boot refused the layout: as in phase 4 */
        } else if (j->confirmed) {
            /* The routine came first (see THE CONFIRM): nothing to write. */
            status = BOOT_OK;
            j->self_skipped++;
        } else {
            status = boot_confirm(j->boot, j->trial_slot);
            *written = true;

            if (status == BOOT_OK) {
                j->confirmed = true;
            }
        }
    }

    return status;
}

const char *flash_job_kind_name(flash_job_kind_t kind)
{
    const char *name = "?";
    const uint32_t index = (uint32_t)kind;

    if (index < (uint32_t)FLASH_JOB_KINDS) {
        name = KIND_NAMES[index];
    }

    return name;
}
