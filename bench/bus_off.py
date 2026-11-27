#!/usr/bin/env python3
"""Measure the node's way into bus off and its recovery, at three bit rates.

Each trial takes the simulated controller bus off with a fault applied through simulation
control, removes the fault, and follows the node back onto the bus. Two fault kinds take a
controller bus off (lib/can/can_ctrl.h; a bus held dominant and a bus nobody acknowledges never
do):

  short      can.fault short (CAN_H shorted to CAN_L), held until the node is bus off, then
             removed with can.fault none while the node waits out its hold-off;
  tx_errors  can.inject tx 32: 32 transmissions destroyed by bit errors, a disturbance that ends
             by itself with the last of them, at the bus off.

The trials alternate between the two, 100 of each by default at 500, 125 and 1000 kbit/s, on an
idle bus (no other traffic than the node's own frames). Before each the node has been back on
the bus for the stable time of its policy (5 s: its hold-off is the initial 100 ms again) and
its bus-off DTC has healed. At each bit rate a short is also held through 7 bus offs in a row,
which shows the hold-off doubling up to its limit, before it is removed.

What each trial records, and on which clock (fcn_tools/busoff.py has the whole account):

  - the controller's trace (can.trace, platform/posix/posix_can_trace.h): for every step of the
    way the model's time and the time of the hardware thread's pass that carried it out, the
    error counters attempt by attempt, the restart request, the recovery, the first frame;
  - the node's log: when the transmit task took the bus-off event, when it asked for the
    recovery, when it took the recovery, when the health task set and healed the DTC;
  - can.status before and after: destroyed attempts, bus offs, late re-synchronisations;
  - an observer on the bus: every frame the node sent, the BMS_Heartbeat values it never sent,
    and BMS_Fault's report of the DTCs; the first frame after each recovery is found by its
    number among the node's frames, which the trace gives, not by comparing clocks;
  - at the end, the node's statistics: the counts are checked against what the trials did.

Every time is CLOCK_MONOTONIC. The recovery sequence is the model's: 128 occurrences of 11
recessive bits from the moment the count starts, 1408 bit times on an idle bus, whatever the
host does. When the count starts, how late the thread reports what the model decided, and
everything the driver does, is the host's; the hold-off is the node's policy. The result names
the part each figure belongs to.

    python bench/bus_off.py                    # the final measurement, about 54 minutes
    python bench/bus_off.py --quick --out DIR  # a test of the bench, about 30 s
    python bench/bus_off.py --replot           # redraw results/bus_off.png from the JSON
"""
from __future__ import annotations

import argparse
import json
import random
import sys
import threading
import time
from pathlib import Path

import numpy as np

from fcn_tools import busoff, plotstyle, results, runner
from fcn_tools import bus as bus_module
from fcn_tools.simctl import SimControl, SimError, TraceRecord

NAME = "bus_off"
VERSION = 1

DEFAULT_BITRATES = (500000, 125000, 1000000)
DEFAULT_TRIALS = 100
DEFAULT_PERSIST_BUS_OFFS = 7
QUICK_TRIALS = 2
QUICK_PERSIST_BUS_OFFS = 3
#: The seed of the random wait before each fault (BENCH_SEED of fcn_tools.bench, + 3).
DEFAULT_SEED = 20261124
#: Before each fault a random wait of up to this long, so that the faults fall at every phase of
#: the node's 10 ms schedule and not in step with the bench's own loop.
DEFAULT_MAX_DELAY_MS = 20.0

#: How long after the recovery the next trial waits at least: the policy's stable time, after
#: which the hold-off is the initial one again, and a margin for the transmit task, which starts
#: that time when it takes the recovery event (tens of microseconds after the model's moment).
STABLE_WAIT_S = busoff.STABLE_MS / 1000.0 + 0.2
#: The bench reads can.status this often until the node is bus off (and then removes a short),
#: and this often while it waits for the recovery.
POLL_FAST_S = 0.001
POLL_SLOW_S = 0.005
#: Bounds of the waits, not measurements: a node that misses one has failed the trial.
BUS_OFF_TIMEOUT_S = 2.0
RECOVERY_TIMEOUT_S = busoff.HOLDOFF_MAX_MS / 1000.0 + 3.0
HEAL_TIMEOUT_S = 12.0
START_TIMEOUT_S = 10.0
#: Trials repeated because the host held the bench up (a short removed only after the node had
#: asked to recover, a hold-off a host stall made look doubled): at most this share of the
#: trials, plus RETRY_EXTRA (so that a --quick run of 4 trials on a busy test machine may repeat
#: 5); beyond it the run fails. Every repeat is listed in the result ("repeated_trials").
RETRY_SHARE = 0.1
RETRY_EXTRA = 5

TIMING_NOTE = (
    "The recovery sequence (128 x 11 bit times on an idle bus) and the other figures marked "
    "\"model\" are the controller model's own times and do not depend on the host; the hold-off "
    "is the node's policy, timed by the FreeRTOS tick; every figure marked \"host\" is when the "
    "host ran the hardware thread, the node's tasks or the observer, and says nothing about a "
    "microcontroller.")


class TrialFailed(runner.BenchFailure):
    """A trial that went wrong in a way that is no fault of the host."""


# ----------------------------------------------------------------------------- one trial


def wait_status(sim: SimControl, reached, timeout: float, poll: float, what: str) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        status = sim.can_status()
        if reached(status):
            return status
        if time.monotonic() > deadline:
            raise TrialFailed(f"{what} within {timeout} s: {status}")
        time.sleep(poll)


