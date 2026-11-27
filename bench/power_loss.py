#!/usr/bin/env python3
"""The power-loss campaign: cut the flash's supply in the middle of an update, again and again,
and check after every cut what docs/update.md promises: the board starts what the reference model
says and can be updated again.

Every trial copies a base flash, runs one sequence of the update until the supply is cut in one
of its flash operations, then starts the board again through fcn_boot, as after a loss of supply,
and runs the next update. Three classes of trial, each cut drawn over every flash operation of
its sequence (fcn_tools.powerloss has the sequences, operation by operation):

  stage     fcn_flash stage of an image: the BEGIN record, the slot's erases, its page programs,
            the COMMIT record. The next update is a staging of the node, which confirms itself.
  download  the same operations made by the node's flash task for a tester's download over the
            bus (fcn_tools.tester.download(): eraseMemory, RequestDownload, TransferData,
            RequestTransferExit, checkProgrammingDependencies). The next update is a download
            of the node with the flasher (python -m fcn_tools.flasher), confirmed by its routine.
  boot      the bootloader's attempt and the node's CONFIRM record of a trial that runs and
            confirms itself, and the three attempts and the ROLLBACK record of a trial that
            crashes at every start. The next update is a staging, as for "stage".

The image cut in "stage" and "download" is the 9256-byte data image of the phase 5 tests (44
operations: 2 + 3 erases + 37 pages + 2); the node's own image (252,560 bytes at the final runs)
would make 1053, too many to cut each of them several times. Each of these classes also runs at
two places of the metadata log where its BEGIN or its COMMIT record is the first of a sector,
and so erases a sector that holds older records first; "boot" the same for its CONFIRM and ROLLBACK records.

After every cut: the run ended as a power loss (12) with the FATAL line of the operation drawn;
`fcn_flash info` and fcn_tools.flash read the flash the same; check_invariants() holds; the state
is the one before the operation or the one after it; every start after the cut does what
expected_boot() (fcn_tools.update_model, the reference, not the code under test) says, a trial is
started at most three times in all and then rolled back; and the next update succeeds and
confirms. A trial that fails a check is kept with its seed, its cut and what it found, and the
campaign goes on: a failure is a finding, never left out of the result.

The cuts are armed on the command lines (--flash-cut N:SEED of fcn_flash, fcn_boot or the node):
the operation is counted by the program that makes it and the tear is drawn from the seed, so
what a cut leaves does not depend on the host's timing, and the trials run in parallel worker
processes, each trial with its own flash file, retained cell and UDP ports. Nothing is armed at
run time.

    python bench/power_loss.py                        # the campaign: 1062 trials, 8 workers
    python bench/power_loss.py --quick --out /tmp/pl  # one trial per class, about half a minute
    python bench/power_loss.py --replay boot:rollback:5:123456789   # one trial again, kept
    python bench/power_loss.py --replot               # redraw results/power_loss.png

The default campaign is estimated from the verification's rehearsal at about 27 minutes on the
8-CPU WSL2 VM of this project (a trial took about 6 s for "stage", 10 s for "boot" and 25 to
30 s for "download", whose next update is a download of 244 KB at the node's 12 KB/s); the
result records the wall time it took.

Exit status: 0 the result was written and every trial passed; 5 the result was written and holds
failed trials (a finding: see its "failures"); 1 an error; 2 a usage error; 3 a loaded host; 4 a
binary not built from the tree; 128 + N ended by signal N. --replay: 0 the trial passed, 5 it
failed, 2 a malformed trial.
"""
from __future__ import annotations

import argparse
import json
import multiprocessing
import os
import queue
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from fcn_tools import bench as bench_module
from fcn_tools import paths
from fcn_tools import bus as bus_module
from fcn_tools import flash as flash_layout
from fcn_tools import plotstyle, powerloss, results, runner, tester
from fcn_tools.node import (BOOT_JUMP_FAILED, EXIT_OK, RESET_IMAGE_FAULT, RESET_POWER_LOSS,
                            RESET_REQUESTED, RESETS)
from fcn_tools.powerloss import (CLASSES, Cell, FlashSequence, Notes, Op, Trial,
                                 describe_state, start_text, state_after_start, state_of)
from fcn_tools.update_model import MAX_ATTEMPTS, Boot, check_invariants, expected_boot

NAME = "power_loss"

#: The seed of the default plan (the order of the trials and every cut's seed).
CAMPAIGN_SEED = 20261123

#: Each cell (an operation of a sequence) is cut this many times per class by default. "stage"
#: and "download" have 50 cells each (the 44 operations of the data image's update, and the
#: record writes, three operations each, that begin a metadata sector at the two places of the
#: log), "boot" 14 (confirm 3 + its wrap 3, rollback 5 + its wrap 3): 400 + 200 + 462 = 1062.
#: "boot" gets the most per cell because each of its operations changes the metadata, and a
#: one-unit program (an attempt, a commit field) has three outcomes (none, some or all of its
#: transitions done) that each must occur; "download" the fewest, because its next update takes
#: 25 s.
REPEATS = {"stage": 8, "download": 4, "boot": 33}

#: Worker processes by default. A worker runs one trial at a time: a node, now and then fcn_boot
#: or fcn_flash, and for "download" a tester. In pilot runs (under shared load) six workers kept
#: 1.7 of the VM's 8 logical CPUs busy and twelve 3.5, with no window of good health cut short.
DEFAULT_WORKERS = min(8, os.cpu_count() or 1)

#: The images (fcn_tools.bench.make_images()) and what each is to the campaign.
FACTORY_IMAGE = "node-1.0.0"        # slot A of every base flash, the confirmed image
CUT_IMAGE = "data"                  # what "stage" and "download" write when they are cut
TRIAL_IMAGE = "node-1.1.0"          # the trial of the "confirm" sequences
CRASH_IMAGE = "crash-stripped"      # the trial of "rollback" and of the log's history
FOLLOW_UP_IMAGE = "node-1.4.0"      # the next update of every trial
#: What runs from an image, by its version: the node, the crash payload (exits 13 at once) or
#: nothing (the data image is no program: fcn_boot's exec fails, 42).
IMAGE_KINDS = {"1.0.0": "node", "1.1.0": "node", "1.4.0": "node", "1.2.0": "crash",
               "1.5.0": "data"}

#: The bases: name -> (the sequences that start from it). The log's history is made of
#: crashing trials rolled back (see build_bases()).
#:   factory           fcn_flash factory of FACTORY_IMAGE: ADOPT, slot B empty
#:   wrap-begin        the log full up to the end of a sector: BEGIN erases the next one
#:   wrap-commit       one entry left in the sector: COMMIT erases the next one
#:   trial-node(-wrap) a staged TRIAL_IMAGE, attempts 0 (-wrap: CONFIRM erases)
#:   trial-crash(-wrap) a staged CRASH_IMAGE, attempts 0 (-wrap: ROLLBACK erases)
SEQUENCES = {  # (class, sequence) -> (base, kind of sequence, only the log's wrap)
    ("stage", "fresh"): ("factory", "update", False),
    ("stage", "wrap-begin"): ("wrap-begin", "update", True),
    ("stage", "wrap-commit"): ("wrap-commit", "update", True),
    ("download", "fresh"): ("factory", "update", False),
    ("download", "wrap-begin"): ("wrap-begin", "update", True),
    ("download", "wrap-commit"): ("wrap-commit", "update", True),
    ("boot", "confirm"): ("trial-node", "confirm", False),
    ("boot", "confirm-wrap"): ("trial-node-wrap", "confirm", True),
    ("boot", "rollback"): ("trial-crash", "rollback", False),
    ("boot", "rollback-wrap"): ("trial-crash-wrap", "rollback", True),
}
#: --quick: one trial of each class, of these (class, sequence, role).
QUICK = (("stage", "fresh", "commit.commit_field"), ("download", "fresh", "slot.page"),
         ("boot", "rollback", "attempt.2"))

