# SPDX-License-Identifier: MIT
"""fcn_flash, the flash tool (platform/posix/flash_tool.c): what it refuses and with which exit
status (0 done, 1 a usage or start-up error, 2 refused by lib/image or lib/boot, 12 a power cut),
and what factory and stage leave, read with fcn_tools.flash. The states of the metadata are
cross-checked with the C code in tests/python/test_flash_decoder.py; the tool in the node's
updates in tests/system/test_boot.py."""
from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path

import pytest

from fcn_tools import flash as flash_decoder
from fcn_tools import image as image_format
from fcn_tools import paths


def run(*args) -> subprocess.CompletedProcess:
    tool = paths.flash_tool_binary()
    if not tool.exists():
        pytest.fail(f"{tool} not found: build it with scripts/build.sh release", pytrace=False)
    return subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True,
                          timeout=60, check=False)


def image(tmp_path: Path, name: str, size: int = 2000, version: str = "1.0.0",
          target: str = "host") -> Path:
    path = tmp_path / f"{name}.img"
    payload = hashlib.shake_256(name.encode()).digest(size)
    path.write_bytes(image_format.pack(payload, image_format.parse_version(version),
                                       image_format.TARGETS[target]))
    return path


def test_factory_makes_a_new_flash_with_slot_a_confirmed(tmp_path):
    flash = tmp_path / "flash.bin"
    done = run("factory", "--flash-file", flash, "--no-wait", image(tmp_path, "a"))
    assert done.returncode == 0, done.stderr
    out = json.loads(done.stdout)
    assert (out["status"], out["slot"], out["event"], out["image_version"]) == (
        "ok", "A", "adopt", "1.0.0")
    # A fresh file needs no erase; the image's pages and one record (two programs).
    assert out["flash"]["created"] is True and out["flash"]["erases"] == 0
    assert out["flash"]["programs"] == (256 + 2000 + 255) // 256 + 2
    described = flash_decoder.describe(flash.read_bytes())
    assert [slot["state"] for slot in described["slots"]] == ["confirmed", "empty"]
    assert described["slots"][0]["id"] == out["id"]


def test_factory_refuses_an_existing_file_and_an_image_that_does_not_validate(tmp_path):
    flash = tmp_path / "flash.bin"
    flash.write_bytes(b"")
    exists = run("factory", "--flash-file", flash, "--no-wait", image(tmp_path, "a"))
    assert exists.returncode == 1 and "exists: factory makes a new flash file" in exists.stderr
    assert flash.read_bytes() == b""

    flash.unlink()
    other = run("factory", "--flash-file", flash, "--no-wait",
                image(tmp_path, "m3", target="mps2-an385"))
    assert other.returncode == 2
    assert json.loads(other.stdout)["detail"] == "bad_target"
    assert not flash.exists()

    damaged = image(tmp_path, "damaged")
    data = bytearray(damaged.read_bytes())
    data[-1] ^= 0x01
    damaged.write_bytes(bytes(data))
    refused = run("factory", "--flash-file", flash, "--no-wait", damaged)
    assert refused.returncode == 2 and json.loads(refused.stdout)["detail"] == "bad_digest"
    assert not flash.exists()


def test_stage_refuses_what_lib_boot_refuses(tmp_path):
    flash = tmp_path / "flash.bin"
    assert run("factory", "--flash-file", flash, "--no-wait", image(tmp_path, "a")).returncode == 0
    b = image(tmp_path, "b", version="1.1.0")

    # The confirmed slot named by its address: lib/boot refuses to write the running image.
    running = run("stage", "--flash-file", flash, "--no-wait", "--address", "0x2000", b)
    assert running.returncode == 2
    assert "state at address 0x002000, writer idle" in json.loads(running.stdout)["detail"]
    # An address where no slot starts.
    nowhere = run("stage", "--flash-file", flash, "--no-wait", "--address", "0x102100", b)
    assert nowhere.returncode == 2 and "arg" in json.loads(nowhere.stdout)["detail"]

    first = run("stage", "--flash-file", flash, "--no-wait", "--chunk", "100", b)
    assert first.returncode == 0, first.stderr
    staged = json.loads(first.stdout)
    assert (staged["slot"], staged["address"], staged["writes"]) == ("B", 0x102000, 23)
    # While a trial is pending no update may begin.
    second = run("stage", "--flash-file", flash, "--no-wait", image(tmp_path, "c"))
    assert second.returncode == 2
    assert [slot["state"] for slot in flash_decoder.describe(flash.read_bytes())["slots"]] == [
        "confirmed", "trial"]


def test_usage_errors_and_a_missing_flash(tmp_path):
    assert run().returncode == 1
    assert run("nonsense").returncode == 1
    assert run("info").returncode == 1                       # no --flash-file
    missing = run("info", "--flash-file", tmp_path / "none.bin")
    assert missing.returncode == 1 and "no flash file" in missing.stderr
    assert run("stage", "--flash-file", tmp_path / "none.bin", image(tmp_path, "a")).returncode == 1
    assert run("info", "--flash-file", tmp_path / "none.bin", "extra").returncode == 1
    assert run("factory", "--flash-file", tmp_path / "f.bin", "--flash-cut", "0",
               image(tmp_path, "a")).returncode == 1
    assert run("--help").returncode == 0


def test_a_cut_ends_the_tool_as_a_loss_of_supply(tmp_path):
    flash = tmp_path / "flash.bin"
    cut = run("factory", "--flash-file", flash, "--no-wait", "--flash-cut", "3:5",
              image(tmp_path, "a"))
    assert cut.returncode == 12
    assert "FATAL flash: power cut in operation 3" in cut.stderr
    # Torn in the image's pages: no record, the factory state.
    assert flash_decoder.describe(flash.read_bytes())["metadata"]["have_record"] is False
