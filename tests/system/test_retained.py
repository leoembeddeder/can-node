# SPDX-License-Identifier: MIT
"""The retained-RAM cell in the running node: the --retained-file option, the reset cause the
node reports for a start ("reset" in its statistics, a line of its log), and what it leaves in
the cell (platform/include/plat_retained.h).

The cell is read and written here with nothing but struct and zlib, from the layout in
plat_retained.h: the node's CRC is checked by an implementation that is not the node's. The
reset that fills the cell in the last test comes from the platform's watchdog self-test program
(tests/platform/wdg_selftest.c, `expire`), a FreeRTOS program on the same platform layer that
lets its hardware watchdog expire. The node's own watchdog resets, a hanging sensor task
reset by the node's watchdogs and the next start on the same cell, are in
tests/system/test_faults.py. The C tests of the cell are tests/platform/test_posix_retained.c
(without a kernel) and wdg_selftest `retained` (across resets of a FreeRTOS program)."""
from __future__ import annotations

import fcntl
import struct
import subprocess
import time
import zlib
from pathlib import Path

import pytest

from fcn_tools import paths
from fcn_tools.node import EXIT_OK, RESET_WATCHDOG

MAGIC = b"FCNR"                   # the magic word's bytes, little-endian
LAYOUT = struct.Struct("<4sHHII4I")  # magic, version, size, reason, resets, reserved[4]
CELL_SIZE = LAYOUT.size + 4       # and the CRC
POWER_ON = 0
REASONS = {"POWER_ON": 0, "REQUESTED": 10, "WATCHDOG": 11, "POWER_LOSS": 12, "IMAGE_FAULT": 13,
           "ASSERT": 20, "STACK_OVERFLOW": 21, "MALLOC_FAILED": 22}


def cell_bytes(reason: int, resets: int = 0, reserved=(0, 0, 0, 0), *, magic: bytes = MAGIC,
               version: int = 1, size: int = CELL_SIZE, crc: int | None = None) -> bytes:
    head = LAYOUT.pack(magic, version, size, reason, resets, *reserved)
    return head + struct.pack("<I", zlib.crc32(head) if crc is None else crc)


def read_cell(path: Path) -> dict:
    data = path.read_bytes()
    assert len(data) == CELL_SIZE
    magic, version, size, reason, resets, *reserved = LAYOUT.unpack(data[:LAYOUT.size])
    (crc,) = struct.unpack("<I", data[LAYOUT.size:])
    return {"magic": magic, "version": version, "size": size, "reason": reason,
            "resets": resets, "reserved": tuple(reserved),
            "crc_ok": crc == zlib.crc32(data[:LAYOUT.size])}


def fresh(cell: dict) -> bool:
    """A cell as after power-on: valid, nothing recorded, every word 0."""
    return cell == {"magic": MAGIC, "version": 1, "size": CELL_SIZE, "reason": POWER_ON,
                    "resets": 0, "reserved": (0, 0, 0, 0), "crc_ok": True}


def wait_until_up(node, seconds: float = 10.0) -> None:
    """Wait for the log line that the node writes once its platform is up, and so its handler
    of SIGTERM; a SIGTERM before that would end the process before it could stop in order.
    Not a timing tolerance: the wait only bounds how long the test waits."""
    deadline = time.monotonic() + seconds
    while "reset cause" not in node.log():
        assert node.running, f"the node ended with {node.wait(timeout=5)}: {node.stderr()}"
        assert time.monotonic() < deadline, "the node did not come up"
        time.sleep(0.01)


