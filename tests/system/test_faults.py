# SPDX-License-Identifier: MIT
"""The fault handling of phase 3 in the running node, from outside: the DTCs in BMS_Fault, in
BMS_PackStatus and in the statistics, their clear by TESTER_Command, the supervision of the
tasks and the hardware watchdog, and the reset cause the next start reports from the
retained-RAM cell (docs/faults.md).

Faults are applied as on a bench (simulation control: sensor.*, can.*; the retained cell
through --retained-file); the node has no hook for them. Every frame is decoded with cantools from
the DBC and every protected one checked end to end with fcn_tools.e2e, an implementation of
Profile 11 independent of the node's lib/e2e (fcn_tools.decode). The tests that concern the
bus run on both transports: the bus fault, the watchdog reset and the frames after it, the
reset causes, and one fault of the pack with its clear. A sensor fault is the same fault
whichever transport carries BMS_Fault, so the table of sensor faults runs on the UDP bus only.

What a status byte must be is written here from ISO 14229-1 Annex D and the table of
app/fault.c (SPEC below): a DTC whose test failed in this operation cycle (a run of the node)
and still fails has TF, TFTOC, PDTC, CDTC and TFSLC, with WIR if it requests the warning
indicator; once its test has passed again it has lost TF and kept the rest until a clear.

A test waits for frames or for the node to end, never for a fixed time. Counts are exact
against the node's statistics; where a result depends on the host keeping time it is a timing
tolerance (check_timing, the tests marked `timing`). The fault monitor's rules are tested
without a node in tests/unit/test_fault.c and lib/dtc's in tests/unit/test_dtc.c; here the
question is whether the tasks put them together as app/app.c says.

What each test prints (pytest -rP) is what the next engineer can quote: the time from a fault
to the frame that reports it, and from a hang to the reset, measured on CLOCK_MONOTONIC, the
clock of the node's plat_time_ns() and of the `t_ns` in a simulation control reply.
"""
from __future__ import annotations

import os
import re
import struct
import threading
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import can
import pytest

from fcn_tools import e2e
from fcn_tools.bus import ID_PING, open_bus
from fcn_tools.decode import DecodedFrame, Decoder, load_database
from fcn_tools.node import EXIT_OK, RESET_WATCHDOG, NodeProcess
from fcn_tools.simctl import SimControl
from fcn_tools.timing import check_timing

DB = load_database()
ID = {message.name: message.frame_id for message in DB.messages}
FAULT = "BMS_Fault"
PACK_STATUS = "BMS_PackStatus"
HEARTBEAT = "BMS_Heartbeat"
NODE_INFO = "BMS_NodeInfo"
COMMAND = "TESTER_Command"
#: The faults by name, their numbers the bits of FaultActive and FaultConfirmed (app/fault.h).
NUMBER = {str(name): value for value, name in
          DB.get_message_by_name(FAULT).get_signal_by_name("FaultMostRecent").choices.items()
          if str(name) != "NONE"}
NAME = {number: name for name, number in NUMBER.items()}


def bit(name: str) -> int:
    return 1 << NUMBER[name]


# --------------------------------------------------------------- the DTCs and their status


@dataclass(frozen=True)
class Dtc:
    """A fault's entry in the table of app/fault.c, as docs/faults.md documents it: its DTC
    number and display code, its debounce thresholds (every step is 1) and whether it requests
    the warning indicator."""

    number: int
    code: str
    failed: int     # failed_threshold
    passed: int     # passed_threshold
    warning: bool


SPEC = {
    "CELL_OVERVOLTAGE": Dtc(0x1A0100, "P1A01-00", 5, -5, True),
    "CELL_UNDERVOLTAGE": Dtc(0x1A0200, "P1A02-00", 5, -5, True),
    "OVERTEMPERATURE": Dtc(0x1A0300, "P1A03-00", 3, -3, True),
    "UNDERTEMPERATURE": Dtc(0x1A0400, "P1A04-00", 3, -3, True),
    "OVERCURRENT_DISCHARGE": Dtc(0x1A0500, "P1A05-00", 50, -50, True),
    "OVERCURRENT_CHARGE": Dtc(0x1A0600, "P1A06-00", 50, -50, True),
    "SENSOR_CHANNEL_INVALID": Dtc(0x1A1000, "P1A10-00", 1, -1, True),
    "FRONT_END_NO_ANSWER": Dtc(0x1A1100, "P1A11-00", 1, -1, True),
    "SAMPLE_TIMEOUT": Dtc(0x1A1200, "P1A12-00", 1, -1, True),
    "CAN_BUS_OFF": Dtc(0xD00100, "U1001-00", 1, -50, False),
    "CAN_ERROR_PASSIVE": Dtc(0xD00200, "U1002-00", 1, -10, False),
    "SUPERVISION_EXPIRED": Dtc(0x1B0100, "P1B01-00", 1, -1, False),
    "RESET_WATCHDOG": Dtc(0x1B0200, "P1B02-00", 1, -1, False),
    "RESET_ASSERT": Dtc(0x1B0300, "P1B03-00", 1, -1, False),
    "RESET_STACK_OVERFLOW": Dtc(0x1B0400, "P1B04-00", 1, -1, False),
    "RESET_MALLOC_FAILED": Dtc(0x1B0500, "P1B05-00", 1, -1, False),
    "UPDATE_ROLLBACK": Dtc(0x1B0600, "P1B06-00", 1, -1, False),
}
assert list(SPEC) == [NAME[n] for n in range(len(NAME))], "SPEC is in the DBC's order"

# The bits of statusOfDTC, ISO 14229-1 Annex D.
TF, TFTOC, PDTC, CDTC, TNCSLC, TFSLC, TNCTOC, WIR = (1 << n for n in range(8))
#: The status byte of every DTC after the start of a run and after a clear (lib/dtc).
AFTER_CLEAR = TNCSLC | TNCTOC
#: The bits only a DTC whose test failed since the start or the last clear can have.
FAILED_BITS = TF | TFTOC | PDTC | CDTC | TFSLC | WIR


def failing(name: str) -> int:
    """The status byte of a DTC whose test failed in this run and still fails."""
    return TF | TFTOC | PDTC | CDTC | TFSLC | (WIR if SPEC[name].warning else 0)


def healed(name: str) -> int:
    """The status byte of a DTC whose test failed in this run and has passed since."""
    return failing(name) & ~TF


def fdc(name: str, counter: int) -> int:
    """The DTCFaultDetectionCounter of a debounce counter (lib/dtc: linear to -128..127,
    rounded toward zero)."""
    spec = SPEC[name]
    if counter >= 0:
        return counter * 127 // spec.failed
    return -((-counter * 128) // -spec.passed)


def check_dtcs(stats: dict, expected: dict[str, set[int] | int]) -> dict[str, dict]:
    """The DTCs in the statistics: each one's number and code as documented, its debounce
    counter within its thresholds and its fault detection counter mapped from it, the status
    byte `expected` gives for the DTCs a test made fail (one value or a set of them), and for
    every other DTC no bit that only a failed test sets; and the summary agrees with the
    bytes. (The summary also has a fault active whose testFailed rose in the last health period
    and fell again; the tests stop the node after a steady period, in which none did.)
    Returns the DTCs by name."""
    dtcs = {entry["name"]: entry for entry in stats["faults"]["dtcs"]}
    assert list(dtcs) == list(SPEC)
    active = confirmed = 0
    for name, entry in dtcs.items():
        spec = SPEC[name]
        assert (entry["event"], entry["dtc"], entry["code"]) == \
            (NUMBER[name], spec.number, spec.code), entry
        assert spec.passed <= entry["counter"] <= spec.failed, entry
        assert entry["fdc"] == fdc(name, entry["counter"]), entry
        wanted = expected.get(name)
        if wanted is None:
            assert entry["status"] & FAILED_BITS == 0, (entry, "a DTC that should not have failed")
            assert entry["failed_cycles"] == 0, entry
        else:
            allowed = {wanted} if isinstance(wanted, int) else wanted
            assert entry["status"] in allowed, (entry, [hex(s) for s in allowed])
            assert entry["failed_cycles"] == 1, entry
        active |= bit(name) if entry["status"] & TF else 0
        confirmed |= bit(name) if entry["status"] & CDTC else 0
    faults = stats["faults"]
    assert (faults["active"], faults["confirmed"]) == (active, confirmed), faults
    assert faults["pack_fault_active"] == (active != 0)
    assert faults["warning"] == any(entry["status"] & WIR for entry in dtcs.values())
    return dtcs


# ------------------------------------------------------------------------------ the tester


@dataclass(eq=False)
class Heard:
    index: int              # its position in Recorder.heard
    message: can.Message
    frame: DecodedFrame
    at_ns: int              # CLOCK_MONOTONIC when the tester read it


class Recorder:
    """Reads and decodes every frame of a bus in a thread of its own; tests wait for a frame
    that meets a condition."""

    def __init__(self, bus: can.BusABC) -> None:
        self._bus = bus
        self._decoder = Decoder(DB)
        self.heard: list[Heard] = []
        self._changed = threading.Condition()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            message = self._bus.recv(timeout=0.05)
            if message is not None:
                at_ns = time.monotonic_ns()
                decoded = self._decoder.decode(message)
                if decoded is not None:
                    with self._changed:
                        self.heard.append(Heard(len(self.heard), message, decoded, at_ns))
                        self._changed.notify_all()

    def until(self, name: str, condition: Callable[[DecodedFrame], bool] = lambda f: True,
              timeout: float = 10.0, after: int = 0) -> Heard:
        """The first frame of `name` heard from position `after` on for which `condition` holds;
        fails after `timeout` seconds (a bound on the wait, not a timing tolerance)."""
        position = after
        deadline = time.monotonic() + timeout
        with self._changed:
            while True:
                while position < len(self.heard):
                    heard = self.heard[position]
                    position += 1
                    if heard.frame.name == name and condition(heard.frame):
                        return heard
                remaining = deadline - time.monotonic()
                assert remaining > 0, f"no {name} frame as expected within {timeout} s"
                self._changed.wait(remaining)

    def next(self, name: str, heard: Heard, count: int = 1) -> list[Heard]:
        """The `count` frames of `name` after `heard`."""
        frames = []
        for _ in range(count):
            heard = self.until(name, after=heard.index + 1)
            frames.append(heard)
        return frames

    def mark(self) -> int:
        """A position in the frames heard, for until(after=...)."""
        with self._changed:
            return len(self.heard)

    def quiet(self, seconds: float = 0.1, timeout: float = 5.0) -> int:
        """Wait until no frame has come for `seconds` (after a run ended: its last frames are
        read) and return the position then."""
        deadline = time.monotonic() + timeout
        with self._changed:
            while time.monotonic() < deadline:
                count = len(self.heard)
                self._changed.wait(seconds)
                if len(self.heard) == count:
                    return count
        raise AssertionError(f"the bus did not go quiet within {timeout} s")

    def frames(self, name: str, start: int = 0, end: int | None = None) -> list[Heard]:
        with self._changed:
            return [h for h in self.heard[start:end] if h.frame.name == name]

    def decoded_again(self, name: str, start: int = 0, end: int | None = None) \
            -> list[DecodedFrame]:
        """The frames of `name` heard from `start` to `end`, decoded again by a decoder of their
        own: the E2E checks of one run of the node, from its first frame."""
        decoder = Decoder(DB)
        with self._changed:
            messages = [h.message for h in self.heard[start:end]]
        return [frame for frame in map(decoder.decode, messages)
                if frame is not None and frame.name == name]

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=5)


