"""The analysis of bench/isotp_throughput.py on constructed frames and counts with known answers:
the frame and event arithmetic of ISO-TP and the node's event budget, the cutting of the frames
on the wire into ISO-TP messages and the turns of the two sides, the line and the projection,
and the figures of one block size, of a response and of the summary."""
from __future__ import annotations

import importlib.util
import math
import sys

import pytest

from fcn_tools import paths, results
from fcn_tools.canbits import frame_bits


def _load(name: str):
    path = paths.REPO_ROOT / "bench" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"bench_{name}", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module      # dataclasses look their module up by name
    spec.loader.exec_module(module)
    return module


bench = _load("isotp_throughput")
REQ, RESP = bench.REQUEST_ID, bench.RESPONSE_ID
PAD = 0xCC


# ------------------------------------------------------------------------- frame arithmetic


@pytest.mark.parametrize("length, frames", [
    (1, (1, 0)), (7, (1, 0)), (8, (2, 1)), (13, (2, 1)), (14, (3, 2)),
    (1026, (147, 146)), (2050, (293, 292)), (4095, (586, 585)),
])
def test_a_message_takes_an_sf_or_an_ff_and_its_cfs(length, frames):
    assert bench.isotp_frames(length) == frames


@pytest.mark.parametrize("length", [0, 4096])
def test_a_length_classical_can_cannot_carry_is_refused(length):
    with pytest.raises(ValueError):
        bench.isotp_frames(length)


@pytest.mark.parametrize("cfs, block_size, expected", [
    (0, 16, 0), (1, 16, 1), (16, 16, 1), (17, 16, 2), (32, 16, 2), (33, 16, 3),
    (146, 16, 10), (292, 16, 19), (146, 0, 1), (5, 1, 5),
])
def test_a_receiver_sends_an_fc_after_the_ff_and_after_every_block_but_the_last(
        cfs, block_size, expected):
    assert bench.flow_controls(cfs, block_size) == expected


@pytest.mark.parametrize("data_bytes, events", [
    (1, 1), (5, 1), (6, 3), (16, 4), (64, 11), (1024, 157), (2048, 312),
])
def test_the_events_of_a_block_are_its_frames_and_the_reports_of_the_fcs(data_bytes, events):
    assert bench.block_events(data_bytes) == events


def test_the_event_floor_counts_the_blocks_events_and_its_responses_report_at_two_a_tick():
    # docs/diagnostics.md: 147 frames and the reports of 10 FCs, at least 79 ticks; with the
    # report of the response, (157 + 1) / 2.
    assert bench.event_floor_s(157) == pytest.approx(0.079)
    assert bench.event_floor_s(312) == pytest.approx(0.1565)
    assert bench.event_floor_s(1) == pytest.approx(0.001)
    assert bench.event_floor_s(4) == pytest.approx(0.0025)
    assert bench.event_floor_s(3, per_tick=1, tick_s=0.002) == pytest.approx(0.008)
    with pytest.raises(ValueError):
        bench.event_floor_s(3, per_tick=0)


def test_events_that_are_all_there_take_whole_ticks():
    assert bench.ticks_for_s(2) == pytest.approx(0.001)
    assert bench.ticks_for_s(3) == pytest.approx(0.002)
    assert bench.ticks_for_s(3, per_tick=1, tick_s=0.002) == pytest.approx(0.006)
    with pytest.raises(ValueError):
        bench.ticks_for_s(3, tick_s=0.0)


def test_a_frame_takes_its_stuffed_bits_and_the_intermission_on_the_bus():
    data = bytes([0x30, 0x10, 0x00] + [PAD] * 5)
    bits = frame_bits(RESP, data).frame_bits
    assert bench.frame_bus_s(RESP, data) == pytest.approx((bits + 3) / 500_000)
    # Without stuff bits an 8-byte base frame is 111 bits; it never takes less.
    assert bench.frame_bus_s(REQ, bytes(8)) >= 114 / 500_000


def test_pages_and_the_last_burst():
    assert bench.pages(1024) == 4 and bench.pages(2048) == 8 and bench.pages(64) == 0.25
    assert bench.last_burst(146, 10) == 2
    assert bench.last_burst(16, 1) == 16
    assert bench.last_burst(17, 2) == 1
    assert bench.last_burst(0, 0) == 0


# ------------------------------------------------------------------------------ the wire


