# SPDX-License-Identifier: MIT
"""fcn_tools.decode and fcn_tools.monitor: decoding with the DBC and the end-to-end check, on
frames written out by hand from the layout in docs/can-protocol.md. (What the node sends is
checked with them in tests/system/test_telemetry.py.)"""
from __future__ import annotations

import io
import subprocess
import sys
import time

import can
import pytest

from fcn_tools import e2e
from fcn_tools.bus import open_bus
from fcn_tools.decode import (Decoder, E2EVerdict, data_id, format_frame, format_unknown,
                              load_database)
from fcn_tools.monitor import main, monitor
from fcn_tools.ports import PortAllocator

DB = load_database()
PACK_STATUS_ID = 0x020
CELLS1_ID = 0x040


def frame(frame_id: int, data: bytes, timestamp: float = 12.5) -> can.Message:
    return can.Message(arbitration_id=frame_id, data=data, is_extended_id=False,
                       timestamp=timestamp)


def protected(data_id_: int, counter: int, payload: bytes) -> bytes:
    """`payload` (CRC and counter bytes included, as zeros) protected with fcn_tools.e2e."""
    data, _ = e2e.protect(e2e.Config(data_id=data_id_, data_length=8 * len(payload)), counter,
                          payload)
    return data


# BMS_PackStatus worked out by hand: PackCurrent -1234 (-12.34 A) as int16 in bytes 2..3,
# PackVoltage 29568 mV in bytes 4..5, then byte 6: PackState 1 (bits 48..49), PackFaultActive 0
# (bit 50), PackCurrentValid 1 (bit 51), PackVoltageValid 1 (bit 52) = 0x19.
PACK_STATUS_PAYLOAD = bytes([0, 0, 0x2E, 0xFB, 0x80, 0x73, 0x19, 0x00])


def pack_status(counter: int, data_id_: int = 0x0120) -> bytes:
    return protected(data_id_, counter, PACK_STATUS_PAYLOAD)


def test_the_protected_messages_and_their_data_ids_come_from_the_dbc():
    assert Decoder().protected == {0x010: 0x0110, 0x020: 0x0120, 0x040: 0x0140, 0x041: 0x0141,
                                   0x050: 0x0150, 0x100: 0x0200}
    assert data_id(DB.get_message_by_frame_id(0x030)) is None


def test_a_frame_written_by_hand_decodes_to_its_values():
    decoded = Decoder().decode(frame(PACK_STATUS_ID, pack_status(0)))
    assert decoded.name == "BMS_PackStatus" and decoded.frame_id == 0x020
    assert decoded.raw == {"E2E_Crc": pack_status(0)[0], "E2E_Counter": 0,
                           "PackCurrent": -1234, "PackVoltage": 29568, "PackState": 1,
                           "PackFaultActive": 0, "PackCurrentValid": 1, "PackVoltageValid": 1}
    assert decoded.signals["PackCurrent"] == pytest.approx(-12.34)
    assert decoded.signals["PackVoltage"] == pytest.approx(29.568)
    assert decoded.signals["PackState"] == "NORMAL"
    assert decoded.e2e == E2EVerdict(status=e2e.Status.OK, counter=0, crc_ok=True)
    assert decoded.error is None and decoded.timestamp == 12.5


def test_sna_and_value_table_entries_are_named_and_raw_values_are_kept():
    # BMS_CellVoltages1 from the codec's own example (tests/tools/can_db_cli.c): cells 2700, 0,
    # 4094 and 4095 raw, 12 bits each from bit 16. Its CRC byte is 0, which is wrong.
    data = bytes.fromhex("000E8C0A00FEFFFF")
    decoded = Decoder().decode(frame(CELLS1_ID, data))
    assert [decoded.raw[f"Cell{n}Voltage"] for n in range(1, 5)] == [2700, 0, 4094, 4095]
    assert decoded.signals["Cell1Voltage"] == pytest.approx(3.7)
    assert decoded.signals["Cell2Voltage"] == pytest.approx(1.0)
    assert decoded.signals["Cell3Voltage"] == pytest.approx(5.094)
    assert decoded.signals["Cell4Voltage"] == "SNA"
    assert decoded.e2e.status == e2e.Status.ERROR and not decoded.e2e.crc_ok
    assert decoded.e2e.counter == 14


