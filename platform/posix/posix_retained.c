/* SPDX-License-Identifier: MIT */
/*
 * The retained-RAM cell of the host build: plat_retained.h says what it holds and why, and
 * posix_retained.h what each function does. This comment says how the file stands in for RAM.
 *
 * THE FILE IS THE RAM. The cell is mapped with MAP_SHARED, so every store to it lands in the
 * kernel's page cache at once and is seen by the next process that opens the file, whether or
 * not this one exits normally: _exit() in plat_reset() loses nothing, and no msync() is needed.
 * Only a crash of the host's kernel could lose the stores, and that is a loss of supply anyway.
 *
 * A TORN WRITE IS A POWER-ON. plat_reset() writes the reason, the count and then the CRC. A
 * process killed between those stores (SIGKILL from the bench) leaves a cell whose CRC does not
 * match, which the next start takes as a power-on, as garbage in RAM after a power cycle would
 * be.
 *
 * ONE BOARD, ONE CELL. The file is locked with flock() while it is open, and a second node
 * that names it fails to start: two nodes writing one cell would make each other's cause up.
 *
 * THE LOCK BELONGS TO THE OPEN FILE DESCRIPTION, not to the descriptor (flock(2)), and the
 * mapping refers to that description as the descriptor does. Without flock(LOCK_UN) the lock
 * goes only with the last reference: closing the descriptor leaves it held while the mapping
 * exists, and the mapping of a process image that has exec'd or exited exists until the last
 * reference to the old memory map goes, which another process can hold (a read of
 * /proc/<pid>/stat or /proc/<pid>/maps takes one). The next process on the cell, started the
 * moment this one has exec'd or its exit has been seen, would then find the cell "in use". So
 * nothing is left to the kernel: posix_retained_unlock() releases the lock at once, whatever
 * still refers to the description, and it is called at every end of a run that does not close
 * the cell: directly before the bootloader's exec, and after the reset is recorded and
 * directly before _exit() in plat_reset() (every reset reason), in the shutdown guard
 * (posix_lifecycle.c), at a power cut without a kernel (posix_flash.c) and in the test
 * payloads. posix_retained_close(), the orderly end, releases it the same way after the unmap.
 * While the lock is released nothing here writes into the cell (s_locked), until
 * posix_retained_relock() has it again. A process that is killed (SIGKILL, a signal's default
 * action, a sanitizer's abort) runs none of this: its lock goes with its memory map.
 */
#include "posix_retained.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crc/crc.h"

static plat_retained_cell_t *s_cell; /* the mapping, or NULL */
static int s_fd = -1;                /* open while the cell is mapped */
static bool s_locked; /* s_fd holds the lock: from the open to the close, except between
                         posix_retained_unlock() and a posix_retained_relock() that took it */
static posix_retained_info_t s_info = {.open = false,
                                       .cause = PLAT_RESET_POWER_ON,
                                       .resets = 0U,
                                       .verdict = "no cell",
                                       .handoff_verdict = "none",
                                       .have_handoff = false};

/* Word 1 of a hand-off: its fields' places (plat_retained.h, THE HAND-OFF). */
#define HANDOFF_SLOT_SHIFT    0U
#define HANDOFF_MODE_SHIFT    8U
#define HANDOFF_ATTEMPT_SHIFT 16U
#define HANDOFF_EVENT_SHIFT   24U
/* Word 3. */
#define HANDOFF_EVENT_SLOT_SHIFT  0U
#define HANDOFF_WRITE_ERROR_SHIFT 8U
#define HANDOFF_BYTE              0xFFU
/* The largest boot_status_t of lib/boot (BOOT_E_POWER_LOST), the bound of a write error. */
#define HANDOFF_WRITE_ERROR_MAX 10U

_Static_assert(PLAT_RETAINED_RESERVED_WORDS == 4U, "the hand-off takes the four reserved words");

void posix_handoff_encode(const plat_boot_handoff_t *handoff,
                          uint32_t words[PLAT_RETAINED_RESERVED_WORDS])
{
    words[0] = PLAT_HANDOFF_MAGIC;
    words[1] = ((uint32_t)handoff->slot << HANDOFF_SLOT_SHIFT) |
               ((uint32_t)handoff->mode << HANDOFF_MODE_SHIFT) |
               ((uint32_t)handoff->attempt << HANDOFF_ATTEMPT_SHIFT) |
               ((uint32_t)handoff->event << HANDOFF_EVENT_SHIFT);
    words[2] = handoff->image_version;
    words[3] = ((uint32_t)handoff->event_slot << HANDOFF_EVENT_SLOT_SHIFT) |
               ((uint32_t)handoff->select_write_error << HANDOFF_WRITE_ERROR_SHIFT);
}

