# SPDX-License-Identifier: MIT
"""Phase 5: downloads that go wrong, and what the node keeps. The node runs the factory image
1.0.0 of slot A, started through fcn_boot; a tester downloads into slot B, with the flasher
(python -m fcn_tools.flasher) or request by request (diag_bench.download()), and something in
the download is wrong:

  - an image that does not validate (a bit flipped in transit, an image for the other target):
    refused by the check routine with generalProgrammingFailure, the old image stays active
    (the next start runs it) and the next download succeeds;
  - an image that crashes: downloaded, committed, started by the bootloader on each of its
    BOOT_MAX_ATTEMPTS attempts, rolled back; the image of slot A reports the rollback through
    ReadDataByIdentifier 0xFD00, ReadDTCInformation (UPDATE_ROLLBACK) and BMS_Fault, and
    ClearDiagnosticInformation removes it;
  - a download while a trial runs: refused for both slots (conditionsNotCorrect, lib/boot's
    BOOT_E_STATE: the trial's fallback would be overwritten), accepted after the confirm;
  - a tester that falls silent in the middle of a download (S3server) or resets the node in the
    middle of one (ECUReset): the download is abandoned, slot B stays WRITING, which the
    bootloader never starts, the old image stays active, and the next download succeeds;
  - a tester that sends ECUReset while it still receives an earlier answer, which it holds with
    its flow control: the node resets at the ECUReset's deadline, 2 s after the request, and
    starts again through fcn_boot.

Functional tests: they decide from the node's answers, counters, frames and flash. A trial that
would confirm itself 5 s after its start, before the tester's requests, is taken out of that
race: it runs with a window longer than the test (--confirm-periods,
update_bench.TRIAL_WINDOW_PERIODS), so that only the tester's routine confirms it. When the
reset behind a held answer comes, which depends on how the host times the tester's flow
control, is checked by a test of its own, marked timing.
"""
from __future__ import annotations

import itertools
import time
from dataclasses import dataclass

import pytest

from diag_bench import (BEYOND_S3_S, NODE_TIMEOUT_S, SLOT_A, SLOT_B, RawLink, Uds, check_counts,
                        download, erase_request, flow_control, hexbytes, run_flasher, show,
                        single_frame, start_ready, steps_of, wait_until)
from fcn_tools import diag
from fcn_tools.bus import BusReader, is_status, open_bus, wait_for
from fcn_tools.node import Board, EXIT_OK, NodeProcess, RESET_IMAGE_FAULT, RESET_REQUESTED
from fcn_tools.timing import check_timing
from update_bench import (MAX_ATTEMPTS, NO_SELF_CONFIRM, ROLLBACK_BIT, RUN_WAIT_S,
                          TRIAL_WINDOW_PERIODS, Boot, check_frames_of, check_handoff,
                          check_invariants, decoded, factory, info, listen, stage, states)

#: UPDATE_ROLLBACK's DTC (app/fault.h) and BMS_Fault's bit for it.
ROLLBACK_DTC = 0x1B0600


def boot_info(uds: Uds) -> diag.BootInfo:
    return diag.decode_boot_info(uds.request("22 FD 00").response[3:])


def then(action):
    """For listen(): once the run's BMS_NodeInfo and BMS_BootInfo are out, `action()` (the next
    download), then the run stopped in order."""
    def run(again, reader) -> None:
        wait_until(lambda: decoded(reader.frames, "BMS_NodeInfo")
                   and decoded(reader.frames, "BMS_BootInfo"), "BMS_NodeInfo and BMS_BootInfo",
                   running=lambda: again.running)
        action()
        assert again.stop() == EXIT_OK
    return run


