# SPDX-License-Identifier: MIT
"""fcn_tools.sched, the schedulability check of the task table, against answers that do not come
from it:

  - task sets whose response times are worked out by hand below, step by step;
  - the examples of Audsley, Burns, Richardson, Tindell and Wellings, "Applying new scheduling
    theory to static priority pre-emptive scheduling", Software Engineering Journal 8(5),
    1993, checked against the text of the paper;
  - a simulation of the schedule itself, one time unit at a time, on random task sets: the
    analysis must never be below the worst response time the simulation sees, and must equal
    it where the theory says the analysis is exact;
  - the command line: exit status, the JSON report, and the inputs it refuses.

Times are microseconds in the module; the hand-worked sets use small numbers, which are
microseconds too.
"""
from __future__ import annotations

import collections
import dataclasses
import itertools
import json
import math
import os
import random
import subprocess
import sys
from fractions import Fraction
from pathlib import Path

import pytest

import fcn_tools
from fcn_tools import sched
from fcn_tools.sched import CriticalSection, Task, TaskTable

TOOLS = Path(fcn_tools.__file__).resolve().parents[1]


def task(name: str, priority: int, period: int, wcet: int, deadline: int | None = None,
         jitter: int = 0, sections: dict[str, int] | None = None,
         kind: str = sched.PERIODIC, nonpreemptive: int = 0) -> Task:
    return Task(name=name, priority=priority, kind=kind, period_us=period,
                deadline_us=period if deadline is None else deadline, wcet_us=wcet,
                jitter_us=jitter,
                critical_sections=tuple(CriticalSection(resource, length)
                                        for resource, length in (sections or {}).items()),
                nonpreemptive_us=nonpreemptive)


def analyse(*tasks: Task) -> sched.Analysis:
    return sched.analyse(TaskTable(tuple(tasks)))


def responses(analysis: sched.Analysis) -> dict[str, int | None]:
    return {result.task.name: result.response_us for result in analysis.results}


def blockings(analysis: sched.Analysis) -> dict[str, int]:
    return {result.task.name: result.blocking.blocking_us for result in analysis.results}


# ------------------------------------------------------------------------------------------
# Worked out by hand
#
# Notation: w is the window of the job being analysed, measured from the critical instant;
# each line of an iteration is w = B + (q + 1) C + the sum over higher-priority tasks j of
# ceil((w + J_j) / T_j) C_j, evaluated at the previous line's w, until it repeats. The module
# starts each iteration at B + C + the sum of the C_j (every task released at the critical
# instant counted once), so that is the first line. R = w - q T + J.
# ------------------------------------------------------------------------------------------


def test_three_independent_tasks_that_fail_both_bounds_but_meet_their_deadlines():
    # a: T 10, C 3, priority 3      b: T 15, C 4, priority 2      c: T 35, C 10, priority 1
    #
    # R_a = 3: nothing preempts it.
    # R_b: w = 4 + 3 = 7; 4 + ceil(7/10) 3 = 7.                                     R_b = 7
    # R_c: w = 10 + 3 + 4 = 17
    #      10 + ceil(17/10) 3 + ceil(17/15) 4 = 10 + 6 + 8 = 24
    #      10 + ceil(24/10) 3 + ceil(24/15) 4 = 10 + 9 + 8 = 27
    #      10 + ceil(27/10) 3 + ceil(27/15) 4 = 10 + 9 + 8 = 27                    R_c = 27
    # Each window ends before the task's next release (7 <= 15, 27 <= 35): one job each.
    #
    # U = 3/10 + 4/15 + 10/35 = 0.8524 > 3(2^(1/3) - 1) = 0.7798: Liu and Layland fails.
    # (1 + 3/10)(1 + 4/15)(1 + 10/35) = 1.3 * 1.2667 * 1.2857 = 2.1171 > 2: hyperbolic fails.
    # Both are sufficient tests only: the set is schedulable all the same.
    result = analyse(task("a", 3, 10, 3), task("b", 2, 15, 4), task("c", 1, 35, 10))
    assert responses(result) == {"a": 3, "b": 7, "c": 27}
    assert result.schedulable
    assert result.utilisation == Fraction(3, 10) + Fraction(4, 15) + Fraction(10, 35)
    assert result.liu_layland.applies and not result.liu_layland.holds
    assert result.liu_layland.verdict == "inconclusive"
    assert result.liu_layland.limit == pytest.approx(0.779763, abs=1e-6)
    assert result.hyperbolic.applies and not result.hyperbolic.holds
    assert float(result.hyperbolic.value) == pytest.approx(2.117143, abs=1e-6)
    assert result.rate_monotonic and result.deadline_monotonic
    assert result.deadline_monotonic_results is None
    assert [len(r.response.jobs) for r in result.results] == [1, 1, 1]


def test_a_set_that_both_bounds_accept():
    # a: T 4, C 2 (U 0.5), priority 2      b: T 10, C 3 (U 0.3), priority 1
    # U = 0.8 <= 2(2^(1/2) - 1) = 0.8284 and (1.5)(1.3) = 1.95 <= 2.
    # R_a = 2. R_b: w = 3 + 2 = 5; 3 + ceil(5/4) 2 = 7; 3 + ceil(7/4) 2 = 7.         R_b = 7
    result = analyse(task("a", 2, 4, 2), task("b", 1, 10, 3))
    assert result.liu_layland.verdict == "schedulable"
    assert result.hyperbolic.verdict == "schedulable"
    assert responses(result) == {"a": 2, "b": 7}


def test_the_hyperbolic_bound_accepts_a_product_of_exactly_two_that_liu_and_layland_rejects():
    # a: T 4, C 1 (U 1/4), priority 2      b: T 5, C 3 (U 3/5), priority 1
    # U = 0.85 > 0.8284, but (1 + 1/4)(1 + 3/5) = 5/4 * 8/5 = 2 exactly: "<= 2" holds.
    # R_a = 1. R_b: w = 3 + 1 = 4; 3 + ceil(4/4) 1 = 4.                               R_b = 4
    result = analyse(task("a", 2, 4, 1), task("b", 1, 5, 3))
    assert result.hyperbolic.value == 2
    assert result.hyperbolic.verdict == "schedulable"
    assert result.liu_layland.verdict == "inconclusive"
    assert responses(result) == {"a": 1, "b": 4}


def test_a_single_task_using_the_whole_processor_meets_the_liu_and_layland_bound_of_one():
    # n = 1: the bound is 1(2^1 - 1) = 1, and U = 5/5 = 1 meets it with equality. R = C = 5.
    result = analyse(task("only", 1, 5, 5))
    assert result.liu_layland.limit == 1.0
    assert result.liu_layland.verdict == "schedulable"
    assert responses(result) == {"only": 5}


def test_direct_and_push_through_blocking_under_priority_inheritance():
    # high:   priority 4, T 10, C 2, holds r for 1
    # mid:    priority 3, T 20, C 4, holds nothing
    # low:    priority 2, T 40, C 8, holds r for 3 and s for 1
    # lowest: priority 1, T 80, C 4, holds s for 2
    #
    # B_high = 3: low holds r, which high takes itself (direct blocking).
    # B_mid  = 3: low holds r, which a task above mid takes; low inherits high's priority and
    #        runs ahead of mid (push-through). s blocks nobody at mid's level or above: only
    #        low and lowest take it.
    # B_low  = 2: lowest holds s, which low takes (direct).
    # B_lowest = 0: nobody is below it.
    #
    # R_high: w = 3 + 2 = 5.                                                       R_high = 5
    # R_mid:  w = 3 + 4 + 2 = 9; 7 + ceil(9/10) 2 = 9.                              R_mid = 9
    # R_low:  w = 2 + 8 + 2 + 4 = 16; 10 + ceil(16/10) 2 + ceil(16/20) 4 = 10 + 4 + 4 = 18;
    #         10 + ceil(18/10) 2 + ceil(18/20) 4 = 18.                              R_low = 18
    # R_lowest: w = 4 + 2 + 4 + 8 = 18; 4 + ceil(18/10) 2 + ceil(18/20) 4 + ceil(18/40) 8
    #         = 4 + 4 + 4 + 8 = 20; again 20.                                    R_lowest = 20
    result = analyse(task("high", 4, 10, 2, sections={"r": 1}), task("mid", 3, 20, 4),
                     task("low", 2, 40, 8, sections={"r": 3, "s": 1}),
                     task("lowest", 1, 80, 4, sections={"s": 2}))
    assert blockings(result) == {"high": 3, "mid": 3, "low": 2, "lowest": 0}
    assert responses(result) == {"high": 5, "mid": 9, "low": 18, "lowest": 20}
    assert result.result("mid").blocking.per_resource == (sched.Blocker("r", "low", 3),)
    # Blocking breaks an assumption of both utilisation bounds.
    assert not result.liu_layland.applies and not result.hyperbolic.applies
    assert any("blocking" in reason for reason in result.liu_layland.not_applicable)


