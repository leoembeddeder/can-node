# SPDX-License-Identifier: MIT
"""Power cuts in an update, on the system: the supply of the flash cut (posix_flash.c tears the
operation as lib/flash's model says and ends the run with status 12, the retained cell lost)
while the flash tool stages an update, while the bootloader counts a trial's attempt or writes
its rollback, and while the node writes its confirm (simulation command flash.cut at run time,
and --flash-cut on the node's command line once). After every cut the bench starts the board
again through fcn_boot, as after a loss of supply, and checks decision 5's invariants from
outside (docs/update.md, the power-cut campaign):

  - the metadata decode, both readings agree (update_bench.info()), and the state is the one
    before the operation that was cut or the one after it;
  - exactly one slot is CONFIRMED and holds its recorded image intact (check_invariants());
  - the bootloader starts what the state machine says for the state found (expected_boot()),
    and the node's frames and statistics say so: BMS_NodeInfo's ActiveSlot, BMS_BootInfo's
    mode, attempt and event, the hand-off, the reset cause POWER_ON;
  - a trial is started at most three times in all, counting the starts before the cut, and is
    then rolled back; a cut in an attempt counts against the trial, never for it;
  - the next update can be staged (and, once, runs to its confirm).

The campaign is small on purpose, 24 cuts (13 in staging, 3 in attempts, 4 in the rollback,
4 in the confirm) that take about a minute in a functional run and half a minute in the timing
run (the 3 cuts armed at run time, marked `timing`, see the confirm's test): lib/boot's unit
tests cut every operation of every sequence at every position (tests/unit/test_boot_cuts.c),
tests/python/test_flash_decoder.py every operation of a staged update with three seeds, and
bench/power_loss.py runs the large campaign (phase 6: 1062 cuts of the real programs, every
operation of each class several times). A cut of a one-unit program (a commit field, an attempt)
can leave none, some or all of its transitions done, as the seed says; the seeds of those cuts
are chosen so that each outcome occurs, and the tests check that it does (with lib/flash's model
on this geometry, measured on the units these tests cut: seed 7 leaves none, seeds 0, 1, 2, 8
and 9 some, seeds 3 to 6 all).
"""
from __future__ import annotations

import math
import random
import time

import pytest

from fcn_tools.node import EXIT_OK, RESET_IMAGE_FAULT, RESET_POWER_LOSS
from fcn_tools.simctl import SimError
from fcn_tools.timing import check_timing

from update_bench import (BENCH_SEED, MAX_ATTEMPTS, ROLLBACK_BIT, RUN_WAIT_S, Boot,
                          check_frames_of, check_handoff, check_invariants, decoded,
                          expected_boot, factory, flash_tool, info, listen, slot_index, stage,
                          states, until_confirmed)

#: The seeds of the cuts of one-unit programs, by what lib/flash's model leaves of the unit.
SEED_NONE, SEED_SOME, SEED_ALL = 7, 0, 4

#: How long a run of the node after a cut lasts: two seconds, so that BMS_NodeInfo (once a
#: second) and BMS_BootInfo (in health periods 2, 12, ...) are on the bus.
RUN_S = "2"


def version_in(flash_info: dict, slot: str) -> str:
    """The version in the header of the image in `slot` ("1.0.0")."""
    return flash_info["slots"][slot_index(slot)]["header"]["version"]


def start_and_check(flash_info: dict, start, ports) -> tuple[object, Boot]:
    """Start the board after a cut (`start()`, a run of RUN_S seconds through fcn_boot) on the
    flash `flash_info` describes, and check that it started what the state machine says, with
    the reset cause of a loss of supply or of a first start: POWER_ON."""
    boot = expected_boot(flash_info)
    node, frames = listen(f"udp:{ports.bus}", start)
    check_frames_of(boot, frames, version_in(flash_info, boot.slot), reset_cause="POWER_ON")
    check_handoff(node.stats(), boot)
    return node, boot


# ------------------------------------------------------------------------------ the staging


