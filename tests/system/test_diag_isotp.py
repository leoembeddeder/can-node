# SPDX-License-Identifier: MIT
"""Phase 5: the node's ISO-TP (lib/isotp behind app/diag.c) on the bus, frame by frame. The
tester writes the frames itself with python-can (diag_bench.RawLink), because the cases that
matter here are the ones a transport implementation never makes: a wrong sequence number, a
stray consecutive or flow control frame, a new single frame in the middle of a reception, a
first frame larger than the node's buffer, a tester that stops in the middle of a message, a
flow control WAIT, a reserved flow status. What a tester built on can-isotp sees is tested in
test_diag_services.py and test_download.py.

Every case runs on both transports (the UDP bus and SocketCAN vcan0): the node's frames go out
through its transmit queue and the controller model on either, and come back with their
transmit-done reports. Each test then compares the counters the node wrote when it stopped
(lib/isotp's results, frames and ignored frames) with what the tester did, exactly.

The node's parameters (app/diag.h): BS 16, STmin 0, a receive buffer of 1026 bytes, N_As, N_Ar,
N_Bs and N_Cr 1000 ms, up to 4 flow control WAITs accepted in a row, every frame of 8 bytes
padded with 0xCC.

Functional tests: they decide from the frames and the counters. A pause that looks for a frame
that must not come (diag_bench.QUIET_S) finds none from a correct node, and most such checks
need none: the node answers in the order it receives, so an answer that must not come would come
before the next one the test waits for. The tests that let a timer of the node run out wait
BEYOND_TIMEOUT_S, twice the timer, before they count on it. Only the separation time between the
node's consecutive frames is a timing tolerance (marked `timing`). What the tests need from the
host is what the protocol asks of any tester: its next frame within the node's timer while the
node waits for it (N_Bs, N_Cr: 1000 ms). A pause while the node waits for the tester's flow
control (test_the_node_keeps_to_the_tester_s_flow_control: after a block, after the WAITs and in
full duplex) leaves the host the timer less the pause, 0.7 s.
"""
from __future__ import annotations

import time

import pytest

from diag_bench import (BEYOND_TIMEOUT_S, PADDING, RawLink, flow_control, is_padded,
                        segments, show, single_frame, start_ready, used_bytes)
from fcn_tools.node import EXIT_OK
from fcn_tools.timing import check_timing

#: The DTCs of the fault table (app/fault.h): reportSupportedDTC answers 3 + 4 bytes each.
DTC_COUNT = 17
SUPPORTED_DTC_LENGTH = 3 + 4 * DTC_COUNT

#: The node's flow control after a first frame and after every 16 consecutive frames.
NODE_CTS = bytes([0x30, 0x10, 0x00]) + bytes([PADDING] * 5)
NODE_OVFLW = bytes([0x32, 0x00, 0x00]) + bytes([PADDING] * 5)

#: A request of 20 bytes, an FF and 2 CFs, answered 7F 22 13 (ReadDataByIdentifier with more
#: than a DID: incorrectMessageLengthOrInvalidFormat).
LONG_READ = bytes.fromhex("22 F1 86") + bytes(17)
LONG_READ_ANSWER = bytes.fromhex("03 7F 22 13 CC CC CC CC")
TESTER_PRESENT = bytes.fromhex("3E 00")
TESTER_PRESENT_ANSWER = bytes.fromhex("02 7E 00 CC CC CC CC CC")


def check_padding(frames: list[bytes]) -> None:
    """Every frame the node sent has 8 bytes, padded with 0xCC after its N_PCI and data: the
    remaining length of a multi-frame message is followed from its FF to its last CF."""
    remaining = None
    for frame in frames:
        kind = frame[0] >> 4
        if kind == 1:
            remaining = ((frame[0] & 0x0F) << 8 | frame[1]) - 6
        used = used_bytes(frame, remaining)
        if kind == 2:
            remaining -= 7
        assert is_padded(frame, used), f"not padded with 0xCC to 8 bytes: {show(frame)}"


def counts(stats: dict, group: str) -> dict:
    """lib/isotp's counters of `group` without the zeros."""
    return {name: count for name, count in stats["diag"]["isotp"][group].items() if count}


def stop(node) -> dict:
    assert node.stop() == EXIT_OK
    return node.stats()


