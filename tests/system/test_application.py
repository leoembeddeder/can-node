"""The application of phase 2 as a whole, from outside: the tasks and the table the
schedulability check reads, the cyclic messages and their cycle times, the end-to-end protection
as an independent receiver sees it, and TESTER_Command switching the telemetry.

These are the integration checks of app/app.c. The codec, the conversions, the protection and
the command receiver have unit tests of their own (tests/unit); here the question is whether the
tasks put them together as app/app.c says. Assertions are on counts and on the node's own time
(heartbeats); a test that waits waits for a number of frames, never for a fixed time.
"""
from __future__ import annotations

import itertools
import json
import threading
import time
from pathlib import Path
from typing import Iterator

import can
import cantools
import pytest

from fcn_tools import e2e, sched
from fcn_tools.bus import ID_STATUS, is_status, open_bus, wait_for_id
from fcn_tools.node import EXIT_OK

DBC = Path(__file__).resolve().parents[2] / "dbc" / "fcn.dbc"

# The message set (dbc/fcn.dbc): identifier and Data ID of the protected messages.
PROTECTED = {0x020: 0x0120, 0x040: 0x0140, 0x041: 0x0141, 0x050: 0x0150}
ID_COMMAND = 0x100
COMMAND_DATA_ID = 0x0200

# The messages sent in a pass of the sensor task or with its sample (app/app.c, WHO SENDS
# WHAT), and the period of that task.
PER_SENSOR_PASS = {"BMS_Heartbeat", "BMS_PackStatus", "BMS_CellVoltages1", "BMS_CellVoltages2",
                   "BMS_Temperatures"}
SENSOR_PERIOD_MS = 10


@pytest.fixture(scope="module")
def dbc():
    return cantools.database.load_file(str(DBC))


def stats_of(link) -> dict:
    assert link.node.stop() == EXIT_OK
    return link.node.stats()


class CommandSender:
    """A tester sending TESTER_Command every 100 ms from a bus of its own, protected with
    fcn_tools.e2e (independent of the node's C code). `telemetry_on` may be changed while it
    runs; the next frame carries the new value."""

    def __init__(self, bus: can.BusABC, telemetry_on: bool, clear_faults_seq: int = 0) -> None:
        self.telemetry_on = telemetry_on
        self.clear_faults_seq = clear_faults_seq
        self.sent = 0
        self._bus = bus
        self._sender = e2e.Sender(e2e.Config(data_id=COMMAND_DATA_ID, data_length=64))
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        while not self._stop.is_set():
            payload = bytearray(8)
            payload[2] = 1 if self.telemetry_on else 0      # CmdTelemetryOn, bit 16
            payload[3] = self.clear_faults_seq & 0xFF      # CmdClearFaultsSeq, bits 24..31
            data = self._sender.protect(bytes(payload))
            self._bus.send(can.Message(arbitration_id=ID_COMMAND, data=data,
                                       is_extended_id=False))
            self.sent += 1
            self._stop.wait(0.1)

    def __enter__(self) -> "CommandSender":
        self._thread.start()
        return self

    def __exit__(self, *exc_info) -> None:
        self._stop.set()
        self._thread.join(timeout=5)


def heartbeats_without_protected_frames(bus: can.BusABC, count: int,
                                        timeout: float = 10.0) -> bool:
    """Read frames until `count` status frames in a row have arrived with no protected frame
    among them: the node has stopped its protected telemetry. False if `timeout` runs out."""
    run = 0
    for message in _frames(bus, timeout):
        if message.arbitration_id in PROTECTED:
            run = 0
        elif is_status(message):
            run += 1
            if run >= count:
                return True
    return False


