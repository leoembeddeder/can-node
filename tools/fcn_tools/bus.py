"""Open the project's CAN bus from one specification string, on either transport.

The node and every tool name a bus the same way:

    udp:29500           the virtual bus over UDP on the loopback interface, base port 29500
    udp                 the same on the default port
    socketcan:vcan0     a Linux SocketCAN interface

so a test or a script written against this module runs unchanged on both.

A frame that a tester has not read yet waits in its socket's receive buffer, and one that
arrives while the buffer is full is dropped by the kernel. open_bus() gives a socket of either
transport the same buffer (fcn_tools.udpbus.RECEIVE_BUFFER_BYTES), and receive_overflow() tells
how many frames did not fit. BusReader reads a bus in a thread of its own, so that frames only
pile up while the whole test process is held up.
"""
from __future__ import annotations

import contextlib
import fcntl
import socket
import struct
import tempfile
import threading
import time
from pathlib import Path
from typing import Callable, Iterable, Iterator

import can

from .udpbus import RECEIVE_BUFFER_BYTES, UdpBus, socket_drops

DEFAULT_UDP_PORT = 29500

# Identifiers that exist before the DBC does; the DBC of phase 2 is the reference after that.
ID_STATUS = 0x030   # node -> bus, every 10 ms in phase 1
ID_PING = 0x6F0     # tester -> node
ID_PONG = 0x6F1     # node -> tester, same payload as the ping


def open_bus(spec: str, **kwargs) -> can.BusABC:
    """Open the bus named by `spec`. Extra keyword arguments go to python-can."""
    kind, _, argument = spec.partition(":")
    if kind == "udp":
        return can.Bus(interface="fcn_udp", channel=int(argument or DEFAULT_UDP_PORT), **kwargs)
    if kind == "socketcan":
        if not argument:
            raise ValueError("socketcan needs an interface, for example socketcan:vcan0")
        bus = can.Bus(interface="socketcan", channel=argument, **kwargs)
        try:
            # The receive buffer UdpBus asks for. The kernel caps the request at
            # net.core.rmem_max and doubles it; with the stock 212,992 bytes a socket then
            # holds 476 unread frames instead of the 238 of its default buffer, as many as a
            # UdpBus (both measured on Linux 6.6).
            bus.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, RECEIVE_BUFFER_BYTES)
        except BaseException:
            bus.shutdown()
            raise
        return bus
    raise ValueError(f"unknown bus '{spec}': use udp[:<base port>] or socketcan:<interface>")


def receive_overflow(bus: can.BusABC) -> int:
    """Frames the kernel dropped because the receive buffer of `bus`, a bus of open_bus(), was
    full: frames the reader lost on its own side, because nothing read the bus for longer than
    the buffer lasts. Exact at the time of the call, on both transports: UdpBus.rx_overflow,
    and on SocketCAN the drop counter of the socket, which counts every frame that did not
    fit (measured: frames received plus frames dropped were the frames sent, Linux 6.6)."""
    if isinstance(bus, UdpBus):
        return bus.rx_overflow
    sock = getattr(bus, "socket", None)
    if not isinstance(sock, socket.socket):
        raise TypeError(f"{type(bus).__name__} is no bus of open_bus(): no socket to ask")
    return socket_drops(sock)


class BusReader:
    """Reads every frame a bus receives, in a thread of its own, from its start until stop().

    A test that only reads at the end leaves everything the node sends meanwhile to the
    socket's buffer, which holds 476 frames with the stock net.core.rmem_max (open_bus()), and
    loses frames whenever it does something else for longer than that lasts. With a reader
    thread only a stall of the whole test process that long can lose frames;
    receive_overflow() says whether one did.

    Use it as a context manager: the reader stops at the end of the block, and if the block
    ended normally it takes the frames still on their way as well (stop()). `frames` is
    complete, and safe to read, once it has stopped.
    """

    def __init__(self, bus: can.BusABC, first: Iterable[can.Message] = ()) -> None:
        self._bus = bus
        #: `first` (frames the caller had already read), then every frame read here.
        self.frames: list[can.Message] = list(first)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="BusReader", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            message = self._bus.recv(timeout=0.05)
            if message is not None:
                self.frames.append(message)

    def _end_thread(self) -> None:
        self._stop.set()
        self._thread.join(timeout=5.0)
        if self._thread.is_alive():
            raise RuntimeError("the reader thread did not stop")

    def stop(self) -> list[can.Message]:
        """Stop reading and return every frame, with those still on their way (drain()): call
        it once the sender has stopped."""
        if not self._stop.is_set():
            self._end_thread()
            self.frames += drain(self._bus)
        return self.frames

    def __enter__(self) -> "BusReader":
        return self

    def __exit__(self, exc_type, *_) -> None:
        if exc_type is None:
            self.stop()
        else:
            self._end_thread()   # no drain: the sender may still be running


