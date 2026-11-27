# SPDX-License-Identifier: MIT
"""The tester's two ends of the node's diagnostic connection, and the download as a sequence of
plain requests: the bench of phase 5's system tests of the diagnostic connection
(tests/system/diag_bench.py imports it: test_diag_isotp.py, test_diag_services.py,
test_download_faults.py, test_download_cuts.py) and of the measurements (bench/). It moved here
from tests/system/diag_bench.py in phase 6 unchanged.

  RawLink     python-can frames on UDS_Request 0x7E0 and UDS_Response 0x7E8, one at a time, for
              what a transport implementation never sends: a wrong sequence number, a stray
              frame, a first frame too large for the node, a tester that stops in the middle of
              a message. The node's frames are read in a thread of their own into a queue.
  Uds         can-isotp 2.0.7 with the tester's parameters of fcn_tools.diag (ISOTP_PARAMS),
              carrying the bytes of a request as they are, with no udsoncan above it: the tests
              make requests udsoncan would refuse to build (a wrong length, a sub-function the
              service does not have, two requests at once) and read the bytes of every answer,
              each responsePending with the time it arrived.
  download()  the programming sequence of docs/diagnostics.md with Uds, request by request, for
              the tests that stop it, cut it or break it on purpose (the flasher, python -m
              fcn_tools.flasher, makes the whole one and is tested end to end in
              test_download.py; run_flasher() runs it).

Every wait here is a bound, never a timing tolerance: a test decides from what the node
answered and counted, and a tester that waits longer than a bound has found the node silent.
Waits that a correct node can never make fail (a frame that must not come, looked for during a
pause) only make a test less sensitive on a host that stalls. What every exchange needs from the
host is what the protocol asks of any tester: its next frame within the node's timer while the
node waits for it (N_Bs and N_Cr, 1000 ms; S3server, 5000 ms, between two requests in a session
other than the default one); a pause that runs while the node waits for the tester uses up part
of that timer.

The checks here are assert statements, as in the tests, and fail with AssertionError; so the
module refuses to be imported under python -O, which would remove them.
"""
from __future__ import annotations

import json
import queue
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterator, Sequence

import can
import isotp

from . import bus as bus_module
from . import diag

if not __debug__:  # pragma: no cover - only under python -O
    raise ImportError("fcn_tools.tester checks with assert statements, which python -O removes: "
                      "run it without -O")

__all__ = [
    "ANSWER_WAIT_S", "Answer", "BEYOND_S3_S", "BEYOND_TIMEOUT_S", "BLOCK_DATA",
    "DATA_PAYLOAD_SIZE", "DATA_SIZE", "FLASHER_TIMEOUT_S", "MAX_BLOCK_LENGTH", "NODE_BS",
    "NODE_RX_BUFFER", "NODE_STMIN", "NODE_TIMEOUT_S", "NRC_NAMES", "NodeEnded", "PADDING",
    "Progress", "QUIET_S", "REQUEST_ID", "RESPONSE_ID", "RawLink", "S3_SERVER_S", "SERVICE_NAMES",
    "SLOT_A", "SLOT_B", "START_WAIT_S", "STEPS", "Uds", "blocks", "check_counts", "check_table",
    "download", "download_request", "erase_request", "flow_control", "hexbytes", "is_padded",
    "location", "nrc_counts", "run_flasher", "segments", "show", "single_frame", "start_ready",
    "steps_of", "used_bytes", "wait_until",
]

REQUEST_ID = diag.REQUEST_ID
RESPONSE_ID = diag.RESPONSE_ID

#: The node's padding byte (lib/isotp, ISOTP_PADDING) and its ISO-TP parameters (app/diag.h).
PADDING = 0xCC
NODE_BS = 16
NODE_STMIN = 0x00
NODE_RX_BUFFER = 1026

#: How long a test waits for a frame or an answer the node has to send at once (a flow control,
#: the answer to a request that needs no flash): a bound only.
ANSWER_WAIT_S = 2.0

