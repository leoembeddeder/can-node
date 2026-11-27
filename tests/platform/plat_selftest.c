/* SPDX-License-Identifier: MIT */
/*
 * Self-tests of the POSIX platform layer. Each sub-command is a small FreeRTOS program that
 * exercises one mechanism the node depends on. CTest runs them through run_selftest.sh, which
 * checks the exit status and, for the fatal paths, the text on stderr; the measurement script
 * bench/platform_timing.py reads the JSON that `tick` and `irq` print on stdout.
 *
 *   tick <locked|stock> <seconds>      effective tick period, periodic wake-ups; locked: every
 *                                      tick period is counted or reported as dropped, and
 *                                      few are dropped; stock: as slow as the unmodified port
 *   suspend <milliseconds>             the tick keeps time across a suspended scheduler
 *   tick-late <rounds>                 a tick made up for one that was only late is given
 *                                      back at the next boundary: the count is never ahead
 *                                      of the clock for long
 *   irq <mode> <period_us> <seconds> <load>
 *                                      simulated interrupts from a free-running source:
 *                                      latency of every raise, sampled at the task's first
 *                                      run after its ISR and within bounds in poll mode, every
 *                                      raise noted by an ISR and seen by the task, nothing left
 *                                      latched, served by the path the mode names, handlers
 *                                      never nested; period 0 is an interrupt storm (a lost
 *                                      interrupt is irq-rounds' business: here the next raise
 *                                      would hide it)
 *   irq-rounds <mode> <rounds>         ping-pong: one raise, wait for the task to answer, at a
 *                                      random phase to the tick; no interrupt is lost and the
 *                                      task runs at once, not at the next tick
 *   irq-lines <mode>                   several lines and contexts, raised singly and together;
 *                                      lines without an ISR or out of range do nothing
 *   stack                              stack painting measures real depth
 *   log <path>                         lines from tasks and an external thread arrive whole
 *                                      and in order, none dropped
 *   log-preempt <path>                 the same while tasks are preempted inside the log sink
 *   assert | malloc-fail | stack-overflow | reset
 *                                      fatal paths exit with the documented status
 *   reset-log <path>                   a line logged just before a reset is in the log file
 *   guard                              a scheduler that does not stop is ended by the guard
 *   run                                idle until SIGINT/SIGTERM, then stop cleanly
 *
 * <mode> of the interrupt tests: signal (signal delivery, tick hook as safety net),
 * signal-only (the safety net switched off: the signal path has to serve everything alone) or
 * poll (tick-hook polling only, no signal is ever sent).
 *
 * Exit status: 0 passed, 1 failed, 2 usage, 75 (EX_TEMPFAIL) a timing tolerance was missed
 * although the mechanism itself worked: the host did not run the process well enough. Only 75
 * may be repeated (run_selftest.sh --retry-timing); a test must never use it for a wrong
 * result. The fatal-path commands exit with the platform's reset status instead.
 */
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"

#include "build_id.h"
#include "posix_platform.h"

#define TASK_STACK_WORDS configMINIMAL_STACK_SIZE

/* Exit status of a timing tolerance missed (sysexits.h: EX_TEMPFAIL). See the top of the file. */
#define TIMING_MISSED 75

/* ------------------------------------------------------------------------------ helpers */

static uint32_t s_run_seconds = 1U;

/* Combine two verdicts: a failure outweighs a missed timing tolerance, which outweighs a pass.
 * A run that found a defect must say so even if it also ran on a slow host. */
static int worse(int a, int b)
{
    int result = EXIT_SUCCESS;

    if ((a == EXIT_FAILURE) || (b == EXIT_FAILURE)) {
        result = EXIT_FAILURE;
    } else if ((a == TIMING_MISSED) || (b == TIMING_MISSED)) {
        result = TIMING_MISSED;
    } else {
        result = EXIT_SUCCESS;
    }

    return result;
}

static int compare_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

/* Opens every JSON report: the test's name and the build id of this binary, which
 * bench/platform_timing.py checks against the tree it records. */
static void print_report_head(const char *test)
{
    (void)printf("{\"test\": \"%s\", \"build_id\": \"%s\", ", test, fcn_build_id);
}

/* If FCN_SELFTEST_DUMP names a file, write the raw samples to it, one per line in
 * nanoseconds and in the order they were taken. bench/platform_timing.py plots them. */
static void dump_samples(const uint64_t *v, uint32_t n)
{
    const char *path = getenv("FCN_SELFTEST_DUMP");

    if (path != NULL) {
        FILE *file = fopen(path, "w");

        if (file != NULL) {
            for (uint32_t i = 0U; i < n; i++) {
                (void)fprintf(file, "%llu\n", (unsigned long long)v[i]);
            }

            (void)fclose(file);
        }
    }
}

/* Prints {"n":..,"mean":..,"min":..,"p50":..,"p99":..,"max":..} in microseconds; sorts v. */
static void print_stats_us(const char *key, uint64_t *v, uint32_t n)
{
    double sum = 0.0;

    if (n == 0U) {
        (void)printf("\"%s\": {\"n\": 0}", key);
        return;
    }

    qsort(v, n, sizeof(v[0]), compare_u64);

    for (uint32_t i = 0U; i < n; i++) {
        sum += (double)v[i];
    }

    const uint32_t median = n / 2U;
    const uint32_t p99 = (uint32_t)(((uint64_t)(n - 1U) * 99U) / 100U);

    (void)printf("\"%s\": {\"n\": %u, \"mean\": %.2f, \"min\": %.2f, \"p50\": %.2f, "
                 "\"p99\": %.2f, \"max\": %.2f}",
                 key, n, sum / (double)n / 1e3, (double)v[0] / 1e3, (double)v[median] / 1e3,
                 (double)v[p99] / 1e3, (double)v[n - 1U] / 1e3);
}

static void sleep_ns(uint64_t ns)
{
    const struct timespec pause = {(time_t)(ns / 1000000000ULL), (long)(ns % 1000000000ULL)};

    (void)nanosleep(&pause, NULL);
}

static void stopper_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(s_run_seconds * 1000U));
    plat_end_scheduler();

    for (;;) {
    }
}

static void create_task(TaskFunction_t code, const char *name, UBaseType_t priority,
                        TaskHandle_t *handle)
{
    const BaseType_t created = xTaskCreate(code, name, TASK_STACK_WORDS, NULL, priority, handle);

    configASSERT(created == pdPASS);
}

static void init_platform(posix_tick_mode_t tick, posix_irq_mode_t irq, const char *log_path)
{
    const posix_config_t config = {
        .tick_mode = tick, .irq_mode = irq, .log_level = LOG_LEVEL_INFO, .log_path = log_path};

    posix_platform_init(&config);
}

/* ------------------------------------------------------------------------- dropped ticks */

/*
 * Locked mode books the ticks it gives up as dropped (posix_tick.c). That counter is kept by
 * the code under test, so a test that accepted any number of dropped ticks would accept a tick
 * that loses time and books the loss: a tick 10 % slow passed every test that way. The tests
 * therefore keep the accounting identity (counted + dropped = wall time) and in addition
 * bound the dropped ticks that the host does not explain.
 *
 * The explanation comes from a witness that does not depend on the code under test: a thread
 * outside the scheduler that sleeps to absolute deadlines one tick period apart and measures
 * how late it wakes. When the host stops running the process (a loaded machine, a virtual
 * machine that is descheduled, a stopped process), the witness wakes late by about as long
 * as the tick was stopped, and the tick drops about one tick per period of that lateness.
 * Each late wake-up of the witness excuses that many ticks plus WITNESS_SLACK_TICKS for the
 * edges of the stall. Ticks dropped beyond what the witness excuses may be at most
 * DROPPED_SHARE_MAX of the measured span; more is TIMING_MISSED (75), repeated once. A tick
 * that is slow by itself drops ticks on every run while the witness sees nothing: 10 % slow
 * drops about 10 %, half speed about 50 %.
 *
 * Only a wake-up later than WITNESS_LATE_NS counts as a stall. Under CPU contention the
 * witness is also kept waiting for a CPU, a few milliseconds at a time, and every such wait
 * would excuse ticks; a tick that is slow by itself could hide behind them. A stall shorter
 * than POSIX_TICK_MAX_CATCH_UP_TICKS (6) periods drops no tick (posix_tick_plan.h makes those
 * up), so a threshold of 5 ms leaves out only stalls too short to explain a drop. Measured on
 * the development laptop with the process on 2 CPUs shared with 2 busy loops, `tick locked 3`
 * and `suspend 20`: the unmodified code left at most 0.79 % unexcused (40 runs each, debug and
 * asan, raw drops up to 6.8 %); a tick 3.3 % slow left at least 3.7 % unexcused in every one
 * of 10 runs (with a 2 ms threshold: 4.8 % at the median, against 6.1 % with 5 ms). A
 * threshold of 10 ms was too high: the unmodified code then missed the bound in 1 of 10 asan
 * runs (2.42 %).
 *
 * A host that starves only the node's threads and not the witness can still exceed the bound;
 * that is what the one repetition is for. platform_tick_suspended_scheduler, which bounds the
 * same drops over its own span, catches a slow tick as well.
 */
#define DROPPED_SHARE_MAX   0.02
#define WITNESS_LATE_NS     5000000ULL /* a wake-up later than this counts as a host stall */
#define WITNESS_SLACK_TICKS 2ULL

static _Atomic uint64_t s_witness_stall_ns; /* sum of the lateness of all stalls seen */
static _Atomic uint64_t s_witness_stalls;   /* number of stalls seen */
static _Atomic bool s_witness_run;
static pthread_t s_witness;

static void *witness_main(void *arg)
{
    const uint64_t period_ns = 1000000000ULL / (uint64_t)configTICK_RATE_HZ;
    uint64_t deadline_ns;

    (void)arg;
    posix_external_thread_setup("witness");
    (void)prctl(PR_SET_TIMERSLACK, 1UL);
    deadline_ns = plat_time_ns();

    while (atomic_load(&s_witness_run)) {
        deadline_ns += period_ns;

        const struct timespec deadline = {(time_t)(deadline_ns / 1000000000ULL),
                                          (long)(deadline_ns % 1000000000ULL)};

        (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);

        const uint64_t now_ns = plat_time_ns();

        if (now_ns > (deadline_ns + WITNESS_LATE_NS)) {
            (void)atomic_fetch_add(&s_witness_stall_ns, now_ns - deadline_ns);
            (void)atomic_fetch_add(&s_witness_stalls, 1U);
            deadline_ns = now_ns; /* start afresh rather than catch up */
        }
    }

    return NULL;
}

static void witness_start(void)
{
    atomic_store(&s_witness_run, true);

    if (pthread_create(&s_witness, NULL, witness_main, NULL) != 0) {
        (void)fprintf(stderr, "cannot start the witness thread\n");
        exit(EXIT_FAILURE);
    }
}

static void witness_stop(void)
{
    atomic_store(&s_witness_run, false);
    (void)pthread_join(s_witness, NULL);
}

/* What the tick has dropped and what the witness has seen, at one moment. */
typedef struct {
    uint64_t dropped;
    uint64_t stall_ns;
    uint64_t stalls;
} drop_snapshot_t;

static drop_snapshot_t drop_snapshot(void)
{
    posix_tick_stats_t tick;
    drop_snapshot_t now;

    posix_tick_get_stats(&tick);
    now.dropped = tick.dropped_ticks;
    now.stall_ns = atomic_load(&s_witness_stall_ns);
    now.stalls = atomic_load(&s_witness_stalls);
    return now;
}

/* Between two snapshots: ticks dropped, ticks the witness excuses, and how many of the dropped
 * ones it does not excuse. */
typedef struct {
    uint64_t dropped;
    uint64_t stalls;
    uint64_t stall_us;
    uint64_t excused;
    uint64_t unexcused;
} drop_account_t;

static drop_account_t drop_account(const drop_snapshot_t *from, const drop_snapshot_t *to)
{
    const uint64_t period_ns = 1000000000ULL / (uint64_t)configTICK_RATE_HZ;
    const uint64_t stall_ns = to->stall_ns - from->stall_ns;
    drop_account_t account;

    account.dropped = to->dropped - from->dropped;
    account.stalls = to->stalls - from->stalls;
    account.stall_us = stall_ns / 1000ULL;
    account.excused =
        ((stall_ns + period_ns - 1U) / period_ns) + (account.stalls * WITNESS_SLACK_TICKS);
    account.unexcused =
        (account.dropped > account.excused) ? (account.dropped - account.excused) : 0U;
    return account;
}

/* Prints the account as JSON members, without braces. */
static void print_drop_account(const drop_account_t *account, double span_ticks)
{
    (void)printf("\"dropped_ticks_in_span\": %llu, \"host_stalls_seen\": %llu, "
                 "\"host_stall_us\": %llu, \"dropped_excused_by_stalls\": %llu, "
                 "\"dropped_unexcused\": %llu, \"dropped_unexcused_percent\": %.3f, "
                 "\"dropped_unexcused_max_percent\": %.1f, ",
                 (unsigned long long)account->dropped, (unsigned long long)account->stalls,
                 (unsigned long long)account->stall_us, (unsigned long long)account->excused,
                 (unsigned long long)account->unexcused,
                 100.0 * (double)account->unexcused / span_ticks, DROPPED_SHARE_MAX * 100.0);
}

