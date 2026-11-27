# SPDX-License-Identifier: MIT
"""Phase 5: the node's UDS server (lib/uds behind app/diag.c) on the bus, request by request:
every service, sub-function and negative response code a tester can reach, the order of the
checks for requests that break several rules at once, the sessions and S3server, the
suppressPosRspMsgIndicationBit, a request while another is pending, responsePending during an
erase, and ReadDTCInformation and ClearDiagnosticInformation against the DTCs the node reports in
BMS_Fault. The tester is can-isotp carrying plain bytes (diag_bench.Uds), so that it can send
what udsoncan would refuse to build.

Each test ends by comparing the node's own counters with what the tester saw
(diag_bench.check_counts()): every request by service, every negative answer by its NRC, the
positive and the suppressed answers and the responsePendings, exactly.

What the bus cannot reach, and is tested in lib/uds's unit tests (tests/unit/test_uds.c,
test_uds_download.c) instead: responseTooLong 0x14 (the node's response buffer, UDS_RESPONSE_MAX,
holds the longest response), a TransferData longer than maxNumberOfBlockLength (the node's
ISO-TP buffer of 1026 bytes answers its first frame with OVFLW, test_diag_isotp.py), generalReject
0x10 and an ECUReset the backend refuses (the node's backend never does), an empty request (ISO-TP
has no message of 0 bytes), and a write or exit that lib/boot refuses (only a fault of the flash
makes it, and a cut of its supply ends the run: test_download_cuts.py).

Functional tests decide from the node's answers and counters; the cadence of responsePending,
P2server, a session kept alive by TesterPresent and requests that come while an erase is pending
are timing tolerances (marked `timing`).
"""
from __future__ import annotations

import time

import pytest

from diag_bench import (BEYOND_S3_S, SLOT_A, SLOT_B, Uds, blocks, check_counts, check_table,
                        erase_request, hexbytes, location, show, start_ready, wait_until)
from fcn_tools import diag
from fcn_tools.bus import BusReader, open_bus
from fcn_tools.node import EXIT_OK
from fcn_tools.timing import check_timing
from update_bench import decoded, factory, info, states, wait_for_alive

#: The DTCs of the fault table, in its order (app/fault.h).
DTCS = [0x1A0100, 0x1A0200, 0x1A0300, 0x1A0400, 0x1A0500, 0x1A0600, 0x1A1000, 0x1A1100,
        0x1A1200, 0xD00100, 0xD00200, 0x1B0100, 0x1B0200, 0x1B0300, 0x1B0400, 0x1B0500,
        0x1B0600]

#: DiagnosticSessionControl's positive answer: P2server 50 ms (0x0032, 1 ms), P2*server 5000 ms
#: (0x01F4, 10 ms).
TIMING = "00 32 01 F4"

UDP_ONLY = pytest.mark.parametrize("can_spec", ["udp"], indirect=True)


def dtc_records(answer: bytes) -> list[tuple[int, int]]:
    """(DTC, status) of each record of a 59 02 or 59 0A answer."""
    assert answer[2] == 0xFF and (len(answer) - 3) % 4 == 0, show(answer)
    return [(int.from_bytes(answer[at:at + 3], "big"), answer[at + 3])
            for at in range(3, len(answer), 4)]


def erase_with_probes(uds: Uds, erase: str, size: int, probes: list[str]) -> list[bytes]:
    """An eraseMemory of `size` bytes asked for with the SPRMIB (`31 81 ` and `erase`, the rest
    of the request), and, once its first responsePending is in, each of the requests `probes`,
    the next once the one before is answered. Returns the final answers in the order they came,
    the erase's among them; its responsePendings are counted in `uds`."""
    uds.send("31 81 " + erase)
    first = uds.receive(timeout=2.0)
    assert first == hexbytes("7F 31 78"), show(first)
    uds.pendings += 1
    if probes:
        uds.send(probes[0])
    arrived = []
    deadline = time.monotonic() + diag.erase_timeout_s(size)
    while len(arrived) < 1 + len(probes):
        message = uds.receive(timeout=max(0.0, deadline - time.monotonic()))
        assert message is not None, f"answers so far: {[show(m) for m in arrived]}"
        if message == hexbytes("7F 31 78"):
            uds.pendings += 1
            continue
        uds.answers.append(message)
        arrived.append(message)
        answered = len([m for m in arrived if m[0] != 0x71])
        if message[0] != 0x71 and answered < len(probes):
            uds.send(probes[answered])
    return arrived


