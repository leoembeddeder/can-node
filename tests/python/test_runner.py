"""The node runner of the benches (tools/fcn_tools/runner.py), with a stand-in for the node: a
small program that waits for SIGTERM and then writes statistics and exits 0, as the node does,
or that dies at once, or that ignores SIGTERM. What is checked is how the session ends on every
path out of it, a signal to the bench included: no process is left behind. (The real node runs
through a session in tests/system/test_bench_runner.py.)"""
from __future__ import annotations

import fcntl
import os
import signal
import subprocess
import sys
import textwrap
import threading
import time
from pathlib import Path

import pytest

from fcn_tools import bus as bus_module
from fcn_tools import runner

FAKE_NODE = textwrap.dedent("""\
    import json, os, signal, sys, time
    args = sys.argv[1:]
    stats = args[args.index("--stats-json") + 1]
    mode = os.environ.get("FAKE_NODE_MODE", "run")
    if mode == "die":
        sys.exit(3)
    if mode == "hang":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    stop = []
    if mode == "run":
        signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    print("ready", flush=True)      # SIGTERM is handled from here on
    while not stop:
        time.sleep(0.01)
    with open(stats, "w") as out:
        json.dump({"build_id": "fake", "hw_watchdog": {"started": True, "kicks": 2},
                   "watchdog_manager": {"kicks": 2, "kicks_withheld": 0, "cycles": 2}}, out)
    """)


@pytest.fixture
def fake_binaries(tmp_path) -> runner.Binaries:
    script = tmp_path / "fake_node.py"
    script.write_text(FAKE_NODE)
    program = tmp_path / "fake_node"
    program.write_text(f"#!/bin/sh\nexec '{sys.executable}' '{script}' \"$@\"\n")
    program.chmod(0o755)
    return runner.Binaries(node=program, boot=program, flash=program, origin="test")


def _ready(node) -> object:
    """`node`, once the stand-in handles SIGTERM (it says so on stdout); a bound of 30 s."""
    deadline = time.monotonic() + 30.0
    while "ready" not in (node.stdout_path.read_text() if node.stdout_path.exists() else ""):
        assert time.monotonic() < deadline and node.running, "the stand-in did not start"
        time.sleep(0.01)
    return node


def _gone(pid: int) -> bool:
    """True if no process `pid` runs (or it is a zombie nobody waits for any more)."""
    try:
        state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
    except (OSError, IndexError):
        return True
    return state == "Z"


def test_finish_returns_the_statistics_of_a_run_that_ended_in_order(tmp_path, fake_binaries):
    with runner.Session(tmp_path / "w", binaries=fake_binaries) as session:
        node = _ready(session.start_node("--duration-s", "1"))
        assert node.running
        assert node.sim_port == session.ports.sim          # the first node serves simulation
        second = _ready(session.start_node(name="second"))
        assert second.sim_port is None
        stats = session.finish(node)
    assert stats["build_id"] == "fake"
    assert not node.running and not second.running
    assert session.problems == []


def test_an_exception_in_the_block_stops_every_node_and_goes_on(tmp_path, fake_binaries):
    with pytest.raises(ValueError, match="the bench's own error"):
        with runner.Session(tmp_path, binaries=fake_binaries) as session:
            nodes = [_ready(session.start_node(name=f"n{i}")) for i in range(2)]
            raise ValueError("the bench's own error")
    assert all(_gone(node.pid) for node in nodes)
    # They were stopped in order, so their statistics exist: nothing went wrong unseen.
    assert session.problems == [] and all(node.stats_path.exists() for node in nodes)


def test_a_node_that_ended_unseen_fails_the_session(tmp_path, fake_binaries, monkeypatch):
    monkeypatch.setenv("FAKE_NODE_MODE", "die")
    with pytest.raises(runner.BenchFailure, match="ended by itself with status 3"):
        with runner.Session(tmp_path, binaries=fake_binaries) as session:
            node = session.start_node()
            node._proc.wait(timeout=30)  # noqa: SLF001 - the bench does not look at the status
    assert not node.running


