"""The node's host-only trace (--trace-file, platform/posix/posix_trace.h), read with
fcn_tools.trace: what it records agrees with the node's own counters; a node without the option
records nothing; a full buffer counts what it could not record; the options are checked. Counts
only: no timing is judged here."""
from __future__ import annotations

import subprocess

import numpy as np
import pytest

from fcn_tools import trace
from fcn_tools.node import EXIT_OK

TASKS = {"can_rx", "can_tx", "diag", "sensor", "process", "health", "flash", "IDLE"}


def test_the_trace_agrees_with_the_node_s_counters(start_node, workdir):
    path = workdir / "node.trace"
    node = start_node("--duration-s", "2", "--trace-file", str(path))
    assert node.wait(timeout=30) == EXIT_OK
    stats = node.stats()
    info = stats["trace"]
    assert info["dropped"] == 0 and info["events"] > 0 and info["record_bytes"] == 32
    assert info["capacity"] == 2097152 and info["event_cost_ns"] > 0 and info["cost_records"] > 0
    found = trace.read(path, info)
    assert set(found.names.values()) == TASKS
    # One sensor read per sample, in order from 0.
    numbers, _ = trace.sensor_reads(found)
    assert numbers.tolist() == list(range(stats["sensor"]["reads"]["current"]))
    # Every frame the queue sent is in the trace; a few more may have ended after the health task
    # took the counters (at most the three mailboxes).
    frames = trace.sent_frames(found)
    for name, message in stats["telemetry"]["messages"].items():
        sent = len(frames.of(message["id"]))
        assert message["queue"]["sent"] <= sent <= message["queue"]["sent"] + 3, name
    assert len(frames) == stats["can"]["controller"]["tx_frames"]
    # The sensor task blocks in xTaskDelayUntil once a period: its jobs are its periods, but for
    # one that had not ended when the scheduler stopped.
    jobs, merged = trace.periodic_jobs(found, found.number("sensor"))
    assert merged == 0
    assert stats["sensor"]["periods"] - 1 <= len(jobs) <= stats["sensor"]["periods"]
    assert np.all(jobs.wake_tick > 0) and np.all(jobs.start_ns >= jobs.release_ns)
    assert np.all(jobs.end_ns > jobs.start_ns) and np.all(jobs.exec_ns > 0)
    # The tick's base, and the run time the trace gives each task next to the kernel's.
    assert trace.tick_grid(found).period_ns == 1_000_000
    intervals = trace.running_intervals(found)
    assert set(np.unique(intervals.task).tolist()) == set(found.names)


def test_a_node_without_the_option_records_nothing(start_node, workdir):
    node = start_node("--duration-s", "1")
    assert node.wait(timeout=30) == EXIT_OK
    assert node.stats()["trace"] is False


def test_a_full_trace_counts_the_events_it_could_not_keep(start_node, workdir):
    path = workdir / "node.trace"
    node = start_node("--duration-s", "1", "--trace-file", str(path), "--trace-events", "1000")
    assert node.wait(timeout=30) == EXIT_OK
    info = node.stats()["trace"]
    assert info["capacity"] == 1000 and info["events"] == 1000 and info["dropped"] > 0
    found = trace.read(path, info)
    assert found.events.size == 1000 and found.dropped == info["dropped"]


@pytest.mark.parametrize("args, message", [
    (["--trace-events", "0", "--trace-file", "t.bin"], "invalid --trace-events"),
    (["--trace-events", "8388609", "--trace-file", "t.bin"], "invalid --trace-events"),
    (["--trace-events", "10"], "--trace-events needs a trace"),
    (["--trace-file", "/no/such/directory/t.bin"], "cannot write --trace-file"),
])
def test_a_bad_trace_option_is_refused(node_binary, tmp_path, args, message):
    out = subprocess.run([str(node_binary), *args], capture_output=True, text=True, timeout=10,
                         cwd=tmp_path)
    assert out.returncode == 1
    assert message in out.stderr
