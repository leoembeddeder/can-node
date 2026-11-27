# SPDX-License-Identifier: MIT
"""The power-loss campaign's model and figures (fcn_tools.powerloss, used by bench/power_loss.py)
on constructed data with known answers: the operations of each sequence, operation by operation,
with the addresses docs/update.md gives for the host's geometry (entries of 128 bytes, records of
96 bytes, commit fields and attempt fields of 8, sectors of 4096, pages of 256) and the states
before and after each; the log's wrap; what each start of the board leaves; the FATAL line of a
cut; the plan; the rows of the result; and the counts of the summary. The bench's plot is drawn
from a constructed result."""
from __future__ import annotations

import importlib.util
import json
import struct
import sys
import zlib

import pytest

from fcn_tools import flash as flash_layout
from fcn_tools import paths, powerloss, results
from fcn_tools.powerloss import (Cell, LogPosition, State, Trial, after_record, cells_of,
                                 confirm_sequence, cut_mismatch, erase_ahead, headline,
                                 left_by, make_plan, parse_cut, parse_replay, parse_row,
                                 position_ahead, rollback_sequence, row_text, start_text,
                                 state_after_start, state_of, summarize, update_sequence,
                                 wrap_ahead, wrapping_ops)
from fcn_tools.update_model import Boot

SLOT_B = 0x102000
DATA_SIZE = 9256            # the data image of the phase 5 tests: 3 sectors, 37 pages


def described(a: str, b: str, seq: int, event: str, attempts: int = 0, *,
              next_write=(0, 1, False), record=(0, 0)) -> dict:
    """A flash description (fcn_flash info's members that the model reads)."""
    sector, index, erases = next_write
    return {"metadata": {"seq": seq, "event": event, "attempts_used": attempts,
                         "next_sector": sector, "next_index": index, "next_write_erases": erases,
                         "record_sector": record[0], "record_index": record[1],
                         "entries_used": index + 1},
            "slots": [{"name": "A", "state": a}, {"name": "B", "state": b}]}


FACTORY = described("confirmed", "empty", 1, "adopt", next_write=(0, 1, False))


# --------------------------------------------------------------------------- the sequences


def test_an_update_of_the_data_image_is_44_operations_at_the_documented_addresses():
    seq = update_sequence("fresh", "factory", FACTORY, DATA_SIZE, "fcn_flash")
    assert len(seq.ops) == 2 + 3 + 37 + 2
    assert seq.counts() == {"programs": 41, "erases": 3, "erases_meta": 0}
    roles = [op.role for op in seq.ops]
    assert roles[:5] == ["begin.record", "begin.commit_field", "slot.erase", "slot.erase",
                         "slot.erase"]
    assert roles[5:41] == ["slot.page"] * 36 and roles[41] == "slot.last_page"
    assert roles[42:] == ["commit.record", "commit.commit_field"]
    # The BEGIN record goes into entry 1 (entry 0 holds the factory's ADOPT), COMMIT into 2.
    begin, field = seq.ops[0], seq.ops[1]
    assert (begin.kind, begin.address, begin.length) == ("program", 0x80, 96)
    assert (field.kind, field.address, field.length) == ("program", 0xE0, 8)
    assert [(op.address, op.length) for op in seq.ops[2:5]] == [
        (SLOT_B, 4096), (SLOT_B + 4096, 4096), (SLOT_B + 8192, 4096)]
    assert (seq.ops[5].address, seq.ops[5].length) == (SLOT_B, 256)
    # 9256 = 36 pages and 40 bytes: the last page is padded to whole units of 8.
    assert (seq.ops[41].address, seq.ops[41].length) == (SLOT_B + 36 * 256, 40)
    assert (seq.ops[42].address, seq.ops[43].address) == (0x100, 0x160)
    # One run of one program: its N is the operation's number.
    assert all((op.start, op.program, op.in_program) == (0, "fcn_flash", op.number)
               for op in seq.ops)


