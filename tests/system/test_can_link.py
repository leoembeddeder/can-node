"""The CAN path of the node, end to end, on both transports.

What these tests rely on in the application (app/app.c): a status frame (BMS_Heartbeat) every
10 ms that carries a period counter and the tick count of the moment it was published, and a
ping frame that is answered with a pong carrying the same payload. The node sends other cyclic
messages besides (the telemetry of phase 2); values of the status frame are accounted for with
the counters of its own slot of the transmit queue (fcn_tools.bus.status_queue_counts).

Assertions are made on counts, order and the node's own time (ticks). Wall-clock assertions are
confined to the tests marked `timing`. A test that waits for frames waits for a number of them
or for the node to end, never for a fixed time; the timeouts only bound a node that has gone
silent.
"""
from __future__ import annotations

import socket
import statistics
import struct
import subprocess
import time

import can
import pytest

from fcn_tools.bus import (ID_PING, collect_count, collect_while, drain, is_pong, is_status,
                           open_bus, status_fields, status_gaps, status_missing_since_start,
                           status_publish_lateness, status_queue_counts, wait_for, wait_for_id)
from fcn_tools.node import EXIT_OK
from fcn_tools.simctl import SimControl, SimError
from fcn_tools.timing import check_timing

BITRATE = 500_000


def ping(payload: bytes) -> can.Message:
    return can.Message(arbitration_id=ID_PING, data=payload, is_extended_id=False)


# ------------------------------------------------------------------------ cyclic transmission


def test_every_status_value_is_on_the_bus_or_accounted_for(open_link):
    link = open_link("--duration-s", "2")
    frames = [link.first_status] + collect_while(link.bus, lambda: link.node.running,
                                                 keep=is_status)
    assert link.node.wait(timeout=20) == EXIT_OK
    stats = link.node.stats()
    queue = status_queue_counts(stats)

    # The tester was on the bus before the node started, so it heard every value that left
    # the node, from value 1 on. The values are in order, one per period of the task in the
    # node's own time, and every one that is missing, before the first one heard or between
    # two, was replaced by its successor before it could go out, and counted as such by the
    # node. On a host that keeps up nothing is missing; a host that stalls the node for longer
    # than a period makes it replace values, and the count is what has to agree. There is no
    # bus fault here, so no limit on the step between two values: how long a stall lasts is
    # up to the host.
    assert status_missing_since_start(frames, max_step=None) == queue["superseded"]

    # One value per 10 ms of the node's own time, but for the periods the sensor task skipped
    # after an overrun (app/app.c, sensor_task()), and every value accounted for. The queue was
    # sampled just before the controller stopped: a value it still counts as pending may
    # already have reached the tester.
    last_counter = status_fields(frames[-1])[0]
    skipped = stats["sensor"]["skipped_periods"]
    assert abs(queue["produced"] + skipped - stats["tick"]["count"] / 10) <= 2, (
        queue, skipped, stats["tick"])
    assert last_counter <= queue["produced"] <= last_counter + queue["pending"]


@pytest.mark.timing
def test_the_status_frame_keeps_its_period_on_the_wall_clock(open_link):
    link = open_link()
    frames = collect_count(link.bus, 200, keep=is_status, timeout=10.0)
    assert len(frames) == 200
    status_gaps(frames, max_step=None)
    counters = [status_fields(frame)[0] for frame in frames]
    stamps = [frame.timestamp for frame in frames]

    # The period of the ticker on the wall clock: the time between the first and the last
    # value heard over the number of periods between them, so that a value the node replaced
    # (missing on the bus, counted by the queue) does not count as a long period. A value
    # that left late shows as the longest gap.
    mean_ms = (stamps[-1] - stamps[0]) * 1000.0 / (counters[-1] - counters[0])
    longest_ms = max((b - a) * 1000.0 for a, b in zip(stamps, stamps[1:]))
    check_timing(9.9 <= mean_ms <= 10.1, f"mean period {mean_ms:.3f} ms")
    check_timing(longest_ms < 30.0, f"longest gap {longest_ms:.1f} ms")
    # Each value is published in the tick its period ended in, or the next one: a ticker that
    # the host runs late reads a later tick.
    late = max(status_publish_lateness(frames))
    check_timing(late <= 2, f"a status value was published {late} ticks after its period")


