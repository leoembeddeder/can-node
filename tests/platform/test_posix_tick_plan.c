/* SPDX-License-Identifier: MIT */
/*
 * Tests of the locked tick's decisions (platform/posix/posix_tick_plan.h), independent of the
 * host's timing: a model plays the port's tick thread and the kernel through the plan, in the
 * order of events each test chooses, and checks the kernel's tick count against the plan's
 * grid of CLOCK_MONOTONIC.
 *
 * THE MODEL. Time is a number of nanoseconds. The tick thread raises tick after tick and calls
 * the plan as posix_tick.c's __wrap_usleep() does: posix_tick_plan_after_raise() ENTRY_READ_NS
 * after a raise (later by a stall where a test says so), posix_tick_plan_at_deadline() WAKE_NS
 * after each deadline it sleeps to (its wake-up delay); after a catch-up the next raise comes
 * POSIX_TICK_CATCH_UP_SPACING_NS later, otherwise when the plan returns. What becomes of each
 * raise is the test's choice:
 *
 *   AT_ONCE     taken TAKE_AT_ONCE_NS after the raise, before the plan reads the count
 *   AFTER_READ  taken TAKE_AFTER_READ_NS after the raise, after the plan has read the count:
 *               what the development laptop showed for about 99.9 % of the raises
 *               (docs/bug-log.md, entry 011)
 *   LATE        pending on a thread that was switched out, taken `late_ns` after the raise
 *   LOST        merged with a signal already pending: never taken
 *
 * THE CHECKS. From the times at which the kernel took its ticks and the grid (tick k due at
 * base + (k - 1) periods, with the base moved where ticks were given up, as the trace records
 * it). The kernel's count reaches k at its k-th take: tick k is early if that comes before k is
 * due, and a task waiting for tick k is then released early, as node_timing counts it. Ticks
 * early one after the other are one stretch ahead of the clock, from the first of them to the
 * due time of the last, when the count is back on the grid. A count one ahead for good, as
 * with the give-back of bug 011, makes every tick after it early. Further: the latest tick,
 * the largest and smallest lead of the count over the ticks due at any moment, and whether the
 * count is on the grid at the end. Whatever the test, the model fails a plan that stops the tick
 * (no raise for STOPPED_PERIODS periods), that sleeps to no later boundary without a raise
 * between (the tick thread would spin) or whose grid moves without a dropped tick.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "posix_tick_plan.h"
#include "unity.h"

#define PERIOD_NS          1000000ULL
#define T0_NS              1000000000ULL /* the port's first raise */
#define ENTRY_READ_NS      2000ULL
#define TAKE_AT_ONCE_NS    1000ULL
#define TAKE_AFTER_READ_NS 5000ULL
#define WAKE_NS            50000ULL

#define MAX_TAKES 8192U
#define MAX_BASES 64U

/* A plan that raises no tick for this many periods after the tick thread's last read has
 * stopped the tick: the model fails instead of sleeping on for ever. */
#define STOPPED_PERIODS 50U

typedef enum { AT_ONCE, AFTER_READ, LATE, LOST } fate_kind_t;

typedef struct {
    fate_kind_t kind;
    uint64_t late_ns; /* LATE only */
} fate_t;

/* What becomes of raise number `raise` (1, 2, ..., replacements included), made at `raise_ns`. */
typedef fate_t (*fate_fn_t)(uint64_t raise, uint64_t raise_ns, const void *context);

typedef struct {
    fate_fn_t fate;
    const void *context;
    uint64_t periods;     /* the run ends at the first boundary this many periods after T0 */
    uint64_t stall_raise; /* the tick thread's entry after this raise comes stall_ns late */
    uint64_t stall_ns;
} scenario_t;

typedef struct {
    uint64_t raises;
    uint64_t catch_ups;
    uint64_t give_backs;
    uint64_t dropped;
    int64_t max_lead;
    int64_t min_lead;
    int64_t lead_at_end;
    uint64_t early_ticks;      /* ticks taken before they were due */
    uint64_t early_row;        /* the most of them in a row */
    uint64_t longest_ahead_ns; /* the longest stretch ahead (see the top of the file) */
    uint64_t ahead_ends_ns;    /* when the last stretch ahead ended (0: none) */
    uint64_t latest_ns;        /* the latest a tick was taken after it was due */
    uint64_t taken_at_end;
    uint64_t due_on_first_base_at_end; /* the ticks due at the end, had no tick been given up */
    uint64_t first_base_ns;
    uint64_t last_base_ns;
} outcome_t;

static uint64_t s_take_ns[MAX_TAKES];
static size_t s_takes;
static uint64_t s_base_first_tick[MAX_BASES];
static uint64_t s_base_ns[MAX_BASES];
static size_t s_bases;

void setUp(void)
{
}

void tearDown(void)
{
}

static fate_t fate_of(fate_kind_t kind, uint64_t late_ns)
{
    const fate_t fate = {kind, late_ns};

    return fate;
}

static void raise_tick(uint64_t raise_ns, fate_t fate)
{
    uint64_t at = 0U;

    switch (fate.kind) {
    case AT_ONCE:
        at = raise_ns + TAKE_AT_ONCE_NS;
        break;
    case AFTER_READ:
        at = raise_ns + TAKE_AFTER_READ_NS;
        break;
    case LATE:
        at = raise_ns + fate.late_ns;
        break;
    case LOST:
    default:
        return;
    }

    TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(MAX_TAKES, s_takes, "the model's take buffer is full");
    s_take_ns[s_takes] = at;
    s_takes++;
}

