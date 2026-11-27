"""Bus faults, applied to the simulated hardware through simulation control and observed
from outside the node: on the bus, in the controller's status and in the node's statistics.

The step-by-step behaviour of the controller model under each fault is asserted in model time
by its unit tests (tests/unit/test_can_ctrl.c). These tests check that the whole chain holds
together in the running node: fault, controller, interrupt, transmit task, queue policy, bus.
"""
from __future__ import annotations

import re
import signal
import statistics
import threading
import time

import can
import pytest

from fcn_tools.bus import (ID_PING, BusReader, collect, collect_count, is_pong, is_status,
                           receive_overflow, status_fields, status_gaps,
                           status_missing_since_start, status_queue_counts, wait_for)
from fcn_tools.node import EXIT_OK
from fcn_tools.timing import check_timing


def ping(payload: bytes) -> can.Message:
    return can.Message(arbitration_id=ID_PING, data=payload, is_extended_id=False)


def wait_status(sim, reached, timeout: float = 10.0) -> dict:
    """Poll the controller's status until `reached(status)` holds; fail with the last status."""
    deadline = time.monotonic() + timeout
    while True:
        status = sim.can_status()
        if reached(status):
            return status
        if time.monotonic() > deadline:
            pytest.fail(f"the controller did not get there within {timeout} s: {status}",
                        pytrace=False)
        time.sleep(0.005)


def assert_silent(bus, seconds: float = 0.3) -> None:
    """Nothing from the node reaches the bus. Frames that were already under way when the
    fault was applied are let through first."""
    collect(bus, 0.05)
    assert collect(bus, seconds, keep=lambda m: is_status(m) or is_pong(m)) == []


def assert_fresh_and_in_step(frames: list[can.Message]) -> None:
    """What comes out after a fault is the current value, the first frame included, not one
    that waited in a mailbox through the fault: such a value would be followed by a jump of
    the fault's whole length (tens of periods and more), far more than status_gaps() allows
    for a host that stalled the node."""
    assert len(frames) >= 10
    status_gaps(frames)


def next_status_frames(bus, count: int = 10) -> list[can.Message]:
    """The next `count` status frames: a number of frames, not a time, so that a slow host
    does not make the list short."""
    return collect_count(bus, count, keep=is_status, timeout=10.0)


# ------------------------------------------------------------------------ no acknowledgement


def test_without_acknowledgement_the_node_goes_error_passive_and_never_bus_off(open_link):
    link = open_link()
    with link.node.sim() as sim:
        sim.can_fault("noack")

        # Sixteen failed attempts take the transmit counter to 128; from there the exception
        # for a node that is alone on the bus applies and the counter stays where it is.
        status = wait_status(sim, lambda s: s["state"] == "passive")
        assert status["tec"] == 128
        assert_silent(link.bus)
        later = sim.can_status()
        assert later["state"] == "passive" and later["tec"] == 128 and later["bus_off"] == 0
        assert later["tx_failed"] > status["tx_failed"] + 100      # it keeps trying

        sim.can_fault("none")
        assert_fresh_and_in_step(next_status_frames(link.bus))
        # One acknowledged frame takes the counter below 128: error active again.
        status = wait_status(sim, lambda s: s["state"] == "active")
        assert status["tec"] < 128 and status["bus_off"] == 0

    assert link.node.stop() == EXIT_OK
    queue = link.node.stats()["can"]["queue"]
    assert queue["bus_off_count"] == 0
    assert queue["superseded"] > 0          # values that were replaced while nothing got out
    # Without bus off, only calling back the stale value from its mailbox kept it off the bus.
    assert queue["recall_requests"] > 0
    assert queue["produced"] == (queue["sent"] + queue["superseded"] + queue["dropped"]
                                 + queue["expired"] + queue["pending"])


# ----------------------------------------------------------------------------- shorted bus