def socketcan_available(interface: str = "vcan0") -> bool:
    """True if the SocketCAN interface exists and is up."""
    state = Path("/sys/class/net") / interface / "operstate"
    try:
        return state.read_text().strip() in ("up", "unknown")
    except OSError:
        return False


def socketcan_lock_path(interface: str) -> Path:
    """The lock file of a SocketCAN interface (socketcan_lock())."""
    return Path(tempfile.gettempdir()) / f"fcn-test-{interface}.lock"


@contextlib.contextmanager
def socketcan_lock(interface: str, on_wait: Callable[[], None] | None = None) -> Iterator[None]:
    """Hold the lock of a SocketCAN interface while the block runs. One interface is shared by
    everything on the machine: the test harness (tests/conftest.py) and the benches
    (fcn_tools.runner) take this lock file, so that two test runs, or a test run and a bench,
    never use it at the same time; they still assume that nobody else transmits on it.
    `on_wait` is called once if the lock is held by someone else, before waiting for it."""
    with open(socketcan_lock_path(interface), "w") as lock:
        if on_wait is None:
            fcntl.flock(lock, fcntl.LOCK_EX)
        else:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                on_wait()
                fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def receive_until(bus: can.BusABC, deadline: float) -> Iterator[can.Message]:
    """Yield every frame received until the monotonic time `deadline`."""
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        message = bus.recv(timeout=remaining)
        if message is not None:
            yield message


def collect(bus: can.BusABC, seconds: float,
            keep: Callable[[can.Message], bool] | None = None) -> list[can.Message]:
    """All frames received during `seconds`, optionally only those `keep` accepts.

    The result depends on how fast the host is: use it to let frames pass, or to show that
    nothing arrives. To get a number of frames, use collect_count()."""
    deadline = time.monotonic() + seconds
    return [m for m in receive_until(bus, deadline) if keep is None or keep(m)]


def collect_count(bus: can.BusABC, count: int, keep: Callable[[can.Message], bool] | None = None,
                  timeout: float = 5.0) -> list[can.Message]:
    """The next `count` frames that `keep` accepts. Fewer only if `timeout` runs out first, a
    bound for a node that has stopped sending, not a measurement: a test asserts the length."""
    deadline = time.monotonic() + timeout
    frames: list[can.Message] = []
    for message in receive_until(bus, deadline):
        if keep is None or keep(message):
            frames.append(message)
            if len(frames) == count:
                break
    return frames


def collect_while(bus: can.BusABC, running: Callable[[], bool],
                  keep: Callable[[can.Message], bool] | None = None,
                  timeout: float = 60.0) -> list[can.Message]:
    """Every frame received while `running()` holds (the node runs), then drain(): the frames
    that were still on their way when it stopped. However slowly the host ran the node, this
    listens for its whole run; `timeout` only bounds a node that never ends."""
    deadline = time.monotonic() + timeout
    frames: list[can.Message] = []
    while running() and time.monotonic() < deadline:
        message = bus.recv(timeout=0.05)
        if message is not None and (keep is None or keep(message)):
            frames.append(message)
    return frames + drain(bus, keep=keep)


def drain(bus: can.BusABC, keep: Callable[[can.Message], bool] | None = None,
          quiet: float = 0.2, timeout: float = 10.0) -> list[can.Message]:
    """Every frame the bus still holds, read until none has arrived for `quiet` seconds. Meant
    for after the node has ended: what it sent is waiting in the socket by then, and a reader
    that the host delays still gets all of it, except what arrived unread while the socket was
    full (receive_overflow() counts that). `timeout` only bounds a sender that never stops."""
    deadline = time.monotonic() + timeout
    frames: list[can.Message] = []
    while time.monotonic() < deadline:
        message = bus.recv(timeout=quiet)
        if message is None:
            break
        if keep is None or keep(message):
            frames.append(message)
    return frames


def wait_for(bus: can.BusABC, wanted: Callable[[can.Message], bool],
             timeout: float) -> can.Message | None:
    """The first frame that `wanted` accepts, or None if none arrives within `timeout`."""
    deadline = time.monotonic() + timeout
    for message in receive_until(bus, deadline):
        if wanted(message):
            return message
    return None