/* The number of tick interrupts the kernel has taken by `t`. */
static uint64_t counted_at(uint64_t t)
{
    uint64_t n = 0U;

    for (size_t i = 0U; i < s_takes; i++) {
        n += (s_take_ns[i] <= t) ? 1U : 0U;
    }

    return n;
}

static void record_base(uint64_t first_tick, uint64_t base_ns)
{
    TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(MAX_BASES, s_bases, "the model's base list is full");
    s_base_first_tick[s_bases] = first_tick;
    s_base_ns[s_bases] = base_ns;
    s_bases++;
}

/* When tick k is due on the grid the trace would have: the last base from tick k on. */
static uint64_t due_ns(uint64_t k)
{
    size_t index = 0U;

    for (size_t i = 0U; i < s_bases; i++) {
        if (s_base_first_tick[i] <= k) {
            index = i;
        }
    }

    return s_base_ns[index] + ((k - 1U) * PERIOD_NS);
}

static int compare_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

static uint64_t max_u64(uint64_t a, uint64_t b)
{
    return (a > b) ? a : b;
}

/* Every tick k against its due time: early ticks, how many in a row, the stretches ahead, the
 * latest tick. Takes after `end_ns` are left out. */
static void judge_ticks(outcome_t *out, uint64_t end_ns)
{
    uint64_t row = 0U;
    uint64_t row_from_ns = 0U;

    for (size_t i = 0U; (i < s_takes) && (s_take_ns[i] <= end_ns); i++) {
        const uint64_t take_ns = s_take_ns[i];
        const uint64_t due = due_ns((uint64_t)i + 1U);

        if (take_ns < due) {
            row_from_ns = (row == 0U) ? take_ns : row_from_ns;
            row++;
            out->early_ticks++;
            out->early_row = max_u64(out->early_row, row);
            out->longest_ahead_ns = max_u64(out->longest_ahead_ns, due - row_from_ns);
            out->ahead_ends_ns = due;
        } else {
            row = 0U;
            out->latest_ns = max_u64(out->latest_ns, take_ns - due);
        }
    }
}

/* Walks the takes and the due times up to `end_ns`: the lead of the count over time. */
static void follow_lead(outcome_t *out, uint64_t end_ns)
{
    size_t take = 0U;
    uint64_t taken = 0U;
    uint64_t due = 0U;

    out->max_lead = 0;
    out->min_lead = 0;

    for (;;) {
        const uint64_t next_due = due_ns(due + 1U);
        const uint64_t next_take = (take < s_takes) ? s_take_ns[take] : UINT64_MAX;

        /* At the same moment the tick falls due first: a tick taken when it is due is on time. */
        if ((next_due <= next_take) && (next_due <= end_ns)) {
            due++;
        } else if (next_take <= end_ns) {
            take++;
            taken++;
        } else {
            break;
        }

        const int64_t lead = (int64_t)taken - (int64_t)due;

        out->max_lead = (lead > out->max_lead) ? lead : out->max_lead;
        out->min_lead = (lead < out->min_lead) ? lead : out->min_lead;
    }

    out->taken_at_end = taken;
    out->lead_at_end = (int64_t)taken - (int64_t)due;
    out->due_on_first_base_at_end = ((end_ns - s_base_ns[0]) / PERIOD_NS) + 1U;
}

static void analyse(outcome_t *out, uint64_t end_ns)
{
    qsort(s_take_ns, s_takes, sizeof(s_take_ns[0]), compare_u64);
    judge_ticks(out, end_ns);
    follow_lead(out, end_ns);
}