class CommandTester:
    """Sends TESTER_Command, protected with fcn_tools.e2e, every 20 ms from a thread of its own,
    with the CmdClearFaultsSeq of the moment: the node acts from the third frame on, and a change
    of the sequence between two frames it acts on is a clear request (app/command.h)."""

    def __init__(self, spec: str, seq: int) -> None:
        self.seq = seq
        self._bus = open_bus(spec)
        self._sender = e2e.Sender(e2e.Config(data_id=Decoder(DB).protected[ID[COMMAND]],
                                             data_length=64))
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _send(self) -> None:
        payload = DB.encode_message(COMMAND, {"E2E_Crc": 0, "E2E_Counter": 0,
                                              "CmdTelemetryOn": 1,
                                              "CmdClearFaultsSeq": self.seq})
        data = self._sender.protect(payload)
        self._bus.send(can.Message(arbitration_id=ID[COMMAND], data=data, is_extended_id=False))

    def _run(self) -> None:
        while not self._stop.is_set():
            with self._lock:
                self._send()
            self._stop.wait(0.02)

    def change_now(self, seq: int, ping: bytes) -> None:
        """Change CmdClearFaultsSeq and send a command with it at once, then a TESTER_Ping with
        `ping` as its payload, both on this tester's bus. The node's receive task takes the
        frames of the bus in their order and answers the ping after it has checked the command:
        once its BMS_Pong is heard, the request has been counted."""
        with self._lock:
            self.seq = seq
            self._send()
            self._bus.send(can.Message(arbitration_id=ID_PING, data=ping, is_extended_id=False))

    def close(self) -> None:
        if not self._stop.is_set():
            self._stop.set()
            self._thread.join(timeout=5)
            self._bus.shutdown()


class EndWatch:
    """The moment a node's process ends, as the bench sees it: a thread blocked in waitid()
    with WNOWAIT, which returns at the exit and leaves the process for NodeProcess.wait() to
    reap. (Popen.wait() with a timeout polls, and can see the end up to 50 ms late.)"""

    def __init__(self, node: NodeProcess) -> None:
        self.at_ns: int | None = None
        self._thread = threading.Thread(target=self._run, args=(node.pid,), daemon=True)
        self._thread.start()

    def _run(self, pid: int) -> None:
        try:
            os.waitid(os.P_PID, pid, os.WEXITED | os.WNOWAIT)
        except ChildProcessError:
            pass                        # reaped by NodeProcess.wait() in the same instant
        self.at_ns = time.monotonic_ns()

    def ended_ns(self, timeout: float = 5.0) -> int:
        self._thread.join(timeout)
        assert self.at_ns is not None, "the node has not ended"
        return self.at_ns


def check_fault_frames(recorder: Recorder, start: int, end: int | None, stats: dict,
                       gaps_exact: bool = True) -> list[DecodedFrame]:
    """Every BMS_Fault frame of one run (positions `start` to `end` of the recorder) against
    the run's statistics: one value per health period, nothing failed or switched off, every
    value the transmit queue sent heard, every frame intact by the independent Profile 11
    check, and (with `gaps_exact`) the values the queue replaced before they went out exactly
    the gaps in the counter. Returns the frames."""
    frames = recorder.decoded_again(FAULT, start, end)
    message = stats["telemetry"]["messages"][FAULT]
    queue = message["queue"]
    assert (message["failed"], message["suppressed"]) == (0, 0), message
    assert message["published"] == stats["faults"]["health_cycles"], message
    assert queue["produced"] == message["published"], message
    assert queue["produced"] == queue["sent"] + queue["superseded"] + queue["pending"], message
    assert queue["sent"] <= len(frames) <= queue["sent"] + queue["pending"], (len(frames), queue)
    assert frames and frames[0].raw["E2E_Counter"] == 0, "the run's first value"
    assert all(frame.e2e is not None and frame.e2e.crc_ok for frame in frames)
    if gaps_exact:
        steps = [(b.raw["E2E_Counter"] - a.raw["E2E_Counter"]) % e2e.COUNTER_MODULUS
                 for a, b in zip(frames, frames[1:])]
        assert 0 not in steps, "a value repeated"
        assert sum(step - 1 for step in steps) == queue["superseded"], (steps, queue)
        for frame, step in zip(frames, [1, *steps]):
            assert frame.e2e.status == (e2e.Status.OK if step == 1
                                        else e2e.Status.WRONGSEQUENCE), frame
    return frames


FAULT_LINE = re.compile(r" app: fault (\w+) (?:active \(DTC 0x([0-9A-F]{6})\)|(no longer active))$")


def fault_lines(log: str) -> list[tuple[str, bool]]:
    """The node's log lines about faults that became active (True) or stopped being active
    (False), in their order, each with the DTC number the table gives."""
    events = []
    for line in log.splitlines():
        match = FAULT_LINE.search(line)
        if match:
            name, number, gone = match.groups()
            if not gone:
                assert int(number, 16) == SPEC[name].number, line
            events.append((name, not gone))
    return events


STAMP = re.compile(r"^\[\s*(\d+)\.(\d{6})\] ")


def stamped_fault_lines(log: str) -> list[tuple[int, str, bool]]:
    """fault_lines() with the time of each line: CLOCK_MONOTONIC in ns, as the log stamps it
    (plat_time_ns(), to the microsecond)."""
    events = []
    for line in log.splitlines():
        if FAULT_LINE.search(line):
            stamp = STAMP.match(line)
            (name, active), = fault_lines(line)
            events.append((int(stamp.group(1)) * 10**9 + int(stamp.group(2)) * 1000,
                           name, active))
    return events


def transitions(frames: list[DecodedFrame], initial: int = 0) -> list[tuple[str, bool]]:
    """The changes of FaultActive from frame to frame, from `initial` before the first, in the
    order the node logs them (by fault number within one period)."""
    events = []
    before = initial
    for frame in frames:
        after = frame.raw["FaultActive"]
        events += [(NAME[n], bool(after >> n & 1)) for n in range(len(NAME))
                   if (before ^ after) >> n & 1]
        before = after
    return events


def pack_statuses(recorder: Recorder, first: Heard, last: Heard) -> list[DecodedFrame]:
    """The BMS_PackStatus frames heard between two frames."""
    return [h.frame for h in recorder.frames(PACK_STATUS, first.index + 1, last.index)]


def ms(ns: int) -> str:
    return f"{ns / 1e6:.1f} ms"


# ---------------------------------------------------------------------- the sensor faults


@dataclass(frozen=True)
class Case:
    """A fault of the pack or its measurement as the bench applies it, and the period of the
    evidence for it: a reading of the group the fault is in, or a sample."""

    apply: Callable[[SimControl], dict]
    remove: Callable[[SimControl], dict]
    evidence_ms: int


CASES = {
    "CELL_OVERVOLTAGE": Case(lambda s: s.sensor_set("cell3", 4300),
                             lambda s: s.sensor_clear("cell3"), 20),
    "CELL_UNDERVOLTAGE": Case(lambda s: s.sensor_set("cell6", 2400),
                              lambda s: s.sensor_clear("cell6"), 20),
    "OVERTEMPERATURE": Case(lambda s: s.sensor_set("temp1", 700),        # 70.0 degC
                            lambda s: s.sensor_clear("temp1"), 100),
    "UNDERTEMPERATURE": Case(lambda s: s.sensor_set("temp2", -300),      # -30.0 degC
                             lambda s: s.sensor_clear("temp2"), 100),
    "OVERCURRENT_DISCHARGE": Case(lambda s: s.sensor_set("current", 280000),
                                  lambda s: s.sensor_clear("current"), 10),
    "OVERCURRENT_CHARGE": Case(lambda s: s.sensor_set("current", -150000),
                               lambda s: s.sensor_clear("current"), 10),
    "SENSOR_CHANNEL_INVALID": Case(lambda s: s.sensor_open("cell5"),
                                   lambda s: s.sensor_clear("cell5"), 20),
    "FRONT_END_NO_ANSWER": Case(lambda s: s.sensor_offline(True),
                                lambda s: s.sensor_offline(False), 10),
}


def evidence_to_fail(name: str) -> int:
    """Pieces of evidence from a passed test to a failed one: the whole span of the counter,
    or one for a fault the first piece decides."""
    spec = SPEC[name]
    return 1 if spec.failed == 1 else spec.failed - spec.passed


def evidence_to_heal(name: str) -> int:
    spec = SPEC[name]
    return 1 if spec.passed == -1 else spec.failed - spec.passed


#: Every sensor fault on the UDP bus (see the module's docstring).
UDP_ONLY = pytest.mark.parametrize("can_spec", ["udp"], indirect=True)