def test_only_a_commit_field_changes_the_state():
    seq = update_sequence("fresh", "factory", FACTORY, DATA_SIZE, "node")
    s0 = State("confirmed", "empty", 1, "adopt", 0)
    s1 = State("confirmed", "writing", 2, "begin", 0)
    s2 = State("confirmed", "trial", 3, "commit", 0)
    assert (seq.ops[0].before, seq.ops[0].after) == (s0, s0)
    assert (seq.ops[1].before, seq.ops[1].after) == (s0, s1)
    assert all((op.before, op.after) == (s1, s1) for op in seq.ops[2:43])
    assert (seq.ops[43].before, seq.ops[43].after) == (s1, s2)
    changing = [op.role for op in seq.ops if op.before != op.after]
    assert changing == ["begin.commit_field", "commit.commit_field"]
    assert seq.ops[0].allowed() == (s0,) and seq.ops[1].allowed() == (s0, s1)


def test_a_record_that_begins_a_sector_erases_it_first():
    full = described("confirmed", "rejected", 64, "rollback", next_write=(0, 0, True))
    seq = update_sequence("wrap-begin", "wrap-begin", full, DATA_SIZE, "fcn_flash")
    assert len(seq.ops) == 45 and seq.counts()["erases_meta"] == 1
    erase, record, field = seq.ops[:3]
    assert (erase.kind, erase.address, erase.length, erase.role) == ("erase", 0, 4096,
                                                                     "begin.meta_erase")
    assert (record.address, field.address) == (0x0, 0x60)
    assert erase.before == erase.after == record.after == field.before
    assert field.after == State("confirmed", "writing", 65, "begin", 0)
    assert [op.role for op in wrapping_ops(seq)] == ["begin.meta_erase", "begin.record",
                                                     "begin.commit_field"]
    # One entry left in sector 1: BEGIN takes it, COMMIT erases sector 0 and goes to entry 0.
    last = described("confirmed", "rejected", 127, "rollback", next_write=(1, 31, False))
    seq = update_sequence("wrap-commit", "wrap-commit", last, DATA_SIZE, "fcn_flash")
    assert seq.ops[0].address == 0x1000 + 31 * 128
    commit = [op for op in seq.ops if op.role.startswith("commit.")]
    assert [(op.kind, op.address) for op in commit] == [("erase", 0x0), ("program", 0x0),
                                                        ("program", 0x60)]
    assert [op.number for op in wrapping_ops(seq)] == [43, 44, 45]


def test_an_update_of_a_writing_slot_writes_no_begin_and_none_while_a_trial_is_pending():
    writing = described("confirmed", "writing", 2, "begin", next_write=(0, 2, False))
    seq = update_sequence("again", "x", writing, DATA_SIZE, "fcn_flash")
    assert len(seq.ops) == 3 + 37 + 2 and seq.ops[0].role == "slot.erase"
    assert seq.ops[-1].after == State("confirmed", "trial", 3, "commit", 0)
    # The slot written is the one that is not CONFIRMED, here A.
    other_way = described("previous", "confirmed", 4, "confirm", next_write=(0, 4, False))
    seq = update_sequence("a", "x", other_way, 300, "fcn_flash")
    assert {op.address for op in seq.ops if op.role.startswith("slot.")} == {0x2000, 0x2100}
    with pytest.raises(ValueError):
        update_sequence("x", "x", described("confirmed", "trial", 3, "commit"), 300, "fcn_flash")


def test_a_trial_that_confirms_itself():
    base = described("confirmed", "trial", 3, "commit", next_write=(0, 3, False), record=(0, 2))
    seq = confirm_sequence("confirm", "trial-node", base)
    attempt, record, field = seq.ops
    # Attempt field 0 of entry 2: after the record's 96 bytes and its 8-byte commit field.
    assert (attempt.kind, attempt.address, attempt.length) == ("program", 2 * 128 + 104, 8)
    assert (attempt.program, attempt.in_program, attempt.start) == ("fcn_boot", 1, 0)
    assert (attempt.before.attempts, attempt.after.attempts) == (0, 1)
    assert [(op.program, op.in_program, op.address) for op in (record, field)] == [
        ("node", 1, 3 * 128), ("node", 2, 3 * 128 + 96)]
    assert field.after == State("previous", "confirmed", 4, "confirm", 0)
    assert seq.counts("node", 0) == {"programs": 2, "erases": 0, "erases_meta": 0}
    with pytest.raises(ValueError):
        confirm_sequence("x", "x", described("confirmed", "trial", 3, "commit", 1))
    with pytest.raises(ValueError):
        confirm_sequence("x", "x", FACTORY)