/* Runs the tick thread through the plan as posix_tick.c's __wrap_usleep() does. */
static outcome_t run(const scenario_t *scenario)
{
    outcome_t out;
    posix_tick_plan_t plan;
    uint64_t now = T0_NS;
    uint64_t raise = 1U;
    const uint64_t end_ns = T0_NS + (scenario->periods * PERIOD_NS);

    (void)memset(&out, 0, sizeof(out));
    s_takes = 0U;
    s_bases = 0U;

    /* The port's first raise comes just before posix_tick.c starts the grid at its first call,
     * so it is taken after that call, whatever the scenario says for the others. */
    const fate_t first = scenario->fate(raise, now, scenario->context);

    raise_tick(now, (first.kind == AT_ONCE) ? fate_of(AFTER_READ, 0U) : first);
    out.raises = 1U;

    for (;;) {
        now += ENTRY_READ_NS + ((raise == scenario->stall_raise) ? scenario->stall_ns : 0U);

        if (raise == 1U) {
            /* posix_tick.c starts the plan at its first call, right after the first raise. */
            posix_tick_plan_init(&plan, now, PERIOD_NS);
            record_base(1U, plan.base_ns);
            out.first_base_ns = plan.base_ns;
        }

        const uint64_t read_ns = now;
        uint64_t last_deadline_ns = 0U;
        uint64_t counted = counted_at(now);
        posix_tick_step_t step = posix_tick_plan_after_raise(&plan, now, counted);

        for (;;) {
            /* What posix_tick.c counts and traces after every step. */
            TEST_ASSERT_MESSAGE(!step.given_back || (step.dropped == 0U),
                                "a step gave back and dropped at once");
            out.give_backs += step.given_back ? 1U : 0U;

            if (step.dropped > 0U) {
                out.dropped += step.dropped;
                record_base(counted + 1U, plan.base_ns);
            }

            if (step.action != POSIX_TICK_SLEEP) {
                break;
            }

            /* Every sleep without a raise between goes to a later boundary (else the tick
             * thread would spin), and the tick does not stop. */
            TEST_ASSERT_GREATER_THAN_UINT64_MESSAGE(last_deadline_ns, step.deadline_ns,
                                                    "the plan slept to no later boundary");
            TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(read_ns + (STOPPED_PERIODS * PERIOD_NS),
                                                 step.deadline_ns, "the tick stopped");
            last_deadline_ns = step.deadline_ns;
            now = ((step.deadline_ns > now) ? step.deadline_ns : now) + WAKE_NS;
            counted = counted_at(now);
            step = posix_tick_plan_at_deadline(&plan, counted);
        }

        if (step.action == POSIX_TICK_CATCH_UP) {
            out.catch_ups++;
            now += POSIX_TICK_CATCH_UP_SPACING_NS;
        }

        /* The port raises the next tick. The last one is raised as well, so that the count is
         * whole at the end, which comes once that raise has been taken. */
        raise++;
        out.raises++;
        raise_tick(now, scenario->fate(raise, now, scenario->context));

        if (now >= end_ns) {
            break;
        }
    }

    out.last_base_ns = plan.base_ns;
    /* The grid moves only where ticks were given up, and the trace then learns of it: the
     * plan's grid is the last base recorded. */
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(s_base_ns[s_bases - 1U], plan.base_ns,
                                     "the plan's grid moved without a dropped tick");
    analyse(&out, now + TAKE_AFTER_READ_NS);
    return out;
}

/* ------------------------------------------------------------------------------ fates */

typedef struct {
    fate_kind_t fallback;
    uint64_t raise;   /* the raise that is LATE (late_ns) or LOST (late_ns 0); 0 for none */
    uint64_t late_ns; /* 0: LOST */
    uint64_t raise2;  /* a second one, 0 for none */
    uint64_t late2_ns;
    uint64_t at_once_from_ns; /* raises from then on are AT_ONCE; 0 for never */
} one_off_t;

static fate_t one_off(uint64_t raise, uint64_t raise_ns, const void *context)
{
    const one_off_t *c = context;

    if ((c->raise != 0U) && ((raise == c->raise) || (raise == c->raise2))) {
        const uint64_t late_ns = (raise == c->raise) ? c->late_ns : c->late2_ns;

        return fate_of((late_ns == 0U) ? LOST : LATE, late_ns);
    }

    if ((c->at_once_from_ns != 0U) && (raise_ns >= c->at_once_from_ns)) {
        return fate_of(AT_ONCE, 0U);
    }

    return fate_of(c->fallback, 0U);
}

/* When raise number `raise` is made if every raise before it was taken on time: the port's
 * first at T0, the next at each boundary after the tick thread's wake-up. */
static uint64_t raise_ns_on_time(uint64_t raise)
{
    return (raise == 1U) ? T0_NS : (T0_NS + ENTRY_READ_NS + ((raise - 1U) * PERIOD_NS) + WAKE_NS);
}

/* The task threads stand still from `from_ns` to `to_ns`: the first raise in that time is
 * pending on the stalled thread until `to_ns`, every later one merges with it. */
typedef struct {
    uint64_t from_ns;
    uint64_t to_ns;
} task_stall_t;

static fate_t task_stall(uint64_t raise, uint64_t raise_ns, const void *context)
{
    static uint64_t pending_raise = 0U;
    const task_stall_t *c = context;

    if (raise == 1U) {
        pending_raise = 0U;
    }

    if ((raise_ns >= c->from_ns) && (raise_ns < c->to_ns)) {
        if (pending_raise == 0U) {
            pending_raise = raise;
            return fate_of(LATE, c->to_ns - raise_ns);
        }

        return fate_of(LOST, 0U);
    }

    return fate_of(AFTER_READ, 0U);
}

/* ------------------------------------------------------------------ checks used by many */

static void assert_on_the_grid_at_the_end(const outcome_t *out, const char *what)
{
    TEST_ASSERT_EQUAL_INT64_MESSAGE(0, out->lead_at_end, what);
    /* Every tick period is counted or given up: what the tick promises (plat_selftest tick). */
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(out->due_on_first_base_at_end,
                                     out->taken_at_end + out->dropped, what);
}

/* A count one ahead comes back to the grid at the next boundary: within a period and the tick
 * thread's wake-up. */
#define AHEAD_BOUND_NS (PERIOD_NS + WAKE_NS)

/* ---------------------------------------------------------------- the plan, step by step */