# ------------------------------------------------------------------ the sessions without flash


@UDP_ONLY
def test_the_default_extended_and_programming_sessions_answer_as_docs_diagnostics_md_says(
        can_spec, start_node):
    """A node without a flash and without the bootloader (no hand-off): every request of this
    table, in its order, gets exactly its answer, in the session the requests before it left.
    Among them the general checks in their order (busy is tested below): an unknown service
    (0x11), a service outside the session (0x7F) before any length, the length of a
    sub-function service (0x13), its sub-function (0x12) and its session (0x7E) before the
    service's own length check; the SPRMIB, which suppresses a positive answer and never a
    negative one; the session transitions; and the services' own checks."""
    node = start_ready(start_node, can_spec)
    table = [
        # TesterPresent
        ("3E 00", "7E 00"),
        ("3E 80", None),                  # SPRMIB: no answer
        ("3E", "7F 3E 13"),               # step 4: a sub-function service needs 2 bytes
        ("3E 00 00", "7F 3E 13"),         # its own length check
        ("3E 01", "7F 3E 12"),
        ("3E 81", "7F 3E 12"),            # a negative answer is never suppressed
        # ReadDataByIdentifier, without a hand-off
        ("22 F1 86", "62 F1 86 01"),
        ("22 F1 89", "7F 22 22"),         # no image header: no version to give
        ("22 FD 00", "62 FD 00 01 00 FF 00 00 00 00 FF 00 00 00 00"),
        ("22 F1", "7F 22 13"),
        ("22 F1 86 F1 89", "7F 22 13"),   # one DID per request
        ("22 F1 86 00", "7F 22 13"),
        ("22 12 34", "7F 22 31"),
        # DiagnosticSessionControl, in the default session
        ("10", "7F 10 13"),
        ("10 01 00", "7F 10 13"),
        ("10 04", "7F 10 12"),
        ("10 84", "7F 10 12"),
        ("10 04 00", "7F 10 12"),         # step 5 before the service's length check
        ("10 02", "7F 10 7E"),            # programming is entered from extended only
        ("10 02 00", "7F 10 7E"),         # step 6 before the service's length check
        ("10 01", f"50 01 {TIMING}"),
        # ECUReset: hardReset only (its positive answer resets: test_download_faults.py)
        ("11", "7F 11 13"),
        ("11 02", "7F 11 12"),
        ("11 03", "7F 11 12"),
        ("11 04", "7F 11 12"),
        ("11 05", "7F 11 12"),
        ("11 01 00", "7F 11 13"),
        # services the node does not have (docs/diagnostics.md, not implemented)
        ("27 01", "7F 27 11"),            # SecurityAccess
        ("28 00 01", "7F 28 11"),         # CommunicationControl
        ("85 02", "7F 85 11"),            # ControlDTCSetting
        ("2E F1 90 00", "7F 2E 11"),      # WriteDataByIdentifier
        ("35 00 44 00 00 20 00 00 00 10 00", "7F 35 11"),   # RequestUpload
        ("29 00", "7F 29 11"),            # Authentication
        # the programming session's services, and RoutineControl, in the default session:
        # step 3 before any check of length
        ("34", "7F 34 7F"),
        ("34 00 44 00 10 20 00 00 00 10 00", "7F 34 7F"),
        ("36 01 00", "7F 36 7F"),
        ("37", "7F 37 7F"),
        ("31", "7F 31 7F"),
        ("31 01 FF 00", "7F 31 7F"),
        # ReadDTCInformation's checks (its answers below)
        ("19", "7F 19 13"),
        ("19 01", "7F 19 13"),
        ("19 02", "7F 19 13"),
        ("19 01 FF 00", "7F 19 13"),
        ("19 0A 00", "7F 19 13"),
        ("19 03 FF", "7F 19 12"),
        ("19 81 FF", "7F 19 12"),         # no SPRMIB: 0x81 is a reportType it does not have
        ("19 8A", "7F 19 12"),
        # ClearDiagnosticInformation
        ("14 FF FF", "7F 14 13"),
        ("14 FF FF FF 00 00", "7F 14 13"),
        ("14 00 00 01", "7F 14 31"),      # a group other than all
        ("14 FF FF FF 00", "7F 14 31"),   # a MemorySelection
        ("14 FF FF FF", "54"),
        # into the extended session with the SPRMIB: the session changes, no answer
        ("10 83", None),
        ("22 F1 86", "62 F1 86 03"),
        ("10 03", f"50 03 {TIMING}"),     # the session begins again
        # RoutineControl in the extended session
        ("31", "7F 31 13"),               # step 4 now that the service is in its session
        ("31 02", "7F 31 12"),            # step 5 before the routine's length check
        ("31 02 F0 00", "7F 31 12"),      # stopRoutine is not supported
        ("31 04 F0 00", "7F 31 12"),
        ("31 01 F0", "7F 31 13"),
        ("31 01 12 34", "7F 31 31"),      # no such routine
        ("31 01 FF 00 44 00 10 20 00 00 00 10 00", "7F 31 31"),   # eraseMemory: programming only
        ("31 01 FF 01", "7F 31 31"),      # checkProgrammingDependencies: programming only
        ("31 03 F0 00", "7F 31 24"),      # requestRoutineResults before any start ran
        ("31 01 F0 00 00", "7F 31 13"),   # the confirm routine takes no option record
        ("31 01 F0 00", "7F 31 22"),      # no trial to confirm (no flash: BOOT_E_STATE)
        ("31 03 F0 00", "71 03 F0 00 01 22"),   # the start that ran: failed, with 0x22
        ("31 03 F0 00 00", "7F 31 13"),
        ("31 81 F0 00", "7F 31 22"),      # SPRMIB: the negative answer all the same
        ("34 00 44 00 10 20 00 00 00 10 00", "7F 34 7F"),   # the download: programming only
        ("14 FF FF FF", "54"),
        # into the programming session, and what it does not have
        ("10 02", f"50 02 {TIMING}"),
        ("22 F1 86", "62 F1 86 02"),
        ("31 03 F0 00", "7F 31 31"),      # the results were forgotten, and F000 is extended's
        ("10 03", "7F 10 7E"),            # not from programming to extended
        ("19 01 FF", "7F 19 7F"),
        ("19 0A", "7F 19 7F"),
        ("14 FF FF FF", "7F 14 7F"),
        ("31 01 F0 00", "7F 31 31"),      # the confirm routine is extended's
        ("31 01 FF 01", "7F 31 24"),      # the check routine: no download to check
        ("3E 00", "7E 00"),
        ("10 02", f"50 02 {TIMING}"),
        ("10 01", f"50 01 {TIMING}"),
        ("22 F1 86", "62 F1 86 01"),
    ]
    with Uds(can_spec) as uds:
        check_table(uds, table)
        # ReadDTCInformation's answers: the availability mask 0xFF, the DTC format 0x01, every
        # DTC of the fault table in its order, and the lists and the count by mask agree.
        supported = dtc_records(uds.request("19 0A").response)
        assert [dtc for dtc, _ in supported] == DTCS
        for mask in (0x00, 0x01, 0x10, 0x40, 0x50, 0xFF):
            listed = dtc_records(uds.request(bytes([0x19, 0x02, mask])).response)
            current = dtc_records(uds.request("19 0A").response)
            if mask == 0x00:
                assert listed == []                     # (status & 0) never matches
            count = uds.request(bytes([0x19, 0x01, mask])).response
            assert count[:4] == hexbytes("59 01 FF 01"), show(count)
            # A list is a snapshot of the health task's last period: the reads in between may
            # see another one, so only the DTCs whose status did not change are compared.
            steady = {dtc for (dtc, status), (_, again) in zip(supported, current)
                      if status == again}
            assert [dtc for dtc, status in listed if dtc in steady] == [
                dtc for dtc, status in current if dtc in steady and status & mask]
        assert not uds.isotp_errors, uds.isotp_errors
    assert node.stop() == EXIT_OK
    stats = node.stats()
    check_counts(stats, uds)
    assert stats["diag"]["session"] == "defaultSession"
    # Counted when the session differs: 10 83 (extended), 10 02 (programming), 10 01 (default).
    assert stats["diag"]["uds"]["session_changes"] == 3
    assert stats["diag"]["uds"]["s3_timeouts"] == 0
    assert stats["faults"]["clears"] == 2
    assert stats["diag"]["backend"]["clears"] == stats["diag"]["backend"]["clears_done"] == 2


