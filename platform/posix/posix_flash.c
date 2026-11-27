/* SPDX-License-Identifier: MIT */
/*
 * The flash of the host build: plat_flash.h says what the application sees, posix_flash.h
 * what each function does and the simulation commands. This comment says how it is built.
 *
 * THE FILE IS THE ARRAY. The NOR model (lib/flash/nor.h) works in place on the memory it is
 * given; here that memory is the file, mapped with MAP_SHARED. Every store of a program or an
 * erase lands in the kernel's page cache at once and is seen by the next process that maps the
 * file, also after _exit() in the middle of a run. No msync() is needed for that; only a crash
 * of the host's kernel could lose the stores, as with the retained cell (posix_retained.c).
 *
 * THE LOCK AND THE END OF A RUN. The file is locked with flock() while it is open. The lock
 * belongs to the open file description, and the mapping refers to that description as the
 * descriptor does: left to the kernel, the lock would go only with the process's memory map,
 * which another process can keep alive for a moment after the process has ended
 * (posix_retained.c, THE LOCK BELONGS TO THE OPEN FILE DESCRIPTION), and the next process to
 * open the file would find it "in use". So the process gives the file up itself wherever a run
 * ends: with posix_flash_close() at an orderly end, and with posix_flash_unlock() directly
 * before the bootloader's exec and before every _exit() (the end without a kernel below;
 * plat_reset() and the shutdown guard of a FreeRTOS program, posix_lifecycle.c). Both first put
 * the file out of this process's reach and then remove the lock with flock(LOCK_UN), which acts
 * at once whatever still refers to the description. The close unmaps the file. The unlock can
 * come while another thread still operates the device (a reset from the hardware watchdog's
 * thread stops no task before the exit does), so it replaces the shared mapping in place by a
 * private copy-on-write mapping of the same file: a store made after that lands in a private
 * page, never in the file, which another process may have locked by then.
 *
 * CREATED WHOLE OR NOT AT ALL. A missing file is created with O_EXCL, locked, and then filled
 * with 0xFF by writing it from the start; the file only reaches the device's size once the
 * last byte is written. A process killed while it creates the file therefore leaves one that
 * is too short, which the next open refuses, never one of the right size that is not erased.
 *
 * THE OPERATIONS. Each call of the device (op_read, op_program, op_erase) first takes a cut
 * request left by flash.cut or posix_flash_arm_cut() and arms it in the model, then runs the
 * model, publishes the counters, and waits the operation's modelled time. A program or erase
 * that the model reports cut (its count of cuts grew) is torn already when the model returns;
 * the call waits the time up to the cut, writes the FATAL line and calls the power-loss
 * function. The model itself is touched only by the thread that runs the operations.
 *
 * THE REQUEST WORD. A request is one 64-bit atomic: bit 63 says a request is there, bits 32 to
 * 62 hold N (0: disarm), bits 0 to 31 the seed. The thread that serves simulation control
 * stores it; the next operation reads it, arms the model, publishes the armed cut, and only
 * then clears the word, with a compare-and-exchange that leaves a newer request in place. So
 * a status read sees a request either still in the word or already published, never neither,
 * and the newest request always wins. The published cut is a word of the same shape: bit 63
 * armed, bit 62 fired, the countdown and the seed.
 */
#include "posix_flash.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "image/image.h"
#include "platform.h"
#include "posix_retained.h"
#include "posix_sim.h"

/* ------------------------------------------------------------------------------ the layout */

/* The table of plat_flash.h, in its numbers; the checks below tie them together. */
#define SECTOR_SIZE  4096U
#define PAGE_SIZE    256U
#define UNIT_SIZE    8U
#define META_OFFSET  0x000000U
#define META_SIZE    0x002000U
#define SLOT_A       0x002000U
#define SLOT_B       0x102000U
#define SLOT_SIZE    0x100000U
#define FLASH_SIZE   0x202000U
#define SECTOR_COUNT (FLASH_SIZE / SECTOR_SIZE)