#: How long a test looks for a frame that must not come (a consecutive frame beyond the block
#: size, an answer to a suppressed request): long against the node's reaction (a few
#: milliseconds), short against its timers (1000 ms).
QUIET_S = 0.3

#: The node's ISO-TP timers N_As, N_Ar, N_Bs, N_Cr (app/diag.h, DIAG_TIMEOUT_US), and how long a
#: test waits beyond one of them before it counts on its expiry: the margin is the host's.
NODE_TIMEOUT_S = 1.0
BEYOND_TIMEOUT_S = 2.0

#: The node's S3server (lib/uds/uds.h) and how long a test is silent to let it run out: the
#: timer starts when the node produces the response, before the tester receives it, so a tester
#: that waits longer than S3server from the response's arrival always finds it expired.
S3_SERVER_S = 5.0
BEYOND_S3_S = 5.5

#: The start of each slot on the host (platform/include/plat_flash.h, fcn_tools.flasher).
SLOT_A = 0x002000
SLOT_B = 0x102000

#: The payload of the "data" images (tests/system/conftest.py) and the size of the image: not a
#: whole number of pages (256 bytes) with its header, so that RequestTransferExit programs a
#: partial last page; 3 sectors, 37 pages, 10 blocks of TransferData.
DATA_PAYLOAD_SIZE = 9000
DATA_SIZE = DATA_PAYLOAD_SIZE + 256

#: maxNumberOfBlockLength of the node (lib/uds, UDS_MAX_BLOCK_LENGTH) and its data per block.
MAX_BLOCK_LENGTH = 1026
BLOCK_DATA = MAX_BLOCK_LENGTH - 2


def hexbytes(text: str) -> bytes:
    """b"..." from "10 03 ...", for the request and answer tables of the tests."""
    return bytes.fromhex(text)


def show(data: bytes | None) -> str:
    return "(nothing)" if data is None else data.hex(" ").upper()


# ------------------------------------------------------------------------------- raw frames


class RawLink:
    """The tester's raw end of the connection (see the module's text). Use as a context
    manager. `received` keeps every frame of UDS_Response the link has read, for checks over
    all of them (padding, DLC)."""

    def __init__(self, spec: str) -> None:
        self._bus = bus_module.open_bus(spec)
        self._queue: queue.Queue[can.Message] = queue.Queue()
        self.received: list[can.Message] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="RawLink", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            message = self._bus.recv(timeout=0.05)
            if message is not None and message.arbitration_id == RESPONSE_ID \
                    and not message.is_extended_id:
                self.received.append(message)
                self._queue.put(message)

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=5.0)
        self._bus.shutdown()

    def __enter__(self) -> "RawLink":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def send(self, data: bytes | str, pad: bool = True) -> None:
        """A frame of UDS_Request with `data` (bytes, or hex text), padded to 8 bytes with
        0xCC unless `pad` is False (then its DLC is the length of `data`)."""
        payload = hexbytes(data) if isinstance(data, str) else bytes(data)
        if pad:
            payload = payload + bytes([PADDING] * (8 - len(payload)))
        self._bus.send(can.Message(arbitration_id=REQUEST_ID, data=payload,
                                   is_extended_id=False))

    def next(self, timeout: float = ANSWER_WAIT_S) -> bytes:
        """The data of the next frame of the node; the test fails if none comes in time."""
        try:
            return bytes(self._queue.get(timeout=timeout).data)
        except queue.Empty:
            raise AssertionError(f"no frame from the node within {timeout} s") from None

    def quiet(self, seconds: float = QUIET_S) -> list[bytes]:
        """The frames of the node that arrive within `seconds` (none, if the node is quiet)."""
        deadline = time.monotonic() + seconds
        frames = []
        while (remaining := deadline - time.monotonic()) > 0:
            try:
                frames.append(bytes(self._queue.get(timeout=remaining).data))
            except queue.Empty:
                break
        return frames

    def pending(self) -> list[bytes]:
        """The frames of the node that have arrived and not been taken."""
        frames = []
        while True:
            try:
                frames.append(bytes(self._queue.get_nowait().data))
            except queue.Empty:
                return frames


