"""The analysis of bench/node_timing.py (fcn_tools.latency): the sample of a frame from its E2E
counter, the latency of the protected frames, the observer's frames against the node's, the
periods at the observer, the CPU shares and the stack, on constructed data with known answers."""
from __future__ import annotations

import numpy as np
import pytest

from fcn_tools import latency
from fcn_tools.latency import MatchError
from fcn_tools.trace import SentFrames


def counter_data(counters) -> np.ndarray:
    """Frames whose byte 1 holds `counters` in its low nibble (and 0xA0 above, which the counter
    must ignore)."""
    data = np.zeros((len(counters), 8), dtype=np.uint8)
    data[:, 1] = 0xA0 | np.asarray(counters, dtype=np.uint8)
    return data


# --------------------------------------------------------------------- counters and publishes


def test_the_counter_is_the_low_nibble_of_byte_1():
    assert latency.e2e_counters(counter_data([0, 7, 14])).tolist() == [0, 7, 14]


def test_publish_indices_undo_the_wrap_and_count_the_gaps():
    # 0..14, then 0 again (publish 15), then 2 (publish 17: one superseded), then 1 (publish 31:
    # 13 superseded, a step of 14).
    counters = list(range(15)) + [0, 2, 1]
    indices = latency.publish_indices(counters)
    assert indices.tolist() == list(range(16)) + [17, 31]
    assert latency.gaps(indices) == 1 + 13
    assert latency.publish_indices([4, 5]).tolist() == [4, 5]   # the first is its counter
    assert latency.publish_indices([]).size == 0 and latency.gaps(np.array([])) == 0


@pytest.mark.parametrize("counters", [[3, 3], [1, 15], [16]])
def test_a_repeated_or_reserved_counter_is_refused(counters):
    with pytest.raises(MatchError):
        latency.publish_indices(counters)


# ------------------------------------------------------------------------- samples and jobs


def test_each_sample_takes_the_release_of_the_job_whose_first_pass_read_it():
    # Jobs [100, 200) and [1100, 1300), arrivals 90 and 1090, releases 95 and 1095. Sample 0 is
    # read at 150 (job 0); samples 1 and 2 at 1150 and 1250, both in job 1 (an overrun: the
    # second pass of the job read sample 2); sample 3 at 2000, outside every job.
    samples, overruns = latency.samples_of_jobs(
        np.array([0, 1, 2, 3]), np.array([150, 1150, 1250, 2000]),
        np.array([100, 1100]), np.array([200, 1300]), np.array([90, 1090]), np.array([95, 1095]))
    assert overruns == 1
    assert samples.number.tolist() == [0, 1, 2]
    assert samples.arrival_ns.tolist() == [90, 1090, -1]
    assert samples.release_ns.tolist() == [95, 1095, -1]
    assert samples.start_ns.tolist() == [100, 1100, -1]
    assert samples.released.tolist() == [True, True, False]


def test_samples_must_be_numbered_without_a_gap():
    with pytest.raises(MatchError):
        latency.samples_of_jobs(np.array([0, 2]), np.array([1, 2]), np.array([0]),
                                np.array([10]), np.array([0]), np.array([0]))


def ten_samples() -> latency.Samples:
    """Samples 0..9, released every 10,000 ns from 1000 (arrival 100 ns before), read 50 ns
    after the release; sample 5 was taken by an overrun pass."""
    release = 1000 + 10_000 * np.arange(10)
    released = np.arange(10) != 5
    return latency.Samples(number=np.arange(10), read_ns=release + 50,
                           arrival_ns=np.where(released, release - 100, -1),
                           release_ns=np.where(released, release, -1),
                           start_ns=np.where(released, release + 10, -1))


def frames_of(counters, eof) -> SentFrames:
    n = len(counters)
    return SentFrames(can_id=np.full(n, 0x40), flags=np.zeros(n, np.int64),
                      dlc=np.full(n, 8), data=counter_data(counters),
                      eof_ns=np.asarray(eof, np.int64), write_ns=np.asarray(eof, np.int64) + 7)


def test_a_frame_s_latency_runs_from_its_sample_s_release_and_arrival():
    # Every second sample: publishes 0, 1, 2, 3, 4 carry samples 0, 2, 4, 6, 8. Publish 3 is
    # missing on the bus (superseded). Each frame ends 400 ns after its sample's release.
    samples = ten_samples()
    eof = [1400, 21_400, 41_400, 81_400]
    found = latency.frame_latency("BMS_CellVoltages1", 0x40, frames_of([0, 1, 2, 4], eof), 2,
                                  samples)
    assert found.sample.tolist() == [0, 2, 4, 8]
    assert found.position.tolist() == [0, 1, 2, 3]
    assert found.from_release_ns.tolist() == [400] * 4
    assert found.from_arrival_ns.tolist() == [500] * 4
    assert found.write_ns.tolist() == [1407, 21_407, 41_407, 81_407]
    assert found.gaps == 1 and found.unreleased == 0 and found.outside == 0


def test_a_frame_of_an_overrun_sample_or_beyond_the_trace_s_samples_is_left_out():
    samples = ten_samples()
    # Publishes 5 (sample 5: an overrun pass), 6 and 10 (sample 10, after the last).
    found = latency.frame_latency("BMS_PackStatus", 0x20,
                                  frames_of([5, 6, 10], [52_000, 61_400, 200_000]), 1, samples)
    assert found.sample.tolist() == [6] and found.position.tolist() == [1]
    assert found.unreleased == 1 and found.outside == 1