def read_until_first_frame(sim: SimControl, start: int, timeout: float) -> list[TraceRecord]:
    """The trace from record `start` on, once it holds the first frame after a recovery."""
    deadline = time.monotonic() + timeout
    while True:
        read = sim.can_trace(start)
        if read.lost:
            raise TrialFailed(f"the trace ring lost {read.lost} records of the trial")
        if any(record.kind == "first_tx" for record in read.records):
            return read.records
        if time.monotonic() > deadline:
            raise TrialFailed("no frame after the recovery in the trace")
        time.sleep(POLL_SLOW_S)


def run_trial(sim: SimControl, fault: str, start: int, delay_s: float) -> dict:
    """One trial (see the module text); returns what it collected. `start`: the number of the
    trial's first trace record."""
    time.sleep(delay_s)
    before = sim.can_status()
    if before["state"] != "active" or before["tec"] != 0:
        raise TrialFailed(f"the node was not error active with TEC 0 before the fault: {before}")
    if fault == "short":
        sim.can_fault("short")
    else:
        sim.can_inject("tx", busoff.TX_ERRORS)
    wait_status(sim, lambda s: s["bus_off"] > before["bus_off"], BUS_OFF_TIMEOUT_S, POLL_FAST_S,
                "the node did not go bus off")
    removal_ns = int(sim.can_fault("none")["t_ns"]) if fault == "short" else None
    wait_status(sim, lambda s: s["state"] == "active", RECOVERY_TIMEOUT_S, POLL_SLOW_S,
                "the node did not recover")
    records = read_until_first_frame(sim, start, 1.0)
    after = sim.can_status()
    return {"fault": fault, "records": records, "removal_ns": removal_ns,
            "status_before": before, "status_after": after}


def run_persist(sim: SimControl, start: int, bus_offs: int) -> dict:
    """A short held through `bus_offs` bus offs, removed right after the last one, and the node
    followed back onto the bus. The trace is read on the way, so that its ring cannot overrun."""
    before = sim.can_status()
    sim.can_fault("short")
    records: list[TraceRecord] = []
    at = start
    deadline = time.monotonic() + sum(busoff.expected_holdoffs_ms(bus_offs)) / 1000.0 + 5.0
    while True:
        status = sim.can_status()
        read = sim.can_trace(at)
        if read.lost:
            raise TrialFailed(f"the trace ring lost {read.lost} records of the persisting short")
        records += read.records
        at = read.newest
        if status["bus_off"] - before["bus_off"] >= bus_offs:
            break
        if time.monotonic() > deadline:
            raise TrialFailed(f"{bus_offs} bus offs did not come in time: {status}")
        time.sleep(POLL_SLOW_S)
    removal_ns = int(sim.can_fault("none")["t_ns"])
    wait_status(sim, lambda s: s["state"] == "active", RECOVERY_TIMEOUT_S, POLL_SLOW_S,
                "the node did not recover after the persisting short")
    records += read_until_first_frame(sim, at, 1.0)
    return {"records": records, "removal_ns": removal_ns, "status_before": before,
            "status_after": sim.can_status()}


#: A frame as the observer keeps it: (monotonic time its thread received it, ns; the kernel's
#: receive time stamp, CLOCK_REALTIME, s; identifier; data).
Frame = tuple[int, float, int, bytes]


class FrameLog:
    """Every frame the observer's socket receives, read in a thread of its own from its start
    until stop(), kept compact as Frame tuples. A run at one bit rate is some 400,000 frames;
    can.Message objects for all of them would take several hundred megabytes, the tuples a
    fraction of that. Like bus.BusReader, only a stall of the whole bench process longer than the
    socket's buffer lasts can lose frames, and bus.receive_overflow() counts them.

    The monotonic time is taken when recv() returns: the kernel's arrival plus the wake-up of
    this thread. It is the observer's clock for everything that is compared with the node's
    times, because the kernel's own stamp is on CLOCK_REALTIME, which this VM steps
    (fcn_tools.busoff, the module text)."""

    def __init__(self, bus) -> None:
        self._bus = bus
        self.frames: list[Frame] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="FrameLog", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            message = self._bus.recv(timeout=0.05)
            if message is not None:
                self.frames.append((time.monotonic_ns(), message.timestamp,
                                    message.arbitration_id, bytes(message.data)))

    def stop(self, drain: bool) -> list[Frame]:
        """Stop reading; with `drain`, take what is still on its way (once the node ended)."""
        if not self._stop.is_set():
            self._stop.set()
            self._thread.join(timeout=5.0)
            if self._thread.is_alive():
                raise RuntimeError("the observer's thread did not stop")
            if drain:
                for message in bus_module.drain(self._bus):
                    self.frames.append((time.monotonic_ns(), message.timestamp,
                                        message.arbitration_id, bytes(message.data)))
        return self.frames

    def last_fault_frame(self) -> Frame | None:
        for frame in reversed(self.frames):
            if frame[2] == busoff.ID_FAULT:
                return frame
        return None


def heard_between(frames: list[Frame], mono_ns: np.ndarray, from_ns: int,
                  until_ns: int) -> tuple[int, list[busoff.Heard]]:
    """The frames the observer's thread received from `from_ns` to `until_ns` (monotonic), and
    how many it had received before them; `mono_ns` holds their monotonic times, in order."""
    low = int(np.searchsorted(mono_ns, from_ns, side="left"))
    high = int(np.searchsorted(mono_ns, until_ns, side="left"))
    return low, [busoff.Heard(mono_ns=frames[i][0], real_s=frames[i][1], id=frames[i][2],
                              data=frames[i][3]) for i in range(low, high)]