#: Bounds of the waits, never timing tolerances: a node or a tool that takes longer has failed.
RUN_WAIT_S = 30.0          # a run that ends by itself (a cut, a crash, --duration-s)
CONFIRM_WAIT_S = 30.0      # a trial's confirm: 5 s of good health, the rest is room
START_WAIT_S = 10.0        # the first status frame of a node
FLASHER_TIMEOUT_S = 240.0  # tester.FLASHER_TIMEOUT_S: a download of the node at 500 kbit/s
TOOL_TIMEOUT_S = 120.0
#: The restart after a cut: at most this many starts until the board runs a confirmed image
#: (three attempts, the rollback, the image: five).
MAX_STARTS = 6
#: A start that finds the flash or the cell still locked by the run before it is made again,
#: this often at most, after this pause, and counted (the row's lock_retries). A flock lives as
#: long as its open file description, which a process that holds the ended run's memory map for
#: a moment (one that reads its /proc entries does) keeps alive after the run's exit. Since
#: docs/bug-log.md 012 every run releases its locks itself before it ends (a cut, a reset, an
#: orderly end), so only a run that was killed can leave one, and the count is expected to be
#: 0. Such a start ends at its first open (status 1, "in use by another ...") and changes
#: nothing in the flash.
LOCK_RETRIES = 3
LOCK_RETRY_S = 0.2
#: The log's history is at most this many crashing trials long.
MAX_CYCLES = 64
#: The node's log line of a confirm (app/app.c).
CONFIRMED_LINE = "is the active image"
#: Exit status of a written result that holds failed trials.
EXIT_FINDINGS = 5
#: The name of the node binary that is packed into the images, in the result's "build".
PACKED = "fcn_node (packed into the images)"
#: The classes as the plot names them: who makes the operations cut.
CLASS_LABELS = {"stage": "stage (fcn_flash)", "download": "download (node)",
                "boot": "boot (fcn_boot, node)"}


class TrialFailure(Exception):
    """A check of a trial failed: `check` is one of powerloss.CHECKS."""

    def __init__(self, check: str, message: str) -> None:
        super().__init__(message)
        self.check = check


@dataclass(frozen=True)
class Context:
    """What every trial needs, handed to the workers."""
    workdir: Path
    binaries: runner.Binaries
    images: dict[str, Path]
    bases: dict[str, Path]
    sequences: dict[tuple[str, str], FlashSequence]
    keep: bool


# -------------------------------------------------------------------------- the programs


def _run(argv: list, timeout: float = TOOL_TIMEOUT_S) -> subprocess.CompletedProcess:
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True,
                          timeout=timeout, check=False, stdin=subprocess.DEVNULL)


def read_flash(ctx_flash_tool: Path, flash: Path) -> dict:
    """`fcn_flash info` of `flash`, after checking that fcn_tools.flash reads the same (the
    decoder check); TrialFailure("decoder") names the first member that differs."""
    run = _run([ctx_flash_tool, "info", "--flash-file", flash])
    if run.returncode != 0:
        raise TrialFailure("bench", f"fcn_flash info ended with {run.returncode}: {run.stderr}")
    reported = json.loads(run.stdout)
    decoded = flash_layout.describe(flash.read_bytes())
    without_counts = {key: value for key, value in reported.items() if key != "flash"}
    if decoded != without_counts:
        raise TrialFailure("decoder", f"fcn_flash info and fcn_tools.flash disagree: "
                                      f"{_first_difference(without_counts, decoded)}")
    return reported


def _first_difference(a, b, path: str = "") -> str:
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if a.get(key) != b.get(key):
                return _first_difference(a.get(key), b.get(key), f"{path}.{key}")
    if isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for i, (x, y) in enumerate(zip(a, b)):
            if x != y:
                return _first_difference(x, y, f"{path}[{i}]")
    return f"{path or '(top)'}: tool {a!r}, decoder {b!r}"


def invariants(flash_info: dict, where: str) -> None:
    try:
        check_invariants(flash_info)
    except AssertionError as error:
        raise TrialFailure("invariants", f"{where}: {error}") from None


def stage(ctx: Context, flash: Path, image: str, *cut: str) -> subprocess.CompletedProcess:
    return _run([ctx.binaries.flash, "stage", "--flash-file", flash, "--no-wait", *cut,
                 ctx.images[image]])


def image_kind(flash_info: dict, slot: str) -> str:
    header = flash_info["slots"]["AB".index(slot)]["header"]
    version = header["version"] if header else None
    if version not in IMAGE_KINDS:
        raise TrialFailure("boot", f"slot {slot} holds no image of the campaign ({version})")
    return IMAGE_KINDS[version]


# ---------------------------------------------------------------------- starts of the board


def _wait(node, timeout: float = RUN_WAIT_S) -> int:
    """The exit status of a run that ends by itself; a reset counts as one the bench waited for
    (the session's end checks still report a power loss without a cut armed)."""
    try:
        status = node.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        node.kill()
        raise TrialFailure("bench", f"{node.name} did not end within {timeout} s") from None
    if status in RESETS:
        node.note_reset_awaited()
    return status


def _in_use(node, status: int) -> bool:
    return status == 1 and "in use by another" in node.stderr()


def start(session: runner.Session, notes: Notes, name: str, flash: Path, cell: Path, wait,
          node_args: tuple = (), boot_args: tuple = ()):
    """A start of the board through fcn_boot, waited for with `wait(node)`, which returns the
    exit status; the node and its status. A start that found a file still locked by the run
    before it is made again (LOCK_RETRIES) and counted: it ended before it changed anything."""
    for attempt in range(LOCK_RETRIES + 1):
        node = session.start_node(*node_args, name=name if attempt == 0 else f"{name}-{attempt}",
                                  flash_file=flash, retained_file=cell, boot_args=boot_args)
        status = wait(node)
        if not _in_use(node, status):
            return node, status
        notes.lock_retries += 1
        time.sleep(LOCK_RETRY_S)
    raise TrialFailure("bench", f"{name}: the flash or the cell stayed locked "
                                f"({LOCK_RETRIES + 1} starts)\n{node.stderr()}")


def wait_confirmed(node) -> int:
    """For a trial of the node: wait until its log says it confirmed itself, then stop it in
    order; its exit status. CONFIRM_WAIT_S only bounds the wait."""
    deadline = time.monotonic() + CONFIRM_WAIT_S
    while CONFIRMED_LINE not in node.log():
        if not node.running:
            return _wait(node)
        if time.monotonic() >= deadline:
            node.stop()
            raise TrialFailure("bench", f"{node.name} did not confirm itself within "
                                        f"{CONFIRM_WAIT_S} s")
        time.sleep(0.05)
    return node.stop()


def _node_stats(node, notes: Notes, where: str) -> dict:
    try:
        stats = node.stats()
    except FileNotFoundError:
        raise TrialFailure("boot", f"{where}: {node.name} ended in order but wrote no "
                                   "statistics") from None
    notes.build_ids.add(stats.get("build_id", "missing"))
    return stats


def _handoff(stats: dict, boot: Boot, where: str, check: str = "boot") -> None:
    try:
        bench_module.check_handoff(stats, boot)
    except AssertionError as error:
        raise TrialFailure(check, f"{where}: the node's hand-off is not {start_text(boot)}: "
                                  f"{error}") from None


