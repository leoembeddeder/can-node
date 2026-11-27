"""The node against things that go wrong around it: a configuration it cannot work with, a bus
that is busier than it is fast, a host that stalls it, a transport that goes away.

Each of these came out of the review of phase 1; two of them are entries of docs/bug-log.md
(007 and 008). Assertions about how well the host kept time are made with
fcn_tools.timing.check_timing in tests marked `timing`; everything else is exact.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

import can
import pytest

from fcn_tools.bus import ID_PING, open_bus
from fcn_tools.node import EXIT_OK, lost_kicks_in
from fcn_tools.timing import check_timing

# platform/posix/posix_platform.h: the node's exit status when its CAN transport went away.
EXIT_CAN_TRANSPORT_LOST = 74

BUS_SLOTS = 16


def run_node(node_binary, *args: str, timeout: float = 30) -> subprocess.CompletedProcess:
    return subprocess.run([str(node_binary), *args], capture_output=True, text=True,
                          timeout=timeout)


def can_stats(node) -> dict:
    return node.stats()["can"]


def assert_every_received_frame_is_accounted_for(stats: dict) -> None:
    """What the controller model delivered was taken by the driver, lost to a receive FIFO
    overrun, or still in the FIFO when the node stopped; nothing else (node_main.c)."""
    controller, driver = stats["controller"], stats["driver"]
    assert controller["rx_frames"] == (driver["rx_frames"] + driver["rx_fifo_overruns"]
                                       + driver["rx_fifo_pending"])
    assert stats["pacing"]["rx_frames"] == controller["rx_frames"]


class Flood:
    """Sends base-format data frames without data to the node at a set rate, from a port of the
    node's UDP bus (the last slot; the node, started first, holds the first)."""

    def __init__(self, bus_port: int, arbitration_id: int, frames_per_ms: int) -> None:
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.bind(("127.0.0.1", bus_port + BUS_SLOTS - 1))
        self._target = ("127.0.0.1", bus_port)
        # The datagram of platform/posix/can_backend.h: can_id, length 0, padding, no data.
        self._datagram = struct.pack("<IB3x8x", arbitration_id, 0)
        self._frames_per_ms = frames_per_ms
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self.sent = 0

    def _run(self) -> None:
        # Keeps to the rate on average: after a late wake-up the frames that are due go out at
        # once, at most a millisecond's worth per batch (the node's model holds 64 that arrive
        # early, and drops and counts the rest as wire overruns).
        start = time.monotonic()
        while not self._stop.is_set():
            due = int((time.monotonic() - start) * 1000 * self._frames_per_ms)
            batch = min(due - self.sent, self._frames_per_ms)
            for _ in range(batch):
                self._socket.sendto(self._datagram, self._target)
            self.sent += max(batch, 0)
            if batch < self._frames_per_ms:
                time.sleep(0.0002)

    def __enter__(self) -> "Flood":
        self._thread.start()
        return self

    def __exit__(self, *exc_info) -> None:
        self._stop.set()
        self._thread.join(timeout=5)
        self._socket.close()


# --------------------------------------------------------------------- configuration errors


@pytest.mark.parametrize("offset", [0, 1, BUS_SLOTS - 1])
def test_a_simulation_control_port_on_the_udp_bus_is_refused_at_start(node_binary, ports, offset):
    # It would take a slot of the bus, hear every frame and answer each with an error reply.
    sim_port = ports.bus + offset
    out = run_node(node_binary, "--duration-s", "1", "--can", f"udp:{ports.bus}",
                   "--sim-port", str(sim_port))
    assert out.returncode == 1
    assert (f"--sim-port {sim_port} is one of the ports of the UDP bus udp:{ports.bus} "
            f"({ports.bus} to {ports.bus + BUS_SLOTS - 1})") in out.stderr


def test_a_simulation_control_port_on_the_default_bus_is_refused_at_start(node_binary):
    out = run_node(node_binary, "--duration-s", "1", "--sim-port", "29507")
    assert out.returncode == 1
    assert "is one of the ports of the UDP bus udp (29500 to 29515)" in out.stderr


