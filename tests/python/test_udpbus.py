# SPDX-License-Identifier: MIT
"""The UDP bus of tools/fcn_tools/udpbus.py, tested against the protocol it implements (see
platform/posix/can_backend.h) and against what python-can expects of a bus.

Wherever the datagram itself matters, the other end is a plain UDP socket and the expected
bytes are written out by hand from the layout in the protocol. A bus that only ever talked to
itself would pass with any layout, as long as it decoded what it encoded.

Every test has a bus of its own (see base_port), so the tests can run in parallel with each
other, with the C tests of the same protocol and with a node on the default bus. No test
depends on how the machine schedules two threads or processes against each other: where
frames have to wait for a reader, the test sends no more than the socket buffer is known to
hold.
"""
from __future__ import annotations

import errno
import gc
import itertools
import logging
import os
import random
import select
import socket
import struct
import threading
import time
from pathlib import Path
from typing import Callable, Iterator

import can
import pytest

from fcn_tools import udpbus
from fcn_tools.udpbus import SLOTS, UdpBus

LOOPBACK = "127.0.0.1"

# ------------------------------------------------------------------------------------------
# A bus of its own for every test
# ------------------------------------------------------------------------------------------

# Blocks of 32 ports between 30000 and 60000. The 16 slots of the test's bus are in the
# middle, so that the ports on either side of the bus belong to the same test: they serve
# as "a program that is not on the bus". The first port of the block is the claim.
_FIRST_PORT = 30000
_BLOCK_PORTS = 32
_BUS_OFFSET = 8
_BLOCKS = (60000 - _FIRST_PORT) // _BLOCK_PORTS
_tests_so_far = itertools.count()


def _claim_block(first: int) -> socket.socket | None:
    """Claim the block of ports that starts at `first`: the claim socket, or None if any
    port of the block is in use. Binding is atomic, so of two test runs that pick the same
    block only one gets it, without a lock file."""
    claim = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        claim.bind((LOOPBACK, first))
        for port in range(first + 1, first + _BLOCK_PORTS):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.bind((LOOPBACK, port))
    except OSError:
        claim.close()
        return None
    return claim


@pytest.fixture
def base_port() -> Iterator[int]:
    """Base port of a bus that only this test uses.

    The block is derived from the process id and the number of the test within the run:
    two runs at the same time start 64 blocks apart for neighbouring process ids, and one
    run never uses a block twice. The range overlaps the kernel's ephemeral ports (32768 and
    up), so a block is taken only if every one of its ports can be bound right now, and the
    claim is held until the test is over.
    """
    for _ in range(_BLOCKS):
        block = (os.getpid() * 64 + next(_tests_so_far)) % _BLOCKS
        first = _FIRST_PORT + block * _BLOCK_PORTS
        claim = _claim_block(first)
        if claim is not None:
            break
    else:
        pytest.fail("no free block of UDP ports on the loopback interface")
    try:
        yield first + _BUS_OFFSET
    finally:
        claim.close()


@pytest.fixture
def open_bus(base_port: int) -> Iterator[Callable[..., UdpBus]]:
    """open_bus(**arguments) joins the test's bus. Every bus is shut down after the test."""
    buses: list[UdpBus] = []

    def _open(**arguments) -> UdpBus:
        bus = UdpBus(base_port, **arguments)
        buses.append(bus)
        return bus

    yield _open
    for bus in buses:
        bus.shutdown()


@pytest.fixture
def plain_socket() -> Iterator[Callable[..., socket.socket]]:
    """plain_socket(port, host) is a bound UDP socket that is not a UdpBus: another program,
    well-behaved or not. Reading from it fails after one second instead of blocking."""
    sockets: list[socket.socket] = []

    def _bind(port: int, host: str = LOOPBACK) -> socket.socket:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sockets.append(sock)
        sock.bind((host, port))
        sock.settimeout(1.0)
        return sock

    yield _bind
    for sock in sockets:
        sock.close()


# ------------------------------------------------------------------------------------------
# Frames and what to expect of them
# ------------------------------------------------------------------------------------------


def data_frame(identifier: int, data: bytes = b"", extended: bool = False) -> can.Message:
    return can.Message(arbitration_id=identifier, is_extended_id=extended, data=data)


def remote_frame(identifier: int, dlc: int, extended: bool = False) -> can.Message:
    return can.Message(arbitration_id=identifier, is_extended_id=extended,
                       is_remote_frame=True, dlc=dlc)


def fields(message: can.Message) -> tuple[int, bool, bool, int, bytes]:
    """Everything a frame is on the bus: identifier, format, kind, DLC and data."""
    return (message.arbitration_id, message.is_extended_id, message.is_remote_frame,
            message.dlc, bytes(message.data))


def numbered(number: int, identifier: int = 0x123) -> can.Message:
    """A frame that carries its place in a sequence."""
    return data_frame(identifier, struct.pack("<I", number))


def number_of(message: can.Message) -> int:
    return struct.unpack("<I", message.data)[0]


def receive(bus: can.BusABC, timeout: float = 1.0) -> can.Message:
    """The next frame; a frame on the loopback interface that takes a second is missing."""
    message = bus.recv(timeout)
    assert message is not None, f"no frame within {timeout} s on {bus.channel_info}"
    return message


def nothing_arrives(bus: can.BusABC, within: float = 0.05) -> None:
    message = bus.recv(within)
    assert message is None, f"unexpected frame {message}"


def open_files() -> int:
    return len(os.listdir("/proc/self/fd"))