static void test_the_first_sleep_is_to_the_boundary_of_the_second_tick(void)
{
    posix_tick_plan_t plan;

    posix_tick_plan_init(&plan, 5000U, PERIOD_NS);

    const posix_tick_step_t step = posix_tick_plan_after_raise(&plan, 5000U, 0U);

    TEST_ASSERT_EQUAL(POSIX_TICK_SLEEP, step.action);
    TEST_ASSERT_EQUAL_UINT64(5000U + PERIOD_NS, step.deadline_ns);
    TEST_ASSERT_EQUAL_UINT64(2U, plan.next);
    TEST_ASSERT_EQUAL_UINT64(5000U, posix_tick_plan_due_ns(&plan, 1U));
    TEST_ASSERT_EQUAL_UINT64(5000U + (9U * PERIOD_NS), posix_tick_plan_due_ns(&plan, 10U));
}

static void test_the_tick_just_raised_is_not_a_lag_and_one_more_is(void)
{
    posix_tick_plan_t plan;

    posix_tick_plan_init(&plan, 0U, PERIOD_NS);

    /* 10 ticks due by now (the one just raised included), 9 taken: on time. */
    posix_tick_step_t step = posix_tick_plan_after_raise(&plan, (9U * PERIOD_NS) + 7U, 9U);

    TEST_ASSERT_EQUAL(POSIX_TICK_SLEEP, step.action);
    TEST_ASSERT_EQUAL_UINT64(10U * PERIOD_NS, step.deadline_ns);
    TEST_ASSERT_EQUAL_UINT64(11U, plan.next);

    /* Every lag from 2 to the limit is made up, one replacement at a time. */
    for (uint64_t lag = 2U; lag <= POSIX_TICK_MAX_CATCH_UP_TICKS; lag++) {
        step = posix_tick_plan_after_raise(&plan, (9U * PERIOD_NS) + 7U, 10U - lag);
        TEST_ASSERT_EQUAL(POSIX_TICK_CATCH_UP, step.action);
        TEST_ASSERT_EQUAL_UINT64(0U, step.dropped);
        TEST_ASSERT_FALSE(step.given_back);
        TEST_ASSERT_EQUAL_UINT64(0U, plan.base_ns);
    }
}

static void test_a_lag_beyond_the_limit_is_given_up_but_the_tick_just_raised(void)
{
    posix_tick_plan_t plan;
    const uint64_t lag = POSIX_TICK_MAX_CATCH_UP_TICKS + 1U;

    posix_tick_plan_init(&plan, 0U, PERIOD_NS);

    /* 20 due by now, 20 - lag taken. */
    const posix_tick_step_t step =
        posix_tick_plan_after_raise(&plan, (19U * PERIOD_NS) + 3U, 20U - lag);

    TEST_ASSERT_EQUAL(POSIX_TICK_SLEEP, step.action);
    TEST_ASSERT_EQUAL_UINT64(lag - 1U, step.dropped);
    TEST_ASSERT_FALSE(step.given_back);
    TEST_ASSERT_EQUAL_UINT64((lag - 1U) * PERIOD_NS, plan.base_ns);
    /* On the moved base the ticks counted and the one just raised are due by now. */
    TEST_ASSERT_EQUAL_UINT64(20U - lag + 2U, plan.next);
    TEST_ASSERT_EQUAL_UINT64(20U * PERIOD_NS, step.deadline_ns);
}

static void test_at_a_boundary_a_tick_the_kernel_already_holds_is_not_raised(void)
{
    posix_tick_plan_t plan;

    posix_tick_plan_init(&plan, 0U, PERIOD_NS);
    (void)posix_tick_plan_after_raise(&plan, (4U * PERIOD_NS) + 2U, 4U);
    TEST_ASSERT_EQUAL_UINT64(6U, plan.next);

    /* Behind or on time: the port raises tick 6. */
    TEST_ASSERT_EQUAL(POSIX_TICK_RAISE, posix_tick_plan_at_deadline(&plan, 4U).action);
    TEST_ASSERT_EQUAL(POSIX_TICK_RAISE, posix_tick_plan_at_deadline(&plan, 5U).action);
    TEST_ASSERT_EQUAL_UINT64(6U, plan.next);

    /* The kernel holds 6 already: no raise, the next boundary, and the same question there. */
    posix_tick_step_t step = posix_tick_plan_at_deadline(&plan, 6U);

    TEST_ASSERT_EQUAL(POSIX_TICK_SLEEP, step.action);
    TEST_ASSERT_TRUE(step.given_back);
    TEST_ASSERT_EQUAL_UINT64(0U, step.dropped);
    TEST_ASSERT_EQUAL_UINT64(6U * PERIOD_NS, step.deadline_ns);
    TEST_ASSERT_EQUAL_UINT64(7U, plan.next);
    /* A give-back does not move the grid. */
    TEST_ASSERT_EQUAL_UINT64(0U, plan.base_ns);

    step = posix_tick_plan_at_deadline(&plan, 6U);
    TEST_ASSERT_EQUAL(POSIX_TICK_RAISE, step.action);
    TEST_ASSERT_FALSE(step.given_back);
}

/* ------------------------------------------------------------- modelled orders of events */

