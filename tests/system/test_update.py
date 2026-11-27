# SPDX-License-Identifier: MIT
"""The life of an update, without power cuts (those are in test_update_cuts.py): an image staged
with the flash tool is started as a trial from slot B, confirms itself after five seconds in good
health and stays the active image; one that fails is started exactly three times and rolled
back; a slot with one bit flipped by the bench is never started; and no update can begin while a
trial is pending (docs/update.md; decisions 5, 6 and 8 of phase 4). A trial whose frames nobody
acknowledges does not confirm itself until it is acknowledged again, a bus lost inside its
window cuts the window short, and nothing a node that works does cuts it short (app/update.h,
THE CONFIRM OF A TRIAL).

The functional tests decide from what the node itself reports (its statistics, its frames, its
log, the flash afterwards), never from the host's clock or from how fast the test process is: a
bus lost from the start is the node's own --can-fault. Marked `timing` are the test of the
five-second window on the wall clock, the one that a working node's window is never cut short
(a host that holds the node up can cut it), and the one of a bus lost at run time inside the
window, which the test process has to reach in time. tests/system/update_bench.py is the bench.
"""
from __future__ import annotations

import re
import subprocess
import time

import pytest

from fcn_tools import paths
from fcn_tools.node import (BOOT_NOTHING_BOOTABLE, EXIT_OK, RESET_IMAGE_FAULT, RESET_WATCHDOG)
from fcn_tools.timing import check_timing

from update_bench import (BENCH_SEED, FLOOD_BITRATE, MAX_ATTEMPTS, ROLLBACK_BIT, RUN_WAIT_S,
                          Boot, Flood, TelemetryOff, alive_seconds, check_frames_of,
                          check_handoff, check_invariants, decoded, expected_boot, factory,
                          flip_bit, info, listen, stage, states, until_confirmed,
                          wait_for_alive, wait_for_can_state, wait_for_confirm_seconds)

#: UPDATE_CONFIRM_AFTER_S (app/update.h) in health periods of 100 ms.
WINDOW_PERIODS = 50


