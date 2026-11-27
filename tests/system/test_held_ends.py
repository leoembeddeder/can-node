# SPDX-License-Identifier: MIT
"""The ends of the programs that had the flash file or the retained cell open, with the ended
program's memory map held alive after its end (docs/bug-log.md, 012).

A flock lock belongs to the open file description, and a shared mapping of the file refers to
that description as the descriptor does. A program that leaves its locks to its exit therefore
holds them for as long as its memory map exists, and another process can hold that map for a
moment after the exit: the kernel takes a reference for each read of /proc/<pid>/stat or
/proc/<pid>/maps. The bench starts the next program as soon as it has seen the exit, fcn_flash
info after a power cut, fcn_boot after a reset, and that program then found the file "in use"
now and then, on a host where something read those files (CI run 45128453578).

Here the memory map is held every time. The programs under test are the held variants of
tests/tools/CMakeLists.txt (fcn_flash_held, fcn_boot_held, fcn_node_held and
fcn_payload_crash_held): the real programs' sources with the held end of tests/tools/exit_held.c
linked in. Such a program's end starts a process, the holder, which shares the memory map and
keeps it until the test ends it. The test learns the holder's process id from a FIFO (the `held`
fixture), starts the next program at once, and checks afterwards that the holder was there all
the while. A program that left a lock to its exit fails its test every time, and no verdict
depends on the host's timing.

The ends: a power cut in the flash tool, in the bootloader and in the node's flash task; a reset
of the node by its hardware watchdog (plat_reset() from the watchdog's thread) and one requested
by a tester (from a task); the crash payload's end, and its end at a start-up error. Then the
ends that close the files in order, which pass whether or not an _exit() releases anything and
fail for an end that forgot its close: the bootloader's exit statuses 40 and 42 and the node's
orderly end. The shutdown guard's end (status 70) is one that nothing outside the node can
provoke; it is tested where a program arms the guard itself, with both files open and a task
that programs the flash meanwhile: tests/platform/flash_selftest.c, end-while-writing.
"""
from __future__ import annotations

import os
import select
import signal
import subprocess
from pathlib import Path

import pytest

from fcn_tools import paths
from fcn_tools.bench import pack_image, stripped_copy
from fcn_tools.node import (BOOT_JUMP_FAILED, BOOT_NOTHING_BOOTABLE, EXIT_OK, RESET_IMAGE_FAULT,
                            RESET_POWER_LOSS, RESET_REQUESTED, RESET_WATCHDOG)
from fcn_tools.tester import Uds, start_ready

from update_bench import RUN_WAIT_S, factory, info, stage, states

#: How long the end of a test waits for a holder it killed to be gone: a bound, not a tolerance.
HOLDER_WAIT_S = 10.0


class Held:
    """The test's side of the held ends (tests/tools/exit_held.c). FCN_HELD_EXIT names the FIFO
    `fifo` to every program the test starts; a held variant that ends writes its holder's
    process id into it. holding() takes the ids that came and checks the holders; release()
    ends them."""

    def __init__(self, directory: Path) -> None:
        self.fifo = directory / "held.fifo"
        self.fifo.unlink(missing_ok=True)
        os.mkfifo(self.fifo)
        # Without waiting for a writer; a read finds what the ends wrote so far.
        self._fifo = os.open(self.fifo, os.O_RDONLY | os.O_NONBLOCK)
        self._text = b""
        #: The holders: process id and a descriptor of the process (pidfd_open), which names
        #: that process and no later one with the same id.
        self._holders: list[tuple[int, int]] = []

    def _collect(self) -> None:
        while True:
            try:
                data = os.read(self._fifo, 4096)
            except BlockingIOError:     # nothing written, and a holder has the FIFO open
                break
            if not data:                # nothing written, and nobody has it open for writing
                break
            self._text += data
        *lines, self._text = self._text.split(b"\n")
        for line in lines:
            pid = int(line)
            self._holders.append((pid, os.pidfd_open(pid)))

    @staticmethod
    def _ended(holder: int) -> bool:
        return bool(select.select([holder], [], [], 0)[0])

    def holding(self, count: int) -> None:
        """`count` holders were started so far, one by each held program that has ended, and
        every one of them still runs: the memory maps of those programs still exist."""
        self._collect()
        pids = [pid for pid, _ in self._holders]
        assert len(pids) == count, f"{len(pids)} holders were started, not {count}: {pids}"
        gone = [pid for pid, holder in self._holders if self._ended(holder)]
        assert not gone, f"the holders {gone} ended before the test let them go"

    def release(self) -> None:
        """End every holder (SIGKILL) and wait until each is gone."""
        self._collect()
        for _, holder in self._holders:
            try:
                signal.pidfd_send_signal(holder, signal.SIGKILL)
            except ProcessLookupError:
                pass                    # ended by itself
        late = [pid for pid, holder in self._holders
                if not select.select([holder], [], [], HOLDER_WAIT_S)[0]]
        for _, holder in self._holders:
            os.close(holder)
        os.close(self._fifo)
        assert not late, f"the holders {late} had not ended {HOLDER_WAIT_S} s after SIGKILL"


