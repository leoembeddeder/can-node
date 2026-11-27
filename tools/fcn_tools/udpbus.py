# SPDX-License-Identifier: MIT
"""The project's virtual CAN bus over UDP, as a python-can interface.

The node's CAN controller model normally sits on Linux SocketCAN (vcan0). Machines whose
kernel has no vcan module, the stock WSL2 kernel for one, use a second transport: a bus made
of UDP datagrams on the loopback interface. platform/posix/can_backend_udp.c is the node's
side of it. This module is the same protocol for python-can, so that tests, measurement
scripts and the stock python-can tools (can.logger, can.viewer) can join that bus::

    bus = can.Bus(interface="fcn_udp", channel=29500)

The protocol
------------
- A bus is 16 UDP ports ("slots") on 127.0.0.1: B .. B+15. The base port B names the bus; it
  is the python-can channel, 29500 by default. There is no broker, and nothing leaves the
  machine.
- A participant owns one slot: it binds the first of the 16 ports that is free. The bind is
  plain, without SO_REUSEADDR and SO_REUSEPORT, so a port that is taken fails to bind, and
  the kernel decides who has which slot without a lock file or a registry. A 17th participant
  finds no slot and is refused.
- To send a frame, a participant sends one datagram to each of the other 15 ports, whether
  anybody is bound there or not (the kernel discards a datagram to an unbound port). It never
  sends to its own port, so it never receives its own frames.
- A datagram is exactly 16 bytes, laid out like Linux's struct can_frame, little-endian::

      bytes 0..3   can_id   bits 0..28 identifier, bit 29 error frame (never sent),
                            bit 30 remote request, bit 31 extended identifier
      byte  4      length   0..8: the DLC, also in a remote request
      bytes 5..7   zero
      bytes 8..15  data     zero beyond the length; all zero in a remote request

- A receiver silently drops, and counts, every datagram that is not exactly that: another
  size, the error flag, a length above 8, an identifier above 0x7FF without the extended
  flag, a non-zero byte where the layout says zero, or a source that is not 127.0.0.1 with
  the port of another slot of the same bus. Every frame therefore has exactly one encoding,
  and a stray datagram from another program cannot become a frame.

What the bus does not model
---------------------------
Every frame reaches every other participant at once and without error. There is no bit
timing, no arbitration and no acknowledgement; the node's controller model adds those above
the transport (lib/can/can_ctrl.h). Two consequences are worth knowing when writing a test:

- Frames of one sender arrive everywhere in the order they were sent, because Linux puts a
  loopback datagram into the receiving socket's queue inside sendto(), in the context of the
  sender. The protocol itself has no sequence numbers and could neither detect nor repair a
  reordering. Frames of different senders have no common order at all: two receivers may see
  them interleaved differently, which cannot happen on a wire, where arbitration puts all
  frames into one sequence.
- UDP has no back-pressure. A receiver that does not read loses frames once its socket
  buffer is full; rx_overflow counts them, and RECEIVE_BUFFER_BYTES says how many fit.

Timestamps
----------
Message.timestamp is the kernel's receive timestamp (SO_TIMESTAMPNS: CLOCK_REALTIME, in
seconds), as with python-can's socketcan interface. It is the time the datagram reached the
socket, not the time Python got round to reading it, which is what makes it usable for
measurements. The option is set before the socket is bound, so that no datagram is queued
without it.

That alone is not enough on a machine where no other socket asks for timestamps. The kernel
then switches arrival timestamping on from a work item, which runs once the CPU is free, and
until it has run a datagram is stamped when it is read. Measured on Linux 6.6 (WSL2): a
process that kept running after setting the option got such read-time stamps for the next 1
to 36 ms, while after any blocking call, even one of 0.2 ms, the first datagram was stamped
on arrival in 64 of 64 trials. The constructor therefore blocks for a millisecond between
setting the option and binding the port: with that pause the first frame after opening a bus
was stamped on arrival in 80 of 80 trials, without it in 20 of 50. It is a remedy that was
measured, not a promise the kernel makes; a measurement that depends on its first frames
should check them.

time.time() is used only if the kernel supplies no timestamp at all.

Threads
-------
One thread may receive while others send. Sending is serialised by a lock, so that every
receiver sees this participant's frames in one and the same order: without it, a frame of
one thread could reach some slots before and others after a frame of another thread.
Receiving from several threads needs can.ThreadSafeBus, as with every python-can interface.
"""
from __future__ import annotations