# Datagrams written out from the layout: can_id little-endian | length | 3 zero bytes | data.
# The first three are also the vectors of tests/platform/test_can_backend.c, which holds the
# node's implementation to the same bytes.
LAYOUT = [
    # base identifier 0x123, three data bytes
    pytest.param(data_frame(0x123, bytes.fromhex("112233")),
                 "23010000" "03" "000000" "1122330000000000", id="base-0x123-3-bytes"),
    # extended identifier 0x1ABCDEF0: bit 31 marks the format, can_id is 0x9ABCDEF0
    pytest.param(data_frame(0x1ABCDEF0, b"\x7f", extended=True),
                 "f0debc9a" "01" "000000" "7f00000000000000", id="extended-0x1abcdef0-1-byte"),
    # remote request for 0x7FF with DLC 8: bit 30, the DLC in the length, no data
    pytest.param(remote_frame(0x7FF, 8),
                 "ff070040" "08" "000000" "0000000000000000", id="remote-0x7ff-dlc-8"),
    # extended remote request: bits 31 and 30
    pytest.param(remote_frame(0x00000001, 2, extended=True),
                 "010000c0" "02" "000000" "0000000000000000", id="extended-remote-dlc-2"),
    # identifier 0 without data is sixteen zero bytes
    pytest.param(data_frame(0x000), "00000000" "00" "000000" "0000000000000000",
                 id="base-0x000-no-data"),
    # the largest identifiers, eight bytes
    pytest.param(data_frame(0x7FF, b"\xff" * 8),
                 "ff070000" "08" "000000" "ffffffffffffffff", id="base-0x7ff-8-bytes"),
    pytest.param(data_frame(0x1FFFFFFF, bytes.fromhex("0102030405060708"), extended=True),
                 "ffffff9f" "08" "000000" "0102030405060708", id="extended-0x1fffffff-8-bytes"),
    # the flag makes a frame extended, not the size of its identifier
    pytest.param(data_frame(0x123, b"\xab", extended=True),
                 "23010080" "01" "000000" "ab00000000000000", id="extended-0x123"),
]

#: Identifier 0x123, data 11 22 33: the datagram that the malformed ones are derived from.
VALID = bytes.fromhex("23010000" "03" "000000" "1122330000000000")
#: Identifier 0x456 without data: sent after a datagram that must be dropped, to show that
#: the bus is still listening and to give recv() something to return.
SENTINEL = bytes.fromhex("56040000" "00" "000000" "0000000000000000")
SENTINEL_FIELDS = (0x456, False, False, 0, b"")


def altered(offset: int, value: int) -> bytes:
    """VALID with one byte replaced."""
    datagram = bytearray(VALID)
    datagram[offset] = value
    return bytes(datagram)


MALFORMED = [
    pytest.param(b"", id="empty"),
    pytest.param(VALID[:8], id="8-bytes-header-only"),
    pytest.param(VALID[:15], id="15-bytes"),
    pytest.param(VALID + b"\x00", id="17-bytes"),
    pytest.param(VALID + bytes(56), id="72-bytes-like-struct-canfd_frame"),
    pytest.param(altered(3, 0x20), id="error-flag"),                   # can_id 0x20000123
    pytest.param(altered(3, 0xA0), id="error-flag-on-extended"),       # can_id 0xA0000123
    pytest.param(altered(4, 9), id="length-9"),
    pytest.param(altered(4, 0xFF), id="length-255"),
    pytest.param(altered(1, 0x09), id="base-identifier-0x923"),        # one bit too many
    pytest.param(altered(3, 0x10), id="base-identifier-bit-28"),       # can_id 0x10000123
    pytest.param(altered(5, 0x01), id="byte-5-not-zero"),
    pytest.param(altered(6, 0x80), id="byte-6-not-zero"),
    pytest.param(altered(7, 0xFF), id="byte-7-not-zero"),
    pytest.param(altered(11, 0x55), id="data-beyond-the-length"),      # length is 3
    pytest.param(altered(15, 0x01), id="last-byte-beyond-the-length"),
    pytest.param(altered(3, 0x40), id="remote-frame-with-data"),       # can_id 0x40000123
]


# ------------------------------------------------------------------------------------------
# The datagram
# ------------------------------------------------------------------------------------------


@pytest.mark.parametrize("frame, datagram", LAYOUT)
def test_a_frame_is_encoded_as_the_protocol_lays_it_out(frame, datagram):
    assert udpbus.encode(frame).hex() == datagram


@pytest.mark.parametrize("frame, datagram", LAYOUT)
def test_a_datagram_is_decoded_as_the_protocol_lays_it_out(frame, datagram):
    decoded = udpbus.decode(bytes.fromhex(datagram))
    assert decoded is not None
    assert fields(decoded) == fields(frame)
    assert not decoded.is_error_frame and not decoded.is_fd


@pytest.mark.parametrize("datagram", MALFORMED)
def test_a_datagram_that_is_not_exactly_one_frame_decodes_to_nothing(datagram):
    assert udpbus.decode(datagram) is None


def test_data_is_cut_off_at_the_length_and_remote_frames_have_none():
    # Unused bytes are zero on the wire, so the receiver has to take the number of data
    # bytes from the length: trailing zero bytes of the payload are data, the fill is not.
    with_zeros = udpbus.decode(bytes.fromhex("23010000" "04" "000000" "1100000000000000"))
    assert fields(with_zeros) == (0x123, False, False, 4, bytes.fromhex("11000000"))
    remote = udpbus.decode(bytes.fromhex("23010040" "04" "000000" "0000000000000000"))
    assert fields(remote) == (0x123, False, True, 4, b"")


@pytest.mark.parametrize("message", [
    can.Message(arbitration_id=0x123, is_extended_id=False, is_fd=True, data=bytes(12)),
    can.Message(arbitration_id=0x123, is_extended_id=False, is_fd=True, data=bytes(2)),
    can.Message(arbitration_id=0x123, is_extended_id=False, is_fd=True, bitrate_switch=True),
    can.Message(arbitration_id=0x004, is_extended_id=True, is_error_frame=True, dlc=8,
                data=bytes(8)),
], ids=["fd-12-bytes", "fd-2-bytes", "fd-bitrate-switch", "error-frame"])
def test_fd_and_error_frames_have_no_encoding(message):
    with pytest.raises(can.CanOperationError):
        udpbus.encode(message)


def _remote_frame_with_data() -> can.Message:
    message = remote_frame(0x123, 2)
    message.data = bytearray(b"\x01\x02")  # can.Message() itself drops the data of a remote frame
    return message


@pytest.mark.parametrize("message", [
    data_frame(0x800),                                           # 12 bits in base format
    data_frame(0x1FFFFFFF),                                      # 29 bits in base format
    data_frame(0x20000000, extended=True),                       # 30 bits
    data_frame(-1),
    can.Message(arbitration_id=0x123, is_extended_id=False, dlc=9, data=bytes(8)),
    can.Message(arbitration_id=0x123, is_extended_id=False, dlc=9, data=bytes(9)),
    can.Message(arbitration_id=0x123, is_extended_id=False, dlc=4, data=bytes(3)),
    can.Message(arbitration_id=0x123, is_extended_id=False, dlc=2, data=bytes(3)),
    can.Message(arbitration_id=0x123, is_extended_id=False, dlc=-1),
    remote_frame(0x123, 9),
    _remote_frame_with_data(),
], ids=["base-0x800", "base-29-bits", "extended-30-bits", "negative-identifier", "dlc-9",
        "9-data-bytes", "dlc-above-data", "dlc-below-data", "negative-dlc", "remote-dlc-9",
        "remote-with-data"])