# ----------------------------------------------------------------- reception and event frames


def test_a_ping_is_answered_with_a_pong_carrying_its_payload(open_link):
    link = open_link()

    for n in range(27):
        payload = bytes((n * 37 + i) & 0xFF for i in range(n % 9))   # lengths 0..8
        link.bus.send(ping(payload))
        pong = wait_for(link.bus, is_pong, timeout=5.0)
        assert pong is not None, f"ping {n} was not answered"
        assert pong.dlc == len(payload)
        assert bytes(pong.data) == payload


@pytest.mark.timing
def test_a_ping_is_answered_within_a_few_milliseconds(open_link):
    link = open_link()
    delays_ms = []
    for n in range(40):
        # Irregular spacing, so that the pings do not keep one phase to the ticker's period.
        time.sleep((7 + (n % 7) * 3) / 1000.0)
        payload = struct.pack("<I", n)
        sent = time.time()      # the clock of the bus's receive timestamps
        link.bus.send(ping(payload))
        pong = wait_for(link.bus, lambda m, p=payload: is_pong(m) and bytes(m.data) == p,
                        timeout=5.0)
        assert pong is not None, f"ping {n} was not answered"
        delays_ms.append((pong.timestamp - sent) * 1000.0)

    # Two frames of 4 data bytes on the bus (about 0.2 ms each at 500 kbit/s), the receive
    # interrupt waking the receive task, the transmit task woken by the pong it is given:
    # about 1 ms on a host with a CPU to spare. A task that is not woken but finds the frame
    # on its next periodic wake-up takes up to 10 ms (the ticker's publish) or 50 ms (its
    # idle wait).
    delays_ms.sort()
    median_ms = statistics.median(delays_ms)
    p90_ms = delays_ms[int(0.9 * len(delays_ms))]
    check_timing(median_ms < 3.0, f"ping to pong: median {median_ms:.2f} ms")
    check_timing(p90_ms < 5.0, f"ping to pong: 90th percentile {p90_ms:.2f} ms")


def test_only_a_base_format_data_frame_with_the_ping_identifier_is_a_ping(open_link):
    link = open_link("--duration-s", "2")
    not_pings = [
        can.Message(arbitration_id=ID_PING, data=b"\x01", is_extended_id=True),
        can.Message(arbitration_id=ID_PING, is_remote_frame=True, dlc=1, is_extended_id=False),
        can.Message(arbitration_id=0x123, data=b"\x02\x03", is_extended_id=False),
        can.Message(arbitration_id=0x1ABCDEF0, data=bytes(8), is_extended_id=True),
    ]
    for message in not_pings:
        link.bus.send(message)

    # The real ping goes last: its pong shows that the node has dealt with everything before
    # it, so "no pong for the others" needs no waiting time: the first pong is the one.
    link.bus.send(ping(b"\x55"))
    pong = wait_for(link.bus, is_pong, timeout=5.0)
    assert pong is not None and bytes(pong.data) == b"\x55"

    assert link.node.wait(timeout=20) == EXIT_OK
    assert drain(link.bus, keep=is_pong) == []
    driver = link.node.stats()["can"]["driver"]
    assert driver["rx_frames"] == len(not_pings) + 1
    assert driver["rx_unhandled"] == len(not_pings)


def test_a_burst_of_pings_is_answered_completely_and_in_order(open_link):
    link = open_link()
    burst = [struct.pack("<II", n, 0xC0FFEE00 + n) for n in range(30)]

    for payload in burst:
        link.bus.send(ping(payload))      # as fast as the tester can: far above the bit rate

    pongs: list[can.Message] = []
    while len(pongs) < len(burst):
        pong = wait_for(link.bus, is_pong, timeout=2.0)
        assert pong is not None, f"only {len(pongs)} of {len(burst)} pongs arrived"
        pongs.append(pong)

    assert [bytes(pong.data) for pong in pongs] == burst


