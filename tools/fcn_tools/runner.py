# SPDX-License-Identifier: MIT
"""Run the node from a bench script outside pytest, as the fixtures of tests/conftest.py run it
for a test: in a working directory of its own, on UDP ports of its own (fcn_tools.ports) or on
the SocketCAN interface (locked, as the tests lock it), started directly or through fcn_boot
(and followed across requested resets as a Board), and with its statistics read at the end.

    binaries = runner.prepare_binaries(build=not args.no_build)
    results.require_tree_build(binaries.as_dict())          # before measuring anything
    with runner.Session(runner.bench_workdir("node_timing"), transport="socketcan",
                        binaries=binaries) as session:
        node = session.start_node("--duration-s", "60")
        node.wait(timeout=120)
        stats = session.finish(node)                         # statistics of a run ended in order

THE BINARIES. prepare_binaries() builds the release preset (scripts/build.sh release) unless
the bench was told which binaries to use: FCN_NODE, FCN_BOOT or FCN_FLASH_TOOL set (fcn_tools.paths),
or build=False (a bench's --no-build). It records each binary's SHA-256, so that write_result()
can refuse a binary that changed while the bench ran (Binaries.sha256).

THE END OF A SESSION, on every path out of the `with` block. Every node the session started that
still runs is stopped in order (SIGTERM; one that has not ended STOP_TIMEOUT_S later is killed),
then each is killed for certain, the ports are released and the interface unlocked. A session
that ends normally then fails (BenchFailure) if one of its nodes ended by itself without the
bench looking at its exit status, did not stop in order, was reset without the bench waiting for
the reset (wait_for_reset()), ended as a power loss without a cut armed, or ended in order with
a kick of the hardware watchdog missing: the same end checks as the tests' (NodeProcess). A
measurement of a node that went wrong unseen is not written. A session left by an exception
reports those problems on stderr and lets the exception go on.

SIGNALS. While a session is open in the main thread, SIGTERM and SIGHUP raise Interrupted (a
KeyboardInterrupt), as Ctrl-C raises KeyboardInterrupt, so that the block is left through its
end and no node is left behind; during the end itself these signals are held off. The nodes run
in a session of their own (a Ctrl-C at the terminal reaches the bench, not them) and ask the
kernel to kill them when the thread that started them ends (FCN_DIE_WITH_PARENT, node_main.c),
which covers a bench killed with SIGKILL; an atexit hook kills what is left at interpreter exit.
A bench's main() turns Interrupted into exit status 128 + the signal (interrupted_status()).
"""
from __future__ import annotations

import atexit
import os
import signal
import subprocess
import sys
import threading
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Sequence

from . import bus as bus_module
from . import node as node_module
from . import paths
from .node import EXIT_OK, Board, NodeProcess
from .ports import PortAllocator, PortBlock
from .results import sha256_of

#: How long the end of a session waits for a node it stops (SIGTERM) to end, as the test
#: harness does (tests/conftest.py, STOP_TIMEOUT_S): the node stops at the end of the health
#: period it is in (100 ms); the rest is room for a slow host. A bound, not a tolerance.
STOP_TIMEOUT_S = 5.0

#: The SocketCAN interface of the benches, as of the tests (FCN_TEST_CAN_INTERFACE overrides).
DEFAULT_INTERFACE = os.environ.get("FCN_TEST_CAN_INTERFACE", "vcan0")

TRANSPORTS = ("udp", "socketcan")


class BenchFailure(AssertionError):
    """A node of the session went wrong in a way the bench did not see (see the module text)."""


class Interrupted(KeyboardInterrupt):
    """SIGTERM or SIGHUP arrived while a session was open: the bench is to end, cleaning up."""

    def __init__(self, signum: int) -> None:
        super().__init__(f"signal {signal.Signals(signum).name}")
        self.signum = signum