def test_a_message_that_is_no_classical_frame_has_no_encoding(message):
    with pytest.raises(ValueError):
        udpbus.encode(message)


def test_the_constants_are_those_of_the_node():
    # CAN_UDP_DEFAULT_PORT, CAN_UDP_SLOTS and CAN_UDP_DATAGRAM_LEN in
    # platform/posix/can_backend.h. Two programs that disagree here are on different buses.
    assert udpbus.DEFAULT_BASE_PORT == 29500
    assert udpbus.SLOTS == 16
    assert udpbus.DATAGRAM_LEN == 16


# ------------------------------------------------------------------------------------------
# Slots
# ------------------------------------------------------------------------------------------


def test_participants_take_the_slots_in_order_and_a_seventeenth_is_refused(base_port, open_bus):
    buses = [open_bus() for _ in range(SLOTS)]
    assert [bus.slot for bus in buses] == list(range(16))
    assert [bus.port for bus in buses] == list(range(base_port, base_port + 16))
    assert all(bus.base_port == base_port for bus in buses)

    files_before = open_files()
    with pytest.raises(can.CanInitializationError,
                       match=f"all 16 slots of the UDP bus at base port {base_port} are taken"
                       ) as refused:
        UdpBus(base_port)
    # `refused` holds the traceback and with it the half-built bus, so a socket that only
    # garbage collection would close is still open at this point.
    assert open_files() == files_before, "the refused participant left its socket open"
    del refused

    # A slot is free again as soon as its owner has shut down, and the next one gets it.
    buses[5].shutdown()
    late = open_bus()
    assert (late.slot, late.port) == (5, base_port + 5)
    buses[0].send(data_frame(0x321, b"\x05"))
    assert fields(receive(late)) == (0x321, False, False, 1, b"\x05")
    with pytest.raises(can.CanInitializationError):
        UdpBus(base_port)


def test_a_port_that_another_program_holds_is_not_a_free_slot(base_port, open_bus, plain_socket):
    plain_socket(base_port)
    plain_socket(base_port + 1)
    bus = open_bus()
    assert (bus.slot, bus.port) == (2, base_port + 2)
    assert open_bus().slot == 3


class UnbindableSocket:
    """A socket that may not be bound at all, like one for a privileged port."""

    def __init__(self) -> None:
        self.bind_attempts = 0
        self.closed = False

    def bind(self, address: tuple[str, int]) -> None:
        self.bind_attempts += 1
        raise PermissionError(errno.EACCES, os.strerror(errno.EACCES))

    def close(self) -> None:
        self.closed = True


def test_a_port_that_may_not_be_bound_is_reported_as_that_and_not_as_a_full_bus(
        base_port, monkeypatch):
    unbindable = UnbindableSocket()
    monkeypatch.setattr(udpbus, "_open_socket", lambda receive_buffer_bytes: unbindable)

    with pytest.raises(can.CanInitializationError,
                       match=f"cannot bind 127.0.0.1:{base_port}") as failure:
        UdpBus(base_port)

    assert failure.value.error_code == errno.EACCES
    assert unbindable.bind_attempts == 1  # no slot is "taken": the other 15 are not tried
    assert unbindable.closed


def test_the_bus_says_where_it_is(base_port, open_bus, plain_socket):
    plain_socket(base_port)
    bus = open_bus()
    for part in (f"{base_port}..{base_port + 15}", "slot 1", f"port {base_port + 1}"):
        assert part in bus.channel_info
    assert str(bus) == bus.channel_info
    assert bus.protocol is can.CanProtocol.CAN_20


# ------------------------------------------------------------------------------------------
# Frames between participants
# ------------------------------------------------------------------------------------------

IDENTIFIERS = [
    (0x000, False), (0x7FF, False), (0x2A5, False),
    (0x00000000, True), (0x1FFFFFFF, True), (0x000007FF, True), (0x12345678, True),
]


def every_kind_of_frame(seed: int) -> list[can.Message]:
    """Base and extended identifiers from both ends of their ranges, each as data frame and
    as remote frame with every DLC from 0 to 8. The data is seeded noise, plus all zeros and
    all ones, which are the bytes a fill or a mask would hide."""
    rng = random.Random(seed)
    frames = []
    for identifier, extended in IDENTIFIERS:
        for dlc in range(9):
            frames.append(data_frame(identifier, rng.randbytes(dlc), extended))
            frames.append(remote_frame(identifier, dlc, extended))
    frames.append(data_frame(0x100, bytes(8)))
    frames.append(data_frame(0x101, b"\xff" * 8))
    return frames


def test_three_buses_exchange_every_kind_of_frame_in_all_directions(base_port, open_bus):
    buses = [open_bus() for _ in range(3)]
    per_sender = len(every_kind_of_frame(0))
    assert per_sender == 128  # fewer than the 238 that even an unenlarged buffer holds

    for index, sender in enumerate(buses):
        frames = every_kind_of_frame(seed=0xF00D + index)
        for frame in frames:
            sender.send(frame)
        for receiver in buses:
            if receiver is sender:
                continue
            received = [receive(receiver) for _ in frames]
            assert [fields(message) for message in received] == [fields(f) for f in frames]
            for message in received:
                assert message.is_rx and message.channel == base_port
                assert not (message.is_error_frame or message.is_fd)

    for bus in buses:
        nothing_arrives(bus)
        assert (bus.tx_frames, bus.rx_frames) == (per_sender, 2 * per_sender)
        assert (bus.rx_malformed, bus.rx_overflow) == (0, 0)


def test_a_sender_does_not_hear_itself(open_bus):
    alone = open_bus()
    alone.send(data_frame(0x123, b"\x01\x02"))
    nothing_arrives(alone)

    other = open_bus()
    alone.send(data_frame(0x124))
    assert receive(other).arbitration_id == 0x124
    nothing_arrives(alone)
    # Not even a datagram that it had to drop: nothing was sent to its own port.
    assert (alone.tx_frames, alone.rx_frames, alone.rx_malformed) == (2, 0, 0)


