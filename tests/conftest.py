"""Fixtures of the Python tests.

A system test starts the real node binary and observes it from outside, over the CAN bus and
through the simulation control channel. Every test gets

  - its own work directory for the node's log, statistics and data files, kept so that CI can
    upload it;
  - its own UDP ports (a bus and a simulation control port), so tests cannot talk to each
    other's nodes or to anything else running on the machine;
  - a clean end: when it finishes, its nodes that still run are stopped in order (SIGTERM,
    as stop() does; one that has not ended STOP_TIMEOUT_S later is killed, and the test
    fails). The test fails if one of its nodes ended by itself without the test looking at
    its exit status (or exited 0 but wrote to stderr), did not end in order when the harness
    stopped it, was reset (an exit status of plat_reset_reason_t, the hardware watchdog's 11
    among them) without the test waiting for that reset with wait_for_reset(), ended as a loss
    of supply (12) in a run without a cut of the flash's supply armed (NodeProcess.cut_armed),
    or ended in
    order with statistics that show a kick of the hardware watchdog missing
    (NodeProcess.lost_kicks()). The session fails if a node is still running at the end, and
    the kernel kills a node whose test run died. The reset check shows that no node was
    reset without the test waiting for it; the kick check, that in every run that ended in
    order the hardware watchdog was started and every kick the health task decided on
    reached it. Neither shows that the kicks came often enough in a run shorter than the
    watchdog's timeout (500 ms) or in the last 500 ms before an orderly stop, which switches
    the watchdog off: the watchdog cannot reset the node there.

With --timing-report FILE, every failed test is listed in FILE as "miss <test>" if it failed
only on a timing tolerance (fcn_tools.timing) and as "fail <test>" otherwise; that is how
fcn_tools.timing.run_with_one_retry, which scripts/test-system.sh runs, decides whether a
repetition is allowed. tests/python/test_harness.py checks both with a copy of this file.

Tests that use the `can_spec` or `open_link` fixture run once per transport: on the UDP bus and
on SocketCAN. The SocketCAN run is skipped on a machine without the interface, unless
FCN_REQUIRE_SOCKETCAN is set (CI sets it), in which case a missing interface is a failure.
"""
from __future__ import annotations

import contextlib
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

import can
import pytest

from fcn_tools import bus as bus_module
from fcn_tools import node as node_module
from fcn_tools import paths
from fcn_tools.node import EXIT_OK, NodeProcess
from fcn_tools.ports import PortAllocator, PortBlock
from fcn_tools.timing import TimingToleranceMissed

SOCKETCAN_INTERFACE = os.environ.get("FCN_TEST_CAN_INTERFACE", "vcan0")

#: How long the end of a test waits for a node it stops (SIGTERM) to end: the node stops at the
#: end of the health period it is in (100 ms, app/app.c), the rest is room for a slow host. Not
#: a timing tolerance: a node that does not end in this time is killed, and the test fails.
STOP_TIMEOUT_S = 5.0

# Written into an artifacts directory the harness created; only such a directory is ever
# emptied by it.
ARTIFACTS_MARKER = ".fcn-artifacts"


def pytest_addoption(parser):
    parser.addoption(
        "--artifacts", default=None,
        help="directory that receives one sub-directory per test with the node's files "
             "(default: <build root>/system-test/logs)")
    parser.addoption(
        "--timing-report", default=None,
        help="append 'miss <test>' or 'fail <test>' to this file for every failed test")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    path = item.config.getoption("--timing-report")
    if path and report.failed:
        timing_only = (report.when == "call" and call.excinfo is not None
                       and call.excinfo.errisinstance(TimingToleranceMissed))
        with open(path, "a", encoding="utf-8") as out:
            out.write(f"{'miss' if timing_only else 'fail'} {item.nodeid}\n")


@pytest.fixture(scope="session")
def artifacts_root(request) -> Path:
    option = request.config.getoption("--artifacts")
    root = Path(option) if option else paths.build_root() / "system-test" / "logs"
    if root.exists():
        # Only a directory this harness made is emptied: --artifacts pointing at anything
        # else (the work tree, a directory of notes) must never lose a file.
        if not (root / ARTIFACTS_MARKER).is_file():
            if any(root.iterdir()):
                pytest.exit(f"--artifacts {root} is not empty and was not created by the test "
                            f"harness (no {ARTIFACTS_MARKER} in it): refusing to delete it",
                            returncode=4)
        else:
            shutil.rmtree(root)
    root.mkdir(parents=True, exist_ok=True)
    (root / ARTIFACTS_MARKER).write_text("created by tests/conftest.py; emptied on every run\n")
    return root