_Static_assert((SECTOR_SIZE & (SECTOR_SIZE - 1U)) == 0U, "the sector size is a power of two");
_Static_assert((PAGE_SIZE & (PAGE_SIZE - 1U)) == 0U, "the page size is a power of two");
_Static_assert((UNIT_SIZE & (UNIT_SIZE - 1U)) == 0U, "the unit size is a power of two");
_Static_assert((UNIT_SIZE <= PAGE_SIZE) && (PAGE_SIZE <= SECTOR_SIZE), "units in pages in sectors");
_Static_assert(META_OFFSET < SLOT_A, "the metadata come before the slots");
_Static_assert((META_OFFSET % SECTOR_SIZE) == 0U, "the metadata start at a sector");
_Static_assert(META_SIZE == (2U * SECTOR_SIZE), "lib/boot uses two metadata sectors in turn");
_Static_assert(SLOT_A == (META_OFFSET + META_SIZE), "slot A follows the metadata");
_Static_assert((SLOT_SIZE % SECTOR_SIZE) == 0U, "a slot is whole sectors");
_Static_assert(SLOT_SIZE >= 0x100000U, "a slot holds 1 MiB, the largest build of the node");
_Static_assert(SLOT_B == (SLOT_A + SLOT_SIZE), "slot B follows slot A");
_Static_assert(FLASH_SIZE == (SLOT_B + SLOT_SIZE), "the device is the three areas, nothing more");
/* The image header of lib/image is 256 bytes at the start of a slot; the payload after it
 * starts on a page, as a vector table would on a target. */
_Static_assert(((SLOT_A + 256U) % PAGE_SIZE) == 0U, "a payload starts on a page");

static const plat_flash_layout_t LAYOUT = {
    .geometry = {.size = FLASH_SIZE,
                 .sector_size = SECTOR_SIZE,
                 .page_size = PAGE_SIZE,
                 .unit_size = UNIT_SIZE},
    .meta = {.offset = META_OFFSET, .size = META_SIZE},
    .slot = {{.offset = SLOT_A, .size = SLOT_SIZE}, {.offset = SLOT_B, .size = SLOT_SIZE}},
    .image_target = IMAGE_TARGET_HOST,
};

_Static_assert(PLAT_FLASH_SLOTS == 2U, "the table has slots A and B");

/* The areas of the layout, for the erase counts: 0 the metadata, 1 + n slot n. */
#define AREAS 3U

/* ------------------------------------------------------------------------------ the state */

#define REQUEST_PRESENT (1ULL << 63U)
#define PUBLISHED_ARMED (1ULL << 63U)
#define PUBLISHED_FIRED (1ULL << 62U)
#define COUNT_MASK      0x7FFFFFFFULL

_Static_assert(POSIX_FLASH_CUT_MAX == COUNT_MASK, "N fits bits 32 to 62 of the request word");

/* The operations' side: the model and the mapping, touched only by the operating thread
 * (and by the main thread before and after it runs). */
static nor_t s_nor;
static uint32_t s_erase_counts[SECTOR_COUNT];
static flash_dev_t s_dev;
static uint8_t *s_map; /* the mapping, or NULL */
static int s_fd = -1;  /* open while the file is mapped; locked but after posix_flash_unlock() */
/* s_map and s_fd are written by the main thread only, in the open and in the close, while no
 * other thread uses the flash; posix_flash_unlock() reads them in whichever thread ends the
 * run. */
static uint32_t s_armed_seed;
static posix_flash_wait_fn s_wait;             /* NULL: sleep */
static posix_flash_power_loss_fn s_power_loss; /* NULL: the end without a kernel */
/* Programs and erases until the one that hangs, 1: the next; 0: none (posix_flash_arm_hang()).
 * Set before the operations start, then only the operating thread counts it down. */
static uint32_t s_hang_countdown;
static uint64_t s_operations; /* programs and erases started, for the line of a hang */