def test_a_trial_that_crashes_is_counted_three_times_and_rolled_back():
    base = described("confirmed", "trial", 96, "commit", next_write=(1, 0, True),
                     record=(0, 31))
    seq = rollback_sequence("rollback-wrap", "trial-crash-wrap", base)
    entry = 31 * 128
    assert [(op.role, op.start, op.program, op.in_program, op.address) for op in seq.ops] == [
        ("attempt.1", 0, "fcn_boot", 1, entry + 104),
        ("attempt.2", 1, "fcn_boot", 1, entry + 112),
        ("attempt.3", 2, "fcn_boot", 1, entry + 120),
        ("rollback.meta_erase", 3, "fcn_boot", 1, 0x1000),
        ("rollback.record", 3, "fcn_boot", 2, 0x1000),
        ("rollback.commit_field", 3, "fcn_boot", 3, 0x1060)]
    assert [op.after.attempts for op in seq.ops[:3]] == [1, 2, 3]
    assert seq.ops[-1].before == State("confirmed", "trial", 96, "commit", 3)
    assert seq.ops[-1].after == State("confirmed", "rejected", 97, "rollback", 0)
    assert seq.counts("fcn_boot", 3) == {"programs": 2, "erases": 1, "erases_meta": 1}


def test_a_sequence_without_a_wrap_has_no_wrapping_ops():
    with pytest.raises(ValueError):
        wrapping_ops(update_sequence("fresh", "f", FACTORY, DATA_SIZE, "fcn_flash"))


# ------------------------------------------------------------------------------- the log


def test_after_a_records_entry_comes_the_next_one_or_the_next_sector_erased():
    assert after_record(LogPosition(0, 5, False)) == LogPosition(0, 6, False)
    assert after_record(LogPosition(0, 31, False)) == LogPosition(1, 0, True)
    assert after_record(LogPosition(1, 31, True)) == LogPosition(0, 0, True)


def _record(seq: int) -> bytes:
    """One valid log entry (docs/update.md, the record and the commit field): seq, ADOPT,
    slot A confirmed (version 1.0.0, a zero id), slot B empty; erased attempt fields."""
    body = bytearray(b"FCNB" + bytes([1, 1, 0, 0xFF]) + struct.pack("<I", seq))
    body += bytes([3]) + b"\xff" * 3 + struct.pack("<I", 0x01000000) + bytes(32)
    body += bytes([0]) + b"\xff" * 3 + struct.pack("<I", 0xFFFFFFFF) + b"\xff" * 32
    body += struct.pack("<I", zlib.crc32(bytes(body)))
    entry = bytes(body) + flash_layout.commit_field(bytes(body), 8) + b"\xff" * 24
    assert len(entry) == 128
    return entry


def _meta(sector0: range, sector1: range) -> bytes:
    meta = bytearray(b"\xff" * flash_layout.META_SIZE)
    for sector, seqs in ((0, sector0), (1, sector1)):
        for index, seq in enumerate(seqs):
            at = sector * 4096 + index * 128
            meta[at:at + 128] = _record(seq)
    return bytes(meta)


def test_the_constructed_log_reads_as_built():
    md = flash_layout.read_metadata(_meta(range(1, 33), range(33, 41)))
    assert (md.newest.seq, md.newest_sector, md.newest_index, md.used) == (40, 1, 7, (32, 8))


def test_where_a_record_goes_and_whether_it_wraps_onto_older_records():
    # 31 entries in sector 0, sector 1 never used: the second record erases an empty sector.
    meta = _meta(range(1, 32), range(0))
    assert position_ahead(meta, 1) == (LogPosition(0, 31, False), 31)
    assert position_ahead(meta, 2) == (LogPosition(1, 0, True), 0)
    assert erase_ahead(meta, 2) and not wrap_ahead(meta, 2) and not erase_ahead(meta, 1)
    # Both sectors full, the newest in sector 1: the next record erases sector 0's 32 records.
    meta = _meta(range(1, 33), range(33, 65))
    assert position_ahead(meta, 1) == (LogPosition(0, 0, True), 32)
    assert wrap_ahead(meta, 1)
    # Two entries left in sector 1: the third record wraps.
    meta = _meta(range(1, 33), range(33, 63))
    assert not erase_ahead(meta, 1) and not erase_ahead(meta, 2) and wrap_ahead(meta, 3)
    with pytest.raises(ValueError):
        position_ahead(meta, 0)