def frame(t: float, can_id: int, *data: int) -> bench.WireFrame:
    payload = bytes(data)
    return bench.WireFrame(t, can_id, payload + bytes([PAD] * (8 - len(payload))))


def request_frames(data_bytes: int, counter: int, start: float, fc_after: int = 16,
                   cf_step: float = 0.0001, fc_wait: float = 0.002, react: float = 0.0002):
    """The frames of a TransferData of `data_bytes` from the tester at `start` and the node's
    FCs (after the FF and after every `fc_after` CFs but the last): the frames, the time of the
    last one, and the times of the FCs."""
    length = data_bytes + 2
    payload = bytes([0x36, counter]) + bytes(range(data_bytes))
    if length <= 7:
        return [frame(start, REQ, length, *payload)], start, []
    frames = [frame(start, REQ, 0x10 | (length >> 8), length & 0xFF, *payload[:6])]
    rest = payload[6:]
    t = start + fc_wait
    frames.append(frame(t, RESP, 0x30, 0x10, 0x00))
    fcs = [t]
    t += react
    sn = 1
    chunks = [rest[i:i + 7] for i in range(0, len(rest), 7)]
    for index, chunk in enumerate(chunks):
        frames.append(frame(t, REQ, 0x20 | sn, *chunk))
        sn = (sn + 1) & 0x0F
        last = t
        if (index + 1) % fc_after == 0 and index + 1 < len(chunks):
            t += fc_wait
            frames.append(frame(t, RESP, 0x30, 0x10, 0x00))
            fcs.append(t)
            t += react
        else:
            t += cf_step
    return frames, last, fcs


def test_frames_become_messages_with_the_other_sides_flow_controls():
    frames, last, fcs = request_frames(20, 1, 1.0)
    frames.append(frame(last + 0.005, RESP, 0x02, 0x76, 0x01))
    frames.append(frame(last + 0.006, REQ, 0x21, 0xAA))          # a CF nothing expects
    messages, strays = bench.isotp_messages(frames)
    assert strays == 1
    assert [(m.sender, m.length) for m in messages] == [("tester", 22), ("node", 2)]
    request = messages[0]
    assert len(request.frames) == 4 and request.complete       # FF and 3 CFs: 6 + 7 + 7 + 2
    assert [f.t for f in request.flow_controls] == fcs
    assert request.head[:2] == bytes([0x36, 0x01])
    assert messages[1].head == bytes([0x76, 0x01])


def test_a_block_is_cut_into_the_turns_of_the_two_sides_and_they_add_up():
    # 40 data bytes: 42 bytes, an FF and 6 CFs; an FC after the FF and one after CF 2 (the
    # constructed receiver's block of 2), so two bursts.
    frames, last, fcs = request_frames(40, 1, 10.0, fc_after=2, cf_step=0.0001,
                                       fc_wait=0.003, react=0.0002)
    response_t = last + 0.006
    frames.append(frame(response_t, RESP, 0x02, 0x76, 0x01))
    frames.append(frame(response_t + 0.0005, REQ, 0x01, 0x37))
    frames.append(frame(response_t + 0.001, RESP, 0x01, 0x77))
    messages, _ = bench.isotp_messages(frames)
    (timeline,) = bench.transfer_timelines(messages)
    assert (timeline.counter, timeline.data_bytes, timeline.frames) == (1, 40, 7)
    assert timeline.flow_controls == len(fcs) == 3
    assert timeline.node_fc_s == pytest.approx([0.003, 0.003, 0.003])
    assert timeline.tester_fc_s == pytest.approx([0.0002, 0.0002, 0.0002])
    assert timeline.tester_burst_s == pytest.approx([0.0001, 0.0001, 0.0001])
    assert timeline.node_final_s == pytest.approx(0.006)
    assert timeline.turnaround_s == pytest.approx(0.0005)
    assert timeline.on_wire_s == pytest.approx(response_t - 10.0)
    assert timeline.period_s == pytest.approx(response_t + 0.0005 - 10.0)
    assert timeline.node_s + timeline.tester_s == pytest.approx(timeline.period_s)
    assert timeline.response == bytes([0x76, 0x01])
    expected_bus = sum(bench.frame_bus_s(f.can_id, f.data) for f in frames[:-2])
    assert timeline.bus_s == pytest.approx(expected_bus)