/* TIMING_MISSED if more dropped ticks than DROPPED_SHARE_MAX of the span are unexcused. */
static int check_drop_account(const drop_account_t *account, double span_ticks)
{
    const double share = (double)account->unexcused / span_ticks;
    int status = EXIT_SUCCESS;

    if (share > DROPPED_SHARE_MAX) {
        (void)fprintf(stderr,
                      "%llu of %.0f tick periods were dropped, %llu of them (%.1f %%) without a "
                      "host stall to explain them (%llu stalls, %llu us, seen by the witness); "
                      "at most %.0f %% may be unexplained: the tick loses time, or the host "
                      "starved the node's threads only\n",
                      (unsigned long long)account->dropped, span_ticks,
                      (unsigned long long)account->unexcused, share * 100.0,
                      (unsigned long long)account->stalls, (unsigned long long)account->stall_us,
                      DROPPED_SHARE_MAX * 100.0);
        status = TIMING_MISSED;
    }

    return status;
}

/* --------------------------------------------------------------------------------- tick */

#define TICK_MAX_SAMPLES    12000U
#define TICK_TASK_PERIOD_MS 10U
/* Lowest effective tick period of the stock mode, as a multiple of the nominal one. The
 * development laptop (WSL2, debug, asan and release) measured 1.097 to 1.117 idle and 1.071
 * to 1.175 on 2 CPUs shared with 2 busy loops; with the timer slack removed from the stock
 * tick 1.039 to 1.054. A stock mode that ran the locked tick measured 0.9999 to 1.012. */
#define STOCK_PERIOD_MIN 1.02

static uint64_t s_wake_ns[TICK_MAX_SAMPLES];
static volatile uint32_t s_wake_count;
static drop_snapshot_t s_drops_at_first_wake;
static drop_snapshot_t s_drops_at_last_wake;

static void periodic_task(void *arg)
{
    TickType_t last_wake;

    (void)arg;
    plat_task_enter("periodic");
    last_wake = xTaskGetTickCount();

    for (;;) {
        (void)xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_TASK_PERIOD_MS));

        if (s_wake_count < TICK_MAX_SAMPLES) {
            /* The dropped count is taken with every sample, so that the ticks dropped between
             * the first and the last sample are known exactly; ticks dropped while starting
             * or stopping the scheduler lie outside the measured span. */
            const drop_snapshot_t drops = drop_snapshot();

            if (s_wake_count == 0U) {
                s_drops_at_first_wake = drops;
            }

            s_drops_at_last_wake = drops;
            s_wake_ns[s_wake_count] = plat_time_ns();
            s_wake_count++;
        }
    }
}

static int cmd_tick(posix_tick_mode_t mode)
{
    static uint64_t interval_ns[TICK_MAX_SAMPLES];
    posix_tick_stats_t tick;
    int status = EXIT_SUCCESS;

    init_platform(mode, POSIX_IRQ_SIGNAL, NULL);
    create_task(periodic_task, "periodic", 3U, NULL);
    create_task(stopper_task, "stopper", 5U, NULL);
    witness_start();
    posix_platform_run();
    witness_stop();
    posix_platform_shutdown();
    posix_tick_get_stats(&tick);

    const uint32_t n = s_wake_count;

    if (n < 3U) {
        (void)printf("{\"test\": \"tick\", \"error\": \"too few samples\"}\n");
        return EXIT_FAILURE;
    }

    for (uint32_t i = 1U; i < n; i++) {
        interval_ns[i - 1U] = s_wake_ns[i] - s_wake_ns[i - 1U];
    }

    /* Steady-state tick period: wall time between the first and the last wake-up of the
     * periodic task over the ticks that passed in between. */
    const double span_ns = (double)(s_wake_ns[n - 1U] - s_wake_ns[0]);
    const double span_ticks = (double)(n - 1U) * (double)pdMS_TO_TICKS(TICK_TASK_PERIOD_MS);
    const double period_us = span_ns / span_ticks / 1e3;
    const double nominal_us = 1e6 / (double)configTICK_RATE_HZ;
    const double drift_us = (span_ns / 1e3) - (span_ticks * nominal_us);
    const drop_account_t drops = drop_account(&s_drops_at_first_wake, &s_drops_at_last_wake);

    /* What locked mode promises is an identity: every tick period of wall time is either
     * counted by the kernel or reported as dropped, so (counted + dropped) periods equal the
     * time that passed. It holds on a loaded host too, up to the few ticks that are being
     * made up or given back when the span ends (posix_tick_plan.h,
     * POSIX_TICK_MAX_CATCH_UP_TICKS); a miss is a defect.
     * How many ticks were dropped is bounded separately (DROPPED_SHARE_MAX).
     * The stock mode is the port as it comes: it never makes up or drops a tick, and its
     * figure is simply how far the tick count fell behind. */
    const double accounted_us = (span_ticks + (double)drops.dropped) * nominal_us;
    const double unaccounted = ((span_ns / 1e3) - accounted_us) / (span_ns / 1e3);

    print_report_head("tick");
    (void)printf("\"mode\": \"%s\", \"seconds\": %u, "
                 "\"nominal_tick_period_us\": %.1f, \"effective_tick_period_us\": %.3f, "
                 "\"drift_us\": %.1f, \"tick_thread_seen\": %s, \"signals_sent\": %llu, "
                 "\"catch_ups\": %llu, \"dropped_ticks\": %llu, \"give_backs\": %llu, "
                 "\"unaccounted_time_percent\": %.3f, ",
                 (mode == POSIX_TICK_LOCKED) ? "locked" : "stock", (unsigned int)s_run_seconds,
                 nominal_us, period_us, drift_us, tick.tick_thread_seen ? "true" : "false",
                 (unsigned long long)tick.signals_sent, (unsigned long long)tick.catch_ups,
                 (unsigned long long)tick.dropped_ticks, (unsigned long long)tick.give_backs,
                 unaccounted * 100.0);
    print_drop_account(&drops, span_ticks);
    dump_samples(interval_ns, n - 1U);
    print_stats_us("wake_interval_us", interval_ns, n - 1U);
    (void)printf("}\n");

    if (!tick.tick_thread_seen) {
        (void)fprintf(stderr, "the usleep wrapper never saw the port's tick thread\n");
        status = EXIT_FAILURE;
    }

    if (tick.mode != mode) {
        (void)fprintf(stderr, "the tick code runs in the other mode than the one configured\n");
        status = EXIT_FAILURE;
    }

    if ((mode == POSIX_TICK_STOCK) &&
        ((tick.catch_ups != 0U) || (tick.dropped_ticks != 0U) || (tick.give_backs != 0U))) {
        (void)fprintf(stderr,
                      "stock mode changed the tick: %llu catch-ups, %llu dropped, %llu given "
                      "back\n",
                      (unsigned long long)tick.catch_ups, (unsigned long long)tick.dropped_ticks,
                      (unsigned long long)tick.give_backs);
        status = EXIT_FAILURE;
    }

    /* The counters above are kept by the tick code itself and only show what it chose to
     * report. What the stock port does can be seen from outside: every tick lasts the relative
     * sleep plus the wake-up's latency (with the default timer slack of up to 50 us) plus the
     * loop, so the period is clearly longer than nominal (STOCK_PERIOD_MIN). A stock mode that
     * ran the locked tick and reported stock would keep the nominal period. */
    if ((mode == POSIX_TICK_STOCK) && (period_us < (STOCK_PERIOD_MIN * nominal_us))) {
        (void)fprintf(stderr,
                      "stock mode kept time: tick period %.1f us, the unmodified port's relative "
                      "sleep lasts at least %.0f us\n",
                      period_us, STOCK_PERIOD_MIN * nominal_us);
        status = EXIT_FAILURE;
    }

    if ((mode == POSIX_TICK_LOCKED) && ((unaccounted < -0.005) || (unaccounted > 0.005))) {
        (void)fprintf(stderr,
                      "tick count and wall clock disagree by %.2f %% (tick period %.1f us, "
                      "%llu ticks reported as dropped)\n",
                      unaccounted * 100.0, period_us, (unsigned long long)drops.dropped);
        status = EXIT_FAILURE;
    }

    if (mode == POSIX_TICK_LOCKED) {
        status = worse(status, check_drop_account(&drops, span_ticks));
    }

    return status;
}

/* ------------------------------------------------------------------ suspended scheduler */

/*
 * A task keeps the scheduler suspended across several tick periods. The kernel takes its
 * ticks all the same, notes them and adds them to the tick count on resume. The locked tick
 * must not mistake those ticks for lost ones (docs/bug-log.md, entry 003): the tick count has
 * to advance by the time that passed, during the suspension and after it.
 *
 * After the resume the task blocks for SUSPEND_AFTER_MS ticks and the wall time of that delay
 * is compared with it: a count that ran ahead and then stood still to let the clock catch up
 * makes the delay last longer than its ticks. The task blocks instead of busy-waiting because
 * a thread that never sleeps is the first one the host takes the CPU from when other work
 * competes for it; a second of busy-waiting dropped up to 10 % of the ticks on the development
 * laptop while other builds ran, with nothing wrong in the tick. The 20 ms inside the
 * suspension have to be busy, and are short.
 */
#define SUSPEND_SETTLE_MS 50U
#define SUSPEND_AFTER_MS  1000U
/* Ticks. At any moment the tick count may trail the clock by the ticks that are still being
 * made up (posix_tick_plan.h, POSIX_TICK_MAX_CATCH_UP_TICKS = 6) plus the one just raised, or
 * lead it by a made-up tick that is given back at the next boundary, and ticks given up are
 * booked when the tick thread next wakes, a period later. An interval between two readings can
 * therefore be off by 8 ticks either way without anything being wrong; on two CPUs shared with
 * two busy loops a tolerance of 3 failed 2 runs of 40 with 4 and 5 ticks. */
#define SUSPEND_TOLERANCE 8U

static uint32_t s_suspend_ms = 20U;
static uint32_t s_suspend_ticks_during;
static uint32_t s_suspend_ticks_after;
static uint64_t s_suspend_wall_during_ns;
static uint64_t s_suspend_wall_after_ns;
static drop_snapshot_t s_suspend_drops[3]; /* before, at the resume, at the end */

static void busy_for_ms(uint32_t milliseconds)
{
    static volatile uint32_t sink;
    const uint64_t until = plat_time_ns() + ((uint64_t)milliseconds * 1000000ULL);

    while (plat_time_ns() < until) {
        sink++;
    }
}

static void suspend_task(void *arg)
{
    (void)arg;
    plat_task_enter("suspend");
    vTaskDelay(pdMS_TO_TICKS(SUSPEND_SETTLE_MS));

    const TickType_t before = xTaskGetTickCount();
    const uint64_t t0 = plat_time_ns();

    s_suspend_drops[0] = drop_snapshot();
    vTaskSuspendAll();
    busy_for_ms(s_suspend_ms);
    (void)xTaskResumeAll();

    const TickType_t resumed = xTaskGetTickCount();
    const uint64_t t1 = plat_time_ns();

    s_suspend_drops[1] = drop_snapshot();
    vTaskDelay(pdMS_TO_TICKS(SUSPEND_AFTER_MS));

    const TickType_t after = xTaskGetTickCount();
    const uint64_t t2 = plat_time_ns();

    s_suspend_drops[2] = drop_snapshot();
    s_suspend_ticks_during = (uint32_t)(resumed - before);
    s_suspend_ticks_after = (uint32_t)(after - resumed);
    s_suspend_wall_during_ns = t1 - t0;
    s_suspend_wall_after_ns = t2 - t1;
    plat_end_scheduler();

    for (;;) {
    }
}

/* EXIT_SUCCESS if `ticks` is the number of tick periods in `wall_ns`, give or take the
 * tolerance and the ticks dropped in that interval; EXIT_FAILURE otherwise. */
static int check_ticks_against_wall(const char *interval, uint32_t ticks, uint64_t wall_ns,
                                    uint64_t dropped)
{
    const uint64_t period_ns = 1000000000ULL / (uint64_t)configTICK_RATE_HZ;
    const uint64_t expected = wall_ns / period_ns;
    int status = EXIT_SUCCESS;

    if ((uint64_t)ticks > (expected + SUSPEND_TOLERANCE)) {
        (void)fprintf(stderr, "the tick count ran ahead %s: %u ticks in %llu us\n", interval,
                      (unsigned int)ticks, (unsigned long long)(wall_ns / 1000ULL));
        status = EXIT_FAILURE;
    }

    if (((uint64_t)ticks + SUSPEND_TOLERANCE + dropped) < expected) {
        (void)fprintf(stderr, "time was lost silently %s: %u ticks and %llu dropped in %llu us\n",
                      interval, (unsigned int)ticks, (unsigned long long)dropped,
                      (unsigned long long)(wall_ns / 1000ULL));
        status = EXIT_FAILURE;
    }

    return status;
}

