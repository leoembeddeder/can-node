# SPDX-License-Identifier: MIT
"""fcn_tools.bus: the checks the system tests make on what they hear, with frames built here.

status_gaps() is what tells a test that a status value waited in a mailbox through a bus
fault (tests/system/test_can_faults.py) or that values went missing without the node counting
them (tests/system/test_can_link.py); the filters decide which frames count at all. A test
can only tell the node's losses from its own if its bus loses nothing unseen: open_bus() gives
both transports the same receive buffer, and receive_overflow() counts what did not fit."""
from __future__ import annotations

import os
import socket
import struct
import uuid
from pathlib import Path

import can
import pytest

from fcn_tools import udpbus
from fcn_tools.bus import (ID_PING, ID_PONG, ID_STATUS, STATUS_MAX_STEP, STATUS_PERIOD_TICKS,
                           BusReader, collect_count, drain, is_pong, is_status, open_bus,
                           receive_overflow, status_fields, status_gaps,
                           status_missing_since_start, status_publish_lateness, wait_for_id)


def status(counter: int, tick: int | None = None) -> can.Message:
    """A status frame as the node sends it: counter and tick, little-endian, 8 bytes. Without
    a tick, the one of a value published in the tick its period ended."""
    if tick is None:
        tick = STATUS_PERIOD_TICKS * counter
    return can.Message(arbitration_id=ID_STATUS, data=struct.pack("<II", counter, tick),
                       is_extended_id=False)


def run(counters: list[int], late: dict[int, int] | None = None) -> list[can.Message]:
    late = late or {}
    return [status(c, STATUS_PERIOD_TICKS * c + late.get(c, 0)) for c in counters]


# ----------------------------------------------------------------------------- status_gaps


def test_consecutive_values_have_no_gaps():
    assert status_gaps(run([1, 2, 3, 4])) == 0


def test_each_missing_value_is_counted():
    assert status_gaps(run([1, 4])) == 2
    assert status_gaps(run([3, 4, 6, 7, 10])) == 1 + 2


def test_values_missing_since_start_include_those_before_the_first_one_heard():
    assert status_missing_since_start(run([1, 2, 3])) == 0
    assert status_missing_since_start(run([3, 4, 6])) == 2 + 1
    assert status_missing_since_start([]) == 0
    with pytest.raises(AssertionError):
        status_missing_since_start(run([3, 3]))


def test_no_frames_or_one_frame_have_no_gaps():
    assert status_gaps([]) == 0
    assert status_gaps([status(7)]) == 0


def test_a_step_up_to_the_limit_passes_and_one_more_fails():
    assert status_gaps(run([1, 1 + STATUS_MAX_STEP])) == STATUS_MAX_STEP - 1
    with pytest.raises(AssertionError, match="status counter 1 followed by"):
        status_gaps(run([1, 2 + STATUS_MAX_STEP]))


def test_without_a_step_limit_any_step_forward_is_counted_and_none_back():
    assert status_gaps(run([1, 100]), max_step=None) == 98
    assert status_missing_since_start(run([3, 60]), max_step=None) == 2 + 56
    with pytest.raises(AssertionError, match="status counter 5 followed by 5"):
        status_gaps(run([5, 5]), max_step=None)
    with pytest.raises(AssertionError):
        status_missing_since_start(run([3, 60]))


def test_the_step_limit_is_far_below_the_length_of_a_fault():
    # A value that waited in a mailbox through one of the faults of test_can_faults.py is
    # followed by a jump of the fault's length: 35 periods and more (a stale value after the
    # shorted bus showed up as 2 -> 56). The limit must stay well below that.
    assert STATUS_MAX_STEP <= 10
    with pytest.raises(AssertionError):
        status_gaps(run([2, 56]))


@pytest.mark.parametrize("counters", [[5, 5], [5, 4], [1, 2, 3, 2]])
def test_a_counter_that_repeats_or_goes_back_fails(counters):
    with pytest.raises(AssertionError, match="status counter"):
        status_gaps(run(counters))


def test_a_value_published_late_passes():
    # The ticker can be run late by the host: the tick it reads is later than the end of its
    # period, by any amount. That is a matter for the timing tests.
    assert status_gaps(run([1, 2, 3], late={2: 9})) == 0
    assert status_gaps(run([1, 3], late={1: 12})) == 1


def test_a_tick_that_does_not_increase_fails():
    with pytest.raises(AssertionError, match="ticks"):
        status_gaps([status(1, 20), status(2, 20)])
    with pytest.raises(AssertionError, match="ticks"):
        status_gaps([status(1, 25), status(2, 24)])


def test_a_tick_before_the_end_of_its_period_fails():
    # Period n ends at tick 10 n at the earliest: a tick below that is not a tick count read
    # after the period, whatever came before it.
    with pytest.raises(AssertionError, match="status counter 3 at tick 29"):
        status_gaps([status(1), status(2), status(3, 29)])
    with pytest.raises(AssertionError, match="at tick"):
        status_gaps([status(1, 9)])


def test_a_status_frame_must_have_eight_bytes():
    short = can.Message(arbitration_id=ID_STATUS, data=b"\x01\x00\x00\x00", is_extended_id=False)
    with pytest.raises(ValueError, match="8 data bytes"):
        status_fields(short)
    assert status_fields(status(0x01020304, 0xA0B0C0D0)) == (0x01020304, 0xA0B0C0D0)


def test_publish_lateness_is_relative_to_the_earliest_value():
    # A ticker that started at tick 3: every value 3 ticks after 10 n, one of them 2 later.
    frames = [status(c, 3 + STATUS_PERIOD_TICKS * c + (2 if c == 3 else 0)) for c in (1, 2, 3, 4)]
    assert status_publish_lateness(frames) == [0, 0, 2, 0]