# ------------------------------------------------------------------- the download's services


@UDP_ONLY
def test_the_download_services_check_their_requests_in_order(can_spec, start_node, images,
                                                             workdir):
    """The programming session with a flash, started through the bootloader (factory image 1.0.0
    in slot A): every check of eraseMemory, RequestDownload, TransferData, RequestTransferExit,
    checkProgrammingDependencies and requestRoutineResults in its order, the refusals of lib/boot
    behind them (no slot at the address, the running slot, a size of 0 or beyond the slot, a
    trial pending), an erase asked for with the SPRMIB, the repeated block,
    transferDataSuspended, which ends the download, and a whole download that commits the
    trial. (A request while the erase is pending: test_requests_while_an_erase_is_pending_...)"""
    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    data = images["data"].read_bytes()
    size = len(data)
    node = start_ready(start_node, can_spec, flash_file=flash, retained_file=workdir / "cell.bin")
    image_blocks = list(blocks(data))
    assert len(image_blocks) == 10 and len(image_blocks[-1][1]) == size - 9 * 1024

    def erase(address: int, length: int) -> str:
        return erase_request(address, length).hex(" ")

    def request_download(address: int, length: int, dfi: int = 0x00) -> str:
        return (bytes([0x34, dfi]) + location(address, length)).hex(" ")

    def block(counter: int, payload: bytes) -> str:
        return (bytes([0x36, counter]) + payload).hex(" ")

    with Uds(can_spec) as uds:
        boot = diag.decode_boot_info(uds.request("22 FD 00").response[3:])
        assert (boot.slot, boot.mode, boot.attempt, boot.confirm_state, boot.image_version) == (
            "A", "confirmed", 0, "NOT_NEEDED", "1.0.0"), boot
        check_table(uds, [
            ("22 F1 89", "62 F1 89 " + b"1.0.0".hex(" ")),
            ("10 03", f"50 03 {TIMING}"),
            ("10 02", f"50 02 {TIMING}"),
            ("31 03 FF 00", "7F 31 24"),          # no erase has run
            ("31 01 FF 01", "7F 31 24"),          # no download to check
            ("31 01 FF 01 00", "7F 31 13"),       # its length before that
            # RequestDownload: 0x13 below 3 bytes, ALFID 0x31, 0x13 not 11 bytes, DFI 0x31,
            # 0x22 in TRANSFER, 0x70 nothing erased, 0x31 not the erase's address and size
            ("34", "7F 34 13"),
            ("34 00", "7F 34 13"),
            ("34 00 33 00", "7F 34 31"),          # the ALFID before the length of the rest
            ("34 00 44 00 10 20", "7F 34 13"),
            ("34 11 44 00", "7F 34 13"),          # the length before the DFI
            (request_download(SLOT_B, size, dfi=0x11), "7F 34 31"),   # before 0x70
            (request_download(SLOT_B, size), "7F 34 70"),
            # TransferData and RequestTransferExit without a download
            ("36", "7F 36 13"),
            ("36 01 00", "7F 36 24"),
            ("37 00", "7F 37 13"),
            ("37", "7F 37 24"),
            # eraseMemory: 0x13 no option record, ALFID 0x31, 0x13 not 13 bytes, then lib/boot
            ("31 01 FF 00", "7F 31 13"),
            ("31 01 FF 00 33", "7F 31 31"),       # the ALFID before the length
            ("31 01 FF 00 44 00 10 20", "7F 31 13"),
            (erase(0x001000, size), "7F 31 31"),  # no slot starts there: BOOT_E_ARG
            (erase(SLOT_B + 0x1000, size), "7F 31 31"),
            (erase(SLOT_A, size), "7F 31 22"),    # the running slot: BOOT_E_STATE
            (erase(SLOT_B, 0), "7F 31 31"),       # BOOT_E_SIZE
            (erase(SLOT_B, 0x100001), "7F 31 31"),
            ("31 03 FF 00", "71 03 FF 00 01 31"),  # the last start that ran, and its NRC
            ("31 03 FF 00 00", "7F 31 13"),
        ])

        # An erase of 32 sectors asked for with the SPRMIB (31 81): once a responsePending has
        # gone out, its final positive answer comes all the same. (Requests while it is pending:
        # the timing test below.)
        big = 32 * 4096
        assert erase_with_probes(uds, erase(SLOT_B, big)[6:], big, probes=[]) == [
            hexbytes("71 01 FF 00 00 00")]

        last_counter, last = image_blocks[-1]
        check_table(uds, [
            ("31 03 FF 00", "71 03 FF 00 00 00"),
            (request_download(SLOT_B, size), "7F 34 31"),   # not the size the erase was for
            (request_download(SLOT_A, big), "7F 34 31"),    # not its address
            (erase(SLOT_B, size), "71 01 FF 00 00 00"),     # begins again
            (request_download(SLOT_B, size), "74 20 04 02"),
            (request_download(SLOT_B, size), "7F 34 22"),   # in TRANSFER already
            (erase(SLOT_B, size), "7F 31 22"),              # not in TRANSFER; it goes on
            ("37", "7F 37 24"),                             # bytes missing
            (block(2, image_blocks[1][1]), "7F 36 73"),     # the first block is 01
            ("36 01", "7F 36 13"),                          # no data
            (block(0, image_blocks[0][1]), "7F 36 73"),
            (block(1, image_blocks[0][1]), "76 01"),
            (block(1, image_blocks[0][1]), "76 01"),        # a repeat: not written again
            (block(1, bytes(1024)), "76 01"),               # its data are not compared
            (block(3, image_blocks[2][1]), "7F 36 73"),
            *[(block(counter, payload), f"76 {counter:02X}")
              for counter, payload in image_blocks[1:-1]],
            (block(last_counter, last + b"\x00"), "7F 36 71"),   # more than the size leaves
            (block(last_counter, last), "7F 36 24"),         # the download has ended
            ("37", "7F 37 24"),
            (request_download(SLOT_B, size), "7F 34 70"),   # nothing erased for one any more
            # The whole download, once more from the erase.
            (erase(SLOT_B, size), "71 01 FF 00 00 00"),
            (request_download(SLOT_B, size), "74 20 04 02"),
            *[(block(counter, payload), f"76 {counter:02X}") for counter, payload in image_blocks],
            (block(last_counter + 1, b"\x00"), "7F 36 24"),   # every byte taken
            (block(last_counter, last), f"76 {last_counter:02X}"),   # the last one repeated
            ("37 00", "7F 37 13"),
            ("31 01 FF 01", "7F 31 24"),          # the check before the exit
            ("37", "77"),
            ("37", "7F 37 24"),                   # no transfer any more
            (block(last_counter, last), "7F 36 24"),
            ("31 01 FF 01 00", "7F 31 13"),
            ("31 01 FF 01", "71 01 FF 01 00 00"),
            ("31 01 FF 01", "7F 31 24"),          # once
            ("31 03 FF 01", "71 03 FF 01 00 00"),
            (erase(SLOT_B, size), "7F 31 22"),    # a trial is pending: BOOT_E_STATE
            (erase(SLOT_A, size), "7F 31 22"),    # its fallback would be overwritten
            ("10 01", f"50 01 {TIMING}"),
        ])
        assert not uds.isotp_errors, uds.isotp_errors
    assert node.stop() == EXIT_OK
    stats = node.stats()
    check_counts(stats, uds)
    download = stats["diag"]["uds"]["download"]
    assert (download["erases"], download["started"], download["exited"], download["committed"],
            download["aborted"], download["failed"]) == (3, 2, 1, 1, 0, 1), download
    assert (download["received"], download["blocks"], download["repeated_blocks"],
            download["counter"], download["state"]) == (size, 10, 1, last_counter, "idle")
    flash_info = info(flash)
    assert states(flash_info) == ("confirmed", "trial")
    assert flash_info["slots"][1]["header"]["version"] == "1.5.0"


