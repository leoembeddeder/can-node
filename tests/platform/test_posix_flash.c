/* SPDX-License-Identifier: MIT */
/*
 * The flash of the host build (platform/posix/posix_flash.c) without a kernel: the layout
 * table; the file (created erased, kept across a restart of the process, a file of another size
 * or kind refused and left alone, locked while open, and free for another process once
 * posix_flash_unlock() has given it up while it is still open and mapped, after which nothing
 * this process stores reaches the file, or once posix_flash_close() has closed it, also while
 * another process still maps it); the syntax of a cut; the device's rules
 * and counters; the wait function; the simulation commands flash.status and flash.cut with
 * every reply; a cut that ends the process with the status of a power loss (12), leaves exactly
 * the tear that lib/flash's model gives for its seed on a copy, takes the retained cell
 * with it, and leaves both files free for the next process while mappings of them outlive the
 * one that ended; and the commands served by another thread while operations run (also run
 * under ThreadSanitizer).
 *
 * A test that needs a process to end plays the bench: it forks, the child opens the flash and
 * runs the operations of flash_script.c, and the parent, which has the flash closed, looks at
 * how the child ended and at the file it left. That the waits take the modelled time, and a
 * cut under the FreeRTOS scheduler, are tested by flash_selftest.c.
 *
 * A test of a lock that must be free although something still refers to the file's open file
 * description uses a keeper (map_keeper.h): a process forked while the file was mapped, which
 * has the mapping too. That is the state a process leaves behind that ended while another
 * process held its memory map for a moment, as a reader of /proc/<pid>/maps does
 * (docs/bug-log.md, 012), here for as long as the test wants and so without chance.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "flash/flash.h"
#include "flash/nor.h"
#include "flash_script.h"
#include "map_keeper.h"
#include "plat_flash.h"
#include "posix_flash.h"
#include "posix_retained.h"
#include "posix_sim.h"
#include "unity.h"

#define DEVICE_SIZE 0x202000U

_Static_assert(DEVICE_SIZE == FLASH_SCRIPT_DEVICE_SIZE, "the script runs on the host's device");

/* Programs of 1 us and erases of 50 us: fast, and different, so that busy_ns shows which. */
static const nor_timing_t FAST = {.page_program_ns = 1000U, .sector_erase_ns = 50000U};

static char s_dir[256];
static char s_path[300];   /* the flash file */
static char s_cell[300];   /* a retained cell */
static char s_stderr[300]; /* a child's stderr */
static char s_error[1024];
static uint8_t s_file[DEVICE_SIZE];
static uint8_t s_expect[DEVICE_SIZE];
static uint8_t s_before[DEVICE_SIZE];
static atomic_uint s_power_losses; /* calls of the test's power-loss function */

static const char *at(const char *what, long value)
{
    static char text[96];

    (void)snprintf(text, sizeof(text), "%s %ld", what, value);
    return text;
}

/* ------------------------------------------------------------------------------ the file */

static off_t file_size(const char *path)
{
    struct stat info;

    return (stat(path, &info) == 0) ? info.st_size : (off_t)-1;
}

/* The whole flash file into s_file. */
static void read_file(void)
{
    const int fd = open(s_path, O_RDONLY);

    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, "cannot open the flash file");
    TEST_ASSERT_EQUAL_INT64((int64_t)DEVICE_SIZE, (int64_t)file_size(s_path));
    TEST_ASSERT_EQUAL_INT64((int64_t)DEVICE_SIZE, (int64_t)read(fd, s_file, DEVICE_SIZE));
    (void)close(fd);
}

static void write_file(const char *path, const uint8_t *data, size_t len)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, "cannot create a file");
    TEST_ASSERT_EQUAL_INT64((int64_t)len, (int64_t)write(fd, data, len));
    (void)close(fd);
}

static void open_flash(const nor_timing_t *timing)
{
    TEST_ASSERT_TRUE_MESSAGE(posix_flash_open(s_path, timing, s_error, sizeof(s_error)), s_error);
}

static posix_flash_info_t flash_info(void)
{
    posix_flash_info_t out;

    posix_flash_get_info(&out);
    return out;
}

static void survive_power_loss(void)
{
    (void)atomic_fetch_add(&s_power_losses, 1U);
}

void setUp(void)
{
    (void)unlink(s_path);
    (void)unlink(s_cell);
    (void)unlink(s_stderr);
    atomic_store(&s_power_losses, 0U);
    posix_flash_set_wait(NULL);
    posix_flash_set_power_loss(survive_power_loss);
}

void tearDown(void)
{
    posix_flash_close();
}

/* ------------------------------------------------------------------------ simulation control */

static int s_client = -1;
static uint16_t s_port;

