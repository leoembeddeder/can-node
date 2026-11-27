# SPDX-License-Identifier: MIT
"""Phase 5: the update over the bus, end to end. A tester, python -m fcn_tools.flasher on
udsoncan and can-isotp (fcn_tools.diag), makes the update that `fcn_flash stage` makes, with the
node running: the programming sequence, the ECUReset, the trial started by fcn_boot, the confirm
routine. The bench is a Board (fcn_tools.node, tests/conftest.py's start_board): after the
requested reset the node is started again through fcn_boot with the same flash and retained cell.

These tests show that the integration of lib/isotp and lib/uds into the node works, and that it
fails when it is wrong: a download over SocketCAN and one over the UDP bus from the factory image
to the confirmed new image, every step of it read back from the tester's report, the node's
statistics, its log and the flash, and a restart that keeps the new image; a trial that no tester
confirms, which confirms itself, after which the routine finds nothing to confirm; a download
the slot writer refuses, which leaves the flash as it was; a flash job that never returns, which
ends in a watchdog reset, never in a hang; the DTC services; and no frame lost under a flood.
The other system tests of phase 5 (docs/diagnostics.md, How it is tested): the transport frame by
frame (test_diag_isotp.py), every service and NRC (test_diag_services.py), downloads broken on
purpose (test_download_faults.py) and power cuts during a download (test_download_cuts.py).

Functional tests, but for the two marked `timing`: they decide from the tester's report and the
node's answers, statistics, log and flash, and where the node's own clock could race the tester,
the race is removed or decided by the node. The download whose trial the tester's routine
confirms starts the trial with a window longer than any run here (--confirm-periods
TRIAL_WINDOW_PERIODS), so that only the routine can confirm it; the trial that confirms itself
does so with the product's window and is followed through DID 0xFD00 until the node says it is
confirmed; the DTC test expects the one status that changes in the node's first seconds
(CAN_BUS_OFF's, at 5 s of its uptime) as the node's own reads before and after found it. Marked
`timing`: the block time, and the flood, whose download needs the host to read the node's bus
in time (LOW_FLOOD_LOAD): the node counts a frame its bus model dropped (wire overruns), and the
run is then a missed timing tolerance. The times in the tester's report are printed for the
record.
"""
from __future__ import annotations

import can
import pytest

from diag_bench import Uds, hexbytes, run_flasher, show, steps_of, wait_until
from fcn_tools import diag as diag_module
from fcn_tools.node import EXIT_OK, RESET_REQUESTED, RESET_WATCHDOG
from update_bench import (NO_SELF_CONFIRM, TRIAL_WINDOW_PERIODS, Boot, check_frames_of,
                          check_handoff, check_invariants, factory, info, listen, stage, states,
                          wait_for_alive)

#: How long a test waits for the run the bench started after the reset, and for a reset.
RUN_WAIT_S = 30.0


def check_a_restart_keeps_it(restart_node, run, can_spec: str, version: str) -> None:
    """A run after the one that confirmed the trial (a power-on of the board, through fcn_boot):
    slot B starts as the confirmed image, as its frames and its hand-off say."""
    again, frames = listen(can_spec, lambda: restart_node(run, name="restart",
                                                          args=("--duration-s", "2")))
    check_frames_of(Boot("B", "confirmed", 0, "NONE"), frames, version)
    check_handoff(again.stats(), Boot("B", "confirmed", 0, "NONE"))


