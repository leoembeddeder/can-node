"""The observer of the benches (fcn_tools.capture): the offset of the wall clock, the frames' times
on CLOCK_MONOTONIC across steps of the wall clock, the windows of a run, the file and its
sidecar, on constructed data; and one capture of a UDP bus with frames a test sends."""
from __future__ import annotations

import json
import time

import can
import numpy as np
import pytest

from fcn_tools import capture, udpbus
from fcn_tools.capture import CAPTURE_DTYPE, OFFSET_NOISE_NS
from fcn_tools.ports import PortAllocator


def test_the_offset_is_taken_from_the_narrowest_bracket():
    # MONOTONIC readings in pairs around a REALTIME reading; the second try has the narrowest
    # bracket (10 ns, around 1005): offset = 5000 - 1005.
    monotonic = iter([0, 100, 1000, 1010, 2000, 2050]).__next__
    realtime = iter([4000, 5000, 7000]).__next__
    found = capture.clock_offset(3, monotonic=monotonic, realtime=realtime)
    assert (found.offset_ns, found.bracket_ns, found.monotonic_ns) == (3995, 10, 1005)
    assert found.as_json() == {"offset_ns": 3995, "bracket_ns": 10, "monotonic_ns": 1005}


def frames(realtime, read, offset, ids=None, brackets=None) -> np.ndarray:
    out = np.zeros(len(realtime), dtype=CAPTURE_DTYPE)
    out["realtime_ns"], out["read_ns"], out["offset_ns"] = realtime, read, offset
    out["bracket_ns"] = 100 if brackets is None else brackets
    if ids is not None:
        out["can_id"] = ids
    return out


def test_a_frame_s_time_is_put_on_monotonic_by_the_offset_read_with_it():
    # The wall clock steps by 1.79 s between the second and the third frame: the third frame
    # may have arrived before the step and is not trusted; the fourth, which arrived after the
    # third was read, is again. A frame whose time would come after its own read is not trusted
    # either (the last).
    step = 1_790_000_000
    found = frames(realtime=[1_100, 2_100, 3_100 + step, 4_300 + step, 9_000],
                   read=[1_200, 2_200, 3_200, 4_200, 5_000],
                   offset=[1_000, 1_000 + OFFSET_NOISE_NS, 1_000 + step, 1_000 + step, 1_000])
    times, trusted = capture.to_monotonic(found)
    assert times.tolist() == [100, 2_100 - 1_000 - OFFSET_NOISE_NS, 2_100, 3_300, 8_000]
    assert capture.clock_steps(found["offset_ns"]).tolist() == [False, False, True, False, True]
    assert trusted.tolist() == [True, True, False, True, False]
    assert capture.clock_steps(np.array([])).size == 0
    assert capture.to_monotonic(frames([], [], []))[1].size == 0


def test_a_backlog_read_across_a_step_is_not_trusted_before_the_read_that_saw_the_step():
    # The capture did not run from 2_000 to 10_000 (monotonic). Three frames arrived meanwhile,
    # at 3_000, 4_000 and 9_000, and the wall clock stepped forward by 1 s at 5_000, between the
    # second and the third. The capture read all three at 10_000 to 10_200, each with the new
    # offset. The first one read shows the step; the second arrived before the step too, and
    # with the new offset its time is 1 s early: no other rule sees that. The third arrived
    # after the step, but before the read that saw it, so it may as well have arrived before:
    # not trusted either. The frame after the backlog is trusted again.
    step = 1_000_000_000
    found = frames(realtime=[1_100 + 1_000, 3_000 + 1_000, 4_000 + 1_000, 9_000 + 1_000 + step,
                             11_000 + 1_000 + step],
                   read=[1_200, 10_000, 10_100, 10_200, 11_100],
                   offset=[1_000, 1_000 + step, 1_000 + step, 1_000 + step, 1_000 + step])
    times, trusted = capture.to_monotonic(found)
    assert times.tolist() == [1_100, 3_000 - step, 4_000 - step, 9_000, 11_000]
    assert capture.clock_steps(found["offset_ns"]).tolist() == [False, True, False, False, False]
    assert trusted.tolist() == [True, False, False, False, True]


def test_an_offset_read_with_a_wide_bracket_is_no_evidence_of_a_step():
    # The second reading was interrupted (a bracket of 50 us) and is off by 25 us: the third
    # frame is compared with the first, the last good reading, and the second frame takes the
    # offset of the good readings around it, which agree.
    found = frames(realtime=[1_100, 2_100, 3_100], read=[1_200, 2_200, 3_200],
                   offset=[1_000, 26_000, 1_000], brackets=[100, 50_000, 100])
    assert capture.clock_steps(found["offset_ns"], found["bracket_ns"]).tolist() == \
        [False, False, False]
    times, trusted = capture.to_monotonic(found)
    assert times.tolist() == [100, 1_100, 2_100]
    assert trusted.tolist() == [True, True, True]