import contextlib
import errno
import select
import socket
import struct
import sys
import threading
import time
from typing import Any

import can
from can.typechecking import AutoDetectedConfig, CanFilters

__all__ = [
    "DATAGRAM_LEN",
    "DEFAULT_BASE_PORT",
    "RECEIVE_BUFFER_BYTES",
    "SLOTS",
    "UdpBus",
    "decode",
    "encode",
    "socket_drops",
]

#: Base port of the bus that the node and the tools use when none is given
#: (CAN_UDP_DEFAULT_PORT in platform/posix/can_backend.h).
DEFAULT_BASE_PORT = 29500
#: Ports, and therefore participants, of one bus.
SLOTS = 16
#: Size of the one valid datagram: sizeof(struct can_frame).
DATAGRAM_LEN = 16

#: Receive buffer requested from the kernel (SO_RCVBUF); the node asks for the same.
#:
#: Frames that have not been read wait in the socket's receive buffer, and a frame that
#: arrives while it is full is lost (and counted, see UdpBus.rx_overflow). Linux charges a
#: datagram of this size 896 bytes of buffer, the truesize of its sk_buff, whatever its
#: payload (measured on 6.6, x86-64). The default buffer of 212,992 bytes therefore holds 238
#: frames, about 50 ms of a saturated 500 kbit/s bus: less than a Python program can spend
#: in one garbage collection or one plot.
#:
#: The kernel doubles the size it is asked for, to cover that bookkeeping, and silently caps
#: the request of an unprivileged process at net.core.rmem_max. With the stock value of
#: 212,992 this request yields 425,984 bytes, which is 476 frames (measured). With
#: net.core.rmem_max raised to 4 MiB or more it yields 8 MiB, room for about 9,300 frames by
#: the same arithmetic. UdpBus.receive_buffer_bytes tells what a bus got.
RECEIVE_BUFFER_BYTES = 4 * 1024 * 1024

_LOOPBACK = "127.0.0.1"

# Flags in can_id, as in <linux/can.h>.
_EFF_FLAG = 0x8000_0000
_RTR_FLAG = 0x4000_0000
_ERR_FLAG = 0x2000_0000
_ID_MASK = 0x1FFF_FFFF
_ID_BASE_MAX = 0x7FF
_MAX_DLC = 8

# can_id, length, three bytes that must be zero, data. "<" also switches alignment off: in
# struct can_frame it is exactly these three bytes that align the data field to 8 bytes.
_FRAME = struct.Struct("<IB3s8s")
_ZERO_PADDING = bytes(3)

# Not in the socket module of Python 3.12. The values are those of asm-generic/socket.h,
# which x86-64 and arm64 use. SO_TIMESTAMPNS is also the type of the control message that
# carries the timestamp (SCM_TIMESTAMPNS).
_SO_TIMESTAMPNS = 35
_SO_MEMINFO = 55
_SK_MEMINFO_DROPS = 8  # index into the array of 32-bit counters that SO_MEMINFO returns
_SK_MEMINFO_VARS = 9
_TIMESPEC = struct.Struct("@ll")  # struct timespec: tv_sec, tv_nsec
_INT_MAX = 2**31 - 1

# How long the constructor blocks so that the kernel can switch arrival timestamps on; see
# "Timestamps" in the module documentation. Five times the shortest pause that was enough.
_TIMESTAMP_SETTLE_S = 0.001

_IS_LINUX = sys.platform.startswith("linux")
_ANCILLARY_LEN = socket.CMSG_SPACE(_TIMESPEC.size) if _IS_LINUX else 0