/* Shared with the thread that serves simulation control: atomics only. */
static _Atomic bool s_open;
static _Atomic bool s_created;
static _Atomic uint64_t s_request;   /* see THE REQUEST WORD */
static _Atomic uint64_t s_published; /* the armed or fired cut */
static _Atomic uint64_t s_reads;
static _Atomic uint64_t s_bytes_read;
static _Atomic uint64_t s_programs;
static _Atomic uint64_t s_units;
static _Atomic uint64_t s_erases;
static _Atomic uint64_t s_refused;
static _Atomic uint64_t s_busy_ns;
static _Atomic uint32_t s_area_erases[AREAS];
static _Atomic uint32_t s_max_sector_erases;

const plat_flash_layout_t *plat_flash_layout(void)
{
    return &LAYOUT;
}

const flash_dev_t *plat_flash_device(void)
{
    return atomic_load(&s_open) ? &s_dev : NULL;
}

/* -------------------------------------------------------------------------- the cut syntax */

/* A decimal number of at most `max` with nothing after it but `end`; *next past it. */
static bool parse_decimal(const char *text, char end, unsigned long long max,
                          unsigned long long *out, const char **next)
{
    unsigned long long value = 0ULL;
    size_t digits = 0U;
    bool ok = true;

    while ((text[digits] >= '0') && (text[digits] <= '9') && ok) {
        value = (value * 10ULL) + (unsigned long long)(text[digits] - '0');
        ok = (value <= max);
        digits++;
    }

    if (ok && (digits > 0U) && (text[digits] == end)) {
        *out = value;
        *next = &text[digits];
    } else {
        ok = false;
    }

    return ok;
}

bool posix_flash_parse_cut(const char *text, posix_flash_cut_t *out, char *error, size_t error_len)
{
    unsigned long long operation = 0ULL;
    unsigned long long seed = 0ULL;
    const char *rest = NULL;
    const bool has_seed = (text != NULL) && (strchr(text, ':') != NULL);
    bool ok = (text != NULL) &&
              parse_decimal(text, has_seed ? ':' : '\0', POSIX_FLASH_CUT_MAX, &operation, &rest) &&
              (operation >= 1ULL);

    if (ok && has_seed) {
        ok = parse_decimal(&rest[1], '\0', 0xFFFFFFFFULL, &seed, &rest);
    }

    if (ok) {
        out->operation = (uint32_t)operation;
        out->seed = (uint32_t)seed;
    } else {
        (void)snprintf(error, error_len,
                       "a cut is N[:SEED], N from 1 to %u and SEED from 0 to 4294967295, not '%s'",
                       (unsigned int)POSIX_FLASH_CUT_MAX, (text != NULL) ? text : "");
    }

    return ok;
}

/* ------------------------------------------------------------------------- the operations */

static uint64_t now_ns(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/* Wait `duration_ns` from now: with the wait function, polled until the time has passed; or
 * one sleep to the absolute deadline, which clock_nanosleep never ends early but for a
 * signal, after which it sleeps on. */
static void wait_for(uint32_t duration_ns)
{
    if (duration_ns != 0U) {
        const uint64_t deadline = now_ns() + duration_ns;

        if (s_wait != NULL) {
            while (now_ns() < deadline) {
                s_wait();
            }
        } else {
            const struct timespec until = {.tv_sec = (time_t)(deadline / 1000000000ULL),
                                           .tv_nsec = (long)(deadline % 1000000000ULL)};

            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR) {
            }
        }
    }
}

/* The cut as the model has it now, in the published word's shape. */
static uint64_t cut_word(void)
{
    const uint32_t countdown = nor_cut_countdown(&s_nor);
    uint64_t word = 0ULL;

    if (countdown != 0U) {
        word = PUBLISHED_ARMED | ((uint64_t)countdown << 32U) | (uint64_t)s_armed_seed;
    } else if (!nor_powered(&s_nor)) {
        word = PUBLISHED_FIRED;
    } else {
        word = 0ULL;
    }

    return word;
}

