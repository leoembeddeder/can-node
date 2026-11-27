/* SPDX-License-Identifier: MIT */
/*
 * The retained-RAM cell of the host build (platform/posix/posix_retained.c) without a kernel:
 * the test plays the start-up code and plat_reset() of successive runs of the node. Each
 * "start" is a posix_retained_open() and each "reset" a posix_retained_note_reset() followed by
 * posix_retained_close(), which is what the process's exit does to the mapping. Between them
 * the test reads and writes the file itself, as the bench does, from its own reading of the
 * layout in plat_retained.h.
 *
 * The lock is tested against another process: the test forks, and the child opens the file
 * anew and tries to lock it (or holds the lock until the test lets it go). The bootloader's
 * release of the lock before its exec (posix_retained_unlock()) is shown to work while the open
 * file description, and with it the mapping, still exists: that is the state an exec leaves
 * when another process holds a reference to the old memory map (docs/bug-log.md, 010). The
 * close is shown to release the lock while a process forked from the test still maps the cell:
 * the state an exit leaves in the same case (docs/bug-log.md, 012).
 *
 * What a real reset of the node does to the cell (plat_reset() from a task, and from the
 * hardware watchdog's thread) is tested by tests/platform/wdg_selftest.c, and a cell written by
 * Python from its own CRC by tests/system/test_retained.py.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "crc/crc.h"
#include "map_keeper.h"
#include "plat_retained.h"
#include "posix_retained.h"
#include "unity.h"

static char s_dir[256];
static char s_path[300];
static char s_error[512];

/* ------------------------------------------------------------------------------ the file */

static void write_bytes(const void *data, size_t length)
{
    FILE *file = fopen(s_path, "wb");

    if (file == NULL) {
        TEST_FAIL_MESSAGE("cannot create the cell's file");
    } else {
        const size_t written = fwrite(data, 1U, length, file);
        const int closed = fclose(file);

        TEST_ASSERT_EQUAL_size_t(length, written);
        TEST_ASSERT_EQUAL_INT(0, closed);
    }
}

static size_t file_size(void)
{
    struct stat info;

    TEST_ASSERT_EQUAL_INT(0, stat(s_path, &info));
    return (size_t)info.st_size;
}

static plat_retained_cell_t read_cell(void)
{
    plat_retained_cell_t cell = {0};
    FILE *file = fopen(s_path, "rb");

    if (file == NULL) {
        TEST_FAIL_MESSAGE("cannot open the cell's file");
    } else {
        const size_t read = fread(&cell, sizeof(cell), 1U, file);
        const int closed = fclose(file);

        TEST_ASSERT_EQUAL_size_t(1U, read);
        TEST_ASSERT_EQUAL_INT(0, closed);
    }

    return cell;
}

/* The CRC from its definition in plat_retained.h: CRC-32/ISO-HDLC over the 32 bytes before
 * it, here through the one-shot helper of lib/crc on a byte copy. */
static uint32_t expected_crc(const plat_retained_cell_t *cell)
{
    uint8_t bytes[32];

    (void)memcpy(bytes, cell, sizeof(bytes));
    return crc32_compute(bytes, sizeof(bytes));
}

/* A valid cell with the given contents. */
static plat_retained_cell_t make_cell(uint32_t reason, uint32_t resets)
{
    plat_retained_cell_t cell;

    (void)memset(&cell, 0, sizeof(cell));
    cell.magic = 0x524E4346U; /* the bytes 'F' 'C' 'N' 'R' */
    cell.version = 1U;
    cell.size = 36U;
    cell.reset_reason = reason;
    cell.resets = resets;
    cell.reserved[0] = 0x11111111U;
    cell.reserved[1] = 0x22222222U;
    cell.reserved[2] = 0x33333333U;
    cell.reserved[3] = 0x44444444U;
    cell.crc = expected_crc(&cell);
    return cell;
}

/* A cell as after power-on: header, reason POWER_ON (0), every other word 0, CRC right. */
static void assert_fresh_cell(void)
{
    const plat_retained_cell_t cell = read_cell();

    TEST_ASSERT_EQUAL_size_t(36U, file_size());
    TEST_ASSERT_EQUAL_HEX32(0x524E4346U, cell.magic);
    TEST_ASSERT_EQUAL_UINT16(1U, cell.version);
    TEST_ASSERT_EQUAL_UINT16(36U, cell.size);
    TEST_ASSERT_EQUAL_UINT32(0U, cell.reset_reason);
    TEST_ASSERT_EQUAL_UINT32(0U, cell.resets);

    for (uint32_t i = 0U; i < PLAT_RETAINED_RESERVED_WORDS; i++) {
        TEST_ASSERT_EQUAL_HEX32(0U, cell.reserved[i]);
    }

    TEST_ASSERT_EQUAL_HEX32(expected_crc(&cell), cell.crc);
}

/* One start of the node: open, and return what it found. */
static posix_retained_info_t start(void)
{
    posix_retained_info_t info;

    TEST_ASSERT_TRUE_MESSAGE(posix_retained_open(s_path, s_error, sizeof(s_error)), s_error);
    posix_retained_get_info(&info);
    TEST_ASSERT_EQUAL_INT((int)info.cause, (int)plat_reset_cause());
    return info;
}

/* One reset of the node: what plat_reset() does to the cell, then the end of the process. */
static void reset(plat_reset_reason_t reason)
{
    posix_retained_note_reset(reason);
    posix_retained_close();
}

/* ---------------------------------------------------------------------- another process */

/* What lock_from_another_process() found. */
#define LOCKED 0 /* the other process got the lock */
#define IN_USE 1 /* flock() refused it with EWOULDBLOCK: another description holds it */
#define BROKEN 2 /* the file could not be opened, or flock() failed otherwise */

/* Another process tries to lock the cell's file: a child opens it anew (an open file
 * description of its own) and tries flock(LOCK_EX | LOCK_NB), then ends, which gives up a lock
 * it got. */