def encode(msg: can.Message) -> bytes:
    """The datagram that carries `msg`.

    Raises can.CanOperationError for a CAN FD or an error frame, which this bus does not
    carry, and ValueError for a message that is no valid classical frame: an identifier that
    does not fit its format, a DLC above 8, a data frame whose DLC is not the number of its
    data bytes, or a remote frame with data. Such a message has no encoding, and a receiver
    would drop any attempt at one, so it is refused here, where the mistake was made.
    """
    if msg.is_fd:
        raise can.CanOperationError("the UDP bus carries classical CAN frames only, not CAN FD")
    if msg.is_error_frame:
        raise can.CanOperationError("an error frame cannot be sent on the UDP bus")

    id_max = _ID_MASK if msg.is_extended_id else _ID_BASE_MAX
    if not 0 <= msg.arbitration_id <= id_max:
        raise ValueError(
            f"identifier {msg.arbitration_id:#x} does not fit the "
            f"{'extended' if msg.is_extended_id else 'base'} format (0..{id_max:#x})"
        )
    if not 0 <= msg.dlc <= _MAX_DLC:
        raise ValueError(f"a classical CAN frame has a DLC of 0..8, not {msg.dlc}")

    can_id = msg.arbitration_id
    if msg.is_extended_id:
        can_id |= _EFF_FLAG
    if msg.is_remote_frame:
        # A remote frame has a DLC, the length of the data it asks for, but no data field.
        if msg.data:
            raise ValueError("a remote frame carries no data")
        can_id |= _RTR_FLAG
        data = b""
    else:
        data = bytes(msg.data)
        if len(data) != msg.dlc:
            raise ValueError(f"DLC {msg.dlc} does not match {len(data)} data byte(s)")

    # "3s" and "8s" fill up with zero bytes: the padding, and the data beyond the length.
    return _FRAME.pack(can_id, msg.dlc, b"", data)


def decode(datagram: bytes) -> can.Message | None:
    """The frame that `datagram` carries, or None if it is not exactly one valid frame.

    The checks are those of can_udp_decode() in the node, so that both ends of a bus agree
    on what a frame is. The message has no timestamp and no channel; the bus adds them.
    """
    if len(datagram) != DATAGRAM_LEN:
        return None
    can_id, length, padding, data = _FRAME.unpack(datagram)
    is_extended_id = bool(can_id & _EFF_FLAG)
    is_remote_frame = bool(can_id & _RTR_FLAG)
    identifier = can_id & _ID_MASK
    if (
        can_id & _ERR_FLAG
        or length > _MAX_DLC
        or (not is_extended_id and identifier > _ID_BASE_MAX)
        or padding != _ZERO_PADDING
    ):
        return None
    # Data bytes that the frame does not have must be zero too. Accepting anything there
    # would give one frame many encodings, and of two receivers that check differently one
    # would see a frame where the other sees none.
    used = 0 if is_remote_frame else length
    if any(data[used:]):
        return None
    return can.Message(
        arbitration_id=identifier,
        is_extended_id=is_extended_id,
        is_remote_frame=is_remote_frame,
        dlc=length,
        data=data[:used],
    )


