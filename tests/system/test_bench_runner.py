"""The node runner of the benches (fcn_tools.runner) with the real node: a session starts it on
each transport and through the bootloader as a Board, and reads its statistics at the end; every
program reports the build id of the binaries the bench checks. The session takes its own ports
and the interface's lock, so these tests do not use the can_spec fixture (which holds that lock
itself)."""
from __future__ import annotations

import os
import sys

import pytest

from fcn_tools import bench, bus, paths, results, runner
from fcn_tools.node import EXIT_OK

TRANSPORTS = ["udp", pytest.param("socketcan", marks=pytest.mark.socketcan)]


def _usable(transport: str) -> None:
    if transport == "socketcan" and not bus.socketcan_available(runner.DEFAULT_INTERFACE):
        message = f"SocketCAN interface {runner.DEFAULT_INTERFACE} is not available"
        if os.environ.get("FCN_REQUIRE_SOCKETCAN"):
            pytest.fail(message, pytrace=False)
        pytest.skip(message)


@pytest.mark.parametrize("transport", TRANSPORTS)
def test_a_session_runs_the_node_and_reads_its_statistics(tmp_path, transport):
    _usable(transport)
    binaries = runner.prepare_binaries(build=False)
    with runner.Session(tmp_path, transport, binaries=binaries) as session:
        node = session.start_node("--duration-s", "1")
        assert node.wait(timeout=30) == EXIT_OK
        stats = session.finish(node)
    assert stats["schema"] == "fcn.node_stats/1"
    assert stats["can"]["transport"] == transport
    assert stats["build_id"] == results.binary_build_id(binaries.node)
    assert session.problems == []


def test_a_session_runs_a_board_through_the_bootloader(tmp_path, images):
    binaries = runner.prepare_binaries(build=False)
    flash = tmp_path / "flash.bin"
    bench.factory(flash, images["node-1.0.0"])
    with runner.Session(tmp_path, binaries=binaries) as session:
        board = session.start_board("--duration-s", "1", flash_file=flash,
                                    retained_file=tmp_path / "cell.bin")
        assert board.current.wait(timeout=30) == EXIT_OK
        stats = session.finish(board.current)
    assert stats["handoff"]["verdict"] == "taken" and stats["handoff"]["slot"] == "A"
    # The image is the release node, stripped; the tree it was built from is the same.
    assert stats["build_id"] == results.binary_build_id(binaries.node)
    assert "build " + stats["build_id"] in board.current.boot_log()
    assert session.problems == []


def test_the_benches_accept_the_binaries_of_this_tree(monkeypatch):
    # In-process, where the program running is pytest: stand for a bench of this checkout, the
    # script require_tree_build() checks (results.script_mismatch()).
    monkeypatch.setattr(sys, "argv", [str(paths.REPO_ROOT / "bench" / "node_timing.py")])
    binaries = runner.prepare_binaries(build=False)
    assert results.require_tree_build(binaries.as_dict()) == results.tree_build_id(
        results.provenance())