static int lock_from_another_process(void)
{
    int status = -1;
    const pid_t pid = fork();

    TEST_ASSERT_TRUE(pid >= 0);

    if (pid == 0) {
        const int fd = open(s_path, O_RDWR | O_CLOEXEC);
        int found = BROKEN;

        if (fd >= 0) {
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                found = LOCKED;
            } else if (errno == EWOULDBLOCK) {
                found = IN_USE;
            } else {
                found = BROKEN;
            }
        }

        _exit(found);
    }

    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    return WEXITSTATUS(status);
}

/* A child that holds the lock of the cell's file until release_holder(). */
typedef struct {
    pid_t pid;
    int release; /* the write end of a pipe the child reads until it is closed */
} holder_t;

static holder_t hold_from_another_process(void)
{
    int ready[2];
    int release[2];
    char answer = 'n';

    TEST_ASSERT_EQUAL_INT(0, pipe(ready));
    TEST_ASSERT_EQUAL_INT(0, pipe(release));

    const pid_t pid = fork();

    TEST_ASSERT_TRUE(pid >= 0);

    if (pid == 0) {
        const int fd = open(s_path, O_RDWR | O_CLOEXEC);

        (void)close(ready[0]);
        (void)close(release[1]);
        answer = ((fd >= 0) && (flock(fd, LOCK_EX | LOCK_NB) == 0)) ? 'y' : 'n';
        (void)!write(ready[1], &answer, 1U);

        if (answer == 'y') {
            (void)!read(release[0], &answer, 1U); /* 0 bytes once the parent closes its end */
        }

        _exit(0);
    }

    (void)close(ready[1]);
    (void)close(release[0]);

    const ssize_t got = read(ready[0], &answer, 1U);

    (void)close(ready[0]);

    if ((got != 1) || (answer != 'y')) {
        (void)close(release[1]);
        (void)waitpid(pid, NULL, 0);
        TEST_FAIL_MESSAGE("another process could not lock the cell's file");
    }

    return (holder_t){.pid = pid, .release = release[1]};
}

/* Let the holder go: it ends, and its lock goes with its descriptor. */
static void release_holder(holder_t holder)
{
    int status = -1;

    (void)close(holder.release);
    TEST_ASSERT_EQUAL_INT(holder.pid, waitpid(holder.pid, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status) && (WEXITSTATUS(status) == 0));
}

#define ASSERT_START(cause_name, verdict_text, reset_count, info)                                  \
    do {                                                                                           \
        TEST_ASSERT_EQUAL_STRING((cause_name), posix_reset_reason_name((info).cause));             \
        TEST_ASSERT_EQUAL_STRING((verdict_text), (info).verdict);                                  \
        TEST_ASSERT_EQUAL_UINT32((reset_count), (info).resets);                                    \
        TEST_ASSERT_TRUE((info).open);                                                             \
    } while (0)

void setUp(void)
{
    (void)unlink(s_path);
}

void tearDown(void)
{
    posix_retained_close();
    (void)unlink(s_path);
}

/* ------------------------------------------------------------------------------ the tests */

static void test_without_a_file_every_start_is_a_power_on(void)
{
    posix_retained_info_t info;

    TEST_ASSERT_TRUE(posix_retained_open(NULL, s_error, sizeof(s_error)));
    posix_retained_get_info(&info);
    TEST_ASSERT_FALSE(info.open);
    TEST_ASSERT_EQUAL_STRING("no cell", info.verdict);
    TEST_ASSERT_EQUAL_INT((int)PLAT_RESET_POWER_ON, (int)plat_reset_cause());
    posix_retained_note_reset(PLAT_RESET_WATCHDOG); /* nothing to write: no effect */
    posix_retained_close();
    TEST_ASSERT_TRUE(posix_retained_open(NULL, s_error, sizeof(s_error)));
    TEST_ASSERT_EQUAL_INT((int)PLAT_RESET_POWER_ON, (int)plat_reset_cause());
}

static void test_a_missing_file_is_a_power_on_and_becomes_a_fresh_cell(void)
{
    const posix_retained_info_t info = start();

    ASSERT_START("POWER_ON", "missing", 0U, info);
    assert_fresh_cell();
}

/* A cell that records a reset: its reason is the cause of this start, once. The count and the
 * reserved words are kept; the reason goes back to POWER_ON, so that a run ending without a
 * reset makes the next start a power-on. */
static void test_a_valid_cell_gives_its_reason_once_and_keeps_its_words(void)
{
    const plat_retained_cell_t before = make_cell((uint32_t)PLAT_RESET_WATCHDOG, 3U);
    posix_retained_info_t info;
    plat_retained_cell_t after;

    write_bytes(&before, sizeof(before));
    info = start();
    ASSERT_START("WATCHDOG", "reset", 3U, info);
    TEST_ASSERT_EQUAL_INT((int)PLAT_RESET_WATCHDOG, (int)plat_reset_cause());

    after = read_cell();
    TEST_ASSERT_EQUAL_UINT32(0U, after.reset_reason);
    TEST_ASSERT_EQUAL_UINT32(3U, after.resets);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.reserved, after.reserved, PLAT_RETAINED_RESERVED_WORDS);
    TEST_ASSERT_EQUAL_HEX32(expected_crc(&after), after.crc);

    /* An end without a reset (an orderly stop, a killed process) and the next start. */
    posix_retained_close();
    info = start();
    ASSERT_START("POWER_ON", "no reset", 0U, info);
    assert_fresh_cell();
}