def test_a_single_frame_block_is_the_nodes_turn_and_the_turnaround():
    frames = [frame(0.0, REQ, 0x07, 0x36, 0x05, 1, 2, 3, 4, 5),
              frame(0.002, RESP, 0x02, 0x76, 0x05),
              frame(0.0025, REQ, 0x07, 0x36, 0x06, 1, 2, 3, 4, 5)]
    messages, _ = bench.isotp_messages(frames)
    (first,) = bench.transfer_timelines(messages)       # the second has no answer: no block
    assert (first.data_bytes, first.flow_controls, first.node_fc_s) == (5, 0, [])
    assert first.node_final_s == pytest.approx(0.002)
    assert first.turnaround_s == pytest.approx(0.0005)
    assert first.period_s == pytest.approx(0.0025)
    assert first.node_s + first.tester_s == pytest.approx(first.period_s)
    # The last block has no next request: no period and no tester's turn.
    messages, _ = bench.isotp_messages(frames[:2])
    (only,) = bench.transfer_timelines(messages)
    assert only.period_s is None and only.tester_s is None


def test_a_request_without_an_answer_is_no_block():
    frames = [frame(0.0, REQ, 0x07, 0x36, 0x05, 1, 2, 3, 4, 5)]
    messages, _ = bench.isotp_messages(frames)
    assert bench.transfer_timelines(messages) == []


def test_a_multi_frame_response_is_cut_into_the_first_frame_the_fc_and_the_cfs():
    frames = [frame(0.0, REQ, 0x02, 0x19, 0x0A),
              frame(0.001, RESP, 0x10, 20, 0x59, 0x0A, 0xFF, 0, 0, 0),
              frame(0.0012, REQ, 0x30, 0x00, 0x00),
              frame(0.0015, RESP, 0x21, *range(7)),
              frame(0.0021, RESP, 0x22, *range(7))]
    messages, strays = bench.isotp_messages(frames)
    assert strays == 0
    (timeline,) = bench.response_timelines(messages, 0x19)
    assert (timeline.length, timeline.frames) == (20, 3)
    assert timeline.node_first_s == pytest.approx(0.001)
    assert timeline.tester_fc_s == pytest.approx(0.0002)
    assert timeline.node_cfs_s == pytest.approx(0.0009)
    assert timeline.total_s == pytest.approx(0.0021)
    assert timeline.cf_gaps_s == pytest.approx([0.0006])
    assert timeline.bus_s == pytest.approx(sum(bench.frame_bus_s(f.can_id, f.data)
                                               for f in frames))
    # An incomplete response is left out.
    messages, _ = bench.isotp_messages(frames[:-1])
    assert bench.response_timelines(messages, 0x19) == []


def test_the_bus_time_of_other_frames_counts_those_in_the_interval():
    others = [(1.0, 0.1), (2.0, 0.2), (3.0, 0.3), (4.0, 0.4)]
    assert bench.bus_time_between(others, 2.0, 4.0) == pytest.approx(0.5)
    assert bench.bus_time_between(others, 0.0, 1.0) == 0.0
    assert bench.bus_time_between([], 0.0, 9.0) == 0.0


# --------------------------------------------------------------------- figures from the parts


def test_the_line_through_points_on_a_line():
    fit = bench.fit_line([1, 2, 3, 10], [2.5, 3.0, 3.5, 7.0])
    assert fit["intercept"] == pytest.approx(2.0) and fit["slope"] == pytest.approx(0.5)
    assert fit["max_abs_residual"] == pytest.approx(0.0, abs=1e-12)
    assert fit["r2"] == pytest.approx(1.0) and fit["n"] == 4
    noisy = bench.fit_line([0, 1, 2], [0.0, 2.0, 1.0])
    assert noisy["slope"] == pytest.approx(0.5) and noisy["intercept"] == pytest.approx(0.5)
    assert noisy["max_abs_residual"] == pytest.approx(1.0) and noisy["r2"] == pytest.approx(0.25)
    for x, y in (([1, 2], [1, 2]), ([1, 1, 1], [1, 2, 3])):
        with pytest.raises(ValueError):
            bench.fit_line(x, y)