def test_a_node_that_ignores_sigterm_is_killed_and_reported(tmp_path, fake_binaries,
                                                            monkeypatch):
    monkeypatch.setenv("FAKE_NODE_MODE", "hang")
    with pytest.raises(runner.BenchFailure, match="had not ended"):
        with runner.Session(tmp_path, binaries=fake_binaries, stop_timeout=0.5) as session:
            node = _ready(session.start_node())
    assert _gone(node.pid)


def test_finish_of_a_run_that_did_not_end_in_order_fails(tmp_path, fake_binaries, monkeypatch):
    monkeypatch.setenv("FAKE_NODE_MODE", "die")
    with runner.Session(tmp_path, binaries=fake_binaries) as session:
        node = session.start_node()
        node._proc.wait(timeout=30)  # noqa: SLF001
        with pytest.raises(runner.BenchFailure, match="status 3"):
            session.finish(node)
    assert session.problems == []    # finish() looked at the status: the end was noticed


BENCH_SCRIPT = textwrap.dedent("""\
    import signal, sys, time
    from pathlib import Path
    from fcn_tools import runner
    # As at an interactive terminal, whatever the test runner's own SIGINT disposition is.
    signal.signal(signal.SIGINT, signal.default_int_handler)
    binaries = runner.Binaries(node=Path(sys.argv[2]), boot=Path(sys.argv[2]),
                               flash=Path(sys.argv[2]), origin="test")
    try:
        with runner.Session(Path(sys.argv[1]), binaries=binaries) as session:
            node = session.start_node()
            deadline = time.monotonic() + 30          # until the stand-in handles SIGTERM
            while (not node.stdout_path.exists() or "ready" not in node.stdout_path.read_text()
                   ) and time.monotonic() < deadline:
                time.sleep(0.01)
            print(node.pid, flush=True)
            time.sleep(60)
    except KeyboardInterrupt as error:
        sys.exit(runner.interrupted_status(error))
    sys.exit(0)
    """)


@pytest.mark.parametrize("signum", [signal.SIGTERM, signal.SIGINT, signal.SIGHUP])
def test_a_signal_to_the_bench_leaves_no_node_behind(tmp_path, fake_binaries, signum):
    script = tmp_path / "bench_script.py"
    script.write_text(BENCH_SCRIPT)
    env = dict(os.environ, PYTHONPATH=os.pathsep.join(
        [str(Path(runner.__file__).parents[1]), os.environ.get("PYTHONPATH", "")]))
    bench = subprocess.Popen([sys.executable, str(script), str(tmp_path / "w"),
                              str(fake_binaries.node)], stdout=subprocess.PIPE, text=True,
                             env=env)
    try:
        pid = int(bench.stdout.readline())
        assert not _gone(pid)
        bench.send_signal(signum)
        assert bench.wait(timeout=30) == 128 + signum
    finally:
        bench.kill()
        bench.wait(timeout=10)
    assert _gone(pid)
    # Stopped in order (SIGTERM), not killed: it wrote its statistics.
    assert (tmp_path / "w" / "node.stats.json").exists()


def test_the_session_releases_its_ports_and_its_signal_handlers(tmp_path, fake_binaries):
    before = signal.getsignal(signal.SIGTERM)
    with runner.Session(tmp_path, binaries=fake_binaries) as session:
        ports = session.ports
        assert session.can_spec == f"udp:{ports.bus}"
        assert signal.getsignal(signal.SIGTERM) is not before
    assert signal.getsignal(signal.SIGTERM) is before
    assert ports._claim.fileno() == -1  # noqa: SLF001 - the claim's socket is closed
    with pytest.raises(RuntimeError):
        session.ports