def wait_ready(log: FrameLog, active_ns: int) -> None:
    """Wait until the node is fit for the next trial: the stable time since its recovery has
    passed, and the last BMS_Fault heard, received after the recovery, says that neither bus-off
    DTC is active."""
    both = (1 << busoff.DTC_BUS_OFF_BIT) | (1 << busoff.DTC_PASSIVE_BIT)
    ready_ns = active_ns + round(STABLE_WAIT_S * 1e9)
    deadline_ns = active_ns + round(HEAL_TIMEOUT_S * 1e9)
    while True:
        now = time.monotonic_ns()
        last = log.last_fault_frame()
        healed = (last is not None and last[0] > active_ns
                  and not busoff.fault_bits(last[3])[0] & both)
        if now >= ready_ns and healed:
            return
        if now > deadline_ns:
            raise TrialFailed(f"the bus-off DTCs had not healed {HEAL_TIMEOUT_S} s after the "
                              "recovery")
        time.sleep(0.05)


def last_recovery(records: list[TraceRecord]) -> TraceRecord:
    return [r for r in records if r.kind == "state" and r.old_state == 2][-1]


# ------------------------------------------------------------------------- one bit rate


def run_bitrate(binaries: runner.Binaries, transport: str, bitrate: int, faults: list[str],
                trials: int, persist: int, rng: random.Random, max_delay_ms: float,
                say) -> dict:
    """Every trial at one bit rate, on one node of its own; returns the analysis."""
    schedule = [faults[i % len(faults)] for i in range(trials * len(faults))]
    retry_limit = int(RETRY_SHARE * len(schedule)) + RETRY_EXTRA
    collected: list[dict] = []
    retried: list[dict] = []
    persist_raw = None
    with runner.Session(runner.bench_workdir(NAME), transport, binaries=binaries) as session:
        observer_bus = session.open_bus()
        frame_log = FrameLog(observer_bus)
        ended = False
        try:
            node = session.start_node("--bitrate", str(bitrate))
            with node.sim() as sim:
                sim.ping()
                deadline = time.monotonic() + START_TIMEOUT_S
                while sum(1 for f in frame_log.frames if f[2] == busoff.ID_FAULT) < 3:
                    if time.monotonic() > deadline:
                        raise runner.BenchFailure("the node sent no BMS_Fault")
                    time.sleep(0.05)
                start = sim.can_trace().newest
                if start != 0:
                    raise runner.BenchFailure("the trace has records before any fault")
                position = 0
                while position < len(schedule):
                    fault = schedule[position]
                    raw = run_trial(sim, fault, start, rng.uniform(0.0, max_delay_ms) / 1000.0)
                    start = raw["records"][-1].seq + 1
                    wait_ready(frame_log, last_recovery(raw["records"]).model_ns)
                    verdict = busoff.trial_figures(trial_input(raw, bitrate,
                                                               busoff.parse_log(node.log())))
                    raw["retry"] = verdict["retry"]
                    raw["problems"] = verdict["problems"]
                    if verdict["problems"] and verdict["retry"]:
                        retried.append(raw)
                        say(f"  {bitrate} bit/s, trial {position + 1} ({fault}) repeated: "
                            f"{'; '.join(verdict['problems'])}")
                        if len(retried) > retry_limit:
                            raise runner.BenchFailure(
                                f"{len(retried)} trials had to be repeated, more than "
                                f"{retry_limit}: the host held the bench up too often")
                        continue
                    if verdict["problems"]:
                        raise TrialFailed(f"trial {position + 1} ({fault}) at {bitrate} bit/s: "
                                          f"{'; '.join(verdict['problems'])}")
                    collected.append(raw)
                    position += 1
                    if position % 20 == 0 or position == len(schedule):
                        say(f"  {bitrate} bit/s: {position} of {len(schedule)} trials")
                if persist:
                    persist_raw = run_persist(sim, start, persist)
                    wait_ready(frame_log, last_recovery(persist_raw["records"]).model_ns)
            stats = session.finish(node)
            ended = True
        finally:
            frames = frame_log.stop(drain=ended)
            overflow = bus_module.receive_overflow(observer_bus)
            observer_bus.shutdown()
        log_text = node.log()
    return analyse_bitrate(bitrate, transport, collected, retried, persist_raw, persist, stats,
                           frames, overflow, log_text)


def trial_input(raw: dict, bitrate: int, log: list[busoff.LogEvent]) -> busoff.TrialInput:
    return busoff.TrialInput(fault=raw["fault"], bitrate=bitrate, records=raw["records"],
                             log=log, status_before=raw["status_before"],
                             status_after=raw["status_after"], removal_ns=raw["removal_ns"])


#: The figures of a trial kept in the result, per trial: the intervals (ns), the bit-time
#: figures, whether no host time entered error passive to bus off, the counts, and what the
#: observer heard.
TRIAL_COLUMNS = (*busoff.INTERVALS, "recovery_sequence_bits", "passive_to_bus_off_bits",
                 "passive_to_bus_off_exact", "first_frame_on_bus_bits", "attempts_to_bus_off",
                 "tec_at_passive", "tec_at_bus_off", "late_resyncs", "counter_samples",
                 "first_frame_id", "t_fault_ns")
OBSERVER_COLUMNS = ("silence_ns", "observer_lag_ns", "removal_to_observer_ns", "heartbeats_lost",
                    "first_heartbeat_fresh", "dtc_bus_off_after", "dtc_passive_after",
                    "dtc_bus_off_healed_ns", "dtc_passive_healed_ns")