static int cmd_suspend(void)
{
    posix_tick_stats_t tick;
    int status = EXIT_SUCCESS;

    init_platform(POSIX_TICK_LOCKED, POSIX_IRQ_SIGNAL, NULL);
    create_task(suspend_task, "suspend", 3U, NULL);
    witness_start();
    posix_platform_run();
    witness_stop();
    posix_platform_shutdown();
    posix_tick_get_stats(&tick);

    const uint64_t period_ns = 1000000000ULL / (uint64_t)configTICK_RATE_HZ;
    const double span_ticks =
        (double)(s_suspend_wall_during_ns + s_suspend_wall_after_ns) / (double)period_ns;
    const drop_account_t during = drop_account(&s_suspend_drops[0], &s_suspend_drops[1]);
    const drop_account_t after = drop_account(&s_suspend_drops[1], &s_suspend_drops[2]);
    const drop_account_t whole = drop_account(&s_suspend_drops[0], &s_suspend_drops[2]);

    print_report_head("suspend");
    (void)printf("\"suspended_ms\": %u, "
                 "\"ticks_during\": %u, \"wall_during_us\": %llu, \"dropped_during\": %llu, "
                 "\"ticks_after\": %u, \"wall_after_us\": %llu, \"dropped_after\": %llu, ",
                 (unsigned int)s_suspend_ms, (unsigned int)s_suspend_ticks_during,
                 (unsigned long long)(s_suspend_wall_during_ns / 1000ULL),
                 (unsigned long long)during.dropped, (unsigned int)s_suspend_ticks_after,
                 (unsigned long long)(s_suspend_wall_after_ns / 1000ULL),
                 (unsigned long long)after.dropped);
    print_drop_account(&whole, span_ticks);
    (void)printf("\"catch_ups\": %llu, \"dropped_ticks\": %llu, \"give_backs\": %llu}\n",
                 (unsigned long long)tick.catch_ups, (unsigned long long)tick.dropped_ticks,
                 (unsigned long long)tick.give_backs);

    status = worse(status, check_ticks_against_wall("while the scheduler was suspended",
                                                    s_suspend_ticks_during,
                                                    s_suspend_wall_during_ns, during.dropped));
    status = worse(status, check_ticks_against_wall("after the scheduler was resumed",
                                                    s_suspend_ticks_after, s_suspend_wall_after_ns,
                                                    after.dropped));
    return worse(status, check_drop_account(&whole, span_ticks));
}

/* ------------------------------------------------------------------- a tick taken late */

/*
 * A tick that the port aims at a task's thread while that task is being switched out stays
 * pending on the thread, with its signals blocked, until the task runs again. If that is more
 * than a period later, the locked tick takes it for lost and makes it up; when the task runs
 * again it takes the late one as well, and the kernel holds one tick more than the clock. The
 * locked tick has to give it back at the next boundary (platform/posix/posix_tick_plan.h,
 * docs/bug-log.md, entry 011).
 *
 * The test makes that happen on purpose, round after round. The holder task enters a critical
 * section (the port blocks its thread's signals) and waits there until the tick is pending on
 * its thread (sigpending()). Still inside, it notifies the runner, of a higher priority, and
 * the kernel switches to the runner at once: the holder's thread is suspended with the tick
 * pending. The runner works for LATE_RUNNER_NS, 3.5 periods, taking the ticks the port now
 * sends to its own thread, while the locked tick makes up the one pending on the holder's.
 * Then the runner blocks, the holder leaves its critical section and takes the late tick.
 *
 * From there the holder watches the tick count for LATE_WATCH_PERIODS periods. A count seen
 * before its tick was due (posix_tick_due_ns()) was reached early: a task waiting for that
 * tick would have been released early. The late tick itself is early (by about half a period),
 * and the next tick may not be: given back at the boundary, the count is on the grid again. A
 * count one ahead for good makes every tick after it early. Two early ticks in a row fail the
 * test (status 1). This verdict does not depend on how promptly the host runs the threads: a
 * tick raised late is late, not early, and a count that changed between two looks of the
 * holder ends a row instead of making one.
 *
 * What depends on the host is whether the late tick is made up at all: the locked tick takes a
 * tick for lost only if the tick it has just raised is not yet taken when it reads the count
 * (posix_tick_plan.h). A round in which every raise to the runner was taken before that read
 * makes nothing up and shows no early tick. At least a quarter of the rounds have to show one;
 * fewer means the host did not let the test provoke what it tests (75).
 */
#define LATE_RUNNER_NS            3500000ULL
#define LATE_WATCH_PERIODS        6U
#define LATE_PENDING_WAIT_PERIODS 5U
#define LATE_MAX_ROUNDS           1000U

static uint32_t s_late_rounds = 50U;
static TaskHandle_t s_late_runner;

typedef struct {
    bool pended;          /* the tick became pending on the holder's thread */
    uint32_t early_ticks; /* counts the holder saw before their tick was due */
    uint32_t early_row;   /* the most of them in a row */
    uint64_t earliest_ns; /* how long before its tick was due the earliest was seen */
} late_round_t;

static late_round_t s_late[LATE_MAX_ROUNDS];

static bool alarm_pending_here(void)
{
    sigset_t pending;

    return (sigpending(&pending) == 0) && (sigismember(&pending, SIGALRM) == 1);
}

static void busy_until_ns(uint64_t until_ns)
{
    static volatile uint32_t sink;

    while (plat_time_ns() < until_ns) {
        sink++;
    }
}

static void late_runner_task(void *arg)
{
    (void)arg;
    plat_task_enter("runner");

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        busy_until_ns(plat_time_ns() + LATE_RUNNER_NS);
    }
}

/* Watches the tick count until `until_ns`: every count at its first sight, early or not. */
static void watch_early(uint64_t until_ns, late_round_t *r)
{
    uint64_t last = 0U;
    uint32_t row = 0U;

    for (;;) {
        /* The count first: it was reached before the time is read. */
        const uint64_t count = (uint64_t)xTaskGetTickCount();
        const uint64_t now = plat_time_ns();

        if (count != last) {
            const uint64_t due_ns = posix_tick_due_ns(count);

            /* A count that went up by more than one hides the ones between: a new row. */
            row = ((last != 0U) && (count == (last + 1U))) ? row : 0U;

            if (now < due_ns) {
                row++;
                r->early_ticks++;
                r->early_row = (row > r->early_row) ? row : r->early_row;
                r->earliest_ns =
                    ((due_ns - now) > r->earliest_ns) ? (due_ns - now) : r->earliest_ns;
            } else {
                row = 0U;
            }

            last = count;
        }

        if (now >= until_ns) {
            break;
        }
    }
}

static void late_holder_task(void *arg)
{
    const uint64_t period_ns = 1000000000ULL / (uint64_t)configTICK_RATE_HZ;

    (void)arg;
    plat_task_enter("holder");
    vTaskDelay(pdMS_TO_TICKS(SUSPEND_SETTLE_MS));

    for (uint32_t round = 0U; round < s_late_rounds; round++) {
        late_round_t *r = &s_late[round];

        vTaskDelay(1U + (round % 3U));

        const uint64_t give_up_ns = plat_time_ns() + (LATE_PENDING_WAIT_PERIODS * period_ns);

        taskENTER_CRITICAL();

        while (!r->pended && (plat_time_ns() < give_up_ns)) {
            r->pended = alarm_pending_here();
        }

        if (r->pended) {
            /* Switches to the runner at once: this thread is suspended with the tick pending. */
            (void)xTaskNotifyGive(s_late_runner);
        }

        taskEXIT_CRITICAL(); /* the late tick is taken here */
        watch_early(plat_time_ns() + (LATE_WATCH_PERIODS * period_ns), r);
    }

    plat_end_scheduler();

    for (;;) {
    }
}

static int cmd_tick_late(void)
{
    posix_tick_stats_t tick;
    uint32_t pended = 0U;
    uint32_t provoked = 0U;
    uint32_t failed = 0U;
    uint32_t early_ticks = 0U;
    uint32_t longest_row = 0U;
    uint64_t earliest_ns = 0U;
    int status = EXIT_SUCCESS;

    init_platform(POSIX_TICK_LOCKED, POSIX_IRQ_SIGNAL, NULL);
    create_task(late_holder_task, "holder", 2U, NULL);
    create_task(late_runner_task, "runner", 4U, &s_late_runner);
    posix_platform_run();
    posix_platform_shutdown();
    posix_tick_get_stats(&tick);

    for (uint32_t i = 0U; i < s_late_rounds; i++) {
        const late_round_t *r = &s_late[i];

        pended += r->pended ? 1U : 0U;
        provoked += (r->early_ticks > 0U) ? 1U : 0U;
        failed += (r->early_row > 1U) ? 1U : 0U;
        early_ticks += r->early_ticks;
        longest_row = (r->early_row > longest_row) ? r->early_row : longest_row;
        earliest_ns = (r->earliest_ns > earliest_ns) ? r->earliest_ns : earliest_ns;
    }

    print_report_head("tick-late");
    (void)printf("\"rounds\": %u, \"tick_pending_in_rounds\": %u, \"rounds_with_an_early_tick\": "
                 "%u, \"rounds_with_early_ticks_in_a_row\": %u, \"early_ticks\": %u, "
                 "\"most_early_ticks_in_a_row\": %u, \"earliest_us\": %.1f, "
                 "\"catch_ups\": %llu, \"give_backs\": %llu, \"dropped_ticks\": %llu}\n",
                 (unsigned int)s_late_rounds, (unsigned int)pended, (unsigned int)provoked,
                 (unsigned int)failed, (unsigned int)early_ticks, (unsigned int)longest_row,
                 (double)earliest_ns / 1e3, (unsigned long long)tick.catch_ups,
                 (unsigned long long)tick.give_backs, (unsigned long long)tick.dropped_ticks);

    if (failed > 0U) {
        (void)fprintf(stderr,
                      "after a late tick the count stayed ahead of the clock: %u ticks early in "
                      "a row (at most 1: the late one, given back at the next boundary), in %u "
                      "of %u rounds\n",
                      (unsigned int)longest_row, (unsigned int)failed, (unsigned int)s_late_rounds);
        status = EXIT_FAILURE;
    } else if ((provoked * 4U) < s_late_rounds) {
        (void)fprintf(stderr,
                      "an early tick in only %u of %u rounds (the tick pending in %u): the host "
                      "did not let the test provoke a tick made up for a late one\n",
                      (unsigned int)provoked, (unsigned int)s_late_rounds, (unsigned int)pended);
        status = TIMING_MISSED;
    } else {
        /* every round within the rule, enough of them provoked */
    }

    return status;
}

/* ---------------------------------------------------------------------- interrupt modes */

#define IRQ_TEST_LINE 0U

typedef enum { MODE_SIGNAL, MODE_SIGNAL_ONLY, MODE_POLL } irq_test_mode_t;

static const char *mode_name(irq_test_mode_t mode)
{
    const char *name = "poll";

    if (mode == MODE_SIGNAL) {
        name = "signal";
    } else if (mode == MODE_SIGNAL_ONLY) {
        name = "signal-only";
    } else {
        /* poll */
    }

    return name;
}

/* Sets up the platform for an interrupt test. signal-only is signal delivery with the tick
 * hook's safety net switched off. */
static void init_irq_platform(irq_test_mode_t mode)
{
    init_platform(POSIX_TICK_LOCKED, (mode == MODE_POLL) ? POSIX_IRQ_POLL : POSIX_IRQ_SIGNAL, NULL);
    posix_irq_test_set_tick_fallback(mode != MODE_SIGNAL_ONLY);
}

/* EXIT_FAILURE unless the delivery counters show the mechanism the mode names. */
static int check_delivery_path(irq_test_mode_t mode, const posix_irq_stats_t *irq)
{
    int status = EXIT_SUCCESS;

    if (mode == MODE_POLL) {
        /* Polling only: the signal is never used. */
        if ((irq->signals_sent != 0U) || (irq->serviced_by_signal != 0U) ||
            (irq->serviced_by_tick == 0U)) {
            status = EXIT_FAILURE;
        }
    } else if (mode == MODE_SIGNAL_ONLY) {
        /* Without the safety net every signal must be served by its own handler run: each
         * signal is sent when the latch goes from empty to non-empty, and the handler run it
         * causes is the only thing that empties the latch again. */
        if ((irq->serviced_by_tick != 0U) || (irq->signals_sent == 0U) ||
            (irq->serviced_by_signal != irq->signals_sent)) {
            status = EXIT_FAILURE;
        }
    } else {
        /* With the safety net the tick may serve what the signal path did not reach first;
         * the signal path has to have served some. */
        if ((irq->signals_sent == 0U) || (irq->serviced_by_signal == 0U)) {
            status = EXIT_FAILURE;
        }
    }

    if (status != EXIT_SUCCESS) {
        (void)fprintf(stderr,
                      "%s mode, wrong delivery path: %llu signals sent, %llu passes served by "
                      "the signal handler, %llu by the tick hook\n",
                      mode_name(mode), (unsigned long long)irq->signals_sent,
                      (unsigned long long)irq->serviced_by_signal,
                      (unsigned long long)irq->serviced_by_tick);
    }

    return status;
}

/* ---------------------------------------------------------------------------------- irq */

/*
 * A free-running "peripheral" raises the line; a task takes each interrupt. Latency is
 * measured per raise, from the raise to the task's first run after an ISR noted it.
 *
 * The source writes the time of raise i into s_raise_ns[i], counts it in s_raises_started,
 * calls posix_irq_raise() and, once that has returned, counts it in s_raises_published. The
 * ISR reads s_raises_started and then the clock, writes that time into s_isr_ns for every raise
 * it is the first to find counted, and notes the count in s_started_at_isr. When the task
 * wakes it takes one sample for every raise that an ISR has noted and that has been published.
 *
 * What that guarantees: a sample is taken after posix_irq_raise() has returned and after an
 * ISR ran that began after the raise was counted. It is not quite "after the ISR that served
 * the raise": the source counts a raise just before posix_irq_raise() latches the line, so a
 * dispatch pass that begins in between notes the raise without serving it, a later pass serves
 * it, and a wake-up of the task in between samples it too early. The window is the few
 * instructions between the count and the latch: rare at a period of 250 us, frequent in a
 * storm. A sample can also be too long: a raise whose ISR ran before the source could publish
 * it (the signal handler started on another CPU while kill() was returning) is sampled at the
 * task's next wake-up.
 *
 * Each sample is checked for what the guarantee needs, all of it things the test can see
 * directly, whatever the host does:
 *   - an ISR noted the raise (s_isr_ns is set) and the raise was timestamped no later than
 *     that ISR ran;
 *   - the sample is longer than the time from the raise to that ISR: the task runs after the
 *     ISR, so a sample of raise-to-ISR, or of 0, is impossible;
 *   - it was taken at the first wake-up at which the raise was ready: a raise that was noted
 *     and published when an earlier wake-up looked (s_ready_at_last_wakeup) must have been
 *     sampled then, not credited with a later wake-up's wait.
 * n is the number of raises, less those without a sample (latency_samples_not_recorded): in a
 * run with a period only the last raise can be left over (its ISR ran before it was published
 * and no later wake-up came). Raises beyond IRQ_MAX_SAMPLES (a storm, or a run longer than
 * IRQ_MAX_SAMPLES periods) have no slot and are not expected to have a sample.
 */
