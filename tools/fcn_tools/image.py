"""Firmware images: the format of docs/update.md ("The image format"), a second time.

Written from that text and not from lib/image, with nothing but struct, hashlib and zlib, so
that tests/python/test_image.py can hold the two implementations against each other: images
packed here are validated by the C code, images packed by the C code are verified here, and the
two must agree on every byte, every header field and every verdict.

    python -m fcn_tools.image pack --version 1.2.3 --target host [--slot-size N] PAYLOAD OUT
    python -m fcn_tools.image info IMAGE [--offset N]
    python -m fcn_tools.image verify IMAGE [--slot-size N] [--target T] [--offset N]

pack refuses a payload that is empty or does not fit into the slot (default: the host layout's
1 MiB). info prints the header's fields as JSON, with the members image_cli prints. verify
prints the verdict (ok, or the name of the first check that failed) and exits 0 for ok, 1 for a
refused image; every command exits 2 for a usage error or a file it cannot read or write.

A file given to info or verify stands for the content of a slot, from byte `--offset` of the
file on (a flash file holds several slots): the slot's bytes that lie beyond the end of the file
are taken as erased (0xFF), as they are in a slot that was erased before the image was written,
and bytes of the file beyond the slot are not read.
"""
from __future__ import annotations

import argparse
import enum
import hashlib
import json
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence

#: Bytes of the header; the payload starts at this offset of the slot.
HEADER_SIZE = 256
#: The header layout of the table in docs/update.md.
HEADER_VERSION = 1
#: The four ASCII bytes "FCNI", and the same as the little-endian word the header stores.
MAGIC_BYTES = b"FCNI"
MAGIC = int.from_bytes(MAGIC_BYTES, "little")
#: The fields of bytes 0..55, little-endian: magic, header_version, header_size, target,
#: image_version, payload_size, flags, payload_sha256.
FIELDS = struct.Struct("<IHHIIII32s")
#: Bytes 56..251 are unused and erased; bytes 252..255 hold the CRC of bytes 0..251.
UNUSED_OFFSET = FIELDS.size
CRC_OFFSET = 252
ERASED = 0xFF
#: Target identifiers by name.
TARGETS = {"host": 1, "mps2-an385": 2}
#: A slot of the host layout: 1 MiB.
HOST_SLOT_SIZE = 1 << 20
#: Bytes one validation step reads: the header takes HEADER_SIZE / CHUNK steps, the payload
#: one per started chunk.
CHUNK = 64

assert FIELDS.size == 56 and CRC_OFFSET == HEADER_SIZE - 4


class Status(enum.Enum):
    """The results of a validation, in the order of its checks (docs/update.md)."""
    OK = "ok"
    BAD_ARGUMENT = "bad_argument"
    FLASH_ERROR = "flash_error"  # a read of the flash failed: never here, files are read whole
    BAD_MAGIC = "bad_magic"
    BAD_HEADER_VERSION = "bad_header_version"
    BAD_HEADER_SIZE = "bad_header_size"
    BAD_HEADER_CRC = "bad_header_crc"
    BAD_RESERVED = "bad_reserved"
    BAD_TARGET = "bad_target"
    BAD_PAYLOAD_SIZE = "bad_payload_size"
    BAD_DIGEST = "bad_digest"


@dataclass(frozen=True)
class Header:
    """The fields of a header as numbers, the digest as 32 bytes."""
    magic: int
    header_version: int
    header_size: int
    target: int
    image_version: int
    payload_size: int
    flags: int
    payload_sha256: bytes
    header_crc: int = 0

    def as_json(self) -> dict[str, Any]:
        """The members image_cli prints for a header."""
        return {
            "magic": self.magic, "header_version": self.header_version,
            "header_size": self.header_size, "target": self.target,
            "image_version": self.image_version, "version": format_version(self.image_version),
            "payload_size": self.payload_size, "flags": self.flags,
            "payload_sha256": self.payload_sha256.hex(), "header_crc": self.header_crc,
        }


# ------------------------------------------------------------------------------------ versions