def test_an_image_that_does_not_validate_is_refused_at_the_check_and_the_old_one_stays(
        start_node, restart_node, images, workdir, ports):
    """Two images the flasher downloads whole, which the check routine refuses because lib/image
    does not validate them: one bit of the payload flipped after the header was made (its
    SHA-256 no longer matches), and an image packed for mps2-an385. Each ends with 7F 31 72 at
    the check, slot B stays WRITING, the next start runs the image of slot A, and a good image
    is downloaded after them."""
    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    spec = f"udp:{ports.bus}"
    node = start_ready(start_node, spec, flash_file=flash, retained_file=workdir / "cell.bin")
    for name in ("data-damaged", "data-mps2"):
        status, report, _ = run_flasher(spec, images[name], workdir / f"{name}.json",
                                        "--no-reset")
        assert status == 2, report
        assert report["failed_step"] == "check", report
        check = steps_of(report)["check"]
        assert (check["ok"], check["nrc"], check["nrc_name"]) == (False, 0x72,
                                                                  "GeneralProgrammingFailure")
        assert all(step["ok"] for step in report["steps"][:-1])
    assert node.stop() == EXIT_OK
    stats = node.stats()
    uds = stats["diag"]["uds"]
    assert uds["nrc"]["generalProgrammingFailure"] == 2
    assert (uds["download"]["exited"], uds["download"]["committed"],
            uds["download"]["failed"]) == (2, 0, 2)
    assert stats["diag"]["backend"]["jobs"]["check"]["last_status"] == "invalid"
    assert "flash job check: invalid" in node.log()
    torn = info(flash)
    check_invariants(torn)
    assert states(torn) == ("confirmed", "writing")

    # The old image stays: the next start runs slot A, and the next download goes through.
    reports = []

    def next_download() -> None:
        reports.append(run_flasher(spec, images["data"], workdir / "data.json", "--no-reset"))

    again, frames = listen(spec, lambda: restart_node(node, name="again", args=()),
                           then(next_download))
    check_frames_of(Boot("A", "confirmed", 0, "NONE"), frames, "1.0.0")
    check_handoff(again.stats(), Boot("A", "confirmed", 0, "NONE"))
    status, report, _ = reports[0]
    assert status == 0 and report["result"] == "committed", report
    assert states(info(flash)) == ("confirmed", "trial")


