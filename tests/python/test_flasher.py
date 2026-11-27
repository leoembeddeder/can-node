# SPDX-License-Identifier: MIT
"""fcn_tools.flasher and fcn_tools.diag without a node: the blocks of a download, the boot
information record, the request timeouts from the part's maximum times, and the programming
sequence against a fake UDS client that answers as the node's server does (lib/uds/uds.h) or as
the test makes it answer: the steps in their order, the report, a refusal, a trial that confirmed
itself before the routine, an answer of the old image while the node resets, answers udsoncan
finds unexpected or invalid and errors of the bus, each a failed step with its report, an error
closing the bus after the sequence, also with its report, and the exit statuses of the command
line. And the wait for the trial with udsoncan's own client against a node that restarts, on a
clock of the test's: the first request one poll interval after the reset, never a wait of
P2client, and a late answer taken by the next request with the one still due taken before
read_version. The node itself is the system tests' (tests/system/test_download.py)."""
from __future__ import annotations

import contextlib
import errno
import json
from pathlib import Path
from types import SimpleNamespace

import can
import pytest
import udsoncan
import udsoncan.client
import udsoncan.connections
from udsoncan.exceptions import (InvalidResponseException, NegativeResponseException,
                                 TimeoutException, UnexpectedResponseException)

from fcn_tools import diag, flasher
from fcn_tools import image as image_format


def boot_info_bytes(slot: int, mode: int, attempt: int, state: int, version: int,
                    handoff: bool = True) -> bytes:
    """DID 0xFD00 as lib/uds encodes it (layout 1)."""
    return bytes([1, 1 if handoff else 0, slot if handoff else 0xFF, mode if handoff else 0,
                  attempt, state, 0, 0xFF]) + (version if handoff else 0).to_bytes(4, "big")


def test_the_blocks_carry_max_length_minus_two_bytes_and_the_counter_wraps():
    blocks = flasher.plan_blocks(1024 * 300 + 5, 1026)
    assert len(blocks) == 301
    assert blocks[0] == (1, 0, 1024)
    assert blocks[254] == (255, 254 * 1024, 1024)
    assert blocks[255] == (0, 255 * 1024, 1024)       # 0xFF wraps to 0x00
    assert blocks[256] == (1, 256 * 1024, 1024)
    assert blocks[-1] == (301 & 0xFF, 300 * 1024, 5)
    assert flasher.plan_blocks(1024, 1026) == [(1, 0, 1024)]
    assert flasher.plan_blocks(1025, 1026) == [(1, 0, 1024), (2, 1024, 1)]
    with pytest.raises(ValueError):
        flasher.plan_blocks(10, 2)


def test_the_boot_information_record_is_decoded_as_lib_uds_writes_it():
    info = diag.decode_boot_info(boot_info_bytes(1, 2, 1, 1, 0x01020003))
    assert (info.handoff, info.slot, info.mode, info.attempt, info.confirm_state,
            info.image_version) == (True, "B", "trial", 1, "WAITING", "1.2.3")
    none = diag.decode_boot_info(boot_info_bytes(0, 0, 0, 0, 0, handoff=False))
    assert (none.handoff, none.slot, none.mode, none.image_version) == (False, None, None, None)
    assert none.confirm_state == "NOT_NEEDED"
    with pytest.raises(ValueError):
        diag.decode_boot_info(bytes(11))
    with pytest.raises(ValueError):
        diag.decode_boot_info(bytes([2]) + bytes(11))
    assert diag.version_text(0x01FF1234) == "1.255.4660"


