# SPDX-License-Identifier: MIT
"""The node's host-only trace (fcn_node --trace-file, platform/posix/posix_trace.h), read and
taken apart into what bench/node_timing.py measures: when every tick was due, when each task
ran, its jobs (release, start, end, execution time), the frames the node completed on the bus
and the samples its sensor task took.

THE RECORDS. EVENT_DTYPE is posix_trace_event_t: the time (CLOCK_MONOTONIC, ns, taken where
the event happened), a value, an argument, the kind, a task number, the other task, flags and 8
bytes of data; posix_trace.h's table says what each field holds for each kind. The times of the
kernel's events are those of its decisions: a SWITCH is stamped inside vTaskSwitchContext(),
before the port hands the processor to the next task's thread, exactly where FreeRTOS's own
run-time statistics take their time, so the running times of this module and the node's
run_time_us count the same intervals.

THE TICK. TICK_BASE events say when each tick was due: tick k at base + (k - 1) * period, for k
from the event's first tick on, until a later base (posix_tick.c moves it later by the ticks it
gives up). TickGrid.due_ns() is that function, and gives a tick's due time on another base when
asked (the one in force before a drop); TickGrid.ticks_given_up() counts the ticks given up
between two bases. tick_lead() holds the kernel's tick count, which every SWITCH and READY event
carries, against it.

JOBS. A task's job begins when the kernel makes it ready from the blocked state (a READY event
while the task was blocked: released), starts at its first switch-in after that, and ends when it
is switched out blocked (a SWITCH whose task switched out is it, with the BLOCKED flag). Being
preempted or yielding (switched out still ready) does not end a job; a READY of a task that is
not blocked (the kernel moves a mutex holder between ready lists when it inherits a priority) is
no release. Its execution time is the sum of its running intervals between start and end. A job
that blocks in the middle (on a mutex) becomes two; merge_continuations() joins the second part
to the first: for a periodic task every job that was not released after an xTaskDelayUntil
(DELAY_UNTIL) is such a part. The first stretch of each task, from its creation to its first
block, is no job: it was never released.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping

import numpy as np

EVENT_DTYPE = np.dtype([("time_ns", "<u8"), ("value", "<u8"), ("arg", "<u4"), ("kind", "u1"),
                        ("task", "u1"), ("other", "u1"), ("flags", "u1"), ("data", "u1", (8,))])
assert EVENT_DTYPE.itemsize == 32

#: The file's header (posix_trace.c, file_header_t): magic, version, record size, records,
#: dropped, capacity, cost of a record (ps), records timed for it, 12 reserved bytes.
HEADER = struct.Struct("<8sIIQQQQII8x")
assert HEADER.size == 64
MAGIC = b"FCNTRACE"
FILE_VERSION = 1

SWITCH, READY, DELAY_UNTIL, TX, SENSOR, TICK_BASE = 1, 2, 3, 4, 5, 6
FLAG_BLOCKED = 0x01

#: The kernel's and the tasks' events, which the port serialises (one task thread runs at a
#: time, and the tick's handler runs on it): in their order they are in time order.
SCHEDULER_KINDS = (SWITCH, READY, DELAY_UNTIL, SENSOR)


class TraceError(ValueError):
    """A trace that cannot be what the node wrote, or that the analysis cannot use."""


@dataclass
class Trace:
    """A trace: its records in the order of their slots, what the node said about it in its
    statistics ("trace": the cost of a record, the task names) and the header's counts."""
    events: np.ndarray
    names: dict[int, str]
    capacity: int = 0
    dropped: int = 0
    cost_ps: int = 0
    cost_runs: int = 0

    def number(self, name: str) -> int:
        """The task number of the task called `name`; TraceError if the trace never saw it."""
        for number, known in self.names.items():
            if known == name:
                return number
        raise TraceError(f"the trace saw no task called {name!r}")

    def of_kind(self, *kinds: int) -> np.ndarray:
        """The records of these kinds, in time order (a stable sort keeps the slot order of
        records with the same time)."""
        selected = self.events[np.isin(self.events["kind"], kinds)]
        return selected[np.argsort(selected["time_ns"], kind="stable")]