static void test_ticks_taken_on_time_need_nothing_made_up_or_given_back(void)
{
    const fate_kind_t kinds[] = {AFTER_READ, AT_ONCE};

    for (size_t i = 0U; i < (sizeof(kinds) / sizeof(kinds[0])); i++) {
        const one_off_t context = {kinds[i], 0U, 0U, 0U, 0U, 0U};
        const scenario_t scenario = {one_off, &context, 2000U, 0U, 0U};
        const outcome_t out = run(&scenario);

        TEST_ASSERT_EQUAL_UINT64(0U, out.catch_ups);
        TEST_ASSERT_EQUAL_UINT64(0U, out.give_backs);
        TEST_ASSERT_EQUAL_UINT64(0U, out.dropped);
        TEST_ASSERT_EQUAL_UINT64(0U, out.early_ticks);
        TEST_ASSERT_EQUAL_INT64(0, out.max_lead);
        /* Behind only from each boundary until the tick raised there is taken. */
        TEST_ASSERT_EQUAL_INT64(-1, out.min_lead);
        TEST_ASSERT_LESS_OR_EQUAL_UINT64(WAKE_NS + TAKE_AFTER_READ_NS, out.latest_ns);
        TEST_ASSERT_EQUAL_UINT64(out.first_base_ns, out.last_base_ns);
        assert_on_the_grid_at_the_end(&out, "steady state");
    }
}

/* The next raise is taken after the read, so the entry read after it sees two ticks missing. */
static void test_a_lost_tick_is_made_up_at_once_and_nothing_is_given_back(void)
{
    const one_off_t context = {AFTER_READ, 100U, 0U, 0U, 0U, 0U};
    const scenario_t scenario = {one_off, &context, 300U, 0U, 0U};
    const outcome_t out = run(&scenario);

    TEST_ASSERT_EQUAL_UINT64(1U, out.catch_ups);
    TEST_ASSERT_EQUAL_UINT64(0U, out.give_backs);
    TEST_ASSERT_EQUAL_UINT64(0U, out.dropped);
    TEST_ASSERT_EQUAL_UINT64(0U, out.early_ticks);
    TEST_ASSERT_EQUAL_INT64(0, out.max_lead);
    /* The count reaches 100 with the next raise, which comes a period late, and 101 with the
     * replacement raised right after. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(PERIOD_NS + WAKE_NS + TAKE_AFTER_READ_NS, out.latest_ns);
    assert_on_the_grid_at_the_end(&out, "a lost tick");
}

static void test_a_tick_taken_late_within_a_period_is_no_lag(void)
{
    const one_off_t context = {AFTER_READ, 100U, PERIOD_NS / 2U, 0U, 0U, 0U};
    const scenario_t scenario = {one_off, &context, 300U, 0U, 0U};
    const outcome_t out = run(&scenario);

    TEST_ASSERT_EQUAL_UINT64(0U, out.catch_ups);
    TEST_ASSERT_EQUAL_UINT64(0U, out.give_backs);
    TEST_ASSERT_EQUAL_UINT64(0U, out.early_ticks);
    assert_on_the_grid_at_the_end(&out, "a tick half a period late");
}

/* The checks of a tick made up for raise 100, which was only late by `late_ns`. */
static void assert_given_back_at_the_next_boundary(const outcome_t *out, uint64_t late_ns,
                                                   const char *what)
{
    /* One tick early, the late one, or none where it is taken just as the next tick falls due
     * (before the boundary's read, which then skips that tick's raise); the count back on the
     * grid at the first boundary after the late tick, and never ahead again. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT64_MESSAGE(1U, out->early_ticks, what);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64_MESSAGE(AHEAD_BOUND_NS, out->longest_ahead_ns, what);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64_MESSAGE(raise_ns_on_time(100U) + late_ns + AHEAD_BOUND_NS,
                                             out->ahead_ends_ns, what);
    TEST_ASSERT_LESS_OR_EQUAL_INT64_MESSAGE(1, out->max_lead, what);
    assert_on_the_grid_at_the_end(out, what);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(1U, out->catch_ups, what);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(1U, out->give_backs, what);
    /* A give-back is no dropped tick: the grid stays where it was. */
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0U, out->dropped, what);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(out->first_base_ns, out->last_base_ns, what);
}

/*
 * Bug 011. Raise 100 is pending on a thread that was switched out and taken more than a period
 * late; the plan made it up in the meantime, so with the late one the kernel holds one tick
 * too many. Every other raise is taken just after the plan has read the count, as on the
 * laptop: read right after a raise, the count one ahead then looks on time. The count has to
 * be back on the grid at the first boundary after the late tick, within AHEAD_BOUND_NS, and
 * never be ahead again. Every delay from 1.1 to 6 periods in steps of 0.05, so that the late
 * tick lands at every phase of a period.
 */
static void test_a_tick_made_up_for_one_that_was_only_late_is_given_back_at_the_next_boundary(void)
{
    char what[64];

    for (uint64_t late_ns = (11U * PERIOD_NS) / 10U; late_ns <= (6U * PERIOD_NS);
         late_ns += PERIOD_NS / 20U) {
        const one_off_t context = {AFTER_READ, 100U, late_ns, 0U, 0U, 0U};
        const scenario_t scenario = {one_off, &context, 200U, 0U, 0U};
        const outcome_t out = run(&scenario);

        (void)snprintf(what, sizeof(what), "the tick %llu us late",
                       (unsigned long long)(late_ns / 1000U));
        assert_given_back_at_the_next_boundary(&out, late_ns, what);
    }
}

/* The same, but from the late tick on every raise is taken before the plan reads the count:
 * the one case in which the count read right after a raise shows the tick too many. The
 * give-back of bug 011 saw it there, after the next raise had made a second tick early; at the
 * boundary it comes before that raise. */