def test_a_projected_block_is_the_line_at_its_events_and_its_pages_of_writing():
    fit = {"intercept": 0.003, "slope": 0.0005}
    block = bench.project_block(fit, 2048, write_per_page_s=0.001)
    assert block["events"] == 312
    assert block["write_s"] == pytest.approx(0.008)
    assert block["period_s"] == pytest.approx(0.003 + 0.156 + 0.008)
    assert block["event_floor_s"] == pytest.approx(0.1565)
    assert block["bytes_per_s"] == pytest.approx(2048 / 0.167)


def test_an_image_takes_its_blocks_each_counted_whole():
    assert bench.image_time_s(262_144, 1024, 0.08) == pytest.approx(256 * 0.08)
    assert bench.image_time_s(262_144, 2048, 0.16) == pytest.approx(128 * 0.16)
    assert bench.image_time_s(1025, 1024, 1.0) == 2.0


def test_the_nodes_last_turn_in_its_parts():
    parts = bench.final_parts(146, 10, 0.0051, 0.0040)
    assert parts["last_frames_floor"] == pytest.approx(1.0)        # 2 CFs: one tick, in ms
    assert parts["rest"] == pytest.approx(0.1, abs=1e-9)
    assert bench.final_parts(0, 0, 0.002, 0.0005)["last_frames_floor"] == pytest.approx(1.0)
    assert bench.final_parts(146, 10, None, 0.004)["rest"] is None


def test_mean_and_central():
    assert bench.mean([1.0, None, 3.0]) == 2.0 and bench.mean([]) is None
    assert bench.central({"p50": 2.0, "mean": 3.0}) == 2.0
    assert bench.central({"p50": None, "mean": 3.0}) == 3.0


# ----------------------------------------------------------------- one block size, end to end


def constructed_raw(data_bytes: int, blocks: int, period: float = 0.01) -> dict:
    """The raw material of measure_block_size() for `blocks` blocks of `data_bytes`, a block
    every `period` s, the tester measuring each 0.0003 s longer than the wire shows."""
    frames, others, tester, starts = [], [], [], []
    t = 100.0
    plan = []
    for index in range(blocks):
        block_frames, last, _ = request_frames(data_bytes, index + 1, t)
        response = last + 0.004
        frames += block_frames + [frame(response, RESP, 0x02, 0x76, index + 1)]
        tester.append(response - t + 0.0003)
        starts.append(t - 50.0)               # the tester's monotonic clock: another origin
        others.append((t + 0.001, 0.0002))
        plan.append(data_bytes)
        t += period
    frames.append(frame(t, REQ, 0x01, 0x37))
    frames.append(frame(t + 0.001, RESP, 0x01, 0x77))
    starts.append(t - 50.0)
    return {
        "data_bytes": data_bytes, "image_bytes": data_bytes * blocks,
        "times": {"blocks_s": tester, "starts_s": starts, "transfer_s": blocks * period,
                  "erase_s": 0.05, "plan": plan},
        "frames": frames, "others": others,
        "node": {"build_id": "x", "tick_rate_hz": 1000, "events_per_tick": 2, "isotp_bs": 16,
                 "isotp_stmin": 0,
                 "download": {"state": "checked", "size": data_bytes * blocks,
                              "received": data_bytes * blocks, "blocks": blocks,
                              "repeated_blocks": 0, "committed": 1, "erase_us": 50_000,
                              "transfer_us": int(blocks * period * 1e6), "check_us": 2000},
                 "write_jobs": {"run": blocks, "total_ns": blocks * 2_000_000,
                                "max_ns": 2_500_000, "last_ns": 2_000_000, "last_status": "ok"},
                 "task": {"events": 0, "passes": 0, "budget_waits": 3 * blocks, "rx_lost": 0,
                          "rx_high_water": 3, "tx_done_lost": 0}},
    }


def test_one_block_size_gives_its_counts_and_its_distributions():
    figures = bench.analyse_block_size(constructed_raw(20, 12))
    assert (figures["message_bytes"], figures["frames_to_node"], figures["flow_controls"],
            figures["events"]) == (22, 4, 1, 5)
    assert figures["event_floor_ms"] == pytest.approx(3.0)
    assert (figures["blocks"], figures["full_blocks"], figures["full_blocks_decomposed"]) == (
        12, 12, 12)
    assert figures["clock_disagreements"]["blocks"] == 0 and figures["stray_frames"] == 0
    assert figures["response_pendings"] == 0
    wire = figures["wire"]
    assert wire["period_ms"]["n"] == 12 and wire["period_ms"]["mean"] == pytest.approx(10.0)
    assert wire["node_flow_control_each_ms"]["mean"] == pytest.approx(2.0)
    assert wire["tester_minus_wire_ms"]["mean"] == pytest.approx(0.3)
    assert wire["telemetry_bus_ms"]["mean"] == pytest.approx(0.2)
    assert figures["write_job_ms"]["mean"] == pytest.approx(2.0)
    assert figures["throughput"]["tester"] == pytest.approx(240 / 0.12)
    assert figures["throughput"]["node"] == pytest.approx(2000.0)
    assert figures["budget_waits_per_block"] == 3
    assert figures["raw"]["tester_block_ms"]["n"] == 12