def test_a_lower_priority_task_blocks_at_most_once_even_when_it_holds_two_resources():
    # high: priority 3, T 10, C 2, holds r1 for 1 and r2 for 1
    # mid:  priority 2, T 20, C 4
    # low:  priority 1, T 40, C 8, holds r1 for 3 and r2 for 4
    #
    # Once per resource would allow 3 + 4 = 7. But low holds one mutex at a time (sections are
    # not nested), and once it has given one back it runs at its own priority, below high's
    # and mid's, until they are done: it blocks each of them once, for its longest section.
    # B_high = B_mid = min(7, 4) = 4.
    # R_high = 2 + 4 = 6. R_mid: w = 4 + 4 + 2 = 10; 8 + ceil(10/10) 2 = 10.        R_mid = 10
    # R_low: w = 8 + 2 + 4 = 14; 8 + ceil(14/10) 2 + ceil(14/20) 4 = 16; again 16.  R_low = 16
    result = analyse(task("high", 3, 10, 2, sections={"r1": 1, "r2": 1}), task("mid", 2, 20, 4),
                     task("low", 1, 40, 8, sections={"r1": 3, "r2": 4}))
    assert blockings(result) == {"high": 4, "mid": 4, "low": 0}
    assert result.result("high").blocking.per_resource_us == 7
    assert result.result("high").blocking.per_task_us == 4
    assert responses(result) == {"high": 6, "mid": 10, "low": 16}


def test_two_lower_priority_tasks_holding_two_resources_block_once_each():
    # As above, but r1 and r2 are held by two different tasks below high: low1 holds r1 for 3,
    # low2 holds r2 for 4. low2 can take r2 while low1 is preempted inside its section on r1,
    # so high can find both taken: B_high = min(3 + 4, 3 + 4) = 7. R_high = 2 + 7 = 9.
    result = analyse(task("high", 3, 10, 2, sections={"r1": 1, "r2": 1}),
                     task("low1", 2, 40, 8, sections={"r1": 3}),
                     task("low2", 1, 40, 8, sections={"r2": 4}))
    assert blockings(result)["high"] == 7
    assert responses(result)["high"] == 9


def test_a_non_preemptive_stretch_of_a_lower_priority_task_blocks_every_task_above_it():
    # top: priority 3, T 10, C 2, takes no mutex
    # mid: priority 2, T 20, C 3, holds m for 2
    # low: priority 1, T 50, C 6, of which 4 inside taskENTER_CRITICAL; holds m for 1
    #
    # B_top = 4: low may be inside its non-preemptive stretch when top is released. m blocks
    #   nobody at top's level: neither top nor a task above it takes m.
    # B_mid = min(4 + 1, 4) = 4: low's non-preemptive stretch (4) or its section on m (1), but
    #   only one of them: low blocks mid once.
    # B_low = 0.
    # R_top = 2 + 4 = 6.
    # R_mid: w = 4 + 3 + 2 = 9; 7 + ceil(9/10) 2 = 9.                               R_mid = 9
    # R_low: w = 6 + 2 + 3 = 11; 6 + ceil(11/10) 2 + ceil(11/20) 3 = 6 + 4 + 3 = 13;
    #        6 + ceil(13/10) 2 + ceil(13/20) 3 = 13.                               R_low = 13
    tasks = (task("top", 3, 10, 2), task("mid", 2, 20, 3, sections={"m": 2}),
             task("low", 1, 50, 6, sections={"m": 1}, nonpreemptive=4))
    result = sched.analyse(TaskTable(tasks))
    assert blockings(result) == {"top": 4, "mid": 4, "low": 0}
    assert responses(result) == {"top": 6, "mid": 9, "low": 13}
    assert result.result("top").blocking.per_resource == (
        sched.Blocker(sched.NONPREEMPTIVE, "low", 4),)
    text = sched.format_report(result, "t")
    assert "  top: B = 4, once per resource: low non-preemptive 4\n" in text
    assert ("  mid: B = 4, once per lower-priority task: low non-preemptive 4 (once per "
            "resource would be 5)\n") in text
    # The simulated schedule with low inside its stretch at the critical instant agrees.
    simulated = simulate(list(tasks), worst_case_releases, holders={"low": NONPREEMPTIVE_SECTION})
    assert simulated == {"top": 6, "mid": 9, "low": 13}


def test_release_jitter_of_a_higher_priority_task_and_of_the_task_itself():
    # a: priority 2, T 10, C 2, J 3       b: priority 1, T 12, D 12, C 6, J 1
    #
    # R_a = w + J_a = 2 + 3 = 5.
    # R_b: w = 6 + 2 = 8
    #      6 + ceil((8 + 3)/10) 2 = 6 + 4 = 10      a's second job, released 10 - 3 = 7 after
    #                                               its first, falls into the window
    #      6 + ceil((10 + 3)/10) 2 = 10
    #      R_b = w + J_b = 10 + 1 = 11 <= 12. The window ends before the next job of b can be
    #      released: 10 <= 12 - 1.
    # Without a's jitter in the ceiling w would stay at 8, and without J_b, R_b would be 10.
    result = analyse(task("a", 2, 10, 2, jitter=3), task("b", 1, 12, 6, jitter=1))
    assert responses(result) == {"a": 5, "b": 11}
    assert result.schedulable
    assert not result.liu_layland.applies  # jitter breaks an assumption of the bounds


def test_the_worst_response_can_belong_to_a_later_job_of_the_busy_period():
    # a: priority 2, T 70, C 26        b: priority 1, T 100, C 62 (U = 0.371 + 0.62 = 0.991)
    #
    # Job q of b completes at the smallest w = (q + 1) 62 + ceil(w/70) 26; it arrived at
    # 100 q, so its response is w - 100 q. The busy period goes on while w > 100 (q + 1).
    #   q = 0: w = 62 + 26 = 88; 62 + 2*26 = 114; 62 + ceil(114/70) 26 = 114.   R = 114
    #   q = 1: w = 114 + 62 = 176; 124 + ceil(176/70) 26 = 202; 124 + 3*26 = 202.  R = 102
    #   q = 2: w = 264; 186 + 4*26 = 290; 186 + ceil(290/70) 26 = 316; 186 + 5*26 = 316.
    #                                                                            R = 116
    #   q = 3: w = 378; 248 + 6*26 = 404; 248 + ceil(404/70) 26 = 404.            R = 104
    #   q = 4: w = 466; 310 + 7*26 = 492; 310 + ceil(492/70) 26 = 518; 310 + 8*26 = 518.
    #                                                                            R = 118
    #   q = 5: w = 580; 372 + 9*26 = 606; 372 + ceil(606/70) 26 = 606.            R = 106
    #   q = 6: w = 668; 434 + 10*26 = 694; 434 + ceil(694/70) 26 = 694.           R = 94
    #          694 <= 700: the busy period ends with the seventh job.
    # R_b = 118, reached by the fifth job; the first job alone would say 114.
    result = analyse(task("a", 2, 70, 26), task("b", 1, 100, 62))
    b = result.result("b").response
    assert [(job.finish_us, job.response_us) for job in b.jobs] == [
        (114, 114), (202, 102), (316, 116), (404, 104), (518, 118), (606, 106), (694, 94)]
    assert b.response_us == 118 and b.worst_job == 4 and b.busy_period_us == 694
    assert not result.schedulable


