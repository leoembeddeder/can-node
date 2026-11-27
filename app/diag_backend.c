/* SPDX-License-Identifier: MIT */
#include "diag_backend.h"

#include <stddef.h>
#include <string.h>

static diag_backend_t *backend_of(void *ctx)
{
    return (diag_backend_t *)ctx;
}

/* A job lib/uds waits for: posted at the start, its result at the end. */
static uds_be_t run_job(diag_backend_t *b, uds_call_t call, const flash_job_t *job,
                        boot_status_t *status)
{
    uds_be_t answer = UDS_BE_PENDING;
    flash_job_result_t result = {.status = BOOT_E_FLASH, .steps = 0U};

    if (call == UDS_CALL_START) {
        if (b->node.post_job(b->node.ctx, job)) {
            b->pending = job->kind;
            b->stats.jobs[(uint32_t)job->kind]++;
        } else {
            b->stats.post_refused++;
            *status = BOOT_E_FLASH;
            answer = UDS_BE_DONE;
        }
    } else if (b->node.job_done(b->node.ctx, &result)) {
        const flash_job_kind_t kind = b->pending;
        const bool ok = (result.status == BOOT_OK) ||
                        ((kind == FLASH_JOB_EXIT) && (result.status == BOOT_BUSY));

        if ((uint32_t)kind < (uint32_t)FLASH_JOB_KINDS) {
            b->stats.last_steps[(uint32_t)kind] = result.steps;
            b->stats.last_status[(uint32_t)kind] = result.status;

            if (!ok) {
                b->stats.jobs_failed[(uint32_t)kind]++;
            }
        }

        b->pending = FLASH_JOB_NONE;
        *status = result.status;
        answer = UDS_BE_DONE;
    } else {
        /* the job runs: lib/uds asks again at its next poll */
    }

    return answer;
}

static uds_be_t be_ecu_reset(void *ctx, uds_call_t call, uint8_t reset_type, uint8_t *nrc)
{
    diag_backend_t *b = backend_of(ctx);

    (void)call;
    (void)reset_type; /* lib/uds accepts hardReset only */
    b->stats.resets++;
    *nrc = UDS_NRC_NONE;
    return UDS_BE_DONE;
}

static uds_be_t be_boot_info(void *ctx, uds_call_t call, uds_boot_info_t *info, uint8_t *nrc)
{
    diag_backend_t *b = backend_of(ctx);

    (void)call;
    b->node.boot_info(b->node.ctx, info);
    *nrc = UDS_NRC_NONE;
    return UDS_BE_DONE;
}

static uds_be_t be_dtc_snapshot(void *ctx, uds_call_t call, const dtc_state_t **state, uint8_t *nrc)
{
    diag_backend_t *b = backend_of(ctx);

    (void)call;
    *state = b->node.dtc_snapshot(b->node.ctx);
    b->stats.snapshots++;
    *nrc = UDS_NRC_NONE; /* a NULL snapshot is lib/uds's generalReject */
    return UDS_BE_DONE;
}

static uds_be_t be_dtc_clear(void *ctx, uds_call_t call, uint32_t group, uint8_t *nrc)
{
    diag_backend_t *b = backend_of(ctx);
    uds_be_t answer = UDS_BE_PENDING;

    (void)group; /* lib/uds accepts all groups only */

    if (call == UDS_CALL_START) {
        b->clear_number = b->node.clear_post(b->node.ctx);
        b->stats.clears++;
    } else if (b->node.clear_done(b->node.ctx, b->clear_number)) {
        b->stats.clears_done++;
        *nrc = UDS_NRC_NONE;
        answer = UDS_BE_DONE;
    } else {
        /* the health task has not made it yet */
    }

    return answer;
}

static uds_be_t be_erase(void *ctx, uds_call_t call, uint32_t address, uint32_t size,
                         boot_status_t *status)
{
    const flash_job_t job = {.kind = FLASH_JOB_ERASE,
                             .address = address,
                             .size = size,
                             .offset = 0U,
                             .data = NULL,
                             .len = 0U};

    return run_job(backend_of(ctx), call, &job, status);
}

static uds_be_t be_write(void *ctx, uds_call_t call, uint32_t offset, const uint8_t *data,
                         uint32_t len, boot_status_t *status)
{
    const flash_job_t job = {.kind = FLASH_JOB_WRITE,
                             .address = 0U,
                             .size = 0U,
                             .offset = offset,
                             .data = data,
                             .len = len};

    return run_job(backend_of(ctx), call, &job, status);
}

static uds_be_t be_simple(void *ctx, uds_call_t call, flash_job_kind_t kind, boot_status_t *status)
{
    const flash_job_t job = {
        .kind = kind, .address = 0U, .size = 0U, .offset = 0U, .data = NULL, .len = 0U};

    return run_job(backend_of(ctx), call, &job, status);
}

static uds_be_t be_transfer_exit(void *ctx, uds_call_t call, boot_status_t *status)
{
    return be_simple(ctx, call, FLASH_JOB_EXIT, status);
}

static uds_be_t be_check(void *ctx, uds_call_t call, boot_status_t *status)
{
    return be_simple(ctx, call, FLASH_JOB_CHECK, status);
}

static uds_be_t be_confirm(void *ctx, uds_call_t call, boot_status_t *status)
{
    return be_simple(ctx, call, FLASH_JOB_CONFIRM, status);
}

bool diag_backend_init(diag_backend_t *b, const diag_node_t *node, uds_backend_t *out)
{
    const bool ok = (b != NULL) && (node != NULL) && (out != NULL) && (node->post_job != NULL) &&
                    (node->job_done != NULL) && (node->boot_info != NULL) &&
                    (node->dtc_snapshot != NULL) && (node->clear_post != NULL) &&
                    (node->clear_done != NULL);

    if (ok) {
        (void)memset(b, 0, sizeof(*b));
        b->node = *node;
        b->pending = FLASH_JOB_NONE;

        for (uint32_t kind = 0U; kind < (uint32_t)FLASH_JOB_KINDS; kind++) {
            b->stats.last_status[kind] = BOOT_OK;
        }

        out->ctx = b;
        out->ecu_reset = be_ecu_reset;
        out->boot_info = be_boot_info;
        out->dtc_snapshot = be_dtc_snapshot;
        out->dtc_clear = be_dtc_clear;
        out->erase = be_erase;
        out->write = be_write;
        out->transfer_exit = be_transfer_exit;
        out->check = be_check;
        out->confirm = be_confirm;
    }

    return ok;
}