static void send_request(const char *text)
{
    struct sockaddr_in to;

    (void)memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(s_port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT((int)strlen(text), (int)sendto(s_client, text, strlen(text), 0,
                                                         (const struct sockaddr *)&to, sizeof(to)));
}

static const char *receive_reply(int timeout_ms)
{
    static char reply[2U * POSIX_SIM_MAX_DATAGRAM];
    struct pollfd fd = {.fd = s_client, .events = POLLIN, .revents = 0};
    const char *result = NULL;

    if (poll(&fd, 1U, timeout_ms) > 0) {
        const ssize_t n = recv(s_client, reply, sizeof(reply) - 1U, 0);

        if (n >= 0) {
            reply[n] = '\0';
            result = reply;
        }
    }

    return result;
}

/* One request, served by this thread, and its reply. */
static const char *exchange(const char *request)
{
    const char *reply;

    send_request(request);
    posix_sim_service();
    reply = receive_reply(500);
    return (reply != NULL) ? reply : "<no reply>";
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/* A reply that is `expected` followed by " t_ns=<the time of the exchange>". */
static void expect_timed_ok(UNITY_LINE_TYPE line, const char *request, const char *expected)
{
    const uint64_t before = monotonic_ns();
    const char *reply = exchange(request);
    const uint64_t after = monotonic_ns();
    const size_t length = strlen(expected);
    char *end = NULL;

    UNITY_TEST_ASSERT_EQUAL_STRING_LEN(expected, reply, length, line, request);
    UNITY_TEST_ASSERT_EQUAL_STRING_LEN(" t_ns=", &reply[length], 6U, line, request);

    const unsigned long long t_ns = strtoull(&reply[length + 6U], &end, 10);

    UNITY_TEST_ASSERT((end != &reply[length + 6U]) && (*end == '\0'), line, "ends with t_ns");
    UNITY_TEST_ASSERT((t_ns >= before) && (t_ns <= after), line, "t_ns is the time of the command");
}

#define EXPECT_TIMED_OK(request, expected) expect_timed_ok(__LINE__, (request), (expected))
#define EXPECT_REPLY(request, expected)    TEST_ASSERT_EQUAL_STRING((expected), exchange(request))

/* The value of `key` in the reply to flash.status. */
static unsigned long long status_number(const char *key)
{
    char needle[64];
    const char *reply = exchange("s1 flash.status");
    const char *found;

    (void)snprintf(needle, sizeof(needle), " %s=", key);
    found = strstr(reply, needle);
    TEST_ASSERT_NOT_NULL_MESSAGE(found, reply);
    return (found != NULL) ? strtoull(&found[strlen(needle)], NULL, 10) : 0ULL;
}

/* --------------------------------------------------------------------------- the layout */

static void test_the_layout_is_the_documented_table(void)
{
    const plat_flash_layout_t *layout = plat_flash_layout();

    TEST_ASSERT_EQUAL_UINT32(DEVICE_SIZE, layout->geometry.size);
    TEST_ASSERT_EQUAL_UINT32(4096U, layout->geometry.sector_size);
    TEST_ASSERT_EQUAL_UINT32(256U, layout->geometry.page_size);
    TEST_ASSERT_EQUAL_UINT32(8U, layout->geometry.unit_size);
    TEST_ASSERT_TRUE(nor_geometry_valid(&layout->geometry));

    TEST_ASSERT_EQUAL_HEX32(0x000000U, layout->meta.offset);
    TEST_ASSERT_EQUAL_HEX32(0x002000U, layout->meta.size);
    TEST_ASSERT_EQUAL_HEX32(0x002000U, layout->slot[0].offset);
    TEST_ASSERT_EQUAL_HEX32(0x100000U, layout->slot[0].size);
    TEST_ASSERT_EQUAL_HEX32(0x102000U, layout->slot[1].offset);
    TEST_ASSERT_EQUAL_HEX32(0x100000U, layout->slot[1].size);

    /* The areas follow each other without a gap and cover the device, in whole sectors. */
    const plat_flash_area_t areas[] = {layout->meta, layout->slot[0], layout->slot[1]};
    uint32_t next = 0U;

    for (uint32_t i = 0U; i < 3U; i++) {
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(next, areas[i].offset, at("area", (long)i));
        TEST_ASSERT_EQUAL_UINT32(0U, areas[i].offset % 4096U);
        TEST_ASSERT_EQUAL_UINT32(0U, areas[i].size % 4096U);
        TEST_ASSERT_TRUE(areas[i].size > 0U);
        next = areas[i].offset + areas[i].size;
    }

    TEST_ASSERT_EQUAL_HEX32(DEVICE_SIZE, next);
}

/* ------------------------------------------------------------------------------ the file */

static void test_without_a_file_there_is_no_device_and_nothing_to_cut(void)
{
    TEST_ASSERT_TRUE(posix_flash_open(NULL, NULL, s_error, sizeof(s_error)));
    TEST_ASSERT_NULL(plat_flash_device());
    TEST_ASSERT_FALSE(flash_info().open);
    EXPECT_REPLY("a1 flash.status", "a1 ok flash=off");
    EXPECT_REPLY("a2 flash.cut 1", "a2 err no flash in this run (started without --flash-file)");
    EXPECT_REPLY("a3 flash.cut off", "a3 err no flash in this run (started without --flash-file)");

    const posix_flash_cut_t cut = {1U, 0U};

    posix_flash_arm_cut(&cut); /* nothing to arm */
    TEST_ASSERT_EQUAL_UINT32(0U, flash_info().cut_in);
    posix_flash_close();
    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)file_size(s_path));
}

static void test_a_missing_file_is_created_erased_and_then_opened_as_it_is(void)
{
    uint8_t data[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

    open_flash(&FAST);

    const flash_dev_t *dev = plat_flash_device();

    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_TRUE(flash_info().open);
    TEST_ASSERT_TRUE(flash_info().created);
    TEST_ASSERT_EQUAL_UINT32(DEVICE_SIZE, dev->geometry.size);
    TEST_ASSERT_EQUAL_UINT32(4096U, dev->geometry.sector_size);
    TEST_ASSERT_EQUAL_UINT32(256U, dev->geometry.page_size);
    TEST_ASSERT_EQUAL_UINT32(8U, dev->geometry.unit_size);
    read_file();
    TEST_ASSERT_EACH_EQUAL_HEX8(0xFFU, s_file, DEVICE_SIZE);

    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->program(dev->ctx, 0x102008U, data, sizeof(data)));
    posix_flash_close();
    TEST_ASSERT_NULL(plat_flash_device());

    /* Opened again: not created, the content as it was. */
    open_flash(&FAST);
    TEST_ASSERT_FALSE(flash_info().created);
    read_file();
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, &s_file[0x102008U], sizeof(data));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xFFU, s_file, 0x102008U);
}

/* A child that opens the flash, runs the script and ends with _exit(0), as a reset ends a run:
 * the lock released (posix_flash_unlock(), as plat_reset() does), no close, no flush. */