def test_single_frames_and_frames_the_node_ignores(can_spec, start_node):
    """A single frame is answered with a single frame padded to 8 bytes; an unpadded one (its
    DLC covers its data) is taken as well. Frames shorter than their N_PCI needs, lengths the
    protocol does not allow, reserved N_PCI types, and a CF or an FC that nothing waits for are
    ignored: no answer, a count by reason, and the next request is answered as if they had not
    come."""
    node = start_ready(start_node, can_spec)
    with RawLink(can_spec) as link:
        link.send(single_frame(TESTER_PRESENT))
        assert link.next() == TESTER_PRESENT_ANSWER
        link.send(single_frame(TESTER_PRESENT), pad=False)            # DLC 3
        assert link.next() == TESTER_PRESENT_ANSWER
        ignored = [("02 3E", False, "SHORT"),          # SF_DL 2 in a frame of 2 bytes
                   ("", False, "SHORT"),               # DLC 0
                   ("00 3E 00", True, "LENGTH"),       # SF_DL 0: the CAN FD escape
                   ("08 3E 00 00 00 00 00 00", True, "LENGTH"),   # SF_DL 8 does not fit
                   ("10 07 22 F1 86 00 00 00", True, "LENGTH"),   # FF_DL 7: below FF_DLmin
                   ("40 3E 00", True, "RESERVED_TYPE"),
                   ("F2 3E 00", True, "RESERVED_TYPE"),
                   ("21 3E 00", True, "UNEXPECTED_CF"),  # no reception in progress
                   ("30 00 00", True, "UNEXPECTED_FC")]  # no transmission waits
        for data, pad, _ in ignored:
            link.send(bytes.fromhex(data), pad=pad)
        # The node answers its requests in their order: an answer to one of these frames would
        # come before this one's.
        link.send(single_frame(TESTER_PRESENT))
        assert link.next() == TESTER_PRESENT_ANSWER
        check_padding([bytes(frame.data) for frame in link.received])
    stats = stop(node)
    expected = {}
    for _, _, reason in ignored:
        expected[reason] = expected.get(reason, 0) + 1
    assert counts(stats, "ignored") == expected
    assert counts(stats, "rx_results") == {"OK": 3}
    assert counts(stats, "tx_results") == {"OK": 3}
    assert stats["diag"]["uds"]["requests"]["TesterPresent"] == 3


def test_a_long_request_gets_the_node_s_flow_control_every_16_frames(can_spec, start_node):
    """The node's flow control is CTS with BS 16 and STmin 0 (30 10 00, padded): after the first
    frame and after every 16th consecutive frame, never after the last. The sequence number
    wraps from 15 to 0. The longest request it takes is 1026 bytes; a first frame of 1027 bytes,
    and every first frame with the escape length, get OVFLW (32 00 00) and nothing of their
    message is taken."""
    node = start_ready(start_node, can_spec)
    with RawLink(can_spec) as link:
        # 200 bytes: TransferData in the default session, answered 7F 36 7F. An FF and 28 CFs.
        request = bytes([0x36, 0x01]) + bytes(range(198))
        first, cfs = segments(request)
        assert len(cfs) == 28 and [cf[0] for cf in cfs[14:17]] == [0x2F, 0x20, 0x21]
        link.send(first)
        assert link.next() == NODE_CTS
        for cf in cfs[:15]:
            link.send(cf)
        assert link.quiet() == [], "a flow control before the 16th consecutive frame"
        link.send(cfs[15])
        assert link.next() == NODE_CTS
        for cf in cfs[16:]:
            link.send(cf)
        # The answer is the node's next frame: no flow control after the last CF.
        assert link.next() == bytes.fromhex("03 7F 36 7F CC CC CC CC")

        # The longest message that fits: 1026 bytes, an FF and 146 CFs in blocks of 16.
        request = bytes([0x36, 0x01]) + bytes(1024)
        first, cfs = segments(request)
        link.send(first)
        flow_controls = 0
        for start in range(0, len(cfs), 16):
            assert link.next() == NODE_CTS
            flow_controls += 1
            for cf in cfs[start:start + 16]:
                link.send(cf)
        assert link.next() == bytes.fromhex("03 7F 36 7F CC CC CC CC")
        assert flow_controls == 10

        # One byte more: OVFLW, and its CFs are no reception's.
        first, cfs = segments(bytes([0x36, 0x01]) + bytes(1025))
        link.send(first)
        assert link.next() == NODE_OVFLW
        link.send(cfs[0])
        assert link.quiet() == []
        # The escape first frame: FF_DL 0 and a length of 32 bits. 4096 bytes: OVFLW; 4095 or
        # less is no escape (the 12-bit FF_DL takes it): ignored.
        link.send("10 00 00 00 10 00 36 01")
        assert link.next() == NODE_OVFLW
        link.send("10 00 00 00 0F FF 36 01")
        assert link.quiet() == []
        check_padding([bytes(frame.data) for frame in link.received])
    stats = stop(node)
    assert counts(stats, "rx_results") == {"OK": 2}
    assert stats["diag"]["isotp"]["rx_overflow"] == 2
    assert counts(stats, "frames_out") == {"SF": 2, "FC_CTS": 2 + 10, "FC_OVFLW": 2}
    assert counts(stats, "ignored") == {"UNEXPECTED_CF": 1, "LENGTH": 1}
    assert stats["diag"]["task"]["rx_lost"] == 0
    assert stats["diag"]["task"]["rx_high_water"] <= 16