def read(path: Path, stats_trace: Mapping) -> Trace:
    """The trace file at `path`, with the node's statistics' "trace" member for the task names.
    TraceError if the file is no trace of this layout or disagrees with the statistics."""
    raw = Path(path).read_bytes()
    if len(raw) < HEADER.size:
        raise TraceError(f"{path}: {len(raw)} bytes, no trace header")
    magic, version, record, records, dropped, capacity, cost_ps, cost_runs, _ = \
        HEADER.unpack_from(raw)
    if magic != MAGIC or version != FILE_VERSION or record != EVENT_DTYPE.itemsize:
        raise TraceError(f"{path}: not a trace of version {FILE_VERSION} "
                         f"(magic {magic!r}, version {version}, record {record} bytes)")
    body = len(raw) - HEADER.size
    if body != records * record:
        raise TraceError(f"{path}: the header says {records} records, the file holds "
                         f"{body / record:g}")
    if stats_trace.get("events") != records or stats_trace.get("dropped") != dropped:
        raise TraceError(f"{path}: {records} records and {dropped} dropped, the statistics say "
                         f"{stats_trace.get('events')} and {stats_trace.get('dropped')}")
    events = np.frombuffer(raw, dtype=EVENT_DTYPE, offset=HEADER.size).copy()
    names = {int(entry["number"]): str(entry["name"]) for entry in stats_trace["tasks"]}
    return Trace(events=events, names=names, capacity=capacity, dropped=dropped,
                 cost_ps=cost_ps, cost_runs=cost_runs)


# ---------------------------------------------------------------------------------- the tick


@dataclass(frozen=True)
class TickGrid:
    """When each tick was due (see the module text): bases sorted by their first tick."""
    first_ticks: np.ndarray
    bases_ns: np.ndarray
    period_ns: int

    def base_index(self, ticks) -> np.ndarray:
        """The base in force for each tick of `ticks` (tick counts, 1 or more), as an index into
        first_ticks and bases_ns: the last base whose first tick is at or before it (of two
        with the same first tick, the later one)."""
        ticks = np.asarray(ticks, dtype=np.int64)
        index = np.searchsorted(self.first_ticks, ticks, side="right") - 1
        if np.any(index < 0):
            raise TraceError("a tick before the first base of the trace")
        return index

    def due_ns(self, ticks, base=None) -> np.ndarray:
        """The time each tick of `ticks` (tick counts, 1 or more) was due, in ns: on the base in
        force for it, or with `base` (indices into bases_ns, as base_index() gives them) on
        those bases instead."""
        ticks = np.asarray(ticks, dtype=np.int64)
        index = self.base_index(ticks) if base is None else np.asarray(base, dtype=np.int64)
        return self.bases_ns[index] + (ticks - 1) * self.period_ns

    def ticks_given_up(self, base, since) -> np.ndarray:
        """The ticks given up from base `since` to base `base` (indices into bases_ns): the
        periods by which the one is later than the other. TraceError for a base moved by part
        of a period, which posix_tick.c never does (it moves the base by the ticks it gives
        up)."""
        moved = self.bases_ns[np.asarray(base)] - self.bases_ns[np.asarray(since)]
        if np.any(moved % self.period_ns != 0):
            raise TraceError("a base of the trace moved by part of a tick period")
        return moved // self.period_ns


def tick_grid(trace: Trace) -> TickGrid:
    """The grid of the trace's TICK_BASE events. TraceError without one, or if the period
    changes."""
    bases = trace.events[trace.events["kind"] == TICK_BASE]
    if bases.size == 0:
        raise TraceError("the trace has no TICK_BASE event: was the tick locked (--tick-mode)?")
    periods = np.unique(bases["arg"])
    if periods.size != 1:
        raise TraceError(f"the tick period changes in the trace: {periods.tolist()}")
    order = np.argsort(bases["value"], kind="stable")
    return TickGrid(first_ticks=bases["value"][order].astype(np.int64),
                    bases_ns=bases["time_ns"][order].astype(np.int64),
                    period_ns=int(periods[0]))


