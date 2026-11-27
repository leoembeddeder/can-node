# SPDX-License-Identifier: MIT
"""The firmware image format: fcn_tools.image on its own, and the C code of lib/image against it.

fcn_tools.image was written from docs/update.md ("The image format") with struct, hashlib and
zlib only. The first part of this file holds it to the table of that text and to values from
elsewhere (the FIPS 180-4 digest of "abc", zlib's CRC). The second part makes the two
implementations meet through tests/tools/image_cli.c (release build), which packs images with
lib/image and validates files with it in steps:

  Python -> C    images packed here, of many lengths, versions and both targets, are valid for
                 the C code, which reads the same header from them and takes the number of
                 steps the text predicts;
  C -> Python    images packed by the C code are byte for byte the images packed here, and
                 valid here;
  corruptions    one image with every single bit of its header and its payload flipped, every
                 field changed under a correct CRC, random damage, short and empty files: the
                 C code and Python give the same verdict, the same header and the same number
                 of steps for every one, all checked by one run of image_cli.
"""
from __future__ import annotations

import hashlib
import json
import os
import random
import struct
import subprocess
import sys
import zlib
from dataclasses import replace
from pathlib import Path

import pytest

from fcn_tools import image, paths
from fcn_tools.image import HEADER_SIZE, Header, Status

CLI = "image_cli"
CLI_TIMEOUT_S = 120
RANDOM_SEED = 0x1A6E0004

#: FIPS 180-4's first example (NIST, SHA-256 example "abc").
ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

# ------------------------------------------------------------------------------------------
# Part 1: fcn_tools.image against the text
# ------------------------------------------------------------------------------------------


def test_header_bytes_follow_the_table():
    """Every field at the offset and in the byte order of the table, written out by hand
    rather than through the module's struct format."""
    data = image.pack(b"abc", image.make_version(1, 2, 3), image.TARGETS["host"])
    assert len(data) == 256 + 3
    assert data[0:4] == b"FCNI"
    assert data[4:6] == bytes([1, 0])                 # header_version 1
    assert data[6:8] == bytes([0x00, 0x01])           # header_size 256
    assert data[8:12] == bytes([1, 0, 0, 0])          # target: host
    assert data[12:16] == bytes([3, 0, 2, 1])         # 1.2.3 = 0x01020003
    assert data[16:20] == bytes([3, 0, 0, 0])         # payload_size 3
    assert data[20:24] == bytes(4)                    # flags 0
    assert data[24:56].hex() == ABC_SHA256
    assert data[56:252] == b"\xff" * 196
    assert data[252:256] == zlib.crc32(data[:252]).to_bytes(4, "little")
    assert data[256:] == b"abc"


def test_magic_constant_is_fcni_little_endian():
    assert image.MAGIC == 0x494E4346
    assert struct.pack("<I", image.MAGIC) == b"FCNI"


def test_version_encoding():
    assert image.make_version(1, 2, 3) == 0x01020003
    assert image.make_version(255, 255, 65535) == 0xFFFFFFFF
    assert image.parse_version("10.20.300") == (10 << 24) | (20 << 16) | 300
    assert image.format_version(0x01020003) == "1.2.3"
    for bad in ["1.2", "1.2.3.4", "1.2.x", "256.0.0", "0.256.0", "0.0.65536", "-1.0.0",
                " 1.2.3", "1.2.3 ", "", "1..3", "+1.2.3",
                "١.٢.٣"]:  # digits that are not ASCII
        with pytest.raises(ValueError):
            image.parse_version(bad)
    rng = random.Random(RANDOM_SEED)
    for _ in range(2000):
        a = (rng.randrange(3), rng.randrange(3), rng.randrange(3) * 0x7FFF)
        b = (rng.randrange(3), rng.randrange(3), rng.randrange(3) * 0x7FFF)
        assert (a < b) == (image.make_version(*a) < image.make_version(*b))
        assert image.format_version(image.make_version(*a)) == "{}.{}.{}".format(*a)


def test_targets():
    assert image.parse_target("host") == 1
    assert image.parse_target("mps2-an385") == 2
    assert image.parse_target("0x10") == 16
    for bad in ["linux", "-1", "0x100000000"]:
        with pytest.raises(ValueError):
            image.parse_target(bad)