@UDP_ONLY
@pytest.mark.parametrize("fault", sorted(CASES))
def test_a_fault_applied_from_outside_is_its_dtc_while_it_lasts_and_stays_confirmed(
        open_link, fault):
    """The fault applied through simulation control: BMS_Fault has its DTC, and only its DTC,
    active and confirmed and as the most recent from one frame on, for as long as the fault
    lasts, with PackFaultActive and PackState FAULT; removed, the DTC is no longer active but
    stays confirmed (and so does the warning indicator) until a clear. The statistics give its
    status byte: testFailed cleared, every other bit of a failed test kept, the debounce counter
    at its passed threshold; no other DTC failed. Every change is a line of the log and a change
    between two BMS_Fault frames, one frame per health period."""
    case, spec, mask = CASES[fault], SPEC[fault], bit(fault)
    link = open_link()
    recorder = Recorder(link.bus)
    try:
        with link.node.sim() as sim:
            recorder.until(FAULT)                    # the health task's first period
            case.apply(sim)
            active = recorder.until(FAULT, lambda f: f.raw["FaultActive"] != 0)
            lasting = [active, *recorder.next(FAULT, active, 3)]
            case.remove(sim)
            gone = recorder.until(FAULT, lambda f: f.raw["FaultActive"] == 0,
                                  after=lasting[-1].index)
            after = [gone, *recorder.next(FAULT, gone, 3)]
        assert link.node.stop() == EXIT_OK
        recorder.quiet()
    finally:
        recorder.stop()
    stats = link.node.stats()

    for h in recorder.frames(FAULT, 0, active.index):
        assert (h.frame.raw["FaultActive"], h.frame.raw["FaultConfirmed"]) == (0, 0), h.frame
        assert h.frame.signals["FaultMostRecent"] == "NONE", h.frame
    lasted = recorder.frames(FAULT, active.index, gone.index)
    for h in lasted:
        assert (h.frame.raw["FaultActive"], h.frame.raw["FaultConfirmed"]) == (mask, mask), h.frame
        assert h.frame.signals["FaultMostRecent"] == fault
        assert h.frame.raw["FaultWarning"] == spec.warning
    for h in recorder.frames(FAULT, gone.index):
        assert (h.frame.raw["FaultActive"], h.frame.raw["FaultConfirmed"]) == (0, mask), h.frame
        assert h.frame.signals["FaultMostRecent"] == fault
        assert h.frame.raw["FaultWarning"] == spec.warning     # until a clear
    while_active = pack_statuses(recorder, lasting[1], lasting[3])
    assert while_active
    for frame in while_active:
        assert (frame.raw["PackFaultActive"], frame.signals["PackState"]) == (1, "FAULT"), frame
    for frame in pack_statuses(recorder, after[2], after[3]):
        assert (frame.raw["PackFaultActive"], frame.signals["PackState"]) == (0, "NORMAL"), frame
    print(f"{fault}: active in {len(lasted)} BMS_Fault frames, {len(while_active)} "
          f"BMS_PackStatus frames checked while it lasted")

    dtcs = check_dtcs(stats, {fault: healed(fault)})
    assert (dtcs[fault]["counter"], dtcs[fault]["fdc"]) == (spec.passed, -128)
    assert stats["faults"]["most_recent"] == fault
    frames = check_fault_frames(recorder, 0, None, stats)
    lines = fault_lines(link.node.log())
    assert lines == [(fault, True), (fault, False)], lines
    if stats["telemetry"]["messages"][FAULT]["queue"]["superseded"] == 0:
        assert transitions(frames) == lines


@UDP_ONLY
@pytest.mark.parametrize("fault", sorted(CASES))
def test_a_fault_still_there_after_a_clear_comes_back_as_its_dtc(open_link, fault):
    """A tester clears the DTCs (CmdClearFaultsSeq changes) while the fault is there: the
    frame of the clear holds nothing from before the request, so it is confirmed there only if
    it is active again, from evidence after the request; the fault comes back as soon as its
    monitor has debounced it again, in the frame of the clear for a fault the first piece of
    evidence decides. The run stopped while the fault is active: its status byte is that of a
    failed test in this run, the counter at the failed threshold, and the four reset faults,
    judged only at the start of a run, stay cleared."""
    case, spec, mask = CASES[fault], SPEC[fault], bit(fault)
    link = open_link()
    recorder = Recorder(link.bus)
    tester = CommandTester(link.spec, seq=7)
    try:
        with link.node.sim() as sim:
            recorder.until(FAULT)
            case.apply(sim)
            active = recorder.until(FAULT, lambda f: f.raw["FaultActive"] & mask)
            tester.seq = 8
            cleared = recorder.until(FAULT, lambda f: f.raw["FaultClearCount"] == 1,
                                     after=active.index)
            back = recorder.until(FAULT, lambda f: f.raw["FaultActive"] & mask,
                                  after=cleared.index)
            recorder.next(FAULT, back)
        tester.close()
        assert link.node.stop() == EXIT_OK
        recorder.quiet()
    finally:
        tester.close()
        recorder.stop()
    stats = link.node.stats()

    # The frame of the clear: nothing from before the request; the fault itself only if the
    # evidence after the request made it active again.
    raw = cleared.frame.raw
    again = raw["FaultActive"]
    assert again in (0, mask), raw
    assert (raw["FaultConfirmed"], raw["FaultWarning"]) == (again, bool(again) and spec.warning)
    assert cleared.frame.signals["FaultMostRecent"] == (fault if again else "NONE")
    for h in recorder.frames(FAULT, cleared.index, back.index):
        assert (h.frame.raw["FaultActive"], h.frame.raw["FaultConfirmed"]) == (0, 0), h.frame
    raw = back.frame.raw
    assert (raw["FaultActive"], raw["FaultConfirmed"], raw["FaultClearCount"]) == (mask, mask, 1)
    assert back.frame.signals["FaultMostRecent"] == fault
    assert raw["FaultWarning"] == spec.warning
    print(f"{fault}: active again in the frame of the clear: {bool(again)}")

    assert (stats["faults"]["clears"], stats["faults"]["clear_requests_seen"]) == (1, 1)
    assert stats["command"]["clear_requests"] == 1
    dtcs = check_dtcs(stats, {fault: failing(fault)})
    assert (dtcs[fault]["counter"], dtcs[fault]["fdc"]) == (spec.failed, 127)
    for name in SPEC:
        if name.startswith("RESET_"):
            assert dtcs[name]["status"] == AFTER_CLEAR, dtcs[name]
    frames = check_fault_frames(recorder, 0, None, stats)
    lines = fault_lines(link.node.log())
    assert lines == [(fault, True), (fault, False), (fault, True)], lines
    assert link.node.log().count("DTCs cleared: CmdClearFaultsSeq changed (clear 1)") == 1
    if stats["telemetry"]["messages"][FAULT]["queue"]["superseded"] == 0:
        # Active again in the frame of the clear, the fault never left FaultActive: the clear
        # and its return are two lines of one period and no change between two frames.
        assert transitions(frames) == (lines[:1] if again else lines)


def test_an_over_voltage_is_a_dtc_until_it_heals_and_a_clear_removes_its_record(
        open_link, can_spec):
    """The same on both transports, for one fault of the pack: a cell held at 4300 mV, valid
    and beyond the 4250 mV limit, is the over-voltage DTC; back at the model's value, the fault
    heals (FaultActive) and its record stays (FaultConfirmed) until a tester changes
    CmdClearFaultsSeq."""
    link = open_link()
    recorder = Recorder(link.bus)
    tester = None
    try:
        with link.node.sim() as sim:
            recorder.until(FAULT)                       # the health task's first period
            start = recorder.mark()
            t_set = int(sim.sensor_set("cell3", 4300)["t_ns"])
            active = recorder.until(FAULT, lambda f: f.raw["FaultActive"]
                                    & bit("CELL_OVERVOLTAGE"), after=start)
            assert active.frame.signals["FaultMostRecent"] == "CELL_OVERVOLTAGE"
            assert active.frame.raw["FaultConfirmed"] == bit("CELL_OVERVOLTAGE")
            assert active.frame.raw["FaultWarning"] == 1
            print(f"cell3 4300 mV to BMS_Fault with the over-voltage: {ms(active.at_ns - t_set)}")

            status = recorder.until(PACK_STATUS, lambda f: f.raw["PackFaultActive"] == 1,
                                    after=start)
            assert status.frame.signals["PackState"] == "FAULT"
            assert status.frame.raw["PackVoltageValid"] == 1     # no channel is invalid
            print(f"cell3 4300 mV to PackFaultActive: {ms(status.at_ns - t_set)}")

            mark = recorder.mark()
            t_clear = int(sim.sensor_clear("cell3")["t_ns"])
            gone = recorder.until(FAULT, lambda f: f.raw["FaultActive"] == 0, after=mark)
            assert gone.frame.raw["FaultConfirmed"] == bit("CELL_OVERVOLTAGE")
            assert gone.frame.raw["FaultWarning"] == 1
            print(f"cell3 back to BMS_Fault without the over-voltage: {ms(gone.at_ns - t_clear)}")
            normal = recorder.until(PACK_STATUS, lambda f: f.signals["PackState"] == "NORMAL",
                                    after=mark)
            assert normal.frame.raw["PackFaultActive"] == 0

            # The tester's first command acted on sets the reference, the change is the clear.
            tester = CommandTester(link.spec, seq=1)
            recorder.next(FAULT, recorder.until(FAULT, after=recorder.mark()), 2)  # acted on
            tester.seq = 2
            cleared = recorder.until(FAULT, lambda f: f.raw["FaultClearCount"] == 1)
            assert cleared.frame.raw["FaultConfirmed"] == 0
            assert cleared.frame.signals["FaultMostRecent"] == "NONE"
            assert cleared.frame.raw["FaultWarning"] == 0
        tester.close()
        assert link.node.stop() == EXIT_OK
        recorder.quiet()
    finally:
        if tester is not None:
            tester.close()
        recorder.stop()

    stats = link.node.stats()
    assert stats["command"]["clear_requests"] == 1 and stats["faults"]["clears"] == 1
    check_dtcs(stats, {})                # after the clear: no DTC that failed
    check_fault_frames(recorder, 0, None, stats)
    assert fault_lines(link.node.log()) == [("CELL_OVERVOLTAGE", True),
                                            ("CELL_OVERVOLTAGE", False)]


