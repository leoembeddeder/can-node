"""bench/node_timing.py in its --quick mode, into a temporary directory, on each transport: it
writes a result of the right schema whose counts agree with each other. No timing figure is
judged here (the host is loaded by the test run itself): only structure and counts. The bench
takes the vcan0 lock itself (fcn_tools.runner.Session), so this test does not use the can_spec
fixture."""
from __future__ import annotations

import math

import pytest

from bench_quick import TRANSPORTS, run_bench, usable
from fcn_tools import results

#: pytest's limit for the tests of this file, above the bounds of the runs they make (the
#: project's default of 120 s per test would end a quick run on a slow host first, without
#: the bench's output); a bound, not a timing tolerance.
pytestmark = pytest.mark.timeout(360)

TELEMETRY = ("BMS_PackStatus", "BMS_CellVoltages1", "BMS_CellVoltages2", "BMS_Temperatures")
TASKS = ("can_rx", "sensor", "process", "can_tx", "diag", "health", "flash", "IDLE")


def check_distribution(entry: dict, n: int) -> None:
    assert entry["unit"] == "us" and entry["n"] == n
    if n:
        assert entry["min"] <= entry["max"]


@pytest.mark.parametrize("transport", TRANSPORTS)
def test_the_quick_run_writes_a_consistent_result(tmp_path, transport):
    usable(transport)
    result = run_bench("node_timing", tmp_path, "--transport", transport)
    assert result["schema"] == "fcn.result/node_timing/1"
    assert result["loaded"] in (True, False) and result["config"]["quick"] is True
    assert result["config"]["transports"] == [transport]
    results.check_summary(result["summary"])
    reported = result["build"]["reported"]
    assert set(reported) == {f"{transport} {name}" for name in
                             ("idle", "flood", "download-1", "control")}
    assert set(reported.values()) == {result["build"]["tree"]}

    conditions = result["transports"][transport]
    assert set(conditions) == {"idle", "flood", "download", "control"}
    for condition in ("idle", "flood", "download"):
        entry = conditions[condition]
        assert len(entry["runs"]) == 1
        run = entry["runs"][0]
        assert run["trace_dropped"] == 0 and run["trace_events"] > 0
        assert run["observer_frames"] > 0
        if condition == "flood":
            assert run["flood"]["sent"] > 0 and run["flood"]["load"] == 0.5
        if condition == "download":
            assert run["download"]["result"] == "committed" and run["download"]["blocks"] > 0
        for task in ("sensor", "health", "process"):
            release = entry["release"][task]
            assert release["jobs"] > 0
            for key in ("release_lateness", "start_lateness", "start_after_release", "response",
                        "execution"):
                check_distribution(release[key], release["jobs"])
            assert release["raw_start_lateness"]["n"] == release["jobs"]
        # A job released more than a tick early is one of those released before due, and they
        # are among the jobs (how many there are is the host's matter).
        for task in ("sensor", "health"):
            release = entry["release"][task]
            assert 0 <= release["released_more_than_a_tick_early"] \
                <= release["released_before_due"] <= release["jobs"]
        summary = result["summary"][transport][condition]
        assert summary["sensor_released_more_than_a_tick_early"] == results.figure(
            entry["release"]["sensor"]["released_more_than_a_tick_early"], "jobs")
        # The first jobs after a drop (posix_tick.c gave ticks up and moved the grid), listed
        # whether the host made the node give ticks up or not (that is its matter): only of runs
        # that gave ticks up, at least 6 a drop (a lag of more than
        # POSIX_TICK_MAX_CATCH_UP_TICKS, all but one tick of it) and no more than the run gave
        # up; each job later on the grid of the job before than on its own by exactly the ticks
        # given up (1 ms each); the summary counts those in the window.
        dropped = {run_["name"]: run_["tick_dropped"] for run_ in entry["runs"]}
        for task in ("sensor", "health"):
            listed = entry["release"][task]["after_drop"]
            for found in listed:
                assert found["drops"] >= 1 and found["dropped_ticks"] >= 6 * found["drops"]
                assert found["wake_tick"] >= found["first_tick"]
                for key in ("release_lateness", "start_lateness"):
                    assert math.isclose(found[f"{key}_on_the_base_before_us"] - found[f"{key}_us"],
                                        found["dropped_ticks"] * 1000.0, abs_tol=0.002)
            for name, ticks in dropped.items():
                assert sum(found["dropped_ticks"] for found in listed if found["run"] == name) \
                    <= ticks
            assert {found["run"] for found in listed} <= set(dropped)
            assert summary[f"{task}_first_jobs_after_a_drop"] == results.figure(
                sum(found["in_window"] for found in listed), "jobs")
        # The tick count against the grid: the counts first seen ahead are among the counts
        # seen, and a stretch ahead needs a count seen ahead; a result is written only when the
        # longest is within the limit (the node's tick period is 1 ms).
        lead = entry["tick_lead"]
        assert 0 <= lead["seen_more_than_a_tick_before_due"] <= lead["seen_before_due"] \
            <= lead["counts_seen"] and lead["counts_seen"] > 0
        stretch = lead["longest_stretch_ahead"]
        assert stretch["unit"] == "ms" and 0 <= stretch["value"]
        if lead["seen_before_due"] == 0:
            assert stretch["value"] == 0 and lead["most_before_due_us"] == 0
        assert stretch["value"] <= result["config"]["lead_limit_periods"]
        # Every processing job counted was released by a sample, and a sensor job takes one
        # sample, or more after an overrun: no more processing jobs than that, but for a job at
        # an edge of the window. (Not fewer is the host's matter: a processing task held up past
        # the next sample takes both in one job.)
        overruns = sum(run_["overrun_samples_in_trace"] for run_ in entry["runs"])
        assert 0 < entry["release"]["process"]["jobs"] \
            <= entry["release"]["sensor"]["jobs"] + overruns + 2
        for name in TELEMETRY:
            latency = entry["latency"][name]
            assert latency["frames"] > 0 or name == "BMS_Temperatures"
            check_distribution(latency["sample_to_tx_done"], latency["frames"])
            seen = latency["frames"] - latency["frames_time_not_trusted"]
            check_distribution(latency["sample_to_observer"], seen)
            assert latency["raw_sample_to_tx_done"]["n"] == latency["frames"]
        # Every second sample carries cells: at most half as many publishes of them as of
        # BMS_PackStatus (on the bus or superseded), but for the edges of the window. (How many
        # the queue superseded of each is the host's matter.)
        pack = entry["latency"]["BMS_PackStatus"]
        assert entry["latency"]["BMS_CellVoltages1"]["frames"] <= \
            (pack["frames"] + pack["publishes_missing_on_the_bus"]) / 2 + 2
        heartbeat = entry["periods"]["BMS_Heartbeat"]
        assert heartbeat["cycle_ms"] == 10 and heartbeat["deviation"]["n"] == heartbeat["intervals"]
        cpu = entry["cpu"]
        assert set(cpu["tasks"]) == set(TASKS)
        assert sum(task["run_time_us"] for task in cpu["tasks"].values()) <= \
            cpu["run_time_total_us"]
        assert math.isclose(cpu["idle_share"], cpu["tasks"]["IDLE"]["share"])
        for task in ("sensor", "process", "health", "can_rx", "can_tx", "diag", "flash"):
            assert cpu["tasks"][task]["design_budget_us"] > 0
    for entry in result["stack_max"]:
        assert entry["own_bytes"] == entry["peak_bytes"] - entry["start_bytes"]
        assert entry["margin_bytes"] == entry["size_bytes"] - entry["own_bytes"]
    assert {entry["name"] for entry in result["stack_max"]} == set(TASKS)
    control = conditions["control"]
    assert control["trace"] is False and control["observer_frames"] > 0
    assert control["periods"]["BMS_Heartbeat"]["intervals"] > 0