static void test_a_tick_too_many_is_given_back_too_when_the_raises_after_it_are_taken_at_once(void)
{
    char what[64];

    for (uint64_t late_ns = (11U * PERIOD_NS) / 10U; late_ns <= (6U * PERIOD_NS);
         late_ns += PERIOD_NS / 20U) {
        const one_off_t context = {AFTER_READ, 100U, late_ns,
                                   0U,         0U,   raise_ns_on_time(100U) + late_ns};
        const scenario_t scenario = {one_off, &context, 200U, 0U, 0U};
        const outcome_t out = run(&scenario);

        (void)snprintf(what, sizeof(what), "the tick %llu us late",
                       (unsigned long long)(late_ns / 1000U));
        assert_given_back_at_the_next_boundary(&out, late_ns, what);
    }
}

/* Every raise taken before the plan reads the count: a late tick looks like the one just
 * raised, nothing is made up for it, and the count is never ahead. */
static void test_a_late_tick_among_raises_taken_at_once_is_waited_for(void)
{
    const one_off_t context = {AT_ONCE, 100U, (25U * PERIOD_NS) / 10U, 0U, 0U, 0U};
    const scenario_t scenario = {one_off, &context, 200U, 0U, 0U};
    const outcome_t out = run(&scenario);

    TEST_ASSERT_EQUAL_UINT64(0U, out.catch_ups);
    TEST_ASSERT_EQUAL_UINT64(0U, out.give_backs);
    TEST_ASSERT_EQUAL_UINT64(0U, out.early_ticks);
    assert_on_the_grid_at_the_end(&out, "a late tick, the others taken at once");
}

/* Two ticks late at once, on two threads, each made up, and both taken in the same period: the
 * count is two ahead, and one is given back at each of the next two boundaries. */
static void test_two_late_ticks_made_up_are_given_back_one_boundary_each(void)
{
    const one_off_t context = {
        AFTER_READ, 100U, (33U * PERIOD_NS) / 10U, 101U, (24U * PERIOD_NS) / 10U, 0U};
    const scenario_t scenario = {one_off, &context, 300U, 0U, 0U};
    const outcome_t out = run(&scenario);

    TEST_ASSERT_EQUAL_UINT64(2U, out.catch_ups);
    TEST_ASSERT_EQUAL_UINT64(2U, out.give_backs);
    TEST_ASSERT_EQUAL_UINT64(0U, out.dropped);
    TEST_ASSERT_EQUAL_INT64(2, out.max_lead);
    TEST_ASSERT_EQUAL_UINT64(2U, out.early_ticks);
    TEST_ASSERT_EQUAL_UINT64(2U, out.early_row);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(2U * AHEAD_BOUND_NS, out.longest_ahead_ns);
    assert_on_the_grid_at_the_end(&out, "two late ticks");
}

/*
 * Bug 003's suspended scheduler. While a task holds the scheduler suspended the kernel's tick
 * count stands still, but the kernel takes every tick interrupt and its tick hook counts it;
 * that count is what posix_tick.c passes to the plan, and the kernel adds the noted ticks to
 * its tick count on resume. The plan therefore sees 20 ms of suspension as 20 ordinary ticks:
 * nothing made up, nothing given back, and the tick count is on the grid after the resume.
 * (Whether posix_tick.c passes the tick hook's count is its own business, and bug 003's own
 * test, plat_selftest suspend 20, checks that.)
 */
static void test_a_suspended_scheduler_is_no_lag_for_the_count_of_the_tick_hook(void)
{
    const one_off_t context = {AFTER_READ, 0U, 0U, 0U, 0U, 0U};
    const scenario_t scenario = {one_off, &context, 200U, 0U, 0U};
    const outcome_t out = run(&scenario);
    const uint64_t suspended_from = T0_NS + (50U * PERIOD_NS);
    const uint64_t resumed = suspended_from + (20U * PERIOD_NS);
    /* The kernel's tick count after the resume: what it had at the suspension plus the ticks
     * the hook counted while it lasted, which xTaskResumeAll() adds. */
    const uint64_t at_suspension = counted_at(suspended_from);
    const uint64_t noted = counted_at(resumed) - at_suspension;

    TEST_ASSERT_EQUAL_UINT64(20U, noted);
    TEST_ASSERT_EQUAL_UINT64(((resumed - out.first_base_ns) / PERIOD_NS) + 1U,
                             at_suspension + noted);
    TEST_ASSERT_EQUAL_UINT64(0U, out.catch_ups);
    TEST_ASSERT_EQUAL_UINT64(0U, out.give_backs);
    TEST_ASSERT_EQUAL_UINT64(0U, out.dropped);
    assert_on_the_grid_at_the_end(&out, "a suspended scheduler");
}

/* The tick thread itself stalls for 20.4 periods after raise 100: a lag of 20 at its next read,
 * beyond what is made up. The plan gives up all of it but one tick, which it takes for the one
 * just raised, and moves the base. Raise 100 was taken during the stall, so that tick is
 * missing as well: it is made up once, and nothing is given back. */
