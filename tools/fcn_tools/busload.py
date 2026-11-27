# SPDX-License-Identifier: MIT
"""The analysis of bench/bus_load.py, as functions of arrays that tests/python feeds with
constructed data: the exact length of every captured frame, the load per identifier and in all,
and the load the DBC's cycle times promise.

A FRAME'S LENGTH is its bit count on the wire from start of frame to end of frame with every
stuff bit, computed from its identifier, format, DLC and data by fcn_tools.canbits (the
independent reference of lib/can/can_bits, held against it on more than 100,000 frames). Every
frame is followed by the 3 bits of intermission (canbits.INTERMISSION_BITS), during which no other
frame can start: the load of a bus is the share of the time its frames and their intermissions
take, and a bus whose frames follow each other without any other gap is at 100 %. The figures
without intermission are given as well: they are what the node's controller model counts as busy
(lib/can/can_ctrl.h, busy_ns).

THE NOMINAL LOAD of a cyclic message of the DBC is its length over its cycle time: worst case
with the most stuff bits any content can need (canbits.worst_case_bits()), and typical with the
mean length of NOMINAL_DRAWS frames of its identifier and DLC with uniformly random data, drawn
from a generator seeded with NOMINAL_SEED (the real content is not random: the measured mean
length per identifier says what it does).
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Mapping

import numpy as np

from . import canbits

#: Random payloads per message for the typical length, and their seed.
NOMINAL_DRAWS = 2000
NOMINAL_SEED = 20261123

EFF_FLAG = 0x8000_0000
RTR_FLAG = 0x4000_0000
ERR_FLAG = 0x2000_0000


def frame_lengths(can_id: np.ndarray, dlc: np.ndarray, data: np.ndarray) -> np.ndarray:
    """The exact length in bits (start of frame to end of frame, stuff bits included, without
    the intermission) of every frame: `can_id` as struct can_frame has it (identifier and the
    EFF and RTR flags), `dlc`, `data` (8 bytes a frame). Frames of equal content are computed
    once. ValueError for an error frame, which has no length to look up."""
    can_id = np.asarray(can_id, dtype=np.uint32)
    dlc = np.asarray(dlc, dtype=np.int64)
    data = np.asarray(data, dtype=np.uint8).reshape(-1, 8)
    if np.any(can_id & ERR_FLAG):
        raise ValueError("an error frame has no length to look up")
    lengths = np.zeros(can_id.size, dtype=np.int64)
    known: dict[tuple[int, int, bytes], int] = {}
    for i in range(can_id.size):
        raw = int(can_id[i])
        remote = bool(raw & RTR_FLAG)
        length = int(dlc[i])
        key = (raw, length, b"" if remote else bytes(data[i, :length]))
        bits = known.get(key)
        if bits is None:
            extended = bool(raw & EFF_FLAG)
            identifier = raw & (0x1FFF_FFFF if extended else 0x7FF)
            bits = canbits.frame_bits(identifier, key[2], is_extended_id=extended,
                                      is_remote_frame=remote, dlc=length).frame_bits
            known[key] = bits
        lengths[i] = bits
    return lengths


@dataclass(frozen=True)
class IdLoad:
    """The frames of one identifier in a window: how many, how often, how long, how much of the
    bus."""
    can_id: int
    frames: int
    frames_per_s: float
    bits_min: int
    bits_mean: float
    bits_max: int
    bits_per_s: float           # with the intermission after each frame
    load: float                 # bits_per_s over the bit rate
    load_without_intermission: float

    def as_json(self, decimals: int = 6) -> dict:
        return {"id": self.can_id, "frames": self.frames,
                "frames_per_s": round(self.frames_per_s, 3), "bits_min": self.bits_min,
                "bits_mean": round(self.bits_mean, 3), "bits_max": self.bits_max,
                "bits_per_s": round(self.bits_per_s, 3), "load": round(self.load, decimals),
                "load_without_intermission": round(self.load_without_intermission, decimals)}


def load_by_id(ids: np.ndarray, lengths: np.ndarray, window_s: float,
               bitrate: float) -> dict[int, IdLoad]:
    """The load of every identifier among frames (`ids`, `lengths` in bits without the
    intermission) seen in a window of `window_s` seconds on a bus of `bitrate` bit/s."""
    if window_s <= 0 or bitrate <= 0:
        raise ValueError("the window and the bit rate must be positive")
    ids = np.asarray(ids, dtype=np.int64)
    lengths = np.asarray(lengths, dtype=np.int64)
    out = {}
    for can_id in np.unique(ids):
        mine = lengths[ids == can_id]
        with_gap = float((mine + canbits.INTERMISSION_BITS).sum()) / window_s
        out[int(can_id)] = IdLoad(
            can_id=int(can_id), frames=int(mine.size), frames_per_s=mine.size / window_s,
            bits_min=int(mine.min()), bits_mean=float(mine.mean()), bits_max=int(mine.max()),
            bits_per_s=with_gap, load=with_gap / bitrate,
            load_without_intermission=float(mine.sum()) / window_s / bitrate)
    return out


def total_load(loads: Iterable[IdLoad]) -> tuple[float, float]:
    """The load of all of them, with and without the intermissions."""
    loads = list(loads)
    return (sum(entry.load for entry in loads),
            sum(entry.load_without_intermission for entry in loads))


@dataclass(frozen=True)
class Nominal:
    """What the DBC promises for one cyclic message."""
    name: str
    can_id: int
    cycle_ms: float
    dlc: int
    worst_bits: int
    typical_bits: float

    @property
    def frames_per_s(self) -> float:
        return 1000.0 / self.cycle_ms

    def load(self, bitrate: float, bits: float) -> float:
        return (bits + canbits.INTERMISSION_BITS) * self.frames_per_s / bitrate


def typical_bits(can_id: int, extended: bool, dlc: int, draws: int = NOMINAL_DRAWS,
                 seed: int = NOMINAL_SEED) -> float:
    """The mean length of `draws` frames of this identifier and DLC with uniformly random data
    (a generator seeded with `seed`)."""
    generator = np.random.default_rng(seed + can_id)
    payloads = generator.integers(0, 256, size=(draws, dlc), dtype=np.uint8)
    return float(np.mean([canbits.frame_bits(can_id, bytes(row), is_extended_id=extended).frame_bits
                          for row in payloads]))


def nominal(messages: Iterable, draws: int = NOMINAL_DRAWS, seed: int = NOMINAL_SEED,
            senders: Iterable[str] | None = None) -> list[Nominal]:
    """The nominal figures of every message with a cycle time (cantools messages: name,
    frame_id, is_extended_frame, length, cycle_time, senders), of `senders` only if given."""
    wanted = None if senders is None else set(senders)
    out = []
    for message in messages:
        if not message.cycle_time:
            continue
        if wanted is not None and not wanted.intersection(message.senders):
            continue
        extended = bool(message.is_extended_frame)
        out.append(Nominal(name=message.name, can_id=int(message.frame_id),
                           cycle_ms=float(message.cycle_time), dlc=int(message.length),
                           worst_bits=canbits.worst_case_bits(extended, False, message.length),
                           typical_bits=typical_bits(int(message.frame_id), extended,
                                                     int(message.length), draws, seed)))
    return out


def pool(entries: list[dict], bitrate: float) -> dict:
    """Several windows as one (the downloads of a bench, each its own run): per identifier the
    frames and their bits summed over the windows, over the windows' total length. `entries`
    have "window_s", "by_id" (IdLoad.as_json() by key) and "groups" (by name: "frames_per_s",
    "bits_per_s", "load"); the result has the same members and "runs", the entries."""
    window = sum(entry["window_s"] for entry in entries)
    if window <= 0:
        raise ValueError("the windows have no length")
    by_id: dict[str, dict] = {}
    for entry in entries:
        for key, item in entry["by_id"].items():
            target = by_id.setdefault(key, {"id": item["id"], "frames": 0, "bits": 0.0,
                                            "bits_min": item["bits_min"],
                                            "bits_max": item["bits_max"]})
            target["frames"] += item["frames"]
            target["bits"] += item["bits_mean"] * item["frames"]
            target["bits_min"] = min(target["bits_min"], item["bits_min"])
            target["bits_max"] = max(target["bits_max"], item["bits_max"])
    merged = {}
    for key, item in sorted(by_id.items()):
        with_gap = item["bits"] + canbits.INTERMISSION_BITS * item["frames"]
        merged[key] = {"id": item["id"], "frames": item["frames"],
                       "frames_per_s": round(item["frames"] / window, 3),
                       "bits_min": item["bits_min"],
                       "bits_mean": round(item["bits"] / item["frames"], 3),
                       "bits_max": item["bits_max"], "bits_per_s": round(with_gap / window, 3),
                       "load": round(with_gap / window / bitrate, 6),
                       "load_without_intermission": round(item["bits"] / window / bitrate, 6)}
    groups = {name: {key: round(sum(entry["groups"][name][key] * entry["window_s"]
                                    for entry in entries) / window, 6)
                     for key in ("frames_per_s", "bits_per_s", "load")}
              for name in entries[0]["groups"]}
    return {"runs": entries, "window_s": round(window, 6), "by_id": merged,
            "load": round(sum(item["load"] for item in merged.values()), 6),
            "load_without_intermission": round(sum(item["load_without_intermission"]
                                                   for item in merged.values()), 6),
            "groups": groups}


def compare(measured: Mapping[int, IdLoad], expected: Iterable[Nominal], bitrate: float) -> dict:
    """Per cyclic message: what the DBC promises next to what was measured, and the ratios."""
    out = {}
    for entry in expected:
        seen = measured.get(entry.can_id)
        fps = seen.frames_per_s if seen is not None else 0.0
        load = seen.load if seen is not None else 0.0
        worst = entry.load(bitrate, entry.worst_bits)
        typical = entry.load(bitrate, entry.typical_bits)
        out[entry.name] = {
            "id": entry.can_id, "cycle_ms": entry.cycle_ms, "dlc": entry.dlc,
            "nominal_frames_per_s": round(entry.frames_per_s, 3),
            "measured_frames_per_s": round(fps, 3),
            "frames_ratio": round(fps / entry.frames_per_s, 6),
            "worst_case_bits": entry.worst_bits, "typical_bits": round(entry.typical_bits, 3),
            "measured_mean_bits": None if seen is None else round(seen.bits_mean, 3),
            "nominal_load_worst_case": round(worst, 6),
            "nominal_load_typical": round(typical, 6),
            "measured_load": round(load, 6),
            "load_ratio_to_typical": round(load / typical, 6) if typical else None,
        }
    return out