def analyse_bitrate(bitrate: int, transport: str, collected: list[dict], retried: list[dict],
                    persist_raw: dict | None, persist: int, stats: dict, frames: list[Frame],
                    overflow: int, log_text: str) -> dict:
    """The run's counts against what the bench did, the persisting short, every trial judged
    again with the whole log, and what the observer heard around each. The counts come first:
    the observer finds each trial's first frame after the recovery by its number among the
    node's frames, which only holds when it heard every frame the node sent."""
    log = busoff.parse_log(log_text)

    persist_out = None
    persist_bus_offs = 0
    if persist_raw is not None:
        persist_out = busoff.persist_figures(persist_raw["records"], log, bitrate, persist)
        if persist_out["problems"]:
            raise TrialFailed(f"the persisting short at {bitrate} bit/s: "
                              f"{'; '.join(persist_out['problems'])}")
        del persist_out["problems"]
        persist_bus_offs = persist

    # What the bench did: one bus off per trial it kept (its trace showed exactly one), the bus
    # offs of the trials it repeated (can.status counted them: a late removal leaves two), and
    # those of the persisting short. Each trial and the persisting short is one stretch of the
    # DTCs, set and healed once.
    repeated_bus_offs = sum(int(r["status_after"]["bus_off"]) - int(r["status_before"]["bus_off"])
                            for r in retried)
    bus_offs = len(collected) + repeated_bus_offs + persist_bus_offs
    episodes = len(collected) + len(retried) + (1 if persist_raw is not None else 0)
    heard_by_id: dict[int, int] = {}
    for frame in frames:
        heard_by_id[frame[2]] = heard_by_id.get(frame[2], 0) + 1
    counters = [busoff.heartbeat_counter(f[3]) for f in frames if f[2] == busoff.ID_HEARTBEAT]
    missing = busoff.heartbeat_values_missing(counters) if counters else None
    checks = busoff.session_checks(stats, bus_offs, episodes, log, heard_by_id, missing, overflow)
    failed = [c for c in checks if not c.ok]
    if failed:
        raise TrialFailed(f"the counts of the run at {bitrate} bit/s do not fit what it did: "
                          + "; ".join(f"{c.name}: expected {c.expected}, found {c.found}"
                                      for c in failed))

    mono_ns = np.asarray([f[0] for f in frames], dtype=np.int64)
    starts = sorted([raw["records"][0].pass_ns for raw in collected + retried]
                    + ([persist_raw["records"][0].pass_ns] if persist_raw else []))
    trials: dict[str, dict[str, list]] = {}
    pairs: dict[str, dict[tuple[int, int], int]] = {}
    observer_problems: list[str] = []
    for index, raw in enumerate(collected):
        verdict = busoff.trial_figures(trial_input(raw, bitrate, log))
        if verdict["problems"]:
            raise TrialFailed(f"trial {index + 1} at {bitrate} bit/s, judged again with the whole "
                              f"log: {'; '.join(verdict['problems'])}")
        figures = verdict["figures"]
        begin = figures["t_fault_ns"]
        until = next((s for s in starts if s > begin), 2**62)
        offset, heard = heard_between(frames, mono_ns, begin - 500_000_000, until)
        seen = busoff.observer_figures(heard, figures, until, offset)
        observer_problems += [f"trial {index + 1} ({raw['fault']}): {p}"
                              for p in seen["problems"]]
        group = trials.setdefault(raw["fault"], {})
        for key in TRIAL_COLUMNS:
            group.setdefault(key, []).append(figures[key])
        for key in OBSERVER_COLUMNS:
            group.setdefault(key, []).append(seen.get(key))
        counts = pairs.setdefault(raw["fault"], {})
        for attempts, tec in verdict["trajectory"]:
            counts[(attempts, tec)] = counts.get((attempts, tec), 0) + 1
    if observer_problems:
        raise TrialFailed("what the observer heard does not fit:\n  "
                          + "\n  ".join(observer_problems[:20]))
    return {
        "bitrate": bitrate,
        "transport": transport,
        "build_id": stats["build_id"],
        "trials": trials,
        "tec_samples": {fault: [[a, t, n] for (a, t), n in sorted(counts.items())]
                        for fault, counts in pairs.items()},
        "repeated_trials": [{"fault": r["fault"], "t_fault_ns": r["records"][0].pass_ns,
                             "bus_offs": int(r["status_after"]["bus_off"])
                             - int(r["status_before"]["bus_off"]),
                             "problems": r["problems"]} for r in retried],
        "persisting_short": persist_out,
        "bus_offs_caused": bus_offs,
        "frames_heard": len(frames),
        "checks": [c.as_dict() for c in checks],
        "node": {"queue": stats["can"]["queue"], "controller": stats["can"]["controller"],
                 "pacing": stats["can"]["pacing"],
                 "superseded_by_message": {name: m["queue"]["superseded"]
                                           for name, m in stats["telemetry"]["messages"].items()},
                 "dtcs": {e["name"]: {"status": e["status"], "counter": e["counter"],
                                      "failed_cycles": e["failed_cycles"]}
                          for e in stats["faults"]["dtcs"]
                          if e["name"] in (busoff.DTC_BUS_OFF, busoff.DTC_PASSIVE)}},
    }


# ---------------------------------------------------------------------------- the summary

