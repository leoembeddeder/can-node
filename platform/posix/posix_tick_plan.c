/* SPDX-License-Identifier: MIT */
/* The decisions of the locked tick (posix_tick_plan.h). */
#include "posix_tick_plan.h"

void posix_tick_plan_init(posix_tick_plan_t *plan, uint64_t base_ns, uint64_t period_ns)
{
    plan->base_ns = base_ns;
    plan->period_ns = period_ns;
    plan->next = 2U;
}

uint64_t posix_tick_plan_due_ns(const posix_tick_plan_t *plan, uint64_t tick)
{
    return plan->base_ns + ((tick - 1U) * plan->period_ns);
}

static posix_tick_step_t sleep_to_next(const posix_tick_plan_t *plan)
{
    const posix_tick_step_t step = {.action = POSIX_TICK_SLEEP,
                                    .deadline_ns = posix_tick_plan_due_ns(plan, plan->next),
                                    .dropped = 0U,
                                    .given_back = false};

    return step;
}

posix_tick_step_t posix_tick_plan_after_raise(posix_tick_plan_t *plan, uint64_t now_ns,
                                              uint64_t counted)
{
    /* The ticks due by now, the one just raised included (the first was raised at the base). */
    const uint64_t elapsed_ns = (now_ns > plan->base_ns) ? (now_ns - plan->base_ns) : 0U;
    uint64_t expected = (elapsed_ns / plan->period_ns) + 1U;
    uint64_t dropped = 0U;

    /* The tick just raised may not have been taken yet: behind only by more than that one. */
    if ((counted + 1U) < expected) {
        const uint64_t lag = expected - counted;

        if (lag <= (uint64_t)POSIX_TICK_MAX_CATCH_UP_TICKS) {
            const posix_tick_step_t step = {.action = POSIX_TICK_CATCH_UP,
                                            .deadline_ns = 0U,
                                            .dropped = 0U,
                                            .given_back = false};

            return step;
        }

        /* The task threads did not run for too long to make up: shift the base instead of
         * bursting. Every tick of the lag is given up but the one just raised. */
        dropped = lag - 1U;
        plan->base_ns += dropped * plan->period_ns;
        expected -= dropped;
    }

    /* Sleep to the boundary of the next tick. Whether the port raises it there is decided at
     * the boundary, once the tick just raised has had its period (at_deadline below): read
     * now, a count one ahead whose last raise is not taken yet looks on time. */
    plan->next = expected + 1U;

    posix_tick_step_t step = sleep_to_next(plan);

    step.dropped = dropped;
    return step;
}

posix_tick_step_t posix_tick_plan_at_deadline(posix_tick_plan_t *plan, uint64_t counted)
{
    posix_tick_step_t step = {
        .action = POSIX_TICK_RAISE, .deadline_ns = 0U, .dropped = 0U, .given_back = false};

    /* Settled: the raise before had a whole period to be taken. A kernel that already holds
     * the tick about to be raised took one too many; raising it would put the count one ahead
     * of the clock. */
    if (counted >= plan->next) {
        plan->next++;
        step = sleep_to_next(plan);
        step.given_back = true;
    }

    return step;
}