# ----------------------------------------------------------------------------------- timing


@UDP_ONLY
@pytest.mark.timing
def test_response_pending_comes_40_ms_after_the_request_and_then_every_2_s(
        can_spec, start_node, images, workdir):
    """An eraseMemory of 110 sectors (about 5 s at the part's typical 45 ms): responsePending
    40 ms after the request, then every 2000 ms after the one before, then the final answer;
    and requests that need no flash answered within P2server (50 ms). The number of
    responsePendings is the node's count (functional); when they came is the host's clock."""
    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    node = start_ready(start_node, can_spec, flash_file=flash, retained_file=workdir / "cell.bin")
    size = 110 * 4096
    with Uds(can_spec) as uds:
        uds.request("10 03")
        uds.request("10 02")
        erased = uds.request(erase_request(SLOT_B, size), timeout=diag.erase_timeout_s(size))
        round_trips = sorted(uds.request("3E 00").took_s for _ in range(20))
    assert node.stop() == EXIT_OK
    stats = node.stats()
    check_counts(stats, uds)
    assert erased.response == hexbytes("71 01 FF 00 00 00")
    pending = erased.pending_s
    print(f"responsePending at {[round(t, 3) for t in pending]} s, the answer at "
          f"{erased.took_s:.3f} s; TesterPresent round trips {round_trips[0] * 1000:.1f} to "
          f"{round_trips[-1] * 1000:.1f} ms")
    assert len(pending) >= 1
    check_timing(len(pending) == 3, f"{len(pending)} responsePending in {erased.took_s:.2f} s")
    check_timing(0.030 <= pending[0] <= 0.150, f"the first responsePending at {pending[0]:.3f} s")
    gaps = [later - earlier for earlier, later in zip(pending, pending[1:])]
    check_timing(all(1.9 <= gap <= 2.3 for gap in gaps), f"responsePending every {gaps} s")
    check_timing(round_trips[18] <= diag.P2_SERVER_S,
                 f"19 of 20 TesterPresent answered within {round_trips[18] * 1000:.1f} ms")