@dataclass(frozen=True)
class TickLead:
    """The kernel's tick count against the grid as the trace saw it (tick_lead()): how many
    counts it saw, how many of them before their tick was due (the count ahead of the clock),
    how many more than a period and a margin before (two or more ahead), the most a count was
    seen before its tick was due, and the longest stretch ahead: its length, the time of the
    sight that began it and the counts in it (0 when no count was seen ahead). Times in ns on
    CLOCK_MONOTONIC."""
    counts_seen: int
    seen_before_due: int
    seen_beyond: int
    most_before_due_ns: int
    longest_ns: int
    longest_from_ns: int
    longest_counts: int


def tick_lead(trace: Trace, grid: TickGrid, margin_ns: int = 0) -> TickLead:
    """The kernel's tick count against `grid`, from the trace's SWITCH and READY events, which
    carry the count at their time. Each count is taken at its first sight, the first of these
    events that carries it. A count seen before its tick was due was ahead of the clock then: the
    kernel held a tick that was not due yet. One seen more than a period and `margin_ns` before
    was two or more ahead. A stretch ahead is a run of consecutive counts that were each first
    seen before their tick was due, from the first sight of the first to the time the last one's
    tick was due, when the clock had caught up with it. The trace sees the count only at these
    events: a stretch begins at a sight, which can be after the count got ahead, and a count the
    trace never saw (one followed by the next before any event) is taken to be like the counts
    around it. Counts below the grid's first tick (0, before the kernel's first tick) are left
    out."""
    events = trace.of_kind(SWITCH, READY)
    counts = events["value"].astype(np.int64)
    times = events["time_ns"].astype(np.int64)
    ticked = counts >= grid.first_ticks[0]
    counts, times = counts[ticked], times[ticked]
    if counts.size == 0:
        return TickLead(0, 0, 0, 0, 0, 0, 0)
    # A count's first sight: the first event that carries a count above every one before it.
    first = np.ones(counts.size, dtype=bool)
    first[1:] = counts[1:] > np.maximum.accumulate(counts)[:-1]
    counts, times = counts[first], times[first]
    due = grid.due_ns(counts)
    before = due - times                      # > 0: seen before its tick was due
    ahead = before > 0
    out = {"counts_seen": int(counts.size), "seen_before_due": int(ahead.sum()),
           "seen_beyond": int((before > grid.period_ns + margin_ns).sum()),
           "most_before_due_ns": max(int(before.max()), 0),
           "longest_ns": 0, "longest_from_ns": 0, "longest_counts": 0}
    if ahead.any():
        # The runs of counts ahead: where `ahead` turns on, and the last count before it turns
        # off.
        edges = np.diff(np.concatenate(([0], ahead.astype(np.int8), [0])))
        starts = np.flatnonzero(edges == 1)
        ends = np.flatnonzero(edges == -1) - 1
        lengths = due[ends] - times[starts]
        best = int(np.argmax(lengths))
        out.update(longest_ns=int(lengths[best]), longest_from_ns=int(times[starts[best]]),
                   longest_counts=int(ends[best] - starts[best] + 1))
    return TickLead(**out)


# ------------------------------------------------------------------------ running intervals


@dataclass
class Intervals:
    """When which task ran: interval i is [start_ns[i], end_ns[i]) of task[i], one per switch,
    in time order. The last switch has no end and gives no interval."""
    task: np.ndarray
    start_ns: np.ndarray
    end_ns: np.ndarray

    def of(self, task: int) -> tuple[np.ndarray, np.ndarray]:
        mine = self.task == task
        return self.start_ns[mine], self.end_ns[mine]


def running_intervals(trace: Trace) -> Intervals:
    switches = trace.of_kind(SWITCH)
    times = switches["time_ns"].astype(np.int64)
    return Intervals(task=switches["task"][:-1].astype(np.int64), start_ns=times[:-1],
                     end_ns=times[1:])