static bool handoff_event_known(uint32_t event)
{
    return (event == (uint32_t)PLAT_BOOT_EVENT_NONE) ||
           (event == (uint32_t)PLAT_BOOT_EVENT_ADOPT) ||
           (event == (uint32_t)PLAT_BOOT_EVENT_ROLLBACK) ||
           (event == (uint32_t)PLAT_BOOT_EVENT_REJECT) ||
           (event == (uint32_t)PLAT_BOOT_EVENT_FALLBACK);
}

/* The rules of THE HAND-OFF for decoded fields. */
static bool handoff_valid(uint32_t slot, uint32_t mode, uint32_t attempt, uint32_t event,
                          uint32_t event_slot, uint32_t write_error)
{
    const bool trial = (mode == (uint32_t)PLAT_BOOT_TRIAL);
    const bool confirmed = (mode == (uint32_t)PLAT_BOOT_CONFIRMED);
    const bool attempt_ok =
        trial ? ((attempt >= 1U) && (attempt <= PLAT_HANDOFF_MAX_ATTEMPT)) : (attempt == 0U);
    const bool event_ok = handoff_event_known(event) && ((event == (uint32_t)PLAT_BOOT_EVENT_NONE)
                                                             ? (event_slot == PLAT_HANDOFF_NO_SLOT)
                                                             : (event_slot < 2U));

    return (slot < 2U) && (trial || confirmed) && attempt_ok && event_ok &&
           (write_error <= HANDOFF_WRITE_ERROR_MAX);
}

const char *posix_handoff_decode(const uint32_t words[PLAT_RETAINED_RESERVED_WORDS],
                                 plat_boot_handoff_t *out)
{
    const char *verdict = "none";

    if (words[0] == PLAT_HANDOFF_MAGIC) {
        const uint32_t slot = (words[1] >> HANDOFF_SLOT_SHIFT) & HANDOFF_BYTE;
        const uint32_t mode = (words[1] >> HANDOFF_MODE_SHIFT) & HANDOFF_BYTE;
        const uint32_t attempt = (words[1] >> HANDOFF_ATTEMPT_SHIFT) & HANDOFF_BYTE;
        const uint32_t event = (words[1] >> HANDOFF_EVENT_SHIFT) & HANDOFF_BYTE;
        const uint32_t event_slot = (words[3] >> HANDOFF_EVENT_SLOT_SHIFT) & HANDOFF_BYTE;
        const uint32_t write_error = (words[3] >> HANDOFF_WRITE_ERROR_SHIFT) & HANDOFF_BYTE;
        const bool rest_zero = (words[3] >> 16U) == 0U;

        verdict = "invalid";

        if (rest_zero && handoff_valid(slot, mode, attempt, event, event_slot, write_error)) {
            out->slot = (uint8_t)slot;
            out->mode = (mode == (uint32_t)PLAT_BOOT_TRIAL) ? PLAT_BOOT_TRIAL : PLAT_BOOT_CONFIRMED;
            out->attempt = (uint8_t)attempt;
            out->image_version = words[2];
            out->event = (plat_boot_event_t)event;
            out->event_slot = (uint8_t)event_slot;
            out->select_write_error = (uint8_t)write_error;
            verdict = "taken";
        }
    }

    return verdict;
}

bool posix_retained_reason_recorded(uint32_t reason)
{
    bool recorded = false;

    switch (reason) {
    case (uint32_t)PLAT_RESET_REQUESTED:
    case (uint32_t)PLAT_RESET_WATCHDOG:
    case (uint32_t)PLAT_RESET_IMAGE_FAULT:
    case (uint32_t)PLAT_RESET_ASSERT:
    case (uint32_t)PLAT_RESET_STACK_OVERFLOW:
    case (uint32_t)PLAT_RESET_MALLOC_FAILED:
        recorded = true;
        break;
    default:
        recorded = false;
        break;
    }

    return recorded;
}

bool posix_reset_reason_valid(plat_reset_reason_t reason)
{
    return posix_retained_reason_recorded((uint32_t)reason) || (reason == PLAT_RESET_POWER_LOSS);
}

const char *posix_reset_reason_name(plat_reset_reason_t reason)
{
    const char *name;

    switch (reason) {
    case PLAT_RESET_POWER_ON:
        name = "POWER_ON";
        break;
    case PLAT_RESET_REQUESTED:
        name = "REQUESTED";
        break;
    case PLAT_RESET_WATCHDOG:
        name = "WATCHDOG";
        break;
    case PLAT_RESET_POWER_LOSS:
        name = "POWER_LOSS";
        break;
    case PLAT_RESET_IMAGE_FAULT:
        name = "IMAGE_FAULT";
        break;
    case PLAT_RESET_ASSERT:
        name = "ASSERT";
        break;
    case PLAT_RESET_STACK_OVERFLOW:
        name = "STACK_OVERFLOW";
        break;
    case PLAT_RESET_MALLOC_FAILED:
        name = "MALLOC_FAILED";
        break;
    default:
        name = "unknown"; /* a value outside the enumeration, from damaged memory */
        break;
    }

    return name;
}

