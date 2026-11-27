#!/usr/bin/env python3
"""Measure the node's timing on the host: release jitter of its periodic tasks, the latency of
its data path from a sample to the bus and to an observer, the periods of its cyclic frames on
the bus, its CPU load and its stack high-water marks, on an idle bus, under a flood of other
traffic and during a download, on both transports.

Everything measured here is the timing of the FreeRTOS POSIX port on a Linux host (WSL2 on the
development laptop), not of a microcontroller: when a task's thread runs is decided by the host's
scheduler. docs/measurements.md says what each figure is and what it is not.

HOW. Each run starts an observer of the bus (python -m fcn_tools.capture: kernel receive
timestamps), then the release node with its host-only trace (--trace-file: platform/posix/
posix_trace.h; every task switch, release and xTaskDelayUntil, every frame the CAN model
completed, every sample read, on CLOCK_MONOTONIC) and reads, after an orderly stop, its
statistics (run_time_us per task, stack_peak_bytes, the counters every check below needs):

  idle      the node alone with the observer, --seconds of tick time
  flood     the same with fcn_tools.bench.Flood sending frames of 0x700, below every frame of
            the node in priority, at --flood-load of the 500 kbit/s bus
  download  the node started through fcn_boot from a flash with the factory image, and the
            flasher (python -m fcn_tools.flasher --no-reset) downloading the release node as a
            new image, --downloads times, one run each; the figures cover the download itself,
            from the tester's first request frame to the node's last response frame as the
            observer saw them
  control   the idle run again without the trace, the observer only: the periods on the bus and
            the CPU shares with and without the trace show what the trace costs the node

From the trace (fcn_tools.trace, fcn_tools.latency):
  tick lead the kernel's tick count against the tick's grid over each whole run: the counts
            seen before their tick was due, and the longest stretch with the count ahead. A
            count ahead is real (a tick made up for one that was only late, until it is given
            back: EARLY_MARGIN_NS); a stretch longer than LEAD_LIMIT_PERIODS refuses the run
  release   per periodic task (sensor 10 ms, health 100 ms) and per job: the arrival (its
            period's tick as posix_tick.c had it due), the release (when the kernel's tick made
            the task ready), the start (its first switch-in), the end (its block in
            xTaskDelayUntil), the execution time between; for the processing task, released by
            the sensor task's sample, the same from the sensor job's arrival; the jobs released
            before their tick was due, and more than a tick early, are counted. When posix_tick.c
            gives ticks up it moves the grid later by as many periods, and each job is measured
            on the grid in force for its tick: the first job of a periodic task after a drop is
            listed with its lateness on the grid before as well (after_drop)
  latency   per frame of the protected telemetry (BMS_PackStatus, BMS_CellVoltages1/2,
            BMS_Temperatures): from the release of the sensor job whose sample it carries to the
            model's end of frame (transmit done), and to the observer's kernel timestamp; the
            sample of a frame is found by its E2E counter (fcn_tools.latency)
  periods   per cyclic identifier at the observer: the intervals between arrivals against the
            DBC's cycle time
  cpu       FreeRTOS's run-time statistics: each task's share of the run-time counter, the idle
            task's among them, and its run time per job, next to the task table's design budget
  stack     stack_peak_bytes against stack_size_bytes per task, the maximum over every run

    python bench/node_timing.py                      # the final measurement, about 13 minutes
    python bench/node_timing.py --transport socketcan --seconds 60
    python bench/node_timing.py --quick --out /tmp/nt
    python bench/node_timing.py --replot
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from fcn_tools import bench, capture, latency, paths, plotstyle, results, runner, sched, tester
from fcn_tools import bus as bus_module
from fcn_tools import trace as trace_module
from fcn_tools.decode import load_database

NAME = "node_timing"
VERSION = 1

#: The bit rate of every run (the node's default and the DBC's).
BITRATE = 500_000

#: The defaults of the final measurement.
SECONDS = 120
DOWNLOADS = 3
CONTROL_SECONDS = 60
FLOOD_LOAD = 0.5

#: Room in the trace per second of a run: the most the node recorded in a pilot was about 10,000
#: events a second (a download), so three times that, and never less than 2**20 events.
TRACE_EVENTS_PER_S = 30_000
TRACE_EVENTS_MIN = 1 << 20

#: How long a run of a download may take at most, and how long the node runs on after it.
DOWNLOAD_RUN_S = 120
AFTER_DOWNLOAD_S = 0.5

#: The periodic tasks and the processing task, by the name the kernel gives them.
PERIODIC = ("sensor", "health")
SAMPLE_TASK = "process"

#: The tasks whose budget the task table gives per window rather than per job (their demand in
#: any window of the table's period): docs/architecture.md, "How the node's tasks are modelled".
PER_WINDOW = ("can_rx", "can_tx", "diag")

#: The protected telemetry, by identifier.
TELEMETRY = {0x020: "BMS_PackStatus", 0x040: "BMS_CellVoltages1", 0x041: "BMS_CellVoltages2",
             0x050: "BMS_Temperatures"}

#: The diagnostic identifiers (the download window is the first and last frame of these).
UDS_REQUEST, UDS_RESPONSE = 0x7E0, 0x7E8

#: BMS_Heartbeat: the first frame of every sensor period, which marks the node's run at the
#: observer.
HEARTBEAT = 0x030

#: The start of a run that no figure of an idle, flood or control run covers: the tasks' first
#: passes, the first frames on the bus. Every figure of such a run is taken from SETTLE_S after
#: the node's first heartbeat at the observer to its last frame.
SETTLE_S = 1.0


#: A release can come before its tick was due: when a tick the platform took for lost was only
#: late, the tick made up for it (platform/posix/posix_tick_plan.h) puts the tick count one ahead
#: of the clock from the moment the late one is taken to the next tick boundary, where the tick
#: thread skips a raise to give it back (docs/bug-log.md, entry 011), and a task woken at that
#: count is released less than one period early: a count one ahead reaches the task's tick no
#: earlier than the tick before it was due. A release more than one period and this margin early
#: (room for the clock reads: the base is read just after the port raised the first tick) takes
#: the count two or more ahead, as when two late ticks are taken in the same period; one is
#: given back at each of the following boundaries. Such releases are real, and are counted
#: (released_more_than_a_tick_early) and kept in the distributions; so are the counts the trace
#: saw that far ahead (tick_lead).
EARLY_MARGIN_NS = 100_000

#: The longest stretch with the tick count ahead of the grid (fcn_tools.trace.tick_lead()) that
#: a correct node and a correct trace can give, in tick periods; a longer one refuses the run.
#: With bug 011's fix the tick thread raises no tick at a boundary while the kernel already
#: holds it and makes none up unless the count is behind (posix_tick_plan.h), so a count ahead
#: rises only by ticks that were pending when it got ahead, one at most on each task's thread (a
#: second SIGALRM sent to a thread that holds one merges with it): with the node's 8 tasks it is
#: back on the grid within 8 periods. The stretch the trace measures can join two of them when
#: the trace saw no new count on time in between; the verifier of bug 011 measured 0.7 to 1.9
#: ms in 27 conditions of 24 s rehearsals of the fix (docs/measurements.md, node_timing). The old
#: give-back of bug 011 kept the count ahead for 250 ms to 1.8 s in the same rehearsals, and a
#: grid whose ticks fall due most of a period or more later than the kernel's (a TICK_BASE with
#: too late a time) keeps nearly every count ahead for as long as that base holds: either is far
#: above 50 periods. A base less late than that keeps ahead only the counts the trace sees soon
#: enough after their tick, and is not always refused; nor is a trace that missed a base, whose
#: counts look late (docs/measurements.md, node_timing).
LEAD_LIMIT_PERIODS = 50

US = 1e-3  # ns to µs


# --------------------------------------------------------------------------------- the runs


@dataclass
class Run:
    """One run of the node, what was needed to analyse it, and the files it left."""
    condition: str
    transport: str
    name: str
    stats: dict
    trace: trace_module.Trace | None
    frames: np.ndarray
    sidecar: dict
    extra: dict


def trace_events_for(seconds: float) -> int:
    return int(min(max(TRACE_EVENTS_PER_S * seconds, TRACE_EVENTS_MIN),
                   8_388_608))


def wait_for_heartbeat(spec: str, node) -> None:
    reader = bus_module.open_bus(spec)
    try:
        first = bus_module.wait_for(reader, bus_module.is_status, timeout=tester.START_WAIT_S)
    finally:
        reader.shutdown()
    if first is None:
        raise runner.BenchFailure(f"{node.name}: no heartbeat within {tester.START_WAIT_S} s\n"
                                  f"{node.stderr()}")


def run_node(session: runner.Session, workdir: Path, condition: str, name: str, seconds: int,
             traced: bool, args: argparse.Namespace) -> Run:
    """An idle or a flood run (or the control): the observer, the flood, the node for `seconds`
    of tick time, and the statistics after its orderly stop."""
    trace_path = workdir / f"{name}.trace"
    options = ["--duration-s", str(seconds), "--bitrate", str(BITRATE),
               "--sensor-seed", str(args.sensor_seed)]
    if traced:
        options += ["--trace-file", str(trace_path), "--trace-events",
                    str(trace_events_for(seconds))]
    flood = None
    extra: dict = {}
    with capture.Observer(session.can_spec, workdir / f"{name}.capture") as observer:
        if condition == "flood":
            flood = bench.Flood(session.can_spec, load=args.flood_load, bitrate=BITRATE)
        try:
            node = session.start_node(*options, name=name)
            status = node.wait(timeout=seconds * 3 + 60)
            if status != 0:
                raise runner.BenchFailure(f"{name} ended with status {status}\n{node.stderr()}")
            stats = session.finish(node)
        finally:
            if flood is not None:
                flood.stop()
                extra["flood"] = {"load": args.flood_load, "bitrate": BITRATE,
                                  "frame_id": int(flood.frame.arbitration_id),
                                  "rate_per_s": round(flood.rate, 3), "sent": flood.sent,
                                  "given_up": flood.given_up}
        frames, sidecar = observer.stop()
    trace = trace_module.read(trace_path, stats["trace"]) if traced else None
    return Run(condition, session.transport, name, stats, trace, frames, sidecar, extra)


def run_download(session: runner.Session, workdir: Path, name: str, images: dict[str, Path],
                 image: str, args: argparse.Namespace) -> Run:
    """A run through fcn_boot from a factory flash, with the flasher downloading `image` into the
    other slot (--no-reset: the node keeps running), the trace on."""
    flash = workdir / f"{name}.flash"
    cell = workdir / f"{name}.cell"
    trace_path = workdir / f"{name}.trace"
    report_path = workdir / f"{name}.flasher.json"
    bench.factory(flash, images["node-1.0.0"])
    options = ["--bitrate", str(BITRATE), "--sensor-seed", str(args.sensor_seed),
               "--trace-file", str(trace_path), "--trace-events",
               str(trace_events_for(DOWNLOAD_RUN_S))]
    with capture.Observer(session.can_spec, workdir / f"{name}.capture") as observer:
        node = session.start_node(*options, name=name, flash_file=flash, retained_file=cell)
        wait_for_heartbeat(session.can_spec, node)
        status, report, stderr = tester.run_flasher(session.can_spec, images[image], report_path,
                                                    "--no-reset")
        if status != 0 or report.get("result") != "committed":
            raise runner.BenchFailure(f"{name}: the flasher ended with status {status}, result "
                                      f"{report.get('result')!r}\n{stderr}")
        time.sleep(AFTER_DOWNLOAD_S)
        stats = session.finish(node)
        frames, sidecar = observer.stop()
    steps = tester.steps_of(report)
    extra = {"download": {"image": image, "image_bytes": report["image"]["size"],
                          "result": report["result"], "total_s": report["total_s"],
                          "erase_s": steps["erase"]["duration_s"],
                          "transfer_s": steps["transfer"]["duration_s"],
                          "blocks": len(report["blocks_s"])}}
    trace = trace_module.read(trace_path, stats["trace"])
    return Run("download", session.transport, name, stats, trace, frames, sidecar, extra)


# ----------------------------------------------------------------------------- the analysis


def task_table(stats: dict) -> dict[str, dict]:
    return {task["name"]: task for task in stats["task_table"]["tasks"]}


def design_responses(stats: dict) -> dict[str, int | None]:
    """The response time of every task by the schedulability check (fcn_tools.sched) on the
    node's own task table, with the design budgets."""
    table = sched.parse_table(json.dumps(stats["task_table"]))
    return {result.task.name: result.response_us
            for result in sched.analyse_response_times(table.tasks)}


