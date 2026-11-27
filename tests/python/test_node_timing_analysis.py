"""The analysis of bench/node_timing.py that holds the node's tick against the grid of its trace,
on slices of real traces (traces/, read by trace_slices) and on constructed ones: a job released
more than a tick early, with the tick count two ahead, is counted and kept as the node made it;
the first job after ticks were given up (the grid moved) is listed with its lateness on the grid
of the job before as well; a run whose tick count stays ahead of the grid for longer than
LEAD_LIMIT_PERIODS, which a node that gives its made-up ticks back cannot do, is refused; a
condition adds up its runs' jobs released early and pools the tick count's figures and the
first jobs after a drop over its runs."""
from __future__ import annotations

import importlib.util
import sys

import numpy as np
import pytest

import trace_slices
from fcn_tools import paths, runner
from fcn_tools import trace as trace_module
from fcn_tools.trace import (DELAY_UNTIL, EVENT_DTYPE, FLAG_BLOCKED, READY, SWITCH, TICK_BASE,
                             TickLead, Trace)


def _load(name: str):
    path = paths.REPO_ROOT / "bench" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"bench_{name}", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module      # dataclasses look their module up by name
    spec.loader.exec_module(module)
    return module


bench = _load("node_timing")

MS = 1_000_000

#: The periods of the node's task table (app/task_table.c) that the analysis reads.
STATS = {"task_table": {"tasks": [{"name": "sensor", "period_us": 10_000},
                                  {"name": "health", "period_us": 100_000}]}}


def run_of(found: Trace, name: str = "flood"):
    return bench.Run(condition="flood", transport="socketcan", name=name, stats=STATS,
                     trace=found, frames=np.zeros(0), sidecar={}, extra={})


def ahead_for(periods: int, every: int = 1) -> Trace:
    """Base 10 ms, period 1 ms (tick k due at 9 + k ms); counts 1, 1 + `every`, ... up to
    `periods` each first seen exactly a period before its tick was due (the others never seen,
    as in an idle run): one ahead, a stretch from count 1's sight (9 ms) to the due time of the
    last (9 + `periods` ms)."""
    counts = np.arange(1, periods + 1, every)
    assert counts[-1] == periods
    events = np.zeros(counts.size + 1, dtype=EVENT_DTYPE)
    events["kind"] = [TICK_BASE, *[READY] * counts.size]
    events["time_ns"] = [10 * MS, *((8 + counts) * MS)]
    events["value"] = [1, *counts]
    events["arg"][0] = MS
    events["task"][1:] = 4
    return Trace(events=events, names={4: "sensor"})


# ------------------------------------------------------------------------------ the releases


def test_a_job_released_with_the_count_two_ahead_is_counted_and_kept():
    # traces/two-ahead.txt: the sensor job that waited for tick 3202 (xTaskDelayUntil) was
    # released at count 3202, 3199.480 ms after the base, and tick 3202 was due at 3201 ms:
    # 1519.6 us early, more than the period (1 ms) and EARLY_MARGIN_NS (0.1 ms). It is a release
    # the node made: counted, and kept in the lateness figures.
    found = trace_slices.load("two-ahead.txt")
    grid = trace_module.tick_grid(found)
    entry, jobs, arrival = bench.periodic_release(run_of(found), "sensor", grid,
                                                  trace_module.running_intervals(found), None)
    assert jobs.wake_tick.tolist() == [3202] and jobs.release_tick.tolist() == [3202]
    assert (arrival - grid.bases_ns[0]).tolist() == [3201 * MS]
    assert entry["jobs"] == 1 and entry["early"] == 1 and entry["beyond_a_tick"] == 1
    assert entry["release_lateness"].tolist() == [-1_519_634]
    after = int(entry["start_after_release"][0])
    assert 0 < after and entry["start_lateness"].tolist() == [-1_519_634 + after]
    # Out of the window, it is not counted.
    entry, _, _ = bench.periodic_release(run_of(found), "sensor", grid,
                                         trace_module.running_intervals(found), (0, 1))
    assert (entry["jobs"], entry["early"], entry["beyond_a_tick"]) == (0, 0, 0)