def test_a_crashing_image_is_rolled_back_after_its_attempts_and_0x19_reports_it(
        start_board, restart_node, images, workdir, ports):
    """The crash payload (it ends every start with an image fault, 13) downloaded and committed
    by the flasher, then the tester's ECUReset: the bootloader starts the trial, which crashes,
    BOOT_MAX_ATTEMPTS times, and the next start rolls it back and runs slot A. That run reports
    the rollback on the bus: DID 0xFD00 (event ROLLBACK of slot B), ReadDTCInformation
    (UPDATE_ROLLBACK active and confirmed, in the lists of its masks), BMS_Fault (its bit in
    FaultActive and FaultConfirmed). ClearDiagnosticInformation removes it from both, and it
    does not come back: a reset fault is judged once per start. Then a download succeeds."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    spec = f"udp:{ports.bus}"
    board = start_board(flash_file=flash, retained_file=cell, can=spec)
    status, report, _ = run_flasher(spec, images["crash-stripped"], workdir / "report.json",
                                    "--no-reset")
    assert status == 0 and report["result"] == "committed", report
    board.expect_resets(1)
    with Uds(spec) as uds:
        assert uds.request("11 01").response == hexbytes("51 01")
    first = board.runs[0]
    assert first.wait(timeout=RUN_WAIT_S) == RESET_REQUESTED
    run = board.wait_for_run(2, timeout=RUN_WAIT_S)
    for attempt in range(1, MAX_ATTEMPTS + 1):
        if attempt > 1:
            run = restart_node(run, name=f"attempt-{attempt}")
        assert run.wait_for_reset(timeout=RUN_WAIT_S) == RESET_IMAGE_FAULT
        assert f"payload crash: hand-off slot B trial attempt {attempt}" in run.log()
    assert info(flash)["metadata"]["attempts_used"] == MAX_ATTEMPTS

    bus = open_bus(spec)
    try:
        with BusReader(bus) as reader:
            node = restart_node(run, name="rolled-back")
            wait_until(lambda: len(decoded(reader.frames, "BMS_Fault")) >= 2,
                       "two BMS_Fault frames", running=lambda: node.running)
            with Uds(spec) as uds:
                boot = boot_info(uds)
                assert (boot.slot, boot.mode, boot.event, boot.event_slot,
                        boot.image_version) == ("A", "confirmed", 5, "B", "1.0.0"), boot
                listed = uds.request("19 02 08").response
                assert ROLLBACK_DTC.to_bytes(3, "big") in listed, show(listed)
                supported = uds.request("19 0A").response
                status_byte = supported[3 + 4 * 16 + 3]
                assert supported[3 + 4 * 16:3 + 4 * 16 + 3] == ROLLBACK_DTC.to_bytes(3, "big")
                assert status_byte & 0x09 == 0x09, hex(status_byte)     # testFailed, confirmed
                fault = decoded(reader.frames, "BMS_Fault")[-1]
                assert fault.raw["FaultActive"] & ROLLBACK_BIT
                assert fault.raw["FaultConfirmed"] & ROLLBACK_BIT
                assert uds.request("14 FF FF FF").response == hexbytes("54")
                wait_until(lambda: decoded(reader.frames, "BMS_Fault")[-1].raw[
                    "FaultClearCount"] == 1, "BMS_Fault after the clear",
                           running=lambda: node.running)
                fault = decoded(reader.frames, "BMS_Fault")[-1]
                assert not (fault.raw["FaultActive"] | fault.raw["FaultConfirmed"]) & ROLLBACK_BIT
                # (After a clear every status is 0x50, testNotCompleted: 0x09 matches none.)
                assert ROLLBACK_DTC.to_bytes(3, "big") not in uds.request("19 02 09").response
                # The next download goes into slot B, which the rollback left REJECTED.
                download(uds, images["data"].read_bytes(), SLOT_B)
            assert node.stop() == EXIT_OK
    finally:
        bus.shutdown()
    stats = node.stats()
    check_handoff(stats, Boot("A", "confirmed", 0, "ROLLBACK"))
    assert stats["faults"]["clears"] == 1
    assert states(info(flash)) == ("confirmed", "trial")


def test_a_download_is_refused_while_a_trial_runs_and_accepted_after_its_confirm(
        start_node, images, workdir, ports):
    """A trial staged with the flash tool runs, started by fcn_boot as attempt 1. An eraseMemory
    of either slot is refused with conditionsNotCorrect: slot B is the trial that runs, slot A
    its fallback. After the tester's confirm routine the trial is the confirmed image, and a
    download into slot A, which then holds the image before it, succeeds. The trial runs with a
    window longer than the test (--confirm-periods, TRIAL_WINDOW_PERIODS): it is still waiting
    for its confirm when the erases come, however late they come, and only the routine
    confirms it."""
    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    stage(flash, images["node-1.1.0"])
    spec = f"udp:{ports.bus}"
    node = start_ready(start_node, spec, *NO_SELF_CONFIRM, flash_file=flash,
                       retained_file=workdir / "cell.bin")
    data = images["data"].read_bytes()
    with Uds(spec) as uds:
        before = boot_info(uds)
        assert (before.slot, before.mode, before.attempt) == ("B", "trial", 1), before
        assert uds.request("10 03").positive and uds.request("10 02").positive
        refused = [uds.request(erase_request(address, len(data))).response
                   for address in (SLOT_B, SLOT_A)]
        assert uds.request("10 01").positive and uds.request("10 03").positive
        assert boot_info(uds).confirm_state == "WAITING"
        confirmed = uds.request("31 01 F0 00").response
        assert refused == [hexbytes("7F 31 22")] * 2, [show(answer) for answer in refused]
        assert confirmed == hexbytes("71 01 F0 00 00 00"), show(confirmed)
        after = boot_info(uds)
        assert (after.slot, after.confirm_state) == ("B", "CONFIRMED"), after
        download(uds, data, SLOT_A)
    assert node.stop() == EXIT_OK
    stats = node.stats()
    check_counts(stats, uds)
    assert stats["confirm"]["periods_needed"] == TRIAL_WINDOW_PERIODS
    assert stats["confirm"]["flash_task"]["confirmed_by_routine"] is True
    assert stats["diag"]["backend"]["jobs"]["erase"]["posted"] == 3
    flash_info = info(flash)
    check_invariants(flash_info)
    assert states(flash_info) == ("trial", "confirmed")
    assert flash_info["slots"][0]["header"]["version"] == "1.5.0"


def test_s3server_in_the_middle_of_a_download_abandons_it_and_the_next_one_works(
        start_node, restart_node, images, workdir, ports):
    """A tester that falls silent for longer than S3server after the second block: the node
    goes back to the default session and abandons the download (the next block is a service
    outside the session, 7F 36 7F), slot B stays WRITING, the next start runs slot A, and the
    next download succeeds. Functional: the tester's silence begins when the answer to the block
    arrives, after the node started S3server for it."""
    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    spec = f"udp:{ports.bus}"
    data = images["data"].read_bytes()
    node = start_ready(start_node, spec, flash_file=flash, retained_file=workdir / "cell.bin")
    with Uds(spec) as uds:
        progress = download(uds, data, SLOT_B, stop_after_blocks=2)
        assert progress.blocks_sent == 2
        time.sleep(BEYOND_S3_S)
        assert uds.request(bytes([0x36, 0x03]) + data[2048:3072]).response == hexbytes(
            "7F 36 7F")
        assert uds.request("22 F1 86").response == hexbytes("62 F1 86 01")
        boot = boot_info(uds)
        assert (boot.slot, boot.mode) == ("A", "confirmed")
    assert node.stop() == EXIT_OK
    stats = node.stats()
    check_counts(stats, uds)
    download_stats = stats["diag"]["uds"]["download"]
    assert stats["diag"]["uds"]["s3_timeouts"] == 1
    assert (download_stats["state"], download_stats["aborted"], download_stats["received"]) == (
        "idle", 1, 2048)
    torn = info(flash)
    check_invariants(torn)
    assert states(torn) == ("confirmed", "writing")

    def next_download() -> None:
        with Uds(spec) as tester:
            download(tester, data, SLOT_B)

    again, frames = listen(spec, lambda: restart_node(node, name="again", args=()),
                           then(next_download))
    check_frames_of(Boot("A", "confirmed", 0, "NONE"), frames, "1.0.0")
    assert states(info(flash)) == ("confirmed", "trial")


def test_an_ecu_reset_in_the_middle_of_a_download_leaves_the_old_image_and_the_next_one_works(
        start_board, images, workdir, ports):
    """ECUReset with the suppressPosRspMsgIndicationBit (11 81) after the third block: no answer,
    the reset at once; the bootloader starts slot A again (slot B is WRITING), the run after the
    reset says so through DID 0xFD00, and the next download succeeds in it."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    spec = f"udp:{ports.bus}"
    data = images["data"].read_bytes()
    board = start_board(flash_file=flash, retained_file=cell, can=spec)
    first = board.runs[0]

    def first_status() -> None:
        """Wait for a status frame of the run that runs now: on a bus opened after its start,
        so that no frame of an earlier run can answer for it."""
        reader = open_bus(spec)
        try:
            assert wait_for(reader, is_status, timeout=10.0) is not None
        finally:
            reader.shutdown()

    first_status()
    with Uds(spec) as uds:
        download(uds, data, SLOT_B, stop_after_blocks=3)
        board.expect_resets(1)
        uds.send("11 81")
        assert first.wait(timeout=RUN_WAIT_S) == RESET_REQUESTED
        second = board.wait_for_run(2, timeout=RUN_WAIT_S)
        first_status()
        # Nothing answered the reset: the next message of the node answers the next request.
        assert uds.receive(timeout=0.0) is None
        boot = boot_info(uds)
        assert (boot.slot, boot.mode, boot.attempt, boot.event) == ("A", "confirmed", 0, 0)
        download(uds, data, SLOT_B)
    assert "ECUReset: resetting (requested by the tester)" in first.log()
    assert board.stop() == EXIT_OK
    stats = second.stats()
    assert stats["reset"]["cause"] == "REQUESTED"
    assert (stats["handoff"]["slot"], stats["handoff"]["mode"]) == ("A", "confirmed")
    assert stats["diag"]["uds"]["download"]["committed"] == 1
    flash_info = info(flash)
    check_invariants(flash_info)
    assert states(flash_info) == ("confirmed", "trial")


