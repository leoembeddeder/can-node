/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The decisions of the locked tick (posix_tick.c) as pure functions: no kernel, no clock read,
 * no sleep. posix_tick.c's wrapper of the port's usleep() reads the clock and the count of tick
 * interrupts the kernel has taken, passes them in here and carries out the step it gets back;
 * tests/platform/test_posix_tick_plan.c plays modelled orders of events through the same
 * functions.
 *
 * THE GRID. Tick k (the k-th tick interrupt the kernel takes, k = 1, 2, ...) is due at
 * base + (k - 1) * period. The base is the moment of posix_tick.c's first call, right after the
 * port raised its first tick, and it moves forward only when ticks are given up (below); the
 * host-only trace records every base (posix_trace_tick_base()), so the trace and this plan
 * always agree on when a tick was due.
 *
 * ONE CALL OF THE PORT'S usleep(). The port's tick thread loops "raise a tick (SIGALRM to the
 * thread of the running task); usleep(period)". Each usleep() is one pass through this plan:
 *
 *   1. posix_tick_plan_after_raise(), right after the raise. The tick just raised may not have
 *      been taken yet, so the kernel is behind only if it trails the ticks due by more than
 *      that one. Then
 *        - a lag of up to POSIX_TICK_MAX_CATCH_UP_TICKS: POSIX_TICK_CATCH_UP. The wrapper
 *          returns after POSIX_TICK_CATCH_UP_SPACING_NS and the port raises a replacement;
 *        - a longer lag: all of it but the tick just raised is given up (`dropped`), the base
 *          moves forward by as many periods, and the step is a POSIX_TICK_SLEEP as below;
 *        - otherwise POSIX_TICK_SLEEP: sleep to the boundary of the next tick.
 *   2. posix_tick_plan_at_deadline(), at that boundary, before the port raises the next tick.
 *      The tick raised before has had a whole period to be taken, so the count read here is
 *      settled. If the kernel already holds the tick that is about to be raised, a made-up
 *      tick turned out to be one too many: the raise is skipped (`given_back`) and the step is
 *      a POSIX_TICK_SLEEP to the next boundary, where the same question is asked again.
 *      Otherwise POSIX_TICK_RAISE: the wrapper returns and the port raises the tick.
 *
 * WHY A MADE-UP TICK CAN BE ONE TOO MANY. A tick aimed at a thread whose task is being switched
 * out is not lost: it stays pending on that thread, with its signals blocked, and is taken when
 * the task next runs. If that is more than a period later, step 1 cannot tell it from a lost
 * tick and makes it up; when the late one is taken too, the kernel holds one tick more than the
 * clock. Step 2 gives it back at the first boundary after it was taken, so a count one ahead is
 * back on the grid within one period and the tick thread's wake-up delay; if several were late
 * at once, one is given back at each boundary (docs/bug-log.md, entry 011). Step 1 cannot give
 * it back: it runs right after a raise that may still be on its way, and a count one ahead whose
 * last raise is not taken yet reads the same as a count on time whose last raise was taken.
 * Nor does step 1 wait before it makes a tick up: a tick still missing a period after its raise
 * is lost, or pending on a thread that was switched out or that the host has not run yet;
 * nothing the tick thread can read tells these apart however long it waits, and a lost tick
 * that is not made up at once delays every release after it. Step 2 takes back the ones that
 * turn out late.
 *
 * WHAT STEP 1 CANNOT SEE. For the same reason, a lost tick goes unseen while the raises after it
 * are taken before step 1 reads the count: the count then trails the ticks due by just the one
 * step 1 allows for the raise it has just made. It stays a tick behind, every task released up
 * to a period late, until the first raise that is taken after the read, which sees two missing
 * and makes the lost one up. On the development laptop nearly every raise is taken after the
 * read (docs/bug-log.md, entry 011).
 *
 * WHAT IS NOT LOST. A skipped raise is neither a dropped tick nor a lost one: the kernel holds
 * that tick already, the count stays on the grid and the base does not move.
 */
#ifndef FCN_POSIX_TICK_PLAN_H
#define FCN_POSIX_TICK_PLAN_H

#include <stdbool.h>
#include <stdint.h>

/** The longest lag of the count behind the clock that is made up tick by tick. A longer one is
 *  given up: a burst would make every periodic task run back to back. */
#define POSIX_TICK_MAX_CATCH_UP_TICKS 6U

/** The spacing of the replacements raised for lost ticks: long enough for the previous signal
 *  to be handled. */
#define POSIX_TICK_CATCH_UP_SPACING_NS 100000U

/** What the wrapper does next. */
typedef enum {
    POSIX_TICK_SLEEP = 0,   /**< sleep until deadline_ns, then posix_tick_plan_at_deadline() */
    POSIX_TICK_RAISE = 1,   /**< return now: the port raises the next tick */
    POSIX_TICK_CATCH_UP = 2 /**< return after POSIX_TICK_CATCH_UP_SPACING_NS: a replacement */
} posix_tick_action_t;

/** The grid and the boundary the plan is at. Only the plan's functions change it. */
typedef struct {
    uint64_t base_ns;   /**< when tick 1 is due; moved forward when ticks are given up */
    uint64_t period_ns; /**< the tick period, more than 0 */
    uint64_t next;      /**< the tick the port raises at the boundary slept to */
} posix_tick_plan_t;

/** One step of the plan. */
typedef struct {
    posix_tick_action_t action;
    uint64_t deadline_ns; /**< POSIX_TICK_SLEEP: the boundary of tick `next` of the plan */
    uint64_t dropped;     /**< ticks given up in this step (the base moved by that many periods) */
    bool given_back;      /**< this step skipped the raise of a tick the kernel already held */
} posix_tick_step_t;

/** A plan whose tick 1 is due at `base_ns` (the port has just raised it) and whose ticks are
 *  `period_ns` apart (more than 0). */
void posix_tick_plan_init(posix_tick_plan_t *plan, uint64_t base_ns, uint64_t period_ns);

/** Step 1, right after a raise: `now_ns` is the time on the grid's clock, `counted` the number
 *  of tick interrupts the kernel has taken. */
posix_tick_step_t posix_tick_plan_after_raise(posix_tick_plan_t *plan, uint64_t now_ns,
                                              uint64_t counted);

/** Step 2, after a POSIX_TICK_SLEEP to its deadline: `counted` as above, read there. */
posix_tick_step_t posix_tick_plan_at_deadline(posix_tick_plan_t *plan, uint64_t counted);

/** When tick `tick` (1 or more) is due on the plan's grid. */
uint64_t posix_tick_plan_due_ns(const posix_tick_plan_t *plan, uint64_t tick);

#endif /* FCN_POSIX_TICK_PLAN_H */
