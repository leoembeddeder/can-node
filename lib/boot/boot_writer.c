/* SPDX-License-Identifier: MIT */
#include "boot/boot.h"

#include <stddef.h>
#include <string.h>

#include "boot/boot_internal.h"

/*
 * The slot writer of boot.h, THE SLOT WRITER. Its phases:
 *
 *   IDLE --begin--> ERASING --erase_step (last sector)--> WRITING --finish_step (flush)-->
 *   VALIDATING --finish_step (validator VALID)--> COMMITTING --finish_step (record)--> DONE
 *
 * and FAILED from any phase on a flash error, a validation that fails, or a slot that is no
 * longer WRITING in the metadata. boot_writer_begin() starts again from any phase.
 *
 * What holds between two calls while the writer is ERASING to COMMITTING: `boot` is a ready
 * boot_t, `slot` < BOOT_SLOTS, 1 <= image_size <= the slot's size, erase_done <= erase_total,
 * written <= image_size, page_offset is a multiple of the page size, and page_offset +
 * page_fill == written with page_fill < page size: the bytes page_offset..written - 1 are in
 * page[] and not yet programmed, everything before page_offset is.
 *
 * Before every erase and every program the writer checks that the boot_t's last view of the
 * metadata still has the slot WRITING: the BEGIN record is what allows the slot to be erased,
 * and no other call on the same boot_t can change that view without making the slot something
 * else (a fresh begin on another writer of the same slot keeps it WRITING).
 */

/* A writer that a begin has prepared (whatever happened after). */
static bool writer_usable(const boot_writer_t *w)
{
    return (w != NULL) && boot_ready(w->boot) && (w->slot < BOOT_SLOTS);
}

/* The metadata, as last read, still has the writer's slot WRITING. */
static bool slot_writing(const boot_writer_t *w)
{
    return w->boot->have_record && (w->boot->record.slot[w->slot].state == BOOT_SLOT_WRITING);
}

static const flash_dev_t *device_of(const boot_writer_t *w)
{
    return w->boot->config.dev;
}

boot_status_t boot_writer_begin(boot_writer_t *w, boot_t *b, uint8_t slot, uint32_t image_size)
{
    boot_status_t status = BOOT_OK;

    if (w == NULL) {
        status = BOOT_E_ARG;
    } else {
        w->boot = NULL;
        w->phase = BOOT_WRITER_IDLE;
        w->slot = BOOT_NO_SLOT;
        w->image_size = 0U;
        w->erase_total = 0U;
        w->erase_done = 0U;
        w->written = 0U;
        w->page_offset = 0U;
        w->page_fill = 0U;
        w->check_steps = 0U;

        if (!boot_ready(b) || (slot >= BOOT_SLOTS) || !boot_has_validator(b)) {
            status = BOOT_E_ARG;
        } else {
            status = boot_scan(b);
        }
    }

    if (status == BOOT_OK) {
        if (!b->have_record || (b->record.slot[0].state == BOOT_SLOT_TRIAL) ||
            (b->record.slot[1].state == BOOT_SLOT_TRIAL) ||
            (b->record.slot[slot].state == BOOT_SLOT_CONFIRMED)) {
            status = BOOT_E_STATE;
        } else if ((image_size == 0U) || (image_size > b->config.slot[slot].size)) {
            status = BOOT_E_SIZE;
        } else if (b->record.slot[slot].state != BOOT_SLOT_WRITING) {
            /* The record that makes the slot unbootable, before its first erase. */
            boot_record_t next = b->record;

            boot_record_clear_slot(&next, slot, BOOT_SLOT_WRITING);
            next.event = BOOT_EVENT_BEGIN;
            next.event_slot = slot;
            status = boot_write_record(b, &next);
            if (status != BOOT_OK) {
                w->phase = BOOT_WRITER_FAILED;
            }
        } else {
            /* WRITING already: an earlier update was abandoned; nothing to record */
        }
    }

    if (status == BOOT_OK) {
        const uint32_t sector = b->config.dev->geometry.sector_size;

        w->boot = b;
        w->slot = slot;
        w->image_size = image_size;
        w->erase_total = (image_size + sector - 1U) / sector;
        w->phase = BOOT_WRITER_ERASING;
    }

    return status;
}

