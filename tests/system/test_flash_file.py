# SPDX-License-Identifier: MIT
"""The simulated flash in the running node: the --flash-file and --flash-cut options, the
"flash" member of the statistics, the simulation commands flash.status and flash.cut
(platform/posix/posix_flash.h), and that the node under test fits into a slot of the layout.

These tests are about the platform, with the node started directly (without the bootloader): a
node without the option has no flash and runs as before, a node with it creates the file erased
or opens the one there, refuses one of the wrong size or one another node holds, and arms a cut
from the command line or at run time. Started that way the node has no hand-off and writes
nothing to the flash. What a cut does to an operation, and that it ends the run with status 12,
is tested where operations run: tests/platform/test_posix_flash.c and flash_selftest.c, and with
the node's own writes (its confirm) in tests/system/test_update_cuts.py."""
from __future__ import annotations

import shutil
import subprocess
import time

import pytest

from fcn_tools import paths
from fcn_tools.node import EXIT_OK
from fcn_tools.simctl import SimError

FLASH_SIZE = 2105344      # plat_flash.h: two sectors of metadata and two slots of 1 MiB
ZERO_COUNTS = {"reads": 0, "bytes_read": 0, "programs": 0, "units_programmed": 0, "erases": 0,
               "refused": 0, "busy_ns": 0, "erases_meta": 0, "erases_slot_a": 0,
               "erases_slot_b": 0, "max_sector_erases": 0}


def wait_until_up(node, seconds: float = 10.0) -> None:
    """Wait for the log line the node writes once its platform is up (and its SIGTERM handler
    is in place). Not a timing tolerance: it only bounds how long the test waits."""
    deadline = time.monotonic() + seconds
    while "CAN on " not in node.log():
        assert node.running, f"the node ended with {node.wait(timeout=5)}: {node.stderr()}"
        assert time.monotonic() < deadline, "the node did not come up"
        time.sleep(0.01)