def test_a_socketcan_session_needs_its_interface_and_releases_the_ports(tmp_path):
    session = runner.Session(tmp_path, "socketcan", interface="fcnnone0")
    with pytest.raises(RuntimeError, match="fcnnone0 is not available"):
        session.open()
    assert session._ports is None  # noqa: SLF001


def test_an_unknown_transport_is_refused(tmp_path):
    with pytest.raises(ValueError):
        runner.Session(tmp_path, "can-fd")


def test_the_interface_lock_waits_for_its_holder_and_says_so(tmp_path):
    interface = f"fcn-unit-{os.getpid()}"
    waited = threading.Event()
    entered = threading.Event()
    with open(bus_module.socketcan_lock_path(interface), "w") as held:
        fcntl.flock(held, fcntl.LOCK_EX)

        def take() -> None:
            with bus_module.socketcan_lock(interface, on_wait=waited.set):
                entered.set()

        thread = threading.Thread(target=take)
        thread.start()
        assert waited.wait(timeout=10)
        assert not entered.is_set()
        fcntl.flock(held, fcntl.LOCK_UN)
        thread.join(timeout=10)
    assert entered.is_set()
    bus_module.socketcan_lock_path(interface).unlink()


def _release_tree(root: Path) -> None:
    for name in ("fcn_node", "fcn_boot", "fcn_flash"):
        path = root / "release" / "bin" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode())


def test_prepare_binaries_builds_the_release_preset_first(tmp_path, monkeypatch):
    monkeypatch.setenv("FCN_BUILD_ROOT", str(tmp_path))
    for name in runner.BINARY_OVERRIDES:
        monkeypatch.delenv(name, raising=False)
    _release_tree(tmp_path)
    calls = []
    found = runner.prepare_binaries(run=lambda command, check: calls.append(command))
    assert len(calls) == 1 and calls[0][1].endswith("scripts/build.sh")
    assert calls[0][-1] == "release"
    assert found.node == tmp_path / "release" / "bin" / "fcn_node"
    assert found.origin == "scripts/build.sh release"
    assert set(found.sha256) == {"fcn_node", "fcn_boot", "fcn_flash"}


def test_prepare_binaries_builds_nothing_when_told_which_binaries(tmp_path, monkeypatch):
    monkeypatch.setenv("FCN_BUILD_ROOT", str(tmp_path))
    for name in runner.BINARY_OVERRIDES:      # the asan run of the tests sets all three
        monkeypatch.delenv(name, raising=False)
    _release_tree(tmp_path)
    other = tmp_path / "other_node"
    other.write_bytes(b"x")
    monkeypatch.setenv("FCN_NODE", str(other))
    calls = []
    found = runner.prepare_binaries(run=lambda command, check: calls.append(command))
    assert calls == [] and found.node == other and "FCN_NODE" in found.origin
    monkeypatch.delenv("FCN_NODE")
    found = runner.prepare_binaries(build=False, run=lambda command, check: calls.append(command))
    assert calls == [] and "--no-build" in found.origin


def test_prepare_binaries_refuses_a_missing_binary(tmp_path, monkeypatch):
    monkeypatch.setenv("FCN_BUILD_ROOT", str(tmp_path))
    for name in runner.BINARY_OVERRIDES:
        monkeypatch.delenv(name, raising=False)
    with pytest.raises(FileNotFoundError, match="fcn_node"):
        runner.prepare_binaries(build=False)


def test_each_bench_run_gets_a_new_working_directory(tmp_path, monkeypatch):
    monkeypatch.setenv("FCN_BUILD_ROOT", str(tmp_path))
    first = runner.bench_workdir("probe")
    assert first.is_dir() and first.parent == tmp_path / "bench" / "probe"
    assert not any(first.iterdir())


def test_an_interrupted_bench_exits_with_128_and_the_signal():
    assert runner.interrupted_status(runner.Interrupted(signal.SIGTERM)) == 128 + 15
    assert runner.interrupted_status(KeyboardInterrupt()) == 128 + 2