def test_more_than_a_tick_early_is_more_than_the_period_and_the_margin():
    # traces/two-ahead.txt with its grid moved earlier: by 470 us, the job is released 1049.6 us
    # before its tick was due, more than the period (1 ms) but within EARLY_MARGIN_NS (0.1 ms)
    # of it, which is room for the clock reads: early, not more than a tick early. By 380 us,
    # 1139.6 us before: more than a tick early.
    found = trace_slices.load("two-ahead.txt")
    for moved, beyond in ((470_000, 0), (380_000, 1)):
        events = found.events.copy()
        events["time_ns"][events["kind"] == TICK_BASE] -= moved
        shifted = Trace(events=events, names=found.names)
        entry, _, _ = bench.periodic_release(run_of(shifted), "sensor",
                                             trace_module.tick_grid(shifted),
                                             trace_module.running_intervals(shifted), None)
        assert entry["release_lateness"].tolist() == [-1_519_634 + moved]
        assert (entry["early"], entry["beyond_a_tick"]) == (1, beyond)


def test_a_job_released_less_than_a_tick_early_is_early_but_not_beyond():
    # traces/old-give-back.txt (one ahead throughout): every sensor job of the 80 ms was released
    # before its tick was due, by less than a period.
    found = trace_slices.load("old-give-back.txt")
    entry, _, _ = bench.periodic_release(run_of(found), "sensor", trace_module.tick_grid(found),
                                         trace_module.running_intervals(found), None)
    assert entry["jobs"] > 0 and entry["early"] == entry["jobs"]
    assert entry["beyond_a_tick"] == 0
    assert all(-MS < late < 0 for late in entry["release_lateness"].tolist())


# ---------------------------------------------------------------- the first jobs after a drop


def test_the_first_job_after_a_drop_is_listed_with_its_lateness_on_the_base_before():
    # traces/dropped-ticks.txt: posix_tick.c gave up 6 ticks and moved the base 6 ms later from
    # tick 9636 on. The sensor job that waited for tick 9631 is on the first base, 31.1 us late.
    # The next one waited for tick 9641, due 6 ms later on the moved base than on the first, and
    # was released 2163.6 us after the one and 8163.6 us after the other (started 0.8 us after
    # its release). The distributions hold its lateness on the moved base, the one in force for
    # its tick; after_drop lists the job with both. The period between the two starts is 18.1 ms.
    found = trace_slices.load("dropped-ticks.txt")
    grid = trace_module.tick_grid(found)
    entry, jobs, _ = bench.periodic_release(run_of(found, "download-3"), "sensor", grid,
                                            trace_module.running_intervals(found), None)
    assert jobs.wake_tick.tolist() == [9631, 9641]
    assert entry["release_lateness"].tolist() == [31_083, 2_163_612]
    assert entry["start_lateness"].tolist() == [50_149, 2_164_448]
    assert entry["period"].tolist() == [8_114_299]
    assert entry["after_drop"] == [{
        "run": "download-3", "first_tick": 9636, "drops": 1, "dropped_ticks": 6,
        "wake_tick": 9641, "in_window": True,
        "release_lateness": 2_163_612, "release_lateness_on_the_base_before": 8_163_612,
        "start_lateness": 2_164_448, "start_lateness_on_the_base_before": 8_164_448}]
    # Out of the window it is still listed, marked so, and in no distribution.
    entry, _, _ = bench.periodic_release(run_of(found, "download-3"), "sensor", grid,
                                         trace_module.running_intervals(found), (0, 1))
    assert entry["jobs"] == 0 and [job["in_window"] for job in entry["after_drop"]] == [False]


def one_task_trace(bases: list[tuple[int, int]], jobs: list[tuple[int, int, int]]) -> Trace:
    """A sensor task (4) and the idle task (8), tick k due at k ms on the first base: the
    TICK_BASE events `bases` (time in ns, first tick), and after the task's first stretch, which
    waits for the first job's tick, jobs (release in ns, the tick it waited for, the tick it
    then waits for): each released at its tick, started 50 us and blocked 100 us later."""
    rows = [(TICK_BASE, time, first, MS, 0, 0, 0) for time, first in bases]
    rows += [(SWITCH, 500_000, 0, 0, 4, 8, 0), (DELAY_UNTIL, 600_000, jobs[0][1], 0, 4, 0, 0),
             (SWITCH, 700_000, 0, 0, 8, 4, FLAG_BLOCKED)]
    for release, wake, then in jobs:
        rows += [(READY, release, wake, 0, 4, 0, 0), (SWITCH, release + 50_000, wake, 0, 4, 8, 0),
                 (DELAY_UNTIL, release + 90_000, then, 0, 4, 0, 0),
                 (SWITCH, release + 100_000, wake, 0, 8, 4, FLAG_BLOCKED)]
    events = np.zeros(len(rows), dtype=EVENT_DTYPE)
    for column, field in enumerate(("kind", "time_ns", "value", "arg", "task", "other", "flags")):
        events[field] = [row[column] for row in rows]
    return Trace(events=events, names={4: "sensor", 8: "IDLE"})