def test_encode_and_decode_are_inverse_for_any_fields():
    rng = random.Random(RANDOM_SEED)
    for _ in range(300):
        header = Header(rng.getrandbits(32), rng.getrandbits(16), rng.getrandbits(16),
                        rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32),
                        rng.getrandbits(32), rng.randbytes(32))
        data = image.encode_header(header)
        assert len(data) == HEADER_SIZE
        assert image.decode_header(data) == replace(header, header_crc=zlib.crc32(data[:252]))


def test_pack_refuses_payloads_that_cannot_be_images():
    with pytest.raises(ValueError):
        image.pack(b"", 0, 1)
    with pytest.raises(ValueError):
        image.pack(bytes(4096 - 255), 0, 1, slot_size=4096)
    assert len(image.pack(bytes(4096 - 256), 0, 1, slot_size=4096)) == 4096


def good(payload: bytes = b"x" * 300) -> bytes:
    return image.pack(payload, image.make_version(1, 2, 3), image.TARGETS["host"])


def with_fields(data: bytes, **fields) -> bytes:
    """`data` with header fields changed and a CRC that matches the change."""
    header = replace(image.decode_header(data), **fields)
    return image.encode_header(header) + data[HEADER_SIZE:]


def test_verify_checks_in_the_order_of_the_text():
    data = good()
    assert image.verify(data).status is Status.OK
    assert image.verify(b"").status is Status.BAD_MAGIC                 # erased slot
    assert image.verify(b"").header == image.decode_header(b"\xff" * HEADER_SIZE)
    assert image.verify(bytes(512)).status is Status.BAD_MAGIC
    assert image.verify(with_fields(data, magic=0)).status is Status.BAD_MAGIC
    assert image.verify(with_fields(data, magic=0, header_version=2)).status is Status.BAD_MAGIC
    assert image.verify(with_fields(data, header_version=2)).status is Status.BAD_HEADER_VERSION
    assert image.verify(with_fields(data, header_version=2,
                                    header_size=128)).status is Status.BAD_HEADER_VERSION
    assert image.verify(with_fields(data, header_size=128)).status is Status.BAD_HEADER_SIZE
    damaged = bytearray(data)
    damaged[100] ^= 1
    assert image.verify(bytes(damaged)).status is Status.BAD_HEADER_CRC
    # A flip in the version or the size is reported as such: they are checked before the CRC.
    for at, status in ((4, Status.BAD_HEADER_VERSION), (7, Status.BAD_HEADER_SIZE)):
        damaged = bytearray(data)
        damaged[at] ^= 0x10
        assert image.verify(bytes(damaged)).status is status
    assert image.verify(with_fields(data, flags=1)).status is Status.BAD_RESERVED
    unused = bytearray(data[:252])
    unused[60] = 0
    rewritten = bytes(unused) + zlib.crc32(bytes(unused)).to_bytes(4, "little") + data[256:]
    assert image.verify(rewritten).status is Status.BAD_RESERVED
    assert image.verify(with_fields(data, flags=1, target=2)).status is Status.BAD_RESERVED
    assert image.verify(with_fields(data, target=2)).status is Status.BAD_TARGET
    assert image.verify(with_fields(data, target=2), target=2).status is Status.OK
    assert image.verify(with_fields(data, target=2,
                                    payload_size=0)).status is Status.BAD_TARGET
    assert image.verify(with_fields(data, payload_size=0)).status is Status.BAD_PAYLOAD_SIZE
    assert image.verify(data[:-1], slot_size=256 + 299).status is Status.BAD_PAYLOAD_SIZE
    assert image.verify(data, slot_size=256 + 300).status is Status.OK
    assert image.verify(with_fields(data, payload_size=299)).status is Status.BAD_DIGEST
    assert image.verify(with_fields(data, payload_size=301)).status is Status.BAD_DIGEST
    assert image.verify(data[:-1]).status is Status.BAD_DIGEST          # last byte erased
    assert image.verify(data, slot_size=256).status is Status.BAD_ARGUMENT
    assert image.verify(data + b"\xff" * 10, slot_size=len(data)).status is Status.BAD_ARGUMENT
    # The bytes after the payload are not part of the image.
    assert image.verify(data + b"\x00" * 10).status is Status.OK


