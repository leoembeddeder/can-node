/* SPDX-License-Identifier: MIT */
/*
 * Self-tests of the host's flash (plat_flash.h, posix_flash.c) that need real time or the
 * FreeRTOS scheduler. CTest runs them through run_selftest.sh (tests/platform/CMakeLists.txt).
 *
 *   waits <path>             without a kernel, as the bootloader and the tools run: every
 *                            program and erase of the W25Q128JV figures (0.4 ms, 45 ms) takes
 *                            at least its modelled time (else a failure: the sleep must never be
 *                            short) and at most WAIT_LATE_MS more (else status 75: the host did
 *                            not wake the process in time)
 *   task-wait <path>         the same under the scheduler, with the node's wait function (one
 *                            tick of vTaskDelay per look at the busy flag): at least the modelled
 *                            time, at most two ticks and WAIT_LATE_MS more (else 75); and the
 *                            flash task is blocked while it waits: a task of lower priority
 *                            runs during every erase (else a failure)
 *   task-cut <flash> <cell> <N[:SEED]>
 *                            a cut of the N-th program or erase under the scheduler ends the
 *                            child through plat_reset(PLAT_RESET_POWER_LOSS) with status 12, the
 *                            flash file holds exactly the tear lib/flash's model gives for N and
 *                            SEED on a copy, the retained cell is gone, stderr has the FATAL
 *                            line, and the log the line logged just before (plat_reset() writes
 *                            out the log; an end that skipped it would lose the line); N past
 *                            the script's end lets the child end in order. Either way the
 *                            child leaves a keeper of its mappings of both files behind
 *                            (map_keeper.h), and the parent opens the cell and the flash at
 *                            once: the child released both locks itself, at the reset or at
 *                            its closes (docs/bug-log.md, 012)
 *   end-while-writing <watchdog|guard> <flash> <cell>
 *                            a run that is ended from outside the scheduler while a task
 *                            programs the flash: by the hardware watchdog (plat_reset() from
 *                            the watchdog's thread, status 11) or by the shutdown guard (the
 *                            logger thread, status 70). Neither stops the task before the exit
 *                            does, and both release the locks of the cell and the flash file
 *                            first (posix_lifecycle.c). This program's _exit() holds the end
 *                            open at that point until the task has made more programs: none of
 *                            them may be in the file, which another process could have locked
 *                            by then. Its flock() sees the order of the release: when the
 *                            flash file's lock is removed, the mapping of the file must be the
 *                            private one already. And the parent opens the cell and the flash
 *                            at once, while a keeper still has the child's mappings: the end
 *                            released both locks, the guard's as well as the reset's
 *
 * A reset ends the process, so task-cut and end-while-writing play the bench as wdg_selftest.c
 * does: they fork, the child runs the FreeRTOS program, and the parent, which never starts the
 * platform, looks at the exit status and the files.
 *
 * Exit status: 0 passed, 1 failed, 2 usage, 75 (EX_TEMPFAIL) a timing tolerance was missed
 * although the mechanism itself worked.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"

#include "flash/nor.h"
#include "flash_script.h"
#include "map_keeper.h"
#include "plat_flash.h"
#include "plat_wdg.h"
#include "posix_flash.h"
#include "posix_platform.h"
#include "posix_retained.h"

#define TIMING_MISSED  75
#define WAIT_LATE_MS   20U /* an operation may end this much after its modelled time */
#define NS_PER_MS      1000000ULL
#define TASK_STACK     configMINIMAL_STACK_SIZE
#define DEVICE_SIZE    FLASH_SCRIPT_DEVICE_SIZE
#define CHILD_LIMIT_MS 30000U

static uint8_t s_before[DEVICE_SIZE];
static uint8_t s_expect[DEVICE_SIZE];
static uint8_t s_file[DEVICE_SIZE];

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------------------ the waits */

typedef struct {
    uint32_t operations;
    uint32_t short_ones; /* ended before their modelled time: a defect */
    uint32_t late_ones;  /* ended more than the allowance after it: the host */
    uint64_t worst_late_ns;
} wait_result_t;

