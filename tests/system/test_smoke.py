"""The node as a process: it starts, keeps time, reports its statistics and stops cleanly."""
from __future__ import annotations

import ctypes
import os
import signal
import subprocess
import sys
import time

import pytest

from fcn_tools import paths, results
from fcn_tools.bus import ID_STATUS, open_bus, status_queue_counts, wait_for_id
from fcn_tools.node import EXIT_OK
from fcn_tools.simctl import SimError
from fcn_tools.timing import check_timing


_AT_MINSIGSTKSZ = 51     # <linux/auxvec.h>


def _min_signal_stack_bytes() -> int:
    """The smallest signal frame the kernel builds on this CPU, as it reports it (0 on a
    kernel that does not); then MINSIGSTKSZ, 2048, the traditional value."""
    libc = ctypes.CDLL(None)
    libc.getauxval.restype = ctypes.c_ulong
    libc.getauxval.argtypes = [ctypes.c_ulong]
    return int(libc.getauxval(_AT_MINSIGSTKSZ)) or 2048


def test_version_names_the_node_and_the_kernel(node_binary):
    out = subprocess.run([str(node_binary), "--version"], capture_output=True, text=True,
                         timeout=10)
    assert out.returncode == 0
    assert out.stdout.startswith("fcn_node ")
    assert "FreeRTOS V11.3.3" in out.stdout
    assert results.BUILD_ID.match(out.stdout.split()[-1]), out.stdout


def test_the_node_the_bootloader_and_the_flash_tool_report_one_build_id(node_binary):
    """platform/posix/build_id.h: the three programs of one build carry the same id, and the
    node's log names it at the start."""
    ids = {program.name: results.binary_build_id(program)
           for program in (node_binary, paths.boot_binary(), paths.flash_tool_binary())}
    assert len(set(ids.values())) == 1, ids
    for program in (paths.boot_binary(), paths.flash_tool_binary()):
        out = subprocess.run([str(program), "--version"], capture_output=True, text=True,
                             timeout=10)
        assert out.returncode == 0 and out.stdout.startswith(f"{program.name} "), out.stdout


def test_the_programs_carry_the_build_id_of_the_tree(node_binary):
    """What a bench checks before it writes a result (fcn_tools.results.require_tree_build()):
    the binaries under test were built from this tree. scripts/test-system.sh builds before it
    tests; a tree edited after the build fails here (build again)."""
    expected = results.tree_build_id(results.provenance())
    assert results.binary_build_id(node_binary) == expected


def test_help_lists_every_option(node_binary):
    out = subprocess.run([str(node_binary), "--help"], capture_output=True, text=True, timeout=10)
    assert out.returncode == 0
    for option in ("--can", "--bitrate", "--sim-port", "--sensor-seed", "--duration-s",
                   "--stats-json", "--tick-mode", "--irq-mode", "--log-file", "--log-level",
                   "--trace-file", "--trace-events", "--version"):
        assert option in out.stdout


@pytest.mark.parametrize("args, message", [
    (["--no-such-option"], "unrecognized option"),
    (["--stat-json", "x.json"], "unrecognized option"),      # a typo must not be ignored
    (["--duration-s", "1", "stray"], "unexpected argument: stray"),
    (["--duration-s", "2x"], "invalid --duration-s"),
    (["--duration-s", "86401"], "invalid --duration-s"),
    (["--duration-s", "-1"], "invalid --duration-s"),
    (["--tick-mode", "nonsense"], "invalid --tick-mode"),
    (["--irq-mode", "nonsense"], "invalid --irq-mode"),
    (["--log-level", "loud"], "invalid --log-level"),
    (["--sensor-seed", "4294967296"], "invalid --sensor-seed"),
    (["--sensor-seed", "-1"], "invalid --sensor-seed"),
    (["--sensor-seed", "0x10"], "invalid --sensor-seed"),
    (["--confirm-periods", "0"], "invalid --confirm-periods"),
    (["--confirm-periods", "4294967296"], "invalid --confirm-periods"),
])
def test_a_bad_command_line_is_a_usage_error(node_binary, args, message):
    out = subprocess.run([str(node_binary), *args], capture_output=True, text=True, timeout=10)
    assert out.returncode == 1
    assert message in out.stderr