#define IRQ_MAX_SAMPLES 1000000U
/* No task of this test has a reason to go deeper than this. More means interrupt handlers
 * were stacked on top of each other (docs/bug-log.md, entry 002). */
#define IRQ_STACK_LIMIT_BYTES (48U * 1024U)
/* Bounds on the latency of a run with a period; see check_latency(). */
#define IRQ_NOT_RECORDED_MAX     1U
#define IRQ_POLL_MEDIAN_MAX_TICK 0.88

static TaskHandle_t s_rx_task;
static _Atomic bool s_irq_go; /* the task is ready: the source may start raising */
static _Atomic uint32_t s_raises_started;
static _Atomic uint32_t s_raises_published;
static _Atomic uint32_t s_started_at_isr; /* s_raises_started when the ISR last ran */
static _Atomic uint32_t s_task_seen;
static _Atomic bool s_source_run;
static _Atomic bool s_source_done;       /* the source has left its loop: no raise follows */
static uint32_t s_irq_period_us = 1000U; /* 0: raise as fast as the source thread can */
static uint64_t s_source_first_ns;
static uint64_t s_source_last_ns;
static uint64_t s_raise_ns[IRQ_MAX_SAMPLES];
static _Atomic uint64_t s_isr_ns[IRQ_MAX_SAMPLES]; /* when an ISR first found raise i counted */
static uint64_t s_latency_ns[IRQ_MAX_SAMPLES];
static volatile uint32_t s_latency_count;
static volatile uint32_t s_task_wakeups;
static uint32_t s_samples_unnoted;      /* sampled although no ISR had noted the raise */
static uint32_t s_samples_stamped_late; /* raise timestamped after the ISR that noted it */
static uint32_t s_samples_too_short;    /* no longer than the raise-to-ISR time */
static uint32_t s_samples_late;         /* not sampled at the first wake-up it was ready at */
static uint32_t s_ready_at_last_wakeup; /* raises ready when the task last looked (capped) */

static void test_isr(void *context, BaseType_t *woken)
{
    const uint32_t started = atomic_load(&s_raises_started);
    const uint64_t now_ns = plat_time_ns();
    uint32_t noted = atomic_load(&s_started_at_isr);

    (void)context;

    /* The times first, then the count that lets the task read them. The count is only ever
     * raised, in case the signal handler and the tick hook both dispatch. */
    while (started > noted) {
        const uint32_t end = (started < IRQ_MAX_SAMPLES) ? started : IRQ_MAX_SAMPLES;

        for (uint32_t i = noted; i < end; i++) {
            atomic_store_explicit(&s_isr_ns[i], now_ns, memory_order_relaxed);
        }

        if (atomic_compare_exchange_weak(&s_started_at_isr, &noted, started)) {
            break;
        }
    }

    vTaskNotifyGiveFromISR(s_rx_task, woken);
}

static void rx_task(void *arg)
{
    (void)arg;
    plat_task_enter("rx");
    atomic_store(&s_irq_go, true);

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_task_wakeups++;

        /* Raises an ISR has noted, and of those the ones that may be sampled (published). */
        const uint32_t served = atomic_load(&s_started_at_isr);
        const uint32_t published = atomic_load(&s_raises_published);
        const uint32_t ready = (served < published) ? served : published;
        const uint32_t limit = (ready < IRQ_MAX_SAMPLES) ? ready : IRQ_MAX_SAMPLES;
        const uint64_t now_ns = plat_time_ns();

        /* Everything that was ready the last time must have been sampled then. */
        if (s_ready_at_last_wakeup > s_latency_count) {
            s_samples_late += s_ready_at_last_wakeup - s_latency_count;
        }

        for (uint32_t i = s_latency_count; i < limit; i++) {
            s_latency_ns[i] = now_ns - s_raise_ns[i];

            const uint64_t isr_ns = atomic_load_explicit(&s_isr_ns[i], memory_order_relaxed);

            if (isr_ns == 0U) {
                s_samples_unnoted++;
            } else if (s_raise_ns[i] > isr_ns) {
                s_samples_stamped_late++;
            } else if (s_latency_ns[i] <= (isr_ns - s_raise_ns[i])) {
                s_samples_too_short++;
            } else {
                /* raised, then noted by an ISR, then sampled by the task after it */
            }
        }

        if (limit > s_latency_count) {
            s_latency_count = limit;
        }

        s_ready_at_last_wakeup = (ready < IRQ_MAX_SAMPLES) ? ready : IRQ_MAX_SAMPLES;

        /* The task has run after an ISR noted these raises. */
        atomic_store(&s_task_seen, served);
    }
}

/* A mid-priority task that is busy 2 ms out of every 5 ms, so that interrupts also arrive
 * while a task other than idle is running and while it sits in the kernel. */
static void load_task(void *arg)
{
    static volatile uint32_t sink;

    (void)arg;
    plat_task_enter("load");

    for (;;) {
        const uint64_t until = plat_time_ns() + 2000000ULL;

        while (plat_time_ns() < until) {
            sink++;
        }

        vTaskDelay(pdMS_TO_TICKS(3U));
    }
}

static void irq_stopper_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(s_run_seconds * 1000U));
    atomic_store(&s_source_run, false);

    /* The source may be inside a raise: wait until it has left its loop, so that no raise can
     * come after delivery is switched off, then let the last interrupt be served. */
    while (!atomic_load(&s_source_done)) {
        vTaskDelay(1U);
    }

    vTaskDelay(pdMS_TO_TICKS(50U));
    plat_end_scheduler();

    for (;;) {
    }
}

/* Waits until the task under test has started, so that no raise happens before delivery is
 * enabled (a raise latched while delivery is off sends no signal; without the safety net it
 * would never be served). */
static bool wait_for_go(const _Atomic bool *go, const _Atomic bool *run)
{
    while (!atomic_load(go)) {
        if ((run != NULL) && !atomic_load(run)) {
            return false;
        }

        sleep_ns(100000ULL);
    }

    return true;
}

/*
 * The "peripheral": a thread outside the scheduler that raises the line.
 *
 * With a period it raises at absolute deadlines whose spacing is jittered by +-17 %, so that
 * the mean rate is the nominal one and raises are never phase-locked to the tick. With period
 * 0 it raises in a tight loop, millions of times per second: an interrupt storm, which is
 * what shows whether handlers can pile up on a task's stack.
 */