def receive_message(link: RawLink, first: bytes) -> tuple[int, bytes]:
    """FF_DL and the data of the FF `first`."""
    assert first[0] >> 4 == 1, show(first)
    return (first[0] & 0x0F) << 8 | first[1], first[2:8]


def test_the_node_keeps_to_the_tester_s_flow_control(can_spec, start_node):
    """The node's answer of 71 bytes (reportSupportedDTC: an FF and 10 CFs) follows the tester's
    flow control: a block of BS CFs, then a wait for the next FC; up to 4 FC WAITs in a row and
    then CTS, but a fifth WAIT ends the transmission (N_WFT_OVRN, the node's bound,
    DIAG_TX_WFT_MAX); OVFLW and a reserved flow status end it; no FC at all ends it after N_Bs.
    A request that comes while the node waits for an FC is received (reception and transmission
    are independent), and its answer goes out after the transmission ahead of it."""
    node = start_ready(start_node, can_spec)
    with RawLink(can_spec) as link:
        link.send(single_frame(bytes.fromhex("19 0A")))
        length, data = receive_message(link, link.next())
        assert length == SUPPORTED_DTC_LENGTH and data[:3] == bytes.fromhex("59 0A FF")
        link.send(flow_control(block_size=3))
        cfs = [link.next() for _ in range(3)]
        assert [cf[0] for cf in cfs] == [0x21, 0x22, 0x23]
        assert link.quiet() == [], "more than BS consecutive frames"
        for _ in range(4):
            link.send(flow_control(status=1))
        assert link.quiet() == [], "a consecutive frame after a WAIT"
        link.send(flow_control())
        cfs += [link.next() for _ in range(7)]
        assert [cf[0] for cf in cfs] == [0x20 | sn for sn in range(1, 11)]
        message = data + b"".join(cf[1:] for cf in cfs)
        assert len(message) >= length
        message = message[:length]
        dtcs = [message[3 + 4 * n:6 + 4 * n].hex().upper() for n in range(DTC_COUNT)]
        assert dtcs[0] == "1A0100" and dtcs[-1] == "1B0600", dtcs

        # The ends of a transmission the tester makes: 5 WAITs, a reserved FS, OVFLW, no FC.
        for name, frames in (("WFT_OVRN", [flow_control(status=1)] * 5),
                             ("INVALID_FS", [flow_control(status=3)]),
                             ("BUFFER_OVFLW", [flow_control(status=2)])):
            link.send(single_frame(bytes.fromhex("19 0A")))
            assert link.next()[:2] == bytes([0x10, length])
            for frame in frames:
                link.send(frame)
            # A consecutive frame after it would come before the next first frame (and is
            # counted in frames_out below).
        link.send(single_frame(bytes.fromhex("19 0A")))
        assert link.next()[:2] == bytes([0x10, length])
        assert link.quiet(BEYOND_TIMEOUT_S) == [], "a frame without a flow control"

        # Full duplex: TesterPresent while the node waits for the FC of its answer.
        link.send(single_frame(bytes.fromhex("19 0A")))
        assert link.next()[:2] == bytes([0x10, length])
        link.send(single_frame(TESTER_PRESENT))
        assert link.quiet() == [], "the second answer must wait for the first"
        link.send(flow_control())
        rest = [link.next() for _ in range(10)]
        assert [cf[0] >> 4 for cf in rest] == [2] * 10
        assert link.next() == TESTER_PRESENT_ANSWER
        check_padding([bytes(frame.data) for frame in link.received])
    stats = stop(node)
    assert counts(stats, "tx_results") == {"OK": 3, "WFT_OVRN": 1, "INVALID_FS": 1,
                                           "BUFFER_OVFLW": 1, "TIMEOUT_BS": 1}
    assert counts(stats, "frames_in") == {"SF": 7, "FC_CTS": 3, "FC_WAIT": 9, "FC_OVFLW": 1,
                                          "FC_RESERVED": 1}
    assert counts(stats, "frames_out") == {"SF": 1, "FF": 6, "CF": 20}
    connection = stats["diag"]["connection"]
    assert (connection["responses"], connection["responses_sent"],
            connection["responses_failed"], connection["responses_dropped"]) == (7, 3, 4, 0)