static int run_child_script(void)
{
    const pid_t pid = fork();
    int status = -1;

    TEST_ASSERT_TRUE(pid >= 0);

    if (pid == 0) {
        posix_flash_set_power_loss(NULL);

        if (!posix_flash_open(s_path, &FAST, s_error, sizeof(s_error))) {
            _exit(2);
        }

        const int result =
            (flash_script_run(plat_flash_device(), NULL) == FLASH_SCRIPT_WRITES) ? 0 : 3;

        posix_flash_unlock();
        _exit(result);
    }

    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void test_the_flash_survives_a_restart_of_the_process(void)
{
    uint8_t back[256];

    (void)memset(s_before, 0xFF, sizeof(s_before));
    TEST_ASSERT_EQUAL_INT(0, run_child_script());

    /* The file holds what the script leaves, as the model computes it. */
    flash_script_replay(s_before, s_expect, 0U, 0U);
    read_file();
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_expect, s_file, DEVICE_SIZE);
    TEST_ASSERT_FALSE(memcmp(s_before, s_expect, DEVICE_SIZE) == 0); /* the script wrote */

    /* And the next process reads it through the device. */
    open_flash(&FAST);
    TEST_ASSERT_FALSE(flash_info().created);
    TEST_ASSERT_EQUAL_INT(FLASH_OK, plat_flash_device()->read(NULL, 0x102F00U, back, 256U));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&s_expect[0x102F00U], back, 256U);
}

static void test_a_file_of_another_size_is_refused_and_left_alone(void)
{
    const size_t sizes[] = {
        0U, 1U, 4096U, DEVICE_SIZE - 1U, DEVICE_SIZE + 1U, (size_t)2U * DEVICE_SIZE};

    for (uint32_t i = 0U; i < (sizeof(sizes) / sizeof(sizes[0])); i++) {
        char expected[512];

        for (size_t k = 0U; k < sizes[i]; k++) {
            s_before[k % DEVICE_SIZE] = (uint8_t)(k * 7U);
        }

        (void)unlink(s_path);

        if (sizes[i] <= DEVICE_SIZE) {
            write_file(s_path, s_before, sizes[i]);
        } else {
            write_file(s_path, s_before, DEVICE_SIZE);
            TEST_ASSERT_EQUAL_INT(0, truncate(s_path, (off_t)sizes[i]));
        }

        TEST_ASSERT_FALSE(posix_flash_open(s_path, &FAST, s_error, sizeof(s_error)));
        (void)snprintf(expected, sizeof(expected),
                       "--flash-file %s has %lu bytes, a flash file has exactly 2105344 (remove it "
                       "to start from an erased flash)",
                       s_path, (unsigned long)sizes[i]);
        TEST_ASSERT_EQUAL_STRING(expected, s_error);
        TEST_ASSERT_NULL(plat_flash_device());
        TEST_ASSERT_FALSE(flash_info().open);

        /* Left as it was: its size, and its bytes. */
        TEST_ASSERT_EQUAL_INT64((int64_t)sizes[i], (int64_t)file_size(s_path));

        if ((sizes[i] > 0U) && (sizes[i] <= DEVICE_SIZE)) {
            const int fd = open(s_path, O_RDONLY);

            TEST_ASSERT_EQUAL_INT64((int64_t)sizes[i], (int64_t)read(fd, s_file, sizes[i]));
            (void)close(fd);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(s_before, s_file, sizes[i]);
        }
    }
}

static void test_a_file_that_cannot_be_used_is_refused_with_the_reason(void)
{
    char path[400];

    TEST_ASSERT_FALSE(posix_flash_open(s_dir, &FAST, s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_error, "cannot open --flash-file"), s_error);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_error, s_dir), s_error);

    (void)snprintf(path, sizeof(path), "%s/no/such/dir/flash.bin", s_dir);
    TEST_ASSERT_FALSE(posix_flash_open(path, &FAST, s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_error, "No such file or directory"), s_error);
    TEST_ASSERT_NULL(plat_flash_device());

    /* Opened twice: refused, the first stays open. */
    open_flash(&FAST);
    TEST_ASSERT_FALSE(posix_flash_open(s_path, &FAST, s_error, sizeof(s_error)));
    TEST_ASSERT_EQUAL_STRING("a flash is already open", s_error);
    TEST_ASSERT_NOT_NULL(plat_flash_device());
}

static void test_the_file_is_locked_while_it_is_open(void)
{
    open_flash(&FAST);

    /* Another open file description of it cannot lock it. */
    int fd = open(s_path, O_RDWR);

    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(-1, flock(fd, LOCK_EX | LOCK_NB));
    TEST_ASSERT_EQUAL_INT(EWOULDBLOCK, errno);
    posix_flash_close();
    TEST_ASSERT_EQUAL_INT(0, flock(fd, LOCK_EX | LOCK_NB));

    /* And a file another process has locked is not opened. */
    TEST_ASSERT_FALSE(posix_flash_open(s_path, &FAST, s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_error, "is in use by another process"), s_error);
    TEST_ASSERT_NULL(plat_flash_device());
    (void)close(fd);
    open_flash(&FAST);
}

/* Another process tries to lock the file `path` (the flash file, or the cell): a child opens it
 * anew (an open file description of its own) and tries flock(LOCK_EX | LOCK_NB). Its exit
 * status: 0 it got the lock (given up when it ends: its description has no mapping), 1
 * EWOULDBLOCK (another description holds it), 2 otherwise. */
static int lock_from_another_process(const char *path)
{
    int status = -1;
    const pid_t pid = fork();

    TEST_ASSERT_TRUE(pid >= 0);

    if (pid == 0) {
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        int found = 2;

        if (fd >= 0) {
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                found = 0;
            } else if (errno == EWOULDBLOCK) {
                found = 1;
            } else {
                found = 2;
            }
        }

        _exit(found);
    }

    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    return WEXITSTATUS(status);
}

/* The bootloader's release before the exec: with the flash still open and mapped, so that its
 * open file description exists (as after an exec whose old memory map another process still
 * holds), posix_flash_unlock() lets another process lock the file; before it, that process
 * cannot. The file stays open until the close; a second release, and one without a flash,
 * change nothing. */