# -------------------------------------------------------- an ECUReset behind a held answer


#: The tester's flow control while it holds the node's answer to 19 0A in the two tests below:
#: two WAITs, then a CTS for one consecutive frame (BS 1, STmin 0), over and over, one every
#: HOLD_STEP_S after the one before. Each comes within the node's N_Bs (NODE_TIMEOUT_S) of the one
#: before, with the difference as the host's margin, and never more than two WAITs in a row, fewer
#: than the node accepts (app/diag.h, DIAG_TX_WFT_MAX): the node keeps sending the answer for as
#: long as the tester holds it.
HOLD_PATTERN = (flow_control(status=1), flow_control(status=1), flow_control(block_size=1))
HOLD_STEP_S = 0.4

#: How long the tester holds the answer at most: a bound only. With the node's deadline the run
#: ends about RESET_DEADLINE_S after the request; without it, only once the whole answer is sent.
HOLD_LIMIT_S = 30.0

#: The node's deadline for an accepted ECUReset (app/diag.h, DIAG_RESET_DEADLINE_US, 2 x N_As),
#: and the host's margin on it in the timing test: the deadline starts when the diagnostic task
#: takes the request, after the tester sent it, and the run's end is seen when the test notices it.
RESET_DEADLINE_S = 2 * NODE_TIMEOUT_S
RESET_MARGIN_S = 0.5


