# SPDX-License-Identifier: MIT
"""fcn_tools.ports: every test or measurement run gets UDP ports that nobody else is using."""
from __future__ import annotations

import socket

import pytest

from fcn_tools import ports
from fcn_tools.ports import BUS_SLOTS, PortAllocator

LOOPBACK = "127.0.0.1"


def _can_bind(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.bind((LOOPBACK, port))
        except OSError:
            return False
    return True


def test_a_block_has_a_free_bus_and_a_simulation_port_below_the_ephemeral_range():
    block = PortAllocator().take()
    try:
        used = list(range(block.bus, block.bus + BUS_SLOTS)) + [block.sim]
        assert len(set(used)) == BUS_SLOTS + 1          # the simulation port is not a bus slot
        assert all(20000 <= port < 29500 for port in used)   # not the default bus, not ephemeral
        assert all(_can_bind(port) for port in used)
    finally:
        block.release()


def test_two_blocks_never_share_a_port():
    allocator = PortAllocator()
    first, second = allocator.take(), allocator.take()
    try:
        ports_of = lambda b: set(range(b.bus, b.bus + BUS_SLOTS)) | {b.sim}   # noqa: E731
        assert not ports_of(first) & ports_of(second)
    finally:
        first.release()
        second.release()


def test_a_block_that_another_allocator_holds_is_not_handed_out_again():
    """Two processes that start at the same place must not get the same block. Two allocators
    in one process stand in for them: the claim is a bound socket, which the kernel arbitrates
    between processes exactly as it does here."""
    one, other = PortAllocator(), PortAllocator()
    other._next = one._next                    # both would pick the same block first
    held = one.take()
    try:
        rival = other.take()
        try:
            assert rival.bus != held.bus
        finally:
            rival.release()
    finally:
        held.release()


def _index_of(block) -> int:
    return (block.bus - ports._FIRST) // ports._BLOCK


def test_a_block_with_a_port_in_use_is_skipped():
    allocator = PortAllocator()
    free = allocator.take()                 # a block that is known to be free right now
    free.release()
    allocator._next = _index_of(free)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as squatter:
        squatter.bind((LOOPBACK, free.bus + 3))            # one slot of its bus gets taken
        block = allocator.take()
        try:
            assert block.bus != free.bus
        finally:
            block.release()


def test_a_released_block_can_be_taken_again():
    allocator = PortAllocator()
    block = allocator.take()
    block.release()
    allocator._next = _index_of(block)
    again = allocator.take()
    try:
        assert again.bus == block.bus
    finally:
        again.release()


def test_running_out_of_blocks_is_an_error_not_a_hang(monkeypatch):
    monkeypatch.setattr(ports, "_claim", lambda first: None)
    with pytest.raises(RuntimeError, match="no free block"):
        PortAllocator().take()
