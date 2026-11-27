# SPDX-License-Identifier: MIT
"""Phase 5: power cuts during a download over the bus. The node runs the factory image of slot
A and a tester downloads into slot B (diag_bench.download()); the supply of the flash is cut in
one of the download's flash operations, the run ends as a loss of supply (status 12, the
retained cell lost), and the node is started again through fcn_boot, as after a loss of supply.
Then decision 5's invariants of phase 4 are checked from outside (docs/update.md, the power-cut
campaign; update_bench.py):

  - the metadata decode, `fcn_flash info` and fcn_tools.flash agree (update_bench.info()), and
    slot B is in the state before the operation that was cut or the one after it;
  - exactly one slot is CONFIRMED and holds its recorded image intact (check_invariants());
  - the bootloader starts what the state machine says for the state found (expected_boot()),
    and the node's frames and statistics say so: BMS_NodeInfo's ActiveSlot and ResetCause
    POWER_ON, BMS_BootInfo's mode, attempt and event, the hand-off;
  - a trial left by a cut after the COMMIT is started, and rolled back after its three
    attempts; and the next download, in the run after the cut (or after the rollback),
    succeeds: the trial it commits is in slot B.

THE OPERATIONS OF A DOWNLOAD (lib/boot's slot writer, docs/update.md; the node makes no flash
operation of its own before them, since a factory image needs no confirm): eraseMemory writes the
BEGIN record (a program of the record, then one of its commit field) and erases every sector the
image needs; TransferData programs each page as it fills; RequestTransferExit programs the last,
partial page; checkProgrammingDependencies validates the image (reads only: no operation to cut)
and writes the COMMIT record (record, commit field). For the "data" image of 9256 bytes that is
2 + 3 erases + 37 pages + 2 = 44 operations.

HOW THE CUTS ARE ARMED, without a race: on the node's command line (--flash-cut N:SEED, the N-th
operation of the run, all of them the download's), or at run time (flash.cut N:SEED through
simulation control, the N-th operation from then) only at a point where the node makes no flash
operation until the tester's next request: after the answer to RequestDownload (the next
operations are the pages) and after the answer to RequestTransferExit (the next is the COMMIT
record, so a cut "in the check routine's validation" falls in it). The tester waits for the
reply to flash.cut before its next request, so the count starts where it is meant to, however
the host runs.

THE CAMPAIGN, 11 cuts (CUTS, one test each), each with its seed: drawn from BENCH_SEED, or, for
the one-unit programs (the commit fields), the seeds of test_update_cuts.py, which leave some or
all of the unit's transitions done; the test checks which they leave here. Ten are cuts of the
"data" image; the last is the crash payload's download cut in COMMIT's commit field after its
last transition, which leaves a complete trial that the bootloader then starts three times and
rolls back. About half a minute in a functional run. lib/boot's unit tests cut every operation
of every sequence at every position, test_update_cuts.py the staging of fcn_flash, and
bench/power_loss.py (phase 6) runs the large campaign of 1062 cuts of the real programs, every
operation of a staging, a download and a boot of a trial several times.
"""
from __future__ import annotations

import math
import random

import pytest

from diag_bench import (DATA_SIZE, SLOT_B, NodeEnded, Progress, Uds, download, start_ready,
                        wait_until)
from fcn_tools.node import EXIT_OK, RESET_IMAGE_FAULT, RESET_POWER_LOSS
from update_bench import (BENCH_SEED, MAX_ATTEMPTS, RUN_WAIT_S, Boot, check_frames_of,
                          check_handoff, check_invariants, decoded, expected_boot, factory, info,
                          listen, slot_index, states)

#: The seeds of the commit fields' cuts (test_update_cuts.py): some of the unit's transitions,
#: or all of them, done before the supply goes.
SEED_SOME, SEED_ALL = 0, 4


def operations(size: int) -> dict[str, int]:
    """The operations of the download of an image of `size` bytes, numbered from 1."""
    sectors, pages = math.ceil(size / 4096), math.ceil(size / 256)
    return {"sectors": sectors, "pages": pages, "first_erase": 3, "last_erase": 2 + sectors,
            "first_page": 3 + sectors, "last_page": 2 + sectors + pages,
            "commit_record": 3 + sectors + pages, "total": 4 + sectors + pages}


