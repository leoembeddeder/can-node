# SPDX-License-Identifier: MIT
"""The analysis of the bus-off bench (bench/bus_off.py): what the records of one trial say,
checked against ISO 11898-1's fault confinement and the node's recovery policy, and the
figures of a run. Pure functions of the records, so that tests/python/test_busoff.py can feed
them constructed records with known answers; the bench does the running.

THE SOURCES OF A TRIAL, and the clock of each. Every time is CLOCK_MONOTONIC in nanoseconds
(time.monotonic_ns() in Python), except where said.

  - The controller's trace (can.trace, platform/posix/posix_can_trace.h; SimControl.can_trace):
    for each step the model's time (model_ns) and the start of the hardware thread's pass that
    carried it out (pass_ns). The model's times follow exactly from the model's rules and the
    times they start from, which are sometimes the host's (model_exact()); the passes are the
    host's.
  - The node's log (the driver's side, app/can_io.c, written by the transmit task): "controller
    state 1 -> 2 (TEC 256, REC 0)" when it took the state event, "bus off: requesting recovery"
    just before it asked, and the health task's "fault CAN_BUS_OFF active (DTC 0xD00100)" and
    "... no longer active". Stamped by lib/log with plat_time_ns() when the line is made, to the
    microsecond.
  - The observer, a tester's socket on the bus, read by a thread of the bench: every frame the
    node sent, with the monotonic time at which the thread received it (Heard.mono_ns: the
    kernel's arrival and the thread's wake-up) and the kernel's receive time stamp
    (Heard.real_s, CLOCK_REALTIME, seconds). The kernel's stamp is the more precise one, but
    CLOCK_REALTIME is not monotonic: this WSL2 VM steps it (four steps of +1.14 to +1.20 s, one
    every 32 s, in a probe that compared time.time_ns() with time.monotonic_ns() every 10 ms
    for 120 s on 2026-11-22), so it is never mixed with the monotonic clock, and an interval
    between two kernel stamps is used only where the monotonic clock agrees with it within
    CLOCK_STEP_LIMIT_NS.
  - can.status before and after the trial: the model's counters (destroyed attempts, bus offs,
    late re-synchronisations).

WHAT THE MODEL AND THE STANDARD FIX, and what the host adds. A recovery takes 128 occurrences of
11 recessive bits (rule 12) from the moment the model counts them, on an idle bus exactly
RECOVERY_BITS bit times: the model reports it at that time, whenever the thread runs. When the
count starts is the host's (the pass that served the driver's request), and so is everything the
driver does (when it saw the bus off, when its hold-off timer ended, when it loaded the next
frame). The node's policy adds a hold-off of HOLDOFF_INITIAL_MS before it asks (app/can_io.c).
On the way into bus off the destroyed attempts follow each other at the model's times, unless a
host time starts one (model_exact()); which frames are attempted is the node's schedule.
trial_figures() names each interval with the part it belongs to (INTERVALS).
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Iterable, Mapping, Sequence

import numpy as np

from .simctl import TraceRecord

# ------------------------------------------------------------------ ISO 11898-1 (lib/can/can_fc.h)

#: Rule 12: a bus-off node may become error active after 128 occurrences of 11 consecutive
#: recessive bits; on an idle bus that is 1408 bit times, the standard's minimum.
RECOVERY_OCCURRENCES = 128
RECESSIVE_RUN_BITS = 11
RECOVERY_BITS = RECOVERY_OCCURRENCES * RECESSIVE_RUN_BITS
#: Rules 9 and 10: error passive at a transmit error counter of 128, bus off at 256.
PASSIVE_LIMIT = 128
BUS_OFF_LIMIT = 256

# ------------------------------------------------------------- the model (lib/can/can_ctrl.h)

#: A destroyed attempt under a short: a bit error on the start-of-frame bit, then the error frame
#: (6 flag and 8 delimiter bits); then the intermission, and an error-passive transmitter's
#: suspend.
SHORT_ATTEMPT_BITS = 1 + 14
INTERMISSION_BITS = 3
SUSPEND_BITS = 8
#: From error passive to bus off under a short: 16 attempts, 26 bit times apart, whatever the
#: frames. The model's figure in a trial that no host time entered (model_exact()).
SHORT_PASSIVE_TO_BUS_OFF_BITS = 16 * (SHORT_ATTEMPT_BITS + INTERMISSION_BITS + SUSPEND_BITS)
#: The part of error passive to bus off in a trial that no host time entered: under a short the
#: model's alone; injected errors destroy each attempt halfway through its frame (half the frame
#: and the error frame, then the intermission and the suspend), so there the frames the node had
#: in its mailboxes, its schedule, set the length too. In the other trials it is "mixed" under
#: both faults.
PASSIVE_TO_BUS_OFF_PART = {"short": "model", "tx_errors": "mixed"}


def model_exact(span_ns: int, bitrate: int, late_resyncs: int) -> bool:
    """Whether an interval between two of the model's times in a trial is the model's alone:
    no late pass counted in the trial (`late_resyncs`, can.status's count over the whole trial,
    0) and the interval a whole number of bit times, to the nanosecond.

    The model's times follow exactly from the times they start from, and two kinds of start are
    host times (lib/can/can_ctrl.c). A pass of the hardware thread more than a frame length late
    starts the frame that was due at its own time (start_transmission(), counted in
    late_resyncs). A frame loaded into an idle bus starts when it entered its mailbox, the
    driver's time (transmission_start()); between two destroyed attempts that happens only after
    the transmit queue recalled the mailboxes (lib/can/can_txq.h, stale frames), and nothing
    counts it. The trace does not say when each attempt started, so such a start shows only as an
    interval off the bit grid; one whose host time fell on the grid by chance passes the test (at
    1 Mbit/s a bit is 1000 ns: about one host time in 1000). The test needs a bit time of a whole
    number of nanoseconds, as at the bench's bit rates (8000, 2000 and 1000 ns); at another the
    model rounds its times to the nanosecond, and its own intervals are seldom whole bit
    times."""
    return late_resyncs == 0 and span_ns * bitrate % 1_000_000_000 == 0


# ------------------------------------------------------- the node (app/can_io.c, app/fault.c)

#: The transmit queue's recovery policy (app/can_io.c, lib/can/can_txq.h): a hold-off before
#: the request that doubles with every further bus off up to its maximum, and back to the
#: initial value once the node has been out of bus off for the stable time.
HOLDOFF_INITIAL_MS = 100
HOLDOFF_MAX_MS = 1600
STABLE_MS = 5000
#: The DTCs (app/fault.c): CAN_BUS_OFF heals after 50 health periods of 100 ms without a bus
#: off, CAN_ERROR_PASSIVE after 10 error active.
HEALTH_PERIOD_MS = 100
BUS_OFF_HEAL_PERIODS = 50
PASSIVE_HEAL_PERIODS = 10
#: The fault numbers of the two DTCs: their bits in BMS_Fault's FaultActive and FaultConfirmed.
DTC_BUS_OFF_BIT = 9
DTC_PASSIVE_BIT = 10
DTC_BUS_OFF = "CAN_BUS_OFF"
DTC_PASSIVE = "CAN_ERROR_PASSIVE"
#: The status byte of a DTC whose test failed in this run and has passed since (lib/dtc: TFTOC,
#: PDTC, CDTC, TFSLC).
DTC_HEALED_STATUS = 0x2E

ID_FAULT = 0x010       # BMS_Fault
ID_HEARTBEAT = 0x030   # BMS_Heartbeat: PeriodCounter, TickCount

# ------------------------------------------------------------------------------ the faults

#: The fault kinds that take the controller bus off. A bus held dominant and a bus nobody
#: acknowledges never do (lib/can/can_ctrl.h; tests/system/test_can_faults.py).
#:   short      can.fault short until the node is bus off, then can.fault none
#:   tx_errors  can.inject tx 32: 32 destroyed transmissions, which end by themselves
FAULTS = ("short", "tx_errors")
TX_ERRORS = 32
#: Destroyed attempts from error active with TEC 0 to bus off: under a short +16 per attempt
#: while error active (rule 3, and rule 4 for the active flag that reads back recessive), +8
#: once passive: 8 + 16; injected errors +8 each: 16 + 16.
ATTEMPTS_TO_BUS_OFF = {"short": 24, "tx_errors": 32}
#: The trace record that starts a trial of each kind: (kind, value).
FAULT_RECORD = {"short": ("fault", 1), "tx_errors": ("inject_tx", TX_ERRORS)}


def bits_to_ns(bits: int, bitrate: int) -> int:
    """Bit times in nanoseconds as the model converts them (can_bits_to_ns(), lib/can/can_bits.c:
    rounded to the nearest nanosecond, a half up)."""
    if bitrate <= 0:
        raise ValueError(f"a bit rate is positive, not {bitrate}")
    return (bits * 1_000_000_000 + bitrate // 2) // bitrate


def ns_to_bits(ns: float, bitrate: int) -> float:
    """A time in bit times at `bitrate`."""
    return ns * bitrate / 1e9


def tec_after_attempts(fault: str, attempts: int) -> int:
    """The transmit error counter after `attempts` destroyed attempts from error active with TEC
    0 (ISO 11898-1 rules 3, 4 and 9; see ATTEMPTS_TO_BUS_OFF)."""
    if attempts < 0:
        raise ValueError("a count of attempts is not negative")
    if fault == "short":
        return 16 * attempts if attempts <= 8 else PASSIVE_LIMIT + 8 * (attempts - 8)
    if fault == "tx_errors":
        return 8 * attempts
    raise ValueError(f"unknown fault {fault!r}")


def expected_holdoffs_ms(count: int) -> list[int]:
    """The hold-offs of `count` bus offs in a row, none of them after a stable time: the initial
    one, doubled at each further bus off, up to the maximum."""
    holdoffs, holdoff = [], HOLDOFF_INITIAL_MS
    for _ in range(count):
        holdoffs.append(holdoff)
        holdoff = min(2 * holdoff, HOLDOFF_MAX_MS)
    return holdoffs


def nominal_holdoff_ms(measured_ms: float) -> int:
    """The hold-off of the policy that a measured one stands for: the nearest of 100, 200, ...,
    1600 ms on a logarithmic scale (the boundaries lie at 141, 283, 566 and 1131 ms), so that a
    request the host delayed by up to 41 % still counts as the hold-off it was; 0 for a
    measurement that is not positive. The driver's two log lines bracket the hold-off only
    approximately (the timer starts just before the first line is stamped), so a measurement
    may also lie a little below its nominal value."""
    if not measured_ms > 0:
        return 0
    values = sorted(set(expected_holdoffs_ms(6)))
    return min(values, key=lambda value: (abs(np.log(measured_ms / value)), value))


# -------------------------------------------------------------------------------- the log

#: A line of the node's log (lib/log/log.c): "[<seconds>.<microseconds>] <level> <tag>: <text>".
LOG_LINE = re.compile(r"^\[\s*(\d+)\.(\d{6})\] ([EWID]) ([\w-]+): (.*)$", re.MULTILINE)
_STATE = re.compile(r"controller state (\d) -> (\d) \(TEC (\d+), REC (\d+)\)")
_RESTART = "bus off: requesting recovery"
_DTC_ACTIVE = re.compile(r"fault (\w+) active \(DTC 0x[0-9A-F]+\)")
_DTC_GONE = re.compile(r"fault (\w+) no longer active")


@dataclass(frozen=True)
class LogEvent:
    """A line of the node's log the bench reads: "state" (the transmit task took a state event,
    `old` -> `new`, with the counters), "restart" (it asks for the recovery next), "dtc_active"
    and "dtc_gone" (the health task: `name`'s testFailed rose or fell)."""
    t_ns: int
    kind: str
    old: int | None = None
    new: int | None = None
    tec: int | None = None
    name: str | None = None


def parse_log(text: str) -> list[LogEvent]:
    """The events of the node's log `text` the bench needs, in the order of the log."""
    events = []
    for seconds, micros, _level, tag, message in LOG_LINE.findall(text):
        t_ns = int(seconds) * 1_000_000_000 + int(micros) * 1_000
        if tag == "can":
            state = _STATE.fullmatch(message)
            if state is not None:
                old, new, tec, _rec = (int(group) for group in state.groups())
                events.append(LogEvent(t_ns, "state", old=old, new=new, tec=tec))
            elif message == _RESTART:
                events.append(LogEvent(t_ns, "restart"))
        elif tag == "app":
            active, gone = _DTC_ACTIVE.fullmatch(message), _DTC_GONE.fullmatch(message)
            if active is not None:
                events.append(LogEvent(t_ns, "dtc_active", name=active.group(1)))
            elif gone is not None:
                events.append(LogEvent(t_ns, "dtc_gone", name=gone.group(1)))
    return events


def _first_log(events: Sequence[LogEvent], after_ns: int, kind: str, **fields) -> LogEvent | None:
    for event in events:
        if event.t_ns >= after_ns and event.kind == kind and all(
                getattr(event, key) == value for key, value in fields.items()):
            return event
    return None


# ----------------------------------------------------------------------------- the frames


@dataclass(frozen=True)
class Heard:
    """A frame the observer received: when its thread got it (monotonic), the kernel's receive
    time stamp (CLOCK_REALTIME, seconds; see the module text), identifier and data."""
    mono_ns: int
    real_s: float
    id: int
    data: bytes


#: Two kernel time stamps give an interval only where the monotonic times of the same two frames
#: give the same within this much: else CLOCK_REALTIME was stepped in between (or the observer's
#: thread was held up by more than this).
CLOCK_STEP_LIMIT_NS = 1_000_000


def kernel_interval_ns(earlier: Heard, later: Heard) -> int | None:
    """The time between two frames' arrivals by the kernel's stamps, or None where the realtime
    clock moved against the monotonic one in between (CLOCK_STEP_LIMIT_NS)."""
    real = round((later.real_s - earlier.real_s) * 1e9)
    mono = later.mono_ns - earlier.mono_ns
    return real if abs(real - mono) <= CLOCK_STEP_LIMIT_NS else None


def heartbeat_counter(data: bytes) -> int:
    """BMS_Heartbeat's PeriodCounter (bits 0..31, little endian): the node's 10 ms periods since
    its start, one per value, never skipped."""
    return int.from_bytes(bytes(data[:4]), "little")


def heartbeat_values_missing(counters: Sequence[int]) -> int:
    """The BMS_Heartbeat values the node never sent, from the PeriodCounters an observer heard
    from the node's start on, in order: the values before the first one heard (the counter
    starts at 1) and every gap. bus.status_missing_since_start() for counters alone; ValueError
    if they do not increase."""
    if not counters:
        return 0
    missing = counters[0] - 1
    for earlier, later in zip(counters, counters[1:]):
        if later <= earlier:
            raise ValueError(f"heartbeat counter {earlier} followed by {later}")
        missing += later - earlier - 1
    return missing


def fault_bits(data: bytes) -> tuple[int, int]:
    """BMS_Fault's FaultActive (bits 16..32) and FaultConfirmed (bits 33..49), little endian as
    the DBC lays them out: bit n is fault n (app/fault.h)."""
    word = int.from_bytes(bytes(data[:8]).ljust(8, b"\0"), "little")
    return (word >> 16) & 0x1FFFF, (word >> 33) & 0x1FFFF


# ---------------------------------------------------------------------------- one trial

#: Every interval of a trial (trial_figures()), with the part of the system it belongs to:
#: "model" (the controller model by ISO 11898-1's rules alone: exact, and the same on any host
#: and whatever frames the node sends), "policy" (the node's recovery policy: the hold-off, run
#: on the tick), "host" (when the host ran the hardware thread, the node's tasks or the
#: observer), or a sum of them ("mixed"; the node's schedule, which frames it sends when, is
#: one of them). Each is (start, end, part, what it is).
INTERVALS: dict[str, tuple[str, str, str, str]] = {
    "fault_to_bus_off": ("fault applied", "bus off (model)", "mixed",
                         "the wait for a frame to send under the fault (the node's schedule) "
                         "and the destroyed attempts (model)"),
    "passive_to_bus_off": ("error passive (model)", "bus off (model)", "mixed",
                           "the destroyed attempts of an error-passive transmitter (model; "
                           "under tx_errors of the frames the node had in its mailboxes, its "
                           "schedule), and a start a late pass moved or a frame loaded into an "
                           "idle bus (host); passive_to_bus_off_exact: neither of the last two "
                           "(model_exact())"),
    "bus_off_report_lag": ("bus off (model)", "the pass that carried it out", "host",
                           "how late the hardware thread ran"),
    "bus_off_to_driver": ("bus off (model)", "the driver took the event (log)", "host",
                          "the pass, the simulated interrupt and the transmit task"),
    "holdoff": ("the driver took the event (log)", "the driver asks (log)", "policy",
                "the hold-off (100 ms) of app/can_io.c, on the tick"),
    "request_to_count": ("the driver asks (log)", "the pass that served it", "host",
                         "the wake-up of the hardware thread"),
    "recovery_sequence": ("the count starts (model)", "error active (model)", "model",
                          "128 occurrences of 11 recessive bits"),
    "recovery_report_lag": ("error active (model)", "the pass that carried it out", "host",
                            "how late the hardware thread ran"),
    "recovery_to_driver": ("error active (model)", "the driver took the event (log)", "host",
                           "the pass, the simulated interrupt and the transmit task"),
    "active_to_first_submit": ("error active (model)", "the first frame in a mailbox", "host",
                               "the driver loads the mailboxes again"),
    "first_frame_on_bus": ("the first frame in a mailbox", "its end of frame (model)", "mixed",
                           "arbitration and the length (model) of whichever frame the node sent "
                           "first (its schedule), and a start a late pass moved (host)"),
    "first_frame_write_lag": ("its end of frame (model)", "its write to the transport", "host",
                              "how late the hardware thread ran"),
    "bus_off_to_active": ("bus off (model)", "error active (model)", "mixed",
                          "hold-off, host and the recovery sequence"),
    "removal_to_active": ("fault removed", "error active (model)", "mixed",
                          "short: the rest of the hold-off, host and the recovery sequence; "
                          "tx_errors: the fault ends at the bus off"),
    "removal_to_first_frame": ("fault removed", "the first frame's end of frame (model)",
                               "mixed", "removal_to_active and the first frame"),
    "bus_off_to_dtc": ("bus off (model)", "the DTC active (log)", "mixed",
                       "the health task's period (100 ms) and the host"),
    "dtc_heal": ("error active (model)", "the DTC no longer active (log)", "mixed",
                 "50 health periods without a bus off, and the host"),
}


@dataclass(frozen=True)
class TrialInput:
    """What the bench collected for one trial (see the module text)."""
    fault: str
    bitrate: int
    records: Sequence[TraceRecord]          # from the trial's first record on
    log: Sequence[LogEvent]                 # the run's log events, all of them
    status_before: Mapping[str, int | str]  # can.status before the fault
    status_after: Mapping[str, int | str]   # can.status after the first frame
    removal_ns: int | None = None           # short: can.fault none's t_ns


def _first(records: Sequence[TraceRecord], start: int, kind: str, **fields) -> int | None:
    for index in range(start, len(records)):
        record = records[index]
        if record.kind == kind and all(getattr(record, k) == v for k, v in fields.items()):
            return index
    return None


def trial_figures(trial: TrialInput) -> dict:
    """The figures of one trial and what is wrong with it.

    Returns {"problems": [...], "retry": bool, "figures": {...}, "trajectory": [[attempts,
    tec], ...]}: every interval of INTERVALS in nanoseconds, the bit-time figures
    ("recovery_sequence_bits", "passive_to_bus_off_bits", "first_frame_on_bus_bits"), whether
    no host time entered error passive to bus off ("passive_to_bus_off_exact", model_exact()),
    the counts ("attempts_to_bus_off", "tec_at_passive", "tec_at_bus_off", "late_resyncs",
    "bus_offs", "counter_samples") and the times the observer needs ("t_fault_ns",
    "t_removal_ns", "t_bus_off_ns", "t_restart_ns", "t_active_ns", "t_first_write_ns",
    "first_frame_id", "first_frame_number"). An interval whose source is missing is None.

    A trial with a problem is not a sample. "retry" says that the host, not the node, made it
    one, so that the bench may repeat it: a short removed only after the node had asked to
    recover (the bench was held up for most of the hold-off; the node then recovered under the
    short and went bus off again), or a hold-off that a stall of the transmit task made look
    like a doubled one. Any other problem contradicts the rules or the model (or, for error
    passive to bus off under a short, is a host time that fell on the bit grid; model_exact())."""
    problems: list[str] = []
    rec = list(trial.records)
    kind, value = FAULT_RECORD[trial.fault]
    figures: dict = {name: None for name in INTERVALS}
    out = {"problems": problems, "retry": False, "figures": figures, "trajectory": []}

    if not rec or rec[0].kind != kind or rec[0].value != value:
        problems.append(f"the trial's first record is not its fault ({kind} {value})")
        return out
    start = rec[0]
    i_passive = _first(rec, 1, "state", old_state=0, state=1)
    i_bus_off = _first(rec, 1, "state", old_state=1, state=2)
    if i_passive is None or i_bus_off is None or i_bus_off < i_passive:
        problems.append("no way through error passive into bus off in the trace")
        return out
    i_restart = _first(rec, i_bus_off, "restart")
    i_active = _first(rec, i_bus_off, "state", old_state=2, state=0)
    i_first = _first(rec, i_active, "first_tx") if i_active is not None else None
    if i_restart is None or i_active is None or i_first is None or not (
            i_restart < i_active < i_first):
        problems.append("no restart, recovery and first frame after the bus off in the trace")
        return out
    passive, bus_off, restart, active, first = (rec[i] for i in (i_passive, i_bus_off, i_restart,
                                                                 i_active, i_first))

    # The removal: for a short the bench's can.fault none, which must come before the request
    # (else the node recovered under the short and went bus off again: the bench was late, and
    # nothing more of the trial is judged); injected errors end with the last destroyed attempt,
    # at the bus off.
    if trial.fault == "short":
        i_removal = _first(rec, i_bus_off, "fault", value=0)
        if i_removal is None or trial.removal_ns is None:
            problems.append("the short was not removed after the bus off")
            return out
        removal_ns = rec[i_removal].pass_ns
        if removal_ns != trial.removal_ns:
            problems.append("can.fault none's time and its trace record differ")
        if i_removal > i_restart:
            problems.append("the bench removed the short only after the node had asked to "
                            "recover")
            out["retry"] = True
            return out
    else:
        removal_ns = bus_off.model_ns

    bus_offs = sum(1 for r in rec[:i_first + 1] if r.kind == "state" and r.state == 2)
    if bus_offs != 1:
        problems.append(f"{bus_offs} bus offs in one trial")
    if restart.state != 2:
        problems.append("the restart request was served while the node was not bus off")

    # The way up: every sample of the counters on the rule of the fault.
    attempts0 = start.attempts_failed
    trajectory = []
    for r in rec[1:i_bus_off + 1]:
        if r.kind in ("counters", "state"):
            attempts = r.attempts_failed - attempts0
            if r.kind == "counters" and r.tec != tec_after_attempts(trial.fault, attempts):
                problems.append(f"TEC {r.tec} after {attempts} destroyed attempts, not "
                                f"{tec_after_attempts(trial.fault, attempts)}")
            if r.kind == "counters":
                trajectory.append([attempts, r.tec])
    out["trajectory"] = trajectory

    attempts = bus_off.attempts_failed - attempts0
    if attempts != ATTEMPTS_TO_BUS_OFF[trial.fault]:
        problems.append(f"{attempts} destroyed attempts to bus off, not "
                        f"{ATTEMPTS_TO_BUS_OFF[trial.fault]}")
    if passive.tec != PASSIVE_LIMIT or bus_off.tec != BUS_OFF_LIMIT:
        problems.append(f"TEC {passive.tec} at error passive and {bus_off.tec} at bus off")
    sequence_ns = active.model_ns - restart.model_ns
    if sequence_ns != bits_to_ns(RECOVERY_BITS, trial.bitrate):
        problems.append(f"the recovery took {sequence_ns} ns, not {RECOVERY_BITS} bit times")
    if active.tec != 0 or active.rec != 0:
        problems.append("the counters were not 0 at the recovery")

    resyncs = int(trial.status_after["late_resyncs"]) - int(trial.status_before["late_resyncs"])
    failed = int(trial.status_after["tx_failed"]) - int(trial.status_before["tx_failed"])
    status_bus_offs = int(trial.status_after["bus_off"]) - int(trial.status_before["bus_off"])
    if failed != ATTEMPTS_TO_BUS_OFF[trial.fault]:
        problems.append(f"can.status counted {failed} destroyed attempts in the trial")
    if status_bus_offs != 1:
        problems.append(f"can.status counted {status_bus_offs} bus offs in the trial")

    # The driver's side, from the log: the first of each after the model's moment.
    seen_bus_off = _first_log(trial.log, bus_off.model_ns, "state", old=1, new=2)
    asked = _first_log(trial.log, seen_bus_off.t_ns, "restart") if seen_bus_off else None
    seen_active = _first_log(trial.log, active.model_ns, "state", old=2, new=0)
    dtc_on = _first_log(trial.log, bus_off.model_ns, "dtc_active", name=DTC_BUS_OFF)
    dtc_off = _first_log(trial.log, active.model_ns, "dtc_gone", name=DTC_BUS_OFF)
    if seen_bus_off is None or asked is None or seen_active is None:
        problems.append("the driver's lines about the bus off are not all in the log")
    elif not seen_bus_off.t_ns <= asked.t_ns <= restart.pass_ns:
        problems.append("the driver's request in the log does not fit the trace")

    def span(begin: int | None, end: int | None) -> int | None:
        return None if begin is None or end is None else end - begin

    figures.update({
        "fault_to_bus_off": bus_off.model_ns - start.pass_ns,
        "passive_to_bus_off": bus_off.model_ns - passive.model_ns,
        "bus_off_report_lag": bus_off.pass_ns - bus_off.model_ns,
        "bus_off_to_driver": span(bus_off.model_ns, seen_bus_off and seen_bus_off.t_ns),
        "holdoff": span(seen_bus_off and seen_bus_off.t_ns, asked and asked.t_ns),
        "request_to_count": span(asked and asked.t_ns, restart.pass_ns),
        "recovery_sequence": sequence_ns,
        "recovery_report_lag": active.pass_ns - active.model_ns,
        "recovery_to_driver": span(active.model_ns, seen_active and seen_active.t_ns),
        "active_to_first_submit": first.submit_ns - active.model_ns,
        "first_frame_on_bus": first.model_ns - first.submit_ns,
        "first_frame_write_lag": first.pass_ns - first.model_ns,
        "bus_off_to_active": active.model_ns - bus_off.model_ns,
        "removal_to_active": span(removal_ns, active.model_ns),
        "removal_to_first_frame": span(removal_ns, first.model_ns),
        "bus_off_to_dtc": span(bus_off.model_ns, dtc_on and dtc_on.t_ns),
        "dtc_heal": span(active.model_ns, dtc_off and dtc_off.t_ns),
    })
    exact = model_exact(figures["passive_to_bus_off"], trial.bitrate, resyncs)
    figures.update({
        "recovery_sequence_bits": ns_to_bits(sequence_ns, trial.bitrate),
        "passive_to_bus_off_bits": ns_to_bits(bus_off.model_ns - passive.model_ns, trial.bitrate),
        "passive_to_bus_off_exact": exact,
        "first_frame_on_bus_bits": ns_to_bits(first.model_ns - first.start_ns, trial.bitrate),
        "attempts_to_bus_off": attempts,
        "tec_at_passive": passive.tec,
        "tec_at_bus_off": bus_off.tec,
        "late_resyncs": resyncs,
        "bus_offs": bus_offs,
        "counter_samples": len(trajectory),
        "t_fault_ns": start.pass_ns,
        "t_removal_ns": removal_ns,
        "t_bus_off_ns": bus_off.model_ns,
        "t_restart_ns": restart.pass_ns,
        "t_active_ns": active.model_ns,
        "t_first_write_ns": first.pass_ns,
        "first_frame_id": first.value,
        "first_frame_number": first.frames,
    })
    # The bench waits out the stable time before every trial, so the hold-off is the initial
    # one. A measured one that looks doubled is a transmit task the host held up (retry).
    if figures["holdoff"] is not None and (nominal_holdoff_ms(figures["holdoff"] / 1e6)
                                           != HOLDOFF_INITIAL_MS):
        problems.append(f"a hold-off of {figures['holdoff'] / 1e6:.3f} ms, not the initial "
                        f"{HOLDOFF_INITIAL_MS} ms")
        out["retry"] = True
    # Under a short the attempts from error passive on are 26 bit times apart whatever the
    # frames, so a trial that no host time entered takes exactly 416 bit times. A trial that one
    # did is no sample of the model, not a fault of it (only one whose host time fell on the bit
    # grid by chance passes for exact, and is refused here). Injected errors destroy frames
    # halfway, so there the interval depends on the frames attempted, which the trace does not
    # give.
    if (trial.fault == "short" and exact
            and figures["passive_to_bus_off"] != bits_to_ns(SHORT_PASSIVE_TO_BUS_OFF_BITS,
                                                             trial.bitrate)):
        problems.append(f"{figures['passive_to_bus_off']} ns from error passive to bus off "
                        f"without a late pass and on the bit grid, not "
                        f"{SHORT_PASSIVE_TO_BUS_OFF_BITS} bit times")
    return out


def observer_figures(frames: Sequence[Heard], figures: Mapping, until_ns: int,
                     offset: int = 0) -> dict:
    """What the observer heard around one trial: `frames` in the order received, the node's
    frames number offset + 1, offset + 2, ... (the observer hears every frame of the node from
    its start, in the order sent), `figures` the trial's (trial_figures()), `until_ns` the end
    of the trial (the next one's start, or the end of the run). The first frame after the
    recovery is the one whose number the trace gives ("first_frame_number"): found by counting,
    not by comparing clocks, so that an observer the host held up still finds it. Returns
    {"problems": [...], ...}:

      - "silence_ns": from the last frame heard before the bus off to the first after the
        recovery, by the kernel's stamps (kernel_interval_ns(); None across a clock step);
      - "observer_lag_ns": the first frame's write to the transport (trace) to the observer's
        thread receiving it;
      - "removal_to_observer_ns": the removal of the fault to that moment;
      - "heartbeats_lost": BMS_Heartbeat values the node never sent (PeriodCounter's jump - 1),
        and "first_heartbeat_fresh", whether the first one after the recovery was the current
        value (the next one heard is counter + 1);
      - "dtc_bus_off_after", "dtc_passive_after": whether the first BMS_Fault after the recovery
        reports the DTC active and confirmed; "dtc_bus_off_healed_ns" and
        "dtc_passive_healed_ns": from error active (model) to the first BMS_Fault without the
        bit (None if none came before `until_ns`).

    The problems are what cannot happen whatever the host does: the frame the trace names not
    heard, or heard with another identifier, or before the trace's write of it; too few
    heartbeats around the bus off; no BMS_Fault after the recovery; or BMS_Fault reporting a
    fault other than the two of the bus (the node then had another fault in the trial).
    Freshness and the DTCs in the first BMS_Fault are figures: a late host task can change
    them."""
    problems: list[str] = []
    out: dict = {"problems": problems}
    active = figures["t_active_ns"]
    after = figures["first_frame_number"] - 1 - offset
    if not 0 < after < len(frames) or frames[after].mono_ns >= until_ns:
        problems.append(f"the observer did not hear the node's frame {after + offset + 1}, the "
                        "first after the recovery, or none before it")
        return out
    first, last = frames[after], frames[after - 1]
    if first.id != figures["first_frame_id"]:
        problems.append(f"the node's frame {after + offset + 1}, the first after the recovery, "
                        f"was heard as {first.id:#x}; the trace says {figures['first_frame_id']:#x}")
    if first.mono_ns < figures["t_first_write_ns"]:
        problems.append("the first frame after the recovery was heard before the pass that wrote "
                        "it")
    out["silence_ns"] = kernel_interval_ns(last, first)
    out["observer_lag_ns"] = first.mono_ns - figures["t_first_write_ns"]
    removal = figures["t_removal_ns"]
    out["removal_to_observer_ns"] = None if removal is None else first.mono_ns - removal

    window = [f for f in frames if f.mono_ns < until_ns]
    before_beats = [heartbeat_counter(f.data) for f in window[:after] if f.id == ID_HEARTBEAT]
    after_beats = [heartbeat_counter(f.data) for f in window[after:] if f.id == ID_HEARTBEAT]
    if not before_beats or len(after_beats) < 2:
        problems.append("too few heartbeats heard around the trial")
        out["heartbeats_lost"] = None
        out["first_heartbeat_fresh"] = None
    else:
        # A figure, not a problem: the second heartbeat can also miss its successor when the host
        # holds the transmit task up for a period (the value is then superseded and counted).
        out["heartbeats_lost"] = after_beats[0] - before_beats[-1] - 1
        out["first_heartbeat_fresh"] = after_beats[1] == after_beats[0] + 1

    faults = [f for f in window[after:] if f.id == ID_FAULT]
    out.update({"dtc_bus_off_after": None, "dtc_passive_after": None,
                "dtc_bus_off_healed_ns": None, "dtc_passive_healed_ns": None})
    if not faults:
        problems.append("no BMS_Fault heard after the recovery")
        return out
    # Figures, not problems: the health task sets the DTCs and publishes BMS_Fault every 100 ms,
    # and the node is off the bus for little more than that (the hold-off and the recovery). A
    # health cycle the host runs late, or one that ran between the model's bus off and the
    # transmit task taking it, leaves the first BMS_Fault after the recovery without the bits.
    # Whether every episode set and healed the DTCs once is a count (session_checks()).
    active_bits, confirmed_bits = fault_bits(faults[0].data)
    both = (1 << DTC_BUS_OFF_BIT) | (1 << DTC_PASSIVE_BIT)
    out["dtc_bus_off_after"] = bool(active_bits >> DTC_BUS_OFF_BIT & 1) and bool(
        confirmed_bits >> DTC_BUS_OFF_BIT & 1)
    out["dtc_passive_after"] = bool(active_bits >> DTC_PASSIVE_BIT & 1) and bool(
        confirmed_bits >> DTC_PASSIVE_BIT & 1)
    for frame in faults:
        active_bits, _ = fault_bits(frame.data)
        if active_bits & ~both:
            problems.append(f"BMS_Fault reports another fault: FaultActive {active_bits:#x}")
            break
    for bit, key in ((DTC_BUS_OFF_BIT, "dtc_bus_off_healed_ns"),
                     (DTC_PASSIVE_BIT, "dtc_passive_healed_ns")):
        healed = next((f for f in faults if not fault_bits(f.data)[0] >> bit & 1), None)
        out[key] = None if healed is None else healed.mono_ns - active
    return out


# --------------------------------------------------------------- a persisting short


def persist_figures(records: Sequence[TraceRecord], log: Sequence[LogEvent], bitrate: int,
                    bus_offs: int) -> dict:
    """A short held through `bus_offs` bus offs, then removed: each bus off's hold-off (from the
    driver's lines in the log), each recovery sequence, the destroyed attempts of each way into
    bus off, against the policy (expected_holdoffs_ms()). Returns {"problems", "holdoffs_ms",
    "nominal_ms", "expected_ms", "holdoffs_as_policy", "recovery_sequence_bits", "attempts",
    "recoveries_under_short"}. `records` start with the trial's can.fault short.

    The hold-offs are host timing (the transmit task's wake-up on the tick): whether each one's
    nominal value is the policy's is reported ("holdoffs_as_policy"), not counted as a problem.
    Everything else is the model's or a count, and a problem if it is wrong."""
    problems: list[str] = []
    rec = list(records)
    out: dict = {"problems": problems, "holdoffs_ms": [], "nominal_ms": [],
                 "expected_ms": expected_holdoffs_ms(bus_offs), "holdoffs_as_policy": False,
                 "recovery_sequence_bits": [], "attempts": [], "recoveries_under_short": 0}
    if not rec or rec[0].kind != "fault" or rec[0].value != 1:
        problems.append("the persisting trial's first record is not can.fault short")
        return out
    removal = _first(rec, 1, "fault", value=0)
    if removal is None:
        problems.append("the short was never removed")
        return out
    downs = [i for i, r in enumerate(rec) if r.kind == "state" and r.state == 2]
    if len(downs) != bus_offs:
        problems.append(f"{len(downs)} bus offs, not {bus_offs}")
    attempts_before = rec[0].attempts_failed
    for i in downs:
        out["attempts"].append(rec[i].attempts_failed - attempts_before)
        attempts_before = rec[i].attempts_failed
        seen = _first_log(log, rec[i].model_ns, "state", old=1, new=2)
        asked = _first_log(log, seen.t_ns, "restart") if seen else None
        if seen is None or asked is None:
            problems.append("a bus off without its driver lines in the log")
            continue
        holdoff_ms = (asked.t_ns - seen.t_ns) / 1e6
        out["holdoffs_ms"].append(round(holdoff_ms, 3))
        out["nominal_ms"].append(nominal_holdoff_ms(holdoff_ms))
        restart = _first(rec, i, "restart")
        active = _first(rec, i, "state", old_state=2, state=0)
        if restart is None or active is None or active < restart:
            problems.append("a bus off without its restart and recovery in the trace")
            continue
        out["recovery_sequence_bits"].append(
            ns_to_bits(rec[active].model_ns - rec[restart].model_ns, bitrate))
        if active < removal:
            out["recoveries_under_short"] += 1
    out["holdoffs_as_policy"] = out["nominal_ms"] == out["expected_ms"]
    if any(a != ATTEMPTS_TO_BUS_OFF["short"] for a in out["attempts"]):
        problems.append(f"destroyed attempts per bus off {out['attempts']}, not 24 each")
    if any(bits != RECOVERY_BITS for bits in out["recovery_sequence_bits"]):
        problems.append("a recovery sequence that was not 1408 bit times")
    if out["recoveries_under_short"] != bus_offs - 1:
        problems.append(f"{out['recoveries_under_short']} recoveries while the short was there, "
                        f"not {bus_offs - 1}")
    return out


# ----------------------------------------------------------------------- the run's counts


@dataclass(frozen=True)
class Check:
    """One count of the run checked against what the bench did: equal to `expected`, or with
    `at_most` given, from `expected` to `at_most`."""
    name: str
    expected: object
    found: object
    at_most: object = None

    @property
    def ok(self) -> bool:
        if self.at_most is None:
            return self.expected == self.found
        return self.expected <= self.found <= self.at_most  # type: ignore[operator]

    def as_dict(self) -> dict:
        entry = {"name": self.name, "expected": self.expected, "found": self.found, "ok": self.ok}
        if self.at_most is not None:
            entry["at_most"] = self.at_most
        return entry


def session_checks(stats: Mapping, bus_offs: int, episodes: int, log: Sequence[LogEvent],
                   heard: Mapping[int, int], heartbeats_missing: int | None,
                   observer_overflow: int) -> list[Check]:
    """The counts of a node's run (its statistics `stats`, its log events) against what the bench
    did: `bus_offs` bus offs caused in all, in `episodes` stretches each followed by a recovery
    and the heal of the DTCs (one per trial, one for a persisting short). `heard`: the frames the
    observer received per identifier, `heartbeats_missing` the BMS_Heartbeat values missing in
    what it heard from the start (bus.status_missing_since_start), `observer_overflow` the
    frames its socket dropped (bus.receive_overflow)."""
    can, telemetry = stats["can"], stats["telemetry"]["messages"]
    queue, controller = can["queue"], can["controller"]
    dtcs = {entry["name"]: entry for entry in stats["faults"]["dtcs"]}
    checks = [
        Check("controller bus off events", bus_offs, controller["bus_off_events"]),
        Check("queue bus off count", bus_offs, queue["bus_off_count"]),
        Check("queue restart requests", bus_offs, queue["restart_requests"]),
        Check("queue hold-off at the end (ms)", HOLDOFF_INITIAL_MS, queue["holdoff_ms"]),
        Check("queue dropped + expired event frames", 0, queue["dropped"] + queue["expired"]),
        Check("queue produced = sent + superseded + dropped + expired + pending",
              queue["produced"], queue["sent"] + queue["superseded"] + queue["dropped"]
              + queue["expired"] + queue["pending"]),
        Check("driver state events merged", 0, can["driver"]["state_events_merged"]),
        Check("driver event FIFO overruns", 0, can["driver"]["event_fifo_overruns"]),
        Check("transport frames sent = controller frames sent", controller["tx_frames"],
              can["transport_counters"]["tx_frames"]),
        Check(f"{DTC_BUS_OFF} status byte", DTC_HEALED_STATUS, dtcs[DTC_BUS_OFF]["status"]),
        Check(f"{DTC_PASSIVE} status byte", DTC_HEALED_STATUS, dtcs[DTC_PASSIVE]["status"]),
        Check(f"{DTC_BUS_OFF} failed operation cycles", 1, dtcs[DTC_BUS_OFF]["failed_cycles"]),
        Check(f"log lines '{DTC_BUS_OFF} active'", episodes,
              sum(1 for e in log if e.kind == "dtc_active" and e.name == DTC_BUS_OFF)),
        Check(f"log lines '{DTC_BUS_OFF} no longer active'", episodes,
              sum(1 for e in log if e.kind == "dtc_gone" and e.name == DTC_BUS_OFF)),
        Check(f"log lines '{DTC_PASSIVE} active'", episodes,
              sum(1 for e in log if e.kind == "dtc_active" and e.name == DTC_PASSIVE)),
        Check(f"log lines '{DTC_PASSIVE} no longer active'", episodes,
              sum(1 for e in log if e.kind == "dtc_gone" and e.name == DTC_PASSIVE)),
        Check("log lines 'requesting recovery'", bus_offs,
              sum(1 for e in log if e.kind == "restart")),
        Check("observer socket overflow", 0, observer_overflow),
    ]
    if observer_overflow == 0:
        checks.append(Check("frames heard = controller frames sent", controller["tx_frames"],
                            sum(heard.values())))
    for name, message in telemetry.items():
        slot = message["queue"]
        checks.append(Check(f"{name}: produced = sent + superseded + pending", slot["produced"],
                            slot["sent"] + slot["superseded"] + slot["pending"]))
        if observer_overflow == 0:
            # The queue's counters are taken just before the scheduler ends, the controller's
            # after the hardware thread stopped: a frame that left in between is heard, and
            # counted by the queue as pending (node_main.c, write_can_stats).
            checks.append(Check(f"{name}: frames heard, from sent to sent + pending",
                                slot["sent"], heard.get(message["id"], 0),
                                at_most=slot["sent"] + slot["pending"]))
    beats = telemetry.get("BMS_Heartbeat")
    if beats is not None and heartbeats_missing is not None and observer_overflow == 0:
        checks.append(Check("BMS_Heartbeat values missing on the bus = superseded",
                            beats["queue"]["superseded"], heartbeats_missing))
    return checks


# -------------------------------------------------------------------------- distributions


def column(trials: Iterable[Mapping], key: str, scale: float = 1.0) -> list[float]:
    """The values of `key` in the trials that have one (not None), times `scale`."""
    return [value * scale for value in (trial.get(key) for trial in trials) if value is not None]


def share(values: Iterable[bool | None]) -> tuple[int, int]:
    """(how many are True, how many are not None)."""
    known = [bool(v) for v in values if v is not None]
    return sum(known), len(known)


def as_float_array(values: Iterable[float]) -> np.ndarray:
    return np.asarray(list(values), dtype=np.float64)