static void *irq_source(void *arg)
{
    uint32_t lfsr = 0xACE1U;
    uint64_t deadline_ns;

    (void)arg;
    posix_external_thread_setup("irq-src");
    (void)prctl(PR_SET_TIMERSLACK, 1UL); /* wake at the deadline, not up to 50 us after it */

    if (!wait_for_go(&s_irq_go, &s_source_run)) {
        atomic_store(&s_source_done, true);
        return NULL;
    }

    deadline_ns = plat_time_ns();

    while (atomic_load(&s_source_run)) {
        if (s_irq_period_us != 0U) {
            lfsr = (lfsr >> 1U) ^ ((0U - (lfsr & 1U)) & 0xB400U);

            /* period * (0.83 .. 1.17) */
            deadline_ns += ((uint64_t)s_irq_period_us * 833ULL) +
                           (((uint64_t)(lfsr % 1000U) * (uint64_t)s_irq_period_us) / 3ULL);

            const struct timespec deadline = {(time_t)(deadline_ns / 1000000000ULL),
                                              (long)(deadline_ns % 1000000000ULL)};

            (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        }

        const uint64_t now_ns = plat_time_ns();
        const uint32_t index = atomic_load(&s_raises_started);

        if (index == 0U) {
            s_source_first_ns = now_ns;
        }

        s_source_last_ns = now_ns;

        if (index < IRQ_MAX_SAMPLES) {
            s_raise_ns[index] = now_ns;
        }

        atomic_store(&s_raises_started, index + 1U);
        posix_irq_raise(IRQ_TEST_LINE);
        atomic_store(&s_raises_published, index + 1U);
    }

    atomic_store(&s_source_done, true);
    return NULL;
}

/*
 * What the latency samples must show; s_latency_ns has to be sorted.
 *
 * In every run each sample belongs to a raise that an ISR had noted and that was timestamped
 * before that ISR ran (see the top of this section). Anything else is a sample taken at the
 * wrong moment, which no host can cause: failure.
 *
 * In a run with a period (a storm tests survival, not latency):
 *   - at most IRQ_NOT_RECORDED_MAX raises without a sample, the last one (see above). More
 *     means raises were skipped: failure.
 *   - poll mode: the median below IRQ_POLL_MEDIAN_MAX_TICK tick periods. A raise waits for
 *     the next tick, at a random phase to it, so the median is about half a tick. Figures of
 *     the development laptop (WSL2; debug, asan and release): 556 to 584 us idle (15 runs);
 *     with the process on 2 CPUs shared with 2 busy loops at most 828 us (45 runs, the highest
 *     for release, 806 us for asan). Crediting every raise of a wake-up with the wait of its
 *     oldest raise gave 932 to 1870 us; the bound, 880 us, lies about midway between 828 and
 *     932. A poll that serves only every second tick gave 968 to 1484 us, except one run of
 *     796 us under the contention above. A host that delays the tick raises the median as
 *     well, so a miss is TIMING_MISSED.
 *
 * There is no lower bound on a single sample. A floor was tried and measured: the unmodified
 * code reached 1.5 us in poll mode under the contention above, while sampling published raises
 * that no ISR had noted gave 0.2 to 3.0 us, so no floor separates the two. A short sample can
 * be legitimate: a raise just before a tick, served by that tick while the task is still busy
 * with the previous wake-up, is sampled without a task switch. What a floor was meant to
 * catch, a sample taken before an ISR noted the raise, is checked directly
 * (latency_samples_unnoted), and that check failed every run of that mutant.
 */
static int check_latency(irq_test_mode_t mode, uint32_t raised, uint32_t samples)
{
    const double tick_ns = 1e9 / (double)configTICK_RATE_HZ;
    int status = EXIT_SUCCESS;

    const uint32_t sampleable = (raised < IRQ_MAX_SAMPLES) ? raised : IRQ_MAX_SAMPLES;

    if ((s_samples_unnoted != 0U) || (s_samples_stamped_late != 0U) ||
        (s_samples_too_short != 0U) || (s_samples_late != 0U)) {
        (void)fprintf(stderr,
                      "latency sampled at the wrong moment: %u samples for raises that no ISR "
                      "had noted, %u for raises timestamped after the ISR that noted them, %u "
                      "no longer than raise to ISR, %u not taken at the first wake-up at which "
                      "the raise was ready\n",
                      (unsigned int)s_samples_unnoted, (unsigned int)s_samples_stamped_late,
                      (unsigned int)s_samples_too_short, (unsigned int)s_samples_late);
        status = EXIT_FAILURE;
    }

    if (s_irq_period_us != 0U) {
        if ((sampleable - samples) > IRQ_NOT_RECORDED_MAX) {
            (void)fprintf(stderr, "%u of %u raises have no latency sample (at most %u may)\n",
                          (unsigned int)(sampleable - samples), (unsigned int)sampleable,
                          (unsigned int)IRQ_NOT_RECORDED_MAX);
            status = EXIT_FAILURE;
        }

        if (mode == MODE_POLL) {
            const uint64_t median_ns = s_latency_ns[samples / 2U];

            if ((double)median_ns > (IRQ_POLL_MEDIAN_MAX_TICK * tick_ns)) {
                (void)fprintf(stderr,
                              "poll mode: median latency %.1f us, more than %.0f us (%.2f tick "
                              "periods): the tick hook served late or the samples are wrong\n",
                              (double)median_ns / 1e3, IRQ_POLL_MEDIAN_MAX_TICK * tick_ns / 1e3,
                              IRQ_POLL_MEDIAN_MAX_TICK);
                status = worse(status, TIMING_MISSED);
            }
        }
    }

    return status;
}

static int cmd_irq(irq_test_mode_t mode, bool with_load)
{
    plat_stack_info_t stacks[8];
    posix_irq_stats_t irq;
    posix_tick_stats_t tick;
    pthread_t source;
    uint32_t stack_peak = 0U;
    int status = EXIT_SUCCESS;

    init_irq_platform(mode);
    posix_irq_attach(IRQ_TEST_LINE, test_isr, NULL);
    create_task(rx_task, "rx", 5U, &s_rx_task);
    create_task(irq_stopper_task, "stopper", 6U, NULL);

    if (with_load) {
        create_task(load_task, "load", 2U, NULL);
    }

    atomic_store(&s_source_run, true);

    if (pthread_create(&source, NULL, irq_source, NULL) != 0) {
        return EXIT_FAILURE;
    }

    posix_platform_run();
    atomic_store(&s_source_run, false);
    (void)pthread_join(source, NULL);
    posix_platform_shutdown();
    posix_irq_get_stats(&irq);
    posix_tick_get_stats(&tick);

    const size_t stack_count = plat_stack_report(stacks, sizeof(stacks) / sizeof(stacks[0]));

    for (size_t i = 0U; i < stack_count; i++) {
        if (stacks[i].peak_bytes > stack_peak) {
            stack_peak = stacks[i].peak_bytes;
        }
    }

    const uint32_t raised = atomic_load(&s_raises_started);
    const uint32_t seen = atomic_load(&s_task_seen);
    const uint32_t samples = s_latency_count;
    const double mean_period_us =
        (raised > 1U)
            ? ((double)(s_source_last_ns - s_source_first_ns) / (double)(raised - 1U) / 1e3)
            : 0.0;

    /* "raised" counts calls of the source; a line that is raised again before it was served
     * stays one pending interrupt, as with a real interrupt flag, so the task wakes up less
     * often than the source raises once the source is faster than the service.
     * "unserved_at_end" must be zero: an ISR noted the last raise and the task ran after it.
     * So must "lines_latched_at_end": an ISR can note a raise whose line is not latched yet
     * (see the top of this section), so only an empty latch shows that it was served. */
    print_report_head("irq");
    (void)printf("\"mode\": \"%s\", \"nominal_period_us\": %u, "
                 "\"mean_raise_period_us\": %.3f, \"seconds\": %u, \"load\": %s, "
                 "\"raised\": %u, \"task_wakeups\": %u, \"unserved_at_end\": %u, "
                 "\"signals_sent\": %llu, \"serviced_by_signal\": %llu, "
                 "\"serviced_by_tick\": %llu, \"max_handler_nesting\": %u, "
                 "\"stack_peak_max_bytes\": %u, \"tick_catch_ups\": %llu, "
                 "\"tick_dropped\": %llu, \"lines_latched_at_end\": %u, "
                 "\"latency_samples_not_recorded\": %u, \"latency_samples_unnoted\": %u, "
                 "\"latency_samples_stamped_after_isr\": %u, \"latency_samples_too_short\": %u, "
                 "\"latency_samples_late\": %u, ",
                 mode_name(mode), (unsigned int)s_irq_period_us, mean_period_us,
                 (unsigned int)s_run_seconds, with_load ? "true" : "false", (unsigned int)raised,
                 (unsigned int)s_task_wakeups, (unsigned int)(raised - seen),
                 (unsigned long long)irq.signals_sent, (unsigned long long)irq.serviced_by_signal,
                 (unsigned long long)irq.serviced_by_tick, (unsigned int)irq.max_nesting,
                 (unsigned int)stack_peak, (unsigned long long)tick.catch_ups,
                 (unsigned long long)tick.dropped_ticks, (unsigned int)irq.pending,
                 (unsigned int)(raised - samples), (unsigned int)s_samples_unnoted,
                 (unsigned int)s_samples_stamped_late, (unsigned int)s_samples_too_short,
                 (unsigned int)s_samples_late);
    dump_samples(s_latency_ns, samples);
    print_stats_us("latency_per_interrupt_us", s_latency_ns, samples); /* sorts the samples */
    (void)printf("}\n");

    /* The source stopped before delivery was switched off, and everything it raised has had
     * 50 ms to be served (irq_stopper_task). */
    if ((seen != raised) || (irq.pending != 0U)) {
        (void)fprintf(stderr,
                      "an interrupt was never served: raised %u, task saw %u, lines still "
                      "latched 0x%x\n",
                      (unsigned int)raised, (unsigned int)seen, (unsigned int)irq.pending);
        status = EXIT_FAILURE;
    }

    if (samples == 0U) {
        (void)fprintf(stderr, "no interrupt reached the task\n");
        status = EXIT_FAILURE;
    } else {
        status = worse(status, check_latency(mode, raised, samples));
    }

    if (irq.max_nesting > 1U) {
        (void)fprintf(stderr, "interrupt handlers nested %u deep\n", (unsigned int)irq.max_nesting);
        status = EXIT_FAILURE;
    }

    if (stack_peak > IRQ_STACK_LIMIT_BYTES) {
        (void)fprintf(stderr, "a task used %u bytes of stack, more than %u\n",
                      (unsigned int)stack_peak, (unsigned int)IRQ_STACK_LIMIT_BYTES);
        status = EXIT_FAILURE;
    }

    return worse(status, check_delivery_path(mode, &irq));
}

/* --------------------------------------------------------------------------- irq-rounds */

/*
 * Ping-pong between a peripheral thread and a task: raise the line once, wait until the task
 * has answered, wait a random time (0 to 1.5 tick periods, so the raise lands at every phase
 * of the tick), and again. A free-running source cannot show a lost interrupt, because the
 * next raise wakes the task anyway; here a lost one leaves the peripheral waiting.
 *
 * After ROUNDS_TIMEOUT_NS without an answer the round is classified. If the line is still
 * latched, or the ISR has run and only the task has not, the interrupt is late, not lost: the
 * host did not run the process (TIMING_MISSED). If neither, the interrupt was lost (failure).
 *
 * The test also checks that the task runs at once: with signal delivery the ISR's request for
 * a context switch (portYIELD_FROM_ISR) switches to the task inside the same tick. Without
 * that request the task only runs at the next switch for another reason, mostly the next
 * tick. The share of rounds answered in the tick in which the ISR ran must be at least
 * ROUNDS_SAME_TICK_MIN. On the development laptop the unmodified code answered at least 88.5 %
 * in the ISR's tick with the process on 2 CPUs shared with 2 busy loops (debug and asan, every
 * mode; usually above 96 %) and at least 90.2 % with 4 busy loops. Without portYIELD_FROM_ISR
 * it answered at most 25.5 % (signal) and 15.1 % (signal-only); dropping every 4th request
 * gave 72 to 79.5 %, every 3rd about 66 %. The bound, 85 %, fails all of those and is below
 * every unmodified figure measured; under heavier contention than that a correct build can
 * miss it, which is a timing miss and repeated once.
 *
 * How a miss is reported depends on how far below the bound the share is. A host that runs
 * the process badly delays some answers past a tick boundary, which lowers the share a little;
 * between ROUNDS_SAME_TICK_DEFECT and ROUNDS_SAME_TICK_MIN that is a timing miss
 * (TIMING_MISSED, repeated once). Below ROUNDS_SAME_TICK_DEFECT the context switch is missing,
 * which is a defect (failure, not repeated): no contention measured here took the unmodified
 * code near it.
 */
#define ROUNDS_TIMEOUT_NS       2000000000ULL
#define ROUNDS_POLL_NS          20000ULL
#define ROUNDS_MAX_DELAY_US     1500U
#define ROUNDS_SAME_TICK_MIN    0.85
#define ROUNDS_SAME_TICK_DEFECT 0.40

enum { ROUND_OK = 0, ROUND_LATE = 1, ROUND_LOST = 2 };

static TaskHandle_t s_rounds_task;
static uint32_t s_rounds = 3000U;
static _Atomic bool s_rounds_go;
static _Atomic bool s_rounds_done;
static _Atomic uint32_t s_rounds_isr_calls;
static _Atomic uint64_t s_rounds_isr_tick;
static _Atomic uint32_t s_rounds_answered;
static _Atomic uint32_t s_rounds_same_tick;
static uint32_t s_rounds_completed;
static int s_rounds_outcome = ROUND_OK;

static void rounds_isr(void *context, BaseType_t *woken)
{
    (void)context;
    atomic_store(&s_rounds_isr_tick, (uint64_t)xTaskGetTickCountFromISR());
    (void)atomic_fetch_add(&s_rounds_isr_calls, 1U);
    vTaskNotifyGiveFromISR(s_rounds_task, woken);
}

static void rounds_task(void *arg)
{
    (void)arg;
    plat_task_enter("rounds");
    atomic_store(&s_rounds_go, true);

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if ((uint64_t)xTaskGetTickCount() == atomic_load(&s_rounds_isr_tick)) {
            (void)atomic_fetch_add(&s_rounds_same_tick, 1U);
        }

        (void)atomic_fetch_add(&s_rounds_answered, 1U);
    }
}

static void rounds_stopper_task(void *arg)
{
    (void)arg;

    while (!atomic_load(&s_rounds_done)) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }

    plat_end_scheduler();

    for (;;) {
    }
}

/* Waits for the task's answer to round `round` (0-based); classifies a round that timed out. */
static int wait_for_answer(uint32_t round, uint32_t isr_calls_before)
{
    const uint64_t deadline_ns = plat_time_ns() + ROUNDS_TIMEOUT_NS;
    int outcome = ROUND_OK;

    while (atomic_load(&s_rounds_answered) <= round) {
        if (plat_time_ns() > deadline_ns) {
            posix_irq_stats_t irq;

            posix_irq_get_stats(&irq);
            const bool latched = ((irq.pending & (1UL << IRQ_TEST_LINE)) != 0U);
            const bool isr_ran = (atomic_load(&s_rounds_isr_calls) != isr_calls_before);

            outcome = (latched || isr_ran) ? ROUND_LATE : ROUND_LOST;
            break;
        }

        sleep_ns(ROUNDS_POLL_NS);
    }

    return outcome;
}

static void *rounds_source(void *arg)
{
    uint32_t lfsr = 0x1D872U;

    (void)arg;
    posix_external_thread_setup("irq-rounds");
    (void)prctl(PR_SET_TIMERSLACK, 1UL);
    (void)wait_for_go(&s_rounds_go, NULL);

    for (uint32_t round = 0U; round < s_rounds; round++) {
        lfsr = (lfsr >> 1U) ^ ((0U - (lfsr & 1U)) & 0xB400U);
        sleep_ns((uint64_t)(lfsr % ROUNDS_MAX_DELAY_US) * 1000ULL);

        const uint32_t isr_calls_before = atomic_load(&s_rounds_isr_calls);

        posix_irq_raise(IRQ_TEST_LINE);
        s_rounds_outcome = wait_for_answer(round, isr_calls_before);

        if (s_rounds_outcome != ROUND_OK) {
            break;
        }

        s_rounds_completed = round + 1U;
    }

    atomic_store(&s_rounds_done, true);
    return NULL;
}

static const char *outcome_name(int outcome)
{
    const char *name = "lost";

    if (outcome == ROUND_OK) {
        name = "ok";
    } else if (outcome == ROUND_LATE) {
        name = "late";
    } else {
        /* lost */
    }

    return name;
}

static int cmd_irq_rounds(irq_test_mode_t mode)
{
    posix_irq_stats_t irq;
    pthread_t source;
    int status = EXIT_SUCCESS;

    init_irq_platform(mode);
    posix_irq_attach(IRQ_TEST_LINE, rounds_isr, NULL);
    create_task(rounds_task, "rounds", 5U, &s_rounds_task);
    create_task(rounds_stopper_task, "stopper", 6U, NULL);
    create_task(load_task, "load", 2U, NULL);

    if (pthread_create(&source, NULL, rounds_source, NULL) != 0) {
        return EXIT_FAILURE;
    }

    posix_platform_run();
    (void)pthread_join(source, NULL);
    posix_platform_shutdown();
    posix_irq_get_stats(&irq);

    const uint32_t completed = s_rounds_completed;
    const uint32_t isr_calls = atomic_load(&s_rounds_isr_calls);
    const uint32_t same_tick = atomic_load(&s_rounds_same_tick);
    const double same_share = (completed > 0U) ? ((double)same_tick / (double)completed) : 0.0;
    const uint64_t passes = irq.serviced_by_signal + irq.serviced_by_tick;

    print_report_head("irq-rounds");
    (void)printf("\"mode\": \"%s\", \"rounds\": %u, \"completed\": %u, \"outcome\": \"%s\", "
                 "\"isr_calls\": %u, \"answered_in_the_isr_tick\": %u, "
                 "\"answered_in_the_isr_tick_percent\": %.1f, \"signals_sent\": %llu, "
                 "\"serviced_by_signal\": %llu, \"serviced_by_tick\": %llu}\n",
                 mode_name(mode), (unsigned int)s_rounds, (unsigned int)completed,
                 outcome_name(s_rounds_outcome), (unsigned int)isr_calls, (unsigned int)same_tick,
                 same_share * 100.0, (unsigned long long)irq.signals_sent,
                 (unsigned long long)irq.serviced_by_signal,
                 (unsigned long long)irq.serviced_by_tick);

    if (s_rounds_outcome == ROUND_LOST) {
        (void)fprintf(stderr,
                      "round %u: the interrupt was lost (not latched, ISR not run, no answer "
                      "within %llu ms)\n",
                      (unsigned int)(completed + 1U),
                      (unsigned long long)(ROUNDS_TIMEOUT_NS / 1000000ULL));
        status = EXIT_FAILURE;
    } else if (s_rounds_outcome == ROUND_LATE) {
        (void)fprintf(stderr,
                      "round %u: no answer within %llu ms although the interrupt was "
                      "latched or served\n",
                      (unsigned int)(completed + 1U),
                      (unsigned long long)(ROUNDS_TIMEOUT_NS / 1000000ULL));
        status = TIMING_MISSED;
    } else {
        /* every round answered */
    }

    /* One raise per round, each into an empty latch: exactly one ISR call and one dispatch
     * pass per round, and with signal delivery exactly one signal. */
    if ((s_rounds_outcome == ROUND_OK) &&
        ((isr_calls != completed) || (passes != completed) ||
         ((mode != MODE_POLL) && (irq.signals_sent != completed)))) {
        (void)fprintf(stderr, "%u rounds, but %u ISR calls, %llu dispatch passes, %llu signals\n",
                      (unsigned int)completed, (unsigned int)isr_calls, (unsigned long long)passes,
                      (unsigned long long)irq.signals_sent);
        status = EXIT_FAILURE;
    }

    if ((mode != MODE_POLL) && (completed > 0U) && (same_share < ROUNDS_SAME_TICK_MIN)) {
        const bool defect = (same_share < ROUNDS_SAME_TICK_DEFECT);

        (void)fprintf(stderr,
                      "only %.1f %% of the rounds were answered in the tick in which the ISR "
                      "ran (at least %.0f %% expected): %s\n",
                      same_share * 100.0, ROUNDS_SAME_TICK_MIN * 100.0,
                      defect ? "the ISR's context switch is missing (below "
                               "ROUNDS_SAME_TICK_DEFECT, not a timing miss)"
                             : "the host delayed the task (a timing miss)");
        status = worse(status, defect ? EXIT_FAILURE : TIMING_MISSED);
    }

    return worse(status, check_delivery_path(mode, &irq));
}