/* Arm the model with a request that is waiting, if any (see THE REQUEST WORD). */
static void take_request(void)
{
    uint64_t request = atomic_load(&s_request);

    if ((request & REQUEST_PRESENT) != 0ULL) {
        const uint32_t operation = (uint32_t)((request >> 32U) & COUNT_MASK);
        const uint32_t seed = (uint32_t)(request & 0xFFFFFFFFULL);

        if (operation == 0U) {
            nor_disarm_cut(&s_nor);
            s_armed_seed = 0U;
        } else {
            nor_arm_cut(&s_nor, operation, NOR_CUT_SEEDED_POSITION, seed);
            s_armed_seed = seed;
        }

        atomic_store(&s_published, cut_word());
        (void)atomic_compare_exchange_strong(&s_request, &request, 0ULL);
    }
}

static void publish(void)
{
    nor_stats_t stats;

    nor_get_stats(&s_nor, &stats);
    atomic_store(&s_reads, stats.reads);
    atomic_store(&s_bytes_read, stats.bytes_read);
    atomic_store(&s_programs, stats.programs);
    atomic_store(&s_units, stats.units_programmed);
    atomic_store(&s_erases, stats.erases);
    atomic_store(&s_refused, stats.refused);
    atomic_store(&s_busy_ns, stats.busy_ns);
    atomic_store(&s_published, cut_word());
}

static const char *op_name(nor_op_t op)
{
    return (op == NOR_OP_ERASE) ? "erase" : "program";
}

/* Report the cut on stderr with write(), which takes no lock and works from any thread. */
static void report_cut(void)
{
    nor_cut_t cut;
    char text[256];

    (void)nor_last_cut(&s_nor, &cut);

    const int len = snprintf(
        text, sizeof(text),
        "FATAL flash: power cut in operation %llu (%s of %lu bytes at 0x%06lx, position %lu of "
        "%lu, seed %lu; %lu of %lu bits changed)\n",
        (unsigned long long)cut.operation, op_name(cut.op), (unsigned long)cut.len,
        (unsigned long)cut.addr, (unsigned long)cut.position, (unsigned long)cut.positions,
        (unsigned long)cut.seed, (unsigned long)cut.done, (unsigned long)cut.transitions);

    if (len > 0) {
        const size_t count = ((size_t)len < sizeof(text)) ? (size_t)len : (sizeof(text) - 1U);

        (void)!write(STDERR_FILENO, text, count);
    }
}

/* The end of a run by a loss of supply without a kernel: the RAM of the retained cell is
 * lost, the locks of the cell and of the flash file are released (THE LOCK AND THE END OF A
 * RUN; the cut operation's stores are in the file, and nothing writes after this), and the
 * process ends with the status of a power loss. */
static void end_without_kernel(void)
{
    posix_retained_note_reset(PLAT_RESET_POWER_LOSS);
    posix_retained_unlock();
    posix_flash_unlock();
    _exit((int)PLAT_RESET_POWER_LOSS);
}

/* The operation that was armed to hang (see A HANG in the header): its busy flag never clears. */
static void hang_for_ever(void)
{
    char text[128];
    const int len = snprintf(text, sizeof(text),
                             "flash: operation %llu hangs: its busy flag never clears "
                             "(--flash-hang)\n",
                             (unsigned long long)s_operations);

    if (len > 0) {
        const size_t count = ((size_t)len < sizeof(text)) ? (size_t)len : (sizeof(text) - 1U);

        (void)!write(STDERR_FILENO, text, count);
    }

    for (;;) {
        if (s_wait != NULL) {
            s_wait();
        } else {
            const struct timespec nap = {0, 1000000L};

            (void)nanosleep(&nap, NULL);
        }
    }
}

/* After a program or erase: wait its time; if it was cut, the supply is gone; if it was armed
 * to hang, it never returns. */