# --------------------------------------------------------------------- the starts of the board


def test_what_each_start_writes():
    trial = State("confirmed", "trial", 3, "commit", 1)
    assert state_after_start(trial, Boot("B", "trial", 2, "NONE")) == replace_attempts(trial, 2)
    assert state_after_start(trial, Boot("B", "trial", 2, "NONE"), confirmed_itself=True) == \
        State("previous", "confirmed", 4, "confirm", 0)
    used = State("confirmed", "trial", 3, "commit", 3)
    assert state_after_start(used, Boot("A", "confirmed", 0, "ROLLBACK")) == \
        State("confirmed", "rejected", 4, "rollback", 0)
    assert state_after_start(trial, Boot("A", "confirmed", 0, "REJECT")) == \
        State("confirmed", "rejected", 4, "reject", 0)
    broken = State("previous", "confirmed", 4, "confirm", 0)
    assert state_after_start(broken, Boot("A", "confirmed", 0, "FALLBACK")) == \
        State("confirmed", "rejected", 5, "fallback", 0)
    assert state_after_start(broken, Boot("B", "confirmed", 0, "NONE")) == broken
    assert start_text(Boot("B", "trial", 2, "NONE")) == "B t2"
    assert start_text(Boot("A", "confirmed", 0, "ROLLBACK")) == "A c ROLLBACK"


def replace_attempts(state: State, attempts: int) -> State:
    return State(state.a, state.b, state.seq, state.event, attempts)


def test_the_state_of_a_description_and_its_compact_form():
    info = described("confirmed", "writing", 2, "begin", next_write=(0, 2, False))
    assert state_of(info) == State("confirmed", "writing", 2, "begin", 0)
    assert state_of(info).compact() == "CW 2:begin a0"
    assert powerloss.describe_state(info) == "CW 2:begin a0 u3"
    op = update_sequence("fresh", "f", FACTORY, DATA_SIZE, "fcn_flash").ops[1]
    assert left_by(op.before, op) == "before" and left_by(op.after, op) == "after"
    assert left_by(State("confirmed", "trial", 9, "commit", 0), op) == "neither"
    assert op.compact() == ("2 program 0x0000e0+8 begin.commit_field start 0 fcn_flash #2: "
                            "CE 1:adopt a0 -> CW 2:begin a0")


# ------------------------------------------------------------------------------- the cut


FATAL_PROGRAM = ("FATAL flash: power cut in operation 8 (program of 256 bytes at 0x102f00, "
                 "position 21 of 32, seed 9; 30 of 33 bits changed)\n")


def test_the_fatal_line_of_a_cut():
    cut = parse_cut("noise\n" + FATAL_PROGRAM)
    assert (cut.operation, cut.kind, cut.length, cut.address) == (8, "program", 256, 0x102F00)
    assert (cut.position, cut.positions, cut.seed, cut.done, cut.transitions) == (21, 32, 9, 30,
                                                                                  33)
    assert (cut.tear, cut.compact()) == ("some", "p21/32 30/33")
    erase = parse_cut("FATAL flash: power cut in operation 3 (erase of 4096 bytes at 0x102000, "
                      "position 15 of 16, seed 1; 0 of 0 bits changed)")
    assert (erase.kind, erase.tear) == ("erase", "nothing")
    line = ("FATAL flash: power cut in operation 2 (program of 8 bytes at 0x0000e0, position 0 "
            "of 1, seed {}; {} of 32 bits changed)")
    assert parse_cut(line.format(7, 0)).tear == "none"
    assert parse_cut(line.format(4, 32)).tear == "all"
    assert parse_cut("no cut here") is None
    with pytest.raises(ValueError):
        parse_cut(FATAL_PROGRAM * 2)