@UDP_ONLY
def test_bms_fault_goes_out_once_per_health_period_and_carries_every_change(open_link):
    """Faults that overlap, one change at a time, then a clear. One BMS_Fault value per health
    period, and every change of the DTCs in the frame of the period that made it: the changes
    between frames are exactly the node's log lines about the faults, in their order. In every
    frame every active fault is confirmed, a confirmed fault stays confirmed until the clear,
    the most recent fault is the one that went active last, and the warning indicator is on
    while a fault of the pack or its measurement is confirmed; PackFaultActive follows the
    active faults."""
    ov, na, ot = bit("CELL_OVERVOLTAGE"), bit("FRONT_END_NO_ANSWER"), bit("OVERTEMPERATURE")
    steps = (
        # While the front end is off line no cell reading is valid: the over-voltage is not
        # judged and stays active; back on line, cell3 is still beyond the limit.
        (lambda s: s.sensor_set("cell3", 4300), ov),
        (lambda s: s.sensor_offline(True), ov | na),
        (lambda s: s.sensor_offline(False), ov),
        (lambda s: s.sensor_set("temp1", 700), ov | ot),
        (lambda s: s.sensor_clear("cell3"), ot),
        (lambda s: s.sensor_clear("temp1"), 0),
    )
    link = open_link()
    recorder = Recorder(link.bus)
    tester = CommandTester(link.spec, seq=3)
    try:
        with link.node.sim() as sim:
            recorder.until(FAULT)
            for act, wanted in steps:
                act(sim)
                recorder.until(FAULT, lambda f: f.raw["FaultActive"] == wanted,
                               after=recorder.mark())
            tester.seq = 4
            cleared = recorder.until(FAULT, lambda f: f.raw["FaultClearCount"] == 1)
            recorder.next(FAULT, cleared)
        tester.close()
        assert link.node.stop() == EXIT_OK
        recorder.quiet()
    finally:
        tester.close()
        recorder.stop()
    stats = link.node.stats()

    frames = check_fault_frames(recorder, 0, None, stats)
    lines = fault_lines(link.node.log())
    assert lines == [("CELL_OVERVOLTAGE", True), ("FRONT_END_NO_ANSWER", True),
                     ("FRONT_END_NO_ANSWER", False), ("OVERTEMPERATURE", True),
                     ("CELL_OVERVOLTAGE", False), ("OVERTEMPERATURE", False)], lines
    exact = stats["telemetry"]["messages"][FAULT]["queue"]["superseded"] == 0 \
        and stats["log_lines_dropped"] == 0
    print(f"{len(frames)} BMS_Fault frames, {stats['faults']['health_cycles']} health periods; "
          f"every value heard: {exact}")
    if exact:
        assert transitions(frames) == lines

    pack_faults = sum(bit(name) for name, spec in SPEC.items() if spec.warning)
    confirmed, most_recent, before, clears = 0, "NONE", 0, 0
    for frame in frames:
        raw = frame.raw
        if raw["FaultClearCount"] != clears:
            clears, confirmed, most_recent = raw["FaultClearCount"], 0, "NONE"
        rose = raw["FaultActive"] & ~before
        if rose:
            assert rose & (rose - 1) == 0, frame          # one fault at a time
            most_recent = NAME[rose.bit_length() - 1]
        confirmed |= raw["FaultActive"]
        assert raw["FaultConfirmed"] == confirmed, frame
        assert frame.signals["FaultMostRecent"] == most_recent, frame
        assert raw["FaultWarning"] == (confirmed & pack_faults != 0), frame
        before = raw["FaultActive"]
    assert clears == 1 and frames[-1].raw["FaultConfirmed"] == 0

    # A BMS_PackStatus says what the summary of the health task's last period said, and that
    # summary is in the BMS_Fault frame of that period: where three frames in a row agree on
    # whether a fault is active, the status frames between the second and the third agree too.
    shown = recorder.frames(FAULT)
    checked = 0
    for a, b, c in zip(shown, shown[1:], shown[2:]):
        states = {h.frame.raw["FaultActive"] != 0 for h in (a, b, c)}
        if len(states) == 1:
            (state,) = states
            for frame in pack_statuses(recorder, b, c):
                assert frame.raw["PackFaultActive"] == int(state), frame
                assert frame.signals["PackState"] == "FAULT" or not state, frame
                checked += 1
    assert checked > 0
    check_dtcs(stats, {})


def miss_one_read(sim: SimControl) -> int:
    """Take the front end off line for as short as the bench can make it: on, until the node
    has taken a read that the front end did not answer (sensor.status, asked again and again
    without a pause), then off. Returns the reads that went unanswered, one or a few; the node
    counts the same reads in its statistics."""
    before = sim.sensor_status()["unanswered"]
    sim.sensor_offline(True)
    deadline = time.monotonic() + 5.0
    while sim.sensor_status()["unanswered"] == before:
        assert time.monotonic() < deadline, "the node took no reading while the front end was off"
    sim.sensor_offline(False)
    return sim.sensor_status()["unanswered"] - before


@UDP_ONLY
def test_a_fault_that_comes_and_goes_within_a_health_period_is_reported_all_the_same(open_link):
    """FRONT_END_NO_ANSWER, which one piece of evidence decides either way, for as short as the
    bench can make it: the front end goes off line just after a BMS_Fault frame and back as soon
    as the node has missed a read, so that the DTC fails and passes again within one health
    period. The frame of that period has it active all the same, a BMS_PackStatus after it says
    PackFaultActive and FAULT, the log has the pair of lines, and no frame has a fault confirmed
    that no frame so far had active. The statistics have the healed DTC and the reads missed."""
    mask = bit("FRONT_END_NO_ANSWER")
    link = open_link()
    recorder = Recorder(link.bus)
    try:
        with link.node.sim() as sim:
            recorder.next(FAULT, recorder.until(FAULT))
            start = recorder.until(FAULT, after=recorder.mark())        # just after a frame
            missed = miss_one_read(sim)
            confirmed = recorder.until(FAULT, lambda f: f.raw["FaultConfirmed"] & mask,
                                       after=start.index)
            # The first frame without it, whenever the front end came back on line.
            gone = recorder.until(FAULT, lambda f: (f.raw["FaultActive"] & mask) == 0,
                                  after=confirmed.index)
            later = recorder.next(FAULT, gone, 2)
        assert link.node.stop() == EXIT_OK
        recorder.quiet()
    finally:
        recorder.stop()
    stats = link.node.stats()

    frames = check_fault_frames(recorder, 0, None, stats)
    shown = [h for h in recorder.frames(FAULT) if h.frame.raw["FaultActive"] & mask]
    print(f"{missed} reads unanswered ({stats['sensor']['unanswered']}); FRONT_END_NO_ANSWER "
          f"active in {len(shown)} BMS_Fault frames")
    assert shown, "the fault came and went without a BMS_Fault frame that has it active"
    seen = 0
    for frame in frames:
        seen |= frame.raw["FaultActive"]
        assert frame.raw["FaultConfirmed"] & ~seen == 0, ("confirmed, never active", frame)
    after_it = pack_statuses(recorder, shown[0], recorder.next(FAULT, shown[-1], 2)[1])
    assert any((frame.raw["PackFaultActive"], frame.signals["PackState"]) == (1, "FAULT")
               for frame in after_it), after_it
    for frame in pack_statuses(recorder, later[0], later[1]):
        assert (frame.raw["PackFaultActive"], frame.signals["PackState"]) == (0, "NORMAL"), frame
    assert fault_lines(link.node.log()) == [("FRONT_END_NO_ANSWER", True),
                                            ("FRONT_END_NO_ANSWER", False)]
    dtcs = check_dtcs(stats, {"FRONT_END_NO_ANSWER": healed("FRONT_END_NO_ANSWER")})
    assert dtcs["FRONT_END_NO_ANSWER"]["counter"] == SPEC["FRONT_END_NO_ANSWER"].passed
    assert sum(stats["sensor"]["unanswered"].values()) == missed


@UDP_ONLY
def test_a_failure_detected_after_a_clear_request_survives_the_clear(open_link):
    """A clear erases what came before its request and nothing after it. Just after a
    BMS_Fault frame: cell5 goes open until a sample with it has been processed (its
    BMS_PackStatus says so) and comes back, so that SENSOR_CHANNEL_INVALID fails and heals
    before the request; then the tester changes CmdClearFaultsSeq and sends a ping behind the
    command, and once its BMS_Pong is heard the node has counted the request; then the front end
    misses a read (as short an outage as the bench can make). As a rule all of it falls into
    the health period of the clear. The frame of the clear holds nothing of the channel fault;
    the missed read is FRONT_END_NO_ANSWER, confirmed from the first frame that has it on (as a
    rule the frame of the clear; when the host delays the test so much that the read is missed
    in a later period, the test shows less, and says so in its output, but its verdict does not
    change), and the statistics have its record."""
    invalid, no_answer = bit("SENSOR_CHANNEL_INVALID"), bit("FRONT_END_NO_ANSWER")
    link = open_link()
    recorder = Recorder(link.bus)
    tester = CommandTester(link.spec, seq=7)
    try:
        with link.node.sim() as sim:
            recorder.next(FAULT, recorder.until(FAULT), 2)            # commands acted on
            start = recorder.until(FAULT, after=recorder.mark())      # just after a frame
            sim.sensor_open("cell5")
            recorder.until(PACK_STATUS, lambda f: f.raw["PackVoltageValid"] == 0,
                           after=start.index)
            sim.sensor_clear("cell5")
            request = recorder.mark()
            tester.change_now(8, ping=b"clear-08")
            recorder.until("BMS_Pong", after=request)
            missed = miss_one_read(sim)
            cleared = recorder.until(FAULT, lambda f: f.raw["FaultClearCount"] == 1,
                                     after=start.index)
            try:
                kept = recorder.until(FAULT, lambda f: f.raw["FaultConfirmed"] & no_answer,
                                      after=cleared.index)
            except AssertionError:
                raise AssertionError("the failure after the request was erased") from None
            recorder.next(FAULT, kept, 2)
        tester.close()
        assert link.node.stop() == EXIT_OK
        recorder.quiet()
    finally:
        tester.close()
        recorder.stop()
    stats = link.node.stats()

    raw = cleared.frame.raw
    assert (raw["FaultActive"] | raw["FaultConfirmed"]) & invalid == 0, raw
    assert raw["FaultConfirmed"] & ~no_answer == 0, raw
    before = [h.frame.raw["FaultConfirmed"] for h in recorder.frames(FAULT, start.index + 1,
                                                                       cleared.index)]
    print(f"{missed} reads unanswered after the request; the channel fault in a frame before "
          f"the clear's: {any(c & invalid for c in before)}; the missed read in the frame of "
          f"the clear: {kept.index == cleared.index} (health periods after it: "
          f"{len(recorder.frames(FAULT, cleared.index + 1, kept.index + 1))})")
    for h in recorder.frames(FAULT, kept.index):
        assert h.frame.raw["FaultConfirmed"] == no_answer, h.frame
    assert stats["command"]["clear_requests"] == 1 and stats["faults"]["clears"] == 1
    dtcs = check_dtcs(stats, {"FRONT_END_NO_ANSWER": healed("FRONT_END_NO_ANSWER")})
    assert dtcs["SENSOR_CHANNEL_INVALID"]["status"] & FAILED_BITS == 0
    check_fault_frames(recorder, 0, None, stats)
    assert fault_lines(link.node.log()) == [
        ("SENSOR_CHANNEL_INVALID", True), ("SENSOR_CHANNEL_INVALID", False),
        ("FRONT_END_NO_ANSWER", True), ("FRONT_END_NO_ANSWER", False)]


# ------------------------------------------------------------------------- the CAN bus


def short_until_bus_off(sim: SimControl) -> None:
    """Short the bus until the node has gone bus off, and hold the short for two health periods
    more, so that the health task judges the controller while it is off the bus."""
    sim.can_fault("short")
    deadline = time.monotonic() + 10
    while sim.can_status()["bus_off"] < 1:
        assert time.monotonic() < deadline, "the node did not go bus off"
        time.sleep(0.01)
    time.sleep(0.25)                  # how long the bench holds the fault, not a wait for it
    sim.can_fault("none")