def one_start(ctx: Context, session, notes: Notes, name: str, flash: Path, cell: Path,
              cur: dict, power_on: bool) -> tuple[Boot, int, dict, bool]:
    """The next start of the board on the flash `cur` describes: what expected_boot() says it
    must do, checked from the image's own report (the node's hand-off, the crash payload's log
    line, or for the data image fcn_boot's exit 42 and its log) and from the flash afterwards
    (state_after_start()). `power_on`: the first start after a cut, whose reset cause must be
    POWER_ON. Returns the start, its exit status, the flash after it, and whether the board now
    runs a confirmed image (or a trial that confirmed itself in this run)."""
    try:
        boot = expected_boot(cur)
    except AssertionError as error:
        raise TrialFailure("boot", f"{name}: the reference finds nothing to start: {error}") \
            from None
    kind = image_kind(cur, boot.slot)
    where = f"{name} ({start_text(boot)}, {kind})"
    confirmed_itself = False
    if kind == "node" and boot.mode == "confirmed":
        node, status = start(session, notes, name, flash, cell, _wait,
                             node_args=("--duration-s", "1"))
        if status != EXIT_OK:
            raise TrialFailure("boot", f"{where} ended with {status}\n{node.stderr()}")
        stats = _node_stats(node, notes, where)
        _handoff(stats, boot, where)
        if power_on and stats["reset"]["cause"] != "POWER_ON":
            raise TrialFailure("boot", f"{where}: reset cause {stats['reset']['cause']}, not "
                                       "POWER_ON after a loss of supply")
        done = True
    elif kind == "node":
        node, status = start(session, notes, name, flash, cell, wait_confirmed)
        if status != EXIT_OK:
            raise TrialFailure("boot", f"{where} ended with {status}\n{node.stderr()}")
        stats = _node_stats(node, notes, where)
        _handoff(stats, boot, where)
        if power_on and stats["reset"]["cause"] != "POWER_ON":
            raise TrialFailure("boot", f"{where}: reset cause {stats['reset']['cause']}, not "
                                       "POWER_ON after a loss of supply")
        if stats["confirm"]["state"] != "CONFIRMED":
            raise TrialFailure("boot", f"{where}: confirm state {stats['confirm']['state']}")
        confirmed_itself = done = True
    elif kind == "crash":
        node, status = start(session, notes, name, flash, cell, _wait)
        line = f"payload crash: hand-off slot {boot.slot} {boot.mode} attempt {boot.attempt}, "
        if status != RESET_IMAGE_FAULT or line not in node.log():
            raise TrialFailure("boot", f"{where} ended with {status}; its log: {node.log()!r}")
        if power_on and "reset cause POWER_ON" not in node.log():
            raise TrialFailure("boot", f"{where}: not a power-on: {node.log()!r}")
        done = False
    else:  # data: no program
        node, status = start(session, notes, name, flash, cell, _wait)
        line = f"select: slot {boot.slot} as a trial, attempt {boot.attempt} of {MAX_ATTEMPTS}"
        if status != BOOT_JUMP_FAILED or line not in node.boot_log():
            raise TrialFailure("boot", f"{where} ended with {status}; fcn_boot's log: "
                                       f"{node.boot_log()!r}")
        if power_on and "reset cause POWER_ON" not in node.boot_log():
            raise TrialFailure("boot", f"{where}: not a power-on: {node.boot_log()!r}")
        done = False
    after = read_flash(ctx.binaries.flash, flash)
    invariants(after, f"after {where}")
    want = state_after_start(state_of(cur), boot, confirmed_itself)
    if state_of(after) != want:
        raise TrialFailure("boot", f"{where} left {state_of(after).compact()}, not "
                                   f"{want.compact()}")
    return boot, status, after, done


def restart(ctx: Context, session, notes: Notes, flash: Path, cell: Path, cur: dict,
            trial_starts: int) -> tuple[str, list[str], dict]:
    """The starts of the board after a cut until it runs a confirmed image: the first start
    (start_text()), every start with its exit status, and the flash at the end. A trial is
    started at most MAX_ATTEMPTS times in all, counting the `trial_starts` made before the cut,
    and never more often than attempts were counted for it (TrialFailure("attempts"))."""
    starts: list[str] = []
    first = None
    for n in range(MAX_STARTS):
        boot, status, after, done = one_start(ctx, session, notes, f"start-{n + 1}", flash,
                                              cell, cur, power_on=(n == 0))
        first = first or start_text(boot)
        starts.append(f"{start_text(boot)} {status}")
        if boot.mode == "trial":
            trial_starts += 1
            if trial_starts > MAX_ATTEMPTS or trial_starts > boot.attempt:
                raise TrialFailure("attempts", f"start {n + 1} is the trial's start "
                                               f"{trial_starts} in all, after attempt "
                                               f"{boot.attempt} was counted")
        cur = after
        if done:
            return first, starts, cur
    raise TrialFailure("boot", f"no confirmed image runs after {MAX_STARTS} starts: {starts}")


# ----------------------------------------------------------------------- the next update


def follow_up_stage(ctx: Context, session, notes: Notes, flash: Path, cell: Path,
                    workdir: Path) -> str:
    """The next update as a staging: FOLLOW_UP_IMAGE staged into the slot that is not
    CONFIRMED, started by fcn_boot as a trial, confirmed by itself after its window."""
    run = stage(ctx, flash, FOLLOW_UP_IMAGE)
    if run.returncode != 0:
        raise TrialFailure("follow_up", f"fcn_flash stage ended with {run.returncode}: "
                                        f"{run.stdout}{run.stderr}")
    staged = read_flash(ctx.binaries.flash, flash)
    invariants(staged, "after the next staging")
    state = state_of(staged)
    slot = powerloss.trial_slot(state)
    if slot is None or state.attempts != 0 or image_kind(staged, slot) != "node":
        raise TrialFailure("follow_up", f"the next staging left {state.compact()}")
    node, status = start(session, notes, "update", flash, cell, wait_confirmed)
    if status != EXIT_OK:
        alive = [line for line in node.log().splitlines() if " alive: " in line][-3:]
        raise TrialFailure("follow_up", f"the update's trial ended with {status}: "
                                        f"{' / '.join(node.stderr().strip().splitlines()[-3:])}; "
                                        f"its last alive lines: {' / '.join(alive)}")
    stats = _node_stats(node, notes, "the update's trial")
    _handoff(stats, Boot(slot, "trial", 1, "NONE"), "the update's trial", "follow_up")
    return check_update_confirmed(ctx, flash, slot, stats)


def check_update_confirmed(ctx: Context, flash: Path, slot: str, stats: dict) -> str:
    if stats["confirm"]["state"] != "CONFIRMED":
        raise TrialFailure("follow_up", f"the update's confirm state is "
                                        f"{stats['confirm']['state']}")
    after = read_flash(ctx.binaries.flash, flash)
    invariants(after, "after the next update")
    state = state_of(after)
    version = after["slots"]["AB".index(slot)]["header"]["version"]
    want = FOLLOW_UP_IMAGE.split("-")[1]
    if (state.slot(slot), state.slot(powerloss.other(slot)), state.event, version) != (
            "confirmed", "previous", "confirm", want):
        raise TrialFailure("follow_up", f"after the next update: {state.compact()}, slot "
                                        f"{slot} holds {version}, not a confirmed {want}")
    return f"{slot} {version} confirmed"


def _board_end(board) -> str:
    """How the Board's run ended when it ended by itself: the evidence of a failed follow-up
    (a watchdog reset says the node was not kicked, and its log says when it last was)."""
    if board.unexpected_end is None:
        return ""
    run = board.current
    stderr = " / ".join(run.stderr().strip().splitlines()[-3:])
    alive = [line for line in run.log().splitlines() if " alive: " in line][-3:]
    return (f"; the board's run {run.name} ended by itself with {board.unexpected_end}: "
            f"{stderr}; its last alive lines: {' / '.join(alive)}")