def observed_times(run: Run) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """The observer's frames' identifiers, kernel times on CLOCK_MONOTONIC (ns) and which of
    those times to trust (fcn_tools.capture.to_monotonic(): not across a step of the wall
    clock)."""
    for what in ("kernel_drops", "malformed"):
        if run.sidecar[what] != 0:
            raise runner.BenchFailure(f"{run.name}: the observer counted {run.sidecar[what]} "
                                      f"{what.replace('_', ' ')}: it did not see every frame")
    times, trusted = capture.to_monotonic(run.frames)
    return capture.identifiers(run.frames), times, trusted


def download_window(run: Run, ids: np.ndarray, times: np.ndarray,
                    trusted: np.ndarray) -> tuple[int, int]:
    """The download as the observer saw it: from the tester's first request frame to the
    node's last response frame (CLOCK_MONOTONIC, ns), of the frames whose times are trusted."""
    try:
        return capture.exchange_window(ids, times, UDS_REQUEST, UDS_RESPONSE, trusted)
    except ValueError as error:
        raise runner.BenchFailure(f"{run.name}: the observer saw no download: {error}") from None


def steady_window(run: Run, ids: np.ndarray, times: np.ndarray,
                  trusted: np.ndarray) -> tuple[int, int]:
    """An idle, flood or control run without its start: from SETTLE_S after the node's first
    heartbeat at the observer to its last heartbeat (CLOCK_MONOTONIC, ns), of the heartbeats
    whose times are trusted (one read across a step of the wall clock can be a second off)."""
    try:
        return capture.steady_window(ids, times, HEARTBEAT, int(SETTLE_S * 1e9), trusted)
    except ValueError as error:
        raise runner.BenchFailure(f"{run.name}: {error}") from None


