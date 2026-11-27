/* SPDX-License-Identifier: MIT */
/*
 * The hardware watchdog of the host build (plat_wdg.h): an independent watchdog modelled by a
 * thread outside the scheduler. plat_wdg.h says what it does for the application; this comment
 * says how the model counts.
 *
 * THE COUNT. The watchdog's own clock is the thread: it wakes every WDG_PASS_NS and adds the
 * time since its previous pass (CLOCK_MONOTONIC) to `counted`, the time since the last kick.
 * A kick stores its own time stamp and then counts itself; a pass that finds the kick count
 * changed starts `counted` again from that time stamp. When `counted` reaches the timeout, the
 * thread resets the node with PLAT_RESET_WATCHDOG. Two properties follow:
 *
 *  - never early: `counted` is never more than the time since the last kick the thread has
 *    seen. Each step adds at most the real time between two passes, and a new kick restarts the
 *    count at most at the real time since that kick;
 *  - late by at most one pass and the host's wake-up latency, as long as the host runs the
 *    thread: the expiry is noticed at the first pass after it.
 *
 * HOST STALLS. A gap between two passes longer than WDG_GAP_MAX_NS counts as WDG_GAP_MAX_NS
 * (and is counted in `gaps`). A virtual machine that is descheduled, or a host too loaded to
 * run the process, stops the node's tasks as well; without this rule the thread, which wakes
 * first, would reset a node that had no chance to kick. A target's watchdog does not count
 * while its clock is stopped either. The rule only ever makes a reset later. Ordinary
 * scheduling delays of a few milliseconds are far below the limit and count in full.
 *
 * A KICK RACING THE EXPIRY. A kick whose count has not reached the thread when it decides is
 * not seen: the reset happens although the task kicked a moment before. That is a kick that
 * came at the deadline, after the full timeout without one, and a real watchdog behaves the
 * same: a refresh that arrives as the counter reaches zero does not stop the reset.
 *
 * THREADS. plat_wdg_start() creates the thread, inside a critical section when a task calls it
 * (pthread_create allocates and takes the C library's locks, see posix_platform.h). The kick
 * touches only atomics. The thread writes the statistics, as atomics, and reads the rest.
 * plat_end_scheduler() stops the count (posix_wdg_stop()): the orderly end of a run is the
 * board being switched off, and from then on the shutdown guard looks after a hang.
 */
#include "plat_wdg.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "posix_platform.h"
#include "task.h"

#define WDG_PASS_NS    1000000ULL  /* the model's clock: one pass per millisecond */
#define WDG_GAP_MAX_NS 50000000ULL /* a longer gap between two passes counts as this much */
#define NS_PER_MS      1000000ULL

_Static_assert(WDG_GAP_MAX_NS > (10U * WDG_PASS_NS), "a stall is far longer than a pass");
_Static_assert(((uint64_t)PLAT_WDG_TIMEOUT_MAX_MS * NS_PER_MS) < (UINT64_MAX / 2U),
               "a timeout in ns fits easily");

static atomic_flag s_start_claimed = ATOMIC_FLAG_INIT;
static _Atomic uint32_t s_timeout_ms; /* 0 until started */
static _Atomic uint64_t s_kick_ns;    /* time stamp of the last kick (or of the start) */
static _Atomic uint64_t s_kicks;      /* kicks since the start */
static _Atomic bool s_stop;           /* the orderly end of the run: stop counting */
static pthread_t s_thread;
static bool s_thread_created;

/* Written by the thread, read by the statistics. */
static _Atomic uint64_t s_longest_unkicked_ns;
static _Atomic uint64_t s_gaps;
static _Atomic uint64_t s_longest_gap_ns;

static void to_timespec(uint64_t ns, struct timespec *out)
{
    out->tv_sec = (time_t)(ns / 1000000000ULL);
    out->tv_nsec = (long)(ns % 1000000000ULL);
}

/* The count ran out: say so on stderr, as the other fatal paths do, and reset. */
static _Noreturn void expire(uint64_t counted_ns, uint32_t timeout_ms)
{
    char text[128];
    const int len =
        snprintf(text, sizeof(text), "FATAL watchdog: not kicked for %llu ms (timeout %u ms)\n",
                 (unsigned long long)(counted_ns / NS_PER_MS), (unsigned int)timeout_ms);

    if (len > 0) {
        const size_t count = ((size_t)len < sizeof(text)) ? (size_t)len : (sizeof(text) - 1U);

        (void)!write(STDERR_FILENO, text, count);
    }

    plat_reset(PLAT_RESET_WATCHDOG);
}