def test_a_drop_before_the_first_job_and_two_drops_between_two_jobs():
    # Tick k due at k ms on the first base. 6 ticks given up from tick 3 on (the base 6 ms
    # later), 6 more from tick 10 on and 7 more from tick 12 on (12 and 19 ms later than the
    # first). The task's first job waited for tick 5, due at 11 ms on its base and at 5 ms on
    # the first (the base it is held against, as it has no job before): released at 11.2 ms.
    # The next waited for tick 15, due at 34 ms on its base and at 21 ms on the base of the job
    # before; released at 34.1 ms, it is listed once, for the two drops since that job (13
    # ticks). The third, on the same base as the second, is not listed. In a window from 20 ms,
    # the first is out of it.
    found = one_task_trace([(MS, 1), (7 * MS, 3), (13 * MS, 10), (20 * MS, 12)],
                           [(11_200_000, 5, 15), (34_100_000, 15, 25), (44_050_000, 25, 35)])
    grid = trace_module.tick_grid(found)
    entry, jobs, arrival = bench.periodic_release(run_of(found), "sensor", grid,
                                                  trace_module.running_intervals(found),
                                                  (20 * MS, 50 * MS))
    assert jobs.wake_tick.tolist() == [5, 15, 25]
    assert arrival.tolist() == [11 * MS, 34 * MS, 44 * MS]
    assert entry["release_lateness"].tolist() == [100_000, 50_000]
    assert entry["after_drop"] == [
        {"run": "flood", "first_tick": 3, "drops": 1, "dropped_ticks": 6, "wake_tick": 5,
         "in_window": False, "release_lateness": 200_000,
         "release_lateness_on_the_base_before": 6_200_000, "start_lateness": 250_000,
         "start_lateness_on_the_base_before": 6_250_000},
        {"run": "flood", "first_tick": 12, "drops": 2, "dropped_ticks": 13, "wake_tick": 15,
         "in_window": True, "release_lateness": 100_000,
         "release_lateness_on_the_base_before": 13_100_000, "start_lateness": 150_000,
         "start_lateness_on_the_base_before": 13_150_000}]


def test_a_run_without_a_drop_lists_no_job():
    found = trace_slices.load("two-ahead.txt")
    entry, _, _ = bench.periodic_release(run_of(found), "sensor", trace_module.tick_grid(found),
                                         trace_module.running_intervals(found), None)
    assert entry["after_drop"] == []


def test_a_condition_lists_its_runs_first_jobs_after_a_drop_in_us_and_counts_those_in_the_window():
    # The slice's job (download-3) and the constructed trace's two (one out of its window).
    parts = []
    for name, found, window in (
            ("download-3", trace_slices.load("dropped-ticks.txt"), None),
            ("download-1", one_task_trace([(MS, 1), (7 * MS, 3), (13 * MS, 10), (20 * MS, 12)],
                                          [(11_200_000, 5, 15), (34_100_000, 15, 25)]),
             (20 * MS, 50 * MS))):
        entry, _, _ = bench.periodic_release(run_of(found, name), "sensor",
                                             trace_module.tick_grid(found),
                                             trace_module.running_intervals(found), window)
        parts.append(entry)
    listed = bench.after_drop_data(parts)
    assert [(job["run"], job["wake_tick"], job["in_window"]) for job in listed] == [
        ("download-3", 9641, True), ("download-1", 5, False), ("download-1", 15, True)]
    assert listed[0] == {
        "run": "download-3", "first_tick": 9636, "drops": 1, "dropped_ticks": 6,
        "wake_tick": 9641, "in_window": True, "release_lateness_us": 2163.612,
        "release_lateness_on_the_base_before_us": 8163.612, "start_lateness_us": 2164.448,
        "start_lateness_on_the_base_before_us": 8164.448}
    assert (listed[2]["release_lateness_us"], listed[2]["release_lateness_on_the_base_before_us"]) \
        == (100.0, 13100.0)
    condition = {"release": {"sensor": {"after_drop": listed}, "health": {"after_drop": []}}}
    assert bench.after_drop_count(condition, "sensor") == {"value": 2, "unit": "jobs"}
    assert bench.after_drop_count(condition, "health") == {"value": 0, "unit": "jobs"}