def follow_up_download(ctx: Context, session, notes: Notes, flash: Path, cell: Path,
                       workdir: Path) -> str:
    """The next update as a download: the flasher downloads FOLLOW_UP_IMAGE into the slot that
    is not running, resets the node (a Board starts it again through fcn_boot) and confirms the
    trial with its routine (or finds that it confirmed itself first)."""
    slot = powerloss.other(powerloss.confirmed_slot(state_of(read_flash(ctx.binaries.flash,
                                                                         flash))))
    board = session.start_board(flash_file=flash, retained_file=cell, name="update")
    board.expect_resets(1)
    report_path = workdir / "flasher.json"
    try:
        run = subprocess.run([sys.executable, "-m", "fcn_tools.flasher", "--can",
                              session.can_spec, "--report", str(report_path),
                              str(ctx.images[FOLLOW_UP_IMAGE])], capture_output=True, text=True,
                             timeout=FLASHER_TIMEOUT_S, check=False, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        raise TrialFailure("follow_up", f"the flasher did not end within {FLASHER_TIMEOUT_S} s") \
            from None
    report = json.loads(report_path.read_text()) if report_path.exists() else {}
    if run.returncode != 0 or report.get("result") != "confirmed":
        raise TrialFailure("follow_up", f"the flasher ended with {run.returncode}, result "
                                        f"{report.get('result')}, failed step "
                                        f"{report.get('failed_step')}{_board_end(board)}\n"
                                        f"{run.stderr[-1500:]}")
    if len(board.runs) != 2 or board.runs[0].wait(timeout=RUN_WAIT_S) != RESET_REQUESTED:
        raise TrialFailure("follow_up", f"the board ran {len(board.runs)} runs, not the image "
                                        "and, after the requested reset, the trial"
                                        f"{_board_end(board)}")
    try:
        stats = session.finish(board.current)
    except runner.BenchFailure as error:
        raise TrialFailure("follow_up", str(error)) from None
    notes.build_ids.add(stats.get("build_id", "missing"))
    _handoff(stats, Boot(slot, "trial", 1, "NONE"), "the downloaded trial", "follow_up")
    text = check_update_confirmed(ctx, flash, slot, stats)
    return f"{text} by the {report.get('confirmed_by')}"


# --------------------------------------------------------------------------- the cuts


def check_cut(status: int, stderr: str, op: Op, seed: int, row: dict) -> None:
    if status != RESET_POWER_LOSS:
        raise TrialFailure("cut", f"the run ended with {status}, not {RESET_POWER_LOSS} (a "
                                  f"loss of supply)\n{stderr[-2000:]}")
    try:
        cut = powerloss.parse_cut(stderr)
    except ValueError as error:
        raise TrialFailure("cut", str(error)) from None
    mismatch = powerloss.cut_mismatch(cut, op, seed)
    if mismatch is not None:
        raise TrialFailure("cut", mismatch)
    row["cut"], row["tear"] = cut.compact(), cut.tear


def cut_stage(ctx: Context, session, notes: Notes, op: Op, seed: int, flash: Path,
              cell: Path, row: dict) -> int:
    run = stage(ctx, flash, CUT_IMAGE, "--flash-cut", f"{op.in_program}:{seed}")
    check_cut(run.returncode, run.stderr, op, seed, row)
    return 0


def cut_download(ctx: Context, session, notes: Notes, op: Op, seed: int, flash: Path,
                 cell: Path, row: dict) -> int:
    reader = session.open_bus()
    try:
        node = session.start_node("--flash-cut", f"{op.in_program}:{seed}", name="cut",
                                  flash_file=flash, retained_file=cell)
        first = bus_module.wait_for(reader, bus_module.is_status, timeout=START_WAIT_S)
    finally:
        reader.shutdown()
    if first is None:
        raise TrialFailure("bench", f"no status frame within {START_WAIT_S} s\n{node.stderr()}")
    completed = False
    failed = None
    with tester.Uds(session.can_spec) as uds:
        try:
            tester.download(uds, ctx.images[CUT_IMAGE].read_bytes(), tester.SLOT_B,
                            alive=lambda: node.running)
            completed = True
        except tester.NodeEnded:
            pass
        except AssertionError as error:     # an answer the download did not expect
            failed = str(error)
    if node.running:
        if not completed and failed is not None:
            node.stop()
            raise TrialFailure("bench", f"the download failed with the node running: {failed}")
        node.stop()
        raise TrialFailure("cut", f"the download completed, but operation {op.number} "
                                  f"({op.role}) was to be cut")
    check_cut(_wait(node), node.stderr(), op, seed, row)
    return 0


def cut_boot(ctx: Context, session, notes: Notes, op: Op, seed: int, flash: Path,
             cell: Path, row: dict) -> int:
    """The starts of the sequence before the one that is cut (the crashing trial's), then that
    one with the cut on fcn_boot's or the node's command line. Returns the trial's starts."""
    starts = 0
    for k in range(op.start):
        node, status = start(session, notes, f"before-{k + 1}", flash, cell, _wait)
        if status != RESET_IMAGE_FAULT or f"trial attempt {k + 1}, " not in node.log():
            raise TrialFailure("bench", f"start {k + 1} before the cut ended with {status}: "
                                        f"{node.log()!r}")
        starts += 1
    armed = ("--flash-cut", f"{op.in_program}:{seed}")
    if op.program == "fcn_boot":
        node, status = start(session, notes, "cut", flash, cell, _wait, boot_args=armed)
        if status == RESET_POWER_LOSS and node.log_path.exists():
            raise TrialFailure("cut", "the bootloader jumped to the image before its cut")
    else:
        node, status = start(session, notes, "cut", flash, cell, _wait, node_args=armed)
        starts += 1                          # the trial ran, up to its confirm
        if status == RESET_POWER_LOSS and "writing its confirm" not in node.log():
            raise TrialFailure("cut", "the node was cut before it asked for its confirm")
    check_cut(status, node.stderr(), op, seed, row)
    return starts


CUTS = {"stage": cut_stage, "download": cut_download, "boot": cut_boot}


# -------------------------------------------------------------------------------- a trial


def run_trial(ctx: Context, trial: Trial) -> dict:
    """One trial (see the module text); its row (powerloss.COLUMNS). Never raises for a failed
    check: the row says which check failed and why."""
    began = time.monotonic()
    cell_ = trial.cell
    sequence = ctx.sequences[(cell_.cls, cell_.sequence)]
    op = sequence.op(cell_.op)
    row: dict = {"index": trial.index, "class": cell_.cls, "sequence": cell_.sequence,
                 "op": op.number, "seed": trial.seed, "role": op.role, "program": op.program,
                 "before": op.before.compact()}
    notes = Notes()
    workdir = ctx.workdir / "trials" / f"{trial.index:05d}"
    shutil.rmtree(workdir, ignore_errors=True)
    workdir.mkdir(parents=True)
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    shutil.copyfile(ctx.bases[sequence.base], flash)
    try:
        with runner.Session(workdir, binaries=ctx.binaries) as session:
            trial_starts = CUTS[cell_.cls](ctx, session, notes, op, trial.seed, flash, cell, row)
            torn = read_flash(ctx.binaries.flash, flash)
            row["after"] = describe_state(torn)
            invariants(torn, "after the cut")
            row["left"] = powerloss.left_by(state_of(torn), op)
            if row["left"] == "neither":
                raise TrialFailure("state", f"the cut left {state_of(torn).compact()}, neither "
                                            f"{op.before.compact()} nor {op.after.compact()}")
            row["boot"], row["starts"], _ = restart(ctx, session, notes, flash, cell, torn,
                                                    trial_starts)
            follow_up = follow_up_download if cell_.cls == "download" else follow_up_stage
            row["follow_up"] = follow_up(ctx, session, notes, flash, cell, workdir)
    except TrialFailure as failure:
        row["check"], row["message"] = failure.check, str(failure)[:4000]
    except runner.BenchFailure as failure:          # the session's end checks
        row["check"], row["message"] = "bench", str(failure)[:4000]
    except (KeyboardInterrupt, SystemExit):
        raise
    except Exception as error:  # noqa: BLE001 - every other error is the trial's, kept
        row["check"], row["message"] = "bench", f"{type(error).__name__}: {error}"[:4000]
    row["lock_retries"] = notes.lock_retries
    row["build_ids"] = sorted(notes.build_ids)
    row["seconds"] = round(time.monotonic() - began, 3)
    if row.get("check") is None and not ctx.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    return row


# ----------------------------------------------------------------------- the preparation


def _boot_command(binaries: runner.Binaries, flash: Path, cell: Path, log: Path,
                  *node_args: str) -> list:
    return [binaries.boot, "--flash-file", flash, "--retained-file", cell, "--log-file", log,
            "--", *node_args]


def crash_cycle(ctx_like: Context, flash: Path, scratch: Path) -> None:
    """One step of the log's history: a crashing trial staged (BEGIN, COMMIT), started three
    times, and rolled back by the fourth start (ROLLBACK), whose image is asked only for its
    version: three records."""
    run = stage(ctx_like, flash, CRASH_IMAGE)
    if run.returncode != 0:
        raise RuntimeError(f"staging the history's trial: {run.returncode} {run.stderr}")
    cell = scratch / "cycle.cell"
    for attempt in range(1, MAX_ATTEMPTS + 1):
        done = _run(_boot_command(ctx_like.binaries, flash, cell, scratch / "cycle.log"))
        if done.returncode != RESET_IMAGE_FAULT:
            raise RuntimeError(f"the history's trial, start {attempt}: {done.returncode} "
                               f"{done.stderr}")
    done = _run(_boot_command(ctx_like.binaries, flash, cell, scratch / "cycle.log", "--version"))
    if done.returncode != EXIT_OK:
        raise RuntimeError(f"the history's rollback: {done.returncode} {done.stderr}")


def build_bases(ctx_like: Context, wanted: set[str], directory: Path) -> tuple[dict, dict]:
    """The base flashes in `directory`: their paths and how each was made (its state, the
    crashing trials of its history, its SHA-256). Every one is read both ways and checked."""
    directory.mkdir(parents=True, exist_ok=True)
    paths_: dict[str, Path] = {}
    made: dict[str, dict] = {}
    factory = directory / "factory.bin"
    done = _run([ctx_like.binaries.flash, "factory", "--flash-file", factory, "--no-wait",
                 ctx_like.images[FACTORY_IMAGE]])
    if done.returncode != 0:
        raise RuntimeError(f"fcn_flash factory: {done.returncode} {done.stderr}")
    paths_["factory"], made["factory"] = factory, {"cycles": 0}

    def staged(name: str, source: Path, image: str, cycles: int) -> Path:
        target = directory / f"{name}.bin"
        shutil.copyfile(source, target)
        run = stage(ctx_like, target, image)
        if run.returncode != 0:
            raise RuntimeError(f"staging {image} for {name}: {run.returncode} {run.stderr}")
        paths_[name], made[name] = target, {"cycles": cycles, "staged": image}
        return target

    for name, image in (("trial-node", TRIAL_IMAGE), ("trial-crash", CRASH_IMAGE)):
        if name in wanted:
            staged(name, factory, image, 0)
    # The log's history: crashing trials rolled back, three records each, until each wrap is
    # where its sequence needs it, with older records in the sector that is erased.
    need = {name for name in ("wrap-begin", "wrap-commit", "trial-node-wrap",
                              "trial-crash-wrap") if name in wanted}
    if need:
        work = directory / "history.bin"
        shutil.copyfile(factory, work)
        for cycles in range(1, MAX_CYCLES + 1):
            crash_cycle(ctx_like, work, directory)
            meta = work.read_bytes()[:flash_layout.META_SIZE]
            erases = [powerloss.erase_ahead(meta, r) for r in (1, 2, 3)]
            wraps = [powerloss.wrap_ahead(meta, r) for r in (1, 2, 3)]
            if "wrap-begin" in need and wraps[0]:
                paths_["wrap-begin"] = directory / "wrap-begin.bin"
                shutil.copyfile(work, paths_["wrap-begin"])
                made["wrap-begin"] = {"cycles": cycles}
                need.discard("wrap-begin")
            if "wrap-commit" in need and wraps[1] and not erases[0]:
                paths_["wrap-commit"] = directory / "wrap-commit.bin"
                shutil.copyfile(work, paths_["wrap-commit"])
                made["wrap-commit"] = {"cycles": cycles}
                need.discard("wrap-commit")
            if wraps[2] and not erases[0] and not erases[1]:
                for name, image in (("trial-node-wrap", TRIAL_IMAGE),
                                    ("trial-crash-wrap", CRASH_IMAGE)):
                    if name in need:
                        staged(name, work, image, cycles)
                        need.discard(name)
            if not need:
                break
        if need:
            raise RuntimeError(f"no history of {MAX_CYCLES} crashing trials gives {need}")
        work.unlink()
    for name, path in paths_.items():
        flash_info = read_flash(ctx_like.binaries.flash, path)
        check_invariants(flash_info)
        made[name].update({"state": describe_state(flash_info), "sha256": results.sha256_of(path),
                           "next_write": [flash_info["metadata"]["next_sector"],
                                          flash_info["metadata"]["next_index"],
                                          flash_info["metadata"]["next_write_erases"]]})
    return paths_, made


def make_sequences(ctx_like: Context, keys) -> dict[tuple[str, str], FlashSequence]:
    out = {}
    size = ctx_like.images[CUT_IMAGE].stat().st_size
    for cls, name in keys:
        base, kind, _ = SEQUENCES[(cls, name)]
        flash_info = read_flash(ctx_like.binaries.flash, ctx_like.bases[base])
        if kind == "update":
            program = "fcn_flash" if cls == "stage" else "node"
            out[(cls, name)] = powerloss.update_sequence(name, base, flash_info, size, program)
        elif kind == "confirm":
            out[(cls, name)] = powerloss.confirm_sequence(name, base, flash_info)
        else:
            out[(cls, name)] = powerloss.rollback_sequence(name, base, flash_info)
    return out


def reference(ctx: Context, key: tuple[str, str], directory: Path) -> dict:
    """The sequence run once without a cut: the operations its programs counted must be those
    of the model (fcn_tools.powerloss), and the state at its end the last operation's after.
    Returns what was counted; RuntimeError when the model is wrong."""
    cls, name = key
    sequence = ctx.sequences[key]
    work = directory / f"{cls}-{name}"
    work.mkdir(parents=True, exist_ok=True)
    flash, cell = work / "flash.bin", work / "cell.bin"
    shutil.copyfile(ctx.bases[sequence.base], flash)
    counted: dict = {}
    notes = Notes()
    with runner.Session(work, binaries=ctx.binaries) as session:
        if cls == "stage":
            run = stage(ctx, flash, CUT_IMAGE)
            if run.returncode != 0:
                raise RuntimeError(f"the reference stage: {run.returncode} {run.stderr}")
            report = json.loads(run.stdout)["flash"]
            counted["fcn_flash"] = {key_: report[key_] for key_ in ("programs", "erases",
                                                                     "erases_meta")}
        elif cls == "download":
            reader = session.open_bus()
            try:
                node = session.start_node(name="reference", flash_file=flash, retained_file=cell)
                if bus_module.wait_for(reader, bus_module.is_status,
                                       timeout=START_WAIT_S) is None:
                    raise RuntimeError("the reference node sent no status frame")
            finally:
                reader.shutdown()
            with tester.Uds(session.can_spec) as uds:
                tester.download(uds, ctx.images[CUT_IMAGE].read_bytes(), tester.SLOT_B,
                                alive=lambda: node.running)
            stats = session.finish(node)
            counted["node"] = {key_: stats["flash"][key_] for key_ in ("programs", "erases",
                                                                        "erases_meta")}
            notes.build_ids.add(stats["build_id"])
        elif name.startswith("confirm"):
            node, status = start(session, notes, "reference", flash, cell, wait_confirmed)
            if status != EXIT_OK:
                raise RuntimeError(f"the reference trial ended with {status}: {node.stderr()}")
            stats = _node_stats(node, notes, "the reference trial")
            # The trial's slot is the one its confirm makes CONFIRMED.
            trial = powerloss.confirmed_slot(sequence.ops[-1].after)
            bench_module.check_handoff(stats, Boot(trial, "trial", 1, "NONE"))
            counted["node"] = {key_: stats["flash"][key_] for key_ in ("programs", "erases",
                                                                        "erases_meta")}
        else:
            for k in range(MAX_ATTEMPTS):
                node, status = start(session, notes, f"reference-{k + 1}", flash, cell, _wait)
                if status != RESET_IMAGE_FAULT:
                    raise RuntimeError(f"the reference trial's start {k + 1} ended with {status}")
            node, status = start(session, notes, "reference-image", flash, cell, _wait,
                                 node_args=("--duration-s", "1"))
            if status != EXIT_OK:
                raise RuntimeError(f"the reference rollback's image ended with {status}")
            stats = _node_stats(node, notes, "the reference rollback")
            active = powerloss.confirmed_slot(sequence.ops[-1].after)
            bench_module.check_handoff(stats, Boot(active, "confirmed", 0, "ROLLBACK"))
    final = state_of(read_flash(ctx.binaries.flash, flash))
    for program, counts in counted.items():
        start_ = 0 if program != "fcn_boot" else None
        want = sequence.counts(program, start_)
        if counts != want:
            raise RuntimeError(f"{cls}:{name}: {program} counted {counts}, the model says {want}")
    if final != sequence.ops[-1].after:
        raise RuntimeError(f"{cls}:{name}: the reference run ended at {final.compact()}, the "
                           f"model says {sequence.ops[-1].after.compact()}")
    shutil.rmtree(work, ignore_errors=True)
    return {"counted": counted, "final": final.compact(), "build_ids": sorted(notes.build_ids)}


def prepare(args, binaries: runner.Binaries, workdir: Path, keys: list) -> tuple[Context, dict]:
    """Images, bases, sequences and the reference runs for the sequences `keys`."""
    images = bench_module.make_images(_mkdir(workdir / "images"))
    shell = Context(workdir, binaries, images, {}, {}, args.keep)
    wanted = {SEQUENCES[key][0] for key in keys}
    bases, made = build_bases(shell, wanted, workdir / "bases")
    shell = Context(workdir, binaries, images, bases, {}, args.keep)
    sequences = make_sequences(shell, keys)
    ctx = Context(workdir, binaries, images, bases, sequences, args.keep)
    refs = {f"{cls}:{name}": reference(ctx, (cls, name), workdir / "references")
            for cls, name in keys}
    about = {"images": {name: {"size": path.stat().st_size, "sha256": results.sha256_of(path)}
                        for name, path in images.items()},
             "bases": made, "references": refs}
    return ctx, about


def _mkdir(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    return path


def plan_for(args, sequences: dict) -> tuple[list[Trial], dict]:
    cells: dict[str, list[Cell]] = {cls: [] for cls in CLASSES}
    for (cls, name), sequence in sequences.items():
        cells[cls] += powerloss.cells_of(cls, sequence, wrap=SEQUENCES[(cls, name)][2])
    if args.quick:
        chosen = {}
        for cls, name, role in QUICK:
            ops = [op for op in sequences[(cls, name)].ops if op.role == role]
            chosen[cls] = [Cell(cls, name, ops[0].number)]
        return powerloss.make_plan(chosen, {cls: 1 for cls in CLASSES}, args.seed), \
            {cls: len(c) for cls, c in chosen.items()}
    repeats = {"stage": args.stage_repeats, "download": args.download_repeats,
               "boot": args.boot_repeats}
    return powerloss.make_plan(cells, repeats, args.seed), {cls: len(c) for cls, c in cells.items()}


# ----------------------------------------------------------------------------- the workers


def _worker(ctx: Context, tasks, done) -> None:
    signal.signal(signal.SIGINT, signal.SIG_IGN)     # the main process ends the workers
    while True:
        waited = time.monotonic()
        trial = tasks.get()
        if trial is None:
            return
        began = time.monotonic()
        try:
            row = run_trial(ctx, trial)
        except KeyboardInterrupt:       # SIGTERM from the main process: the session cleaned up
            return
        # For the journal only: when the worker took the trial and how long it waited for it
        # (CLOCK_MONOTONIC, the same clock in every process of the host).
        row["journal"] = {"worker": os.getpid(), "took_at": round(began, 3),
                          "waited_s": round(began - waited, 3),
                          "ended_at": round(time.monotonic(), 3)}
        done.put(row)


def _interrupted(signum, frame) -> None:  # noqa: ARG001 - a signal handler
    raise runner.Interrupted(signum)


def campaign(ctx: Context, plan: list[Trial], workers: int, journal: Path) -> list[dict]:
    """Run the trials in `workers` processes; their rows, in the order they finished. Every row
    also goes to `journal` (JSON lines) as it comes."""
    mp = multiprocessing.get_context("spawn")
    tasks, done = mp.Queue(), mp.Queue()
    procs = [mp.Process(target=_worker, args=(ctx, tasks, done), name=f"{NAME}-{i}")
             for i in range(min(workers, len(plan)))]
    previous = {s: signal.signal(s, _interrupted) for s in (signal.SIGTERM, signal.SIGHUP)}
    rows: list[dict] = []
    began = last = time.monotonic()
    try:
        for proc in procs:
            proc.start()
        for trial in plan:
            tasks.put(trial)
        for _ in procs:
            tasks.put(None)
        with open(journal, "a", encoding="utf-8") as out:
            while len(rows) < len(plan):
                try:
                    row = done.get(timeout=5)
                except queue.Empty:
                    if not any(proc.is_alive() for proc in procs):
                        raise RuntimeError(f"the workers ended with {len(plan) - len(rows)} "
                                           f"trials not done (journal: {journal})") from None
                    continue
                rows.append(row)
                row.setdefault("journal", {})["received_at"] = round(time.monotonic(), 3)
                out.write(json.dumps(row) + "\n")
                out.flush()
                now = time.monotonic()
                if now - last >= 60 or len(rows) == len(plan):
                    last = now
                    failed = sum(r.get("check") is not None for r in rows)
                    rate = (now - began) / len(rows)
                    print(f"{len(rows)}/{len(plan)} trials, {failed} failed, {now - began:.0f} s, "
                          f"about {rate * (len(plan) - len(rows)):.0f} s left", flush=True)
        for proc in procs:
            proc.join(timeout=30)
    finally:
        for proc in procs:
            if proc.is_alive():
                proc.terminate()
        for proc in procs:
            proc.join(timeout=15)
            if proc.is_alive():
                proc.kill()
        for s, handler in previous.items():
            signal.signal(s, handler)
    return rows


# ------------------------------------------------------------------------------- the result


LEGEND = {
    "trials": "one line per trial, its columns (as named) separated by |, the starts by ;, an "
              "absent value empty (fcn_tools.powerloss.parse_row() reads it): class, sequence "
              "and op (the operation cut, numbered in its sequence; fcn_tools.powerloss), seed "
              "(the cut's), role (what the operation writes), program (whose --flash-cut N "
              "counts it), cut (the FATAL line: the position cut of the positions and the "
              "transitions done of all), tear (none, some or all of the transitions done; "
              "nothing: the torn unit or sector had no bit to change), "
              "before (the state before the operation, from the model), after (the state the "
              "cut left, read both ways), left (before or after: which of the two it is), boot "
              "(what the first start after the cut did), starts (every start until a confirmed "
              "image ran, with its exit status), follow_up (the next update), lock_retries, "
              "seconds (the trial's wall time), check (the check that failed; its message is in "
              "failures)",
    "sequences": "per sequence its operations: the number, the kind, address+length, the "
                 "role, the start of the board that makes it, the program and its N (of "
                 "--flash-cut N), the state before -> the state after (the model's)",
    "state": "slot A and slot B as letters (E empty, W writing, T trial, C confirmed, "
             "P previous, R rejected), the newest record's sequence number:event, aN the "
             "trial's attempts used, uN the log's entries in use",
    "start": "the slot, then t<attempt> for a trial or c for the confirmed image, and the "
             "bootloader's event if it wrote one (ROLLBACK)",
    "replay": "python bench/power_loss.py --replay <class>:<sequence>:<op>:<seed> runs a trial "
              "again, the same way, and keeps its files",
}


def assemble(args, ctx: Context, about: dict, plan: list[Trial], cells_per_class: dict,
             rows: list[dict], wall_s: float, workers: int) -> tuple[dict, dict, dict, dict]:
    rows = sorted(rows, key=lambda r: r["index"])
    reported = sorted({i for r in rows for i in r.get("build_ids", [])} |
                      {i for ref in about["references"].values() for i in ref["build_ids"]})
    repeats = ({cls: 1 for cls in CLASSES} if args.quick else
               {"stage": args.stage_repeats, "download": args.download_repeats,
                "boot": args.boot_repeats})
    config = {
        "quick": args.quick, "seed": args.seed, "workers": workers,
        "repeats_per_cell": repeats, "cells_per_class": cells_per_class,
        "trials_per_class": {cls: sum(t.cell.cls == cls for t in plan) for cls in CLASSES},
        "cut_image": {"name": CUT_IMAGE, "bytes": ctx.images[CUT_IMAGE].stat().st_size},
        "follow_up_image": {"name": FOLLOW_UP_IMAGE,
                            "bytes": ctx.images[FOLLOW_UP_IMAGE].stat().st_size},
        "factory_image": FACTORY_IMAGE, "confirm_trial_image": TRIAL_IMAGE,
        "rollback_trial_image": CRASH_IMAGE,
        "cuts": "armed on the command line (--flash-cut N:SEED of fcn_flash, fcn_boot or the "
                "node); none at run time",
        "transport": "udp, a bus of the trial's own ports; the node's default 500 kbit/s",
        "confirm_window": "the product's: 50 healthy health periods (no --confirm-periods)",
        "flash_waits": "fcn_flash --no-wait; fcn_boot and the node wait the modelled times",
        "bounds_s": {"run": RUN_WAIT_S, "confirm": CONFIRM_WAIT_S, "first_frame": START_WAIT_S,
                     "flasher": FLASHER_TIMEOUT_S},
        "sequences": {f"{cls}:{name}": {"base": s.base, "operations": len(s.ops),
                                        "cut": "the log's wrap only" if SEQUENCES[(cls, name)][2]
                                        else "every operation"}
                      for (cls, name), s in ctx.sequences.items()},
    }
    summary = powerloss.headline(rows, wall_s, workers)
    data = {
        "legend": LEGEND,
        "images": about["images"],
        "bases": about["bases"],
        "sequences": {f"{cls}:{name}": [op.compact() for op in s.ops]
                      for (cls, name), s in ctx.sequences.items()},
        "references": about["references"],
        "by_class": powerloss.summarize(rows),
        "failures": [{key: r.get(key) for key in (*powerloss.COLUMNS, "message")} |
                     {"replay": f"{r['class']}:{r['sequence']}:{r['op']}:{r['seed']}"}
                     for r in rows if r.get("check") is not None],
        "trials": {"columns": list(powerloss.COLUMNS),
                   "rows": [powerloss.row_text(r) for r in rows]},
    }
    # Every node run that wrote statistics reported the build id of the image it ran (the
    # release node, packed): one id, or write_result() names each one that is not the tree's.
    return config, summary, data, {f"node runs, build {i + 1} of {len(reported)}": build
                                   for i, build in enumerate(reported)}


def plot(result: dict, path: Path) -> None:
    """Left: per class and operation role, the share of its cuts that left the state before the
    operation, the state after it, or failed a check, with the trials counted at each bar.
    Right: the wall time of a trial per class."""
    import matplotlib.pyplot as plt
    import numpy as np

    plotstyle.apply()
    by_class = result["by_class"]
    fig, (ax_ops, ax_time) = plt.subplots(1, 2, figsize=(11.0, 7.4),
                                          gridspec_kw={"width_ratios": [2.0, 1.0]})
    fig.subplots_adjust(left=0.25, right=0.97, top=0.84, bottom=0.11, wspace=0.30)
    labels, shares = [], []
    for cls in CLASSES:
        for role, entry in by_class.get(cls, {}).get("by_role", {}).items():
            n = entry["trials"]
            passed = n - entry["failed"]
            # A cut whose check failed is counted as failed, not by what it left.
            before = min(entry["left"].get("before", 0), passed)
            after = min(entry["left"].get("after", 0), passed - before)
            labels.append((f"{CLASS_LABELS[cls]}: {role}", n))
            shares.append((before / n, after / n, entry["failed"] / n))
    y = np.arange(len(labels))[::-1]
    columns = np.array(shares).reshape(-1, 3).T * 100.0 if shares else np.zeros((3, 0))
    left = np.zeros(len(labels))
    for values, colour, label in zip(columns, (plotstyle.SERIES[0], plotstyle.SERIES[1],
                                               plotstyle.STATUS["critical"]),
                                     ("the state before the operation",
                                      "the state after it (its change complete)",
                                      "a check failed")):
        ax_ops.barh(y, values, left=left, color=colour, height=0.7, label=label)
        left = left + values
    ax_ops.set_yticks(y, [name for name, _ in labels], fontsize=7)
    for row, (_, n) in zip(y, labels):
        ax_ops.annotate(f"n = {n}", xy=(100.0, row), xytext=(3, 0), textcoords="offset points",
                        va="center", fontsize=6.5, color=plotstyle.INK_MUTED)
    ax_ops.set_xlim(0, 100)
    ax_ops.grid(axis="x")
    ax_ops.grid(axis="y", visible=False)
    ax_ops.set_xlabel("share of the role's cuts (%): what the board came back to")
    summary = result["summary"]
    ax_ops.set_title(f"What each cut left: {summary['trials']['value']} trials, "
                     f"{summary['invariant_violations']['value']} invariant violations")
    plotstyle.legend_above(ax_ops)

    trials = [powerloss.parse_row(text) for text in result["trials"]["rows"]]
    present = [(cls, [t["seconds"] for t in trials if t["class"] == cls]) for cls in CLASSES]
    present = [(cls, s) for cls, s in present if s]
    if present:
        ax_time.boxplot([s for _, s in present], orientation="horizontal", widths=0.5,
                        boxprops={"color": plotstyle.INK_SECONDARY},
                        whiskerprops={"color": plotstyle.INK_SECONDARY},
                        capprops={"color": plotstyle.INK_SECONDARY},
                        medianprops={"color": plotstyle.SERIES[1], "linewidth": 2.0},
                        flierprops={"markersize": 3, "markeredgecolor": plotstyle.INK_MUTED})
        ax_time.set_yticks(range(1, len(present) + 1),
                           [f"{cls}\n(n = {len(s)})" for cls, s in present],
                           fontsize=7.5)
    ax_time.set_xlabel("seconds per trial")
    ax_time.set_title("Wall time of a trial (host)")
    ax_time.grid(axis="x")
    ax_time.grid(axis="y", visible=False)
    workers = summary["workers"]["value"]
    plotstyle.footnote(fig, f"bench/power_loss.py, {workers} workers, wall time "
                            f"{summary['wall_time']['value']:.0f} s. A trial: the cut, the starts "
                            "until a confirmed image runs, the next update. What a cut leaves is "
                            "decided by the node and the flash, not the host's clock; the times "
                            "are the host's.")
    plotstyle.save(fig, path)


# --------------------------------------------------------------------------------- main


def _arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     epilog="See the module text (python -c 'import "
                                            "bench.power_loss' or the file) for the method.")
    results.add_bench_arguments(parser, quick="one trial of each class (a staging cut in its "
                                              "COMMIT's commit field, a download cut in a page, a "
                                              "crashing trial cut in its second attempt)")
    parser.add_argument("--workers", type=int, default=DEFAULT_WORKERS, metavar="N",
                        help=f"worker processes (default {DEFAULT_WORKERS})")
    parser.add_argument("--seed", type=int, default=CAMPAIGN_SEED,
                        help=f"the seed of the plan: the order and every cut's seed (default "
                             f"{CAMPAIGN_SEED})")
    for cls in CLASSES:
        parser.add_argument(f"--{cls}-repeats", type=int, default=REPEATS[cls], metavar="R",
                            help=f"cuts of each operation of the {cls} sequences (default "
                                 f"{REPEATS[cls]})")
    parser.add_argument("--replay", metavar="CLASS:SEQUENCE:OP:SEED",
                        help="run one trial again (a row's class, sequence, op and seed), keep "
                             "its files, print its row; writes no result")
    parser.add_argument("--keep", action="store_true",
                        help="keep the files of every trial (normally only of failed ones)")
    args = parser.parse_args(argv)
    if args.workers < 1 or min(args.stage_repeats, args.download_repeats, args.boot_repeats) < 0:
        parser.error("--workers must be at least 1 and the repeats at least 0")
    return args


def replay(args, binaries: runner.Binaries) -> int:
    try:
        cell, seed = powerloss.parse_replay(args.replay)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    if (cell.cls, cell.sequence) not in SEQUENCES:
        print(f"no sequence {cell.cls}:{cell.sequence}; there are "
              f"{', '.join(f'{c}:{n}' for c, n in SEQUENCES)}", file=sys.stderr)
        return 2
    args.keep = True
    # The trial's files are kept: under --out DIR if given, else under the build root.
    workdir = (Path(args.out) / f"{NAME}-replay" if args.out is not None
               else runner.bench_workdir(f"{NAME}-replay"))
    shutil.rmtree(workdir, ignore_errors=True)
    workdir.mkdir(parents=True)
    ctx, about = prepare(args, binaries, workdir, [(cell.cls, cell.sequence)])
    sequence = ctx.sequences[(cell.cls, cell.sequence)]
    if not 1 <= cell.op <= len(sequence.ops):
        print(f"{cell.cls}:{cell.sequence} has operations 1 to {len(sequence.ops)}",
              file=sys.stderr)
        return 2
    base = about["bases"][sequence.base]
    print(f"base {sequence.base}: {base['state']}, sha256 {base['sha256']}")
    print(f"operation {sequence.op(cell.op).compact()}")
    row = run_trial(ctx, Trial(0, cell, seed))
    for part in ("bases", "images", "references"):
        shutil.rmtree(workdir / part, ignore_errors=True)
    print(json.dumps(row))                  # one line: the trial's row, as the campaign has it
    print(f"files: {workdir / 'trials' / '00000'}")
    return 0 if row.get("check") is None else EXIT_FINDINGS


def main(argv=None) -> int:
    args = _arguments(argv)
    out = results.output_dir(args)
    json_path, png_path = out / f"{NAME}.json", out / f"{NAME}.png"
    if args.replot:
        if not json_path.exists():
            print(f"nothing to replot: {json_path} does not exist", file=sys.stderr)
            return 1
        plot(json.loads(json_path.read_text()), png_path)
        print(f"redrew {png_path}")
        return 0
    try:
        load = results.check_load(args.max_load, args.allow_loaded)
        binaries = runner.prepare_binaries(build=not args.no_build)
        # The node that runs is the release build packed into the images (the asan node does
        # not fit a slot), whatever FCN_NODE names: its build id is checked as well.
        release_node = paths.build_root() / "release" / "bin" / "fcn_node"
        checked = binaries.as_dict() | {PACKED: release_node}
        results.require_tree_build(checked)
        packed_sha256 = results.sha256_of(release_node)
        if args.replay:
            return replay(args, binaries)
        workdir = runner.bench_workdir(NAME)
        keys = [key for key in SEQUENCES
                if not args.quick or key in {(c, n) for c, n, _ in QUICK}]
        began = time.monotonic()
        print(f"preparing {len(keys)} sequences in {workdir} ...", flush=True)
        ctx, about = prepare(args, binaries, workdir, keys)
        plan, cells_per_class = plan_for(args, ctx.sequences)
        workers = min(args.workers, 3) if args.quick else args.workers
        per_class = ", ".join(f"{cls} {sum(t.cell.cls == cls for t in plan)}" for cls in CLASSES)
        print(f"{len(plan)} trials ({per_class}) in {workers} workers ...", flush=True)
        rows = campaign(ctx, plan, workers, workdir / "journal.jsonl")
        wall = time.monotonic() - began
        config, summary, data, reported = assemble(args, ctx, about, plan, cells_per_class,
                                                   rows, wall, workers)
        sha = dict(binaries.sha256) | {PACKED: packed_sha256}
        path = results.write_result(
            NAME, data, summary, config, out, load=load, binaries=checked,
            reported_build_ids=reported, binary_sha256=sha,
            timing_note="What a cut leaves and the state the board comes back in are decided "
                        "by the node and the flash, not the host's clock. The host can still "
                        "fail a trial (a stall past the node's 500 ms watchdog resets the node, "
                        "one past the 1 s ISO-TP timeout ends a download); such a trial is kept "
                        "as a failure, never repeated, and no stall makes a check pass, so a "
                        "count of 0 failures holds whatever the host did. The host's timing "
                        "can also change how often a start finds the files still locked by the "
                        "run before it (lock_retries: such a start is repeated and counted). "
                        "The times per trial and the wall time are the host's, with the "
                        "workers running in parallel.")
    except results.HostLoaded as error:
        print(error, file=sys.stderr)
        return results.EXIT_HOST_LOADED
    except results.BuildIdMismatch as error:
        print(f"no result written: {error}", file=sys.stderr)
        return results.EXIT_BUILD_MISMATCH
    except KeyboardInterrupt as error:
        print("interrupted: no result written", file=sys.stderr)
        return runner.interrupted_status(error)
    plot(json.loads(path.read_text()), png_path)
    failed = sum(1 for r in rows if r.get("check") is not None)
    print(f"{summary['trials']['value']} trials in {wall:.0f} s: {failed} failed, "
          f"{summary['invariant_violations']['value']} invariant violations")
    print(f"wrote {path} and {png_path}")
    if failed:
        print(f"FINDINGS: {failed} trials failed a check; each is in the result's \"failures\" "
              f"with its replay, and its files are kept under {workdir / 'trials'}",
              file=sys.stderr)
        return EXIT_FINDINGS
    if not args.keep:
        # The journal (a line per trial, with the worker and its times) stays; the flashes,
        # images and trial files go.
        for part in ("trials", "bases", "images", "references"):
            shutil.rmtree(workdir / part, ignore_errors=True)
        print(f"journal: {workdir / 'journal.jsonl'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