def single_frame(payload: bytes) -> bytes:
    """The SF of `payload` (1 to 7 bytes), unpadded."""
    assert 1 <= len(payload) <= 7
    return bytes([len(payload)]) + payload


def segments(payload: bytes, first_sn: int = 1) -> tuple[bytes, list[bytes]]:
    """The FF of `payload` (8 to 4095 bytes) and its CFs, unpadded (RawLink.send pads them)."""
    assert 8 <= len(payload) <= 4095
    first = bytes([0x10 | (len(payload) >> 8), len(payload) & 0xFF]) + payload[:6]
    rest = payload[6:]
    frames = []
    sn = first_sn
    for start in range(0, len(rest), 7):
        frames.append(bytes([0x20 | sn]) + rest[start:start + 7])
        sn = (sn + 1) & 0x0F
    return first, frames


def flow_control(status: int = 0, block_size: int = 0, stmin: int = 0) -> bytes:
    return bytes([0x30 | status, block_size, stmin])


def is_padded(frame: bytes, used: int) -> bool:
    """`frame` has 8 bytes, and every byte after its first `used` is the padding."""
    return len(frame) == 8 and all(byte == PADDING for byte in frame[used:])


def used_bytes(frame: bytes, remaining: int | None = None) -> int:
    """The bytes of a frame of the node that carry its N_PCI and data: an SF's 1 + SF_DL, an
    FF's 8, a CF's 1 + min(7, `remaining` bytes of its message), an FC's 3."""
    kind = frame[0] >> 4
    if kind == 0:
        return 1 + (frame[0] & 0x0F)
    if kind == 1:
        return 8
    if kind == 2:
        assert remaining is not None, "a CF's length needs the bytes left of its message"
        return 1 + min(7, remaining)
    if kind == 3:
        return 3
    raise AssertionError(f"a frame of a reserved type from the node: {show(frame)}")


# ------------------------------------------------------------------------------------- UDS


class NodeEnded(Exception):
    """The node ended (a reset, a cut of its supply) while the tester waited for an answer."""


@dataclass
class Answer:
    """The final answer to one request, and the responsePendings that came before it."""
    request: bytes
    response: bytes
    pending_s: list[float] = field(default_factory=list)   # arrival of each 0x78, from the send
    took_s: float = 0.0                                     # from the send to the final answer

    @property
    def positive(self) -> bool:
        return self.response[:1] == bytes([self.request[0] + 0x40])

    @property
    def nrc(self) -> int | None:
        if len(self.response) == 3 and self.response[0] == 0x7F:
            return self.response[2]
        return None