@pytest.mark.parametrize("port", ["+{bus}", " {bus}", "{bus} ", "-1", "-18446744073709512516",
                                  "0x10", "99999999999999999999999"])
def test_a_udp_port_that_is_not_plain_digits_is_refused_at_start(node_binary, ports, port):
    # strtoul() alone takes a sign, leading spaces and a negative number wrapped around:
    # "-18446744073709512516" was port 39100.
    spec = "udp:" + port.format(bus=ports.bus)
    out = run_node(node_binary, "--duration-s", "1", "--can", spec)
    assert out.returncode == 1
    assert f"unknown CAN backend '{spec}'" in out.stderr


@pytest.mark.parametrize("option", ["--log-file", "--stats-json"])
@pytest.mark.parametrize("where", ["missing directory", "a directory"])
def test_an_output_file_that_cannot_be_written_is_refused_at_start(node_binary, ports, workdir,
                                                                   option, where):
    # Before the fix the log went to stderr instead, with exit status 0, and the statistics
    # were found unwritable only after the whole run: an hour here.
    path = (str(workdir / "no-such-directory" / "out.txt") if where == "missing directory"
            else str(workdir))
    out = run_node(node_binary, "--duration-s", "3600", "--can", f"udp:{ports.bus}",
                   option, path)
    assert out.returncode == 1
    reason = "No such file or directory" if where == "missing directory" else "Is a directory"
    assert out.stderr == f"fcn_node: cannot write {option} {path}: {reason}\n"


# ---------------------------------------------------------------------- the receive path


def test_every_received_frame_is_accounted_for_when_the_node_stops_during_traffic(
        start_node, ports):
    # Frames keep arriving while the node stops: the driver's counters are taken before the
    # end of the scheduler, the controller's after the hardware thread has stopped.
    node = start_node("--duration-s", "2")
    with node.sim() as sim:
        sim.ping()    # the node is up and holds the first slot of the bus
    with Flood(ports.bus, 0x555, frames_per_ms=2) as flood:
        assert node.wait(timeout=20) == EXIT_OK
    stats = can_stats(node)
    assert flood.sent > 0 and stats["controller"]["rx_frames"] > 0
    assert_every_received_frame_is_accounted_for(stats)


# The window over which load and losses are measured, the least load it must see (more frames
# per millisecond than a batch of 16 per tick takes) and the share of the window's frames that
# a receive path that keeps up may lose.
WINDOW_S = 2.0
LEAST_FRAMES_PER_MS = 16.8
LOSS_LIMIT = 0.01
# A full 1 Mbit/s bus: the shortest frame, 44 bits and the 3-bit intermission, every 47 us. It
# fills the receive FIFO (PLAT_CAN_RX_FIFO_DEPTH, platform/include/plat_can.h: 512 frames) in
# 24 ms.
FULL_BUS_FRAMES_PER_MS = 21.3
# CAN_CTRL_WIRE_QUEUE (lib/can/can_ctrl.h): the received frames the controller model holds
# while they wait for the bus. The model delivers frames no faster than the bus carries them,
# except these: a hardware thread that the host did not run for a while delivers them at once.
WIRE_QUEUE = 64
# A loss above LOSS_LIMIT is more than 336 frames at the least load. Less the WIRE_QUEUE frames
# that can come at once, the bus needs 12.8 ms to deliver them, so a receive path that keeps up
# loses that many only if the host held its receive task up for LEAST_STALL_MS or longer
# inside the window.
LEAST_STALL_MS = ((LOSS_LIMIT * LEAST_FRAMES_PER_MS * WINDOW_S * 1000 - WIRE_QUEUE)
                  / FULL_BUS_FRAMES_PER_MS)
