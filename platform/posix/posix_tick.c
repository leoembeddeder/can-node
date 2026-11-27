/* SPDX-License-Identifier: MIT */
/*
 * Keeps the tick of the FreeRTOS POSIX port locked to the wall clock.
 *
 * The port (third_party/FreeRTOS-Kernel/portable/ThirdParty/GCC/Posix/port.c) generates its
 * tick from a thread named "Scheduler timer" that loops
 *
 *     pthread_kill(<thread of the running task>, SIGALRM);
 *     usleep(portTICK_RATE_MICROSECONDS);
 *
 * Two properties of that loop make FreeRTOS time fall behind real time:
 *
 *   1. The sleep is relative, so every tick lasts the period plus the loop's own latency.
 *      results/platform_timing.json (tick.stock) has the measured size of the error.
 *   2. SIGALRM is not queued. A tick raised while the previous one is still pending merges
 *      with it, and the kernel counts one tick where two were due.
 *
 * A third makes a tick late rather than lost: a tick aimed at a thread whose task is being
 * switched out stays pending on that thread, with its signals blocked, and is counted when its
 * task next runs.
 *
 * The kernel is a pinned, unmodified submodule, so the correction is applied at link time:
 * the executables are linked with -Wl,--wrap=usleep, which routes the port's call to
 * __wrap_usleep() below. For the tick thread it replaces the relative sleep with a sleep
 * until an absolute deadline on a grid of CLOCK_MONOTONIC (tick k due at base + (k - 1)
 * periods), and it compares the number of ticks the kernel has actually counted with that
 * grid. The decisions are posix_tick_plan.h's, which says each step: a tick missing after a
 * period is taken for lost and made up at once; a made-up tick that turns out one too many,
 * because the tick it replaced was only late, is given back at the next boundary, where the
 * raise of a tick the kernel already holds is skipped. The tick count therefore tracks
 * CLOCK_MONOTONIC instead of drifting away from it, and is ahead of it only by ticks taken
 * late, each from the moment it is taken to the boundary that gives it back: the next one, or
 * for a second taken in the same period the one after.
 *
 * What cannot be fixed from here: whenever the kernel's count trails the wall clock by more
 * than POSIX_TICK_MAX_CATCH_UP_TICKS, the missing ticks are given up rather than replayed as a
 * burst (a burst would make every periodic task run back to back). That happens when the host
 * does not run the task threads for that long, for whatever reason: a loaded machine, a
 * stopped process, and equally a task that keeps a critical section for several milliseconds.
 * The ticks given up are counted as dropped_ticks and reported with every measurement, because
 * a run that dropped ticks did not keep time and has to say so.
 */
#include "posix_platform.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "posix_tick_plan.h"
#include "posix_trace.h"
#include "task.h"

/* Name the port gives its tick thread (prvTimerTickHandler in port.c). */
#define PORT_TICK_THREAD_NAME "Scheduler timer"

enum { ROLE_UNKNOWN = 0, ROLE_TICK_THREAD = 1, ROLE_OTHER = 2 };

static _Atomic int s_mode = POSIX_TICK_LOCKED;
static _Atomic bool s_tick_thread_seen;
static _Atomic uint64_t s_signals_sent;
static _Atomic uint64_t s_catch_ups;
static _Atomic uint64_t s_dropped_ticks;
static _Atomic uint64_t s_give_backs;
static _Atomic uint64_t s_ticks_taken;

/* The grid, published for posix_tick_due_ns(): written by the tick thread only. */
static _Atomic uint64_t s_grid_base_ns;
static _Atomic uint64_t s_grid_period_ns;

/*
 * Called from the tick hook: one more tick interrupt has reached the kernel.
 *
 * This count, not xTaskGetTickCount(), is what the wrapper below compares with the wall
 * clock. The two differ while a task holds the scheduler suspended (vTaskSuspendAll): the
 * kernel still takes every tick, but only notes it and adds all of them to the tick count
 * when the scheduler resumes. A loop that watched the tick count would take each of those
 * ticks for lost, raise replacements several times too fast, and after the resume find the
 * count far ahead of the clock (docs/bug-log.md, entry 003). The tick hook runs once for
 * every tick interrupt, suspended scheduler or not, and not again when the noted ticks are
 * replayed.
 */
void posix_tick_taken(void)
{
    (void)atomic_fetch_add(&s_ticks_taken, 1U);
}

/* The number of tick interrupts the kernel has taken (see posix_tick_taken()). */
static uint64_t ticks_counted(void)
{
    return atomic_load(&s_ticks_taken);
}

/* The names are fixed by the linker's --wrap convention. */
int __real_usleep(useconds_t usec);
int __wrap_usleep(useconds_t usec);

void posix_tick_configure(posix_tick_mode_t mode)
{
    atomic_store(&s_mode, (int)mode);
}

void posix_tick_get_stats(posix_tick_stats_t *out)
{
    out->mode = (posix_tick_mode_t)atomic_load(&s_mode);
    out->tick_thread_seen = atomic_load(&s_tick_thread_seen);
    out->signals_sent = atomic_load(&s_signals_sent);
    out->catch_ups = atomic_load(&s_catch_ups);
    out->dropped_ticks = atomic_load(&s_dropped_ticks);
    out->give_backs = atomic_load(&s_give_backs);
}