def check_counts(run: Run) -> None:
    """The node's counters that the sample of a frame relies on (fcn_tools.latency's text)."""
    stats = run.stats
    problems = []
    if stats["sample_queue"]["full"] != 0:
        problems.append(f"{stats['sample_queue']['full']} samples lost at the full queue")
    for name in TELEMETRY.values():
        message = stats["telemetry"]["messages"][name]
        if message["suppressed"] or message["failed"]:
            problems.append(f"{name}: {message['suppressed']} suppressed, {message['failed']} "
                            "failed")
    if run.trace is not None and run.trace.dropped:
        problems.append(f"the trace dropped {run.trace.dropped} events (--trace-events)")
    if problems:
        raise runner.BenchFailure(f"{run.name}: " + "; ".join(problems))


def in_window(values: np.ndarray, window: tuple[int, int] | None) -> np.ndarray:
    if window is None:
        return np.ones(np.asarray(values).shape, dtype=bool)
    return (values >= window[0]) & (values <= window[1])


def tick_lead_of(run: Run, grid: trace_module.TickGrid) -> trace_module.TickLead:
    """The tick count against the grid over the whole run (fcn_tools.trace.tick_lead()).
    BenchFailure for a stretch ahead longer than LEAD_LIMIT_PERIODS, which a node that gives its
    made-up ticks back and a trace whose grid is the kernel's cannot give."""
    assert run.trace is not None
    lead = trace_module.tick_lead(run.trace, grid, EARLY_MARGIN_NS)
    if lead.longest_ns > LEAD_LIMIT_PERIODS * grid.period_ns:
        since = (lead.longest_from_ns - int(grid.bases_ns[0])) / 1e6
        raise runner.BenchFailure(
            f"{run.name}: the tick count ran ahead of the trace's grid for "
            f"{lead.longest_ns / 1e6:.1f} ms ({lead.longest_counts} counts in a row seen before "
            f"their tick was due, from {since:.1f} ms after the first tick), more than "
            f"{LEAD_LIMIT_PERIODS} periods: a node that gives its made-up ticks back "
            "(posix_tick_plan.h) is back on the grid within a few periods, so either the node "
            "keeps them (as before bug 011's fix) or the trace's grid is not the kernel's (a "
            "TICK_BASE event with too late a time)")
    return lead


def first_jobs_after_drops(run: Run, grid: trace_module.TickGrid, jobs: trace_module.Jobs,
                           arrival: np.ndarray, keep: np.ndarray) -> list[dict]:
    """The jobs of a periodic task whose tick was due on a later base than the tick of the job
    before (for its first job, than the trace's first base): the first job after one drop or
    more, when posix_tick.c gave ticks up and moved the base. Its lateness on its own base is
    the one the distributions hold, from a due time moved later by the ticks given up; on the
    base of the job before, its lateness is larger by exactly those ticks. For each such job of
    the whole run (raw, in ns): the run, the base's first tick, the drops since the job before
    and the ticks they gave up, the tick the job waited for, whether it is in the window
    (`keep`), and its release and start lateness on both bases."""
    base = grid.base_index(jobs.wake_tick)
    before = np.concatenate(([0], base[:-1]))
    found = []
    for i in np.flatnonzero(base > before):
        old = int(grid.due_ns(jobs.wake_tick[i:i + 1], base=before[i:i + 1])[0])
        found.append({
            "run": run.name, "first_tick": int(grid.first_ticks[base[i]]),
            "drops": int(base[i] - before[i]),
            "dropped_ticks": int(grid.ticks_given_up(base[i], before[i])),
            "wake_tick": int(jobs.wake_tick[i]), "in_window": bool(keep[i]),
            "release_lateness": int(jobs.release_ns[i] - arrival[i]),
            "release_lateness_on_the_base_before": int(jobs.release_ns[i]) - old,
            "start_lateness": int(jobs.start_ns[i] - arrival[i]),
            "start_lateness_on_the_base_before": int(jobs.start_ns[i]) - old})
    return found


def periodic_release(run: Run, name: str, grid: trace_module.TickGrid,
                     intervals: trace_module.Intervals,
                     window: tuple[int, int] | None) -> tuple[dict, trace_module.Jobs, np.ndarray]:
    """The release figures of the periodic task `name` in the window (raw, in ns, for pooling
    over runs), the first jobs after a drop of the whole run (first_jobs_after_drops()), and
    every job of it in the trace with its arrival."""
    trace = run.trace
    assert trace is not None
    period_ns = task_table(run.stats)[name]["period_us"] * 1000
    jobs, merged = trace_module.periodic_jobs(trace, trace.number(name), intervals)
    if len(jobs) == 0:
        raise runner.BenchFailure(f"{run.name}: the trace has no job of {name}")
    arrival = grid.due_ns(jobs.wake_tick)
    lateness = jobs.release_ns - arrival
    keep = in_window(jobs.release_ns, window)
    entry = {
        "after_drop": first_jobs_after_drops(run, grid, jobs, arrival, keep),
        "early": int((lateness[keep] < 0).sum()),
        # The tick count two or more ahead (see EARLY_MARGIN_NS).
        "beyond_a_tick": int((lateness[keep] < -(grid.period_ns + EARLY_MARGIN_NS)).sum()),
        "release_lateness": lateness[keep],
        "start_lateness": jobs.start_ns[keep] - arrival[keep],
        # From the release to the start: what the scheduler added, whatever the tick's error
        # against the clock (a release before its tick was due included).
        "start_after_release": jobs.start_ns[keep] - jobs.release_ns[keep],
        "response": jobs.end_ns[keep] - arrival[keep],
        "exec": jobs.exec_ns[keep],
        # From one job's start to the next one's, less the period: the period jitter.
        "period": np.diff(jobs.start_ns[keep]) - period_ns,
        "jobs": int(keep.sum()), "merged": merged,
        "late_ticks": int((jobs.release_tick[keep] != jobs.wake_tick[keep]).sum())}
    return entry, jobs, arrival