# POSIX_TICK_MAX_CATCH_UP_TICKS (platform/posix/posix_tick_plan.h, which posix_tick.c carries
# out). While the receive task is held up with frames waiting, the kernel counts at most one
# tick: ticks and the simulated interrupt are taken on the thread of the running task, and the
# receive task, which outranks every other, becomes the running task as soon as one of them
# has been taken. So the tick code's lag grows by a tick per millisecond. It makes up a lag of
# up to 6 ticks and gives up all but one tick of a longer one, at least 6 at a time: all at
# once when the tick thread runs again, if the host held that thread up too, or 6 every 6 ms
# while the hold-up lasts, if it did not (the last ticks of the lag are then made up, not
# given up). The part of a hold-up inside the window alone takes the lag past 6, so at least one
# give-up is counted between the window's start and the end of the run, however long before the
# window the hold-up began. (The run does not end first: the health task ends it, on a tick,
# and the tick thread checks the lag, and gives up one longer than 6, right after each tick it
# raises.) A whole-run count would add the ticks given up at start-up and for the test's own
# stop; and 23, what a 24 ms hold-up gives up when the tick thread is held up too, is no bound
# for a hold-up that the tick thread ran through (18 or 24 then), nor for the part of one inside
# the window.
# All of this is about a hold-up in one piece. A host that holds the task up in many short
# pieces, running it late at every wake-up, after the next tick but before the one after it,
# is not covered: the kernel takes the tick that came meanwhile as soon as the task runs, and
# the task, which was not waiting for that tick, takes its batch and waits for the next one.
# It gets a batch every other tick while the kernel misses no tick and nothing is given up,
# and the test calls the losses a failure (docs/bug-log.md, 007, has a host made to do that).
MAX_CATCH_UP_TICKS = 6
STALL_DROPPED_TICKS = MAX_CATCH_UP_TICKS
# The stop the test makes before its window. When the node runs again, the model delivers at
# once the WIRE_QUEUE frames that waited for the bus (the tester's frames beyond those were
# wire overruns): more than a batch of 16. A receive path that keeps up drains them in about
# 6 ms, taking 32 per tick against 21.3 arriving, long before the window begins.
STOP_S = 0.010