static void test_a_lock_released_while_the_file_stays_open_lets_another_process_take_it(void)
{
    open_flash(&FAST);
    TEST_ASSERT_EQUAL_INT(1, lock_from_another_process(s_path));

    posix_flash_unlock();
    TEST_ASSERT_TRUE(flash_info().open);
    TEST_ASSERT_NOT_NULL(plat_flash_device());
    TEST_ASSERT_EQUAL_INT(0, lock_from_another_process(s_path));
    posix_flash_unlock();
    TEST_ASSERT_EQUAL_INT(0, lock_from_another_process(s_path));
    posix_flash_close();
    TEST_ASSERT_FALSE(flash_info().open);

    posix_flash_unlock();
    open_flash(&FAST);
    TEST_ASSERT_EQUAL_INT(1, lock_from_another_process(s_path));
}

/* The other half of giving the file up (posix_flash.c, THE LOCK AND THE END OF A RUN): once
 * the lock is released, another process may hold the file, so nothing this process stores may
 * reach it any more. A reset from the hardware watchdog's thread stops no task before the exit
 * does, and a task in the middle of an update goes on programming until then: here the test
 * goes on after the unlock. The device works as before, on this process's own copy; the file
 * keeps what it held at the unlock, also after the close. */
static void test_nothing_stored_after_the_file_is_given_up_reaches_the_file(void)
{
    uint8_t page[256];
    uint8_t back[256];

    for (uint32_t i = 0U; i < sizeof(page); i++) {
        page[i] = (uint8_t)(i * 5U);
    }

    open_flash(&FAST);

    const flash_dev_t *dev = plat_flash_device();

    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->program(dev->ctx, 0x003000U, page, sizeof(page)));
    read_file();
    (void)memcpy(s_before, s_file, DEVICE_SIZE); /* the file as the unlock finds it */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(page, &s_before[0x003000U], sizeof(page));

    posix_flash_unlock();
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->read(dev->ctx, 0x003000U, back, sizeof(back)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(page, back, sizeof(page)); /* still what the file held */

    /* A program of an erased page and an erase of the programmed one, as the model takes them. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->program(dev->ctx, 0x104000U, page, sizeof(page)));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0x003000U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->read(dev->ctx, 0x104000U, back, sizeof(back)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(page, back, sizeof(page));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->read(dev->ctx, 0x003000U, back, sizeof(back)));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xFFU, back, sizeof(back));

    /* None of it is in the file, which another process could have locked by now. */
    read_file();
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_before, s_file, DEVICE_SIZE);
    TEST_ASSERT_EQUAL_INT(0, lock_from_another_process(s_path));
    posix_flash_close();
    read_file();
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_before, s_file, DEVICE_SIZE);
}

/* The orderly end: posix_flash_close() releases the lock itself. With a keeper that still maps
 * the file, the open file description outlives the close, and a lock left to the description's
 * end would still be held: another process, and this one again, could not open the file. */
static void test_the_close_frees_the_file_while_another_process_still_maps_it(void)
{
    map_keeper_t keeper;
    int status = -1;

    open_flash(&FAST);
    TEST_ASSERT_TRUE(map_keeper_prepare(&keeper));

    const pid_t pid = map_keeper_fork(&keeper);

    TEST_ASSERT_TRUE(pid > 0);
    TEST_ASSERT_EQUAL_INT(1, lock_from_another_process(s_path));

    posix_flash_close();
    TEST_ASSERT_EQUAL_INT(0, lock_from_another_process(s_path));
    open_flash(&FAST);
    TEST_ASSERT_EQUAL_INT(1, lock_from_another_process(s_path));
    posix_flash_close();

    TEST_ASSERT_TRUE(map_keeper_end(&keeper));
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
}

/* ------------------------------------------------------------------------- the cut syntax */

static void test_a_cut_is_written_n_or_n_colon_seed(void)
{
    const struct {
        const char *text;
        uint32_t operation;
        uint32_t seed;
    } good[] = {
        {"1", 1U, 0U},
        {"7:42", 7U, 42U},
        {"12:0", 12U, 0U},
        {"0003:0009", 3U, 9U},
        {"2147483647:4294967295", 2147483647U, 4294967295U},
    };
    const char *const bad[] = {
        "",   "0",          "0:5",          "-1",    "+1",   " 1",
        "1 ", "1:",         ":1",           "1:2:3", "1:-2", "x",
        "1x", "2147483648", "1:4294967296", "1,2",   "1::2", "99999999999999999999999"};

    for (uint32_t i = 0U; i < (sizeof(good) / sizeof(good[0])); i++) {
        posix_flash_cut_t cut = {0U, 0U};

        TEST_ASSERT_TRUE_MESSAGE(
            posix_flash_parse_cut(good[i].text, &cut, s_error, sizeof(s_error)), good[i].text);
        TEST_ASSERT_EQUAL_UINT32(good[i].operation, cut.operation);
        TEST_ASSERT_EQUAL_UINT32(good[i].seed, cut.seed);
    }

    for (uint32_t i = 0U; i < (sizeof(bad) / sizeof(bad[0])); i++) {
        posix_flash_cut_t cut = {5U, 6U};
        char expected[512];

        TEST_ASSERT_FALSE_MESSAGE(posix_flash_parse_cut(bad[i], &cut, s_error, sizeof(s_error)),
                                  bad[i]);
        TEST_ASSERT_EQUAL_UINT32(5U, cut.operation); /* left alone */
        TEST_ASSERT_EQUAL_UINT32(6U, cut.seed);
        (void)snprintf(expected, sizeof(expected),
                       "a cut is N[:SEED], N from 1 to 2147483647 and SEED from 0 to 4294967295, "
                       "not '%s'",
                       bad[i]);
        TEST_ASSERT_EQUAL_STRING(expected, s_error);
    }

    posix_flash_cut_t cut = {5U, 6U};

    TEST_ASSERT_FALSE(posix_flash_parse_cut(NULL, &cut, s_error, sizeof(s_error)));
}

/* -------------------------------------------------------------------- the device's rules */