def run_node(node_binary, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([str(node_binary), *args], capture_output=True, text=True, timeout=30)


SLOT_SIZE = 0x100000      # plat_flash.h: each slot holds one image
IMAGE_HEADER_SIZE = 256   # lib/image: the header before the payload


def test_the_release_build_of_the_node_fits_into_a_slot_as_a_payload(tmp_path):
    # plat_flash.h: a slot holds an image of the release build (and of the debug build),
    # stripped, after the image header: the payload the update tests pack (tests/system/
    # conftest.py). Since phase 5 the unstripped release build (RelWithDebInfo, its DWARF
    # included) is larger than a slot; symbols never go into an image, as a target's binary
    # has none. The AddressSanitizer build is larger than a slot even stripped and is not an
    # image (docs/update.md). Every size is printed for the record. Checked on the release
    # build whichever node the run tests (FCN_NODE), since that is the one packed.
    release = paths.build_root() / "release" / "bin" / "fcn_node"
    if not release.exists():
        pytest.fail(f"{release} not found: build it with scripts/build.sh release", pytrace=False)
    strip = shutil.which("strip")
    assert strip is not None, "strip (binutils) is needed: the images are packed stripped"
    stripped = {}
    for preset in ("release", "debug", "asan"):
        built = paths.build_root() / preset / "bin" / "fcn_node"
        if built.exists():
            out = tmp_path / f"fcn_node.{preset}.stripped"
            subprocess.run([strip, "-o", str(out), str(built)], check=True)
            stripped[preset] = out.stat().st_size
            print(f"{preset}: {built.stat().st_size} bytes, stripped {stripped[preset]}, "
                  f"{SLOT_SIZE - IMAGE_HEADER_SIZE - stripped[preset]} to spare")
    assert stripped["release"] <= SLOT_SIZE - IMAGE_HEADER_SIZE


def test_without_the_option_the_node_has_no_flash(start_node):
    node = start_node()
    wait_until_up(node)
    with node.sim() as sim:
        assert sim.flash_status() == {"flash": "off"}
        with pytest.raises(SimError, match=r"no flash in this run \(started without "
                                           r"--flash-file\)"):
            sim.flash_cut(1)
    assert node.stop() == EXIT_OK
    assert node.stats()["flash"] == {"file": False, "created": False, **ZERO_COUNTS,
                                     "cut": "none", "cut_in": 0, "cut_seed": 0}


def test_a_missing_flash_file_is_created_erased(start_node, workdir):
    flash = workdir / "flash.bin"
    node = start_node("--flash-file", str(flash))
    wait_until_up(node)
    assert "I main: flash created erased, no power cut: " in node.log()
    with node.sim() as sim:
        assert sim.flash_status() == {"flash": "on", "size": FLASH_SIZE, **ZERO_COUNTS,
                                      "cut": "none", "cut_in": 0, "cut_seed": 0}
    assert node.stop() == EXIT_OK
    assert flash.read_bytes() == b"\xff" * FLASH_SIZE
    assert node.stats()["flash"]["file"] is True
    assert node.stats()["flash"]["created"] is True


def test_a_flash_file_is_opened_as_it_is_by_the_next_run(start_node, workdir):
    flash = workdir / "flash.bin"
    content = bytes((n * 7) & 0xFF for n in range(FLASH_SIZE))
    flash.write_bytes(content)
    node = start_node("--flash-file", str(flash))
    wait_until_up(node)
    assert node.stop() == EXIT_OK
    assert "I main: flash opened, no power cut: " in node.log()
    assert node.stats()["flash"]["created"] is False
    assert flash.read_bytes() == content


@pytest.mark.parametrize("size", [0, 4096, FLASH_SIZE - 1, FLASH_SIZE + 1])
def test_a_flash_file_of_another_size_is_a_start_up_error_and_left_alone(node_binary, workdir,
                                                                        size):
    flash = workdir / "flash.bin"
    content = bytes((n * 3) & 0xFF for n in range(size))
    flash.write_bytes(content)
    result = run_node(node_binary, "--flash-file", str(flash))
    assert result.returncode == 1
    assert (f"fcn_node: --flash-file {flash} has {size} bytes, a flash file has exactly "
            f"{FLASH_SIZE} (remove it to start from an erased flash)") in result.stderr
    assert flash.read_bytes() == content


def test_a_flash_file_another_node_holds_is_a_start_up_error(start_node, node_binary, workdir):
    flash = workdir / "flash.bin"
    node = start_node("--flash-file", str(flash))
    wait_until_up(node)
    # The second node stops at the flash, before it would open a bus.
    result = run_node(node_binary, "--flash-file", str(flash))
    assert result.returncode == 1
    assert f"--flash-file {flash} is in use by another process" in result.stderr
    assert node.stop() == EXIT_OK


@pytest.mark.parametrize("args, message", [
    (("--flash-cut", "3"), "fcn_node: --flash-cut needs a flash (--flash-file)"),
    (("--flash-file", "f.bin", "--flash-cut", "0"),
     "invalid --flash-cut: a cut is N[:SEED], N from 1 to 2147483647 and SEED from 0 to "
     "4294967295, not '0'"),
    (("--flash-file", "f.bin", "--flash-cut", "4:"),
     "invalid --flash-cut: a cut is N[:SEED], N from 1 to 2147483647 and SEED from 0 to "
     "4294967295, not '4:'"),
])
def test_a_cut_that_cannot_be_armed_is_a_usage_error(node_binary, workdir, args, message):
    result = subprocess.run([str(node_binary), *args], capture_output=True, text=True,
                            timeout=30, cwd=workdir)
    assert result.returncode == 1
    assert message in result.stderr
    assert not (workdir / "f.bin").exists()


def test_a_cut_armed_on_the_command_line_or_at_run_time_waits_for_its_operation(start_node,
                                                                               workdir):
    flash = workdir / "flash.bin"
    node = start_node("--flash-file", str(flash), "--flash-cut", "2:9")
    wait_until_up(node)
    assert ("I main: flash created erased, power cut in program or erase 2, seed 9: "
            in node.log())
    with node.sim() as sim:
        status = sim.flash_status()
        assert (status["cut"], status["cut_in"], status["cut_seed"]) == ("armed", 2, 9)
        reply = sim.flash_cut(5, 77)
        assert (reply["cut"], reply["cut_in"], reply["cut_seed"]) == ("armed", "5", "77")
        status = sim.flash_status()
        assert (status["cut"], status["cut_in"], status["cut_seed"]) == ("armed", 5, 77)
        assert sim.flash_cut_off()["cut"] == "none"
        assert sim.flash_status()["cut"] == "none"
        sim.flash_cut(3, 1)
    assert node.stop() == EXIT_OK
    stats = node.stats()["flash"]
    assert (stats["cut"], stats["cut_in"], stats["cut_seed"]) == ("armed", 3, 1)