def _frames(bus: can.BusABC, timeout: float) -> Iterator[can.Message]:
    """Every frame received until `timeout` runs out; the caller stops when it has enough."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message = bus.recv(timeout=max(0.0, deadline - time.monotonic()))
        if message is not None:
            yield message


# ------------------------------------------------------------------------------- the tasks


def test_the_task_table_in_the_statistics_is_schedulable_and_names_the_tasks(start_node):
    node = start_node("--duration-s", "1")
    assert node.wait(timeout=20) == EXIT_OK
    stats = node.stats()

    table = sched.parse_table(json.dumps(stats["task_table"]))
    assert table.wcet_basis == "design"
    analysis = sched.analyse(table)
    assert analysis.schedulable, sched.format_report(analysis, "task_table")

    # Every task of the node is in the table with the priority it runs at; the two entries
    # above the highest task priority are the interrupts.
    running = {task["name"]: task["priority"] for task in stats["tasks"] if task["name"] != "IDLE"}
    in_table = {task.name: task.priority for task in table.tasks}
    assert {name: in_table[name] for name in running} == running
    assert {name for name in in_table if name not in running} == {"can_irq", "tick_irq"}
    assert min(in_table["can_irq"], in_table["tick_irq"]) > max(running.values())


def test_every_cyclic_message_is_published_with_the_samples_or_periods_it_belongs_to(start_node):
    node = start_node("--duration-s", "2")
    assert node.wait(timeout=20) == EXIT_OK
    stats = node.stats()
    messages = stats["telemetry"]["messages"]
    sensor, process = stats["sensor"], stats["process"]

    # The sensor task sends the heartbeat in every period, and reads the cells in every second
    # and the temperatures in every tenth; the processing task publishes each protected
    # message with every sample that carries its measurements.
    assert messages["BMS_Heartbeat"]["published"] == sensor["periods"]
    assert sensor["reads"]["current"] == sensor["periods"]
    assert sensor["reads"]["cells"] == (sensor["periods"] + 1) // 2
    assert sensor["reads"]["temps"] == (sensor["periods"] + 9) // 10
    assert process["samples"] == sensor["periods"] == stats["sample_queue"]["posted"]
    assert messages["BMS_PackStatus"]["published"] == process["samples"]
    assert messages["BMS_CellVoltages1"]["published"] == sensor["reads"]["cells"]
    assert messages["BMS_CellVoltages2"]["published"] == sensor["reads"]["cells"]
    assert messages["BMS_Temperatures"]["published"] == sensor["reads"]["temps"]
    # The health task: task statistics every 200 ms, node information every second, 2 s long.
    assert messages["BMS_TaskStats"]["published"] == 10
    assert messages["BMS_NodeInfo"]["published"] == 2

    # In the node's own time, each at its cycle time. The messages of the sensor task's passes
    # leave out the periods it skipped after an overrun (app/app.c, sensor_task()).
    for name, message in messages.items():
        ticks = stats["tick"]["count"]
        if name in PER_SENSOR_PASS:
            ticks -= sensor["skipped_periods"] * SENSOR_PERIOD_MS
        expected = ticks / message["cycle_ms"]
        assert abs(message["published"] - expected) <= 2, (name, message, stats["tick"], sensor)
        assert message["failed"] == 0 and message["suppressed"] == 0, name
        assert message["queue"]["produced"] == message["published"], name

    # Without a tester the command receiver judged every period as "no new data": one receive
    # cycle per 100 ms of the node's time, each a check.
    command = stats["command"]
    assert command["frames"] == 0 and command["sm_state"] == "NODATA"
    assert command["status"]["NONEWDATA"] == command["cycles"] == command["checks"]
    assert abs(command["cycles"] - stats["tick"]["count"] / 100) <= 1, (command, stats["tick"])
    assert stats["can"]["driver"]["rx_cycles"] == command["cycles"]
    assert command["telemetry_on"] is True and stats["telemetry"]["enabled"] is True


# ------------------------------------------------------------------- end-to-end protection


def test_an_independent_receiver_accepts_every_protected_frame(open_link, dbc):
    link = open_link()
    receivers = {can_id: e2e.Receiver(e2e.Config(data_id=data_id, data_length=64))
                 for can_id, data_id in PROTECTED.items()}
    heard: dict[int, list[can.Message]] = {can_id: [] for can_id in PROTECTED}

    for message in itertools.chain(link.before_first_status, [link.first_status],
                                   _frames(link.bus, 10.0)):
        if message.arbitration_id in heard:
            heard[message.arbitration_id].append(message)
        if len(heard[0x050]) >= 5:      # 5 temperature frames: 500 ms of the node's time
            break
    stats = stats_of(link)

    for can_id, frames in heard.items():
        statuses = [receivers[can_id].check(bytes(frame.data)) for frame in frames]
        superseded = next(m for m in stats["telemetry"]["messages"].values()
                          if m["id"] == can_id)["queue"]["superseded"]
        # The tester heard the node from its first frame on: counter 0 first, then every value
        # one step after the last, unless the node replaced values a stalled host held up.
        assert statuses[0] == e2e.Status.OK, hex(can_id)
        bad = [s for s in statuses if s != e2e.Status.OK]
        assert len(bad) <= superseded, (hex(can_id), statuses, superseded)

    # The values, decoded with the DBC: the model's pack at rest, every channel valid.
    status = dbc.decode_message(0x020, bytes(heard[0x020][-1].data))
    assert status["PackState"] == "NORMAL"
    assert status["PackCurrentValid"] == 1 and status["PackVoltageValid"] == 1
    assert 29.0 < status["PackVoltage"] < 30.0
    cells = dbc.decode_message(0x040, bytes(heard[0x040][-1].data))
    assert all(3.6 < cells[f"Cell{n}Voltage"] < 3.8 for n in range(1, 5))
    # The model's 25.1, 24.8, 25.6 and 24.5 degC with up to 0.2 degC of noise, in whole degrees.
    temps = dbc.decode_message(0x050, bytes(heard[0x050][-1].data))
    assert all(24 <= temps[f"Temp{n}"] <= 26 for n in range(1, 5)), temps


def test_a_front_end_that_stops_answering_turns_into_sna_and_fault(open_link, dbc):
    """The sensor task passes a group the front end did not answer as invalid (lib/bms/bms.h),
    so the bus says "not available" instead of repeating the last values."""
    link = open_link()

    def next_frame(can_id: int) -> dict:
        message = wait_for_id(link.bus, can_id, timeout=5.0)
        assert message is not None, hex(can_id)
        return dbc.decode_message(can_id, bytes(message.data))

    with link.node.sim() as sim:
        sim.sensor_offline(True)
        # The temperatures are read every tenth period, the last group to see the fault: wait
        # for a frame of them that says so, then every frame after it says so too.
        for _ in range(20):
            temps = next_frame(0x050)
            if all(temps[f"Temp{n}"] == "SNA" for n in range(1, 5)):
                break
        assert all(temps[f"Temp{n}"] == "SNA" for n in range(1, 5)), temps
        status = next_frame(0x020)
        assert status["PackState"] == "FAULT"
        assert status["PackCurrentValid"] == 0 and status["PackVoltageValid"] == 0
        cells = next_frame(0x040)
        assert all(cells[f"Cell{n}Voltage"] == "SNA" for n in range(1, 5)), cells

        sim.sensor_offline(False)
        for _ in range(50):
            status = next_frame(0x020)
            if status["PackState"] == "NORMAL":
                break
        assert status["PackState"] == "NORMAL", status
    stats = stats_of(link)
    assert stats["sensor"]["unanswered"]["current"] > 0
    assert stats["sensor"]["unanswered"]["cells"] > 0
    assert stats["sensor"]["unanswered"]["temps"] > 0


# ------------------------------------------------------------------------- TESTER_Command


def test_a_valid_command_switches_the_protected_telemetry_off_and_on(open_link, can_spec):
    link = open_link()
    tester = open_bus(can_spec)
    try:
        with CommandSender(tester, telemetry_on=False, clear_faults_seq=7) as sender:
            # The node acts from the third frame on (app/command.h): then the protected
            # messages stop, while the heartbeat goes on.
            assert heartbeats_without_protected_frames(link.bus, 20), "telemetry did not stop"
            sender.telemetry_on = True
            assert wait_for_id(link.bus, 0x020, timeout=5.0) is not None, "it did not resume"
            assert wait_for_id(link.bus, 0x050, timeout=5.0) is not None
    finally:
        tester.shutdown()
    stats = stats_of(link)

    command, messages = stats["command"], stats["telemetry"]["messages"]
    assert command["accepted"] >= 2 and command["clear_faults_seq"] == 7
    assert command["status"]["OK"] >= 3 and command["status"]["ERROR"] == 0
    for name in ("BMS_PackStatus", "BMS_CellVoltages1", "BMS_CellVoltages2", "BMS_Temperatures"):
        assert messages[name]["suppressed"] > 0, name
        assert messages[name]["gated"] is True
    for name in ("BMS_Heartbeat", "BMS_TaskStats", "BMS_NodeInfo"):
        assert messages[name]["suppressed"] == 0 and messages[name]["gated"] is False, name
    assert stats["telemetry"]["enabled"] is True


def test_a_command_stream_that_stops_times_out_and_the_last_command_stays(open_link, can_spec):
    link = open_link()
    tester = open_bus(can_spec)
    try:
        with CommandSender(tester, telemetry_on=False):
            assert heartbeats_without_protected_frames(link.bus, 5)
    finally:
        tester.shutdown()
    # 60 heartbeats are 600 ms of the node's time: six supervision periods without a frame,
    # twice what it takes the state machine to leave VALID (the third).
    assert heartbeats_without_protected_frames(link.bus, 60), "telemetry came back by itself"
    stats = stats_of(link)

    command = stats["command"]
    assert command["sm_state"] == "INVALID", command
    assert command["status"]["NONEWDATA"] >= 3
    assert command["telemetry_on"] is False and stats["telemetry"]["enabled"] is False


def test_a_command_with_a_wrong_crc_is_not_carried_out(open_link, can_spec):
    link = open_link()
    tester = open_bus(can_spec)
    try:
        sender = e2e.Sender(e2e.Config(data_id=0x0120, data_length=64))   # PackStatus's Data ID
        for _ in range(10):
            data = sender.protect(bytes([0, 0, 0, 0, 0, 0, 0, 0]))       # telemetry off
            tester.send(can.Message(arbitration_id=ID_COMMAND, data=data, is_extended_id=False))
            # One frame per 10 heartbeats, the command's cycle time in the node's own time. The
            # node is stopped after the 10 heartbeats that follow the last frame, however late
            # the tester was: none of the counts below depends on when the frames came.
            for _ in range(10):
                assert wait_for_id(link.bus, ID_STATUS, timeout=5.0) is not None
    finally:
        tester.shutdown()
    stats = stats_of(link)

    command = stats["command"]
    assert command["frames"] == 10 and command["accepted"] == 0
    assert command["status"]["ERROR"] == 10
    assert command["sm_state"] == "NODATA" and command["telemetry_on"] is True
    assert stats["telemetry"]["messages"]["BMS_PackStatus"]["suppressed"] == 0