def test_a_job_released_before_the_previous_one_completes_extends_the_busy_period():
    # alone: T 4, C 3, J 2, the only task.
    # Job 0 arrives at -2, is released at 0 and completes at w = 3: R = 3 + 2 = 5.
    # Job 1 arrives at 4 - 2 = 2 and is released at once, before job 0 completes (3 > 4 - 2):
    #   w = 2 * 3 = 6, R = 6 - 4 + 2 = 4.
    # Job 2 is released at 8 - 2 = 6, when job 1 completes (6 <= 6): the busy period is the
    # two jobs, 6 long. R = max(5, 4) = 5 > D = 4.
    result = analyse(task("alone", 1, 4, 3, jitter=2))
    response = result.result("alone").response
    assert [(job.finish_us, job.response_us) for job in response.jobs] == [(3, 5), (6, 4)]
    assert response.busy_period_us == 6 and response.response_us == 5


def test_the_bounds_do_not_apply_to_a_deadline_shorter_than_the_period():
    # a: priority 2, T 4, C 1        b: priority 1, T 10, D 2, C 2 (rate-monotonic order)
    # U = 0.25 + 0.2 = 0.45, well under both bounds, and the set misses a deadline:
    # R_b: w = 2 + 1 = 3; 2 + ceil(3/4) 1 = 3 > D_b = 2. The bounds were derived for D = T.
    # Deadline-monotonic (b above a): R_b = 2; R_a: w = 1 + 2 = 3; 1 + ceil(3/10) 2 = 3 <= 4.
    result = analyse(task("a", 2, 4, 1), task("b", 1, 10, 2, deadline=2))
    assert result.liu_layland.holds and result.hyperbolic.holds
    for bound in (result.liu_layland, result.hyperbolic):
        assert bound.verdict == "not applicable"
        assert bound.not_applicable == ("deadline shorter than period: b",)
    assert responses(result) == {"a": 1, "b": 3} and not result.schedulable
    assert result.rate_monotonic and not result.deadline_monotonic
    assert {r.task.name: r.response_us for r in result.deadline_monotonic_results} == {
        "b": 2, "a": 3}


def test_tasks_of_equal_priority_count_each_other_as_interference():
    # a and b: priority 2, T 10, C 3 and C 4. FreeRTOS time-slices them in an order the
    # analysis does not know, so each is analysed as if the other had a higher priority.
    # R_a: w = 3 + 4 = 7; 3 + ceil(7/10) 4 = 7.   R_b: w = 4 + 3 = 7; 4 + ceil(7/10) 3 = 7.
    result = analyse(task("a", 2, 10, 3), task("b", 2, 10, 4))
    assert responses(result) == {"a": 7, "b": 7}
    assert result.result("a").interfering == ("b",)
    assert not result.liu_layland.applies
    assert any("equal priorities" in reason for reason in result.liu_layland.not_applicable)


def test_a_level_that_needs_more_than_the_processor_has_no_bound():
    # a: priority 2, T 10, C 6        b: priority 1, T 10, C 5: a and b need 110 %.
    # R_a = 6; b's busy period never ends and its jobs fall further and further behind.
    result = analyse(task("a", 2, 10, 6), task("b", 1, 10, 5))
    assert responses(result) == {"a": 6, "b": None}
    assert "110.0%" in result.result("b").response.unbounded_reason
    assert not result.schedulable
    assert not result.result("b").meets_deadline and result.result("b").slack_us is None


def test_the_whole_processor_without_jitter_or_blocking_is_schedulable():
    # a: priority 2, T 2, C 1        b: priority 1, T 4, C 2: U = 1.
    # R_b: w = 2 + 1 = 3; 2 + ceil(3/2) 1 = 4; 2 + ceil(4/2) 1 = 4. 4 <= 4: one job. R_b = 4.
    result = analyse(task("a", 2, 2, 1), task("b", 1, 4, 2))
    assert responses(result) == {"a": 1, "b": 4}
    assert result.schedulable


@pytest.mark.parametrize("a, b", [
    # a's jitter: its demand in any window w is ceil((w + 1)/2) >= (w + 1)/2, so a and b ask
    # for more than w in every window and the busy period never ends.
    (task("a", 2, 2, 1, jitter=1), task("b", 1, 4, 2)),
    # c, below b, holds r for 1, which b takes: B_b = 1 on top of a demand that fills w.
    (task("a", 2, 2, 1), task("b", 1, 4, 2, sections={"r": 1})),
])
def test_the_whole_processor_with_jitter_or_blocking_has_no_bound(a, b):
    tasks = [a, b]
    if b.critical_sections:
        tasks.append(task("c", 0, 100, 1, sections={"r": 1}))
    result = analyse(*tasks)
    assert result.result("b").response_us is None
    assert "never ends" in result.result("b").response.unbounded_reason


def test_priorities_out_of_deadline_monotonic_order_and_what_the_order_changes():
    # b: priority 1, T 4, C 1        a: priority 2, T 10, C 6 (b has the shorter period)
    # As given: R_a = 6. R_b, job 0: w = 1 + 6 = 7; 1 + ceil(7/10) 6 = 7: R = 7 > 4.
    #   7 > 4 (b's next release): job 1: w = 7 + 1 = 8; 2 + ceil(8/10) 6 = 8: R = 8 - 4 = 4.
    #   8 <= 8: the busy period ends. R_b = 7 > D_b = 4: not schedulable.
    # Deadline-monotonic (b above a): R_b = 1; R_a: w = 6 + 1 = 7; 6 + ceil(7/4) 1 = 8;
    #   6 + ceil(8/4) 1 = 8 <= 10. Schedulable.
    result = analyse(task("b", 1, 4, 1), task("a", 2, 10, 6))
    assert responses(result) == {"a": 6, "b": 7}
    assert not result.schedulable
    assert not result.rate_monotonic and not result.deadline_monotonic
    assert result.rate_monotonic_violations == (sched.OrderViolation("b", "a", 4, 10, 1, 2),)
    assert "priorities not in rate-monotonic order" in result.liu_layland.not_applicable
    alternative = {r.task.name: r.response_us for r in result.deadline_monotonic_results}
    assert alternative == {"b": 1, "a": 8}
    assert [r.task.name for r in result.deadline_monotonic_results] == ["b", "a"]


def test_rate_monotonic_and_deadline_monotonic_orders_differ_when_a_deadline_is_short():
    # fast: T 10, D 10; slow: T 50, D 5. Deadline-monotonic puts slow first, as given here;
    # rate-monotonic would put fast first. No alternative is computed: the order given is
    # deadline-monotonic already.
    result = analyse(task("fast", 1, 10, 2), task("slow", 2, 50, 1, deadline=5))
    assert result.deadline_monotonic and not result.rate_monotonic
    assert result.deadline_monotonic_results is None


def test_equal_periods_may_have_any_priorities_but_equal_priorities_need_equal_periods():
    same_period = analyse(task("a", 2, 10, 1), task("b", 1, 10, 1))
    assert same_period.rate_monotonic
    tied = analyse(task("a", 1, 10, 1), task("b", 1, 20, 1))
    assert not tied.rate_monotonic
    assert tied.rate_monotonic_violations == (sched.OrderViolation("a", "b", 10, 20, 1, 1),)


def test_the_deadline_monotonic_order_breaks_ties_by_the_given_priority_then_by_the_table():
    tasks = (task("x", 1, 20, 1, deadline=10), task("y", 3, 20, 1, deadline=10),
             task("z", 5, 20, 1, deadline=20), task("w", 1, 20, 1, deadline=10))
    ordered = sched.deadline_monotonic(tasks)
    assert {t.name: t.priority for t in ordered} == {"y": 4, "x": 3, "w": 2, "z": 1}


def test_a_sporadic_task_is_analysed_at_its_minimum_inter_arrival_time():
    # irq: sporadic, at least 5 apart, C 1, priority 2. main: T 20, C 8.
    # R_main: w = 8 + 1 = 9; 8 + ceil(9/5) 1 = 10; 8 + ceil(10/5) 1 = 10.           R = 10
    result = analyse(task("irq", 2, 5, 1, kind=sched.SPORADIC), task("main", 1, 20, 8))
    assert responses(result) == {"irq": 1, "main": 10}