def test_receptions_the_node_ends_and_what_it_does_after(can_spec, start_node):
    """A reception the tester breaks ends without an answer, and the next request is served: a
    consecutive frame with a wrong sequence number (N_WRONG_SN; the CF the reception expected,
    sent afterwards, is no reception's), a single frame or a first frame in the middle of a
    reception (N_UNEXP_PDU: the new message is received and answered), and a tester that stops
    in the middle of its message (N_TIMEOUT_Cr after 1000 ms: its CF afterwards is ignored)."""
    node = start_ready(start_node, can_spec)
    first, cfs = segments(LONG_READ)
    assert len(cfs) == 2
    with RawLink(can_spec) as link:
        link.send(first)
        assert link.next() == NODE_CTS
        link.send(bytes([0x22]) + cfs[0][1:])          # SN 2 where 1 is expected
        link.send(cfs[0])
        # An answer to anything before would come before the next flow control.

        link.send(first)
        assert link.next() == NODE_CTS
        link.send(cfs[0])
        link.send(single_frame(TESTER_PRESENT))          # ends the reception, is received
        assert link.next() == TESTER_PRESENT_ANSWER
        link.send(cfs[1])

        link.send(first)
        assert link.next() == NODE_CTS
        link.send(cfs[0])
        link.send(first)                                 # ends it, starts a new one
        assert link.next() == NODE_CTS
        link.send(cfs[0])
        link.send(cfs[1])
        assert link.next() == LONG_READ_ANSWER

        link.send(first)
        assert link.next() == NODE_CTS
        link.send(cfs[0])
        time.sleep(BEYOND_TIMEOUT_S)                     # N_Cr runs out in the node
        link.send(cfs[1])

        link.send(first)
        assert link.next() == NODE_CTS
        for cf in cfs:
            link.send(cf)
        assert link.next() == LONG_READ_ANSWER
        check_padding([bytes(frame.data) for frame in link.received])
    stats = stop(node)
    assert counts(stats, "rx_results") == {"OK": 3, "WRONG_SN": 1, "UNEXP_PDU": 2,
                                           "TIMEOUT_CR": 1}
    assert counts(stats, "ignored") == {"UNEXPECTED_CF": 3}
    assert stats["diag"]["isotp"]["ff_indications"] == 6


@pytest.mark.timing
@pytest.mark.parametrize("can_spec", ["udp"], indirect=True)
def test_the_node_keeps_the_separation_time_the_tester_asks_for(can_spec, start_node):
    """With STmin 20 ms (0x14) in the tester's flow control, the node's 10 consecutive frames
    of reportSupportedDTC come no closer than that, as the tester receives them (the node counts
    STmin from the confirmation of the frame before, so the gaps are a little longer). Timing:
    the gaps are measured on the host's clock, which a busy host can bunch up."""
    node = start_ready(start_node, can_spec)
    with RawLink(can_spec) as link:
        link.send(single_frame(bytes.fromhex("19 0A")))
        assert link.next()[0] >> 4 == 1
        link.send(flow_control(stmin=0x14))
        arrivals = []
        for _ in range(10):
            frame = link.next()
            arrivals.append(time.monotonic())
            assert frame[0] >> 4 == 2
    assert node.stop() == EXIT_OK
    gaps = [later - earlier for earlier, later in zip(arrivals, arrivals[1:])]
    print("gaps between the consecutive frames (ms):", [round(gap * 1000, 1) for gap in gaps])
    check_timing(min(gaps) >= 0.016, f"a gap of {min(gaps) * 1000:.1f} ms with STmin 20 ms")
    check_timing(arrivals[-1] - arrivals[0] >= 9 * 0.020 * 0.9,
                 f"9 gaps of {(arrivals[-1] - arrivals[0]) * 1000:.1f} ms in all")