@UDP_ONLY
@pytest.mark.timing
def test_requests_while_an_erase_is_pending_get_busy_repeat_request(can_spec, start_node, images,
                                                                    workdir):
    """While an erase is pending a request gets busyRepeatRequest, whatever it is (step 1, before
    the SID's check): a TesterPresent and an unknown SID, the first sent once the erase's first
    responsePending is in (40 ms), the second once the first is answered; the erase of 32 sectors
    takes 1.4 s at the part's typical time. Each answer is checked against the order in which the
    node answered (one that came before the erase's final answer was processed while it was
    pending), which no host can change. That both came while it was pending is a timing
    tolerance: a host that holds the tester up for longer than the erase leaves nothing pending."""
    flash = workdir / "flash.bin"
    factory(flash, images["node-1.0.0"])
    node = start_ready(start_node, can_spec, flash_file=flash, retained_file=workdir / "cell.bin")
    big = 32 * 4096
    probes = [("3E 00", "7F 3E 21", "7E 00"), ("27 01", "7F 27 21", "7F 27 11")]
    with Uds(can_spec) as uds:
        assert uds.request("10 03").positive and uds.request("10 02").positive
        arrived = erase_with_probes(uds, erase_request(SLOT_B, big).hex(" ")[6:], big,
                                    [request for request, _, _ in probes])
        assert not uds.isotp_errors, uds.isotp_errors
    assert node.stop() == EXIT_OK
    check_counts(node.stats(), uds)
    erased_at = next(i for i, m in enumerate(arrived) if m[0] == 0x71)
    assert arrived[erased_at] == hexbytes("71 01 FF 00 00 00"), show(arrived[erased_at])
    answers = [m for m in arrived if m[0] != 0x71]
    for number, (answer, (_, busy, normal)) in enumerate(zip(answers, probes)):
        # The node answers in the order it processes, so a probe answered before the erase's
        # final answer was processed while the erase was pending.
        before = arrived.index(answer) < erased_at
        assert answer == hexbytes(busy if before else normal), (number, show(answer))
    check_timing(erased_at == 2, "the erase had ended before the tester's second request came: "
                                 "the host held the tester up for more than the erase's 1.4 s")