class UdpBus(can.BusABC):
    """One participant of a UDP bus; the python-can interface "fcn_udp".

    :param channel:
        Base port of the bus, 1..65520, as an integer or a numeric string.
    :param can_filters:
        See can.BusABC.set_filters(). Filtering is done by python-can, in this process.
    :param receive_own_messages:
        Must be False. A participant never receives its own frames on this bus, and asking
        for them is refused instead of ignored.
    :param state:
        can.BusState.ACTIVE, or can.BusState.PASSIVE to open the bus listen-only.
    :param receive_buffer_bytes:
        Size of the socket receive buffer to ask the kernel for; see RECEIVE_BUFFER_BYTES.
    :param kwargs:
        Accepted and passed on to can.BusABC, which ignores them: python-can's tools and
        configuration files add arguments (bitrate, fd, single_handle) that mean nothing on
        this bus. A CAN FD frame is refused when it is sent.

    :raises ValueError, TypeError: for a channel that is no base port, or another bad argument
    :raises can.CanInterfaceNotImplementedError: on a system other than Linux
    :raises can.CanInitializationError: if all 16 slots are taken or a socket call fails

    Attributes: ``base_port``, ``slot`` (0..15), ``port`` (base_port + slot) and
    ``receive_buffer_bytes`` (what the kernel granted). Counters, readable at any time and
    after shutdown():

    - ``tx_frames``: frames sent.
    - ``rx_frames``: frames received from the bus, counted before can_filters are applied.
    - ``rx_malformed``: datagrams dropped because they were not a frame from this bus.
    - ``rx_overflow``: datagrams the kernel discarded because the receive buffer was full.
    """

    def __init__(
        self,
        channel: int | str = DEFAULT_BASE_PORT,
        can_filters: CanFilters | None = None,
        receive_own_messages: bool = False,
        state: can.BusState = can.BusState.ACTIVE,
        receive_buffer_bytes: int = RECEIVE_BUFFER_BYTES,
        **kwargs: Any,
    ) -> None:
        if not _IS_LINUX:
            raise can.CanInterfaceNotImplementedError(
                "the UDP bus needs Linux: it relies on recvmsg() with kernel receive timestamps"
            )
        base_port = _base_port(channel)
        if receive_own_messages:
            raise can.CanInitializationError(
                "a participant of the UDP bus cannot receive its own frames"
            )
        if isinstance(receive_buffer_bytes, bool) or not isinstance(receive_buffer_bytes, int):
            raise TypeError(
                f"receive_buffer_bytes must be an integer, not {receive_buffer_bytes!r}"
            )
        if not 0 < receive_buffer_bytes <= _INT_MAX:  # the socket option is a C int
            raise ValueError(
                f"receive_buffer_bytes must be 1..{_INT_MAX}, not {receive_buffer_bytes}"
            )
        self._state = _settable_state(state)

        self.base_port = base_port
        self._socket = _open_socket(receive_buffer_bytes)
        try:
            self.slot = _bind_first_free_slot(self._socket, base_port)
            # Linux reports twice the size it granted: the doubling pays for its bookkeeping.
            self.receive_buffer_bytes: int = self._socket.getsockopt(
                socket.SOL_SOCKET, socket.SO_RCVBUF
            )
        except BaseException:
            self._socket.close()
            raise
        self.port = base_port + self.slot
        self.channel_info = (
            f"UDP bus on {_LOOPBACK}, ports {base_port}..{base_port + SLOTS - 1}, "
            f"slot {self.slot} (port {self.port})"
        )
        self._peers = [
            (_LOOPBACK, base_port + slot) for slot in range(SLOTS) if slot != self.slot
        ]
        self._send_lock = threading.Lock()
        self._closed = False
        self._overflow = 0

        self.tx_frames = 0
        self.rx_frames = 0
        self.rx_malformed = 0

        # Last, as python-can asks: from here on the base class regards the bus as open and
        # shuts it down if it is garbage-collected without shutdown().
        super().__init__(channel=base_port, can_filters=can_filters, **kwargs)

    # ------------------------------------------------------------------------- transmit

    def send(self, msg: can.Message, timeout: float | None = None) -> None:
        """Send one classical data or remote frame to every other participant.

        `timeout` is accepted for python-can and not used: a datagram on the loopback
        interface is delivered inside sendto(), there is nothing to wait for.

        :raises can.CanOperationError:
            for a CAN FD or an error frame, on a bus that is listen-only or shut down, and
            if the kernel refuses a datagram; in the last case the frame may have reached
            some participants.
        :raises ValueError: for a message that is no valid classical frame, see encode()
        """
        datagram = encode(msg)
        with self._send_lock:
            if self._closed:
                raise can.CanOperationError("the UDP bus is shut down")
            if self._state is not can.BusState.ACTIVE:
                raise can.CanOperationError(
                    "the UDP bus is listen-only (BusState.PASSIVE): nothing is sent"
                )
            failure: OSError | None = None
            for peer in self._peers:
                # A slot nobody has bound is not an error: the kernel discards the datagram,
                # like a frame on a bus segment with no node attached, and tells an
                # unconnected socket nothing about it. Whatever does fail must not keep the
                # frame from the slots that follow.
                try:
                    self._socket.sendto(datagram, peer)
                except OSError as error:
                    failure = failure or error
            if failure is not None:
                raise can.CanOperationError(
                    f"sending to the UDP bus failed: {failure.strerror}", failure.errno
                ) from failure
            self.tx_frames += 1

    # -------------------------------------------------------------------------- receive

    def _recv_internal(self, timeout: float | None) -> tuple[can.Message | None, bool]:
        """Next frame from the bus, or None after `timeout` seconds (None: wait for ever).

        Datagrams that are not frames of this bus are consumed, counted and skipped within
        the same timeout. The second value is always False: can_filters are not applied here
        but by can.BusABC.recv().
        """
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            if self._closed:
                raise can.CanOperationError("the UDP bus is shut down")
            try:
                # One byte more than a frame has, so that a longer datagram shows its length.
                datagram, ancillary, _flags, source = self._socket.recvmsg(
                    DATAGRAM_LEN + 1, _ANCILLARY_LEN
                )
            except BlockingIOError:
                # Nothing is queued. Only now is it worth a second system call to wait:
                # under load the next datagram is already there and recvmsg() alone does.
                remaining = None if deadline is None else deadline - time.monotonic()
                if remaining is not None and remaining <= 0:
                    return None, False
                try:
                    select.select([self._socket], [], [], remaining)
                except (OSError, ValueError) as error:  # closed by shutdown() meanwhile
                    raise can.CanOperationError(
                        f"waiting for the UDP bus failed: {error}"
                    ) from error
                continue
            except OSError as error:
                raise can.CanOperationError(
                    f"receiving from the UDP bus failed: {error.strerror}", error.errno
                ) from error

            message = decode(datagram) if self._is_other_slot(source) else None
            if message is None:
                self.rx_malformed += 1
                continue
            timestamp = _kernel_timestamp(ancillary)
            message.timestamp = time.time() if timestamp is None else timestamp
            message.channel = self.base_port
            self.rx_frames += 1
            return message, False

    def _is_other_slot(self, source: tuple[str, int]) -> bool:
        """True if a datagram from `source` comes from another participant of this bus."""
        host, port = source
        return (
            host == _LOOPBACK
            and self.base_port <= port < self.base_port + SLOTS
            and port != self.port
        )

    @property
    def rx_overflow(self) -> int:
        """Datagrams the kernel discarded because this bus's receive buffer was full.

        They never reached this object, so the count is the kernel's (the drop counter of
        the socket, SO_MEMINFO) and is exact at the time of the call. A measurement that
        ends with rx_overflow at zero did not lose a frame on its own side.
        """
        if not self._closed:
            self._overflow = self._kernel_drops()
        return self._overflow

    def _kernel_drops(self) -> int:
        return socket_drops(self._socket)

    # ---------------------------------------------------------------- python-can plumbing

    @property
    def state(self) -> can.BusState:
        """ACTIVE, or PASSIVE for a bus that only listens: send() is then refused.

        There is no acknowledgement on this bus that a passive participant would have to
        withhold, so listen-only means exactly that it cannot disturb what it observes.
        can.logger sets the state when it is started with --active or --passive.
        """
        return self._state

    @state.setter
    def state(self, new_state: can.BusState) -> None:
        self._state = _settable_state(new_state)

    def fileno(self) -> int:
        """File descriptor of the socket, for select() and asyncio; -1 after shutdown()."""
        return self._socket.fileno()

    def shutdown(self) -> None:
        """Stop periodic tasks, close the socket and give the slot back. May be repeated."""
        super().shutdown()
        # Under the send lock: a send() in another thread finishes all its datagrams first.
        with self._send_lock:
            if self._closed:
                return
            with contextlib.suppress(can.CanError):
                self._overflow = self._kernel_drops()
            self._closed = True
            self._socket.close()

    @staticmethod
    def _detect_available_configs() -> list[AutoDetectedConfig]:
        """The default bus, on a system that can run it.

        A bus needs no set-up and exists as soon as somebody opens it, so any base port
        would do; the default is the one the node uses. Whether a slot is free is not
        probed: the answer could be stale at once, and probing would mean taking a slot.
        """
        if not _IS_LINUX:
            return []
        return [{"interface": "fcn_udp", "channel": DEFAULT_BASE_PORT}]