def test_runs_for_the_requested_time_and_reports_statistics(start_node):
    node = start_node("--duration-s", "2")
    assert node.wait(timeout=20) == EXIT_OK

    stats = node.stats()
    assert stats["schema"] == "fcn.node_stats/1"
    assert stats["kernel"] == "V11.3.3"
    assert stats["tick"]["thread_seen"] is True
    # The node stops itself after 2 s of tick time, in the health period that ends at tick
    # 2000, plus the few ticks of ending the scheduler. One health period more would be 2100.
    assert 2000 <= stats["tick"]["count"] <= 2050
    assert stats["log_lines_dropped"] == 0
    # The build id, as --version and the log say it (platform/posix/build_id.h).
    assert stats["build_id"] == results.binary_build_id(node.binary)
    assert f" build {stats['build_id']}" in node.log()

    tasks = {task["name"]: task for task in stats["tasks"]}
    assert set(tasks) == {"can_rx", "sensor", "process", "can_tx", "diag", "health", "flash",
                          "IDLE"}
    # Priorities as documented in app/app_tasks.h, highest first.
    by_priority = sorted(tasks, key=lambda name: tasks[name]["priority"], reverse=True)
    assert by_priority == ["can_rx", "sensor", "process", "can_tx", "diag", "health", "flash",
                           "IDLE"]
    assert len({task["priority"] for task in tasks.values()}) == len(tasks)
    assert sum(task["run_time_us"] for task in tasks.values()) <= stats["run_time_total_us"]
    # A node that sends a few frames every 10 ms is idle nearly all the time.
    assert tasks["IDLE"]["run_time_us"] > 0.5 * stats["run_time_total_us"]
    # Every task registered its real stack, and none of them is anywhere near its limit.
    for task in tasks.values():
        assert 0 < task["stack_peak_bytes"] < 256 * 1024
        # Three different figures (platform/include/platform.h, plat_stack_info_t): what the C
        # library and the port had in use before the task function ran, less than the peak;
        # and the stack the task was created with, the size meant for the target, not the
        # 512 KiB pthread stack it runs on here.
        assert 0 < task["stack_start_bytes"] < task["stack_peak_bytes"], task
        assert 0 < task["stack_size_bytes"] < 256 * 1024, task
    # What each application task's own code used, its peak less the start, has to fit the
    # stack it was created with. A tick or interrupt signal can land on a task at its deepest
    # point, in poll mode too (the tick is SIGALRM to the running task), so one signal frame
    # is allowed: the kernel's minimum for this CPU (AT_MINSIGSTKSZ; larger on a CPU with
    # AVX-512) plus 512 bytes for the handler. This catches a gross regression on the host;
    # the Cortex-M sizing is checked on the target, by configCHECK_FOR_STACK_OVERFLOW. The
    # idle task is left out: its stack is the kernel's minimum, 2048 bytes, and on the host
    # its own use, about 2000 bytes, is mostly the C library's nanosleep. A sanitizer build
    # says so ("stack_instrumented"), and its figures are not checked against the budget:
    # AddressSanitizer's redzones make every frame larger, about 6.5 KiB for can_tx against
    # 1.9 KiB in the release build.
    assert stats["stack_instrumented"] is (node.binary.parent.parent.name in ("asan", "tsan"))
    signal_frame = _min_signal_stack_bytes() + 512
    for task in tasks.values():
        if task["name"] != "IDLE" and not stats["stack_instrumented"]:
            used = task["stack_peak_bytes"] - task["stack_start_bytes"]
            assert used <= task["stack_size_bytes"] + signal_frame, (task, signal_frame)

    log = node.log()
    assert "starting" in log
    assert "stopping after" in log
    assert "scheduler stopped" in log


def test_the_longest_run_is_accepted(start_node):
    # 86400 s, the limit; one more is refused (above). The run is ended early, by a stop.
    node = start_node("--duration-s", "86400")
    with node.sim() as sim:
        assert sim.ping() == node.pid
    assert node.stop() == EXIT_OK