@pytest.fixture(scope="session", autouse=True)
def no_node_survives_the_session():
    """A node left running after the last test is a harness bug: fail loudly, then clean up."""
    yield
    survivors = node_module.still_running()
    for node in survivors:
        node.kill()
    assert not survivors, f"{len(survivors)} node process(es) were still running at session end"


@pytest.fixture(scope="session")
def node_binary() -> Path:
    binary = paths.node_binary()
    if not binary.exists():
        pytest.fail(f"{binary} not found: build it with scripts/build.sh release", pytrace=False)
    return binary


@pytest.fixture
def workdir(artifacts_root, request) -> Path:
    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", request.node.nodeid.split("::", 1)[-1])
    path = artifacts_root / name
    path.mkdir(parents=True, exist_ok=True)
    return path


@pytest.fixture(scope="session")
def port_allocator() -> PortAllocator:
    return PortAllocator()


@pytest.fixture
def ports(port_allocator) -> PortBlock:
    """UDP ports that belong to this test alone: `ports.bus` and `ports.sim`."""
    block = port_allocator.take()
    yield block
    block.release()


def _stop_in_order(node: NodeProcess) -> str | None:
    """Stop a node that the test left running, as stop() does; what went wrong, or None."""
    try:
        status = node.stop(timeout=STOP_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return (f"{node.name} had not ended {STOP_TIMEOUT_S} s after the end of the test asked "
                "it to stop (SIGTERM), and was killed (SIGKILL): it wrote no statistics, so its "
                "kicks were not checked")
    if status == EXIT_OK:
        return None
    tail = "\n".join(node.stderr().strip().splitlines()[-20:])
    return (f"{node.name} ended with status {status}, not in order, when the end of the test "
            f"asked it to stop (SIGTERM)\n--- stderr (last lines)\n{tail}")


@pytest.fixture
def started_nodes():
    """Every node a test started through start_node or restart_node. When the test ends, the
    nodes that still run are stopped in order, and the test fails on a death it did not see,
    a node that did not stop in order, a reset it did not wait for, or a kick of the hardware
    watchdog missing from a run that ended in order."""
    started: list[NodeProcess] = []

    yield started

    # A node that still runs has not ended by itself (unnoticed_end() has nothing to say about
    # it yet); stopping it in order makes its statistics, and with them its kicks, checkable.
    problems = []
    for node in started:
        problem = _stop_in_order(node) if node.running else None
        problem = (problem or node.unnoticed_end() or node.unexpected_reset()
                   or node.lost_kicks())
        if problem:
            problems.append(problem)
    for node in started:
        node.kill()     # every node has ended by now; this only makes sure
    if problems:
        pytest.fail("\n\n".join(problems), pytrace=False)


@pytest.fixture
def start_node(workdir, node_binary, ports, started_nodes):
    """Factory: start_node(*args, name="node", can=None, retained_file=None, flash_file=None,
    boot_args=(), boot_binary=None) returns a running NodeProcess.

    Without `can` the node gets the test's own UDP bus. The first node of a test serves
    simulation control on the test's port; further nodes of the same test do not, except runs
    started by restart_node, which take the place of the run they follow. `retained_file` is
    the node's retained-RAM cell (--retained-file). With a `flash_file` the node is started
    through the bootloader, fcn_boot (or `boot_binary`), with `boot_args` as its own options:
    the image the bootloader selects in the flash runs, and `args` are its options
    (NodeProcess).
    """

    def _start(*args: str, name: str = "node", can: str | None = None,
               retained_file: Path | None = None, flash_file: Path | None = None,
               boot_args: tuple[str, ...] = (), boot_binary: Path | None = None) -> NodeProcess:
        node = NodeProcess(
            workdir=workdir, args=args, binary=node_binary, name=name,
            can=can or f"udp:{ports.bus}",
            sim_port=None if started_nodes else ports.sim,
            retained_file=retained_file, flash_file=flash_file, boot_args=boot_args,
            **({} if boot_binary is None else {"boot_binary": boot_binary}),
        ).start()
        started_nodes.append(node)
        return node

    return _start


@pytest.fixture
def start_board(workdir, node_binary, ports, started_nodes):
    """Factory: start_board(*args, flash_file, retained_file, name="node", can=None,
    boot_args=()) returns a running fcn_tools.node.Board: the node started through fcn_boot on
    `flash_file` and `retained_file`, and started again through fcn_boot after every requested
    reset (exit status 10) the test announces with Board.expect_resets(), as a reset into the
    bootloader starts a board again. Every run it starts is one of started_nodes, with the same end
    checks; the runs take the simulation control port of the test, as restart_node's do. When the
    test ends, the Board's current run is stopped in order and its thread joined."""
    boards: list[node_module.Board] = []

    def _start(*args: str, flash_file: Path, retained_file: Path, name: str = "node",
               can: str | None = None, boot_args: tuple[str, ...] = ()) -> node_module.Board:
        first = NodeProcess(workdir=workdir, args=args, binary=node_binary, name=name,
                            can=can or f"udp:{ports.bus}",
                            sim_port=None if started_nodes else ports.sim,
                            retained_file=retained_file, flash_file=flash_file,
                            boot_args=boot_args)
        board = node_module.Board(first, on_start=started_nodes.append).start()
        boards.append(board)
        return board

    yield _start

    for board in boards:
        if board.current.running:
            try:
                board.stop(timeout=STOP_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                pass    # started_nodes reports it: killed, no statistics


@pytest.fixture
def restart_node(started_nodes):
    """Factory: restart_node(node, name=None, args=None, boot_args=None) starts the next run of
    a node that has ended, with its options, bus, simulation control port, retained-RAM cell and
    flash file, as a board starts again after a reset (NodeProcess.restarted(), which also says
    what `args` and `boot_args` replace), and returns it running."""

    def _restart(node: NodeProcess, name: str | None = None, args=None,
                 boot_args=None) -> NodeProcess:
        again = node.restarted(name, args=args, boot_args=boot_args).start()
        started_nodes.append(again)
        return again

    return _restart


# ------------------------------------------------------------------------------- the bus


#: The transports every bus test runs on, as fixture parameters.
TRANSPORTS = ["udp", pytest.param("socketcan", marks=pytest.mark.socketcan)]


@contextlib.contextmanager
def _transport(kind: str, ports: PortBlock) -> Iterator[str]:
    """The bus specification of one transport ("udp" or "socketcan") while it is in use."""
    if kind == "udp":
        yield f"udp:{ports.bus}"
        return

    if not bus_module.socketcan_available(SOCKETCAN_INTERFACE):
        message = (f"SocketCAN interface {SOCKETCAN_INTERFACE} is not available "
                   "(scripts/vcan-up.sh creates it; see docs/wsl-vcan.md)")
        if os.environ.get("FCN_REQUIRE_SOCKETCAN"):
            pytest.fail(message, pytrace=False)
        pytest.skip(message)

    # One interface is shared by everything on the machine. A lock file keeps two test runs,
    # or a test run and a bench, from using it at the same time; the tests still assume nobody
    # else transmits on it.
    with bus_module.socketcan_lock(SOCKETCAN_INTERFACE):
        yield f"socketcan:{SOCKETCAN_INTERFACE}"


@pytest.fixture(params=TRANSPORTS)
def can_spec(request, ports) -> str:
    """The bus of this test, once per transport."""
    with _transport(request.param, ports) as spec:
        yield spec


@pytest.fixture(scope="session")
def transport():
    """transport(kind, ports): the context manager behind can_spec, for a fixture of a wider
    scope than one test (a run that several tests of a module look at). The SocketCAN
    interface is locked only inside the `with` block: release it before the tests run, or a
    test that takes can_spec waits for it for ever."""
    return _transport


@dataclass
class Link:
    """A running node and the tester's end of its bus."""

    bus: can.BusABC
    node: NodeProcess
    spec: str
    first_status: can.Message   # the first status frame (BMS_Heartbeat) the node sent
    # What the node sent before it: the frames that won the bus in the same tick. Phase 2
    # publishes BMS_PackStatus (0x020) and others with the first heartbeat (0x030), and a lower
    # identifier goes first. With these, a tester has every frame the node sent.
    before_first_status: list[can.Message]


@pytest.fixture
def open_link(can_spec, start_node):
    """Factory: open_link(*node_args) starts a node on the test's bus and returns a Link.

    The tester's end of the bus is opened before the node starts, so the tester sees every
    frame from the first one, and the link is handed out once the first status frame has
    arrived, with the frames that came before it: tests never sleep to wait for the node.
    """
    buses: list[can.BusABC] = []

    def _open(*node_args: str) -> Link:
        bus = bus_module.open_bus(can_spec)
        buses.append(bus)
        node = start_node(*node_args, can=can_spec)
        before: list[can.Message] = []

        def is_first_status(message: can.Message) -> bool:
            if bus_module.is_status(message):
                return True
            before.append(message)
            return False

        first = bus_module.wait_for(bus, is_first_status, timeout=5.0)
        if first is None:
            pytest.fail("the node sent no status frame within 5 s of its start\n"
                        f"--- stderr\n{node.stderr()}\n--- log\n{node.log()}", pytrace=False)
        return Link(bus=bus, node=node, spec=can_spec, first_status=first,
                    before_first_status=before)

    yield _open

    for bus in buses:
        bus.shutdown()