def interrupted_status(error: KeyboardInterrupt) -> int:
    """The exit status for a bench ended by `error`: 128 + the signal (SIGINT for Ctrl-C)."""
    return 128 + (error.signum if isinstance(error, Interrupted) else signal.SIGINT)


# ------------------------------------------------------------------------------ the binaries


@dataclass(frozen=True)
class Binaries:
    """The node, the bootloader and the flash tool a bench runs, where they came from, and the
    SHA-256 of each when they were prepared."""
    node: Path
    boot: Path
    flash: Path
    origin: str
    sha256: dict[str, str] = field(default_factory=dict)

    def as_dict(self) -> dict[str, Path]:
        """By program name, as results.require_tree_build() and write_result() take them."""
        return {"fcn_node": self.node, "fcn_boot": self.boot, "fcn_flash": self.flash}


#: The environment variables that name other binaries than the release build's (paths.py).
BINARY_OVERRIDES = ("FCN_NODE", "FCN_BOOT", "FCN_FLASH_TOOL")


def prepare_binaries(build: bool = True, preset: str = "release",
                     run: Callable[..., object] = subprocess.run) -> Binaries:
    """The binaries of a bench: the `preset` build, built first with scripts/build.sh unless
    `build` is False or one of BINARY_OVERRIDES names a binary (then nothing is built and those
    binaries are used as they are). FileNotFoundError if one is missing. `run` is the command
    runner (a test passes its own)."""
    told = [name for name in BINARY_OVERRIDES if os.environ.get(name)]
    if build and not told:
        print(f"building the {preset} preset (scripts/build.sh {preset}) ...", flush=True)
        run(["bash", str(paths.REPO_ROOT / "scripts" / "build.sh"), preset], check=True)
        origin = f"scripts/build.sh {preset}"
    elif told:
        origin = ", ".join(f"{name}={os.environ[name]}" for name in told)
    else:
        origin = f"the {preset} build as it was (--no-build)"
    found = Binaries(node=paths.node_binary(preset), boot=paths.boot_binary(preset),
                     flash=paths.flash_tool_binary(preset), origin=origin)
    for binary in found.as_dict().values():
        if not binary.exists():
            raise FileNotFoundError(f"{binary} not found: build it with scripts/build.sh {preset}")
    return Binaries(node=found.node, boot=found.boot, flash=found.flash, origin=origin,
                    sha256={name: sha256_of(path) for name, path in found.as_dict().items()})


def bench_workdir(name: str) -> Path:
    """A new, empty working directory for a run of the bench `name`, under the build root:
    <build root>/bench/<name>/<UTC time>-<pid>. Nothing in it is ever deleted by the benches."""
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    path = paths.build_root() / "bench" / name / f"{stamp}-{os.getpid()}"
    path.mkdir(parents=True, exist_ok=False)
    return path


# ------------------------------------------------------------------------------ the session

# The sessions that are open, for the atexit hook.
_OPEN: set["Session"] = set()


def _kill_what_is_left() -> None:
    for session in list(_OPEN):
        session._kill_all()  # noqa: SLF001
    for node in node_module.still_running():
        node.kill()


atexit.register(_kill_what_is_left)