/* One operation, timed against its modelled time and the allowance after it. */
static void time_operation(const flash_dev_t *dev, bool erase, uint32_t addr, uint64_t allowed_ns,
                           wait_result_t *result)
{
    static const uint8_t unit[8] = {0x12U, 0x34U, 0x56U, 0x78U, 0x9AU, 0xBCU, 0xDEU, 0xF0U};
    const uint64_t modelled =
        erase ? (uint64_t)POSIX_FLASH_SECTOR_ERASE_NS : (uint64_t)POSIX_FLASH_PAGE_PROGRAM_NS;
    const uint64_t start = monotonic_ns();
    const flash_status_t status =
        erase ? dev->erase(dev->ctx, addr) : dev->program(dev->ctx, addr, unit, sizeof(unit));
    const uint64_t took = monotonic_ns() - start;

    result->operations++;

    if (status != FLASH_OK) {
        (void)fprintf(stderr, "operation at 0x%06lx: status %d\n", (unsigned long)addr,
                      (int)status);
        result->short_ones++;
    } else if (took < modelled) {
        (void)fprintf(stderr, "%s at 0x%06lx took %llu ns, less than its %llu ns\n",
                      erase ? "erase" : "program", (unsigned long)addr, (unsigned long long)took,
                      (unsigned long long)modelled);
        result->short_ones++;
    } else if (took > (modelled + allowed_ns)) {
        result->late_ones++;
    } else {
        /* within the tolerance */
    }

    if ((took > modelled) && ((took - modelled) > result->worst_late_ns)) {
        result->worst_late_ns = took - modelled;
    }
}

/* Ten programs and four erases in slot B, timed one by one. */
static void time_operations(const flash_dev_t *dev, uint64_t allowed_ns, wait_result_t *result)
{
    for (uint32_t i = 0U; i < 4U; i++) {
        time_operation(dev, true, 0x1F0000U + (i * 4096U), allowed_ns, result);
    }

    for (uint32_t i = 0U; i < 10U; i++) {
        time_operation(dev, false, 0x1F0000U + (i * 264U), allowed_ns, result);
    }
}

static int verdict(const char *what, const wait_result_t *result, uint64_t allowed_ns)
{
    (void)printf("{\"test\": \"%s\", \"operations\": %u, \"short\": %u, \"late\": %u, "
                 "\"worst_late_ns\": %llu, \"allowed_late_ns\": %llu}\n",
                 what, (unsigned int)result->operations, (unsigned int)result->short_ones,
                 (unsigned int)result->late_ones, (unsigned long long)result->worst_late_ns,
                 (unsigned long long)allowed_ns);

    int status = EXIT_SUCCESS;

    if ((result->short_ones != 0U) || (result->operations != 14U)) {
        status = EXIT_FAILURE;
    } else if (result->late_ones != 0U) {
        (void)fprintf(stderr, "%u operations ended more than %llu ns after their time\n",
                      (unsigned int)result->late_ones, (unsigned long long)allowed_ns);
        status = TIMING_MISSED;
    } else {
        /* passed */
    }

    return status;
}

static bool open_fresh(const char *path)
{
    char error[512];

    (void)unlink(path);

    if (!posix_flash_open(path, NULL, error, sizeof(error))) {
        (void)fprintf(stderr, "%s\n", error);
        return false;
    }

    return true;
}

static int cmd_waits(const char *path)
{
    const uint64_t allowed = (uint64_t)WAIT_LATE_MS * NS_PER_MS;
    wait_result_t result = {0U, 0U, 0U, 0U};

    if (!open_fresh(path)) {
        return EXIT_FAILURE;
    }

    time_operations(plat_flash_device(), allowed, &result);
    posix_flash_close();
    (void)unlink(path);
    return verdict("waits", &result, allowed);
}

/* ------------------------------------------------------------------- under the scheduler */

static void init_platform(const char *log_path)
{
    const posix_config_t config = {.tick_mode = POSIX_TICK_LOCKED,
                                   .irq_mode = POSIX_IRQ_SIGNAL,
                                   .log_level = LOG_LEVEL_WARN,
                                   .log_path = log_path};

    posix_platform_init(&config);
}

static void create_task(TaskFunction_t code, const char *name, UBaseType_t priority, void *arg)
{
    const BaseType_t created = xTaskCreate(code, name, TASK_STACK, arg, priority, NULL);

    configASSERT(created == pdPASS);
}

static void idle_forever(void)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* The node's wait (platform/posix/node_main.c): one tick of vTaskDelay per look. */
static void wait_one_tick(void)
{
    vTaskDelay(1U);
}