#: The figures of each fault kind in the summary: (key in the trial columns, summary key, unit,
#: scale, part). The part says who fixes the figure (busoff.INTERVALS): "model" (the controller
#: model by ISO 11898-1's rules alone: the same on any host and whatever frames the node
#: sends), "policy" (the node's recovery policy, run on the FreeRTOS tick), "host" (when the
#: host ran a thread), or "mixed" (a sum of them, the node's schedule among them;
#: busoff.INTERVALS says of which). Each summary entry carries it. Error passive to bus off is
#: not among them: whether a host time entered it differs from trial to trial
#: (passive_to_bus_off()).
SUMMARY_FIGURES = (
    ("fault_to_bus_off", "fault_to_bus_off_ms", "ms", 1e-6, "mixed"),
    ("attempts_to_bus_off", "destroyed_attempts_to_bus_off", "attempts", 1.0, "model"),
    ("bus_off_to_active", "bus_off_to_error_active_ms", "ms", 1e-6, "mixed"),
    ("removal_to_active", "removal_to_error_active_ms", "ms", 1e-6, "mixed"),
    ("removal_to_first_frame", "removal_to_first_frame_ms", "ms", 1e-6, "mixed"),
    ("removal_to_observer_ns", "removal_to_frame_heard_ms", "ms", 1e-6, "mixed"),
    ("holdoff", "holdoff_ms", "ms", 1e-6, "policy"),
    ("recovery_sequence_bits", "recovery_sequence_bits", "bit times", 1.0, "model"),
    ("bus_off_to_driver", "host_bus_off_to_driver_ms", "ms", 1e-6, "host"),
    ("request_to_count", "host_request_to_count_ms", "ms", 1e-6, "host"),
    ("recovery_report_lag", "host_recovery_report_lag_ms", "ms", 1e-6, "host"),
    ("active_to_first_submit", "host_active_to_first_submit_ms", "ms", 1e-6, "host"),
    ("first_frame_on_bus_bits", "first_frame_bits", "bit times", 1.0, "mixed"),
    ("heartbeats_lost", "heartbeat_values_lost", "frames", 1.0, "mixed"),
    ("silence_ns", "bus_silence_heard_ms", "ms", 1e-6, "mixed"),
    ("dtc_heal", "bus_off_dtc_heal_ms", "ms", 1e-6, "mixed"),
)


def with_part(entry: dict, part: str) -> dict:
    """A summary figure or distribution with the part of the system that fixes it."""
    return {**entry, "part": part}


def passive_to_bus_off(fault: str, columns: dict) -> dict:
    """The summary entries of error passive to bus off, in bit times. A trial that no host time
    entered (busoff.model_exact(), "passive_to_bus_off_exact") is the model's under a short and
    "mixed" under tx_errors, where the frames attempted are the node's schedule
    (busoff.PASSIVE_TO_BUS_OFF_PART); any other trial is "mixed". "passive_to_bus_off_bits" is
    the distribution of the exact trials where they are the model's (a short), else of every
    trial, with the exact ones apart as "passive_to_bus_off_bits_exact" (tx_errors);
    "passive_to_bus_off_bits_not_exact" gives the count and the values of the others."""
    bits = columns["passive_to_bus_off_bits"]
    exact = columns["passive_to_bus_off_exact"]
    exact_bits = [value for value, ok in zip(bits, exact) if ok]
    other_bits = [value for value, ok in zip(bits, exact) if not ok]
    part = busoff.PASSIVE_TO_BUS_OFF_PART[fault]
    entries: dict = {}
    if part == "model":
        entries["passive_to_bus_off_bits"] = with_part(
            results.distribution(exact_bits, "bit times"), part)
    else:
        entries["passive_to_bus_off_bits"] = with_part(
            results.distribution(bits, "bit times"), "mixed")
        entries["passive_to_bus_off_bits_exact"] = with_part(
            results.distribution(exact_bits, "bit times"), part)
    entries["passive_to_bus_off_bits_not_exact"] = with_part(
        results.raw_samples(other_bits, "bit times"), "mixed")
    return entries


def summarize(runs: list[dict]) -> dict:
    """The figures the README may quote, each with its unit (results.check_summary()) and, where
    it is a time or a count of the trials, the part that fixes it (SUMMARY_FIGURES,
    passive_to_bus_off())."""
    summary: dict = {"standard_minimum_recovery_bits": with_part(
        results.figure(busoff.RECOVERY_BITS, "bit times"), "standard")}
    for run in runs:
        rate = run["bitrate"]
        group: dict = {
            "standard_minimum_recovery_ms": with_part(results.figure(
                round(busoff.bits_to_ns(busoff.RECOVERY_BITS, rate) / 1e6, 6), "ms"), "standard"),
        }
        every = [value for columns in run["trials"].values()
                 for value in columns["recovery_sequence"]]
        group["recovery_sequence_ms"] = with_part(
            results.distribution(every, "ms", scale=1e-6, decimals=6), "model")
        group["recovery_sequence_bits"] = with_part(
            results.distribution([busoff.ns_to_bits(v, rate) for v in every], "bit times"),
            "model")
        for fault, columns in run["trials"].items():
            entry = {"trials": results.figure(len(columns["recovery_sequence"]), "trials")}
            for key, name, unit, scale, part in SUMMARY_FIGURES:
                values = [v for v in columns.get(key, []) if v is not None]
                entry[name] = with_part(results.distribution(values, unit, scale=scale), part)
            entry.update(passive_to_bus_off(fault, columns))
            fresh, known = busoff.share(columns["first_heartbeat_fresh"])
            entry["first_heartbeat_fresh"] = results.figure(fresh, f"of {known} trials")
            reported, known = busoff.share(columns["dtc_bus_off_after"])
            entry["dtc_reported_after_recovery"] = results.figure(reported, f"of {known} trials")
            group[fault] = entry
        group["repeated_trials"] = results.figure(len(run["repeated_trials"]), "trials")
        persisting = run.get("persisting_short")
        if persisting:
            group["persisting_short_holdoffs_ms"] = {
                str(i + 1): with_part(results.figure(h, "ms"), "policy")
                for i, h in enumerate(persisting["holdoffs_ms"])}
        summary[f"{rate // 1000}_kbit_s"] = group
    return summary