class Uds:
    """UDS requests as bytes over can-isotp (see the module's text). Use as a context manager.
    `isotp_errors` collects what can-isotp reported as wrong in the node's frames."""

    def __init__(self, spec: str, params: dict | None = None) -> None:
        self.isotp_errors: list[Exception] = []
        #: Every request sent, every final answer received, and the responsePendings, so that
        #: a test can compare the node's counters with what the tester did (check_counts()).
        self.sent: list[bytes] = []
        self.answers: list[bytes] = []
        self.pendings = 0
        self._bus = bus_module.open_bus(spec)
        self._notifier = can.Notifier(self._bus, [], timeout=0.05)
        merged = dict(diag.ISOTP_PARAMS)
        merged.update(params or {})
        address = isotp.Address(isotp.AddressingMode.Normal_11bits, txid=REQUEST_ID,
                                rxid=RESPONSE_ID)
        self._stack = isotp.NotifierBasedCanStack(self._bus, self._notifier, address=address,
                                                  params=merged,
                                                  error_handler=self.isotp_errors.append)
        self._stack.start()

    def close(self) -> None:
        self._stack.stop()
        self._notifier.stop()
        self._bus.shutdown()

    def __enter__(self) -> "Uds":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def send(self, request: bytes | str) -> bytes:
        payload = hexbytes(request) if isinstance(request, str) else bytes(request)
        self._stack.send(payload)
        self.sent.append(payload)
        return payload

    def receive(self, timeout: float) -> bytes | None:
        message = self._stack.recv(block=True, timeout=timeout)
        return None if message is None else bytes(message)

    def answer(self, request: bytes, timeout: float = ANSWER_WAIT_S,
               alive: Callable[[], bool] | None = None, sent_at: float | None = None) -> Answer:
        """The final answer to `request`, sent already: responsePendings (7F <SID> 78) are
        noted and waited through, each one giving the node P2*client (fcn_tools.diag) more.
        NodeEnded if `alive` says the node has ended; AssertionError for an answer to another
        service or none within the bound."""
        start = time.monotonic() if sent_at is None else sent_at
        deadline = start + timeout
        pending = []
        while True:
            now = time.monotonic()
            if now >= deadline:
                raise AssertionError(f"no answer to {show(request)} within the bound "
                                     f"({len(pending)} responsePending before)")
            message = self.receive(min(0.05, deadline - now))
            if message is None:
                if alive is not None and not alive():
                    raise NodeEnded(show(request))
                continue
            if len(message) == 3 and message[0] == 0x7F and message[1] == request[0] \
                    and message[2] == 0x78:
                pending.append(time.monotonic() - start)
                self.pendings += 1
                deadline = max(deadline, time.monotonic() + diag.P2_STAR_CLIENT_S)
                continue
            if message[:1] != bytes([request[0] + 0x40]) and message[:2] != bytes([0x7F,
                                                                                  request[0]]):
                raise AssertionError(f"the answer to {show(request)} is {show(message)}")
            self.answers.append(message)
            return Answer(request, message, pending, time.monotonic() - start)

    def request(self, request: bytes | str, timeout: float = ANSWER_WAIT_S,
                alive: Callable[[], bool] | None = None) -> Answer:
        """Send `request` and return its final answer (answer())."""
        start = time.monotonic()
        payload = self.send(request)
        return self.answer(payload, timeout=timeout, alive=alive, sent_at=start)

    def no_answer(self, request: bytes | str) -> None:
        """Send a request whose answer is suppressed, then ReadDataByIdentifier 0xF186: the next
        message of the node must be the answer to that read. The node processes its requests one
        after the other and sends their answers in that order, so an answer to the first would
        have come before it, however the host ran."""
        payload = self.send(request)
        probe = self.request("22 F1 86")
        assert probe.response[:3] == hexbytes("62 F1 86"), (show(payload), show(probe.response))


def check_table(uds: Uds, table: Sequence[tuple[str, str | None]]) -> list[bytes]:
    """Every (request, expected answer) of `table`, in its order: the answer as hex text, or
    None for a request whose positive answer is suppressed (Uds.no_answer()). Fails the test
    with every mismatch at once. Returns the answers, for the counts of a test."""
    wrong = []
    answers = []
    for request, expected in table:
        if expected is None:
            uds.no_answer(request)
            continue
        got = uds.request(request).response
        answers.append(got)
        if got != hexbytes(expected):
            wrong.append(f"{request}: {show(got)}, not {expected}")
    assert not wrong, "\n".join(wrong)
    return answers


NRC_NAMES = {0x10: "generalReject", 0x11: "serviceNotSupported", 0x12: "subFunctionNotSupported",
             0x13: "incorrectMessageLengthOrInvalidFormat", 0x14: "responseTooLong",
             0x21: "busyRepeatRequest", 0x22: "conditionsNotCorrect",
             0x24: "requestSequenceError", 0x31: "requestOutOfRange",
             0x70: "uploadDownloadNotAccepted", 0x71: "transferDataSuspended",
             0x72: "generalProgrammingFailure", 0x73: "wrongBlockSequenceCounter",
             0x7E: "subFunctionNotSupportedInActiveSession",
             0x7F: "serviceNotSupportedInActiveSession"}