@UDP_ONLY
@pytest.mark.timing
def test_tester_present_keeps_the_session_and_silence_ends_it(can_spec, start_node):
    """In the extended session, a suppressed TesterPresent every second keeps the session past
    S3server (5 s) without an answer to any of them; then, after more than S3server of silence,
    the node is in the default session and has counted one S3server timeout. That the session
    was kept is a timing tolerance (the gaps between the requests are the host's); that silence
    ends it is not: the tester's silence starts after the answer, the node's timer before it."""
    node = start_ready(start_node, can_spec)
    with Uds(can_spec) as uds:
        assert uds.request("10 03").positive
        for _ in range(8):
            time.sleep(1.0)
            uds.send("3E 80")
        kept = uds.request("22 F1 86").response
        time.sleep(BEYOND_S3_S)
        after = uds.request("22 F1 86").response
    assert node.stop() == EXIT_OK
    stats = node.stats()
    check_counts(stats, uds)
    assert after == hexbytes("62 F1 86 01")
    check_timing(kept == hexbytes("62 F1 86 03") and stats["diag"]["uds"]["s3_timeouts"] == 1,
                 f"the session after 8 TesterPresent 1 s apart: {show(kept)}, "
                 f"{stats['diag']['uds']['s3_timeouts']} S3server timeouts")