static void finish(uint32_t cuts_before)
{
    nor_stats_t stats;

    nor_get_stats(&s_nor, &stats);
    s_operations++;

    if (s_hang_countdown != 0U) {
        s_hang_countdown--;

        if (s_hang_countdown == 0U) {
            hang_for_ever();
        }
    }

    wait_for(nor_last_duration_ns(&s_nor));

    if (stats.cuts != cuts_before) {
        report_cut();

        if (s_power_loss != NULL) {
            s_power_loss();
        } else {
            end_without_kernel();
        }
    }
}

static uint32_t cuts_so_far(void)
{
    nor_stats_t stats;

    nor_get_stats(&s_nor, &stats);
    return stats.cuts;
}

static flash_status_t op_read(void *ctx, uint32_t addr, uint8_t *dst, uint32_t len)
{
    (void)ctx;
    take_request();

    const flash_status_t status = nor_read(&s_nor, addr, dst, len);

    publish();
    return status;
}

static flash_status_t op_program(void *ctx, uint32_t addr, const uint8_t *src, uint32_t len)
{
    (void)ctx;
    take_request();

    const uint32_t cuts = cuts_so_far();
    const flash_status_t status = nor_program(&s_nor, addr, src, len);

    publish();
    finish(cuts);
    return status;
}

/* The area of the layout a sector lies in, for the erase counts. */
static uint32_t area_of(uint32_t addr)
{
    uint32_t area = 0U;

    if (addr >= SLOT_B) {
        area = 2U;
    } else if (addr >= SLOT_A) {
        area = 1U;
    } else {
        area = 0U;
    }

    return area;
}

static flash_status_t op_erase(void *ctx, uint32_t addr)
{
    nor_stats_t before;

    (void)ctx;
    take_request();
    nor_get_stats(&s_nor, &before);

    const flash_status_t status = nor_erase(&s_nor, addr);
    nor_stats_t after;

    nor_get_stats(&s_nor, &after);

    if (after.erases != before.erases) {
        const uint32_t count = nor_sector_erases(&s_nor, addr / SECTOR_SIZE);

        (void)atomic_fetch_add(&s_area_erases[area_of(addr)], 1U);

        if (count > atomic_load(&s_max_sector_erases)) {
            atomic_store(&s_max_sector_erases, count);
        }
    }

    publish();
    finish(before.cuts);
    return status;
}

/* ------------------------------------------------------------------------ simulation control */

/* The first word of `args` into `word`; false if there is none, it is too long, or another
 * word follows. */
static bool only_word(const char *args, char *word, size_t word_len)
{
    const char *start = args;
    size_t length = 0U;

    while (*start == ' ') {
        start++;
    }

    while ((start[length] != ' ') && (start[length] != '\0')) {
        length++;
    }

    const char *after = &start[length];

    while (*after == ' ') {
        after++;
    }

    const bool ok = (length > 0U) && (length < word_len) && (*after == '\0');

    if (ok) {
        (void)memcpy(word, start, length);
        word[length] = '\0';
    }

    return ok;
}

static bool status_reply(char *reply, size_t reply_len)
{
    posix_flash_info_t flash;
    int length;

    posix_flash_get_info(&flash);

    if (!flash.open) {
        length = snprintf(reply, reply_len, "flash=off");
    } else {
        const char *state = "none";

        if (flash.cut_in != 0U) {
            state = "armed";
        } else if (flash.cut_fired) {
            state = "fired";
        } else {
            /* none armed */
        }

        length = snprintf(
            reply, reply_len,
            "flash=on size=%lu reads=%llu bytes_read=%llu programs=%llu units_programmed=%llu "
            "erases=%llu refused=%llu busy_ns=%llu erases_meta=%lu erases_slot_a=%lu "
            "erases_slot_b=%lu max_sector_erases=%lu cut=%s cut_in=%lu cut_seed=%lu",
            (unsigned long)FLASH_SIZE, (unsigned long long)flash.reads,
            (unsigned long long)flash.bytes_read, (unsigned long long)flash.programs,
            (unsigned long long)flash.units_programmed, (unsigned long long)flash.erases,
            (unsigned long long)flash.refused, (unsigned long long)flash.busy_ns,
            (unsigned long)flash.erases_meta, (unsigned long)flash.erases_slot[0],
            (unsigned long)flash.erases_slot[1], (unsigned long)flash.max_sector_erases, state,
            (unsigned long)flash.cut_in, (unsigned long)flash.cut_seed);
    }

    /* A reply cut short would lose the keys at its end without anyone noticing. */
    const bool fits = (length >= 0) && ((size_t)length < reply_len);

    if (!fits) {
        (void)snprintf(reply, reply_len, "the status does not fit into a reply");
    }

    return fits;
}