def test_a_frame_goes_once_to_each_of_the_other_fifteen_slots(base_port, open_bus, plain_socket):
    # The bus under test sits in the middle of the bus, with other programs on all sides.
    others = [plain_socket(base_port + slot) for slot in range(5)]
    bus = open_bus()
    assert bus.slot == 5
    others += [plain_socket(base_port + slot) for slot in range(6, SLOTS)]

    bus.send(data_frame(0x123, bytes.fromhex("112233")))

    for other in others:
        datagram, source = other.recvfrom(64)
        assert datagram == VALID
        assert source == (LOOPBACK, bus.port)
        other.setblocking(False)
        with pytest.raises(BlockingIOError):  # one datagram per slot, not two
            other.recvfrom(64)
    nothing_arrives(bus)


def test_frames_from_the_first_and_the_last_slot_are_accepted(base_port, open_bus, plain_socket):
    first = plain_socket(base_port)
    bus = open_bus()
    assert bus.slot == 1
    last = plain_socket(base_port + SLOTS - 1)

    first.sendto(VALID, (LOOPBACK, bus.port))
    last.sendto(SENTINEL, (LOOPBACK, bus.port))

    assert fields(receive(bus)) == (0x123, False, False, 3, bytes.fromhex("112233"))
    assert fields(receive(bus)) == SENTINEL_FIELDS
    assert (bus.rx_frames, bus.rx_malformed) == (2, 0)


# ------------------------------------------------------------------------------------------
# What a receiver drops
# ------------------------------------------------------------------------------------------


@pytest.mark.parametrize("datagram", MALFORMED)
def test_a_malformed_datagram_is_dropped_and_counted(datagram, base_port, open_bus,
                                                     plain_socket):
    bus = open_bus()
    participant = plain_socket(base_port + 1)  # a slot of the bus: only the content is wrong

    participant.sendto(datagram, (LOOPBACK, bus.port))
    participant.sendto(SENTINEL, (LOOPBACK, bus.port))

    assert fields(receive(bus)) == SENTINEL_FIELDS
    assert (bus.rx_malformed, bus.rx_frames) == (1, 1)
    nothing_arrives(bus)


def _from_the_port_below(bus, base_port, plain_socket):
    plain_socket(base_port - 1).sendto(VALID, (LOOPBACK, bus.port))


def _from_the_port_above(bus, base_port, plain_socket):
    plain_socket(base_port + SLOTS).sendto(VALID, (LOOPBACK, bus.port))


def _from_another_loopback_address(bus, base_port, plain_socket):
    # All of 127.0.0.0/8 is the local machine. The port is a slot of the bus, the host is not.
    plain_socket(base_port + 7, host="127.0.0.2").sendto(VALID, (LOOPBACK, bus.port))


def _from_its_own_port(bus, base_port, plain_socket):
    # A second handle on the bus's own socket is the only way to send from its port.
    with socket.socket(fileno=os.dup(bus.fileno())) as own:
        own.sendto(VALID, (LOOPBACK, bus.port))


@pytest.mark.parametrize("send_valid_datagram", [
    _from_the_port_below, _from_the_port_above, _from_another_loopback_address,
    _from_its_own_port,
])
def test_a_frame_that_does_not_come_from_another_slot_is_dropped_and_counted(
        send_valid_datagram, base_port, open_bus, plain_socket):
    bus = open_bus()

    send_valid_datagram(bus, base_port, plain_socket)
    plain_socket(base_port + 7).sendto(SENTINEL, (LOOPBACK, bus.port))

    assert fields(receive(bus)) == SENTINEL_FIELDS
    assert (bus.rx_malformed, bus.rx_frames) == (1, 1)
    nothing_arrives(bus)


def test_dropped_datagrams_do_not_hide_the_frame_behind_them_from_a_poll(base_port, open_bus,
                                                                         plain_socket):
    # recv(0) is how an event loop reads a bus whose file descriptor became readable. It must
    # not come back empty-handed because the first thing in the buffer was not a frame.
    bus = open_bus()
    participant = plain_socket(base_port + 1)
    for datagram in (VALID[:15], b"", SENTINEL):
        participant.sendto(datagram, (LOOPBACK, bus.port))
    assert select.select([bus], [], [], 1.0)[0] == [bus]
    time.sleep(0.02)  # the first datagram is there; this is for the last, should it be slower

    message = bus.recv(timeout=0)

    assert message is not None and fields(message) == SENTINEL_FIELDS
    assert bus.rx_malformed == 2


def test_a_wait_that_sees_only_dropped_datagrams_ends_at_its_timeout(base_port, open_bus,
                                                                     plain_socket):
    bus = open_bus()
    participant = plain_socket(base_port + 1)
    for _ in range(3):
        participant.sendto(VALID[:15], (LOOPBACK, bus.port))

    started = time.monotonic()
    assert bus.recv(timeout=0.2) is None
    waited = time.monotonic() - started

    # Not earlier: a dropped datagram is no reason to stop waiting. The upper bound only
    # says that the call came back at all.
    assert 0.19 <= waited < 5.0
    assert bus.rx_malformed == 3


# ------------------------------------------------------------------------------------------
# Timestamps
# ------------------------------------------------------------------------------------------


def wait_until_arrivals_are_stamped(sender: UdpBus, receiver: UdpBus) -> None:
    """Returns once a frame that is read late carries the time it arrived.

    The kernel switches arrival timestamps on some time after the first socket has asked
    for them (see "Timestamps" in udpbus.py); until then a frame is stamped when it is read.
    The constructor of UdpBus waits for that, but the kernel promises nothing, so the tests
    that are about timestamps make sure.
    """
    deadline = time.monotonic() + 5.0
    while True:
        sender.send(data_frame(0x7F0))
        sent = time.time()
        time.sleep(0.005)
        if receive(receiver).timestamp <= sent:
            return
        assert time.monotonic() < deadline, "the kernel never stamped a frame on arrival"


def test_the_timestamp_is_the_time_of_arrival_not_of_reading(open_bus):
    sender, receiver = open_bus(), open_bus()
    wait_until_arrivals_are_stamped(sender, receiver)

    # The kernel stamps the datagram inside sendto(), between the two readings of the same
    # clock (CLOCK_REALTIME, in seconds as a float). The tolerance covers what a float of
    # this size cannot resolve (0.24 us) and leaves room; the reader comes 10 ms later, a
    # hundred times the tolerance, so a timestamp taken at reading cannot pass.
    tolerance = 100e-6
    stamps = []
    for number in range(20):
        before = time.time()
        sender.send(numbered(number))
        after = time.time()
        time.sleep(0.010)
        message = receive(receiver)
        assert number_of(message) == number
        assert before - tolerance <= message.timestamp <= after + tolerance
        stamps.append(message.timestamp)
    assert stamps == sorted(stamps)