static void test_a_tick_thread_stalled_beyond_the_limit_gives_ticks_up_and_moves_the_base(void)
{
    const one_off_t context = {AFTER_READ, 0U, 0U, 0U, 0U, 0U};
    const scenario_t scenario = {one_off, &context, 300U, 100U, (204U * PERIOD_NS) / 10U};
    const outcome_t out = run(&scenario);

    TEST_ASSERT_EQUAL_UINT64(19U, out.dropped);
    TEST_ASSERT_EQUAL_UINT64(out.first_base_ns + (19U * PERIOD_NS), out.last_base_ns);
    TEST_ASSERT_EQUAL_UINT64(1U, out.catch_ups);
    TEST_ASSERT_EQUAL_UINT64(0U, out.give_backs);
    TEST_ASSERT_EQUAL_UINT64(0U, out.early_ticks);
    TEST_ASSERT_EQUAL_INT64(0, out.max_lead);
    assert_on_the_grid_at_the_end(&out, "a stalled tick thread");
}

/* The tick thread stalls for 5.4 periods after raise 100, which was taken during the stall: the
 * five ticks missing are made up, 100 us apart, and none is given up. */
static void test_a_tick_thread_stalled_within_the_limit_has_its_ticks_made_up(void)
{
    const one_off_t context = {AFTER_READ, 0U, 0U, 0U, 0U, 0U};
    const scenario_t scenario = {one_off, &context, 300U, 100U, (54U * PERIOD_NS) / 10U};
    const outcome_t out = run(&scenario);

    TEST_ASSERT_EQUAL_UINT64(0U, out.dropped);
    TEST_ASSERT_EQUAL_UINT64(5U, out.catch_ups);
    TEST_ASSERT_EQUAL_UINT64(0U, out.give_backs);
    TEST_ASSERT_EQUAL_UINT64(0U, out.early_ticks);
    assert_on_the_grid_at_the_end(&out, "a short stall of the tick thread");
}

/* The task threads stand still for 15.5 periods: the first tick raised then is pending on the
 * stalled thread, every later one merges with it. The plan makes up and then gives up what it
 * takes for lost; when the threads run again the pending tick is taken. However that falls,
 * at most one tick is early, the count is ahead for less than AHEAD_BOUND_NS, and every tick
 * period is counted or given up. */
static void test_task_threads_stalled_beyond_the_limit_leave_the_count_on_the_grid(void)
{
    char what[64];

    for (uint64_t phase = 0U; phase < PERIOD_NS; phase += PERIOD_NS / 10U) {
        const task_stall_t context = {T0_NS + (100U * PERIOD_NS) + phase,
                                      T0_NS + (115U * PERIOD_NS) + (PERIOD_NS / 2U) + phase};
        const scenario_t scenario = {task_stall, &context, 300U, 0U, 0U};
        const outcome_t out = run(&scenario);

        (void)snprintf(what, sizeof(what), "stall from %llu us into a period",
                       (unsigned long long)(phase / 1000U));
        TEST_ASSERT_GREATER_THAN_UINT64_MESSAGE(0U, out.dropped, what);
        TEST_ASSERT_LESS_OR_EQUAL_UINT64_MESSAGE(1U, out.early_ticks, what);
        TEST_ASSERT_LESS_OR_EQUAL_INT64_MESSAGE(1, out.max_lead, what);
        TEST_ASSERT_LESS_OR_EQUAL_UINT64_MESSAGE(AHEAD_BOUND_NS, out.longest_ahead_ns, what);
        assert_on_the_grid_at_the_end(&out, what);
    }
}

/* Raise `lost` is never taken, and every raise before `after_read_from_ns` is taken before the
 * plan reads the count, every later one after it. */
typedef struct {
    uint64_t lost;
    uint64_t after_read_from_ns;
} lost_among_at_once_t;

static fate_t lost_among_at_once(uint64_t raise, uint64_t raise_ns, const void *context)
{
    const lost_among_at_once_t *c = context;

    if (raise == c->lost) {
        return fate_of(LOST, 0U);
    }

    return fate_of((raise_ns < c->after_read_from_ns) ? AT_ONCE : AFTER_READ, 0U);
}

/*
 * The limit of what the read right after a raise can see (posix_tick_plan.h). A tick is lost
 * while every raise after it is taken before the plan reads the count: the count then trails
 * the ticks due by just the one the plan allows for the raise it has just made, and nothing is
 * made up. The count stays one tick behind, every tick from the lost one on is taken a period
 * late, until the first raise taken after the read; that read sees two missing and the lost
 * tick is made up. Nothing is early, nothing is given back, and the count is never more than
 * one tick behind beyond the raise just made.
 */
static void test_a_lost_tick_among_raises_taken_at_once_is_made_up_at_the_first_taken_after(void)
{
    const lost_among_at_once_t context = {100U, T0_NS + (150U * PERIOD_NS)};
    const scenario_t scenario = {lost_among_at_once, &context, 300U, 0U, 0U};
    const outcome_t out = run(&scenario);

    TEST_ASSERT_EQUAL_UINT64(1U, out.catch_ups);
    TEST_ASSERT_EQUAL_UINT64(0U, out.give_backs);
    TEST_ASSERT_EQUAL_UINT64(0U, out.dropped);
    TEST_ASSERT_EQUAL_UINT64(0U, out.early_ticks);
    TEST_ASSERT_EQUAL_INT64(0, out.max_lead);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(-2, out.min_lead);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(PERIOD_NS + WAKE_NS + TAKE_AFTER_READ_NS, out.latest_ns);
    assert_on_the_grid_at_the_end(&out, "a lost tick among raises taken at once");
}