def analyse_trace(run: Run, window: tuple[int, int] | None) -> dict:
    """Release, latency and CPU figures of one traced run (raw, in ns, for pooling over runs),
    and the tick count against the grid over the whole run."""
    trace = run.trace
    assert trace is not None
    grid = trace_module.tick_grid(trace)
    intervals = trace_module.running_intervals(trace)
    out: dict = {"release": {}, "latency": {}, "cpu_trace": {}, "checks": {},
                 "tick_lead": tick_lead_of(run, grid)}
    table = task_table(run.stats)

    periodic: dict[str, tuple[trace_module.Jobs, np.ndarray]] = {}
    for name in PERIODIC:
        entry, jobs, arrival = periodic_release(run, name, grid, intervals, window)
        out["release"][name] = entry
        periodic[name] = (jobs, arrival)

    # The processing task: released by the sensor task's sample, measured from that job's
    # arrival; a stretch released by the health task (the pack mutex given) continues a job.
    sensor_jobs, sensor_arrival = periodic["sensor"]
    process = trace.number(SAMPLE_TASK)
    health = trace.number("health")
    sensor = trace.number("sensor")
    jobs = trace_module.task_jobs(trace, process, intervals)
    jobs, merged = trace_module.merge_continuations(jobs, jobs.readied_by == health)
    by_sample = jobs.readied_by == sensor
    owner = np.searchsorted(sensor_jobs.start_ns, jobs.release_ns, side="right") - 1
    by_sample &= owner >= 0
    by_sample &= jobs.release_ns <= sensor_jobs.end_ns[np.maximum(owner, 0)]
    keep = by_sample & in_window(jobs.release_ns, window)
    arrival = sensor_arrival[owner[keep]]
    out["release"][SAMPLE_TASK] = {
        "release_lateness": jobs.release_ns[keep] - arrival,
        "start_lateness": jobs.start_ns[keep] - arrival,
        "start_after_release": jobs.start_ns[keep] - jobs.release_ns[keep],
        "response": jobs.end_ns[keep] - arrival,
        "exec": jobs.exec_ns[keep],
        "period": np.diff(jobs.start_ns[keep]) - table["sensor"]["period_us"] * 1000,
        "jobs": int(keep.sum()), "merged": merged,
        "not_by_a_sample": int((~by_sample & in_window(jobs.release_ns, window)).sum())}

    # Samples and the frames that carry them.
    numbers, reads = trace_module.sensor_reads(trace)
    samples, overrun_samples = latency.samples_of_jobs(
        numbers, reads, sensor_jobs.start_ns, sensor_jobs.end_ns, sensor_arrival,
        sensor_jobs.release_ns)
    out["checks"]["overrun_samples"] = overrun_samples
    out["checks"]["sensor_overruns"] = run.stats["sensor"]["overruns"]
    sent = trace_module.sent_frames(trace)
    sensor_period_ms = task_table(run.stats)["sensor"]["period_us"] // 1000
    messages = run.stats["telemetry"]["messages"]
    for can_id, name in TELEMETRY.items():
        mine = sent.of(can_id)
        every = messages[name]["cycle_ms"] // sensor_period_ms
        found = latency.frame_latency(name, can_id, mine, every, samples,
                                      period_ns=sensor_period_ms * 1_000_000)
        queue = messages[name]["queue"]
        if not queue["sent"] <= len(mine) <= queue["sent"] + 3:
            raise runner.BenchFailure(f"{run.name}: {len(mine)} frames of {name} in the trace, "
                                      f"the node's queue sent {queue['sent']}")
        if found.gaps > queue["superseded"]:
            raise runner.BenchFailure(f"{run.name}: {found.gaps} publishes of {name} missing on "
                                      f"the bus, the queue superseded {queue['superseded']}")
        keep = in_window(found.eof_ns, window)
        out["latency"][name] = {"found": found, "keep": keep}

    # CPU from the trace, for the window (the statistics cover the whole run).
    start, end = window if window is not None else trace_module.window(trace)
    total = end - start
    for number, name in trace.names.items():
        ran = int(trace_module.running_between(intervals, number, np.array([start]),
                                               np.array([end]))[0])
        task_jobs = trace_module.task_jobs(trace, number, intervals)
        mine = in_window(task_jobs.release_ns, window)
        entry = {"share": ran / total, "ran_ns": ran, "window_ns": total,
                 "jobs": int(mine.sum()), "jobs_whole_run": len(task_jobs),
                 "exec": task_jobs.exec_ns[mine]}
        if name in PER_WINDOW and name in table:
            # Consecutive windows of the table's period on CLOCK_MONOTONIC from the start of
            # the analysis window: what the task ran in each.
            period_us = table[name].get("period_us", table[name].get("min_interarrival_us"))
            edges = np.arange(start, end + 1, int(period_us) * 1000, dtype=np.int64)
            if edges.size > 1:
                entry["window_us"] = int(period_us)
                entry["demand"] = trace_module.demand_per_window(intervals, number, edges)
        out["cpu_trace"][name] = entry
    out["cpu_trace_window_ns"] = total
    first, last = trace_module.window(trace)
    out["trace_rate_per_s"] = len(trace.events) / ((last - first) / 1e9)
    return out


def analyse_observer(run: Run, ids: np.ndarray, times: np.ndarray, trusted: np.ndarray,
                     window: tuple[int, int] | None, db) -> dict:
    """The periods of every cyclic frame of the node at the observer."""
    periods = {}
    # In or out of the window by its time, or, for a frame whose time is not trusted, by the
    # time of its read (at most the read's delay later): a time across a step of the wall clock
    # could drop a frame inside the window and join its neighbours into one long interval.
    placed = np.where(trusted, times, run.frames["read_ns"].astype(np.int64))
    for message in db.messages:
        cycle = message.cycle_time
        if not cycle or "BMS" not in message.senders:
            continue
        mine = (ids == message.frame_id) & in_window(placed, window)
        if mine.sum() < 2:
            continue
        found = latency.interarrival(times[mine], cycle, trusted[mine])
        periods[message.name] = {"id": message.frame_id, "cycle_ms": cycle,
                                 "interval": found["interval_ns"],
                                 "deviation": found["deviation_ns"], "long": found["long"],
                                 "untrusted": found["untrusted"]}
    return periods


def match_frames(run: Run, ids: np.ndarray, times: np.ndarray, trusted: np.ndarray,
                 analysis: dict) -> None:
    """Every frame the node sent, at the observer: the same frames, in the same order (checked);
    for the protected telemetry the observer's time of each one."""
    sent = trace_module.sent_frames(run.trace)
    for can_id in np.unique(sent.can_id):
        mine = sent.of(int(can_id))
        seen = run.frames[ids == can_id]
        try:
            latency.match_observed(mine.data, mine.dlc, seen["data"], seen["dlc"])
        except latency.MatchError as error:
            raise runner.BenchFailure(f"{run.name}: identifier {int(can_id):#05x}: {error}") \
                from None
        if int(can_id) in TELEMETRY:
            entry = analysis["latency"][TELEMETRY[int(can_id)]]
            # The observer's frames of the identifier are the node's, one for one (checked
            # above): the frames the latency kept are at the same positions.
            entry["observed_ns"] = times[ids == can_id][entry["found"].position]
            entry["observed_trusted"] = trusted[ids == can_id][entry["found"].position]


# ------------------------------------------------------------------------ the result's data


def dist_us(values) -> dict:
    return results.distribution(np.asarray(values, dtype=np.float64), "us", scale=US)


def raw_us(values) -> dict:
    return results.raw_samples(np.asarray(values, dtype=np.float64), "us", scale=US)


def pooled(parts: list[dict], key: str) -> np.ndarray:
    arrays = [np.asarray(part[key]) for part in parts if key in part]
    return np.concatenate(arrays) if arrays else np.zeros(0)


#: How the stretch of tick_lead_data() is measured (fcn_tools.trace.tick_lead()).
STRETCH_MEASURED = (
    "consecutive counts each first seen before its tick was due, from the first sight of the "
    "first to the due time of the last; the trace sees the count only at a SWITCH or READY "
    "event, so a stretch begins at a sight, which can be after the count got ahead")