def test_a_wide_bracket_is_trusted_only_between_good_readings_that_bracket_no_step():
    # Wide brackets (50 us) at the start, before a step of 1 s seen by the fourth reading, and
    # at the end: the first has no good reading before it, the third's next good reading saw a
    # step (it may have arrived on either side of it), the last has no good reading after it.
    # The sixth lies between two good readings that agree after the step, and takes their
    # offset. Every frame arrived 100 ns before its read; the wall clock is 1_000 ns ahead of
    # MONOTONIC before the step.
    step = 1_000_000_000
    read = [1_200, 2_200, 3_200, 4_200, 5_200, 6_200, 7_200, 8_200]
    true_offset = [1_000] * 3 + [1_000 + step] * 5
    found = frames(realtime=[r - 100 + o for r, o in zip(read, true_offset)], read=read,
                   offset=[7_000, 1_000, 9_000, 1_000 + step, 1_000 + step, 5_000 + step,
                           1_000 + step, 3_000],
                   brackets=[50_000, 100, 50_000, 100, 100, 50_000, 100, 50_000])
    assert capture.clock_steps(found["offset_ns"], found["bracket_ns"]).tolist() == \
        [False, False, False, True, False, False, False, False]
    times, trusted = capture.to_monotonic(found)
    assert times[[1, 3, 4, 5, 6]].tolist() == [2_100, 4_100, 5_100, 6_100, 7_100]
    assert trusted.tolist() == [False, True, False, False, True, True, True, False]


def test_identifiers_drop_the_flags():
    found = frames([0, 0], [0, 0], [0, 0], ids=[0x8000_1234, 0x4000_07FF])
    assert capture.identifiers(found).tolist() == [0x1234, 0x7FF]


def test_the_steady_window_starts_a_settling_time_after_the_first_marker():
    ids = np.array([0x30, 0x20, 0x30, 0x30])
    times = np.array([1_000, 1_500, 2_000, 9_000])
    assert capture.steady_window(ids, times, 0x30, 500) == (1_500, 9_000)
    with pytest.raises(ValueError):
        capture.steady_window(ids, times, 0x30, 8_000)
    with pytest.raises(ValueError):
        capture.steady_window(ids, times, 0x99, 0)


def test_the_exchange_window_runs_from_the_first_request_to_the_last_response():
    ids = np.array([0x30, 0x7E0, 0x7E8, 0x7E0, 0x7E8, 0x30])
    times = np.array([0, 10, 20, 30, 40, 50])
    assert capture.exchange_window(ids, times, 0x7E0, 0x7E8) == (10, 40)
    with pytest.raises(ValueError):
        capture.exchange_window(ids, times, 0x7E0, 0x123)


def test_only_frames_whose_time_is_trusted_mark_the_edges_of_a_window():
    # The first heartbeat was read across a step of the wall clock and its time is a second
    # early (as in a rehearsal run, where it put the start of a flood run's window into the
    # node's start-up); the last response's time is a second late. Neither marks an edge.
    step = 1_000_000_000
    ids = np.array([0x30, 0x30, 0x7E0, 0x30, 0x7E8, 0x7E8, 0x30])
    times = np.array([1_000 - step, 2_000, 2_500, 3_000, 3_500, 4_500 + step, 9_000])
    trusted = np.array([False, True, True, True, True, False, True])
    assert capture.steady_window(ids, times, 0x30, 500) == (1_500 - step, 9_000)
    assert capture.steady_window(ids, times, 0x30, 500, trusted) == (2_500, 9_000)
    assert capture.exchange_window(ids, times, 0x7E0, 0x7E8) == (2_500, 4_500 + step)
    assert capture.exchange_window(ids, times, 0x7E0, 0x7E8, trusted) == (2_500, 3_500)
    with pytest.raises(ValueError, match="trusted"):
        capture.steady_window(ids, times, 0x30, 0, np.zeros(ids.size, dtype=bool))


def test_read_checks_the_file_against_its_sidecar(tmp_path):
    path = tmp_path / "c.bin"
    frames([1, 2], [3, 4], [0, 0]).tofile(path)
    (tmp_path / "c.bin.json").write_text(json.dumps({"schema": capture.SCHEMA, "frames": 2}))
    found, sidecar = capture.read(path)
    assert found["read_ns"].tolist() == [3, 4] and sidecar["frames"] == 2
    (tmp_path / "c.bin.json").write_text(json.dumps({"schema": capture.SCHEMA, "frames": 3}))
    with pytest.raises(ValueError, match="holds 2"):
        capture.read(path)
    (tmp_path / "c.bin.json").write_text(json.dumps({"schema": "other", "frames": 2}))
    with pytest.raises(ValueError, match="sidecar"):
        capture.read(path)


def test_the_observer_captures_every_frame_of_a_udp_bus_in_order(tmp_path):
    ports = PortAllocator().take()
    try:
        spec = f"udp:{ports.bus}"
        with capture.Observer(spec, tmp_path / "bus.capture") as observer:
            sender = udpbus.UdpBus(channel=ports.bus)
            try:
                for i in range(50):
                    sender.send(can.Message(arbitration_id=0x100 + i, data=bytes([i] * (i % 9)),
                                            is_extended_id=False))
                sender.send(can.Message(arbitration_id=0x1ABCDE, data=b"\x01",
                                        is_extended_id=True))
            finally:
                sender.shutdown()
            time.sleep(0.2)
            found, sidecar = observer.stop()
    finally:
        ports.release()
    assert sidecar["frames"] == 51 and sidecar["kernel_drops"] == 0
    assert sidecar["malformed"] == 0 and sidecar["can"] == spec
    assert capture.identifiers(found).tolist() == [0x100 + i for i in range(50)] + [0x1ABCDE]
    assert found["dlc"].tolist() == [i % 9 for i in range(50)] + [1]
    assert all(bytes(found["data"][i][:i % 9]) == bytes([i] * (i % 9)) for i in range(50))
    times, trusted = capture.to_monotonic(found)
    # The times are kernel times on CLOCK_MONOTONIC, each before its read.
    assert np.all(times[trusted] <= found["read_ns"][trusted])