def test_a_shorted_bus_is_a_bus_off_and_an_error_passive_dtc_until_the_bus_is_good_again(
        open_link):
    """A shorted bus takes the node bus off, through error passive (lib/can). Off the bus it can
    report nothing; once the short is gone and it is back, BMS_Fault says both happened, with
    PackFaultActive and FAULT while they are active. Error passive heals after 10 health
    periods error active, bus off after 50 without a bus-off (5 s); both stay confirmed."""
    off, passive = bit("CAN_BUS_OFF"), bit("CAN_ERROR_PASSIVE")
    link = open_link()
    recorder = Recorder(link.bus)
    try:
        with link.node.sim() as sim:
            recorder.until(FAULT)
            start = recorder.mark()
            short_until_bus_off(sim)
            back = recorder.until(FAULT, lambda f: f.raw["FaultActive"] & off, after=start)
            passive_gone = recorder.until(FAULT, lambda f: not f.raw["FaultActive"] & passive,
                                          after=back.index)
            gone = recorder.until(FAULT, lambda f: f.raw["FaultActive"] == 0,
                                  after=back.index, timeout=15)
            after = recorder.next(FAULT, gone, 3)
        assert link.node.stop() == EXIT_OK
        recorder.quiet()
    finally:
        recorder.stop()
    stats = link.node.stats()

    for h in recorder.frames(FAULT, 0, back.index):
        assert h.frame.raw["FaultActive"] & ~(off | passive) == 0, h.frame
    for h in recorder.frames(FAULT, back.index, gone.index):
        raw = h.frame.raw
        assert raw["FaultActive"] & off and raw["FaultActive"] & ~(off | passive) == 0, h.frame
        assert raw["FaultConfirmed"] == off | passive, h.frame
        assert h.frame.signals["FaultMostRecent"] in ("CAN_BUS_OFF", "CAN_ERROR_PASSIVE")
        assert raw["FaultWarning"] == 0                    # not a fault of the pack
    assert back.index < passive_gone.index < gone.index
    for h in recorder.frames(FAULT, gone.index):
        assert (h.frame.raw["FaultActive"], h.frame.raw["FaultConfirmed"]) == (0, off | passive)
    while_active = pack_statuses(recorder, recorder.next(FAULT, back)[0], gone)
    assert while_active
    for frame in while_active:
        assert (frame.raw["PackFaultActive"], frame.signals["PackState"]) == (1, "FAULT"), frame
    for frame in pack_statuses(recorder, after[1], after[2]):
        assert (frame.raw["PackFaultActive"], frame.signals["PackState"]) == (0, "NORMAL"), frame
    print(f"back on the bus: FaultActive {back.frame.raw['FaultActive']:#06x}; error passive "
          f"healed {ms(passive_gone.at_ns - back.at_ns)} and bus off "
          f"{ms(gone.at_ns - back.at_ns)} after the first frame back")

    assert stats["can"]["queue"]["bus_off_count"] >= 1
    dtcs = check_dtcs(stats, {"CAN_BUS_OFF": healed("CAN_BUS_OFF"),
                              "CAN_ERROR_PASSIVE": healed("CAN_ERROR_PASSIVE")})
    assert dtcs["CAN_BUS_OFF"]["counter"] == -50 and dtcs["CAN_ERROR_PASSIVE"]["counter"] == -10
    check_fault_frames(recorder, 0, None, stats, gaps_exact=False)   # replaced while bus off
    lines = fault_lines(link.node.log())
    assert sorted(lines) == sorted([("CAN_BUS_OFF", True), ("CAN_ERROR_PASSIVE", True),
                                    ("CAN_ERROR_PASSIVE", False), ("CAN_BUS_OFF", False)]), lines


def test_a_node_stopped_soon_after_a_bus_off_has_the_dtc_active(open_link):
    """The status bytes while the bus-off DTC is still active: stopped as soon as the node is
    back on the bus, bus off has a failed test that still fails; error passive the same, or
    healed if 10 health periods error active have passed by the stop."""
    link = open_link()
    recorder = Recorder(link.bus)
    try:
        with link.node.sim() as sim:
            recorder.until(FAULT)
            start = recorder.mark()
            short_until_bus_off(sim)
            back = recorder.until(FAULT, lambda f: f.raw["FaultActive"] & bit("CAN_BUS_OFF"),
                                  after=start)
        assert link.node.stop() == EXIT_OK
        recorder.quiet()
    finally:
        recorder.stop()
    stats = link.node.stats()
    assert back.frame.raw["FaultConfirmed"] == bit("CAN_BUS_OFF") | bit("CAN_ERROR_PASSIVE")
    dtcs = check_dtcs(stats, {"CAN_BUS_OFF": failing("CAN_BUS_OFF"),
                              "CAN_ERROR_PASSIVE": {failing("CAN_ERROR_PASSIVE"),
                                                    healed("CAN_ERROR_PASSIVE")}})
    assert dtcs["CAN_BUS_OFF"]["counter"] > SPEC["CAN_BUS_OFF"].passed
    check_fault_frames(recorder, 0, None, stats, gaps_exact=False)


# ------------------------------------------------------------- the watchdogs and the reset

LAYOUT = struct.Struct("<4sHHII4I")    # the retained cell (platform/include/plat_retained.h)


def read_cell(path: Path) -> tuple[int, int, tuple[int, ...]]:
    """The reason, the count of resets and the reserved words of a valid cell."""
    data = path.read_bytes()
    assert len(data) == LAYOUT.size + 4
    (crc,) = struct.unpack("<I", data[LAYOUT.size:])
    assert crc == zlib.crc32(data[:LAYOUT.size]), "the cell's CRC"
    magic, version, size, reason, resets, *reserved = LAYOUT.unpack(data[:LAYOUT.size])
    assert (magic, version, size) == (b"FCNR", 1, LAYOUT.size + 4)
    return reason, resets, tuple(reserved)


def cell_image(reason: int) -> bytes:
    """A valid cell that records one reset with `reason`, as plat_reset() leaves it."""
    head = LAYOUT.pack(b"FCNR", 1, LAYOUT.size + 4, reason, 1, 0, 0, 0, 0)
    return head + struct.pack("<I", zlib.crc32(head))


#: The reset reasons a cell can record (plat_reset_reason_t) and the fault each one is
#: (app/fault.h): a reset because of a fault is the DTC of that fault; the others are none.
REASON = {"REQUESTED": (10, None), "WATCHDOG": (11, "RESET_WATCHDOG"), "IMAGE_FAULT": (13, None),
          "ASSERT": (20, "RESET_ASSERT"), "STACK_OVERFLOW": (21, "RESET_STACK_OVERFLOW"),
          "MALLOC_FAILED": (22, "RESET_MALLOC_FAILED")}


@pytest.mark.parametrize("cause", sorted(REASON))
def test_the_cause_of_a_start_is_reported_from_the_first_frames_on(open_link, workdir, cause):
    """A cell that records a reset, written here as plat_reset() writes it: a reset because of a
    fault is the active, confirmed DTC of that fault in the first BMS_Fault, and every
    BMS_PackStatus from the first one on says PackFaultActive and FAULT; a reset for another
    reason is no DTC. BMS_NodeInfo carries the cause either way."""
    code, fault = REASON[cause]
    path = workdir / "cell.bin"
    path.write_bytes(cell_image(code))
    link = open_link("--retained-file", str(path))
    recorder = Recorder(link.bus)
    try:
        first = recorder.until(FAULT).frame
        info = recorder.until(NODE_INFO).frame
        statuses = [frame for frame in (Decoder(DB).decode(m) for m in link.before_first_status)
                    if frame is not None and frame.name == PACK_STATUS]
        statuses += [h.frame for h in recorder.frames(PACK_STATUS)]
        assert link.node.stop() == EXIT_OK
        recorder.quiet()
    finally:
        recorder.stop()

    expected = bit(fault) if fault else 0
    assert (first.raw["FaultActive"], first.raw["FaultConfirmed"]) == (expected, expected)
    assert first.signals["FaultMostRecent"] == (fault or "NONE")
    assert first.raw["FaultWarning"] == 0                # the reset faults request no indicator
    assert info.signals["ResetCause"] == cause
    # The first of the run (its E2E counter starts at 0) and every one until BMS_NodeInfo.
    assert statuses and statuses[0].raw["E2E_Counter"] == 0, statuses[:1]
    for frame in statuses:
        assert frame.raw["PackFaultActive"] == (1 if fault else 0), frame
        assert (frame.signals["PackState"] == "FAULT") == bool(fault), frame
    stats = link.node.stats()
    assert stats["reset"]["cause"] == cause and stats["faults"]["reset_cause"] == cause
    dtcs = check_dtcs(stats, {fault: failing(fault)} if fault else {})
    for name in SPEC:
        if name.startswith("RESET_"):
            assert dtcs[name]["status"] == (failing(name) if name == fault else 0x00), dtcs[name]
    check_fault_frames(recorder, 0, None, stats)
    assert fault_lines(link.node.log()) == []      # active from the start: no change to log


#: How long a test waits for the watchdog's reset after the hang: the node's bound is 0.8 s
#: (app/app.c, SUPERVISION); the rest is room for a slow host. Not a timing tolerance: it only
#: bounds the wait (the timing tests below measure the time).
RESET_WAIT_S = 10.0
FATAL_LINE = re.compile(r"FATAL watchdog: not kicked for (\d+) ms \(timeout 500 ms\)")