@pytest.mark.timing
@pytest.mark.parametrize("arbitration_id", [0x555, ID_PING], ids=["unregistered", "ping"])
def test_the_receive_path_keeps_up_with_a_full_bus_of_the_shortest_frames_at_1_mbit(
        start_node, ports, arbitration_id):
    """docs/bug-log.md, 007. A full 1 Mbit/s bus carries up to 21.3 frames per millisecond
    when they have no data. The receive task takes at most one batch per activation and, when
    the batch is full, waits for the next tick. With a batch of 16, once 16 frames were waiting
    it took 16 per tick from then on, the backlog only grew, and the receive FIFO overran for
    as long as the load lasted, whether a handler was registered or not.

    What starts that is any delay of the task that leaves a full batch waiting; on a fast host
    the task may otherwise take the frames as they come. So the test makes the delay itself:
    0.3 s into the load it stops the node (SIGSTOP) for STOP_S, as a busy host would, and when
    the node runs again the model delivers WIRE_QUEUE frames at once. A receive path that keeps
    up drains them in about 6 ms; one that takes 16 per tick falls behind. Then load and losses
    are measured over the same window of WINDOW_S, 0.2 s later. At the LEAST_FRAMES_PER_MS the
    window must see at least, a 16-per-tick path that stays behind for the whole window loses
    at least 4.8 % of what the window delivers; the test allows LOSS_LIMIT.

    A path that keeps up loses more than that only if the host held its receive task up for
    LEAST_STALL_MS or longer inside the window. For a hold-up in one piece, the tick code gives
    up ticks after the window has begun: at least STALL_DROPPED_TICKS, counted by the end of
    the run (the constants above say why). So the test counts the ticks dropped from the
    window's start (ticks_dropped in the first can.status) to the end of the run (the
    statistics), and ends the run (SIGTERM) right after the window. That span takes in a
    hold-up that overlaps the window's start or its end, and leaves out the start-up and the
    test's own stop, both before the window. Losses above LOSS_LIMIT are a timing miss in a run
    that dropped STALL_DROPPED_TICKS or more in that span, and a plain failure in one that
    dropped fewer: then the receive path did not keep up, or the host ran it late at every
    wake-up, which costs frames without a dropped tick (the constants above say how)."""
    # The 4 s only bound a run that the test does not get to end itself.
    node = start_node("--bitrate", "1000000", "--duration-s", "4")
    with node.sim() as sim:
        sim.ping()
        with Flood(ports.bus, arbitration_id, frames_per_ms=32) as flood:
            time.sleep(0.3)
            node.send_signal(signal.SIGSTOP)
            time.sleep(STOP_S)
            node.send_signal(signal.SIGCONT)
            time.sleep(0.2)
            first = sim.can_status()
            sent_first = flood.sent
            time.sleep(WINDOW_S)
            last = sim.can_status()
            sent_last = flood.sent
            # The orderly end, under the load, right after the window: the span of the dropped
            # ticks counted below ends soon after the window does.
            assert node.stop() == EXIT_OK

    node_stats = node.stats()
    stats = node_stats["can"]
    assert_every_received_frame_is_accounted_for(stats)

    window_ns = last["elapsed_ns"] - first["elapsed_ns"]
    busy = (last["busy_ns"] - first["busy_ns"]) / window_ns
    rate = (last["rx"] - first["rx"]) / (window_ns / 1e6)
    offered = (sent_last - sent_first) / (window_ns / 1e6)
    load = (f"{offered:.1f} frames/ms offered, {rate:.1f} received, bus busy {busy:.0%}, "
            f"largest wake-up lag of the hardware thread {last['wakeup_lag_max_ns'] / 1e6:.1f} ms")
    # First, the load was real: the model delivered more frames per millisecond than 16 per
    # tick. Without that the run says nothing about the receive path, either way.
    check_timing(rate >= LEAST_FRAMES_PER_MS, f"too little load to show the bug ({load}): the "
                                              "tester or the host was too slow")
    delivered = last["rx"] - first["rx"]
    overruns = last["rx_fifo_overruns"] - first["rx_fifo_overruns"]
    # The statistics are written at the end of the run: from the window's start to then.
    whole_run = node_stats["tick"]["dropped"]
    dropped = whole_run - first["ticks_dropped"]
    lost = (f"{overruns} of {delivered} frames delivered in the window lost to receive FIFO "
            f"overruns ({load}; ticks dropped from the window's start to the end of the run "
            f"{dropped}, in the whole run {whole_run}; rx_batches_cut in the whole run "
            f"{stats['driver']['rx_batches_cut']})")
    print(lost)  # shown for passing runs too with pytest -rP (the bug-log entry's test.sh)
    if dropped >= STALL_DROPPED_TICKS:
        check_timing(overruns <= LOSS_LIMIT * delivered, lost)
    else:
        assert overruns <= LOSS_LIMIT * delivered, lost


# ----------------------------------------------------------------------------- host stalls


STALL_S = 0.06
HOLD_FRAMES = 20
# The frames that hold the bus have an identifier below the node's status frame (0x030), so
# each of them wins the arbitration against every frame of the node.
HOLD_ID = 0x001
# One 8-byte base frame at 10 kbit/s: 108 bits without stuff bits; at most 132 with them, plus
# the 3-bit intermission.
MIN_FRAME_S = 0.0108
FRAME_S = 0.0135


def wait_for_status(sim, done, timeout: float = 10.0) -> dict:
    """can.status until `done` holds for it; the last reply if `timeout` runs out first, a bound
    for a node that has gone silent."""
    deadline = time.monotonic() + timeout
    status = sim.can_status()
    while not done(status) and time.monotonic() < deadline:
        time.sleep(0.002)
        status = sim.can_status()
    return status