def make_version(major: int, minor: int, patch: int) -> int:
    """major << 24 | minor << 16 | patch; major and minor 0..255, patch 0..65535."""
    if not (0 <= major <= 255 and 0 <= minor <= 255 and 0 <= patch <= 65535):
        raise ValueError(f"version {major}.{minor}.{patch} out of range")
    return (major << 24) | (minor << 16) | patch


def format_version(version: int) -> str:
    return f"{version >> 24}.{(version >> 16) & 0xFF}.{version & 0xFFFF}"


def parse_version(text: str) -> int:
    """"M.m.p", three decimal numbers and nothing else."""
    parts = text.split(".")
    if len(parts) != 3 or not all(part.isdigit() and part.isascii() for part in parts):
        raise ValueError(f"not a version M.m.p: {text!r}")
    return make_version(*(int(part) for part in parts))


def parse_target(text: str) -> int:
    """A target name of TARGETS, or a number (decimal, 0x hexadecimal) for tests."""
    if text in TARGETS:
        return TARGETS[text]
    value = int(text, 0)
    if not 0 <= value <= 0xFFFFFFFF:
        raise ValueError(f"target {text} out of range")
    return value


# ------------------------------------------------------------------------------------ the header

def encode_header(header: Header) -> bytes:
    """The 256 header bytes: the fields as given, unused bytes 0xFF, and the CRC-32 of bytes
    0..251 computed here (header.header_crc is not used)."""
    body = FIELDS.pack(header.magic, header.header_version, header.header_size, header.target,
                       header.image_version, header.payload_size, header.flags,
                       header.payload_sha256)
    body += bytes([ERASED]) * (CRC_OFFSET - len(body))
    return body + struct.pack("<I", zlib.crc32(body))


def decode_header(data: bytes) -> Header:
    """The fields of the first 256 bytes of `data`, as stored."""
    if len(data) < HEADER_SIZE:
        raise ValueError(f"a header has {HEADER_SIZE} bytes, not {len(data)}")
    fields = FIELDS.unpack_from(data, 0)
    (crc,) = struct.unpack_from("<I", data, CRC_OFFSET)
    return Header(*fields, header_crc=crc)


def make_header(payload: bytes, version: int, target: int) -> Header:
    return Header(MAGIC, HEADER_VERSION, HEADER_SIZE, target, version, len(payload), 0,
                  hashlib.sha256(payload).digest())


def pack(payload: bytes, version: int, target: int, slot_size: int = HOST_SLOT_SIZE) -> bytes:
    """The image of `payload`: its header, then the payload. A payload that is empty or does
    not fit into a slot of `slot_size` bytes is refused (ValueError)."""
    if not 1 <= len(payload) <= slot_size - HEADER_SIZE:
        raise ValueError(f"the payload must be 1 to {slot_size - HEADER_SIZE} bytes, "
                         f"not {len(payload)}")
    return encode_header(make_header(payload, version, target)) + payload


# ------------------------------------------------------------------------------------ validation