def stage_cuts(image_size: int, counted: dict) -> list[tuple[int, int, str]]:
    """The cuts of the staging campaign, (operation, seed, what): the operations of a stage are,
    in their order (lib/boot's slot writer, docs/update.md), the BEGIN record (its record, then
    its commit field), one erase per sector the image needs, one program per page of the image,
    and the COMMIT record (record, commit field). `counted` are the counts of an uncut stage,
    which must be those. Cut: both programs of each record (the commit fields with a seed that
    leaves all of the unit done and one that leaves some), the first, the last and a drawn
    erase, the first, the last and two drawn pages; the drawn ones from BENCH_SEED, with seeds
    drawn as well."""
    sectors, pages = math.ceil(image_size / 4096), math.ceil(image_size / 256)
    assert (counted["erases"], counted["programs"]) == (sectors, pages + 4), counted
    total = sectors + pages + 4
    first_erase, last_erase = 3, 2 + sectors
    first_page, last_page = last_erase + 1, total - 2
    draw = random.Random(BENCH_SEED)
    seed = lambda: draw.randrange(1 << 32)               # noqa: E731
    return [(1, seed(), "BEGIN's record"),
            (2, SEED_SOME, "BEGIN's commit field, some of it"),
            (2, SEED_ALL, "BEGIN's commit field, all of it"),
            (first_erase, seed(), "the first erase"),
            (draw.randint(first_erase + 1, last_erase - 1), seed(), "an erase"),
            (last_erase, seed(), "the last erase"),
            (first_page, seed(), "the first page"),
            (draw.randint(first_page + 1, last_page - 1), seed(), "a page"),
            (draw.randint(first_page + 1, last_page - 1), seed(), "a page"),
            (last_page, seed(), "the last page"),
            (total - 1, seed(), "COMMIT's record"),
            (total, SEED_SOME, "COMMIT's commit field, some of it"),
            (total, SEED_ALL, "COMMIT's commit field, all of it")]


def test_a_cut_anywhere_in_staging_leaves_the_confirmed_image_and_the_next_update_works(
        start_node, restart_node, workdir, ports, images):
    image = images["node-1.1.0"]
    base = workdir / "base.bin"
    factory(base, images["node-1.0.0"])
    reference = workdir / "reference.bin"
    reference.write_bytes(base.read_bytes())
    done = stage(reference, image)
    cuts = stage_cuts(done["image_size"], done["flash"])
    total = done["flash"]["programs"] + done["flash"]["erases"]
    assert len({operation for operation, _, _ in cuts}) == len(cuts) - 2   # two seeds twice
    outcomes = {}
    for operation, seed, what in cuts:
        name = f"cut-{operation}-{seed}"
        flash = workdir / f"{name}.bin"
        flash.write_bytes(base.read_bytes())
        flash_tool("stage", "--flash-file", flash, "--no-wait", "--flash-cut",
                   f"{operation}:{seed}", image, expect=RESET_POWER_LOSS)
        torn = info(flash)
        check_invariants(torn)
        state = torn["slots"][1]["state"]
        # Before the cut slot B was EMPTY; the BEGIN record makes it WRITING, the COMMIT record
        # TRIAL. A torn record does not count; a commit field cut after its last transition
        # completes its record.
        allowed = ({"empty"} if operation == 1 else {"empty", "writing"} if operation == 2
                   else {"writing", "trial"} if operation == total else {"writing"})
        assert state in allowed, f"{what} (operation {operation}, seed {seed}) left B {state}"
        assert states(torn)[0] == "confirmed"
        outcomes[(operation, seed)] = state

        node, boot = start_and_check(
            torn, lambda: start_node("--duration-s", RUN_S, name=name, flash_file=flash,
                                     retained_file=workdir / f"{name}.cell"), ports)
        assert boot == (Boot("B", "trial", 1, "NONE") if state == "trial"
                        else Boot("A", "confirmed", 0, "NONE")), what
        after = info(flash)
        check_invariants(after)
        if state == "trial":
            # A committed trial: no update may begin until it has confirmed or been rolled back.
            assert after["metadata"]["attempts_used"] == 1
            stage(flash, images["node-1.4.0"], expect=2)
            continue
        # The next update is written over whatever the cut left in slot B.
        again = stage(flash, images["node-1.4.0"])
        assert again["slot"] == "B" and again["erase_steps"] == math.ceil(
            again["image_size"] / 4096)
        staged = info(flash)
        check_invariants(staged)
        assert states(staged) == ("confirmed", "trial") and staged["metadata"]["attempts_used"] == 0
    # The seeds of the commit fields were chosen so that both outcomes occur.
    assert (outcomes[(2, SEED_SOME)], outcomes[(2, SEED_ALL)]) == ("empty", "writing"), outcomes
    assert (outcomes[(total, SEED_SOME)], outcomes[(total, SEED_ALL)]) == ("writing", "trial"), \
        outcomes

    # Once, all the way: the update staged after the cut in the middle of the erases is started
    # as a trial and confirms itself.
    operation, seed, _ = cuts[4]
    flash = workdir / f"cut-{operation}-{seed}.bin"
    node, frames = listen(f"udp:{ports.bus}",
                          lambda: start_node(name="after-cut", flash_file=flash,
                                             retained_file=workdir / "after-cut.cell"),
                          until_confirmed)
    check_frames_of(Boot("B", "trial", 1, "NONE"), frames, "1.4.0")
    assert states(info(flash)) == ("previous", "confirmed")