boot_status_t boot_writer_erase_step(boot_writer_t *w)
{
    boot_status_t status = BOOT_E_ARG;

    if (!writer_usable(w)) {
        /* status stays BOOT_E_ARG */
    } else if (w->phase != BOOT_WRITER_ERASING) {
        status = BOOT_E_SEQUENCE;
    } else if (!slot_writing(w)) {
        w->phase = BOOT_WRITER_FAILED;
        status = BOOT_E_STATE;
    } else {
        const flash_dev_t *dev = device_of(w);
        const uint32_t addr =
            w->boot->config.slot[w->slot].addr + (w->erase_done * dev->geometry.sector_size);

        status = boot_from_flash(dev->erase(dev->ctx, addr));
        if (status == BOOT_OK) {
            w->erase_done++;
            if (w->erase_done < w->erase_total) {
                status = BOOT_BUSY;
            } else {
                w->phase = BOOT_WRITER_WRITING;
            }
        } else {
            w->phase = BOOT_WRITER_FAILED;
        }
    }

    return status;
}

/* Program the first `len` bytes of page[] (whole units) at page_offset of the slot. */
static boot_status_t program_page(boot_writer_t *w, uint32_t len)
{
    const boot_status_t status =
        boot_program(w->boot, w->boot->config.slot[w->slot].addr + w->page_offset, w->page, len);

    if (status != BOOT_OK) {
        w->phase = BOOT_WRITER_FAILED;
    }

    return status;
}

boot_status_t boot_writer_write(boot_writer_t *w, uint32_t offset, const uint8_t *data,
                                uint32_t len)
{
    boot_status_t status = BOOT_E_ARG;

    if (!writer_usable(w) || (data == NULL) || (len == 0U)) {
        /* status stays BOOT_E_ARG */
    } else if ((w->phase != BOOT_WRITER_WRITING) || (offset != w->written)) {
        status = BOOT_E_SEQUENCE;
    } else if (len > (w->image_size - w->written)) {
        status = BOOT_E_SIZE;
    } else if (!slot_writing(w)) {
        w->phase = BOOT_WRITER_FAILED;
        status = BOOT_E_STATE;
    } else {
        const uint32_t page = device_of(w)->geometry.page_size;
        uint32_t done = 0U;

        status = BOOT_OK;
        while ((status == BOOT_OK) && (done < len)) {
            const uint32_t room = page - w->page_fill;
            const uint32_t n = ((len - done) < room) ? (len - done) : room;

            (void)memcpy(&w->page[w->page_fill], &data[done], n);
            w->page_fill += n;
            w->written += n;
            done += n;
            if (w->page_fill == page) {
                status = program_page(w, page);
                w->page_offset += page;
                w->page_fill = 0U;
            }
        }
    }

    return status;
}

/* WRITING with every byte taken: program the partial last page, padded with 0xFF to whole
 * units, and start the validation. */
static boot_status_t finish_flush(boot_writer_t *w)
{
    boot_status_t status = BOOT_OK;

    if (w->page_fill > 0U) {
        const uint32_t unit = device_of(w)->geometry.unit_size;
        const uint32_t len = ((w->page_fill + unit - 1U) / unit) * unit;

        (void)memset(&w->page[w->page_fill], 0xFF, len - w->page_fill);
        status = program_page(w, len);
    }

    if (status == BOOT_OK) {
        const boot_validator_t *v = &w->boot->config.validator;

        v->start(v->ctx, device_of(w), w->boot->config.slot[w->slot].addr, w->image_size);
        w->check_steps = 0U;
        w->phase = BOOT_WRITER_VALIDATING;
        status = BOOT_BUSY;
    }

    return status;
}