def test_steps_follow_the_text():
    assert image.verify(good(b"y" * 1)).steps == 5
    assert image.verify(good(b"y" * 64)).steps == 5
    assert image.verify(good(b"y" * 65)).steps == 6
    assert image.verify(b"").steps == 4
    assert image.verify(good(), slot_size=256).steps == 0


def test_command_line(tmp_path, capsys):
    payload = tmp_path / "payload.bin"
    payload.write_bytes(b"abc")
    out = tmp_path / "image.img"
    assert image.main(["pack", "--version", "1.2.3", "--target", "host", str(payload),
                       str(out)]) == 0
    assert out.read_bytes() == image.pack(b"abc", 0x01020003, 1)

    assert image.main(["info", str(out)]) == 0
    info = json.loads(capsys.readouterr().out)
    assert info["version"] == "1.2.3" and info["payload_size"] == 3
    assert info["payload_sha256"] == ABC_SHA256

    assert image.main(["verify", str(out)]) == 0
    assert capsys.readouterr().out == "ok\n"
    assert image.main(["verify", "--target", "mps2-an385", str(out)]) == 1
    assert capsys.readouterr().out == "bad_target\n"

    # An image inside a larger file, as in a flash file: --offset.
    flash = tmp_path / "flash.bin"
    flash.write_bytes(b"\xff" * 0x2000 + out.read_bytes() + b"\xff" * 0x3000)
    assert image.main(["verify", "--offset", "0x2000", "--slot-size", "0x2000", str(flash)]) == 0
    assert capsys.readouterr().out == "ok\n"
    assert image.main(["verify", str(flash)]) == 1
    assert capsys.readouterr().out == "bad_magic\n"
    assert image.main(["info", "--offset", "0x2000", str(flash)]) == 0
    assert json.loads(capsys.readouterr().out) == info

    empty = tmp_path / "empty.bin"
    empty.write_bytes(b"")
    assert image.main(["pack", "--version", "1.2.3", "--target", "host", str(empty),
                       str(out)]) == 2
    assert image.main(["pack", "--version", "1.2", "--target", "host", str(payload),
                       str(out)]) == 2
    assert image.main(["verify", str(tmp_path / "missing.img")]) == 2
    capsys.readouterr()


def test_module_runs_as_a_program(tmp_path):
    payload = tmp_path / "payload.bin"
    payload.write_bytes(b"\x00" * 1000)
    out = tmp_path / "image.img"
    # The fcn_tools of this test run, not whatever copy the environment would find.
    env = {**os.environ, "PYTHONPATH": str(Path(image.__file__).resolve().parents[1])}

    def run(*args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-m", "fcn_tools.image", *args], env=env,
                              capture_output=True, timeout=60, check=False)

    packed = run("pack", "--version", "0.0.1", "--target", "mps2-an385", str(payload), str(out))
    assert packed.returncode == 0, packed.stderr
    verified = run("verify", "--target", "mps2-an385", str(out))
    assert (verified.returncode, verified.stdout) == (0, b"ok\n")


# ------------------------------------------------------------------------------------------
# Part 2: lib/image (through image_cli) against fcn_tools.image
# ------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def cli() -> Path:
    binary = paths.tool_binary(CLI)
    if not binary.is_file():
        pytest.fail(f"{binary} not found: build it with scripts/build.sh release, with "
                    f"FCN_BUILD_ROOT set as for this test run (build root: {paths.build_root()})",
                    pytrace=False)
    return binary