#define CUT_USAGE "usage: flash.cut N[:SEED] | flash.cut off"

static bool cut_command(const char *args, char *reply, size_t reply_len)
{
    char word[32];
    char error[160];
    posix_flash_cut_t cut = {0U, 0U};
    bool ok = false;

    if (!atomic_load(&s_open)) {
        (void)snprintf(reply, reply_len, "no flash in this run (started without --flash-file)");
    } else if (!only_word(args, word, sizeof(word))) {
        (void)snprintf(reply, reply_len, CUT_USAGE);
    } else if (strcmp(word, "off") == 0) {
        atomic_store(&s_request, REQUEST_PRESENT);
        (void)snprintf(reply, reply_len, "cut=none t_ns=%llu", (unsigned long long)now_ns());
        ok = true;
    } else if (!posix_flash_parse_cut(word, &cut, error, sizeof(error))) {
        (void)snprintf(reply, reply_len, "%s (%s)", error, CUT_USAGE);
    } else {
        posix_flash_arm_cut(&cut);
        (void)snprintf(reply, reply_len, "cut=armed cut_in=%lu cut_seed=%lu t_ns=%llu",
                       (unsigned long)cut.operation, (unsigned long)cut.seed,
                       (unsigned long long)now_ns());
        ok = true;
    }

    return ok;
}

/* Commands under "flash.": status | cut N[:SEED] | cut off. Runs in the thread that serves
 * simulation control. */
static bool sim_flash_handler(const char *verb, const char *args, char *reply, size_t reply_len)
{
    bool ok = false;

    if (strcmp(verb, "status") == 0) {
        if (args[0] != '\0') {
            (void)snprintf(reply, reply_len, "usage: flash.status");
        } else {
            ok = status_reply(reply, reply_len);
        }
    } else if (strcmp(verb, "cut") == 0) {
        ok = cut_command(args, reply, reply_len);
    } else {
        (void)snprintf(reply, reply_len,
                       "unknown command 'flash.%s' (flash.status, flash.cut N[:SEED]|off)", verb);
    }

    return ok;
}

/* ------------------------------------------------------------------------ opening the file */

static bool fail(char *error, size_t error_len, const char *what, const char *path, int fd)
{
    const int saved = errno;

    if (fd >= 0) {
        (void)close(fd);
    }

    (void)snprintf(error, error_len, "cannot %s --flash-file %s: %s", what, path, strerror(saved));
    return false;
}

/* Fill a file just created with 0xFF from its start to the device's size. */
static bool fill_erased(int fd)
{
    static uint8_t block[64U * 1024U];
    off_t done = 0;
    bool ok = true;

    (void)memset(block, 0xFF, sizeof(block));

    while (ok && (done < (off_t)FLASH_SIZE)) {
        const size_t want = (((off_t)FLASH_SIZE - done) < (off_t)sizeof(block))
                                ? (size_t)((off_t)FLASH_SIZE - done)
                                : sizeof(block);
        const ssize_t written = pwrite(fd, block, want, done);

        if (written > 0) {
            done += written;
        } else if ((written < 0) && (errno == EINTR)) {
            /* try again */
        } else {
            ok = false;
        }
    }

    return ok;
}