def test_a_hanging_sensor_task_ends_in_a_watchdog_reset_and_nothing_else(
        start_node, restart_node, workdir, can_spec):
    """sensor.hang on: the sensor task waits inside its read for ever. Its samples stop
    (SAMPLE_TIMEOUT), the supervision finds the sensor task gone (SUPERVISION_EXPIRED), both go
    out in BMS_Fault and nothing else does; the health task stops kicking and the hardware
    watchdog resets the node: exit status 11, one FATAL line, WATCHDOG in the retained cell.
    Started again on the same cell, as a board starts after a reset, the node reports the
    watchdog reset as the cause (BMS_NodeInfo.ResetCause) and as the active, confirmed DTC
    RESET_WATCHDOG from its first frames on, until a tester clears it. Started once more after
    the bench has cut the power (removed the cell), it reports a power-on and no DTC."""
    cell = workdir / "cell.bin"
    cell.unlink(missing_ok=True)
    bus = open_bus(can_spec)
    recorder = Recorder(bus)
    tester = None
    expected_before_reset = bit("SAMPLE_TIMEOUT") | bit("SUPERVISION_EXPIRED")
    try:
        node = start_node(can=can_spec, retained_file=cell)
        recorder.until(FAULT)
        with node.sim() as sim:
            sim.sensor_hang("on")
            # Stuck once a read waits; from then on the front end's counts stay as they are.
            # (5 s bounds the wait; it is not a timing tolerance.)
            deadline = time.monotonic() + 5.0
            while sim.sensor_status()["hung"] != 1:
                assert time.monotonic() < deadline, "the sensor task did not get stuck"
                time.sleep(0.01)
            reads = sim.sensor_status()
            recorder.until(FAULT, lambda f: f.raw["FaultActive"] & bit("SUPERVISION_EXPIRED"))
        assert node.wait_for_reset(timeout=RESET_WAIT_S) == RESET_WATCHDOG
        first_end = recorder.quiet()

        # The bench starts the board again; the cell says why it stopped.
        again = restart_node(node)
        info = recorder.until(NODE_INFO, after=first_end)
        tester = CommandTester(can_spec, seq=5)
        recorder.next(FAULT, info, 3)                            # commands acted on with 5
        tester.seq = 6
        cleared = recorder.until(FAULT, lambda f: f.raw["FaultClearCount"] == 1, after=first_end)
        normal = recorder.until(PACK_STATUS, lambda f: f.signals["PackState"] == "NORMAL",
                                after=cleared.index)
        tester.close()
        tester = None
        assert again.stop() == EXIT_OK
        second_end = recorder.quiet()
        after_stop = read_cell(cell)

        # The bench cuts the power: the cell is gone.
        cell.unlink()
        third = restart_node(again)
        info3 = recorder.until(NODE_INFO, after=second_end)
        assert third.stop() == EXIT_OK
        third_end = recorder.quiet()
    finally:
        if tester is not None:
            tester.close()
        recorder.stop()
        bus.shutdown()

    # The first run: the two DTCs of the hang and nothing else, then the reset.
    shown = recorder.frames(FAULT, 0, first_end)
    for h in shown:
        assert h.frame.raw["FaultActive"] & ~expected_before_reset == 0, h.frame
        assert h.frame.e2e.crc_ok
    last = shown[-1].frame
    assert (last.raw["FaultActive"], last.raw["FaultConfirmed"]) == \
        (expected_before_reset, expected_before_reset), last
    assert last.signals["FaultMostRecent"] == "SUPERVISION_EXPIRED"
    # The sensor task sent nothing after the period it got stuck in, whenever the hang came.
    # A period sends its heartbeat (PeriodCounter: the period's number, from 1), then reads
    # the current, and the cells and the temperatures in the periods of their messages' cycle
    # times, from the first period on (app/app.c: sensor_task(), acquire() and the asserts on
    # the DBC's cycle times). The front end counts a reading once it no longer hangs
    # (platform/posix/posix_sensor.c). So with n current readings counted, the stuck read is
    # the current reading of period n + 1, unless period n still lacks its cells or
    # temperatures: then the hang came between two reads of period n.
    n = reads["current_reads"]
    period_ms = DB.get_message_by_name(HEARTBEAT).cycle_time
    cells_every = DB.get_message_by_name("BMS_CellVoltages1").cycle_time // period_ms
    temps_every = DB.get_message_by_name("BMS_Temperatures").cycle_time // period_ms
    period_n_complete = (reads["cell_reads"], reads["temp_reads"]) == \
        (-(-n // cells_every), -(-n // temps_every))
    stuck = n + 1 if period_n_complete else n
    numbers = [h.frame.raw["PeriodCounter"] for h in recorder.frames(HEARTBEAT, 0, first_end)]
    assert max(numbers) == stuck, (f"last heartbeats {numbers[-3:]}, stuck in period {stuck} "
                                   f"after {n} current, {reads['cell_reads']} cell and "
                                   f"{reads['temp_reads']} temperature readings")
    # At most one BMS_PackStatus per sample, and only the periods before the stuck one posted
    # a sample.
    assert len(recorder.frames(PACK_STATUS, 0, first_end)) <= stuck - 1
    stderr = node.stderr().strip().splitlines()
    assert len(stderr) == 1 and FATAL_LINE.fullmatch(stderr[0]), stderr
    assert int(FATAL_LINE.fullmatch(stderr[0]).group(1)) >= 500
    log = node.log()
    assert log.count("supervision: sensor expired") == 1
    assert log.count("no sample from the sensor task for 30 ms") == 1
    assert fault_lines(log) == [("SAMPLE_TIMEOUT", True), ("SUPERVISION_EXPIRED", True)]
    assert not node.stats_path.exists()             # a reset writes no statistics
    assert after_stop[1] == 1                       # one reset counted since power-on

    # The second run: started after a watchdog reset.
    first = recorder.until(FAULT, after=first_end).frame
    assert (first.raw["FaultActive"], first.raw["FaultConfirmed"]) == \
        (bit("RESET_WATCHDOG"), bit("RESET_WATCHDOG"))
    assert first.signals["FaultMostRecent"] == "RESET_WATCHDOG" and first.raw["FaultWarning"] == 0
    assert info.frame.signals["ResetCause"] == "WATCHDOG"
    statuses = [h.frame for h in recorder.frames(PACK_STATUS, first_end, info.index)]
    assert statuses and statuses[0].raw["E2E_Counter"] == 0
    for frame in statuses:
        assert (frame.raw["PackFaultActive"], frame.signals["PackState"]) == (1, "FAULT"), frame
    assert cleared.frame.raw["FaultActive"] == 0 and normal.frame.raw["PackFaultActive"] == 0
    for h in recorder.frames(FAULT, cleared.index, second_end):
        assert h.frame.raw["FaultActive"] == 0, h.frame        # judged once per run: stays gone
    stats = again.stats()
    assert stats["reset"] == {"cause": "WATCHDOG", "cause_code": 11, "retained_cell": True,
                              "verdict": "reset", "resets_since_power_on": 1}
    dtcs = check_dtcs(stats, {})
    assert dtcs["RESET_WATCHDOG"]["status"] == AFTER_CLEAR     # cleared, not judged again
    assert stats["faults"]["clears"] == 1 and stats["faults"]["reset_cause"] == "WATCHDOG"
    assert stats["watchdog_manager"]["global_status"] == "OK"
    assert stats["hw_watchdog"]["kicks"] == stats["watchdog_manager"]["kicks"]
    check_fault_frames(recorder, first_end, second_end, stats)
    assert "the last reset was not a power-on: reset cause 11" in again.log()
    assert fault_lines(again.log()) == [("RESET_WATCHDOG", False)]  # the clear
    assert after_stop[0] == 0                       # an orderly stop: no reset recorded

    # The third run, after a power loss.
    first = recorder.until(FAULT, after=second_end).frame
    assert (first.raw["FaultActive"], first.raw["FaultConfirmed"]) == (0, 0)
    assert info3.frame.signals["ResetCause"] == "POWER_ON"
    stats = third.stats()
    assert stats["reset"] == {"cause": "POWER_ON", "cause_code": 0, "retained_cell": True,
                              "verdict": "missing", "resets_since_power_on": 0}
    check_dtcs(stats, {})
    check_fault_frames(recorder, second_end, third_end, stats)
    assert read_cell(cell) == (0, 0, (0, 0, 0, 0))


@UDP_ONLY
def test_without_a_retained_cell_the_start_after_a_watchdog_reset_is_a_power_on(
        start_node, restart_node, can_spec):
    """The same reset without --retained-file: nothing survives it, so the next start is a
    power-on, with no reset DTC."""
    bus = open_bus(can_spec)
    recorder = Recorder(bus)
    try:
        node = start_node(can=can_spec)
        recorder.until(FAULT)
        with node.sim() as sim:
            sim.sensor_hang("on")
        assert node.wait_for_reset(timeout=RESET_WAIT_S) == RESET_WATCHDOG
        end = recorder.quiet()
        again = restart_node(node)
        first = recorder.until(FAULT, after=end).frame
        info = recorder.until(NODE_INFO, after=end).frame
        assert again.stop() == EXIT_OK
    finally:
        recorder.stop()
        bus.shutdown()
    assert (first.raw["FaultActive"], first.raw["FaultConfirmed"]) == (0, 0)
    assert info.signals["ResetCause"] == "POWER_ON"
    stats = again.stats()
    assert stats["reset"]["cause"] == "POWER_ON" and stats["reset"]["verdict"] == "no cell"
    check_dtcs(stats, {})


#: The times from a hang to what it causes, in the node's own terms (app/app.c, SUPERVISION),
#: with h the hang command. The sensor task's last alive indication comes at a, 0 to 10 ms before
#: h. The first health job after it, j1, is 0 to 100 ms later and still sees it; the next three
#: see none: j1 + 100 and j1 + 200 make the entity FAILED, j1 + 300 EXPIRED, the global status
#: STOPPED and the job sends SUPERVISION_EXPIRED and withholds its kick. The hardware watchdog
#: resets 500 ms after the last kick (j1 + 200): at j1 + 700, so 690 to 800 ms after h, and
#: 400 ms after the expiry's frame. The processing task times out 30 ms after the last sample
#: and the next health job sends SAMPLE_TIMEOUT: 20 to 130 ms after h. The upper bounds add the
#: host's latency.
HANG_TO_SAMPLE_TIMEOUT_S = (0.02, 0.16)
HANG_TO_EXPIRY_S = (0.29, 0.43)
HANG_TO_RESET_S = (0.69, 0.85)
EXPIRY_TO_RESET_S = (0.39, 0.45)
#: With `spin` the sensor task keeps the CPU from its next read on, 0 to 10 ms after h: nothing
#: below it runs, the last kick was the health job before that read, so the reset comes 400 to
#: 510 ms after h.
SPIN_TO_RESET_S = (0.40, 0.56)


def within(value_s: float, bounds: tuple[float, float]) -> bool:
    return bounds[0] <= value_s <= bounds[1]


@pytest.mark.timing
def test_a_hanging_sensor_task_is_reset_by_the_watchdog_within_the_documented_time(
        start_node, can_spec):
    """sensor.hang on, timed: SAMPLE_TIMEOUT and SUPERVISION_EXPIRED in BMS_Fault and the reset
    (as the bench sees the process end) within the bounds above."""
    bus = open_bus(can_spec)
    recorder = Recorder(bus)
    try:
        node = start_node(can=can_spec)
        recorder.until(FAULT)
        watch = EndWatch(node)
        with node.sim() as sim:
            reads = sim.sensor_status()["current_reads"]
            hang = recorder.mark()
            t_hang = int(sim.sensor_hang("on")["t_ns"])
            timeout = recorder.until(FAULT, lambda f: f.raw["FaultActive"]
                                     & bit("SAMPLE_TIMEOUT"), after=hang)
            expired = recorder.until(FAULT, lambda f: f.raw["FaultActive"]
                                     & bit("SUPERVISION_EXPIRED"), after=hang)
        assert node.wait_for_reset(timeout=RESET_WAIT_S) == RESET_WATCHDOG
        t_reset = watch.ended_ns()
    finally:
        recorder.stop()
        bus.shutdown()
    to_timeout, to_expiry = timeout.at_ns - t_hang, expired.at_ns - t_hang
    to_reset, expiry_to_reset = t_reset - t_hang, t_reset - expired.at_ns
    print(f"hang (after {reads} current readings) to SAMPLE_TIMEOUT in BMS_Fault "
          f"{ms(to_timeout)}, to SUPERVISION_EXPIRED {ms(to_expiry)}, to the end of the process "
          f"{ms(to_reset)}; from the expiry's frame to the end {ms(expiry_to_reset)}")
    for value, bounds, what in ((to_timeout, HANG_TO_SAMPLE_TIMEOUT_S, "SAMPLE_TIMEOUT"),
                                (to_expiry, HANG_TO_EXPIRY_S, "SUPERVISION_EXPIRED"),
                                (to_reset, HANG_TO_RESET_S, "the reset")):
        check_timing(within(value / 1e9, bounds),
                     f"{what} came {ms(value)} after the hang, not within {bounds} s")
    check_timing(within(expiry_to_reset / 1e9, EXPIRY_TO_RESET_S),
                 f"the reset came {ms(expiry_to_reset)} after the frame that reported the "
                 f"expiry, not within {EXPIRY_TO_RESET_S} s")


@pytest.mark.timing
def test_a_spinning_sensor_task_starves_the_kicks_and_the_watchdog_resets_the_node(
        start_node, restart_node, workdir, can_spec):
    """sensor.hang spin: the sensor task keeps the CPU, every task below it starves, the health
    task with its kicks and BMS_Fault. Nothing in the node can record the fault; the hardware
    watchdog resets it all the same, in the time above, and the next start reports it."""
    cell = workdir / "cell.bin"
    cell.unlink(missing_ok=True)
    bus = open_bus(can_spec)
    recorder = Recorder(bus)
    try:
        node = start_node(can=can_spec, retained_file=cell)
        recorder.until(FAULT)
        watch = EndWatch(node)
        with node.sim() as sim:
            t_hang = int(sim.sensor_hang("spin")["t_ns"])
            mark = recorder.mark()
        assert node.wait_for_reset(timeout=RESET_WAIT_S) == RESET_WATCHDOG
        t_reset = watch.ended_ns()
        end = recorder.quiet()
        again = restart_node(node)
        first = recorder.until(FAULT, after=end).frame
        assert again.stop() == EXIT_OK
    finally:
        recorder.stop()
        bus.shutdown()
    assert read_cell(cell)[0] == 0                  # spent by the start, no reset since
    assert "supervision:" not in node.log()
    # After the hang nothing below the sensor task ran: no BMS_Fault reported anything.
    late = [h for h in recorder.frames(FAULT, mark, end) if h.at_ns > t_hang + 20_000_000]
    assert all(h.frame.raw["FaultActive"] == 0 for h in late), late
    assert first.raw["FaultActive"] == bit("RESET_WATCHDOG")
    assert again.stats()["reset"]["cause"] == "WATCHDOG"
    print(f"spin to the end of the process {ms(t_reset - t_hang)}")
    check_timing(within((t_reset - t_hang) / 1e9, SPIN_TO_RESET_S),
                 f"the reset came {ms(t_reset - t_hang)} after the spin, not within "
                 f"{SPIN_TO_RESET_S} s")


#: A stall of the sensor task that ends, measured by the node (app/app.c, SUPERVISION): from its
#: last alive indication before the stall to its first after it, a stall of less than 300 ms
#: leaves at most two examinations of the watchdog manager without an indication, as many as
#: the failed tolerance allows, and one of more than 400 ms leaves three, which make the entity
#: EXPIRED. The windows the tests need keep 10 ms from both figures, for the tick the
#: measurement is made in and the health task's response time.
STALL_SURVIVED_MS = 290
STALL_EXPIRED_MS = 410
SENSOR_PERIOD_MS = 10


@dataclass(frozen=True)
class Stall:
    """A stall of the sensor task as the node saw it."""

    hung: Heard          # the heartbeat of the period whose read stalled
    back: Heard          # the first heartbeat after the stall
    stall_ms: int        # from the last alive indication before it to the first after it
    expired: bool        # a BMS_Fault frame since the hang has SUPERVISION_EXPIRED


def stall_the_sensor_task(sim: SimControl, recorder: Recorder, hold_s: float) -> Stall:
    """Just after a BMS_Fault frame, make the front end hang (sensor.hang on), hold it for
    `hold_s` once the sensor task is stuck in its read, and end it (sensor.hang off). The stall
    the node saw comes from the heartbeats' tick counts: the stuck pass sent its heartbeat (the
    period after the N readings taken by then) before its read, one period after the last alive
    indication, and the first pass after the stall sends its heartbeat at once, in the tick the
    stuck read returned and the stuck pass made its alive indication. Waits for two BMS_Fault
    frames after that heartbeat: an expiry of the sensor task is in one of the frames by then."""
    start = recorder.until(FAULT, after=recorder.mark())
    assert sim.sensor_hang("on")["hang"] == "on"
    deadline = time.monotonic() + 5.0
    while sim.sensor_status()["hung"] != 1:
        assert time.monotonic() < deadline, "the sensor task did not get stuck in its read"
    reads = sim.sensor_status()["current_reads"]
    time.sleep(hold_s)                  # how long the bench holds the fault, not a wait for it
    assert sim.sensor_hang("off")["hang"] == "off"
    hung = recorder.until(HEARTBEAT, lambda f: f.raw["PeriodCounter"] == reads + 1,
                          after=start.index)
    back = recorder.until(HEARTBEAT, lambda f: f.raw["PeriodCounter"] > reads + 1,
                          after=hung.index)
    recorder.next(FAULT, back, 2)
    expired = any(h.frame.raw["FaultActive"] & bit("SUPERVISION_EXPIRED")
                  for h in recorder.frames(FAULT, start.index))
    stall_ms = back.frame.raw["TickCount"] - hung.frame.raw["TickCount"] + SENSOR_PERIOD_MS
    return Stall(hung=hung, back=back, stall_ms=stall_ms, expired=expired)


@pytest.mark.timing
@UDP_ONLY
def test_a_sensor_stall_within_the_failed_tolerance_is_survived_and_its_fault_heals(
        open_link):
    """The sensor task stands still for about 250 ms and goes on: two examinations without an
    alive indication, the failed tolerance, and then a correct one, because the task skips the
    periods it missed instead of making them up back to back (app/app.c, sensor_task()). The
    node is not reset. SAMPLE_TIMEOUT goes active while the samples do not come and heals with
    the first one: BMS_Fault and BMS_PackStatus report it and then no fault, the statistics
    give its status byte and counter of a healed DTC, and the processing task's stall is a
    pair of log lines the health task wrote. On the bus the period counter goes on without a
    gap; the statistics count the periods the sensor task skipped. Whether a stall leaves two
    examinations without an indication depends on how long it lasted and where it fell against
    the health task's period, so the test decides from what the node saw: a stall the node saw
    outside the window, or one that left fewer than two such examinations, is a timing miss."""
    timeout = bit("SAMPLE_TIMEOUT")
    link = open_link()
    recorder = Recorder(link.bus)
    try:
        with link.node.sim() as sim:
            recorder.next(FAULT, recorder.until(FAULT), 2)
            stall = stall_the_sensor_task(sim, recorder, 0.23)
            if stall.expired:
                assert link.node.wait_for_reset(timeout=RESET_WAIT_S) == RESET_WATCHDOG
                assert stall.stall_ms > STALL_SURVIVED_MS, (
                    f"a stall of {stall.stall_ms} ms, within the failed tolerance, reset the "
                    f"node\n{link.node.log()}")
                check_timing(False, f"the stall lasted {stall.stall_ms} ms, not less than "
                                    f"{STALL_SURVIVED_MS} ms: the node was reset")
            gone = recorder.until(FAULT, lambda f: f.raw["FaultActive"] == 0,
                                  after=stall.back.index)
            after = recorder.next(FAULT, gone, 3)
        assert link.node.stop() == EXIT_OK
        recorder.quiet()
    finally:
        recorder.stop()
    stats = link.node.stats()
    manager = stats["watchdog_manager"]
    (sensor,) = [entity for entity in manager["entities"] if entity["name"] == "sensor"]
    print(f"stall of {stall.stall_ms} ms as the node saw it; sensor entity: incorrect "
          f"{sensor['incorrect']}, counts {sensor['min_count']} to {sensor['max_count']}; "
          f"periods skipped {stats['sensor']['skipped_periods']}")
    check_timing(stall.stall_ms < STALL_SURVIVED_MS,
                 f"the stall lasted {stall.stall_ms} ms, not less than {STALL_SURVIVED_MS} ms")
    check_timing(sensor["incorrect"] == 2,
                 f"the stall left {sensor['incorrect']} examinations without an alive "
                 f"indication, not the 2 the failed tolerance allows")

    # Supervised all along: the sensor entity failed twice and is OK again, every cycle kicked.
    assert sensor["status"] == "OK", sensor
    assert sensor["min_count"] == 0 and sensor["max_count"] <= 20, sensor
    assert manager["global_status"] == "OK" and manager["kicks_withheld"] == 0, manager
    assert manager["kicks"] == manager["cycles"], manager
    assert all(entity["incorrect"] == 0 for entity in manager["entities"]
               if entity["name"] != "sensor"), manager

    # SAMPLE_TIMEOUT while the samples did not come, healed with the first one; no other DTC.
    shown = recorder.frames(FAULT, stall.hung.index, gone.index)
    assert any(h.frame.raw["FaultActive"] == timeout for h in shown), shown
    for h in shown:
        assert h.frame.raw["FaultActive"] & ~timeout == 0, h.frame
    for h in [gone, *after]:
        assert (h.frame.raw["FaultActive"], h.frame.raw["FaultConfirmed"]) == (0, timeout)
    # The samples after the stall are processed before the health task's next job works their
    # verdicts in, so the first BMS_PackStatus after the stall still has the fault. (It can go
    # out before the first heartbeat after the stall: its identifier is the more urgent.)
    assert any(frame.raw["PackFaultActive"] == 1
               for frame in pack_statuses(recorder, stall.hung, gone)), "no PackFaultActive"
    for frame in pack_statuses(recorder, after[1], after[2]):
        assert (frame.raw["PackFaultActive"], frame.signals["PackState"]) == (0, "NORMAL"), frame
    dtcs = check_dtcs(stats, {"SAMPLE_TIMEOUT": healed("SAMPLE_TIMEOUT")})
    assert (dtcs["SAMPLE_TIMEOUT"]["counter"], dtcs["SAMPLE_TIMEOUT"]["fdc"]) == (-1, -128)
    assert fault_lines(link.node.log()) == [("SAMPLE_TIMEOUT", True), ("SAMPLE_TIMEOUT", False)]

    # The processing task's stall, logged by the health task from its counts.
    process = stats["process"]
    assert process["timeouts"] >= 1 and process["recoveries"] == 1, process
    log = link.node.log()
    assert log.count("no sample from the sensor task for 30 ms") == 1
    assert log.count(f"samples again after {process['timeouts']} timeouts") == 1

    # The sensor task skipped the periods it missed, those between the stuck pass and the first
    # after it, with one overrun, and the period counter went on without a gap. (A host that
    # stalls the sensor task's thread makes more of both, and can make a heartbeat be replaced
    # before it goes out: those are timing checks.)
    skipped = (stall.back.frame.raw["TickCount"] - stall.hung.frame.raw["TickCount"]) \
        // SENSOR_PERIOD_MS - 1
    overruns, skipped_periods = stats["sensor"]["overruns"], stats["sensor"]["skipped_periods"]
    assert overruns >= 1 and skipped_periods >= skipped - 1, (skipped, stats["sensor"])
    check_timing(overruns == 1 and abs(skipped_periods - skipped) <= 1,
                 f"{skipped} periods skipped in the stall, but {stats['sensor']} in the run")
    period_counters = [h.frame.raw["PeriodCounter"] for h in (stall.hung, stall.back)]
    check_timing(period_counters[1] == period_counters[0] + 1,
                 f"the period counters of the stuck pass and the next one: {period_counters}")


@pytest.mark.timing
@UDP_ONLY
def test_a_sensor_stall_beyond_the_failed_tolerance_ends_in_a_reset(open_link):
    """The sensor task stands still for about 450 ms and then goes on: three examinations
    without an alive indication, wherever the stall falls against the health task's period.
    The entity is EXPIRED and the global status STOPPED before the task goes on, and STOPPED is
    final: no more kicks, and the hardware watchdog resets the node although the task runs
    again. A stall the node saw shorter than the window is a timing miss."""
    link = open_link()
    recorder = Recorder(link.bus)
    try:
        with link.node.sim() as sim:
            recorder.next(FAULT, recorder.until(FAULT), 2)
            stall = stall_the_sensor_task(sim, recorder, 0.45)
        print(f"stall of {stall.stall_ms} ms as the node saw it; SUPERVISION_EXPIRED reported: "
              f"{stall.expired}")
        if not stall.expired:
            assert stall.stall_ms <= STALL_EXPIRED_MS, (
                f"a stall of {stall.stall_ms} ms, beyond the failed tolerance, did not expire")
            check_timing(False, f"the stall lasted {stall.stall_ms} ms, not more than "
                                f"{STALL_EXPIRED_MS} ms: the node was not reset")
        assert link.node.wait_for_reset(timeout=RESET_WAIT_S) == RESET_WATCHDOG
    finally:
        recorder.stop()
    stderr = link.node.stderr().strip().splitlines()
    assert len(stderr) == 1 and FATAL_LINE.fullmatch(stderr[0]), stderr
    log = link.node.log()
    assert log.count("supervision: sensor expired (0 alive in its last cycle)") == 1, log
    assert "samples again after" in log                    # it went on, and was reset all the same


@pytest.mark.timing
@UDP_ONLY
def test_a_fault_reaches_bms_fault_within_its_debounce_time_and_one_health_period(open_link):
    """Each sensor fault, applied after its test has passed, and removed: the frame that shows
    the change comes after the readings its debouncing needs (k of them, one every evidence
    period T after the command) and at most one health period (100 ms) after the last of them:
    within (k - 1) T and k T + 100 ms, plus the host's latency. Meanwhile BMS_Fault keeps its
    period of 100 ms."""
    slack_ms = 25
    link = open_link()
    recorder = Recorder(link.bus)
    results = []
    try:
        with link.node.sim() as sim:
            heard = recorder.until(FAULT)
            recorder.next(FAULT, heard, 7)      # every test of the pack has passed (500 ms)
            for fault, case in CASES.items():
                mask = bit(fault)
                mark = recorder.mark()
                t_apply = int(case.apply(sim)["t_ns"])
                active = recorder.until(FAULT, lambda f: f.raw["FaultActive"] & mask, after=mark)
                mark = recorder.mark()
                t_remove = int(case.remove(sim)["t_ns"])
                gone = recorder.until(FAULT, lambda f: not f.raw["FaultActive"] & mask,
                                      after=mark)
                results.append((fault, case, active.at_ns - t_apply, gone.at_ns - t_remove))
        assert link.node.stop() == EXIT_OK
        recorder.quiet()
    finally:
        recorder.stop()

    for fault, case, to_active, to_gone in results:
        for what, latency, k in (("active", to_active, evidence_to_fail(fault)),
                                 ("gone", to_gone, evidence_to_heal(fault))):
            low, high = (k - 1) * case.evidence_ms, k * case.evidence_ms + 100 + slack_ms
            print(f"{fault} {what} in BMS_Fault after {ms(latency)} ({k} pieces of evidence "
                  f"every {case.evidence_ms} ms: {low} to {high} ms)")
            check_timing(low <= latency / 1e6 <= high,
                         f"{fault}: {what} {ms(latency)} after the command, not within {low} to "
                         f"{high} ms")
    shown = recorder.frames(FAULT)
    stamps = [h.at_ns for h in shown]
    gaps = [(b - a) / 1e6 for a, b in zip(stamps, stamps[1:])]
    mean = (stamps[-1] - stamps[0]) / 1e6 / len(gaps)
    print(f"BMS_Fault: {len(stamps)} frames, mean period {mean:.2f} ms, longest gap "
          f"{max(gaps):.1f} ms")
    check_timing(abs(mean - 100) <= 1, f"BMS_Fault: mean period {mean:.2f} ms")
    check_timing(max(gaps) < 100 + slack_ms, f"BMS_Fault: a gap of {max(gaps):.1f} ms")

    # At once on a change: the frame that first shows a change follows the node's log line
    # about it (written in the same health job, stamped on CLOCK_MONOTONIC) within the time
    # the frame takes to the tester, not a health period later.
    delays = []
    position = {name: 0 for name in SPEC}
    for logged_ns, name, now_active in stamped_fault_lines(link.node.log()):
        frame = next(h for h in shown[position[name]:]
                     if bool(h.frame.raw["FaultActive"] & bit(name)) == now_active)
        position[name] = shown.index(frame)
        delays.append((frame.at_ns - logged_ns) / 1e6)
    assert len(delays) == 2 * len(CASES)
    print(f"from the log line of a change to the frame that shows it: {min(delays):.2f} to "
          f"{max(delays):.2f} ms")
    check_timing(0 <= min(delays) and max(delays) <= slack_ms,
                 f"a change reached the bus {min(delays):.2f} to {max(delays):.2f} ms after the "
                 f"log line about it")


#: The supervision of the design, as docs/faults.md gives it (The watchdogs in the node): per
#: entity, in the order of the watchdog manager's table, the alive indications expected per
#: reference cycle and the correct count from the lowest to the highest, the reference cycle
#: (one health period) and the failed tolerance; and the expired tolerance.
SUPERVISION = [("can_rx", 2, 1, 65535, 1, 2), ("sensor", 10, 1, 20, 1, 2),
               ("process", 10, 1, 21, 1, 2), ("can_tx", 2, 1, 65535, 1, 2),
               ("diag", 2, 1, 65535, 1, 2), ("flash", 2, 1, 65535, 6, 1)]
EXPIRED_TOLERANCE = 0


def test_a_run_without_faults_supervises_every_task_and_kicks_in_every_period(start_node):
    """The supervision of a run without a fault: the configuration of the design (the bounds,
    reference cycles and tolerances of docs/faults.md), every entity OK with its counts within
    those bounds, a kick after every supervision cycle and none withheld, the hardware watchdog
    started with the timeout of the design; no DTC failed, and every one whose test ran in the
    2 s has passed: the 4 reset faults at the start, the supervision, error passive after 1 s
    and the 9 faults of the samples, one status change each (bus off passes only after 5 s)."""
    node = start_node("--duration-s", "2")
    assert node.wait(timeout=20) == EXIT_OK
    stats = node.stats()
    manager = stats["watchdog_manager"]
    print(manager)
    assert manager["global_status"] == "OK" and manager["first_expired"] is None
    assert manager["kicks"] == manager["cycles"] == 20 and manager["kicks_withheld"] == 0
    assert manager["hw_started"] is True and manager["hw_timeout_ms"] == 500
    assert manager["expired_tolerance"] == EXPIRED_TOLERANCE
    configured = [(e["name"], e["expected_alive"], e["expected_alive"] - e["min_margin"],
                   e["expected_alive"] + e["max_margin"], e["reference_cycle"],
                   e["failed_tolerance"]) for e in manager["entities"]]
    assert configured == SUPERVISION
    for entity, (_, _, lower, upper, reference_cycle, _) in zip(manager["entities"], SUPERVISION):
        assert entity["status"] == "OK" and entity["incorrect"] == 0, entity
        assert entity["examinations"] == 20 // reference_cycle, entity
        assert lower <= entity["min_count"] <= entity["max_count"] <= upper, entity
    hw = stats["hw_watchdog"]
    assert hw["started"] is True and hw["timeout_ms"] == 500
    # Every kick the model counted came from the health task, after a supervision cycle.
    assert hw["kicks"] == manager["kicks"]
    assert hw["longest_unkicked_ns"] < hw["timeout_ms"] * 1_000_000
    print(f"longest time without a kick: {ms(hw['longest_unkicked_ns'])}")
    dtcs = check_dtcs(stats, {})
    for name, entry in dtcs.items():
        assert entry["status"] == (AFTER_CLEAR if name == "CAN_BUS_OFF" else 0x00), entry
    assert dtcs["CAN_BUS_OFF"]["counter"] == -20          # one step per health period
    # The four reset faults and UPDATE_ROLLBACK passed at the start, and the other changes of
    # this run.
    assert stats["faults"]["status_changes"] == 16
    # A verdict per sample, every one taken by the health task but those of the samples after
    # its last period: fewer than the queue holds.
    process = stats["process"]
    assert process["verdicts_lost"] == 0 and process["verdicts_posted"] == process["samples"]
    assert 0 <= process["verdicts_posted"] - stats["faults"]["verdicts_applied"] <= 32
    assert stats["faults"]["health_cycles"] == 20
    assert fault_lines(node.log()) == []