/* VALIDATING: one validator step. */
static boot_status_t finish_check(boot_writer_t *w)
{
    const boot_validator_t *v = &w->boot->config.validator;
    boot_status_t status = BOOT_BUSY;

    switch (v->step(v->ctx, &w->info)) {
    case BOOT_CHECK_BUSY:
        w->check_steps++;
        if (w->check_steps > w->image_size) {
            w->phase = BOOT_WRITER_FAILED; /* no progress: see THE VALIDATOR */
            status = BOOT_E_INVALID;
        }
        break;
    case BOOT_CHECK_VALID:
        w->phase = BOOT_WRITER_COMMITTING;
        break;
    case BOOT_CHECK_INVALID:
        w->phase = BOOT_WRITER_FAILED;
        status = BOOT_E_INVALID;
        break;
    case BOOT_CHECK_ERROR:
    default:
        w->phase = BOOT_WRITER_FAILED;
        status = BOOT_E_FLASH;
        break;
    }

    return status;
}

/* COMMITTING: the COMMIT record, if the metadata still has the slot WRITING. */
static boot_status_t finish_commit(boot_writer_t *w)
{
    boot_t *b = w->boot;
    boot_status_t status = boot_scan(b);

    if ((status == BOOT_OK) && !slot_writing(w)) {
        status = BOOT_E_STATE;
    }

    if (status == BOOT_OK) {
        boot_record_t next = b->record;

        next.slot[w->slot].state = BOOT_SLOT_TRIAL;
        next.slot[w->slot].version = w->info.version;
        (void)memcpy(next.slot[w->slot].id, w->info.id, BOOT_IMAGE_ID_SIZE);
        next.event = BOOT_EVENT_COMMIT;
        next.event_slot = w->slot;
        status = boot_write_record(b, &next);
    }

    w->phase = (status == BOOT_OK) ? BOOT_WRITER_DONE : BOOT_WRITER_FAILED;

    return status;
}

boot_status_t boot_writer_finish_step(boot_writer_t *w)
{
    boot_status_t status = BOOT_E_ARG;

    if (writer_usable(w)) {
        switch (w->phase) {
        case BOOT_WRITER_WRITING:
            if (w->written != w->image_size) {
                status = BOOT_E_SEQUENCE;
            } else if (!slot_writing(w)) {
                w->phase = BOOT_WRITER_FAILED;
                status = BOOT_E_STATE;
            } else {
                status = finish_flush(w);
            }
            break;
        case BOOT_WRITER_VALIDATING:
            status = finish_check(w);
            break;
        case BOOT_WRITER_COMMITTING:
            status = finish_commit(w);
            break;
        case BOOT_WRITER_IDLE:
        case BOOT_WRITER_ERASING:
        case BOOT_WRITER_DONE:
        case BOOT_WRITER_FAILED:
        default:
            status = BOOT_E_SEQUENCE;
            break;
        }
    }

    return status;
}

void boot_writer_progress(const boot_writer_t *w, boot_writer_progress_t *p)
{
    if ((w != NULL) && (p != NULL)) {
        p->phase = w->phase;
        p->slot = w->slot;
        p->image_size = w->image_size;
        p->sectors_total = w->erase_total;
        p->sectors_erased = w->erase_done;
        p->written = w->written;
    }
}

/* The names of boot_writer_phase_t, in the order of its values (from 0, no gaps). */
static const char *const s_phase_names[] = {"idle",       "erasing", "writing", "validating",
                                            "committing", "done",    "failed"};

_Static_assert((sizeof(s_phase_names) / sizeof(s_phase_names[0])) ==
                   ((uint32_t)BOOT_WRITER_FAILED + 1U),
               "one name per boot_writer_phase_t");

const char *boot_writer_phase_name(boot_writer_phase_t phase)
{
    const uint32_t count = (uint32_t)(sizeof(s_phase_names) / sizeof(s_phase_names[0]));

    return ((uint32_t)phase < count) ? s_phase_names[(uint32_t)phase] : "?";
}