@pytest.fixture
def held(workdir, monkeypatch) -> Held:
    """The held ends of this test. Ask for it before start_node: the nodes that the end of the
    test stops start holders too, and this fixture, set up first, ends them last."""
    hold = Held(workdir)
    monkeypatch.setenv("FCN_HELD_EXIT", str(hold.fifo))
    yield hold
    hold.release()


def held_program(name: str) -> Path:
    """The held variant `name` of the build whose bootloader and flash tool run (the asan build
    in the asan run of scripts/test-system.sh)."""
    program = paths.flash_tool_binary().with_name(name)
    if not program.exists():
        pytest.fail(f"{program} not found: build it with scripts/build.sh", pytrace=False)
    return program


@pytest.fixture(scope="module")
def held_images(tmp_path_factory) -> dict[str, Path]:
    """Images with held ends, packed from the release build as the `images` fixture packs the
    real ones (an image of the asan node does not fit a slot): "node-1.0.0" and "node-1.1.0",
    fcn_node_held, stripped; "crash" (1.2.0), fcn_payload_crash_held."""
    release = paths.build_root() / "release" / "bin"
    node, crash = release / "fcn_node_held", release / "fcn_payload_crash_held"
    for binary in (node, crash):
        if not binary.exists():
            pytest.fail(f"{binary} not found: build it with scripts/build.sh release",
                        pytrace=False)
    out = tmp_path_factory.mktemp("held-images")
    stripped = stripped_copy(node, out / "fcn_node_held.stripped")
    made = {}
    for name, payload, version in (("node-1.0.0", stripped, "1.0.0"),
                                   ("node-1.1.0", stripped, "1.1.0"), ("crash", crash, "1.2.0")):
        made[name] = out / f"{name}.img"
        made[name].write_bytes(pack_image(payload.read_bytes(), version))
    return made


# ----------------------------------------------------------------------------- the flash tool


@pytest.mark.parametrize("cut", [3, 10])
def test_the_flash_is_free_at_once_after_a_power_cut_in_the_flash_tool(held, workdir, images,
                                                                      cut):
    """fcn_flash stage --flash-cut ends with 12, and fcn_flash info reads the flash at once: the
    two calls of the tests that failed on CI (cut-3.bin and cut-10.bin of test_flash_decoder.py).
    The cut tool's memory map, with its mapping of the flash file, exists all the while."""
    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    run = subprocess.run(
        [str(held_program("fcn_flash_held")), "stage", "--flash-file", str(flash), "--no-wait",
         "--flash-cut", f"{cut}:7", str(images["node-1.1.0"])],
        capture_output=True, text=True, timeout=120, check=False)
    assert run.returncode == RESET_POWER_LOSS, run.stderr
    assert f"FATAL flash: power cut in operation {cut} (" in run.stderr
    held.holding(1)

    torn = info(flash)          # fcn_flash info, and fcn_tools.flash reading the same
    assert states(torn)[0] == "confirmed"
    held.holding(1)


# ----------------------------------------------------------------------------------- the node


@pytest.mark.parametrize("reset", ["watchdog", "requested", "power cut"])
def test_the_board_starts_again_at_once_after_a_reset_of_the_node(held, start_node, restart_node,
                                                                 workdir, ports, held_images,
                                                                 reset):
    """The node ends in plat_reset(): by its hardware watchdog (sensor.hang; the reset comes from
    the watchdog's thread, outside the scheduler), by a tester's ECUReset (from the diagnostic
    task), and by a cut of the flash's supply in the confirm of a trial (from the flash task, as
    PLAT_RESET_POWER_LOSS). At once fcn_flash info reads the flash and fcn_boot starts the board
    again, as a board starts again after a reset: both open the files the node had locked, while
    the node's memory map still exists. The next run reports the cause the reset left."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    spec = f"udp:{ports.bus}"
    factory(flash, held_images["node-1.0.0"])

    if reset == "watchdog":
        first = start_ready(start_node, spec, flash_file=flash, retained_file=cell)
        with first.sim() as sim:
            sim.sensor_hang("on")
        assert first.wait_for_reset(timeout=RUN_WAIT_S) == RESET_WATCHDOG
        cause = "WATCHDOG"
    elif reset == "requested":
        first = start_ready(start_node, spec, flash_file=flash, retained_file=cell)
        with Uds(spec) as tester:
            tester.send("11 81")        # ECUReset without an answer: the reset at once
            assert first.wait_for_reset(timeout=RUN_WAIT_S) == RESET_REQUESTED
        cause = "REQUESTED"
    else:
        # A trial that confirms itself after two health periods: the first write of its run
        # is the flash task's, the confirm, and the cut is in it. The cell goes with the supply.
        stage(flash, held_images["node-1.1.0"])
        first = start_node("--confirm-periods", "2", "--flash-cut", "1:5", flash_file=flash,
                           retained_file=cell)
        assert first.wait_for_reset(timeout=RUN_WAIT_S) == RESET_POWER_LOSS
        assert "writing its confirm" in first.log()
        cause = "POWER_ON"
    held.holding(1)

    info(flash)
    again = restart_node(first, args=("--duration-s", "1"))
    assert again.wait(timeout=RUN_WAIT_S) == EXIT_OK, again.stderr()
    assert again.stats()["reset"]["cause"] == cause
    held.holding(2)             # the first run's holder, and the second run's


# -------------------------------------------------------------------------------- the payload


def test_the_board_starts_again_at_once_after_the_crash_payload_ended(held, start_node,
                                                                     restart_node, workdir,
                                                                     held_images):
    """The crash payload records its reset in the cell and ends with 13; fcn_boot, started again
    at once, opens the cell the payload had locked and finds the reason."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, held_images["crash"])
    first = start_node(flash_file=flash, retained_file=cell)
    assert first.wait_for_reset(timeout=RUN_WAIT_S) == RESET_IMAGE_FAULT
    held.holding(1)

    again = restart_node(first)
    assert again.wait_for_reset(timeout=RUN_WAIT_S) == RESET_IMAGE_FAULT
    assert ("reset cause IMAGE_FAULT (retained cell: reset, 1 resets since power-on)"
            in again.boot_log())
    held.holding(2)