def test_a_cut_is_judged_against_the_operation_drawn():
    op = update_sequence("fresh", "f", FACTORY, DATA_SIZE, "fcn_flash").ops[1]   # 8 bytes at 0xe0
    line = ("FATAL flash: power cut in operation {} (program of {} bytes at 0x{:06x}, position "
            "0 of 1, seed {}; 3 of 32 bits changed)")
    assert cut_mismatch(parse_cut(line.format(2, 8, 0xE0, 5)), op, 5) is None
    for wrong in (line.format(3, 8, 0xE0, 5), line.format(2, 96, 0xE0, 5),
                  line.format(2, 8, 0x80, 5), line.format(2, 8, 0xE0, 6)):
        assert "not in operation 2 of fcn_flash" in cut_mismatch(parse_cut(wrong), op, 5)
    assert cut_mismatch(None, op, 5) == "the run wrote no FATAL line of a cut"


# ------------------------------------------------------------------------------- the plan


def test_every_cell_is_cut_exactly_as_often_as_asked_in_a_seeded_order():
    cells = {"stage": [Cell("stage", "fresh", n) for n in range(1, 45)],
             "boot": [Cell("boot", "rollback", n) for n in range(1, 6)]}
    plan = make_plan(cells, {"stage": 3, "boot": 7, "download": 2}, seed=11)
    assert len(plan) == 44 * 3 + 5 * 7
    assert [t.index for t in plan] == list(range(len(plan)))
    counts = {}
    for t in plan:
        counts[t.cell] = counts.get(t.cell, 0) + 1
    assert all(counts[c] == 3 for c in cells["stage"])
    assert all(counts[c] == 7 for c in cells["boot"])
    assert all(0 <= t.seed < 1 << 32 for t in plan)
    assert make_plan(cells, {"stage": 3, "boot": 7}, seed=11) == plan
    other = make_plan(cells, {"stage": 3, "boot": 7}, seed=12)
    assert [t.cell for t in other] != [t.cell for t in plan]
    # Shuffled: not class by class.
    assert {t.cell.cls for t in plan[:20]} == {"stage", "boot"}


def test_a_trial_is_replayed_from_its_class_sequence_op_and_seed():
    trial = Trial(5, Cell("boot", "rollback-wrap", 4), 123456789)
    assert trial.replay == "boot:rollback-wrap:4:123456789"
    assert parse_replay(trial.replay) == (trial.cell, trial.seed)
    for bad in ("boot:rollback:4", "nope:rollback:4:1", "boot:rollback:0:1",
                f"boot:rollback:4:{1 << 32}", "boot:rollback:x:1"):
        with pytest.raises(ValueError):
            parse_replay(bad)


def test_the_cells_of_a_sequence():
    full = described("confirmed", "rejected", 64, "rollback", next_write=(0, 0, True))
    seq = update_sequence("wrap-begin", "wrap-begin", full, DATA_SIZE, "node")
    assert cells_of("download", seq) == [Cell("download", "wrap-begin", n) for n in range(1, 46)]
    assert cells_of("download", seq, wrap=True) == [Cell("download", "wrap-begin", n)
                                                    for n in (1, 2, 3)]


# ------------------------------------------------------------------------------ the result


def trial_row(index: int, cls: str, role: str, *, left="before", tear="none", check=None,
              seconds=1.0, after="CW 2:begin a0 u3", boot="A c") -> dict:
    return {"index": index, "class": cls, "sequence": "fresh", "op": 1, "seed": 7, "role": role,
            "program": "fcn_flash", "cut": "p0/1 0/32", "tear": tear, "before": "CE 1:adopt a0",
            "after": after, "left": left, "boot": boot, "starts": ["A c 0"],
            "follow_up": "B 1.4.0 confirmed", "lock_retries": 0, "seconds": seconds,
            "check": check}


def test_a_row_is_one_line_and_reads_back():
    row = trial_row(3, "boot", "attempt.2") | {"starts": ["B t3 13", "A c ROLLBACK 0"]}
    text = row_text(row)
    assert "\n" not in text and text.count("|") == len(powerloss.COLUMNS) - 1
    assert parse_row(text) == row
    failed = trial_row(4, "stage", "slot.page", check="boot") | {"follow_up": None, "tear": None}
    assert parse_row(row_text(failed)) == failed
    with pytest.raises(ValueError):
        row_text(trial_row(5, "stage", "a|b"))
    with pytest.raises(ValueError):
        row_text(trial_row(5, "stage", "x") | {"starts": ["a;b"]})
    with pytest.raises(ValueError):
        parse_row("1|2")