# -------------------------------------------------------------------------------- filters


def test_only_base_format_data_frames_count_as_status_or_pong():
    for wanted, other_id, test in ((ID_STATUS, ID_PONG, is_status), (ID_PONG, ID_STATUS, is_pong)):
        assert test(can.Message(arbitration_id=wanted, data=bytes(8), is_extended_id=False))
        assert not test(can.Message(arbitration_id=wanted, data=bytes(8), is_extended_id=True))
        assert not test(can.Message(arbitration_id=wanted, is_remote_frame=True, dlc=8,
                                    is_extended_id=False))
        assert not test(can.Message(arbitration_id=other_id, data=bytes(8), is_extended_id=False))


@pytest.fixture
def virtual_pair():
    """Two ends of a python-can virtual bus of this test alone."""
    channel = f"fcn-test-{uuid.uuid4()}"
    ends = [can.Bus(interface="virtual", channel=channel) for _ in range(2)]
    yield ends
    for end in ends:
        end.shutdown()


def test_wait_for_id_skips_extended_and_remote_frames(virtual_pair):
    sender, receiver = virtual_pair
    sender.send(can.Message(arbitration_id=ID_PING, data=b"\x01", is_extended_id=True))
    sender.send(can.Message(arbitration_id=ID_PING, is_remote_frame=True, dlc=1,
                            is_extended_id=False))
    sender.send(can.Message(arbitration_id=ID_PING + 1, data=b"\x02", is_extended_id=False))
    sender.send(can.Message(arbitration_id=ID_PING, data=b"\x03", is_extended_id=False))
    found = wait_for_id(receiver, ID_PING, timeout=2.0)
    assert found is not None and bytes(found.data) == b"\x03"
    assert wait_for_id(receiver, ID_PING, timeout=0.05) is None


def test_collect_count_returns_the_frames_it_was_asked_for(virtual_pair):
    sender, receiver = virtual_pair
    for counter in range(1, 6):
        sender.send(status(counter))
        sender.send(can.Message(arbitration_id=ID_PONG, data=b"", is_extended_id=False))
    frames = collect_count(receiver, 3, keep=is_status, timeout=2.0)
    assert [status_fields(f)[0] for f in frames] == [1, 2, 3]
    # The rest is still there, and a count that cannot be reached returns what there was.
    rest = collect_count(receiver, 10, keep=is_status, timeout=0.2)
    assert [status_fields(f)[0] for f in rest] == [4, 5]


def test_drain_reads_everything_that_is_waiting(virtual_pair):
    sender, receiver = virtual_pair
    for counter in range(1, 21):
        sender.send(status(counter))
    assert [status_fields(f)[0] for f in drain(receiver, quiet=0.05)] == list(range(1, 21))
    assert drain(receiver, quiet=0.05) == []


def test_a_bus_reader_returns_what_came_before_and_everything_it_read(virtual_pair):
    sender, receiver = virtual_pair
    with BusReader(receiver, [status(1)]) as reader:
        for counter in range(2, 21):
            sender.send(status(counter))
    # Read in its thread or, for frames still on their way when it stopped, by drain().
    assert [status_fields(f)[0] for f in reader.frames] == list(range(1, 21))
    assert reader.stop() is reader.frames


def test_a_bus_reader_left_by_an_exception_stops_reading(virtual_pair):
    sender, receiver = virtual_pair
    with pytest.raises(KeyError):
        with BusReader(receiver) as reader:
            raise KeyError("the test failed")
    sender.send(status(1))
    assert [status_fields(f)[0] for f in drain(receiver, quiet=0.05)] == [1]
    assert reader.frames == []


# ----------------------------------------------------------- the receive buffer of a tester


def socket_of(bus: can.BusABC) -> socket.socket:
    """A handle of its own on the socket of `bus` (a duplicate of its descriptor): an option
    read or set through it is the socket's. The caller closes it."""
    return socket.socket(fileno=os.dup(bus.fileno()))


def test_a_bus_of_either_transport_gets_the_receive_buffer_of_the_udp_bus(can_spec):
    try:
        rmem_max = int(Path("/proc/sys/net/core/rmem_max").read_text())
    except OSError:
        pytest.skip("net.core.rmem_max cannot be read here")
    bus = open_bus(can_spec)
    try:
        with socket_of(bus) as sock:
            granted = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        # socket(7): the kernel doubles the value that is set, and caps the request at
        # rmem_max.
        assert granted == 2 * min(udpbus.RECEIVE_BUFFER_BYTES, rmem_max)
    finally:
        bus.shutdown()


def test_receive_overflow_counts_every_frame_that_did_not_fit(can_spec):
    sender, receiver = open_bus(can_spec), open_bus(can_spec)
    try:
        # Room for a handful of frames (the kernel's least buffer is a few kilobytes).
        with socket_of(receiver) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        burst = 200
        for number in range(burst):
            sender.send(can.Message(arbitration_id=0x123, data=struct.pack("<I", number),
                                    is_extended_id=False))
        arrived = [struct.unpack("<I", bytes(m.data))[0] for m in drain(receiver, quiet=0.1)]
        # What fitted is the beginning of the burst, in order; every frame after that was
        # dropped by the kernel, and counted.
        assert 0 < len(arrived) < burst
        assert arrived == list(range(len(arrived)))
        assert receive_overflow(receiver) == burst - len(arrived)
    finally:
        sender.shutdown()
        receiver.shutdown()


def test_receive_overflow_needs_a_bus_with_a_socket(virtual_pair):
    with pytest.raises(TypeError, match="no socket"):
        receive_overflow(virtual_pair[0])