uint32_t posix_retained_crc(const plat_retained_cell_t *cell)
{
    /* The CRC covers the members before it, which are all the bytes before it (no padding:
     * plat_retained.h checks the offsets). */
    return crc32_compute((const uint8_t *)cell, offsetof(plat_retained_cell_t, crc));
}

/* Why a cell that was read is not a reset, or NULL if it records one. */
static const char *judge(const plat_retained_cell_t *cell)
{
    const char *verdict = NULL;

    if (cell->magic != PLAT_RETAINED_MAGIC) {
        verdict = "magic";
    } else if ((cell->version != PLAT_RETAINED_VERSION) ||
               (cell->size != (uint16_t)sizeof(plat_retained_cell_t))) {
        verdict = "version";
    } else if (cell->crc != posix_retained_crc(cell)) {
        verdict = "crc";
    } else if (cell->reset_reason == (uint32_t)PLAT_RESET_POWER_ON) {
        verdict = "no reset";
    } else if (!posix_retained_reason_recorded(cell->reset_reason)) {
        verdict = "reason";
    } else {
        verdict = NULL;
    }

    return verdict;
}

static void seal(plat_retained_cell_t *cell)
{
    cell->crc = posix_retained_crc(cell);
}

/* A cell as after power-on: every word 0 but the header, nothing recorded. */
static void initialise(plat_retained_cell_t *cell)
{
    (void)memset(cell, 0, sizeof(*cell));
    cell->magic = PLAT_RETAINED_MAGIC;
    cell->version = (uint16_t)PLAT_RETAINED_VERSION;
    cell->size = (uint16_t)sizeof(plat_retained_cell_t);
    cell->reset_reason = (uint32_t)PLAT_RESET_POWER_ON;
    seal(cell);
}

static bool fail(char *error, size_t error_len, const char *what, const char *path, int fd)
{
    const int saved = errno;

    if (fd >= 0) {
        (void)close(fd);
    }

    (void)snprintf(error, error_len, "cannot %s --retained-file %s: %s", what, path,
                   strerror(saved));
    return false;
}

/* A cell whose magic, version, size and CRC are right, whatever its reason says. */
static bool intact(const char *verdict)
{
    return (verdict == NULL) || (strcmp(verdict, "no reset") == 0) ||
           (strcmp(verdict, "reason") == 0);
}

/* Take the hand-off out of an intact cell: decode it into s_info and, if word 0 was the magic
 * (a hand-off, valid or not), zero the four words. The caller seals the cell. */
static void take_handoff(plat_retained_cell_t *cell)
{
    plat_boot_handoff_t handoff;
    const char *verdict = posix_handoff_decode(cell->reserved, &handoff);

    s_info.handoff_verdict = verdict;
    s_info.have_handoff = (strcmp(verdict, "taken") == 0);

    if (s_info.have_handoff) {
        s_info.handoff = handoff;
    }

    if (strcmp(verdict, "none") != 0) {
        (void)memset(cell->reserved, 0, sizeof(cell->reserved));
    }
}

/* posix_retained_open() and posix_retained_open_boot(): `keep_reason` leaves the reason of a
 * reset in the cell (the bootloader). */
