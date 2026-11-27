"""UDP ports on the loopback interface for one test or one measurement run.

A node under test needs a bus of its own (16 consecutive ports, see
platform/posix/can_backend.h) and a port for its simulation control channel. Fixed numbers
would make two runs on one machine, say a developer's test run and a benchmark, talk to each
other's nodes, so every run takes a block that nobody else is using.

Blocks lie below the kernel's range of ephemeral ports (32768 and up by default), so a socket
that is given "any free port" can never land inside one. A block is claimed by keeping its
last port bound for as long as the block is in use: binding is atomic, which makes the claim
safe between processes without a lock file.
"""
from __future__ import annotations

import os
import socket
from dataclasses import dataclass, field

BUS_SLOTS = 16
_BLOCK = 20          # 16 bus slots, the simulation control port, two spare, the claim
_FIRST = 20000
_LAST = 29000        # the default bus (29500) and the ephemeral range stay untouched
_LOOPBACK = "127.0.0.1"


@dataclass
class PortBlock:
    """A claimed block. Call release() when the run is over."""

    bus: int                                   # base port of the UDP bus
    sim: int                                   # port for a node's simulation control channel
    _claim: socket.socket = field(repr=False)

    def release(self) -> None:
        self._claim.close()


def _bind(port: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((_LOOPBACK, port))
    except OSError:
        sock.close()
        raise
    return sock


def _claim(first: int) -> PortBlock | None:
    """Claim the block starting at `first`, or return None if any of its ports is in use."""
    try:
        claim = _bind(first + _BLOCK - 1)
    except OSError:
        return None
    try:
        for port in range(first, first + _BLOCK - 1):
            _bind(port).close()
    except OSError:
        claim.close()
        return None
    return PortBlock(bus=first, sim=first + BUS_SLOTS, _claim=claim)


class PortAllocator:
    """Hands out free blocks, starting at a different place in every process."""

    def __init__(self) -> None:
        self._count = (_LAST - _FIRST) // _BLOCK
        self._next = os.getpid() % self._count

    def take(self) -> PortBlock:
        for _ in range(self._count):
            first = _FIRST + self._next * _BLOCK
            self._next = (self._next + 1) % self._count
            block = _claim(first)
            if block is not None:
                return block
        raise RuntimeError("no free block of UDP ports on the loopback interface")