def test_the_request_timeouts_allow_the_parts_maximum_times():
    # 240,272 bytes: 59 sectors of 4 KiB at 400 ms, the BEGIN record (a sector erase and three
    # page programs at their maximum), and the hosts' margin.
    assert diag.erase_timeout_s(240272) == pytest.approx(59 * 0.4 + 0.409 + 10.0)
    assert diag.check_timeout_s(240272) == pytest.approx((240272 // 64 + 8) * 0.0001 + 0.409
                                                         + 10.0)
    assert diag.write_timeout_s(1024) == pytest.approx(5 * 0.003 + 10.0)
    # P2*client allows the node's P2*server; responsePending comes every 2 s.
    assert diag.P2_STAR_CLIENT_S > diag.P2_STAR_SERVER_S > diag.PENDING_EVERY_S


def _negative(service, code: int) -> NegativeResponseException:
    return NegativeResponseException(udsoncan.Response(service=service, code=code))


class FakeClient:
    """Answers the flasher's requests as the node does; `answers` changes what one request
    answers: name -> a callable (raising or returning)."""

    def __init__(self, running: str = "A", version: int = 0x01010000) -> None:
        self.calls: list[tuple] = []
        self.config = {"request_timeout": 10.0}
        self.running = running
        self.version = version
        self.reset_done = False
        self.confirmed = False
        self.old_answers_after_reset = 0
        self.answers: dict = {}
        self.received = bytearray()

    def set_config(self, key, value):
        self.config[key] = value

    def _answer(self, name, default):
        self.calls.append((name, self.config["request_timeout"]))
        action = self.answers.get(name)
        return action() if action is not None else default()

    def change_session(self, session):
        return self._answer(f"session_{session}", lambda: SimpleNamespace())

    def read_data_by_identifier(self, did):
        def default():
            if did == diag.DID_BOOT_INFO:
                if self.reset_done and self.old_answers_after_reset > 0:
                    self.old_answers_after_reset -= 1
                    raw = boot_info_bytes(0, 1, 0, 0, 0x01000000)     # the old image
                elif self.reset_done:
                    raw = boot_info_bytes(1, 2, 1, 3 if self.confirmed else 1, self.version)
                else:
                    raw = boot_info_bytes("AB".index(self.running), 1, 0, 0, 0x01000000)
                return SimpleNamespace(service_data=SimpleNamespace(values={did: raw}))
            return SimpleNamespace(service_data=SimpleNamespace(
                values={did: diag.version_text(self.version)}))
        return self._answer(f"did_{did:04X}", default)

    def start_routine(self, rid, data=None):
        def default():
            if rid == diag.RID_CONFIRM_TRIAL:
                self.confirmed = True
            return SimpleNamespace(service_data=SimpleNamespace(routine_status_record=b"\x00\x00"))
        self.calls.append(("routine_data", data))
        return self._answer(f"routine_{rid:04X}", default)

    def request_download(self, location):
        self.calls.append(("location", location.address, location.memorysize))
        assert (location.address_format, location.memorysize_format) == (32, 32)  # ALFID 0x44
        return self._answer("download",
                            lambda: SimpleNamespace(service_data=SimpleNamespace(max_length=1026)))

    def transfer_data(self, counter, data):
        self.received += data
        return self._answer(f"transfer_{counter}", lambda: SimpleNamespace())

    def request_transfer_exit(self):
        return self._answer("exit", lambda: SimpleNamespace())

    def ecu_reset(self, reset_type):
        def default():
            self.reset_done = True
            return SimpleNamespace()
        return self._answer(f"reset_{reset_type}", default)


@pytest.fixture
def fake(monkeypatch):
    client = FakeClient()

    @contextlib.contextmanager
    def connect(spec, **kwargs):
        client.spec = spec
        yield client

    monkeypatch.setattr(diag, "connect", connect)
    monkeypatch.setattr(flasher, "TRIAL_POLL_S", 0.0)
    return client


@pytest.fixture
def image_file(tmp_path) -> Path:
    payload = bytes((i * 7) & 0xFF for i in range(3000))
    path = tmp_path / "node-1.1.0.img"
    path.write_bytes(image_format.pack(payload, image_format.parse_version("1.1.0"),
                                       image_format.TARGETS["host"]))
    return path


def test_the_sequence_writes_the_image_into_the_other_slot_and_confirms_it(fake, image_file):
    report = flasher.flash("udp:1", image_file).as_json()
    names = [step["name"] for step in report["steps"]]
    assert names == ["boot_info", "default_session", "extended_session", "programming_session",
                     "erase", "request_download", "transfer", "transfer_exit", "check",
                     "ecu_reset", "wait_for_trial", "read_version", "extended_session_trial",
                     "confirm", "boot_info_confirmed"]
    assert all(step["ok"] for step in report["steps"])
    assert report["result"] == "confirmed" and report["confirmed_by"] == "routine"
    size = image_file.stat().st_size
    assert (report["slot"], report["address"]) == ("B", 0x102000)
    assert ("location", 0x102000, size) in fake.calls
    assert ("routine_data", bytes([0x44]) + (0x102000).to_bytes(4, "big")
            + size.to_bytes(4, "big")) in fake.calls
    assert bytes(fake.received) == image_file.read_bytes()
    assert report["blocks"] == len(report["blocks_s"]) == -(-size // 1024)
    # The erase and the check wait as long as the part can take at its maximum times.
    timeouts = dict(fake.calls[i] for i in range(len(fake.calls)) if len(fake.calls[i]) == 2
                    and isinstance(fake.calls[i][1], float))
    assert timeouts["routine_FF00"] == pytest.approx(diag.erase_timeout_s(size))
    assert timeouts["routine_FF01"] == pytest.approx(diag.check_timeout_s(size))
    assert fake.config["request_timeout"] == 10.0          # put back after each
    assert report["schema"] == "fcn.flash_report/1"
    assert report["image"]["version"] == "1.1.0" and report["image"]["size"] == size
    json.dumps(report)


def test_a_slot_running_b_is_updated_in_a(fake, image_file):
    fake.running = "B"
    report = flasher.flash("udp:1", image_file, reset=False).as_json()
    assert (report["slot"], report["address"], report["result"]) == ("A", 0x2000, "committed")
    assert "ecu_reset" not in [step["name"] for step in report["steps"]]


def test_a_refused_erase_ends_the_sequence_with_its_nrc(fake, image_file):
    def refuse():
        raise _negative(udsoncan.services.RoutineControl, 0x22)
    fake.answers["routine_FF00"] = refuse
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["result"] == "failed" and report["failed_step"] == "erase"
    erase = report["steps"][-1]
    assert (erase["name"], erase["ok"], erase["nrc"]) == ("erase", False, 0x22)
    assert erase["nrc_name"] == "ConditionsNotCorrect"


def test_a_timeout_is_reported_as_one(fake, image_file):
    def silent():
        raise TimeoutException("P2* timeout")
    fake.answers["exit"] = silent
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["failed_step"] == "transfer_exit"
    assert report["steps"][-1]["error"].startswith("timeout")


def test_a_trial_that_confirmed_itself_first_counts_as_confirmed_by_the_node(fake, image_file):
    def already():
        fake.confirmed = True
        raise _negative(udsoncan.services.RoutineControl, 0x22)
    fake.answers["routine_F000"] = already
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["result"] == "confirmed" and report["confirmed_by"] == "node"
    confirm = [step for step in report["steps"] if step["name"] == "confirm"][0]
    assert confirm["ok"] and confirm["nrc"] == 0x22 and "confirmed itself" in confirm["outcome"]


def test_a_refused_confirm_of_a_trial_that_is_not_confirmed_fails(fake, image_file):
    def refuse():
        raise _negative(udsoncan.services.RoutineControl, 0x22)
    fake.answers["routine_F000"] = refuse
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["result"] == "failed" and report["failed_step"] == "confirm_check"


def test_answers_of_the_old_image_after_the_reset_are_not_the_trial(fake, image_file):
    fake.old_answers_after_reset = 3
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["result"] == "confirmed"
    asked = [call for call in fake.calls if call[0] == "did_FD00"]
    assert len(asked) == 1 + 4 + 1      # before, three old answers and the trial's, after


def test_no_trial_within_the_wait_fails_wait_for_trial(fake, image_file):
    fake.old_answers_after_reset = 10**6
    report = flasher.flash("udp:1", image_file, trial_wait_s=0.05).as_json()
    assert report["failed_step"] == "wait_for_trial"


class Clock:
    """A clock that only the test moves, with time.monotonic() and time.sleep() of its own."""

    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += max(seconds, 0.0)


class RestartingNode(udsoncan.connections.BaseConnection):
    """The node from the ECUReset's answer on (time 0 of `clock`), at the level of UDS payloads,
    under udsoncan's own client: nothing on the bus until `up_at` (a request sent before then
    reaches no node); then every request is answered, in the order of the requests (app/diag.h,
    RESPONSES), `turnaround` after the later of the request, `answers_from` (the trial's
    diagnostic task held up until then) and the answer before it. 22 FD 00 answers the trial's
    boot information (slot B, attempt 1, WAITING, 1.1.0) and 22 F1 89 its version. What has
    arrived is dropped by empty_rxqueue(), as can-isotp's receive queue is cleared."""

    def __init__(self, clock: Clock, up_at: float, answers_from: float = 0.0,
                 turnaround: float = 0.002) -> None:
        super().__init__("restarting node")
        self.clock = clock
        self.up_at = up_at
        self.answers_from = answers_from
        self.turnaround = turnaround
        self.free_at = 0.0
        self.due: list[tuple[float, bytes]] = []      # (arrival, answer), in their order
        self.requests: list[tuple[float, bytes]] = []
        self.waits: list[float] = []                  # the timeout of every wait for an answer
        self.opened = False

    def open(self) -> RestartingNode:
        self.opened = True
        return self

    def close(self) -> None:
        self.opened = False

    def is_open(self) -> bool:
        return self.opened

    def empty_txqueue(self) -> None:
        pass

    def empty_rxqueue(self) -> None:
        self.due = [(at, answer) for at, answer in self.due if at > self.clock.now]

    def specific_send(self, payload: bytes, timeout: float | None = None) -> None:
        now = self.clock.now
        request = bytes(payload)
        self.requests.append((now, request))
        if now < self.up_at:
            return
        self.free_at = max(now, self.answers_from, self.free_at) + self.turnaround
        if request == bytes([0x22, 0xFD, 0x00]):
            answer = request[1:3] + boot_info_bytes(1, 2, 1, 1, 0x01010000)
        elif request == bytes([0x22, 0xF1, 0x89]):
            answer = request[1:3] + b"1.1.0"
        else:
            raise AssertionError(f"not a request of this test: {request.hex(' ')}")
        self.due.append((self.free_at, bytes([0x62]) + answer))

    def specific_wait_frame(self, timeout: float | None = None) -> bytes | None:
        assert timeout is not None
        self.waits.append(timeout)
        if self.due and self.due[0][0] <= self.clock.now + timeout:
            at, answer = self.due.pop(0)
            self.clock.now = max(self.clock.now, at)
            return answer
        self.clock.now += timeout
        raise TimeoutException(f"no answer within {timeout} s")


@pytest.fixture
def clock(monkeypatch) -> Clock:
    """The test's clock for the flasher and for udsoncan's client (its request timeouts)."""
    now = Clock()
    monkeypatch.setattr(flasher, "time", now)
    monkeypatch.setattr(udsoncan.client, "time", now)
    return now


def _wait_and_read_version(node: RestartingNode
                           ) -> tuple[diag.BootInfo, float, list[float], str]:
    """wait_for_trial from the ECUReset's answer (time 0), as the flasher makes it; what it
    found, the time it took, its waits for an answer, and what read_version gets after it."""
    with udsoncan.client.Client(node, config=diag.client_config()) as client:
        found = flasher._wait_for_trial(client, "B", flasher.TRIAL_WAIT_S)
        took, waits = node.clock.now, list(node.waits)
        return found, took, waits, diag.read_version(client)


@pytest.mark.parametrize("up_at, polls, took", [
    # The trial up 5 ms after the reset: the first request, one interval after it, finds it.
    (0.005, 1, flasher.TRIAL_POLL_S + 0.002),
    # Up after 120 ms: the first request reaches no node and waits P2server, the second finds
    # the trial an interval later, and the step waits P2server for an answer still due.
    (0.120, 2, 2 * flasher.TRIAL_POLL_S + 2 * diag.P2_SERVER_S + 0.002),
], ids=["restart-5ms", "restart-120ms"])
def test_wait_for_trial_asks_every_poll_interval_and_never_waits_p2client(clock, up_at, polls,
                                                                          took):
    node = RestartingNode(clock, up_at=up_at)
    found, elapsed, waits, version = _wait_and_read_version(node)
    assert (found.slot, found.mode, found.attempt, found.confirm_state) == ("B", "trial", 1,
                                                                         "WAITING")
    assert version == "1.1.0"
    asked = [at for at, request in node.requests if request == bytes([0x22, 0xFD, 0x00])]
    assert len(asked) == polls
    assert asked[0] == pytest.approx(flasher.TRIAL_POLL_S)   # not at once: no node is there
    # Every wait of the step for an answer is the node's P2server, none P2client: the step is
    # the restart rounded up to the poll (at most a period of P2server and the interval beyond
    # it), one request, and P2server for an answer still due; not the second of P2client.
    assert waits and all(wait == pytest.approx(diag.P2_SERVER_S) for wait in waits)
    assert elapsed == pytest.approx(took)
    assert elapsed < (up_at + diag.P2_SERVER_S + flasher.TRIAL_POLL_S + node.turnaround
                      + diag.P2_SERVER_S) < diag.P2_CLIENT_S


def test_a_late_answer_is_taken_by_the_next_request_and_the_one_still_due_before_read_version(
        clock):
    # The trial's receive path is up after 10 ms, its diagnostic task only after 180 ms: the
    # first request (at 50 ms) gets no answer within P2server; its answer comes at 182 ms, while
    # the second request (at 150 ms) waits, and is taken as the second's (the same request,
    # 22 FD 00); the second's own answer follows at 184 ms.
    node = RestartingNode(clock, up_at=0.010, answers_from=0.180)
    found, elapsed, _, version = _wait_and_read_version(node)
    assert (found.slot, found.mode) == ("B", "trial")
    assert [request.hex(" ") for _, request in node.requests] == ["22 fd 00"] * 2 + ["22 f1 89"]
    # The step ended only with the second answer taken: read_version got its own answer, and
    # nothing is left.
    assert elapsed == pytest.approx(0.184)
    assert version == "1.1.0" and node.due == []


def test_a_trial_of_another_version_fails(fake, image_file):
    fake.version = 0x01040000
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["failed_step"] == "version_check"


def test_a_node_without_a_hand_off_needs_an_address(fake, image_file):
    fake.answers["did_FD00"] = lambda: SimpleNamespace(service_data=SimpleNamespace(
        values={diag.DID_BOOT_INFO: boot_info_bytes(0, 0, 0, 0, 0, handoff=False)}))
    report = flasher.flash("udp:1", image_file).as_json()
    assert report["failed_step"] == "target_slot"
    report = flasher.flash("udp:1", image_file, address=0x102000, reset=False).as_json()
    assert report["result"] == "committed" and report["slot"] == "B"


def test_the_command_line_writes_the_report_and_says_how_it_went(fake, image_file, tmp_path,
                                                                capsys):
    path = tmp_path / "report.json"
    assert flasher.main(["--can", "udp:1", "--report", str(path), str(image_file)]) == 0
    assert json.loads(path.read_text())["result"] == "confirmed"
    assert "result: confirmed" in capsys.readouterr().err

    def refuse():
        raise _negative(udsoncan.services.DiagnosticSessionControl, 0x7E)
    fake.answers["session_3"] = refuse
    assert flasher.main(["--can", "udp:1", str(image_file)]) == 2
    assert json.loads(capsys.readouterr().out)["failed_step"] == "extended_session"

    not_an_image = tmp_path / "junk.img"
    not_an_image.write_bytes(bytes(1000))
    assert flasher.main(["--can", "udp:1", str(not_an_image)]) == 1
    assert flasher.main(["--can", "udp:1", str(tmp_path / "missing.img")]) == 1


def _raising(error: BaseException):
    def action():
        raise error
    return action


#: Each request of the sequence (the FakeClient's name of its answer) and the step it is in.
REQUESTS_AND_STEPS = [("did_FD00", "boot_info"), ("session_1", "default_session"),
                      ("session_3", "extended_session"), ("session_2", "programming_session"),
                      ("routine_FF00", "erase"), ("download", "request_download"),
                      ("transfer_2", "transfer"), ("exit", "transfer_exit"),
                      ("routine_FF01", "check"), ("reset_1", "ecu_reset"),
                      ("did_FD00 after the reset", "wait_for_trial"),
                      ("did_F189", "read_version"), ("routine_F000", "confirm")]


def _answer_with(fake: FakeClient, request: str, error: BaseException) -> None:
    if request == "did_FD00 after the reset":
        before_the_reset = fake.read_data_by_identifier

        def read(did):
            if did == diag.DID_BOOT_INFO and fake.reset_done:
                raise error
            return before_the_reset(did)
        fake.read_data_by_identifier = read
    else:
        fake.answers[request] = _raising(error)


def _failed_run(image_file: Path, tmp_path: Path, capsys) -> tuple[dict, str]:
    """main() with a report file: status 2; the report and what went to stderr."""
    path = tmp_path / "report.json"
    assert flasher.main(["--can", "udp:1", "--report", str(path), str(image_file)]) == 2
    return json.loads(path.read_text()), capsys.readouterr().err


@pytest.mark.parametrize("request_name, step", REQUESTS_AND_STEPS,
                         ids=[step for _, step in REQUESTS_AND_STEPS])
def test_an_unexpected_answer_fails_its_step_with_the_report(fake, image_file, tmp_path, capsys,
                                                             request_name, step):
    # A valid answer that is not the one to the request, as udsoncan reports it with
    # exception_on_unexpected_response (fcn_tools.diag); at every request the exception that
    # client.transfer_data() raises for 76 with the counter of another block.
    error = UnexpectedResponseException(
        udsoncan.Response.from_payload(bytes([0x76, 0x03])),
        "Block sequence number of response (0x03) does not match request block sequence "
        "number (0x02)")
    _answer_with(fake, request_name, error)
    report, err = _failed_run(image_file, tmp_path, capsys)
    assert report["result"] == "failed" and report["failed_step"] == step
    record = report["steps"][-1]
    assert (record["name"], record["ok"]) == (step, False)
    assert record["error"] == f"unexpected response: {error}"
    assert "does not match request block sequence number (0x02)" in record["error"]
    assert all(earlier["ok"] for earlier in report["steps"][:-1])
    assert f"FAILED unexpected response: {error}" in err and "result: failed" in err


def _too_short_routine_answer() -> None:
    # 71 01 FF: a RoutineControl answer without the second byte of its RID, decoded as udsoncan
    # decodes it (exception_on_invalid_response raises what this raises).
    udsoncan.services.RoutineControl.interpret_response(
        udsoncan.Response.from_payload(bytes([0x71, 0x01, 0xFF])))


@pytest.mark.parametrize("request_name, step, error, text", [
    ("routine_FF01", "check", None, "invalid response: RoutineControl service execution "
                                    "returned an invalid response"),
    ("exit", "transfer_exit", OSError(errno.ENETDOWN, "Network is down"),
     f"OSError: [Errno {errno.ENETDOWN}] Network is down"),
    # As python-can's SocketCAN bus raises it for a send(2) that fails.
    ("transfer_2", "transfer", can.CanOperationError("Failed to transmit: No buffer space "
                                                     "available", errno.ENOBUFS),
     f"CanOperationError: Failed to transmit: No buffer space available [Error Code "
     f"{errno.ENOBUFS}]"),
], ids=["invalid-answer", "oserror", "can-error"])
def test_an_invalid_answer_or_an_error_of_the_bus_fails_its_step_with_the_report(
        fake, image_file, tmp_path, capsys, request_name, step, error, text):
    if error is None:
        with pytest.raises(InvalidResponseException):        # what udsoncan raises for it
            _too_short_routine_answer()
        fake.answers[request_name] = _too_short_routine_answer
    else:
        _answer_with(fake, request_name, error)
    report, err = _failed_run(image_file, tmp_path, capsys)
    assert report["result"] == "failed" and report["failed_step"] == step
    record = report["steps"][-1]
    assert (record["name"], record["ok"]) == (step, False)
    assert record["error"].startswith(text), record["error"]
    assert f"FAILED {text}" in err


def test_a_block_length_that_leaves_no_data_fails_request_download(fake, image_file, tmp_path,
                                                                   capsys):
    fake.answers["download"] = lambda: SimpleNamespace(service_data=SimpleNamespace(max_length=2))
    report, _ = _failed_run(image_file, tmp_path, capsys)
    assert report["failed_step"] == "request_download"
    assert report["steps"][-1]["error"] == "maxNumberOfBlockLength 2 leaves no data"
    assert not [call for call in fake.calls if call[0].startswith("transfer")]


def test_an_error_of_the_flasher_between_two_steps_ends_it_with_the_report(
        fake, image_file, tmp_path, capsys, monkeypatch):
    def defect(response):
        raise RuntimeError("a defect after the erase")
    monkeypatch.setattr(flasher, "_routine_record", defect)
    report, err = _failed_run(image_file, tmp_path, capsys)
    assert report["result"] == "failed" and report["failed_step"] is None
    assert report["error"] == "RuntimeError: a defect after the erase"
    assert [step["name"] for step in report["steps"]][-1] == "erase"
    assert all(step["ok"] for step in report["steps"])
    assert "error: RuntimeError: a defect after the erase" in err


def test_an_error_closing_the_bus_after_the_sequence_ends_it_with_the_report(
        monkeypatch, image_file, tmp_path, capsys):
    client = FakeClient()

    @contextlib.contextmanager
    def connect(spec, **kwargs):
        yield client
        # After the last request, as shutting the bus down could fail.
        raise OSError(errno.EBADF, "Bad file descriptor")
    monkeypatch.setattr(diag, "connect", connect)
    monkeypatch.setattr(flasher, "TRIAL_POLL_S", 0.0)
    report, err = _failed_run(image_file, tmp_path, capsys)
    text = f"closing the bus: OSError: [Errno {errno.EBADF}] Bad file descriptor"
    assert report["result"] == "failed" and report["failed_step"] is None
    assert report["error"] == text
    assert [step["name"] for step in report["steps"]][-1] == "boot_info_confirmed"
    assert all(step["ok"] for step in report["steps"])
    assert report["confirmed_by"] == "routine"
    assert f"error: {text}" in err


def test_an_interrupt_goes_through_and_its_step_still_has_a_complete_record(fake, image_file):
    report = flasher.Report(can="udp:1", image={})
    with pytest.raises(KeyboardInterrupt):
        flasher.run_step(report, "transfer", _raising(KeyboardInterrupt()))
    assert (report.steps[-1]["name"], report.steps[-1]["ok"]) == ("transfer", False)
    assert "duration_s" in report.steps[-1]
    fake.answers["transfer_2"] = _raising(KeyboardInterrupt())
    with pytest.raises(KeyboardInterrupt):
        flasher.main(["--can", "udp:1", str(image_file)])


@pytest.mark.parametrize("arguments", [
    [], ["--address", "zz"], ["--address", "0x100000000"], ["--address", "-1"], ["--bogus"],
    ["--trial-wait-s", "soon"],
], ids=["no-arguments", "address-not-a-number", "address-too-big", "address-negative",
        "unknown-option", "wait-not-a-number"])
def test_an_argument_the_flasher_refuses_is_status_1_before_any_request(fake, image_file,
                                                                        capsys, arguments):
    argv = ["--can", "udp:1", *arguments, str(image_file)] if arguments else []
    with pytest.raises(SystemExit) as stopped:
        flasher.main(argv)
    assert stopped.value.code == 1
    assert fake.calls == []
    assert "python -m fcn_tools.flasher: error: " in capsys.readouterr().err


def test_a_report_file_that_cannot_be_written_is_status_1_before_any_request(fake, image_file,
                                                                             tmp_path):
    report = tmp_path / "no such directory" / "report.json"
    assert flasher.main(["--can", "udp:1", "--report", str(report), str(image_file)]) == 1
    assert fake.calls == []


def test_a_bus_that_cannot_be_opened_is_status_1(monkeypatch, image_file, tmp_path, capsys):
    @contextlib.contextmanager
    def cannot_open(spec, **kwargs):
        # As fcn_tools.udpbus raises it when its socket cannot bind.
        raise can.CanInitializationError("cannot bind 127.0.0.1:1 for the UDP bus: "
                                         "Permission denied", errno.EACCES)
        yield  # a generator, as contextmanager needs
    monkeypatch.setattr(diag, "connect", cannot_open)
    report = tmp_path / "report.json"
    assert flasher.main(["--can", "udp:1", "--report", str(report), str(image_file)]) == 1
    assert "cannot bind 127.0.0.1:1" in capsys.readouterr().err
    assert not report.exists()               # opened first, and removed again


def test_an_image_that_cannot_be_read_leaves_no_report_file(fake, tmp_path):
    report = tmp_path / "report.json"
    report.write_text("a report of an earlier run\n")
    assert flasher.main(["--can", "udp:1", "--report", str(report),
                         str(tmp_path / "missing.img")]) == 1
    assert not report.exists() and fake.calls == []