def test_a_frame_that_ends_before_its_sample_was_read_is_a_wrong_match():
    with pytest.raises(MatchError, match="before its sample"):
        latency.frame_latency("BMS_PackStatus", 0x20, frames_of([1], [10_000]), 1, ten_samples())


def test_frames_matched_to_the_sample_before_their_own_are_refused_by_the_median():
    # The frames of samples 1 to 4, 400 ns after each release, taken one sample too early
    # (counters 0..3 where 1..4 were sent): no frame ends before "its" sample was read, but every
    # latency is a period (10,000 ns) too long, and the median says so.
    samples = ten_samples()
    early = frames_of([0, 1, 2, 3], [11_400, 21_400, 31_400, 41_400])
    found = latency.frame_latency("BMS_PackStatus", 0x20, early, 1, samples)
    assert found.from_release_ns.tolist() == [10_400] * 4
    with pytest.raises(MatchError, match="before their own"):
        latency.frame_latency("BMS_PackStatus", 0x20, early, 1, samples, period_ns=10_000)
    # One frame held up for longer than a period does not move the median.
    late_one = frames_of([0, 1, 2], [1_400, 11_400, 21_400 + 30_000])
    found = latency.frame_latency("BMS_PackStatus", 0x20, late_one, 1, samples, period_ns=10_000)
    assert found.from_release_ns.tolist() == [400, 400, 30_400]


def test_a_latency_beyond_the_bound_is_refused():
    late = 1000 + latency.MAX_LATENCY_NS + 1
    with pytest.raises(MatchError, match="after its"):
        latency.frame_latency("BMS_PackStatus", 0x20, frames_of([0], [late]), 1, ten_samples())


def test_a_sample_missing_between_the_trace_s_samples_is_refused():
    samples = ten_samples()
    holed = latency.Samples(*(np.delete(getattr(samples, name), 4) for name in
                              ("number", "read_ns", "arrival_ns", "release_ns", "start_ns")))
    with pytest.raises(MatchError, match="not among"):
        holed.lookup([4])


# ------------------------------------------------------------------------------ the observer


def test_the_observer_must_have_seen_exactly_the_frames_the_node_sent():
    data = counter_data([1, 2])
    latency.match_observed(data, [8, 8], data.copy(), np.array([8, 8]))
    with pytest.raises(MatchError, match="sent 2"):
        latency.match_observed(data, [8, 8], data[:1], [8])
    with pytest.raises(MatchError, match="length"):
        latency.match_observed(data, [8, 8], data, [8, 7])
    other = data.copy()
    other[1, 7] ^= 1
    with pytest.raises(MatchError, match="frame 1"):
        latency.match_observed(data, [8, 8], other, [8, 8])
    # Bytes beyond the DLC do not count.
    short = data.copy()
    short[:, 5] = 0x55
    latency.match_observed(data, [2, 2], short, [2, 2])


def test_the_periods_are_the_intervals_against_the_cycle_time():
    # A 10 ms frame at 0, 10.1, 20, 40 ms (a period missing) and 50 ms; the arrival at 40 ms is
    # not trusted, so the intervals 20-40 and 40-50 are left out.
    times = np.array([0, 10_100_000, 20_000_000, 40_000_000, 50_000_000])
    found = latency.interarrival(times, 10, np.array([True, True, True, False, True]))
    assert found["interval_ns"].tolist() == [10_100_000, 9_900_000]
    assert found["deviation_ns"].tolist() == [100_000, -100_000]
    assert found["long"] == 0 and found["untrusted"] == 2
    found = latency.interarrival(times, 10)
    assert found["long"] == 1 and found["untrusted"] == 0
    with pytest.raises(MatchError):
        latency.interarrival(np.array([5, 1]), 10)


# ---------------------------------------------------------------------------------- CPU, stack


def test_the_cpu_shares_are_run_time_over_the_counter():
    shares = latency.cpu_shares([{"name": "IDLE", "run_time_us": 900},
                                 {"name": "sensor", "run_time_us": 100}], 1000)
    assert shares == {"IDLE": 0.9, "sensor": 0.1}
    with pytest.raises(ValueError):
        latency.cpu_shares([], 0)
    assert latency.run_time_per_job(1000, 8) == 125.0
    assert latency.run_time_per_job(1000, 0) is None


def test_a_share_over_windows_of_different_lengths_is_pooled_not_averaged():
    # 10 % of a 1 s window and 40 % of a 3 s window: 1.3 s of 4 s, 32.5 %, not the mean 25 %.
    assert latency.pooled_share([100_000_000, 1_200_000_000],
                                [1_000_000_000, 3_000_000_000]) == pytest.approx(0.325)
    with pytest.raises(ValueError):
        latency.pooled_share([1], [])
    with pytest.raises(ValueError):
        latency.pooled_share([0], [0])


def test_the_stack_s_own_use_and_margin_and_the_deepest_run():
    tasks = [{"name": "sensor", "stack_peak_bytes": 6000, "stack_start_bytes": 4432,
              "stack_size_bytes": 4096},
             {"name": "gone", "stack_peak_bytes": -1, "stack_start_bytes": -1,
              "stack_size_bytes": -1}]
    uses = latency.stack_uses(tasks)
    assert [use.name for use in uses] == ["sensor"]
    assert uses[0].own_bytes == 1568 and uses[0].margin_bytes == 4096 - 1568
    deeper = latency.StackUse("sensor", 7000, 4432, 4096)
    deepest = latency.stack_maxima([uses, [deeper]])
    assert deepest["sensor"] is deeper and deeper.margin_bytes == 4096 - 2568