def test_a_block_whose_wire_time_exceeds_the_testers_is_left_out_and_counted():
    raw = constructed_raw(20, 12)
    raw["times"]["blocks_s"][3] -= 0.002          # the tester saw less than the wire shows
    figures = bench.analyse_block_size(raw)
    assert figures["clock_disagreements"] == {"blocks": 1, "order": 0, "on_wire": 1,
                                              "period": 0}
    assert figures["full_blocks_decomposed"] == 11


def test_a_block_the_clock_stepped_in_is_left_out_and_counted():
    raw = constructed_raw(20, 12)
    # The listener's clock steps back 1.3 s from the response of block 5 on: block 5's stamps
    # go back, and its period and those after it are of the new clock, which only block 5 spans.
    frames = raw["frames"]
    fifth = next(i for i, f in enumerate(frames) if f.data[:3] == bytes([0x02, 0x76, 0x05]))
    raw["frames"] = frames[:fifth] + [bench.WireFrame(f.t - 1.3, f.can_id, f.data)
                                      for f in frames[fifth:]]
    figures = bench.analyse_block_size(raw)
    disagreements = figures["clock_disagreements"]
    assert disagreements["blocks"] == 1 and disagreements["order"] == 1
    assert figures["full_blocks_decomposed"] == 11
    assert figures["wire"]["period_ms"]["max"] == pytest.approx(10.0)


def test_a_block_whose_period_disagrees_with_the_testers_is_left_out_and_counted():
    raw = constructed_raw(20, 12)
    raw["times"]["starts_s"][6] += 0.006       # the tester started block 7 6 ms later
    figures = bench.analyse_block_size(raw)
    # Blocks 6 and 7: their periods on the wire and at the tester now differ by 6 ms.
    assert figures["clock_disagreements"] == {"blocks": 2, "order": 0, "on_wire": 0,
                                              "period": 2}


def test_block_clock_problems_names_each_reason():
    frames = bench.indexed([frame(0.0, REQ, 0x07, 0x36, 1, 1, 2, 3, 4, 5),
                            frame(0.003, RESP, 0x02, 0x76, 0x01),
                            frame(0.002, REQ, 0x07, 0x36, 2, 1, 2, 3, 4, 5)])
    messages, _ = bench.isotp_messages(frames)
    (timeline, ) = bench.transfer_timelines(messages)[:1]
    assert timeline.period_s == pytest.approx(0.002)
    assert bench.block_clock_problems(timeline, frames, 0.0031, 0.002) == ["order"]
    assert bench.block_clock_problems(timeline, frames, 0.001, 0.010) == ["order", "on_wire",
                                                                          "period"]
    assert bench.in_order(frames, 0, 1) and not bench.in_order(frames, 0, 2)


def test_a_block_missing_on_the_wire_fails_the_bench():
    raw = constructed_raw(20, 4)
    raw["times"]["plan"].append(20)
    raw["times"]["blocks_s"].append(0.01)
    with pytest.raises(AssertionError):
        bench.analyse_block_size(raw)


def size_figures(data_bytes: int, period_ms: float, write_ms: float) -> dict:
    events = bench.block_events(data_bytes)
    return {"data_bytes": data_bytes, "events": events,
            "event_floor_ms": bench.event_floor_s(events) * 1e3,
            "wire": {"period_ms": {"mean": period_ms, "p50": period_ms},
                     "bus_ms": {"mean": 1.0}},
            "write_job_ms": {"mean": write_ms}}