@pytest.mark.slow
def test_a_download_confirms_the_new_image(can_spec, start_board, restart_node, images, workdir):
    """Factory image 1.0.0 in slot A; the flasher downloads 1.1.0 into slot B, resets the node,
    finds the trial and confirms it with the routine. Checked from the tester's report, the two
    runs' logs and statistics, and the flash read both ways; and a restart afterwards keeps the
    new image. The trial's window is TRIAL_WINDOW_PERIODS: only the routine confirms it."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    image_size = images["node-1.1.0"].stat().st_size
    board = start_board(*NO_SELF_CONFIRM, flash_file=flash, retained_file=cell, can=can_spec)
    board.expect_resets(1)

    status, report, _ = run_flasher(can_spec, images["node-1.1.0"], workdir / "report.json")

    assert status == 0, report
    assert report["schema"] == "fcn.flash_report/1"
    assert report["result"] == "confirmed" and report["failed_step"] is None
    assert report["confirmed_by"] == "routine"
    steps = steps_of(report)
    assert list(steps) == ["boot_info", "default_session", "extended_session",
                           "programming_session", "erase",
                           "request_download", "transfer", "transfer_exit", "check", "ecu_reset",
                           "wait_for_trial", "read_version", "extended_session_trial", "confirm",
                           "boot_info_confirmed"]
    assert all(step["ok"] for step in report["steps"]), report["steps"]
    assert report["slot"] == "B" and report["address"] == 0x102000
    assert report["boot_info_before"]["slot"] == "A"
    assert report["boot_info_before"]["mode"] == "confirmed"
    assert report["boot_info_before"]["image_version"] == "1.0.0"
    # maxNumberOfBlockLength 1026: 1024 bytes of data per block (lib/uds).
    assert report["max_block_length"] == 1026 and report["block_data"] == 1024
    assert report["blocks"] == -(-image_size // 1024) == len(report["blocks_s"])
    assert report["erase_status_record"] == [0, 0] and report["check_status_record"] == [0, 0]
    trial = report["boot_info_trial"]
    assert (trial["slot"], trial["mode"], trial["attempt"], trial["confirm_state"],
            trial["image_version"]) == ("B", "trial", 1, "WAITING", "1.1.0")
    assert report["version_read"] == "1.1.0"
    after = report["boot_info_after"]
    assert (after["slot"], after["confirm_state"]) == ("B", "CONFIRMED")
    print({name: step["duration_s"] for name, step in steps.items()})
    print(report["transfer"])

    # The run that took the download was reset by the tester, and the bench started the trial.
    first = board.runs[0]
    second = board.wait_for_run(2, timeout=RUN_WAIT_S)
    assert first.wait(timeout=RUN_WAIT_S) == RESET_REQUESTED
    assert "ECUReset: resetting (requested by the tester)" in first.log()
    assert "download: slot at 0x102000 erased for" in first.log()
    assert "the image validated and is committed as the trial" in first.log()
    assert board.stop() == EXIT_OK
    assert len(board.runs) == 2

    # The trial's run: started by fcn_boot as attempt 1 after a requested reset, confirmed by the
    # tester's routine, with every frame of the connection accounted for.
    stats = second.stats()
    handoff = stats["handoff"]
    assert (handoff["slot"], handoff["mode"], handoff["attempt"], handoff["image_version"]) == (
        "B", "trial", 1, "1.1.0")
    assert stats["reset"]["cause"] == "REQUESTED"
    assert stats["confirm"]["periods_needed"] == TRIAL_WINDOW_PERIODS
    assert stats["confirm"]["state"] == "CONFIRMED"
    assert stats["confirm"]["flash_task"]["confirmed_by_routine"] is True
    assert stats["confirm"]["flash_task"]["jobs"]["confirm"]["run"] == 1
    assert stats["confirm"]["flash_task"]["jobs"]["confirm"]["last_status"] == "ok"
    diag = stats["diag"]
    assert diag["task"]["rx_lost"] == 0 and diag["task"]["tx_done_lost"] == 0
    connection = diag["connection"]
    assert connection["responses"] == connection["responses_sent"] >= 5
    assert connection["confirms"] == connection["confirms_passed"] == connection["frames_out"]
    assert connection["frames_refused"] == 0 and connection["responses_dropped"] == 0
    # Every frame of the connection went through can_tx's event FIFO, and the frames queued while
    # no other event frame was held woke can_tx (can_io_send_event_stamped()): not all were quiet.
    driver = stats["can"]["driver"]
    assert driver["tx_done_reports"] == connection["confirms"]
    assert driver["tx_quiet_enqueues"] < connection["frames_out"]
    isotp = diag["isotp"]
    assert all(count == 0 for result, count in isotp["rx_results"].items() if result != "OK")
    assert all(count == 0 for result, count in isotp["tx_results"].items() if result != "OK")
    assert isotp["unconfirmed"] == 0 and isotp["outstanding"] == 0
    uds = diag["uds"]
    assert uds["requests"]["RoutineControl"] == 1 and uds["positive"] >= 5
    assert sum(uds["nrc"].values()) == 0

    # The flash: B confirmed with the new image, A the one before it, both intact.
    flash_info = info(flash)
    check_invariants(flash_info)
    assert states(flash_info) == ("previous", "confirmed")
    assert flash_info["slots"][1]["header"]["version"] == "1.1.0"
    check_a_restart_keeps_it(restart_node, second, can_spec, "1.1.0")


@pytest.mark.slow
def test_a_trial_no_tester_confirms_confirms_itself_and_the_routine_then_finds_none(
        start_board, restart_node, images, workdir, ports):
    """The flasher downloads 1.1.0, resets the node and reads the trial's version, but does not
    confirm it (--no-confirm). The trial confirms itself after its 5 s of good health (update.h)
    while the tester only reads DID 0xFD00; the confirm routine afterwards is refused
    (conditionsNotCorrect: there is no trial to confirm; the first confirm wins, flash_job.h) and
    writes nothing; a restart keeps the new image."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    spec = f"udp:{ports.bus}"
    board = start_board(flash_file=flash, retained_file=cell, can=spec)
    board.expect_resets(1)
    status, report, _ = run_flasher(spec, images["node-1.1.0"], workdir / "report.json",
                                    "--no-confirm")
    assert status == 0 and report["result"] == "trial", report
    assert report["version_read"] == "1.1.0"
    second = board.wait_for_run(2, timeout=RUN_WAIT_S)
    assert board.runs[0].wait(timeout=RUN_WAIT_S) == RESET_REQUESTED
    with Uds(spec) as uds:
        def confirmed() -> bool:
            info_now = diag_module.decode_boot_info(uds.request("22 FD 00").response[3:])
            assert (info_now.slot, info_now.mode) == ("B", "trial"), info_now
            return info_now.confirm_state == "CONFIRMED"

        wait_until(confirmed, "the trial's confirm in DID 0xFD00", running=lambda: second.running)
        assert uds.request("10 03").positive
        refused = uds.request("31 01 F0 00").response
        assert refused == hexbytes("7F 31 22"), show(refused)
    assert board.stop() == EXIT_OK
    stats = second.stats()
    assert stats["confirm"]["state"] == "CONFIRMED"
    assert stats["confirm"]["flash_task"]["confirmed_by_routine"] is False
    assert stats["confirm"]["flash_task"]["jobs"]["confirm"]["last_status"] == "state"
    assert "trial in slot B ran 5 s in good health: writing its confirm" in second.log()
    flash_info = info(flash)
    check_invariants(flash_info)
    assert states(flash_info) == ("previous", "confirmed")
    # One CONFIRM record, the self-confirm's: ADOPT, BEGIN, COMMIT, CONFIRM.
    assert (flash_info["metadata"]["event"], flash_info["metadata"]["records_valid"]) == (
        "confirm", 4)
    check_a_restart_keeps_it(restart_node, second, spec, "1.1.0")