# ------------------------------------------------------------------- the bootloader's writes


def test_a_cut_in_an_attempt_counts_against_the_trial_and_never_gives_it_a_fourth_start(
        start_node, restart_node, workdir, ports, images):
    """A trial that crashes at once (fcn_payload_crash), and starts of the bootloader that are
    cut in its first program, the attempt: one that leaves the attempt field erased (the attempt
    is not counted, and the trial was not started), one that leaves some of its bits (counted,
    although the trial never ran), one that leaves all of them (counted). In all, the trial runs
    only as often as attempts are left, and the fourth start rolls it back."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    stage(flash, images["crash"])
    steps = [("cut", SEED_NONE, 0), ("run", None, 1), ("cut", SEED_SOME, 1), ("cut", SEED_ALL, 1)]
    runs = 0
    node = None
    for step, seed, counted in steps:
        before = info(flash)
        used = before["metadata"]["attempts_used"]
        boot = expected_boot(before)
        assert boot.mode == "trial"
        name = f"{step}-{used}"
        if step == "cut":
            boot_args = ("--flash-cut", f"1:{seed}")
            node = (start_node(name=name, flash_file=flash, retained_file=cell, boot_args=boot_args)
                    if node is None else restart_node(node, name=name, boot_args=boot_args))
            assert node.wait_for_reset(timeout=RUN_WAIT_S) == RESET_POWER_LOSS
            assert "FATAL flash: power cut in operation 1 (program of 8 bytes" in node.stderr()
            assert not node.log_path.exists()          # the bootloader never jumped
        else:
            node = restart_node(node, name=name)
            assert node.wait_for_reset(timeout=RUN_WAIT_S) == RESET_IMAGE_FAULT
            assert f"payload crash: hand-off slot B trial attempt {used + 1}" in node.log()
            # The cut took the retained cell with it: this start was a power-on.
            assert "reset cause POWER_ON" in node.boot_log()
            runs += 1
        after = info(flash)
        check_invariants(after)
        assert after["metadata"]["attempts_used"] == used + counted, (step, seed)
    assert info(flash)["metadata"]["attempts_used"] == MAX_ATTEMPTS
    assert runs == 1

    last, frames = listen(f"udp:{ports.bus}",
                          lambda: restart_node(node, name="rolled-back",
                                               args=("--duration-s", RUN_S)))
    check_handoff(last.stats(), Boot("A", "confirmed", 0, "ROLLBACK"))
    check_frames_of(Boot("A", "confirmed", 0, "ROLLBACK"), frames, "1.0.0")
    assert states(info(flash)) == ("confirmed", "rejected")


#: The cuts of the rollback's record: (operation, seed, whether the record is complete after it).
ROLLBACK_CUTS = [(1, BENCH_SEED, False), (2, SEED_NONE, False), (2, SEED_SOME, False),
                 (2, SEED_ALL, True)]


def test_a_cut_in_the_rollback_never_starts_the_trial_again(start_node, restart_node, workdir,
                                                             ports, images):
    """A trial that used its three attempts (fcn_payload_crash), and the start that rolls it
    back cut in the ROLLBACK record's program or in its commit field. Whatever the cut leaves,
    the next start does not start the trial: either the record is torn, and that start writes
    the rollback again and reports it (UPDATE_ROLLBACK), or the cut came after the commit
    field's last transition, and the rollback is complete, but its report is lost with the start
    that made it: the hand-off is written after the record, and the next start finds nothing
    to report (docs/faults.md, a stated limitation)."""
    used = workdir / "three.bin"
    factory(used, images["node-1.0.0"])
    stage(used, images["crash"])
    run = start_node(name="attempt-1", flash_file=used, retained_file=workdir / "three.cell")
    for attempt in range(1, MAX_ATTEMPTS + 1):
        assert run.wait_for_reset(timeout=RUN_WAIT_S) == RESET_IMAGE_FAULT
        if attempt < MAX_ATTEMPTS:
            run = restart_node(run, name=f"attempt-{attempt + 1}")
    assert info(used)["metadata"]["attempts_used"] == MAX_ATTEMPTS

    for operation, seed, complete in ROLLBACK_CUTS:
        name = f"rollback-{operation}-{seed}"
        flash, cell = workdir / f"{name}.bin", workdir / f"{name}.cell"
        flash.write_bytes(used.read_bytes())
        cut = start_node(name=name, flash_file=flash, retained_file=cell,
                         boot_args=("--flash-cut", f"{operation}:{seed}"))
        assert cut.wait_for_reset(timeout=RUN_WAIT_S) == RESET_POWER_LOSS
        # The cut came in select's record write, before select could say what it decided.
        assert "slot B trial 1.2.0, trial attempts used 3 of 3" in cut.boot_log()
        assert f"FATAL flash: power cut in operation {operation}" in cut.stderr()
        assert not cut.log_path.exists()
        torn = info(flash)
        check_invariants(torn)
        if complete:
            assert (torn["metadata"]["event"], states(torn)) == ("rollback", ("confirmed", "rejected"))
            expected = Boot("A", "confirmed", 0, "NONE")
        else:
            assert (torn["metadata"]["event"], states(torn)) == ("commit", ("confirmed", "trial"))
            assert torn["metadata"]["attempts_used"] == MAX_ATTEMPTS
            expected = Boot("A", "confirmed", 0, "ROLLBACK")
        node, boot = start_and_check(
            torn, lambda: restart_node(cut, name=f"{name}-again", args=("--duration-s", RUN_S)),
            ports)
        assert boot == expected
        stats = node.stats()
        assert bool(stats["faults"]["active"] & ROLLBACK_BIT) == (not complete), stats["faults"]
        assert states(info(flash)) == ("confirmed", "rejected")


# --------------------------------------------------------------------------- the node's confirm


#: How long a run whose confirm is cut at run time lasts at most: longer than the window of good
#: health (5 s) and the confirm after it, so that a cut armed too late ends the run in order
#: instead of never.
CONFIRM_CUT_RUN_S = "8"


def cut_came_too_late(node, flash, operation: int) -> None:
    """For a cut armed at run time that did not cut the program it was armed for: the run must
    have ended in one of the three ways a test process or a host that fell behind the node
    leaves, and then the test missed a timing tolerance; any other end fails it.
      - The command was served after the confirm's programs had begun and cut none of them, and
        the run confirmed and ended in order (status 0) after CONFIRM_CUT_RUN_S (as does a run
        that ended before the command could be served at all).
      - The cut was armed for the record's program, was served after that program had begun,
        and fell in the commit field instead (status 12, the FATAL line of the run's operation 2:
        it numbers the programs and erases since the flash was opened).
      - The run ended in order before its confirm, the trial still waiting with its window cut
        short and nothing written: with nothing wrong on the bus, only a host that held the node
        up for longer than a health period cuts the window short (update.h, WHY EVERY PERIOD
        NEEDS AN ACKNOWLEDGED FRAME), and a window that began again after 3 s does not end
        within CONFIRM_CUT_RUN_S."""
    status, stderr = node.wait(timeout=1), node.stderr()
    after = info(flash)
    check_invariants(after)
    confirmed = "confirmed: slot B, image 1.1.0, is the active image" in node.log()
    if status == EXIT_OK and not confirmed:
        confirm = node.stats()["confirm"]
        assert "FATAL" not in stderr, stderr
        assert confirm["state"] == "WAITING" and confirm["window_restarts"] >= 1, confirm
        assert node.stats()["flash"]["programs"] == 0
        assert states(after) == ("confirmed", "trial")
        missed = (f"the run ended before its confirm, with its window cut short "
                  f"{confirm['window_restarts']} times: the host held the node up")
    elif status == EXIT_OK:
        assert "FATAL" not in stderr, stderr
        assert node.stats()["flash"]["programs"] == 2
        assert states(after) == ("previous", "confirmed")
        late = ("was served after the confirm's programs had begun, and cut none of them"
                if node.cut_armed else
                "was never armed: the run had confirmed and ended before the command was answered")
        missed = (f"the cut for operation {operation} of the confirm {late}: the test process "
                  "fell behind the node")
    else:
        assert (status, operation) == (RESET_POWER_LOSS, 1), f"status {status}\n{stderr}"
        assert "FATAL flash: power cut in operation 2 (program of 8 bytes" in stderr, stderr
        missed = ("the cut for operation 1 of the confirm was served after the program of the "
                  "confirm's record had begun, and cut its commit field: the test process fell "
                  "behind the node")
    check_timing(False, missed)


@pytest.mark.parametrize("how, operation, seed, leaves", [
    ("command line", 1, SEED_NONE, "commit"),
    pytest.param("run time", 1, BENCH_SEED, "commit", marks=pytest.mark.timing),
    pytest.param("run time", 2, SEED_SOME, "commit", marks=pytest.mark.timing),
    pytest.param("run time", 2, SEED_ALL, "confirm", marks=pytest.mark.timing)])
def test_a_cut_in_the_confirm_leaves_the_trial_or_the_confirmed_image(
        start_node, restart_node, workdir, ports, images, how, operation, seed, leaves):
    """The node's first flash programs after its start are the CONFIRM record's: the record, then
    its commit field. Cut in either, the run ends as a loss of supply (12). A torn record or a
    torn commit field does not count, the trial keeps its state with one attempt used, and its
    second start confirms it; a commit field cut after its last transition is the complete
    confirm, and the next start is of the confirmed image.

    On the command line the cut is armed before the run starts. At run time (flash.cut) the test
    arms it once the node's first BMS_BootInfo is out, and the cut lands where it is meant to
    only if the command is served before the confirm's first program begins, 5 s after the
    start: only if the test process keeps up with the node. So these cases are marked `timing`,
    and their run is bounded (--duration-s CONFIRM_CUT_RUN_S), so that a cut armed too late ends
    the run in order instead of never. Whether the cut came in time is decided from how the run
    ended, as the node reports it: a loss of supply with the FATAL line of the operation of the
    run the cut was armed for, or one of the ends of cut_came_too_late(), a missed timing
    tolerance. The flash's counts when the command is served could not decide it: a program
    that has begun but not yet been counted then is missing from them, and the cut does not
    count it either."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    stage(flash, images["node-1.1.0"])
    args = (("--flash-cut", f"{operation}:{seed}") if how == "command line"
            else ("--duration-s", CONFIRM_CUT_RUN_S))
    length = 96 if operation == 1 else 8
    cut_line = f"FATAL flash: power cut in operation {operation} (program of {length} bytes"

    def cut_in_the_confirm(node, reader):
        if how == "command line":
            assert node.wait_for_reset(timeout=RUN_WAIT_S) == RESET_POWER_LOSS
            return
        # Once BMS_BootInfo is out, simulation control is up; a run that has already ended can
        # no longer be cut.
        deadline = time.monotonic() + RUN_WAIT_S
        while node.running and not decoded(reader.frames, "BMS_BootInfo"):
            assert time.monotonic() < deadline, node.stderr()
            time.sleep(0.01)
        try:
            reply = node.arm_flash_cut(operation, seed) if node.running else None
        except SimError:
            if node.running:
                raise
            reply = None                       # the run ended before the command was served
        if reply is not None:
            assert (reply["cut"], reply["cut_in"]) == ("armed", str(operation)), reply
        # A cut ends the run as a reset; one armed too late lets it end in order.
        if node.wait(timeout=RUN_WAIT_S) != EXIT_OK:
            assert node.wait_for_reset(timeout=RUN_WAIT_S) == RESET_POWER_LOSS

    node, frames = listen(f"udp:{ports.bus}",
                          lambda: start_node(*args, flash_file=flash, retained_file=cell),
                          cut_in_the_confirm)
    if how == "run time" and not (node.wait(timeout=1) == RESET_POWER_LOSS
                                  and cut_line in node.stderr()):
        cut_came_too_late(node, flash, operation)
    assert cut_line in node.stderr()
    assert "trial in slot B ran 5 s in good health: writing its confirm" in node.log()
    assert all(frame.signals["BootMode"] == "TRIAL" for frame in decoded(frames, "BMS_BootInfo"))
    torn = info(flash)
    check_invariants(torn)
    assert torn["metadata"]["event"] == leaves
    if leaves == "commit":
        assert states(torn) == ("confirmed", "trial") and torn["metadata"]["attempts_used"] == 1
        assert torn["metadata"]["records_valid"] == 3       # ADOPT, BEGIN, COMMIT
        boot = Boot("B", "trial", 2, "NONE")
    else:
        assert states(torn) == ("previous", "confirmed")
        boot = Boot("B", "confirmed", 0, "NONE")
    assert expected_boot(torn) == boot

    if boot.mode == "trial":
        again, frames = listen(f"udp:{ports.bus}", lambda: restart_node(node, args=()),
                               until_confirmed)
        assert again.stats()["confirm"]["state"] == "CONFIRMED"
    else:
        again, frames = listen(f"udp:{ports.bus}",
                               lambda: restart_node(node, args=("--duration-s", RUN_S)))
    # The loss of supply took the retained cell with it.
    assert "reset cause POWER_ON (retained cell: magic" in again.boot_log()
    check_frames_of(boot, frames, "1.1.0", reset_cause="POWER_ON")
    check_handoff(again.stats(), boot)
    after = info(flash)
    check_invariants(after)
    assert states(after) == ("previous", "confirmed")
    assert (after["metadata"]["event"], after["metadata"]["records_valid"]) == ("confirm", 4)
    assert again.wait(timeout=1) == EXIT_OK