def test_frames_that_wait_in_the_buffer_keep_their_own_arrival_times(open_bus):
    sender, receiver = open_bus(), open_bus()
    wait_until_arrivals_are_stamped(sender, receiver)

    sent = []
    for number in range(5):
        sender.send(numbered(number))
        sent.append(time.time())
        time.sleep(0.010)
    stamps = [receive(receiver).timestamp for _ in sent]

    # Read in one go, but stamped 10 ms apart, each before its sender went on.
    assert all(stamp <= moment for stamp, moment in zip(stamps, sent))
    assert all(later - earlier >= 0.009 for earlier, later in zip(stamps, stamps[1:]))


def test_the_timestamp_is_read_from_the_timespec_control_message():
    stamp = struct.pack("@ll", 1_700_000_000, 250_000_000)
    timestamp_message = (socket.SOL_SOCKET, 35, stamp)  # SCM_TIMESTAMPNS
    drop_count_message = (socket.SOL_SOCKET, 40, bytes(4))  # SO_RXQ_OVFL: not a timestamp
    # To the microsecond. The default tolerance of approx() is relative, and a millionth of
    # this number is 28 minutes.
    expected = pytest.approx(1_700_000_000.25, rel=0, abs=1e-6)

    assert udpbus._kernel_timestamp([timestamp_message]) == expected
    assert udpbus._kernel_timestamp([drop_count_message, timestamp_message]) == expected
    assert udpbus._kernel_timestamp([]) is None
    assert udpbus._kernel_timestamp([drop_count_message]) is None
    assert udpbus._kernel_timestamp([(socket.SOL_SOCKET, 35, stamp[:8])]) is None


def test_without_a_kernel_timestamp_the_time_of_reading_is_used(open_bus, monkeypatch):
    sender, receiver = open_bus(), open_bus()
    # No room for control messages: recvmsg() then delivers the datagram without any.
    monkeypatch.setattr(udpbus, "_ANCILLARY_LEN", 0)

    sender.send(numbered(1))
    time.sleep(0.010)
    before = time.time()
    message = receive(receiver)
    after = time.time()

    assert before <= message.timestamp <= after


# ------------------------------------------------------------------------------------------
# The bus as python-can sees it
# ------------------------------------------------------------------------------------------


def test_python_can_opens_the_bus_by_its_interface_name(base_port, open_bus):
    assert can.interfaces.BACKENDS.get("fcn_udp") == ("fcn_tools.udpbus", "UdpBus"), \
        "the entry point is not installed: run scripts/setup-python.sh"
    sender = open_bus()

    with can.Bus(interface="fcn_udp", channel=base_port) as bus:
        assert isinstance(bus, UdpBus)
        assert (bus.base_port, bus.slot) == (base_port, 1)
        sender.send(data_frame(0x321, b"\x01"))
        assert fields(receive(bus)) == (0x321, False, False, 1, b"\x01")

    # Leaving the block shut the bus down: its slot is free again.
    assert open_bus().slot == 1


def test_can_filters_are_applied(base_port, open_bus):
    sender = open_bus()
    wanted = [{"can_id": 0x100, "can_mask": 0x7F0, "extended": False}]

    # The channel as python-can's command-line tools pass it: a string.
    with can.Bus(interface="fcn_udp", channel=str(base_port), can_filters=wanted) as bus:
        for frame in (data_frame(0x200), data_frame(0x100, extended=True),
                      data_frame(0x10F, b"\xaa"), data_frame(0x110), data_frame(0x105)):
            sender.send(frame)

        assert fields(receive(bus)) == (0x10F, False, False, 1, b"\xaa")
        assert fields(receive(bus)) == (0x105, False, False, 0, b"")
        nothing_arrives(bus)
        # All five reached the bus; python-can sorted three of them out afterwards.
        assert bus.rx_frames == 5

        bus.set_filters(None)
        sender.send(data_frame(0x200))
        assert receive(bus).arbitration_id == 0x200


@pytest.mark.parametrize("as_channel", [int, str, lambda port: f" {port}\n"],
                         ids=["integer", "numeric-string", "string-with-white-space"])
def test_the_channel_is_the_base_port_as_a_number_or_a_numeric_string(as_channel, base_port):
    with UdpBus(as_channel(base_port)) as bus:
        assert (bus.base_port, bus.port) == (base_port, base_port)


def test_a_numeric_string_reaches_the_bus_when_python_can_does_not_convert_it(base_port):
    # can.Bus() turns "29500" into a number while it merges its configuration sources;
    # with ignore_config it passes the string on as it is.
    with can.Bus(interface="fcn_udp", channel=str(base_port), ignore_config=True) as bus:
        assert bus.base_port == base_port


@pytest.mark.parametrize("channel, error", [
    ("vcan0", ValueError), ("", ValueError), ("udp:29500", ValueError),
    ("29500.0", ValueError), ("-29500", ValueError), ("0x733c", ValueError),
    (0, ValueError), (-29500, ValueError), (65521, ValueError), (70000, ValueError),
    (29500.0, TypeError), (None, TypeError), (True, TypeError), ((29500,), TypeError),
])
def test_a_channel_that_is_no_base_port_is_refused(channel, error):
    with pytest.raises(error):
        UdpBus(channel)


def test_the_base_port_ranges_over_every_port_that_leaves_room_for_sixteen_slots():
    # 65520 + 15 = 65535 is the last port there is.
    assert udpbus._base_port(1) == 1
    assert udpbus._base_port(65520) == 65520
    assert udpbus._base_port("65520") == 65520
    with pytest.raises(ValueError, match="1..65520"):
        udpbus._base_port(65521)


def test_arguments_that_python_can_tools_add_are_tolerated(base_port):
    # can.logger and can.viewer pass single_handle, and bitrate or fd if they were given.
    with UdpBus(base_port, bitrate=500000, fd=True, single_handle=True) as bus:
        assert bus.slot == 0
        with pytest.raises(can.CanOperationError):  # tolerated is not the same as supported
            bus.send(can.Message(arbitration_id=0x1, is_extended_id=False, is_fd=True))


def test_asking_for_own_frames_is_refused_and_takes_no_slot(base_port, open_bus):
    with pytest.raises(can.CanInitializationError, match="own frames"):
        UdpBus(base_port, receive_own_messages=True)
    assert open_bus(receive_own_messages=False).slot == 0