def test_the_projection_follows_the_line_through_the_measured_blocks():
    # Constructed so that period - write = 3 ms + 0.5 ms per event exactly.
    sizes = [size_figures(n, 3.0 + 0.5 * bench.block_events(n) + n / 256.0, n / 256.0)
             for n in (6, 64, 256, 1024)] + [size_figures(1, 99.0, 0.0)]
    projection = bench.fit_and_project(sizes)
    assert projection["projection"] is True and projection["fitted"] is True
    assert projection["fit"]["intercept_ms"] == pytest.approx(3.0)
    assert projection["fit"]["slope_ms_per_event"] == pytest.approx(0.5)
    assert projection["fit"]["points"] == 4                 # the single-frame block left out
    block = projection["block"]
    assert block["events"] == 312 and block["write_ms"] == pytest.approx(8.0)
    assert block["period_ms"] == pytest.approx(3.0 + 156.0 + 8.0)
    measured = 3.0 + 0.5 * 157 + 4.0
    image = projection["image"]
    assert image["transfer_s_measured_size"] == pytest.approx(256 * measured / 1e3, abs=1e-3)
    assert image["transfer_s_measured_size_on_the_line"] == pytest.approx(256 * measured / 1e3,
                                                                          abs=1e-3)
    assert image["transfer_s_projected_size"] == pytest.approx(128 * 167.0 / 1e3, abs=1e-3)
    # The line's saving: 128 blocks fewer, each the intercept and 2 events (one FF and one FC
    # fewer per 2048 bytes): 128 x (3 + 2 x 0.5) ms.
    assert image["saving_s"] == pytest.approx(0.512, abs=1e-3)
    assert bench.fit_and_project(sizes[:2])["fitted"] is False


def test_the_saving_is_the_lines_not_the_residual_of_the_measured_size():
    sizes = [size_figures(n, 3.0 + 0.5 * bench.block_events(n) + n / 256.0, n / 256.0)
             for n in (6, 64, 256)]
    sizes.append(size_figures(1024, 3.0 + 0.5 * 157 + 4.0 + 1.0, 4.0))   # 1 ms above the line
    image = bench.fit_and_project(sizes)["image"]
    assert image["transfer_s_measured_size"] > image["transfer_s_measured_size_on_the_line"]
    # Each figure is rounded to 1 ms on its own.
    assert image["saving_s"] == pytest.approx(image["transfer_s_measured_size_on_the_line"]
                                              - image["transfer_s_projected_size"], abs=2e-3)


def test_the_transports_are_compared_by_their_median_periods():
    first = {"transfer": [size_figures(1024, 80.0, 4.0)]}
    second = {"transfer": [size_figures(1024, 82.0, 4.0)]}
    difference = bench.transport_difference({"socketcan": first, "udp": second})
    (row,) = difference["period_p50"]
    assert row["difference_ms"] == pytest.approx(2.0)
    assert row["difference_percent"] == pytest.approx(2.5)
    assert bench.transport_difference({"udp": second}) is None


def test_the_response_figures_and_the_summary_carry_their_units():
    frames = []
    took = []
    for index in range(12):
        t = 10.0 + index * 0.01
        frames += [frame(t, REQ, 0x02, 0x19, 0x0A),
                   frame(t + 0.001, RESP, 0x10, 20, 0x59, 0x0A, 0xFF, 0, 0, 0),
                   frame(t + 0.0012, REQ, 0x30, 0x00, 0x00),
                   frame(t + 0.0015, RESP, 0x21, *range(7)),
                   frame(t + 0.0021, RESP, 0x22, *range(7))]
        took.append(0.0025)
    response = bench.analyse_responses({"took_s": took, "lengths": [20], "frames": frames,
                                        "node": {"build_id": "x"}})
    assert (response["response_bytes"], response["frames"], response["requests"],
            response["responses_on_the_wire"]) == (20, 3, 12, 12)
    assert response["wire"]["total_ms"]["p50"] == pytest.approx(2.1)
    assert response["throughput"]["p50"] == pytest.approx(20 / 0.0021, rel=1e-3)
    sizes = [bench.analyse_block_size(constructed_raw(n, 12)) for n in (6, 20, 64)]
    data = {"transports": {"udp": {"transfer": sizes, "response": response,
                                   "projection_2048": bench.fit_and_project(sizes)}}}
    summary = bench.summarise(data)
    results.check_summary(summary)
    group = summary["udp"]
    assert group["block_data_bytes"]["value"] == 64
    assert group["projected_2048_throughput"]["projection"] is True
    assert group["block_period"]["unit"] == "ms"
    assert math.isfinite(group["fit_slope"]["value"])