def test_a_stalled_host_is_accounted_as_dropped_ticks(start_node, ports):
    bus = open_bus(f"udp:{ports.bus}")
    try:
        node = start_node("--duration-s", "2")
        # The first status frame goes out at tick 10: the tick is running. (Simulation control
        # answers earlier, before the scheduler has started.)
        assert wait_for_id(bus, ID_STATUS, timeout=10.0) is not None
    finally:
        bus.shutdown()
    # The host does not run the node for 1.5 s: far more ticks than the tick code makes up
    # (POSIX_TICK_MAX_CATCH_UP_TICKS in platform/posix/posix_tick_plan.h, 6), so it gives them
    # up as dropped, all at once when the node runs again.
    with node.sim() as sim:
        before = sim.can_status()["ticks_dropped"]
        node.send_signal(signal.SIGSTOP)
        time.sleep(1.5)
        node.send_signal(signal.SIGCONT)
        # can.status reports them once the tick thread has counted them, when it first runs
        # after the stall; a reply right after the stall can come before that. The count comes
        # with the first tick after the stall, long before the node has run its 2 s of ticks.
        # So only a node that never reports it is still waited for when the run ends and
        # can.status gets no reply; the assertions below then say what it reported.
        after = sim.can_status()["ticks_dropped"]
        try:
            while after - before < 1490:
                time.sleep(0.01)
                after = sim.can_status()["ticks_dropped"]
        except SimError:
            pass
    assert node.wait(timeout=30) == EXIT_OK

    tick = node.stats()["tick"]
    # The 1500 ticks of the stall are reported as dropped, less the one that is then counted
    # and a few for the time the signals take to arrive, however busy the host was besides.
    # The node's time stopped with it: the run still ends after 2 s of ticks.
    assert tick["dropped"] >= 1490, tick
    assert 2000 <= tick["count"] <= 2050, tick
    # can.status (ticks_dropped) reports the same count while the node runs: it showed the
    # stall's ticks, and no more than the statistics report at the end.
    assert after - before >= 1490, (before, after, tick)
    assert before <= after <= tick["dropped"], (before, after, tick)


@pytest.mark.parametrize("stop_signal", [signal.SIGTERM, signal.SIGINT])
def test_a_stop_signal_ends_the_node_through_its_orderly_path(start_node, stop_signal):
    node = start_node()
    # The simulated hardware answers as soon as the node is up: no sleeping to wait for it.
    with node.sim(timeout=0.5) as sim:
        assert sim.ping() == node.pid
    node.send_signal(stop_signal)
    # Longer than the shutdown guard (POSIX_SHUTDOWN_TIMEOUT_MS, 5 s), so that a stop that
    # hangs is reported with the guard's exit status 70 and not as a timeout of this test.
    assert node.wait(timeout=8) == EXIT_OK
    assert "scheduler stopped" in node.log()
    assert node.stats()["schema"] == "fcn.node_stats/1"     # it went through write_stats()


@pytest.mark.timing
def test_the_tick_tracks_the_wall_clock(start_node):
    node = start_node("--duration-s", "3")
    assert node.wait(timeout=20) == EXIT_OK
    stats = node.stats()
    tick = stats["tick"]
    # Every tick period of wall time is either counted or reported as dropped (dropped: the
    # host did not run the node; zero on a machine with a CPU to spare). elapsed_ns includes
    # starting and stopping the scheduler, so this bound is loose; the platform self-test
    # checks the same identity precisely.
    period_us = stats["elapsed_ns"] / 1000 / (tick["count"] + tick["dropped"])
    check_timing(970 <= period_us <= 1030,
                 f"one accounted tick period took {period_us:.1f} us ({tick['dropped']} dropped)")
    # A dropped tick is excused only as long as there are few: the count comes from the tick
    # code itself, and without a bound a tick that runs slow and books the difference as
    # dropped would pass. 2 % is 60 ticks here; a busy laptop drops about 0.1 %.
    check_timing(tick["dropped"] <= 0.02 * tick["count"],
                 f"{tick['dropped']} of {tick['count']} ticks dropped")


