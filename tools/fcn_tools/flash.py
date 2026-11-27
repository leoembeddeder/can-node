# SPDX-License-Identifier: MIT
"""The flash file of the host build, read from outside: its layout, lib/boot's metadata log and
the images in the two slots.

A second implementation, written from the text of docs/update.md (the flash, the image format,
boot control: the entry, the record, the commit field, which record counts, writing a record)
with nothing but the standard library, so that a test can hold `fcn_flash info` against it:
where the C code and this file disagree about a flash file, one of them does not do what the
text says. The images are read with fcn_tools.image, the image format's own second
implementation.

    python -m fcn_tools.flash info FLASH_FILE     # JSON as `fcn_flash info` writes it

describe() returns the same members as `fcn_flash info` (schema fcn.flash_info/1) but for
"flash", the tool's operation counts, which a reader of the file cannot know.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from . import image as image_format

# ------------------------------------------------------------------------------ the layout

#: The host's flash (plat_flash.h): geometry and areas, in bytes.
SIZE = 0x202000
SECTOR_SIZE = 4096
PAGE_SIZE = 256
UNIT_SIZE = 8
META_OFFSET = 0x000000
META_SIZE = 0x002000
SLOTS = ((0x002000, 0x100000), (0x102000, 0x100000))
SLOT_NAMES = ("A", "B")
ERASED = 0xFF

# ------------------------------------------------------------------------- the metadata log

#: The record before its padding, its magic ("FCNB"), its format version.
RECORD_SIZE = 96
RECORD_MAGIC = b"FCNB"
RECORD_VERSION = 1
#: The attempts a trial has.
MAX_ATTEMPTS = 3
#: Sequence numbers: 1 to 0xFFFFFFFE.
SEQ_FIRST, SEQ_LAST = 1, 0xFFFFFFFE
#: The CRC covers bytes 0 to 91 and is the word at 92.
CRC_OFFSET = 92

EVENTS = {1: "adopt", 2: "begin", 3: "commit", 4: "confirm", 5: "rollback", 6: "reject",
          7: "fallback"}
STATES = {0: "empty", 1: "writing", 2: "trial", 3: "confirmed", 4: "previous", 5: "rejected"}
#: The states that name no image: their version is 0xFFFFFFFF and their id 0xFF bytes.
WITHOUT_IMAGE = ("empty", "writing")

#: One slot of a record: state (1), 3 reserved bytes 0xFF, version (4), id (32).
SLOT_RECORD = struct.Struct("<B3sI32s")
SLOTS_OFFSET = 12


def _round_up(value: int, to: int) -> int:
    return -(-value // to) * to


def entry_sizes(unit: int = UNIT_SIZE, page: int = PAGE_SIZE) -> tuple[int, int, int, int]:
    """The field size F = max(8, unit), the record body (96 rounded up to whole units), the
    entry (body, commit field, three attempt fields) and the stride from one entry to the next:
    the entry rounded up to a power of two when that fits in a page, otherwise to whole pages."""
    field = max(8, unit)
    body = _round_up(RECORD_SIZE, unit)
    entry = body + (1 + MAX_ATTEMPTS) * field
    if entry <= page:
        stride = unit
        while stride < entry:
            stride *= 2
    else:
        stride = _round_up(entry, page)
    return field, body, entry, stride


@dataclass(frozen=True)
class SlotRecord:
    state: str
    version: int
    image_id: bytes


@dataclass(frozen=True)
class Record:
    seq: int
    event: str
    event_slot: int
    slots: tuple[SlotRecord, SlotRecord]


def zero_bits(data: bytes) -> int:
    """The number of 0 bits in `data`: what the commit field holds (a Berger code)."""
    return len(data) * 8 - sum(bin(byte).count("1") for byte in data)


def commit_field(body: bytes, field: int) -> bytes:
    """The commit field of the record bytes `body`: the count of their 0 bits as a 32-bit word,
    the same word inverted, then 0xFF up to the field size."""
    zeros = zero_bits(body)
    return struct.pack("<II", zeros, zeros ^ 0xFFFFFFFF) + bytes([ERASED]) * (field - 8)


def decode_record(body: bytes) -> Record | None:
    """The record in `body` if it is well formed and its CRC matches; None otherwise. Well formed:
    the magic, the version, the reserved byte 0xFF, a known event, an event slot of 0 or 1, a
    sequence number in range, known states with reserved bytes 0xFF, no version and no id for
    EMPTY and WRITING, exactly one CONFIRMED slot and at most one TRIAL."""
    if (body[0:4] != RECORD_MAGIC or body[4] != RECORD_VERSION or body[7] != ERASED
            or body[5] not in EVENTS or body[6] not in (0, 1)):
        return None
    if struct.unpack_from("<I", body, CRC_OFFSET)[0] != zlib.crc32(body[:CRC_OFFSET]):
        return None
    seq = struct.unpack_from("<I", body, 8)[0]
    if not SEQ_FIRST <= seq <= SEQ_LAST:
        return None
    slots = []
    for s in range(2):
        state, reserved, version, image_id = SLOT_RECORD.unpack_from(
            body, SLOTS_OFFSET + s * SLOT_RECORD.size)
        if state not in STATES or reserved != b"\xff\xff\xff":
            return None
        if STATES[state] in WITHOUT_IMAGE and (version != 0xFFFFFFFF
                                              or image_id != bytes([ERASED]) * 32):
            return None
        slots.append(SlotRecord(STATES[state], version, image_id))
    states = [slot.state for slot in slots]
    if states.count("confirmed") != 1 or states.count("trial") > 1:
        return None
    return Record(seq, EVENTS[body[5]], body[6], (slots[0], slots[1]))


def decode_entry(entry: bytes, unit: int = UNIT_SIZE, page: int = PAGE_SIZE) -> Record | None:
    """The valid record of an entry, or None: its commit field must be exactly the one its
    record bytes give, and the record must decode."""
    field, body_size, _, _ = entry_sizes(unit, page)
    body = entry[:body_size]
    if entry[body_size:body_size + field] != commit_field(body, field):
        return None
    return decode_record(body)


@dataclass(frozen=True)
class Metadata:
    """What the log holds, as the doc text defines it."""
    newest: Record | None          # the valid record with the highest sequence number
    newest_sector: int | None
    newest_index: int | None
    attempts_used: int             # its attempt fields that do not read erased
    records_valid: int             # valid records in the used parts
    entries_used: int              # entries in the used parts
    used: tuple[int, ...]          # per sector, the entries of its used part
    entries_per_sector: int
    sectors: int


def read_metadata(meta: bytes, sector_size: int = SECTOR_SIZE, unit: int = UNIT_SIZE,
                  page: int = PAGE_SIZE) -> Metadata:
    """Read the log from the bytes of the metadata area. The used part of a sector is its
    entries from entry 0 up to the first that reads completely erased; the newest record is the
    valid one with the highest sequence number in the used parts (the first found of two with
    the same number, which no sequence of operations writes)."""
    field, body_size, entry_size, stride = entry_sizes(unit, page)
    per_sector = sector_size // stride
    sectors = len(meta) // sector_size
    newest = None
    where = (None, None)
    valid = 0
    used_counts = []
    for s in range(sectors):
        used = 0
        while used < per_sector:
            at = s * sector_size + used * stride
            entry = meta[at:at + entry_size]
            if entry == bytes([ERASED]) * entry_size:
                break
            record = decode_entry(entry, unit, page)
            if record is not None:
                valid += 1
                if newest is None or record.seq > newest.seq:
                    newest, where = record, (s, used)
            used += 1
        used_counts.append(used)
    attempts = 0
    if newest is not None:
        at = where[0] * sector_size + where[1] * stride + body_size + field
        for k in range(MAX_ATTEMPTS):
            if meta[at + k * field:at + (k + 1) * field] != bytes([ERASED]) * field:
                attempts += 1
    return Metadata(newest, where[0], where[1], attempts, valid, sum(used_counts),
                    tuple(used_counts), per_sector, sectors)


def next_write(md: Metadata) -> tuple[int, int, bool]:
    """Where the next record goes: the entry that ends the used part of the newest record's
    sector (sector 0 in the factory state), or, when that used part is the whole sector, entry 0
    of the next sector of the ring, which is erased first. (sector, index, erases)."""
    current = md.newest_sector if md.newest is not None else 0
    used = md.used[current]
    if used < md.entries_per_sector:
        return current, used, False
    return (current + 1) % md.sectors, 0, True


# ------------------------------------------------------------------------------ the file


def _slot_name(index: int | None) -> str:
    return SLOT_NAMES[index] if index in (0, 1) else "none"


def _version(version: int) -> str:
    return image_format.format_version(version)


def describe(flash: bytes) -> dict:
    """The flash file's content as `fcn_flash info` reports it (without the tool's "flash"
    counts)."""
    if len(flash) != SIZE:
        raise ValueError(f"a flash file has {SIZE} bytes, this one {len(flash)}")
    field, body_size, _, stride = entry_sizes()
    md = read_metadata(flash[META_OFFSET:META_OFFSET + META_SIZE])
    sector, index, erases = next_write(md)
    record = md.newest
    trial = active = None
    if record is not None:
        states = [slot.state for slot in record.slots]
        active = states.index("confirmed")
        trial = states.index("trial") if "trial" in states else None
    attempts_used = md.attempts_used if trial is not None else 0
    metadata = {
        "have_record": record is not None,
        "seq": record.seq if record else None,
        "event": record.event if record else None,
        "event_slot": _slot_name(record.event_slot) if record else None,
        "record_sector": md.newest_sector, "record_index": md.newest_index,
        "active_slot": _slot_name(active), "trial_slot": _slot_name(trial),
        "attempts_used": attempts_used,
        "attempts_left": (MAX_ATTEMPTS - attempts_used) if trial is not None else 0,
        "records_valid": md.records_valid, "entries_used": md.entries_used,
        "entries_total": md.sectors * md.entries_per_sector,
        "entries_per_sector": md.entries_per_sector, "entry_stride": stride,
        "field_size": field, "record_body": body_size,
        "next_write_erases": erases, "next_sector": sector, "next_index": index,
    }
    slots = []
    for s, (offset, size) in enumerate(SLOTS):
        content = flash[offset:offset + size]
        verdict = image_format.verify(content, size)
        recorded = record is not None and record.slots[s].state not in WITHOUT_IMAGE
        slot = {
            "name": SLOT_NAMES[s], "offset": offset, "size": size,
            "state": record.slots[s].state if record else "empty",
            "recorded_version": _version(record.slots[s].version) if recorded else None,
            "recorded_version_code": record.slots[s].version if recorded else None,
            "recorded_id": record.slots[s].image_id.hex() if recorded else None,
            "validation": verdict.status.value,
        }
        if verdict.status is image_format.Status.OK:
            image_id = hashlib.sha256(content[:image_format.HEADER_SIZE]).hexdigest()
            slot["header"] = verdict.header.as_json()
            slot["id"] = image_id
            slot["id_matches_record"] = recorded and image_id == slot["recorded_id"]
        else:
            slot.update({"header": None, "id": None, "id_matches_record": False})
        slots.append(slot)
    return {
        "schema": "fcn.flash_info/1",
        "layout": {"size": SIZE, "sector_size": SECTOR_SIZE, "page_size": PAGE_SIZE,
                   "unit_size": UNIT_SIZE, "meta_offset": META_OFFSET, "meta_size": META_SIZE},
        "metadata": metadata,
        "slots": slots,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m fcn_tools.flash", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    info = sub.add_parser("info", help="the flash file as JSON")
    info.add_argument("flash_file", type=Path)
    args = parser.parse_args(argv)
    try:
        data = args.flash_file.read_bytes()
        print(json.dumps(describe(data), indent=2))
    except (OSError, ValueError) as error:
        print(f"fcn_tools.flash: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