@pytest.mark.timing
def test_a_burst_leaves_the_node_at_the_pace_of_the_bus(open_link):
    link = open_link()
    burst = [struct.pack("<II", n, 0) for n in range(30)]

    for payload in burst:
        link.bus.send(ping(payload))

    stamps = []
    while len(stamps) < len(burst):
        pong = wait_for(link.bus, is_pong, timeout=2.0)
        assert pong is not None
        stamps.append(pong.timestamp)

    # A base-format frame with 8 data bytes is at least 108 bits long and is followed by a
    # 3-bit intermission. The node writes a frame at its modelled end of frame and never
    # earlier, so 29 frame times must lie between the first and the last pong. The margin is
    # for the first write being late on the host.
    floor_s = (len(burst) - 1) * (108 + 3) / BITRATE
    span_s = stamps[-1] - stamps[0]
    check_timing(span_s >= 0.8 * floor_s,
                 f"30 pongs in {span_s * 1e3:.2f} ms, bus needs {floor_s * 1e3:.2f}")
    # And not much slower: pings and pongs share the bus, so the burst takes about 60 frame
    # times, 13 ms, and with stuff bits and the status frames about 20 ms. A transmit task
    # that is not woken when a mailbox frees up refills it only on its next wake-up, and the
    # burst then takes hundreds of milliseconds.
    check_timing(span_s < 0.1, f"30 pongs took {span_s * 1e3:.2f} ms")


def test_without_pacing_the_link_still_works(open_link):
    link = open_link("--bitrate", "0", "--duration-s", "1")
    link.bus.send(ping(b"\x01\x02\x03"))
    pong = wait_for(link.bus, is_pong, timeout=5.0)
    assert pong is not None and bytes(pong.data) == b"\x01\x02\x03"

    assert link.node.wait(timeout=20) == EXIT_OK
    stats = link.node.stats()["can"]
    assert stats["bitrate"] == 0
    assert stats["controller"]["busy_ns"] == 0      # a frame takes no time on an unpaced bus
    assert stats["controller"]["tx_frames"] >= 90


def test_every_participant_sees_every_frame(open_link, can_spec):
    link = open_link()
    second = open_bus(can_spec)
    try:
        # From the node to both testers ...
        assert wait_for(second, is_status, timeout=5.0) is not None
        # ... and from one tester to the node and to the other tester.
        link.bus.send(ping(b"\xAB\xCD"))
        seen_ping = wait_for_id(second, ID_PING, timeout=5.0)
        assert seen_ping is not None and bytes(seen_ping.data) == b"\xAB\xCD"
        for bus in (link.bus, second):
            pong = wait_for(bus, is_pong, timeout=5.0)
            assert pong is not None and bytes(pong.data) == b"\xAB\xCD"
    finally:
        second.shutdown()


# ------------------------------------------------------------------------------- accounting