static void test_the_device_follows_flash_h_and_counts_what_it_did(void)
{
    uint8_t page[256];
    uint8_t back[256];

    open_flash(&FAST);

    const flash_dev_t *dev = plat_flash_device();

    (void)memset(page, 0x3C, sizeof(page));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->program(dev->ctx, 0x2000U, page, 256U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, dev->program(dev->ctx, 0x2000U, page, 8U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, dev->program(dev->ctx, 0x2004U, page, 8U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, dev->program(dev->ctx, 0x20F8U, page, 16U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, dev->program(dev->ctx, DEVICE_SIZE - 8U, page, 16U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->read(dev->ctx, 0x2000U, back, 256U));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(page, back, 256U);
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, dev->erase(dev->ctx, 0x2100U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_RANGE, dev->erase(dev->ctx, DEVICE_SIZE));

    /* Erases in every area: the metadata once, slot A twice in one sector, slot B once. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0x1000U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0x2000U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0x2000U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, DEVICE_SIZE - 4096U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->read(dev->ctx, 0x2000U, back, 256U));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xFFU, back, 256U);

    /* What it counted, and that every change is in the file. */
    const posix_flash_info_t counted = flash_info();

    TEST_ASSERT_EQUAL_UINT64(2U, counted.reads);
    TEST_ASSERT_EQUAL_UINT64(512U, counted.bytes_read);
    TEST_ASSERT_EQUAL_UINT64(1U, counted.programs);
    TEST_ASSERT_EQUAL_UINT64(32U, counted.units_programmed);
    TEST_ASSERT_EQUAL_UINT64(4U, counted.erases);
    TEST_ASSERT_EQUAL_UINT64(6U, counted.refused);
    TEST_ASSERT_EQUAL_UINT64(1000U + (4U * 50000U), counted.busy_ns);
    TEST_ASSERT_EQUAL_UINT32(1U, counted.erases_meta);
    TEST_ASSERT_EQUAL_UINT32(2U, counted.erases_slot[0]);
    TEST_ASSERT_EQUAL_UINT32(1U, counted.erases_slot[1]);
    TEST_ASSERT_EQUAL_UINT32(2U, counted.max_sector_erases);
    read_file();
    TEST_ASSERT_EACH_EQUAL_HEX8(0xFFU, s_file, DEVICE_SIZE);
}

/* The wait function, set as the node sets one: called until the time has passed. */
static atomic_uint s_waits;

static void counting_wait(void)
{
    const struct timespec nap = {0, 200000L};

    (void)atomic_fetch_add(&s_waits, 1U);
    (void)nanosleep(&nap, NULL);
}

static void test_an_operation_waits_through_the_wait_function_until_its_time_has_passed(void)
{
    const nor_timing_t slow = {.page_program_ns = 3000000U, .sector_erase_ns = 7000000U};
    uint8_t unit[8] = {0U};
    uint8_t back[8];

    open_flash(&slow);
    posix_flash_set_wait(counting_wait);

    const flash_dev_t *dev = plat_flash_device();
    uint64_t start = monotonic_ns();

    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->program(dev->ctx, 0U, unit, 8U));
    TEST_ASSERT_TRUE(monotonic_ns() - start >= 3000000U); /* never shorter */
    TEST_ASSERT_TRUE(atomic_load(&s_waits) >= 1U);

    atomic_store(&s_waits, 0U);
    start = monotonic_ns();
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0U));
    TEST_ASSERT_TRUE(monotonic_ns() - start >= 7000000U);
    TEST_ASSERT_TRUE(atomic_load(&s_waits) >= 1U);

    /* A read, and a refused call, take no time and do not wait. */
    atomic_store(&s_waits, 0U);
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->read(dev->ctx, 0U, back, 8U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_ALIGN, dev->erase(dev->ctx, 8U));
    TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_waits));

    /* Without the function the thread sleeps, also never shorter. */
    posix_flash_set_wait(NULL);
    start = monotonic_ns();
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0U));
    TEST_ASSERT_TRUE(monotonic_ns() - start >= 7000000U);
    TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_waits));
}

/* ----------------------------------------------------------------- the simulation commands */

static void test_flash_status_names_every_count(void)
{
    uint8_t unit[16] = {0U};

    open_flash(&FAST);

    const flash_dev_t *dev = plat_flash_device();

    EXPECT_REPLY("b1 flash.status",
                 "b1 ok flash=on size=2105344 reads=0 bytes_read=0 programs=0 units_programmed=0 "
                 "erases=0 refused=0 busy_ns=0 erases_meta=0 erases_slot_a=0 erases_slot_b=0 "
                 "max_sector_erases=0 cut=none cut_in=0 cut_seed=0");

    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->program(dev->ctx, 0x1000U, unit, 16U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->read(dev->ctx, 0x1000U, unit, 5U));
    TEST_ASSERT_EQUAL_INT(FLASH_E_NOT_ERASED, dev->program(dev->ctx, 0x1000U, unit, 8U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0x1000U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0x103000U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0x103000U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0x103000U));

    EXPECT_REPLY("b2 flash.status",
                 "b2 ok flash=on size=2105344 reads=1 bytes_read=5 programs=1 units_programmed=2 "
                 "erases=4 refused=1 busy_ns=201000 erases_meta=1 erases_slot_a=0 erases_slot_b=3 "
                 "max_sector_erases=3 cut=none cut_in=0 cut_seed=0");
}

