#!/usr/bin/env python3
"""Measure the ISO-TP and UDS transfer rate between the tester and the node, in both directions.

Everything measured here runs on a Linux host: the node is the FreeRTOS POSIX port, the tester
is udsoncan 1.26.1 over can-isotp 2.0.7 in Python (fcn_tools.diag, as the flasher uses them),
and the bus is the node's model of a 500 kbit/s CAN bus over SocketCAN (vcan0) or the UDP bus.

1. Tester to node, TransferData. For every block size of --block-sizes (data bytes per block,
   1 to the node's maximum of 1024: maxNumberOfBlockLength 1026), one run of the node with a
   fresh flash (slot A holds a factory image, so that slot B can be written), and in it the
   flasher's programming sequence with blocks of that size: the sessions, eraseMemory,
   RequestDownload, --blocks blocks of TransferData, RequestTransferExit and the check routine,
   each block timed by the tester as the flasher times it (time.monotonic() around udsoncan's
   transfer_data()). A third participant on the bus records every frame with the kernel's
   receive time stamp, one clock for the frames of both sides, and each block is cut into the
   turns of the two sides: the node's flow controls (from the frame that completes a block of
   consecutive frames to the node's FC), the tester's bursts (from an FC to the last consecutive
   frame it releases), the node's last turn (from the last consecutive frame to the response,
   which holds the write job in the flash task) and the tester's turnaround (from the response
   to its next request). The node's statistics give the write jobs' mean time
   (confirm.flash_task.jobs.write total_ns / run) and the counts of its diagnostic task. The bus
   time of a block's frames is computed from their exact bit counts at 500 kbit/s
   (fcn_tools.canbits), and so is the floor that the diagnostic task's budget of
   APP_DIAG_EVENTS_PER_TICK (2) events per tick sets, from the block's events: the frames the node
   receives and the transmit-done reports of the flow controls it sends (docs/diagnostics.md).
2. Node to tester: the longest response the node has, ReadDTCInformation reportSupportedDTC
   (19 0A: 71 bytes with the node's 17 DTCs), --responses times, cut into the node's first frame,
   the tester's flow control (BS 0, STmin 0, as the flasher's) and the node's consecutive frames,
   which leave one per transmit-done report.
3. A projection, not a measurement: what blocks of 2048 bytes would take, by a straight line
   fitted to the measured block periods less their write jobs against their events, and a write
   job scaled by the pages it programs. The node is not rebuilt with another
   maxNumberOfBlockLength or another event budget (phase 6, decision 2).

Builds the release preset first (scripts/build.sh release) unless --no-build, refuses to measure
with binaries that are not the tree's build, and writes results/isotp_throughput.json and
results/isotp_throughput.png.

    python bench/isotp_throughput.py                          # both transports, about 70 s
    python bench/isotp_throughput.py --transports udp         # one transport
    python bench/isotp_throughput.py --quick --out /tmp/b     # a test of the bench, seconds
    python bench/isotp_throughput.py --replot                 # redraw the plot from the JSON
"""
from __future__ import annotations

import argparse
import json
import math
import random
import sys
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Sequence

import numpy as np

from fcn_tools import bench, bus, diag, flasher, plotstyle, results, runner
from fcn_tools.canbits import INTERMISSION_BITS, frame_bits

NAME = "isotp_throughput"
VERSION = 1

#: The node's identifiers, bit rate and receiver parameters (dbc/fcn.dbc, app/diag.h, lib/uds).
#: The bench reads the block size, the event budget and the tick from the node's statistics and
#: maxNumberOfBlockLength from its answer, and refuses to go on if they are not these.
REQUEST_ID = diag.REQUEST_ID
RESPONSE_ID = diag.RESPONSE_ID
BITRATE = 500_000
NODE_BS = 16
EVENTS_PER_TICK = 2
TICK_RATE_HZ = 1000
MAX_BLOCK_LENGTH = 1026
MAX_BLOCK_DATA = MAX_BLOCK_LENGTH - flasher.BLOCK_OVERHEAD
#: The data bytes of a classical CAN frame with normal addressing: an SF 7, an FF 6, a CF 7.
SF_DATA, FF_DATA, CF_DATA = 7, 6, 7
#: A page of the host's flash: a write job programs the pages its block completes.
PAGE_SIZE = diag.PAGE_SIZE

#: The slot the downloads write (the host's slot B) and the versions of the images.
SLOT_B = flasher.SLOT_ADDRESSES["B"]
FACTORY_VERSION = "1.0.0"
DATA_VERSION = "1.5.0"
#: The smallest image a run downloads: the 256-byte header and something after it.
MIN_IMAGE_BYTES = 512
#: The size of the factory image in slot A, a data payload: the node runs without the
#: bootloader here, so slot A only has to hold a CONFIRMED image for slot B to be writable.
FACTORY_PAYLOAD_BYTES = 4096
#: The seed of the payloads, so that every run sends the same bytes (and their bit stuffing).
PAYLOAD_SEED = 20261123

DEFAULT_BLOCK_SIZES = (1, 5, 6, 16, 64, 128, 256, 512, 768, 1024)
DEFAULT_BLOCKS = 64
DEFAULT_RESPONSES = 500
DEFAULT_TRANSPORTS = ("socketcan", "udp")
QUICK = {"block_sizes": (6, 1024), "blocks": 6, "responses": 30, "transports": ("udp",)}

#: The longest response of the node (docs/diagnostics.md): ReadDTCInformation 0x0A lists every
#: DTC of its table, 17 of them, 3 + 4 x 17 bytes.
LONGEST_REQUEST = "19 0A"
LONGEST_RESPONSE_BYTES = 71

#: The block size of the projection, and the image whose transfer time it is turned into.
PROJECTED_BLOCK_DATA = 2048
PROJECTION_IMAGE_BYTES = 256 * 1024
#: The block sizes the line is fitted to: those of more than one frame (an SF block has no flow
#: control and no consecutive frame, so it shows the fixed part alone).
FIT_FROM_DATA_BYTES = 6

#: How long the bench waits for the node's first status frame: a bound only.
START_WAIT_S = 10.0
#: The listener's stamps are the kernel's CLOCK_REALTIME, which the time synchronisation of a
#: WSL2 VM steps (pilot runs found stamps 1.3 s and 1.8 s off, and the VM's clock stepped by over
#: two hours during one), while the tester's own times are time.monotonic(). A block is left out
#: of the decomposition, and counted (clock_disagreements), when its stamps cannot be right
#: (block_clock_problems()): one goes back; its wire time from the first frame to the response
#: exceeds the tester's own measurement of the request, which encloses it, by more than
#: CLOCK_SLACK_S; or its wire period differs from the tester's period by more than
#: PERIOD_SLACK_S (both periods run from a send of the tester to its next; they differ by how
#: long the tester's Python took to get each request onto the bus).
CLOCK_SLACK_S = 0.0005
PERIOD_SLACK_S = 0.005

TIMING_NOTE = (
    "The block periods are bounded below by the node's 1 ms tick and its budget of 2 events of "
    "the diagnostic task per tick, which are the node's own and would be the same on a "
    "microcontroller; the host adds the latency of its scheduler at the tick boundaries and the "
    "turnaround of a tester in Python, which a tester on a CAN adapter does not have. The bus "
    "times and the event floors are computed from the frames, not measured, and the 2048-byte "
    "block is a projection from the measured parts, not a measurement."
)


# ------------------------------------------------------------------------- frame arithmetic