@pytest.mark.timing
def test_a_host_stall_shows_in_the_wake_up_and_receive_lag(start_node, ports):
    """The pacing lag of transmitted frames does not show a host stall during which the bus was
    held by received frames (posix_can.c). The wake-up lag and the receive lag do.

    At 10 kbit/s, HOLD_FRAMES frames that win every arbitration hold the bus for at least
    HOLD_FRAMES * MIN_FRAME_S after they were sent, and the node's status frame waits behind
    them. Once the node has taken them all in and delivered the first, it is stopped (SIGSTOP)
    for STALL_S. Its hardware thread then waits for the end of the frame on the bus, at most
    FRAME_S ahead, and serves it at least STALL_S - FRAME_S late: the wake-up lag shows the
    stall, whatever the frame is. If the frame is one of the received ones, it is delivered
    that late as well: the receive lag shows it too. It is one of them unless the host let the
    node take them in so far apart that a frame of the node's own got onto the bus between
    them. Whether the node was stopped while the frames held the bus also depends on the host.
    These two are timing tolerances; the bound on the wake-up lag, given the first, is not."""
    node = start_node("--bitrate", "10000")
    bus = open_bus(f"udp:{ports.bus}")
    try:
        with node.sim() as sim:
            before = sim.can_status()
            sent_at = time.monotonic()
            for n in range(HOLD_FRAMES):
                bus.send(can.Message(arbitration_id=HOLD_ID, data=struct.pack("<II", n, n),
                                     is_extended_id=False))
            held = wait_for_status(sim, lambda s: (
                s["transport_rx"] - before["transport_rx"] == HOLD_FRAMES
                and s["rx"] > before["rx"]))
            assert held["transport_rx"] - before["transport_rx"] == HOLD_FRAMES, held
            assert held["rx"] > before["rx"], held
            node.send_signal(signal.SIGSTOP)
            stopped_at = time.monotonic()
            time.sleep(STALL_S)
            node.send_signal(signal.SIGCONT)
            # The node goes on and delivers every frame.
            status = wait_for_status(sim, lambda s: s["rx"] - before["rx"] == HOLD_FRAMES)
            assert status["rx"] - before["rx"] == HOLD_FRAMES, status
        assert node.stop() == EXIT_OK
    finally:
        bus.shutdown()

    check_timing(stopped_at - sent_at < HOLD_FRAMES * MIN_FRAME_S,
                 f"the node was stopped {stopped_at - sent_at:.3f} s after the frames were sent, "
                 "too late to be sure that they still held the bus")
    least_ns = (STALL_S - FRAME_S) * 1e9
    assert status["wakeup_lag_max_ns"] >= least_ns
    stats = can_stats(node)
    pacing = stats["pacing"]
    assert pacing["wakeup_lag_max_ns"] >= least_ns and pacing["wakeups"] > 0
    assert pacing["rx_frames"] == stats["controller"]["rx_frames"] == HOLD_FRAMES
    # No frame was received after the last can.status, so its receive lags are final and the
    # same as in the statistics; the wake-up lag can only have grown since.
    assert status["rx_lag_max_ns"] == pacing["rx_lag_max_ns"]
    assert status["rx_lag_mean_ns"] == pacing["rx_lag_mean_ns"]
    assert status["wakeup_lag_max_ns"] <= pacing["wakeup_lag_max_ns"]
    check_timing(status["rx_lag_max_ns"] >= least_ns,
                 f"largest receive lag {status['rx_lag_max_ns'] / 1e6:.1f} ms, below "
                 f"{least_ns / 1e6:.1f} ms: a frame of the node's own was on the bus when it was "
                 "stopped, because the host let it take in the received frames too far apart")


# --------------------------------------------------------------------- a transport that fails