/* ---------------------------------------------------------------------------- irq-lines */

/*
 * Three lines (0, 3 and the last one) with an ISR each and a context of their own. An
 * external thread raises them one at a time and all three at once, then a line without an
 * ISR and two lines that do not exist. Each step is checked for the ISR calls it caused, the
 * context each ISR received, the raises counted, what is still latched afterwards and the
 * notifications the ISRs gave the driving task, which waits for them.
 *
 * "At once" has to mean latched together before any dispatch runs. The driving task makes
 * sure of that: it holds a critical section (signals blocked, the tick included) while the
 * external thread raises the three lines, so the dispatch pass that follows finds all three.
 */
#define LINES_ISR_COUNT 3U
#define LINES_WAIT_MS   1000U
#define LINES_UNUSED    5U  /* a line inside the range without an ISR */
#define LINES_FAR       31U /* a line far outside the range */

typedef struct {
    unsigned int line;
    _Atomic uint32_t calls;           /* this ISR ran with its own context */
    _Atomic uint32_t foreign_context; /* this ISR ran with a context that is not its own */
} line_context_t;

static line_context_t s_line_ctx[LINES_ISR_COUNT] = {
    {.line = 0U}, {.line = 3U}, {.line = POSIX_IRQ_LINES - 1U}};

typedef struct {
    const char *what;
    uint32_t lines; /* bit n: raise line n */
    bool together;
    uint32_t calls[LINES_ISR_COUNT];
    uint32_t raised;
    uint32_t passes;
} lines_step_t;

static const lines_step_t k_lines_steps[] = {
    {"line 0 alone", 1UL << 0U, false, {1U, 0U, 0U}, 1U, 1U},
    {"line 3 alone", 1UL << 3U, false, {0U, 1U, 0U}, 1U, 1U},
    {"last line alone", 1UL << (POSIX_IRQ_LINES - 1U), false, {0U, 0U, 1U}, 1U, 1U},
    {"three lines together",
     (1UL << 0U) | (1UL << 3U) | (1UL << (POSIX_IRQ_LINES - 1U)),
     true,
     {1U, 1U, 1U},
     3U,
     1U},
    {"line without an ISR", 1UL << LINES_UNUSED, false, {0U, 0U, 0U}, 1U, 1U},
    {"lines out of range",
     (1UL << POSIX_IRQ_LINES) | (1UL << LINES_FAR),
     false,
     {0U, 0U, 0U},
     0U,
     0U},
};

#define LINES_STEPS (sizeof(k_lines_steps) / sizeof(k_lines_steps[0]))

typedef struct {
    uint32_t calls[LINES_ISR_COUNT];
    uint32_t foreign;
    uint64_t raised;
    uint64_t passes;
    uint32_t pending_after;
    uint32_t notified; /* notifications the ISRs gave the driving task */
} lines_result_t;

static lines_result_t s_lines_result[LINES_STEPS];
static _Atomic uint32_t s_lines_command;  /* lines to raise */
static _Atomic uint32_t s_lines_posted;   /* sequence number of the last command */
static _Atomic uint32_t s_lines_executed; /* sequence number of the last command carried out */
static _Atomic bool s_lines_run;

static TaskHandle_t s_lines_driver;

/* What every line's ISR does: count the call in the context it was given if that is its own,
 * else count a foreign context against itself, and notify the driving task the way a real ISR
 * hands an event to a task. The test counts the notifications; whether the context-switch
 * request in `woken` takes effect at once is what irq-rounds checks, not this test. */
static void line_isr_common(size_t index, void *context, BaseType_t *woken)
{
    line_context_t *got = context;
    line_context_t *own = &s_line_ctx[index];

    if (got == own) {
        (void)atomic_fetch_add(&got->calls, 1U);
    } else {
        (void)atomic_fetch_add(&own->foreign_context, 1U);
    }

    vTaskNotifyGiveFromISR(s_lines_driver, woken);
}

static void line_isr_a(void *context, BaseType_t *woken)
{
    line_isr_common(0U, context, woken);
}

static void line_isr_b(void *context, BaseType_t *woken)
{
    line_isr_common(1U, context, woken);
}

static void line_isr_c(void *context, BaseType_t *woken)
{
    line_isr_common(2U, context, woken);
}

static void *lines_peripheral(void *arg)
{
    uint32_t done = 0U;

    (void)arg;
    posix_external_thread_setup("irq-lines");

    while (atomic_load(&s_lines_run)) {
        const uint32_t posted = atomic_load(&s_lines_posted);

        if (posted == done) {
            sleep_ns(50000ULL);
            continue;
        }

        const uint32_t lines = atomic_load(&s_lines_command);

        for (unsigned int line = 0U; line < 32U; line++) {
            if ((lines & (1UL << line)) != 0U) {
                posix_irq_raise(line);
            }
        }

        done = posted;
        atomic_store(&s_lines_executed, done);
    }

    return NULL;
}

static void lines_run_step(const lines_step_t *step, lines_result_t *result)
{
    posix_irq_stats_t before;
    posix_irq_stats_t after;
    uint32_t calls_before[LINES_ISR_COUNT];
    uint32_t foreign_before = 0U;
    uint32_t expected_calls = 0U;

    posix_irq_get_stats(&before);

    for (size_t i = 0U; i < LINES_ISR_COUNT; i++) {
        calls_before[i] = atomic_load(&s_line_ctx[i].calls);
        foreign_before += atomic_load(&s_line_ctx[i].foreign_context);
        expected_calls += step->calls[i];
    }

    const uint32_t sequence = atomic_load(&s_lines_posted) + 1U;
    uint32_t notified = 0U;

    atomic_store(&s_lines_command, step->lines);

    if (step->together) {
        /* No dispatch can run while this task holds the critical section: the signal and
         * the tick stay pending until it ends. The external thread is not held by it. */
        taskENTER_CRITICAL();
        atomic_store(&s_lines_posted, sequence);

        while (atomic_load(&s_lines_executed) != sequence) {
        }

        taskEXIT_CRITICAL();
    } else {
        atomic_store(&s_lines_posted, sequence);

        while (atomic_load(&s_lines_executed) != sequence) {
            vTaskDelay(1U);
        }
    }

    /* Wait for the ISR calls this step should cause: each one notifies this task. Then a few
     * ticks more for any that it should not, and for a tick-hook poll to have run in poll
     * mode; notifications that arrive meanwhile are counted too. */
    while (notified < expected_calls) {
        const uint32_t taken = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LINES_WAIT_MS));

        if (taken == 0U) {
            break;
        }

        notified += taken;
    }

    vTaskDelay(pdMS_TO_TICKS(5U));
    notified += ulTaskNotifyTake(pdTRUE, 0U);
    result->notified = notified;
    posix_irq_get_stats(&after);

    uint32_t foreign_after = 0U;

    for (size_t i = 0U; i < LINES_ISR_COUNT; i++) {
        result->calls[i] = atomic_load(&s_line_ctx[i].calls) - calls_before[i];
        foreign_after += atomic_load(&s_line_ctx[i].foreign_context);
    }

    result->foreign = foreign_after - foreign_before;
    result->raised = after.raised - before.raised;
    result->passes = (after.serviced_by_signal + after.serviced_by_tick) -
                     (before.serviced_by_signal + before.serviced_by_tick);
    result->pending_after = after.pending;
}

static void lines_driver_task(void *arg)
{
    (void)arg;
    plat_task_enter("lines");

    for (size_t i = 0U; i < LINES_STEPS; i++) {
        lines_run_step(&k_lines_steps[i], &s_lines_result[i]);
    }

    plat_end_scheduler();

    for (;;) {
    }
}

static int cmd_irq_lines(irq_test_mode_t mode)
{
    static const posix_isr_fn isrs[LINES_ISR_COUNT] = {line_isr_a, line_isr_b, line_isr_c};
    pthread_t peripheral;
    int status = EXIT_SUCCESS;

    init_irq_platform(mode);

    for (size_t i = 0U; i < LINES_ISR_COUNT; i++) {
        posix_irq_attach(s_line_ctx[i].line, isrs[i], &s_line_ctx[i]);
    }

    create_task(lines_driver_task, "lines", 4U, &s_lines_driver);
    atomic_store(&s_lines_run, true);

    if (pthread_create(&peripheral, NULL, lines_peripheral, NULL) != 0) {
        return EXIT_FAILURE;
    }

    posix_platform_run();
    atomic_store(&s_lines_run, false);
    (void)pthread_join(peripheral, NULL);
    posix_platform_shutdown();

    print_report_head("irq-lines");
    (void)printf("\"mode\": \"%s\", \"steps\": [", mode_name(mode));

    for (size_t i = 0U; i < LINES_STEPS; i++) {
        const lines_step_t *step = &k_lines_steps[i];
        const lines_result_t *got = &s_lines_result[i];
        uint32_t expected_calls = 0U;
        bool ok = (got->foreign == 0U) && (got->raised == step->raised) &&
                  (got->passes == step->passes) && (got->pending_after == 0U);

        for (size_t j = 0U; j < LINES_ISR_COUNT; j++) {
            ok = ok && (got->calls[j] == step->calls[j]);
            expected_calls += step->calls[j];
        }

        /* One notification per ISR call: each ISR call notified the task once. */
        ok = ok && (got->notified == expected_calls);

        (void)printf("%s{\"step\": \"%s\", \"calls\": [%u, %u, %u], \"foreign_context\": %u, "
                     "\"raised\": %llu, \"dispatch_passes\": %llu, \"pending_after\": %u, "
                     "\"task_notified\": %u, \"ok\": %s}",
                     (i == 0U) ? "" : ", ", step->what, (unsigned int)got->calls[0],
                     (unsigned int)got->calls[1], (unsigned int)got->calls[2],
                     (unsigned int)got->foreign, (unsigned long long)got->raised,
                     (unsigned long long)got->passes, (unsigned int)got->pending_after,
                     (unsigned int)got->notified, ok ? "true" : "false");

        if (!ok) {
            (void)fprintf(stderr,
                          "%s mode, %s: ISR calls [%u, %u, %u] (expected [%u, %u, %u]), %u "
                          "with a foreign context, %llu raises counted (expected %u), %llu "
                          "dispatch passes (expected %u), lines still latched 0x%x, task "
                          "notified %u times (expected %u)\n",
                          mode_name(mode), step->what, (unsigned int)got->calls[0],
                          (unsigned int)got->calls[1], (unsigned int)got->calls[2],
                          (unsigned int)step->calls[0], (unsigned int)step->calls[1],
                          (unsigned int)step->calls[2], (unsigned int)got->foreign,
                          (unsigned long long)got->raised, (unsigned int)step->raised,
                          (unsigned long long)got->passes, (unsigned int)step->passes,
                          (unsigned int)got->pending_after, (unsigned int)got->notified,
                          (unsigned int)expected_calls);
            status = EXIT_FAILURE;
        }
    }

    (void)printf("]}\n");
    return status;
}

/* -------------------------------------------------------------------------------- stack */

#define BURN_LEVELS      24U
#define BURN_BLOCK_BYTES 1024U

static volatile uint32_t s_burn_result;

/* Uses about BURN_BLOCK_BYTES of stack per level. Not instrumented: AddressSanitizer would
 * move the array to its fake stack and the real stack would stay shallow. */
/* NOLINTNEXTLINE(misc-no-recursion): recursion is the point, it is what uses the stack */
__attribute__((noinline, no_sanitize("address"))) static uint32_t burn_stack(uint32_t levels)
{
    volatile uint8_t block[BURN_BLOCK_BYTES];
    uint32_t sum = 0U;

    for (uint32_t i = 0U; i < BURN_BLOCK_BYTES; i++) {
        block[i] = (uint8_t)(i + levels);
    }

    if (levels > 1U) {
        sum = burn_stack(levels - 1U);
    }

    return sum + block[levels % BURN_BLOCK_BYTES];
}