def config_of(args: argparse.Namespace, bitrates: list[int], faults: list[str], trials: int,
              persist: int) -> dict:
    return {
        "bitrates": bitrates, "faults": faults, "trials_per_fault": trials,
        "transport": args.transport, "persisting_short_bus_offs": persist,
        "seed": args.seed, "max_delay_ms": args.max_delay_ms, "quick": args.quick,
        "bus": "idle: no frames but the node's own",
        "fault_actions": {"short": "can.fault short, then can.fault none once can.status shows "
                                   "the bus off",
                          "tx_errors": f"can.inject tx {busoff.TX_ERRORS}"},
        "stable_wait_s": STABLE_WAIT_S, "poll_until_bus_off_s": POLL_FAST_S,
        "poll_until_recovered_s": POLL_SLOW_S, "retry_share": RETRY_SHARE,
        "retry_extra": RETRY_EXTRA,
        "clock_step_limit_ns": busoff.CLOCK_STEP_LIMIT_NS,
        "standard": {"recovery_occurrences": busoff.RECOVERY_OCCURRENCES,
                     "recessive_run_bits": busoff.RECESSIVE_RUN_BITS,
                     "recovery_bits": busoff.RECOVERY_BITS,
                     "passive_tec": busoff.PASSIVE_LIMIT, "bus_off_tec": busoff.BUS_OFF_LIMIT},
        "policy": {"holdoff_initial_ms": busoff.HOLDOFF_INITIAL_MS,
                   "holdoff_max_ms": busoff.HOLDOFF_MAX_MS, "stable_ms": busoff.STABLE_MS,
                   "source": "app/can_io.c (TX_HOLDOFF_INITIAL_MS, TX_HOLDOFF_MAX_MS, "
                             "TX_STABLE_MS), app/fault.c (the DTCs)"},
        "parts": {name: {"from": start, "to": end, "part": part, "what": what}
                  for name, (start, end, part, what) in busoff.INTERVALS.items()},
    }


# ------------------------------------------------------------------------------- the plot