# Runs inside a user and network namespace of its own (unshare -rn), where it may create a
# vcan interface, take it down or delete it without touching the machine's vcan0. The mode
# "delete" deletes the interface and waits for the node to end; "down-up" takes it down for
# 1 s, brings it up again, waits for 20 more frames on the transport and stops the node.
# Prints one JSON line.
_TRANSPORT_LOST = r"""
import json, os, signal, subprocess, sys, time
from fcn_tools.simctl import SimControl

binary, workdir, sim_port, mode = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
interface = "fcnlost0"

def ip(*args):
    return subprocess.run(["ip", *args], capture_output=True, text=True)

for args in (["link", "set", "lo", "up"], ["link", "add", interface, "type", "vcan"],
             ["link", "set", interface, "up"]):
    done = ip(*args)
    if done.returncode != 0:
        print(json.dumps({"skip": f"ip {' '.join(args)}: {done.stderr.strip()}"}))
        sys.exit(0)

with open(os.path.join(workdir, "node.stdout.txt"), "wb") as out, \
        open(os.path.join(workdir, "node.stderr.txt"), "wb") as err:
    node = subprocess.Popen(
        [binary, "--can", f"socketcan:{interface}", "--sim-port", str(sim_port),
         "--duration-s", "30", "--log-file", os.path.join(workdir, "node.log"),
         "--stats-json", os.path.join(workdir, "node.stats.json")],
        stdout=out, stderr=err, stdin=subprocess.DEVNULL,
        env=dict(os.environ, FCN_DIE_WITH_PARENT=str(os.getpid())))
try:
    with SimControl(sim_port, timeout=1.0) as sim:
        def wait_for_transport_tx(count):
            deadline = time.monotonic() + 10
            status = sim.can_status()
            while status["transport_tx"] < count and time.monotonic() < deadline:
                time.sleep(0.01)
                status = sim.can_status()
            return status

        status = wait_for_transport_tx(5)
        after = None
        if mode == "delete":
            changed = ip("link", "del", interface)
        else:
            changed = ip("link", "set", interface, "down")
            time.sleep(1.0)
            up = ip("link", "set", interface, "up")
            changed = changed if changed.returncode != 0 else up
            after = wait_for_transport_tx(sim.can_status()["transport_tx"] + 20)
            node.send_signal(signal.SIGTERM)
    try:
        status_code = node.wait(timeout=10)
    except subprocess.TimeoutExpired:
        status_code = ("still running 10 s after its interface was deleted" if mode == "delete"
                       else "still running 10 s after a SIGTERM")
finally:
    if node.poll() is None:
        node.kill()
        node.wait()
print(json.dumps({"before": status, "after": after, "deleted": changed.returncode,
                  "deleted_stderr": changed.stderr, "status": status_code}))
"""


def _run_in_private_namespace(node_binary, workdir, ports, mode: str) -> dict:
    """Runs _TRANSPORT_LOST in a user and network namespace of its own and returns its result.
    Where that cannot be done here the test is skipped, or fails if FCN_REQUIRE_SOCKETCAN is
    set (CI sets it): there a test of the SocketCAN transport that cannot run is a failure."""
    def cannot_run(message: str) -> None:
        if os.environ.get("FCN_REQUIRE_SOCKETCAN"):
            pytest.fail(message, pytrace=False)
        pytest.skip(message)

    reason = _user_namespaces_work()
    if reason is not None:
        cannot_run(reason)

    out = subprocess.run(
        ["unshare", "-rn", sys.executable, "-c", _TRANSPORT_LOST, str(node_binary), str(workdir),
         str(ports.sim), mode],
        capture_output=True, text=True, timeout=90)
    assert out.returncode == 0, out.stderr
    result = json.loads(out.stdout.strip().splitlines()[-1])
    if "skip" in result:
        cannot_run(f"no vcan interface in a private namespace: {result['skip']}")
    assert result["deleted"] == 0, result["deleted_stderr"]
    return result