/* Every way a cell can be wrong is a power-on, and the cell is written afresh. */
static void test_a_cell_that_is_not_valid_is_a_power_on(void)
{
    static const struct {
        const char *what;
        const char *verdict;
    } cases[] = {{"empty file", "size"},
                 {"short file", "size"},
                 {"long file", "size"},
                 {"magic", "magic"},
                 {"version", "version"},
                 {"size member", "version"},
                 {"a bit of a reserved word", "crc"},
                 {"the CRC", "crc"},
                 {"reason POWER_LOSS", "reason"},
                 {"reason 99", "reason"},
                 {"reason POWER_ON", "no reset"}};
    uint8_t bytes[64];

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        plat_retained_cell_t cell = make_cell((uint32_t)PLAT_RESET_ASSERT, 5U);
        size_t length = sizeof(cell);

        switch (i) {
        case 0U:
            length = 0U;
            break;
        case 1U:
            length = sizeof(cell) - 1U;
            break;
        case 2U:
            length = sizeof(cell) + 1U;
            break;
        case 3U:
            cell.magic ^= 1U;
            cell.crc = expected_crc(&cell);
            break;
        case 4U:
            cell.version = 2U;
            cell.crc = expected_crc(&cell);
            break;
        case 5U:
            cell.size = 40U;
            cell.crc = expected_crc(&cell);
            break;
        case 6U:
            cell.reserved[2] ^= 0x00010000U;
            break;
        case 7U:
            cell.crc ^= 0x80000000U;
            break;
        case 8U:
            cell.reset_reason = (uint32_t)PLAT_RESET_POWER_LOSS;
            cell.crc = expected_crc(&cell);
            break;
        case 9U:
            cell.reset_reason = 99U;
            cell.crc = expected_crc(&cell);
            break;
        default:
            cell.reset_reason = (uint32_t)PLAT_RESET_POWER_ON;
            cell.crc = expected_crc(&cell);
            break;
        }

        (void)memset(bytes, 0xEE, sizeof(bytes));
        (void)memcpy(bytes, &cell, sizeof(cell));
        write_bytes(bytes, length);

        const posix_retained_info_t info = start();

        TEST_ASSERT_EQUAL_STRING_MESSAGE("POWER_ON", posix_reset_reason_name(info.cause),
                                         cases[i].what);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].verdict, info.verdict, cases[i].what);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, info.resets, cases[i].what);
        assert_fresh_cell();
        posix_retained_close();
    }
}

/* What plat_reset() writes: the reason, one more reset, a CRC over both; the next start
 * reports the reason. Resets accumulate until a run ends without one. */
static void test_resets_are_recorded_and_counted_until_a_run_ends_without_one(void)
{
    static const plat_reset_reason_t reasons[] = {
        PLAT_RESET_REQUESTED, PLAT_RESET_WATCHDOG,       PLAT_RESET_IMAGE_FAULT,
        PLAT_RESET_ASSERT,    PLAT_RESET_STACK_OVERFLOW, PLAT_RESET_MALLOC_FAILED};
    posix_retained_info_t info = start();

    ASSERT_START("POWER_ON", "missing", 0U, info);

    for (uint32_t i = 0U; i < (sizeof(reasons) / sizeof(reasons[0])); i++) {
        plat_retained_cell_t cell;

        reset(reasons[i]);
        cell = read_cell();
        TEST_ASSERT_EQUAL_UINT32((uint32_t)reasons[i], cell.reset_reason);
        TEST_ASSERT_EQUAL_UINT32(i + 1U, cell.resets);
        TEST_ASSERT_EQUAL_HEX32(expected_crc(&cell), cell.crc);

        info = start();
        TEST_ASSERT_EQUAL_INT((int)reasons[i], (int)info.cause);
        TEST_ASSERT_EQUAL_STRING("reset", info.verdict);
        TEST_ASSERT_EQUAL_UINT32(i + 1U, info.resets);
    }

    posix_retained_close();
    info = start();
    ASSERT_START("POWER_ON", "no reset", 0U, info);
}

/* plat_reset(PLAT_RESET_POWER_LOSS) takes the cell with it: the next start is a power-on. */
static void test_a_power_loss_destroys_the_cell(void)
{
    posix_retained_info_t info;
    plat_retained_cell_t cell;
    const plat_retained_cell_t zero = {0};

    (void)start();
    reset(PLAT_RESET_REQUESTED);
    info = start();
    ASSERT_START("REQUESTED", "reset", 1U, info);
    reset(PLAT_RESET_POWER_LOSS);
    cell = read_cell();
    TEST_ASSERT_EQUAL_MEMORY(&zero, &cell, sizeof(cell));
    info = start();
    ASSERT_START("POWER_ON", "magic", 0U, info);
}

/* The bench's way to cut the power: remove the file. */
static void test_a_removed_file_is_a_power_on(void)
{
    posix_retained_info_t info;

    (void)start();
    reset(PLAT_RESET_ASSERT);
    TEST_ASSERT_EQUAL_INT(0, unlink(s_path));
    info = start();
    ASSERT_START("POWER_ON", "missing", 0U, info);
}

/* One board, one cell: while a node has the file open, nobody else can lock it, and a second
 * open in the same process is refused. */
static void test_the_cell_is_locked_while_it_is_open(void)
{
    int fd;

    (void)start();
    fd = open(s_path, O_RDWR | O_CLOEXEC);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(-1, flock(fd, LOCK_EX | LOCK_NB));
    TEST_ASSERT_EQUAL_INT(EWOULDBLOCK, errno);
    TEST_ASSERT_FALSE(posix_retained_open(s_path, s_error, sizeof(s_error)));
    TEST_ASSERT_EQUAL_STRING("a retained cell is already open", s_error);
    posix_retained_close();
    TEST_ASSERT_EQUAL_INT(0, flock(fd, LOCK_EX | LOCK_NB));

    /* Now the test holds the lock: the node cannot start on this file. */
    TEST_ASSERT_FALSE(posix_retained_open(s_path, s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL(strstr(s_error, "is in use by another node"));
    TEST_ASSERT_EQUAL_INT(0, close(fd));
}

static void test_a_file_that_cannot_be_used_is_an_error_with_a_reason(void)
{
    char path[400];

    (void)snprintf(path, sizeof(path), "%s/no/such/dir/cell", s_dir);
    TEST_ASSERT_FALSE(posix_retained_open(path, s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_error, "cannot open --retained-file"), s_error);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_error, "No such file or directory"), s_error);

    TEST_ASSERT_FALSE(posix_retained_open(s_dir, s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_error, s_dir), s_error);
}