static bool open_cell(const char *path, bool keep_reason, char *error, size_t error_len)
{
    struct stat info;
    bool existed;
    const char *size_verdict = NULL;
    int fd;
    void *map;

    if (s_cell != NULL) {
        (void)snprintf(error, error_len, "a retained cell is already open");
        return false;
    }

    s_info = (posix_retained_info_t){.open = false,
                                     .cause = PLAT_RESET_POWER_ON,
                                     .resets = 0U,
                                     .verdict = "no cell",
                                     .handoff_verdict = "none",
                                     .have_handoff = false};

    if (path == NULL) {
        return true;
    }

    existed = (stat(path, &info) == 0);
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);

    if (fd < 0) {
        return fail(error, error_len, "open", path, -1);
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            (void)close(fd);
            (void)snprintf(error, error_len, "--retained-file %s is in use by another node", path);
            return false;
        }

        return fail(error, error_len, "lock", path, fd);
    }

    if (fstat(fd, &info) != 0) {
        return fail(error, error_len, "inspect", path, fd);
    }

    if (!S_ISREG(info.st_mode)) {
        errno = EINVAL;
        return fail(error, error_len, "use (not a regular file)", path, fd);
    }

    if (info.st_size != (off_t)sizeof(plat_retained_cell_t)) {
        /* Not a cell of this layout: give the file the cell's size; its content is garbage
         * and is replaced below. */
        size_verdict = existed ? "size" : "missing";

        if (ftruncate(fd, (off_t)sizeof(plat_retained_cell_t)) != 0) {
            return fail(error, error_len, "size", path, fd);
        }
    }

    map = mmap(NULL, sizeof(plat_retained_cell_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (map == MAP_FAILED) {
        return fail(error, error_len, "map", path, fd);
    }

    s_cell = (plat_retained_cell_t *)map;
    s_fd = fd;
    s_locked = true;

    {
        const char *verdict = (size_verdict != NULL) ? size_verdict : judge(s_cell);

        s_info.open = true;

        /* The hand-off first, whatever the reason: a bootloader that started the image after
         * power-on wrote it into a cell that records no reset. */
        if ((size_verdict == NULL) && intact(verdict)) {
            take_handoff(s_cell);
        }

        if (verdict == NULL) {
            /* A reset: report its reason once, and keep the count and the reserved words (but
             * for a hand-off, taken above). The bootloader leaves the reason for the image. */
            s_info.cause = (plat_reset_reason_t)s_cell->reset_reason;
            s_info.resets = s_cell->resets;
            s_info.verdict = "reset";

            if (!keep_reason) {
                s_cell->reset_reason = (uint32_t)PLAT_RESET_POWER_ON;
            }

            seal(s_cell);
        } else {
            s_info.verdict = verdict;
            initialise(s_cell);
        }
    }

    return true;
}

bool posix_retained_open(const char *path, char *error, size_t error_len)
{
    return open_cell(path, false, error, error_len);
}

bool posix_retained_open_boot(const char *path, char *error, size_t error_len)
{
    return open_cell(path, true, error, error_len);
}

bool posix_retained_set_handoff(const plat_boot_handoff_t *handoff)
{
    bool written = false;

    if ((s_cell != NULL) && s_locked && (handoff != NULL)) {
        uint32_t words[PLAT_RETAINED_RESERVED_WORDS];
        plat_boot_handoff_t check;

        posix_handoff_encode(handoff, words);

        /* Only a hand-off the image will take: what the encoding made of it must decode. */
        if (strcmp(posix_handoff_decode(words, &check), "taken") == 0) {
            (void)memcpy(s_cell->reserved, words, sizeof(words));
            seal(s_cell);
            written = true;
        }
    }

    return written;
}

void posix_retained_clear_handoff(void)
{
    /* What take_handoff() removes: four words whose first is the magic. */
    if ((s_cell != NULL) && s_locked && (s_cell->reserved[0] == PLAT_HANDOFF_MAGIC)) {
        (void)memset(s_cell->reserved, 0, sizeof(s_cell->reserved));
        seal(s_cell);
    }
}

void posix_retained_unlock(void)
{
    if ((s_cell != NULL) && s_locked) {
        /* LOCK_UN removes the lock of the open file description at once, also while the
         * mapping (or any copy of the descriptor) still refers to the description. */
        (void)flock(s_fd, LOCK_UN);
        s_locked = false;
    }
}

bool posix_retained_relock(void)
{
    if ((s_cell != NULL) && !s_locked && (flock(s_fd, LOCK_EX | LOCK_NB) == 0)) {
        s_locked = true;
    }

    return (s_cell != NULL) && s_locked;
}

bool plat_boot_handoff(plat_boot_handoff_t *out)
{
    bool present = false;

    if ((out != NULL) && s_info.have_handoff) {
        *out = s_info.handoff;
        present = true;
    }

    return present;
}

void posix_retained_note_reset(plat_reset_reason_t reason)
{
    plat_retained_cell_t *const cell = s_cell;

    if ((cell != NULL) && s_locked) {
        if (reason == PLAT_RESET_POWER_LOSS) {
            (void)memset(cell, 0, sizeof(*cell)); /* RAM does not survive a loss of supply */
        } else {
            cell->reset_reason = (uint32_t)reason;
            cell->resets++;
            seal(cell);
        }
    }
}

void posix_retained_close(void)
{
    if (s_cell != NULL) {
        (void)munmap(s_cell, sizeof(plat_retained_cell_t));
        s_cell = NULL;
        /* The lock explicitly, and only then the descriptor: the close alone removes the lock
         * only when nothing else refers to the open file description, and the memory map of a
         * process forked while the cell was mapped does, for as long as that map exists. */
        (void)flock(s_fd, LOCK_UN);
        (void)close(s_fd);
        s_fd = -1;
        s_locked = false;
    }
}

void posix_retained_get_info(posix_retained_info_t *out)
{
    *out = s_info;
}

plat_reset_reason_t plat_reset_cause(void)
{
    return s_info.cause;
}