def wait_until_free(path: Path, seconds: float = 10.0) -> None:
    """Wait until no process holds the lock of the file `path`: after a kill. A node releases
    its locks itself at every end it controls, a reset and a power cut among them, so the next
    start can follow such an end at once. A killed node runs nothing: its lock goes when the
    kernel has taken its memory map down, and another process can hold that map for a moment
    after the kill has been seen (one that reads /proc/<pid>/stat does; docs/bug-log.md, 012).
    The bench that pulled the plug therefore waits here, as it would wait for a supply to be
    gone, before it switches the board on again; the node itself never waits for a file that
    is in use. The lock is tried as the node tries it and given up at once. Not a timing
    tolerance: the wait only bounds how long the test waits."""
    deadline = time.monotonic() + seconds
    with open(path, "rb") as file:
        while True:
            try:
                fcntl.flock(file, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return      # the close gives it up
            except BlockingIOError:
                assert time.monotonic() < deadline, f"{path} stayed locked after the kill"
                time.sleep(0.01)


def run_once(start_node, *args: str, name: str = "node") -> dict:
    """Start the node, stop it in an orderly way, and return its statistics' "reset"."""
    node = start_node(*args, name=name)
    wait_until_up(node)
    assert node.stop() == EXIT_OK
    return node.stats()["reset"]


def test_without_the_option_every_start_is_a_power_on(start_node):
    node = start_node()
    wait_until_up(node)
    assert node.stop() == EXIT_OK
    assert node.stats()["reset"] == {"cause": "POWER_ON", "cause_code": 0, "retained_cell": False,
                                     "verdict": "no cell", "resets_since_power_on": 0}
    assert "reset cause POWER_ON (retained cell: no cell, 0 resets since power-on)" in node.log()


def test_a_missing_cell_is_a_power_on_and_the_node_creates_a_fresh_one(start_node, workdir):
    path = workdir / "cell.bin"
    path.unlink(missing_ok=True)
    reset = run_once(start_node, "--retained-file", str(path))
    assert reset == {"cause": "POWER_ON", "cause_code": 0, "retained_cell": True,
                     "verdict": "missing", "resets_since_power_on": 0}
    assert fresh(read_cell(path))


def test_the_reason_in_a_valid_cell_is_the_cause_of_one_start(start_node, workdir):
    path = workdir / "cell.bin"
    reserved = (0x11111111, 0x22222222, 0x33333333, 0x44444444)
    path.write_bytes(cell_bytes(REASONS["WATCHDOG"], resets=4, reserved=reserved))

    node = start_node("--retained-file", str(path))
    # While it runs: the reason is spent, the count and the reserved words are kept.
    with node.sim() as sim:
        sim.ping()
    assert read_cell(path) == {"magic": MAGIC, "version": 1, "size": CELL_SIZE,
                               "reason": POWER_ON, "resets": 4, "reserved": reserved,
                               "crc_ok": True}
    assert node.stop() == EXIT_OK
    assert node.stats()["reset"] == {"cause": "WATCHDOG", "cause_code": 11, "retained_cell": True,
                                     "verdict": "reset", "resets_since_power_on": 4}
    assert "reset cause WATCHDOG (retained cell: reset, 4 resets since power-on)" in node.log()

    # The run ended without a reset (an orderly stop): the next start is a power-on.
    assert run_once(start_node, "--retained-file", str(path), name="again") == {
        "cause": "POWER_ON", "cause_code": 0, "retained_cell": True, "verdict": "no reset",
        "resets_since_power_on": 0}
    assert fresh(read_cell(path))


@pytest.mark.parametrize("reason", sorted(set(REASONS) - {"POWER_ON", "POWER_LOSS"}))
def test_every_reason_a_reset_records_is_reported_with_its_dbc_value(start_node, workdir, reason):
    path = workdir / "cell.bin"
    path.write_bytes(cell_bytes(REASONS[reason], resets=1))
    reset = run_once(start_node, "--retained-file", str(path))
    assert (reset["cause"], reset["cause_code"], reset["verdict"]) == \
        (reason, REASONS[reason], "reset")


DAMAGED = {
    "empty file": (b"", "size"),
    "short file": (cell_bytes(11)[:-1], "size"),
    "long file": (cell_bytes(11) + b"\0", "size"),
    "random bytes": (bytes(range(7, 7 + CELL_SIZE)), "magic"),
    "magic": (cell_bytes(11, magic=b"FCNr"), "magic"),
    "version": (cell_bytes(11, version=2), "version"),
    "size member": (cell_bytes(11, size=40), "version"),
    "CRC": (cell_bytes(11, crc=zlib.crc32(b"something else")), "crc"),
    "flipped bit": (cell_bytes(11, resets=1 ^ (1 << 20), crc=zlib.crc32(
        LAYOUT.pack(MAGIC, 1, CELL_SIZE, 11, 1, 0, 0, 0, 0))), "crc"),
    "reason POWER_LOSS": (cell_bytes(12), "reason"),
    "reason 99": (cell_bytes(99), "reason"),
    "reason POWER_ON": (cell_bytes(0, resets=5), "no reset"),
}


@pytest.mark.parametrize("damage", sorted(DAMAGED))
def test_a_cell_that_is_not_valid_is_a_power_on_and_is_written_afresh(start_node, workdir,
                                                                      damage):
    data, verdict = DAMAGED[damage]
    path = workdir / "cell.bin"
    path.write_bytes(data)
    assert run_once(start_node, "--retained-file", str(path)) == {
        "cause": "POWER_ON", "cause_code": 0, "retained_cell": True, "verdict": verdict,
        "resets_since_power_on": 0}
    assert fresh(read_cell(path))


def test_a_killed_node_leaves_a_power_on(start_node, workdir):
    """Pulling the plug is not a reset: the next start is a power-on."""
    path = workdir / "cell.bin"
    path.write_bytes(cell_bytes(REASONS["ASSERT"], resets=2))
    node = start_node("--retained-file", str(path))
    with node.sim() as sim:
        sim.ping()
    node.kill()
    wait_until_free(path)
    assert run_once(start_node, "--retained-file", str(path), name="after") == {
        "cause": "POWER_ON", "cause_code": 0, "retained_cell": True, "verdict": "no reset",
        "resets_since_power_on": 0}


def test_two_nodes_cannot_share_one_cell(start_node, workdir):
    path = workdir / "cell.bin"
    first = start_node("--retained-file", str(path))
    with first.sim() as sim:
        sim.ping()
    second = start_node("--retained-file", str(path), name="second")
    assert second.wait(timeout=10) == 1
    assert f"--retained-file {path} is in use by another node" in second.stderr()
    assert first.stop() == EXIT_OK


def test_a_cell_in_a_directory_that_does_not_exist_is_a_start_up_error(start_node, workdir):
    path = workdir / "no-such-directory" / "cell.bin"
    node = start_node("--retained-file", str(path))
    assert node.wait(timeout=10) == 1
    assert f"cannot write --retained-file {path}" in node.stderr()


def test_a_watchdog_reset_of_the_platform_is_the_cause_of_the_next_start(start_node, workdir):
    """The platform's self-test lets its hardware watchdog expire with this cell; the node,
    started on the same cell, reports WATCHDOG; after the node's orderly stop the cell records
    no reset any more."""
    selftest = paths.platform_test_binary("wdg_selftest")
    if not selftest.exists():
        pytest.fail(f"{selftest} not found: build it with scripts/build.sh release", pytrace=False)
    path = workdir / "cell.bin"
    path.unlink(missing_ok=True)

    run = subprocess.run([str(selftest), "expire", "200", str(path)], capture_output=True,
                         text=True, timeout=60, check=False)
    # 0: reset in time; 75: reset, but later than the self-test's tolerance. Either way the
    # watchdog reset the program, which is all this test needs.
    assert run.returncode in (0, 75), run.stdout + run.stderr
    assert '"status": 11' in run.stdout
    cell = read_cell(path)
    assert (cell["reason"], cell["resets"], cell["crc_ok"]) == (RESET_WATCHDOG, 1, True)

    assert run_once(start_node, "--retained-file", str(path)) == {
        "cause": "WATCHDOG", "cause_code": 11, "retained_cell": True, "verdict": "reset",
        "resets_since_power_on": 1}
    assert run_once(start_node, "--retained-file", str(path), name="again")["cause"] == "POWER_ON"