static void test_the_reasons_and_their_names(void)
{
    static const struct {
        const char *name;
        plat_reset_reason_t reason;
        bool valid;
        bool recorded;
    } cases[] = {{"POWER_ON", PLAT_RESET_POWER_ON, false, false},
                 {"REQUESTED", PLAT_RESET_REQUESTED, true, true},
                 {"WATCHDOG", PLAT_RESET_WATCHDOG, true, true},
                 {"POWER_LOSS", PLAT_RESET_POWER_LOSS, true, false},
                 {"IMAGE_FAULT", PLAT_RESET_IMAGE_FAULT, true, true},
                 {"ASSERT", PLAT_RESET_ASSERT, true, true},
                 {"STACK_OVERFLOW", PLAT_RESET_STACK_OVERFLOW, true, true},
                 {"MALLOC_FAILED", PLAT_RESET_MALLOC_FAILED, true, true},
                 /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): on purpose */
                 {"unknown", (plat_reset_reason_t)14, false, false},
                 /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): on purpose */
                 {"unknown", (plat_reset_reason_t)63, false, false}};

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        TEST_ASSERT_EQUAL_STRING(cases[i].name, posix_reset_reason_name(cases[i].reason));
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].valid, posix_reset_reason_valid(cases[i].reason),
                                  cases[i].name);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].recorded,
                                  posix_retained_reason_recorded((uint32_t)cases[i].reason),
                                  cases[i].name);
    }

    /* The values the DBC's ResetCause carries (dbc/fcn.dbc), and POWER_ON outside them. */
    TEST_ASSERT_EQUAL_INT(0, (int)PLAT_RESET_POWER_ON);
    TEST_ASSERT_EQUAL_INT(10, (int)PLAT_RESET_REQUESTED);
    TEST_ASSERT_EQUAL_INT(11, (int)PLAT_RESET_WATCHDOG);
    TEST_ASSERT_EQUAL_INT(22, (int)PLAT_RESET_MALLOC_FAILED);
}

/* The CRC of the layout's definition, and the check value of CRC-32/ISO-HDLC, so that the
 * Python bench's zlib.crc32 is the same function. */
static void test_the_crc_is_crc32_over_the_bytes_before_it(void)
{
    const plat_retained_cell_t cell = make_cell((uint32_t)PLAT_RESET_ASSERT, 7U);
    static const uint8_t check[] = "123456789";

    TEST_ASSERT_EQUAL_HEX32(expected_crc(&cell), posix_retained_crc(&cell));
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, crc32_compute(check, 9U));
}

/* ------------------------------------------------------------------------- the hand-off */

/* The four words of a hand-off, written here from the table of plat_retained.h, THE HAND-OFF:
 * the magic, then slot | mode << 8 | attempt << 16 | event << 24, the version, and
 * event_slot | write_error << 8. */
static void handoff_words(uint32_t words[4], uint32_t slot, uint32_t mode, uint32_t attempt,
                          uint32_t event, uint32_t version, uint32_t event_slot,
                          uint32_t write_error)
{
    words[0] = 0x484E4346U; /* the bytes 'F' 'C' 'N' 'H' */
    words[1] = slot | (mode << 8U) | (attempt << 16U) | (event << 24U);
    words[2] = version;
    words[3] = event_slot | (write_error << 8U);
}

/* A valid cell with `reason` and the hand-off `words` in its reserved words. */
static plat_retained_cell_t cell_with_words(uint32_t reason, const uint32_t words[4])
{
    plat_retained_cell_t cell = make_cell(reason, 2U);

    (void)memcpy(cell.reserved, words, sizeof(cell.reserved));
    cell.crc = expected_crc(&cell);
    return cell;
}

/* A hand-off in a cell that records a reset, and in one that records none: taken with every
 * field, the reason of the cell as before, the four words zeroed and the cell sealed; the next
 * start without the bootloader finds none. */
static void test_the_image_takes_the_hand_off_once_and_zeroes_its_words(void)
{
    static const uint32_t reasons[] = {(uint32_t)PLAT_RESET_WATCHDOG,
                                       (uint32_t)PLAT_RESET_POWER_ON};
    uint32_t words[4];

    handoff_words(words, 1U, 2U, 2U, 0U, 0x01020003U, 0xFFU, 0U);

    for (size_t i = 0U; i < 2U; i++) {
        const plat_retained_cell_t before = cell_with_words(reasons[i], words);
        plat_boot_handoff_t got;

        write_bytes(&before, sizeof(before));
        const posix_retained_info_t info = start();

        TEST_ASSERT_EQUAL_STRING("taken", info.handoff_verdict);
        TEST_ASSERT_TRUE(info.have_handoff);
        TEST_ASSERT_TRUE(plat_boot_handoff(&got));
        TEST_ASSERT_EQUAL_UINT8(1U, got.slot);
        TEST_ASSERT_EQUAL_INT((int)PLAT_BOOT_TRIAL, (int)got.mode);
        TEST_ASSERT_EQUAL_UINT8(2U, got.attempt);
        TEST_ASSERT_EQUAL_HEX32(0x01020003U, got.image_version);
        TEST_ASSERT_EQUAL_INT((int)PLAT_BOOT_EVENT_NONE, (int)got.event);
        TEST_ASSERT_EQUAL_UINT8(PLAT_HANDOFF_NO_SLOT, got.event_slot);
        TEST_ASSERT_EQUAL_UINT8(0U, got.select_write_error);
        TEST_ASSERT_EQUAL_INT((int)reasons[i], (int)info.cause);

        const plat_retained_cell_t after = read_cell();

        for (uint32_t w = 0U; w < PLAT_RETAINED_RESERVED_WORDS; w++) {
            TEST_ASSERT_EQUAL_HEX32(0U, after.reserved[w]);
        }

        TEST_ASSERT_EQUAL_HEX32(expected_crc(&after), after.crc);
        posix_retained_close();

        const posix_retained_info_t next = start();

        TEST_ASSERT_EQUAL_STRING("none", next.handoff_verdict);
        TEST_ASSERT_FALSE(next.have_handoff);
        TEST_ASSERT_FALSE(plat_boot_handoff(&got));
        posix_retained_close();
    }

    TEST_ASSERT_FALSE(plat_boot_handoff(NULL));
}