def test_each_message_is_checked_by_a_receiver_of_its_own_in_the_order_given():
    decoder = Decoder()
    statuses = []
    for frame_id, data in [
            (PACK_STATUS_ID, pack_status(0)),
            (CELLS1_ID, protected(0x0140, 0, bytes(8))),    # another message: its own counter
            (PACK_STATUS_ID, pack_status(1)),
            (PACK_STATUS_ID, pack_status(1)),                # the same counter again
            (PACK_STATUS_ID, pack_status(3)),                # one lost
            (PACK_STATUS_ID, pack_status(4, data_id_=0x0140)),   # masquerade: another Data ID
            (PACK_STATUS_ID, pack_status(4)),
            (CELLS1_ID, protected(0x0140, 1, bytes(8)))]:
        verdict = decoder.decode(frame(frame_id, data)).e2e
        statuses.append((frame_id, verdict.status, verdict.crc_ok))
    assert statuses == [
        (0x020, e2e.Status.OK, True), (0x040, e2e.Status.OK, True),
        (0x020, e2e.Status.OK, True), (0x020, e2e.Status.REPEATED, True),
        (0x020, e2e.Status.WRONGSEQUENCE, True), (0x020, e2e.Status.ERROR, False),
        (0x020, e2e.Status.OK, True), (0x040, e2e.Status.OK, True)]


def test_the_tolerance_for_lost_frames_is_the_decoders():
    decoder = Decoder(max_delta_counter=2)
    assert decoder.decode(frame(PACK_STATUS_ID, pack_status(0))).e2e.status == e2e.Status.OK
    assert decoder.decode(frame(PACK_STATUS_ID, pack_status(2))).e2e.status == \
        e2e.Status.OKSOMELOST
    assert decoder.decode(frame(PACK_STATUS_ID, pack_status(5))).e2e.status == \
        e2e.Status.WRONGSEQUENCE


def test_a_frame_of_the_wrong_length_has_no_signals_and_fails_the_check():
    decoder = Decoder()
    decoded = decoder.decode(frame(PACK_STATUS_ID, pack_status(0)[:7]))
    assert decoded.signals == {} and decoded.raw == {}
    assert decoded.error == "7 data bytes, the DBC says 8"
    assert decoded.e2e.status == e2e.Status.ERROR and decoded.e2e.counter == -1
    # The receiver's state is kept: the next frame, counter 0, is the first one it accepts.
    assert decoder.decode(frame(PACK_STATUS_ID, pack_status(0))).e2e.status == e2e.Status.OK


def test_unprotected_and_signal_less_messages_and_foreign_frames():
    decoder = Decoder()
    heartbeat = decoder.decode(frame(0x030, bytes([7, 0, 0, 0, 82, 0, 0, 0])))
    assert heartbeat.e2e is None
    assert heartbeat.signals == {"PeriodCounter": 7, "TickCount": 82}

    pong = decoder.decode(frame(0x6F1, b"\x01\x02\x03"))
    assert (pong.name, pong.signals, pong.e2e, pong.error) == ("BMS_Pong", {}, None, None)

    assert decoder.decode(frame(0x123, bytes(8))) is None
    assert decoder.decode(can.Message(arbitration_id=0x020, data=bytes(8),
                                      is_extended_id=True)) is None
    assert decoder.decode(can.Message(arbitration_id=0x020, is_remote_frame=True, dlc=8,
                                      is_extended_id=False)) is None