@dataclass
class HeldReset:
    """What reset_behind_a_held_answer() saw."""
    first: NodeProcess    # the run that got 11 01
    request_at: float     # when the tester sent 11 01 (time.monotonic())
    ended_at: float       # when the test saw that run end
    frames: list[bytes]   # the node's frames after the first frame of its answer to 19 0A
    answer_cfs: int       # the consecutive frames of that whole answer, by its first frame
    ctses: int            # the CTSes the tester sent


def wait_for_status_frame(spec: str) -> None:
    """Wait for a status frame of the run that runs now, on a bus opened after its start."""
    reader = open_bus(spec)
    try:
        assert wait_for(reader, is_status, timeout=10.0) is not None
    finally:
        reader.shutdown()


def reset_behind_a_held_answer(board: Board, spec: str) -> HeldReset:
    """The tester of the two tests below: 19 0A, whose answer needs a first frame and several
    consecutive frames; at its first frame, 11 01 at once, without waiting for the rest; then
    that answer held with HOLD_PATTERN until the run ends (HOLD_LIMIT_S, a bound). Checks what
    holds however the host ran: the run ends with the requested reset, after its log line; the
    node's frames after the first frame are consecutive frames of the answer in their order,
    no more than the CTSes asked for, and then at most the 51 01, as the last frame."""
    first = board.runs[0]
    wait_for_status_frame(spec)
    with RawLink(spec) as link:
        link.send(single_frame(hexbytes("19 0A")))
        ff = link.next()
        assert ff[0] >> 4 == 1, show(ff)
        board.expect_resets(1)
        link.send(single_frame(hexbytes("11 01")))
        request_at = time.monotonic()
        sent = ctses = 0
        due = request_at + HOLD_STEP_S
        while first.running:
            now = time.monotonic()
            assert now - request_at < HOLD_LIMIT_S, f"no reset {HOLD_LIMIT_S} s after 11 01"
            if now >= due:
                flow = HOLD_PATTERN[sent % len(HOLD_PATTERN)]
                link.send(flow)
                sent += 1
                ctses += flow[0] == 0x30
                due = time.monotonic() + HOLD_STEP_S
            time.sleep(0.002)
        ended_at = time.monotonic()
        frames = link.quiet()
    assert first.wait(timeout=RUN_WAIT_S) == RESET_REQUESTED
    assert "ECUReset: resetting (requested by the tester)" in first.log()
    answer_cfs = -(-((((ff[0] & 0x0F) << 8) | ff[1]) - 6) // 7)
    assert 2 < answer_cfs < 16, show(ff)
    cfs = list(itertools.takewhile(lambda frame: frame[0] >> 4 == 2, frames))
    shown = [show(frame) for frame in frames]
    assert [frame[0] & 0x0F for frame in cfs] == list(range(1, len(cfs) + 1)), shown
    assert len(cfs) <= min(ctses, answer_cfs), (ctses, shown)
    assert frames[len(cfs):] in ([], [hexbytes("02 51 01 CC CC CC CC CC")]), shown
    print(f"the run ended {ended_at - request_at:.3f} s after 11 01, after {sent} flow controls "
          f"({ctses} CTS); the node's frames after the first: {shown}")
    return HeldReset(first, request_at, ended_at, frames, answer_cfs, ctses)


def test_an_ecu_reset_behind_an_answer_the_tester_holds_resets_and_the_node_starts_again(
        start_board, images, workdir, ports):
    """A tester that sends ECUReset (11 01) while it still receives the answer to its request
    before (19 0A), and holds that answer with its flow control, WAITs and blocks of one
    consecutive frame, as long as it likes. The node resets at the ECUReset's deadline (app/diag.h,
    ECURESET) and starts again through fcn_boot, with the image of slot A, after a requested
    reset. Functional: the order of the node's frames, the reset and the next start, which hold
    however the host ran (a host that holds up a flow control beyond N_Bs ends the answer early,
    and the 51 01 then goes out before the reset); that the reset came at its deadline, before
    the end of the answer, is the timing test's after this one."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    spec = f"udp:{ports.bus}"
    board = start_board(flash_file=flash, retained_file=cell, can=spec)
    reset_behind_a_held_answer(board, spec)
    second = board.wait_for_run(2, timeout=RUN_WAIT_S)
    wait_for_status_frame(spec)
    with Uds(spec) as uds:
        boot = boot_info(uds)
        assert (boot.slot, boot.mode, boot.attempt, boot.event) == ("A", "confirmed", 0, 0), boot
    assert board.stop() == EXIT_OK
    stats = second.stats()
    assert stats["reset"]["cause"] == "REQUESTED"
    assert (stats["handoff"]["slot"], stats["handoff"]["mode"]) == ("A", "confirmed")


@pytest.mark.timing
def test_an_ecu_reset_behind_an_answer_the_tester_holds_comes_at_its_deadline(
        start_board, images, workdir, ports):
    """The tester of the test before, timed: the run ends with the requested reset within
    RESET_DEADLINE_S (2 x N_As) and RESET_MARGIN_S of the request, with the held answer cut and
    no 51 01, however much longer the tester would have held the answer. Timing: the host's
    clock times the tester's flow control and the reset."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    spec = f"udp:{ports.bus}"
    board = start_board(flash_file=flash, retained_file=cell, can=spec)
    held = reset_behind_a_held_answer(board, spec)
    board.wait_for_run(2, timeout=RUN_WAIT_S)
    assert board.stop() == EXIT_OK
    took = held.ended_at - held.request_at
    check_timing(took <= RESET_DEADLINE_S + RESET_MARGIN_S,
                 f"the reset {took:.3f} s after 11 01, not within {RESET_DEADLINE_S} s + "
                 f"{RESET_MARGIN_S} s")
    shown = [show(frame) for frame in held.frames]
    check_timing(len(held.frames) < held.answer_cfs
                 and hexbytes("02 51 01 CC CC CC CC CC") not in held.frames,
                 f"the answer was not held until the reset: {shown}")