def allowed_states(operation: int, ops: dict[str, int]) -> set[str]:
    """What slot B may be after a cut in `operation`: EMPTY before, WRITING after the BEGIN
    record, TRIAL after the COMMIT; a torn record does not count, and a commit field cut after
    its last transition completes its record."""
    if operation == 1:
        return {"empty"}
    if operation == 2:
        return {"empty", "writing"}
    if operation == ops["total"]:
        return {"writing", "trial"}
    return {"writing"}


def step_of(operation: int, ops: dict[str, int], size: int) -> tuple[str, int]:
    """The request the tester was waiting for when `operation` was cut: (the last step answered,
    the blocks answered). The pages of a block are programmed by its TransferData; the partial
    last page by RequestTransferExit; the BEGIN record and the erases by eraseMemory; the COMMIT
    record by the check."""
    blocks = math.ceil(size / 1024)
    if operation <= ops["last_erase"]:
        return "programming", 0
    if operation < ops["commit_record"]:
        page = operation - ops["first_page"] + 1
        if page * 256 > size:                           # the partial last page
            return "block", blocks
        return "request_download", math.ceil(page / 4) - 1
    return "exit", blocks


def follow_the_trial(node, restart_node, name: str) -> object:
    """The runs a trial left by a cut gets: the crash payload, started by fcn_boot as attempts
    1 to 3, each a reset of its own (13); the last run is returned. The first start after the
    cut is a power-on: the cut took the retained cell with it."""
    for attempt in range(1, MAX_ATTEMPTS + 1):
        node = restart_node(node, name=f"{name}-attempt-{attempt}", args=())
        assert node.wait_for_reset(timeout=RUN_WAIT_S) == RESET_IMAGE_FAULT
        assert f"payload crash: hand-off slot B trial attempt {attempt}" in node.log()
        if attempt == 1:
            assert "reset cause POWER_ON" in node.boot_log()
    return node


#: The pages of the "data" image (DATA_SIZE bytes, tests/system/conftest.py): a page drawn from
#: BENCH_SEED among those of a block, as the seeds of the cuts that have no special one.
DATA_OPS = operations(DATA_SIZE)
_DRAW = random.Random(BENCH_SEED)
_PAGE = _DRAW.randint(2, DATA_OPS["pages"] - 1)


def _seed() -> int:
    return _DRAW.randrange(1 << 32)


#: The campaign: (how the cut is armed, the N it is armed with, the download's operation it
#: cuts, its seed, the image, what slot B must be after it), from the operations of the image.
#: The last one is the crash payload's download, whose COMMIT completes: its trial must run.
CUTS = [
    pytest.param("command line", lambda o: 1, lambda o: 1, _seed(), "data", "empty",
                 id="begin-record"),
    pytest.param("command line", lambda o: 2, lambda o: 2, SEED_SOME, "data", "empty",
                 id="begin-commit-field-some"),
    pytest.param("command line", lambda o: 2, lambda o: 2, SEED_ALL, "data", "writing",
                 id="begin-commit-field-all"),
    pytest.param("command line", lambda o: o["first_erase"], lambda o: o["first_erase"],
                 _seed(), "data", "writing", id="first-erase"),
    pytest.param("command line", lambda o: o["last_erase"], lambda o: o["last_erase"],
                 _seed(), "data", "writing", id="last-erase"),
    pytest.param("after request_download", lambda o: 1, lambda o: o["first_page"], _seed(),
                 "data", "writing", id="first-page"),
    pytest.param("after request_download", lambda o: _PAGE,
                 lambda o: o["first_page"] + _PAGE - 1, _seed(), "data", "writing",
                 id=f"page-{_PAGE}"),
    pytest.param("after request_download", lambda o: o["pages"], lambda o: o["last_page"],
                 _seed(), "data", "writing", id="last-page-at-the-exit"),
    pytest.param("after exit", lambda o: 1, lambda o: o["commit_record"], _seed(), "data",
                 "writing", id="commit-record"),
    pytest.param("command line", lambda o: o["total"], lambda o: o["total"], SEED_SOME, "data",
                 "writing", id="commit-field-some"),
    pytest.param("command line", lambda o: o["total"], lambda o: o["total"], SEED_ALL,
                 "crash-stripped", "trial", id="commit-field-all-the-trial-runs"),
]