/* Each rule of THE HAND-OFF broken on its own: no hand-off ("invalid"), and the words zeroed,
 * so that the garbage does not stay for a later start. */
static void test_a_hand_off_that_breaks_a_rule_is_none(void)
{
    /* slot, mode, attempt, event, event slot, write error, word 3's top half */
    static const uint32_t broken[][7] = {
        {2U, 1U, 0U, 0U, 0xFFU, 0U, 0U},  /* no such slot */
        {0U, 0U, 0U, 0U, 0xFFU, 0U, 0U},  /* no mode */
        {0U, 3U, 1U, 0U, 0xFFU, 0U, 0U},  /* no such mode */
        {0U, 2U, 0U, 0U, 0xFFU, 0U, 0U},  /* a trial without an attempt */
        {0U, 2U, 4U, 0U, 0xFFU, 0U, 0U},  /* a fourth attempt */
        {0U, 1U, 1U, 0U, 0xFFU, 0U, 0U},  /* the confirmed image with an attempt */
        {0U, 1U, 0U, 2U, 0U, 0U, 0U},     /* BEGIN is no event of a select */
        {0U, 1U, 0U, 0U, 0U, 0U, 0U},     /* NONE acting on a slot */
        {0U, 1U, 0U, 5U, 0xFFU, 0U, 0U},  /* ROLLBACK acting on no slot */
        {0U, 1U, 0U, 5U, 2U, 0U, 0U},     /* ROLLBACK of no such slot */
        {0U, 1U, 0U, 0U, 0xFFU, 11U, 0U}, /* no such status of lib/boot */
        {0U, 1U, 0U, 0U, 0xFFU, 0U, 1U},  /* the unused half of word 3 */
    };

    for (size_t i = 0U; i < (sizeof(broken) / sizeof(broken[0])); i++) {
        uint32_t words[4];

        handoff_words(words, broken[i][0], broken[i][1], broken[i][2], broken[i][3], 7U,
                      broken[i][4], broken[i][5]);
        words[3] |= broken[i][6] << 16U;

        const plat_retained_cell_t before = cell_with_words((uint32_t)PLAT_RESET_REQUESTED, words);

        write_bytes(&before, sizeof(before));
        const posix_retained_info_t info = start();

        TEST_ASSERT_EQUAL_STRING_MESSAGE("invalid", info.handoff_verdict, "a broken hand-off");
        TEST_ASSERT_FALSE(info.have_handoff);

        const plat_retained_cell_t after = read_cell();

        for (uint32_t w = 0U; w < PLAT_RETAINED_RESERVED_WORDS; w++) {
            TEST_ASSERT_EQUAL_HEX32(0U, after.reserved[w]);
        }

        posix_retained_close();
    }

    /* In a cell that is not valid there is nothing to take. */
    uint32_t words[4];

    handoff_words(words, 0U, 1U, 0U, 0U, 7U, 0xFFU, 0U);
    plat_retained_cell_t bad = cell_with_words((uint32_t)PLAT_RESET_REQUESTED, words);

    bad.crc ^= 1U;
    write_bytes(&bad, sizeof(bad));
    const posix_retained_info_t info = start();

    TEST_ASSERT_EQUAL_STRING("crc", info.verdict);
    TEST_ASSERT_EQUAL_STRING("none", info.handoff_verdict);
    TEST_ASSERT_FALSE(info.have_handoff);
}

/* The bootloader's side: its open leaves the reason in the cell for the image, a cell that is
 * not valid is written afresh, and what it writes the image takes. */
static void test_the_bootloader_keeps_the_reason_and_hands_over(void)
{
    const plat_retained_cell_t before = make_cell((uint32_t)PLAT_RESET_IMAGE_FAULT, 3U);
    const plat_boot_handoff_t handoff = {.slot = 0U,
                                         .mode = PLAT_BOOT_CONFIRMED,
                                         .attempt = 0U,
                                         .image_version = 0x01000000U,
                                         .event = PLAT_BOOT_EVENT_ROLLBACK,
                                         .event_slot = 1U,
                                         .select_write_error = 0U};
    posix_retained_info_t info;
    uint32_t words[4];

    write_bytes(&before, sizeof(before));
    TEST_ASSERT_TRUE_MESSAGE(posix_retained_open_boot(s_path, s_error, sizeof(s_error)), s_error);
    posix_retained_get_info(&info);
    ASSERT_START("IMAGE_FAULT", "reset", 3U, info);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PLAT_RESET_IMAGE_FAULT, read_cell().reset_reason);
    TEST_ASSERT_TRUE(posix_retained_set_handoff(&handoff));

    plat_retained_cell_t cell = read_cell();

    handoff_words(words, 0U, 1U, 0U, 5U, 0x01000000U, 1U, 0U);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(words, cell.reserved, 4U);
    TEST_ASSERT_EQUAL_HEX32(expected_crc(&cell), cell.crc);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PLAT_RESET_IMAGE_FAULT, cell.reset_reason);
    posix_retained_close();

    info = start();
    ASSERT_START("IMAGE_FAULT", "reset", 3U, info);
    TEST_ASSERT_TRUE(info.have_handoff);
    TEST_ASSERT_EQUAL_INT((int)PLAT_BOOT_EVENT_ROLLBACK, (int)info.handoff.event);
    TEST_ASSERT_EQUAL_UINT8(1U, info.handoff.event_slot);
    posix_retained_close();

    /* After power-on (no file): a fresh cell from the bootloader, the image's start sees no
     * reset and the hand-off. */
    (void)unlink(s_path);
    TEST_ASSERT_TRUE_MESSAGE(posix_retained_open_boot(s_path, s_error, sizeof(s_error)), s_error);
    posix_retained_get_info(&info);
    ASSERT_START("POWER_ON", "missing", 0U, info);
    TEST_ASSERT_TRUE(posix_retained_set_handoff(&handoff));
    posix_retained_close();
    info = start();
    ASSERT_START("POWER_ON", "no reset", 0U, info);
    TEST_ASSERT_TRUE(info.have_handoff);
}