# --------------------------------------------------------------------- the count against the grid


def test_a_run_with_the_count_two_ahead_for_a_moment_is_not_refused():
    # Counts 3201 and 3202 first seen before their ticks were due, 1.63 ms in a row.
    found = trace_slices.load("two-ahead.txt")
    lead = bench.tick_lead_of(run_of(found), trace_module.tick_grid(found))
    assert lead.seen_beyond == 1 and lead.longest_counts == 2 and lead.longest_ns == 1_629_171


def test_a_run_whose_count_stays_ahead_as_with_the_old_give_back_is_refused():
    # traces/old-give-back.txt: 80 counts in a row seen ahead, 79.8 ms from 560.2 ms after the
    # first base, more than LEAD_LIMIT_PERIODS (50 ms at 1 ms a tick).
    found = trace_slices.load("old-give-back.txt")
    with pytest.raises(runner.BenchFailure,
                       match=r"flood: the tick count ran ahead of the trace's grid for 79\.8 ms "
                             r"\(80 counts in a row seen before their tick was due, from 560\.2 "
                             r"ms after the first tick\), more than 50 periods.*bug 011.*"
                             r"TICK_BASE"):
        bench.tick_lead_of(run_of(found), trace_module.tick_grid(found))


def test_the_limit_is_fifty_periods_ahead_in_a_row():
    assert bench.LEAD_LIMIT_PERIODS == 50
    at_limit = ahead_for(50)
    lead = bench.tick_lead_of(run_of(at_limit), trace_module.tick_grid(at_limit))
    assert (lead.longest_ns, lead.longest_counts, lead.seen_beyond) == (50 * MS, 50, 0)
    over = ahead_for(51)
    with pytest.raises(runner.BenchFailure, match=r"for 51\.0 ms \(51 counts"):
        bench.tick_lead_of(run_of(over), trace_module.tick_grid(over))
    # The limit is on the stretch's time, not on the counts the trace saw in it: one count in
    # five seen, 51 ms in 11 counts, is refused; 46 ms in 10 is not.
    sparse = ahead_for(46, every=5)
    lead = bench.tick_lead_of(run_of(sparse), trace_module.tick_grid(sparse))
    assert (lead.longest_ns, lead.longest_counts) == (46 * MS, 10)
    sparse = ahead_for(51, every=5)
    with pytest.raises(runner.BenchFailure, match=r"for 51\.0 ms \(11 counts"):
        bench.tick_lead_of(run_of(sparse), trace_module.tick_grid(sparse))


def test_a_condition_adds_up_its_runs_jobs_released_early_and_more_than_a_tick_early():
    # Two runs: the two-ahead slice (its one sensor job early, and more than a tick early) and
    # the old give-back's (every job early, none by more than a tick).
    parts = []
    for name in ("two-ahead.txt", "old-give-back.txt"):
        found = trace_slices.load(name)
        entry, _, _ = bench.periodic_release(run_of(found), "sensor",
                                             trace_module.tick_grid(found),
                                             trace_module.running_intervals(found), None)
        parts.append(entry)
    assert parts[1]["early"] > 1 and parts[1]["beyond_a_tick"] == 0
    assert bench.release_counts(parts) == {
        "released_at_a_later_tick": parts[0]["late_ticks"] + parts[1]["late_ticks"],
        "released_before_due": 1 + parts[1]["early"], "released_more_than_a_tick_early": 1}


def test_a_condition_adds_up_its_runs_counts_and_keeps_the_longest_stretch():
    data = bench.tick_lead_data([
        TickLead(counts_seen=100, seen_before_due=3, seen_beyond=1, most_before_due_ns=1_519_634,
                 longest_ns=1_629_171, longest_from_ns=5, longest_counts=2),
        TickLead(counts_seen=200, seen_before_due=4, seen_beyond=0, most_before_due_ns=900_000,
                 longest_ns=2_250_000, longest_from_ns=7, longest_counts=3)])
    assert (data["counts_seen"], data["seen_before_due"],
            data["seen_more_than_a_tick_before_due"]) == (300, 7, 1)
    assert data["most_before_due_us"] == 1519.634
    assert data["longest_stretch_ahead"] == {"value": 2.25, "unit": "ms"}
    assert data["stretch"] == bench.STRETCH_MEASURED