static atomic_uint_fast64_t s_background; /* passes of the low-priority task */
static wait_result_t s_task_result;
static uint32_t s_erases_without_background;
static uint64_t s_allowed_ns; /* set before the scheduler starts */

/* Runs whenever the flash task does not: counts its passes. */
static void background_task(void *arg)
{
    (void)arg;
    plat_task_enter("background");

    for (;;) {
        (void)atomic_fetch_add(&s_background, 1U);
    }
}

static void flash_task(void *arg)
{
    const uint64_t allowed = s_allowed_ns;
    const flash_dev_t *dev = plat_flash_device();

    (void)arg;
    plat_task_enter("flash");

    for (uint32_t i = 0U; i < 4U; i++) {
        const uint64_t before = atomic_load(&s_background);

        time_operation(dev, true, 0x1F0000U + (i * 4096U), allowed, &s_task_result);

        if (atomic_load(&s_background) == before) {
            s_erases_without_background++;
        }
    }

    for (uint32_t i = 0U; i < 10U; i++) {
        time_operation(dev, false, 0x1F0000U + (i * 264U), allowed, &s_task_result);
    }

    plat_end_scheduler();
    idle_forever();
}

static int cmd_task_wait(const char *path)
{
    /* Two ticks: a look can come up to one tick after the time has passed, and the tick that
     * wakes the task may itself come late by up to one. */
    const uint64_t allowed = ((uint64_t)WAIT_LATE_MS + 2U) * NS_PER_MS;

    if (!open_fresh(path)) {
        return EXIT_FAILURE;
    }

    posix_flash_set_wait(wait_one_tick);
    init_platform(NULL);
    s_allowed_ns = allowed;
    create_task(flash_task, "flash", 2U, NULL);
    create_task(background_task, "background", 1U, NULL);
    posix_platform_run();
    posix_platform_shutdown();
    posix_flash_close();
    (void)unlink(path);

    const int status = verdict("task-wait", &s_task_result, allowed);

    if (s_erases_without_background != 0U) {
        (void)fprintf(stderr,
                      "%u erases in which the task below the flash task did not run: the wait "
                      "did not block the flash task\n",
                      (unsigned int)s_erases_without_background);
        return EXIT_FAILURE;
    }

    return status;
}

/* ------------------------------------------------------------------- a cut under the kernel */

typedef struct {
    const char *flash;
    const char *cell;
    const char *log; /* the child's log file */
    posix_flash_cut_t cut;
    const map_keeper_t *keeper; /* of the child's mappings of both files */
    bool by_guard;              /* end-while-writing: the shutdown guard ends the run */
} cut_plan_t;

/* Logged right before the script: the platform's end of a power cut is plat_reset(), which
 * writes out what the log still holds, so the line is in the log file after the cut. (The end
 * without a kernel would _exit() at once; the logger thread writes every 5 ms, and with the
 * operations taking no time the cut comes long before that.) */
#define LAST_LINE "the last line before the flash script"

static void script_task(void *arg)
{
    (void)arg;
    plat_task_enter("script");
    LOG_WRN("script", LAST_LINE);

    const uint32_t writes = flash_script_run(plat_flash_device(), NULL);

    /* Only reached when no cut fired. */
    (void)fprintf(stderr, "script ended after %u writes\n", (unsigned int)writes);
    plat_end_scheduler();
    idle_forever();
}

/* The child: the retained cell and the flash as the node opens them, a keeper of the two
 * mappings forked before any thread exists, the cut armed as --flash-cut arms it, the node's
 * wait, and the platform's end of a power cut. The operations take no time, so that the cut
 * comes at once (see LAST_LINE). */
static int child_cut(const cut_plan_t *plan)
{
    const nor_timing_t instant = {.page_program_ns = 0U, .sector_erase_ns = 0U};
    char error[512];

    if (!posix_retained_open(plan->cell, error, sizeof(error)) ||
        !posix_flash_open(plan->flash, &instant, error, sizeof(error))) {
        (void)fprintf(stderr, "%s\n", error);
        posix_retained_close(); /* open if it was the flash that failed */
        return 3;
    }

    if (map_keeper_fork(plan->keeper) < 0) {
        (void)fprintf(stderr, "cannot fork the keeper of the mappings\n");
        posix_flash_close();
        posix_retained_close();
        return 3;
    }

    posix_flash_set_wait(wait_one_tick);
    posix_flash_arm_cut(&plan->cut);
    init_platform(plan->log);
    create_task(script_task, "script", 2U, NULL);
    posix_platform_run();
    posix_platform_shutdown();
    posix_flash_close();
    posix_retained_close();
    return EXIT_SUCCESS;
}