/* Random orders of events, the same on every run (a fixed seed for each). */
typedef struct {
    uint64_t seed;
    uint64_t quiet_from_ns; /* every raise from then on is taken just after the read */
} shuffled_t;

static uint64_t s_rng;
static uint64_t s_late_fates;

static uint32_t next_random(void)
{
    /* xorshift64* */
    s_rng ^= s_rng >> 12U;
    s_rng ^= s_rng << 25U;
    s_rng ^= s_rng >> 27U;
    return (uint32_t)((s_rng * 2685821657736338717ULL) >> 32U);
}

static fate_t shuffled(uint64_t raise, uint64_t raise_ns, const void *context)
{
    const shuffled_t *c = context;
    fate_t fate = fate_of(AFTER_READ, 0U);

    if (raise == 1U) {
        s_rng = c->seed;
        s_late_fates = 0U;
    }

    if (raise_ns < c->quiet_from_ns) {
        const uint32_t pick = next_random() % 1000U;

        if (pick < 40U) {
            /* Pending on a switched-out thread for 0.2 to 6.2 periods. */
            fate = fate_of(LATE, (PERIOD_NS / 5U) + (next_random() % (6U * PERIOD_NS)));
            s_late_fates++;
        } else if (pick < 60U) {
            fate = fate_of(LOST, 0U);
        } else if (pick < 110U) {
            fate = fate_of(AT_ONCE, 0U);
        } else {
            /* the common case */
        }
    }

    return fate;
}

/*
 * 300 runs of 400 periods, each raise lost, late, taken before the read or after it at random,
 * and the tick thread stalled once, for up to 12 periods, at a random raise. Whatever the
 * order: a tick is early only if a late one put the count ahead (one early tick for each late
 * tick at most: a count left ahead makes every tick after it early), the plan gives back no
 * more ticks than it made up, and once the events stop (the last 40 periods, every raise taken
 * after the read) the count is on the grid with every tick period counted or given up.
 */
static void test_random_orders_of_events_leave_no_tick_early_but_late_ones_and_end_on_the_grid(void)
{
    char what[64];

    for (uint64_t seed = 1U; seed <= 300U; seed++) {
        const shuffled_t context = {seed * 0x9E3779B97F4A7C15ULL, T0_NS + (360U * PERIOD_NS)};

        s_rng = context.seed;
        const uint64_t stall_raise = 20U + (next_random() % 300U);
        const uint64_t stall_ns = next_random() % (12U * PERIOD_NS);
        const scenario_t scenario = {shuffled, &context, 400U, stall_raise, stall_ns};
        const outcome_t out = run(&scenario);

        (void)snprintf(what, sizeof(what), "seed %llu, %llu late", (unsigned long long)seed,
                       (unsigned long long)s_late_fates);
        TEST_ASSERT_LESS_OR_EQUAL_UINT64_MESSAGE(s_late_fates, out.early_ticks, what);
        TEST_ASSERT_LESS_OR_EQUAL_UINT64_MESSAGE(out.catch_ups, out.give_backs, what);
        TEST_ASSERT_LESS_OR_EQUAL_UINT64_MESSAGE(out.early_row * AHEAD_BOUND_NS,
                                                 out.longest_ahead_ns, what);
        assert_on_the_grid_at_the_end(&out, what);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_first_sleep_is_to_the_boundary_of_the_second_tick);
    RUN_TEST(test_the_tick_just_raised_is_not_a_lag_and_one_more_is);
    RUN_TEST(test_a_lag_beyond_the_limit_is_given_up_but_the_tick_just_raised);
    RUN_TEST(test_at_a_boundary_a_tick_the_kernel_already_holds_is_not_raised);
    RUN_TEST(test_ticks_taken_on_time_need_nothing_made_up_or_given_back);
    RUN_TEST(test_a_lost_tick_is_made_up_at_once_and_nothing_is_given_back);
    RUN_TEST(test_a_tick_taken_late_within_a_period_is_no_lag);
    RUN_TEST(test_a_tick_made_up_for_one_that_was_only_late_is_given_back_at_the_next_boundary);
    RUN_TEST(test_a_tick_too_many_is_given_back_too_when_the_raises_after_it_are_taken_at_once);
    RUN_TEST(test_a_late_tick_among_raises_taken_at_once_is_waited_for);
    RUN_TEST(test_two_late_ticks_made_up_are_given_back_one_boundary_each);
    RUN_TEST(test_a_suspended_scheduler_is_no_lag_for_the_count_of_the_tick_hook);
    RUN_TEST(test_a_tick_thread_stalled_beyond_the_limit_gives_ticks_up_and_moves_the_base);
    RUN_TEST(test_a_tick_thread_stalled_within_the_limit_has_its_ticks_made_up);
    RUN_TEST(test_task_threads_stalled_beyond_the_limit_leave_the_count_on_the_grid);
    RUN_TEST(test_a_lost_tick_among_raises_taken_at_once_is_made_up_at_the_first_taken_after);
    RUN_TEST(test_random_orders_of_events_leave_no_tick_early_but_late_ones_and_end_on_the_grid);
    return UNITY_END();
}