static void test_flash_cut_arms_a_cut_and_the_status_counts_it_down(void)
{
    uint8_t unit[8] = {0U};

    open_flash(&FAST);

    const flash_dev_t *dev = plat_flash_device();

    EXPECT_TIMED_OK("c1 flash.cut 3:9", "c1 ok cut=armed cut_in=3 cut_seed=9");
    TEST_ASSERT_NOT_NULL(strstr(exchange("c2 flash.status"), " cut=armed cut_in=3 cut_seed=9"));
    TEST_ASSERT_EQUAL_UINT32(3U, flash_info().cut_in);
    TEST_ASSERT_EQUAL_UINT32(9U, flash_info().cut_seed);

    /* A read takes the request but does not count; a program does. */
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->read(dev->ctx, 0U, unit, 8U));
    TEST_ASSERT_EQUAL_UINT64(3U, status_number("cut_in"));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->program(dev->ctx, 0U, unit, 8U));
    TEST_ASSERT_EQUAL_UINT64(2U, status_number("cut_in"));
    TEST_ASSERT_EQUAL_UINT64(9U, status_number("cut_seed"));

    /* Armed again: counted from now, the newest wins. */
    EXPECT_TIMED_OK("c3 flash.cut 5", "c3 ok cut=armed cut_in=5 cut_seed=0");
    EXPECT_TIMED_OK("c4 flash.cut 4:1", "c4 ok cut=armed cut_in=4 cut_seed=1");
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0U));
    TEST_ASSERT_NOT_NULL(strstr(exchange("c5 flash.status"), " cut=armed cut_in=3 cut_seed=1"));

    /* Off: nothing armed, also before an operation took the request. */
    EXPECT_TIMED_OK("c6 flash.cut off", "c6 ok cut=none");
    TEST_ASSERT_NOT_NULL(strstr(exchange("c7 flash.status"), " cut=none cut_in=0 cut_seed=0"));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_power_losses));
    TEST_ASSERT_EQUAL_UINT32(0U, flash_info().cut_in);

    /* The largest N, whose top bit shares a place with the flag of a fired cut: armed, waiting
     * and taken, it is armed and nothing has fired. */
    EXPECT_TIMED_OK("c8 flash.cut 2147483647:4294967295",
                    "c8 ok cut=armed cut_in=2147483647 cut_seed=4294967295");
    TEST_ASSERT_FALSE(flash_info().cut_fired);
    TEST_ASSERT_NOT_NULL(
        strstr(exchange("c9 flash.status"), " cut=armed cut_in=2147483647 cut_seed=4294967295"));
    TEST_ASSERT_EQUAL_INT(FLASH_OK, dev->erase(dev->ctx, 0U));
    TEST_ASSERT_FALSE(flash_info().cut_fired);
    TEST_ASSERT_EQUAL_UINT32(2147483646U, flash_info().cut_in);
    TEST_ASSERT_EQUAL_UINT32(4294967295U, flash_info().cut_seed);
    TEST_ASSERT_NOT_NULL(
        strstr(exchange("c10 flash.status"), " cut=armed cut_in=2147483646 cut_seed=4294967295"));
}

static void test_a_wrong_flash_command_is_refused_with_its_usage(void)
{
    open_flash(&FAST);
    EXPECT_REPLY("d1 flash.cut", "d1 err usage: flash.cut N[:SEED] | flash.cut off");
    EXPECT_REPLY("d2 flash.cut 1 2", "d2 err usage: flash.cut N[:SEED] | flash.cut off");
    EXPECT_REPLY("d3 flash.cut 0",
                 "d3 err a cut is N[:SEED], N from 1 to 2147483647 and SEED from 0 to 4294967295, "
                 "not '0' (usage: flash.cut N[:SEED] | flash.cut off)");
    EXPECT_REPLY("d4 flash.cut on",
                 "d4 err a cut is N[:SEED], N from 1 to 2147483647 and SEED from 0 to 4294967295, "
                 "not 'on' (usage: flash.cut N[:SEED] | flash.cut off)");
    EXPECT_REPLY("d5 flash.cut 1234567890123456789012345678901234567890",
                 "d5 err usage: flash.cut N[:SEED] | flash.cut off");
    EXPECT_REPLY("d6 flash.status now", "d6 err usage: flash.status");
    EXPECT_REPLY("d7 flash.erase", "d7 err unknown command 'flash.erase' (flash.status, flash.cut "
                                   "N[:SEED]|off)");
    TEST_ASSERT_EQUAL_UINT32(0U, flash_info().cut_in);
    TEST_ASSERT_NOT_NULL(strstr(exchange("d8 flash.status"), " cut=none cut_in=0 cut_seed=0"));
}

/* The content the cut tests start from: what the script leaves on an erased flash, so that its
 * erases tear sectors that hold data. */
static void prepare_before(void)
{
    (void)memset(s_file, 0xFF, sizeof(s_file));
    flash_script_replay(s_file, s_before, 0U, 0U);
    write_file(s_path, s_before, DEVICE_SIZE);
}

static void test_a_cut_from_simulation_control_tears_what_the_model_tears(void)
{
    const uint32_t seeds[] = {0U, 1U, 4242U};
    uint8_t unit[8] = {0U};

    for (uint32_t s = 0U; s < (sizeof(seeds) / sizeof(seeds[0])); s++) {
        for (uint32_t cut = 1U; cut <= FLASH_SCRIPT_WRITES; cut++) {
            char request[64];
            flash_status_t status = FLASH_OK;

            prepare_before();
            atomic_store(&s_power_losses, 0U);
            open_flash(&FAST);
            (void)snprintf(request, sizeof(request), "e1 flash.cut %lu:%lu", (unsigned long)cut,
                           (unsigned long)seeds[s]);
            TEST_ASSERT_EQUAL_STRING_LEN("e1 ok cut=armed", exchange(request), 15U);

            /* The script stops at the operation cut: the ones before it were done. */
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(
                cut - 1U, flash_script_run(plat_flash_device(), &status), at("cut", (long)cut));
            TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, status);
            TEST_ASSERT_EQUAL_UINT32(1U, atomic_load(&s_power_losses));
            TEST_ASSERT_TRUE(flash_info().cut_fired);
            TEST_ASSERT_NOT_NULL(strstr(exchange("e2 flash.status"), " cut=fired cut_in=0"));

            /* The supply stays gone: nothing more is done. */
            TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST,
                                  plat_flash_device()->read(NULL, 0U, unit, 8U));
            TEST_ASSERT_EQUAL_UINT32(1U, atomic_load(&s_power_losses));
            posix_flash_close();

            /* Exactly the model's tear, byte for byte. */
            flash_script_replay(s_before, s_expect, cut, seeds[s]);
            read_file();
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(s_expect, s_file, DEVICE_SIZE,
                                                 at("cut", (long)cut));
        }
    }
}

/* ------------------------------------------------------------------- a cut ends the run */