static void reset_counters(void)
{
    atomic_store(&s_request, 0ULL);
    atomic_store(&s_published, 0ULL);
    atomic_store(&s_reads, 0ULL);
    atomic_store(&s_bytes_read, 0ULL);
    atomic_store(&s_programs, 0ULL);
    atomic_store(&s_units, 0ULL);
    atomic_store(&s_erases, 0ULL);
    atomic_store(&s_refused, 0ULL);
    atomic_store(&s_busy_ns, 0ULL);

    for (uint32_t area = 0U; area < AREAS; area++) {
        atomic_store(&s_area_erases[area], 0U);
    }

    atomic_store(&s_max_sector_erases, 0U);
    s_armed_seed = 0U;
    s_hang_countdown = 0U;
    s_operations = 0U;
}

/* Open (creating it erased if missing) and lock the file; *fd_out the descriptor. */
static bool open_locked(const char *path, int *fd_out, bool *created, char *error, size_t error_len)
{
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    bool ok = true;

    *created = (fd >= 0);

    if ((fd < 0) && (errno == EEXIST)) {
        fd = open(path, O_RDWR | O_CLOEXEC);
    }

    if (fd < 0) {
        ok = fail(error, error_len, "open", path, -1);
    } else if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            (void)close(fd);
            (void)snprintf(error, error_len, "--flash-file %s is in use by another process", path);
            ok = false;
        } else {
            ok = fail(error, error_len, "lock", path, fd);
        }
    } else if (*created && !fill_erased(fd)) {
        const int saved = errno;

        (void)unlink(path); /* a file of the wrong size would only be refused next time */
        errno = saved;
        ok = fail(error, error_len, "create", path, fd);
    } else {
        *fd_out = fd;
    }

    return ok;
}