# ------------------------------------------------------------------------------------------
# Published examples: Audsley, Burns, Richardson, Tindell and Wellings 1993 (see the top of
# this file). Checked against the authors' 16-page version of the paper, the one at
# https://www.math.unipd.it/~tullio/RTS/2009/ABRTW-1993.pdf. Their r_i is measured from the
# release, the module's response from the arrival: the module's value is J_i + r_i, their
# equation (6). Their B_i is given in the table, so the module's response_time() is called
# with it; it does not come from critical sections.
# ------------------------------------------------------------------------------------------


def test_audsley_et_al_table_1_and_section_3():
    # Table 1, in priority order: task 1 C 1, T 50, D 4; task 2 C 2, T 9, D 6; task 3 C 5,
    # T 20, D 12. Section 3: "task 3 is prevented from executing by task 1 for 1 tick and
    # task 2 for 2 ticks, completing by time 8, giving r_3 = 8"; if task 1 takes two ticks
    # more, "the worst-case response time of task 3 is now 12, just meeting its deadline".
    tasks = [task("task1", 3, 50, 1, deadline=4), task("task2", 2, 9, 2, deadline=6),
             task("task3", 1, 20, 5, deadline=12)]
    assert responses(analyse(*tasks))["task3"] == 8
    tasks[0] = dataclasses.replace(tasks[0], wcet_us=3)
    result = analyse(*tasks)
    assert result.result("task3").response_us == 12 and result.result("task3").slack_us == 0
    assert result.schedulable
    # The priorities are deadline-monotonic, which the paper uses, and not rate-monotonic.
    assert result.deadline_monotonic and not result.rate_monotonic


def test_audsley_et_al_table_2_and_section_4_release_jitter():
    # Table 2: T1 C 3, T 12, D 8, J 4 (higher priority); T2 C 6, T 20, D 10, J 0.
    # Without the jitter the paper's iteration converges at r_2 = 9 (6, 9, 9).
    # With it, "since r_2 + J_1 > T_1 (or 9 + 4 > 12), T2 gets an extra 'hit'", and its
    # Figure 4 shows T2 missing its deadline: by its equation (5), r_2 = 6 + ceil((9 + 4)/12) 3
    # = 12, then 6 + ceil((12 + 4)/12) 3 = 12 > D_2 = 10. "But if C_2 = 5 then r_2 would be 8,
    # and since 8 + 4 <= 12, no extra 'hit' occurs": 5 + ceil((8 + 4)/12) 3 = 8, a ceiling of
    # exactly 1.
    t1 = task("T1", 2, 12, 3, deadline=8, jitter=4)
    t2 = task("T2", 1, 20, 6, deadline=10)
    assert responses(analyse(dataclasses.replace(t1, jitter_us=0), t2))["T2"] == 9
    result = analyse(t1, t2)
    assert responses(result) == {"T1": 4 + 3, "T2": 12}
    assert not result.result("T2").meets_deadline
    assert responses(analyse(t1, dataclasses.replace(t2, wcet_us=5)))["T2"] == 8


# Table 3 (section 6), the avionics case study of Locke, Vogel and Mesler, in priority order:
# C_i, T_i, D_i, r_i, B_i, J_i in microseconds.
AUDSLEY_TABLE_3 = [
    (51, 1000, 1000, 51, 0, 0),
    (3000, 200000, 5000, 3504, 300, 0),
    (2000, 25000, 25000, 5906, 600, 0),
    (5000, 25000, 25000, 11512, 900, 0),
    (1000, 40000, 40000, 13064, 1350, 0),
    (3000, 50000, 50000, 16217, 1350, 0),
    (5000, 50000, 50000, 20821, 750, 0),
    (8000, 59000, 59000, 36637, 750, 0),
    (9000, 80000, 80000, 47798, 1350, 0),
    (2000, 80000, 80000, 48949, 450, 0),
    (5000, 100000, 100000, 115966, 1050, 0),
    (1000, 200000, 200000, 137488, 450, 1000),
    (3000, 200000, 200000, 140641, 450, 0),
    (1000, 200000, 200000, 141692, 450, 0),
    (1000, 200000, 200000, 143694, 1350, 0),
    (3000, 200000, 200000, 145446, 0, 0),
    (1000, 1000000, 1000000, 146497, 0, 0),
    (1000, 1000000, 1000000, 147548, 0, 0),
]  # fmt: skip


def table_3_tasks() -> list[Task]:
    rows = AUDSLEY_TABLE_3
    return [task(f"task{i + 1}", len(rows) - i, period, wcet, deadline=deadline, jitter=jitter)
            for i, (wcet, period, deadline, _, _, jitter) in enumerate(rows)]


def table_3_first_job(index: int, blocking_us: int) -> int:
    """The window of the first job of task index + 1, measured from its release: the paper's
    r_i."""
    tasks = table_3_tasks()
    return sched.response_time(tasks[index], tasks[:index], blocking_us).jobs[0].finish_us


@pytest.mark.parametrize("row", [i for i in range(len(AUDSLEY_TABLE_3)) if i not in (10, 11)])
def test_audsley_et_al_table_3(row):
    # 16 of the 18 rows; rows 11 and 12 are the next two tests.
    _, _, _, r, blocking_us, _ = AUDSLEY_TABLE_3[row]
    assert table_3_first_job(row, blocking_us) == r


def test_audsley_et_al_table_3_row_11_needs_1000_us_more_blocking_than_printed():
    # With the printed B_11 = 1050 the least solution of equation (5) is 99150 (the module's
    # iteration; C_11 = 5000 is confirmed by rows 13 to 18, in whose interference it appears):
    # task 11 would meet its 100000 deadline, where the paper says it misses. 115966 is the
    # least solution with B_11 = 2050:
    #   7050 + 116*51 + 3000 + 5*2000 + 5*5000 + 3*1000 + 3*3000 + 3*5000 + 2*8000 + 2*9000
    #   + 2*2000 = 7050 + 108916 = 115966.
    assert table_3_first_job(10, 1050) == 99150
    assert table_3_first_job(10, 2050) == 115966
    tasks = table_3_tasks()
    with_2050 = sched.response_time(tasks[10], tasks[:10], 2050)
    # A first job that ends after the next release (115966 > 100000) starts a longer busy
    # period; the module reports the worst job of it, which is at least the first.
    assert len(with_2050.jobs) > 1 and with_2050.response_us >= 115966


def test_audsley_et_al_table_3_row_12_is_a_larger_solution_than_the_least():
    # The least solution for task 12 is 99550. 137488 solves the same equation as well:
    #   1450 + 138*51 + 3000 + 6*2000 + 6*5000 + 4*1000 + 3*3000 + 3*5000 + 3*8000 + 2*9000
    #   + 2*2000 + 2*5000 = 1450 + 136038 = 137488,
    # and it is where the iteration ends when it starts from r_11 = 115966, as section 3 of the
    # paper suggests ("with the starting value r_b^0 set to r_a"). Starting there assumes
    # r_b >= r_a, which blocking can break: task 12's C + B = 1450 is less than task 11's B.
    tasks = table_3_tasks()

    def demand(window: int) -> int:
        return 1000 + 450 + sum(math.ceil((window + t.jitter_us) / t.period_us) * t.wcet_us
                                for t in tasks[:11])

    assert demand(137488) == 137488
    assert table_3_first_job(11, 450) == 99550
    assert demand(99550) == 99550


# ------------------------------------------------------------------------------------------
# The oracle: a simulation of the schedule
#
# Discrete time, one unit per step, which is exact when every parameter is an integer: every
# release and every completion falls on an integer. At each step the dispatcher runs the
# ready job of highest effective priority for one unit. A job's work is a list of segments:
# plain execution, a critical section on a mutex, or a non-preemptive stretch. A job that
# reaches a section whose mutex another job holds waits, and while it waits the holder runs at
# the waiting job's priority if that is higher (priority inheritance). Giving the mutex back
# wakes every waiter, and they try again in priority order, as FreeRTOS's xSemaphoreTake loop
# does. A job that has begun a non-preemptive stretch keeps the processor until it ends. The
# jobs of one task run one after the other.
# ------------------------------------------------------------------------------------------

# Marks the non-preemptive segment of a job (mutexes of the random sets are m0, m1, m2).
NONPREEMPTIVE_SECTION = "<non-preemptive>"