def _base_port(channel: int | str) -> int:
    """The base port that a python-can channel names."""
    if isinstance(channel, str):
        text = channel.strip()
        if not (text.isascii() and text.isdigit()):
            raise ValueError(
                f"the channel of a UDP bus is its base port, a number, not {channel!r}"
            )
        port = int(text)
    elif isinstance(channel, int) and not isinstance(channel, bool):
        port = channel
    else:
        raise TypeError(
            "the channel of a UDP bus is its base port, an integer or a numeric string, "
            f"not {channel!r}"
        )
    last_base = 65535 - (SLOTS - 1)
    if not 1 <= port <= last_base:
        raise ValueError(
            f"base port {port} is out of range: a bus is the ports B..B+{SLOTS - 1}, "
            f"so B is 1..{last_base}"
        )
    return port


def _settable_state(state: can.BusState) -> can.BusState:
    """`state` if a bus can be put into it: ERROR is something a bus reports, not a mode."""
    if state not in (can.BusState.ACTIVE, can.BusState.PASSIVE):
        raise ValueError(
            f"the state of a UDP bus is BusState.ACTIVE or BusState.PASSIVE, not {state!r}"
        )
    return state


def _open_socket(receive_buffer_bytes: int) -> socket.socket:
    """An unbound, non-blocking UDP socket with kernel timestamps and a large receive buffer."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # Before bind(), so that no datagram can be queued without a timestamp, and with a
        # pause before the caller binds, in which the kernel switches arrival timestamps on.
        sock.setsockopt(socket.SOL_SOCKET, _SO_TIMESTAMPNS, 1)
        time.sleep(_TIMESTAMP_SETTLE_S)
        # The kernel caps the request at net.core.rmem_max without an error.
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer_bytes)
        sock.setblocking(False)
    except OSError as error:
        sock.close()
        raise can.CanInitializationError(
            f"cannot set up a socket for the UDP bus: {error.strerror}", error.errno
        ) from error
    return sock


def _bind_first_free_slot(sock: socket.socket, base_port: int) -> int:
    """Bind `sock` to the first free port of the bus and return its slot number."""
    for slot in range(SLOTS):
        try:
            # No SO_REUSEADDR, no SO_REUSEPORT: a slot belongs to exactly one participant,
            # and binding a taken one has to fail.
            sock.bind((_LOOPBACK, base_port + slot))
        except OSError as error:
            if error.errno != errno.EADDRINUSE:
                raise can.CanInitializationError(
                    f"cannot bind {_LOOPBACK}:{base_port + slot} for the UDP bus: "
                    f"{error.strerror}",
                    error.errno,
                ) from error
        else:
            return slot
    raise can.CanInitializationError(
        f"all {SLOTS} slots of the UDP bus at base port {base_port} are taken "
        f"({_LOOPBACK}:{base_port}..{base_port + SLOTS - 1})"
    )


def socket_drops(sock: socket.socket) -> int:
    """The kernel's drop counter of a socket (SO_MEMINFO): what it discarded because the
    socket's receive buffer was full. Linux counts that for a SocketCAN raw socket as it does
    for this bus's UDP one (measured on 6.6; fcn_tools.bus.receive_overflow() uses both).

    :raises can.CanOperationError: if the kernel does not report it
    """
    try:
        counters = sock.getsockopt(socket.SOL_SOCKET, _SO_MEMINFO, 4 * _SK_MEMINFO_VARS)
    except OSError as error:
        raise can.CanOperationError(
            f"the kernel does not report the socket's drop counter: {error.strerror}",
            error.errno,
        ) from error
    return int(struct.unpack_from("@I", counters, 4 * _SK_MEMINFO_DROPS)[0])


def _kernel_timestamp(ancillary: list[tuple[int, int, bytes]]) -> float | None:
    """Arrival time from the control messages of recvmsg(), in seconds; None without one."""
    for level, kind, payload in ancillary:
        if (
            level == socket.SOL_SOCKET
            and kind == _SO_TIMESTAMPNS
            and len(payload) == _TIMESPEC.size
        ):
            seconds, nanoseconds = _TIMESPEC.unpack(payload)
            return seconds + nanoseconds * 1e-9
    return None
