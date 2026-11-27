"""The telemetry of phase 2 as a receiver on the bus sees it, on both transports.

Frames are decoded with the DBC and the protected ones are checked with fcn_tools.decode,
which uses fcn_tools.e2e, an implementation of AUTOSAR E2E Profile 11 written independently of
the node's lib/e2e. Every number a test compares with comes from the DBC, from the node's own
statistics or from the documented sensor model (docs/sensor-model.md); a test that waits waits
for frames, heartbeats or readings of the model, never for a fixed time. Wall-clock periods are
checked only in test_every_cyclic_message_keeps_its_period_on_the_wall_clock. It is marked
`timing`, and so are the TESTER_Command tests, whose counts hold only for a tester that the host
ran in time (check_command_cadence()).

WHICH READING A FRAME CARRIES. Nothing on the bus says which sample a value of a protected
message was built from, but its E2E counter comes close: the node advances it once for every
value it builds, so a tester that heard the message from the node's first frame on can number
the values 0, 1, 2, ... Two things blur that number, and the node's statistics count both:
  - a value that the transmit queue replaced by its successor before it went out (superseded)
    is a step of more than one in the counter, and a run of 15 or more in a row looks like a
    shorter one (the counter has 15 values);
  - a sample the queue between the sensor and the processing task had no room for
    (sample_queue.full) is a reading that no value was built from.
A value's number therefore gives the reading it carries as a range: a single number on a host
that kept up, a few readings wide on one that stalled the node. The tests assert only what holds
for every reading in the range (`numbered()`, `Span`).
"""
from __future__ import annotations

import json
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

import can
import pytest

from fcn_tools import e2e
from fcn_tools.bus import (collect_while, drain, is_status, open_bus, status_fields, status_gaps,
                           status_missing_since_start)
from fcn_tools.decode import DecodedFrame, Decoder, load_database
from fcn_tools.node import EXIT_OK, NodeProcess
from fcn_tools.timing import check_timing

# As in tests/conftest.py: every bus test runs on both transports.
TRANSPORTS = ["udp", pytest.param("socketcan", marks=pytest.mark.socketcan)]

DB = load_database()

FAULT = "BMS_Fault"
HEARTBEAT = "BMS_Heartbeat"
PACK_STATUS = "BMS_PackStatus"
CELLS = ("BMS_CellVoltages1", "BMS_CellVoltages2")
TEMPERATURES = "BMS_Temperatures"
TASK_STATS = "BMS_TaskStats"
NODE_INFO = "BMS_NodeInfo"
BOOT_INFO = "BMS_BootInfo"
COMMAND = "TESTER_Command"

ID = {message.name: message.frame_id for message in DB.messages}
CYCLE_MS = {message.name: message.cycle_time for message in DB.messages if message.cycle_time}
#: The node's cyclic messages, and those of them protected end to end, from the DBC.
CYCLIC = tuple(m.name for m in DB.messages if m.cycle_time and m.senders == ["BMS"])
DATA_ID = Decoder(DB).protected
PROTECTED = tuple(name for name in CYCLIC if ID[name] in DATA_ID)
#: The messages CmdTelemetryOn switches off (app/app.c): the protected messages of the
#: measurement chain. BMS_Fault is protected too, and goes on whatever the switch says.
GATED = ("BMS_PackStatus", "BMS_CellVoltages1", "BMS_CellVoltages2", "BMS_Temperatures")
assert set(GATED) == set(PROTECTED) - {FAULT}
#: The faults of BMS_Fault by name: their numbers are the bits of FaultActive (app/fault.h).
FAULT_NUMBER = {str(name): number for number, name in DB.get_message_by_name(FAULT)
                .get_signal_by_name("FaultMostRecent").choices.items()}