def tick_lead_data(leads: list[trace_module.TickLead]) -> dict:
    """The tick count against the grid, over the whole of each run of a condition: the counts
    the trace saw added up over the runs, and the most and the longest of any run."""
    longest = max(lead.longest_ns for lead in leads)
    return {"counts_seen": sum(lead.counts_seen for lead in leads),
            "seen_before_due": sum(lead.seen_before_due for lead in leads),
            "seen_more_than_a_tick_before_due": sum(lead.seen_beyond for lead in leads),
            "most_before_due_us": round(max(lead.most_before_due_ns for lead in leads) * US, 3),
            "longest_stretch_ahead": results.figure(round(longest / 1e6, 3), "ms"),
            "stretch": STRETCH_MEASURED}


def release_counts(parts: list[dict]) -> dict:
    """The jobs of a periodic task (periodic_release()'s entries of a condition's runs) released
    at a later tick than the one they waited for (the scheduler was suspended at its tick),
    released before their tick was due, and of those the ones released more than a period and
    EARLY_MARGIN_NS early (the tick count two or more ahead), added up over the runs."""
    return {"released_at_a_later_tick": sum(part["late_ticks"] for part in parts),
            "released_before_due": sum(part["early"] for part in parts),
            "released_more_than_a_tick_early": sum(part["beyond_a_tick"] for part in parts)}


def after_drop_data(parts: list[dict]) -> list[dict]:
    """The first jobs after a drop (first_jobs_after_drops()) of a condition's runs, in µs."""
    return [{**{key: found[key] for key in ("run", "first_tick", "drops", "dropped_ticks",
                                              "wake_tick", "in_window")},
             **{f"{key}_us": round(found[key] * US, 3) for key in (
                 "release_lateness", "release_lateness_on_the_base_before", "start_lateness",
                 "start_lateness_on_the_base_before")}}
            for part in parts for found in part["after_drop"]]


def condition_data(runs: list[Run], analyses: list[dict], observers: list[dict]) -> dict:
    """One condition of one transport: the figures pooled over its runs."""
    first = runs[0].stats
    responses = design_responses(first)
    table = task_table(first)
    data: dict = {"runs": [], "tick_lead": {}, "release": {}, "latency": {}, "periods": {},
                  "cpu": {}, "stack": []}
    for run, analysis in zip(runs, analyses):
        stats = run.stats
        data["runs"].append({
            "name": run.name, "elapsed_s": round(stats["elapsed_ns"] / 1e9, 3),
            "build_id": stats["build_id"], "trace_events": stats["trace"]["events"],
            "trace_dropped": stats["trace"]["dropped"],
            "trace_event_cost_ns": stats["trace"]["event_cost_ns"],
            "trace_events_per_s": round(analysis["trace_rate_per_s"], 1),
            # The trace's own cost: its events a second times what one cost at the node's
            # start, as a share of the time (an estimate: the cost was timed, not each event).
            "trace_time_share": round(analysis["trace_rate_per_s"]
                                      * stats["trace"]["event_cost_ns"] * 1e-9, 7),
            "analysis_window_s": round(analysis["cpu_trace_window_ns"] / 1e9, 3),
            "tick_dropped": stats["tick"]["dropped"], "tick_catch_ups": stats["tick"]["catch_ups"],
            "tick_give_backs": stats["tick"]["give_backs"],
            "sensor_overruns": stats["sensor"]["overruns"],
            "sensor_skipped_periods": stats["sensor"]["skipped_periods"],
            "overrun_samples_in_trace": analysis["checks"]["overrun_samples"],
            "process_backlog": stats["process"]["backlog"],
            "wire_overruns": stats["can"]["controller"]["wire_overruns"],
            "pacing_lag_max_us": round(stats["can"]["pacing"]["lag_max_ns"] * US, 3),
            "observer_frames": run.sidecar["frames"],
            "observer_receive_buffer_bytes": run.sidecar["receive_buffer_bytes"],
            "wall_clock_steps": run.sidecar["clock_steps"],
            "largest_wall_clock_step_us": round(run.sidecar["largest_clock_step_ns"] * US, 3),
            "observer_wide_brackets": run.sidecar["wide_brackets"],
            **run.extra})

    for name in (*PERIODIC, SAMPLE_TASK):
        parts = [analysis["release"][name] for analysis in analyses]
        entry = {"arrival": ("its period's tick, as due on the base in force for that tick"
                             if name != SAMPLE_TASK else
                             "the arrival of the sensor job whose sample released it"),
                 "jobs": sum(part["jobs"] for part in parts),
                 "merged_parts": sum(part["merged"] for part in parts),
                 "release_lateness": dist_us(pooled(parts, "release_lateness")),
                 "start_lateness": dist_us(pooled(parts, "start_lateness")),
                 "start_after_release": dist_us(pooled(parts, "start_after_release")),
                 "response": dist_us(pooled(parts, "response")),
                 "execution": dist_us(pooled(parts, "exec")),
                 "period_deviation": dist_us(pooled(parts, "period")),
                 "design_response_us": responses.get(name),
                 "design_jitter_us": table[name]["jitter_us"] if name in table else None,
                 "raw_start_lateness": raw_us(pooled(parts, "start_lateness"))}
        if name == SAMPLE_TASK:
            entry["not_released_by_a_sample"] = sum(part["not_by_a_sample"] for part in parts)
        else:
            entry.update(release_counts(parts))
            entry["after_drop"] = after_drop_data(parts)
        data["release"][name] = entry
    data["tick_lead"] = tick_lead_data([analysis["tick_lead"] for analysis in analyses])

    for name in TELEMETRY.values():
        to_tx, to_tx_arrival, to_obs, to_obs_arrival, delivery = [], [], [], [], []
        frames = gaps = unreleased = untrusted = 0
        for analysis in analyses:
            entry = analysis["latency"][name]
            found, keep = entry["found"], entry["keep"]
            observed = entry["observed_ns"]
            seen = keep & entry["observed_trusted"]
            release = found.eof_ns - found.from_release_ns
            arrival = found.eof_ns - found.from_arrival_ns
            to_tx.append(found.from_release_ns[keep])
            to_tx_arrival.append(found.from_arrival_ns[keep])
            to_obs.append((observed - release)[seen])
            to_obs_arrival.append((observed - arrival)[seen])
            delivery.append((observed - found.eof_ns)[seen])
            frames += int(keep.sum())
            untrusted += int((keep & ~entry["observed_trusted"]).sum())
            gaps += found.gaps
            unreleased += found.unreleased
        data["latency"][name] = {
            "frames": frames,
            "frames_time_not_trusted": untrusted,
            "publishes_missing_on_the_bus": gaps,
            "frames_of_an_overrun_sample": unreleased,
            "frames_at_the_runs_edges": sum(analysis["latency"][name]["found"].outside
                                            for analysis in analyses),
            "sample_to_tx_done": dist_us(np.concatenate(to_tx)),
            "sample_to_observer": dist_us(np.concatenate(to_obs)),
            "arrival_to_tx_done": dist_us(np.concatenate(to_tx_arrival)),
            "arrival_to_observer": dist_us(np.concatenate(to_obs_arrival)),
            "tx_done_to_observer": dist_us(np.concatenate(delivery)),
            "raw_sample_to_tx_done": raw_us(np.concatenate(to_tx)),
            "raw_sample_to_observer": raw_us(np.concatenate(to_obs))}

    data["periods"] = periods_data(observers)
    data["cpu"] = cpu_data(runs, analyses)
    data["stack"] = [stack_entry(use) for use in latency.stack_maxima(
        latency.stack_uses(run.stats["tasks"]) for run in runs).values()]
    return data