def _user_namespaces_work() -> str | None:
    """Why a private network namespace cannot be had here, or None if it can."""
    for tool in ("unshare", "ip"):
        if shutil.which(tool) is None:
            return f"{tool} is not installed"
    probe = subprocess.run(["unshare", "-rn", "true"], capture_output=True, text=True, timeout=10)
    if probe.returncode != 0:
        return f"unshare -rn is not permitted here: {probe.stderr.strip()}"
    return None


def test_a_transport_that_goes_away_stops_the_node_with_its_own_exit_status(node_binary,
                                                                             workdir, ports):
    """docs/bug-log.md, 008. A SocketCAN interface deleted under a running node used to go
    unnoticed: every frame was still reported sent, nothing was logged, and the node exited 0.
    Now the failure is logged and counted, and the node stops in an orderly way with
    EXIT_CAN_TRANSPORT_LOST, its statistics written. The bus model is left alone: no fault."""
    result = _run_in_private_namespace(node_binary, workdir, ports, "delete")

    # Before: frames went out and nothing failed.
    before = result["before"]
    assert before["transport_tx"] >= 5
    assert before["transport_tx_failed"] == 0 and before["transport_rx_failed"] == 0

    # Deleting an interface takes it down first and then unregisters it. The socket reports
    # ENETDOWN for the first (a failure the node rides out: see the next test), then ENODEV,
    # or ENXIO on a send from the socket the kernel unbound; either of those stops the node.
    assert result["status"] == EXIT_CAN_TRANSPORT_LOST
    failed = "(Network is down|No such device|No such device or address)"
    gone = "(No such device|No such device or address)"
    stderr = (workdir / "node.stderr.txt").read_text()
    assert re.fullmatch(f"fcn_node: the CAN transport socketcan:fcnlost0 went away: {gone}\n",
                        stderr), stderr

    log = (workdir / "node.log").read_text()
    assert re.search(f"transport socketcan:fcnlost0: (send|receive) failed: {failed} "
                     r"\(failures since the last report: 1\)", log), log
    assert re.search(f"transport socketcan:fcnlost0 is gone \\({gone}\\): stopping the node",
                     log), log
    assert "scheduler stopped" in log

    stats = json.loads((workdir / "node.stats.json").read_text())["can"]
    assert re.fullmatch(gone, stats["transport_error"])
    counters = stats["transport_counters"]
    assert counters["tx_failed"] + counters["rx_failed"] >= 1
    # The bus model was not told anything: the node stayed error active, with no errors.
    assert stats["state"] == "active" and stats["tec"] == 0 and stats["rec"] == 0
    assert stats["controller"]["tx_attempts_failed"] == 0


def test_an_interface_that_goes_down_and_up_again_does_not_stop_the_node(node_binary, workdir,
                                                                          ports):
    """A SocketCAN socket stays bound while its interface is down and works again once it is
    up: an interface taken down for a second is a transport failure the node logs and counts,
    and rides out. It keeps running, frames go out again, and it stops on SIGTERM with exit
    status 0 and no transport error."""
    result = _run_in_private_namespace(node_binary, workdir, ports, "down-up")

    assert result["status"] == EXIT_OK
    # 20 more frames reached the transport after the interface came up (10 s allowed).
    assert result["after"]["transport_tx"] >= result["before"]["transport_tx"] + 20

    log = (workdir / "node.log").read_text()
    assert re.search(r"transport socketcan:fcnlost0: (send|receive) failed: Network is down "
                     r"\(failures since the last report: 1\)", log), log
    assert "is gone" not in log
    assert (workdir / "node.stderr.txt").read_text() == ""

    everything = json.loads((workdir / "node.stats.json").read_text())
    # An orderly end: its kicks are checked as tests/conftest.py checks those of its nodes.
    assert lost_kicks_in(everything, "the node") is None, lost_kicks_in(everything, "the node")
    stats = everything["can"]
    assert stats["transport_error"] is None
    counters = stats["transport_counters"]
    assert counters["tx_failed"] + counters["rx_failed"] >= 1
    assert stats["state"] == "active" and stats["tec"] == 0 and stats["rec"] == 0