static bool read_whole(const char *path, uint8_t *out, size_t len)
{
    struct stat info;
    const int fd = open(path, O_RDONLY);
    bool ok = (fd >= 0) && (fstat(fd, &info) == 0) && (info.st_size == (off_t)len) &&
              (read(fd, out, len) == (ssize_t)len);

    if (fd >= 0) {
        (void)close(fd);
    }

    return ok;
}

static bool write_whole(const char *path, const uint8_t *data, size_t len)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const bool ok = (fd >= 0) && (write(fd, data, len) == (ssize_t)len);

    if (fd >= 0) {
        (void)close(fd);
    }

    return ok;
}

/* Fork, run `child` with its stderr in `errors`, and return its exit status (-1 if it did not
 * exit, or was killed after CHILD_LIMIT_MS). */
static int run_child(int (*child)(const cut_plan_t *plan), const cut_plan_t *plan, FILE *errors)
{
    int wstatus = 0;
    pid_t done = 0;

    (void)fflush(NULL);

    const pid_t pid = fork();

    if (pid == 0) {
        (void)dup2(fileno(errors), STDERR_FILENO);
        (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
        _exit(child(plan));
    }

    const uint64_t deadline = monotonic_ns() + ((uint64_t)CHILD_LIMIT_MS * NS_PER_MS);

    while ((pid > 0) && (done == 0)) {
        const struct timespec nap = {0, 1000000L};

        done = waitpid(pid, &wstatus, WNOHANG);

        if ((done == 0) && (monotonic_ns() > deadline)) {
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &wstatus, 0);
            done = -1;
        } else if (done == 0) {
            (void)nanosleep(&nap, NULL);
        } else {
            /* ended */
        }
    }

    return ((pid > 0) && (done == pid) && WIFEXITED(wstatus)) ? WEXITSTATUS(wstatus) : -1;
}

/* The child's stderr: copied to this process's stderr, and searched for `needle`. */
static bool child_said(FILE *errors, const char *needle)
{
    char line[512];
    bool found = false;

    (void)fseek(errors, 0L, SEEK_SET);

    while (fgets(line, sizeof(line), errors) != NULL) {
        (void)fputs(line, stderr);
        found = found || (strstr(line, needle) != NULL);
    }

    return found;
}

/* True if a line of the file at `path` contains `needle`. */
static bool file_has(const char *path, const char *needle)
{
    char line[512];
    bool found = false;
    FILE *file = fopen(path, "r");

    while ((file != NULL) && !found && (fgets(line, sizeof(line), file) != NULL)) {
        found = (strstr(line, needle) != NULL);
    }

    if (file != NULL) {
        (void)fclose(file);
    }

    return found;
}

