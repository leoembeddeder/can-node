# SPDX-License-Identifier: MIT
"""An observer of the bus that keeps every frame with the kernel's time of its arrival:
python -m fcn_tools.capture, run by bench/node_timing.py and bench/bus_load.py.

    python -m fcn_tools.capture --can SPEC --out FILE [--seconds S] [--ready FILE]

SPEC is a bus as the node takes it: socketcan:<interface> (a raw CAN socket) or udp:<base port>
(a participant of the UDP bus, on the first free one of its 16 ports, fcn_tools.udpbus). It
records until SIGTERM or SIGINT, or for S seconds, then writes FILE (the frames, CAPTURE_DTYPE,
in the order of their arrival) and FILE.json (what the capture knows about itself, below), and
exits 0; 1 if it could not capture at all. --ready names a file it creates once the socket
receives, so that a bench starts what it observes only after that.

THE TIME OF A FRAME is the kernel's receive timestamp of the socket (SO_TIMESTAMPNS), taken when
the frame or datagram was queued to the socket, not when Python read it: on vcan0 that is the
moment the sender's write reached the interface's loopback, on the UDP bus the moment the
datagram reached the observer's socket. The kernel gives it on CLOCK_REALTIME, and a bench needs
it on CLOCK_MONOTONIC, the clock of the node's own times. The two run at the same rate (the
kernel slews both alike), but the wall clock steps: under WSL2 the VM's time is set from the
host's again and again (systemd-resolved logged a "clock change" every 34 s on the development
laptop, and one capture saw a step of 1.79 s). So the capture reads the offset of CLOCK_REALTIME
from CLOCK_MONOTONIC after every frame it takes (a MONOTONIC, a REALTIME and a MONOTONIC reading,
against the middle of the two MONOTONIC ones) and keeps it with the frame, with the bracket of
the reading and the MONOTONIC time of the read: monotonic = realtime - offset (to_monotonic()). A
frame whose offset differs by more than OFFSET_NOISE_NS from the last good reading before it may
have arrived before the step and been read after it (so may the frames read after it in the
same backlog, whose times lie before its read): its time is not trusted (to_monotonic()'s mask),
and the sidecar counts the steps. A reading with a bracket wider than BRACKET_LIMIT_NS was
interrupted (its offset can be off by half of it; on the development laptop 8 to 41 % of the
readings of a run were, nearly all of them the first reading after the capture's thread had
waited a millisecond or more for a frame): its frame takes the offset of the good readings around
it, which agree when no step came between them, and the sidecar counts the wide brackets. The
offset at the start and at the end, each the best of OFFSET_TRIES readings, is in the sidecar as
well.

WHAT IT CANNOT SEE is counted: the frames the kernel dropped because the socket's buffer was
full (SO_MEMINFO's drop counter, "kernel_drops"), and on the UDP bus the datagrams that are no
frame of the bus ("malformed": wrong size, bytes that must be zero, a source that is not a port
of the bus). A bench refuses a capture with drops.

FILE.json: {"schema": "fcn.capture/1", "can", "frames", "kernel_drops", "malformed",
"receive_buffer_bytes", "offset_start": {"offset_ns", "bracket_ns", "monotonic_ns"},
"offset_end": {...}, "clock_steps", "largest_clock_step_ns", "wide_brackets",
"started_monotonic_ns", "stopped_monotonic_ns", "interrupted_by"}.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import signal
import socket
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from . import udpbus

SCHEMA = "fcn.capture/1"

#: One frame of a capture file: the kernel's receive time (CLOCK_REALTIME, ns), the time the
#: capture read it (CLOCK_MONOTONIC, ns), the offset of CLOCK_REALTIME from CLOCK_MONOTONIC then
#: (ns) and the bracket of that reading (ns, the time between its two MONOTONIC readings), the
#: can_id of struct can_frame (identifier and the EFF/RTR/ERR flags), the DLC and the data.
CAPTURE_DTYPE = np.dtype([("realtime_ns", "<i8"), ("read_ns", "<i8"), ("offset_ns", "<i8"),
                          ("bracket_ns", "<u4"), ("can_id", "<u4"), ("dlc", "u1"),
                          ("pad", "u1", (7,)), ("data", "u1", (8,))])
_RECORD = struct.Struct("<qqqIIB7x8s")
assert _RECORD.size == CAPTURE_DTYPE.itemsize

#: Readings of the clock offset at each end of a capture; the narrowest bracket is kept.
OFFSET_TRIES = 200

#: Two readings of the offset closer than this are the same offset: the readings' own
#: uncertainty is their bracket, about 100 ns on the development laptop.
OFFSET_NOISE_NS = 2_000

#: A reading of the offset whose two MONOTONIC readings lie further apart than this was
#: interrupted (the host took the capture's thread away between them): its offset can be off by
#: half of it, and the frame's time is not trusted.
BRACKET_LIMIT_NS = 2_000

#: The flags of can_id (linux/can.h).
EFF_FLAG = 0x8000_0000
RTR_FLAG = 0x4000_0000
ERR_FLAG = 0x2000_0000
EFF_MASK = 0x1FFF_FFFF
SFF_MASK = 0x7FF

# Not in Python 3.12's socket module; the values of asm-generic/socket.h (x86-64, arm64).
_SO_TIMESTAMPNS = 35
_TIMESPEC = struct.Struct("@ll")
_CAN_FRAME = struct.Struct("<IB3x8s")
_ANCILLARY = socket.CMSG_SPACE(_TIMESPEC.size)
_PR_SET_PDEATHSIG = 1

#: How long a receive waits before the loop looks at the stop flag again.
POLL_S = 0.1


@dataclass(frozen=True)
class ClockOffset:
    """CLOCK_REALTIME minus CLOCK_MONOTONIC, in ns, with the bracket of the reading that gave it
    (the time between the two MONOTONIC readings around the REALTIME one) and when it was taken
    (CLOCK_MONOTONIC)."""
    offset_ns: int
    bracket_ns: int
    monotonic_ns: int

    def as_json(self) -> dict:
        return {"offset_ns": self.offset_ns, "bracket_ns": self.bracket_ns,
                "monotonic_ns": self.monotonic_ns}


def clock_offset(tries: int = OFFSET_TRIES, *, monotonic=time.monotonic_ns,
                 realtime=lambda: time.clock_gettime_ns(time.CLOCK_REALTIME)) -> ClockOffset:
    """The offset of CLOCK_REALTIME from CLOCK_MONOTONIC: of `tries` readings (a MONOTONIC, a
    REALTIME and a MONOTONIC reading each), the one with the narrowest bracket, its REALTIME
    reading against the middle of its two MONOTONIC readings. The clocks are parameters so that
    a test can give its own."""
    best: ClockOffset | None = None
    for _ in range(tries):
        before = monotonic()
        real = realtime()
        after = monotonic()
        bracket = after - before
        if best is None or bracket < best.bracket_ns:
            middle = before + bracket // 2
            best = ClockOffset(offset_ns=real - middle, bracket_ns=bracket, monotonic_ns=middle)
    assert best is not None
    return best


def clock_steps(offsets: np.ndarray, brackets: np.ndarray | None = None) -> np.ndarray:
    """Where the wall clock stepped: True for a frame whose offset, read with a bracket within
    BRACKET_LIMIT_NS, differs by more than OFFSET_NOISE_NS from the last such reading before it
    (a reading with a wider bracket is no evidence either way, and is skipped). The first good
    reading is no step."""
    offsets = np.asarray(offsets, dtype=np.int64)
    good = (np.ones(offsets.shape, dtype=bool) if brackets is None
            else np.asarray(brackets, dtype=np.int64) <= BRACKET_LIMIT_NS)
    if offsets.size == 0:
        return np.zeros(0, dtype=bool)
    last_good = np.maximum.accumulate(np.where(good, np.arange(offsets.size), -1))
    before = np.concatenate(([-1], last_good[:-1]))
    moved = np.abs(offsets - offsets[np.maximum(before, 0)]) > OFFSET_NOISE_NS
    return good & (before >= 0) & moved


def to_monotonic(frames: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """The frames' kernel times on CLOCK_MONOTONIC (ns, int64) and which of them to trust.

    The offset of the wall clock only changes when it steps, so between two good readings (a
    bracket within BRACKET_LIMIT_NS) that agree it is the same for every frame read in between.
    A frame read with a good reading is put on MONOTONIC by its own offset; a frame whose reading
    was interrupted (a wider bracket: its offset can be off by half of it) by the offset of the
    next good reading, provided the good reading before it agrees (no step between them).

    Not trusted: the frame at which a step is seen (it may have arrived before the step); a
    frame with a wide bracket whose next good reading saw a step, or that has no good reading
    on one side (the step could have come between it and the one there is); a frame whose time
    would come after its own read; and a frame whose time lies at or before the read of the
    last frame at which a step was seen. That last rule covers a backlog: frames that queued up
    while the capture did not run and were read one after the other once it ran again. If the
    wall clock stepped while they waited, every one of them after the first is read with the new
    offset too, and a frame that arrived before the step then gets a time off by the step (a
    forward step puts it that much early, which no other rule sees). The step happened before
    the read that saw it, so a frame whose time says it arrived before that read may have
    arrived before the step."""
    brackets = frames["bracket_ns"].astype(np.int64)
    read = frames["read_ns"].astype(np.int64)
    offsets = frames["offset_ns"].astype(np.int64)
    realtime = frames["realtime_ns"].astype(np.int64)
    count = read.size
    if count == 0:
        return realtime - offsets, np.zeros(0, dtype=bool)
    index = np.arange(count)
    good = brackets <= BRACKET_LIMIT_NS
    steps = clock_steps(offsets, brackets)
    previous = np.maximum.accumulate(np.where(good, index, -1))
    following = np.minimum.accumulate(np.where(good, index, count)[::-1])[::-1]
    has_both = (previous >= 0) & (following < count)
    following_ = np.minimum(following, count - 1)
    offset = np.where(good, offsets, offsets[following_])
    times = realtime - offset
    # A wide bracket is put on its neighbours' offset only where they bracket no step.
    covered = good | (has_both & ~steps[following_])
    # The read of the last step seen at or before each frame (none: the smallest int64).
    seen_at = np.maximum.accumulate(np.where(steps, read, np.iinfo(np.int64).min))
    trusted = covered & ~steps & (times <= read) & (times > seen_at)
    return times, trusted


def identifiers(frames: np.ndarray) -> np.ndarray:
    """The identifiers of captured frames, without the flags."""
    can_id = frames["can_id"].astype(np.uint32)
    return np.where((can_id & EFF_FLAG) != 0, can_id & EFF_MASK, can_id & SFF_MASK)


def _edges_from(ids: np.ndarray, times: np.ndarray, trusted: np.ndarray | None) -> tuple:
    ids, times = np.asarray(ids), np.asarray(times)
    if trusted is None:
        return ids, times
    keep = np.asarray(trusted, dtype=bool)
    return ids[keep], times[keep]


def steady_window(ids: np.ndarray, times: np.ndarray, marker: int,
                  settle_ns: int, trusted: np.ndarray | None = None) -> tuple[int, int]:
    """A run without its start: from `settle_ns` after the first frame of identifier `marker`
    (a frame the node sends from its first period on, such as its heartbeat) to the last one.
    With `trusted` (to_monotonic()'s mask) only frames whose times are trusted mark the edges:
    a time across a step of the wall clock can be off by the step, a second or more, and would
    move an edge by that much (the start into the run's start-up, or a window's length off).
    ValueError if there is no such frame, or nothing is left after the start."""
    ids, times = _edges_from(ids, times, trusted)
    mine = times[ids == marker]
    if mine.size < 2:
        raise ValueError(f"fewer than two frames of {marker:#05x}"
                         + (" with a trusted time" if trusted is not None else ""))
    start, end = int(mine.min()) + int(settle_ns), int(mine.max())
    if end <= start:
        raise ValueError(f"the frames of {marker:#05x} span less than {settle_ns} ns")
    return start, end


def exchange_window(ids: np.ndarray, times: np.ndarray, first_id: int,
                    last_id: int, trusted: np.ndarray | None = None) -> tuple[int, int]:
    """The span of an exchange (a download): from the first frame of `first_id` (the tester's
    requests) to the last frame of `last_id` (the node's responses); with `trusted`, of those
    whose times are trusted (see steady_window()). ValueError without either."""
    ids, times = _edges_from(ids, times, trusted)
    first, last = times[ids == first_id], times[ids == last_id]
    if first.size == 0 or last.size == 0:
        raise ValueError(f"no frame of {first_id:#05x} or of {last_id:#05x}"
                         + (" with a trusted time" if trusted is not None else ""))
    return int(first.min()), int(last.max())


def read(path: Path) -> tuple[np.ndarray, dict]:
    """A capture file and its sidecar (FILE.json)."""
    path = Path(path)
    sidecar = json.loads(Path(f"{path}.json").read_text())
    if sidecar.get("schema") != SCHEMA:
        raise ValueError(f"{path}.json is not a capture's sidecar ({sidecar.get('schema')!r})")
    frames = np.fromfile(path, dtype=CAPTURE_DTYPE)
    if frames.size != sidecar["frames"]:
        raise ValueError(f"{path} holds {frames.size} frames, its sidecar says "
                         f"{sidecar['frames']}")
    return frames, sidecar


# ------------------------------------------------------------------------------- the sockets


def _timestamp(ancillary) -> int | None:
    for level, kind, payload in ancillary:
        if (level == socket.SOL_SOCKET and kind == _SO_TIMESTAMPNS
                and len(payload) >= _TIMESPEC.size):
            seconds, nanoseconds = _TIMESPEC.unpack(payload[:_TIMESPEC.size])
            return seconds * 1_000_000_000 + nanoseconds
    return None


class _Source:
    """One socket of the observer; receive() returns (realtime_ns, can_id, dlc, data) or None."""

    def __init__(self, spec: str) -> None:
        kind, _, rest = spec.partition(":")
        self.kind = kind
        self.malformed = 0
        if kind == "socketcan":
            sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
            sock.setsockopt(socket.SOL_SOCKET, _SO_TIMESTAMPNS, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, udpbus.RECEIVE_BUFFER_BYTES)
            sock.bind((rest or "vcan0",))
            self.base_port = None
        elif kind == "udp":
            base = int(rest) if rest else udpbus.DEFAULT_BASE_PORT
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, _SO_TIMESTAMPNS, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, udpbus.RECEIVE_BUFFER_BYTES)
            for slot in range(udpbus.SLOTS):
                try:
                    sock.bind(("127.0.0.1", base + slot))
                    break
                except OSError:
                    continue
            else:
                sock.close()
                raise OSError(f"every port of the UDP bus {base} is taken")
            self.base_port = base
        else:
            raise ValueError(f"a bus is socketcan:<interface> or udp:<base port>, not {spec}")
        sock.settimeout(POLL_S)
        self.sock = sock

    @property
    def receive_buffer_bytes(self) -> int:
        return int(self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF))

    def drops(self) -> int:
        return udpbus.socket_drops(self.sock)

    def receive(self):
        try:
            payload, ancillary, _flags, address = self.sock.recvmsg(64, _ANCILLARY)
        except (socket.timeout, InterruptedError):
            return None
        stamp = _timestamp(ancillary)
        if self.kind == "udp":
            host, port = address[0], address[1]
            if (len(payload) != 16 or host != "127.0.0.1"
                    or not self.base_port <= port < self.base_port + udpbus.SLOTS
                    or payload[5:8] != b"\0\0\0"):
                self.malformed += 1
                return None
        elif len(payload) < 16:
            self.malformed += 1
            return None
        can_id, dlc, data = _CAN_FRAME.unpack(payload[:16])
        if stamp is None or dlc > 8:
            self.malformed += 1
            return None
        return stamp, can_id, dlc, data

    def close(self) -> None:
        self.sock.close()


def _die_with_parent() -> None:
    """Ask the kernel to end this process when the one that started it ends (as the node does,
    node_main.c's FCN_DIE_WITH_PARENT), so that a bench killed with SIGKILL leaves no observer."""
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        libc.prctl(_PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0)
    except (OSError, AttributeError):
        pass


def capture(spec: str, out: Path, seconds: float | None = None,
            ready: Path | None = None) -> dict:
    """Capture `spec` into `out` until SIGTERM, SIGINT or `seconds`; the sidecar written."""
    stop: dict[str, str | None] = {"by": None}

    def _stop(signum, _frame):
        stop["by"] = signal.Signals(signum).name

    previous = {signum: signal.signal(signum, _stop) for signum in (signal.SIGTERM, signal.SIGINT)}
    source = _Source(spec)
    records = bytearray()
    count = 0
    monotonic = time.monotonic_ns
    clock_gettime_ns = time.clock_gettime_ns
    realtime = time.CLOCK_REALTIME

    def keep(got) -> None:
        # The offset of the wall clock at the read, against the middle of two MONOTONIC readings.
        before = monotonic()
        real = clock_gettime_ns(realtime)
        after = monotonic()
        stamp, can_id, dlc, data = got
        records.extend(_RECORD.pack(stamp, after, real - (before + after) // 2,
                                    min(after - before, 0xFFFF_FFFF), can_id, dlc, data))

    try:
        start = clock_offset()
        started = time.monotonic_ns()
        if ready is not None:
            Path(ready).write_text(f"{os.getpid()}\n")
        deadline = None if seconds is None else started + int(seconds * 1e9)
        while stop["by"] is None and (deadline is None or time.monotonic_ns() < deadline):
            got = source.receive()
            if got is not None:
                keep(got)
                count += 1
        stopped = time.monotonic_ns()
        # Whatever is still queued arrived before the stop: take it too.
        source.sock.setblocking(False)
        while True:
            try:
                got = source.receive()
            except BlockingIOError:
                break
            if got is None:
                break
            keep(got)
            count += 1
        end = clock_offset()
        drops = source.drops()
        taken = np.frombuffer(bytes(records), dtype=CAPTURE_DTYPE)
        good = taken[taken["bracket_ns"] <= BRACKET_LIMIT_NS]["offset_ns"].astype(np.int64)
        moves = np.abs(np.diff(good))
        sizes = moves[moves > OFFSET_NOISE_NS]
        sidecar = {"schema": SCHEMA, "can": spec, "frames": count, "kernel_drops": drops,
                   "malformed": source.malformed,
                   "receive_buffer_bytes": source.receive_buffer_bytes,
                   "offset_start": start.as_json(), "offset_end": end.as_json(),
                   "clock_steps": int(sizes.size),
                   "largest_clock_step_ns": int(sizes.max()) if sizes.size else 0,
                   "wide_brackets": int((taken["bracket_ns"] > BRACKET_LIMIT_NS).sum()),
                   "started_monotonic_ns": started, "stopped_monotonic_ns": stopped,
                   "interrupted_by": stop["by"]}
    finally:
        source.close()
        for signum, handler in previous.items():
            signal.signal(signum, handler)
    Path(out).write_bytes(bytes(records))
    Path(f"{out}.json").write_text(json.dumps(sidecar, indent=2) + "\n")
    return sidecar


#: How long Observer waits for the capture to receive, and for it to end after SIGTERM: bounds.
READY_TIMEOUT_S = 20.0
STOP_TIMEOUT_S = 20.0


class Observer:
    """A capture in a process of its own (python -m fcn_tools.capture) for a bench: started by
    start(), which returns once its socket receives; stop() ends it and returns its frames and
    sidecar. A process of its own, so that the bench's other threads (a flood, the flasher's
    waits) never hold up the reads, and the socket's buffer never fills while Python is busy
    elsewhere."""

    def __init__(self, spec: str, out: Path) -> None:
        self.spec = spec
        self.out = Path(out)
        self.ready = Path(f"{out}.ready")
        self._proc: "subprocess.Popen | None" = None

    def start(self) -> "Observer":
        import subprocess  # noqa: PLC0415 - only a bench needs it

        for path in (self.out, Path(f"{self.out}.json"), self.ready):
            path.unlink(missing_ok=True)
        self._stderr = open(f"{self.out}.stderr.txt", "wb")  # noqa: SIM115 - closed in stop()
        self._proc = subprocess.Popen(
            [sys.executable, "-m", "fcn_tools.capture", "--can", self.spec, "--out",
             str(self.out), "--ready", str(self.ready)],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=self._stderr)
        deadline = time.monotonic() + READY_TIMEOUT_S
        while not self.ready.exists():
            if self._proc.poll() is not None or time.monotonic() > deadline:
                self.kill()
                raise RuntimeError(f"the capture of {self.spec} did not start: "
                                   f"{Path(f'{self.out}.stderr.txt').read_text(errors='replace')}")
            time.sleep(0.01)
        return self

    @property
    def running(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def stop(self) -> tuple[np.ndarray, dict]:
        assert self._proc is not None, "the observer was never started"
        if self._proc.poll() is None:
            self._proc.send_signal(signal.SIGTERM)
        status = self._proc.wait(timeout=STOP_TIMEOUT_S)
        self._stderr.close()
        if status != 0:
            raise RuntimeError(f"the capture of {self.spec} ended with status {status}: "
                               f"{Path(f'{self.out}.stderr.txt').read_text(errors='replace')}")
        return read(self.out)

    def kill(self) -> None:
        if self._proc is not None and self._proc.poll() is None:
            self._proc.kill()
            self._proc.wait(timeout=5)
        if getattr(self, "_stderr", None) is not None and not self._stderr.closed:
            self._stderr.close()

    def __enter__(self) -> "Observer":
        return self.start()

    def __exit__(self, *exc_info) -> None:
        self.kill()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--can", required=True, help="socketcan:<interface> or udp:<base port>")
    parser.add_argument("--out", type=Path, required=True, help="the capture file")
    parser.add_argument("--seconds", type=float, default=None,
                        help="stop after this long (default: at SIGTERM or SIGINT)")
    parser.add_argument("--ready", type=Path, default=None,
                        help="create this file once the socket receives")
    args = parser.parse_args(argv)
    _die_with_parent()
    try:
        capture(args.can, args.out, args.seconds, args.ready)
    except (OSError, ValueError) as error:
        print(f"fcn_tools.capture: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