def test_a_decoded_frame_is_one_line_with_units_and_the_verdict():
    decoder = Decoder()
    line = format_frame(decoder.decode(frame(PACK_STATUS_ID, pack_status(0))), DB)
    assert line == ("        12.500000  020  BMS_PackStatus      e2e OK             c=0   "
                    "PackCurrent=-12.34 A  PackVoltage=29.568 V  PackState=NORMAL  "
                    "PackFaultActive=0  PackCurrentValid=1  PackVoltageValid=1")
    line = format_frame(decoder.decode(frame(CELLS1_ID, bytes.fromhex("000E8C0A00FEFFFF"))), DB)
    assert line == ("        12.500000  040  BMS_CellVoltages1   e2e ERROR (CRC)    c=14  "
                    "Cell1Voltage=3.700 V  Cell2Voltage=1.000 V  Cell3Voltage=5.094 V  "
                    "Cell4Voltage=SNA")
    line = format_frame(decoder.decode(frame(0x050, protected(0x0150, 0, bytes(
        [0, 0, 65, 0, 255, 254, 0, 0])))), DB)
    assert line.endswith("Temp1=25 degC  Temp2=-40 degC  Temp3=SNA  Temp4=214 degC")
    line = format_frame(decoder.decode(frame(0x030, bytes([7, 0, 0, 0, 82, 0, 0, 0]))), DB)
    assert line == "        12.500000  030  BMS_Heartbeat       PeriodCounter=7  TickCount=82 tick"
    assert format_unknown(frame(0x123, b"\xAB\xCD")) == \
        "        12.500000  123  (not in the DBC)    base, 2 bytes: ab cd"


class ListBus:
    """Hands out the given frames, then nothing."""

    def __init__(self, frames):
        self.frames = list(frames)

    def recv(self, timeout=None):
        if self.frames:
            return self.frames.pop(0)
        time.sleep(min(timeout or 0.0, 0.01))
        return None


def test_the_monitor_prints_the_frames_asked_for_with_their_verdicts():
    frames = [frame(0x030, bytes(8)), frame(PACK_STATUS_ID, pack_status(0)),
              frame(0x030, bytes(8)), frame(PACK_STATUS_ID, pack_status(1)),
              frame(PACK_STATUS_ID, pack_status(1))]
    out = io.StringIO()
    printed = monitor(ListBus(frames), Decoder(), out, count=2, seconds=1.0,
                      identifiers=frozenset({PACK_STATUS_ID}))
    lines = out.getvalue().splitlines()
    assert printed == 2 and len(lines) == 2
    assert all(" 020  BMS_PackStatus" in line and " e2e OK " in line for line in lines)

    # Without a count it runs for the time given; the repeated frame is REPEATED.
    out = io.StringIO()
    started = time.monotonic()
    assert monitor(ListBus(frames), Decoder(), out, seconds=0.3) == 5
    assert time.monotonic() - started >= 0.3
    assert " e2e REPEATED " in out.getvalue().splitlines()[-1]


def test_the_monitor_on_a_bus():
    ports = PortAllocator().take()
    spec = f"udp:{ports.bus}"
    try:
        process = subprocess.Popen(
            [sys.executable, "-m", "fcn_tools.monitor", spec, "--count", "3",
             "--max-delta-counter", "2", "--seconds", "20"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        sender = open_bus(spec)
        try:
            # Every other counter, until the monitor has its three frames (it may join the bus
            # after the first few): one lost frame each time, which max_delta_counter 2 allows.
            sent = 0
            while process.poll() is None and sent < 2000:
                sender.send(frame(PACK_STATUS_ID, pack_status(2 * sent % 15)))
                sent += 1
                time.sleep(0.01)
        finally:
            sender.shutdown()
        stdout, stderr = process.communicate(timeout=20)
    finally:
        ports.release()
    assert process.returncode == 0 and stderr == ""
    lines = stdout.splitlines()
    assert len(lines) == 3 and all("BMS_PackStatus" in line for line in lines)
    assert all(" e2e OKSOMELOST " in line for line in lines[1:]), lines


@pytest.mark.parametrize("argv, message", [
    (["udp", "--count", "0"], "--count must be at least 1"),
    (["udp", "--id", "0x800"], "is not an 11-bit identifier"),
    (["udp", "--max-delta-counter", "15"], "invalid choice"),
])
def test_the_monitor_refuses_a_wrong_command_line(argv, message, capsys):
    with pytest.raises(SystemExit) as stop:
        main(argv)
    assert stop.value.code == 2
    assert message in capsys.readouterr().err


def test_the_monitor_says_when_it_cannot_open_the_bus(capsys):
    assert main(["carrier-pigeon"]) == 1
    assert "monitor: cannot open carrier-pigeon" in capsys.readouterr().err