/* What the bootloader cannot hand over: without a cell, a NULL hand-off, one that breaks a rule
 * (nothing of the cell changes then). */
static void test_a_hand_off_that_cannot_be_written_changes_nothing(void)
{
    plat_boot_handoff_t handoff = {.slot = 1U,
                                   .mode = PLAT_BOOT_TRIAL,
                                   .attempt = 1U,
                                   .image_version = 1U,
                                   .event = PLAT_BOOT_EVENT_NONE,
                                   .event_slot = PLAT_HANDOFF_NO_SLOT,
                                   .select_write_error = 0U};

    TEST_ASSERT_TRUE(posix_retained_open_boot(NULL, s_error, sizeof(s_error)));
    TEST_ASSERT_FALSE(posix_retained_set_handoff(&handoff));
    posix_retained_close();

    TEST_ASSERT_TRUE_MESSAGE(posix_retained_open_boot(s_path, s_error, sizeof(s_error)), s_error);
    const plat_retained_cell_t fresh = read_cell();

    TEST_ASSERT_FALSE(posix_retained_set_handoff(NULL));
    handoff.attempt = 0U; /* a trial without an attempt */
    TEST_ASSERT_FALSE(posix_retained_set_handoff(&handoff));
    handoff.attempt = 1U;
    handoff.slot = 2U;
    TEST_ASSERT_FALSE(posix_retained_set_handoff(&handoff));

    const plat_retained_cell_t after = read_cell();

    TEST_ASSERT_EQUAL_MEMORY(&fresh, &after, sizeof(fresh));
}

/* The trial the bootloader hands over in the tests below. */
static const plat_boot_handoff_t s_trial = {.slot = 1U,
                                            .mode = PLAT_BOOT_TRIAL,
                                            .attempt = 1U,
                                            .image_version = 0x01050000U,
                                            .event = PLAT_BOOT_EVENT_NONE,
                                            .event_slot = PLAT_HANDOFF_NO_SLOT,
                                            .select_write_error = 0U};

/* The bootloader takes back the hand-off of an image it could not start: the four words are
 * zeroed and the cell sealed again, with the header, the reason and the count kept, so that the
 * next start, one without the bootloader, finds no hand-off and the reset cause as before. */
static void test_a_hand_off_taken_back_leaves_none_and_keeps_the_reason(void)
{
    const plat_retained_cell_t before = make_cell((uint32_t)PLAT_RESET_WATCHDOG, 5U);
    plat_boot_handoff_t got;

    write_bytes(&before, sizeof(before));
    TEST_ASSERT_TRUE_MESSAGE(posix_retained_open_boot(s_path, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_TRUE(posix_retained_set_handoff(&s_trial));
    TEST_ASSERT_EQUAL_HEX32(PLAT_HANDOFF_MAGIC, read_cell().reserved[0]);
    posix_retained_clear_handoff();

    const plat_retained_cell_t after = read_cell();

    TEST_ASSERT_EQUAL_HEX32(before.magic, after.magic);
    TEST_ASSERT_EQUAL_UINT16(before.version, after.version);
    TEST_ASSERT_EQUAL_UINT16(before.size, after.size);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PLAT_RESET_WATCHDOG, after.reset_reason);
    TEST_ASSERT_EQUAL_UINT32(5U, after.resets);

    for (uint32_t w = 0U; w < PLAT_RETAINED_RESERVED_WORDS; w++) {
        TEST_ASSERT_EQUAL_HEX32(0U, after.reserved[w]);
    }

    TEST_ASSERT_EQUAL_HEX32(expected_crc(&after), after.crc);
    posix_retained_close();

    const posix_retained_info_t info = start();

    ASSERT_START("WATCHDOG", "reset", 5U, info);
    TEST_ASSERT_EQUAL_STRING("none", info.handoff_verdict);
    TEST_ASSERT_FALSE(info.have_handoff);
    TEST_ASSERT_FALSE(plat_boot_handoff(&got));
}

/* Taking a hand-off back where there is none changes nothing: without a cell, in a cell whose
 * reserved words hold no hand-off (word 0 is not the magic: they are kept, as the image's
 * open keeps them), and a second time after a hand-off was taken back. */
static void test_taking_back_a_hand_off_where_there_is_none_changes_nothing(void)
{
    const plat_retained_cell_t before = make_cell((uint32_t)PLAT_RESET_REQUESTED, 2U);

    TEST_ASSERT_TRUE(posix_retained_open_boot(NULL, s_error, sizeof(s_error)));
    posix_retained_clear_handoff();
    posix_retained_close();

    write_bytes(&before, sizeof(before));
    TEST_ASSERT_TRUE_MESSAGE(posix_retained_open_boot(s_path, s_error, sizeof(s_error)), s_error);
    const plat_retained_cell_t opened = read_cell();

    TEST_ASSERT_EQUAL_HEX32(0x11111111U, opened.reserved[0]);
    posix_retained_clear_handoff();
    const plat_retained_cell_t kept = read_cell();

    TEST_ASSERT_EQUAL_MEMORY(&opened, &kept, sizeof(opened));

    TEST_ASSERT_TRUE(posix_retained_set_handoff(&s_trial));
    posix_retained_clear_handoff();
    const plat_retained_cell_t once = read_cell();

    posix_retained_clear_handoff();
    const plat_retained_cell_t twice = read_cell();

    TEST_ASSERT_EQUAL_MEMORY(&once, &twice, sizeof(once));
    posix_retained_close();

    const posix_retained_info_t info = start();

    ASSERT_START("REQUESTED", "reset", 2U, info);
    TEST_ASSERT_EQUAL_STRING("none", info.handoff_verdict);
    TEST_ASSERT_FALSE(info.have_handoff);
}

/* Why the bootloader releases the lock itself (posix_retained.c, THE LOCK BELONGS TO THE OPEN
 * FILE DESCRIPTION), on the kernel's own terms: a shared mapping refers to the open file
 * description as the descriptor does, so with the descriptor closed the lock is still held, as
 * long as the mapping exists; it goes with the munmap. */
static void test_a_mapping_keeps_the_lock_after_its_descriptor_is_closed(void)
{
    const plat_retained_cell_t cell = make_cell((uint32_t)PLAT_RESET_POWER_ON, 0U);

    write_bytes(&cell, sizeof(cell));

    const int fd = open(s_path, O_RDWR | O_CLOEXEC);

    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, flock(fd, LOCK_EX | LOCK_NB));

    void *const map = mmap(NULL, sizeof(cell), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    TEST_ASSERT_TRUE(map != MAP_FAILED);
    TEST_ASSERT_EQUAL_INT(0, close(fd));
    TEST_ASSERT_EQUAL_INT(IN_USE, lock_from_another_process());
    TEST_ASSERT_EQUAL_INT(0, munmap(map, sizeof(cell)));
    TEST_ASSERT_EQUAL_INT(LOCKED, lock_from_another_process());
}

/* The orderly end: posix_retained_close() releases the lock itself. A keeper (map_keeper.h), a
 * process forked while the cell was mapped, has the mapping too, and with it a reference to the
 * open file description, which therefore outlives the close: a lock left to the description's
 * end would still be held, and neither another process nor this one could open the cell
 * again. */
static void test_the_close_frees_the_cell_while_another_process_still_maps_it(void)
{
    map_keeper_t keeper;
    int status = -1;

    (void)start();
    TEST_ASSERT_TRUE(map_keeper_prepare(&keeper));

    const pid_t pid = map_keeper_fork(&keeper);

    TEST_ASSERT_TRUE(pid > 0);
    TEST_ASSERT_EQUAL_INT(IN_USE, lock_from_another_process());

    posix_retained_close();
    TEST_ASSERT_EQUAL_INT(LOCKED, lock_from_another_process());

    const posix_retained_info_t info = start(); /* and this process opens it again */

    ASSERT_START("POWER_ON", "no reset", 0U, info);
    posix_retained_close();

    TEST_ASSERT_TRUE(map_keeper_end(&keeper));
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status) && (WEXITSTATUS(status) == 0));
}