def is_status(message: can.Message) -> bool:
    return (message.arbitration_id == ID_STATUS and not message.is_extended_id
            and not message.is_remote_frame)


def is_pong(message: can.Message) -> bool:
    return (message.arbitration_id == ID_PONG and not message.is_extended_id
            and not message.is_remote_frame)


def status_fields(message: can.Message) -> tuple[int, int]:
    """(period counter, tick count) of a status frame: the number of the task's period, from
    1, and the node's tick count when the value was published."""
    if message.dlc != 8:
        raise ValueError(f"a status frame has 8 data bytes, this one {message.dlc}")
    return struct.unpack("<II", bytes(message.data))


# Ticks per period of the status frame (APP_SENSOR_PERIOD_MS in app/app_tasks.h at 1 kHz).
STATUS_PERIOD_TICKS = 10

# The status frame's name in the DBC and in the node's statistics ("telemetry.messages").
STATUS_MESSAGE = "BMS_Heartbeat"


def status_queue_counts(stats: dict) -> dict:
    """The transmit queue's counters of the status frame alone (produced, sent, superseded,
    pending), from the node's statistics. They obey produced = sent + superseded + pending.

    The queue's own counters ("can.queue") count every frame the node sends; since phase 2
    that is more than the status frame, so a test that accounts for status values takes the
    counters of its slot."""
    return stats["telemetry"]["messages"][STATUS_MESSAGE]["queue"]

# Most consecutive status values a busy host can make the node replace before they go out:
# a host that does not run the node for 50 ms. The status message is cyclic, so a value that
# could not leave in time is superseded by the next one (lib/can/can_txq.h), which leaves a
# gap in the counters. A value that waited in a mailbox through a bus fault instead shows up as
# a jump the length of the whole fault, hundreds of milliseconds.
STATUS_MAX_STEP = 5


def status_gaps(frames: list[can.Message], max_step: int | None = STATUS_MAX_STEP) -> int:
    """Check a sequence of status frames as received and return how many values are missing.

    Asserted:
      - the counters strictly increase and no step is larger than `max_step`. The limit
        depends on the host (a stall longer than STATUS_MAX_STEP periods exceeds it), so a
        test that has no bus fault to tell apart from a stall passes None: no limit;
      - the tick counts strictly increase, and no value carries a tick count below
        STATUS_PERIOD_TICKS times its counter: period n of the ticker ends at tick 10 n
        after its start (at tick 0 or later), and the tick is read after that.
    How late after the end of its period a value was published depends on the host; that is
    checked on the wall clock, in a test marked `timing`. The missing values are the ones the
    node replaced before they got out; a test compares that number with the node's own
    statistics.
    """
    fields = [status_fields(frame) for frame in frames]
    for counter, tick in fields:
        assert tick >= STATUS_PERIOD_TICKS * counter, f"status counter {counter} at tick {tick}"
    missing = 0
    for (c0, t0), (c1, t1) in zip(fields, fields[1:]):
        step = c1 - c0
        assert step >= 1 and (max_step is None or step <= max_step), (
            f"status counter {c0} followed by {c1}")
        assert t1 > t0, f"counters {c0}->{c1} but ticks {t0}->{t1}"
        missing += step - 1
    return missing


def status_missing_since_start(frames: list[can.Message],
                               max_step: int | None = STATUS_MAX_STEP) -> int:
    """status_gaps() of everything a tester heard from the node's start on, including the
    values before the first one heard (the counter starts at 1): the number to compare with
    the queue's count of superseded values."""
    if not frames:
        return 0
    return status_fields(frames[0])[0] - 1 + status_gaps(frames, max_step)


def status_publish_lateness(frames: list[can.Message]) -> list[int]:
    """How many ticks after the end of its period each value was published, relative to the
    earliest one in `frames` (the ticker's start tick is not on the bus). 0 for every value on
    a host that runs the node in time."""
    offsets = [tick - STATUS_PERIOD_TICKS * counter
               for counter, tick in (status_fields(frame) for frame in frames)]
    earliest = min(offsets)
    return [offset - earliest for offset in offsets]


def wait_for_id(bus: can.BusABC, arbitration_id: int, timeout: float) -> can.Message | None:
    """The first data frame with this base-format identifier."""
    return wait_for(
        bus,
        lambda m: m.arbitration_id == arbitration_id and not m.is_extended_id
        and not m.is_remote_frame,
        timeout,
    )