def periods_data(observers: list[dict]) -> dict:
    out = {}
    for name in observers[0]:
        parts = [observer[name] for observer in observers if name in observer]
        out[name] = {"id": parts[0]["id"], "cycle_ms": parts[0]["cycle_ms"],
                     "intervals": sum(part["interval"].size for part in parts),
                     "a_cycle_and_a_half_or_longer": sum(part["long"] for part in parts),
                     "intervals_time_not_trusted": sum(part["untrusted"] for part in parts),
                     "interval": dist_us(pooled(parts, "interval")),
                     "deviation": dist_us(pooled(parts, "deviation"))}
    return out


def cpu_data(runs: list[Run], analyses: list[dict] | None) -> dict:
    """The run-time statistics pooled over the runs, and the trace's figures for the window."""
    run_time: dict[str, int] = {}
    total = 0
    for run in runs:
        total += run.stats["run_time_total_us"]
        for task in run.stats["tasks"]:
            run_time[task["name"]] = run_time.get(task["name"], 0) + task["run_time_us"]
    table = task_table(runs[0].stats)
    shares = latency.cpu_shares([{"name": name, "run_time_us": value}
                                 for name, value in run_time.items()], total)
    out: dict = {"run_time_total_us": total, "idle_share": round(shares.get("IDLE", 0.0), 6),
                 "tasks": {}}
    for name, value in run_time.items():
        entry: dict = {"run_time_us": value, "share": round(shares[name], 6)}
        budget = table.get(name)
        if budget is not None:
            window_us = budget.get("period_us", budget.get("min_interarrival_us"))
            entry["design_budget_us"] = budget["wcet_us"]
            entry["design_budget_per"] = (f"window of {window_us} us" if name in PER_WINDOW
                                          else "job")
        if analyses is not None:
            parts = [analysis["cpu_trace"][name] for analysis in analyses
                     if name in analysis["cpu_trace"]]
            # The jobs the trace counted over each whole run, which the statistics' run time
            # covers, and the run time per job; the trace's own figures for the window.
            whole = sum(part["jobs_whole_run"] for part in parts)
            entry["jobs_whole_runs"] = whole
            per_job = latency.run_time_per_job(value, whole)
            entry["run_time_per_job_us"] = None if per_job is None else round(per_job, 3)
            # Pooled over the runs' windows, which differ in length (the downloads): the time the
            # task ran in all of them over their total, not a mean of the runs' shares.
            window_ns = sum(part["window_ns"] for part in parts)
            entry["trace_share_in_window"] = (
                round(latency.pooled_share([part["ran_ns"] for part in parts],
                                           [part["window_ns"] for part in parts]), 6)
                if parts and window_ns > 0 else None)
            entry["jobs_in_window"] = sum(part["jobs"] for part in parts)
            if name != "IDLE":
                entry["execution_per_job"] = dist_us(pooled(parts, "exec"))
            if any("demand" in part for part in parts):
                entry["demand_per_window"] = dist_us(pooled(parts, "demand"))
                entry["window_us"] = parts[0]["window_us"]
        out["tasks"][name] = entry
    return out


def stack_entry(use: latency.StackUse) -> dict:
    return {"name": use.name, "peak_bytes": use.peak_bytes, "start_bytes": use.start_bytes,
            "own_bytes": use.own_bytes, "size_bytes": use.size_bytes,
            "margin_bytes": use.margin_bytes}


def control_data(run: Run, observer: dict) -> dict:
    return {"name": run.name, "elapsed_s": round(run.stats["elapsed_ns"] / 1e9, 3),
            "trace": run.stats["trace"], "observer_frames": run.sidecar["frames"],
            "wall_clock_steps": run.sidecar["clock_steps"],
            "tick_dropped": run.stats["tick"]["dropped"],
            "periods": periods_data([observer]), "cpu": cpu_data([run], None),
            "stack": [stack_entry(use) for use in latency.stack_uses(run.stats["tasks"])]}


# --------------------------------------------------------------------------------- measuring


def measure(binaries: runner.Binaries, args: argparse.Namespace) -> tuple[dict, dict[str, str]]:
    db = load_database()
    workroot = runner.bench_workdir(NAME)
    (workroot / "images").mkdir()
    # The images are packed from the release build, as the tests pack them (an image of the
    # asan node does not fit a slot): its build id is the tree's as well, which the node that
    # runs from the flash reports in its statistics.
    images = bench.make_images(workroot / "images", paths.build_root() / "release" / "bin")
    image = "data" if args.quick else "node-1.1.0"
    data: dict = {"transports": {}}
    reported: dict[str, str] = {}
    all_stacks: list[list[latency.StackUse]] = []
    for transport in args.transports:
        workdir = workroot / transport
        print(f"{transport}: ...", flush=True)
        by_condition: dict = {}
        with runner.Session(workdir, transport, binaries=binaries) as session:
            runs: dict[str, list[Run]] = {"idle": [], "flood": [], "download": []}
            for condition in ("idle", "flood"):
                print(f"  {condition}, {args.seconds} s ...", flush=True)
                runs[condition].append(run_node(session, workdir, condition, condition,
                                                args.seconds, True, args))
            for index in range(args.downloads):
                print(f"  download {index + 1} of {args.downloads} ({image}) ...", flush=True)
                runs["download"].append(run_download(session, workdir, f"download-{index + 1}",
                                                     images, image, args))
            print(f"  control (no trace), {args.control_seconds} s ...", flush=True)
            control = run_node(session, workdir, "control", "control", args.control_seconds,
                               False, args)
        for condition, condition_runs in runs.items():
            analyses, observers = [], []
            for run in condition_runs:
                check_counts(run)
                ids, times, trusted = observed_times(run)
                window = (download_window(run, ids, times, trusted) if condition == "download"
                          else steady_window(run, ids, times, trusted))
                analysis = analyse_trace(run, window)
                match_frames(run, ids, times, trusted, analysis)
                analyses.append(analysis)
                observers.append(analyse_observer(run, ids, times, trusted, window, db))
                reported[f"{transport} {run.name}"] = run.stats["build_id"]
                all_stacks.append(latency.stack_uses(run.stats["tasks"]))
            by_condition[condition] = condition_data(condition_runs, analyses, observers)
        ids, times, trusted = observed_times(control)
        by_condition["control"] = control_data(control, analyse_observer(
            control, ids, times, trusted, steady_window(control, ids, times, trusted), db))
        reported[f"{transport} control"] = control.stats["build_id"]
        all_stacks.append(latency.stack_uses(control.stats["tasks"]))
        data["transports"][transport] = by_condition
        data.setdefault("task_table", runs["idle"][0].stats["task_table"])
    data["stack_max"] = [stack_entry(use) for use in latency.stack_maxima(all_stacks).values()]
    data["workdir"] = str(workroot)
    return data, reported