def test_the_summary_counts_per_class_and_role():
    rows = [trial_row(0, "stage", "slot.page", seconds=2.0),
            trial_row(1, "stage", "commit.commit_field", left="after", tear="all",
                      after="CT 3:commit a0 u4", boot="B t1", seconds=4.0),
            trial_row(2, "stage", "commit.commit_field", tear="some", seconds=6.0),
            trial_row(3, "stage", "begin.record", check="invariants", left=None, seconds=1.0),
            trial_row(4, "boot", "attempt.1", left="after", tear="some", check="follow_up"),
            trial_row(5, "download", "slot.page", check="bench"),
            trial_row(6, "download", "slot.page", check="cut")]
    summary = summarize(rows)
    stage = summary["stage"]
    assert (stage["trials"], stage["failed"], stage["failed_by_check"]) == (4, 1,
                                                                           {"invariants": 1})
    assert list(stage["by_role"]) == ["begin.record", "slot.page", "commit.commit_field"]
    assert stage["by_role"]["commit.commit_field"] == {"trials": 2,
                                                      "left": {"after": 1, "before": 1},
                                                      "tear": {"all": 1, "some": 1}, "failed": 0}
    assert stage["by_role"]["begin.record"]["failed"] == 1
    assert stage["outcomes"] == {"CT -> B t1": 1, "CW -> A c": 3}
    assert stage["seconds_per_trial"]["n"] == 4 and stage["seconds_per_trial"]["max"] == 6.0
    assert stage["seconds_per_trial"]["mean"] == 3.25
    assert summary["download"]["failed_by_check"] == {"bench": 1, "cut": 1}

    head = headline(rows, wall_s=123.45, workers=8)
    results.check_summary(head)
    assert head["trials"] == {"value": 7, "unit": "trials"}
    assert {cls: f["value"] for cls, f in head["trials_per_class"].items()} == {
        "stage": 4, "download": 2, "boot": 1}
    assert head["invariant_violations"]["value"] == 1          # only "invariants" here
    assert (head["cuts_not_where_drawn"]["value"], head["follow_up_failures"]["value"],
            head["bench_errors"]["value"]) == (1, 1, 1)
    assert head["wall_time"] == {"value": 123.5, "unit": "s"}
    assert head["seconds_per_trial"]["boot"]["n"] == 1


def test_every_check_but_the_bench_cut_and_follow_up_ones_is_a_violation():
    assert set(powerloss.VIOLATIONS) == set(powerloss.CHECKS) - {"cut", "follow_up", "bench"}


# -------------------------------------------------------------------------------- the plot


def _load_bench():
    path = paths.REPO_ROOT / "bench" / "power_loss.py"
    spec = importlib.util.spec_from_file_location("bench_power_loss", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module      # dataclasses look their module up by name
    spec.loader.exec_module(module)
    return module


def test_the_plot_is_drawn_from_a_result(tmp_path):
    bench = _load_bench()
    rows = [trial_row(0, "stage", "slot.page"),
            trial_row(1, "boot", "attempt.1", left="after", seconds=9.5),
            trial_row(2, "download", "slot.page", check="bench", seconds=25.0)]
    result = {"summary": headline(rows, 30.0, 3), "by_class": summarize(rows),
              "trials": {"columns": list(powerloss.COLUMNS), "rows": [row_text(r) for r in rows]}}
    path = tmp_path / "power_loss.png"
    bench.plot(json.loads(json.dumps(result)), path)
    assert path.stat().st_size > 10_000


def test_the_default_campaign_has_at_least_1000_trials_and_cuts_every_cell_several_times():
    bench = _load_bench()
    # The cells per class with the default sequences: 44 + 3 + 3 for stage and download (the
    # data image, and the two wraps' record writes), 3 + 3 + 5 + 3 for boot.
    cells = {"stage": 50, "download": 50, "boot": 14}
    assert sum(cells[cls] * bench.REPEATS[cls] for cls in cells) >= 1000
    assert min(bench.REPEATS.values()) >= 3
