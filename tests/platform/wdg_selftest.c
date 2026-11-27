/* SPDX-License-Identifier: MIT */
/*
 * Self-tests of the hardware watchdog model (plat_wdg.h, posix_wdg.c), the retained-RAM cell
 * across real resets of a FreeRTOS program (plat_retained.h, posix_retained.c, plat_reset()),
 * and the hang of the sensor front end under the scheduler (posix_sensor.c, "sensor.hang").
 * CTest runs them through run_selftest.sh (tests/platform/CMakeLists.txt).
 *
 * A reset ends the process, so a test that wants to see one plays the bench: the process
 * forks, the child runs the FreeRTOS program, and the parent, which never starts the platform,
 * watches how and when the child ends and what it reported on a pipe. Every report is written
 * with write(), which takes no lock, from a task.
 *
 *   expire <timeout_ms> [<path>]    a watchdog kicked six times and then no more resets the
 *                                   child with status 11 (PLAT_RESET_WATCHDOG), never earlier
 *                                   than the timeout after the last kick, neither as this
 *                                   process sees it nor by the count the model reports in its
 *                                   fatal line (either is a failure), and at the
 *                                   latest EXPIRE_LATE_MS after it (else status 75: the host did
 *                                   not run the model's thread in time); with <path>, the child
 *                                   runs with that retained cell, which then records the reset
 *                                   (tests/system/test_retained.py starts the node on it)
 *   kicked <timeout_ms> <seconds>   a watchdog kicked every quarter of its timeout keeps the
 *                                   child alive for the whole run, which ends orderly (status 0)
 *                                   and after which the model stops; a reset is status 75 only
 *                                   if the host held the kicking task back for a whole timeout,
 *                                   which the reported kick times show, and a failure otherwise
 *   start-rules                     plat_wdg_start() refuses a timeout out of range and a second
 *                                   start; a kick before the start does nothing
 *   kick-cost <kicks>               the time of one plat_wdg_kick() on this host, as JSON
 *   retained <path>                 starts and resets of the child in a row, the bench in
 *                                   between: the cause each start sees through
 *                                   plat_reset_cause(), the reset count, the verdict on the
 *                                   cell; a power-on from a missing, a damaged, a lost cell
 *   watchdog-cause <timeout_ms> <path>
 *                                   a watchdog reset is recorded in the cell and seen by the
 *                                   next start
 *                                   In both, every child leaves a keeper of its mapping of the
 *                                   cell behind (THE KEEPERS below): the next start, which
 *                                   follows at once, opens the cell only if the run before it
 *                                   released the cell's lock itself, at its reset (from a task,
 *                                   and from the watchdog's thread) or at its orderly end
 *   hang <on|spin>                  sensor.hang under the scheduler: with "on" the reading task
 *                                   is blocked and a task below it runs on; with "spin" that
 *                                   task does not run at all until the hang ends
 *
 * Exit status: 0 passed, 1 failed, 2 usage, 75 (EX_TEMPFAIL) a timing tolerance was missed
 * although the mechanism itself worked.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"

#include "map_keeper.h"
#include "plat_retained.h"
#include "plat_sensor.h"
#include "plat_wdg.h"
#include "posix_platform.h"
#include "posix_retained.h"
#include "posix_sensor.h"
#include "posix_sim.h"

#define TIMING_MISSED  75
#define TASK_STACK     configMINIMAL_STACK_SIZE
#define NS_PER_MS      1000000ULL
#define EXPIRE_KICKS   6U
#define EXPIRE_LATE_MS 50U /* the reset may come this much after the timeout */

/* ------------------------------------------------------------------- parent and child */

/* What a child reports on the pipe. */
typedef enum {
    REPORT_KICK = 1, /* value: CLOCK_MONOTONIC just before a kick (or the start) */
    REPORT_OK,       /* the child's own checks passed */
    REPORT_FAIL      /* they did not; the child said why on stderr */
} report_kind_t;

typedef struct {
    uint64_t kind;
    uint64_t value;
} report_t;

static int s_report_fd = -1; /* the child's end of the pipe */

static void report(report_kind_t kind, uint64_t value)
{
    const report_t r = {(uint64_t)kind, value};

    (void)!write(s_report_fd, &r, sizeof(r));
}

typedef int (*child_fn)(void *arg);

typedef struct {
    int status;           /* exit status, or -1 if killed or ended by a signal */
    uint64_t ended_ns;    /* CLOCK_MONOTONIC once the parent saw it end */
    uint32_t kicks;       /* REPORT_KICK received */
    uint64_t kick_ns[2];  /* the last two kick times, [1] the latest */
    uint64_t longest_gap; /* the longest time between two consecutive kicks */
    uint32_t oks;
    uint32_t fails;
    long counted_ms; /* the model's count in its "FATAL watchdog" line on stderr, or -1 */
} child_result_t;