# ------------------------------------------------------------------------ the DTC services


def fault_frames(reader: BusReader):
    return decoded(reader.frames, "BMS_Fault")


@UDP_ONLY
def test_read_dtc_information_and_the_clear_agree_with_bms_fault(can_spec, start_node):
    """A cell over its voltage limit (simulation control) becomes the DTC CELL_OVERVOLTAGE: the
    node's BMS_Fault says it is active and confirmed, and ReadDTCInformation says the same of its
    status byte (testFailed, confirmedDTC), lists it by those masks and counts it, while the
    other DTCs agree with the frame too. Once the cell is back, BMS_Fault and 0x19 agree that it
    is no longer active and still confirmed; ClearDiagnosticInformation is answered once the
    health task has cleared it (0x19 straight after the answer, with no wait, finds it confirmed
    no more), and the next BMS_Fault (FaultClearCount 1) and 0x19 both say so as well."""
    bus = open_bus(can_spec)
    try:
        with BusReader(bus) as reader:
            node = start_ready(start_node, can_spec)
            wait_for_alive(node, 2)          # every DTC tested once (test_download.py)
            with node.sim() as sim, Uds(can_spec) as uds:

                def latest() -> tuple[int, int, int]:
                    frame = fault_frames(reader)[-1]
                    return (frame.raw["FaultActive"], frame.raw["FaultConfirmed"],
                            frame.raw["FaultClearCount"])

                def steady() -> tuple[int, int, int]:
                    """BMS_Fault's masks once three frames in a row agree."""
                    def same() -> bool:
                        frames = fault_frames(reader)[-3:]
                        return len(frames) == 3 and len({(f.raw["FaultActive"],
                                                          f.raw["FaultConfirmed"])
                                                         for f in frames}) == 1
                    wait_until(same, "three BMS_Fault frames alike", running=lambda: node.running)
                    return latest()

                def agree(active: int, confirmed: int) -> list[tuple[int, int]]:
                    """0x19 0A against the masks of BMS_Fault, DTC by DTC."""
                    records = dtc_records(uds.request("19 0A").response)
                    assert [dtc for dtc, _ in records] == DTCS
                    for number, (dtc, status) in enumerate(records):
                        assert bool(status & 0x01) == bool(active >> number & 1), (hex(dtc), status)
                        assert bool(status & 0x08) == bool(confirmed >> number & 1), (hex(dtc),
                                                                                      status)
                    return records

                sim.sensor_set("cell3", 4300)
                wait_until(lambda: fault_frames(reader) and latest()[0] & 1 and latest()[1] & 1,
                           "CELL_OVERVOLTAGE active and confirmed in BMS_Fault",
                           running=lambda: node.running)
                active, confirmed, clears = steady()
                assert (active, confirmed, clears) == (1, 1, 0)
                agree(active, confirmed)
                assert dtc_records(uds.request("19 02 01").response)[0][0] == DTCS[0]
                assert [dtc for dtc, _ in dtc_records(uds.request("19 02 08").response)] == [
                    DTCS[0]]
                assert uds.request("19 01 09").response == hexbytes("59 01 FF 01 00 01")

                sim.sensor_clear("cell3")
                wait_until(lambda: latest()[0] == 0, "the over-voltage gone from FaultActive",
                           running=lambda: node.running)
                active, confirmed, clears = steady()
                assert (active, confirmed, clears) == (0, 1, 0)
                agree(active, confirmed)
                assert dtc_records(uds.request("19 02 01").response) == []
                assert uds.request("19 01 08").response == hexbytes("59 01 FF 01 00 01")

                assert uds.request("14 FF FF FF").response == hexbytes("54")
                # The answer came once the clear was made: the very next read, with no wait,
                # finds the DTC confirmed no more. (After the clear a confirmed bit comes back
                # only with a fault, and the cell is back: this holds however late the read
                # comes. A node that answered before the clear still has it confirmed here,
                # unless the health task's next period, which makes the clear, falls between.)
                assert dtc_records(uds.request("19 02 08").response) == []
                assert uds.request("19 01 08").response == hexbytes("59 01 FF 01 00 00")
                # And the frame of the clear's period has it.
                wait_until(lambda: latest()[2] == 1, "BMS_Fault with FaultClearCount 1",
                           running=lambda: node.running)
                active, confirmed, clears = steady()
                assert (active, confirmed, clears) == (0, 0, 1)
                agree(active, confirmed)
                assert dtc_records(uds.request("19 02 08").response) == []
                assert uds.request("19 01 08").response == hexbytes("59 01 FF 01 00 00")
                assert not uds.isotp_errors, uds.isotp_errors
            assert node.stop() == EXIT_OK
    finally:
        bus.shutdown()
    stats = node.stats()
    check_counts(stats, uds)
    assert stats["faults"]["clears"] == 1
    assert "DTCs cleared: ClearDiagnosticInformation (clear 1)" in node.log()
    table = {entry["dtc"]: entry for entry in stats["faults"]["dtcs"]}
    assert not table[DTCS[0]]["status"] & 0x09, table[DTCS[0]]