@pytest.mark.slow
def test_the_slot_writer_refuses_the_running_slot_and_a_pending_trial(start_node, images, workdir,
                                                                       ports):
    """A trial is pending in slot B (staged with fcn_flash) and the node runs without the
    bootloader, so the trial stays pending: an eraseMemory of B is refused (a trial pending, its
    fallback would be overwritten) and one of A (the confirmed image) as well, both with
    conditionsNotCorrect (lib/boot's BOOT_E_STATE), and the flash is not changed."""
    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    stage(flash, images["node-1.1.0"])
    before = flash.read_bytes()
    node = start_node("--flash-file", str(flash))
    can = f"udp:{ports.bus}"

    for address in ("0x102000", "0x2000"):
        status, report, _ = run_flasher(can, images["node-1.4.0"], workdir / f"report-{address}.json",
                                        "--address", address)
        assert status == 2, report
        assert report["result"] == "failed" and report["failed_step"] == "erase"
        erase = steps_of(report)["erase"]
        # The name is udsoncan's.
        assert (erase["ok"], erase["nrc"], erase["nrc_name"]) == (False, 0x22,
                                                                  "ConditionsNotCorrect")
    assert node.stop() == EXIT_OK
    assert flash.read_bytes() == before
    stats = node.stats()
    assert stats["diag"]["uds"]["nrc"]["conditionsNotCorrect"] == 2
    assert stats["diag"]["backend"]["jobs"]["erase"]["posted"] == 2
    assert stats["diag"]["backend"]["jobs"]["erase"]["failed"] == 2
    assert stats["diag"]["backend"]["jobs"]["erase"]["last_status"] == "state"
    assert stats["diag"]["uds"]["download"]["state"] == "idle"


