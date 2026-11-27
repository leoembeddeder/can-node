# SPDX-License-Identifier: MIT
"""fcn_tools.flash, the second reading of a flash file, against `fcn_flash info` (the C code:
lib/boot's boot_describe(), lib/image, lib/bootimg's id), on the states the C programs leave:
a factory flash, updates staged, attempts counted by the bootloader, rollbacks, the log
running over into the second metadata sector and round the ring, and flash files torn by a
power cut at every program and erase of an update and of a boot. They must agree on every
member; where they do not, one of them does not do what docs/update.md says.

The flash tool and the bootloader run with --no-wait or without waits that matter (the
bootloader's attempt is one program). The images are small and not programs: the metadata do
not care, and a bootloader that tries to start one ends with FCN_BOOT_EXIT_JUMP (42) after it
has written what it decided, which is all these tests need from it.
"""
from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path

import pytest

from fcn_tools import flash as flash_decoder
from fcn_tools import image as image_format
from fcn_tools import paths
from fcn_tools.node import BOOT_JUMP_FAILED, RESET_POWER_LOSS

POWER_LOSS = RESET_POWER_LOSS


def binary(path: Path) -> Path:
    if not path.exists():
        pytest.fail(f"{path} not found: build it with scripts/build.sh release", pytrace=False)
    return path


def tool(*args, expect=(0,)) -> subprocess.CompletedProcess:
    run = subprocess.run([str(binary(paths.flash_tool_binary())), *map(str, args)],
                         capture_output=True, text=True, timeout=60, check=False)
    assert run.returncode in expect, f"fcn_flash {args}: {run.returncode}\n{run.stdout}{run.stderr}"
    return run


def boot(flash: Path, *args, expect=(BOOT_JUMP_FAILED,)) -> int:
    run = subprocess.run([str(binary(paths.boot_binary())), "--flash-file", str(flash),
                          "--log-level", "error", *map(str, args)],
                         capture_output=True, text=True, timeout=60, check=False)
    assert run.returncode in expect, f"fcn_boot {args}: {run.returncode}\n{run.stderr}"
    return run.returncode


def agree(flash: Path) -> dict:
    """Both readings of `flash`, compared member by member; the C one is returned."""
    reported = json.loads(tool("info", "--flash-file", flash).stdout)
    reported.pop("flash")
    decoded = flash_decoder.describe(flash.read_bytes())
    assert decoded == reported, (f"they disagree about {flash}:\n"
                                 f"fcn_tools.flash {json.dumps(decoded, indent=1)}\n"
                                 f"fcn_flash info  {json.dumps(reported, indent=1)}")
    return reported


@pytest.fixture(scope="module")
def images(tmp_path_factory) -> dict[str, Path]:
    out = tmp_path_factory.mktemp("images")
    made = {}
    for name, size, version in (("a", 5000, "1.0.0"), ("b", 3000, "1.1.0"), ("c", 700, "2.0.1")):
        payload = hashlib.shake_256(name.encode()).digest(size)
        path = out / f"{name}.img"
        path.write_bytes(image_format.pack(payload, image_format.parse_version(version),
                                           image_format.TARGETS["host"]))
        made[name] = path
    return made


# ------------------------------------------------------------------ the text, piece by piece


def test_the_entry_sizes_are_those_of_the_text():
    # The host: F 8, the record 96, the entry 96 + 4 x 8 = 128, a stride of 128 (a power of
    # two in a page), 32 entries in a sector of 4 KiB.
    assert flash_decoder.entry_sizes(8, 256) == (8, 96, 128, 128)
    # Units of 16 bytes in pages of 64: F 16, entry 96 + 64 = 160 > 64, so whole pages: 192.
    assert flash_decoder.entry_sizes(16, 64) == (16, 96, 160, 192)
    # A unit of 32: the record is whole units already, F 32, entry 224 within a page of 256.
    assert flash_decoder.entry_sizes(32, 256) == (32, 96, 224, 256)


def test_the_commit_field_counts_the_zero_bits_and_inverts_them():
    body = bytes([0x00] * 2 + [0xFF] * 94)        # 16 zero bits
    assert flash_decoder.commit_field(body, 8) == bytes([16, 0, 0, 0, 0xEF, 0xFF, 0xFF, 0xFF])
    assert flash_decoder.commit_field(body, 16)[8:] == bytes([0xFF] * 8)


def test_an_entry_counts_only_with_its_exact_commit_field_and_crc(tmp_path, images):
    flash = tmp_path / "flash.bin"
    tool("factory", "--flash-file", flash, "--no-wait", images["a"])
    data = bytearray(flash.read_bytes())
    entry = bytes(data[:128])
    assert flash_decoder.decode_entry(entry).event == "adopt"
    # Any bit of the record or of the commit field from 1 to 0, as a torn write leaves it, and
    # one bit of the CRC from 0 to 1: no record.
    for at in (0, 5, 50, 95, 96, 100):
        byte = entry[at]
        bit = next((1 << n for n in range(8) if byte & (1 << n)), None)
        if bit is not None:
            damaged = bytearray(entry)
            damaged[at] &= ~bit
            assert flash_decoder.decode_entry(bytes(damaged)) is None, at
    crc_byte = next(i for i in range(92, 96) if entry[i] != 0xFF)
    damaged = bytearray(entry)
    damaged[crc_byte] ^= 0x01
    assert flash_decoder.decode_entry(bytes(damaged)) is None