@pytest.mark.parametrize("how, armed_at, cut_at, cut_seed, which, leaves", CUTS)
def test_a_power_cut_in_a_download_leaves_the_confirmed_image_and_the_next_download_works(
        start_node, restart_node, workdir, ports, images, how, armed_at, cut_at, cut_seed,
        which, leaves):
    spec = f"udp:{ports.bus}"
    image = images[which].read_bytes()
    data = images["data"].read_bytes()
    assert len(data) == DATA_SIZE and DATA_OPS["total"] == 44
    ops = operations(len(image))
    armed, operation = armed_at(ops), cut_at(ops)
    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    args = ("--flash-cut", f"{armed}:{cut_seed}") if how == "command line" else ()
    node = start_ready(start_node, spec, *args, name="cut", flash_file=flash,
                       retained_file=workdir / "cell.bin")

    def arm(step: str) -> None:
        if how == f"after {step}":
            reply = node.arm_flash_cut(armed, cut_seed)
            assert (reply["cut"], reply["cut_in"]) == ("armed", str(armed)), reply

    progress = Progress()
    with Uds(spec) as uds:
        try:
            download(uds, image, SLOT_B, alive=lambda: node.running, progress=progress,
                     after=arm)
        except NodeEnded:
            pass
        else:
            pytest.fail(f"the download ended although operation {operation} was cut")
    assert node.wait_for_reset(timeout=RUN_WAIT_S) == RESET_POWER_LOSS
    # The FATAL line numbers the operations since the flash was opened: the download's.
    assert f"FATAL flash: power cut in operation {operation} " in node.stderr(), node.stderr()
    assert (progress.done[-1], progress.blocks_sent) == step_of(operation, ops, len(image))

    torn = info(flash)
    check_invariants(torn)
    state = torn["slots"][1]["state"]
    assert states(torn)[0] == "confirmed"
    assert state in allowed_states(operation, ops), state
    assert state == leaves, f"the seed {cut_seed} left B {state}, not {leaves}"
    boot = expected_boot(torn)
    assert boot == (Boot("B", "trial", 1, "NONE") if state == "trial"
                    else Boot("A", "confirmed", 0, "NONE"))
    reset_cause = "POWER_ON"
    if state == "trial":
        # The cut came after COMMIT's last transition: the trial is complete, so the bootloader
        # starts it; it crashes on every attempt, and the next start rolls it back.
        node = follow_the_trial(node, restart_node, "trial")
        assert info(flash)["metadata"]["attempts_used"] == MAX_ATTEMPTS
        boot = Boot("A", "confirmed", 0, "ROLLBACK")
        reset_cause = "IMAGE_FAULT"

    # The next start, through fcn_boot: what the state machine says, and the next download
    # succeeds in the same run.
    def next_download(again, reader) -> None:
        wait_until(lambda: decoded(reader.frames, "BMS_NodeInfo")
                   and decoded(reader.frames, "BMS_BootInfo"),
                   "BMS_NodeInfo and BMS_BootInfo", running=lambda: again.running)
        with Uds(spec) as uds:
            download(uds, data, SLOT_B, alive=lambda: again.running)
        assert again.stop() == EXIT_OK

    again, frames = listen(spec, lambda: restart_node(node, name="again", args=()),
                           next_download)
    check_frames_of(boot, frames, torn["slots"][slot_index(boot.slot)]["header"]["version"],
                    reset_cause=reset_cause)
    check_handoff(again.stats(), boot)
    assert again.stats()["diag"]["uds"]["download"]["committed"] == 1
    after = info(flash)
    check_invariants(after)
    assert states(after) == ("confirmed", "trial")
    assert after["slots"][1]["header"]["version"] == "1.5.0"
    assert after["metadata"]["attempts_used"] == 0
