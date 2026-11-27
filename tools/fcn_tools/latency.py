# SPDX-License-Identifier: MIT
"""The analysis of bench/node_timing.py, as functions of arrays that tests/python feeds with
constructed data: the sensor task's samples and their releases, the frames that carry them, the
same frames at an observer of the bus, the periods of the cyclic frames there, the CPU load from
the node's run-time statistics and its stack high-water marks.

THE SAMPLE OF A FRAME. The processing task protects the messages of a sample in the order of the
samples, and every protect advances that message's E2E Profile 11 counter by one, from 0, modulo
15 (lib/e2e). The k-th BMS_PackStatus the node publishes therefore carries counter k mod 15, and
the sample of the k-th processed sample; BMS_CellVoltages1/2 go out with every second sample and
BMS_Temperatures with every tenth (the message's cycle time over the sensor's period). A publish
whose frame the transmit queue replaced before it went out (superseded) leaves a gap in the
counters on the bus. publish_indices() undoes the wrap: each step between two frames of a message
on the bus is its counter difference modulo 15, which is right as long as fewer than 15 publishes
in a row never reached the bus; a bench checks the count of the gaps against the node's own
counters of superseded frames. That holds the sample of publish k to be sample k * every only if
every sample was processed and every publish made (no sample lost at the queue, the telemetry
never switched off): the bench checks the node's counters for that as well.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Mapping, Sequence

import numpy as np

from . import trace as trace_module

#: E2E Profile 11 (lib/e2e, fcn_tools.e2e): the counter, in the low nibble of byte 1, runs
#: 0..14; 15 is reserved.
COUNTER_MODULUS = 15
COUNTER_BYTE = 1


class MatchError(ValueError):
    """Frames, samples or observations that cannot be matched as the analysis assumes."""


def e2e_counters(data: np.ndarray) -> np.ndarray:
    """The E2E Profile 11 counters of frames (`data`: one row of 8 bytes per frame)."""
    return (np.asarray(data)[:, COUNTER_BYTE] & 0x0F).astype(np.int64)


def publish_indices(counters: Sequence[int]) -> np.ndarray:
    """The publish number (0, 1, 2, ...) of each frame of one message on the bus, in their order,
    from their E2E counters (see the module text). The first frame's publish is its counter: the
    first publish carries counter 0, and a node that lost its first 15 publishes before one
    reached the bus is not what this assumes (the bench bounds the latency it computes from
    this). MatchError for a reserved counter (15) or a counter that repeats."""
    counters = np.asarray(counters, dtype=np.int64)
    if counters.size == 0:
        return counters
    if np.any((counters < 0) | (counters >= COUNTER_MODULUS)):
        raise MatchError("an E2E counter outside 0..14")
    steps = np.diff(counters) % COUNTER_MODULUS
    if np.any(steps == 0):
        raise MatchError("an E2E counter repeats in consecutive frames of one message")
    return counters[0] + np.concatenate(([0], np.cumsum(steps)))


def gaps(indices: np.ndarray) -> int:
    """Publishes between the first and the last frame that never reached the bus."""
    indices = np.asarray(indices, dtype=np.int64)
    return int(indices[-1] - indices[0] + 1 - indices.size) if indices.size else 0


# ------------------------------------------------------------------------- samples and jobs


@dataclass
class Samples:
    """The sensor task's samples (by number, from 0) with what the trace says about each: when it
    was read, and for the sample its job's first pass took, the job's arrival (its period's tick
    as it was due, ns), its release (when the kernel made the sensor task ready) and its start.
    A sample of a later pass of the same job, after an overrun, has none of the three (-1)."""
    number: np.ndarray
    read_ns: np.ndarray
    arrival_ns: np.ndarray
    release_ns: np.ndarray
    start_ns: np.ndarray

    @property
    def released(self) -> np.ndarray:
        return self.release_ns >= 0

    def lookup(self, numbers) -> tuple[np.ndarray, np.ndarray]:
        """The positions of the samples `numbers`, and which of them the trace has: a sample
        before the first or after the last complete sensor job is not there (the run's first
        and last moments). MatchError for one missing in between."""
        numbers = np.asarray(numbers, dtype=np.int64)
        if self.number.size == 0:
            raise MatchError("the trace has no sample")
        position = np.minimum(np.searchsorted(self.number, numbers), self.number.size - 1)
        known = self.number[position] == numbers
        edge = (numbers < self.number[0]) | (numbers > self.number[-1])
        if np.any(~known & ~edge):
            raise MatchError("a frame's sample is not among the samples of the trace")
        return position, known


def samples_of_jobs(numbers: np.ndarray, read_ns: np.ndarray, job_start_ns: np.ndarray,
                    job_end_ns: np.ndarray, job_arrival_ns: np.ndarray,
                    job_release_ns: np.ndarray) -> tuple[Samples, int]:
    """Each sample with the sensor job its read fell into (see Samples), and the number of
    samples taken by a later pass of a job (overruns). Samples read outside every job (before
    the first release, after the last end) are left out. MatchError if the numbers are not
    consecutive."""
    numbers = np.asarray(numbers, dtype=np.int64)
    read_ns = np.asarray(read_ns, dtype=np.int64)
    if numbers.size and not np.array_equal(numbers, numbers[0] + np.arange(numbers.size)):
        raise MatchError("the samples' numbers are not consecutive")
    job = np.searchsorted(job_start_ns, read_ns, side="right") - 1
    inside = (job >= 0) & (read_ns < np.asarray(job_end_ns)[np.maximum(job, 0)])
    numbers, read_ns, job = numbers[inside], read_ns[inside], job[inside]
    first = np.concatenate(([True], job[1:] != job[:-1])) if job.size else job.astype(bool)
    arrival = np.where(first, np.asarray(job_arrival_ns)[job], -1)
    release = np.where(first, np.asarray(job_release_ns)[job], -1)
    start = np.where(first, np.asarray(job_start_ns)[job], -1)
    return (Samples(number=numbers, read_ns=read_ns, arrival_ns=arrival, release_ns=release,
                    start_ns=start), int((~first).sum()))


# ---------------------------------------------------------------------------- frame latency


@dataclass
class FrameLatency:
    """One protected message: per frame on the bus whose sample the trace has with its release
    (position: its place among the message's frames), its sample, its end of frame and the
    intervals from the sample's release and arrival to it (ns); the publishes missing on the
    bus between its first and last frame (gaps), the frames of a sample taken by a later pass of
    a sensor job (unreleased) and those of a sample at the run's edges the trace has no job for
    (outside)."""
    name: str
    can_id: int
    position: np.ndarray
    sample: np.ndarray
    eof_ns: np.ndarray
    write_ns: np.ndarray
    from_release_ns: np.ndarray
    from_arrival_ns: np.ndarray
    gaps: int
    unreleased: int
    outside: int


#: A frame that ends more than this after its sample is taken for a wrong match (or a host that
#: stood still for a second), and the analysis refuses it.
MAX_LATENCY_NS = 1_000_000_000


def frame_latency(name: str, can_id: int, frames: trace_module.SentFrames, every: int,
                  samples: Samples, *, period_ns: int | None = None) -> FrameLatency:
    """The latency of every frame of one protected message (`frames`: the node's frames of its
    identifier, in their order), whose k-th publish carries sample k * `every`. Frames whose
    sample was taken by a later pass of a job (no release) are left out and counted. MatchError
    if a latency is negative or beyond MAX_LATENCY_NS.

    A frame ending before its sample was read shows a match one or more samples too late; a match
    too early (every frame taken for the sample before its own) breaks no such rule, and every
    latency is then a period of the sensor too long. With `period_ns` (the sensor's period) the
    median latency must also be below it, MatchError otherwise: a node whose frames reach the bus
    a whole period after their sample more often than not would have missed every deadline of
    its task table, and a single late frame (a host stall) does not move the median."""
    indices = publish_indices(e2e_counters(frames.data))
    numbers = indices * every
    positions, known = samples.lookup(numbers)
    read = samples.read_ns[positions]
    if np.any(known & (frames.eof_ns <= read)):
        raise MatchError(f"{name}: a frame ends before its sample was read: the match is wrong")
    released = known & samples.released[positions]
    kept = np.flatnonzero(released)
    eof = frames.eof_ns[kept]
    from_release = eof - samples.release_ns[positions[kept]]
    if np.any(from_release > MAX_LATENCY_NS):
        raise MatchError(f"{name}: a frame ends more than {MAX_LATENCY_NS / 1e9:g} s after its "
                         "sample's release")
    if period_ns is not None and from_release.size and np.median(from_release) >= period_ns:
        raise MatchError(f"{name}: the median frame ends {float(np.median(from_release)):.0f} ns "
                         f"after its sample's release, a sensor period ({period_ns} ns) or more: "
                         "the frames are matched to samples before their own")
    return FrameLatency(name=name, can_id=can_id, position=kept, sample=numbers[kept],
                        eof_ns=eof, write_ns=frames.write_ns[kept], from_release_ns=from_release,
                        from_arrival_ns=eof - samples.arrival_ns[positions[kept]],
                        gaps=gaps(indices), unreleased=int((known & ~released).sum()),
                        outside=int((~known).sum()))


# ----------------------------------------------------------------------------- the observer


def match_observed(sent_data: np.ndarray, sent_dlc: np.ndarray, observed_data: np.ndarray,
                   observed_dlc: np.ndarray) -> None:
    """Check that the observer saw exactly the frames the node sent of one identifier, in the
    same order and with the same content; MatchError otherwise. The observer starts before the
    node and stops after it, so every frame the node sent is one it should have seen."""
    if len(sent_dlc) != len(observed_dlc):
        raise MatchError(f"the node sent {len(sent_dlc)} frames, the observer saw "
                         f"{len(observed_dlc)}")
    if not np.array_equal(np.asarray(sent_dlc), np.asarray(observed_dlc)):
        raise MatchError("a frame's length differs between the node and the observer")
    sent = np.asarray(sent_data)
    seen = np.asarray(observed_data)
    for i, dlc in enumerate(np.asarray(sent_dlc)):
        if not np.array_equal(sent[i, :dlc], seen[i, :dlc]):
            raise MatchError(f"frame {i} differs between the node and the observer")


def interarrival(times_ns: np.ndarray, cycle_ms: float,
                 trusted: np.ndarray | None = None) -> dict[str, np.ndarray | int]:
    """The intervals between consecutive arrivals of a cyclic frame (`times_ns`, in the order of
    arrival) and their deviation from its cycle time (ns), and how many intervals are 1.5 cycles
    or longer (a period without a frame on the bus: the frame missed or superseded). An interval
    with an end whose time is not `trusted` (fcn_tools.capture.to_monotonic()) is left out and
    counted ("untrusted")."""
    times = np.asarray(times_ns, dtype=np.int64)
    trusted = np.ones(times.shape, dtype=bool) if trusted is None else np.asarray(trusted, bool)
    intervals = np.diff(times)
    both = trusted[1:] & trusted[:-1]
    if np.any(intervals[both] < 0):
        raise MatchError("the arrivals are not in time order")
    intervals = intervals[both]
    cycle_ns = int(round(cycle_ms * 1e6))
    return {"interval_ns": intervals, "deviation_ns": intervals - cycle_ns,
            "long": int((intervals >= 1.5 * cycle_ns).sum()), "untrusted": int((~both).sum())}


# ------------------------------------------------------------------------- CPU and the stack


def cpu_shares(tasks: Iterable[Mapping], total_us: int) -> dict[str, float]:
    """Each task's share of the run-time counter (FreeRTOS's run-time statistics:
    run_time_us over run_time_total_us), as a fraction."""
    if total_us <= 0:
        raise ValueError("the run-time counter did not advance")
    return {task["name"]: task["run_time_us"] / total_us for task in tasks}


def pooled_share(ran_ns: Sequence[int], window_ns: Sequence[int]) -> float:
    """The share of several windows of different lengths that a task ran: the time it ran in
    all of them over their total length (a mean of the windows' own shares would weigh a short
    window as much as a long one)."""
    total = int(np.sum(np.asarray(window_ns, dtype=np.int64)))
    if len(ran_ns) != len(window_ns) or total <= 0:
        raise ValueError("one running time per window, and windows of some length")
    return float(np.sum(np.asarray(ran_ns, dtype=np.int64))) / total


def run_time_per_job(run_time_us: int, jobs: int) -> float | None:
    """The mean run time of a job: the task's run time over its jobs (None without a job)."""
    return run_time_us / jobs if jobs > 0 else None


@dataclass(frozen=True)
class StackUse:
    """One task's stack in the node's statistics (plat_stack_info_t): the depth of its host
    thread's stack at its deepest (peak), the part of it in use before the task's code ran
    (start: the C library's thread data and the port's start frames), the size it was created
    with, which is meant for the target (size), and what follows."""
    name: str
    peak_bytes: int
    start_bytes: int
    size_bytes: int

    @property
    def own_bytes(self) -> int:
        """What the task's own code used of the host thread's stack: peak - start."""
        return self.peak_bytes - self.start_bytes

    @property
    def margin_bytes(self) -> int:
        """The configured size less the task's own use: on the host, x86-64 frames against a
        size meant for a Cortex-M3."""
        return self.size_bytes - self.own_bytes


def stack_uses(tasks: Iterable[Mapping]) -> list[StackUse]:
    """The stack of every task the platform measured (a task that never registered has -1)."""
    return [StackUse(task["name"], task["stack_peak_bytes"], task["stack_start_bytes"],
                     task["stack_size_bytes"])
            for task in tasks if task["stack_peak_bytes"] >= 0]


def stack_maxima(runs: Iterable[Iterable[StackUse]]) -> dict[str, StackUse]:
    """Per task, the run with the deepest peak among `runs`."""
    deepest: dict[str, StackUse] = {}
    for run in runs:
        for use in run:
            if use.name not in deepest or use.peak_bytes > deepest[use.name].peak_bytes:
                deepest[use.name] = use
    return deepest