/* The bootloader's release before the exec: while the cell is still open and mapped, so that
 * its open file description exists (as after an exec whose old memory map another process
 * still holds), posix_retained_unlock() lets another process lock the file; before it, that
 * process cannot. While the lock is released nothing writes into the cell, and the close
 * afterwards is the close of any open cell. */
static void test_a_lock_released_while_the_cell_stays_open_lets_another_process_take_it(void)
{
    posix_retained_info_t info;

    TEST_ASSERT_TRUE_MESSAGE(posix_retained_open_boot(s_path, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_TRUE(posix_retained_set_handoff(&s_trial));
    TEST_ASSERT_EQUAL_INT(IN_USE, lock_from_another_process());

    posix_retained_unlock();
    posix_retained_get_info(&info);
    TEST_ASSERT_TRUE(info.open);
    TEST_ASSERT_EQUAL_INT(LOCKED, lock_from_another_process());
    posix_retained_unlock(); /* a second release changes nothing */
    TEST_ASSERT_EQUAL_INT(LOCKED, lock_from_another_process());

    const plat_retained_cell_t handed = read_cell();
    plat_boot_handoff_t other = s_trial;

    other.attempt = 2U;
    TEST_ASSERT_FALSE(posix_retained_set_handoff(&other));
    posix_retained_clear_handoff();
    posix_retained_note_reset(PLAT_RESET_REQUESTED);

    const plat_retained_cell_t after = read_cell();

    TEST_ASSERT_EQUAL_HEX32(PLAT_HANDOFF_MAGIC, handed.reserved[0]);
    TEST_ASSERT_EQUAL_MEMORY(&handed, &after, sizeof(handed));
    posix_retained_close();

    /* The image's open after it: the hand-off is there to be taken. */
    info = start();
    TEST_ASSERT_TRUE(info.have_handoff);
    TEST_ASSERT_EQUAL_UINT8(1U, info.handoff.attempt);
}

/* After a failed exec the bootloader takes the lock back before it takes the hand-off back:
 * the lock comes back when nobody else holds the cell (and then nobody else can take it), and
 * does not while another process holds it; then the clear, a reset and a new hand-off write
 * nothing. Once that process has ended, the lock comes back and the hand-off is taken back.
 * Without a cell there is no lock to take. */
static void test_the_lock_comes_back_only_while_nobody_else_holds_the_cell(void)
{
    const plat_retained_cell_t before = make_cell((uint32_t)PLAT_RESET_WATCHDOG, 5U);

    write_bytes(&before, sizeof(before));
    TEST_ASSERT_TRUE_MESSAGE(posix_retained_open_boot(s_path, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_TRUE(posix_retained_relock()); /* never released: held */
    TEST_ASSERT_TRUE(posix_retained_set_handoff(&s_trial));
    posix_retained_unlock();

    /* Nobody took it. */
    TEST_ASSERT_TRUE(posix_retained_relock());
    TEST_ASSERT_EQUAL_INT(IN_USE, lock_from_another_process());
    posix_retained_clear_handoff();
    TEST_ASSERT_EQUAL_HEX32(0U, read_cell().reserved[0]);

    /* Another process holds it. */
    TEST_ASSERT_TRUE(posix_retained_set_handoff(&s_trial));
    posix_retained_unlock();

    const holder_t holder = hold_from_another_process();
    const plat_retained_cell_t handed = read_cell();
    plat_boot_handoff_t other = s_trial;

    other.attempt = 2U;
    TEST_ASSERT_FALSE(posix_retained_relock());
    posix_retained_clear_handoff();
    posix_retained_note_reset(PLAT_RESET_REQUESTED);
    TEST_ASSERT_FALSE(posix_retained_set_handoff(&other));
    TEST_ASSERT_FALSE(posix_retained_relock());

    const plat_retained_cell_t untouched = read_cell();

    TEST_ASSERT_EQUAL_HEX32(PLAT_HANDOFF_MAGIC, handed.reserved[0]);
    TEST_ASSERT_EQUAL_MEMORY(&handed, &untouched, sizeof(handed));

    /* It has ended. */
    release_holder(holder);
    TEST_ASSERT_TRUE(posix_retained_relock());
    posix_retained_clear_handoff();

    const plat_retained_cell_t after = read_cell();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)PLAT_RESET_WATCHDOG, after.reset_reason);
    TEST_ASSERT_EQUAL_UINT32(5U, after.resets);

    for (uint32_t w = 0U; w < PLAT_RETAINED_RESERVED_WORDS; w++) {
        TEST_ASSERT_EQUAL_HEX32(0U, after.reserved[w]);
    }

    TEST_ASSERT_EQUAL_HEX32(expected_crc(&after), after.crc);
    posix_retained_close();

    TEST_ASSERT_TRUE(posix_retained_open_boot(NULL, s_error, sizeof(s_error)));
    posix_retained_unlock();
    TEST_ASSERT_FALSE(posix_retained_relock());
    posix_retained_close();
}

/* Every hand-off the rules allow comes back from its words as it went in. */
static void test_every_valid_hand_off_survives_its_encoding(void)
{
    static const plat_boot_event_t events[] = {PLAT_BOOT_EVENT_NONE, PLAT_BOOT_EVENT_ADOPT,
                                               PLAT_BOOT_EVENT_ROLLBACK, PLAT_BOOT_EVENT_REJECT,
                                               PLAT_BOOT_EVENT_FALLBACK};
    uint32_t count = 0U;

    for (uint8_t slot = 0U; slot < 2U; slot++) {
        for (uint8_t attempt = 0U; attempt <= 3U; attempt++) {
            for (size_t e = 0U; e < 5U; e++) {
                for (uint8_t error = 0U; error <= 10U; error += 5U) {
                    plat_boot_handoff_t in;
                    plat_boot_handoff_t out;
                    uint32_t words[PLAT_RETAINED_RESERVED_WORDS];

                    (void)memset(&in, 0, sizeof(in));
                    (void)memset(&out, 0xA5, sizeof(out));
                    in.slot = slot;
                    in.mode = (attempt == 0U) ? PLAT_BOOT_CONFIRMED : PLAT_BOOT_TRIAL;
                    in.attempt = attempt;
                    in.image_version = 0xFFFEFDFCU - count;
                    in.event = events[e];
                    in.event_slot = (events[e] == PLAT_BOOT_EVENT_NONE) ? PLAT_HANDOFF_NO_SLOT
                                                                        : (uint8_t)(1U - slot);
                    in.select_write_error = error;
                    posix_handoff_encode(&in, words);
                    TEST_ASSERT_EQUAL_STRING("taken", posix_handoff_decode(words, &out));
                    TEST_ASSERT_EQUAL_UINT8(in.slot, out.slot);
                    TEST_ASSERT_EQUAL_INT((int)in.mode, (int)out.mode);
                    TEST_ASSERT_EQUAL_UINT8(in.attempt, out.attempt);
                    TEST_ASSERT_EQUAL_HEX32(in.image_version, out.image_version);
                    TEST_ASSERT_EQUAL_INT((int)in.event, (int)out.event);
                    TEST_ASSERT_EQUAL_UINT8(in.event_slot, out.event_slot);
                    TEST_ASSERT_EQUAL_UINT8(in.select_write_error, out.select_write_error);
                    count++;
                }
            }
        }
    }

    TEST_ASSERT_EQUAL_UINT32(2U * 4U * 5U * 3U, count);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");

    (void)snprintf(s_dir, sizeof(s_dir), "%s/fcn-retained-XXXXXX",
                   ((tmp != NULL) && (tmp[0] != '\0')) ? tmp : "/tmp");

    if (mkdtemp(s_dir) == NULL) {
        (void)printf("cannot create a directory for the test: %s\n", strerror(errno));
        return 1;
    }

    (void)snprintf(s_path, sizeof(s_path), "%s/cell", s_dir);

    UNITY_BEGIN();
    RUN_TEST(test_without_a_file_every_start_is_a_power_on);
    RUN_TEST(test_a_missing_file_is_a_power_on_and_becomes_a_fresh_cell);
    RUN_TEST(test_a_valid_cell_gives_its_reason_once_and_keeps_its_words);
    RUN_TEST(test_a_cell_that_is_not_valid_is_a_power_on);
    RUN_TEST(test_resets_are_recorded_and_counted_until_a_run_ends_without_one);
    RUN_TEST(test_a_power_loss_destroys_the_cell);
    RUN_TEST(test_a_removed_file_is_a_power_on);
    RUN_TEST(test_the_cell_is_locked_while_it_is_open);
    RUN_TEST(test_a_file_that_cannot_be_used_is_an_error_with_a_reason);
    RUN_TEST(test_the_reasons_and_their_names);
    RUN_TEST(test_the_crc_is_crc32_over_the_bytes_before_it);
    RUN_TEST(test_the_image_takes_the_hand_off_once_and_zeroes_its_words);
    RUN_TEST(test_a_hand_off_that_breaks_a_rule_is_none);
    RUN_TEST(test_the_bootloader_keeps_the_reason_and_hands_over);
    RUN_TEST(test_a_hand_off_that_cannot_be_written_changes_nothing);
    RUN_TEST(test_a_hand_off_taken_back_leaves_none_and_keeps_the_reason);
    RUN_TEST(test_taking_back_a_hand_off_where_there_is_none_changes_nothing);
    RUN_TEST(test_a_mapping_keeps_the_lock_after_its_descriptor_is_closed);
    RUN_TEST(test_the_close_frees_the_cell_while_another_process_still_maps_it);
    RUN_TEST(test_a_lock_released_while_the_cell_stays_open_lets_another_process_take_it);
    RUN_TEST(test_the_lock_comes_back_only_while_nobody_else_holds_the_cell);
    RUN_TEST(test_every_valid_hand_off_survives_its_encoding);

    const int status = UNITY_END();

    (void)unlink(s_path);
    (void)rmdir(s_dir);
    return status;
}