@pytest.mark.parametrize("size, error", [(0, ValueError), (-4096, ValueError),
                                         (2**31, ValueError), (4096.0, TypeError),
                                         ("4096", TypeError), (True, TypeError)])
def test_a_receive_buffer_size_that_is_no_positive_c_int_is_refused(size, error, base_port,
                                                                   open_bus):
    with pytest.raises(error):
        UdpBus(base_port, receive_buffer_bytes=size)
    assert open_bus().slot == 0  # and no slot was taken on the way


def test_a_socket_option_that_the_kernel_refuses_fails_the_opening_and_leaks_nothing(
        base_port, open_bus, monkeypatch):
    # An option number that no kernel knows, in place of SO_TIMESTAMPNS: ENOPROTOOPT.
    monkeypatch.setattr(udpbus, "_SO_TIMESTAMPNS", 0x7FFF)
    files_before = open_files()

    with pytest.raises(can.CanInitializationError, match="cannot set up a socket") as failure:
        UdpBus(base_port)

    assert failure.value.error_code == errno.ENOPROTOOPT
    assert open_files() == files_before  # while `failure` still holds the traceback
    del failure
    monkeypatch.undo()
    assert open_bus().slot == 0


def test_the_default_bus_is_offered_to_python_cans_detection():
    default_bus = {"interface": "fcn_udp", "channel": 29500}
    assert UdpBus._detect_available_configs() == [default_bus]
    assert can.detect_available_configs("fcn_udp") == [default_bus]


def test_other_systems_are_told_that_the_bus_needs_linux(base_port, monkeypatch):
    monkeypatch.setattr(udpbus, "_IS_LINUX", False)
    with pytest.raises(can.CanInterfaceNotImplementedError, match="Linux"):
        UdpBus(base_port)
    assert UdpBus._detect_available_configs() == []


# ------------------------------------------------------------------------------------------
# What send() refuses
# ------------------------------------------------------------------------------------------


@pytest.mark.parametrize("message, error", [
    (can.Message(arbitration_id=0x123, is_extended_id=False, is_fd=True, data=bytes(12)),
     can.CanOperationError),
    (can.Message(arbitration_id=0x123, is_extended_id=False, is_fd=True, data=bytes(8)),
     can.CanOperationError),
    (can.Message(arbitration_id=0x004, is_extended_id=True, is_error_frame=True, dlc=8,
                 data=bytes(8)), can.CanOperationError),
    (data_frame(0x800), ValueError),
    (can.Message(arbitration_id=0x123, is_extended_id=False, dlc=9, data=bytes(8)), ValueError),
], ids=["fd-12-bytes", "fd-8-bytes", "error-frame", "identifier-too-large", "dlc-9"])
def test_a_frame_the_bus_cannot_carry_is_refused_and_nothing_is_sent(message, error, open_bus):
    sender, receiver = open_bus(), open_bus()

    with pytest.raises(error):
        sender.send(message)

    nothing_arrives(receiver)
    assert (sender.tx_frames, receiver.rx_frames, receiver.rx_malformed) == (0, 0, 0)
    sender.send(data_frame(0x123))  # and the bus is none the worse for it
    assert receive(receiver).arbitration_id == 0x123


def test_a_passive_bus_listens_and_does_not_send(open_bus):
    listener = open_bus(state=can.BusState.PASSIVE)
    other = open_bus()
    assert listener.state is can.BusState.PASSIVE
    assert other.state is can.BusState.ACTIVE

    with pytest.raises(can.CanOperationError, match="listen-only"):
        listener.send(data_frame(0x001))
    nothing_arrives(other)
    assert listener.tx_frames == 0

    other.send(data_frame(0x002))
    assert receive(listener).arbitration_id == 0x002

    listener.state = can.BusState.ACTIVE  # what can.logger --active does
    listener.send(data_frame(0x003))
    assert receive(other).arbitration_id == 0x003

    other.state = can.BusState.PASSIVE  # and --passive
    with pytest.raises(can.CanOperationError):
        other.send(data_frame(0x004))


@pytest.mark.parametrize("state", [can.BusState.ERROR, "PASSIVE", None, 1])
def test_a_state_that_is_not_active_or_passive_cannot_be_set(state, base_port, open_bus):
    bus = open_bus()
    with pytest.raises(ValueError):
        bus.state = state
    assert bus.state is can.BusState.ACTIVE
    with pytest.raises(ValueError):
        UdpBus(base_port, state=state)


class FaultySocket:
    """Stands in for the socket of a bus and lets chosen calls fail the way the kernel would.

    Nothing short of root can make the kernel refuse a datagram on the loopback interface,
    so the error paths are reached by not asking it.
    """

    def __init__(self, real: socket.socket, refused_port: int | None = None,
                 receive_error: OSError | None = None, gone: bool = False) -> None:
        self._real = real
        self._refused_port = refused_port
        self._receive_error = receive_error
        self._gone = gone

    def sendto(self, data: bytes, address: tuple[str, int]) -> int:
        if address[1] == self._refused_port:
            raise OSError(errno.ENOBUFS, os.strerror(errno.ENOBUFS))
        return self._real.sendto(data, address)

    def recvmsg(self, *arguments):
        if self._receive_error is not None:
            raise self._receive_error
        return self._real.recvmsg(*arguments)

    def fileno(self) -> int:
        return -1 if self._gone else self._real.fileno()  # -1: what a closed socket answers

    def __getattr__(self, name: str):
        return getattr(self._real, name)


def test_a_datagram_the_kernel_refuses_is_reported_and_the_other_slots_still_get_theirs(
        base_port, open_bus, plain_socket, monkeypatch):
    bus = open_bus()
    before, after = plain_socket(base_port + 2), plain_socket(base_port + 4)
    monkeypatch.setattr(bus, "_socket", FaultySocket(bus._socket, refused_port=base_port + 3))

    with pytest.raises(can.CanOperationError) as failure:
        bus.send(data_frame(0x123, bytes.fromhex("112233")))

    assert failure.value.error_code == errno.ENOBUFS
    assert before.recvfrom(64)[0] == VALID
    assert after.recvfrom(64)[0] == VALID
    assert bus.tx_frames == 0  # not sent: it did not reach every slot