@dataclass(frozen=True)
class Verdict:
    status: Status
    #: The header as read; None if the header was not read (a refused argument).
    header: Header | None

    @property
    def steps(self) -> int:
        """The steps a validation in steps takes to reach this verdict: the header in chunks,
        then the payload in chunks if the header was accepted."""
        if self.header is None:
            return 0
        steps = HEADER_SIZE // CHUNK
        if self.status in (Status.OK, Status.BAD_DIGEST):
            steps += -(-self.header.payload_size // CHUNK)
        return steps


def verify(slot: bytes, slot_size: int = HOST_SLOT_SIZE,
           target: int = TARGETS["host"]) -> Verdict:
    """Validate `slot` as the content of a slot of `slot_size` bytes (bytes beyond its end
    erased) for `target`: the checks of docs/update.md in their order, the first failing one
    is the verdict."""
    if slot_size <= HEADER_SIZE or len(slot) > slot_size:
        return Verdict(Status.BAD_ARGUMENT, None)
    head = slot[:HEADER_SIZE].ljust(HEADER_SIZE, bytes([ERASED]))
    header = decode_header(head)

    def verdict(status: Status) -> Verdict:
        return Verdict(status, header)

    if header.magic != MAGIC:
        return verdict(Status.BAD_MAGIC)
    if header.header_version != HEADER_VERSION:
        return verdict(Status.BAD_HEADER_VERSION)
    if header.header_size != HEADER_SIZE:
        return verdict(Status.BAD_HEADER_SIZE)
    if header.header_crc != zlib.crc32(head[:CRC_OFFSET]):
        return verdict(Status.BAD_HEADER_CRC)
    if header.flags != 0 or any(byte != ERASED for byte in head[UNUSED_OFFSET:CRC_OFFSET]):
        return verdict(Status.BAD_RESERVED)
    if header.target != target:
        return verdict(Status.BAD_TARGET)
    if not 1 <= header.payload_size <= slot_size - HEADER_SIZE:
        return verdict(Status.BAD_PAYLOAD_SIZE)
    end = HEADER_SIZE + header.payload_size
    payload = slot[HEADER_SIZE:end].ljust(header.payload_size, bytes([ERASED]))
    if hashlib.sha256(payload).digest() != header.payload_sha256:
        return verdict(Status.BAD_DIGEST)
    return verdict(Status.OK)


# ------------------------------------------------------------------------------------ command line

def _number(text: str) -> int:
    return int(text, 0)


def _read_slot(path: Path, offset: int, slot_size: int) -> bytes:
    """The slot_size bytes of the file from `offset` on, or as many as there are. What lies
    beyond them belongs to something else (a flash file holds more than one slot)."""
    if offset < 0:
        raise ValueError(f"offset {offset} is negative")
    with path.open("rb") as file:
        file.seek(offset)
        return file.read(slot_size)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m fcn_tools.image",
                                     description="Pack, show and verify firmware images.")
    commands = parser.add_subparsers(dest="command", required=True)

    pack_cmd = commands.add_parser("pack", help="write the image of a payload")
    pack_cmd.add_argument("--version", required=True, help="M.m.p")
    pack_cmd.add_argument("--target", required=True,
                          help=f"{' or '.join(TARGETS)}, or a number")
    pack_cmd.add_argument("--slot-size", type=_number, default=HOST_SLOT_SIZE,
                          help="refuse a payload that does not fit (default: 1 MiB)")
    pack_cmd.add_argument("payload", type=Path)
    pack_cmd.add_argument("out", type=Path)

    info_cmd = commands.add_parser("info", help="print the header of an image as JSON")
    info_cmd.add_argument("image", type=Path)
    info_cmd.add_argument("--offset", type=_number, default=0,
                          help="where the slot starts in the file (default: 0)")

    verify_cmd = commands.add_parser("verify", help="validate an image; exit 0 if valid")
    verify_cmd.add_argument("image", type=Path)
    verify_cmd.add_argument("--slot-size", type=_number, default=HOST_SLOT_SIZE,
                            help="bytes of the slot (default: 1 MiB)")
    verify_cmd.add_argument("--target", default="host", help="the target to expect")
    verify_cmd.add_argument("--offset", type=_number, default=0,
                            help="where the slot starts in the file (default: 0)")

    args = parser.parse_args(argv)
    try:
        if args.command == "pack":
            image = pack(args.payload.read_bytes(), parse_version(args.version),
                         parse_target(args.target), args.slot_size)
            args.out.write_bytes(image)
            return 0
        if args.command == "info":
            data = _read_slot(args.image, args.offset, HEADER_SIZE)
            header = decode_header(data.ljust(HEADER_SIZE, bytes([ERASED])))
            print(json.dumps(header.as_json(), indent=2))
            return 0
        result = verify(_read_slot(args.image, args.offset, args.slot_size), args.slot_size,
                        parse_target(args.target))
        print(result.status.value)
        return 0 if result.status is Status.OK else 1
    except (OSError, ValueError) as error:
        print(f"{parser.prog}: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