/* The child's stderr went to `file`: copy it to this process's stderr, where run_selftest.sh
 * looks at it, and take the count of the watchdog model's fatal line out of it. */
static long copy_child_stderr(FILE *file)
{
    static const char prefix[] = "FATAL watchdog: not kicked for ";
    char line[512];
    long counted_ms = -1L;

    if (fseek(file, 0L, SEEK_SET) != 0) {
        (void)fprintf(stderr, "cannot read back the child's stderr\n");
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        (void)fputs(line, stderr);

        if (strncmp(line, prefix, sizeof(prefix) - 1U) == 0) {
            counted_ms = strtol(&line[sizeof(prefix) - 1U], NULL, 10);
        }
    }

    return counted_ms;
}

/* Run `fn` in a child process and wait at most `limit_ms` for it to end. */
static child_result_t run_child(child_fn fn, void *arg, uint32_t limit_ms)
{
    child_result_t result;
    int fds[2];
    pid_t pid;
    FILE *errors = tmpfile(); /* the child's stderr: a file, which never blocks the child */

    (void)memset(&result, 0, sizeof(result));
    result.status = -1;
    result.counted_ms = -1L;

    if ((errors == NULL) || (pipe(fds) != 0)) {
        if (errors != NULL) {
            (void)fclose(errors);
        }

        return result;
    }

    (void)fflush(NULL);
    pid = fork();

    if (pid == 0) {
        (void)close(fds[0]);
        (void)dup2(fileno(errors), STDERR_FILENO);
        s_report_fd = fds[1];
        (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
        exit(fn(arg));
    }

    (void)close(fds[1]);

    if (pid > 0) {
        const uint64_t deadline = posix_monotonic_ns() + ((uint64_t)limit_ms * NS_PER_MS);
        int wstatus = 0;
        pid_t done = 0;

        while (done == 0) {
            const struct timespec nap = {0, 200000L};

            done = waitpid(pid, &wstatus, WNOHANG);

            if ((done == 0) && (posix_monotonic_ns() > deadline)) {
                (void)fprintf(stderr, "the child did not end within %u ms: killed\n",
                              (unsigned int)limit_ms);
                (void)kill(pid, SIGKILL);
                (void)waitpid(pid, &wstatus, 0);
                done = -1;
            } else if (done == 0) {
                (void)nanosleep(&nap, NULL);
            } else {
                /* ended */
            }
        }

        result.ended_ns = posix_monotonic_ns();

        if ((done == pid) && WIFEXITED(wstatus)) {
            result.status = WEXITSTATUS(wstatus);
        }

        report_t r;

        while (read(fds[0], &r, sizeof(r)) == (ssize_t)sizeof(r)) {
            if (r.kind == (uint64_t)REPORT_KICK) {
                if ((result.kicks > 0U) && ((r.value - result.kick_ns[1]) > result.longest_gap)) {
                    result.longest_gap = r.value - result.kick_ns[1];
                }

                result.kick_ns[0] = result.kick_ns[1];
                result.kick_ns[1] = r.value;
                result.kicks++;
            } else if (r.kind == (uint64_t)REPORT_OK) {
                result.oks++;
            } else {
                result.fails++;
            }
        }

        result.counted_ms = copy_child_stderr(errors);
    }

    (void)close(fds[0]);
    (void)fclose(errors);
    return result;
}

static void init_platform(void)
{
    const posix_config_t config = {.tick_mode = POSIX_TICK_LOCKED,
                                   .irq_mode = POSIX_IRQ_SIGNAL,
                                   .log_level = LOG_LEVEL_INFO,
                                   .log_path = NULL};

    posix_platform_init(&config);
}

static void create_task(TaskFunction_t code, const char *name, UBaseType_t priority, void *arg)
{
    const BaseType_t created = xTaskCreate(code, name, TASK_STACK, arg, priority, NULL);

    configASSERT(created == pdPASS);
}

static void run_scheduler(void)
{
    posix_platform_run();
    posix_platform_shutdown();
}

static void idle_forever(void)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* ------------------------------------------------------------------------- the watchdog */

typedef struct {
    uint32_t timeout_ms;
    uint32_t kicks;       /* kicks after the start; then stop kicking */
    uint32_t seconds;     /* kicked: how long to keep kicking, then end orderly */
    const char *retained; /* expire: the retained cell to record the reset in, or NULL */
} kick_plan_t;

/* Starts the watchdog and kicks every quarter timeout, `kicks` times (expire) or for
 * `seconds` (kicked). Each time is reported just before the call, so that the parent's
 * "never early" check uses a time no later than the model's own time stamp. */
static void kicker_task(void *arg)
{
    const kick_plan_t *plan = arg;
    const TickType_t period = pdMS_TO_TICKS(plan->timeout_ms / 4U);
    TickType_t wake = xTaskGetTickCount();

    report(REPORT_KICK, posix_monotonic_ns());

    if (!plat_wdg_start(plan->timeout_ms)) {
        (void)fprintf(stderr, "plat_wdg_start(%u) refused\n", (unsigned int)plan->timeout_ms);
        report(REPORT_FAIL, 0U);
        plat_end_scheduler();
        idle_forever();
    }

    if (plan->seconds == 0U) {
        for (uint32_t i = 0U; i < plan->kicks; i++) {
            (void)xTaskDelayUntil(&wake, period);
            report(REPORT_KICK, posix_monotonic_ns());
            plat_wdg_kick();
        }

        idle_forever(); /* no more kicks: the watchdog has to reset the child */
    }

    const uint64_t end = posix_monotonic_ns() + ((uint64_t)plan->seconds * 1000ULL * NS_PER_MS);

    while (posix_monotonic_ns() < end) {
        (void)xTaskDelayUntil(&wake, period);
        report(REPORT_KICK, posix_monotonic_ns());
        plat_wdg_kick();
    }

    report(REPORT_OK, 0U);
    plat_end_scheduler();
    idle_forever();
}

static int child_kicker(void *arg)
{
    const kick_plan_t *plan = arg;
    posix_wdg_stats_t stats;
    char error[512];

    if (!posix_retained_open(plan->retained, error, sizeof(error))) {
        (void)fprintf(stderr, "%s\n", error);
        return 3;
    }

    init_platform();
    create_task(kicker_task, "kicker", 3U, arg);
    run_scheduler();
    posix_wdg_get_stats(&stats);
    (void)printf("{\"child\": \"kicker\", \"started\": %s, \"timeout_ms\": %u, \"kicks\": %llu, "
                 "\"longest_unkicked_ms\": %.3f, \"gaps\": %llu}\n",
                 stats.started ? "true" : "false", (unsigned int)stats.timeout_ms,
                 (unsigned long long)stats.kicks, (double)stats.longest_unkicked_ns / 1e6,
                 (unsigned long long)stats.gaps);
    posix_retained_close();
    return EXIT_SUCCESS;
}

static int cmd_expire(uint32_t timeout_ms, const char *retained)
{
    kick_plan_t plan = {
        .timeout_ms = timeout_ms, .kicks = EXPIRE_KICKS, .seconds = 0U, .retained = retained};
    const child_result_t r = run_child(child_kicker, &plan, 10U * timeout_ms + 5000U);
    const uint64_t timeout_ns = (uint64_t)timeout_ms * NS_PER_MS;
    const uint64_t after_ns = r.ended_ns - r.kick_ns[1];
    int status = EXIT_SUCCESS;

    (void)printf("{\"test\": \"wdg-expire\", \"timeout_ms\": %u, \"status\": %d, \"kicks\": %u, "
                 "\"reset_after_last_kick_ms\": %.3f, \"model_counted_ms\": %ld, "
                 "\"longest_kick_gap_ms\": %.3f}\n",
                 (unsigned int)timeout_ms, r.status, (unsigned int)r.kicks, (double)after_ns / 1e6,
                 r.counted_ms, (double)r.longest_gap / 1e6);

    if (r.status != (int)PLAT_RESET_WATCHDOG) {
        (void)fprintf(stderr, "the child ended with %d, not %d\n", r.status,
                      (int)PLAT_RESET_WATCHDOG);
        status = EXIT_FAILURE;
    } else if (r.kicks != (EXPIRE_KICKS + 1U)) {
        /* Reset while it was still kicking: excused only if the host held the kicker back for
         * a whole timeout between two kicks. */
        status = (r.longest_gap >= timeout_ns) ? TIMING_MISSED : EXIT_FAILURE;
        (void)fprintf(stderr, "reset after %u of %u kicks (longest gap %.3f ms)\n",
                      (unsigned int)r.kicks, EXPIRE_KICKS + 1U, (double)r.longest_gap / 1e6);
    } else if (after_ns < timeout_ns) {
        (void)fprintf(stderr, "reset %.3f ms after the last kick, before the timeout\n",
                      (double)after_ns / 1e6);
        status = EXIT_FAILURE;
    } else if (r.counted_ms < (long)timeout_ms) {
        /* The model's own count when it decided: a reset a millisecond early is lost in the
         * latency of this outside view, but not in the count, which must have reached the
         * timeout. */
        (void)fprintf(stderr, "the model reset after counting %ld ms, short of %u ms\n",
                      r.counted_ms, (unsigned int)timeout_ms);
        status = EXIT_FAILURE;
    } else if (after_ns > (timeout_ns + ((uint64_t)EXPIRE_LATE_MS * NS_PER_MS))) {
        (void)fprintf(stderr, "reset %.3f ms after the last kick: later than %u + %u ms\n",
                      (double)after_ns / 1e6, (unsigned int)timeout_ms,
                      (unsigned int)EXPIRE_LATE_MS);
        status = TIMING_MISSED;
    } else {
        /* in time */
    }

    return status;
}

static int cmd_kicked(uint32_t timeout_ms, uint32_t seconds)
{
    kick_plan_t plan = {
        .timeout_ms = timeout_ms, .kicks = 0U, .seconds = seconds, .retained = NULL};
    const child_result_t r = run_child(child_kicker, &plan, (seconds * 1000U) + 10000U);
    const uint64_t timeout_ns = (uint64_t)timeout_ms * NS_PER_MS;
    int status = EXIT_SUCCESS;

    (void)printf("{\"test\": \"wdg-kicked\", \"timeout_ms\": %u, \"seconds\": %u, \"status\": %d, "
                 "\"kicks\": %u, \"longest_kick_gap_ms\": %.3f}\n",
                 (unsigned int)timeout_ms, (unsigned int)seconds, r.status, (unsigned int)r.kicks,
                 (double)r.longest_gap / 1e6);

    if ((r.status == EXIT_SUCCESS) && (r.oks == 1U) && (r.fails == 0U)) {
        /* alive throughout, and the model let the orderly end through */
    } else if (r.status == (int)PLAT_RESET_WATCHDOG) {
        /* The kicker was late by a whole timeout, between two kicks or after the last one, or
         * its last kick came just at the deadline and raced the expiry (posix_wdg.c): the host
         * did not run it. A reset without such a gap is a defect. */
        const uint64_t last_gap = r.ended_ns - r.kick_ns[1];
        const uint64_t before_last = r.kick_ns[1] - r.kick_ns[0];
        const bool held_back = (last_gap >= timeout_ns) ||
                               ((r.kicks >= 2U) && (before_last + NS_PER_MS >= timeout_ns)) ||
                               (r.longest_gap >= timeout_ns);

        (void)fprintf(stderr, "reset while kicked (last kick %.3f ms before the end)\n",
                      (double)last_gap / 1e6);
        status = held_back ? TIMING_MISSED : EXIT_FAILURE;
    } else {
        (void)fprintf(stderr, "the child ended with %d\n", r.status);
        status = EXIT_FAILURE;
    }

    return status;
}

/* In one task: the rules of plat_wdg_start() and plat_wdg_kick(), read back through the
 * model's statistics. */
static atomic_int s_rules_status = EXIT_FAILURE;

static bool check(bool ok, const char *what)
{
    if (!ok) {
        (void)fprintf(stderr, "start-rules: %s\n", what);
    }

    return ok;
}

static void rules_task(void *arg)
{
    posix_wdg_stats_t stats;
    bool ok = true;

    (void)arg;
    plat_wdg_kick(); /* before the start: nothing */
    posix_wdg_get_stats(&stats);
    ok = check(!stats.started && (stats.kicks == 0U), "a kick before the start counted") && ok;
    ok = check(!plat_wdg_start(0U), "a timeout of 0 was accepted") && ok;
    ok = check(!plat_wdg_start(PLAT_WDG_TIMEOUT_MAX_MS + 1U), "a timeout too long was accepted") &&
         ok;
    posix_wdg_get_stats(&stats);
    ok = check(!stats.started, "a refused start started the watchdog") && ok;
    ok = check(plat_wdg_start(PLAT_WDG_TIMEOUT_MAX_MS), "the longest timeout was refused") && ok;
    ok = check(!plat_wdg_start(PLAT_WDG_TIMEOUT_MIN_MS), "a second start was accepted") && ok;
    plat_wdg_kick();
    plat_wdg_kick();
    posix_wdg_get_stats(&stats);
    ok = check(stats.started && (stats.timeout_ms == PLAT_WDG_TIMEOUT_MAX_MS),
               "the second start changed the timeout") &&
         ok;
    ok = check(stats.kicks == 2U, "the kicks were not counted") && ok;
    atomic_store(&s_rules_status, ok ? EXIT_SUCCESS : EXIT_FAILURE);
    plat_end_scheduler();
    idle_forever();
}

static int cmd_start_rules(void)
{
    init_platform();
    create_task(rules_task, "rules", 3U, NULL);
    run_scheduler();
    return atomic_load(&s_rules_status);
}

/* The cost of a kick, measured over `kicks` calls in a task. */
static uint32_t s_cost_kicks;
static uint64_t s_cost_total_ns;
static uint64_t s_cost_max_ns;

static void cost_task(void *arg)
{
    (void)arg;
    (void)plat_wdg_start(PLAT_WDG_TIMEOUT_MAX_MS);

    for (uint32_t i = 0U; i < s_cost_kicks; i++) {
        const uint64_t before = posix_monotonic_ns();

        plat_wdg_kick();

        const uint64_t took = posix_monotonic_ns() - before;

        s_cost_total_ns += took;

        if (took > s_cost_max_ns) {
            s_cost_max_ns = took;
        }
    }

    plat_end_scheduler();
    idle_forever();
}

static int cmd_kick_cost(uint32_t kicks)
{
    s_cost_kicks = kicks;
    init_platform();
    create_task(cost_task, "cost", 3U, NULL);
    run_scheduler();
    /* Each figure includes the two clock reads around the kick. */
    (void)printf("{\"test\": \"wdg-kick-cost\", \"kicks\": %u, \"mean_ns\": %.1f, "
                 "\"max_ns\": %llu}\n",
                 (unsigned int)kicks, (double)s_cost_total_ns / (double)kicks,
                 (unsigned long long)s_cost_max_ns);
    return (kicks > 0U) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* ------------------------------------------------------------- the cell across resets */

/* THE KEEPERS. While this is set, every child that opens the cell forks a keeper of its
 * mapping (map_keeper.h) before it starts the platform. The mapping, and with it the cell's
 * open file description, then outlives the child, as it does when another process holds the
 * child's memory map at the moment of its end (docs/bug-log.md, 012), and the next step's
 * child opens the cell at once. A run that left the cell's lock to its exit would make that
 * child end with 3 ("in use by another node"). */
static const map_keeper_t *s_keeper;

typedef struct {
    const char *path;
    plat_reset_reason_t expect_cause;
    const char *expect_verdict;
    uint32_t expect_resets;
    plat_reset_reason_t then; /* reset with this, or PLAT_RESET_POWER_ON: end orderly */
    uint32_t watchdog_ms;     /* not 0: instead of `then`, start the watchdog and stop kicking */
} start_step_t;

/* What the application of this start sees, and then the end of the run. */
static void start_task(void *arg)
{
    const start_step_t *step = arg;
    posix_retained_info_t info;

    posix_retained_get_info(&info);

    if ((plat_reset_cause() == step->expect_cause) &&
        (strcmp(info.verdict, step->expect_verdict) == 0) && (info.resets == step->expect_resets)) {
        report(REPORT_OK, 0U);
    } else {
        (void)fprintf(stderr, "start: cause %s, verdict %s, %u resets; expected %s, %s, %u\n",
                      posix_reset_reason_name(plat_reset_cause()), info.verdict,
                      (unsigned int)info.resets, posix_reset_reason_name(step->expect_cause),
                      step->expect_verdict, (unsigned int)step->expect_resets);
        report(REPORT_FAIL, 0U);
    }

    if (step->watchdog_ms != 0U) {
        (void)plat_wdg_start(step->watchdog_ms);
        idle_forever();
    }

    if (step->then == PLAT_RESET_POWER_ON) {
        plat_end_scheduler();
        idle_forever();
    }

    /* A reason that is not one (POWER_ON is handled above) is passed through a variable the
     * step names, as a programming error would be. */
    plat_reset(step->then);
}

/* The bad reason of one step: plat_reset() with a value that is no reset reason. */
#define BAD_REASON ((plat_reset_reason_t)99)

static int child_start(void *arg)
{
    const start_step_t *step = arg;
    char error[512];

    if (!posix_retained_open(step->path, error, sizeof(error))) {
        (void)fprintf(stderr, "%s\n", error);
        return 3;
    }

    if ((s_keeper != NULL) && (map_keeper_fork(s_keeper) < 0)) {
        (void)fprintf(stderr, "cannot fork the keeper of the cell's mapping\n");
        posix_retained_close();
        return 3;
    }

    init_platform();
    create_task(start_task, "start", 3U, arg);
    run_scheduler();
    posix_retained_close();
    return EXIT_SUCCESS;
}

static bool run_step(start_step_t *step, int expect_status, const char *what)
{
    const child_result_t r = run_child(child_start, step, 10000U);
    const bool ok = (r.status == expect_status) && (r.oks == 1U) && (r.fails == 0U);

    (void)printf("{\"step\": \"%s\", \"status\": %d, \"expected\": %d, \"checks\": \"%s\"}\n", what,
                 r.status, expect_status, ((r.oks == 1U) && (r.fails == 0U)) ? "ok" : "failed");
    return ok;
}

static bool write_file(const char *path, const void *data, size_t length)
{
    FILE *file = fopen(path, "wb");
    bool ok = (file != NULL);

    if (ok) {
        ok = (fwrite(data, 1U, length, file) == length);
        ok = (fclose(file) == 0) && ok;
    }

    return ok;
}

static bool read_cell(const char *path, plat_retained_cell_t *cell)
{
    FILE *file = fopen(path, "rb");
    bool ok = (file != NULL);

    if (ok) {
        ok = (fread(cell, sizeof(*cell), 1U, file) == 1U);
        ok = (fclose(file) == 0) && ok;
    }

    return ok;
}

static int cmd_retained(const char *path)
{
    plat_retained_cell_t cell = {0};
    map_keeper_t keeper;
    bool ok = true;

    (void)unlink(path);

    if (!map_keeper_prepare(&keeper)) {
        return EXIT_FAILURE;
    }

    s_keeper = &keeper;

    /* A missing cell: power-on; then a requested reset. */
    {
        start_step_t s = {path, PLAT_RESET_POWER_ON, "missing", 0U, PLAT_RESET_REQUESTED, 0U};
        ok = run_step(&s, (int)PLAT_RESET_REQUESTED, "missing cell, then REQUESTED") && ok;
    }
    ok = read_cell(path, &cell) && (cell.reset_reason == (uint32_t)PLAT_RESET_REQUESTED) &&
         (cell.resets == 1U) && (cell.crc == posix_retained_crc(&cell)) && ok;

    /* The next start sees it; then an assertion (a reason that is none resets as one). */
    {
        /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): damaged on purpose */
        start_step_t s = {path, PLAT_RESET_REQUESTED, "reset", 1U, BAD_REASON, 0U};
        ok = run_step(&s, (int)PLAT_RESET_ASSERT, "REQUESTED seen, then a bad reason") && ok;
    }

    /* ASSERT seen, two resets; then another reason. */
    {
        start_step_t s = {path, PLAT_RESET_ASSERT, "reset", 2U, PLAT_RESET_STACK_OVERFLOW, 0U};
        ok = run_step(&s, (int)PLAT_RESET_STACK_OVERFLOW, "ASSERT seen, then STACK_OVERFLOW") && ok;
    }

    /* Then a power loss: the cell goes with it. */
    {
        start_step_t s = {path, PLAT_RESET_STACK_OVERFLOW, "reset", 3U, PLAT_RESET_POWER_LOSS, 0U};
        ok = run_step(&s, (int)PLAT_RESET_POWER_LOSS, "STACK_OVERFLOW seen, then POWER_LOSS") && ok;
    }

    /* After the power loss: a power-on; then an orderly end. */
    {
        start_step_t s = {path, PLAT_RESET_POWER_ON, "magic", 0U, PLAT_RESET_POWER_ON, 0U};
        ok = run_step(&s, EXIT_SUCCESS, "power-on after POWER_LOSS, orderly end") && ok;
    }

    /* After an orderly end: a power-on again, no reset recorded; then a reset. */
    {
        start_step_t s = {path, PLAT_RESET_POWER_ON, "no reset", 0U, PLAT_RESET_MALLOC_FAILED, 0U};
        ok = run_step(&s, (int)PLAT_RESET_MALLOC_FAILED, "power-on after an orderly end") && ok;
    }

    /* The bench damages the cell: one bit of the reason. */
    ok = read_cell(path, &cell) && ok;
    cell.reset_reason ^= 0x100U;
    ok = write_file(path, &cell, sizeof(cell)) && ok;
    {
        start_step_t s = {path, PLAT_RESET_POWER_ON, "crc", 0U, PLAT_RESET_IMAGE_FAULT, 0U};
        ok = run_step(&s, (int)PLAT_RESET_IMAGE_FAULT, "power-on from a damaged cell") && ok;
    }

    /* The bench removes the file: a power-on. */
    ok = (unlink(path) == 0) && ok;
    {
        start_step_t s = {path, PLAT_RESET_POWER_ON, "missing", 0U, PLAT_RESET_POWER_ON, 0U};
        ok = run_step(&s, EXIT_SUCCESS, "power-on from a removed file") && ok;
    }

    s_keeper = NULL;
    ok = map_keeper_end(&keeper) && ok;
    (void)unlink(path);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* A watchdog reset (from the model's thread, outside the scheduler) in the cell, seen by the
 * next start. */
static int cmd_watchdog_cause(uint32_t timeout_ms, const char *path)
{
    bool ok = true;
    plat_retained_cell_t cell = {0};
    map_keeper_t keeper;

    (void)unlink(path);

    if (!map_keeper_prepare(&keeper)) {
        return EXIT_FAILURE;
    }

    s_keeper = &keeper;
    {
        start_step_t s = {path, PLAT_RESET_POWER_ON, "missing",
                          0U,   PLAT_RESET_POWER_ON, timeout_ms};
        ok = run_step(&s, (int)PLAT_RESET_WATCHDOG, "watchdog not kicked") && ok;
    }
    ok = read_cell(path, &cell) && (cell.reset_reason == (uint32_t)PLAT_RESET_WATCHDOG) &&
         (cell.resets == 1U) && (cell.crc == posix_retained_crc(&cell)) && ok;
    {
        start_step_t s = {path, PLAT_RESET_WATCHDOG, "reset", 1U, PLAT_RESET_POWER_ON, 0U};
        ok = run_step(&s, EXIT_SUCCESS, "the next start sees WATCHDOG") && ok;
    }
    /* That run ended without a reset: the reason was spent, the next start is a power-on. */
    {
        start_step_t s = {path, PLAT_RESET_POWER_ON, "no reset", 0U, PLAT_RESET_POWER_ON, 0U};
        ok = run_step(&s, EXIT_SUCCESS, "then a power-on") && ok;
    }
    s_keeper = NULL;
    ok = map_keeper_end(&keeper) && ok;
    (void)unlink(path);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* --------------------------------------------------------------------------- the hang */

/*
 * Three tasks and the bench. "reader" (priority 5) reads the current in a loop, as the sensor
 * task does, counting its reads; "low" (2) counts its passes, one per tick; "stopper" (6) ends
 * the run when the bench is done. The bench is a thread outside the scheduler that serves
 * simulation control itself and talks to it through a client socket. With the hang "on" the
 * reader is blocked in vTaskDelay(1) inside its read (the node's wait function), so "low" keeps
 * running; with "spin" the reader never leaves the CPU, so "low" does not run at all.
 */
static atomic_ulong s_reads;
static atomic_ulong s_low_passes;
static atomic_bool s_bench_done;
static atomic_int s_hang_status = EXIT_FAILURE;
static int s_client = -1;
static uint16_t s_sim_port;

static void wait_one_tick(void)
{
    vTaskDelay(1U);
}

static void reader_task(void *arg)
{
    (void)arg;

    for (;;) {
        int32_t current = 0;
        bool invalid = false;

        (void)plat_sensor_read_current(&current, &invalid);
        (void)atomic_fetch_add(&s_reads, 1UL);
        vTaskDelay(1U);
    }
}

static void low_task(void *arg)
{
    (void)arg;

    for (;;) {
        (void)atomic_fetch_add(&s_low_passes, 1UL);
        vTaskDelay(1U);
    }
}

static void stopper_task(void *arg)
{
    (void)arg;

    while (!atomic_load(&s_bench_done)) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }

    plat_end_scheduler();
    idle_forever();
}

/* One request, served by this thread, and its reply ("<no reply>" if none came). */
static const char *exchange(const char *request)
{
    struct sockaddr_in to;
    struct pollfd fd = {.fd = s_client, .events = POLLIN, .revents = 0};

    (void)memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(s_sim_port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)sendto(s_client, request, strlen(request), 0, (const struct sockaddr *)&to, sizeof(to));
    posix_sim_service();

    if (poll(&fd, 1U, 200) > 0) {
        static char reply[2U * POSIX_SIM_MAX_DATAGRAM];
        const ssize_t n = recv(s_client, reply, sizeof(reply) - 1U, 0);

        if (n >= 0) {
            reply[n] = '\0';
            return reply;
        }
    }

    return "<no reply>";
}

static void sleep_ms(uint32_t ms)
{
    const struct timespec pause = {(time_t)(ms / 1000U), (long)(ms % 1000U) * 1000000L};

    (void)nanosleep(&pause, NULL);
}

/* Wait up to 5 s for `counter` to pass `above`. */
static bool wait_for(atomic_ulong *counter, unsigned long above)
{
    for (uint32_t i = 0U; (i < 5000U) && (atomic_load(counter) <= above); i++) {
        sleep_ms(1U);
    }

    return atomic_load(counter) > above;
}

typedef struct {
    bool spin;
} hang_plan_t;

static void *bench_main(void *arg)
{
    const hang_plan_t *plan = arg;
    char request[64];
    bool ok = true;

    posix_external_thread_setup("bench");
    ok = check(wait_for(&s_reads, 5UL), "the reader does not read") && ok;
    ok = check(wait_for(&s_low_passes, 5UL), "the low task does not run") && ok;

    (void)snprintf(request, sizeof(request), "h1 sensor.hang %s", plan->spin ? "spin" : "on");
    ok = check(strncmp(exchange(request), "h1 ok hang=", 11U) == 0, "sensor.hang refused") && ok;

    /* The reader is in its read once `hung` is 1; its read count then stands still. */
    for (uint32_t i = 0U; (i < 5000U) && (strstr(exchange("h2 sensor.status"), " hung=1") == NULL);
         i++) {
        sleep_ms(1U);
    }

    ok = check(strstr(exchange("h3 sensor.status"), " hung=1") != NULL, "no read is hung") && ok;

    const unsigned long reads = atomic_load(&s_reads);
    const unsigned long low = atomic_load(&s_low_passes);

    if (plan->spin) {
        /* The reader keeps the CPU: the task below it does not run at all. */
        sleep_ms(200U);
        ok = check(atomic_load(&s_low_passes) == low, "the low task ran during a spin") && ok;
    } else {
        /* The reader is blocked: the task below it runs on. */
        ok = check(wait_for(&s_low_passes, low + 20UL), "the low task stopped during a hang") && ok;
    }

    ok = check(atomic_load(&s_reads) == reads, "a read returned during the hang") && ok;
    ok = check(strncmp(exchange("h4 sensor.hang off"), "h4 ok hang=off", 14U) == 0,
               "sensor.hang off refused") &&
         ok;
    ok = check(wait_for(&s_reads, reads + 5UL), "the reader did not resume") && ok;
    ok = check(wait_for(&s_low_passes, atomic_load(&s_low_passes) + 5UL),
               "the low task did not resume") &&
         ok;
    atomic_store(&s_hang_status, ok ? EXIT_SUCCESS : EXIT_FAILURE);
    atomic_store(&s_bench_done, true);
    return NULL;
}

static int cmd_hang(bool spin)
{
    hang_plan_t plan = {.spin = spin};
    struct sockaddr_in any;
    socklen_t len = sizeof(any);
    char error[160];
    pthread_t bench;
    int probe = socket(AF_INET, SOCK_DGRAM, 0);

    (void)memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if ((probe < 0) || (bind(probe, (const struct sockaddr *)&any, sizeof(any)) != 0) ||
        (getsockname(probe, (struct sockaddr *)&any, &len) != 0)) {
        (void)fprintf(stderr, "cannot find a free port\n");
        return EXIT_FAILURE;
    }

    s_sim_port = ntohs(any.sin_port);
    (void)close(probe);
    s_client = socket(AF_INET, SOCK_DGRAM, 0);

    init_platform();

    if ((s_client < 0) || !posix_sim_open(s_sim_port, error, sizeof(error))) {
        (void)fprintf(stderr, "simulation control: %s\n", error);
        return EXIT_FAILURE;
    }

    posix_sensor_start(1U);
    posix_sensor_set_wait(wait_one_tick);
    create_task(stopper_task, "stopper", 6U, NULL);
    create_task(reader_task, "reader", 5U, NULL);
    create_task(low_task, "low", 2U, NULL);

    if (pthread_create(&bench, NULL, bench_main, &plan) != 0) {
        return EXIT_FAILURE;
    }

    posix_shutdown_guard_arm(POSIX_SHUTDOWN_TIMEOUT_MS * 4U);
    run_scheduler();
    (void)pthread_join(bench, NULL);
    posix_sim_close();
    (void)close(s_client);
    return atomic_load(&s_hang_status);
}

/* --------------------------------------------------------------------------------- main */

static int usage(void)
{
    (void)fprintf(stderr, "usage: wdg_selftest expire <timeout_ms> [<retained path>]\n"
                          "       wdg_selftest kicked <timeout_ms> <seconds>\n"
                          "       wdg_selftest start-rules | kick-cost <kicks>\n"
                          "       wdg_selftest retained <path>\n"
                          "       wdg_selftest watchdog-cause <timeout_ms> <path>\n"
                          "       wdg_selftest hang on|spin\n");
    return 2;
}

static uint32_t number(const char *text)
{
    return (uint32_t)strtoul(text, NULL, 10);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return usage();
    }

    const char *command = argv[1];

    if ((strcmp(command, "expire") == 0) && ((argc == 3) || (argc == 4))) {
        return cmd_expire(number(argv[2]), (argc == 4) ? argv[3] : NULL);
    }

    if ((strcmp(command, "kicked") == 0) && (argc == 4)) {
        return cmd_kicked(number(argv[2]), number(argv[3]));
    }

    if (strcmp(command, "start-rules") == 0) {
        return cmd_start_rules();
    }

    if ((strcmp(command, "kick-cost") == 0) && (argc == 3)) {
        return cmd_kick_cost(number(argv[2]));
    }

    if ((strcmp(command, "retained") == 0) && (argc == 3)) {
        return cmd_retained(argv[2]);
    }

    if ((strcmp(command, "watchdog-cause") == 0) && (argc == 4)) {
        return cmd_watchdog_cause(number(argv[2]), argv[3]);
    }

    if ((strcmp(command, "hang") == 0) && (argc == 3) &&
        ((strcmp(argv[2], "on") == 0) || (strcmp(argv[2], "spin") == 0))) {
        return cmd_hang(strcmp(argv[2], "spin") == 0);
    }

    return usage();
}