class Session:
    """The nodes of one bench run, on one bus (see the module text). Use as a context manager;
    `workdir` receives every node's log, statistics, stdout and stderr (NodeProcess's files).

    `transport` is "udp" (a bus on ports of the session's own block) or "socketcan" (the
    interface `interface`, locked for the session as the tests lock it). The first node started
    serves simulation control on the block's sim port; later ones do not (pass sim=True to give
    one the port when the first has ended), except restarts and a Board's runs, which take the
    place of the run they follow."""

    def __init__(self, workdir: Path, transport: str = "udp", *, binaries: Binaries | None = None,
                 interface: str = DEFAULT_INTERFACE, stop_timeout: float = STOP_TIMEOUT_S,
                 handle_signals: bool = True) -> None:
        if transport not in TRANSPORTS:
            raise ValueError(f"transport must be one of {', '.join(TRANSPORTS)}, not {transport}")
        self.workdir = Path(workdir)
        self.transport = transport
        self.interface = interface
        self.binaries = binaries
        self.stop_timeout = stop_timeout
        #: Every run the session started (also a Board's, also restarts), in order.
        self.started: list[NodeProcess] = []
        self.boards: list[Board] = []
        #: What the end of the session found wrong (see the module text).
        self.problems: list[str] = []
        self._handle_signals = handle_signals
        self._ports: PortBlock | None = None
        self._lock = None
        self._previous_handlers: dict[int, object] = {}
        self._open = False

    # -------------------------------------------------------------- opening and closing

    def __enter__(self) -> "Session":
        self.open()
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()
        if exc_type is None and self.problems:
            raise BenchFailure("\n\n".join(self.problems))
        if exc_type is not None and self.problems:
            print("the bench's nodes also went wrong:\n" + "\n\n".join(self.problems),
                  file=sys.stderr)

    def open(self) -> "Session":
        if self._open:
            raise RuntimeError("the session is open already")
        self.workdir.mkdir(parents=True, exist_ok=True)
        if self._handle_signals and threading.current_thread() is threading.main_thread():
            for signum in (signal.SIGTERM, signal.SIGHUP):
                self._previous_handlers[signum] = signal.signal(signum, _raise_interrupted)
        try:
            self._ports = PortAllocator().take()
            if self.transport == "socketcan":
                if not bus_module.socketcan_available(self.interface):
                    raise RuntimeError(f"the SocketCAN interface {self.interface} is not "
                                       "available (scripts/vcan-up.sh creates it)")
                self._lock = bus_module.socketcan_lock(
                    self.interface,
                    on_wait=lambda: print(f"waiting for {self.interface}: a test run or another "
                                          "bench is using it", file=sys.stderr, flush=True))
                self._lock.__enter__()
        except BaseException:
            self._release()
            raise
        self._open = True
        _OPEN.add(self)
        return self

    def close(self) -> None:
        """Stop every node in order, check how each ended (`problems`), kill what is left and
        release the ports and the interface. SIGINT, SIGTERM and SIGHUP wait until it is done."""
        if not self._open:
            return
        held = {}
        if threading.current_thread() is threading.main_thread():
            for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                held[signum] = signal.signal(signum, signal.SIG_IGN)
        try:
            self.problems = self._end_checks()
        finally:
            self._kill_all()
            _OPEN.discard(self)
            self._open = False
            self._release()
            for signum, handler in held.items():
                signal.signal(signum, handler)
            for signum, handler in self._previous_handlers.items():
                signal.signal(signum, handler)
            self._previous_handlers.clear()

    def _end_checks(self) -> list[str]:
        problems = []
        for board in self.boards:
            if board.current.running:
                try:
                    board.stop(timeout=self.stop_timeout)
                except subprocess.TimeoutExpired:
                    pass    # reported below: killed, no statistics
        for node in self.started:
            problem = self._stop_in_order(node) if node.running else None
            problem = (problem or node.unnoticed_end() or node.unexpected_reset()
                       or node.lost_kicks())
            if problem:
                problems.append(problem)
        return problems

    def _stop_in_order(self, node: NodeProcess) -> str | None:
        try:
            status = node.stop(timeout=self.stop_timeout)
        except subprocess.TimeoutExpired:
            return (f"{node.name} had not ended {self.stop_timeout} s after the end of the "
                    "session asked it to stop (SIGTERM), and was killed (SIGKILL): it wrote no "
                    "statistics")
        if status == EXIT_OK:
            return None
        tail = "\n".join(node.stderr().strip().splitlines()[-20:])
        return (f"{node.name} ended with status {status}, not in order, when the end of the "
                f"session asked it to stop (SIGTERM)\n--- stderr (last lines)\n{tail}")

    def _kill_all(self) -> None:
        for node in list(self.started):
            try:
                node.kill()
            except Exception:  # noqa: BLE001 - the end must reach every node
                pass

    def _release(self) -> None:
        if self._lock is not None:
            lock, self._lock = self._lock, None
            lock.__exit__(None, None, None)
        if self._ports is not None:
            ports, self._ports = self._ports, None
            ports.release()

    # -------------------------------------------------------------------------- the bus

    @property
    def ports(self) -> PortBlock:
        if self._ports is None:
            raise RuntimeError("the session is not open")
        return self._ports

    @property
    def can_spec(self) -> str:
        """The bus of the session, as fcn_tools.bus.open_bus() and the node's --can take it."""
        if self.transport == "socketcan":
            return f"socketcan:{self.interface}"
        return f"udp:{self.ports.bus}"

    def open_bus(self, **kwargs):
        """A tester's end of the session's bus (fcn_tools.bus.open_bus()); the caller shuts it
        down."""
        return bus_module.open_bus(self.can_spec, **kwargs)

    # ------------------------------------------------------------------------ the nodes

    def _node(self, args: Sequence[str], name: str, sim: bool | None, **kwargs) -> NodeProcess:
        if not self._open:
            raise RuntimeError("the session is not open")
        serve = (not self.started) if sim is None else sim
        extra = {}
        if self.binaries is not None:
            extra = {"binary": self.binaries.node, "boot_binary": self.binaries.boot}
        return NodeProcess(workdir=self.workdir, args=tuple(args), name=name, can=self.can_spec,
                           sim_port=self.ports.sim if serve else None, **extra, **kwargs)

    def start_node(self, *args: str, name: str = "node", sim: bool | None = None,
                   retained_file: Path | None = None, flash_file: Path | None = None,
                   boot_args: Sequence[str] = ()) -> NodeProcess:
        """Start a node on the session's bus and return it running (tests/conftest.py's
        start_node): with a `flash_file`, through fcn_boot with `boot_args`; `args` are the
        node's options."""
        node = self._node(args, name, sim, retained_file=retained_file, flash_file=flash_file,
                          boot_args=tuple(boot_args))
        self.started.append(node)
        return node.start()

    def start_board(self, *args: str, flash_file: Path, retained_file: Path, name: str = "node",
                    sim: bool | None = None, boot_args: Sequence[str] = ()) -> Board:
        """Start a Board (fcn_tools.node.Board): the node through fcn_boot on `flash_file` and
        `retained_file`, started again through fcn_boot after every requested reset announced
        with Board.expect_resets(). Every run it starts is one of the session's."""
        first = self._node(args, name, sim, retained_file=retained_file, flash_file=flash_file,
                           boot_args=tuple(boot_args))
        board = Board(first, on_start=self.started.append)
        self.boards.append(board)
        return board.start()

    def restart(self, node: NodeProcess, name: str | None = None,
                args: Sequence[str] | None = None,
                boot_args: Sequence[str] | None = None) -> NodeProcess:
        """The next run of a node that has ended, as a board starts again after a reset
        (NodeProcess.restarted()), started."""
        again = node.restarted(name, args=args, boot_args=boot_args)
        self.started.append(again)
        return again.start()

    def finish(self, node: NodeProcess, timeout: float | None = None) -> dict:
        """The statistics of `node`, stopped in order now if it still runs (SIGTERM): a run that
        did not end in order (status 0) wrote none, and fails the bench (BenchFailure)."""
        status = node.stop(timeout=self.stop_timeout if timeout is None else timeout)
        if status != EXIT_OK:
            tail = "\n".join(node.stderr().strip().splitlines()[-20:])
            raise BenchFailure(f"{node.name} ended with status {status}, not in order: no "
                               f"statistics\n--- stderr (last lines)\n{tail}")
        try:
            return node.stats()
        except FileNotFoundError:
            raise BenchFailure(f"{node.name} ended in order but wrote no statistics "
                               f"({node.stats_path})") from None


def _raise_interrupted(signum: int, frame) -> None:  # noqa: ARG001 - a signal handler
    raise Interrupted(signum)