class SimJob:
    def __init__(self, sim_task: Task, arrival: int, segments: list[list]):
        self.task = sim_task
        self.arrival = arrival
        self.segments = segments  # [mutex or None, remaining units]
        self.waiting_for: str | None = None
        self.holding: str | None = None


def job_segments(sim_task: Task, placement: str, rng: random.Random,
                 held_first: str | None = None) -> list[list]:
    """The work of one job: its critical sections, its non-preemptive stretch and plain
    execution, in the order the placement says ("start": sections first, "end": sections last,
    "random": interleaved at random). The sections and the stretch are separate parts of the
    job here. `held_first` puts the section on that mutex (or the stretch) first, for a job
    that is inside it at time 0."""
    sections = [[s.resource, s.length_us] for s in sim_task.critical_sections]
    if sim_task.nonpreemptive_us:
        sections.append([NONPREEMPTIVE_SECTION, sim_task.nonpreemptive_us])
    plain = sim_task.wcet_us - sum(length for _, length in sections)
    assert plain >= 0, "the oracle keeps sections and the stretch apart"
    if placement == "start":
        segments = sections + [[None, plain]]
    elif placement == "end":
        segments = [[None, plain]] + sections
    else:
        rng.shuffle(sections)
        cuts = sorted(rng.randint(0, plain) for _ in sections)
        pieces = [b - a for a, b in zip([0, *cuts], [*cuts, plain])]
        segments = [[None, pieces[0]]]
        for section, piece in zip(sections, pieces[1:]):
            segments += [section, [None, piece]]
    if held_first is not None:
        first = next(s for s in segments if s[0] == held_first)
        segments.remove(first)
        segments.insert(0, first)
    return [segment for segment in segments if segment[1] > 0]


def simulate(tasks: list[Task], release, *, holders: dict[str, str] | None = None,
             placement: str = "start", rng: random.Random | None = None) -> dict[str, int]:
    """The worst response time (completion minus arrival) of each task's jobs.

    release(task, k) gives (arrival, release time) of the task's job k; releases are not
    before 0 and do not decrease. `holders` maps a task to the mutex its job 0 holds at time 0
    (or to NONPREEMPTIVE_SECTION: job 0 is inside its non-preemptive stretch): it got there
    just before the other tasks were released. Jobs keep being released until the hyperperiod
    is over and the processor has done every job released before that.
    """
    rng = rng or random.Random(0)
    holders = holders or {}
    horizon = math.lcm(*(t.period_us for t in tasks))
    limit = 50 * horizon + 10 * max(t.jitter_us for t in tasks) + 1000
    queues: dict[str, collections.deque[SimJob]] = {t.name: collections.deque() for t in tasks}
    issued = {t.name: 0 for t in tasks}
    upcoming = {t.name: release(t, 0) for t in tasks}
    owner: dict[str, SimJob] = {}
    worst: dict[str, int] = {}

    def heads() -> list[SimJob]:
        return [queue[0] for queue in queues.values() if queue]

    def effective(job: SimJob) -> int:
        inherited = [other.task.priority for other in heads()
                     if job.holding is not None and other.waiting_for == job.holding]
        return max([job.task.priority, *inherited])

    nonpreemptive: SimJob | None = None  # the job inside its non-preemptive stretch
    now = 0
    while not (now >= horizon and not any(queues.values())):
        assert now < limit, "the simulated schedule does not come to an end"
        for t in tasks:
            while upcoming[t.name][1] <= now:
                arrival, _ = upcoming[t.name]
                held = holders.get(t.name) if issued[t.name] == 0 else None
                job = SimJob(t, arrival, job_segments(t, placement, rng, held))
                if held == NONPREEMPTIVE_SECTION:
                    assert nonpreemptive is None, "two jobs inside non-preemptive stretches"
                    nonpreemptive = job
                elif held is not None:
                    assert held not in owner, "two holders of one mutex"
                    owner[held] = job
                    job.holding = held
                queues[t.name].append(job)
                issued[t.name] += 1
                upcoming[t.name] = release(t, issued[t.name])
        while True:
            if nonpreemptive is not None:
                job = nonpreemptive
            else:
                ready = [job for job in heads() if job.waiting_for is None]
                if not ready:
                    break
                job = max(ready, key=effective)
            mutex = job.segments[0][0]
            if mutex == NONPREEMPTIVE_SECTION:
                nonpreemptive, mutex = job, None
            elif mutex is not None and job.holding != mutex:
                if mutex in owner:
                    job.waiting_for = mutex
                    continue
                owner[mutex] = job
                job.holding = mutex
            job.segments[0][1] -= 1
            if job.segments[0][1] == 0:
                job.segments.pop(0)
                if nonpreemptive is job:
                    nonpreemptive = None
                if mutex is not None:
                    del owner[mutex]
                    job.holding = None
                    for other in heads():
                        if other.waiting_for == mutex:
                            other.waiting_for = None
                if not job.segments:
                    queues[job.task.name].popleft()
                    response = now + 1 - job.arrival
                    worst[job.task.name] = max(worst.get(job.task.name, 0), response)
            break
        now += 1
    return worst


def worst_case_releases(t: Task, k: int) -> tuple[int, int]:
    """The critical instant with jitter: job 0 arrives J before 0 and is released at 0, every
    later job is released as soon as it arrives (but not before 0)."""
    arrival = k * t.period_us - t.jitter_us
    return arrival, max(0, arrival)


def random_jitter_releases(rng: random.Random):
    """Each job released anywhere from its arrival to J after it, in order."""
    last: dict[str, int] = {}

    def release(t: Task, k: int) -> tuple[int, int]:
        arrival = k * t.period_us - t.jitter_us
        moment = max(0, last.get(t.name, 0), arrival + rng.randint(0, t.jitter_us))
        last[t.name] = moment
        return arrival, moment

    return release


def test_the_simulator_reproduces_the_schedules_the_paper_describes():
    # The oracle checked against the text of Audsley et al. (1993), not against the module.
    # Section 3, Figure 1 (Table 1, all released at 0): task 3 "completing by time 8".
    table_1 = [task("task1", 3, 50, 1, deadline=4), task("task2", 2, 9, 2, deadline=6),
               task("task3", 1, 20, 5, deadline=12)]
    assert simulate(table_1, worst_case_releases)["task3"] == 8
    # Section 4, Figure 4 (Table 2): "T1, although released at time zero, is suspended
    # awaiting a message, which it receives at time 4 (this is also the time T2 arrives and is
    # released). On the next release of T1, 12 ticks later, the next message is already
    # available ... T2 misses a deadline": T1 runs 4-7, T2 7-12, T1 12-15, T2 15-16, so T2
    # completes 12 after its arrival, past its deadline of 10.
    t1 = task("T1", 2, 12, 3, deadline=8, jitter=4)
    t2 = task("T2", 1, 20, 6, deadline=10)

    def figure_4(t: Task, k: int) -> tuple[int, int]:
        if t.name == "T1":
            return 12 * k, 4 if k == 0 else 12 * k
        return 4 + 20 * k, 4 + 20 * k

    assert simulate([t1, t2], figure_4)["T2"] == 12
    # The busy period of several jobs worked out by hand above: the fifth job of b, 118.
    assert simulate([task("a", 2, 70, 26), task("b", 1, 100, 62)], worst_case_releases) == {
        "a": 26, "b": 118}


PERIODS = (4, 5, 6, 8, 10, 12, 15, 20, 24, 30, 40, 60)