def nrc_counts(answers: Sequence[bytes]) -> dict[str, int]:
    """The negative answers among `answers` by the node's NRC name (its statistics' keys)."""
    counts = {name: 0 for name in NRC_NAMES.values()}
    for answer in answers:
        if len(answer) == 3 and answer[0] == 0x7F and answer[2] != 0x78:
            counts[NRC_NAMES[answer[2]]] += 1
    return counts


#: The node's services by SID, with the names of its statistics (lib/uds, uds_service_name()).
SERVICE_NAMES = {0x10: "DiagnosticSessionControl", 0x11: "ECUReset",
                 0x14: "ClearDiagnosticInformation", 0x19: "ReadDTCInformation",
                 0x22: "ReadDataByIdentifier", 0x31: "RoutineControl", 0x34: "RequestDownload",
                 0x36: "TransferData", 0x37: "RequestTransferExit", 0x3E: "TesterPresent"}


def check_counts(stats: dict, *testers: Uds) -> None:
    """The node's UDS counters (its statistics' diag.uds) are what the testers saw, exactly:
    every request by service (unknown SIDs as requests_unsupported), every negative answer by
    NRC, the positive answers, the suppressed ones (requests of a known service that got no
    answer), and the responsePendings. Only for a run whose testers waited for every answer."""
    uds = stats["diag"]["uds"]
    sent = [request for tester in testers for request in tester.sent]
    answers = [answer for tester in testers for answer in tester.answers]
    by_service = {name: 0 for name in SERVICE_NAMES.values()}
    unsupported = 0
    for request in sent:
        if request[0] in SERVICE_NAMES:
            by_service[SERVICE_NAMES[request[0]]] += 1
        else:
            unsupported += 1
    positive = sum(1 for answer in answers if answer[0] != 0x7F)
    assert uds["requests"] == by_service
    assert uds["requests_unsupported"] == unsupported
    assert uds["nrc"] == nrc_counts(answers)
    assert uds["positive"] == positive
    assert uds["suppressed"] == len(sent) - len(answers)
    assert uds["response_pending"] == sum(tester.pendings for tester in testers)


# ------------------------------------------------------------------------------ the download


def location(address: int, size: int) -> bytes:
    """addressAndLengthFormatIdentifier 0x44, the address and the size."""
    return bytes([0x44]) + address.to_bytes(4, "big") + size.to_bytes(4, "big")


def erase_request(address: int, size: int) -> bytes:
    return hexbytes("31 01 FF 00") + location(address, size)


def download_request(address: int, size: int) -> bytes:
    return hexbytes("34 00") + location(address, size)


def blocks(data: bytes, block_data: int = BLOCK_DATA) -> Iterator[tuple[int, bytes]]:
    """The TransferData blocks of `data`: (counter, data), the counter from 1, wrapping."""
    counter = 1
    for start in range(0, len(data), block_data):
        yield counter, data[start:start + block_data]
        counter = (counter + 1) & 0xFF


#: The steps of download(), in their order; "block" stands for every TransferData.
STEPS = ("extended", "programming", "erase", "request_download", "block", "exit", "check")


@dataclass
class Progress:
    """How far download() got: the steps answered positively, and the block counters sent."""
    done: list[str] = field(default_factory=list)
    blocks_sent: int = 0
    answers: list[Answer] = field(default_factory=list)