uint64_t posix_tick_due_ns(uint64_t tick)
{
    const uint64_t period_ns = atomic_load(&s_grid_period_ns);

    return (period_ns == 0U) ? 0U : (atomic_load(&s_grid_base_ns) + ((tick - 1U) * period_ns));
}

/* Called once, a little after start-up. If the wrapper below never saw the port's tick thread,
 * the tick is not under this file's control: the executable was linked without
 * -Wl,--wrap=usleep, or the port changed how it generates its tick. Every timing figure would
 * then silently be that of the stock port, so this is fatal rather than a warning. */
void posix_tick_check_active(void)
{
    if (!atomic_load(&s_tick_thread_seen)) {
        static const char message[] =
            "FATAL the tick wrapper is not active (link with -Wl,--wrap=usleep)\n";

        (void)!write(STDERR_FILENO, message, sizeof(message) - 1U);
        plat_reset(PLAT_RESET_ASSERT);
    }
}

static void sleep_until_ns(uint64_t deadline_ns)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(deadline_ns / 1000000000ULL);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ULL);

    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
        /* interrupted: sleep again until the same absolute deadline */
    }
}

static int identify_caller(void)
{
    char name[32] = {0};
    int role = ROLE_OTHER;

    if ((pthread_getname_np(pthread_self(), name, sizeof(name)) == 0) &&
        (strcmp(name, PORT_TICK_THREAD_NAME) == 0)) {
        role = ROLE_TICK_THREAD;
    }

    return role;
}

/* The grid has a new base from tick `first_tick` on: tell posix_tick_due_ns() and the trace. */
static void publish_base(const posix_tick_plan_t *plan, uint64_t first_tick)
{
    atomic_store(&s_grid_base_ns, plan->base_ns);
    atomic_store(&s_grid_period_ns, plan->period_ns);
    posix_trace_tick_base(plan->base_ns, first_tick, (uint32_t)plan->period_ns);
}

int __wrap_usleep(useconds_t usec)
{
    static _Thread_local int role = ROLE_UNKNOWN;
    static _Thread_local posix_tick_plan_t plan;
    static _Thread_local bool base_published = false;

    /* The variable is thread-local and static: it keeps its value between calls. */
    /* cppcheck-suppress knownConditionTrueFalse */
    if (role == ROLE_UNKNOWN) {
        role = identify_caller();

        if (role == ROLE_TICK_THREAD) {
            /* The default timer slack of 50 us would be added to every wake-up. It is only
             * removed in locked mode: the stock mode is the port as it comes, the baseline
             * that the measurements compare against. The mode is set before the scheduler
             * starts, so it is known here. */
            if (atomic_load(&s_mode) == POSIX_TICK_LOCKED) {
                (void)prctl(PR_SET_TIMERSLACK, 1UL);
            }

            /* The port has just raised its first tick: tick 1 is due now. */
            posix_tick_plan_init(&plan, posix_monotonic_ns(), (uint64_t)usec * 1000ULL);
            atomic_store(&s_tick_thread_seen, true);
        }
    }

    if (role != ROLE_TICK_THREAD) {
        return __real_usleep(usec);
    }

    (void)atomic_fetch_add(&s_signals_sent, 1U);

    if (atomic_load(&s_mode) == POSIX_TICK_STOCK) {
        return __real_usleep(usec);
    }

    /* The host-only trace (posix_trace.h) learns when every tick is due: tick k at the base +
     * (k - 1) periods, from here until a drop moves the base. */
    if (!base_published) {
        base_published = true;
        publish_base(&plan, 1U);
    }

    /* The port has just raised a tick. The plan (posix_tick_plan.h) decides what follows;
     * this loop only carries it out and counts. The clock is read before the count, so that
     * a tick taken between the two reads is not taken for missing. */
    const uint64_t now_ns = posix_monotonic_ns();
    uint64_t counted = ticks_counted();
    posix_tick_step_t step = posix_tick_plan_after_raise(&plan, now_ns, counted);

    for (;;) {
        if (step.dropped > 0U) {
            (void)atomic_fetch_add(&s_dropped_ticks, step.dropped);
            /* From the tick after the ones counted, every tick is due on the new base. */
            publish_base(&plan, counted + 1U);
        }

        if (step.given_back) {
            /* The kernel already held the tick due here: no raise, on to the next boundary. */
            (void)atomic_fetch_add(&s_give_backs, 1U);
        }

        if (step.action != POSIX_TICK_SLEEP) {
            break;
        }

        sleep_until_ns(step.deadline_ns);
        counted = ticks_counted();
        step = posix_tick_plan_at_deadline(&plan, counted);
    }

    if (step.action == POSIX_TICK_CATCH_UP) {
        /* A tick was lost: return soon so that the port raises another one. */
        const struct timespec spacing = {0, (long)POSIX_TICK_CATCH_UP_SPACING_NS};

        (void)atomic_fetch_add(&s_catch_ups, 1U);
        (void)nanosleep(&spacing, NULL);
    }

    return 0;
}