def test_the_cell_is_free_at_once_after_the_payload_ended_at_a_start_up_error(held, start_node,
                                                                             workdir,
                                                                             held_images):
    """A payload that cannot open its log has opened the cell before (it takes the hand-off all
    the same) and ends with 1, by a return from main(): it closes the cell first."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, held_images["crash"])
    missing = workdir / "no-such-directory" / "payload.log"
    first = start_node("--log-file", str(missing), flash_file=flash, retained_file=cell)
    assert first.wait(timeout=RUN_WAIT_S) == 1
    assert "payload crash: cannot open --log-file" in first.stderr()
    held.holding(1)

    again = start_node(flash_file=flash, retained_file=cell, name="again")
    assert again.wait_for_reset(timeout=RUN_WAIT_S) == RESET_IMAGE_FAULT
    held.holding(2)


# ----------------------------------------------------------------------------- the bootloader


def test_the_board_starts_at_once_after_a_power_cut_in_the_bootloader(held, start_node,
                                                                      workdir, images):
    """fcn_boot loses its supply in its first write, the attempt it counts before it starts a
    trial, and ends with 12 (the end without a kernel). At once fcn_flash info reads the flash
    and fcn_boot starts the board: the trial runs, as its second attempt."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, images["node-1.0.0"])
    stage(flash, images["node-1.1.0"])
    first = start_node(flash_file=flash, retained_file=cell,
                       boot_binary=held_program("fcn_boot_held"),
                       boot_args=("--flash-cut", "1:3"))
    assert first.wait_for_reset(timeout=RUN_WAIT_S) == RESET_POWER_LOSS
    assert "FATAL flash: power cut in operation 1 (" in first.stderr()
    held.holding(1)

    info(flash)
    again = start_node("--duration-s", "1", flash_file=flash, retained_file=cell, name="again")
    assert again.wait(timeout=RUN_WAIT_S) == EXIT_OK, again.stderr()
    assert "select: slot B as a trial, attempt 2 of 3, image 1.1.0" in again.boot_log()
    held.holding(1)


# ------------------------------------------------------------------- the ends that close first


@pytest.mark.parametrize("end", ["nothing bootable", "image not started"])
def test_the_files_are_free_at_once_after_the_bootloader_ended_in_order(held, start_node,
                                                                        workdir, images, end):
    """fcn_boot's own exit statuses, which it returns from main() after it closed the flash and
    the cell: 40 for an erased flash, and 42 for a trial that is no program, after it had
    released both locks for the exec and taken the cell's back. The next start through fcn_boot
    opens both files at once and ends the same way."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    status = BOOT_NOTHING_BOOTABLE          # a flash file that is missing is created erased
    if end == "image not started":
        factory(flash, images["node-1.0.0"])
        stage(flash, images["data"])
        status = BOOT_JUMP_FAILED
    first = start_node(flash_file=flash, retained_file=cell,
                       boot_binary=held_program("fcn_boot_held"))
    assert first.wait(timeout=RUN_WAIT_S) == status, first.stderr()
    held.holding(1)

    info(flash)
    again = start_node(flash_file=flash, retained_file=cell, name="again")
    assert again.wait(timeout=RUN_WAIT_S) == status, again.stderr()
    held.holding(1)


def test_the_board_starts_again_at_once_after_the_node_ended_in_order(held, start_node,
                                                                      restart_node, workdir,
                                                                      held_images):
    """The node's orderly end (--duration-s) closes the flash and the cell before it writes its
    statistics and returns from main()."""
    flash, cell = workdir / "flash.bin", workdir / "cell.bin"
    factory(flash, held_images["node-1.0.0"])
    first = start_node("--duration-s", "1", flash_file=flash, retained_file=cell)
    assert first.wait(timeout=RUN_WAIT_S) == EXIT_OK, first.stderr()
    held.holding(1)

    info(flash)
    again = restart_node(first)
    assert again.wait(timeout=RUN_WAIT_S) == EXIT_OK, again.stderr()
    assert again.stats()["reset"]["cause"] == "POWER_ON"
    held.holding(2)