def isotp_frames(length: int) -> tuple[int, int]:
    """The frames that carry a message of `length` bytes on classical CAN with normal addressing:
    (frames, consecutive frames). 1 to 7 bytes are one SF; 8 to 4095 an FF and its CFs."""
    if not 1 <= length <= 4095:
        raise ValueError(f"no ISO-TP message of {length} bytes on classical CAN")
    if length <= SF_DATA:
        return 1, 0
    cfs = -(-(length - FF_DATA) // CF_DATA)
    return 1 + cfs, cfs


def flow_controls(cfs: int, block_size: int = NODE_BS) -> int:
    """The flow controls a receiver with `block_size` sends for a message of `cfs` consecutive
    frames: one after the FF, then one after every `block_size`-th CF but the last (none for an
    SF; one for block size 0)."""
    if cfs < 0 or block_size < 0:
        raise ValueError("counts are not negative")
    if cfs == 0:
        return 0
    return 1 + ((cfs - 1) // block_size if block_size else 0)


def block_events(data_bytes: int, block_size: int = NODE_BS) -> int:
    """The events of the node's diagnostic task in receiving a TransferData request with
    `data_bytes` of data (its SID and counter come on top): every frame it receives and the
    transmit-done report of every flow control it sends (docs/diagnostics.md)."""
    frames, cfs = isotp_frames(data_bytes + flasher.BLOCK_OVERHEAD)
    return frames + flow_controls(cfs, block_size)


def event_floor_s(events: int, per_tick: int = EVENTS_PER_TICK,
                  tick_s: float = 1.0 / TICK_RATE_HZ) -> float:
    """The floor the event budget sets on the mean period of blocks of `events` events each: a
    block brings its events and the transmit-done report of its response, and the diagnostic
    task takes at most `per_tick` in a tick, so N blocks take at least N (events + 1) / per_tick
    ticks, less one (docs/diagnostics.md: 157 events and the report, at least 79 ticks). A
    single block can take less, its events falling into fewer ticks of its own."""
    if per_tick < 1 or tick_s <= 0:
        raise ValueError("a budget of at least one event in a positive tick")
    return (events + 1) / per_tick * tick_s


def ticks_for_s(events: int, per_tick: int = EVENTS_PER_TICK,
                tick_s: float = 1.0 / TICK_RATE_HZ) -> float:
    """The ticks the diagnostic task needs to take `events` events that are all there, whole."""
    if per_tick < 1 or tick_s <= 0:
        raise ValueError("a budget of at least one event in a positive tick")
    return math.ceil(events / per_tick) * tick_s


def frame_bus_s(can_id: int, data: bytes, bitrate: float = BITRATE) -> float:
    """The time a base-format frame takes on the bus, its stuff bits and the 3 bits of
    intermission included."""
    return (frame_bits(can_id, bytes(data)).frame_bits + INTERMISSION_BITS) / bitrate


def pages(data_bytes: int) -> float:
    """The pages of PAGE_SIZE bytes that `data_bytes` fill (a fraction for less than one)."""
    return data_bytes / PAGE_SIZE


# ------------------------------------------------------------------------- the wire, analysed


@dataclass(frozen=True)
class WireFrame:
    """A frame as the listener received it: the kernel's receive time (seconds, CLOCK_REALTIME),
    the identifier, the data, and its place among the frames of the connection in the order the
    listener received them (indexed())."""
    t: float
    can_id: int
    data: bytes
    index: int = -1


def indexed(frames: Sequence[WireFrame]) -> list[WireFrame]:
    """`frames`, each with its place in the sequence as its index."""
    return [WireFrame(f.t, f.can_id, f.data, i) for i, f in enumerate(frames)]


def in_order(frames: Sequence[WireFrame], first: int, last: int) -> bool:
    """Whether the time stamps of frames[first..last] (indexed()) never go back. One socket
    receives its frames in order, so a stamp that goes back is one the clock moved under: the
    kernel stamps with CLOCK_REALTIME, which the time synchronisation of a WSL2 VM steps."""
    times = [f.t for f in frames[first:last + 1]]
    return all(b >= a for a, b in zip(times, times[1:]))


@dataclass
class IsoTpMessage:
    """An ISO-TP message on the wire: its sender ("tester" on REQUEST_ID, "node" on
    RESPONSE_ID), its length and first data bytes, its frames (an SF, or an FF and its CFs) and
    the flow controls the receiver sent while it was in progress."""
    sender: str
    length: int
    head: bytes
    frames: list[WireFrame] = field(default_factory=list)
    flow_controls: list[WireFrame] = field(default_factory=list)

    @property
    def start(self) -> float:
        return self.frames[0].t

    @property
    def end(self) -> float:
        return self.frames[-1].t

    @property
    def complete(self) -> bool:
        if self.frames[0].data[0] >> 4 == 0:
            return True
        return FF_DATA + CF_DATA * (len(self.frames) - 1) >= self.length


def wire_frames(messages: Iterable[Any], ids: Sequence[int] = (REQUEST_ID, RESPONSE_ID)
                ) -> list[WireFrame]:
    """The frames of the connection among python-can messages, as WireFrames, in the order the
    listener received them (indexed())."""
    return indexed([WireFrame(float(m.timestamp), int(m.arbitration_id), bytes(m.data))
                    for m in messages if m.arbitration_id in ids and not m.is_extended_id])


def isotp_messages(frames: Sequence[WireFrame], request_id: int = REQUEST_ID,
                   response_id: int = RESPONSE_ID) -> tuple[list[IsoTpMessage], int]:
    """The ISO-TP messages of both sides, in the order they started, each with the other side's
    flow controls during it; and the number of frames that belong to no message (a CF or an FC
    that nothing expects, a frame of a reserved type)."""
    out: list[IsoTpMessage] = []
    current: dict[str, IsoTpMessage | None] = {"tester": None, "node": None}
    strays = 0
    for frame in frames:
        if frame.can_id not in (request_id, response_id) or not frame.data:
            continue
        sender = "tester" if frame.can_id == request_id else "node"
        other = "node" if sender == "tester" else "tester"
        kind = frame.data[0] >> 4
        if kind == 0:
            length = frame.data[0] & 0x0F
            out.append(IsoTpMessage(sender, length, frame.data[1:1 + length], [frame]))
            current[sender] = None
        elif kind == 1:
            length = ((frame.data[0] & 0x0F) << 8) | frame.data[1]
            message = IsoTpMessage(sender, length, frame.data[2:8], [frame])
            out.append(message)
            current[sender] = message
        elif kind == 2 and current[sender] is not None:
            message = current[sender]
            message.frames.append(frame)
            if message.complete:
                current[sender] = None
        elif kind == 3 and current[other] is not None:
            current[other].flow_controls.append(frame)
        else:
            strays += 1
    return out, strays


@dataclass
class BlockTimeline:
    """One TransferData request on the wire, cut into the turns of the two sides (seconds).

    start          the time of its first frame (the listener's clock)
    period_s       from its first frame to the first frame of the tester's next request
    on_wire_s      from its first frame to the first frame of the node's response
    node_fc_s      each flow control of the node: from the tester's frame that completed a block
                   of consecutive frames (the FF, for the first) to the node's FC
    tester_fc_s    from each FC to the tester's first consecutive frame after it
    tester_burst_s from that first consecutive frame to the last one before the next FC (or the
                   end of the request)
    node_final_s   from the request's last frame to the node's response: the last frames taken,
                   the write job, the response
    turnaround_s   from the response to the tester's next request (None for the last request)
    bus_s          the bus time of the request's frames, the node's flow controls and the response
    """
    start: float
    counter: int
    data_bytes: int
    frames: int
    flow_controls: int
    period_s: float | None
    on_wire_s: float
    node_fc_s: list[float]
    tester_fc_s: list[float]
    tester_burst_s: list[float]
    node_final_s: float
    turnaround_s: float | None
    bus_s: float
    response: bytes
    first_index: int = -1
    last_index: int = -1
    pendings: int = 0

    @property
    def node_s(self) -> float:
        """The time the node held the turn: its flow controls and its last turn."""
        return sum(self.node_fc_s) + self.node_final_s

    @property
    def tester_s(self) -> float | None:
        """The time the tester held it: its reactions to the FCs, its bursts, its turnaround."""
        if self.turnaround_s is None:
            return None
        return sum(self.tester_fc_s) + sum(self.tester_burst_s) + self.turnaround_s


def block_timeline(request: IsoTpMessage, response: IsoTpMessage,
                   next_request: IsoTpMessage | None, bitrate: float = BITRATE,
                   pendings: int = 0) -> BlockTimeline:
    """The timeline of the TransferData `request` (see BlockTimeline), answered by `response`
    (after `pendings` responsePending) and followed by the tester's `next_request` (None if none
    came). first_index and last_index: the places (WireFrame.index) of its first frame and of
    the next request's first frame, or of the response's last frame if none came."""
    first = request.frames[0]
    cfs = request.frames[1:]
    fcs = request.flow_controls
    node_fc: list[float] = []
    tester_fc: list[float] = []
    bursts: list[float] = []
    trigger = first.t
    for index, fc in enumerate(fcs):
        node_fc.append(fc.t - trigger)
        until = fcs[index + 1].t if index + 1 < len(fcs) else math.inf
        burst = [cf for cf in cfs if fc.t <= cf.t < until]
        if burst:
            tester_fc.append(burst[0].t - fc.t)
            bursts.append(burst[-1].t - burst[0].t)
            trigger = burst[-1].t
    bus_s = sum(frame_bus_s(f.can_id, f.data, bitrate)
                for f in (*request.frames, *fcs, *response.frames))
    return BlockTimeline(
        start=first.t,
        counter=request.head[1] if len(request.head) > 1 else -1,
        data_bytes=request.length - flasher.BLOCK_OVERHEAD,
        frames=len(request.frames), flow_controls=len(fcs),
        period_s=None if next_request is None else next_request.start - first.t,
        on_wire_s=response.start - first.t,
        node_fc_s=node_fc, tester_fc_s=tester_fc, tester_burst_s=bursts,
        node_final_s=response.start - request.end,
        turnaround_s=None if next_request is None else next_request.start - response.start,
        bus_s=bus_s, response=bytes(response.head), first_index=first.index,
        last_index=(response.frames[-1] if next_request is None else next_request.frames[0]).index,
        pendings=pendings)


def is_pending(message: IsoTpMessage) -> bool:
    """A responsePending of the node: 7F <SID> 78."""
    return message.sender == "node" and message.head[:1] == b"\x7f" and message.head[2:3] == b"\x78"


def answer_after(messages: Sequence[IsoTpMessage], index: int) -> tuple[int, int] | None:
    """The place in `messages` of the node's final answer to the tester's message at `index`
    (the node's next message that is no responsePending), and the responsePendings before it;
    None if the node sent none before the tester's next message. By the order the listener
    received them, not by their time stamps: the node answers one request at a time."""
    pendings = 0
    for place in range(index + 1, len(messages)):
        message = messages[place]
        if message.sender == "tester":
            return None
        if is_pending(message):
            pendings += 1
            continue
        return place, pendings
    return None


def transfer_timelines(messages: Sequence[IsoTpMessage], bitrate: float = BITRATE
                       ) -> list[BlockTimeline]:
    """The timeline of every TransferData request (SID 0x36) of the tester among `messages`
    that the node answered, in order: its response is the node's next final answer, its next
    request the tester's next message after that."""
    out = []
    for index, message in enumerate(messages):
        if message.sender != "tester" or not message.head or message.head[0] != 0x36:
            continue
        found = answer_after(messages, index)
        if found is None:
            continue
        place, pendings = found
        following = next((m for m in messages[place + 1:] if m.sender == "tester"), None)
        out.append(block_timeline(message, messages[place], following, bitrate, pendings))
    return out


@dataclass
class ResponseTimeline:
    """One multi-frame response of the node on the wire (seconds): from the tester's request to
    the node's FF (node_first_s), from the FF to the tester's flow control (tester_fc_s), from
    the FC to the node's last CF (node_cfs_s); total_s from the request to the last CF;
    cf_gaps_s between consecutive CFs; bus_s the bus time of the request, the response and the
    FC."""
    length: int
    frames: int
    total_s: float
    node_first_s: float
    tester_fc_s: float
    node_cfs_s: float
    cf_gaps_s: list[float]
    bus_s: float
    first_index: int = -1
    last_index: int = -1


def response_timelines(messages: Sequence[IsoTpMessage], sid: int,
                       bitrate: float = BITRATE) -> list[ResponseTimeline]:
    """The timeline of every complete multi-frame response of the node, with one flow control of
    the tester, to a request of the tester for the service `sid` (its next final answer, by the
    order received). first_index and last_index: the places of the request's first frame and of
    the response's last."""
    out = []
    for index, message in enumerate(messages):
        if message.sender != "tester" or not message.head or message.head[0] != sid:
            continue
        found = answer_after(messages, index)
        response = None if found is None else messages[found[0]]
        if (response is None or len(response.frames) < 2 or not response.complete
                or len(response.flow_controls) != 1):
            continue
        fc = response.flow_controls[0]
        cfs = response.frames[1:]
        bus_s = sum(frame_bus_s(f.can_id, f.data, bitrate)
                    for f in (*message.frames, *response.frames, fc))
        out.append(ResponseTimeline(
            length=response.length, frames=len(response.frames),
            total_s=response.end - message.start,
            node_first_s=response.start - message.end, tester_fc_s=fc.t - response.start,
            node_cfs_s=response.end - fc.t,
            cf_gaps_s=[b.t - a.t for a, b in zip(cfs, cfs[1:])], bus_s=bus_s,
            first_index=message.frames[0].index, last_index=response.frames[-1].index))
    return out


def block_clock_problems(timeline: BlockTimeline, frames: Sequence[WireFrame],
                         tester_block_s: float, tester_period_s: float | None,
                         slack_s: float = CLOCK_SLACK_S,
                         period_slack_s: float = PERIOD_SLACK_S) -> list[str]:
    """Why the listener's stamps of a block cannot be right, if they cannot (see the constants):
    "order", a stamp goes back within it; "on_wire", its wire time from the first frame to the
    response is longer than the tester's own measurement of the request by more than `slack_s`;
    "period", its wire period differs from the tester's (from its start to the start of its next
    request, time.monotonic()) by more than `period_slack_s`."""
    problems = []
    if timeline.first_index < 0 or not in_order(frames, timeline.first_index,
                                                timeline.last_index):
        problems.append("order")
    if timeline.on_wire_s - tester_block_s > slack_s:
        problems.append("on_wire")
    if (timeline.period_s is not None and tester_period_s is not None
            and abs(timeline.period_s - tester_period_s) > period_slack_s):
        problems.append("period")
    return problems


def bus_time_between(others: Sequence[tuple[float, float]], start: float, end: float) -> float:
    """The bus time of the frames of `others` ((time, bus time) pairs, in time order) received
    from `start` up to, not including, `end`."""
    times = [t for t, _ in others]
    low = int(np.searchsorted(times, start, side="left"))
    high = int(np.searchsorted(times, end, side="left"))
    return float(sum(duration for _, duration in others[low:high]))


# --------------------------------------------------------------------- figures from the parts


def fit_line(x: Sequence[float], y: Sequence[float]) -> dict[str, float | int]:
    """The least-squares line y = intercept + slope * x through the points: intercept, slope,
    the largest absolute residual, the coefficient of determination r2 and n. ValueError for
    fewer than three points or a single value of x."""
    xs = np.asarray(x, dtype=np.float64)
    ys = np.asarray(y, dtype=np.float64)
    if xs.size != ys.size or xs.size < 3 or np.ptp(xs) == 0:
        raise ValueError("a line needs at least three points at two values of x or more")
    slope, intercept = np.polyfit(xs, ys, 1)
    residuals = ys - (intercept + slope * xs)
    total = float(np.sum((ys - ys.mean()) ** 2))
    r2 = 1.0 - float(np.sum(residuals ** 2)) / total if total > 0 else 1.0
    return {"intercept": float(intercept), "slope": float(slope),
            "max_abs_residual": float(np.max(np.abs(residuals))), "r2": r2, "n": int(xs.size)}


def project_block(fit: dict, data_bytes: int, write_per_page_s: float,
                  block_size: int = NODE_BS, per_tick: int = EVENTS_PER_TICK,
                  tick_s: float = 1.0 / TICK_RATE_HZ) -> dict[str, float | int]:
    """What a block of `data_bytes` would take by the fitted line (period less the write job,
    against the block's events) and a write job of `write_per_page_s` per page it fills: its
    events, the event floor, the write job, the period and the throughput (seconds, bytes/s)."""
    events = block_events(data_bytes, block_size)
    write_s = write_per_page_s * pages(data_bytes)
    period = fit["intercept"] + fit["slope"] * events + write_s
    return {"data_bytes": data_bytes, "events": events,
            "event_floor_s": event_floor_s(events, per_tick, tick_s), "write_s": write_s,
            "period_s": period, "bytes_per_s": data_bytes / period}


def last_burst(cfs: int, fcs: int, block_size: int = NODE_BS) -> int:
    """The consecutive frames a sender sends after the receiver's last flow control: all of them
    for one FC, the rest of the last block of `block_size` otherwise (none for an SF)."""
    if fcs == 0:
        return 0
    return cfs - block_size * (fcs - 1)


def final_parts(cfs: int, fcs: int, node_final_s: float | None,
                write_s: float | None) -> dict[str, float | None]:
    """The node's last turn of a block (from its last frame to the response, measured) in its
    parts (ms): the ticks to take the frames of the last burst (computed: ceil(CFs / 2), at
    least one), the write job (the node's statistics) and the rest (the job's hand-over and the
    response)."""
    floor_s = ticks_for_s(max(last_burst(cfs, fcs), 1))
    rest = None if node_final_s is None or write_s is None else node_final_s - write_s - floor_s
    return {"unit": "ms", "measured": ms(node_final_s), "last_frames_floor": ms(floor_s),
            "write_job": ms(write_s), "rest": ms(rest)}


def image_time_s(image_bytes: int, data_per_block: int, period_s: float) -> float:
    """The TransferData of an image of `image_bytes` in blocks of `data_per_block`, each taking
    `period_s` (the last, shorter block counted whole)."""
    return math.ceil(image_bytes / data_per_block) * period_s


def mean(values: Iterable[float | None]) -> float | None:
    """The mean of the values that are not None; None if there are none."""
    data = [value for value in values if value is not None]
    return float(np.mean(data)) if data else None


def ms(value: float | None, decimals: int = 4) -> float | None:
    return None if value is None else round(value * 1e3, decimals)


def central(distribution: dict) -> float | None:
    """A distribution's median, or its mean where the sample is too small for a median."""
    return distribution["p50"] if distribution["p50"] is not None else distribution["mean"]


# ---------------------------------------------------------------------------- measuring


def factory_image(out: Path) -> Path:
    """The factory image of slot A: a data payload, adopted as CONFIRMED by `fcn_flash factory`.
    The node runs without the bootloader here, so nothing ever starts it."""
    path = out / "factory.img"
    path.write_bytes(bench.pack_image(random.Random(PAYLOAD_SEED + 1).randbytes(
        FACTORY_PAYLOAD_BYTES), FACTORY_VERSION))
    return path


def image_for(data_bytes: int, blocks: int) -> bytes:
    """The image a run with blocks of `data_bytes` downloads: a data payload (seeded) packed with
    its header, `blocks` blocks long but at least MIN_IMAGE_BYTES. It validates (the check
    routine) and is never started."""
    size = max(blocks * data_bytes, MIN_IMAGE_BYTES)
    payload = random.Random(PAYLOAD_SEED).randbytes(size - 256)
    return bench.pack_image(payload, DATA_VERSION)


def wait_for_status(reader: bus.BusReader, node: Any, timeout: float = START_WAIT_S) -> None:
    """Wait for the node's first status frame: its tasks run, the diagnostic task among them."""
    deadline = time.monotonic() + timeout
    while not any(bus.is_status(m) for m in list(reader.frames)):
        if not node.running or time.monotonic() >= deadline:
            raise runner.BenchFailure(f"no status frame from {node.name} within {timeout} s\n"
                                      f"{node.stderr()}")
        time.sleep(0.02)


def download(spec: str, image: bytes, data_per_block: int) -> dict[str, Any]:
    """The flasher's programming sequence into slot B with blocks of `data_per_block` data bytes
    (udsoncan configured as fcn_tools.diag configures it for the flasher), each block timed as
    the flasher times it; the times and the lengths of the blocks sent."""
    from udsoncan import MemoryLocation  # noqa: PLC0415 (only here)

    size = len(image)
    location = MemoryLocation(address=SLOT_B, memorysize=size, address_format=32,
                              memorysize_format=32)
    times: dict[str, Any] = {"blocks_s": []}
    with diag.connect(spec) as client:
        client.change_session(1)
        client.change_session(3)
        client.change_session(2)
        start = time.monotonic()
        client.set_config("request_timeout", diag.erase_timeout_s(size))
        client.start_routine(diag.RID_ERASE_MEMORY, bytes([0x44]) + SLOT_B.to_bytes(4, "big")
                             + size.to_bytes(4, "big"))
        times["erase_s"] = time.monotonic() - start
        client.set_config("request_timeout", diag.DEFAULT_REQUEST_TIMEOUT_S)
        max_length = int(client.request_download(location).service_data.max_length)
        if max_length != MAX_BLOCK_LENGTH:
            raise runner.BenchFailure(f"the node answers maxNumberOfBlockLength {max_length}, "
                                      f"not {MAX_BLOCK_LENGTH}")
        times["max_block_length"] = max_length
        plan = flasher.plan_blocks(size, data_per_block + flasher.BLOCK_OVERHEAD)
        transfer = time.monotonic()
        times["starts_s"] = []
        for counter, offset, length in plan:
            # As fcn_tools.flasher times a block: the timeout set and put back within the time.
            block = time.monotonic()
            client.set_config("request_timeout", diag.write_timeout_s(length))
            client.transfer_data(counter, image[offset:offset + length])
            client.set_config("request_timeout", diag.DEFAULT_REQUEST_TIMEOUT_S)
            times["blocks_s"].append(time.monotonic() - block)
            times["starts_s"].append(block)
        times["transfer_s"] = time.monotonic() - transfer
        times["starts_s"].append(time.monotonic())        # the next request: the exit
        client.request_transfer_exit()
        client.set_config("request_timeout", diag.check_timeout_s(size))
        client.start_routine(diag.RID_CHECK_PROGRAMMING)
    times["plan"] = [length for _, _, length in plan]
    return times


def node_counts(stats: dict) -> dict[str, Any]:
    """What a node's statistics say of the download and of its diagnostic task."""
    jobs = stats["confirm"]["flash_task"]["jobs"]
    download_stats = stats["diag"]["uds"]["download"]
    task = stats["diag"]["task"]
    write = jobs["write"]
    return {
        "build_id": stats["build_id"],
        "tick_rate_hz": stats["tick"]["rate_hz"],
        "events_per_tick": task["events_per_tick"],
        "isotp_bs": stats["diag"]["isotp"]["parameters"]["bs"],
        "isotp_stmin": stats["diag"]["isotp"]["parameters"]["stmin"],
        "download": {key: download_stats[key] for key in (
            "state", "size", "received", "blocks", "repeated_blocks", "committed", "erase_us",
            "transfer_us", "check_us")},
        "write_jobs": {key: write[key] for key in ("run", "total_ns", "max_ns", "last_ns",
                                                   "last_status")},
        "task": {key: task[key] for key in ("events", "passes", "budget_waits", "rx_lost",
                                             "rx_high_water", "tx_done_lost")},
    }


def other_frames(messages: Iterable[Any]) -> list[tuple[float, float]]:
    """(time, bus time) of every frame that is not of the diagnostic connection (the node's
    telemetry, which shares the bus with the download), in the order received."""
    return [(float(m.timestamp), frame_bus_s(m.arbitration_id, bytes(m.data)))
            for m in messages
            if m.arbitration_id not in (REQUEST_ID, RESPONSE_ID) and not m.is_extended_id]


def check_node(counts: dict) -> None:
    """The node runs with the parameters the analysis assumes (see the constants)."""
    found = (counts["isotp_bs"], counts["events_per_tick"], counts["tick_rate_hz"])
    if found != (NODE_BS, EVENTS_PER_TICK, TICK_RATE_HZ):
        raise runner.BenchFailure(f"the node runs with BS, events per tick and tick rate {found},"
                                  f" not {(NODE_BS, EVENTS_PER_TICK, TICK_RATE_HZ)}")


def measure_block_size(session: runner.Session, data_bytes: int, blocks: int,
                       factory: Path) -> tuple[dict, str]:
    """One run of the node for blocks of `data_bytes`: the download, the frames on the wire and
    the node's statistics (the raw material of analyse_block_size()), and the build id the node
    reported."""
    image = image_for(data_bytes, blocks)
    flash = session.workdir / f"flash-{session.transport}-{data_bytes}.bin"
    bench.factory(flash, factory)
    listener = session.open_bus()
    try:
        with bus.BusReader(listener) as reader:
            node = session.start_node("--flash-file", str(flash),
                                      name=f"node-{session.transport}-{data_bytes}")
            wait_for_status(reader, node)
            times = download(session.can_spec, image, data_bytes)
            # Stopped before the reader ends: the reader then takes what is still on its way
            # until the bus is quiet (fcn_tools.bus.drain()), which it is not while the node runs.
            stats = session.finish(node)
    finally:
        listener.shutdown()
    counts = node_counts(stats)
    check_node(counts)
    return {"data_bytes": data_bytes, "image_bytes": len(image), "times": times,
            "frames": wire_frames(reader.frames),
            "others": sorted(other_frames(reader.frames)),
            "node": counts}, stats["build_id"]


def measure_responses(session: runner.Session, count: int) -> tuple[dict, str]:
    """`count` requests for the node's longest response with the raw tester (can-isotp with the
    flasher's parameters), each timed by the tester; the frames and the node's statistics, and
    the build id the node reported."""
    from fcn_tools.tester import Uds  # noqa: PLC0415 (asserts: refuses python -O)

    listener = session.open_bus()
    took: list[float] = []
    lengths: set[int] = set()
    try:
        with bus.BusReader(listener) as reader:
            node = session.start_node(name=f"node-{session.transport}-responses")
            wait_for_status(reader, node)
            with Uds(session.can_spec) as uds:
                for _ in range(count):
                    answer = uds.request(LONGEST_REQUEST)
                    if not answer.positive:
                        raise runner.BenchFailure(f"{LONGEST_REQUEST} answered "
                                                  f"{answer.response.hex(' ')}")
                    took.append(answer.took_s)
                    lengths.add(len(answer.response))
            stats = session.finish(node)          # before the reader ends, as above
    finally:
        listener.shutdown()
    task = stats["diag"]["task"]
    return {"took_s": took, "lengths": sorted(lengths), "frames": wire_frames(reader.frames),
            "node": {"build_id": stats["build_id"],
                     "read_dtc_requests": stats["diag"]["uds"]["requests"]["ReadDTCInformation"],
                     "task": {key: task[key] for key in ("events", "passes", "budget_waits")}}
            }, stats["build_id"]


# ------------------------------------------------------------------------------- analysis


def analyse_block_size(raw: dict) -> dict:
    """The figures of one block size from its raw material (measure_block_size()): the tester's
    times, the wire's decomposition of the full blocks, the node's counts, the event floor and
    the bus time. Only full blocks (of `data_bytes`) whose tester's next request is on the wire
    are in the wire's distributions; a block whose stamps cannot be right
    (block_clock_problems()) is counted (clock_disagreements, by reason) and left out."""
    data_bytes = raw["data_bytes"]
    times = raw["times"]
    plan: list[int] = times["plan"]
    tester_blocks: list[float] = times["blocks_s"]
    starts: list[float] = times["starts_s"]
    wire = indexed(raw["frames"])
    messages, strays = isotp_messages(wire)
    timelines = transfer_timelines(messages)
    if len(timelines) != len(plan):
        raise runner.BenchFailure(f"blocks of {data_bytes}: {len(plan)} sent, {len(timelines)} "
                                  "TransferData requests answered on the wire")
    full: list[BlockTimeline] = []
    disagreements = {"blocks": 0, "order": 0, "on_wire": 0, "period": 0}
    differences = []
    for index, (timeline, length) in enumerate(zip(timelines, plan)):
        if timeline.data_bytes != length or timeline.counter != (index + 1) & 0xFF:
            raise runner.BenchFailure(
                f"blocks of {data_bytes}: block {index + 1} on the wire is counter "
                f"{timeline.counter} of {timeline.data_bytes} bytes, not "
                f"{(index + 1) & 0xFF} of {length}")
        if timeline.response[:2] != bytes([0x76, timeline.counter]):
            raise runner.BenchFailure(f"block {index + 1} answered {timeline.response.hex(' ')}")
        differences.append(tester_blocks[index] - timeline.on_wire_s)
        tester_period = starts[index + 1] - starts[index] if index + 1 < len(starts) else None
        problems = block_clock_problems(timeline, wire, tester_blocks[index], tester_period)
        if problems:
            disagreements["blocks"] += 1
            for problem in problems:
                disagreements[problem] += 1
        elif length == data_bytes and timeline.period_s is not None:
            full.append(timeline)
    frames, cfs = isotp_frames(data_bytes + flasher.BLOCK_OVERHEAD)
    fcs = flow_controls(cfs, NODE_BS)
    events = frames + fcs
    write = raw["node"]["write_jobs"]
    write_mean_s = write["total_ns"] / write["run"] / 1e9 if write["run"] else None
    full_tester = [tester_blocks[index] for index, length in enumerate(plan)
                   if length == data_bytes]
    periods = [t.period_s for t in full]
    others = raw["others"]
    telemetry = [bus_time_between(others, t.start, t.start + t.period_s) for t in full]
    node_download = raw["node"]["download"]
    transfer_s = times["transfer_s"]

    def dist(values: Iterable[float | None]) -> dict:
        return results.distribution([v for v in values if v is not None], "ms", scale=1e3)

    return {
        "data_bytes": data_bytes,
        "message_bytes": data_bytes + flasher.BLOCK_OVERHEAD,
        "frames_to_node": frames,
        "flow_controls": fcs,
        "events": events,
        "event_floor_ms": ms(event_floor_s(events)),
        "image_bytes": raw["image_bytes"],
        "blocks": len(plan),
        "full_blocks": len(full_tester),
        "full_blocks_decomposed": len(full),
        "clock_disagreements": disagreements,
        "stray_frames": strays,
        "response_pendings": sum(t.pendings for t in timelines),
        "throughput": {
            "unit": "bytes/s",
            "tester": round(raw["image_bytes"] / transfer_s, 1),
            "node": round(node_download["received"] / (node_download["transfer_us"] / 1e6), 1)
            if node_download["transfer_us"] else None,
            # The median block's rate, under the percentile rule of every result (a median
            # needs results.smallest_n(50) blocks).
            "per_block_p50": round(data_bytes / float(np.median(full_tester)), 1)
            if len(full_tester) >= results.smallest_n(50) else None,
        },
        "transfer_s": round(transfer_s, 6),
        "erase_s": round(times["erase_s"], 6),
        "tester_block_ms": dist(full_tester),
        "wire": {
            "period_ms": dist(periods),
            "on_wire_ms": dist(t.on_wire_s for t in full),
            "node_ms": dist(t.node_s for t in full),
            "tester_ms": dist(t.tester_s for t in full),
            "node_flow_controls_ms": dist(sum(t.node_fc_s) for t in full),
            "node_flow_control_each_ms": dist(v for t in full for v in t.node_fc_s),
            "tester_bursts_ms": dist(sum(t.tester_fc_s) + sum(t.tester_burst_s) for t in full),
            "node_final_ms": dist(t.node_final_s for t in full),
            "turnaround_ms": dist(t.turnaround_s for t in full),
            "bus_ms": dist(t.bus_s for t in full),
            "telemetry_bus_ms": dist(telemetry),
            "tester_minus_wire_ms": dist(differences),
        },
        "write_job_ms": {"unit": "ms", "jobs": write["run"], "mean": ms(write_mean_s),
                         "max": ms(write["max_ns"] / 1e9), "last": ms(write["last_ns"] / 1e9)},
        # The node's last turn (the request's last frame to the response), in its parts: the
        # consecutive frames after the last flow control, taken at the budget (computed), the
        # write job (the node's clock), and what is left: the job's start and end, the response.
        "node_final_parts_ms": final_parts(cfs, fcs, mean(t.node_final_s for t in full),
                                           write_mean_s),
        "budget_waits_per_block": round(raw["node"]["task"]["budget_waits"] / len(plan), 2),
        "node": raw["node"],
        "raw": {"tester_block_ms": results.raw_samples(tester_blocks, "ms", scale=1e3),
                "period_ms": results.raw_samples(periods, "ms", scale=1e3),
                "node_ms": results.raw_samples([t.node_s for t in full], "ms", scale=1e3),
                "node_final_ms": results.raw_samples([t.node_final_s for t in full], "ms",
                                                     scale=1e3)},
    }


def analyse_responses(raw: dict) -> dict:
    """The figures of the node's longest response from measure_responses()'s raw material. A
    response whose stamps go back, or whose wire time exceeds the tester's own time of the
    request by more than CLOCK_SLACK_S, is counted (clock_disagreements) and left out."""
    wire = indexed(raw["frames"])
    messages, strays = isotp_messages(wire)
    sid = int(LONGEST_REQUEST.split()[0], 16)
    every = response_timelines(messages, sid)
    took = raw["took_s"]
    if len(every) != len(took):
        raise runner.BenchFailure(f"{len(took)} requests {LONGEST_REQUEST}, {len(every)} "
                                  "complete responses on the wire")
    timelines = [t for t, tester in zip(every, took)
                 if in_order(wire, t.first_index, t.last_index)
                 and t.total_s - tester <= CLOCK_SLACK_S]
    length = every[0].length if every else None
    frames, cfs = isotp_frames(length) if length else (None, None)

    def dist(values: Iterable[float]) -> dict:
        return results.distribution(list(values), "ms", scale=1e3)

    totals = [t.total_s for t in timelines]
    return {
        "request": LONGEST_REQUEST,
        "response_bytes": length,
        "response_lengths_seen": raw["lengths"],
        "frames": frames,
        "consecutive_frames": cfs,
        "requests": len(took),
        "responses_on_the_wire": len(every),
        "responses_decomposed": len(timelines),
        "clock_disagreements": len(every) - len(timelines),
        "stray_frames": strays,
        "bus_ms": ms(mean(t.bus_s for t in timelines)),
        "throughput": {"unit": "bytes/s",
                       "p50": round(length / float(np.median(totals)), 1)
                       if len(totals) >= results.smallest_n(50) else None,
                       "mean": round(length * len(totals) / float(np.sum(totals)), 1)
                       if totals else None},
        "tester_ms": dist(took),
        "wire": {"total_ms": dist(totals),
                 "node_first_frame_ms": dist(t.node_first_s for t in timelines),
                 "tester_flow_control_ms": dist(t.tester_fc_s for t in timelines),
                 "node_consecutive_frames_ms": dist(t.node_cfs_s for t in timelines),
                 "consecutive_frame_gap_ms": dist(g for t in timelines for g in t.cf_gaps_s)},
        "node": raw["node"],
        "raw": {"total_ms": results.raw_samples(totals, "ms", scale=1e3)},
    }


def fit_and_project(sizes: Sequence[dict]) -> dict:
    """The line through the measured blocks (mean period less mean write job, against the
    events) of FIT_FROM_DATA_BYTES and more, and the projection of PROJECTED_BLOCK_DATA from it,
    with the write job of the largest measured block scaled by its pages. Labelled a projection
    in the result: the node was not rebuilt to measure it."""
    points = [s for s in sizes if s["data_bytes"] >= FIT_FROM_DATA_BYTES
              and s["wire"]["period_ms"]["mean"] is not None and s["write_job_ms"]["mean"]
              is not None]
    if len(points) < 3:
        return {"projection": True, "fitted": False,
                "why": "fewer than three block sizes of more than one frame were measured"}
    x = [s["events"] for s in points]
    y = [(s["wire"]["period_ms"]["mean"] - s["write_job_ms"]["mean"]) / 1e3 for s in points]
    line = fit_line(x, y)
    largest = max(points, key=lambda s: s["data_bytes"])
    write_per_page = largest["write_job_ms"]["mean"] / 1e3 / pages(largest["data_bytes"])
    projected = project_block(line, PROJECTED_BLOCK_DATA, write_per_page)
    # Both sizes by the same line, so that the saving is the line's (128 x the intercept and the
    # events a larger block saves), not the residual of the measured size.
    on_line = project_block(line, largest["data_bytes"], write_per_page)
    measured_period = largest["wire"]["period_ms"]["mean"] / 1e3
    measured_image = image_time_s(PROJECTION_IMAGE_BYTES, largest["data_bytes"], measured_period)
    line_image = image_time_s(PROJECTION_IMAGE_BYTES, largest["data_bytes"], on_line["period_s"])
    projected_image = image_time_s(PROJECTION_IMAGE_BYTES, PROJECTED_BLOCK_DATA,
                                   projected["period_s"])
    return {
        "projection": True,
        "fitted": True,
        "what": (f"period of a block less its write job = intercept + slope x events, fitted to "
                 f"the mean wire periods of the blocks of {FIT_FROM_DATA_BYTES} bytes and more; "
                 f"a block of {PROJECTED_BLOCK_DATA} bytes and one of {largest['data_bytes']} "
                 f"from the line, each with the write job of the {largest['data_bytes']}-byte "
                 "blocks per page times its pages; the saving is the difference of the two for "
                 "the image. Not measured: the node's maxNumberOfBlockLength is 1026."),
        "fit": {"intercept_ms": ms(line["intercept"]), "slope_ms_per_event": ms(line["slope"], 5),
                "max_abs_residual_ms": ms(line["max_abs_residual"]), "r2": round(line["r2"], 5),
                "points": line["n"],
                "budget_ms_per_event": ms(1.0 / TICK_RATE_HZ / EVENTS_PER_TICK, 5)},
        "block": {"data_bytes": PROJECTED_BLOCK_DATA, "events": projected["events"],
                  "event_floor_ms": ms(projected["event_floor_s"]),
                  "write_ms": ms(projected["write_s"]), "period_ms": ms(projected["period_s"]),
                  "bytes_per_s": round(projected["bytes_per_s"], 1)},
        "measured_block": {"data_bytes": largest["data_bytes"], "events": largest["events"],
                           "period_ms": largest["wire"]["period_ms"]["mean"],
                           "period_on_the_line_ms": ms(on_line["period_s"]),
                           "bytes_per_s": round(largest["data_bytes"] / measured_period, 1)},
        "image": {"bytes": PROJECTION_IMAGE_BYTES,
                  "blocks_measured_size": math.ceil(PROJECTION_IMAGE_BYTES
                                                    / largest["data_bytes"]),
                  "blocks_projected_size": math.ceil(PROJECTION_IMAGE_BYTES
                                                     / PROJECTED_BLOCK_DATA),
                  "transfer_s_measured_size": round(measured_image, 3),
                  "transfer_s_measured_size_on_the_line": round(line_image, 3),
                  "transfer_s_projected_size": round(projected_image, 3),
                  "saving_s": round(line_image - projected_image, 3),
                  "saving_percent": round((line_image - projected_image) / line_image * 100, 2)},
    }


def transport_difference(by_transport: dict) -> dict | None:
    """For two transports: the difference of the median wire period (second minus first) per
    block size measured on both, in ms and percent."""
    names = list(by_transport)
    if len(names) != 2:
        return None
    first, second = (by_transport[name]["transfer"] for name in names)
    rows = []
    for a in first:
        b = next((s for s in second if s["data_bytes"] == a["data_bytes"]), None)
        pa, pb = a["wire"]["period_ms"]["p50"], b and b["wire"]["period_ms"]["p50"]
        if pa is None or pb is None:
            continue
        rows.append({"data_bytes": a["data_bytes"], f"{names[0]}_p50_ms": pa,
                     f"{names[1]}_p50_ms": pb, "difference_ms": round(pb - pa, 4),
                     "difference_percent": round((pb - pa) / pa * 100, 3)})
    return {"of": f"{names[1]} - {names[0]}", "period_p50": rows}


def summarise(data: dict) -> dict:
    """The figures the README may quote, each with its unit."""
    summary: dict = {}
    for name, part in data["transports"].items():
        largest = max(part["transfer"], key=lambda s: s["data_bytes"])
        projection = part["projection_2048"]
        response = part["response"]
        group = {
            "block_data_bytes": results.figure(largest["data_bytes"], "bytes"),
            "transfer_throughput": results.figure(largest["throughput"]["tester"], "bytes/s"),
            "block_period": largest["wire"]["period_ms"],
            "block_tester_time": largest["tester_block_ms"],
            "block_event_floor": results.figure(largest["event_floor_ms"], "ms"),
            "block_bus_time": results.figure(largest["wire"]["bus_ms"]["mean"], "ms"),
            "block_node_time": results.figure(largest["wire"]["node_ms"]["mean"], "ms"),
            "block_write_job_mean": results.figure(largest["write_job_ms"]["mean"], "ms"),
            "block_turnaround_mean": results.figure(largest["wire"]["turnaround_ms"]["mean"],
                                                    "ms"),
            "longest_response_bytes": results.figure(response["response_bytes"], "bytes"),
            "longest_response_time": response["wire"]["total_ms"],
            "longest_response_throughput": results.figure(response["throughput"]["p50"],
                                                          "bytes/s"),
        }
        if projection.get("fitted"):
            group["fit_intercept"] = results.figure(projection["fit"]["intercept_ms"], "ms")
            group["fit_slope"] = results.figure(projection["fit"]["slope_ms_per_event"],
                                                "ms/event")
            # Not measured: computed from the measured parts (fit_and_project()).
            for key, value, unit in (
                    ("projected_2048_throughput", projection["block"]["bytes_per_s"], "bytes/s"),
                    ("projected_256KiB_saving", projection["image"]["saving_s"], "s"),
                    ("projected_256KiB_saving_share", projection["image"]["saving_percent"], "%")):
                group[key] = {**results.figure(value, unit), "projection": True}
        summary[name] = group
    return summary


# --------------------------------------------------------------------------------- the plot


def plot(data: dict, path: Path) -> None:
    import matplotlib.pyplot as plt  # noqa: PLC0415
    from matplotlib import ticker  # noqa: PLC0415

    plotstyle.apply()
    fig, (left, middle, right) = plt.subplots(1, 3, figsize=(14.0, 4.9))
    fig.subplots_adjust(left=0.055, right=0.985, top=0.66, bottom=0.15, wspace=0.3)
    transports = list(data["transports"])
    colours = {name: plotstyle.SERIES[i] for i, name in enumerate(transports)}

    first = data["transports"][transports[0]]["transfer"]
    sizes = [s["data_bytes"] for s in first]
    floor = [s["data_bytes"] / (s["event_floor_ms"] / 1e3) / 1e3 for s in first]
    bus_ceiling = [s["data_bytes"] / (s["wire"]["bus_ms"]["mean"] / 1e3) / 1e3
                   if s["wire"]["bus_ms"]["mean"] else math.nan for s in first]
    left.plot(sizes, bus_ceiling, color=plotstyle.INK_MUTED, linewidth=1.0, linestyle=":",
              label="the bus alone (bit counts at 500 kbit/s)")
    left.plot(sizes, floor, color=plotstyle.INK_MUTED, linewidth=1.2, linestyle=(0, (4, 3)),
              label="the event budget alone (2 events per 1 ms tick)")
    for name in transports:
        part = data["transports"][name]["transfer"]
        x = [s["data_bytes"] for s in part]
        y = [s["data_bytes"] / (central(s["wire"]["period_ms"]) / 1e3) / 1e3
             if central(s["wire"]["period_ms"]) else math.nan for s in part]
        left.plot(x, y, color=colours[name], marker="o", markersize=3.5,
                  label=f"measured, {name} (median block period)")
        projection = data["transports"][name]["projection_2048"]
        if projection.get("fitted"):
            left.plot([PROJECTED_BLOCK_DATA], [projection["block"]["bytes_per_s"] / 1e3],
                      marker="o", markersize=5, markerfacecolor="none",
                      markeredgecolor=colours[name], linestyle="none",
                      label=f"projection for {PROJECTED_BLOCK_DATA} bytes, {name} (not measured)")
    left.set_xscale("log", base=2)
    left.set_yscale("log")
    left.set_xlabel("data bytes per TransferData block (log scale)")
    left.set_ylabel("throughput (kB/s, log scale)")
    left.set_title("Tester to node: TransferData by block size")
    plotstyle.legend_above(left)

    # Middle: what is left of a block's period without its write job, against the floor of its
    # events: on the line y = x the event budget alone sets the time.
    low, high = math.inf, 0.0
    for name in transports:
        part = [s for s in data["transports"][name]["transfer"]
                if s["wire"]["period_ms"]["mean"] is not None and s["write_job_ms"]["mean"]
                is not None]
        x = [s["event_floor_ms"] for s in part]
        y = [s["wire"]["period_ms"]["mean"] - s["write_job_ms"]["mean"] for s in part]
        if x:
            low, high = min(low, *x, *y), max(high, *x, *y)
        middle.plot(x, y, color=colours[name], marker="o", markersize=3.5, linestyle="none",
                    label=f"measured, {name}: mean period less the mean write job")
    if high > 0:
        line = [low * 0.8, high * 1.25]
        middle.plot(line, line, color=plotstyle.INK_MUTED, linewidth=1.0, linestyle=(0, (4, 3)),
                    label="the event floor: (events + 1) / 2 ticks of 1 ms")
    middle.set_xscale("log")
    middle.set_yscale("log")
    for axis in (left.yaxis, middle.xaxis, middle.yaxis):
        axis.set_major_formatter(ticker.FuncFormatter(lambda value, _: f"{value:g}"))
        axis.set_minor_formatter(ticker.NullFormatter())
    middle.set_xlabel("event floor of the mean block period (ms, computed, log scale)")
    middle.set_ylabel("measured (ms, log scale)")
    middle.set_title("The event budget sets the floor")
    plotstyle.legend_above(middle)

    # Right: one block of the largest size, where its time goes (means, ms).
    labels, parts = [], []
    for name in transports:
        part = data["transports"][name]
        largest = max(part["transfer"], key=lambda s: s["data_bytes"])
        wire = largest["wire"]
        write = largest["write_job_ms"]["mean"] or 0.0
        final = wire["node_final_ms"]["mean"] or 0.0
        labels.append(f"{name}\n{largest['data_bytes']} bytes")
        parts.append([wire["node_flow_controls_ms"]["mean"] or 0.0, max(final - write, 0.0),
                      write, wire["tester_bursts_ms"]["mean"] or 0.0,
                      wire["turnaround_ms"]["mean"] or 0.0])
    names = ["node: flow controls", "node: last frames and response", "node: write job",
             "tester: bursts", "tester: turnaround"]
    y_positions = np.arange(len(labels))
    lefts = np.zeros(len(labels))
    for index, label in enumerate(names):
        widths = np.array([p[index] for p in parts])
        right.barh(y_positions, widths, left=lefts, height=0.5,
                   color=plotstyle.SERIES[index % len(plotstyle.SERIES)], label=label)
        lefts += widths
    largest = max(first, key=lambda s: s["data_bytes"])
    if largest["write_job_ms"]["mean"] is not None:
        plotstyle.reference_line(right, largest["event_floor_ms"] + largest["write_job_ms"]["mean"],
                                 "event floor + write job")
    if largest["wire"]["bus_ms"]["mean"]:
        plotstyle.reference_line(right, largest["wire"]["bus_ms"]["mean"], "bus time")
    right.set_yticks(y_positions, labels)
    right.set_xlabel("mean time per block (ms): turns on the wire, one clock")
    right.set_title("One block: whose turn it is")
    right.grid(axis="x", color=plotstyle.GRID, linewidth=0.6)
    right.grid(axis="y", visible=False)
    plotstyle.legend_above(right)
    host = "WSL2" if data["provenance"]["host"]["is_wsl"] else "Linux"
    plotstyle.footnote(fig, f"bench/isotp_throughput.py; FreeRTOS POSIX port on a {host} host, "
                            "tester udsoncan/can-isotp in Python. Host timing, not MCU timing; "
                            "hollow markers are projections.")
    plotstyle.save(fig, path)


# ------------------------------------------------------------------------------- the bench


def config_of(settings: dict) -> dict:
    return {
        "bitrate_bit_per_s": BITRATE,
        "transports": list(settings["transports"]),
        "block_sizes_data_bytes": list(settings["block_sizes"]),
        "blocks_per_size": settings["blocks"],
        "min_image_bytes": MIN_IMAGE_BYTES,
        "responses": settings["responses"],
        "longest_request": LONGEST_REQUEST,
        "payload_seed": PAYLOAD_SEED,
        "slot_address": SLOT_B,
        "node_isotp": {"bs": NODE_BS, "stmin": 0, "max_block_length": MAX_BLOCK_LENGTH,
                       "events_per_tick": EVENTS_PER_TICK, "tick_rate_hz": TICK_RATE_HZ},
        "tester_isotp": dict(diag.ISOTP_PARAMS),
        "tester": "udsoncan client of fcn_tools.diag.connect() for the downloads (as the "
                  "flasher); fcn_tools.tester.Uds (can-isotp) for the responses",
        "telemetry": "on (the node's own)",
        "projection": {"data_bytes": PROJECTED_BLOCK_DATA, "image_bytes": PROJECTION_IMAGE_BYTES,
                       "fit_from_data_bytes": FIT_FROM_DATA_BYTES},
        "clock_slack_ms": CLOCK_SLACK_S * 1e3,
    }


def measure(settings: dict, binaries: runner.Binaries) -> tuple[dict, dict[str, str]]:
    """Every run of the bench; the data and the build ids the nodes reported."""
    reported: dict[str, str] = {}
    data: dict = {"transports": {}}
    workdir = runner.bench_workdir(NAME)
    factory = factory_image(workdir)
    for transport in settings["transports"]:
        with runner.Session(workdir / transport, transport, binaries=binaries) as session:
            sizes = []
            for data_bytes in settings["block_sizes"]:
                print(f"{transport}: blocks of {data_bytes} bytes ...", flush=True)
                raw, build_id = measure_block_size(session, data_bytes, settings["blocks"],
                                                   factory)
                reported[f"node {transport} blocks of {data_bytes}"] = build_id
                sizes.append(analyse_block_size(raw))
                done = sizes[-1]
                print(f"  {done['blocks']} blocks, median {done['tester_block_ms']['p50']} ms "
                      f"(floor {done['event_floor_ms']} ms), {done['throughput']['tester']} "
                      "bytes/s", flush=True)
            print(f"{transport}: {settings['responses']} x {LONGEST_REQUEST} ...", flush=True)
            raw, build_id = measure_responses(session, settings["responses"])
            reported[f"node {transport} responses"] = build_id
            response = analyse_responses(raw)
        data["transports"][transport] = {"transfer": sizes, "response": response,
                                         "projection_2048": fit_and_project(sizes)}
    data["transport_difference"] = transport_difference(data["transports"])
    return data, reported


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    results.add_bench_arguments(parser, quick=f"UDP only, blocks of {QUICK['block_sizes']} bytes,"
                                              f" {QUICK['blocks']} of each, "
                                              f"{QUICK['responses']} responses")
    parser.add_argument("--transports", default=",".join(DEFAULT_TRANSPORTS),
                        help="comma-separated: socketcan, udp (default both)")
    parser.add_argument("--block-sizes", default=",".join(map(str, DEFAULT_BLOCK_SIZES)),
                        help=f"data bytes per block, comma-separated, 1 to {MAX_BLOCK_DATA} "
                             "(default %(default)s)")
    parser.add_argument("--blocks", type=int, default=DEFAULT_BLOCKS,
                        help="blocks per size (default %(default)s; at least "
                             f"{MIN_IMAGE_BYTES} bytes are sent)")
    parser.add_argument("--responses", type=int, default=DEFAULT_RESPONSES,
                        help=f"requests {LONGEST_REQUEST} per transport (default %(default)s)")
    args = parser.parse_args(argv)
    out = results.output_dir(args)
    json_path = out / f"{NAME}.json"
    if args.replot:
        if not json_path.exists():
            print(f"nothing to replot: {json_path} does not exist", file=sys.stderr)
            return 1
        plot(json.loads(json_path.read_text()), json_path.with_suffix(".png"))
        print(f"redrew {json_path.with_suffix('.png')}")
        return 0
    try:
        if args.quick:
            settings = dict(QUICK)
        else:
            settings = {"transports": tuple(t for t in args.transports.split(",") if t),
                        "block_sizes": tuple(int(s) for s in args.block_sizes.split(",") if s),
                        "blocks": args.blocks, "responses": args.responses}
    except ValueError as error:
        parser.error(str(error))
    if (any(t not in runner.TRANSPORTS for t in settings["transports"])
            or not settings["transports"]
            or any(not 1 <= s <= MAX_BLOCK_DATA for s in settings["block_sizes"])
            or settings["blocks"] < 2 or settings["responses"] < 1):
        parser.error(f"transports from {runner.TRANSPORTS}, block sizes 1 to {MAX_BLOCK_DATA}, "
                     "at least 2 blocks and 1 response")
    try:
        load = results.check_load(args.max_load, args.allow_loaded)
        binaries = runner.prepare_binaries(build=not args.no_build)
        results.require_tree_build(binaries.as_dict())
        data, reported = measure(settings, binaries)
        path = results.write_result(NAME, data, summarise(data), config_of(settings), out,
                                    version=VERSION, timing_note=TIMING_NOTE, load=load,
                                    binaries=binaries.as_dict(), reported_build_ids=reported,
                                    binary_sha256=binaries.sha256)
    except results.HostLoaded as error:
        print(error, file=sys.stderr)
        return results.EXIT_HOST_LOADED
    except results.BuildIdMismatch as error:
        print(f"no result written: {error}", file=sys.stderr)
        return results.EXIT_BUILD_MISMATCH
    except KeyboardInterrupt as error:
        return runner.interrupted_status(error)
    except AssertionError as error:      # runner.BenchFailure, the tester's checks
        print(f"no result written: {error}", file=sys.stderr)
        return 1
    except Exception as error:  # noqa: BLE001 (udsoncan, python-can, the files: say what)
        traceback.print_exc()
        print(f"no result written: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    plot(json.loads(path.read_text()), path.with_suffix(".png"))
    for name, group in json.loads(path.read_text())["summary"].items():
        print(f"{name}: blocks of {group['block_data_bytes']['value']} bytes "
              f"{group['transfer_throughput']['value']} bytes/s, median block "
              f"{group['block_period']['p50']} ms (event floor "
              f"{group['block_event_floor']['value']}"
              f" ms, bus {group['block_bus_time']['value']} ms); "
              f"{group['longest_response_bytes']['value']}-byte response "
              f"{group['longest_response_time']['p50']} ms")
    print(f"wrote {path} and {path.with_suffix('.png')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