# -------------------------------------------------------------------------------- the summary


def after_drop_count(condition: dict, task: str) -> dict:
    """The first jobs after a drop of `task` in the analysis window of a condition's runs."""
    return results.figure(sum(found["in_window"]
                              for found in condition["release"][task]["after_drop"]), "jobs")


def summary_of(data: dict) -> dict:
    out: dict = {}
    for transport, conditions in data["transports"].items():
        group: dict = {}
        for condition in ("idle", "flood", "download"):
            entry = conditions[condition]
            group[condition] = {
                "sensor_release_lateness": entry["release"]["sensor"]["release_lateness"],
                "sensor_start_lateness": entry["release"]["sensor"]["start_lateness"],
                # The jobs released before their tick was due (the tick count ahead of the clock
                # until a made-up tick is given back), and of those the ones more than a tick
                # early (the count two or more ahead, see EARLY_MARGIN_NS): the lateness
                # figures include them.
                "sensor_jobs": results.figure(entry["release"]["sensor"]["jobs"], "jobs"),
                "sensor_released_before_due": results.figure(
                    entry["release"]["sensor"]["released_before_due"], "jobs"),
                "sensor_released_more_than_a_tick_early": results.figure(
                    entry["release"]["sensor"]["released_more_than_a_tick_early"], "jobs"),
                # The jobs in the window measured on a base moved since the job before
                # (release.<task>.after_drop): from a due time moved later by the ticks given up.
                "sensor_first_jobs_after_a_drop": after_drop_count(entry, "sensor"),
                "health_first_jobs_after_a_drop": after_drop_count(entry, "health"),
                # From the release to the start, which the tick's error does not touch.
                "sensor_start_after_release": entry["release"]["sensor"]["start_after_release"],
                "health_start_lateness": entry["release"]["health"]["start_lateness"],
                "process_response_from_arrival": entry["release"][SAMPLE_TASK]["response"],
                "pack_status_sample_to_tx_done":
                    entry["latency"]["BMS_PackStatus"]["sample_to_tx_done"],
                "pack_status_sample_to_observer":
                    entry["latency"]["BMS_PackStatus"]["sample_to_observer"],
                "heartbeat_period_deviation": entry["periods"]["BMS_Heartbeat"]["deviation"],
                "idle_share": results.figure(entry["cpu"]["idle_share"], "fraction"),
                "cpu_share": {name: results.figure(task["share"], "fraction")
                              for name, task in entry["cpu"]["tasks"].items()},
            }
        control = conditions["control"]
        group["control_without_trace"] = {
            "heartbeat_period_deviation": control["periods"]["BMS_Heartbeat"]["deviation"],
            "idle_share": results.figure(control["cpu"]["idle_share"], "fraction")}
        out[transport] = group
    out["stack_own_bytes_max"] = {entry["name"]: results.figure(entry["own_bytes"], "bytes")
                                  for entry in data["stack_max"]}
    out["stack_size_bytes"] = {entry["name"]: results.figure(entry["size_bytes"], "bytes")
                               for entry in data["stack_max"]}
    return out


def config_of(args: argparse.Namespace, binaries: runner.Binaries) -> dict:
    return {"bitrate": BITRATE, "transports": list(args.transports), "seconds": args.seconds,
            "downloads": args.downloads, "download_image": "data" if args.quick else "node-1.1.0",
            "control_seconds": args.control_seconds, "flood_load": args.flood_load,
            "flood_bitrate": BITRATE, "flood_frame_id": int(bench.FLOOD_FRAME.arbitration_id),
            "sensor_seed": args.sensor_seed, "quick": args.quick,
            "trace_events_per_s": TRACE_EVENTS_PER_S, "binaries": binaries.origin,
            "settle_s": SETTLE_S, "after_download_s": AFTER_DOWNLOAD_S,
            "clock_offset_noise_ns": capture.OFFSET_NOISE_NS, "early_margin_ns": EARLY_MARGIN_NS,
            "lead_limit_periods": LEAD_LIMIT_PERIODS, "max_latency_ns": latency.MAX_LATENCY_NS}


TIMING_NOTE = (
    "The trace's and the observer's times are CLOCK_MONOTONIC of the same Linux VM; a task's "
    "release, start and running time are the kernel's decisions inside vTaskSwitchContext() and "
    "the tick handler, which on this port run in host threads: the figures include the host's "
    "delivery of the tick and of thread wake-ups. The execution times and CPU shares are "
    "x86-64 times of a host thread and say nothing about a Cortex-M3; the stack depths are those "
    "of host threads with x86-64 frames. Which frame carries which sample does not depend on "
    "the host; the counts of a run do: its jobs and frames, the publishes missing on the bus, "
    "the ticks dropped and the jobs released early are what the host let the node do in the "
    "run's duration, and the result keeps every one.")


# ------------------------------------------------------------------------------------ the plot


def ccdf(entry: dict) -> tuple[np.ndarray, np.ndarray]:
    """The share of samples above each value, from raw_samples()' samples or histogram."""
    if "samples" in entry:
        values = np.sort(np.asarray(entry["samples"], dtype=float))
        share = 1.0 - np.arange(values.size) / max(values.size, 1)
        return values, share
    edges = np.asarray(entry["histogram"]["edges"], dtype=float)
    counts = np.asarray(entry["histogram"]["counts"], dtype=float)
    above = counts[::-1].cumsum()[::-1] / max(counts.sum(), 1)
    return edges[:-1], above