def confirmed_update(start_node, workdir, ports, images):
    """Factory image 1.0.0 in slot A, an update to 1.1.0 staged into slot B and started; returns
    the node once its trial has confirmed itself and stopped, and the frames it sent."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    staged = stage(flash, images["node-1.1.0"])
    assert (staged["slot"], staged["address"]) == ("B", 0x102000)
    before = info(flash)
    assert states(before) == ("confirmed", "trial")
    assert before["metadata"]["attempts_used"] == 0
    assert expected_boot(before) == Boot("B", "trial", 1, "NONE")
    return listen(f"udp:{ports.bus}", lambda: start_node(flash_file=flash, retained_file=cell),
                  until_confirmed)


def test_a_trial_confirms_itself_and_stays_the_active_image(start_node, restart_node, workdir,
                                                             ports, images):
    node, frames = confirmed_update(start_node, workdir, ports, images)
    flash = node.flash_file
    assert "select: slot B as a trial, attempt 1 of 3, image 1.1.0" in node.boot_log()
    assert "confirmed: slot B, image 1.1.0, is the active image" in node.log()
    stats = node.stats()
    check_handoff(stats, Boot("B", "trial", 1, "NONE"))
    confirm = stats["confirm"]
    assert (confirm["state"], confirm["status"]) == ("CONFIRMED", "ok")
    assert confirm["flash_task"]["confirms"] == 1
    assert confirm["flash_task"]["confirm_status"] == "ok"
    assert confirm["flash_task"]["trial_slot"] == "B"
    # The window, from the node's own counts: 50 health periods in good health in a row, the
    # last of them the period that asked for the confirm, after every period that cut an
    # earlier window short. (How long that took on the wall clock is the timing test below.)
    assert confirm["periods_needed"] == WINDOW_PERIODS
    assert confirm["healthy_periods"] == WINDOW_PERIODS
    assert confirm["requested_period"] >= WINDOW_PERIODS + confirm["window_restarts"]
    # The confirm is one record: a program of the record and one of its commit field.
    assert (stats["flash"]["programs"], stats["flash"]["erases"]) == (2, 0)

    seen = [(frame.signals["ConfirmState"], frame.raw["ConfirmSeconds"])
            for frame in decoded(frames, "BMS_BootInfo")]
    # Once a second: waiting with the healthy seconds so far, then (after one frame that may
    # still say CONFIRMING) confirmed with all 5 of them.
    first = next(i for i, (state, _) in enumerate(seen) if state != "WAITING")
    assert all(state == "WAITING" and seconds < 5 for state, seconds in seen[:first]), seen
    assert seen[first][0] in ("CONFIRMING", "CONFIRMED") and seen[first][1] == 5, seen
    assert all(entry == ("CONFIRMED", 5) for entry in seen[first + 1:]), seen
    assert seen[-1] == ("CONFIRMED", 5), seen
    check_frames_of(Boot("B", "trial", 1, "NONE"), frames, "1.1.0", reset_cause="POWER_ON")

    after = info(flash)
    check_invariants(after)
    assert states(after) == ("previous", "confirmed")
    assert (after["metadata"]["event"], after["metadata"]["seq"]) == ("confirm", 4)
    assert expected_boot(after) == Boot("B", "confirmed", 0, "NONE")

    # It stays the active image: the next start is of slot B as the confirmed image.
    again, frames = listen(f"udp:{ports.bus}",
                           lambda: restart_node(node, args=("--duration-s", "2")))
    assert "select: slot B, the confirmed image, image 1.1.0" in again.boot_log()
    check_handoff(again.stats(), Boot("B", "confirmed", 0, "NONE"))
    assert again.stats()["confirm"]["state"] == "NOT_NEEDED"
    check_frames_of(Boot("B", "confirmed", 0, "NONE"), frames, "1.1.0")
    assert info(flash)["metadata"]["seq"] == 4        # a start of the confirmed image writes nothing


#: A line of the node's log: its CLOCK_MONOTONIC stamp in seconds, and the text.
LOG_LINE = re.compile(r"\[\s*(\d+\.\d+)\] . (.*)")


def stamp_of(log: str, text: str) -> float:
    for line in log.splitlines():
        match = LOG_LINE.match(line)
        if match and text in match.group(2):
            return float(match.group(1))
    raise AssertionError(f"no log line with {text!r}:\n{log}")


#: How much later than 5 s after the start the confirm may be asked for on a busy host: two
#: health periods. A timing tolerance.
WINDOW_LATE_S = 0.2


@pytest.mark.timing
def test_the_window_of_good_health_is_five_seconds_on_the_wall_clock(start_node, workdir, ports,
                                                                    images):
    """The trial asks for its confirm 5 s after the node's start (its first log line), by the
    node's own CLOCK_MONOTONIC stamps: never earlier (the 50 health periods of the window follow
    the tick, which follows CLOCK_MONOTONIC and never runs ahead of it), and later only if the
    host held the node up, which is a timing tolerance, as is a window cut short by a stall."""
    node, _ = confirmed_update(start_node, workdir, ports, images)
    log = node.log()
    started = stamp_of(log, "fcn_node ")
    asked = stamp_of(log, "ran 5 s in good health: writing its confirm")
    window = asked - started
    assert window >= 4.9, f"the confirm was asked for {window:.3f} s after the start"
    confirm = node.stats()["confirm"]
    check_timing(confirm["window_restarts"] == 0 and confirm["requested_period"] == WINDOW_PERIODS,
                 f"the window of good health was cut short {confirm['window_restarts']} times "
                 f"(the confirm came in period {confirm['requested_period']}, not 50)")
    check_timing(window <= 5.0 + WINDOW_LATE_S,
                 f"the confirm was asked for {window:.3f} s after the start, not 5 s")


@pytest.mark.parametrize("payload, status", [("crash", RESET_IMAGE_FAULT),
                                             ("watchdog", RESET_WATCHDOG)])
def test_a_trial_that_fails_is_started_three_times_and_rolled_back_with_its_dtc(
        start_node, restart_node, workdir, ports, images, payload, status):
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    stage(flash, images[payload])

    run = start_node(flash_file=flash, retained_file=cell)
    for attempt in range(1, MAX_ATTEMPTS + 1):
        assert run.wait_for_reset(timeout=RUN_WAIT_S) == status
        assert f"select: slot B as a trial, attempt {attempt} of 3" in run.boot_log()
        assert f"payload {payload}: hand-off slot B trial attempt {attempt}" in run.log()
        assert info(flash)["metadata"]["attempts_used"] == attempt
        if attempt < MAX_ATTEMPTS:
            run = restart_node(run, name=f"after-{attempt}")
    assert expected_boot(info(flash)) == Boot("A", "confirmed", 0, "ROLLBACK")

    # The fourth start, with the bus open from before it: its first BMS_Fault is on the bus.
    run, frames = listen(f"udp:{ports.bus}",
                         lambda: restart_node(run, name="rolled-back", args=("--duration-s", "2")))
    assert "rolled back: the trial in slot B used its 3 attempts" in run.boot_log()
    assert "the bootloader rolled back before this start (ROLLBACK of slot B)" in run.log()
    stats = run.stats()
    check_handoff(stats, Boot("A", "confirmed", 0, "ROLLBACK"))
    assert stats["handoff"]["event_slot"] == "B"
    dtc = next(d for d in stats["faults"]["dtcs"] if d["name"] == "UPDATE_ROLLBACK")
    assert (dtc["code"], dtc["status"] & 0x09) == ("P1B06-00", 0x09)  # testFailed, confirmedDTC
    assert stats["faults"]["most_recent"] == "UPDATE_ROLLBACK"
    # The reset cause of this start is the last failed attempt's.
    assert stats["reset"]["cause_code"] == status

    faults = decoded(frames, "BMS_Fault")
    assert faults, "no BMS_Fault on the bus"
    assert faults[0].raw["FaultActive"] & ROLLBACK_BIT and faults[0].raw["FaultConfirmed"] & ROLLBACK_BIT
    assert faults[0].signals["FaultMostRecent"] == "UPDATE_ROLLBACK"
    check_frames_of(Boot("A", "confirmed", 0, "ROLLBACK"), frames, "1.0.0",
                    reset_cause="IMAGE_FAULT" if status == RESET_IMAGE_FAULT else "WATCHDOG")
    assert decoded(frames, "BMS_BootInfo")[0].signals["BootEventSlot"] == "B"
    rolled = info(flash)
    check_invariants(rolled)
    assert states(rolled) == ("confirmed", "rejected")

    # The hand-off speaks to that start only: the next one has no rollback to report.
    last = restart_node(run, name="later", args=("--duration-s", "1"))
    assert last.wait(timeout=RUN_WAIT_S) == EXIT_OK
    assert last.stats()["handoff"]["event"] == "NONE"
    assert last.stats()["faults"]["active"] & ROLLBACK_BIT == 0


def flash_tool_run(*args) -> subprocess.CompletedProcess:
    return subprocess.run([str(paths.flash_tool_binary()), *map(str, args)], capture_output=True,
                          text=True, timeout=60, check=False)


def test_no_update_can_begin_while_a_trial_is_pending(start_node, restart_node, workdir, ports,
                                                      images):
    """Once a run of the trial has stopped before its confirm (a run of 2 s, which the node ends
    by its own tick count), lib/boot refuses to begin an update of either slot (the confirmed one
    is the trial's fallback), and nothing of the flash changes. And while the trial runs, the
    node holds the flash file and the tool cannot open it: shown on its next run, which only the
    test ends. The node holds the file from before its first frame until its scheduler has
    stopped, so a node still running after the tool returned held it while the tool ran,
    however late the test process got to start the tool. (Whether that run confirms meanwhile
    depends on how fast the test process is, and is not checked.)"""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    stage(flash, images["node-1.1.0"])

    node, frames = listen(f"udp:{ports.bus}",
                          lambda: start_node("--duration-s", "2", flash_file=flash,
                                             retained_file=cell))
    check_frames_of(Boot("B", "trial", 1, "NONE"), frames, "1.1.0")
    assert all(frame.signals["ConfirmState"] == "WAITING"
               for frame in decoded(frames, "BMS_BootInfo"))
    pending = info(flash)
    assert (pending["metadata"]["trial_slot"], pending["metadata"]["attempts_used"]) == ("B", 1)
    before = flash.read_bytes()

    for options in ((), ("--address", "0x102000"), ("--address", "0x2000")):
        refused = stage(flash, images["node-1.4.0"], *options, expect=2)
        assert refused["status"] == "refused" and refused["detail"].startswith("state at address"), \
            refused
        assert flash.read_bytes() == before, f"stage {options} changed the flash"

    def stage_meanwhile(node, reader):
        deadline = time.monotonic() + RUN_WAIT_S
        while not decoded(reader.frames, "BMS_BootInfo"):      # the node has opened the flash
            assert node.running and time.monotonic() < deadline, node.stderr()
            time.sleep(0.01)
        busy = flash_tool_run("stage", "--flash-file", flash, "--no-wait", images["node-1.4.0"])
        assert node.running, node.stderr()          # and so held the flash while the tool ran
        assert busy.returncode == 1 and "in use by another process" in busy.stderr, busy
        assert node.stop() == EXIT_OK

    listen(f"udp:{ports.bus}", lambda: restart_node(node, name="running", args=()),
           stage_meanwhile)
    check_invariants(info(flash))


@pytest.mark.parametrize("where", ["header", "payload"])
def test_a_trial_with_one_bit_flipped_is_rejected_and_never_started(start_node, workdir, ports,
                                                                    images, where):
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    stage(flash, images["node-1.1.0"])
    at, bit = flip_bit(flash, "B", where, BENCH_SEED)
    damaged = info(flash)
    verdict = damaged["slots"][1]["validation"]
    assert verdict != "ok", f"bit {bit:#04x} at {at:#x} flipped and slot B still validates"
    assert expected_boot(damaged) == Boot("A", "confirmed", 0, "REJECT")

    node, frames = listen(f"udp:{ports.bus}",
                          lambda: start_node("--duration-s", "2", flash_file=flash,
                                             retained_file=cell))
    assert "select: rolled back: the trial in slot B does not validate" in node.boot_log()
    assert "as a trial" not in node.boot_log()
    assert "the bootloader rolled back before this start (REJECT of slot B)" in node.log()
    check_handoff(node.stats(), Boot("A", "confirmed", 0, "REJECT"))
    check_frames_of(Boot("A", "confirmed", 0, "REJECT"), frames, "1.0.0")
    faults = decoded(frames, "BMS_Fault")
    assert faults and faults[0].raw["FaultActive"] & ROLLBACK_BIT
    after = info(flash)
    assert states(after) == ("confirmed", "rejected")
    assert after["metadata"]["event"] == "reject"


def test_a_confirmed_image_with_one_bit_flipped_falls_back_to_the_one_before(
        start_node, restart_node, workdir, ports, images):
    node, _ = confirmed_update(start_node, workdir, ports, images)
    flash = node.flash_file
    assert states(info(flash)) == ("previous", "confirmed")
    flip_bit(flash, "B", "payload", BENCH_SEED)
    damaged = info(flash)
    assert damaged["slots"][1]["validation"] == "bad_digest"
    assert expected_boot(damaged) == Boot("A", "confirmed", 0, "FALLBACK")

    again, frames = listen(f"udp:{ports.bus}",
                           lambda: restart_node(node, args=("--duration-s", "2")))
    assert ("select: fell back: the confirmed image in slot B does not validate; the one before "
            "it, in slot A, is confirmed again") in again.boot_log()
    assert "the bootloader rolled back before this start (FALLBACK of slot A)" in again.log()
    check_handoff(again.stats(), Boot("A", "confirmed", 0, "FALLBACK"))
    check_frames_of(Boot("A", "confirmed", 0, "FALLBACK"), frames, "1.0.0")
    faults = decoded(frames, "BMS_Fault")
    assert faults and faults[0].raw["FaultActive"] & ROLLBACK_BIT
    after = info(flash)
    check_invariants(after)
    assert states(after) == ("confirmed", "rejected")
    # The next update goes to the slot that is not confirmed, the damaged one.
    assert stage(flash, images["node-1.4.0"])["slot"] == "B"
    assert states(info(flash)) == ("confirmed", "trial")


@pytest.mark.parametrize("where", ["header", "payload"])
def test_a_confirmed_image_with_one_bit_flipped_and_nothing_before_it_is_not_started(
        start_node, workdir, images, where):
    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    flip_bit(flash, "A", where, BENCH_SEED)
    verdict = info(flash)["slots"][0]["validation"]
    assert verdict != "ok"
    node = start_node(flash_file=flash, retained_file=workdir / "cell.bin")
    assert node.wait(timeout=RUN_WAIT_S) == BOOT_NOTHING_BOOTABLE
    assert f"nothing bootable: slot A {verdict}, slot B bad_magic" in node.boot_log()
    assert not node.log_path.exists()       # no image ran


# ---------------------------------------------------------------- the confirm and the bus

#: How long the tests keep a trial's bus lost, in the node's own seconds: longer than a whole
#: window (UPDATE_CONFIRM_AFTER_S, 5 s), with two seconds to spare for the log lines the test
#: counts them by, which the node writes out a few milliseconds late (posix_log.c).
LOST_S = 7

#: The identifiers of BMS_PackStatus and BMS_BootInfo (dbc/fcn.dbc).
PACK_STATUS_ID = 0x020
BOOT_INFO_ID = 0x611


def staged_trial(workdir, images):
    """Factory image 1.0.0 in slot A and 1.1.0 staged into slot B: the flash file and the
    retained cell of a node whose next start is the trial's first."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    stage(flash, images["node-1.1.0"])
    assert expected_boot(info(flash)) == Boot("B", "trial", 1, "NONE")
    return flash, cell


def start_without_acknowledgement(start_node, flash, cell):
    """Start the trial with every acknowledgement taken away from its frames from its start on,
    as when no other node is on the bus (--can-fault noack, simulation control's can.fault noack
    put in force by the node itself): nothing here races the node."""
    return start_node("--can-fault", "noack", flash_file=flash, retained_file=cell)


def keep_the_bus_lost(node) -> None:
    """For a trial started without acknowledgement: wait until it has run LOST_S of its own
    seconds so, and check that its controller is then error passive at a transmit error counter
    of 128 and has never been bus off (ISO 11898-1's exception, lib/can/can_fc.c)."""
    wait_for_alive(node, LOST_S)
    status = wait_for_can_state(node, "passive")
    assert (status["fault"], status["tec"], status["bus_off"]) == ("noack", 128, 0), status


def check_lost_periods(confirm: dict) -> None:
    """The node's counts of a run with the bus lost for LOST_S: more periods than a window were
    judged with the controller not error active and with no frame acknowledged, and none after
    a bus-off: the controller never went bus off."""
    needed = confirm["periods_needed"]
    assert confirm["not_active_periods"] > needed, confirm
    assert confirm["unacked_periods"] > needed, confirm
    assert confirm["bus_off_periods"] == 0, confirm


def test_a_trial_that_has_lost_the_bus_does_not_confirm_itself(start_node, workdir, ports,
                                                               images):
    """A trial whose frames nobody acknowledges is error passive at 128 for as long as that
    lasts, never bus off, and does not confirm itself. Started so (--can-fault noack) and
    stopped after LOST_S of its seconds, more than a window; decided from the node's counts (no
    period healthy, none with a frame acknowledged, so no window begun; no confirm asked for or
    made), its controller's state in its statistics, and the flash afterwards, which still holds
    the trial with one attempt used."""
    flash, cell = staged_trial(workdir, images)

    def run(node, reader):
        keep_the_bus_lost(node)
        assert node.stop() == EXIT_OK

    node, _ = listen(f"udp:{ports.bus}",
                     lambda: start_without_acknowledgement(start_node, flash, cell), run)
    assert "bus fault noack from the start (--can-fault)" in node.log()
    stats = node.stats()
    confirm = stats["confirm"]
    assert confirm["state"] == "WAITING", confirm
    assert (confirm["requested_period"], confirm["flash_task"]["confirms"]) == (0, 0), confirm
    assert (confirm["healthy_periods"], confirm["window_restarts"]) == (0, 0), confirm
    assert confirm["unacked_periods"] == confirm["periods"], confirm
    check_lost_periods(confirm)
    can_stats = stats["can"]
    assert (can_stats["state"], can_stats["tec"]) == ("passive", 128), can_stats
    assert can_stats["controller"]["tx_frames"] == 0, can_stats
    assert can_stats["controller"]["bus_off_events"] == 0, can_stats
    assert "writing its confirm" not in node.log()

    after = info(flash)
    check_invariants(after)
    assert states(after) == ("confirmed", "trial")
    assert after["metadata"]["attempts_used"] == 1


def test_a_trial_that_gets_its_bus_back_confirms_itself_after_a_whole_window(
        start_node, workdir, ports, images):
    """The same trial, acknowledged again after LOST_S of its seconds (can.fault none): it
    confirms itself after a window of 50 healthy periods, all of them after the last period with
    the bus lost. Decided from the node's counts (every period with the bus lost came before the
    period that asked for the confirm) and from the flash afterwards."""
    flash, cell = staged_trial(workdir, images)

    def run(node, reader):
        keep_the_bus_lost(node)
        with node.sim() as sim:
            sim.can_fault("none")
        until_confirmed(node, reader)

    node, frames = listen(f"udp:{ports.bus}",
                          lambda: start_without_acknowledgement(start_node, flash, cell), run)
    confirm = node.stats()["confirm"]
    needed = confirm["periods_needed"]
    assert (confirm["state"], confirm["status"]) == ("CONFIRMED", "ok"), confirm
    assert confirm["flash_task"]["confirms"] == 1, confirm
    check_lost_periods(confirm)
    assert confirm["requested_period"] >= needed + confirm["unacked_periods"], confirm
    assert confirm["requested_period"] >= needed + confirm["not_active_periods"], confirm
    assert confirm["requested_period"] >= needed + confirm["window_restarts"], confirm
    assert confirm["healthy_periods"] == needed and confirm["fewest_acked"] >= 1, confirm
    assert decoded(frames, "BMS_BootInfo")[-1].raw["ConfirmSeconds"] == 5

    after = info(flash)
    check_invariants(after)
    assert states(after) == ("previous", "confirmed")


@pytest.mark.timing
def test_a_bus_lost_in_the_window_cuts_it_short(start_node, workdir, ports, images):
    """The bus lost at run time, once the trial's window has begun (simulation control's
    can.fault noack after a BMS_BootInfo with a second of good health), cuts the window short,
    and the trial confirms itself only after a whole new window once it is acknowledged again
    (can.fault none). The fault lands in the window only if the test process sends it before
    the window is full, at least 3.1 s of the node's time after that BMS_BootInfo (at most 19 of
    its 50 periods are behind it): if no period found the controller other than error active
    while the trial waited, the confirm was asked for first, and the test missed a timing
    tolerance. Otherwise the node's counts decide: the window cut short, every period with the
    bus lost before the period that asked for the confirm."""
    flash, cell = staged_trial(workdir, images)

    def run(node, reader):
        wait_for_confirm_seconds(reader, node, 1)
        with node.sim() as sim:
            sim.can_fault("noack")
        status = wait_for_can_state(node, "passive")
        assert (status["fault"], status["tec"], status["bus_off"]) == ("noack", 128, 0), status
        # The fault was in force before the second after the last one logged ended.
        wait_for_alive(node, alive_seconds(node) + 1 + LOST_S)
        with node.sim() as sim:
            sim.can_fault("none")
        until_confirmed(node, reader)

    node, _ = listen(f"udp:{ports.bus}", lambda: start_node(flash_file=flash, retained_file=cell),
                     run)
    confirm = node.stats()["confirm"]
    needed = confirm["periods_needed"]
    assert (confirm["state"], confirm["status"]) == ("CONFIRMED", "ok"), confirm
    check_timing(confirm["not_active_periods"] > 0,
                 f"the bus was lost only after the confirm had been asked for (period "
                 f"{confirm['requested_period']}): the test process fell behind the node")
    assert confirm["window_restarts"] >= 1, confirm
    check_lost_periods(confirm)
    assert confirm["requested_period"] >= needed + confirm["window_restarts"], confirm
    assert confirm["requested_period"] >= needed + confirm["not_active_periods"], confirm
    assert confirm["healthy_periods"] == needed, confirm

    after = info(flash)
    check_invariants(after)
    assert states(after) == ("previous", "confirmed")


#: The other nodes of the conditions below, each on a bus of its own: none but the test's
#: listener, a tester that switches the telemetry off, the flood of test_can_flood.py.
OTHER_NODES = {"no tester": None, "telemetry off": TelemetryOff, "flood": Flood}


@pytest.mark.timing
@pytest.mark.parametrize("condition", list(OTHER_NODES))
def test_no_condition_of_a_working_node_cuts_the_window_short(start_node, workdir, ports, images,
                                                              condition):
    """The four conditions of good health hold in every period of a node that works (update.h,
    WHY EVERY PERIOD NEEDS AN ACKNOWLEDGED FRAME): with nobody on the bus but the test's
    listener, which sends nothing; with the telemetry switched off by a tester's TESTER_Command,
    so that the node sends the fewest frames it ever sends (BMS_Heartbeat, BMS_Fault,
    BMS_TaskStats, BMS_NodeInfo, BMS_BootInfo); and at 125 kbit/s under the flood of
    test_can_flood.py, the heaviest load the system tests make. Plain asserts: the trial
    confirms itself, and no period found the controller other than error active or after a
    bus-off. Timing tolerances: no period cut the window short (a host that holds the node up
    for longer than a period can leave a period without a supervision cycle or without an
    acknowledged frame), the tester and the flood had their effect, and with the tester the
    telemetry was off from the first BMS_BootInfo on (their threads run when the host lets
    them)."""
    flash, cell = staged_trial(workdir, images)
    args = ("--bitrate", str(FLOOD_BITRATE)) if condition == "flood" else ()
    other = {}

    def run(node, reader):
        sender = OTHER_NODES[condition]
        other["node"] = sender(f"udp:{ports.bus}") if sender is not None else None
        try:
            until_confirmed(node, reader)
        finally:
            if other["node"] is not None:
                other["node"].stop()

    node, frames = listen(f"udp:{ports.bus}",
                          lambda: start_node(*args, flash_file=flash, retained_file=cell), run)
    stats = node.stats()
    confirm = stats["confirm"]
    assert (confirm["state"], confirm["status"]) == ("CONFIRMED", "ok"), confirm
    assert confirm["not_active_periods"] == 0 and confirm["bus_off_periods"] == 0, confirm
    assert confirm["fewest_acked"] >= 1, confirm
    # The tester and the flood are threads of the test process, which run when the host lets
    # them: whether they had their effect before the confirm is a timing tolerance.
    if condition == "telemetry off":
        check_timing(stats["telemetry"]["enabled"] is False,
                     f"the tester's commands ({other['node'].sent} sent) did not switch the "
                     "telemetry off before the confirm")
    if condition == "flood":
        check_timing(other["node"].sent > 0 and stats["can"]["driver"]["rx_unhandled"] > 0,
                     f"the flood ({other['node'].sent} frames sent) did not reach the node "
                     "before the confirm")
    check_timing(confirm["window_restarts"] == 0
                 and confirm["requested_period"] == confirm["periods_needed"]
                 and confirm["unacked_periods"] == 0 and confirm["unsupervised_periods"] == 0,
                 f"{condition}: the window was cut short: {confirm}")
    if condition == "telemetry off":
        ids = [message.arbitration_id for message in frames]
        check_timing(PACK_STATUS_ID not in ids[ids.index(BOOT_INFO_ID):],
                     "a BMS_PackStatus after the first BMS_BootInfo: the tester switched the "
                     "telemetry off late")