# What the application does with them (app/app_tasks.h, app/app.c): the sensor task's period is
# the heartbeat's cycle, and every message of the measurement chain goes out with the samples
# that carry its group; the health task sends task statistics and node information.
SENSOR_PERIOD_MS = CYCLE_MS[HEARTBEAT]
HEALTH_PERIOD_MS = 100
SAMPLE_CHAIN = (HEARTBEAT, PACK_STATUS, *CELLS, TEMPERATURES)
#: Samples per reading of each group: the current in every sample, the cells in every second,
#: the temperatures in every tenth (the cycle times of the messages that carry them).
EVERY = {"current": 1, "cells": CYCLE_MS[CELLS[0]] // SENSOR_PERIOD_MS,
         "temps": CYCLE_MS[TEMPERATURES] // SENSOR_PERIOD_MS}
#: The tasks BMS_TaskStats goes round, by TaskIndex (app/app.c, s_stats_tasks).
STATS_TASKS = ("can_rx", "sensor", "process", "can_tx", "health", "flash", "IDLE", "diag")
#: Tasks without a period of their own, which never count an overrun.
UNPERIODIC = ("can_rx", "can_tx", "flash", "IDLE", "diag")

COUNTER_MODULUS = e2e.COUNTER_MODULUS

# ------------------------------------------------------------------------- the sensor model
# docs/sensor-model.md, platform/posix/posix_sensor.c: the model's value of every reading, and
# how far its noise can take a reading from it.

PROFILE = ((300, 2000), (800, 45000), (150, 160000), (450, 45000), (300, -80000))  # (readings, mA)
PROFILE_READINGS = sum(steps for steps, _ in PROFILE)
CELL_OCV_MV = (3702, 3698, 3705, 3695, 3700, 3703, 3697, 3701)
CELL_RESISTANCE_UOHM = (1800, 1900, 1750, 2100, 1850, 1950, 1800, 2000)
TEMP_DC = (251, 248, 256, 245)
NOISE = {"current": 150, "cells": 2, "temps": 2}   # mA, mV, tenths of a degree

#: Readings after which a filter of lib/bms has forgotten the readings before them, to less
#: than the slack below: 50 samples (0.5 s), against the filters' time constants of about 4
#: samples for the current and 4 cell readings for the cells (app/app.c, s_bms_config).
SETTLE = {"current": 50, "cells": 25, "temps": 25}
SETTLED_SLACK = 1   # mA or mV


def round_half_away(numerator: int, denominator: int) -> int:
    """numerator / denominator to the nearest integer, a half away from zero, as the node
    rounds (lib/bms, app/signals.c)."""
    half = denominator // 2
    return (numerator + half) // denominator if numerator >= 0 else -((half - numerator)
                                                                       // denominator)


def profile_ma(reading: int) -> int:
    left = reading % PROFILE_READINGS
    for steps, current_ma in PROFILE:
        if left < steps:
            return current_ma
        left -= steps
    raise AssertionError("unreachable")


def model(group: str, channel: int, reading: int) -> int:
    """The model's value of reading `reading` of a channel, before noise. A cell sees the
    profile's current at the latest current reading, which the sensor task takes just before
    the cells in the same sample (app/app.c, acquire())."""
    if group == "current":
        return profile_ma(reading)
    if group == "cells":
        current_ma = profile_ma(reading * EVERY["cells"])
        drop_mv = round_half_away(current_ma * CELL_RESISTANCE_UOHM[channel], 1_000_000)
        return CELL_OCV_MV[channel] - drop_mv
    return TEMP_DC[channel]


def filtered_range(group: str, channel: int, readings: "Span") -> tuple[int, int]:
    """The range in which the filtered value of a channel lies after the readings 0 .. r of
    its group, for every r in `readings`, without a fault. A filter of lib/bms never leaves the
    range of its inputs (lib/bms/bms.h); once the model has held one value for SETTLE readings,
    the readings before weigh less than SETTLED_SLACK."""
    noise = NOISE[group]
    start = max(0, readings.lo - SETTLE[group])
    recent = {model(group, channel, r) for r in range(start, readings.hi + 1)}
    if len(recent) == 1:
        value = recent.pop()
        slack = 0 if start == 0 else SETTLED_SLACK
        return value - noise - slack, value + noise + slack
    values = [model(group, channel, r) for r in range(readings.hi + 1)]
    return min(values) - noise, max(values) + noise


# ---------------------------------------------------------------- numbering the values heard


@dataclass(frozen=True)
class Span:
    """The numbers a quantity can have: lo .. hi, both included."""

    lo: int
    hi: int

    def scaled_down(self, every: int, slack: int = 0) -> "Span":
        """The span of n // every for n in this span, widened downwards by `slack`."""
        return Span(max(0, self.lo // every - slack), self.hi // every)

    def within(self, lo: int, hi: int) -> bool:
        return lo <= self.lo and self.hi <= hi


@dataclass(frozen=True)
class Value:
    frame: DecodedFrame
    number: Span    # which value of its message it is: 0 is the first one the node built


def numbered(frames: list[DecodedFrame], counters: list[int], modulus: int, first: int,
             replaced: int) -> list[Value]:
    """Number the values a tester heard of one message, from the counter each carries.

    `counters` are the frames' counters, which run modulo `modulus` from `first`; `replaced` is
    the node's count of values of the message that the transmit queue replaced (superseded).
    The node never sends a counter twice in a row, so a step of 0 is a full turn. A run of
    `modulus` or more replaced values in a row cannot be told from a run that is `modulus`
    shorter; the numbers are then known to within the difference between the values found
    missing and `replaced`. Asserts that the two agree as far as the counter can tell."""
    numbers, missing = [], 0
    for i, counter in enumerate(counters):
        step = (counter - first) % modulus if i == 0 else (counter - counters[i - 1]) % modulus
        if i > 0 and step == 0:
            step = modulus
        number = step if i == 0 else numbers[-1] + step
        missing += step if i == 0 else step - 1
        numbers.append(number)
    unseen = replaced - missing
    assert unseen >= 0 and unseen % modulus == 0, (
        f"{missing} values missing between the counters heard, the node replaced {replaced}")
    return [Value(frame, Span(number, number + unseen))
            for frame, number in zip(frames, numbers)]


def numbered_message(run: "Recording", name: str) -> list[Value]:
    """The values of a protected message heard in a run, numbered by their E2E counters."""
    frames = run.decoded(name)
    replaced = run.message(name)["queue"]["superseded"]
    return numbered(frames, [frame.raw["E2E_Counter"] for frame in frames], COUNTER_MODULUS,
                    e2e.SENDER_INITIAL_COUNTER, replaced)


def readings_of(name: str, value: Value, lost: int) -> dict[str, Span]:
    """The latest reading of each group that the pack state held when the value was built.

    BMS_PackStatus is built from every sample the processing task got, so its value n comes
    from sample n or, if samples were lost before it, a later one: n .. n + lost. A message of
    the cells or the temperatures is built from the samples that carry its group, so its value
    n carries reading n of the group or, again, up to `lost` later. A group read less often
    than the sample holds its latest reading: the one of the last sample that carried it, of
    which up to `lost` may have been lost."""
    span = Span(value.number.lo, value.number.hi + lost)
    if name == PACK_STATUS:
        return {group: span.scaled_down(every, 0 if every == 1 else lost)
                for group, every in EVERY.items()}
    group = "cells" if name in CELLS else "temps"
    return {group: span}


# ------------------------------------------------------------------------------ the tester


class Recorder:
    """Reads every frame a bus receives, in a thread of its own, so that the socket never fills
    up while the test does something else (a socket holds about 1.5 s of the node's frames).
    Tests can wait for heartbeats, the node's own clock."""

    def __init__(self, bus: can.BusABC, first: Iterable[can.Message] = ()) -> None:
        self._bus = bus
        self.frames: list[can.Message] = list(first)
        self._heartbeats = sum(1 for frame in self.frames if is_status(frame))
        self._changed = threading.Condition()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            message = self._bus.recv(timeout=0.05)
            if message is not None:
                with self._changed:
                    self.frames.append(message)
                    self._heartbeats += is_status(message)
                    self._changed.notify_all()

    def heartbeats(self, count: int, timeout: float = 10.0) -> None:
        """Wait for `count` more heartbeats: `count` periods of the node's sensor task."""
        with self._changed:
            target = self._heartbeats + count
            arrived = self._changed.wait_for(lambda: self._heartbeats >= target, timeout)
        assert arrived, f"no heartbeat for {timeout} s"

    def watch(self, condition: Callable[[can.Message], bool]) -> Callable[[], bool]:
        """A test of `condition` on the frames received from now on. Each call of the test hands
        the frames that arrived since the last call to `condition`, in order, and returns True
        from the first frame for which it held on."""
        with self._changed:
            seen = len(self.frames)
        met = False

        def test() -> bool:
            nonlocal seen, met
            with self._changed:
                while not met and seen < len(self.frames):
                    met = condition(self.frames[seen])
                    seen += 1
            return met

        return test

    def until(self, condition: Callable[[can.Message], bool], timeout: float = 10.0) -> bool:
        """Wait until `condition` holds for a frame received from now on; False if it did not
        within `timeout`."""
        test = self.watch(condition)
        with self._changed:
            return self._changed.wait_for(test, timeout)

    def stop(self) -> list[can.Message]:
        """Stop reading, and return every frame, those still on their way included."""
        self._stop.set()
        self._thread.join(timeout=5)
        self.frames += drain(self._bus)
        return self.frames


def heartbeats_without(names: Iterable[str], count: int) -> Callable[[can.Message], bool]:
    """A condition for Recorder.until(): `count` heartbeats in a row with no frame of `names`
    among them."""
    ids = {ID[name] for name in names}
    run = 0

    def condition(message: can.Message) -> bool:
        nonlocal run
        if message.arbitration_id in ids:
            run = 0
        elif is_status(message):
            run += 1
        return run >= count

    return condition


def one_of_each(names: Iterable[str]) -> Callable[[can.Message], bool]:
    """A condition for Recorder.watch(): a frame of each of `names` has arrived."""
    missing = {ID[name] for name in names}

    def condition(message: can.Message) -> bool:
        missing.discard(message.arbitration_id)
        return not missing

    return condition


def command(counter: int, telemetry_on: bool, clear_faults_seq: int,
            data_id: int | None = None, length: int = 8) -> bytes:
    """A TESTER_Command frame, protected with fcn_tools.e2e. `data_id` replaces the message's
    Data ID, `length` its DLC (the CRC then covers that many bytes)."""
    payload = DB.encode_message(COMMAND, {"E2E_Crc": 0, "E2E_Counter": 0,
                                          "CmdTelemetryOn": int(telemetry_on),
                                          "CmdClearFaultsSeq": clear_faults_seq})
    config = e2e.Config(data_id=DATA_ID[ID[COMMAND]] if data_id is None else data_id,
                        data_length=8 * length)
    data, _ = e2e.protect(config, counter, payload[:length])
    return data


class CommandStream:
    """Sends TESTER_Command frames from a bus of its own, each after two heartbeats of the node:
    every 20 ms of the node's time, so that each 100 ms supervision period of the node gets
    frames even if the tester runs up to 80 ms late. Whether it did is read off the recorded
    bus afterwards (check_command_cadence())."""

    def __init__(self, spec: str, recorder: Recorder) -> None:
        self._bus = open_bus(spec)
        self._recorder = recorder
        self.sent = 0

    def send(self, data: bytes) -> None:
        self._recorder.heartbeats(2)
        self._bus.send(can.Message(arbitration_id=ID[COMMAND], data=data, is_extended_id=False))
        self.sent += 1

    def close(self) -> None:
        self._bus.shutdown()


# Whether the tester kept its pace is a question of host timing: CommandStream runs in the test
# process, paced by the Recorder's thread, and the host can hold up either. A supervision period
# of the node without a frame is a NONEWDATA verdict, and those can take the state machine out
# of VALID, after which the node rightly ignores good frames (app/command.h); a test that counts
# acceptances or asserts VALID relies on the stream having had none. The recording tells: the
# tester's frames are on the bus between the node's heartbeats. k heartbeats between two frames
# are at most (k + 1) * 10 ms of the node's time, so with at most 8, 90 ms, no 100 ms period
# passed without a frame, with 10 ms to spare.
MAX_HEARTBEATS_BETWEEN_COMMANDS = 8
# After the last frame, with the last three checks OK for the state machine (in every test
# here), VALID is left in the third period in a row without a frame (app/command.h), which ends
# more than 300 ms after the frame; the node has stopped before that if its last period came at
# most 28 after the last heartbeat before the frame: within (28 + 1) * 10 = 290 ms of it.
MAX_HEARTBEATS_AFTER_LAST_COMMAND = 28


def heartbeats_around_commands(frames: list[can.Message], periods: int) -> list[int]:
    """For a recording with TESTER_Command frames: the heartbeats between each frame and the one
    before it, from the second frame on, and last, those after the last frame until the node
    stopped; `periods`, the statistics' sensor.periods, is the number of the node's last period.
    Counted with the heartbeats' PeriodCounter, so that a heartbeat the node replaced or the
    tester's socket dropped counts as well."""
    heartbeats = [frame for frame in frames if is_status(frame)]
    status_gaps(heartbeats, max_step=None)      # the counters run forward with the ticks
    before: list[int] = []      # PeriodCounter of the last heartbeat heard before each frame
    latest = 0
    for frame in frames:
        if is_status(frame):
            latest = status_fields(frame)[0]
        elif frame.arbitration_id == ID[COMMAND] and not frame.is_extended_id:
            before.append(latest)
    assert before, "no TESTER_Command frame on the bus"
    assert latest <= periods, (latest, periods)
    return [b - a for a, b in zip(before, before[1:])] + [periods - before[-1]]


def check_command_cadence(frames: list[can.Message], periods: int,
                          pauses: Iterable[int] = ()) -> None:
    """A timing tolerance: the tester kept its cadence, so that every supervision period of the
    node from the first frame to the last had a frame, except after the frames whose numbers
    (1 for the first) are in `pauses`, where the test stops the stream on purpose. Before the
    first frame there is nothing to keep: the state machine is NODATA, and the first good frame
    clears its window (lib/e2e/e2e_sm.c). `periods` as for heartbeats_around_commands()."""
    *between, _ = heartbeats_around_commands(frames, periods)
    late = [f"{count} before frame {number}" for number, count in enumerate(between, start=2)
            if number - 1 not in pauses and count > MAX_HEARTBEATS_BETWEEN_COMMANDS]
    check_timing(not late, "the command stream fell behind, heartbeats " + ", ".join(late)
                 + f" (at most {MAX_HEARTBEATS_BETWEEN_COMMANDS}): the test process, or the "
                 "heartbeats that pace it, were held up")


def check_stopped_in_time(frames: list[can.Message], periods: int) -> None:
    """A timing tolerance: the test stopped the node before its state machine could leave
    VALID for want of frames after the last one. `periods` as for
    heartbeats_around_commands()."""
    after = heartbeats_around_commands(frames, periods)[-1]
    check_timing(after <= MAX_HEARTBEATS_AFTER_LAST_COMMAND,
                 f"the node was stopped {after} heartbeats after the last command frame "
                 f"(at most {MAX_HEARTBEATS_AFTER_LAST_COMMAND}): the test process was held up")


# ------------------------------------------------------------------------- a recorded run


@dataclass
class Recording:
    """Everything a tester heard of one run of the node, from before its start to after its
    end, and the statistics the node wrote."""

    spec: str
    seconds: int
    frames: list[can.Message]
    stats: dict
    workdir: Path

    def __post_init__(self) -> None:
        decoder = Decoder(DB)
        self.all_decoded = [decoder.decode(frame) for frame in self.frames]

    def decoded(self, name: str) -> list[DecodedFrame]:
        return [frame for frame in self.all_decoded if frame is not None and frame.name == name]

    def message(self, name: str) -> dict:
        return self.stats["telemetry"]["messages"][name]


RUN_SECONDS = 4   # past reading 300 of the current, where the load profile steps to 45 A


@pytest.fixture(scope="module", params=TRANSPORTS)
def run(request, transport, port_allocator, node_binary, artifacts_root) -> Recording:
    """One run of RUN_SECONDS per transport, heard from before the node's start until after its
    end; the tests of this module that take it look at the same run."""
    ports = port_allocator.take()
    workdir = artifacts_root / f"test_telemetry_run_{request.param}"
    try:
        with transport(request.param, ports) as spec:
            bus = open_bus(spec)
            node = NodeProcess(workdir=workdir, binary=node_binary, can=spec,
                               args=("--duration-s", str(RUN_SECONDS)))
            try:
                node.start()
                frames = collect_while(bus, lambda: node.running)
                status = node.wait(timeout=20)
            finally:
                node.kill()
                bus.shutdown()
    finally:
        ports.release()
    assert status == EXIT_OK, node.stderr()
    assert node.stderr() == "", node.stderr()
    assert node.lost_kicks() is None, node.lost_kicks()  # as tests/conftest.py checks its nodes
    return Recording(spec=spec, seconds=RUN_SECONDS, frames=frames, stats=node.stats(),
                     workdir=workdir)


# -------------------------------------------------------------------- counts and cycle times


def test_every_cyclic_message_is_sent_as_often_as_its_cycle_time_and_the_ticks_say(run):
    stats, sensor = run.stats, run.stats["sensor"]
    periods = sensor["periods"]
    full = stats["sample_queue"]["full"]
    health_periods = run.seconds * 1000 // HEALTH_PERIOD_MS   # the health task ends the run
    print(f"ticks {stats['tick']['count']}, sensor periods {periods}, samples lost {full}")
    for name in CYCLIC:
        print(f"{name}: heard {len(run.decoded(name))}, {run.message(name)}")

    # Every frame heard is one of the node's messages, decoded with its DLC.
    assert all(frame is not None and frame.error is None for frame in run.all_decoded)
    assert {frame.name for frame in run.all_decoded} == set(CYCLIC)

    # The node's time: the sensor task ran a period for every 10 ticks, or skipped it after an
    # overrun (app/app.c, sensor_task()), at least until the health task's last period ended
    # (both started at tick 0 or just after it, the sensor task first), and every heartbeat
    # carries a tick count that is at most the final one.
    grid = periods + sensor["skipped_periods"]
    assert health_periods * HEALTH_PERIOD_MS // SENSOR_PERIOD_MS <= grid
    assert grid * SENSOR_PERIOD_MS <= stats["tick"]["count"]
    heartbeats = [frame for frame in run.frames if is_status(frame)]
    assert all(status_fields(frame)[1] <= stats["tick"]["count"] for frame in heartbeats)
    assert all(v.frame.raw["UptimeSeconds"] <= stats["tick"]["count"] // 1000
               for v in numbered_info(run))

    # The samples: one per period; every one the queue took was processed before the end
    # (the health task, which ends the run, has the lowest priority but the idle task's).
    assert sensor["reads"]["current"] == periods
    assert stats["sample_queue"]["posted"] + full == periods
    assert stats["process"]["samples"] == stats["sample_queue"]["posted"]

    # Each message as often as its cycle time says: the measurement chain once for every
    # cycle_ms / 10 ms of the sensor task's periods, from the first one on (a sample the queue
    # had no room for takes its values with it); the health task's messages once for every
    # cycle_ms / 100 ms of its periods.
    for name in CYCLIC:
        message = run.message(name)
        assert message["cycle_ms"] == CYCLE_MS[name] and message["id"] == ID[name], name
        assert message["failed"] == 0 and message["suppressed"] == 0, name
        if name in SAMPLE_CHAIN:
            every = CYCLE_MS[name] // SENSOR_PERIOD_MS
            expected = -(-periods // every)
            lost = 0 if name == HEARTBEAT else full
            assert expected - lost <= message["published"] <= expected, (name, message, full)
        else:
            every = CYCLE_MS[name] // HEALTH_PERIOD_MS
            assert message["published"] == health_periods // every, (name, message)
    assert run.message(HEARTBEAT)["published"] == periods
    assert run.message(PACK_STATUS)["published"] == stats["process"]["samples"]
    for name, group in ((CELLS[0], "cells"), (CELLS[1], "cells"), (TEMPERATURES, "temps")):
        assert sensor["reads"][group] == -(-periods // EVERY[group]), group
        if full == 0:
            assert run.message(name)["published"] == sensor["reads"][group], name

    # On the bus, every value accounted for: what the queue sent reached the tester (and a
    # value it still counted as pending when it was sampled, just before the controller
    # stopped, may have too); the others it replaced or still held.
    for name in CYCLIC:
        message, queue = run.message(name), run.message(name)["queue"]
        heard = len(run.decoded(name))
        assert queue["produced"] == message["published"], name
        assert queue["produced"] == queue["sent"] + queue["superseded"] + queue["pending"], name
        assert queue["sent"] <= heard <= queue["sent"] + queue["pending"], (name, heard, queue)
    assert status_missing_since_start(heartbeats, max_step=None) == \
        run.message(HEARTBEAT)["queue"]["superseded"]


def numbered_info(run: Recording) -> list[Value]:
    """The BMS_NodeInfo values heard, numbered by their position (a value replaced before it
    went out takes a whole second of stall; the tests assert none was)."""
    frames = run.decoded(NODE_INFO)
    assert run.message(NODE_INFO)["queue"]["superseded"] == 0
    return [Value(frame, Span(i, i)) for i, frame in enumerate(frames)]


# ----------------------------------------------------------------------- end-to-end checks


def test_every_protected_frame_passes_an_independent_profile_11_check(run):
    for name in PROTECTED:
        frames = run.decoded(name)
        replaced = run.message(name)["queue"]["superseded"]
        assert frames, name
        assert all(frame.e2e is not None and frame.e2e.crc_ok for frame in frames), name
        assert all(0 <= frame.e2e.counter <= e2e.COUNTER_MAX for frame in frames), name

        # The counters run 0, 1, ..., 14, 0, ... from the node's start, so the receiver (with
        # max_delta_counter 1) finds every frame OK, except one that follows values the queue
        # replaced; numbered() asserts that the counters miss exactly the values the node says
        # it replaced.
        values = numbered_message(run, name)
        steps = [values[0].number.lo + 1] + [b.number.lo - a.number.lo
                                             for a, b in zip(values, values[1:])]
        for value, step in zip(values, steps):
            expected = e2e.Status.OK if step == 1 else (
                e2e.Status.REPEATED if step % COUNTER_MODULUS == 0
                else e2e.Status.WRONGSEQUENCE)
            assert value.frame.e2e.status == expected, (name, value, step)
        if replaced == 0:
            assert [value.number.lo for value in values] == list(range(len(values))), name

    # The unprotected messages carry no E2E header to check.
    for name in set(CYCLIC) - set(PROTECTED):
        assert all(frame.e2e is None for frame in run.decoded(name)), name


# ------------------------------------------------------------------------- decoded values


def test_decoded_values_are_plausible_and_follow_the_sensor_model(run):
    lost = run.stats["sample_queue"]["full"]
    status_values = numbered_message(run, PACK_STATUS)
    cells = {name: numbered_message(run, name) for name in CELLS}
    temps = numbered_message(run, TEMPERATURES)

    # Every channel valid from the first sample on (it reads every group).
    for value in status_values:
        signals, raw = value.frame.signals, value.frame.raw
        assert signals["PackState"] == "NORMAL", value
        assert (raw["PackCurrentValid"], raw["PackVoltageValid"], raw["PackFaultActive"]) == \
            (1, 1, 0), value
        readings = readings_of(PACK_STATUS, value, lost)
        low, high = filtered_range("current", 0, readings["current"])
        assert round_half_away(low, 10) <= raw["PackCurrent"] <= round_half_away(high, 10), (
            value, low, high)
        bounds = [filtered_range("cells", cell, readings["cells"]) for cell in range(8)]
        assert sum(lo for lo, _ in bounds) <= raw["PackVoltage"] <= sum(hi for _, hi in bounds)

    for index, name in enumerate(CELLS):
        for value in cells[name]:
            span = readings_of(name, value, lost)["cells"]
            for n in range(4):
                cell = 4 * index + n
                low, high = filtered_range("cells", cell, span)
                millivolts = value.frame.raw[f"Cell{cell + 1}Voltage"] + 1000
                assert low <= millivolts <= high, (value, cell, low, high)

    for value in temps:
        span = readings_of(TEMPERATURES, value, lost)["temps"]
        for sensor in range(4):
            low, high = filtered_range("temps", sensor, span)
            degrees = value.frame.signals[f"Temp{sensor + 1}"]
            assert round_half_away(low, 10) <= degrees <= round_half_away(high, 10), value

    # The run reaches the load profile's step from 2 A to 45 A (current reading 300), and the
    # current follows it within SETTLE samples.
    assert status_values[-1].number.lo >= 300 + SETTLE["current"]
    assert 4485 <= status_values[-1].frame.raw["PackCurrent"] <= 4515

    # PackVoltage is the sum of the cells the same sample saw: when every value is numbered
    # exactly, value n of BMS_PackStatus belongs to value n // 2 of the cell messages.
    exact = lost == 0 and all(v.number.lo == v.number.hi
                              for v in [*status_values, *cells[CELLS[0]], *cells[CELLS[1]]])
    if exact:
        by_number = {name: {v.number.lo: v.frame.raw for v in cells[name]} for name in CELLS}
        paired = 0
        for value in status_values:
            n = value.number.lo // EVERY["cells"]
            if n in by_number[CELLS[0]] and n in by_number[CELLS[1]]:
                total = sum(by_number[CELLS[i // 4]][n][f"Cell{i + 1}Voltage"] + 1000
                            for i in range(8))
                assert value.frame.raw["PackVoltage"] == total, value
                paired += 1
        # Most have both partners: a cell value that was replaced, or still waited in the
        # queue at the end, leaves the two PackStatus values of its sample without.
        assert paired >= len(status_values) // 2, (paired, len(status_values))
        print(f"PackVoltage equal to the sum of its sample's cells in {paired} of "
              f"{len(status_values)} BMS_PackStatus frames")
    else:
        print(f"values not numbered exactly ({lost} samples lost): sums not compared")


# ------------------------------------------------------------- task statistics, node info


def test_task_stats_and_node_info_decode_and_agree_with_the_statistics(run):
    stats = run.stats
    tasks = {task["name"]: task for task in stats["tasks"]}
    assert set(STATS_TASKS) == set(tasks)

    # One task per frame, round robin from TaskIndex 0.
    frames = run.decoded(TASK_STATS)
    values = numbered(frames, [frame.raw["TaskIndex"] for frame in frames], len(STATS_TASKS), 0,
                      run.message(TASK_STATS)["queue"]["superseded"])
    for value in values:
        raw = value.frame.raw
        task = tasks[STATS_TASKS[raw["TaskIndex"]]]
        assert raw["TaskPriority"] == task["priority"], value
        # A stack peak and an overrun count only grow, so a frame's are at most the final
        # ones (both saturate on the bus); a peak includes what the task's thread had used
        # before the task's code ran.
        assert min(task["stack_start_bytes"], 65535) <= raw["TaskStackPeak"] <= min(
            task["stack_peak_bytes"], 65535), (value, task)
        assert raw["TaskOverruns"] <= min(task["overruns"], 255), (value, task)
        if task["name"] in UNPERIODIC:
            assert raw["TaskOverruns"] == 0 and task["overruns"] == 0, value
        assert 0 <= raw["TaskCpuShare"] <= 10000, value
    assert len(values) >= len(STATS_TASKS)

    # Node information every second: the version the statistics report, no slot yet (phase 4),
    # the reset cause of this start (a power-on: no retained cell), and the seconds the health
    # task has counted in its own
    # periods (app/telemetry.h, telemetry_uptime_t): the n-th frame ends the n-th second and
    # says n, which is at most the seconds of the final tick count.
    major, minor, patch = (int(part) for part in stats["version"].split("."))
    infos = numbered_info(run)
    assert len(infos) >= run.seconds - 1
    for value in infos:
        signals = value.frame.signals
        assert (signals["SwMajor"], signals["SwMinor"], signals["SwPatch"]) == \
            (major, minor, patch)
        assert signals["ActiveSlot"] == "SNA" and signals["ResetCause"] == "POWER_ON"
        assert value.number.lo + 1 == signals["UptimeSeconds"] <= stats["tick"]["count"] // 1000


# ------------------------------------------------------------------------- schedulability


def test_the_task_table_the_node_runs_with_passes_the_schedulability_check(run):
    table_path = run.workdir / "task_table.json"
    report_path = run.workdir / "sched_report.json"
    table_path.write_text(json.dumps(run.stats["task_table"], indent=2))
    out = subprocess.run([sys.executable, "-m", "fcn_tools.sched", str(table_path),
                          "--json", str(report_path)], capture_output=True, text=True,
                         timeout=60)
    print(out.stdout)
    assert out.returncode == 0, out.stdout + out.stderr

    report = json.loads(report_path.read_text())
    assert report["schedulable"] is True and report["wcet_basis"] == "design"
    assert all(task["meets_deadline"] and task["response_us"] <= task["deadline_us"]
               for task in report["tasks"])

    # The table is that of the tasks that ran: their priorities, and the periods of the DBC.
    table = {task["name"]: task for task in run.stats["task_table"]["tasks"]}
    running = {task["name"]: task["priority"] for task in run.stats["tasks"]
               if task["name"] != "IDLE"}
    assert {name: table[name]["priority"] for name in running} == running
    assert table["sensor"]["period_us"] == 1000 * CYCLE_MS[HEARTBEAT]
    assert table["process"]["period_us"] == 1000 * CYCLE_MS[PACK_STATUS]
    assert table["process"]["jitter_us"] == table["sensor"]["deadline_us"]
    assert 2 * table["health"]["period_us"] == 1000 * CYCLE_MS[TASK_STATS]
    assert 10 * table["health"]["period_us"] == 1000 * CYCLE_MS[NODE_INFO]


# ------------------------------------------------------------------------------ sensor faults


@dataclass
class Fault:
    """A fault applied through simulation control, with the model's read counters around it
    (sensor.status): `before` just before it was applied, `after` just after, `clear_before`
    and `clear_after` around its end; each a dict group -> readings taken so far."""

    label: str
    channels: tuple[tuple[str, int], ...]   # (group, channel) it affects
    offline: bool
    before: dict[str, int]
    after: dict[str, int]
    clear_before: dict[str, int]
    clear_after: dict[str, int]

    def faulty(self, group: str, readings: Span) -> bool:
        """Every reading in `readings` certainly saw the fault. A read takes its number first
        and then looks at the channel's setting, but at the off-line flag before it takes its
        number (posix_sensor.c), hence the one reading of difference."""
        first = self.after[group] + (1 if self.offline else 0)
        last = self.clear_before[group] - (1 if self.offline else 2)
        return readings.within(first, last)

    def clean(self, group: str, readings: Span) -> bool:
        """No reading in `readings` saw the fault."""
        last_before = self.before[group] - (1 if self.offline else 2)
        first_after = self.clear_after[group] + (1 if self.offline else 0)
        return readings.hi <= last_before or readings.lo >= first_after


GROUP_OF = {"current": "current", **{f"cell{n}": "cells" for n in range(1, 9)},
            **{f"temp{n}": "temps" for n in range(1, 5)}}


def carrier(group: str, channel: int) -> str:
    """The message that carries a channel."""
    if group == "current":
        return PACK_STATUS
    return CELLS[channel // 4] if group == "cells" else TEMPERATURES


READS = {"current": "current_reads", "cells": "cell_reads", "temps": "temp_reads"}
ALL_CHANNELS = (("current", 0), *(("cells", n) for n in range(8)),
                *(("temps", n) for n in range(4)))

# The faults, one at a time: each channel once, opened (the front end flags it) or held at the
# first value beyond what its sensor can deliver (lib/bms/bms.h), then the whole front end off
# line. Each is held for HOLD readings of its group and followed by GAP clean ones.
FAULTS = (("current", "open", None), ("current", "set", 300001),
          *((f"cell{n}", "open" if n % 2 else "set", None if n % 2 else 5001)
            for n in range(1, 9)),
          ("temp1", "open", None), ("temp2", "set", -401), ("temp3", "open", None),
          ("temp4", "set", 1251), ("offline", "on", None))
HOLD = {"current": 10, "cells": 6, "temps": 4}
GAP = 2


def test_each_sensor_fault_shows_as_not_available_in_the_next_frame_and_clears_again(open_link):
    link = open_link()
    recorder = Recorder(link.bus, [*link.before_first_status, link.first_status])
    faults: list[Fault] = []
    with link.node.sim() as sim:

        def reads() -> dict[str, int]:
            status = sim.sensor_status()
            return {group: status[key] for group, key in READS.items()}

        def wait_for_readings(group: str, count: int) -> dict[str, int]:
            target = reads()[group] + count
            for _ in range(10_000):
                now = reads()
                if now[group] >= target:
                    return now
                recorder.heartbeats(1)
            raise AssertionError(f"the sensor task stopped reading {group}")

        for channel, action, value in FAULTS:
            group = GROUP_OF.get(channel)
            before = reads()
            if channel == "offline":
                sim.sensor_offline(True)
            elif action == "open":
                sim.sensor_open(channel)
            else:
                sim.sensor_set(channel, value)
            after = reads()
            hold = HOLD["temps"] if group is None else HOLD[group]
            clear_before = wait_for_readings(group or "temps", hold)
            if channel == "offline":
                sim.sensor_offline(False)
            else:
                sim.sensor_clear(channel)
            clear_after = reads()
            wait_for_readings(group or "temps", GAP)
            channels = (ALL_CHANNELS if channel == "offline" else
                        ((group, 0 if group == "current" else int(channel[-1]) - 1),))
            faults.append(Fault(f"{channel} {action} {value or ''}".strip(), channels,
                                channel == "offline", before, after, clear_before, clear_after))
        recorder.heartbeats(2 * EVERY["temps"])
    assert link.node.stop() == EXIT_OK
    frames = recorder.stop()
    stats = link.node.stats()
    run = Recording(spec=link.spec, seconds=0, frames=frames, stats=stats, workdir=Path())
    lost = stats["sample_queue"]["full"]

    def state(group: str, channel: int, readings: Span) -> str | None:
        """'faulty' or 'clean' if every reading in `readings` certainly was; None if not known."""
        concerned = [f for f in faults if (group, channel) in f.channels]
        if any(f.faulty(group, readings) for f in concerned):
            return "faulty"
        if all(f.clean(group, readings) for f in concerned):
            return "clean"
        return None

    seen: dict[tuple[str, str, str], int] = {}

    def saw(fault: Fault, name: str, kind: str) -> None:
        seen[(fault.label, name, kind)] = seen.get((fault.label, name, kind), 0) + 1

    for value in numbered_message(run, PACK_STATUS):
        readings, raw = readings_of(PACK_STATUS, value, lost), value.frame.raw
        current = state("current", 0, readings["current"])
        cells = [state("cells", n, readings["cells"]) for n in range(8)]
        temps = [state("temps", n, readings["temps"]) for n in range(4)]
        if current == "faulty":
            assert (raw["PackCurrentValid"], raw["PackCurrent"]) == (0, 0), value
        if current == "clean":
            assert raw["PackCurrentValid"] == 1, value
        if "faulty" in cells:
            assert (raw["PackVoltageValid"], raw["PackVoltage"]) == (0, 0), value
        if all(c == "clean" for c in cells):
            assert raw["PackVoltageValid"] == 1, value
        states = [current, *cells, *temps]
        if "faulty" in states:
            assert value.frame.signals["PackState"] == "FAULT", value
        # A DTC follows its channel through the health task, up to one of its periods later
        # (app/fault.h): while one is active the state is FAULT, with PackFaultActive.
        if raw["PackFaultActive"] == 1:
            assert value.frame.signals["PackState"] == "FAULT", value
        elif all(s == "clean" for s in states):
            assert value.frame.signals["PackState"] == "NORMAL", value
        for fault in faults:
            group, channel = fault.channels[0]
            if fault.faulty(group, readings[group]):
                saw(fault, PACK_STATUS, "faulty")
            elif fault.clean(group, readings[group]) and \
                    readings[group].lo >= fault.clear_after[group]:
                saw(fault, PACK_STATUS, "cleared")

    for name, group, signal, offset in ((CELLS[0], "cells", "Cell{}Voltage", 0),
                                        (CELLS[1], "cells", "Cell{}Voltage", 4),
                                        (TEMPERATURES, "temps", "Temp{}", 0)):
        for value in numbered_message(run, name):
            readings = readings_of(name, value, lost)[group]
            for n in range(4):
                channel = offset + n
                shown = value.frame.signals[signal.format(channel + 1)]
                verdict = state(group, channel, readings)
                if verdict == "faulty":
                    assert shown == "SNA", (value, channel)
                if verdict == "clean":
                    assert shown != "SNA", (value, channel)
            for fault in faults:
                if not any(g == group and offset <= c < offset + 4 for g, c in fault.channels):
                    continue
                if fault.faulty(group, readings):
                    saw(fault, name, "faulty")
                elif fault.clean(group, readings) and readings.lo >= fault.clear_after[group]:
                    saw(fault, name, "cleared")

    # On a host that kept up, every value is numbered exactly, and each fault shows in the
    # messages it concerns from the first reading after its command, and clears again.
    exact = lost == 0 and all(run.message(name)["queue"]["superseded"] == 0
                              for name in PROTECTED)
    print(f"samples lost: {lost}; every value numbered exactly: {exact}")
    for fault in faults:
        print(f"{fault.label}: readings {fault.after} to {fault.clear_before}, frames "
              f"{ {k[1:]: n for k, n in seen.items() if k[0] == fault.label} }")
    if exact:
        for fault in faults:
            carriers = {PACK_STATUS} | {carrier(group, channel)
                                        for group, channel in fault.channels}
            for name in carriers:
                assert seen.get((fault.label, name, "faulty"), 0) >= 1, (fault, name)
                assert seen.get((fault.label, name, "cleared"), 0) >= 1, (fault, name)

    assert stats["pack"]["state"] == "NORMAL"
    assert stats["sensor"]["unanswered"]["current"] > 0 and stats["pack"]["flagged_readings"] > 0
    assert stats["pack"]["implausible_readings"] > 0

    # The faults as DTCs: the channels opened or held beyond their sensors and the front end
    # off line were active in BMS_Fault and are confirmed until a clear; the run ended two
    # health periods after the last fault was gone, with none active any more.
    invalid = 1 << FAULT_NUMBER["SENSOR_CHANNEL_INVALID"]
    no_answer = 1 << FAULT_NUMBER["FRONT_END_NO_ANSWER"]
    heard = run.decoded(FAULT)
    assert any(frame.raw["FaultActive"] & invalid for frame in heard)
    assert any(frame.raw["FaultActive"] & no_answer for frame in heard)
    assert heard[-1].raw["FaultConfirmed"] & (invalid | no_answer) == invalid | no_answer
    assert heard[-1].raw["FaultActive"] == 0, heard[-1]
    assert stats["faults"]["confirmed"] & (invalid | no_answer) == invalid | no_answer
    assert stats["faults"]["active"] == 0


# ----------------------------------------------------------------------- TESTER_Command


@pytest.mark.timing
def test_telemetry_off_silences_exactly_the_protected_messages_and_on_brings_them_back(
        open_link, can_spec):
    link = open_link()
    recorder = Recorder(link.bus, [*link.before_first_status, link.first_status])
    tester = CommandStream(can_spec, recorder)
    counters = (n % COUNTER_MODULUS for n in range(10_000))

    def send_until(test: Callable[[], bool], telemetry_on: bool, seq: int) -> None:
        """Keep the command coming, one frame every 20 ms, until `test` passes."""
        for _ in range(500):
            if test():
                return
            tester.send(command(next(counters), telemetry_on, seq))
        raise AssertionError("the node did not do what the commands said")

    try:
        for _ in range(3):                         # the third is the first acted on
            tester.send(command(next(counters), True, 1))
        # Off: acted on at once, the stream being VALID. Thirty heartbeats in a row without a
        # protected frame are three cycles of the slowest of them.
        send_until(recorder.watch(heartbeats_without(GATED, 3 * EVERY["temps"])), False, 2)
        send_until(recorder.watch(one_of_each(GATED)), True, 3)
        tester.send(command(next(counters), True, 3))
    finally:
        tester.close()
    assert link.node.stop() == EXIT_OK
    frames = recorder.stop()
    run = Recording(spec=link.spec, seconds=0, frames=frames, stats=link.node.stats(),
                    workdir=Path())
    stats, command_stats = run.stats, run.stats["command"]
    print(command_stats)
    print({name: run.message(name) for name in CYCLIC})

    # The counts of frames acted on and the final state hold for a stream that kept its pace.
    check_command_cadence(frames, stats["sensor"]["periods"])
    check_stopped_in_time(frames, stats["sensor"]["periods"])
    assert command_stats["frames"] == tester.sent
    assert command_stats["status"]["OK"] == tester.sent and command_stats["check_errors"] == 0
    assert command_stats["accepted"] == tester.sent - 2 and command_stats["ignored"] == 2
    assert command_stats["sm_state"] == "VALID" and command_stats["telemetry_on"] is True
    assert command_stats["clear_faults_seq"] == 3 and stats["telemetry"]["enabled"] is True

    # While it was off, the protected messages were not built (their counters did not advance,
    # so an independent receiver finds the stream unbroken), and nothing else was held back.
    processed = stats["process"]["samples"]
    full = stats["sample_queue"]["full"]
    for name in GATED:
        message = run.message(name)
        assert message["suppressed"] > 0 and message["failed"] == 0, name
        every = CYCLE_MS[name] // SENSOR_PERIOD_MS
        carried = processed if every == 1 else -(-stats["sensor"]["periods"] // every)
        built = message["published"] + message["suppressed"]
        assert carried - (0 if every == 1 else full) <= built <= carried, (name, message)
        numbered_message(run, name)
        assert all(frame.e2e.crc_ok for frame in run.decoded(name)), name
    for name in set(CYCLIC) - set(GATED):
        assert run.message(name)["suppressed"] == 0, name
    assert run.message(HEARTBEAT)["published"] == stats["sensor"]["periods"]
    # BMS_Fault went on, every frame of it intact; the two changes of CmdClearFaultsSeq (1 to 2
    # to 3) were two clear requests, and the health task carried out each.
    assert all(frame.e2e.crc_ok for frame in run.decoded(FAULT))
    assert command_stats["clear_requests"] == 2 and stats["faults"]["clears"] == 2
    assert run.decoded(FAULT)[-1].raw["FaultClearCount"] == 2

    # On the bus: a stretch with heartbeats and no frame of the gated messages, and every one
    # of them before it and after it.
    ids = [frame.arbitration_id for frame in frames]
    protected_ids = {ID[name] for name in GATED}
    positions = [i for i, frame_id in enumerate(ids) if frame_id in protected_ids]
    gap = max(zip(positions, positions[1:]), key=lambda pair: pair[1] - pair[0])
    inside = ids[gap[0] + 1:gap[1]]
    assert set(inside) <= {ID[HEARTBEAT], ID[TASK_STATS], ID[NODE_INFO], ID[BOOT_INFO],
                           ID[COMMAND], ID[FAULT]}
    assert inside.count(ID[HEARTBEAT]) >= 3 * EVERY["temps"]
    assert protected_ids <= set(ids[:gap[0] + 1]) and protected_ids <= set(ids[gap[1]:])
    heartbeats = [frame for frame in frames if is_status(frame)]
    assert status_missing_since_start(heartbeats, max_step=None) == \
        run.message(HEARTBEAT)["queue"]["superseded"]


# The frames of test_frames_that_fail_the_check_are_counted_and_never_acted_on, in the order
# sent: (what it is, counter, telemetry on, Data ID or None, length, Profile 11 status or None
# for a frame the check refuses, acted on). Good frames carry telemetry on and a
# CmdClearFaultsSeq of 1; the others carry telemetry off and 99, so that a node that acted on
# one of them would stop its telemetry. Between two frames that are not OK come at least three
# OK ones: the state machine (window 4, VALID kept with 2 OKs and at most 1 error, app/command.h)
# then stays VALID, also if a late tester lets one supervision period pass without a frame.
OTHER_DATA_ID = 0x0120   # BMS_PackStatus's
BAD_FRAMES = (
    ("first frame: NODATA to INIT, not counted", 0, True, None, 8, "OK", False),
    ("second: INIT, one OK", 1, True, None, 8, "OK", False),
    ("third: two OKs, VALID", 2, True, None, 8, "OK", True),
    ("", 3, True, None, 8, "OK", True),
    ("wrong CRC", 4, False, None, 8, "ERROR", False),
    ("", 4, True, None, 8, "OK", True),
    ("", 5, True, None, 8, "OK", True),
    ("", 6, True, None, 8, "OK", True),
    ("repeated counter", 6, False, None, 8, "REPEATED", False),
    ("", 7, True, None, 8, "OK", True),
    ("", 8, True, None, 8, "OK", True),
    ("", 9, True, None, 8, "OK", True),
    ("counter jumps by 4", 13, False, None, 8, "WRONGSEQUENCE", False),
    ("", 14, True, None, 8, "OK", True),
    ("", 0, True, None, 8, "OK", True),
    ("", 1, True, None, 8, "OK", True),
    ("the Data ID of another message", 2, False, OTHER_DATA_ID, 8, "ERROR", False),
    ("", 2, True, None, 8, "OK", True),
    ("", 3, True, None, 8, "OK", True),
    ("", 4, True, None, 8, "OK", True),
    ("7 bytes", 5, False, None, 7, None, False),
    ("", 5, True, None, 8, "OK", True),
    ("", 6, True, None, 8, "OK", True),
    ("", 7, True, None, 8, "OK", True),
    ("one frame lost: within max_delta_counter 2", 9, True, None, 8, "OKSOMELOST", True),
    ("", 10, True, None, 8, "OK", True),
)


@pytest.mark.timing
def test_frames_that_fail_the_check_are_counted_and_never_acted_on(open_link, can_spec):
    link = open_link()
    recorder = Recorder(link.bus, [*link.before_first_status, link.first_status])
    tester = CommandStream(can_spec, recorder)
    try:
        for what, counter, on, data_id, length, _, _ in BAD_FRAMES:
            data = command(counter, on, 1 if on else 99, data_id=data_id, length=length)
            if what == "wrong CRC":
                data = bytes([data[0] ^ 0x01]) + data[1:]
            tester.send(data)
        recorder.heartbeats(2)
    finally:
        tester.close()
    assert link.node.stop() == EXIT_OK
    frames = recorder.stop()
    stats = link.node.stats()
    command_stats = stats["command"]
    print(command_stats)

    # The counts of frames acted on and the final state hold for a stream that kept its pace.
    check_command_cadence(frames, stats["sensor"]["periods"])
    check_stopped_in_time(frames, stats["sensor"]["periods"])
    statuses = [status for *_, status, _ in BAD_FRAMES]
    for status in ("OK", "OKSOMELOST", "REPEATED", "WRONGSEQUENCE", "ERROR"):
        assert command_stats["status"][status] == statuses.count(status), (status, command_stats)
    assert command_stats["check_errors"] == statuses.count(None)
    assert command_stats["frames"] == len(BAD_FRAMES)
    assert command_stats["status"]["NONEWDATA"] == command_stats["checks"] - len(BAD_FRAMES)
    # Only a period without a frame is judged "no new data". The frames came 20 ms of the
    # node's time apart, 500 ms from the first to the last, so at least four of the node's
    # 100 ms periods had frames.
    assert command_stats["status"]["NONEWDATA"] <= command_stats["cycles"] - 4
    acted = sum(1 for *_, carried_out in BAD_FRAMES if carried_out)
    assert command_stats["accepted"] == acted
    assert command_stats["ignored"] == len(BAD_FRAMES) - acted
    assert command_stats["sm_state"] == "VALID" and command_stats["sm_errors"] == 0

    # None of the rejected frames was acted on: the telemetry was never switched off, and the
    # last command carried out is a good one.
    assert command_stats["telemetry_on"] is True and command_stats["clear_faults_seq"] == 1
    # Every command carried out had CmdClearFaultsSeq 1: no clear was requested.
    assert command_stats["clear_requests"] == 0 and stats["faults"]["clears"] == 0
    for name in PROTECTED:
        assert stats["telemetry"]["messages"][name]["suppressed"] == 0, name


@pytest.mark.timing
def test_a_command_stream_that_stops_is_not_trusted_until_it_has_proved_itself_again(
        open_link, can_spec):
    link = open_link()
    recorder = Recorder(link.bus, [*link.before_first_status, link.first_status])
    tester = CommandStream(can_spec, recorder)
    try:
        for counter in range(5):                        # VALID from the third
            tester.send(command(counter, True, 1))
        # Six supervision periods without a frame: the node judges "no new data" in each, and
        # the state machine leaves VALID in the third.
        recorder.heartbeats(6 * CYCLE_MS[COMMAND] // SENSOR_PERIOD_MS)
        # In INVALID it takes 3 OKs and no error in the window: the first two frames that
        # come back are not acted on, the third is, and it switches the telemetry off.
        for counter in (5, 6, 7):
            tester.send(command(counter, False, 2))
        switched_off = recorder.until(heartbeats_without(GATED, 2 * EVERY["temps"]))
    finally:
        tester.close()
    assert link.node.stop() == EXIT_OK
    frames = recorder.stop()
    stats = link.node.stats()
    command_stats = stats["command"]
    print(command_stats)

    # Whether the third frame back is acted on, and the counts, depend on a stream that kept
    # its pace within each of its two parts; the final state on a node stopped in time.
    check_command_cadence(frames, stats["sensor"]["periods"], pauses=(5,))
    assert switched_off, "the third frame after the pause did not switch the telemetry off"
    check_stopped_in_time(frames, stats["sensor"]["periods"])
    assert command_stats["frames"] == 8 and command_stats["status"]["OK"] == 8
    assert command_stats["status"]["NONEWDATA"] >= 3
    assert command_stats["accepted"] == 4 and command_stats["ignored"] == 4
    assert command_stats["sm_state"] == "VALID"
    assert command_stats["telemetry_on"] is False and command_stats["clear_faults_seq"] == 2
    # The first command carried out after the stream was lost only set the reference: the
    # tester's change of CmdClearFaultsSeq while it was away (1 to 2) requested no clear.
    assert command_stats["clear_requests"] == 0 and stats["faults"]["clears"] == 0


# ------------------------------------------------------------------------------ the monitor


def test_the_monitor_prints_the_decoded_frames(open_link):
    link = open_link()
    out = subprocess.run(
        [sys.executable, "-m", "fcn_tools.monitor", link.spec, "--count", "250", "--seconds",
         "20", "--max-delta-counter", "14", "--id", "0x020", "--id", "0x030", "--id", "0x050"],
        capture_output=True, text=True, timeout=60)
    assert out.returncode == 0 and out.stderr == "", out.stderr
    lines = out.stdout.splitlines()
    assert len(lines) == 250
    names = [line.split()[2] for line in lines]
    assert set(names) <= {PACK_STATUS, HEARTBEAT, TEMPERATURES} and PACK_STATUS in names
    assert all("PackState=NORMAL" in line and "PackCurrentValid=1" in line
               for line in lines if PACK_STATUS in line)
    assert link.node.stop() == EXIT_OK
    messages = link.node.stats()["telemetry"]["messages"]
    for name in (PACK_STATUS, TEMPERATURES):
        mine = [line for line in lines if line.split()[2] == name]
        # The first frame the monitor sees comes some way into the stream, so its counter is
        # any step from the receiver's initial 14. From the second on each is one step after
        # the last, or a few more (OKSOMELOST) where the node replaced a value.
        later = mine[1:]
        assert sum(" e2e OK " not in line for line in later) <= \
            messages[name]["queue"]["superseded"], later
        assert all(" e2e OK " in line or " e2e OKSOMELOST " in line for line in later), later


# ------------------------------------------------------------------- periods on the wall clock


def to_monotonic(frames: list[can.Message]) -> list[can.Message]:
    """Restate the frames' time stamps, which were just read, in CLOCK_MONOTONIC seconds.

    The kernel stamps a received frame with CLOCK_REALTIME, which the host may set while the
    test runs: under WSL2 on the development laptop it jumped by about +165 ms against
    CLOCK_MONOTONIC in 4 of 36 runs of 3 s. A stamp taken across such a jump would make a long
    or a short period out of nothing. CLOCK_MONOTONIC is never set; the offset between the two
    clocks at the moment a frame is read is the one it was stamped with, unless the clock was
    set in the moment between its arrival and its reading."""
    offset = time.time() - time.monotonic()
    for frame in frames:
        frame.timestamp -= offset
    return frames


@pytest.mark.timing
def test_every_cyclic_message_keeps_its_period_on_the_wall_clock(open_link):
    # Only frames sent while the node runs: frames still waiting when it stops go out while
    # it shuts down, as late as that takes. Three node information frames are three seconds.
    link = open_link()
    offset = time.time() - time.monotonic()
    frames = to_monotonic([*link.before_first_status, link.first_status])
    infos = 0
    while infos < 3:
        message = link.bus.recv(timeout=5.0)
        assert message is not None, "the node went silent"
        frames.append(to_monotonic([message])[0])
        infos += message.arbitration_id == ID[NODE_INFO]
    print(f"the realtime clock was set by {(time.time() - time.monotonic() - offset) * 1e3:+.3f} "
          "ms while the frames came")
    assert link.node.stop() == EXIT_OK
    run = Recording(spec=link.spec, seconds=0, frames=frames, stats=link.node.stats(),
                    workdir=Path())

    for name in CYCLIC:
        decoded = run.decoded(name)
        # Each value's number from the counter it carries; a value the node replaced leaves a
        # gap in the numbers, not a long period.
        if name == HEARTBEAT:
            numbers = [frame.raw["PeriodCounter"] - 1 for frame in decoded]
        elif name in PROTECTED or name == TASK_STATS:
            counter, modulus = (("E2E_Counter", COUNTER_MODULUS) if name in PROTECTED
                                else ("TaskIndex", len(STATS_TASKS)))
            numbers = [0]
            for a, b in zip(decoded, decoded[1:]):
                numbers.append(numbers[-1] + ((b.raw[counter] - a.raw[counter]) % modulus
                                              or modulus))
        elif name == BOOT_INFO:
            # No number in it: one frame a second, and a value the node replaced shows as a
            # long gap.
            numbers = list(range(len(decoded)))
        else:
            numbers = [frame.raw["UptimeSeconds"] for frame in decoded]
        stamps = [frame.timestamp for frame in decoded]
        cycle = CYCLE_MS[name]
        mean_ms = (stamps[-1] - stamps[0]) * 1000.0 / (numbers[-1] - numbers[0])
        longest_ms = max((b - a) * 1000.0 for a, b in zip(stamps, stamps[1:]))
        check_timing(abs(mean_ms - cycle) <= cycle / 100, f"{name}: mean period {mean_ms:.3f} ms")
        check_timing(longest_ms < cycle + 20, f"{name}: longest gap {longest_ms:.1f} ms")

    # The CPU share each task's last frame reported, against the final statistics, taken at
    # most about 1.2 s later (six frames of 200 ms go round the tasks). A share is a run time
    # the host measures, so it is a timing figure: within 5 percentage points.
    tasks = {task["name"]: task for task in run.stats["tasks"]}
    total = run.stats["run_time_total_us"]
    last = {frame.raw["TaskIndex"]: frame.raw["TaskCpuShare"]
            for frame in run.decoded(TASK_STATS)}
    for index, share in last.items():
        final = 10000 * tasks[STATS_TASKS[index]]["run_time_us"] / total
        check_timing(abs(share - final) <= 500,
                     f"{STATS_TASKS[index]}: CPU share {share / 100:.2f} % on the bus, "
                     f"{final / 100:.2f} % at the end")