def test_a_shorted_bus_takes_the_node_bus_off_and_it_returns_by_itself(open_link):
    link = open_link()
    with link.node.sim() as sim:
        sim.can_fault("short")

        # Bus off, recovery after the hold-off, bus off again because the fault is still
        # there, with the hold-off doubling each time (100, 200, 400 ms ...).
        status = wait_status(sim, lambda s: s["bus_off"] >= 3)
        assert status["rx"] == 0
        assert_silent(link.bus)

        sim.can_fault("none")
        # The node is somewhere in a hold-off of at most 1.6 s; after it, it is back.
        frames = next_status_frames(link.bus)
        assert frames, "the node did not return to the bus"
        assert_fresh_and_in_step(frames)
        status = wait_status(sim, lambda s: s["state"] == "active")
        assert status["tec"] < 128
        bus_off_events = status["bus_off"]

        # And it still answers.
        link.bus.send(ping(b"\x42"))
        pong = wait_for(link.bus, is_pong, timeout=5.0)
        assert pong is not None and bytes(pong.data) == b"\x42"

    assert link.node.stop() == EXIT_OK
    stats = link.node.stats()["can"]
    queue = stats["queue"]
    assert stats["controller"]["bus_off_events"] == bus_off_events == queue["bus_off_count"]
    assert queue["restart_requests"] == queue["bus_off_count"]
    assert queue["superseded"] > 0
    assert queue["produced"] == (queue["sent"] + queue["superseded"] + queue["dropped"]
                                 + queue["expired"] + queue["pending"])
    assert "bus off: requesting recovery" in link.node.log()


# Log lines of the transmit task (app/can_io.c): when it took note of bus off, and when it
# asked the controller to recover. Both are stamped with the node's clock when written.
_LOG_LINE = re.compile(r"^\[\s*(\d+)\.(\d{6})\] W can: (.*)$", re.MULTILINE)
_BUS_OFF = "controller state 1 -> 2"
_RECOVERY = "bus off: requesting recovery"


def _holdoffs_ms(log: str) -> list[float]:
    """Time from each bus off to the request to recover from it, in milliseconds."""
    holdoffs, bus_off_at = [], None
    for seconds, micros, text in _LOG_LINE.findall(log):
        at_ms = int(seconds) * 1000.0 + int(micros) / 1000.0
        if text.startswith(_BUS_OFF):
            bus_off_at = at_ms
        elif text == _RECOVERY and bus_off_at is not None:
            holdoffs.append(at_ms - bus_off_at)
            bus_off_at = None
    return holdoffs


@pytest.mark.timing
def test_recovery_is_requested_when_the_hold_off_ends(open_link):
    link = open_link()
    with link.node.sim() as sim:
        sim.can_fault("short")
        # Four times bus off, so three hold-offs have ended: 100, 200 and 400 ms
        # (TX_HOLDOFF_INITIAL_MS in app/can_io.c, doubled at each further bus off).
        wait_status(sim, lambda s: s["bus_off"] >= 4)
    assert link.node.stop() == EXIT_OK

    holdoffs = _holdoffs_ms(link.node.log())
    assert len(holdoffs) >= 3, link.node.log()
    late_ms = [measured - nominal for measured, nominal in zip(holdoffs, (100.0, 200.0, 400.0))]
    # The transmit task sleeps until the queue's next deadline, the end of the hold-off, and
    # rounds that up to the next whole tick: about 1 ms late. A task that slept until some
    # other wake-up instead (the ticker's publish every 10 ms) is late by up to 10 ms.
    check_timing(statistics.median(late_ms) < 3.0,
                 "recovery requested late by " + ", ".join(f"{ms:.1f}" for ms in late_ms) + " ms")


# ------------------------------------------------------------------------ bus held dominant


def test_a_bus_held_dominant_stops_all_traffic_without_bus_off(open_link):
    link = open_link()
    with link.node.sim() as sim:
        sim.can_fault("dominant")

        # The node sees dominant bits where an idle bus should be recessive: its receive
        # counter climbs until it is error passive. It cannot send, so it never goes bus off.
        status = wait_status(sim, lambda s: s["state"] == "passive")
        assert status["rec"] >= 128 and status["tec"] == 0 and status["bus_off"] == 0
        sent_before = status["tx"]
        assert_silent(link.bus)

        # A frame from the tester cannot reach the node either.
        link.bus.send(ping(b"\x01"))
        assert wait_for(link.bus, is_pong, timeout=0.2) is None
        status = sim.can_status()
        assert status["tx"] == sent_before and status["rx_lost_fault"] >= 1
        assert status["bus_off"] == 0

        sim.can_fault("none")
        assert_fresh_and_in_step(next_status_frames(link.bus))

        # Transmitting does not lower the receive counter; the first frame the node receives
        # correctly does, and with it the node is error active again.
        assert sim.can_status()["state"] == "passive"
        link.bus.send(ping(b"\x02"))
        pong = wait_for(link.bus, is_pong, timeout=5.0)
        assert pong is not None and bytes(pong.data) == b"\x02"
        status = wait_status(sim, lambda s: s["state"] == "active")
        assert status["rec"] <= 127