static void deep_task(void *arg)
{
    (void)arg;
    plat_task_enter("deep");
    s_burn_result = burn_stack(BURN_LEVELS);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

static void shallow_task(void *arg)
{
    (void)arg;
    plat_task_enter("shallow");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

/* A task that keeps a large buffer in its own body, the usual firmware pattern. The task
 * function allocates its whole frame, the buffer with it, before its first statement, so the
 * buffer lies above the frame of plat_task_enter(): a depth counted from there leaves it out.
 * Not instrumented: AddressSanitizer would move the buffer to its fake stack. The stack depth
 * it is created with differs from the other tasks' so that the report has to give each task
 * its own; on the host it has nothing to do with the buffer (the code runs on a pthread
 * stack). */
#define BODY_BUFFER_BYTES     (16U * 1024U)
#define BODY_TASK_STACK_WORDS ((size_t)3U * (size_t)configMINIMAL_STACK_SIZE)

static volatile uint32_t s_body_result;
static StackType_t s_body_stack[BODY_TASK_STACK_WORDS]; /* static: the kernel heap is small */
static StaticTask_t s_body_tcb;

__attribute__((no_sanitize("address"))) static void body_buffer_task(void *arg)
{
    volatile uint8_t buffer[BODY_BUFFER_BYTES];

    (void)arg;
    plat_task_enter("body");

    for (uint32_t i = 0U; i < BODY_BUFFER_BYTES; i++) {
        buffer[i] = (uint8_t)i;
    }

    s_body_result = buffer[BODY_BUFFER_BYTES - 1U];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

/* EXIT_FAILURE unless the report gives the task the stack it was created with. */
static int check_configured(const plat_stack_info_t *info, size_t words)
{
    const uint32_t expected = (uint32_t)(words * sizeof(StackType_t));
    int status = EXIT_SUCCESS;

    if (info->configured_bytes != expected) {
        (void)fprintf(stderr, "task %s: configured stack reported as %u bytes, created with %u\n",
                      info->name, (unsigned int)info->configured_bytes, (unsigned int)expected);
        status = EXIT_FAILURE;
    }

    return status;
}

static int cmd_stack(void)
{
    plat_stack_info_t info[8];
    uint32_t deep = 0U;
    uint32_t shallow = 0U;
    uint32_t body = 0U;
    uint32_t starts[3] = {0U, 0U, 0U}; /* start_bytes of deep, shallow, body */
    bool idle_seen = false;
    bool dead_stack_read = false;
    int status = EXIT_SUCCESS;

    init_platform(POSIX_TICK_LOCKED, POSIX_IRQ_SIGNAL, NULL);
    create_task(deep_task, "deep", 3U, NULL);
    create_task(shallow_task, "shallow", 3U, NULL);
    (void)xTaskCreateStatic(body_buffer_task, "body", BODY_TASK_STACK_WORDS, NULL, 3U, s_body_stack,
                            &s_body_tcb);
    create_task(stopper_task, "stopper", 5U, NULL);
    posix_platform_run();
    posix_platform_shutdown();

    const size_t count = plat_stack_report(info, sizeof(info) / sizeof(info[0]));

    print_report_head("stack");
    (void)printf("\"burned_bytes\": %u, \"body_buffer_bytes\": %u, \"tasks\": [",
                 BURN_LEVELS * BURN_BLOCK_BYTES, BODY_BUFFER_BYTES);

    for (size_t i = 0U; i < count; i++) {
        (void)printf("%s{\"name\": \"%s\", \"peak_bytes\": %u, \"start_bytes\": %u, "
                     "\"size_bytes\": %u, \"configured_bytes\": %u}",
                     (i == 0U) ? "" : ", ", info[i].name, (unsigned int)info[i].peak_bytes,
                     (unsigned int)info[i].start_bytes, (unsigned int)info[i].size_bytes,
                     (unsigned int)info[i].configured_bytes);

        if (strcmp(info[i].name, "deep") == 0) {
            deep = info[i].peak_bytes;
            starts[0] = info[i].start_bytes;
            status = worse(status, check_configured(&info[i], TASK_STACK_WORDS));
        } else if (strcmp(info[i].name, "shallow") == 0) {
            shallow = info[i].peak_bytes;
            starts[1] = info[i].start_bytes;
            status = worse(status, check_configured(&info[i], TASK_STACK_WORDS));
        } else if (strcmp(info[i].name, "body") == 0) {
            body = info[i].peak_bytes;
            starts[2] = info[i].start_bytes;
            status = worse(status, check_configured(&info[i], BODY_TASK_STACK_WORDS));
        } else if (strcmp(info[i].name, "IDLE") == 0) {
            idle_seen = true;
            /* configKERNEL_PROVIDED_STATIC_MEMORY: the kernel's idle stack has the minimum */
            status = worse(status, check_configured(&info[i], configMINIMAL_STACK_SIZE));
        } else {
            /* any other task is reported but not judged by name */
        }

        /* No task here uses anything like half its stack. A larger figure means the scan read
         * memory that is no longer a live stack: the idle task is deleted when the scheduler
         * ends, and reading its stack after that once reported it as completely used. */
        if (info[i].peak_bytes > (info[i].size_bytes / 2U)) {
            dead_stack_read = true;
        }
    }

    (void)printf("], \"kernel_high_water_mark_is_meaningful\": false}\n");

    /* The deep task must show at least what it deliberately used, and clearly more than the
     * task that used nothing. The slack covers signal frames and sanitizer overhead. The
     * shallow task must have been measured at all: an unregistered task is not in the report
     * and leaves `shallow` at zero (a registered one reports at least the scan's floor); what
     * its own code used must stay small. */
    const uint32_t burned = BURN_LEVELS * BURN_BLOCK_BYTES;

    if ((deep < burned) || (deep > (burned + (48U * 1024U))) || (shallow == 0U) ||
        (shallow > (starts[1] + (16U * 1024U))) || (deep < (shallow + (burned / 2U)))) {
        (void)fprintf(stderr, "stack measurement implausible: deep %u, shallow %u, burned %u\n",
                      (unsigned int)deep, (unsigned int)shallow, (unsigned int)burned);
        status = EXIT_FAILURE;
    }

    /* The buffer in the body task's own frame must be counted: at least its size, and clearly
     * more than the task that uses nothing (half the buffer leaves room for a signal frame
     * that lands on one task and not the other). A depth counted from plat_task_enter()
     * reports this task at the floor, like the shallow one. */
    if ((body < BODY_BUFFER_BYTES) || (body < (shallow + (BODY_BUFFER_BYTES / 2U))) ||
        (body > (shallow + BODY_BUFFER_BYTES + (16U * 1024U)))) {
        (void)fprintf(stderr,
                      "a buffer in the task function's own frame is not measured: body task %u "
                      "bytes, shallow task %u, buffer %u\n",
                      (unsigned int)body, (unsigned int)shallow, (unsigned int)BODY_BUFFER_BYTES);
        status = EXIT_FAILURE;
    }

    /* These three tasks call plat_task_enter() from their task functions, which the port
     * starts the same way, so the part in use before their code ran is the same, not zero,
     * and below the floor of the task that uses nothing. What remains for the body task is
     * its own code's use, the buffer included. */
    if ((starts[0] == 0U) || (starts[0] != starts[1]) || (starts[0] != starts[2]) ||
        (starts[0] >= shallow) || (body < (starts[2] + BODY_BUFFER_BYTES))) {
        (void)fprintf(stderr,
                      "start of the task functions wrong: deep %u, shallow %u, body %u bytes "
                      "(shallow peak %u, body peak %u)\n",
                      (unsigned int)starts[0], (unsigned int)starts[1], (unsigned int)starts[2],
                      (unsigned int)shallow, (unsigned int)body);
        status = EXIT_FAILURE;
    }

    if (!idle_seen || dead_stack_read) {
        (void)fprintf(stderr, "stack report after shutdown is wrong: idle task %s, %s\n",
                      idle_seen ? "reported" : "missing",
                      dead_stack_read ? "a task shows more than half its stack used"
                                      : "depths plausible");
        status = EXIT_FAILURE;
    }

    return status;
}

/* ---------------------------------------------------------------------------------- log */

#define LOG_LINES_PER_PRODUCER 400U
#define LOG_PRODUCERS          3U
#define LOG_PAYLOAD            "abcdefghijklmnopqrstuvwxyz"

/* What the log file holds for one producer. */
typedef struct {
    const char *tag;
    uint32_t kept;         /* lines of this producer in the file */
    uint32_t malformed;    /* lines cut short or with a damaged payload */
    uint32_t out_of_order; /* lines whose sequence number is not above the previous one */
    uint32_t gaps;         /* places where sequence numbers were skipped */
    long last_seq;
} log_producer_t;

/* Reads the log file and fills in one log_producer_t per tag. Lines of other tags (the
 * platform's own) are skipped. Returns false if the file cannot be read. */
static bool scan_log_file(const char *path, log_producer_t *producers, size_t count)
{
    char line[512];
    FILE *file = fopen(path, "r");

    if (file == NULL) {
        (void)fprintf(stderr, "cannot open %s\n", path);
        return false;
    }

    for (size_t i = 0U; i < count; i++) {
        producers[i].kept = 0U;
        producers[i].malformed = 0U;
        producers[i].out_of_order = 0U;
        producers[i].gaps = 0U;
        producers[i].last_seq = -1;
    }

    while (fgets(line, (int)sizeof(line), file) != NULL) {
        char letter = '\0';
        unsigned long seconds = 0UL;
        unsigned long micros = 0UL;
        long seq = -1;
        char tag[16] = {0};
        char tail[64] = {0};

        /* NOLINTNEXTLINE(cert-err34-c): a field that does not convert makes the line not match */
        if (sscanf(line, "[%lu.%lu] %c %15[^:]: seq %ld payload %63s", &seconds, &micros, &letter,
                   tag, &seq, tail) != 6) {
            continue; /* a line of the platform itself, not of a test producer */
        }

        for (size_t i = 0U; i < count; i++) {
            log_producer_t *p = &producers[i];

            if (strcmp(tag, p->tag) == 0) {
                if ((strcmp(tail, LOG_PAYLOAD) != 0) || (line[strlen(line) - 1U] != '\n')) {
                    p->malformed++;
                }

                if (seq <= p->last_seq) {
                    p->out_of_order++;
                } else if (seq != (p->last_seq + 1)) {
                    p->gaps++;
                } else {
                    /* the next line in sequence */
                }

                p->last_seq = seq;
                p->kept++;
            }
        }
    }

    (void)fclose(file);
    return true;
}

static _Atomic uint32_t s_log_producers_done;

static void log_task(void *arg)
{
    const char *tag = arg;

    plat_task_enter(tag);

    for (uint32_t seq = 0U; seq < LOG_LINES_PER_PRODUCER; seq++) {
        LOG_INF(tag, "seq %u payload " LOG_PAYLOAD, (unsigned int)seq);

        if ((seq % 20U) == 19U) {
            vTaskDelay(1U);
        }
    }

    (void)atomic_fetch_add(&s_log_producers_done, 1U);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

static void *log_external(void *arg)
{
    (void)arg;
    posix_external_thread_setup("log-ext");

    for (uint32_t seq = 0U; seq < LOG_LINES_PER_PRODUCER; seq++) {
        LOG_INF("ext", "seq %u payload " LOG_PAYLOAD, (unsigned int)seq);

        if ((seq % 20U) == 19U) {
            sleep_ns(200000ULL);
        }
    }

    (void)atomic_fetch_add(&s_log_producers_done, 1U);
    return NULL;
}

static void log_stopper_task(void *arg)
{
    (void)arg;

    while (atomic_load(&s_log_producers_done) < LOG_PRODUCERS) {
        vTaskDelay(pdMS_TO_TICKS(5U));
    }

    plat_end_scheduler();

    for (;;) {
    }
}

/* Paced producers: every line arrives, whole and in order, and nothing is dropped. */
static int cmd_log(const char *path)
{
    static char tag_a[] = "taskA";
    static char tag_b[] = "taskB";
    log_producer_t producers[LOG_PRODUCERS] = {{.tag = "taskA"}, {.tag = "taskB"}, {.tag = "ext"}};
    pthread_t external;
    bool ok = true;

    init_platform(POSIX_TICK_LOCKED, POSIX_IRQ_SIGNAL, path);
    (void)xTaskCreate(log_task, "taskA", TASK_STACK_WORDS, tag_a, 2U, NULL);
    (void)xTaskCreate(log_task, "taskB", TASK_STACK_WORDS, tag_b, 3U, NULL);
    create_task(log_stopper_task, "stopper", 1U, NULL);

    if (pthread_create(&external, NULL, log_external, NULL) != 0) {
        return EXIT_FAILURE;
    }

    /* As in log-preempt (see there): a producer stuck in the sink ends the run with status 70. */
    posix_shutdown_guard_arm(POSIX_SHUTDOWN_TIMEOUT_MS);
    posix_platform_run();
    (void)pthread_join(external, NULL);
    posix_platform_shutdown();

    if (!scan_log_file(path, producers, LOG_PRODUCERS)) {
        return EXIT_FAILURE;
    }

    const unsigned int dropped = (unsigned int)posix_log_dropped();

    print_report_head("log");
    (void)printf("\"lines\": [%u, %u, %u], \"dropped\": %u}\n", (unsigned int)producers[0].kept,
                 (unsigned int)producers[1].kept, (unsigned int)producers[2].kept, dropped);

    for (size_t i = 0U; i < LOG_PRODUCERS; i++) {
        const log_producer_t *p = &producers[i];

        ok = ok && (p->kept == LOG_LINES_PER_PRODUCER) && (p->malformed == 0U) &&
             (p->out_of_order == 0U) && (p->gaps == 0U);
    }

    if (!ok || (dropped != 0U)) {
        (void)fprintf(stderr, "log check failed\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/* ---------------------------------------------------------------------------- log-preempt */

/*
 * Producers that never pause, so that the tick preempts tasks inside posix_log_sink():
 *   low   a task of low priority that logs in a tight loop: it is almost always in the sink;
 *   high  a task of higher priority that logs once per tick: it preempts `low`;
 *   ext   an external thread that logs in a tight loop: it competes for the producer flag
 *         from another CPU at any moment.
 * A task preempted while it holds the producer flag would make `high` spin on it forever
 * (the critical section in the sink prevents that), and two producers inside the ring at
 * once would damage, lose or repeat lines (the flag prevents that).
 *
 * The ring overflows here by design, so lines are dropped and counted. What is checked: every
 * line in the file is whole, each producer's sequence numbers only rise, and every line a
 * producer wrote is either in the file or counted as dropped. Each producer must have got
 * lines through, or the test did not test it.
 *
 * A deadlock should fail fast: the shutdown guard is armed before the scheduler starts, so a
 * run whose tasks get stuck ends with status 70 after POSIX_SHUTDOWN_TIMEOUT_MS (the guard is
 * re-armed when the scheduler is told to stop). The guard runs in the logger thread, between
 * two drains of the ring. Two producers inside the ring at once can damage it so that the
 * logger thread hangs in the drain; then the guard does not run either, and the test ends at
 * its CTest TIMEOUT (tests/platform/CMakeLists.txt), which kills the process.
 */
#define LOG_PREEMPT_TICKS 500U

static _Atomic bool s_log_stop;
static _Atomic uint32_t s_log_produced[LOG_PRODUCERS];

static void log_flood_task(void *arg)
{
    uint32_t seq = 0U;

    (void)arg;
    plat_task_enter("low");

    while (!atomic_load(&s_log_stop)) {
        LOG_INF("low", "seq %u payload " LOG_PAYLOAD, (unsigned int)seq);
        seq++;
    }

    atomic_store(&s_log_produced[0], seq);
    (void)atomic_fetch_add(&s_log_producers_done, 1U);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

static void log_tick_task(void *arg)
{
    uint32_t seq = 0U;

    (void)arg;
    plat_task_enter("high");

    while (!atomic_load(&s_log_stop)) {
        vTaskDelay(1U);
        LOG_INF("high", "seq %u payload " LOG_PAYLOAD, (unsigned int)seq);
        seq++;
    }

    atomic_store(&s_log_produced[1], seq);
    (void)atomic_fetch_add(&s_log_producers_done, 1U);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

static void *log_flood_external(void *arg)
{
    uint32_t seq = 0U;

    (void)arg;
    posix_external_thread_setup("log-ext");

    while (!atomic_load(&s_log_stop)) {
        LOG_INF("ext", "seq %u payload " LOG_PAYLOAD, (unsigned int)seq);
        seq++;
    }

    atomic_store(&s_log_produced[2], seq);
    (void)atomic_fetch_add(&s_log_producers_done, 1U);
    return NULL;
}

static void log_preempt_stopper_task(void *arg)
{
    (void)arg;
    vTaskDelay(LOG_PREEMPT_TICKS);
    atomic_store(&s_log_stop, true);

    while (atomic_load(&s_log_producers_done) < LOG_PRODUCERS) {
        vTaskDelay(1U);
    }

    plat_end_scheduler();

    for (;;) {
    }
}

static int cmd_log_preempt(const char *path)
{
    log_producer_t producers[LOG_PRODUCERS] = {{.tag = "low"}, {.tag = "high"}, {.tag = "ext"}};
    pthread_t external;
    uint64_t missing = 0U;
    int status = EXIT_SUCCESS;

    init_platform(POSIX_TICK_LOCKED, POSIX_IRQ_SIGNAL, path);
    create_task(log_flood_task, "low", 2U, NULL);
    create_task(log_tick_task, "high", 3U, NULL);
    create_task(log_preempt_stopper_task, "stopper", 4U, NULL);

    if (pthread_create(&external, NULL, log_flood_external, NULL) != 0) {
        return EXIT_FAILURE;
    }

    posix_shutdown_guard_arm(POSIX_SHUTDOWN_TIMEOUT_MS);
    posix_platform_run();
    (void)pthread_join(external, NULL);
    posix_platform_shutdown();

    if (!scan_log_file(path, producers, LOG_PRODUCERS)) {
        return EXIT_FAILURE;
    }

    const uint32_t dropped = posix_log_dropped();

    print_report_head("log-preempt");
    (void)printf("\"ticks\": %u, \"producers\": [", (unsigned int)LOG_PREEMPT_TICKS);

    for (size_t i = 0U; i < LOG_PRODUCERS; i++) {
        const log_producer_t *p = &producers[i];
        const uint32_t produced = atomic_load(&s_log_produced[i]);

        (void)printf("%s{\"tag\": \"%s\", \"produced\": %u, \"kept\": %u, \"malformed\": %u, "
                     "\"out_of_order\": %u, \"gaps\": %u}",
                     (i == 0U) ? "" : ", ", p->tag, (unsigned int)produced, (unsigned int)p->kept,
                     (unsigned int)p->malformed, (unsigned int)p->out_of_order,
                     (unsigned int)p->gaps);

        if ((p->kept == 0U) || (p->kept > produced) || (p->malformed != 0U) ||
            (p->out_of_order != 0U) || (p->last_seq >= (long)produced)) {
            (void)fprintf(stderr,
                          "producer %s: %u lines written, %u in the file, %u malformed, %u out "
                          "of order\n",
                          p->tag, (unsigned int)produced, (unsigned int)p->kept,
                          (unsigned int)p->malformed, (unsigned int)p->out_of_order);
            status = EXIT_FAILURE;
        } else {
            missing += (uint64_t)(produced - p->kept);
        }
    }

    (void)printf("], \"dropped\": %u, \"missing_from_file\": %llu}\n", (unsigned int)dropped,
                 (unsigned long long)missing);

    /* `dropped` may also hold lines of the platform itself, so it can exceed what the test
     * producers miss, but never fall short of it: a line that is neither in the file nor
     * counted was lost. */
    if (missing > (uint64_t)dropped) {
        (void)fprintf(stderr, "%llu lines are missing from the file, only %u were dropped\n",
                      (unsigned long long)missing, (unsigned int)dropped);
        status = EXIT_FAILURE;
    }

    return status;
}

/* ---------------------------------------------------------------------------- fatal paths */

static void assert_task(void *arg)
{
    (void)arg;
    configASSERT(xTaskGetTickCount() == (TickType_t)0xFFFFFFFFU);

    for (;;) {
    }
}

static void malloc_fail_task(void *arg)
{
    void *block;

    (void)arg;
    block = pvPortMalloc((size_t)configTOTAL_HEAP_SIZE * 2U);
    vPortFree(block);

    for (;;) {
    }
}

static void reset_task(void *arg)
{
    (void)arg;
    plat_reset(PLAT_RESET_REQUESTED);
}

/* The line is logged immediately before the reset, so it is still in the log ring, not yet
 * written by the logger thread (which drains every few milliseconds): only the flush in
 * plat_reset() can put it into the file. */
static void reset_log_task(void *arg)
{
    (void)arg;
    LOG_INF("reset", "the last line before the reset");
    plat_reset(PLAT_RESET_REQUESTED);
}

/* Arms the shutdown guard with a short timeout and never ends the scheduler: the guard has to
 * end the process with POSIX_EXIT_SHUTDOWN_TIMEOUT and say why. */
#define GUARD_TEST_TIMEOUT_MS 300U

static void guard_task(void *arg)
{
    (void)arg;
    posix_shutdown_guard_arm(GUARD_TEST_TIMEOUT_MS);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

/*
 * Stack overflow, as configCHECK_FOR_STACK_OVERFLOW method 2 detects it. The kernel fills a
 * task's stack buffer with a known pattern when it creates the task and checks, every time it
 * switches the task out, that the lowest 16 bytes still hold it; on a microcontroller those
 * are the last bytes a task reaches before it runs off its stack. On the POSIX port the task's
 * code runs on a pthread stack and never reaches that buffer, so the overrun is made by hand:
 * the task owns its statically allocated buffer, overwrites the lowest words the way a
 * runaway stack would, and yields. The kernel has to call the hook, and the hook has to end
 * the process with PLAT_RESET_STACK_OVERFLOW and the task's name on stderr.
 */
#define OVERFLOW_STACK_WORDS configMINIMAL_STACK_SIZE

static StackType_t s_overflow_stack[OVERFLOW_STACK_WORDS];
static StaticTask_t s_overflow_tcb;

static void overflow_task(void *arg)
{
    volatile StackType_t *lowest = s_overflow_stack;

    (void)arg;
    lowest[0] = 0U;
    lowest[1] = 0U;
    taskYIELD();

    /* Only reached if the overrun went unnoticed: end normally, so the test fails on the exit
     * status at once instead of running into its timeout. */
    vTaskDelay(1U);
    plat_end_scheduler();

    for (;;) {
    }
}

static void run_task(void *arg)
{
    (void)arg;
    plat_task_enter("run");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));

        if (plat_shutdown_requested()) {
            plat_end_scheduler();
        }
    }
}

static int cmd_single_task(TaskFunction_t code, const char *log_path)
{
    init_platform(POSIX_TICK_LOCKED, POSIX_IRQ_SIGNAL, log_path);
    create_task(code, "test", 3U, NULL);
    posix_platform_run();
    posix_platform_shutdown();
    return EXIT_SUCCESS;
}

static int cmd_stack_overflow(void)
{
    init_platform(POSIX_TICK_LOCKED, POSIX_IRQ_SIGNAL, NULL);
    (void)xTaskCreateStatic(overflow_task, "ovf", OVERFLOW_STACK_WORDS, NULL, 3U, s_overflow_stack,
                            &s_overflow_tcb);
    posix_platform_run();
    posix_platform_shutdown();
    return EXIT_SUCCESS;
}

/* --------------------------------------------------------------------------------- main */

static int usage(void)
{
    (void)fprintf(stderr,
                  "usage: plat_selftest tick <locked|stock> <seconds>\n"
                  "       plat_selftest suspend <milliseconds>\n"
                  "       plat_selftest tick-late <rounds, 1 to 1000>\n"
                  "       plat_selftest irq <mode> <period_us, 0: storm> <seconds> <load 0|1>\n"
                  "       plat_selftest irq-rounds <mode> <rounds>\n"
                  "       plat_selftest irq-lines <mode>\n"
                  "       plat_selftest log <path> | log-preempt <path> | reset-log <path>\n"
                  "       plat_selftest stack | assert | malloc-fail | stack-overflow | reset |\n"
                  "                     guard | run\n"
                  "<mode>: signal | signal-only | poll\n");
    return 2;
}

static bool parse_irq_mode(const char *text, irq_test_mode_t *mode)
{
    bool ok = true;

    if (strcmp(text, "signal") == 0) {
        *mode = MODE_SIGNAL;
    } else if (strcmp(text, "signal-only") == 0) {
        *mode = MODE_SIGNAL_ONLY;
    } else if (strcmp(text, "poll") == 0) {
        *mode = MODE_POLL;
    } else {
        ok = false;
    }

    return ok;
}

int main(int argc, char **argv)
{
    irq_test_mode_t mode = MODE_SIGNAL;

    if (argc < 2) {
        return usage();
    }

    const char *command = argv[1];

    if ((strcmp(command, "tick") == 0) && (argc == 4)) {
        s_run_seconds = (uint32_t)strtoul(argv[3], NULL, 10);
        return cmd_tick((strcmp(argv[2], "stock") == 0) ? POSIX_TICK_STOCK : POSIX_TICK_LOCKED);
    }

    if ((strcmp(command, "suspend") == 0) && (argc == 3)) {
        s_suspend_ms = (uint32_t)strtoul(argv[2], NULL, 10);
        return cmd_suspend();
    }

    if ((strcmp(command, "tick-late") == 0) && (argc == 3)) {
        s_late_rounds = (uint32_t)strtoul(argv[2], NULL, 10);

        if ((s_late_rounds == 0U) || (s_late_rounds > LATE_MAX_ROUNDS)) {
            return usage();
        }

        return cmd_tick_late();
    }

    if ((strcmp(command, "irq") == 0) && (argc == 6) && parse_irq_mode(argv[2], &mode)) {
        s_irq_period_us = (uint32_t)strtoul(argv[3], NULL, 10);
        s_run_seconds = (uint32_t)strtoul(argv[4], NULL, 10);
        return cmd_irq(mode, strcmp(argv[5], "1") == 0);
    }

    if ((strcmp(command, "irq-rounds") == 0) && (argc == 4) && parse_irq_mode(argv[2], &mode)) {
        s_rounds = (uint32_t)strtoul(argv[3], NULL, 10);
        return cmd_irq_rounds(mode);
    }

    if ((strcmp(command, "irq-lines") == 0) && (argc == 3) && parse_irq_mode(argv[2], &mode)) {
        return cmd_irq_lines(mode);
    }

    if (strcmp(command, "stack") == 0) {
        return cmd_stack();
    }

    if ((strcmp(command, "log") == 0) && (argc == 3)) {
        return cmd_log(argv[2]);
    }

    if ((strcmp(command, "log-preempt") == 0) && (argc == 3)) {
        return cmd_log_preempt(argv[2]);
    }

    if ((strcmp(command, "reset-log") == 0) && (argc == 3)) {
        return cmd_single_task(reset_log_task, argv[2]);
    }

    if (strcmp(command, "assert") == 0) {
        return cmd_single_task(assert_task, NULL);
    }

    if (strcmp(command, "malloc-fail") == 0) {
        return cmd_single_task(malloc_fail_task, NULL);
    }

    if (strcmp(command, "stack-overflow") == 0) {
        return cmd_stack_overflow();
    }

    if (strcmp(command, "reset") == 0) {
        return cmd_single_task(reset_task, NULL);
    }

    if (strcmp(command, "guard") == 0) {
        return cmd_single_task(guard_task, NULL);
    }

    if (strcmp(command, "run") == 0) {
        return cmd_single_task(run_task, NULL);
    }

    return usage();
}