def test_the_counters_of_all_layers_agree_after_a_run(open_link):
    link = open_link("--duration-s", "2")
    heard = link.before_first_status + [link.first_status]
    pings = 12

    for n in range(pings):
        link.bus.send(ping(struct.pack("<II", n, n)))
        while True:
            message = link.bus.recv(timeout=5.0)
            assert message is not None, f"ping {n} was not answered"
            heard.append(message)
            if is_pong(message):
                break

    # Everything else the node sends until it stops, and what was still on its way then.
    heard += collect_while(link.bus, lambda: link.node.running)
    assert link.node.wait(timeout=20) == EXIT_OK
    node_stats = link.node.stats()
    stats = node_stats["can"]
    queue, controller, driver = stats["queue"], stats["controller"], stats["driver"]
    messages = node_stats["telemetry"]["messages"].values()

    # The transmit queue loses nothing without saying so ...
    assert queue["produced"] == (queue["sent"] + queue["superseded"] + queue["dropped"]
                                 + queue["expired"] + queue["pending"])
    # ... and on a healthy bus it loses nothing it should not: no event frame (pong) is
    # dropped or expired, and a status value is only ever replaced by its successor when the
    # host stalled the node, each one visible as a gap in what the tester heard.
    assert queue["dropped"] == 0 and queue["expired"] == 0
    assert status_missing_since_start([m for m in heard if is_status(m)],
                                      max_step=None) == status_queue_counts(node_stats)["superseded"]
    assert queue["bus_off_count"] == 0 and queue["restart_requests"] == 0
    # At most a value of each cyclic message in a mailbox and its successor waiting for it,
    # and no event frame: the pongs were all heard before the node stopped.
    assert all(message["queue"]["pending"] <= 2 for message in messages)
    assert queue["pending"] == sum(message["queue"]["pending"] for message in messages)

    # The queue was sampled a moment before the controller stopped, so the controller may
    # have finished frames that were still pending.
    assert queue["sent"] <= controller["tx_frames"] <= queue["sent"] + queue["pending"]
    # With no bus fault, frames are aborted only when the queue calls the mailboxes back
    # because a value waited in its mailbox for a whole period (a host stall); each recall
    # aborts at most the three mailboxes (CAN_IO_TX_MAILBOXES).
    assert controller["tx_attempts_failed"] == 0
    assert controller["tx_aborted"] <= 3 * queue["recall_requests"]
    assert stats["state"] == "active" and stats["tec"] == 0 and stats["rec"] == 0

    # Every frame the controller completed was written to the transport and reached the tester.
    transport = stats["transport_counters"]
    assert transport["tx_frames"] == controller["tx_frames"] and transport["tx_failed"] == 0
    assert len(heard) == controller["tx_frames"]
    assert sum(1 for m in heard if is_pong(m)) == pings

    # Every ping passed transport, controller and driver exactly once.
    assert transport["rx_frames"] == controller["rx_frames"] == driver["rx_frames"] == pings
    assert transport["rx_malformed"] == 0 and transport["rx_overflow"] == 0
    assert controller["wire_overruns"] == 0 and controller["rx_lost_fault"] == 0
    assert driver["rx_unhandled"] == 0 and driver["rx_fifo_overruns"] == 0
    assert driver["event_fifo_overruns"] == 0 and driver["tx_load_failed"] == 0

    # Bus time: every frame here has 8 data bytes, which is 108 to 132 bits with stuffing.
    frames = controller["tx_frames"] + controller["rx_frames"]
    bits_per_frame = controller["busy_ns"] / frames / (1e9 / BITRATE)
    assert 108 <= bits_per_frame <= 132, bits_per_frame
    assert stats["pacing"]["frames"] == controller["tx_frames"]


# ----------------------------------------------------------------------- simulation control


def test_simulation_control_reports_the_controller(open_link):
    link = open_link()

    with link.node.sim() as sim:
        assert sim.ping() == link.node.pid

        before = sim.can_status()
        assert before["state"] == "active" and before["fault"] == "none"
        assert before["tec"] == 0 and before["rec"] == 0

        # Every frame the tester has heard by now, the first one and twenty more, was counted
        # by the controller before this reply, with its time on the bus: at least 108 bits of
        # 2 us each. (Some of the twenty may have been sent before `before` was sampled and
        # waited in the tester's socket, so the difference to `before` is not a bound.)
        assert len(collect_count(link.bus, 20, keep=is_status)) == 20
        after = sim.can_status()
        assert after["tx"] >= 1 + 20 and after["tx"] >= before["tx"]
        assert after["busy_ns"] >= (1 + 20) * 108 * (1e9 / BITRATE)
        assert after["busy_ns"] >= before["busy_ns"]

        with pytest.raises(SimError, match="unknown command"):
            sim.request("can.nonsense")
        with pytest.raises(SimError, match="unknown command"):
            sim.request("nonsense")
        with pytest.raises(SimError, match="unknown fault"):
            sim.can_fault("lightning")
        with pytest.raises(SimError, match="usage"):
            sim.request("can.inject tx")

    # None of that disturbed the node.
    link.bus.send(ping(b"\x99"))
    assert wait_for(link.bus, is_pong, timeout=5.0) is not None