# ------------------------------------------------------------------------- single bit errors


@pytest.mark.timing
def test_destroyed_transmissions_are_repeated_and_nothing_is_lost(open_link):
    link = open_link()
    # Everything the node sends, from its first frame to its last, read while the test runs.
    with BusReader(link.bus, [*link.before_first_status, link.first_status]) as reader:
        with link.node.sim() as sim:
            before = sim.can_status()
            sim.can_inject("tx", 5)
            status = wait_status(sim, lambda s: s["tx_failed"] == before["tx_failed"] + 5)
            # Five errors add 40; every frame that got through since takes 1 off.
            assert 0 < status["tec"] <= 40 and status["state"] == "active"
            # Ten more frames after that, so that the run goes on well past the errors.
            wait_status(sim, lambda s: s["tx"] >= status["tx"] + 10)
        assert link.node.stop() == EXIT_OK
    heard = reader.frames
    node_stats = link.node.stats()
    stats = node_stats["can"]
    controller, queue = stats["controller"], stats["queue"]
    status_queue = status_queue_counts(node_stats)
    # Each destroyed attempt was repeated: nothing was given up, nothing went bus off. Frames
    # are aborted only when the queue calls the mailboxes back because a value waited in its
    # mailbox for a whole period (a host stall); each recall aborts at most the three
    # mailboxes.
    assert controller["tx_attempts_failed"] == 5
    assert controller["tx_aborted"] <= 3 * queue["recall_requests"]
    assert queue["bus_off_count"] == 0
    assert queue["dropped"] == 0 and queue["expired"] == 0
    # And nothing was lost on the way: every frame the controller completed reached the
    # tester, in order, and every status value that never left was replaced by its successor
    # in the node and counted as such, up to the last one the node produced. The tester hears
    # all of it only if its own socket never overflowed, which takes a host that does not run
    # the test process for as long as the socket's 476 frames last, about 1.5 s of the node's
    # 315 frames per second: a timing miss of the tester, which says nothing about the node.
    assert stats["transport_counters"]["tx_frames"] == controller["tx_frames"]
    lost = receive_overflow(link.bus)
    check_timing(lost == 0, f"the tester lost {lost} of the {controller['tx_frames']} frames "
                            "the node sent: its socket was full because the host did not run "
                            "the test process in time, which says nothing about the node")
    assert len(heard) == controller["tx_frames"]
    status_frames = [m for m in heard if is_status(m)]
    assert status_missing_since_start(status_frames, max_step=None) == status_queue["superseded"]
    last_counter = status_fields(status_frames[-1])[0]
    assert last_counter <= status_queue["produced"] <= last_counter + status_queue["pending"]


def test_a_command_is_carried_out_once_even_if_the_node_answers_late(open_link):
    link = open_link()
    with link.node.sim() as sim:         # gives up after 3 x 1 s
        before = sim.can_status()
        # The node does not run for 1.5 s: longer than the client waits for one answer.
        link.node.send_signal(signal.SIGSTOP)
        resume = threading.Timer(1.5, link.node.send_signal, (signal.SIGCONT,))
        resume.start()
        try:
            sim.can_inject("tx", 5)
        finally:
            resume.join()
        # The five destroyed attempts, and then ten frames that got through: had the request
        # been carried out twice, ten attempts would have been destroyed by now.
        failed = wait_status(sim, lambda s: s["tx_failed"] >= before["tx_failed"] + 5)
        after = wait_status(sim, lambda s: s["tx"] >= failed["tx"] + 10)
        assert after["tx_failed"] == before["tx_failed"] + 5


def test_destroyed_receptions_do_not_reach_the_application(open_link):
    link = open_link()
    with link.node.sim() as sim:
        sim.can_inject("rx", 3)
        for n in range(3):
            link.bus.send(ping(bytes([n])))
        # On a real bus the sender would repeat a destroyed frame; this tester does not, so
        # the three frames are simply gone. The fourth one arrives, and its pong is the first.
        link.bus.send(ping(b"\x99"))
        pong = wait_for(link.bus, is_pong, timeout=5.0)
        assert pong is not None and bytes(pong.data) == b"\x99"
        status = sim.can_status()
        assert status["rx_errors"] == 3 and status["rx"] == 1
        assert status["rec"] == 2          # three errors up, one good frame down