def running_time(intervals: Intervals, task: int, times) -> np.ndarray:
    """F(t): the time `task` ran from the trace's start to each of `times` (ns)."""
    starts, ends = intervals.of(task)
    times = np.asarray(times, dtype=np.int64)
    if starts.size == 0:
        return np.zeros(times.shape, dtype=np.int64)
    lengths = ends - starts
    before = np.concatenate(([0], np.cumsum(lengths)))  # running time before interval i
    index = np.searchsorted(starts, times, side="right") - 1
    inside = np.where(index >= 0, np.clip(times - starts[np.maximum(index, 0)], 0,
                                          lengths[np.maximum(index, 0)]), 0)
    return np.where(index >= 0, before[np.maximum(index, 0)] + inside, 0)


def running_between(intervals: Intervals, task: int, start_ns, end_ns) -> np.ndarray:
    """The time `task` ran within each [start_ns, end_ns) (ns), elementwise."""
    return running_time(intervals, task, end_ns) - running_time(intervals, task, start_ns)


def demand_per_window(intervals: Intervals, task: int, edges) -> np.ndarray:
    """The time `task` ran in each window between consecutive `edges` (ns)."""
    return np.diff(running_time(intervals, task, edges))


# ------------------------------------------------------------------------------------- jobs


@dataclass
class Jobs:
    """The jobs of one task (see the module text), as columns. wake_tick is the tick its
    xTaskDelayUntil() blocked until before the release (-1: released otherwise); readied_by is
    the task that was running when it was released (0: none yet); parts is how many stretches
    between releases and blocks it was made of."""
    release_ns: np.ndarray
    release_tick: np.ndarray
    wake_tick: np.ndarray
    readied_by: np.ndarray
    start_ns: np.ndarray
    end_ns: np.ndarray
    exec_ns: np.ndarray
    parts: np.ndarray

    def __len__(self) -> int:
        return int(self.release_ns.size)

    def select(self, mask) -> "Jobs":
        mask = np.asarray(mask)
        return Jobs(**{name: getattr(self, name)[mask] for name in _JOB_FIELDS})


_JOB_FIELDS = ("release_ns", "release_tick", "wake_tick", "readied_by", "start_ns", "end_ns",
               "exec_ns", "parts")


def task_jobs(trace: Trace, task: int, intervals: Intervals | None = None) -> Jobs:
    """Every complete job of `task`: released, started and ended within the trace."""
    events = trace.of_kind(SWITCH, READY, DELAY_UNTIL)
    kinds = events["kind"]
    tasks = events["task"].astype(np.int64)
    others = events["other"].astype(np.int64)
    is_switch = kinds == SWITCH
    mine = np.flatnonzero((is_switch & ((tasks == task) | (others == task)))
                          | (~is_switch & (tasks == task)))
    times = events["time_ns"].astype(np.int64)
    values = events["value"].astype(np.int64)
    flags = events["flags"]
    rows: list[list[int]] = []   # release, tick, wake, position of the release, start, end
    state = "initial"            # before its first block the task was never released
    wake = -1
    job: list[int] | None = None
    for i in mine:
        kind = kinds[i]
        if kind == SWITCH:
            if others[i] == task:
                if flags[i] & FLAG_BLOCKED:
                    if job is not None and job[4] >= 0:
                        rows.append([*job, int(times[i])])
                    job = None
                    state = "blocked"
                else:
                    state = "ready"
            else:  # switched in
                if job is not None and job[4] < 0:
                    job[4] = int(times[i])
                state = "running"
        elif kind == DELAY_UNTIL:
            wake = int(values[i])
        elif state == "blocked":  # READY
            job = [int(times[i]), int(values[i]), wake, int(i), -1]
            wake = -1
            state = "ready"
    if not rows:
        empty = np.zeros(0, dtype=np.int64)
        return Jobs(*(empty for _ in _JOB_FIELDS))
    table = np.asarray(rows, dtype=np.int64)
    # Who ran when the task was released: the task of the last switch before the release.
    switch_at = np.flatnonzero(is_switch)
    last = np.searchsorted(switch_at, table[:, 3]) - 1
    readied_by = np.where(last >= 0, tasks[switch_at[np.maximum(last, 0)]], 0)
    intervals = running_intervals(trace) if intervals is None else intervals
    exec_ns = running_between(intervals, task, table[:, 4], table[:, 5])
    return Jobs(release_ns=table[:, 0], release_tick=table[:, 1], wake_tick=table[:, 2],
                readied_by=readied_by, start_ns=table[:, 4], end_ns=table[:, 5],
                exec_ns=exec_ns, parts=np.ones(table.shape[0], dtype=np.int64))


