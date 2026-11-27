"""The analysis of bench/bus_load.py (fcn_tools.busload): the length of every frame, the load per
identifier and in all, the DBC's nominal load and the pooling of several windows, on constructed
data with known answers."""
from __future__ import annotations

from types import SimpleNamespace

import numpy as np
import pytest

from fcn_tools import busload, canbits

EFF, RTR, ERR = 0x8000_0000, 0x4000_0000, 0x2000_0000


def test_a_frame_s_length_is_canbits_for_its_content():
    data = np.zeros((4, 8), dtype=np.uint8)
    data[0, :3] = [1, 2, 3]
    data[1, :8] = 0xFF
    data[2, :2] = [0xAA, 0x55]
    ids = np.array([0x123, 0x7FF, EFF | 0x1ABCDE, RTR | 0x10], dtype=np.uint32)
    dlc = np.array([3, 8, 2, 4])
    lengths = busload.frame_lengths(ids, dlc, data)
    assert lengths.tolist() == [
        canbits.frame_bits(0x123, bytes([1, 2, 3])).frame_bits,
        canbits.frame_bits(0x7FF, bytes([0xFF] * 8)).frame_bits,
        canbits.frame_bits(0x1ABCDE, bytes([0xAA, 0x55]), is_extended_id=True).frame_bits,
        canbits.frame_bits(0x10, b"", is_remote_frame=True, dlc=4).frame_bits]
    # Bytes beyond the DLC do not change the length, and an equal frame gets the same one.
    data[0, 5] = 0x77
    again = busload.frame_lengths(ids[[0, 0]], dlc[[0, 0]], data[[0, 0]])
    assert again.tolist() == [lengths[0], lengths[0]]


def test_an_error_frame_has_no_length():
    with pytest.raises(ValueError):
        busload.frame_lengths(np.array([ERR | 1]), np.array([8]), np.zeros((1, 8), np.uint8))


def test_the_load_of_an_identifier_counts_its_intermissions():
    # In 2 s at 1000 bit/s: id 1 twice with 97 and 101 bits, id 2 once with 47 bits.
    loads = busload.load_by_id(np.array([1, 2, 1]), np.array([97, 47, 101]), 2.0, 1000)
    one = loads[1]
    assert (one.frames, one.bits_min, one.bits_max, one.bits_mean) == (2, 97, 101, 99.0)
    assert one.frames_per_s == 1.0
    assert one.bits_per_s == (97 + 101 + 6) / 2 and one.load == 0.102
    assert one.load_without_intermission == 0.099
    assert loads[2].load == 0.025
    total, bare = busload.total_load(loads.values())
    assert total == pytest.approx(0.127) and bare == pytest.approx(0.1225)
    assert one.as_json()["load"] == 0.102
    with pytest.raises(ValueError):
        busload.load_by_id(np.array([1]), np.array([1]), 0.0, 1000)


def test_the_typical_length_is_the_mean_over_seeded_random_payloads():
    first = busload.typical_bits(0x20, False, 8, draws=50)
    assert first == busload.typical_bits(0x20, False, 8, draws=50)
    assert canbits.unstuffed_bits(False, False, 8) < first
    assert first < canbits.worst_case_bits(False, False, 8)
    assert busload.typical_bits(0x20, False, 8, draws=50, seed=1) != first


def message(name, frame_id, cycle, senders=("BMS",), length=8):
    return SimpleNamespace(name=name, frame_id=frame_id, cycle_time=cycle, senders=list(senders),
                           is_extended_frame=False, length=length)


def test_the_nominal_load_is_the_length_over_the_cycle_time():
    found = busload.nominal([message("A", 0x20, 10), message("B", 0x30, 0),
                             message("C", 0x100, 100, senders=("TESTER",))], draws=20,
                            senders=["BMS"])
    assert [entry.name for entry in found] == ["A"]
    entry = found[0]
    assert entry.worst_bits == canbits.worst_case_bits(False, False, 8) == 132
    assert entry.frames_per_s == 100.0
    assert entry.load(500_000, 132) == (132 + 3) * 100 / 500_000
    assert len(busload.nominal([message("C", 0x100, 100, senders=("TESTER",))], draws=5)) == 1


def test_measured_and_nominal_side_by_side():
    expected = [busload.Nominal("A", 0x20, 10.0, 8, 132, 113.0),
                busload.Nominal("B", 0x30, 100.0, 8, 132, 112.0)]
    measured = busload.load_by_id(np.array([0x20] * 50), np.array([117] * 50), 0.5, 500_000)
    found = busload.compare(measured, expected, 500_000)
    a = found["A"]
    assert a["measured_frames_per_s"] == 100.0 and a["frames_ratio"] == 1.0
    assert a["measured_load"] == round((117 + 3) * 100 / 500_000, 6)
    assert a["nominal_load_typical"] == round((113 + 3) * 100 / 500_000, 6)
    assert a["load_ratio_to_typical"] == round(120 / 116, 6)
    assert found["B"]["measured_frames_per_s"] == 0.0 and found["B"]["measured_mean_bits"] is None


def test_pooled_windows_sum_frames_and_bits_over_their_lengths():
    # Window 1: 1 s, id 0x7E0 10 frames of 100 bits; window 2: 3 s, 30 frames of 110 bits.
    def entry(window, frames, bits):
        load = busload.load_by_id(np.array([0x7E0] * frames), np.array([bits] * frames), window,
                                  1000)[0x7E0]
        return {"window_s": window, "by_id": {"0x7e0": load.as_json()},
                "groups": {"tester": {"frames_per_s": load.frames_per_s,
                                      "bits_per_s": load.bits_per_s, "load": load.load}}}
    pooled = busload.pool([entry(1.0, 10, 100), entry(3.0, 30, 110)], 1000)
    item = pooled["by_id"]["0x7e0"]
    assert pooled["window_s"] == 4.0 and item["frames"] == 40 and item["frames_per_s"] == 10.0
    assert item["bits_mean"] == 107.5 and (item["bits_min"], item["bits_max"]) == (100, 110)
    assert item["bits_per_s"] == (10 * 103 + 30 * 113) / 4
    assert item["load"] == round((10 * 103 + 30 * 113) / 4 / 1000, 6)
    assert pooled["groups"]["tester"]["frames_per_s"] == 10.0
    assert pooled["load"] == item["load"] and len(pooled["runs"]) == 2
    with pytest.raises(ValueError):
        busload.pool([{"window_s": 0.0, "by_id": {}, "groups": {}}], 1000)