def c_check(cli: Path, files: list[Path], slot_size: int, target: int) -> list[dict]:
    """One run of `image_cli check` over all `files`, named on standard input; its JSON lines,
    one per file, in their order."""
    names = "".join(f"{path}\n" for path in files).encode()
    try:
        run = subprocess.run([str(cli), "check", "--slot-size", str(slot_size), "--target",
                              str(target)], input=names, capture_output=True,
                             timeout=CLI_TIMEOUT_S, check=False)
    except subprocess.TimeoutExpired:
        pytest.fail(f"{cli} was not done with {len(files)} files after {CLI_TIMEOUT_S} s",
                    pytrace=False)
    lines = run.stdout.decode().splitlines()
    assert run.stderr == b""
    assert len(lines) == len(files)
    answers = [json.loads(line) for line in lines]
    assert [answer["file"] for answer in answers] == [str(path) for path in files]
    assert run.returncode == (0 if all(a["status"] == "ok" for a in answers) else 1)
    return answers


def c_pack(cli: Path, payload: Path, out: Path, version: str, target: str) -> None:
    run = subprocess.run([str(cli), "pack", "--version", version, "--target", target,
                          str(payload), str(out)], capture_output=True, timeout=60, check=False)
    assert (run.returncode, run.stderr) == (0, b"")


def expect_same_verdict(answer: dict, data: bytes, slot_size: int, target: int) -> None:
    verdict = image.verify(data, slot_size, target)
    assert answer["status"] == verdict.status.value, answer["file"]
    assert answer["header"] == (verdict.header.as_json() if verdict.header else None), \
        answer["file"]
    assert answer["steps"] == verdict.steps, answer["file"]


def payload_lengths(rng: random.Random, slot_size: int) -> list[int]:
    edges = [1, 2, 55, 56, 63, 64, 65, 127, 128, 255, 256, 257, 1000, 4096,
             slot_size - HEADER_SIZE - 1, slot_size - HEADER_SIZE]
    return edges + [rng.randrange(1, slot_size - HEADER_SIZE + 1) for _ in range(60)]


def test_images_packed_in_python_are_valid_in_c(cli, tmp_path):
    rng = random.Random(RANDOM_SEED)
    slot_size = 32768
    for target in (image.TARGETS["host"], image.TARGETS["mps2-an385"]):
        files, images = [], []
        for n, length in enumerate(payload_lengths(rng, slot_size)):
            version = image.make_version(rng.randrange(256), rng.randrange(256),
                                         rng.randrange(65536))
            data = image.pack(rng.randbytes(length), version, target, slot_size)
            path = tmp_path / f"py-{target}-{n}.img"
            path.write_bytes(data)
            files.append(path)
            images.append(data)
        for answer, data in zip(c_check(cli, files, slot_size, target), images):
            assert answer["status"] == "ok", answer
            expect_same_verdict(answer, data, slot_size, target)


def test_images_packed_in_c_equal_the_python_images(cli, tmp_path):
    rng = random.Random(RANDOM_SEED + 1)
    for n, length in enumerate(payload_lengths(rng, 8192)[:40]):
        payload = rng.randbytes(length)
        version = (rng.randrange(256), rng.randrange(256), rng.randrange(65536))
        target = rng.choice(list(image.TARGETS))
        source = tmp_path / f"payload-{n}.bin"
        source.write_bytes(payload)
        out = tmp_path / f"c-{n}.img"
        c_pack(cli, source, out, "{}.{}.{}".format(*version), target)
        expected = image.pack(payload, image.make_version(*version), image.TARGETS[target])
        assert out.read_bytes() == expected
        assert image.verify(out.read_bytes(), target=image.TARGETS[target]).status is Status.OK