def merge_continuations(jobs: Jobs, continuation) -> tuple[Jobs, int]:
    """`jobs` with every job whose `continuation` is True joined to the job before it (its end
    and the sum of their execution times; the release and start stay the first part's), and how
    many were joined. A continuation with no job before it is dropped."""
    continuation = np.asarray(continuation, dtype=bool)
    if continuation.size != len(jobs):
        raise ValueError("one continuation flag per job")
    if len(jobs) == 0:
        return jobs, 0
    head = ~continuation
    group = np.cumsum(head) - 1          # the job each part belongs to; -1: before the first
    keep = group >= 0
    count = int(head.sum())
    merged = jobs.select(head)
    exec_ns = np.zeros(count, dtype=np.int64)
    parts = np.zeros(count, dtype=np.int64)
    end_ns = np.zeros(count, dtype=np.int64)
    np.add.at(exec_ns, group[keep], jobs.exec_ns[keep])
    np.add.at(parts, group[keep], jobs.parts[keep])
    np.maximum.at(end_ns, group[keep], jobs.end_ns[keep])
    merged.exec_ns, merged.parts, merged.end_ns = exec_ns, parts, end_ns
    return merged, int((continuation & keep).sum())


def periodic_jobs(trace: Trace, task: int, intervals: Intervals | None = None) -> tuple[Jobs, int]:
    """The jobs of a task that runs in an xTaskDelayUntil() loop: every stretch released after a
    DELAY_UNTIL begins a job, every other one (after a block on a mutex) continues it."""
    jobs = task_jobs(trace, task, intervals)
    return merge_continuations(jobs, jobs.wake_tick < 0)


# ----------------------------------------------------------------------- frames and samples


@dataclass
class SentFrames:
    """The node's frames as the CAN hardware thread completed them, in the order of their end
    of frame: identifier, flags, DLC, data, the model's end of frame and the time of the write
    to the transport (ns)."""
    can_id: np.ndarray
    flags: np.ndarray
    dlc: np.ndarray
    data: np.ndarray
    eof_ns: np.ndarray
    write_ns: np.ndarray

    def __len__(self) -> int:
        return int(self.can_id.size)

    def of(self, can_id: int) -> "SentFrames":
        mine = self.can_id == can_id
        return SentFrames(*(getattr(self, name)[mine] for name in
                            ("can_id", "flags", "dlc", "data", "eof_ns", "write_ns")))


def sent_frames(trace: Trace) -> SentFrames:
    tx = trace.events[trace.events["kind"] == TX]
    tx = tx[np.argsort(tx["value"], kind="stable")]
    return SentFrames(can_id=tx["arg"].astype(np.int64), flags=tx["flags"].astype(np.int64),
                      dlc=tx["task"].astype(np.int64), data=tx["data"].copy(),
                      eof_ns=tx["value"].astype(np.int64), write_ns=tx["time_ns"].astype(np.int64))


def sensor_reads(trace: Trace) -> tuple[np.ndarray, np.ndarray]:
    """The samples' numbers and the times of their reads, in the order of the reads."""
    reads = trace.of_kind(SENSOR)
    return reads["value"].astype(np.int64), reads["time_ns"].astype(np.int64)


def window(trace: Trace) -> tuple[int, int]:
    """The span of the trace's scheduler events: from the first switch to the last (ns)."""
    switches = trace.of_kind(SWITCH)
    if switches.size < 2:
        raise TraceError("the trace has fewer than two task switches")
    return int(switches["time_ns"][0]), int(switches["time_ns"][-1])
