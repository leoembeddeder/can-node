# SPDX-License-Identifier: MIT
"""The power-loss campaign of bench/power_loss.py, as pure functions: which flash operations a
sequence of the update makes and what state each one leaves before and after it, the trials
(which operation of which sequence is cut, with which seed), the FATAL line a cut writes, what
each start of the board must do to the flash, and the figures of the result. Nothing here runs
a program, so tests/python checks all of it on constructed data; the bench runs the programs.

THE SEQUENCES (docs/update.md, the slot writer and the state machine; the geometry of the host's
flash, fcn_tools.flash). A record write is an erase of the next metadata sector when the used
part of the newest record's sector is full (the E step), then a program of the record's 96 bytes
(P1) and one of its 8-byte commit field (P2). Its state changes only with P2: a cut in E or P1
leaves the state before it, a cut in P2 the state before or the one after it.

  stage, download   the BEGIN record, one erase per sector of the image, one program per page
                    (the last one padded to whole units), the COMMIT record: an update of the slot
                    that is not CONFIRMED. Made by fcn_flash stage ("stage") or by the node's
                    flash task for a tester's download ("download"), all in one run.
  confirm           the bootloader's attempt (one 8-byte attempt field), then the node's CONFIRM
                    record: a trial that runs and confirms itself.
  rollback          three starts of a trial that crashes, each after its attempt, and the fourth
                    start's ROLLBACK record.

Each operation is numbered in the sequence (1, 2, ...) and in the run of the program that makes
it (fcn_flash, fcn_boot or the node), which is the N of that program's --flash-cut N: every
program counts the programs and erases since it opened the flash.

THE STATE a cut may leave is compared as a State: the two slots' states, the newest record's
sequence number and event, and the trial's attempts used. A cut of an operation must leave the
state before it or the state after it (decision 5's first invariant), and Op.before and Op.after
are those two, written from the state machine of docs/update.md, never read from the code under
test.
"""
from __future__ import annotations

import math
import random
import re
from collections import Counter
from dataclasses import dataclass, field, replace
from typing import Mapping, Sequence

from . import flash as flash_layout
from .results import distribution, figure
from .update_model import Boot

# ------------------------------------------------------------------------------ the geometry

SECTOR = flash_layout.SECTOR_SIZE
PAGE = flash_layout.PAGE_SIZE
UNIT = flash_layout.UNIT_SIZE
FIELD, BODY, _ENTRY, STRIDE = flash_layout.entry_sizes()
PER_SECTOR = SECTOR // STRIDE
META_SECTORS = flash_layout.META_SIZE // SECTOR
SLOT_START = {name: offset
              for name, (offset, _) in zip(flash_layout.SLOT_NAMES, flash_layout.SLOTS)}

#: BOOT_MAX_ATTEMPTS (lib/boot/boot.h).
MAX_ATTEMPTS = flash_layout.MAX_ATTEMPTS

#: One letter per slot state in a compact state.
LETTERS = {"empty": "E", "writing": "W", "trial": "T", "confirmed": "C", "previous": "P",
           "rejected": "R"}