def plot(data: dict, path: Path) -> None:
    import matplotlib.pyplot as plt

    plotstyle.apply()
    runs = data["runs"]
    fig, axes = plt.subplots(2, 3, figsize=(15.0, 8.6))
    fig.subplots_adjust(left=0.05, right=0.985, top=0.86, bottom=0.08, wspace=0.55,
                        hspace=0.75)
    colours = {run["bitrate"]: plotstyle.SERIES[i] for i, run in enumerate(runs)}
    label = {run["bitrate"]: f"{run['bitrate'] // 1000} kbit/s" for run in runs}
    styles = {"short": "-", "tx_errors": (0, (4, 2))}

    # (a) The transmit error counter on the way to bus off, attempt by attempt.
    ax = axes[0][0]
    first = runs[0]
    for i, fault in enumerate(data["config"]["faults"]):
        n = np.arange(0, busoff.ATTEMPTS_TO_BUS_OFF[fault] + 1)
        ax.step(n, [busoff.tec_after_attempts(fault, int(k)) for k in n], where="post",
                color=plotstyle.SERIES[i], linewidth=1.2, alpha=0.6,
                label=f"{fault}: the rules ({busoff.ATTEMPTS_TO_BUS_OFF[fault]} attempts)")
        samples = np.asarray(first["tec_samples"].get(fault, [[0, 0, 0]]), dtype=float)
        ax.scatter(samples[:, 0], samples[:, 1], s=8 + 3 * np.sqrt(samples[:, 2]),
                   color=plotstyle.SERIES[i], zorder=3,
                   label=f"{fault}: trace, {int(samples[:, 2].sum())} samples")
    for level, text in ((busoff.PASSIVE_LIMIT, "error passive"),
                        (busoff.BUS_OFF_LIMIT, "bus off")):
        ax.axhline(level, color=plotstyle.INK_MUTED, linewidth=0.8, linestyle=(0, (4, 3)))
        ax.annotate(text, xy=(0.5, level), xytext=(2, 3), textcoords="offset points",
                    fontsize=7.5, color=plotstyle.INK_MUTED)
    ax.set_title(f"TEC on the way to bus off ({label[first['bitrate']]})")
    ax.set_xlabel("destroyed transmission attempts")
    ax.set_ylabel("transmit error counter")
    plotstyle.legend_above(ax)

    # (b) The recovery sequence against the standard's minimum.
    ax = axes[0][1]
    for i, run in enumerate(runs):
        dist = data["summary"][f"{run['bitrate'] // 1000}_kbit_s"]["recovery_sequence_ms"]
        minimum = busoff.bits_to_ns(busoff.RECOVERY_BITS, run["bitrate"]) / 1e6
        ax.bar(i, dist["max"], color=colours[run["bitrate"]], width=0.55,
               label=f"{label[run['bitrate']]}: measured {dist['min']:.3f} to {dist['max']:.3f} ms"
                     f" (n = {dist['n']})")
        ax.plot([i - 0.38, i + 0.38], [minimum, minimum], color=plotstyle.INK, linewidth=1.2)
    ax.plot([], [], color=plotstyle.INK, linewidth=1.2,
            label=f"ISO 11898-1 minimum: {busoff.RECOVERY_OCCURRENCES} x "
                  f"{busoff.RECESSIVE_RUN_BITS} = {busoff.RECOVERY_BITS} bit times")
    ax.set_xticks(range(len(runs)), [label[r["bitrate"]] for r in runs])
    ax.set_title("Recovery sequence: request to error active (model)")
    ax.set_ylabel("ms")
    plotstyle.legend_above(ax)

    # (c) Where the time from bus off to the first frame goes (medians, log scale).
    ax = axes[0][2]
    parts = [("bus_off_to_driver", "bus off to driver (host)"),
             ("holdoff", "hold-off (policy)"),
             ("request_to_count", "request to count (host)"),
             ("recovery_sequence", "recovery sequence (model)"),
             ("recovery_report_lag", "report of it (host)"),
             ("active_to_first_submit", "driver reloads (host)"),
             ("first_frame_on_bus", "first frame (mixed)")]
    height = 0.8 / len(runs)
    for i, run in enumerate(runs):
        columns = run["trials"].get("short") or next(iter(run["trials"].values()))
        medians = [float(np.median([v for v in columns[key] if v is not None])) / 1e6
                   for key, _ in parts]
        ax.barh(np.arange(len(parts)) + i * height, medians, height=height,
                color=colours[run["bitrate"]], label=label[run["bitrate"]])
    ax.set_yticks(np.arange(len(parts)) + 0.4 - height / 2, [text for _, text in parts])
    ax.invert_yaxis()
    ax.set_xscale("log")
    ax.grid(axis="x")
    ax.grid(axis="y", visible=False)
    ax.set_title("Bus off to first frame (short, medians)")
    ax.set_xlabel("ms (log scale)")
    plotstyle.legend_above(ax)

    # (d) Fault removed to error active, every trial.
    ax = axes[1][0]
    for run in runs:
        for fault, columns in run["trials"].items():
            ms = np.sort([v for v in columns["removal_to_active"] if v is not None]) / 1e6
            if ms.size:
                ax.plot(ms, np.arange(1, ms.size + 1) / ms.size * 100.0,
                        color=colours[run["bitrate"]], linestyle=styles.get(fault, "-"),
                        label=f"{label[run['bitrate']]}, {fault} (n = {ms.size})")
    ax.set_title("Fault removed to error active (every trial)")
    ax.set_xlabel("ms")
    ax.set_ylabel("share of trials within (%)")
    ax.set_ylim(0, 100)
    plotstyle.legend_above(ax)

    # (e) The hold-off under a persisting short.
    ax = axes[1][1]
    persisting = [run for run in runs if run.get("persisting_short")]
    width = 0.8 / max(len(persisting), 1)
    for i, run in enumerate(persisting):
        holdoffs = run["persisting_short"]["holdoffs_ms"]
        ax.bar(np.arange(len(holdoffs)) + i * width, holdoffs, width=width,
               color=colours[run["bitrate"]], label=f"{label[run['bitrate']]}: measured")
    if persisting:
        expected = persisting[0]["persisting_short"]["expected_ms"]
        for k, value in enumerate(expected):
            ax.plot([k - width / 2 - 0.05, k + 0.8 - width / 2 + 0.05], [value, value],
                    color=plotstyle.INK, linewidth=1.2,
                    label="policy (app/can_io.c)" if k == 0 else None)
        ax.set_xticks(np.arange(len(expected)) + 0.4 - width / 2,
                      [str(k + 1) for k in range(len(expected))])
    ax.set_title("Hold-off before each request, short held on")
    ax.set_xlabel("bus off in a row")
    ax.set_ylabel("ms (driver: bus-off event to request)")
    plotstyle.legend_above(ax)

    # (f) Heartbeat values lost per trial: trials per count, a bar per bit rate side by side.
    ax = axes[1][2]
    lost_by_run = [[int(v) for columns in run["trials"].values()
                    for v in columns["heartbeats_lost"] if v is not None] for run in runs]
    every_lost = [v for lost in lost_by_run for v in lost]
    if every_lost:
        values = np.arange(min(every_lost), max(every_lost) + 1)
        width = 0.8 / len(runs)
        for i, (run, lost) in enumerate(zip(runs, lost_by_run)):
            if lost:
                ax.bar(values - 0.4 + (i + 0.5) * width, [lost.count(int(v)) for v in values],
                       width=width, color=colours[run["bitrate"]],
                       label=f"{label[run['bitrate']]}: median {np.median(lost):.0f} "
                             f"(n = {len(lost)})")
        ax.set_xticks(values)
    ax.set_title("BMS_Heartbeat values never sent, per trial")
    ax.set_xlabel("values lost (one per 10 ms period off the bus)")
    ax.set_ylabel("trials")
    plotstyle.legend_above(ax)

    host = "WSL2" if data["provenance"]["host"]["is_wsl"] else "Linux"
    plotstyle.footnote(fig, f"bench/bus_off.py, {data['config']['trials_per_fault']} trials per "
                            f"fault and bit rate, {data['config']['transport']}, idle bus; "
                            f"FreeRTOS POSIX port on a {host} host. Model figures are exact; the "
                            "hold-off is the node's policy, run on the tick; host figures are "
                            "host timing, not MCU timing.")
    plotstyle.save(fig, path)


# ------------------------------------------------------------------------------- the run