def test_simulation_control_is_off_unless_asked_for(node_binary, workdir, ports):
    from fcn_tools.node import NodeProcess
    from fcn_tools.simctl import SimControl

    with NodeProcess(workdir=workdir, binary=node_binary, can=f"udp:{ports.bus}",
                     args=("--duration-s", "1")) as node:
        with SimControl(ports.sim, timeout=0.2) as sim:
            with pytest.raises(SimError, match="no reply"):
                sim.request("ping", retries=1)
        assert node.wait(timeout=20) == EXIT_OK
        assert node.lost_kicks() is None, node.lost_kicks()  # as tests/conftest.py checks its nodes


# --------------------------------------------------------------------------- start-up errors


def run_node(node_binary, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([str(node_binary), *args], capture_output=True, text=True, timeout=20)


@pytest.mark.parametrize("args, message", [
    (("--can", "carrier-pigeon"), "unknown CAN backend"),
    (("--can", "udp:0"), "unknown CAN backend"),
    (("--can", "socketcan:no-such-if0"), "socketcan:"),
    (("--bitrate", "2000000"), "invalid --bitrate"),
    (("--bitrate", "fast"), "invalid --bitrate"),
    (("--sim-port", "70000"), "invalid --sim-port"),
    (("--can-fault", "loose-wire"), "invalid --can-fault"),
])
def test_a_bad_bus_configuration_is_refused_at_start(node_binary, args, message):
    out = run_node(node_binary, "--duration-s", "1", *args)
    assert out.returncode == 1
    assert message in out.stderr


def test_the_highest_bit_rate_is_accepted(start_node):
    # 1 Mbit/s, the limit of classical CAN; one more is refused (above).
    node = start_node("--duration-s", "1", "--bitrate", "1000000")
    assert node.wait(timeout=20) == EXIT_OK
    can_stats = node.stats()["can"]
    assert can_stats["bitrate"] == 1_000_000
    assert can_stats["controller"]["tx_frames"] >= 90


def test_the_highest_port_number_is_accepted_for_simulation_control(node_binary, workdir, ports):
    from fcn_tools.node import NodeProcess

    # 65535 is in the range the system hands out to other programs' sockets: skip if one of
    # them has it right now (a test cannot reserve it without taking it from the node).
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.bind(("127.0.0.1", 65535))
        except OSError:
            pytest.skip("UDP port 65535 is in use on this machine")
    with NodeProcess(workdir=workdir, binary=node_binary, can=f"udp:{ports.bus}",
                     sim_port=65535, args=("--duration-s", "30")) as node:
        with SimControl(65535) as sim:
            assert sim.ping() == node.pid
        assert node.stop() == EXIT_OK
        assert node.lost_kicks() is None, node.lost_kicks()  # as tests/conftest.py checks its nodes


def test_a_full_udp_bus_refuses_another_participant(node_binary, ports):
    slots = []
    try:
        for slot in range(16):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            slots.append(sock)
            sock.bind(("127.0.0.1", ports.bus + slot))
        out = run_node(node_binary, "--duration-s", "1", "--can", f"udp:{ports.bus}")
    finally:
        for sock in slots:
            sock.close()
    assert out.returncode == 1
    assert "all 16 slots" in out.stderr


def test_a_taken_simulation_control_port_is_a_start_up_error(node_binary, ports):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as taken:
        taken.bind(("127.0.0.1", ports.sim))
        out = run_node(node_binary, "--duration-s", "1", "--can", f"udp:{ports.bus}",
                       "--sim-port", str(ports.sim))
    assert out.returncode == 1
    assert "cannot bind" in out.stderr