/* A child that runs the script with a retained cell and the flash, a cut armed as --flash-cut
 * arms it, and the default end of a run by a power cut; its stderr goes to s_stderr. With a
 * `keeper`, the child forks it once both files are mapped, so that mappings of both outlive the
 * child. A run whose cut does not fire ends as a reset ends one: the locks released, no close.
 * Returns the child's exit status, or -1 if it did not exit. */
static int run_child_with_cut(uint32_t cut, uint32_t seed, const map_keeper_t *keeper)
{
    const pid_t pid = fork();
    int status = -1;

    TEST_ASSERT_TRUE(pid >= 0);

    if (pid == 0) {
        const int err = open(s_stderr, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const posix_flash_cut_t armed = {cut, seed};

        if ((err < 0) || (dup2(err, STDERR_FILENO) < 0)) {
            _exit(4);
        }

        posix_flash_set_power_loss(NULL);

        if (!posix_retained_open(s_cell, s_error, sizeof(s_error)) ||
            !posix_flash_open(s_path, &FAST, s_error, sizeof(s_error))) {
            _exit(2);
        }

        if ((keeper != NULL) && (map_keeper_fork(keeper) < 0)) {
            _exit(5);
        }

        posix_flash_arm_cut(&armed);

        const int result =
            (flash_script_run(plat_flash_device(), NULL) == FLASH_SCRIPT_WRITES) ? 0 : 3;

        posix_retained_unlock();
        posix_flash_unlock();
        _exit(result);
    }

    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* What the child wrote to stderr. */
static const char *child_stderr(void)
{
    static char text[1024];
    const int fd = open(s_stderr, O_RDONLY);
    ssize_t n = -1;

    if (fd >= 0) {
        n = read(fd, text, sizeof(text) - 1U);
        (void)close(fd);
    }

    text[(n > 0) ? n : 0] = '\0';
    return text;
}

static void test_a_cut_ends_the_process_as_a_power_loss_with_the_models_tear(void)
{
    const uint32_t seeds[] = {7U, 1000U};

    for (uint32_t s = 0U; s < (sizeof(seeds) / sizeof(seeds[0])); s++) {
        for (uint32_t cut = 1U; cut <= FLASH_SCRIPT_WRITES; cut++) {
            char line[80];
            posix_retained_info_t cell;

            prepare_before();
            (void)unlink(s_cell);
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)PLAT_RESET_POWER_LOSS,
                                          run_child_with_cut(cut, seeds[s], NULL),
                                          at("exit status, cut", (long)cut));

            /* The FATAL line names the operation. */
            (void)snprintf(line, sizeof(line), "FATAL flash: power cut in operation %lu (",
                           (unsigned long)cut);
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(child_stderr(), line), child_stderr());

            /* The file holds exactly what the model leaves for this cut and seed. */
            flash_script_replay(s_before, s_expect, cut, seeds[s]);
            read_file();
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(s_expect, s_file, DEVICE_SIZE,
                                                 at("cut", (long)cut));

            /* The retained cell went with the supply: the next start is a power-on. */
            TEST_ASSERT_TRUE(posix_retained_open(s_cell, s_error, sizeof(s_error)));
            posix_retained_get_info(&cell);
            posix_retained_close();
            TEST_ASSERT_EQUAL_INT((int)PLAT_RESET_POWER_ON, (int)cell.cause);
            TEST_ASSERT_EQUAL_STRING("magic", cell.verdict);
        }
    }

    /* A cut beyond the run never fires: the run ends as it would without one. */
    prepare_before();
    TEST_ASSERT_EQUAL_INT(0, run_child_with_cut(FLASH_SCRIPT_WRITES + 1U, 3U, NULL));
    flash_script_replay(s_before, s_expect, 0U, 0U);
    read_file();
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_expect, s_file, DEVICE_SIZE);
    TEST_ASSERT_NULL(strstr(child_stderr(), "FATAL"));
}

/* The end without a kernel releases both locks itself before the process exits
 * (docs/bug-log.md, 012). A keeper forked by the child keeps mappings of the flash file and of
 * the cell, and with them the two open file descriptions the child locked, alive after the
 * child has ended with 12: a lock left to the exit would still be held when the parent has
 * seen that exit, and the next process, started at once as a bench starts it, could open
 * neither file. The first and the last write of the script, and one in between. */
static void test_a_cut_leaves_both_files_free_while_mappings_of_them_outlive_the_process(void)
{
    const uint32_t cuts[] = {1U, 4U, FLASH_SCRIPT_WRITES};

    for (uint32_t i = 0U; i < (sizeof(cuts) / sizeof(cuts[0])); i++) {
        map_keeper_t keeper;
        posix_retained_info_t cell;

        prepare_before();
        (void)unlink(s_cell);
        TEST_ASSERT_TRUE(map_keeper_prepare(&keeper));
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)PLAT_RESET_POWER_LOSS,
                                      run_child_with_cut(cuts[i], 11U, &keeper),
                                      at("exit status, cut", (long)cuts[i]));

        /* Another process can lock each file, */
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, lock_from_another_process(s_path),
                                      at("the flash file after cut", (long)cuts[i]));
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, lock_from_another_process(s_cell),
                                      at("the cell after cut", (long)cuts[i]));

        /* and the next start opens both: the cell went with the supply. */
        TEST_ASSERT_TRUE_MESSAGE(posix_retained_open(s_cell, s_error, sizeof(s_error)), s_error);
        posix_retained_get_info(&cell);
        posix_retained_close();
        TEST_ASSERT_EQUAL_STRING("magic", cell.verdict);
        open_flash(&FAST);
        posix_flash_close();

        /* The file holds the model's tear: the keeper's mapping changed nothing in it. */
        flash_script_replay(s_before, s_expect, cuts[i], 11U);
        read_file();
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(s_expect, s_file, DEVICE_SIZE,
                                             at("cut", (long)cuts[i]));
        TEST_ASSERT_TRUE(map_keeper_end(&keeper));
    }
}

/* ------------------------------------------------------------------ from another thread */

static atomic_bool s_serving;