# ---------------------------------------------------------------- both readings, many states


def test_both_readings_agree_through_updates_rollbacks_and_round_the_ring(tmp_path, images):
    """Factory, then updates that fail (three attempts each, then the rollback) until the log has
    gone round both metadata sectors, with a confirm-free cycle of BEGIN, COMMIT and ROLLBACK
    records; every state on the way is read both ways."""
    flash = tmp_path / "flash.bin"
    tool("factory", "--flash-file", flash, "--no-wait", images["a"])
    seen_sectors = set()
    first = agree(flash)
    assert first["metadata"]["seq"] == 1
    for cycle in range(23):                       # 1 + 23 x 3 = 70 records: past 64 entries
        tool("stage", "--flash-file", flash, "--no-wait", images["b" if cycle % 2 else "c"])
        staged = agree(flash)
        assert staged["metadata"]["trial_slot"] == "B"
        for attempt in (1, 2, 3):
            boot(flash)
            assert agree(flash)["metadata"]["attempts_used"] == attempt
        boot(flash)                               # the rollback, then slot A is started
        rolled = agree(flash)
        assert rolled["metadata"]["event"] == "rollback"
        assert [slot["state"] for slot in rolled["slots"]] == ["confirmed", "rejected"]
        seen_sectors.add(rolled["metadata"]["record_sector"])
    assert rolled["metadata"]["seq"] == 1 + 23 * 3
    assert seen_sectors == {0, 1}
    assert rolled["metadata"]["entries_used"] < 64   # an erase made room in the ring


def stage_operations(tmp_path: Path, images: dict[str, Path]) -> tuple[Path, int]:
    """A factory flash with image a, and the number of programs and erases the update with
    image b makes on it."""
    base = tmp_path / "base.bin"
    tool("factory", "--flash-file", base, "--no-wait", images["a"])
    trial = tmp_path / "count.bin"
    trial.write_bytes(base.read_bytes())
    counts = json.loads(tool("stage", "--flash-file", trial, "--no-wait",
                             images["b"]).stdout)["flash"]
    return base, counts["programs"] + counts["erases"]


@pytest.mark.parametrize("seed", [0, 1, 7])
def test_both_readings_agree_after_a_cut_at_every_operation_of_an_update(tmp_path, images, seed):
    base, operations = stage_operations(tmp_path, images)
    assert operations >= 15
    for n in range(1, operations + 1):
        flash = tmp_path / f"cut-{n}.bin"
        flash.write_bytes(base.read_bytes())
        tool("stage", "--flash-file", flash, "--no-wait", "--flash-cut", f"{n}:{seed}",
             images["b"], expect=(POWER_LOSS,))
        torn = agree(flash)
        # Whatever the cut left, the factory image is still the confirmed one.
        assert torn["metadata"]["active_slot"] == "A"
        assert torn["slots"][0]["id_matches_record"]


@pytest.mark.parametrize("seed", [0, 3])
def test_both_readings_agree_after_a_cut_in_the_attempt_and_in_the_rollback(tmp_path, images,
                                                                          seed):
    flash = tmp_path / "flash.bin"
    tool("factory", "--flash-file", flash, "--no-wait", images["a"])
    tool("stage", "--flash-file", flash, "--no-wait", images["b"])
    # The attempt: the bootloader's first program.
    boot(flash, "--flash-cut", f"1:{seed}", expect=(POWER_LOSS,))
    after_cut = agree(flash)
    assert after_cut["metadata"]["attempts_used"] in (0, 1)
    while agree(flash)["metadata"]["attempts_used"] < 3:
        boot(flash)
    # The rollback's record (its first and its second program).
    for n in (1, 2):
        copy = tmp_path / f"rollback-{n}.bin"
        copy.write_bytes(flash.read_bytes())
        status = boot(copy, "--flash-cut", f"{n}:{seed}", expect=(POWER_LOSS,))
        assert status == POWER_LOSS
        torn = agree(copy)
        assert torn["metadata"]["trial_slot"] in ("B", "none")


def test_an_image_rewritten_behind_the_metadata_is_not_the_recorded_one(tmp_path, images):
    """Slot A rewritten with another valid image without lib/boot (a bench, a tool): both
    readings say it validates and that its id is not the one the record names, and the
    bootloader starts nothing."""
    flash = tmp_path / "flash.bin"
    tool("factory", "--flash-file", flash, "--no-wait", images["a"])
    data = bytearray(flash.read_bytes())
    other = images["b"].read_bytes()
    data[0x2000:0x2000 + len(other)] = other
    flash.write_bytes(bytes(data))
    rewritten = agree(flash)
    slot = rewritten["slots"][0]
    assert (slot["state"], slot["validation"], slot["id_matches_record"]) == (
        "confirmed", "ok", False)
    assert slot["id"] != slot["recorded_id"]
    boot(flash, expect=(40,))
