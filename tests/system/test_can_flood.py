"""A flood of lower-priority frames from another node, above what the bus can carry, on both
transports.

On a real bus every node with a frame pending starts it when the bus becomes free, and the
smallest arbitration field wins. The node's status frame (0x030) therefore gets onto the bus
at the next frame boundary however much 0x700 traffic other nodes offer: it waits at most for
the one frame on the bus. Until bug 005 was fixed, the controller model put every frame read
from the transport on the bus ahead of the node's own mailboxes, and a flood just above the
bus capacity kept the status frame off the bus for as long as the flood lasted
(docs/bug-log.md, 005).

The tester floods from its own end of the bus, which also listens: neither transport delivers
a participant's own frames back to it, so what it hears is the node's traffic alone, and its
receive buffer cannot fill with flood frames. The flood follows the clock: before every batch
the tester sends as many frames as `rate` x (time since the start) asks for, so a tester that
the host delays catches up with a burst, and the number sent is the planned one in any case.

The node runs at 125 kbit/s. What is tested does not depend on the bit rate, but the host's
work does: 1.2 times the capacity of a 500 kbit/s bus is over 5,000 frames a second through
the tester, the node's transport thread and its receive task, and on a busy machine that load
alone delayed the node's tasks by more than the tolerances of the status checks (measured: a
status value published 14 ticks after the previous one instead of 10).
"""
from __future__ import annotations

import time

import can
import pytest

from fcn_tools.bus import is_status, status_gaps, status_queue_counts
from fcn_tools.canbits import INTERMISSION_BITS, message_bits
from fcn_tools.node import EXIT_OK
from fcn_tools.timing import check_timing

BITRATE = 125_000
NODE_ARGS = ("--bitrate", str(BITRATE))
FLOOD = can.Message(arbitration_id=0x700, data=bytes([0x55] * 8), is_extended_id=False)
LOAD = 1.2                        # flood rate / what the bus can carry of these frames
FLOOD_S = 1.5
SETTLE_S = 0.3                    # after the flood: the node works off its wire queue
STATUS_PERIOD_S = 0.010


def bus_capacity_fps() -> float:
    """Flood frames per second that the bus could carry if it carried nothing else."""
    return BITRATE / (message_bits(FLOOD).frame_bits + INTERMISSION_BITS)


def run_flood(link) -> tuple[list[can.Message], int, tuple[float, float], dict]:
    """Flood the node's bus for FLOOD_S at LOAD times the bus capacity, collecting the node's
    status frames; let it settle, stop the node and collect what was still on its way.

    Returns the status frames heard (from the node's first one), the number of flood frames
    sent, the flood's (first, last) send times on the wall clock, which the frames'
    timestamps use, and the node's statistics.
    """
    rate = LOAD * bus_capacity_fps()
    heard = [link.first_status]
    sent = 0
    first_wall = time.time()
    last_wall = first_wall
    start = time.monotonic()

    while True:
        elapsed = time.monotonic() - start
        if elapsed >= FLOOD_S + SETTLE_S:
            break
        due = int(min(elapsed, FLOOD_S) * rate)
        while sent < due:
            link.bus.send(FLOOD)
            sent += 1
            last_wall = time.time()
        while (message := link.bus.recv(timeout=0.0)) is not None:
            if is_status(message):
                heard.append(message)
        time.sleep(0.001)

    assert sent == int(FLOOD_S * rate)
    assert link.node.stop() == EXIT_OK

    # What the node sent before it stopped is in the tester's socket by now.
    while (message := link.bus.recv(timeout=0.2)) is not None:
        if is_status(message):
            heard.append(message)

    stats = link.node.stats()
    assert stats["can"]["bitrate"] == BITRATE
    return heard, sent, (first_wall, last_wall), stats


def dropped_frames(stats: dict) -> int:
    can_stats = stats["can"]
    return (can_stats["controller"]["wire_overruns"]
            + can_stats["transport_counters"]["rx_overflow"])


def assert_every_flood_frame_accounted_for(stats: dict, sent: int) -> None:
    """Each frame the tester sent reached the node's socket or was dropped by the kernel when
    the socket's buffer was full (rx_overflow); each frame the node read was delivered by the
    controller model or dropped because its wire queue was full (wire_overruns). Nothing was
    still waiting: the node had SETTLE_S to work off its queue before it stopped."""
    can_stats = stats["can"]
    transport, controller = can_stats["transport_counters"], can_stats["controller"]

    assert transport["rx_malformed"] == 0
    assert transport["rx_frames"] + transport["rx_overflow"] == sent, (transport, sent)
    assert controller["rx_lost_fault"] == 0 and controller["rx_errors"] == 0
    assert controller["rx_frames"] + controller["wire_overruns"] == transport["rx_frames"], (
        controller, transport)


def test_a_flood_above_the_bus_capacity_leaves_the_status_frame_its_turn(open_link):
    link = open_link(*NODE_ARGS)
    heard, sent, _, stats = run_flood(link)
    queue = status_queue_counts(stats)

    # The status values are in order and every one the tester did not hear was replaced by its
    # successor in the node, and counted there. A host that stalls the node for more than a
    # period makes it replace values; status_gaps allows 5 in a row. A flood that keeps the
    # status frame off the bus makes the counter jump by the length of the flood, 150 periods,
    # which status_gaps refuses.
    assert status_gaps(heard) == queue["superseded"]
    assert_every_flood_frame_accounted_for(stats, sent)


@pytest.mark.timing
def test_under_a_flood_above_the_bus_capacity_the_status_frame_keeps_its_period(open_link):
    link = open_link(*NODE_ARGS)
    heard, sent, (first_wall, last_wall), stats = run_flood(link)
    queue = status_queue_counts(stats)

    assert status_gaps(heard) == queue["superseded"]
    assert_every_flood_frame_accounted_for(stats, sent)

    # That the flood exceeded what the bus carries depends on the host running the tester:
    # frames were dropped only if it did.
    check_timing(dropped_frames(stats) > 0,
                 f"no flood frame was dropped: the tester did not get {sent} frames out in "
                 f"{FLOOD_S} s")
    # On a host that keeps up, no status value is replaced, and the frame keeps its period on
    # the wall clock while the flood lasts.
    check_timing(queue["superseded"] == 0,
                 f"{queue['superseded']} status values were replaced before they got out")
    during = [m.timestamp for m in heard if first_wall <= m.timestamp <= last_wall]
    gaps_ms = [(b - a) * 1000.0 for a, b in zip(during, during[1:])]
    expected = (last_wall - first_wall) / STATUS_PERIOD_S
    check_timing(len(during) >= 0.9 * expected,
                 f"{len(during)} status frames during the flood, {expected:.0f} periods")
    check_timing(max(gaps_ms) < 30.0, f"longest gap during the flood {max(gaps_ms):.1f} ms")