def corrupted_images(rng: random.Random) -> list[bytes]:
    base = image.pack(rng.randbytes(300), image.make_version(2, 0, 7), image.TARGETS["host"],
                      slot_size=4096)
    cases = []
    for bit in range(len(base) * 8):                    # every single bit
        damaged = bytearray(base)
        damaged[bit // 8] ^= 1 << (bit % 8)
        cases.append(bytes(damaged))
    header = image.decode_header(base)
    changes = [dict(magic=0), dict(magic=0xFFFFFFFF), dict(magic=0x46434E49),
               dict(header_version=0), dict(header_version=2), dict(header_size=255),
               dict(header_size=512), dict(flags=1), dict(flags=0x80000000), dict(target=0),
               dict(target=2), dict(target=0xFFFFFFFF), dict(payload_size=0),
               dict(payload_size=299), dict(payload_size=301), dict(payload_size=4096 - 256),
               dict(payload_size=4096 - 255), dict(payload_size=0xFFFFFFFF),
               dict(payload_sha256=bytes(32)), dict(magic=0, header_version=7),
               dict(header_size=0, flags=3), dict(flags=1, target=0),
               dict(target=0, payload_size=0)]
    for change in changes:                             # fields changed under a correct CRC
        cases.append(image.encode_header(replace(header, **change)) + base[HEADER_SIZE:])
    for at in (56, 100, 251):                          # an unused byte, under a correct CRC
        body = bytearray(base[:252])
        body[at] = 0x00
        cases.append(bytes(body) + zlib.crc32(bytes(body)).to_bytes(4, "little")
                     + base[HEADER_SIZE:])
    for _ in range(300):                               # random damage
        damaged = bytearray(base)
        for _ in range(rng.randrange(1, 6)):
            damaged[rng.randrange(len(damaged))] = rng.randrange(256)
        cases.append(bytes(damaged))
    for length in (0, 1, 63, 64, 255, 256, 257, len(base) - 1):   # short files: erased rest
        cases.append(base[:length])
    cases.append(base + rng.randbytes(64))             # whatever follows the payload
    cases.append(bytes(4096))                          # a zeroed slot
    cases.append(b"\xff" * 4096)                       # an erased slot
    return cases


def test_corrupted_images_get_the_same_verdict_in_c_and_python(cli, tmp_path):
    rng = random.Random(RANDOM_SEED + 2)
    cases = corrupted_images(rng)
    files = []
    for n, data in enumerate(cases):
        path = tmp_path / f"case-{n}.img"
        path.write_bytes(data)
        files.append(path)
    answers = c_check(cli, files, 4096, image.TARGETS["host"])
    for answer, data in zip(answers, cases):
        expect_same_verdict(answer, data, 4096, image.TARGETS["host"])
    seen = {answer["status"] for answer in answers}
    assert seen == {s.value for s in Status} - {"flash_error", "bad_argument"}
    # Every single flipped bit was refused (the first 556 * 8 cases).
    assert all(answer["status"] != "ok" for answer in answers[:(256 + 300) * 8])


def test_c_command_line_errors(cli, tmp_path):
    payload = tmp_path / "payload.bin"
    payload.write_bytes(b"\x01" * 10)
    empty = tmp_path / "empty.bin"
    empty.write_bytes(b"")
    out = tmp_path / "out.img"

    def status(*args: str) -> int:
        return subprocess.run([str(cli), *args], capture_output=True, timeout=60,
                              check=False).returncode

    assert status() == 2
    assert status("pack", "--version", "1.2.3", "--target", "host", str(empty), str(out)) == 2
    assert status("pack", "--version", "1.2", "--target", "host", str(payload), str(out)) == 2
    assert status("pack", "--version", "1.2.3", "--target", "linux", str(payload), str(out)) == 2
    assert status("pack", "--version", "1.2.3", "--target", "host", "--slot-size", "265",
                  str(payload), str(out)) == 2
    assert status("pack", "--version", "1.2.3", "--target", "host", "--slot-size", "266",
                  str(payload), str(out)) == 0
    assert status("check", "--slot-size", "256", str(out)) == 2
    long_file = tmp_path / "long.img"
    long_file.write_bytes(out.read_bytes() + b"\xff")
    run = subprocess.run([str(cli), "check", "--slot-size", "266", str(out), str(long_file),
                          str(tmp_path / "missing.img")], capture_output=True, timeout=60,
                         check=False)
    lines = [json.loads(line) for line in run.stdout.decode().splitlines()]
    assert run.returncode == 2
    assert [line["status"] for line in lines] == ["ok", "too_long", "io_error"]


def test_known_header_digest_in_c(cli, tmp_path):
    """The C code's SHA-256 of "abc" in the header it packs is FIPS 180-4's."""
    payload = tmp_path / "abc.bin"
    payload.write_bytes(b"abc")
    out = tmp_path / "abc.img"
    c_pack(cli, payload, out, "1.2.3", "host")
    assert out.read_bytes()[24:56].hex() == ABC_SHA256 == hashlib.sha256(b"abc").hexdigest()
