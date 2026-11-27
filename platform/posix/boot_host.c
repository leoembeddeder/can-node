/* SPDX-License-Identifier: MIT */
/* The parts fcn_boot and fcn_flash share; boot_host.h says what each does. */
#include "boot_host.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The hand-off carries lib/boot's values as they are: the events a select writes, the slot
 * numbers and "no slot", the attempts, and a status of lib/boot in one byte. */
_Static_assert((int)BOOT_EVENT_NONE == (int)PLAT_BOOT_EVENT_NONE, "event NONE");
_Static_assert((int)BOOT_EVENT_ADOPT == (int)PLAT_BOOT_EVENT_ADOPT, "event ADOPT");
_Static_assert((int)BOOT_EVENT_ROLLBACK == (int)PLAT_BOOT_EVENT_ROLLBACK, "event ROLLBACK");
_Static_assert((int)BOOT_EVENT_REJECT == (int)PLAT_BOOT_EVENT_REJECT, "event REJECT");
_Static_assert((int)BOOT_EVENT_FALLBACK == (int)PLAT_BOOT_EVENT_FALLBACK, "event FALLBACK");
_Static_assert(BOOT_NO_SLOT == PLAT_HANDOFF_NO_SLOT, "no slot");
_Static_assert(BOOT_MAX_ATTEMPTS == PLAT_HANDOFF_MAX_ATTEMPT, "the attempts of a trial");
_Static_assert(BOOT_SLOTS == PLAT_FLASH_SLOTS, "two slots");
_Static_assert((int)BOOT_E_POWER_LOST == 10, "a status of lib/boot fits the hand-off's byte");

static int s_log_fd = STDERR_FILENO;

uint64_t boot_host_time_ns(void)
{
    struct timespec now;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t)now.tv_sec * 1000000000ULL) + (uint64_t)now.tv_nsec;
}

static void log_sink(const char *line, size_t len)
{
    size_t done = 0U;

    while (done < len) {
        const ssize_t n = write(s_log_fd, &line[done], len - done);

        if (n > 0) {
            done += (size_t)n;
        } else if ((n < 0) && (errno == EINTR)) {
            /* try again */
        } else {
            break; /* a log that cannot be written must not stop the boot */
        }
    }
}

bool boot_host_log_open(const char *path, log_level_t level, char *error, size_t error_len)
{
    if (path != NULL) {
        const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

        if (fd < 0) {
            (void)snprintf(error, error_len, "cannot open --log-file %s: %s", path,
                           strerror(errno));
            return false;
        }

        s_log_fd = fd;
    }

    log_init(log_sink, boot_host_time_ns, level);
    return true;
}

void boot_host_config(boot_config_t *out, const boot_validator_t *validator)
{
    const plat_flash_layout_t *layout = plat_flash_layout();

    (void)memset(out, 0, sizeof(*out));
    out->dev = plat_flash_device();
    out->meta_addr = layout->meta.offset;
    out->meta_sectors = layout->meta.size / layout->geometry.sector_size;

    for (uint32_t s = 0U; s < BOOT_SLOTS; s++) {
        out->slot[s].addr = layout->slot[s].offset;
        out->slot[s].size = layout->slot[s].size;
    }

    if (validator != NULL) {
        out->validator = *validator;
    }
}

void boot_host_handoff(const boot_selection_t *sel, plat_boot_handoff_t *out)
{
    (void)memset(out, 0, sizeof(*out));
    out->slot = sel->slot;
    out->mode = sel->trial ? PLAT_BOOT_TRIAL : PLAT_BOOT_CONFIRMED;
    out->attempt = sel->trial ? sel->attempt : 0U;
    out->image_version = sel->version;
    out->event = (plat_boot_event_t)sel->event;
    out->event_slot = sel->event_slot;
    out->select_write_error = (uint8_t)sel->write_status;
}

const char *boot_host_version_text(uint32_t version, char *text)
{
    (void)snprintf(
        text, BOOT_HOST_VERSION_TEXT, "%u.%u.%u", (unsigned int)image_version_major(version),
        (unsigned int)image_version_minor(version), (unsigned int)image_version_patch(version));
    return text;
}

const char *boot_host_slot_name(uint8_t slot)
{
    static const char *const names[BOOT_SLOTS] = {"A", "B"};

    return (slot < BOOT_SLOTS) ? names[slot] : "none";
}