def test_a_receive_error_of_the_kernel_is_reported(open_bus, monkeypatch):
    bus = open_bus()
    down = OSError(errno.ENETDOWN, os.strerror(errno.ENETDOWN))
    monkeypatch.setattr(bus, "_socket", FaultySocket(bus._socket, receive_error=down))

    with pytest.raises(can.CanOperationError) as failure:
        bus.recv(timeout=0.1)

    assert failure.value.error_code == errno.ENETDOWN


def test_a_socket_that_is_closed_under_a_waiting_reader_is_reported_as_a_bus_error(
        open_bus, monkeypatch):
    # shutdown() in another thread, between a reader's look into the empty buffer and its
    # wait for the next datagram: select() then meets a closed socket and raises ValueError.
    bus = open_bus()
    nothing_queued = BlockingIOError(errno.EAGAIN, os.strerror(errno.EAGAIN))
    monkeypatch.setattr(bus, "_socket",
                        FaultySocket(bus._socket, receive_error=nothing_queued, gone=True))

    with pytest.raises(can.CanOperationError, match="waiting for the UDP bus failed"):
        bus.recv(timeout=0.5)


def test_a_kernel_that_does_not_report_its_drops_is_not_taken_for_one_that_dropped_nothing(
        open_bus, monkeypatch):
    bus = open_bus()
    assert bus.rx_overflow == 0
    # An option number that no kernel knows, in place of SO_MEMINFO: what a kernel that is
    # too old for the option answers.
    monkeypatch.setattr(udpbus, "_SO_MEMINFO", 0x7FFF)

    with pytest.raises(can.CanOperationError, match="drop counter") as failure:
        _ = bus.rx_overflow

    assert failure.value.error_code == errno.ENOPROTOOPT
    bus.shutdown()  # which reads the counter once more, and must not fail for that
    assert bus.fileno() == -1


# ------------------------------------------------------------------------------------------
# Waiting and shutting down
# ------------------------------------------------------------------------------------------


def test_recv_returns_at_once_with_a_frame_and_after_the_timeout_without(open_bus):
    sender, receiver = open_bus(), open_bus()

    started = time.monotonic()
    assert receiver.recv(timeout=0) is None
    assert receiver.recv(timeout=0.15) is None
    waited = time.monotonic() - started
    assert 0.14 <= waited < 5.0

    sender.send(numbered(1))
    sender.send(numbered(2))
    assert select.select([receiver], [], [], 1.0)[0] == [receiver]
    assert number_of(receive(receiver, timeout=0)) == 1  # it is there: no waiting needed
    assert number_of(receive(receiver, timeout=None)) == 2


@pytest.mark.parametrize("timeout", [3.0, None], ids=["timeout-3-s", "no-timeout"])
def test_a_frame_that_arrives_during_the_wait_ends_it(timeout, open_bus):
    sender, receiver = open_bus(), open_bus()
    sent_at: list[float] = []

    def send_later() -> None:
        sent_at.append(time.monotonic())
        sender.send(numbered(1))

    later = threading.Timer(0.05, send_later)
    started = time.monotonic()
    later.start()
    try:
        message = receiver.recv(timeout)
        returned = time.monotonic()
    finally:
        later.join()

    assert message is not None and number_of(message) == 1
    # Not before the frame was sent (measured where it was sent, so that a host that delays
    # this thread cannot make the wait look short), and long before a timeout would have run
    # out: 1.5 s is 30 times the delay, it says that the wait ended, not how fast the wake-up
    # was.
    assert returned >= sent_at[0]
    assert returned - started < 1.5


def test_the_file_descriptor_is_readable_exactly_while_a_frame_waits(open_bus):
    sender, receiver = open_bus(), open_bus()
    assert receiver.fileno() >= 0
    assert select.select([receiver], [], [], 0.05)[0] == []

    sender.send(numbered(1))
    assert select.select([receiver], [], [], 1.0)[0] == [receiver]
    assert number_of(receive(receiver, timeout=0)) == 1
    assert select.select([receiver.fileno()], [], [], 0)[0] == []


def test_shutdown_frees_the_slot_keeps_the_counters_and_may_be_repeated(base_port, open_bus):
    sender, bus = open_bus(), open_bus()
    sender.send(numbered(1))
    receive(bus)
    bus.send(numbered(2))

    bus.shutdown()
    bus.shutdown()

    assert bus.fileno() == -1
    assert (bus.tx_frames, bus.rx_frames, bus.rx_malformed, bus.rx_overflow) == (1, 1, 0, 0)
    with pytest.raises(can.CanOperationError, match="shut down"):
        bus.send(numbered(3))
    with pytest.raises(can.CanOperationError, match="shut down"):
        bus.recv(timeout=0.1)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as anybody:
        anybody.bind((LOOPBACK, base_port + 1))  # the port is free: this would fail otherwise


def test_a_bus_that_is_dropped_without_shutdown_still_gives_its_slot_back(base_port, open_bus,
                                                                          caplog):
    first = open_bus()
    forgotten = UdpBus(base_port)
    assert forgotten.slot == 1

    del forgotten  # python-can's base class shuts a forgotten bus down, and says so
    gc.collect()

    assert "UdpBus was not properly shut down" in caplog.text
    assert open_bus().slot == 1
    assert first.slot == 0


def test_a_bus_that_was_shut_down_is_not_reported_as_forgotten(base_port, caplog):
    # shutdown() has to pass through the base class: that is where python-can notes that the
    # bus is closed, and what it checks when the object goes away.
    bus = UdpBus(base_port)
    bus.shutdown()

    del bus
    gc.collect()

    assert "not properly shut down" not in caplog.text


def test_shutdown_stops_periodic_sending(open_bus, caplog):
    sender, receiver = open_bus(), open_bus()
    task = sender.send_periodic(numbered(7), period=0.02)
    assert number_of(receive(receiver)) == 7
    assert number_of(receive(receiver)) == 7  # it repeats

    sender.shutdown()  # right after a frame: the task is asleep until the next one is due

    task.thread.join(timeout=2.0)
    assert not task.thread.is_alive()
    while receiver.recv(timeout=0) is not None:
        pass
    nothing_arrives(receiver, within=0.1)  # five periods
    # The task was stopped. A task that is left running stops as well, but by failing when it
    # next sends on the closed bus, and python-can logs that failure.
    assert not [record for record in caplog.records if record.levelno >= logging.ERROR]


# ------------------------------------------------------------------------------------------
# Threads
# ------------------------------------------------------------------------------------------


