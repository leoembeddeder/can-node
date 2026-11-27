/* SPDX-License-Identifier: MIT */
/*
 * Test payloads: programs that stand in for broken firmware in an image (decision 6), packed
 * with `python -m fcn_tools.image pack` and started by fcn_boot like a build of the node. One
 * source, two programs (PAYLOAD_KIND):
 *
 *   fcn_payload_crash     an image that crashes at once: it ends with PLAT_RESET_IMAGE_FAULT
 *                         (13), recorded in the retained cell as the node's plat_reset() records
 *                         a reset, so that the next start of fcn_boot finds that reason.
 *   fcn_payload_watchdog  an image that hangs at start: it never kicks a watchdog, and after
 *                         PAYLOAD_WATCHDOG_DELAY_MS it ends as the node's hardware watchdog would
 *                         end it, with PLAT_RESET_WATCHDOG (11) recorded. A STAND-IN: there is no
 *                         watchdog in it, only the delay and the status the model would give
 *                         (platform/posix/posix_wdg.c resets a node whose kicks stop after its
 *                         timeout, 500 ms, APP_WDG_TIMEOUT_MS).
 *
 * Both take the node's command line and use two of its options: --retained-file (the cell, read
 * first, as the node reads it, posix_retained_open(): the reset cause is spent and the
 * bootloader's hand-off taken) and --log-file (one line saying what the hand-off was, so that a
 * test sees which slot and attempt the bootloader started). Every other option, --flash-file
 * among them, is ignored. Like the node, a payload releases the cell's lock itself before it
 * ends (posix_retained_unlock() after the reset is recorded, posix_retained_close() at a
 * start-up error), so that the next start of fcn_boot finds the cell free (docs/bug-log.md,
 * 012). The line, after lib/log's prefix:
 *   payload <kind>: hand-off slot B trial attempt 2, image 1.1.0, event 0; reset cause
 *   IMAGE_FAULT
 * (the event as its number, plat_boot_event_t)
 * or "payload <kind>: no hand-off; reset cause POWER_ON". The FATAL line on stderr names the
 * stand-in. Without a cell nothing is recorded, and the next start is a power-on.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "boot_host.h"
#include "posix_retained.h"

#ifndef PAYLOAD_KIND
#error "PAYLOAD_KIND is 1 (crash) or 2 (watchdog)"
#endif

#define PAYLOAD_CRASH    1
#define PAYLOAD_WATCHDOG 2

/** How long the watchdog stand-in runs before it ends as a watchdog reset: the node's hardware
 *  watchdog timeout (app/app_tasks.h, APP_WDG_TIMEOUT_MS). */
#define PAYLOAD_WATCHDOG_DELAY_MS 500U

static const char TAG[] = "payload";

/* The value of option `name` on the command line, or NULL. */
static const char *option_value(int argc, char **argv, const char *name)
{
    const char *value = NULL;

    for (int i = 1; (i + 1) < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            value = argv[i + 1];
        }
    }

    return value;
}

int main(int argc, char **argv)
{
    const char *const kind = (PAYLOAD_KIND == PAYLOAD_CRASH) ? "crash" : "watchdog";
    const plat_reset_reason_t reason =
        (PAYLOAD_KIND == PAYLOAD_CRASH) ? PLAT_RESET_IMAGE_FAULT : PLAT_RESET_WATCHDOG;
    char error[512];
    posix_retained_info_t retained;
    char version[BOOT_HOST_VERSION_TEXT];

    /* The cell before the log, as the node opens it before anything else: a payload that ends
     * because its log cannot be opened has taken the hand-off all the same. */
    if (!posix_retained_open(option_value(argc, argv, "--retained-file"), error, sizeof(error)) ||
        !boot_host_log_open(option_value(argc, argv, "--log-file"), LOG_LEVEL_INFO, error,
                            sizeof(error))) {
        (void)fprintf(stderr, "payload %s: %s\n", kind, error);
        posix_retained_close(); /* open if it was the log that failed */
        return 1;
    }

    posix_retained_get_info(&retained);

    if (retained.have_handoff) {
        const plat_boot_handoff_t *h = &retained.handoff;

        LOG_INF(TAG,
                "payload %s: hand-off slot %s %s attempt %u, image %s, event %u; reset cause %s",
                kind, boot_host_slot_name(h->slot),
                (h->mode == PLAT_BOOT_TRIAL) ? "trial" : "confirmed", (unsigned int)h->attempt,
                boot_host_version_text(h->image_version, version), (unsigned int)h->event,
                posix_reset_reason_name(retained.cause));
    } else {
        LOG_INF(TAG, "payload %s: no hand-off; reset cause %s", kind,
                posix_reset_reason_name(retained.cause));
    }

    if (PAYLOAD_KIND == PAYLOAD_WATCHDOG) {
        const struct timespec delay = {(time_t)(PAYLOAD_WATCHDOG_DELAY_MS / 1000U),
                                       (long)(PAYLOAD_WATCHDOG_DELAY_MS % 1000U) * 1000000L};

        (void)nanosleep(&delay, NULL);
    }

    (void)fprintf(stderr, "FATAL payload %s: a stand-in for broken firmware ends with status %d\n",
                  kind, (int)reason);
    posix_retained_note_reset(reason);
    posix_retained_unlock();
    _exit((int)reason);
}