static int cmd_task_cut(const char *flash, const char *cell, const char *spec)
{
    char log[600];
    map_keeper_t keeper;
    cut_plan_t plan = {flash, cell, log, {0U, 0U}, &keeper, false};
    char error[256];
    char fatal[96];
    posix_retained_info_t retained;
    bool ok = true;

    if (!posix_flash_parse_cut(spec, &plan.cut, error, sizeof(error))) {
        (void)fprintf(stderr, "%s\n", error);
        return 2;
    }

    /* The content the script tears: what it leaves on an erased flash. */
    (void)memset(s_file, 0xFF, sizeof(s_file));
    flash_script_replay(s_file, s_before, 0U, 0U);
    (void)unlink(cell);
    (void)snprintf(log, sizeof(log), "%s.log", flash);
    (void)unlink(log);

    if (!write_whole(flash, s_before, DEVICE_SIZE)) {
        (void)fprintf(stderr, "cannot write %s\n", flash);
        return EXIT_FAILURE;
    }

    FILE *errors = tmpfile();

    if (errors == NULL) {
        return EXIT_FAILURE;
    }

    if (!map_keeper_prepare(&keeper)) {
        (void)fclose(errors);
        return EXIT_FAILURE;
    }

    const bool fires = (plan.cut.operation <= FLASH_SCRIPT_WRITES);
    const int status = run_child(child_cut, &plan, errors);
    const int expected = fires ? (int)PLAT_RESET_POWER_LOSS : EXIT_SUCCESS;

    (void)snprintf(fatal, sizeof(fatal), "FATAL flash: power cut in operation %lu (",
                   (unsigned long)plan.cut.operation);

    if (child_said(errors, fatal) != fires) {
        (void)fprintf(stderr, "stderr %s the line \"%s\"\n", fires ? "lacks" : "has", fatal);
        ok = false;
    }

    (void)fclose(errors);

    if (status != expected) {
        (void)fprintf(stderr, "the child exited with %d, expected %d\n", status, expected);
        ok = false;
    }

    /* The run ended through the platform's reset, which wrote out the log. */
    if (!file_has(log, " W script: " LAST_LINE)) {
        (void)fprintf(stderr, "the log %s lacks the line logged before the script\n", log);
        ok = false;
    }

    (void)unlink(log);

    flash_script_replay(s_before, s_expect, fires ? plan.cut.operation : 0U, plan.cut.seed);

    if (!read_whole(flash, s_file, DEVICE_SIZE) || (memcmp(s_file, s_expect, DEVICE_SIZE) != 0)) {
        (void)fprintf(stderr, "the flash file is not what lib/flash's model leaves for cut %s\n",
                      spec);
        ok = false;
    }

    /* After a power cut the cell is gone; after an orderly end it is a valid one. The open
     * comes while the keeper still has the child's mapping of the cell: the child released the
     * cell's lock itself. */
    if (!posix_retained_open(cell, error, sizeof(error))) {
        (void)fprintf(stderr, "%s\n", error);
        ok = false;
    } else {
        posix_retained_get_info(&retained);
        posix_retained_close();

        if (fires && (strcmp(retained.verdict, "magic") != 0)) {
            (void)fprintf(stderr, "the retained cell survived the power cut: %s\n",
                          retained.verdict);
            ok = false;
        } else if (!fires && (strcmp(retained.verdict, "no reset") != 0)) {
            (void)fprintf(stderr, "the retained cell after an orderly end: %s\n", retained.verdict);
            ok = false;
        } else {
            /* as expected */
        }
    }

    /* The same for the flash file: the next start opens and locks it at once. */
    if (!posix_flash_open(flash, NULL, error, sizeof(error))) {
        (void)fprintf(stderr, "%s\n", error);
        ok = false;
    } else {
        posix_flash_close();
    }

    if (!map_keeper_end(&keeper)) {
        (void)fprintf(stderr, "the keeper of the child's mappings did not end\n");
        ok = false;
    }

    (void)printf("{\"test\": \"task-cut\", \"cut\": \"%s\", \"status\": %d, \"ok\": %s}\n", spec,
                 status, ok ? "true" : "false");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* ------------------------------- an end from outside the scheduler, while a task writes */

/* The writer programs this many units before it starts the watchdog or arms the guard: they
 * are in the file whenever the end comes. */
#define WRITES_BEFORE_THE_END 4U
/* The end is held open until the writer has completed this many more programs. */
#define WRITES_AFTER_THE_RELEASE 8U
/* How long the held end waits for them: a bound, not a tolerance (a program takes a tick or
 * two). */
#define WRITES_LIMIT_MS 10000U
/* The watchdog's timeout, and the guard's: the writer goes on writing for that long. */
#define END_AFTER_MS 50U

/* What the held end found, as the child's exit status in place of the end's own. */
#define END_FILE_CHANGED     81 /* a program made after the release is in the file */
#define END_NO_MORE_WRITES   82 /* the task programmed nothing more: nothing was shown */
#define END_NOT_READ         83 /* the flash file could not be read */
#define END_NOT_FROM_OUTSIDE 84 /* no start, or the scheduler ended */
#define END_UNLOCKED_SHARED  85 /* the lock was removed while the mapping was still shared */

static const char *s_end_flash;        /* the flash file the held end reads; NULL: no hold */
static atomic_uint_fast32_t s_written; /* programs the writer has completed */
static atomic_bool s_writer_failed;    /* a program was refused, or the watchdog did not start */
static atomic_bool s_unlocked_shared;  /* see __wrap_flock() */

/* Unit n of slot B gets the number n: every program changes the file for as long as the
 * mapping is the shared one, and no two programs store the same. */
static void unit_of(uint32_t n, uint8_t unit[8])
{
    (void)memset(unit, 0, 8U);

    for (uint32_t i = 0U; i < 4U; i++) {
        unit[i] = (uint8_t)(n >> (8U * i));
    }
}

/* The flash task of this test: one unit after the other, each program waited for as the node
 * waits (a tick per look), for longer than the test runs. After the first few it starts the
 * hardware watchdog, which it never kicks, or arms the shutdown guard of a scheduler that it
 * never ends. */
static void writer_task(void *arg)
{
    const cut_plan_t *plan = arg;
    const flash_dev_t *dev = plat_flash_device();
    const uint32_t base = plat_flash_layout()->slot[1].offset;
    const uint32_t units = plat_flash_layout()->slot[1].size / 8U;

    plat_task_enter("writer");

    for (uint32_t n = 0U; n < units; n++) {
        uint8_t unit[8];

        if (n == WRITES_BEFORE_THE_END) {
            if (plan->by_guard) {
                posix_shutdown_guard_arm(END_AFTER_MS);
            } else if (!plat_wdg_start(END_AFTER_MS)) {
                atomic_store(&s_writer_failed, true);
            } else {
                /* started, and never kicked */
            }
        }

        unit_of(n, unit);

        if (dev->program(dev->ctx, base + (n * 8U), unit, sizeof(unit)) != FLASH_OK) {
            atomic_store(&s_writer_failed, true);
        }

        (void)atomic_fetch_add(&s_written, 1U);
    }

    idle_forever(); /* the slot is full, minutes after the end was due */
}

/* True if this process has a shared mapping of the file with this inode number: a line of
 * /proc/self/maps ("from-to perms offset dev inode path") with the number and permissions that
 * end in 's'. Read without stdio, in the thread that ends the run. A list too long for the
 * buffer counts as shared: a check that cannot be made must not pass. */
static bool mapped_shared(ino_t inode)
{
    static char maps[512U * 1024U];
    const int fd = open("/proc/self/maps", O_RDONLY);
    size_t length = 0U;
    ssize_t got = 1;
    bool shared = (fd < 0);

    while ((fd >= 0) && (got > 0) && (length < (sizeof(maps) - 1U))) {
        got = read(fd, &maps[length], sizeof(maps) - 1U - length);

        if (got > 0) {
            length += (size_t)got;
        }
    }

    if (fd >= 0) {
        (void)close(fd);
    }

    maps[length] = '\0';
    shared = shared || (length == (sizeof(maps) - 1U));

    for (const char *line = maps; line != NULL;) {
        char *const end = strchr(line, '\n'); /* in maps, which is this function's to change */

        if (end != NULL) {
            *end = '\0';
        }

        const char *const perms = strchr(line, ' ');
        const char *field = perms;

        /* Three fields on from the permissions: the offset, the device, the inode. */
        for (uint32_t skipped = 0U; (field != NULL) && (skipped < 3U); skipped++) {
            field = strchr(&field[1], ' ');
        }

        if ((perms != NULL) && (field != NULL) && (strlen(perms) > 4U) && (perms[4] == 's') &&
            (strtoull(&field[1], NULL, 10) == (unsigned long long)inode)) {
            shared = true;
        }

        line = (end != NULL) ? &end[1] : NULL;
    }

    return shared;
}

/* Every flock() of this program and of the platform comes here (-Wl,--wrap=flock). The order
 * of the release, seen where it is made: when the lock of the held end's flash file is removed,
 * this process's mapping of the file must be private already. From the removal on another
 * process may have the file, and a store through a mapping that is still shared would reach
 * it (posix_flash.c, posix_flash_unlock()). The closes unmap first and are not concerned. */
int __real_flock(int fd, int operation);
int __wrap_flock(int fd, int operation);

int __wrap_flock(int fd, int operation)
{
    const char *const flash = s_end_flash;
    struct stat file;
    struct stat given;

    if ((operation == LOCK_UN) && (flash != NULL) && (stat(flash, &file) == 0) &&
        (fstat(fd, &given) == 0) && (given.st_ino == file.st_ino) &&
        (given.st_dev == file.st_dev) && mapped_shared(file.st_ino)) {
        atomic_store(&s_unlocked_shared, true);
    }

    return __real_flock(fd, operation);
}

/* The end, held open. This program's _exit() calls it in the thread that ends the run, after
 * that end has released the locks (posix_lifecycle.c, release_and_exit()). The thread is
 * outside the scheduler, so the writer goes on meanwhile. The file as it is now, more programs
 * of the writer, and the file again: it must be the same. Returns the status to exit with. */
static int held_end(int status)
{
    const char *const flash = s_end_flash;
    const uint64_t deadline = monotonic_ns() + ((uint64_t)WRITES_LIMIT_MS * NS_PER_MS);
    int result = status;

    s_end_flash = NULL;

    if (atomic_load(&s_unlocked_shared)) {
        return END_UNLOCKED_SHARED;
    }

    if (!read_whole(flash, s_before, DEVICE_SIZE)) {
        return END_NOT_READ;
    }

    const uint_fast32_t written = atomic_load(&s_written);

    while ((atomic_load(&s_written) < (written + WRITES_AFTER_THE_RELEASE)) &&
           (monotonic_ns() < deadline)) {
        const struct timespec nap = {0, 1000000L};

        (void)nanosleep(&nap, NULL);
    }

    if ((atomic_load(&s_written) < (written + WRITES_AFTER_THE_RELEASE)) ||
        atomic_load(&s_writer_failed)) {
        result = END_NO_MORE_WRITES;
    } else if (!read_whole(flash, s_file, DEVICE_SIZE)) {
        result = END_NOT_READ;
    } else if (memcmp(s_before, s_file, DEVICE_SIZE) != 0) {
        result = END_FILE_CHANGED;
    } else {
        /* nothing reached the file: the end's own status */
    }

    return result;
}

/* Every _exit() of this program and of the platform comes here (-Wl,--wrap=_exit; the names
 * are the linker's). Only the child of end-while-writing holds its end open. */
_Noreturn void __real__exit(int status);
_Noreturn void __wrap__exit(int status);

void __wrap__exit(int status)
{
    __real__exit((s_end_flash != NULL) ? held_end(status) : status);
}

/* The child: the cell and the flash as the node opens them (the part's own times, the node's
 * wait), a keeper of the two mappings forked before any thread exists, and the writer. The run
 * is ended from outside the scheduler and does not come back from posix_platform_run(). */
static int child_end_while_writing(const cut_plan_t *plan)
{
    static cut_plan_t s_plan; /* the task's argument outlives this call */
    char error[512];

    if (!posix_retained_open(plan->cell, error, sizeof(error)) ||
        !posix_flash_open(plan->flash, NULL, error, sizeof(error))) {
        (void)fprintf(stderr, "%s\n", error);
        posix_retained_close(); /* open if it was the flash that failed */
        return END_NOT_FROM_OUTSIDE;
    }

    if (map_keeper_fork(plan->keeper) < 0) {
        (void)fprintf(stderr, "cannot fork the keeper of the mappings\n");
        posix_flash_close();
        posix_retained_close();
        return END_NOT_FROM_OUTSIDE;
    }

    s_plan = *plan;
    posix_flash_set_wait(wait_one_tick);
    init_platform(NULL);
    s_end_flash = plan->flash;
    create_task(writer_task, "writer", 2U, &s_plan);
    posix_platform_run();
    s_end_flash = NULL;
    posix_platform_shutdown();
    posix_flash_close();
    posix_retained_close();
    return END_NOT_FROM_OUTSIDE;
}

/* What a status of the held end says, for the parent's message. */
static const char *held_end_text(int status)
{
    const char *text = "";

    switch (status) {
    case END_FILE_CHANGED:
        text = ": a program the task made after the end had released the lock is in the file";
        break;
    case END_NO_MORE_WRITES:
        text = ": the task programmed nothing more while the end was held open, or a program "
               "failed";
        break;
    case END_NOT_READ:
        text = ": the held end could not read the flash file";
        break;
    case END_NOT_FROM_OUTSIDE:
        text = ": the run did not start, or did not end from outside the scheduler";
        break;
    case END_UNLOCKED_SHARED:
        text = ": the end removed the flash file's lock while its mapping of the file was still "
               "the shared one";
        break;
    default:
        break;
    }

    return text;
}

static int cmd_end_while_writing(const char *end, const char *flash, const char *cell)
{
    const bool by_guard = (strcmp(end, "guard") == 0);
    const int expected = by_guard ? POSIX_EXIT_SHUTDOWN_TIMEOUT : (int)PLAT_RESET_WATCHDOG;
    const char *const fatal =
        by_guard ? "FATAL the scheduler did not stop in time" : "FATAL watchdog: not kicked for";
    const uint32_t base = plat_flash_layout()->slot[1].offset;
    map_keeper_t keeper;
    const cut_plan_t plan = {flash, cell, NULL, {0U, 0U}, &keeper, by_guard};
    char error[256];
    posix_retained_info_t retained;
    bool ok = true;

    if (!by_guard && (strcmp(end, "watchdog") != 0)) {
        return 2;
    }

    (void)memset(s_file, 0xFF, sizeof(s_file));
    (void)unlink(cell);

    if (!write_whole(flash, s_file, DEVICE_SIZE)) {
        (void)fprintf(stderr, "cannot write %s\n", flash);
        return EXIT_FAILURE;
    }

    FILE *errors = tmpfile();

    if (errors == NULL) {
        return EXIT_FAILURE;
    }

    if (!map_keeper_prepare(&keeper)) {
        (void)fclose(errors);
        return EXIT_FAILURE;
    }

    const int status = run_child(child_end_while_writing, &plan, errors);

    if (!child_said(errors, fatal)) {
        (void)fprintf(stderr, "stderr lacks the line \"%s\"\n", fatal);
        ok = false;
    }

    (void)fclose(errors);

    if (status != expected) {
        (void)fprintf(stderr, "the child exited with %d, expected %d%s\n", status, expected,
                      held_end_text(status));
        ok = false;
    }

    /* What the task programmed before the end was on its way is in the file. */
    if (!read_whole(flash, s_file, DEVICE_SIZE)) {
        (void)fprintf(stderr, "cannot read %s\n", flash);
        ok = false;
    } else {
        for (uint32_t n = 0U; n < WRITES_BEFORE_THE_END; n++) {
            uint8_t unit[8];

            unit_of(n, unit);

            if (memcmp(&s_file[base + (n * 8U)], unit, sizeof(unit)) != 0) {
                (void)fprintf(stderr, "unit %u of slot B is not what the task programmed\n",
                              (unsigned int)n);
                ok = false;
            }
        }
    }

    /* The next start, at once: the keeper still has the child's mappings of both files, so the
     * end released both locks itself. A watchdog reset is in the cell; the guard records
     * nothing, and the cell is as the child's start left it. */
    if (!posix_retained_open(cell, error, sizeof(error))) {
        (void)fprintf(stderr, "%s\n", error);
        ok = false;
    } else {
        const plat_reset_reason_t cause = by_guard ? PLAT_RESET_POWER_ON : PLAT_RESET_WATCHDOG;
        const char *const found = by_guard ? "no reset" : "reset";

        posix_retained_get_info(&retained);
        posix_retained_close();

        if ((retained.cause != cause) || (strcmp(retained.verdict, found) != 0)) {
            (void)fprintf(stderr, "the retained cell after the end: cause %s, %s\n",
                          posix_reset_reason_name(retained.cause), retained.verdict);
            ok = false;
        }
    }

    if (!posix_flash_open(flash, NULL, error, sizeof(error))) {
        (void)fprintf(stderr, "%s\n", error);
        ok = false;
    } else {
        posix_flash_close();
    }

    if (!map_keeper_end(&keeper)) {
        (void)fprintf(stderr, "the keeper of the child's mappings did not end\n");
        ok = false;
    }

    (void)printf("{\"test\": \"end-while-writing\", \"end\": \"%s\", \"status\": %d, \"ok\": %s}\n",
                 end, status, ok ? "true" : "false");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int usage(void)
{
    (void)fprintf(stderr, "usage: flash_selftest waits <path>\n"
                          "       flash_selftest task-wait <path>\n"
                          "       flash_selftest task-cut <flash> <cell> <N[:SEED]>\n"
                          "       flash_selftest end-while-writing <watchdog|guard> <flash> "
                          "<cell>\n");
    return 2;
}

int main(int argc, char **argv)
{
    int status = 2;

    if ((argc == 3) && (strcmp(argv[1], "waits") == 0)) {
        status = cmd_waits(argv[2]);
    } else if ((argc == 3) && (strcmp(argv[1], "task-wait") == 0)) {
        status = cmd_task_wait(argv[2]);
    } else if ((argc == 5) && (strcmp(argv[1], "task-cut") == 0)) {
        status = cmd_task_cut(argv[2], argv[3], argv[4]);
    } else if ((argc == 5) && (strcmp(argv[1], "end-while-writing") == 0)) {
        status = cmd_end_while_writing(argv[2], argv[3], argv[4]);

        if (status == 2) {
            status = usage();
        }
    } else {
        status = usage();
    }

    return status;
}