@pytest.mark.slow
def test_a_flash_job_that_never_returns_ends_in_a_watchdog_reset(start_board, restart_node, images,
                                                                 workdir, ports):
    """The first program of the node's run hangs (--flash-hang 1): the BEGIN record of the
    erase. The diagnostic task keeps answering responsePending, the flash task gives no more alive
    indications, the watchdog manager stops the kicks and the hardware watchdog resets the node
    (11). The tester's erase ends in a timeout. The next start, through fcn_boot, starts the
    confirmed image of slot A again, with the reset cause WATCHDOG."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    can = f"udp:{ports.bus}"
    board = start_board("--flash-hang", "1", flash_file=flash, retained_file=cell, can=can)

    status, report, _ = run_flasher(can, images["node-1.1.0"], workdir / "report.json")

    assert status == 2, report
    assert report["failed_step"] == "erase"
    assert steps_of(report)["erase"]["error"].startswith("timeout")
    run = board.current
    assert run.wait_for_reset(timeout=RUN_WAIT_S) == RESET_WATCHDOG
    assert "flash: operation 1 hangs: its busy flag never clears" in run.stderr()
    assert "supervision: flash expired" in run.log()
    assert len(board.runs) == 1

    again = restart_node(run, args=())
    wait_for_alive(again, 1)    # started by fcn_boot, not stopped while the bootloader runs
    assert again.stop() == EXIT_OK
    stats = again.stats()
    assert stats["reset"]["cause"] == "WATCHDOG"
    assert (stats["handoff"]["slot"], stats["handoff"]["mode"]) == ("A", "confirmed")
    flash_info = info(flash)
    check_invariants(flash_info)
    assert states(flash_info)[0] == "confirmed"


#: The status of every DTC from the node's second second on, in a run without a fault
#: (tests/system/test_faults.py): the 4 reset faults, UPDATE_ROLLBACK, the supervision, error
#: passive and the faults of the samples have passed (0x00). CAN_BUS_OFF passes only after 50
#: health cycles without a bus-off, at 5 s of the node's uptime: before, its testNotCompleted
#: bits are set (0x50), and from then on it is 0x00 as well.
AFTER_2_S = 0x00
BUS_OFF_BEFORE_5_S = 0x50


@pytest.mark.slow
def test_the_dtc_services_read_the_health_task_s_snapshot_and_clear(start_node, ports):
    """ReadDTCInformation 0x0A, 0x02 and 0x01 answer the DTCs as the health task left them in its
    last period (the snapshot it publishes), in the order and with the numbers of the fault table;
    ClearDiagnosticInformation is made by the health task. (That it is answered only once the
    clear is made is shown where a DTC is confirmed before it: test_diag_services.py.) The reads
    come after 2 s of the node's uptime, as its log counts it, and CAN_BUS_OFF changes at 5 s:
    reportSupportedDTC is read before and after the list and the count by mask, and CAN_BUS_OFF,
    the list and the count are expected as those two reads found it, never from how long the
    test process took."""
    from fcn_tools import diag
    from update_bench import wait_for_alive

    node = start_node()
    wait_for_alive(node, 2)
    with diag.connect(f"udp:{ports.bus}") as client:
        supported = client.get_supported_dtc().service_data.dtcs
        matching = client.get_dtc_by_status_mask(0xFF).service_data.dtcs
        count = client.get_number_of_dtc_by_status_mask(0xFF).service_data.dtc_count
        again = client.get_supported_dtc().service_data.dtcs
        client.change_session(3)
        client.clear_dtc(0xFFFFFF)
        client.change_session(1)
    assert node.stop() == EXIT_OK
    stats = node.stats()
    table = stats["faults"]["dtcs"]
    assert [dtc.id for dtc in supported] == [entry["dtc"] for entry in table]
    assert [dtc.id for dtc in again] == [entry["dtc"] for entry in table]
    before = {dtc.id: dtc.status.get_byte_as_int() for dtc in supported}
    after = {dtc.id: dtc.status.get_byte_as_int() for dtc in again}
    bus_off = next(entry["dtc"] for entry in table if entry["name"] == "CAN_BUS_OFF")
    for entry in table:
        if entry["dtc"] != bus_off:
            assert before[entry["dtc"]] == after[entry["dtc"]] == AFTER_2_S, entry
    # Before its 5 s in both reads, after them in both, or passed in between; never back.
    passed = (before[bus_off], after[bus_off])
    assert passed in ((BUS_OFF_BEFORE_5_S, BUS_OFF_BEFORE_5_S), (BUS_OFF_BEFORE_5_S, 0x00),
                      (0x00, 0x00)), passed
    print(f"CAN_BUS_OFF before and after the reads by mask: {passed}")
    listed = [dtc.id for dtc in matching]
    if before == after:
        assert listed == [dtc for dtc, status in before.items() if status != 0]
        assert count == len(listed)
    else:
        # It passed between the two reads: the list may have found it either way, and the
        # count, read after the list, finds it only if the list did.
        assert (listed, count) in (([bus_off], 1), ([bus_off], 0), ([], 0)), (listed, count)
    assert stats["faults"]["clears"] == 1
    assert "DTCs cleared: ClearDiagnosticInformation (clear 1)" in node.log()
    uds = stats["diag"]["uds"]
    assert uds["requests"]["ReadDTCInformation"] == 4
    assert uds["requests"]["ClearDiagnosticInformation"] == 1
    assert stats["diag"]["backend"]["clears"] == stats["diag"]["backend"]["clears_done"] == 1
    assert stats["diag"]["backend"]["snapshots"] == 4
    assert sum(uds["nrc"].values()) == 0


@pytest.mark.slow
@pytest.mark.timing
def test_a_block_takes_the_ticks_of_the_diagnostic_task_s_budget(start_node, images, workdir,
                                                                ports):
    """A TransferData of 1024 bytes is 147 frames to the node and the reports of its 10 flow
    controls, at most 2 events in a tick (APP_DIAG_EVENTS_PER_TICK, app_tasks.h): the median block,
    as the tester times it, takes no less than 75 ms, and, with every frame and report waking the
    diagnostic task at once, no more than three times that. Timing: the host's clock decides."""
    from fcn_tools.timing import check_timing

    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    start_node("--flash-file", str(flash))
    status, report, _ = run_flasher(f"udp:{ports.bus}", images["node-1.1.0"],
                                    workdir / "report.json", "--address", "0x102000",
                                    "--no-reset")
    assert status == 0 and report["result"] == "committed", report
    blocks = sorted(report["blocks_s"][:-1])     # the last block is shorter
    median = blocks[len(blocks) // 2]
    print(f"blocks of 1024 bytes: median {median * 1000:.1f} ms, fewest {blocks[0] * 1000:.1f} ms, "
          f"most {blocks[-1] * 1000:.1f} ms")
    check_timing(0.075 <= median <= 0.225, f"median block {median * 1000:.1f} ms, not 75 to 225 ms")


#: A flood of another node's frames below the diagnostic frames in priority (0x7F0 > 0x7E8), at
#: 0.4 times what the node's bus of 500 kbit/s can carry of them. Not more: the bus model keeps the
#: frames of the other nodes in the order the transport delivers them, not in their priority
#: (docs/can-simulation.md, lib/can/can_ctrl.h), so above the bus's capacity it would drop the
#: tester's frames with the flood's (wire_overruns), where a real bus would let them win the
#: arbitration. With the tester's own frames (about 1.7 per ms of the 3.7 the bus carries) the bus
#: is close to full, and the receive task takes the flood's frames as well. Close to full, the
#: model's queue of the other nodes' frames (64, the wire queue) has little room to spare: the
#: host transport stamps every frame of one read pass of the node with the same time, so a host
#: that delays the node's reads of the bus bunches the flood and a block of the tester into one
#: pass. With a stall planted in the node's CAN thread every 2 s of a download
#: (docs/diagnostics.md), stalls of 25 ms left every frame and stalls of 30 ms dropped some. The
#: flood itself never bursts (update_bench.Flood gives up what a delay of its thread cost it),
#: but the node's reads are the host's: the test is marked `timing`.
LOW_FLOOD_FRAME = can.Message(arbitration_id=0x7F0, data=bytes([0x55] * 8), is_extended_id=False)
LOW_FLOOD_BITRATE = 500_000
LOW_FLOOD_LOAD = 0.4


@pytest.mark.slow
@pytest.mark.timing
def test_no_frame_is_lost_in_the_node_under_a_flood_of_lower_priority_frames(start_node, images,
                                                                             workdir, ports):
    """The whole download (to the committed trial, without the reset) while another node floods
    the bus with 8-byte frames of lower priority than UDS_Request and UDS_Response (LOW_FLOOD_LOAD
    says how much and why not more), and the node sends its full telemetry: every frame of the
    tester reaches lib/isotp (no frame lost in the controller's receive FIFO, in the diagnostic
    task's queue, nor in lib/isotp: no reception ended by a wrong sequence number or a timeout),
    every one of the node's frames is confirmed, and the flood's frames were received and dropped
    by the receive task. Timing: the bus model has to keep every frame of the tester, which it
    does only while the host reads the node's bus in time (LOW_FLOOD_LOAD); a frame it dropped is
    counted (can.controller.wire_overruns), and the run is then a missed timing tolerance. With
    none dropped, the counts decide."""
    from fcn_tools.timing import check_timing
    from update_bench import Flood

    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    node = start_node("--flash-file", str(flash))
    flood = Flood(f"udp:{ports.bus}", frame=LOW_FLOOD_FRAME, load=LOW_FLOOD_LOAD,
                  bitrate=LOW_FLOOD_BITRATE)
    try:
        status, report, _ = run_flasher(f"udp:{ports.bus}", images["node-1.1.0"],
                                        workdir / "report.json", "--address", "0x102000",
                                        "--no-reset")
    finally:
        flood.stop()
    assert node.stop() == EXIT_OK
    stats = node.stats()
    controller = stats["can"]["controller"]
    pacing = stats["can"]["pacing"]
    print(f"flood: {flood.sent} frames sent, {flood.given_up} given up; transfer "
          f"{report.get('transfer')}; wire overruns {controller['wire_overruns']}, late resyncs "
          f"{controller['late_resyncs']}, the CAN thread's latest wake-up "
          f"{pacing['wakeup_lag_max_ns'] / 1e6:.1f} ms late")
    check_timing(controller["wire_overruns"] == 0,
                 f"the bus model dropped {controller['wire_overruns']} frames (wire overruns; "
                 f"{controller['late_resyncs']} late resyncs, a wake-up of the node's CAN thread "
                 f"{pacing['wakeup_lag_max_ns'] / 1e6:.1f} ms late): the host delayed the node's "
                 f"reads of the bus; the flasher: {report.get('result')}, failed step "
                 f"{report.get('failed_step')}")
    assert status == 0 and report["result"] == "committed", report
    diag = stats["diag"]
    driver = stats["can"]["driver"]
    assert flood.sent > 0 and driver["rx_unhandled"] > 0
    assert driver["rx_fifo_overruns"] == 0
    assert diag["task"]["rx_lost"] == 0 and diag["task"]["tx_done_lost"] == 0
    assert diag["task"]["rx_high_water"] <= 16      # one block of BS 16 at most
    isotp = diag["isotp"]
    assert isotp["rx_results"]["OK"] == diag["connection"]["requests"]
    assert all(count == 0 for result, count in isotp["rx_results"].items() if result != "OK")
    assert all(count == 0 for result, count in isotp["tx_results"].items() if result != "OK")
    assert diag["connection"]["confirms"] == diag["connection"]["confirms_passed"]
    assert diag["uds"]["download"]["committed"] == 1
    assert diag["uds"]["download"]["received"] == images["node-1.1.0"].stat().st_size
    assert states(info(flash)) == ("confirmed", "trial")