def plot(result: dict, path: Path) -> None:
    import matplotlib.pyplot as plt

    plotstyle.apply()
    transports = list(result["transports"])
    transport = "socketcan" if "socketcan" in transports else transports[0]
    conditions = result["transports"][transport]
    fig, axes = plt.subplots(2, 2, figsize=(11.0, 8.2))
    fig.subplots_adjust(left=0.08, right=0.98, top=0.86, bottom=0.08, wspace=0.25, hspace=0.62)
    colours = {"idle": plotstyle.SERIES[0], "flood": plotstyle.SERIES[1],
               "download": plotstyle.SERIES[2]}

    ax = axes[0][0]
    for condition in ("idle", "flood", "download"):
        entry = conditions[condition]["release"]["sensor"]
        x, y = ccdf(entry["raw_start_lateness"])
        ax.step(x, y, where="post", color=colours[condition],
                label=f"{condition}: p50 {entry['start_lateness']['p50']} µs, max "
                      f"{entry['start_lateness']['max']} µs (n {entry['start_lateness']['n']})")
    ax.set_yscale("log")
    # Symmetric: a start can come before its tick was due (a tick made up for one that was only
    # late, until it is given back: posix_tick_plan.h).
    ax.set_xscale("symlog", linthresh=10.0)
    ax.set_title("Sensor task (10 ms): start after its tick was due")
    ax.set_xlabel("start lateness (µs, log scale beyond ±10 µs)")
    ax.set_ylabel("share of jobs later than x")
    plotstyle.legend_above(ax)

    ax = axes[0][1]
    for condition in ("idle", "flood", "download"):
        entry = conditions[condition]["latency"]["BMS_PackStatus"]
        x, y = ccdf(entry["raw_sample_to_tx_done"])
        ax.step(x, y, where="post", color=colours[condition],
                label=f"{condition}: p50 {entry['sample_to_tx_done']['p50']} µs, max "
                      f"{entry['sample_to_tx_done']['max']} µs (n {entry['frames']})")
    ax.set_yscale("log")
    ax.set_xscale("log")
    ax.set_title("BMS_PackStatus: sample's release to transmit done")
    ax.set_xlabel("latency (µs, log scale)")
    ax.set_ylabel("share of frames later than x")
    plotstyle.legend_above(ax)

    ax = axes[1][0]
    names = [name for name in conditions["idle"]["cpu"]["tasks"] if name != "IDLE"]
    width = 0.27
    positions = np.arange(len(names))
    for offset, condition in enumerate(("idle", "flood", "download")):
        tasks = conditions[condition]["cpu"]["tasks"]
        ax.bar(positions + (offset - 1) * width,
               [tasks.get(name, {}).get("share", 0.0) * 100 for name in names], width,
               color=colours[condition],
               label=f"{condition}: idle task "
                     f"{conditions[condition]['cpu']['idle_share'] * 100:.1f} %")
    ax.set_xticks(positions, names)
    ax.set_ylabel("share of the run-time counter (%)")
    ax.set_title("CPU share per task (FreeRTOS run-time statistics, host)")
    plotstyle.legend_above(ax)

    ax = axes[1][1]
    stack = result["stack_max"]
    names = [entry["name"] for entry in stack]
    positions = np.arange(len(names))
    ax.bar(positions - 0.2, [entry["own_bytes"] for entry in stack], 0.4,
           color=plotstyle.SERIES[0], label="the task's own use of its host stack, deepest run")
    ax.bar(positions + 0.2, [entry["size_bytes"] for entry in stack], 0.4,
           color=plotstyle.SERIES[3], label="stack it was created with (meant for the target)")
    ax.set_xticks(positions, names)
    ax.set_ylabel("bytes")
    ax.set_title("Stack: host thread depth used by the task's code")
    plotstyle.legend_above(ax)

    host = "WSL2" if result["provenance"]["host"]["is_wsl"] else "Linux"
    plotstyle.footnote(fig, f"bench/node_timing.py on {transport}, 500 kbit/s, "
                            f"{result['config']['seconds']} s per run; FreeRTOS POSIX port on a "
                            f"{host} host. Host timing, not MCU timing.")
    plotstyle.save(fig, path)


# -------------------------------------------------------------------------------------- main


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    results.add_bench_arguments(parser, quick="4 s per run, one download of the small data "
                                              "image, 3 s of control")
    parser.add_argument("--transport", choices=("both", *runner.TRANSPORTS), default="both",
                        help="the transports to measure on (default both)")
    parser.add_argument("--seconds", type=int, default=None,
                        help=f"tick time of the idle and the flood run (default {SECONDS})")
    parser.add_argument("--downloads", type=int, default=None,
                        help=f"runs with a download (default {DOWNLOADS})")
    parser.add_argument("--control-seconds", type=int, default=None,
                        help=f"tick time of the run without the trace (default {CONTROL_SECONDS})")
    parser.add_argument("--flood-load", type=float, default=FLOOD_LOAD,
                        help="the flood's share of the 500 kbit/s bus (default %(default)s)")
    parser.add_argument("--sensor-seed", type=int, default=1,
                        help="the sensor model's seed (default %(default)s)")
    args = parser.parse_args(argv)
    quick = args.quick
    args.seconds = args.seconds or (4 if quick else SECONDS)
    args.downloads = args.downloads if args.downloads is not None else (1 if quick else DOWNLOADS)
    args.control_seconds = args.control_seconds or (3 if quick else CONTROL_SECONDS)
    args.transports = runner.TRANSPORTS[::-1] if args.transport == "both" else (args.transport,)
    if args.downloads < 1 or args.seconds < 2 or args.control_seconds < 2:
        parser.error("--downloads must be at least 1, --seconds and --control-seconds at least 2")
    if not 0.0 <= args.flood_load < 1.0:
        parser.error("--flood-load is a share of the bus: 0 to below 1")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    out = results.output_dir(args)
    if args.replot:
        path = out / f"{NAME}.json"
        if not path.exists():
            print(f"nothing to replot: {path} does not exist", file=sys.stderr)
            return 1
        plot(json.loads(path.read_text()), path.with_suffix(".png"))
        print(f"redrew {path.with_suffix('.png')}")
        return 0
    try:
        load = results.check_load(args.max_load, args.allow_loaded)
        binaries = runner.prepare_binaries(build=not args.no_build)
        results.require_tree_build(binaries.as_dict())
        data, reported = measure(binaries, args)
        path = results.write_result(NAME, data, summary_of(data), config_of(args, binaries), out,
                                    version=VERSION, timing_note=TIMING_NOTE, load=load,
                                    binaries=binaries.as_dict(), reported_build_ids=reported,
                                    binary_sha256=binaries.sha256)
    except results.HostLoaded as error:
        print(error, file=sys.stderr)
        return results.EXIT_HOST_LOADED
    except results.BuildIdMismatch as error:
        print(f"no result written: {error}", file=sys.stderr)
        return results.EXIT_BUILD_MISMATCH
    except KeyboardInterrupt as error:
        return runner.interrupted_status(error)
    except (AssertionError, RuntimeError, latency.MatchError, trace_module.TraceError) as error:
        # A node, a trace or an observation that is not what the analysis needs, or a bus that
        # is not there (vcan0, scripts/vcan-up.sh): no result.
        print(f"no result written: {error}", file=sys.stderr)
        return 1
    plot(json.loads(path.read_text()), path.with_suffix(".png"))
    for transport, conditions in data["transports"].items():
        for condition in ("idle", "flood", "download"):
            entry = conditions[condition]
            sensor = entry["release"]["sensor"]["start_lateness"]
            after = entry["release"]["sensor"]["start_after_release"]
            early = entry["release"]["sensor"]["released_before_due"]
            beyond = entry["release"]["sensor"]["released_more_than_a_tick_early"]
            dropped = after_drop_count(entry, "sensor")["value"]
            stretch = entry["tick_lead"]["longest_stretch_ahead"]["value"]
            pack = entry["latency"]["BMS_PackStatus"]["sample_to_tx_done"]
            print(f"{transport:9s} {condition:8s} sensor start lateness p50 {sensor['p50']} us "
                  f"max {sensor['max']} us (n {sensor['n']}, {early} released before due, "
                  f"{beyond} more than a tick early, {dropped} first after a drop), after "
                  f"release p50 {after['p50']} us max {after['max']} us; tick count ahead "
                  f"{stretch} ms in a row at most; "
                  f"PackStatus sample to tx done p50 "
                  f"{pack['p50']} us max {pack['max']} us (n {pack['n']}); idle "
                  f"{entry['cpu']['idle_share'] * 100:.2f} %")
    print(f"wrote {path} and {path.with_suffix('.png')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