def parse_list(text: str, kind) -> list:
    return [kind(word) for word in text.split(",") if word]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    results.add_bench_arguments(
        parser, quick=f"500 kbit/s only, {QUICK_TRIALS} trials of each fault and a short held "
                      f"through {QUICK_PERSIST_BUS_OFFS} bus offs, about 30 s")
    parser.add_argument("--bitrates", default=",".join(str(r) for r in DEFAULT_BITRATES),
                        help="bit rates in bit/s, comma separated (default %(default)s)")
    parser.add_argument("--faults", default=",".join(busoff.FAULTS),
                        help="fault kinds, comma separated (default %(default)s)")
    parser.add_argument("--trials", type=int, default=DEFAULT_TRIALS,
                        help="trials of each fault at each bit rate (default %(default)s)")
    parser.add_argument("--persist", type=int, default=DEFAULT_PERSIST_BUS_OFFS, metavar="N",
                        help="bus offs under a short held on at each bit rate, 0 for none "
                             "(default %(default)s)")
    parser.add_argument("--transport", choices=runner.TRANSPORTS, default="udp",
                        help="the bus: udp (default) or socketcan (vcan0)")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED,
                        help="seed of the random wait before each fault (default %(default)s)")
    parser.add_argument("--max-delay-ms", type=float, default=DEFAULT_MAX_DELAY_MS,
                        help="longest random wait before a fault (default %(default)s)")
    args = parser.parse_args(argv)
    out = results.output_dir(args)
    json_path = out / f"{NAME}.json"
    if args.replot:
        if not json_path.exists():
            print(f"nothing to replot: {json_path} does not exist", file=sys.stderr)
            return 1
        plot(json.loads(json_path.read_text()), json_path.with_suffix(".png"))
        print(f"redrew {json_path.with_suffix('.png')}")
        return 0

    bitrates = [DEFAULT_BITRATES[0]] if args.quick else parse_list(args.bitrates, int)
    faults = parse_list(args.faults, str)
    trials = QUICK_TRIALS if args.quick else args.trials
    persist = min(args.persist, QUICK_PERSIST_BUS_OFFS) if args.quick else args.persist
    if (not bitrates or any(not 0 < r <= 1_000_000 for r in bitrates) or not faults
            or any(f not in busoff.FAULTS for f in faults) or trials < 1 or persist < 0
            or args.max_delay_ms < 0):
        parser.error("bit rates 1 to 1000000, faults of "
                     f"{', '.join(busoff.FAULTS)}, at least one trial, --persist >= 0")

    def say(text: str) -> None:
        print(text, flush=True)

    try:
        load = results.check_load(args.max_load, args.allow_loaded)
        binaries = runner.prepare_binaries(build=not args.no_build)
        results.require_tree_build(binaries.as_dict())
        estimate = len(bitrates) * (trials * len(faults) * (STABLE_WAIT_S + 0.25) + 3.0
                                    + (sum(busoff.expected_holdoffs_ms(persist)) / 1000.0
                                       + STABLE_WAIT_S + 2.0 if persist else 0.0))
        say(f"bus off: {len(bitrates)} bit rates x {len(faults)} faults x {trials} trials, "
            f"{args.transport}; about {estimate / 60:.0f} min")
        rng = random.Random(args.seed)
        runs = []
        for bitrate in bitrates:
            say(f"{bitrate} bit/s ...")
            runs.append(run_bitrate(binaries, args.transport, bitrate, faults, trials, persist,
                                    rng, args.max_delay_ms, say))
        reported = {f"node statistics at {run['bitrate']} bit/s": run["build_id"]
                    for run in runs}
        data = {"runs": runs}
        path = results.write_result(
            NAME, data, summarize(runs), config_of(args, bitrates, faults, trials, persist), out,
            version=VERSION, timing_note=TIMING_NOTE, load=load, binaries=binaries.as_dict(),
            reported_build_ids=reported, binary_sha256=binaries.sha256)
    except results.HostLoaded as error:
        print(error, file=sys.stderr)
        return results.EXIT_HOST_LOADED
    except results.BuildIdMismatch as error:
        print(f"no result written: {error}", file=sys.stderr)
        return results.EXIT_BUILD_MISMATCH
    except KeyboardInterrupt as error:
        print("interrupted: no result written", file=sys.stderr)
        return runner.interrupted_status(error)
    except (runner.BenchFailure, SimError) as error:
        print(f"no result written: {error}", file=sys.stderr)
        return 1
    result = json.loads(path.read_text())
    plot(result, path.with_suffix(".png"))
    for run in runs:
        group = result["summary"][f"{run['bitrate'] // 1000}_kbit_s"]
        seq = group["recovery_sequence_ms"]
        say(f"{run['bitrate']} bit/s: recovery sequence {seq['min']}..{seq['max']} ms "
            f"(standard {group['standard_minimum_recovery_ms']['value']} ms), n {seq['n']}")
        for fault in faults:
            entry = group[fault]

            def brief(name: str) -> str:
                dist = entry[name]
                middle = f"p50 {dist['p50']}" if dist["p50"] is not None else f"mean {dist['mean']}"
                return f"{middle}, max {dist['max']} {dist['unit']}"

            say(f"  {fault} ({entry['trials']['value']} trials): bus off -> active "
                f"{brief('bus_off_to_error_active_ms')}; removal -> first frame "
                f"{brief('removal_to_first_frame_ms')}; hold-off {brief('holdoff_ms')}")
        if "persisting_short_holdoffs_ms" in group:
            say("  persisting short, hold-offs (ms): " + ", ".join(
                str(f["value"]) for f in group["persisting_short_holdoffs_ms"].values()))
    say(f"wrote {path} and {path.with_suffix('.png')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