static void *watchdog_main(void *arg)
{
    const uint32_t timeout_ms = atomic_load(&s_timeout_ms);
    const uint64_t timeout_ns = (uint64_t)timeout_ms * NS_PER_MS;
    uint64_t seen_kicks = atomic_load(&s_kicks);
    uint64_t previous = posix_monotonic_ns();
    const uint64_t started = atomic_load(&s_kick_ns);
    uint64_t counted = (previous > started) ? (previous - started) : 0U;

    (void)arg;
    posix_external_thread_setup("fcn-wdg");

    while (!atomic_load(&s_stop)) {
        struct timespec next;

        to_timespec(previous + WDG_PASS_NS, &next);
        /* All signals are blocked in this thread, so the sleep is not interrupted. */
        (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        const uint64_t now = posix_monotonic_ns();
        uint64_t gap = now - previous;

        previous = now;

        if (gap > WDG_GAP_MAX_NS) {
            (void)atomic_fetch_add(&s_gaps, 1U);

            if (gap > atomic_load(&s_longest_gap_ns)) {
                atomic_store(&s_longest_gap_ns, gap);
            }

            gap = WDG_GAP_MAX_NS;
        }

        const uint64_t kicks = atomic_load(&s_kicks);

        if (kicks != seen_kicks) {
            /* The kick's time stamp was stored before its count (plat_wdg_kick). */
            const uint64_t kicked = atomic_load(&s_kick_ns);
            const uint64_t since = (now > kicked) ? (now - kicked) : 0U;

            seen_kicks = kicks;
            counted = (since < gap) ? since : gap;
        } else {
            counted += gap;
        }

        if (counted > atomic_load(&s_longest_unkicked_ns)) {
            atomic_store(&s_longest_unkicked_ns, counted);
        }

        if ((counted >= timeout_ns) && !atomic_load(&s_stop)) {
            expire(counted, timeout_ms);
        }
    }

    return NULL;
}

bool plat_wdg_start(uint32_t timeout_ms)
{
    bool started = false;

    if ((timeout_ms >= PLAT_WDG_TIMEOUT_MIN_MS) && (timeout_ms <= PLAT_WDG_TIMEOUT_MAX_MS) &&
        !atomic_flag_test_and_set(&s_start_claimed)) {
        /* The start is the first kick. The timeout is published last: plat_wdg_kick() acts
         * only once it is set. */
        atomic_store(&s_kick_ns, posix_monotonic_ns());
        atomic_store(&s_kicks, 0U);
        atomic_store(&s_timeout_ms, timeout_ms);

        taskENTER_CRITICAL();
        s_thread_created = (pthread_create(&s_thread, NULL, watchdog_main, NULL) == 0);
        taskEXIT_CRITICAL();

        if (!s_thread_created) {
            /* No model, no watchdog: kicks go nowhere again, and no second start is tried. */
            atomic_store(&s_timeout_ms, 0U);
        }

        started = s_thread_created;
    }

    return started;
}

void plat_wdg_kick(void)
{
    if (atomic_load(&s_timeout_ms) != 0U) {
        atomic_store(&s_kick_ns, posix_monotonic_ns());
        (void)atomic_fetch_add(&s_kicks, 1U);
    }
}

void posix_wdg_stop(void)
{
    atomic_store(&s_stop, true);
}

void posix_wdg_shutdown(void)
{
    posix_wdg_stop();

    if (s_thread_created) {
        (void)pthread_join(s_thread, NULL);
        s_thread_created = false;
    }
}

void posix_wdg_get_stats(posix_wdg_stats_t *out)
{
    out->timeout_ms = atomic_load(&s_timeout_ms);
    out->started = (out->timeout_ms != 0U);
    out->kicks = atomic_load(&s_kicks);
    out->longest_unkicked_ns = atomic_load(&s_longest_unkicked_ns);
    out->gaps = atomic_load(&s_gaps);
    out->longest_gap_ns = atomic_load(&s_longest_gap_ns);
}