def download(uds: Uds, data: bytes, address: int = SLOT_B, *, sessions: bool = True,
             stop_after_blocks: int | None = None, alive: Callable[[], bool] | None = None,
             progress: Progress | None = None, erase_timeout: float | None = None,
             after: Callable[[str], None] | None = None) -> Progress:
    """The programming sequence for the image `data` into the slot at `address`, with Uds:
    10 03 and 10 02 (unless `sessions` is False: the tester is in the programming session
    already), eraseMemory, RequestDownload, TransferData blocks of 1024 bytes, RequestTransferExit
    and checkProgrammingDependencies. Every answer must be positive (AssertionError otherwise).
    With `stop_after_blocks` it stops after that many blocks. `after(step)` is called once each
    step of STEPS other than "block" has been answered, before the next request. NodeEnded
    (with `progress` filled in so far) when `alive` says the node ended while the tester
    waited."""
    progress = progress if progress is not None else Progress()
    timeout = erase_timeout or diag.erase_timeout_s(len(data))

    def step(name: str, request: bytes, wait: float = ANSWER_WAIT_S) -> Answer:
        answer = uds.request(request, timeout=wait, alive=alive)
        progress.answers.append(answer)
        assert answer.positive, f"{name}: {show(request[:12])} answered {show(answer.response)}"
        if after is not None and name != "block":
            after(name)
        return answer

    if sessions:
        step("extended", hexbytes("10 03"))
        progress.done.append("extended")
        step("programming", hexbytes("10 02"))
        progress.done.append("programming")
    step("erase", erase_request(address, len(data)), wait=timeout)
    progress.done.append("erase")
    answer = step("request_download", download_request(address, len(data)))
    assert answer.response == hexbytes("74 20 04 02"), show(answer.response)
    progress.done.append("request_download")
    for counter, chunk in blocks(data):
        if stop_after_blocks is not None and progress.blocks_sent >= stop_after_blocks:
            return progress
        answer = step("block", bytes([0x36, counter]) + chunk, wait=diag.write_timeout_s(len(chunk)))
        assert answer.response == bytes([0x76, counter]), show(answer.response)
        progress.blocks_sent += 1
    progress.done.append("block")
    step("exit", hexbytes("37"))
    progress.done.append("exit")
    answer = step("check", hexbytes("31 01 FF 01"), wait=diag.check_timeout_s(len(data)))
    assert answer.response == hexbytes("71 01 FF 01 00 00"), show(answer.response)
    progress.done.append("check")
    return progress


#: How long the flasher may take for a whole download of a node image over a bus of 500 kbit/s
#: on the host: an upper bound only, not a timing tolerance (the report has the times).
FLASHER_TIMEOUT_S = 240.0


def run_flasher(can_spec: str, image: Path, report: Path, *options: str) -> tuple[int, dict, str]:
    """python -m fcn_tools.flasher; its exit status, its report and what it wrote to stderr."""
    done = subprocess.run([sys.executable, "-m", "fcn_tools.flasher", "--can", can_spec,
                           "--report", str(report), *options, str(image)],
                          capture_output=True, text=True, timeout=FLASHER_TIMEOUT_S, check=False)
    print(done.stderr)
    loaded = json.loads(report.read_text()) if report.exists() else {}
    return done.returncode, loaded, done.stderr


def steps_of(report: dict) -> dict[str, dict]:
    """The steps of a flasher's report by name."""
    return {step["name"]: step for step in report["steps"]}


#: How long a test waits for the first status frame of a node it started: a bound only.
START_WAIT_S = 10.0


def start_ready(start_node: Callable, spec: str, *args: str, **kwargs):
    """start_node(*args, can=spec, **kwargs), returned once the node's first status frame
    (BMS_Heartbeat) is on the bus: its tasks run, the diagnostic task among them."""
    reader = bus_module.open_bus(spec)
    try:
        node = start_node(*args, can=spec, **kwargs)
        first = bus_module.wait_for(reader, bus_module.is_status, timeout=START_WAIT_S)
    finally:
        reader.shutdown()
    assert first is not None, f"no status frame within {START_WAIT_S} s\n{node.stderr()}"
    return node


def wait_until(condition: Callable[[], bool], what: str, timeout: float = 30.0,
               running: Callable[[], bool] | None = None) -> None:
    """Wait for `condition`, a bound only; the test fails if it does not hold in time or
    `running` says the node ended first."""
    deadline = time.monotonic() + timeout
    while not condition():
        if running is not None and not running():
            raise AssertionError(f"the node ended before {what}")
        if time.monotonic() >= deadline:
            raise AssertionError(f"{what}: not within {timeout} s")
        time.sleep(0.02)