def _round_up(value: int, to: int) -> int:
    return -(-value // to) * to


# --------------------------------------------------------------------------------- the state


@dataclass(frozen=True)
class State:
    """What a cut is judged by: the states of slots A and B, the newest record's sequence
    number and event, and the attempts the trial has used (0 without a trial)."""
    a: str
    b: str
    seq: int | None
    event: str | None
    attempts: int

    def slot(self, name: str) -> str:
        return self.a if name == "A" else self.b

    def with_slot(self, name: str, state: str) -> "State":
        return replace(self, a=state) if name == "A" else replace(self, b=state)

    def compact(self) -> str:
        """"CW 2:begin a0": slot A confirmed, B writing, record 2 (BEGIN), no attempt."""
        return f"{LETTERS[self.a]}{LETTERS[self.b]} {self.seq}:{self.event} a{self.attempts}"


def state_of(flash_info: Mapping) -> State:
    """The State of a flash description (fcn_flash info, fcn_tools.flash.describe())."""
    md = flash_info["metadata"]
    a, b = (slot["state"] for slot in flash_info["slots"])
    return State(a, b, md["seq"], md["event"], md["attempts_used"])


def describe_state(flash_info: Mapping) -> str:
    """The compact state with the log's entries in use: "CW 2:begin a0 u3"."""
    return f"{state_of(flash_info).compact()} u{flash_info['metadata']['entries_used']}"


def other(slot: str) -> str:
    return "B" if slot == "A" else "A"


def confirmed_slot(state: State) -> str:
    return "A" if state.a == "confirmed" else "B"


def trial_slot(state: State) -> str | None:
    if state.a == "trial":
        return "A"
    return "B" if state.b == "trial" else None


# ----------------------------------------------------------------------------- the log


@dataclass(frozen=True)
class LogPosition:
    """Where the next record goes: its sector and entry, and whether that sector is erased
    first (the used part of the newest record's sector is full)."""
    sector: int
    index: int
    erases: bool


def next_position(flash_info: Mapping) -> LogPosition:
    md = flash_info["metadata"]
    return LogPosition(md["next_sector"], md["next_index"], bool(md["next_write_erases"]))


def after_record(position: LogPosition) -> LogPosition:
    """Where the record after the one written at `position` goes (docs/update.md, writing a
    record: the entry after it, or entry 0 of the next sector of the ring, erased first)."""
    if position.index + 1 < PER_SECTOR:
        return LogPosition(position.sector, position.index + 1, False)
    return LogPosition((position.sector + 1) % META_SECTORS, 0, True)


def entry_address(sector: int, index: int) -> int:
    return flash_layout.META_OFFSET + sector * SECTOR + index * STRIDE


def attempt_address(sector: int, index: int, attempt: int) -> int:
    """The attempt field of attempt `attempt` (1 to 3) of the record in entry (sector, index):
    after the record's body and its commit field, one field per attempt."""
    return entry_address(sector, index) + BODY + FIELD + (attempt - 1) * FIELD


def position_ahead(meta: bytes, records: int) -> tuple[LogPosition, int]:
    """Where the `records`-th record written from now on goes (1: the next one), with no cut in
    between, and how many entries the sector it goes to uses now (those an erase of it
    destroys). `meta` is the bytes of the metadata area."""
    if records < 1:
        raise ValueError("records count from 1")
    md = flash_layout.read_metadata(meta)
    sector, index, erases = flash_layout.next_write(md)
    position = LogPosition(sector, index, erases)
    for _ in range(records - 1):
        position = after_record(position)
    return position, md.used[position.sector]


def erase_ahead(meta: bytes, records: int) -> bool:
    """Whether the `records`-th record written from now on erases a metadata sector first."""
    return position_ahead(meta, records)[0].erases


def wrap_ahead(meta: bytes, records: int) -> bool:
    """Whether the `records`-th record written from now on erases a metadata sector that holds
    used entries: the log's wrap, which destroys older records."""
    position, used = position_ahead(meta, records)
    return position.erases and used > 0


# ------------------------------------------------------------------------- the sequences


@dataclass(frozen=True)
class Op:
    """One flash operation of a sequence (see the module text)."""
    number: int        # in the sequence, from 1
    kind: str          # "program" or "erase"
    address: int
    length: int
    role: str          # "begin.record", "slot.page", "attempt.2", ...
    start: int         # the start of the board (or the tool's run) that makes it, from 0
    program: str       # "fcn_flash", "fcn_boot" or "node": whose --flash-cut counts it
    in_program: int    # its number in that program's run: the N of --flash-cut N
    before: State      # the state before the operation
    after: State       # the state once it has completed

    def allowed(self) -> tuple[State, ...]:
        """The states a cut of this operation may leave."""
        return (self.before,) if self.before == self.after else (self.before, self.after)

    def compact(self) -> str:
        """"2 program 0x0000e0+8 begin.commit_field start 0 fcn_flash #2: CE 1:adopt a0 -> CW
        2:begin a0": the number, the kind, the address and length, the role, the start, the
        program and its N, the state before and the state after."""
        return (f"{self.number} {self.kind} 0x{self.address:06x}+{self.length} {self.role} "
                f"start {self.start} {self.program} #{self.in_program}: {self.before.compact()} "
                f"-> {self.after.compact()}")


@dataclass(frozen=True)
class FlashSequence:
    """A sequence of the campaign: its name, the base flash it starts from, and its ops."""
    name: str
    base: str
    ops: tuple[Op, ...]

    def counts(self, program: str | None = None, start: int | None = None) -> dict[str, int]:
        """Programs, erases and metadata erases, of one program's run if given."""
        chosen = [op for op in self.ops if (program is None or op.program == program)
                  and (start is None or op.start == start)]
        return {"programs": sum(op.kind == "program" for op in chosen),
                "erases": sum(op.kind == "erase" for op in chosen),
                "erases_meta": sum(op.role.endswith(".meta_erase") for op in chosen)}

    def op(self, number: int) -> Op:
        if not 1 <= number <= len(self.ops):
            raise ValueError(f"{self.name} has operations 1 to {len(self.ops)}, not {number}")
        return self.ops[number - 1]


class _Builder:
    def __init__(self) -> None:
        self.ops: list[Op] = []
        self._counts: Counter = Counter()

    def add(self, kind: str, address: int, length: int, role: str, start: int, program: str,
            before: State, after: State) -> None:
        self._counts[(start, program)] += 1
        self.ops.append(Op(len(self.ops) + 1, kind, address, length, role, start, program,
                           self._counts[(start, program)], before, after))

    def record(self, event: str, position: LogPosition, before: State, after: State,
               start: int, program: str) -> LogPosition:
        """The ops of one record write (E, P1, P2); the position of the record after it."""
        if position.erases:
            self.add("erase", flash_layout.META_OFFSET + position.sector * SECTOR, SECTOR,
                     f"{event}.meta_erase", start, program, before, before)
        address = entry_address(position.sector, position.index)
        self.add("program", address, BODY, f"{event}.record", start, program, before, before)
        self.add("program", address + BODY, FIELD, f"{event}.commit_field", start, program,
                 before, after)
        return after_record(position)


def update_sequence(name: str, base: str, base_info: Mapping, image_size: int,
                    program: str) -> FlashSequence:
    """The ops of an update of the slot that is not CONFIRMED with an image of `image_size`
    bytes, made by `program` in one run (fcn_flash stage, or the node for a download)."""
    s0 = state_of(base_info)
    slot = other(confirmed_slot(s0))
    if s0.slot(slot) == "trial":
        raise ValueError("no update begins while a trial is pending")
    b = _Builder()
    position = next_position(base_info)
    if s0.slot(slot) == "writing":         # begin() of a WRITING slot writes no record
        s1 = s0
    else:
        s1 = replace(s0.with_slot(slot, "writing"), seq=s0.seq + 1, event="begin", attempts=0)
        position = b.record("begin", position, s0, s1, 0, program)
    start = SLOT_START[slot]
    for sector in range(math.ceil(image_size / SECTOR)):
        b.add("erase", start + sector * SECTOR, SECTOR, "slot.erase", 0, program, s1, s1)
    for page in range(math.ceil(image_size / PAGE)):
        rest = image_size - page * PAGE
        if rest >= PAGE:
            b.add("program", start + page * PAGE, PAGE, "slot.page", 0, program, s1, s1)
        else:
            b.add("program", start + page * PAGE, _round_up(rest, UNIT), "slot.last_page", 0,
                  program, s1, s1)
    s2 = replace(s1.with_slot(slot, "trial"), seq=s1.seq + 1, event="commit", attempts=0)
    b.record("commit", position, s1, s2, 0, program)
    return FlashSequence(name, base, tuple(b.ops))


def _trial_record(base_info: Mapping) -> tuple[State, str, str, int, int]:
    s0 = state_of(base_info)
    trial = trial_slot(s0)
    if trial is None or s0.attempts != 0:
        raise ValueError(f"the base must hold a trial with no attempt used, not {s0.compact()}")
    md = base_info["metadata"]
    return s0, trial, confirmed_slot(s0), md["record_sector"], md["record_index"]


def confirm_sequence(name: str, base: str, base_info: Mapping) -> FlashSequence:
    """A trial that runs and confirms itself: the bootloader's first attempt, then the node's
    CONFIRM record (the node's first flash operations, 5 s after its start)."""
    s0, trial, active, sector, index = _trial_record(base_info)
    b = _Builder()
    s1 = replace(s0, attempts=1)
    b.add("program", attempt_address(sector, index, 1), FIELD, "attempt.1", 0, "fcn_boot", s0, s1)
    s2 = replace(s1.with_slot(trial, "confirmed").with_slot(active, "previous"), seq=s1.seq + 1,
                 event="confirm", attempts=0)
    b.record("confirm", next_position(base_info), s1, s2, 0, "node")
    return FlashSequence(name, base, tuple(b.ops))


def rollback_sequence(name: str, base: str, base_info: Mapping) -> FlashSequence:
    """A trial that crashes at every start: three starts, each after its attempt, and the
    fourth start's ROLLBACK record, all written by the bootloader."""
    s0, trial, _, sector, index = _trial_record(base_info)
    b = _Builder()
    state = s0
    for attempt in range(1, MAX_ATTEMPTS + 1):
        nxt = replace(state, attempts=attempt)
        b.add("program", attempt_address(sector, index, attempt), FIELD, f"attempt.{attempt}",
              attempt - 1, "fcn_boot", state, nxt)
        state = nxt
    done = replace(state.with_slot(trial, "rejected"), seq=state.seq + 1, event="rollback",
                   attempts=0)
    b.record("rollback", next_position(base_info), state, done, MAX_ATTEMPTS, "fcn_boot")
    return FlashSequence(name, base, tuple(b.ops))


def wrapping_ops(sequence: FlashSequence) -> list[Op]:
    """The ops of the record write in `sequence` that begins with a metadata erase: the erase,
    the record and its commit field."""
    erases = [op for op in sequence.ops if op.role.endswith(".meta_erase")]
    if len(erases) != 1:
        raise ValueError(f"{sequence.name} has {len(erases)} metadata erases, not one")
    event = erases[0].role.split(".")[0]
    return [op for op in sequence.ops if op.role in (f"{event}.meta_erase", f"{event}.record",
                                                    f"{event}.commit_field")]


# --------------------------------------------------------------------- the starts of the board


def start_text(boot: Boot) -> str:
    """A start (fcn_tools.update_model.Boot) in a few letters: "B t2" (the trial in slot B,
    attempt 2), "A c" (the confirmed image), "A c ROLLBACK" (after the bootloader's rollback)."""
    text = f"{boot.slot} {'t' + str(boot.attempt) if boot.mode == 'trial' else 'c'}"
    return text if boot.event == "NONE" else f"{text} {boot.event}"


def state_after_start(state: State, boot: Boot, confirmed_itself: bool = False) -> State:
    """The State a start leaves (docs/update.md, the state machine), the start being `boot`
    (what expected_boot() said for `state`): a trial's attempt is counted; a rollback or a
    rejection makes the trial REJECTED; a fallback makes the confirmed image REJECTED and the
    previous one CONFIRMED; a start of the confirmed image writes nothing. With
    `confirmed_itself`, the trial started went on to write its CONFIRM record in that run."""
    if boot.mode == "trial":
        counted = replace(state, attempts=boot.attempt)
        if not confirmed_itself:
            return counted
        active = confirmed_slot(state)
        return replace(counted.with_slot(boot.slot, "confirmed").with_slot(active, "previous"),
                       seq=state.seq + 1, event="confirm", attempts=0)
    if boot.event in ("ROLLBACK", "REJECT"):
        trial = trial_slot(state)
        return replace(state.with_slot(trial, "rejected"), seq=state.seq + 1,
                       event=boot.event.lower(), attempts=0)
    if boot.event == "FALLBACK":
        failed = other(boot.slot)
        return replace(state.with_slot(failed, "rejected").with_slot(boot.slot, "confirmed"),
                       seq=state.seq + 1, event="fallback", attempts=0)
    return state


# ------------------------------------------------------------------------------ the cut


#: The line a cut writes to stderr (platform/posix/posix_flash.c, report_cut()).
FATAL = re.compile(r"FATAL flash: power cut in operation (\d+) \((program|erase) of (\d+) bytes "
                   r"at 0x([0-9a-fA-F]+), position (\d+) of (\d+), seed (\d+); (\d+) of (\d+) "
                   r"bits changed\)")


@dataclass(frozen=True)
class Cut:
    """What a cut's FATAL line says: the operation of the run, its kind, length and address,
    the position cut, the seed, and how many of the operation's transitions were done."""
    operation: int
    kind: str
    length: int
    address: int
    position: int
    positions: int
    seed: int
    done: int
    transitions: int

    @property
    def tear(self) -> str:
        """"none", "some" or "all" of the transitions done; "nothing" when the torn unit or
        sector had no bit to change (a unit of 0xFF bytes, an erase of an erased sector)."""
        if self.transitions == 0:
            return "nothing"
        if self.done == 0:
            return "none"
        return "all" if self.done == self.transitions else "some"

    def compact(self) -> str:
        """"p12/32 0/36": cut at position 12 of 32, 0 of 36 transitions done."""
        return f"p{self.position}/{self.positions} {self.done}/{self.transitions}"


def parse_cut(stderr: str) -> Cut | None:
    """The cut a run's stderr reports, or None; ValueError for more than one."""
    found = FATAL.findall(stderr)
    if len(found) > 1:
        raise ValueError(f"{len(found)} FATAL lines of a cut in one run")
    if not found:
        return None
    op, kind, length, address, position, positions, seed, done, transitions = found[0]
    return Cut(int(op), kind, int(length), int(address, 16), int(position), int(positions),
               int(seed), int(done), int(transitions))


def cut_mismatch(cut: Cut | None, op: Op, seed: int) -> str | None:
    """Why `cut` is not the cut of `op` with `seed` (its program's operation in_program, its
    kind, length and address), or None."""
    if cut is None:
        return "the run wrote no FATAL line of a cut"
    want = (op.in_program, op.kind, op.length, op.address, seed)
    got = (cut.operation, cut.kind, cut.length, cut.address, cut.seed)
    if want == got:
        return None
    return (f"the cut fell in operation {cut.operation} ({cut.kind} of {cut.length} bytes at "
            f"0x{cut.address:06x}, seed {cut.seed}), not in operation {op.in_program} of "
            f"{op.program} ({op.kind} of {op.length} bytes at 0x{op.address:06x}, {op.role}, "
            f"seed {seed})")


# ---------------------------------------------------------------------------- the trials

#: The classes of the campaign, in the order of the result.
CLASSES = ("stage", "download", "boot")


@dataclass(frozen=True)
class Cell:
    """An operation of a sequence that the campaign cuts: (class, sequence, operation)."""
    cls: str
    sequence: str
    op: int


@dataclass(frozen=True)
class Trial:
    index: int
    cell: Cell
    seed: int

    @property
    def replay(self) -> str:
        """The trial as --replay takes it: "class:sequence:operation:seed"."""
        return f"{self.cell.cls}:{self.cell.sequence}:{self.cell.op}:{self.seed}"


def parse_replay(spec: str) -> tuple[Cell, int]:
    """"class:sequence:operation:seed" (Trial.replay) as a cell and a seed; ValueError."""
    parts = spec.split(":")
    if len(parts) != 4 or parts[0] not in CLASSES:
        raise ValueError(f"a trial is class:sequence:operation:seed with a class of "
                         f"{', '.join(CLASSES)}, not {spec!r}")
    cell = Cell(parts[0], parts[1], int(parts[2]))
    seed = int(parts[3])
    if not 0 <= seed < 1 << 32 or cell.op < 1:
        raise ValueError(f"bad operation or seed in {spec!r}")
    return cell, seed


def cells_of(cls: str, sequence: FlashSequence, wrap: bool = False) -> list[Cell]:
    """The cells of a sequence: every operation, or for a sequence that only adds a wrap of the
    log (`wrap`), the operations of the record write that begins with the metadata erase."""
    ops = wrapping_ops(sequence) if wrap else sequence.ops
    return [Cell(cls, sequence.name, op.number) for op in ops]


def make_plan(cells: Mapping[str, Sequence[Cell]], repeats: Mapping[str, int],
              seed: int) -> list[Trial]:
    """The trials: every cell of every class `repeats[class]` times, each with a seed of its
    own drawn uniformly from 0 to 2**32 - 1, in an order shuffled from `seed`. The same
    arguments give the same plan."""
    draw = random.Random(seed)
    chosen = []
    for cls in CLASSES:
        for cell in cells.get(cls, ()):
            for _ in range(repeats.get(cls, 0)):
                chosen.append((cell, draw.randrange(1 << 32)))
    draw.shuffle(chosen)
    return [Trial(index, cell, cut_seed) for index, (cell, cut_seed) in enumerate(chosen)]


# --------------------------------------------------------------------------- the result

#: The checks of a trial, in the order they are made, and what a failure of each means.
CHECKS = {
    "cut": "the run did not end as a power loss (12) in the operation drawn",
    "decoder": "fcn_flash info and fcn_tools.flash read the flash differently",
    "invariants": "check_invariants() does not hold",
    "state": "the state is neither the one before the operation cut nor the one after it",
    "boot": "a start after the cut did not do what expected_boot() says",
    "attempts": "a trial was started more than three times in all",
    "follow_up": "the next update did not succeed and confirm",
    "bench": "the bench could not run the trial (a program did not start, a wait ran out)",
}

#: Decision 5's invariants (docs/update.md): a failure of one of these is a violation.
VIOLATIONS = ("decoder", "invariants", "state", "boot", "attempts")

#: The columns of a trial's row (a dict; the result keeps each row as one line, row_text()),
#: and those that hold whole numbers, a number of seconds and a list of starts.
COLUMNS = ("index", "class", "sequence", "op", "seed", "role", "program", "cut", "tear",
           "before", "after", "left", "boot", "starts", "follow_up", "lock_retries", "seconds",
           "check")
_INTEGERS = ("index", "op", "seed", "lock_retries")
_SEPARATOR, _STARTS = "|", ";"

#: The roles of the operations in the order of the sequences, for the counts and the plot.
ROLE_ORDER = ("begin.meta_erase", "begin.record", "begin.commit_field", "slot.erase",
              "slot.page", "slot.last_page", "commit.meta_erase", "commit.record",
              "commit.commit_field", "attempt.1", "attempt.2", "attempt.3", "confirm.meta_erase",
              "confirm.record", "confirm.commit_field", "rollback.meta_erase", "rollback.record",
              "rollback.commit_field")


def role_rank(role: str) -> tuple[int, str]:
    return (ROLE_ORDER.index(role) if role in ROLE_ORDER else len(ROLE_ORDER), role)


def row_text(trial: Mapping) -> str:
    """A trial's row as one line of the result: the COLUMNS separated by "|", the starts by
    ";", an absent value empty. parse_row() reads it back."""
    fields = []
    for column in COLUMNS:
        value = trial.get(column)
        if value is None:
            fields.append("")
        elif column == "starts":
            fields.append(_STARTS.join(value))
        else:
            fields.append(str(value))
    for column, value in zip(COLUMNS, fields):
        if _SEPARATOR in value or "\n" in value or (column == "starts" and any(
                _STARTS in start for start in trial.get("starts") or ())):
            raise ValueError(f"the {column} of a row may not hold {_SEPARATOR!r}, {_STARTS!r} "
                             f"or a line break: {value!r}")
    return _SEPARATOR.join(fields)


def parse_row(text: str) -> dict:
    """A row of row_text() as a dict of the COLUMNS with their types."""
    fields = text.split(_SEPARATOR)
    if len(fields) != len(COLUMNS):
        raise ValueError(f"a row has {len(COLUMNS)} fields, not {len(fields)}: {text!r}")
    row: dict = {}
    for column, value in zip(COLUMNS, fields):
        if value == "":
            row[column] = None
        elif column in _INTEGERS:
            row[column] = int(value)
        elif column == "seconds":
            row[column] = float(value)
        elif column == "starts":
            row[column] = value.split(_STARTS)
        else:
            row[column] = value
    return row


def left_by(state: State, op: Op) -> str:
    """Whether the state a cut left is the one before `op` ("before"), after it ("after";
    for an op that changes no state, the same: "before"), or neither."""
    if state == op.before:
        return "before"
    if state == op.after:
        return "after"
    return "neither"


def summarize(trials: Sequence[Mapping], classes: Sequence[str] = CLASSES) -> dict:
    """The counts of the campaign from its trial rows (dicts with the COLUMNS): per class the
    trials, the failed ones by check, per operation role the trials and what the cuts left
    (the state before or after, the tear), the outcomes (the state the board came back in and
    what its first start did), and the time per trial."""
    out: dict = {}
    for cls in classes:
        mine = [t for t in trials if t["class"] == cls]
        by_role: dict = {}
        for t in mine:
            entry = by_role.setdefault(t["role"], {"trials": 0, "left": Counter(),
                                                   "tear": Counter(), "failed": 0})
            entry["trials"] += 1
            if t.get("left") is not None:
                entry["left"][t["left"]] += 1
            if t.get("tear") is not None:
                entry["tear"][t["tear"]] += 1
            if t.get("check") is not None:
                entry["failed"] += 1
        outcomes = Counter(f"{t['after'].split(' ')[0]} -> {t['boot']}" for t in mine
                           if t.get("after") and t.get("boot"))
        out[cls] = {
            "trials": len(mine),
            "failed": sum(t.get("check") is not None for t in mine),
            "failed_by_check": dict(Counter(t["check"] for t in mine if t.get("check"))),
            "sequences": dict(Counter(t["sequence"] for t in mine)),
            "by_role": {role: {"trials": e["trials"], "left": dict(e["left"]),
                               "tear": dict(e["tear"]), "failed": e["failed"]}
                        for role, e in sorted(by_role.items(),
                                              key=lambda item: role_rank(item[0]))},
            "outcomes": dict(sorted(outcomes.items())),
            "seconds_per_trial": distribution([t["seconds"] for t in mine], "s"),
        }
    return out


def headline(trials: Sequence[Mapping], wall_s: float, workers: int,
             classes: Sequence[str] = CLASSES) -> dict:
    """The summary the README may quote, each figure with its unit."""
    def count(predicate) -> int:
        return sum(1 for t in trials if predicate(t))
    return {
        "trials": figure(len(trials), "trials"),
        "trials_per_class": {cls: figure(count(lambda t, c=cls: t["class"] == c), "trials")
                             for cls in classes},
        "invariant_violations": figure(count(lambda t: t.get("check") in VIOLATIONS), "trials"),
        "cuts_not_where_drawn": figure(count(lambda t: t.get("check") == "cut"), "trials"),
        "follow_up_failures": figure(count(lambda t: t.get("check") == "follow_up"), "trials"),
        "bench_errors": figure(count(lambda t: t.get("check") == "bench"), "trials"),
        "seconds_per_trial": {cls: distribution([t["seconds"] for t in trials
                                                 if t["class"] == cls], "s")
                              for cls in classes},
        "wall_time": figure(round(wall_s, 1), "s"),
        "workers": figure(workers, "processes"),
    }


@dataclass
class Notes:
    """What a trial collects on its way besides its row: the build ids its node runs reported
    and the starts that were made again because a file was still locked."""
    build_ids: set[str] = field(default_factory=set)
    lock_retries: int = 0
