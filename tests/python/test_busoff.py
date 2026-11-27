# SPDX-License-Identifier: MIT
"""fcn_tools.busoff, the analysis of the bus-off bench, and the bench's summary, on constructed
trials whose every time is chosen here: the figures must come out to the nanosecond, and every
rule the analysis checks must catch the record that breaks it. Also the client side of
can.trace (fcn_tools.simctl) against a small server that pages its records."""
from __future__ import annotations

import importlib.util
import socket
import sys
import threading

import cantools
import pytest

from fcn_tools import busoff, decode, paths, results
from fcn_tools.simctl import SimControl, SimError, TraceRecord, parse_trace_record

BITRATE = 500_000
BIT = 2_000            # ns per bit at 500 kbit/s
T0 = 10_000_000_000    # the pass that applied the fault


def _load_bench():
    path = paths.REPO_ROOT / "bench" / "bus_off.py"
    spec = importlib.util.spec_from_file_location("bench_bus_off", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


bench = _load_bench()


def record(seq, kind, model, pass_=None, *, attempts=100, value=0, state=0, old=None, tec=0,
           rec=0, start=0, submit=0, frames=0) -> TraceRecord:
    return TraceRecord(seq=seq, kind=kind, model_ns=model,
                       pass_ns=model if pass_ is None else pass_, start_ns=start,
                       submit_ns=submit, attempts_failed=attempts, value=value, state=state,
                       old_state=state if old is None else old, tec=tec, rec=rec, frames=frames)


#: The node's frames before the window of heard_around(), and so the number of the first frame
#: after the recovery in it: the 40 heartbeats before the fault come first.
OFFSET = 1000
FIRST_NUMBER = OFFSET + 40 + 1


def log_line(t_ns: int, level: str, tag: str, text: str) -> str:
    assert t_ns % 1000 == 0, "the log has microseconds"
    seconds, micros = divmod(t_ns // 1000, 1_000_000)
    return f"[{seconds:6d}.{micros:06d}] {level} {tag}: {text}"


class Trial:
    """A constructed trial: a short (or 32 injected errors) at 500 kbit/s with every time known.
    The knobs make one thing wrong at a time; `reload_ns` is a frame loaded into an idle bus
    (after a recall of the mailboxes) that much after the bus became free, at the third attempt
    after error passive, which then starts at the driver's time."""

    def __init__(self, fault="short", *, holdoff_ns=100_800_000, sequence_extra_ns=0,
                 bad_tec_at=None, removal_late=False, resyncs=0, passive_bits=26,
                 drop_first_tx=False, attempts_status=None, reload_ns=0):
        self.fault = fault
        attempts_total = busoff.ATTEMPTS_TO_BUS_OFF[fault]
        passive_at = 8 if fault == "short" else 16
        active_bits = 18 if fault == "short" else 80
        records, seq = [], 0

        def add(*args, **kwargs):
            nonlocal seq
            records.append(record(seq, *args, **kwargs))
            seq += 1

        kind, value = busoff.FAULT_RECORD[fault]
        add(kind, T0, value=value)
        s1 = T0 + 1_000_000
        end = s1 - 3 * BIT
        for k in range(1, attempts_total + 1):
            if k <= passive_at:
                end = s1 + ((k - 1) * active_bits + (active_bits - 3)) * BIT
            else:
                end += passive_bits * BIT + (reload_ns if k == passive_at + 3 else 0)
            tec = busoff.tec_after_attempts(fault, k)
            if k == bad_tec_at:
                tec += 8
            if k == passive_at:
                self.passive = end
                add("state", end, end + 10_000, attempts=100 + k, state=1, old=0, tec=tec)
            elif k == attempts_total:
                self.bus_off = end
                add("state", end, end + 7_000, attempts=100 + k, state=2, old=1, tec=tec)
            else:
                add("counters", end + 5_000, attempts=100 + k, state=int(k > passive_at),
                    tec=tec)
        b = self.bus_off
        self.removal = b + 1_000_000 if fault == "short" else None
        self.seen_bus_off = b - b % 1000 + 40_000          # the log has microseconds
        self.asked = self.seen_bus_off + holdoff_ns
        self.restart = self.asked + 21_000
        self.active = self.restart + 2_816_000 + sequence_extra_ns
        self.seen_active = self.active + 50_000
        self.submit = self.active + 100_000
        self.eof = self.submit + 118 * BIT
        self.first_pass = self.eof + 9_000
        if fault == "short" and not removal_late:
            add("fault", self.removal, attempts=124, state=2, tec=256)
        add("restart", self.restart, attempts=100 + attempts_total, state=2, tec=256)
        if fault == "short" and removal_late:
            add("fault", self.removal + 200_000_000, attempts=124, state=2, tec=256)
            self.removal += 200_000_000
        add("state", self.active, self.active + 15_000, attempts=100 + attempts_total, state=0,
            old=2, tec=0)
        if not drop_first_tx:
            add("first_tx", self.eof, self.first_pass, attempts=100 + attempts_total,
                value=busoff.ID_FAULT, start=self.submit, submit=self.submit,
                frames=FIRST_NUMBER)
        self.records = records
        self.dtc_on = self.seen_bus_off + 59_960_000
        self.dtc_off = self.active + 4_950_000_000
        self.log_text = "\n".join([
            log_line(T0 - 1_000_000, "I", "main", "fcn_node 0.1.0 starting"),
            log_line(self.passive - self.passive % 1000 + 40_000, "W", "can",
                     "controller state 0 -> 1 (TEC 128, REC 0)"),
            log_line(self.seen_bus_off, "W", "can", "controller state 1 -> 2 (TEC 256, REC 0)"),
            log_line(self.dtc_on, "W", "app", "fault CAN_BUS_OFF active (DTC 0xD00100)"),
            log_line(self.dtc_on, "W", "app", "fault CAN_ERROR_PASSIVE active (DTC 0xD00200)"),
            log_line(self.asked, "W", "can", "bus off: requesting recovery"),
            log_line(self.seen_active, "W", "can", "controller state 2 -> 0 (TEC 0, REC 0)"),
            log_line(self.active + 1_050_000_000, "I", "app", "fault CAN_ERROR_PASSIVE no longer "
                                                             "active"),
            log_line(self.dtc_off, "I", "app", "fault CAN_BUS_OFF no longer active"),
        ])
        failed = attempts_total if attempts_status is None else attempts_status
        self.before = {"state": "active", "tec": 0, "tx_failed": 100, "bus_off": 3,
                       "late_resyncs": 7}
        self.after = {"state": "active", "tec": 0, "tx_failed": 100 + failed, "bus_off": 4,
                      "late_resyncs": 7 + resyncs}

    def input(self) -> busoff.TrialInput:
        return busoff.TrialInput(fault=self.fault, bitrate=BITRATE, records=self.records,
                                 log=busoff.parse_log(self.log_text),
                                 status_before=self.before, status_after=self.after,
                                 removal_ns=self.removal)


# ----------------------------------------------------------------------- rules and numbers


def test_the_standards_figures():
    assert busoff.RECOVERY_BITS == 1408
    assert busoff.SHORT_PASSIVE_TO_BUS_OFF_BITS == 416
    assert busoff.bits_to_ns(1408, 500_000) == 2_816_000
    assert busoff.bits_to_ns(1408, 125_000) == 11_264_000
    assert busoff.bits_to_ns(1408, 1_000_000) == 1_408_000
    # Rounded to the nearest nanosecond, a half up, as can_bits_to_ns() does.
    assert busoff.bits_to_ns(1, 3) == 333_333_333
    assert busoff.bits_to_ns(2, 3) == 666_666_667
    assert busoff.bits_to_ns(1, 2_000_000_000) == 1   # 0.5 ns, a half: up
    assert busoff.ns_to_bits(2_816_000, 500_000) == 1408.0
    with pytest.raises(ValueError):
        busoff.bits_to_ns(1, 0)


def test_the_transmit_error_counter_follows_the_rules_attempt_by_attempt():
    short = [busoff.tec_after_attempts("short", n) for n in range(25)]
    assert short[:9] == [0, 16, 32, 48, 64, 80, 96, 112, 128]
    assert short[9] == 136 and short[24] == 256
    errors = [busoff.tec_after_attempts("tx_errors", n) for n in range(33)]
    assert errors[16] == 128 and errors[32] == 256 and errors[1] == 8
    for fault in busoff.FAULTS:
        assert busoff.tec_after_attempts(fault, busoff.ATTEMPTS_TO_BUS_OFF[fault]) == 256
        assert busoff.tec_after_attempts(fault, busoff.ATTEMPTS_TO_BUS_OFF[fault] - 1) < 256
    with pytest.raises(ValueError):
        busoff.tec_after_attempts("noack", 3)
    with pytest.raises(ValueError):
        busoff.tec_after_attempts("short", -1)


def test_the_hold_off_doubles_up_to_its_limit():
    assert busoff.expected_holdoffs_ms(7) == [100, 200, 400, 800, 1600, 1600, 1600]
    assert busoff.expected_holdoffs_ms(0) == []
    # The nearest value of the policy on a logarithmic scale: 141.4 ms divides 100 from 200.
    assert busoff.nominal_holdoff_ms(100.0) == 100
    assert busoff.nominal_holdoff_ms(100.9) == 100
    assert busoff.nominal_holdoff_ms(99.98) == 100
    assert busoff.nominal_holdoff_ms(141.0) == 100
    assert busoff.nominal_holdoff_ms(142.0) == 200
    assert busoff.nominal_holdoff_ms(205.0) == 200
    assert busoff.nominal_holdoff_ms(1130.0) == 800
    assert busoff.nominal_holdoff_ms(1132.0) == 1600
    assert busoff.nominal_holdoff_ms(5000.0) == 1600
    assert busoff.nominal_holdoff_ms(0.0) == 0


def test_the_log_gives_the_drivers_lines_and_the_dtcs_and_nothing_else():
    text = "\n".join([
        log_line(1_000_000_000, "I", "main", "fcn_node 0.1.0 starting"),
        log_line(1_000_041_000, "W", "can", "controller state 1 -> 2 (TEC 256, REC 0)"),
        log_line(1_000_050_000, "W", "can-hw", "transport udp: send failed"),
        log_line(1_100_841_000, "W", "can", "bus off: requesting recovery"),
        "garbage without a stamp",
        log_line(1_200_000_000, "W", "app", "fault CAN_BUS_OFF active (DTC 0xD00100)"),
        log_line(1_300_000_000, "I", "app", "fault CAN_BUS_OFF no longer active"),
    ])
    events = busoff.parse_log(text)
    assert events == [
        busoff.LogEvent(1_000_041_000, "state", old=1, new=2, tec=256),
        busoff.LogEvent(1_100_841_000, "restart"),
        busoff.LogEvent(1_200_000_000, "dtc_active", name="CAN_BUS_OFF"),
        busoff.LogEvent(1_300_000_000, "dtc_gone", name="CAN_BUS_OFF"),
    ]


def test_the_frames_are_read_as_the_dbc_lays_them_out():
    db = decode.load_database()
    fault = db.get_message_by_frame_id(busoff.ID_FAULT)
    data = fault.encode({"E2E_Crc": 0x5A, "E2E_Counter": 3, "FaultActive": 0x600,
                         "FaultConfirmed": 0x1FE01, "FaultMostRecent": 9, "FaultWarning": 1,
                         "FaultClearCount": 200}, scaling=False, strict=False)
    assert busoff.fault_bits(data) == (0x600, 0x1FE01)
    beat = db.get_message_by_frame_id(busoff.ID_HEARTBEAT)
    assert busoff.heartbeat_counter(beat.encode({"PeriodCounter": 123456789,
                                                 "TickCount": 7})) == 123456789
    assert isinstance(db, cantools.database.can.Database)


def test_heartbeat_values_missing_since_the_start():
    assert busoff.heartbeat_values_missing([]) == 0
    assert busoff.heartbeat_values_missing([1, 2, 3]) == 0
    assert busoff.heartbeat_values_missing([3, 4, 6, 20]) == 2 + 1 + 13
    with pytest.raises(ValueError):
        busoff.heartbeat_values_missing([1, 3, 3])


def test_a_kernel_interval_counts_only_where_the_monotonic_clock_agrees():
    a = busoff.Heard(1_000_000_000, 100.0, 1, b"")
    b = busoff.Heard(1_115_000_000, 100.1149, 1, b"")
    assert busoff.kernel_interval_ns(a, b) == 114_900_000
    stepped = busoff.Heard(1_115_000_000, 1154.1149, 1, b"")   # realtime stepped by 1054 s
    assert busoff.kernel_interval_ns(a, stepped) is None


# ------------------------------------------------------------------------------ one trial


def test_a_short_trial_gives_every_interval_to_the_nanosecond():
    trial = Trial()
    out = busoff.trial_figures(trial.input())
    assert out["problems"] == [] and out["retry"] is False
    f = out["figures"]
    assert f["fault_to_bus_off"] == trial.bus_off - T0 == 2_114_000
    assert f["passive_to_bus_off"] == 832_000 and f["passive_to_bus_off_bits"] == 416.0
    assert f["passive_to_bus_off_exact"] is True
    assert f["bus_off_report_lag"] == 7_000
    assert f["bus_off_to_driver"] == 40_000
    assert f["holdoff"] == 100_800_000
    assert f["request_to_count"] == 21_000
    assert f["recovery_sequence"] == 2_816_000 and f["recovery_sequence_bits"] == 1408.0
    assert f["recovery_report_lag"] == 15_000
    assert f["recovery_to_driver"] == 50_000
    assert f["active_to_first_submit"] == 100_000
    assert f["first_frame_on_bus"] == 236_000 and f["first_frame_on_bus_bits"] == 118.0
    assert f["first_frame_write_lag"] == 9_000
    assert f["bus_off_to_active"] == 40_000 + 100_800_000 + 21_000 + 2_816_000
    assert f["removal_to_active"] == f["bus_off_to_active"] - 1_000_000
    assert f["removal_to_first_frame"] == f["removal_to_active"] + 100_000 + 236_000
    assert f["bus_off_to_dtc"] == 60_000_000
    assert f["dtc_heal"] == 4_950_000_000
    assert (f["attempts_to_bus_off"], f["tec_at_passive"], f["tec_at_bus_off"]) == (24, 128, 256)
    assert f["late_resyncs"] == 0 and f["bus_offs"] == 1
    assert f["first_frame_id"] == busoff.ID_FAULT
    assert f["counter_samples"] == 22 == len(out["trajectory"])
    assert out["trajectory"][0] == [1, 16] and out["trajectory"][-1] == [23, 248]
    assert set(busoff.INTERVALS) <= set(f)


def test_injected_errors_end_at_the_bus_off():
    trial = Trial("tx_errors")
    out = busoff.trial_figures(trial.input())
    assert out["problems"] == []
    f = out["figures"]
    assert f["attempts_to_bus_off"] == 32
    assert f["t_removal_ns"] == trial.bus_off
    assert f["removal_to_active"] == f["bus_off_to_active"]
    assert f["passive_to_bus_off_bits"] == 16 * 26 and f["passive_to_bus_off_exact"] is True
    assert out["trajectory"][0] == [1, 8] and out["trajectory"][-1] == [31, 248]


@pytest.mark.parametrize("knob, expected", [
    ({"bad_tec_at": 5}, "TEC 88 after 5 destroyed attempts, not 80"),
    ({"sequence_extra_ns": BIT}, "the recovery took 2818000 ns, not 1408 bit times"),
    ({"passive_bits": 27}, "864000 ns from error passive to bus off without a late pass"),
    ({"attempts_status": 23}, "can.status counted 23 destroyed attempts in the trial"),
    ({"drop_first_tx": True}, "no restart, recovery and first frame after the bus off"),
])
def test_a_trial_that_breaks_a_rule_is_no_sample_and_is_not_repeated(knob, expected):
    out = busoff.trial_figures(Trial(**knob).input())
    assert out["retry"] is False
    assert any(expected in problem for problem in out["problems"]), out["problems"]


def test_a_late_pass_excuses_the_spacing_but_nothing_else():
    out = busoff.trial_figures(Trial(passive_bits=27, resyncs=1).input())
    assert out["problems"] == []
    assert out["figures"]["late_resyncs"] == 1
    assert out["figures"]["passive_to_bus_off_bits"] == 16 * 27
    # Wherever in the trial the late pass was, the interval is no sample of the model.
    assert out["figures"]["passive_to_bus_off_exact"] is False


def test_an_interval_is_the_models_only_on_the_bit_grid_and_without_a_late_pass():
    assert busoff.model_exact(832_000, 500_000, 0)
    assert not busoff.model_exact(832_001, 500_000, 0)
    assert not busoff.model_exact(832_000, 500_000, 1)
    assert busoff.model_exact(3_328_000, 125_000, 0)
    assert not busoff.model_exact(3_330_000, 125_000, 0)   # 416.25 bit times
    assert busoff.model_exact(416_000, 1_000_000, 0)


def test_a_frame_loaded_into_an_idle_bus_is_no_fault_of_the_model_and_no_sample_of_it():
    # The queue recalled the mailboxes, and the driver loaded a frame 1.234 µs after the bus had
    # become free: that attempt started at the driver's time, and nothing counts it. Off the bit
    # grid the interval is no sample of the model, and under a short it breaks no rule either.
    for fault in busoff.FAULTS:
        out = busoff.trial_figures(Trial(fault, reload_ns=1_234).input())
        assert out["problems"] == [], (fault, out["problems"])
        f = out["figures"]
        assert f["passive_to_bus_off"] == 16 * 26 * BIT + 1_234, fault
        assert f["late_resyncs"] == 0 and f["passive_to_bus_off_exact"] is False, fault


def test_a_host_time_on_the_bit_grid_cannot_be_told_from_the_model():
    # What model_exact() cannot see (the trace does not say when each attempt started): a frame
    # loaded into an idle bus a whole number of bit times after it became free. Under a short the
    # interval then looks like the model's and breaks its rule, and the trial is refused.
    out = busoff.trial_figures(Trial(reload_ns=2 * BIT).input())
    assert out["figures"]["passive_to_bus_off_exact"] is True
    assert any("836000 ns from error passive to bus off" in p for p in out["problems"])
    out = busoff.trial_figures(Trial("tx_errors", reload_ns=2 * BIT).input())
    assert out["problems"] == [] and out["figures"]["passive_to_bus_off_exact"] is True


@pytest.mark.parametrize("knob, expected", [
    ({"removal_late": True}, "removed the short only after the node had asked to recover"),
    ({"holdoff_ns": 205_000_000}, "a hold-off of 205.000 ms, not the initial 100 ms"),
])
def test_a_trial_the_host_spoilt_is_repeated(knob, expected):
    out = busoff.trial_figures(Trial(**knob).input())
    assert out["retry"] is True
    assert any(expected in problem for problem in out["problems"]), out["problems"]


def test_a_trial_must_start_with_its_fault():
    trial = Trial()
    out = busoff.trial_figures(busoff.TrialInput(
        fault="tx_errors", bitrate=BITRATE, records=trial.records, log=[],
        status_before=trial.before, status_after=trial.after))
    assert out["problems"] == ["the trial's first record is not its fault (inject_tx 32)"]


# ---------------------------------------------------------------------- the observer


def heard_around(trial: Trial, *, lost=10, stale=False, second_fault=False, first_bits=None,
                 observer_late_ns=0):
    """Heartbeats every 10 ms until the fault, `lost` values the node never sent, then the first
    frame after the recovery (BMS_Fault, the node's frame FIRST_NUMBER), heartbeats again, and a
    BMS_Fault every 100 ms whose bits heal 1 s (error passive) and 4.95 s (bus off) after the
    recovery. The kernel's clock runs 1e9 s ahead of the monotonic one. `first_bits`: the
    FaultActive and FaultConfirmed of the first BMS_Fault (default both DTCs in both);
    `observer_late_ns`: the observer's thread got the last frame before the fault that much
    late."""
    frames, counter = [], 100
    ahead = 1_000_000_000_000_000_000

    def add(mono, frame_id, data):
        frames.append(busoff.Heard(mono, (mono + ahead) / 1e9, frame_id, data))

    t = T0 - 400_000_000
    while t < T0:
        late = observer_late_ns if t + 10_000_000 >= T0 else 0
        frames.append(busoff.Heard(t + late, (t + ahead) / 1e9, busoff.ID_HEARTBEAT,
                                   counter.to_bytes(4, "little") + bytes(4)))
        counter += 1
        t += 10_000_000
    last_before = t - 10_000_000
    counter += lost
    arrival = trial.first_pass + 30_000
    fault_word = lambda active, confirmed: ((active << 16) | (confirmed << 33)).to_bytes(8, "little")
    both = (1 << busoff.DTC_BUS_OFF_BIT) | (1 << busoff.DTC_PASSIVE_BIT)
    add(arrival, busoff.ID_FAULT, fault_word(*(first_bits or (both, both))))
    t = arrival + 200_000
    beat_counter = counter - 50 if stale else counter
    for _ in range(600):
        add(t, busoff.ID_HEARTBEAT, beat_counter.to_bytes(4, "little") + bytes(4))
        beat_counter = counter + 1 if stale else beat_counter + 1
        stale = False
        t += 10_000_000
    t = arrival + 100_000_000
    while t < trial.active + 6_000_000_000:
        active = 0
        if t - trial.active < 4_950_000_000:
            active |= 1 << busoff.DTC_BUS_OFF_BIT
        if t - trial.active < 1_000_000_000:
            active |= 1 << busoff.DTC_PASSIVE_BIT
        if second_fault:
            active |= 1
        add(t, busoff.ID_FAULT, fault_word(active, both))
        t += 100_000_000
    frames.sort(key=lambda f: f.mono_ns)
    return frames, last_before, arrival


def test_what_the_observer_heard_around_a_trial():
    trial = Trial()
    figures = busoff.trial_figures(trial.input())["figures"]
    frames, last_before, arrival = heard_around(trial)
    seen = busoff.observer_figures(frames, figures, trial.active + 6_000_000_000, OFFSET)
    assert figures["first_frame_number"] == FIRST_NUMBER
    assert seen["problems"] == []
    assert seen["silence_ns"] == pytest.approx(arrival - last_before, abs=1_000)
    assert seen["observer_lag_ns"] == 30_000
    assert seen["removal_to_observer_ns"] == arrival - trial.removal
    assert seen["heartbeats_lost"] == 10 and seen["first_heartbeat_fresh"] is True
    assert seen["dtc_bus_off_after"] is True and seen["dtc_passive_after"] is True
    healed = seen["dtc_bus_off_healed_ns"]
    assert 4_950_000_000 <= healed < 4_950_000_000 + 100_000_000
    assert 1_000_000_000 <= seen["dtc_passive_healed_ns"] < 1_100_000_000


def test_a_stale_heartbeat_and_dtcs_missing_from_the_first_frame_are_figures_not_problems():
    # Both can come from a host task run late, so they are counted, not refused.
    trial = Trial()
    figures = busoff.trial_figures(trial.input())["figures"]
    passive_only = 1 << busoff.DTC_PASSIVE_BIT
    frames, _, _ = heard_around(trial, stale=True, first_bits=(passive_only, passive_only))
    seen = busoff.observer_figures(frames, figures, trial.active + 6_000_000_000, OFFSET)
    assert seen["problems"] == []
    assert seen["first_heartbeat_fresh"] is False
    assert seen["dtc_bus_off_after"] is False and seen["dtc_passive_after"] is True


def test_the_observer_finds_another_fault_and_no_heal_in_its_window():
    trial = Trial()
    figures = busoff.trial_figures(trial.input())["figures"]
    frames, _, _ = heard_around(trial, second_fault=True)
    seen = busoff.observer_figures(frames, figures, trial.active + 3_000_000_000, OFFSET)
    assert [p for p in seen["problems"]] == ["BMS_Fault reports another fault: FaultActive 0x601"]
    assert seen["dtc_bus_off_healed_ns"] is None          # after the window
    assert seen["dtc_passive_healed_ns"] is not None


def test_the_first_frame_heard_must_be_the_first_the_trace_names():
    trial = Trial()
    figures = dict(busoff.trial_figures(trial.input())["figures"], first_frame_id=0x020)
    frames, _, _ = heard_around(trial)
    seen = busoff.observer_figures(frames, figures, trial.active + 6_000_000_000, OFFSET)
    assert any("the trace says 0x20" in p for p in seen["problems"])


def test_the_first_frame_is_found_by_its_number_even_when_the_observer_was_late():
    # The observer's thread got the last heartbeat before the fault 80 ms late, after the middle
    # of the hold-off: by its number the first frame after the recovery is still the BMS_Fault.
    trial = Trial()
    figures = busoff.trial_figures(trial.input())["figures"]
    frames, _, arrival = heard_around(trial, observer_late_ns=80_000_000)
    assert frames[39].mono_ns > (trial.bus_off + trial.restart) // 2
    seen = busoff.observer_figures(frames, figures, trial.active + 6_000_000_000, OFFSET)
    assert seen["problems"] == []
    assert seen["observer_lag_ns"] == 30_000 and seen["heartbeats_lost"] == 10
    assert seen["silence_ns"] is None      # the late thread's clock disagrees with the kernel's


def test_a_frame_the_observer_missed_shows_as_a_wrong_number():
    trial = Trial()
    figures = busoff.trial_figures(trial.input())["figures"]
    frames, _, _ = heard_around(trial)
    # One frame fewer before the window than the trace counts: the frame taken is a heartbeat.
    seen = busoff.observer_figures(frames, figures, trial.active + 6_000_000_000, OFFSET - 1)
    assert any("was heard as 0x30" in p for p in seen["problems"]), seen["problems"]
    seen = busoff.observer_figures(frames[:40], figures, trial.active + 6_000_000_000, OFFSET)
    assert seen["problems"] == [f"the observer did not hear the node's frame {FIRST_NUMBER}, the "
                                "first after the recovery, or none before it"]


# ---------------------------------------------------------------- a persisting short


def persisting(holdoffs_ms=(100.8, 200.9, 400.7), attempts=24, sequence_bits=1408):
    records, lines, seq = [record(0, "fault", T0, value=1)], [], 1
    t = T0 + 2_000_000
    for i, holdoff in enumerate(holdoffs_ms):
        bus_off = t
        records.append(record(seq, "state", bus_off, attempts=100 + attempts * (i + 1), state=2,
                              old=1, tec=256))
        seen = bus_off + 40_000
        asked = seen + round(holdoff * 1000) * 1000
        lines += [log_line(seen, "W", "can", "controller state 1 -> 2 (TEC 256, REC 0)"),
                  log_line(asked, "W", "can", "bus off: requesting recovery")]
        restart = asked + 20_000
        if i == len(holdoffs_ms) - 1:
            records.append(record(seq + 1, "fault", bus_off + 1_000_000, value=0, state=2))
            seq += 1
        records += [record(seq + 1, "restart", restart, state=2, tec=256),
                    record(seq + 2, "state", restart + busoff.bits_to_ns(sequence_bits, BITRATE),
                           state=0, old=2)]
        seq += 3
        t = restart + 10_000_000
    return records, busoff.parse_log("\n".join(lines))


def test_a_persisting_short_shows_the_hold_off_doubling():
    records, log = persisting()
    out = busoff.persist_figures(records, log, BITRATE, 3)
    assert out["problems"] == []
    assert out["holdoffs_ms"] == [100.8, 200.9, 400.7]
    assert out["nominal_ms"] == out["expected_ms"] == [100, 200, 400]
    assert out["holdoffs_as_policy"] is True
    assert out["recovery_sequence_bits"] == [1408.0] * 3
    assert out["attempts"] == [24, 24, 24]
    assert out["recoveries_under_short"] == 2


def test_a_persisting_short_that_breaks_the_policy_is_reported():
    # Hold-offs are host timing: reported, not a problem.
    records, log = persisting(holdoffs_ms=(100.8, 100.9, 400.7))
    out = busoff.persist_figures(records, log, BITRATE, 3)
    assert out["problems"] == [] and out["holdoffs_as_policy"] is False
    assert out["nominal_ms"] == [100, 100, 400]
    records, log = persisting(sequence_bits=1300)
    assert any("1408" in p for p in busoff.persist_figures(records, log, BITRATE, 3)["problems"])
    records, log = persisting()
    assert any("3 bus offs, not 4" in p
               for p in busoff.persist_figures(records, log, BITRATE, 4)["problems"])
    assert busoff.persist_figures(records[:-3], log, BITRATE, 3)["problems"] == [
        "the short was never removed"]


# ------------------------------------------------------------------- the run's counts


def run_stats(bus_offs=5):
    message = lambda frame_id, sent, superseded: {
        "id": frame_id, "queue": {"produced": sent + superseded + 1, "sent": sent,
                                  "superseded": superseded, "pending": 1}}
    return {
        "can": {"queue": {"produced": 3013, "sent": 2980, "superseded": 31, "dropped": 0,
                          "expired": 0, "pending": 2, "bus_off_count": bus_offs,
                          "restart_requests": bus_offs, "holdoff_ms": 100},
                "controller": {"bus_off_events": bus_offs, "tx_frames": 2980},
                "driver": {"state_events_merged": 0, "event_fifo_overruns": 0},
                "transport_counters": {"tx_frames": 2980}},
        "telemetry": {"messages": {"BMS_Heartbeat": message(0x030, 2000, 21),
                                   "BMS_Fault": message(0x010, 980, 10)}},
        "faults": {"dtcs": [{"name": "CAN_BUS_OFF", "status": 0x2E, "failed_cycles": 1},
                            {"name": "CAN_ERROR_PASSIVE", "status": 0x2E, "failed_cycles": 1}]},
    }


def run_log(episodes=3, bus_offs=5):
    events = []
    for name in ("CAN_BUS_OFF", "CAN_ERROR_PASSIVE"):
        events += [busoff.LogEvent(1, "dtc_active", name=name)] * episodes
        events += [busoff.LogEvent(2, "dtc_gone", name=name)] * episodes
    return events + [busoff.LogEvent(3, "restart")] * bus_offs


def test_the_counts_of_a_run_against_what_the_bench_did():
    checks = busoff.session_checks(run_stats(), 5, 3, run_log(), {0x030: 2000, 0x010: 980}, 21, 0)
    assert all(check.ok for check in checks), [c.as_dict() for c in checks if not c.ok]
    names = {check.name for check in checks}
    assert "BMS_Heartbeat values missing on the bus = superseded" in names
    assert "frames heard = controller frames sent" in names


@pytest.mark.parametrize("change, failing", [
    (lambda s: s["can"]["queue"].update(bus_off_count=6), "queue bus off count"),
    (lambda s: s["can"]["queue"].update(holdoff_ms=200), "queue hold-off at the end (ms)"),
    (lambda s: s["faults"]["dtcs"][0].update(status=0x2F), "CAN_BUS_OFF status byte"),
    (lambda s: s["can"]["driver"].update(state_events_merged=1), "driver state events merged"),
    (lambda s: s["telemetry"]["messages"]["BMS_Fault"]["queue"].update(sent=990, produced=1001),
     "BMS_Fault: frames heard, from sent to sent + pending"),
])
def test_each_count_that_does_not_fit_is_named(change, failing):
    stats = run_stats()
    change(stats)
    checks = busoff.session_checks(stats, 5, 3, run_log(), {0x030: 2000, 0x010: 980}, 21, 0)
    assert [c.name for c in checks if not c.ok] == [failing]


def test_a_message_may_be_heard_once_more_than_its_queue_counted_it_sent():
    # The frame that left after the queue's counters were taken: the controller counted it, the
    # queue still has it pending.
    stats = run_stats()
    stats["can"]["controller"]["tx_frames"] = stats["can"]["transport_counters"]["tx_frames"] = 2981
    checks = busoff.session_checks(stats, 5, 3, run_log(), {0x030: 2001, 0x010: 980}, 21, 0)
    assert all(c.ok for c in checks), [c.as_dict() for c in checks if not c.ok]
    check = next(c for c in checks if c.name.startswith("BMS_Heartbeat: frames heard"))
    assert check.as_dict()["at_most"] == 2001
    # One more than that is not.
    checks = busoff.session_checks(stats, 5, 3, run_log(), {0x030: 2002, 0x010: 979}, 21, 0)
    assert [c.name for c in checks if not c.ok] == [
        "BMS_Heartbeat: frames heard, from sent to sent + pending",
        "BMS_Fault: frames heard, from sent to sent + pending"]


def test_an_observer_that_lost_frames_skips_the_frame_counts():
    checks = busoff.session_checks(run_stats(), 5, 3, run_log(), {}, None, 4)
    assert [c.name for c in checks if not c.ok] == ["observer socket overflow"]
    assert not any("heard" in c.name for c in checks)


def test_the_log_must_show_each_stretch_of_the_dtcs():
    checks = busoff.session_checks(run_stats(), 5, 4, run_log(), {0x030: 2000, 0x010: 980}, 21, 0)
    assert len([c for c in checks if not c.ok]) == 4


# ------------------------------------------------------------------- the bench's summary


def constructed_run(rate=500_000):
    trial = Trial()
    out = busoff.trial_figures(trial.input())
    frames, _, _ = heard_around(trial)
    seen = busoff.observer_figures(frames, out["figures"], trial.active + 6_000_000_000, OFFSET)
    assert seen["problems"] == []
    columns: dict[str, list] = {}
    for n in range(12):
        for key in bench.TRIAL_COLUMNS:
            value = out["figures"][key]
            if key == "holdoff":
                value += n * 1_000_000     # 100.8, 101.8, ..., 111.8 ms
            columns.setdefault(key, []).append(value)
        for key in bench.OBSERVER_COLUMNS:
            columns.setdefault(key, []).append(seen[key])
    columns["dtc_heal"][3] = None             # a heal the log did not show
    return {"bitrate": rate, "trials": {"short": columns}, "repeated_trials": [{}],
            "persisting_short": {"holdoffs_ms": [100.8, 200.9]}}


def test_the_summary_gives_every_figure_with_its_unit():
    summary = bench.summarize([constructed_run(), constructed_run(125_000)])
    results.check_summary(summary)
    assert summary["standard_minimum_recovery_bits"] == {"value": 1408, "unit": "bit times",
                                                         "part": "standard"}
    fast = summary["500_kbit_s"]
    assert fast["standard_minimum_recovery_ms"] == {"value": 2.816, "unit": "ms",
                                                    "part": "standard"}
    assert summary["125_kbit_s"]["standard_minimum_recovery_ms"]["value"] == 11.264
    assert fast["recovery_sequence_ms"]["n"] == 12 and fast["recovery_sequence_ms"]["min"] == 2.816
    assert fast["recovery_sequence_ms"]["part"] == "model"
    # The constructed trials are at 500 kbit/s: at 125 kbit/s their 2.816 ms are 352 bit times.
    assert fast["recovery_sequence_bits"]["max"] == 1408.0
    assert summary["125_kbit_s"]["recovery_sequence_bits"]["max"] == 352.0
    short = fast["short"]
    assert short["trials"] == {"value": 12, "unit": "trials"}
    assert short["holdoff_ms"]["min"] == 100.8 and short["holdoff_ms"]["max"] == 111.8
    assert short["holdoff_ms"]["p50"] == pytest.approx(106.3)
    assert short["holdoff_ms"]["p90"] is None             # needs 50 samples
    assert short["holdoff_ms"]["part"] == "policy"
    assert short["recovery_sequence_bits"]["mean"] == 1408.0
    assert short["recovery_sequence_bits"]["part"] == "model"
    assert short["passive_to_bus_off_bits"] == {
        **results.distribution([416.0] * 12, "bit times"), "part": "model"}
    assert short["passive_to_bus_off_bits_not_exact"] == {"unit": "bit times", "n": 0,
                                                          "samples": [], "part": "mixed"}
    assert short["first_frame_bits"]["max"] == 118.0
    assert short["first_frame_bits"]["part"] == "mixed"
    assert short["host_request_to_count_ms"]["part"] == "host"
    assert short["bus_off_dtc_heal_ms"]["n"] == 11
    assert short["heartbeat_values_lost"] == {**results.distribution([10] * 12, "frames"),
                                              "part": "mixed"}
    assert short["first_heartbeat_fresh"] == {"value": 12, "unit": "of 12 trials"}
    assert fast["repeated_trials"]["value"] == 1
    assert fast["persisting_short_holdoffs_ms"]["2"] == {"value": 200.9, "unit": "ms",
                                                         "part": "policy"}
    # Every summary entry of a fault kind that is a distribution names its part.
    parts = {entry["part"] for entry in short.values() if "n" in entry}
    assert parts == {"model", "policy", "host", "mixed"}
    assert {name: part for _, name, _, _, part in bench.SUMMARY_FIGURES}[
        "host_bus_off_to_driver_ms"] == busoff.INTERVALS["bus_off_to_driver"][2]


def test_the_summary_names_the_same_part_as_the_intervals():
    for key, name, _, _, part in bench.SUMMARY_FIGURES:
        assert part in ("model", "policy", "host", "mixed"), name
        if key in busoff.INTERVALS:
            assert part == busoff.INTERVALS[key][2], name
    # The first frame's length and its time from the mailbox: which frame goes first is the
    # node's schedule.
    parts = {name: part for _, name, _, _, part in bench.SUMMARY_FIGURES}
    assert parts["first_frame_bits"] == busoff.INTERVALS["first_frame_on_bus"][2] == "mixed"
    assert busoff.INTERVALS["passive_to_bus_off"][2] == "mixed"


def run_of(trials: dict[str, list[Trial]], rate: int = BITRATE) -> dict:
    """A run of the bench made of constructed trials, kept as analyse_bitrate() keeps them."""
    columns: dict[str, dict[str, list]] = {}
    for fault, constructed in trials.items():
        group = columns.setdefault(fault, {})
        for trial in constructed:
            out = busoff.trial_figures(trial.input())
            assert out["problems"] == [], out["problems"]
            frames, _, _ = heard_around(trial)
            seen = busoff.observer_figures(frames, out["figures"],
                                           trial.active + 6_000_000_000, OFFSET)
            for key in bench.TRIAL_COLUMNS:
                group.setdefault(key, []).append(out["figures"][key])
            for key in bench.OBSERVER_COLUMNS:
                group.setdefault(key, []).append(seen[key])
    return {"bitrate": rate, "trials": columns, "repeated_trials": [], "persisting_short": None}


def model_figures(entry: dict) -> dict:
    """The figures of a fault kind's summary that are labelled the model's."""
    return {name: figure for name, figure in entry.items() if figure.get("part") == "model"}


def test_a_short_a_late_pass_may_have_moved_is_no_sample_of_the_model():
    # can.status counted a late pass in the trial: wherever it was, the interval is "mixed".
    # The model's figure holds the exact trials, and like every figure of the model it is a
    # constant.
    late = Trial(passive_bits=27, resyncs=1)
    summary = bench.summarize([run_of({"short": [Trial(), late, Trial()]})])
    results.check_summary(summary)
    short = summary["500_kbit_s"]["short"]
    assert short["passive_to_bus_off_bits"] == {
        **results.distribution([416.0, 416.0], "bit times"), "part": "model"}
    assert short["passive_to_bus_off_bits_not_exact"] == {"unit": "bit times", "n": 1,
                                                          "samples": [432.0], "part": "mixed"}
    assert set(model_figures(short)) == {"passive_to_bus_off_bits",
                                         "destroyed_attempts_to_bus_off", "recovery_sequence_bits"}
    for name, figure in model_figures(short).items():
        assert figure["min"] == figure["max"], name


def test_injected_errors_whose_attempt_started_at_a_host_time_are_no_sample_of_the_model():
    # No late pass, but a frame loaded into an idle bus started at the driver's time, off the bit
    # grid. Under tx_errors the figure is "mixed" as a whole (which frames are attempted is the
    # node's schedule), the exact trials apart; no figure labelled the model's holds the other.
    moved = Trial("tx_errors", reload_ns=1_234)
    summary = bench.summarize([run_of({"tx_errors": [Trial("tx_errors"), moved]})])
    results.check_summary(summary)
    errors = summary["500_kbit_s"]["tx_errors"]
    assert errors["passive_to_bus_off_bits"] == {
        **results.distribution([416.0, 416.617], "bit times"), "part": "mixed"}
    assert errors["passive_to_bus_off_bits_exact"] == {
        **results.distribution([416.0], "bit times"), "part": "mixed"}
    assert errors["passive_to_bus_off_bits_not_exact"] == {"unit": "bit times", "n": 1,
                                                           "samples": [416.617], "part": "mixed"}
    assert set(model_figures(errors)) == {"destroyed_attempts_to_bus_off",
                                          "recovery_sequence_bits"}
    for name, figure in model_figures(errors).items():
        assert figure["min"] == figure["max"], name


def test_the_frames_of_a_window_are_taken_by_their_monotonic_time():
    frames = [(10, 1.0, 0x30, b"a"), (20, 2.0, 0x10, b"b"), (30, 3.0, 0x30, b"c")]
    import numpy as np
    offset, heard = bench.heard_between(frames, np.asarray([10, 20, 30]), 15, 30)
    assert offset == 1 and heard == [busoff.Heard(20, 2.0, 0x10, b"b")]


# ---------------------------------------------------------------- the trace's client


def test_a_trace_record_is_read_from_its_reply_word():
    assert parse_trace_record("r12", "first_tx,5,6,3,2,24,16,0,0,0,0,7") == TraceRecord(
        12, "first_tx", 5, 6, 3, 2, 24, 16, 0, 0, 0, 0, 7)
    # Another key, too few fields (the eleven of a trace without the frame count among them),
    # an unknown kind.
    for key, value in (("x12", "state,1,2,3,4,5,6,7,8,9,10,11"), ("r1", "state,1,2"),
                       ("r1", "state,1,2,3,4,5,6,7,8,9,10"),
                       ("r1", "nonsense,1,2,3,4,5,6,7,8,9,10,11")):
        with pytest.raises(ValueError):
            parse_trace_record(key, value)


class PagingNode:
    """Serves can.trace from `records` (their text), at most `page` records per reply, from
    `oldest` on, as posix_can_trace_format() does."""

    def __init__(self, words: list[str], oldest: int, page: int) -> None:
        self.words, self.oldest, self.page = words, oldest, page
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(0.05)
        self.port = self.socket.getsockname()[1]
        self.running = True
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self) -> None:
        while self.running:
            try:
                data, sender = self.socket.recvfrom(2048)
            except TimeoutError:
                continue
            token, command, *rest = data.decode().split()
            start = max(int(rest[0]) if rest else 0, self.oldest)
            newest = self.oldest + len(self.words)
            words = [f"r{seq}={self.words[seq - self.oldest]}"
                     for seq in range(start, min(newest, start + self.page))]
            reply = " ".join([token, "ok", f"oldest={self.oldest}", f"newest={newest}", *words])
            self.socket.sendto(reply.encode(), sender)

    def close(self) -> None:
        self.running = False
        self.thread.join()
        self.socket.close()


def test_the_client_reads_the_trace_page_by_page_and_counts_what_was_lost():
    words = [f"counters,{n},{n},0,0,{n},0,0,0,{8 * n},0,{n}" for n in range(10)]
    node = PagingNode(words, oldest=4, page=3)
    try:
        with SimControl(node.port) as sim:
            read = sim.can_trace(0)
            assert read.lost == 4 and read.newest == 14
            assert [r.seq for r in read.records] == list(range(4, 14))
            assert read.records[2].tec == 16 and read.records[2].kind == "counters"
            assert sim.can_trace(12).records[-1].seq == 13 and sim.can_trace(14).records == []
    finally:
        node.close()


def test_a_trace_page_without_records_below_newest_is_an_error():
    node = PagingNode(["state,1,1,0,0,0,0,1,0,128,0,0"] * 3, oldest=0, page=0)
    try:
        with SimControl(node.port) as sim, pytest.raises(SimError, match="no record"):
            sim.can_trace(0)
    finally:
        node.close()