bool posix_flash_open(const char *path, const nor_timing_t *timing, char *error, size_t error_len)
{
    const nor_timing_t typical = {.page_program_ns = POSIX_FLASH_PAGE_PROGRAM_NS,
                                  .sector_erase_ns = POSIX_FLASH_SECTOR_ERASE_NS};
    struct stat info;
    bool created = false;
    int fd = -1;

    if (atomic_load(&s_open)) {
        (void)snprintf(error, error_len, "a flash is already open");
        return false;
    }

    posix_sim_register("flash.", sim_flash_handler);
    reset_counters();
    atomic_store(&s_created, false);

    if (path == NULL) {
        return true;
    }

    if (!open_locked(path, &fd, &created, error, error_len)) {
        return false;
    }

    if (fstat(fd, &info) != 0) {
        return fail(error, error_len, "inspect", path, fd);
    }

    if (!S_ISREG(info.st_mode)) {
        errno = EINVAL;
        return fail(error, error_len, "use (not a regular file)", path, fd);
    }

    if (info.st_size != (off_t)FLASH_SIZE) {
        (void)close(fd);
        (void)snprintf(error, error_len,
                       "--flash-file %s has %lld bytes, a flash file has exactly %lu (remove it "
                       "to start from an erased flash)",
                       path, (long long)info.st_size, (unsigned long)FLASH_SIZE);
        return false;
    }

    void *const map = mmap(NULL, FLASH_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (map == MAP_FAILED) {
        return fail(error, error_len, "map", path, fd);
    }

    const nor_config_t config = {.geometry = LAYOUT.geometry,
                                 .timing = (timing != NULL) ? *timing : typical,
                                 .memory = (uint8_t *)map,
                                 .erase_counts = s_erase_counts};

    /* The layout's geometry is valid by the checks at the top, so nor_init() accepts it. */
    (void)nor_init(&s_nor, &config);
    nor_device(&s_nor, &s_dev);
    s_dev.read = op_read;
    s_dev.program = op_program;
    s_dev.erase = op_erase;
    s_map = (uint8_t *)map;
    s_fd = fd;
    atomic_store(&s_created, created);
    atomic_store(&s_open, true);
    return true;
}

void posix_flash_arm_cut(const posix_flash_cut_t *cut)
{
    if (atomic_load(&s_open) && (cut != NULL) && (cut->operation >= 1U) &&
        (cut->operation <= POSIX_FLASH_CUT_MAX)) {
        atomic_store(&s_request,
                     REQUEST_PRESENT | ((uint64_t)cut->operation << 32U) | (uint64_t)cut->seed);
    }
}

void posix_flash_arm_hang(uint32_t operation)
{
    if (atomic_load(&s_open) && (operation <= POSIX_FLASH_CUT_MAX)) {
        s_hang_countdown = operation;
    }
}

void posix_flash_set_wait(posix_flash_wait_fn wait)
{
    s_wait = wait;
}

void posix_flash_set_power_loss(posix_flash_power_loss_fn end)
{
    s_power_loss = end;
}

void posix_flash_close(void)
{
    if (atomic_load(&s_open)) {
        atomic_store(&s_open, false);
        (void)munmap(s_map, FLASH_SIZE);
        s_map = NULL;
        /* The lock explicitly, and only then the descriptor: the close alone removes the lock
         * only when nothing else refers to the open file description, and the memory map of a
         * process forked while the file was mapped does, for as long as that map exists. */
        (void)flock(s_fd, LOCK_UN);
        (void)close(s_fd);
        s_fd = -1;
    }
}

void posix_flash_unlock(void)
{
    if (atomic_load(&s_open)) {
        /* Out of reach first (THE LOCK AND THE END OF A RUN): MAP_FIXED puts the private
         * mapping in the shared one's place in one step, so a thread in the middle of a
         * program finds the address mapped at every instant, and its stores from here on stay
         * in this process. Only then the lock: if the kernel refuses the mapping, the shared
         * one may still be there and this process could still write the file, so the lock
         * stays, until the memory map goes. A second call maps again and finds no lock to
         * remove. */
        if (mmap(s_map, FLASH_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, s_fd, 0) !=
            MAP_FAILED) {
            /* LOCK_UN removes the lock of the open file description at once, also while the
             * mapping still refers to the description. */
            (void)flock(s_fd, LOCK_UN);
        }
    }
}

void posix_flash_get_info(posix_flash_info_t *out)
{
    /* The request first: if the operations have taken it, its effect has been published before
     * it was cleared (see THE REQUEST WORD). A request not yet taken is what the next operation
     * will arm. */
    const uint64_t request = atomic_load(&s_request);
    const uint64_t cut = atomic_load(&s_published);
    const uint64_t word = ((request & REQUEST_PRESENT) != 0ULL) ? request : cut;

    (void)memset(out, 0, sizeof(*out));
    out->open = atomic_load(&s_open);
    out->created = atomic_load(&s_created);
    out->reads = atomic_load(&s_reads);
    out->bytes_read = atomic_load(&s_bytes_read);
    out->programs = atomic_load(&s_programs);
    out->units_programmed = atomic_load(&s_units);
    out->erases = atomic_load(&s_erases);
    out->refused = atomic_load(&s_refused);
    out->busy_ns = atomic_load(&s_busy_ns);
    out->erases_meta = atomic_load(&s_area_erases[0]);
    out->erases_slot[0] = atomic_load(&s_area_erases[1]);
    out->erases_slot[1] = atomic_load(&s_area_erases[2]);
    out->max_sector_erases = atomic_load(&s_max_sector_erases);

    /* Bit 62 is the top bit of N in an armed word, and the fired flag only in a word without
     * bit 63. */
    if ((word & REQUEST_PRESENT) != 0ULL) {
        out->cut_in = (uint32_t)((word >> 32U) & COUNT_MASK);
        out->cut_seed = (out->cut_in != 0U) ? (uint32_t)(word & 0xFFFFFFFFULL) : 0U;
    } else {
        out->cut_fired = ((word & PUBLISHED_FIRED) != 0ULL);
    }
}