@pytest.mark.parametrize("option, value", [("--tick-mode", "stock"), ("--irq-mode", "poll")])
def test_the_alternative_platform_modes_still_run(start_node, option, value):
    node = start_node("--duration-s", "1", option, value)
    assert node.wait(timeout=20) == EXIT_OK
    stats = node.stats()
    tick, irq = stats["tick"], stats["irq"]
    # The tick statistics report the mode the tick code ran in, not the option echoed back.
    assert tick["mode"] == ("stock" if value == "stock" else "locked")
    if value == "stock":
        # The unmodified port never makes up for a tick, never gives one up and never skips a
        # raise to give one back.
        assert tick["catch_ups"] == 0 and tick["dropped"] == 0 and tick["give_backs"] == 0
    if value == "poll":
        # Polling only: no signal is ever sent, and every interrupt is served from the tick.
        assert irq["signals_sent"] == 0 and irq["serviced_by_signal"] == 0
        assert irq["serviced_by_tick"] > 0
    else:
        assert irq["signals_sent"] > 0 and irq["serviced_by_signal"] > 0
    # The CAN path works in every mode: one status frame per 10 ticks of the node's time, but
    # for the periods the sensor task skipped after an overrun (app/app.c, sensor_task()).
    skipped = stats["sensor"]["skipped_periods"]
    assert abs(status_queue_counts(stats)["sent"] + skipped - tick["count"] / 10) <= 3
    # Simulated interrupt handlers never run on top of each other (docs/bug-log.md, 002).
    assert irq["max_nesting"] == (1 if value != "poll" else 0)


# A harness passes its process id in FCN_DIE_WITH_PARENT; see platform/posix/node_main.c.

_START_A_NODE_AND_DIE = """
import os, subprocess, sys, time
node = subprocess.Popen(sys.argv[1:], env=dict(os.environ, FCN_DIE_WITH_PARENT=str(os.getpid())))
print(node.pid, flush=True)
time.sleep(0.5)          # the node is up and running by now
os._exit(0)              # the harness dies without stopping its node
"""


def _process_exists(pid: int) -> bool:
    """True while the process runs. A zombie (killed, not yet reaped by its new parent) is
    dead: os.kill(pid, 0) would still succeed for it, and whether it is reaped soon depends
    on who the parent is, not on the node."""
    try:
        with open(f"/proc/{pid}/stat", encoding="ascii") as stat:
            # The state follows the command name, which is in parentheses and may contain
            # spaces: take the first field after the last ')'.
            state = stat.read().rsplit(")", 1)[1].split()[0]
    except (FileNotFoundError, ProcessLookupError, IndexError):
        return False
    return state not in ("Z", "X")


def test_a_node_is_killed_when_the_harness_that_started_it_dies(node_binary, ports):
    # The duration only bounds the damage if this test fails: the node then ends by itself.
    harness = subprocess.run(
        [sys.executable, "-c", _START_A_NODE_AND_DIE, str(node_binary), "--can",
         f"udp:{ports.bus}", "--duration-s", "30"],
        capture_output=True, text=True, timeout=20)
    node_pid = int(harness.stdout.strip())

    deadline = time.monotonic() + 5.0
    while _process_exists(node_pid) and time.monotonic() < deadline:
        time.sleep(0.05)
    try:
        assert not _process_exists(node_pid), "the node outlived its harness"
    finally:
        if _process_exists(node_pid):
            os.kill(node_pid, signal.SIGKILL)


def test_a_node_whose_harness_is_gone_before_it_looks_does_not_run(node_binary, ports):
    # The process id of a parent that this node does not have: what the node sees when its
    # harness died before the node could ask the kernel to be killed with it.
    out = subprocess.run([str(node_binary), "--duration-s", "5", "--can", f"udp:{ports.bus}"],
                         capture_output=True, text=True, timeout=10,
                         env=dict(os.environ, FCN_DIE_WITH_PARENT="1"))
    assert out.returncode == 1