def test_two_sending_threads_reach_every_receiver_in_the_same_order(open_bus):
    # The receivers are the first and the last slot that the sender serves, 14 datagrams
    # apart. Without the send lock a second thread gets in between them: when this was tried
    # with the lock taken out, the two receivers disagreed in 9 runs out of 10.
    sender = open_bus()
    near = open_bus()
    for _ in range(13):
        open_bus()
    far = open_bus()
    assert (near.slot, far.slot) == (1, 15)
    per_thread = 100  # 200 frames wait unread: fewer than an unenlarged buffer holds

    def send_all(identifier: int) -> None:
        for number in range(per_thread):
            sender.send(numbered(number, identifier))

    threads = [threading.Thread(target=send_all, args=(identifier,))
               for identifier in (0x100, 0x200)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    def sequence(bus: UdpBus) -> list[tuple[int, int]]:
        return [(message.arbitration_id, number_of(message))
                for message in (receive(bus) for _ in range(2 * per_thread))]

    seen_near, seen_far = sequence(near), sequence(far)
    assert seen_near == seen_far
    for identifier in (0x100, 0x200):  # and each thread's frames in the order it sent them
        assert [number for ident, number in seen_near if ident == identifier] == \
            list(range(per_thread))
    assert sender.tx_frames == 2 * per_thread


def test_a_notifier_thread_answers_while_the_main_thread_sends(open_bus):
    tester, echo = open_bus(), open_bus()

    def answer(request: can.Message) -> None:
        echo.send(data_frame(request.arbitration_id + 1, bytes(request.data)))

    notifier = can.Notifier(echo, [answer], timeout=0.05)
    try:
        for number in range(200):
            tester.send(numbered(number, identifier=0x6F0))
            reply = receive(tester)
            assert (reply.arbitration_id, number_of(reply)) == (0x6F1, number)
    finally:
        notifier.stop()
    assert notifier.exception is None
    assert (echo.rx_frames, echo.tx_frames, tester.rx_frames) == (200, 200, 200)


# ------------------------------------------------------------------------------------------
# Bursts and the receive buffer
#
# A frame that has not been read waits in the receiver's socket buffer; UDP has no way to
# make a sender wait. What the tests can rely on is therefore the size of that buffer, and
# they do not rely on a reader that runs at the same time as the sender: with 2000 frames
# sent back to back on a busy machine, such a reader was measured to fall up to 220 frames
# behind as a second thread and up to 156 as a second process, with room for 476.
# ------------------------------------------------------------------------------------------

BURST = 2000


def frames_a_receive_buffer_holds(sender: UdpBus, open_bus, limit: int) -> int:
    """How many unread frames fit into the receive buffer of a bus on this machine (at most
    `limit`), found by filling one until the kernel drops a frame. The answer depends on
    net.core.rmem_max and on what the kernel charges for a datagram, which is its business."""
    probe = open_bus()
    try:
        for sent in range(1, limit + 1):
            sender.send(numbered(sent))
            if probe.rx_overflow:
                return sent - 1
        return limit
    finally:
        probe.shutdown()


def test_the_receive_buffer_is_as_large_as_the_kernel_grants(open_bus):
    try:
        rmem_max = int(Path("/proc/sys/net/core/rmem_max").read_text())
    except OSError:
        pytest.skip("net.core.rmem_max cannot be read here")

    # socket(7): the kernel doubles the value that is set, and caps the request at rmem_max.
    asked = udpbus.RECEIVE_BUFFER_BYTES
    assert asked == 4 * 1024 * 1024
    assert open_bus().receive_buffer_bytes == 2 * min(asked, rmem_max)
    assert open_bus(receive_buffer_bytes=8192).receive_buffer_bytes == 2 * min(8192, rmem_max)


def test_a_burst_of_2000_frames_arrives_complete_and_in_order(open_bus):
    """2000 frames, sent back to back as fast as send() returns, with nobody reading while
    they are sent.

    The bus asks for a receive buffer of 4 MiB. 2000 unread frames need 1.8 MB (896 bytes
    each on Linux 6.6), which that would hold more than four times over. A stock kernel
    grants an unprivileged process no more than 425,984 bytes, though, and that is 476
    frames. So the test first finds out how many frames the buffer holds here and then
    sends the 2000 in bursts of three quarters of that (357 with the stock limit), reading
    between the bursts. Where net.core.rmem_max allows a buffer for 2667 frames or more
    (about 1.2 MB, by the same arithmetic), it is one burst of 2000.
    """
    sender = open_bus()
    capacity = frames_a_receive_buffer_holds(sender, open_bus, limit=2 * BURST)
    window = min(BURST, capacity * 3 // 4)
    assert window >= 100, f"the receive buffer holds only {capacity} frames"
    receiver = open_bus()
    already_sent = sender.tx_frames

    received = []
    for first in range(0, BURST, window):
        numbers = range(first, min(first + window, BURST))
        for number in numbers:
            sender.send(numbered(number))
        received += [number_of(receive(receiver)) for _ in numbers]

    assert received == list(range(BURST))
    nothing_arrives(receiver)
    assert (receiver.rx_frames, receiver.rx_malformed, receiver.rx_overflow) == (BURST, 0, 0)
    assert sender.tx_frames - already_sent == BURST


def test_frames_that_do_not_fit_the_receive_buffer_are_counted_not_lost_silently(open_bus):
    sender = open_bus()
    receiver = open_bus(receive_buffer_bytes=4096)  # room for a handful of frames
    burst = 200

    def send_a_burst_and_read_what_arrived(first: int) -> list[int]:
        for number in range(first, first + burst):
            sender.send(numbered(number))
        arrived = []
        while (message := receiver.recv(timeout=0.05)) is not None:
            arrived.append(number_of(message))
        return arrived

    # What fitted is the beginning of the burst, in order and without gaps; every frame
    # after that was dropped by the kernel, and the bus knows how many.
    received = send_a_burst_and_read_what_arrived(first=0)
    assert 0 < len(received) < burst
    assert received == list(range(len(received)))
    assert receiver.rx_overflow == burst - len(received)
    assert receiver.rx_frames == len(received)

    # The loss ends as soon as there is room again. The count survives shutdown(), with
    # what was dropped since it was last asked for: every frame sent to this bus was either
    # received or counted as lost.
    again = send_a_burst_and_read_what_arrived(first=1000)
    assert 0 < len(again) < burst
    assert again == list(range(1000, 1000 + len(again)))
    receiver.shutdown()
    assert receiver.rx_overflow == 2 * burst - receiver.rx_frames