/* The hardware thread's part: serve simulation control until told to stop. */
static void *serve(void *arg)
{
    struct pollfd fd = {.fd = posix_sim_fd(), .events = POLLIN, .revents = 0};

    (void)arg;

    while (atomic_load(&s_serving)) {
        (void)poll(&fd, 1U, 1);
        posix_sim_service();
    }

    return NULL;
}

/* One request answered by the serving thread. */
static const char *ask(const char *request)
{
    const char *reply;

    send_request(request);
    reply = receive_reply(2000);
    return (reply != NULL) ? reply : "<no reply>";
}

static void test_commands_from_another_thread_reach_the_operations(void)
{
    pthread_t thread;
    uint32_t rounds = 0U;
    flash_status_t status = FLASH_OK;

    (void)memset(s_file, 0xFF, sizeof(s_file));
    write_file(s_path, s_file, DEVICE_SIZE);
    open_flash(&FAST);
    atomic_store(&s_serving, true);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, serve, NULL));

    /* Statuses while the operations run, then a cut 25 operations ahead. */
    for (uint32_t i = 0U; i < 20U; i++) {
        TEST_ASSERT_EQUAL_UINT32(FLASH_SCRIPT_WRITES, flash_script_run(plat_flash_device(), NULL));
        TEST_ASSERT_EQUAL_STRING_LEN("f1 ok flash=on", ask("f1 flash.status"), 14U);
    }

    TEST_ASSERT_EQUAL_STRING_LEN("f2 ok cut=armed cut_in=25", ask("f2 flash.cut 25:5"), 25U);

    while ((status == FLASH_OK) && (rounds < 10U)) {
        (void)flash_script_run(plat_flash_device(), &status);
        rounds++;
    }

    TEST_ASSERT_EQUAL_INT(FLASH_E_POWER_LOST, status);
    TEST_ASSERT_EQUAL_UINT32(3U, rounds);                               /* 25 = 2 x 10 + 5 */
    TEST_ASSERT_NOT_NULL(strstr(ask("f3 flash.status"), " programs=")); /* still served */
    atomic_store(&s_serving, false);
    TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));

    /* Every operation counted: 22 whole scripts and 5 operations of the 23rd. */
    TEST_ASSERT_EQUAL_UINT64((22U * 6U) + 3U, flash_info().programs);
    TEST_ASSERT_EQUAL_UINT64((22U * 4U) + 2U, flash_info().erases);
    TEST_ASSERT_EQUAL_UINT32(1U, atomic_load(&s_power_losses));
}

int main(void)
{
    struct sockaddr_in any;
    socklen_t len = sizeof(any);
    const char *tmp = getenv("TMPDIR");
    int probe;

    (void)snprintf(s_dir, sizeof(s_dir), "%s/fcn-flash-XXXXXX",
                   ((tmp != NULL) && (tmp[0] != '\0')) ? tmp : "/tmp");

    if (mkdtemp(s_dir) == NULL) {
        (void)printf("cannot create a directory for the test: %s\n", strerror(errno));
        return 1;
    }

    (void)snprintf(s_path, sizeof(s_path), "%s/flash.bin", s_dir);
    (void)snprintf(s_cell, sizeof(s_cell), "%s/cell.bin", s_dir);
    (void)snprintf(s_stderr, sizeof(s_stderr), "%s/stderr.txt", s_dir);

    /* Let the kernel pick a free port, then give it to simulation control. */
    probe = socket(AF_INET, SOCK_DGRAM, 0);
    (void)memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if ((probe < 0) || (bind(probe, (const struct sockaddr *)&any, sizeof(any)) != 0) ||
        (getsockname(probe, (struct sockaddr *)&any, &len) != 0)) {
        (void)printf("cannot find a free port\n");
        return 1;
    }

    s_port = ntohs(any.sin_port);
    (void)close(probe);

    if (!posix_sim_open(s_port, s_error, sizeof(s_error))) {
        (void)printf("%s\n", s_error);
        return 1;
    }

    s_client = socket(AF_INET, SOCK_DGRAM, 0);

    UNITY_BEGIN();
    RUN_TEST(test_the_layout_is_the_documented_table);
    RUN_TEST(test_without_a_file_there_is_no_device_and_nothing_to_cut);
    RUN_TEST(test_a_missing_file_is_created_erased_and_then_opened_as_it_is);
    RUN_TEST(test_the_flash_survives_a_restart_of_the_process);
    RUN_TEST(test_a_file_of_another_size_is_refused_and_left_alone);
    RUN_TEST(test_a_file_that_cannot_be_used_is_refused_with_the_reason);
    RUN_TEST(test_the_file_is_locked_while_it_is_open);
    RUN_TEST(test_a_lock_released_while_the_file_stays_open_lets_another_process_take_it);
    RUN_TEST(test_nothing_stored_after_the_file_is_given_up_reaches_the_file);
    RUN_TEST(test_the_close_frees_the_file_while_another_process_still_maps_it);
    RUN_TEST(test_a_cut_is_written_n_or_n_colon_seed);
    RUN_TEST(test_the_device_follows_flash_h_and_counts_what_it_did);
    RUN_TEST(test_an_operation_waits_through_the_wait_function_until_its_time_has_passed);
    RUN_TEST(test_flash_status_names_every_count);
    RUN_TEST(test_flash_cut_arms_a_cut_and_the_status_counts_it_down);
    RUN_TEST(test_a_wrong_flash_command_is_refused_with_its_usage);
    RUN_TEST(test_a_cut_from_simulation_control_tears_what_the_model_tears);
    RUN_TEST(test_a_cut_ends_the_process_as_a_power_loss_with_the_models_tear);
    RUN_TEST(test_a_cut_leaves_both_files_free_while_mappings_of_them_outlive_the_process);
    RUN_TEST(test_commands_from_another_thread_reach_the_operations);

    const int status = UNITY_END();

    (void)close(s_client);
    posix_sim_close();
    (void)unlink(s_path);
    (void)unlink(s_cell);
    (void)unlink(s_stderr);
    (void)rmdir(s_dir);
    return status;
}