def random_tasks(rng: random.Random, *, max_utilisation: Fraction, jitter: bool = False,
                 resources: int = 0, nonpreemptive: bool = False,
                 implicit_deadlines: bool = False, rate_monotonic: bool = False) -> list[Task]:
    """A random set of 1 to 6 tasks with distinct priorities and integer parameters whose
    utilisation is at most `max_utilisation`; with `resources`, sections on up to that many
    mutexes, and with `nonpreemptive`, non-preemptive stretches (apart from the sections)."""
    while True:
        n = rng.randint(1, 6)
        periods = [rng.choice(PERIODS) for _ in range(n)]
        # UUniFast: utilisations that add up to the target, uniformly distributed.
        target, shares = rng.uniform(0.4, 1.0), []
        remaining = target
        for i in range(1, n):
            next_remaining = remaining * rng.random() ** (1 / (n - i))
            shares.append(remaining - next_remaining)
            remaining = next_remaining
        shares.append(remaining)
        wcets = [max(1, round(share * period)) for share, period in zip(shares, periods)]
        if rng.random() < 0.2:
            # Fill the processor exactly where the last period allows it.
            rest = sum(Fraction(c, p) for c, p in zip(wcets[:-1], periods[:-1]))
            fill = (1 - rest) * periods[-1]
            if fill.denominator == 1 and fill >= 1:
                wcets[-1] = int(fill)
        total = sum(Fraction(c, p) for c, p in zip(wcets, periods))
        if total <= max_utilisation:
            break
    if rate_monotonic:
        order = sorted(range(n), key=lambda i: (periods[i], rng.random()))
        priority = {i: n - rank for rank, i in enumerate(order)}
    else:
        ranks = rng.sample(range(1, n + 1), n)
        priority = dict(enumerate(ranks))
    names = [f"t{i}" for i in range(n)]
    tasks = []
    for i in range(n):
        deadline = periods[i] if implicit_deadlines or rng.random() < 0.5 else rng.randint(
            max(1, periods[i] // 3), periods[i])
        jitter_us = rng.choice([0, rng.randint(0, periods[i] // 2), rng.randint(0, periods[i])]) \
            if jitter else 0
        sections: dict[str, int] = {}
        budget = wcets[i]
        for r in range(resources):
            if budget > 0 and rng.random() < 0.6:
                length = rng.randint(1, budget)
                sections[f"m{r}"] = length
                budget -= length
        stretch = rng.randint(1, budget) if nonpreemptive and budget and rng.random() < 0.5 else 0
        tasks.append(task(names[i], priority[i], periods[i], wcets[i], deadline=deadline,
                          jitter=jitter_us, sections=sections, nonpreemptive=stretch,
                          kind=rng.choice([sched.PERIODIC, sched.SPORADIC])))
    return tasks


def analysed(tasks: list[Task]) -> dict[str, sched.TaskResult]:
    return {result.task.name: result for result in sched.analyse_response_times(tasks)}


def test_the_analysis_is_the_simulated_worst_case_without_blocking_or_jitter():
    # Exact: with synchronous releases and no blocking the worst response of every task is in
    # its first busy period from the critical instant, and the analysis computes every job of
    # it. Equality, task by task, including tasks that miss their deadlines.
    rng = random.Random(20261118)
    covered = collections.Counter()
    for _ in range(1500):
        tasks = random_tasks(rng, max_utilisation=Fraction(1))
        results = analysed(tasks)
        simulated = simulate(tasks, worst_case_releases)
        for t in tasks:
            result = results[t.name]
            assert result.response_us == simulated[t.name], (tasks, t.name)
            assert result.meets_deadline == (simulated[t.name] <= t.deadline_us)
            covered["several jobs"] += len(result.response.jobs) > 1
            covered["miss"] += not result.meets_deadline
            covered["meets with zero slack"] += result.slack_us == 0
        covered["U = 1"] += sched.utilisation(tasks) == 1
    # The random sets reach the cases that matter: busy periods of several jobs, misses,
    # responses equal to deadlines, a processor exactly full.
    assert min(covered.values()) >= 20, covered
    assert len(covered) == 4


def test_the_analysis_is_the_simulated_worst_case_of_the_critical_instant_with_jitter():
    # With jitter the analysis describes one release pattern exactly: every job 0 arriving its
    # full jitter before 0 and released at 0, every later job released as soon as it arrives.
    # Simulating that pattern must give the analysis' values (the level of every task below 100 %,
    # so that each busy period ends).
    rng = random.Random(1993)
    with_jitter = 0
    for _ in range(800):
        tasks = random_tasks(rng, max_utilisation=Fraction(99, 100), jitter=True)
        results = analysed(tasks)
        simulated = simulate(tasks, worst_case_releases)
        for t in tasks:
            assert results[t.name].response_us == simulated[t.name], (tasks, t.name)
        with_jitter += any(t.jitter_us > 0 for t in tasks)
    assert with_jitter >= 400


def test_no_release_pattern_within_the_jitter_exceeds_the_analysis():
    rng = random.Random(4)
    for _ in range(300):
        tasks = random_tasks(rng, max_utilisation=Fraction(99, 100), jitter=True)
        results = analysed(tasks)
        for _ in range(3):
            simulated = simulate(tasks, random_jitter_releases(rng))
            for t in tasks:
                assert simulated[t.name] <= results[t.name].response_us, (tasks, t.name)


def can_hold(holders: dict[str, str], candidate: Task, what: str, tasks: list[Task]) -> bool:
    """Whether `candidate` can be inside its section on `what` (a mutex or
    NONPREEMPTIVE_SECTION) at time 0 besides the holders already chosen. Each task is in one
    section, each mutex held once. Tasks get into their sections one after the other, each
    preempting the one before, so they hold them in rising priority; nothing preempts a
    non-preemptive stretch, so the task inside one is the last of them, the highest."""
    if candidate.name in holders or what in holders.values():
        return False
    priority = {t.name: t.priority for t in tasks}
    for name, held in holders.items():
        if held == NONPREEMPTIVE_SECTION and priority[name] < candidate.priority:
            return False
        if what == NONPREEMPTIVE_SECTION and priority[name] > candidate.priority:
            return False
    return True


def holders_against(target: Task, tasks: list[Task]) -> dict[str, str]:
    """Lower-priority tasks inside sections at time 0, chosen to block `target` for long: for
    each mutex that a task at or above target's priority takes, and for a non-preemptive
    stretch, the lower task with the longest section, longest first, as far as can_hold()
    allows."""
    above = [t for t in tasks if t.priority >= target.priority]
    below = [t for t in tasks if t.priority < target.priority]
    mutexes = {s.resource for t in above for s in t.critical_sections}
    candidates = [(t.section_us(m), m, t) for t in below for m in mutexes if t.section_us(m) > 0]
    candidates += [(t.nonpreemptive_us, NONPREEMPTIVE_SECTION, t) for t in below
                   if t.nonpreemptive_us > 0]
    holders: dict[str, str] = {}
    for _, what, candidate in sorted(candidates, key=lambda c: (-c[0], c[1], c[2].name)):
        if can_hold(holders, candidate, what, tasks):
            holders[candidate.name] = what
    return holders


def random_holders(rng: random.Random, tasks: list[Task]) -> dict[str, str]:
    """Any tasks inside any of their sections at time 0, as far as can_hold() allows."""
    holders: dict[str, str] = {}
    for t in rng.sample(tasks, len(tasks)):
        choices = [s.resource for s in t.critical_sections]
        choices += [NONPREEMPTIVE_SECTION] if t.nonpreemptive_us else []
        choices = [what for what in choices if can_hold(holders, t, what, tasks)]
        if choices and rng.random() < 0.5:
            holders[t.name] = rng.choice(choices)
    return holders


def test_no_blocking_scenario_exceeds_the_analysis():
    # Mutexes and non-preemptive stretches, sometimes jitter; the sections anywhere in the job.
    rng = random.Random(1990)
    blocked = collections.Counter()
    for _ in range(600):
        tasks = random_tasks(rng, max_utilisation=Fraction(99, 100), resources=rng.randint(1, 3),
                             jitter=rng.random() < 0.3, nonpreemptive=rng.random() < 0.5)
        results = analysed(tasks)
        scenarios = [holders_against(t, tasks) for t in tasks]
        scenarios += [random_holders(rng, tasks) for _ in range(2)]
        for holders in scenarios:
            placement = rng.choice(["start", "end", "random"])
            simulated = simulate(tasks, worst_case_releases, holders=holders,
                                 placement=placement, rng=rng)
            for t in tasks:
                bound = results[t.name].response_us
                assert bound is None or simulated[t.name] <= bound, (tasks, holders, t.name)
        sections = {b.resource for r in results.values() for b in r.blocking.per_resource}
        blocked["by a mutex"] += any(s != sched.NONPREEMPTIVE for s in sections)
        blocked["non-preemptively"] += sched.NONPREEMPTIVE in sections
    assert min(blocked.values()) >= 100 and len(blocked) == 2, blocked


def test_blocking_on_one_mutex_reaches_the_analysis_when_the_task_shares_it_with_no_task_above():
    # One mutex, every section at the start of its job, no jitter. The lower task with the
    # longest section on the mutex holds it at the critical instant; the first task at or
    # above the target's priority that wants it runs into it inside the target's busy period,
    # and the holder, at the inherited priority, finishes its section there. Nothing else
    # changes the work of that busy period, so the bound is reached for the target, on one
    # condition: the target must not take the mutex while a task above it takes it too. If it
    # does, a higher job can arrive while the target holds the mutex, and then waits instead of
    # preempting it: the target finishes earlier than the analysis, which counts that job as
    # interference. (T 6, C 2 holding the mutex all along, below a task of T 5 that takes it
    # first thing: the analysis says 7, the schedule gives 6.) Such targets are skipped here;
    # the test above holds them to the bound.
    rng = random.Random(7)
    reached = collections.Counter()
    for _ in range(600):
        tasks = random_tasks(rng, max_utilisation=Fraction(99, 100), resources=1)
        results = analysed(tasks)
        for t in tasks:
            shares = t.critical_sections and any(
                other.priority > t.priority and other.critical_sections for other in tasks)
            if results[t.name].response_us is None or shares:
                continue
            simulated = simulate(tasks, worst_case_releases, holders=holders_against(t, tasks))
            assert simulated[t.name] == results[t.name].response_us, (tasks, t.name)
            if results[t.name].blocking.blocking_us > 0:
                reached["direct" if t.critical_sections else "push-through"] += 1
    assert min(reached.values()) >= 50 and len(reached) == 2, reached


def test_the_sufficient_tests_never_accept_a_set_that_misses_a_deadline():
    # Sets that meet every assumption of the two bounds: rate-monotonic priorities, distinct,
    # deadlines equal to periods, no jitter, no blocking. The hyperbolic bound accepts every
    # set that Liu and Layland's accepts, and the simulation confirms every acceptance.
    rng = random.Random(1973)
    accepted = collections.Counter()
    for _ in range(1500):
        tasks = random_tasks(rng, max_utilisation=Fraction(1), implicit_deadlines=True,
                             rate_monotonic=True)
        result = sched.analyse(TaskTable(tuple(tasks)))
        assert result.liu_layland.applies and result.hyperbolic.applies
        if result.liu_layland.holds:
            assert result.hyperbolic.holds
        if result.hyperbolic.holds:
            simulated = simulate(tasks, worst_case_releases)
            assert all(simulated[t.name] <= t.deadline_us for t in tasks)
            assert result.schedulable
        accepted["liu_layland"] += result.liu_layland.holds
        accepted["only hyperbolic"] += result.hyperbolic.holds and not result.liu_layland.holds
        accepted["neither, schedulable"] += not result.hyperbolic.holds and result.schedulable
        accepted["neither, misses"] += not result.hyperbolic.holds and not result.schedulable
    assert min(accepted.values()) >= 20 and len(accepted) == 4, accepted


@pytest.mark.parametrize("breaks, options", [
    ("deadline shorter than period", dict(rate_monotonic=True)),
    ("release jitter", dict(rate_monotonic=True, implicit_deadlines=True, jitter=True)),
    ("blocking by lower-priority tasks", dict(rate_monotonic=True, implicit_deadlines=True,
                                              resources=2, nonpreemptive=True)),
    ("priorities not in rate-monotonic order", dict(implicit_deadlines=True)),
])
def test_a_sufficient_test_that_applies_never_contradicts_the_exact_analysis(breaks, options):
    # Random sets that may break one assumption of the bounds, and only that one. Where a
    # bound applies the analysis is exact (no blocking, no jitter: tested above), so a bound
    # that accepts a set the analysis rejects would accept a set that misses. Counted: sets
    # whose bound inequality holds and that miss a deadline because of the broken assumption,
    # the sets a bound would get wrong if that assumption were not checked.
    rng = random.Random(2003)
    wrong_if_applied = 0
    for _ in range(600):
        tasks = random_tasks(rng, max_utilisation=Fraction(1), **options)
        result = sched.analyse(TaskTable(tuple(tasks)))
        for bound in (result.liu_layland, result.hyperbolic):
            assert all(reason.startswith(breaks) for reason in bound.not_applicable)
            if bound.verdict == "schedulable":
                assert result.schedulable, tasks
            wrong_if_applied += bound.holds and not bound.applies and not result.schedulable
    assert wrong_if_applied >= 20


def test_deadline_monotonic_priorities_are_optimal_without_blocking_or_jitter():
    # Leung and Whitehead (1982), as Audsley et al. (1993) quote it: if any fixed-priority
    # order makes a set with D <= T schedulable, deadline-monotonic does. Checked against
    # every order of small random sets (by the analysis, exact here as tested above).
    rng = random.Random(1982)
    rescued = 0
    for _ in range(400):
        tasks = random_tasks(rng, max_utilisation=Fraction(1))
        if len(tasks) > 4:
            continue
        any_order = any(
            all(r.meets_deadline for r in sched.analyse_response_times(
                [dataclasses.replace(t, priority=p) for t, p in zip(tasks, order)]))
            for order in itertools.permutations(range(1, len(tasks) + 1)))
        dm = all(r.meets_deadline for r in sched.analyse_response_times(
            sched.deadline_monotonic(tasks)))
        assert dm == any_order, tasks
        given = all(r.meets_deadline for r in sched.analyse_response_times(tasks))
        rescued += dm and not given
    assert rescued >= 20


# ------------------------------------------------------------------------------------------
# The table, the command line and the report
# ------------------------------------------------------------------------------------------


def entry(name: str, priority: int, period: int, wcet: int, deadline: int | None = None,
          jitter: int = 0, sections: dict[str, int] | None = None,
          kind: str = "periodic", nonpreemptive: int = 0) -> dict:
    period_field = "period_us" if kind == "periodic" else "min_interarrival_us"
    return {"name": name, "priority": priority, "kind": kind, period_field: period,
            "deadline_us": period if deadline is None else deadline, "wcet_us": wcet,
            "jitter_us": jitter, "nonpreemptive_us": nonpreemptive,
            "critical_sections": [{"resource": r, "length_us": length}
                                  for r, length in (sections or {}).items()]}


def document(*entries: dict, **extra) -> dict:
    return {"schema": "fcn.task_table/1", **extra, "tasks": list(entries)}


# Made-up figures in the shape of the node's table, not the node's.
SCHEDULABLE = document(
    entry("can_rx", 6, 500, 40, kind="sporadic"),
    entry("sensor", 5, 10000, 400, sections={"pack_state": 20}),
    entry("health", 2, 100000, 900, sections={"pack_state": 30}, nonpreemptive=10),
    wcet_basis="design", description="a test table")
NOT_SCHEDULABLE = document(entry("a", 2, 10, 6), entry("b", 1, 10, 5))


def write(tmp_path: Path, content, name: str = "table.json") -> Path:
    path = tmp_path / name
    path.write_text(content if isinstance(content, str) else json.dumps(content))
    return path


def run_cli(*args: str) -> subprocess.CompletedProcess:
    env = dict(os.environ, PYTHONPATH=str(TOOLS))
    return subprocess.run([sys.executable, "-m", "fcn_tools.sched", *args], env=env,
                          capture_output=True, text=True, timeout=60)


def test_the_command_exits_0_for_a_schedulable_table_and_writes_the_json_report(tmp_path):
    out = tmp_path / "report.json"
    run = run_cli(str(write(tmp_path, SCHEDULABLE)), "--json", str(out))
    assert run.returncode == 0, run.stderr
    assert run.stderr == ""
    assert "Verdict: schedulable: every task meets its deadline" in run.stdout
    assert "Execution times: design budgets, not measurements." in run.stdout
    report = json.loads(out.read_text())
    assert report["schema"] == "fcn.sched_report/1"
    assert report["schedulable"] is True and report["wcet_basis"] == "design"
    by_name = {t["name"]: t for t in report["tasks"]}
    assert [t["name"] for t in report["tasks"]] == ["can_rx", "sensor", "health"]
    # sensor: health can block it on pack_state (30) or non-preemptively (10), once: B = 30;
    # w = 30 + 400 + 40 = 470; 430 + ceil(470/500) 40 = 470. can_rx: B = 10, R = 50.
    assert by_name["sensor"]["blocking_us"] == 30 and by_name["sensor"]["response_us"] == 470
    assert by_name["sensor"]["blockers_per_resource"] == [
        {"resource": "non-preemptive", "task": "health", "length_us": 10},
        {"resource": "pack_state", "task": "health", "length_us": 30}]
    assert by_name["sensor"]["blocking_per_resource_us"] == 40
    assert by_name["sensor"]["blocking_per_task_us"] == 30
    assert by_name["can_rx"]["blocking_us"] == 10 and by_name["can_rx"]["response_us"] == 50
    assert by_name["health"]["nonpreemptive_us"] == 10
    assert by_name["can_rx"]["kind"] == "sporadic" and by_name["can_rx"]["period_us"] == 500
    assert report["liu_layland"]["verdict"] == "not applicable"
    assert report["with_deadline_monotonic_priorities"] is None


def test_the_command_exits_1_for_a_table_that_is_not_schedulable(tmp_path):
    out = tmp_path / "report.json"
    run = run_cli(str(write(tmp_path, NOT_SCHEDULABLE)), "--json", str(out))
    assert run.returncode == 1, run.stderr
    assert "NOT schedulable: b has no bound" in run.stdout
    report = json.loads(out.read_text())
    assert report["schedulable"] is False
    assert report["tasks"][1]["response_us"] is None
    assert report["utilisation_at_most_1"] is False


def test_the_command_exits_2_for_a_table_it_refuses(tmp_path):
    run = run_cli(str(write(tmp_path, "{")))
    assert run.returncode == 2
    assert run.stdout == "" and "not valid JSON" in run.stderr


def test_the_command_exits_2_for_a_usage_error():
    assert run_cli().returncode == 2


def test_the_report_names_the_deadline_monotonic_alternative(tmp_path, capsys):
    path = write(tmp_path, document(entry("b", 1, 4, 1), entry("a", 2, 10, 6)))
    assert sched.main([str(path)]) == 1
    text = capsys.readouterr().out
    assert "deadline-monotonic  no: b (D 4, priority 1) is not above a (D 10, priority 2)" in text
    assert "  b > a\n" in text
    assert text.rstrip().endswith("schedulable: every task meets its deadline")


def test_the_report_explains_a_blocking_bound_found_per_task(capsys, tmp_path):
    path = write(tmp_path, document(entry("high", 3, 10, 2, sections={"r1": 1, "r2": 1}),
                                    entry("low", 1, 40, 8, sections={"r1": 3, "r2": 4})))
    assert sched.main([str(path)]) == 0
    text = capsys.readouterr().out
    assert ("high: B = 4, once per lower-priority task: low on r2 4 "
            "(once per resource would be 7)") in text


VALID = entry("t", 1, 10, 2)


@pytest.mark.parametrize("content, complaint", [
    ("not json", "not valid JSON"),
    ("[]", "the table must be an object"),
    (json.dumps({"schema": "fcn.task_table/2", "tasks": [VALID]}), "schema must be"),
    (json.dumps({"tasks": [VALID]}), "missing schema"),
    (json.dumps(document()), "the table has no task"),
    (json.dumps(document(VALID, comment="x")), "unknown field(s) comment"),
    (json.dumps(document(VALID, wcet_basis="guessed")), "wcet_basis must be one of"),
    (json.dumps(document({**VALID, "period": 10})), "unknown field(s) period"),
    (json.dumps(document({k: v for k, v in VALID.items() if k != "jitter_us"})),
     "missing jitter_us"),
    (json.dumps(document({k: v for k, v in VALID.items() if k != "kind"})), "missing kind"),
    (json.dumps(document({**VALID, "kind": "aperiodic"})), "kind must be"),
    # A sporadic task gives its minimum inter-arrival time under that name.
    (json.dumps(document({**VALID, "kind": "sporadic"})), "missing min_interarrival_us"),
    (json.dumps(document({**VALID, "deadline_us": 11})), "longer than period_us"),
    (json.dumps(document({**VALID, "deadline_us": 0})), "deadline_us must be at least 1"),
    (json.dumps(document({**VALID, "wcet_us": 0})), "wcet_us must be at least 1"),
    (json.dumps(document({**VALID, "wcet_us": 2.5})), "wcet_us must be an integer"),
    (json.dumps(document({**VALID, "wcet_us": 2.0})), "wcet_us must be an integer"),
    (json.dumps(document({**VALID, "priority": True})), "priority must be an integer"),
    (json.dumps(document({**VALID, "priority": -1})), "priority must be at least 0"),
    (json.dumps(document({**VALID, "jitter_us": -1})), "jitter_us must be at least 0"),
    (json.dumps(document({k: v for k, v in VALID.items() if k != "nonpreemptive_us"})),
     "missing nonpreemptive_us"),
    (json.dumps(document({**VALID, "nonpreemptive_us": -1})),
     "nonpreemptive_us must be at least 0"),
    (json.dumps(document({**VALID, "nonpreemptive_us": 3})),
     "nonpreemptive_us 3 is longer than wcet_us 2"),
    (json.dumps(document(entry("t", 1, 10, 2, sections={"non-preemptive": 1}))),
     "resource 'non-preemptive' is reserved"),
    (json.dumps(document({**VALID, "period_us": "10"})), "period_us must be an integer"),
    (json.dumps(document({**VALID, "name": ""})), "name must be a non-empty string"),
    (json.dumps(document(VALID, VALID)), "task names must be unique: t"),
    (json.dumps(document({**VALID, "critical_sections": {"r": 1}})),
     "critical_sections must be a list"),
    (json.dumps(document(entry("t", 1, 10, 2, sections={"r": 3}))), "is longer than wcet_us"),
    (json.dumps(document(entry("t", 1, 10, 2, sections={"r": 1, "s": 2}))), "add up to 3"),
    (json.dumps(document(entry("t", 1, 10, 2, sections={"r": 0}))), "must be at least 1"),
    (json.dumps(document({**VALID, "critical_sections": [{"resource": "r", "length_us": 1},
                                                         {"resource": "r", "length_us": 1}]})),
     "resource 'r' is listed twice"),
    (json.dumps(document({**VALID, "critical_sections": [{"resource": "r"}]})),
     "missing length_us"),
    ('{"schema": "fcn.task_table/1", "schema": "x", "tasks": []}', "key 'schema' appears twice"),
    (json.dumps(document(VALID)).replace('"wcet_us": 2', '"wcet_us": NaN'), "NaN is not a number"),
    (json.dumps(document(VALID)).replace('"wcet_us": 2', '"wcet_us": Infinity'),
     "Infinity is not a number"),
], ids=lambda value: value if len(value) < 40 else None)
def test_a_bad_table_is_refused_with_exit_status_2(tmp_path, capsys, content, complaint):
    assert sched.main([str(write(tmp_path, content))]) == 2
    captured = capsys.readouterr()
    assert captured.out == ""
    assert complaint in captured.err


def test_a_missing_file_is_refused_with_exit_status_2(tmp_path, capsys):
    assert sched.main([str(tmp_path / "absent.json")]) == 2
    assert "cannot read the table" in capsys.readouterr().err


def test_a_report_that_cannot_be_written_is_exit_status_2(tmp_path, capsys):
    table = write(tmp_path, SCHEDULABLE)
    assert sched.main([str(table), "--json", str(tmp_path / "no-such-dir" / "out.json")]) == 2
    assert "cannot write" in capsys.readouterr().err


def test_the_table_is_read_as_given():
    table = sched.parse_table(json.dumps(SCHEDULABLE))
    assert table.wcet_basis == "design" and table.description == "a test table"
    can_rx, sensor, health = table.tasks
    assert can_rx == Task("can_rx", 6, "sporadic", 500, 500, 40, 0, ())
    assert sensor.critical_sections == (CriticalSection("pack_state", 20),)
    assert health.section_us("pack_state") == 30 and health.section_us("other") == 0
    assert health.nonpreemptive_us == 10 and sensor.nonpreemptive_us == 0


def test_response_time_refuses_a_task_that_preempts_itself():
    a = task("a", 1, 10, 1)
    with pytest.raises(ValueError):
        sched.response_time(a, [a], 0)
