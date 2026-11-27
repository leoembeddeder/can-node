# SPDX-License-Identifier: MIT
"""The update over the bus, as a tester makes it: python -m fcn_tools.flasher.

    python -m fcn_tools.flasher --can SPEC IMAGE [--report PATH] [--address A]
                                [--no-reset] [--no-confirm] [--trial-wait-s S]

IMAGE is an image file (python -m fcn_tools.image pack). The programming sequence of the node
(lib/uds/uds.h, THE DOWNLOAD), each step a UDS request through fcn_tools.diag and timed:

  step                  request                                   expected answer
  boot_info             22 FD 00                                  the running slot: the target is
                                                                  the other one (or --address)
  default_session       10 01                                     50 01 ...: the sequence starts
                                                                  in the default session, whatever
                                                                  a tester before left (the node
                                                                  enters programming only from
                                                                  extended, extended not from
                                                                  programming)
  extended_session      10 03                                     50 03 ...
  programming_session   10 02                                     50 02 ...
  erase                 31 01 FF 00 44 <address> <size>           responsePending, 71 01 FF 00 ...
  request_download      34 00 44 <address> <size>                 74 20 <maxNumberOfBlockLength>
  transfer              36 <counter> <data>, max_length - 2 bytes  76 <counter>, per block
                        of data each, the counter from 1, wrapping
                        from 0xFF to 0x00
  transfer_exit         37                                        77
  check                 31 01 FF 01                               responsePending, 71 01 FF 01 ...
  ecu_reset             11 01                                     51 01; the node resets, and its
                                                                  bootloader starts the trial
  wait_for_trial        22 FD 00 until it names the target slot   the trial, attempt 1
                        as a trial (an answer from the image
                        before the reset does not): the first
                        TRIAL_POLL_S after the ECUReset's
                        answer, each waiting TRIAL_POLL_TIMEOUT_S,
                        the next TRIAL_POLL_S after one that did
                        not find it (_wait_for_trial())
  read_version          22 F1 89                                  the image's version
  extended_session_trial 10 03
  confirm               31 01 F0 00                               71 01 F0 00 00 00; or 7F 31 22
                                                                  when the trial confirmed itself
                                                                  first (its 5 s of good health),
                                                                  which 22 FD 00 then shows
  boot_info_confirmed   22 FD 00                                  confirm state CONFIRMED

--no-reset stops after the check (the trial is committed, the node keeps running the old image);
--no-confirm after read_version. The timeouts are fcn_tools.diag's, but for wait_for_trial's
requests: each request waits as long as the node answers responsePending, up to a bound from
the flash part's maximum times.

THE REPORT (schema fcn.flash_report/1, JSON, to --report or stdout): the bus, the image (its
path, size, version, SHA-256 of the file), the target slot and address, maxNumberOfBlockLength
and the data per block, every step with its start (seconds from the first request, the tester's
monotonic clock), its duration, whether it succeeded and, if not, the NRC (name and code) or the
error, the time of every TransferData block, the transfer's bytes and throughput, the boot
information before, of the trial and after the confirm, who confirmed (the routine or the node
itself), "result" ("confirmed", "committed", "trial", or "failed") and the failed step, or, when
the flasher stopped between two steps on an error of its own, no failed step and "error"
("error" also says why the bus could not be closed after the sequence, if it could not). Times
are the tester's: they include the bus and both hosts.

EXIT STATUS: 0 for a result that is not "failed"; 1 when the flasher cannot start, before its
first request and without a report: an argument it refuses (argparse's own status for that, 2,
is not used), a report file that cannot be written, an image that cannot be read or is no
image, a bus that cannot be opened (the report file is opened first, and removed again after
these last two); 2 when the sequence failed, with its report: a step failed (a negative
response, a timeout, an answer that is not what the sequence needs: one udsoncan finds
unexpected or invalid, or one the flasher cannot use; an error of the bus or the transport), or
the flasher stopped between two steps on an error of its own, or the bus could not be closed
after the sequence. An interrupt (KeyboardInterrupt) ends it as Python ends a program, and the
report is not written.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, NoReturn, Sequence

import can
import udsoncan
from udsoncan import MemoryLocation
from udsoncan.exceptions import (InvalidResponseException, NegativeResponseException,
                                 TimeoutException, UnexpectedResponseException)

from . import diag
from . import image as image_format

SCHEMA = "fcn.flash_report/1"

#: The host's slots (platform/include/plat_flash.h, the layout): where each one starts.
SLOT_ADDRESSES = {"A": 0x002000, "B": 0x102000}

#: The data in a TransferData block: maxNumberOfBlockLength counts the SID and the counter.
BLOCK_OVERHEAD = 2

#: wait_for_trial (_wait_for_trial()): how long it waits for the trial by default; the time from
#: the ECUReset's answer to its first request, and from a request that did not find the trial to
#: the next; and how long each of its requests waits for the answer, the node's P2server.
TRIAL_WAIT_S = 30.0
TRIAL_POLL_S = 0.050
TRIAL_POLL_TIMEOUT_S = diag.P2_SERVER_S


class StepFailed(Exception):
    """A step of the sequence did not get the answer it needs."""


@dataclass
class Report:
    """What the flasher did; as_json() is the report's JSON."""
    can: str
    image: dict[str, Any]
    steps: list[dict[str, Any]] = field(default_factory=list)
    blocks_s: list[float] = field(default_factory=list)
    extra: dict[str, Any] = field(default_factory=dict)
    origin: float = field(default_factory=time.monotonic)
    result: str = "failed"
    failed_step: str | None = None

    def as_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {"schema": SCHEMA, "tool": "fcn_tools.flasher",
                               "versions": {"udsoncan": udsoncan.__version__,
                                            "can-isotp": _isotp_version()},
                               "can": self.can, "image": self.image}
        out.update(self.extra)
        out["steps"] = self.steps
        out["blocks_s"] = [round(value, 6) for value in self.blocks_s]
        out["result"] = self.result
        out["failed_step"] = self.failed_step
        out["total_s"] = round(sum(step["duration_s"] for step in self.steps), 6)
        return out


def _isotp_version() -> str:
    import isotp  # noqa: PLC0415 (only for its version)
    return getattr(isotp, "__version__", "unknown")


def plan_blocks(size: int, max_block_length: int) -> list[tuple[int, int, int]]:
    """The TransferData blocks of an image of `size` bytes: (counter, offset, length) each, the
    counter from 1 wrapping from 0xFF to 0x00, at most max_block_length - 2 bytes of data each."""
    data = max_block_length - BLOCK_OVERHEAD
    if data < 1:
        raise ValueError(f"maxNumberOfBlockLength {max_block_length} leaves no data")
    blocks = []
    counter = 1
    for offset in range(0, size, data):
        blocks.append((counter, offset, min(data, size - offset)))
        counter = (counter + 1) & 0xFF
    return blocks


def describe_image(path: Path) -> tuple[bytes, dict[str, Any]]:
    """The image file's bytes and what the report says of it; ValueError if it is no image."""
    data = path.read_bytes()
    if len(data) <= image_format.HEADER_SIZE:
        raise ValueError(f"{path}: {len(data)} bytes, not an image")
    header = image_format.decode_header(data[:image_format.HEADER_SIZE])
    if header.magic != image_format.MAGIC:
        raise ValueError(f"{path}: no image header")
    return data, {"path": str(path), "size": len(data),
                  "version": image_format.format_version(header.image_version),
                  "version_code": header.image_version, "target": header.target,
                  "sha256": hashlib.sha256(data).hexdigest()}


def _nrc_of(error: NegativeResponseException) -> dict[str, Any]:
    code = error.response.code
    return {"nrc": code, "nrc_name": error.response.code_name}


def run_step(report: Report, name: str, action: Callable[[], Any],
             request: str = "") -> Any:
    """Make one step: its record in the report, timed, and its result; StepFailed if it fails,
    whatever Exception the action raised, with the error in the record. KeyboardInterrupt and
    SystemExit go through; the record, appended all the same, then says ok False."""
    start = time.monotonic()
    record: dict[str, Any] = {"name": name, "request": request,
                              "start_s": round(start - report.origin, 6), "ok": False}
    try:
        result = action()
        record["ok"] = True
        return result
    except NegativeResponseException as error:
        record.update(**_nrc_of(error), error=str(error))
        raise StepFailed(name) from error
    except TimeoutException as error:
        record.update(error=f"timeout: {error}")
        raise StepFailed(name) from error
    except UnexpectedResponseException as error:   # a valid answer, but not to this request
        record.update(error=f"unexpected response: {error}")
        raise StepFailed(name) from error
    except InvalidResponseException as error:      # an answer udsoncan cannot decode
        record.update(error=f"invalid response: {error}")
        raise StepFailed(name) from error
    except (StepFailed, ValueError, KeyError) as error:
        record.update(error=str(error))
        raise StepFailed(name) from error
    except Exception as error:  # noqa: BLE001 (the bus, the transport: python-can, isotp, OSError)
        record.update(error=f"{type(error).__name__}: {error}")
        raise StepFailed(name) from error
    finally:
        record["duration_s"] = round(time.monotonic() - start, 6)
        report.steps.append(record)


def _timed(client: udsoncan.client.Client, timeout: float | None, call: Callable[[], Any]) -> Any:
    """`call` with the client's request timeout set to `timeout` for it."""
    before = client.config["request_timeout"]
    client.set_config("request_timeout", timeout)
    try:
        return call()
    finally:
        client.set_config("request_timeout", before)


def _routine_record(response: Any) -> list[int]:
    status = response.service_data.routine_status_record
    return list(status) if status is not None else []


def _wait_for_trial(client: udsoncan.client.Client, slot: str, wait_s: float) -> diag.BootInfo:
    """Ask for the boot information until it names `slot` as a trial: the node has been reset
    and its bootloader started the trial. Answers of the image before the reset, a negative
    response, and no answer while the node restarts mean "not yet".

    THE POLL. The first request goes TRIAL_POLL_S (50 ms) after the ECUReset's answer: one sent
    at once reaches no node (the node that answered is in its reset, the trial not started yet)
    and could only wait out its timeout, while the restart through fcn_boot takes a few ms on
    the host (bench/updates.py's reset_to_trial, from the node's own log), so the first request
    normally finds the trial. Each request waits TRIAL_POLL_TIMEOUT_S, the node's P2server
    (50 ms), within which a node that runs answers 22 FD 00 (it needs no responsePending): a
    request still unanswered then reached no node, or one the host held up, and is not a
    failure; the next goes TRIAL_POLL_S after any that did not find the trial. So the step
    takes the restart rounded up to the poll, at most one period of 100 ms beyond it, and one
    request, plus, after a request that went unanswered, the wait for the answers still due
    (below: TRIAL_POLL_TIMEOUT_S when none is); up to `wait_s`. (A first request at once with
    P2client, 1 s, would wait out that second every time.)

    A LATE ANSWER. A request the trial received is answered even after its timeout, in the
    order of the requests (app/diag.h, RESPONSES). udsoncan drops what came in before each
    request (Client.send_request() empties the connection's receive queue), so an answer at
    most a period late is dropped; a later one is taken by the next request as its own. That is
    no wrong answer: every request here is the same 22 FD 00, udsoncan takes only 62 FD 00 for
    it, and the slot and the mode it names come from the hand-off, the same for the whole run of
    an image. But the answer to the request that took it is then still due, and arriving after
    this step it would be the answer read_version gets for 22 F1 89, which udsoncan refuses as
    an unexpected response: a failed update. So once the trial has answered, the answers still
    due are taken here first, at most one per request that went unanswered, each awaited for
    TRIAL_POLL_TIMEOUT_S, as the trial sends each right after the one before. Only a hold-up
    of the host longer than that just then lets one through, and read_version then fails with
    "unexpected response" in the report: never a wrong version taken."""
    deadline = time.monotonic() + wait_s
    last: diag.BootInfo | None = None
    unanswered = 0
    while True:
        time.sleep(TRIAL_POLL_S)
        if time.monotonic() >= deadline:
            break
        try:
            last = _timed(client, TRIAL_POLL_TIMEOUT_S, lambda: diag.read_boot_info(client))
        except TimeoutException:
            unanswered += 1
            continue
        except NegativeResponseException:
            continue
        if last.slot == slot and last.mode == "trial":
            for _ in range(unanswered):
                if client.conn.wait_frame(timeout=TRIAL_POLL_TIMEOUT_S) is None:
                    break
            return last
    raise StepFailed(f"no trial in slot {slot} within {wait_s} s (last boot information: "
                     f"{last.as_dict() if last else None})")


def flash(spec: str, image_path: Path, address: int | None = None, reset: bool = True,
          confirm: bool = True, trial_wait_s: float = TRIAL_WAIT_S) -> Report:
    """The programming sequence (see the module's text) on the bus `spec`; the report. A step
    that fails ends the sequence; the report says which. An error of the flasher between two
    steps ends it too, with result "failed" and the error in the report, and so does an error
    closing the bus after the sequence; KeyboardInterrupt and SystemExit go through. Before the
    first request it raises OSError or ValueError for an image that cannot be read or is no
    image, and what fcn_tools.diag.connect() raises for a bus that cannot be opened
    (ValueError, OSError, can.CanError)."""
    data, image = describe_image(image_path)
    size = len(data)
    report = Report(can=spec, image=image)
    with contextlib.ExitStack() as connection:
        client = connection.enter_context(diag.connect(spec))
        try:
            _sequence(client, report, data, size, address, reset, confirm, trial_wait_s)
        except StepFailed as failed:
            report.result = "failed"
            report.failed_step = report.steps[-1]["name"] if report.steps else str(failed)
        except Exception as error:  # noqa: BLE001 (raised outside any step: still a report)
            report.result = "failed"
            report.extra["error"] = f"{type(error).__name__}: {error}"
        try:
            connection.close()      # here, so that an error closing the bus is in the report
        except Exception as error:  # noqa: BLE001 (after the requests: still a report)
            closing = f"closing the bus: {type(error).__name__}: {error}"
            earlier = report.extra.get("error")
            report.result = "failed"
            report.extra["error"] = f"{earlier}; {closing}" if earlier else closing
    return report


def _sequence(client: udsoncan.client.Client, report: Report, data: bytes, size: int,
              address: int | None, reset: bool, confirm: bool, trial_wait_s: float) -> None:
    before = run_step(report, "boot_info", lambda: diag.read_boot_info(client), "22 FD 00")
    report.extra["boot_info_before"] = before.as_dict()
    if address is None:
        if before.slot is None:
            run_step(report, "target_slot", _fail("the node reports no running slot (no "
                                                  "hand-off): give --address"))
        target = "B" if before.slot == "A" else "A"
        address = SLOT_ADDRESSES[target]
    else:
        target = next((name for name, start in SLOT_ADDRESSES.items() if start == address), "?")
    report.extra.update(slot=target, address=address)
    location = MemoryLocation(address=address, memorysize=size, address_format=32,
                              memorysize_format=32)
    run_step(report, "default_session", lambda: client.change_session(1), "10 01")
    run_step(report, "extended_session", lambda: client.change_session(3), "10 03")
    run_step(report, "programming_session", lambda: client.change_session(2), "10 02")
    erase = run_step(report, "erase",
                     lambda: _timed(client, diag.erase_timeout_s(size),
                                    lambda: client.start_routine(
                                        diag.RID_ERASE_MEMORY,
                                        bytes([0x44]) + address.to_bytes(4, "big")
                                        + size.to_bytes(4, "big"))),
                     f"31 01 FF 00 44 {address:08X} {size:08X}")
    report.extra["erase_status_record"] = _routine_record(erase)

    def request_download() -> tuple[int, list[tuple[int, int, int]]]:
        # A maxNumberOfBlockLength that leaves no data (ValueError) fails this step.
        max_length = int(client.request_download(location).service_data.max_length)
        return max_length, plan_blocks(size, max_length)

    max_length, blocks = run_step(report, "request_download", request_download, "34 00 44 ...")
    report.extra.update(max_block_length=max_length, block_data=max_length - BLOCK_OVERHEAD,
                        blocks=len(blocks))

    def transfer() -> None:
        for counter, offset, length in blocks:
            start = time.monotonic()
            _timed(client, diag.write_timeout_s(length),
                   lambda c=counter, o=offset, n=length: client.transfer_data(c, data[o:o + n]))
            report.blocks_s.append(time.monotonic() - start)

    run_step(report, "transfer", transfer, f"36 xx ... x {len(blocks)}")
    transfer_s = report.steps[-1]["duration_s"]
    report.extra["transfer"] = {"bytes": size, "blocks": len(blocks), "duration_s": transfer_s,
                                "bytes_per_s": round(size / transfer_s, 1) if transfer_s else None}
    run_step(report, "transfer_exit", lambda: client.request_transfer_exit(), "37")
    check = run_step(report, "check",
                     lambda: _timed(client, diag.check_timeout_s(size),
                                    lambda: client.start_routine(diag.RID_CHECK_PROGRAMMING)),
                     "31 01 FF 01")
    report.extra["check_status_record"] = _routine_record(check)
    report.result = "committed"
    if not reset:
        return
    run_step(report, "ecu_reset", lambda: client.ecu_reset(1), "11 01")
    trial = run_step(report, "wait_for_trial",
                     lambda: _wait_for_trial(client, target, trial_wait_s), "22 FD 00 ...")
    report.extra["boot_info_trial"] = trial.as_dict()
    version = run_step(report, "read_version", lambda: diag.read_version(client), "22 F1 89")
    report.extra["version_read"] = version
    if version != report.image["version"]:
        run_step(report, "version_check",
                 _fail(f"the trial reports {version}, the image is {report.image['version']}"))
    report.result = "trial"
    if not confirm:
        return
    run_step(report, "extended_session_trial", lambda: client.change_session(3), "10 03")
    refused = None
    try:
        record = run_step(report, "confirm",
                          lambda: client.start_routine(diag.RID_CONFIRM_TRIAL), "31 01 F0 00")
        report.extra["confirm_status_record"] = _routine_record(record)
        report.extra["confirmed_by"] = "routine"
    except StepFailed:
        refused = report.steps[-1]
        if refused.get("nrc") != 0x22:
            raise
    after = run_step(report, "boot_info_confirmed", lambda: diag.read_boot_info(client),
                     "22 FD 00")
    report.extra["boot_info_after"] = after.as_dict()
    if after.confirm_state != "CONFIRMED" or after.slot != target:
        run_step(report, "confirm_check", _fail(f"not confirmed: {after.as_dict()}"))
    if refused is not None:
        # conditionsNotCorrect, and the trial is CONFIRMED: it confirmed itself before the
        # routine came (its 5 s of good health; the first one wins, app/flash_job.h).
        refused["ok"] = True
        refused["outcome"] = "conditionsNotCorrect: the trial had confirmed itself"
        report.extra["confirmed_by"] = "node"
    report.result = "confirmed"


def _fail(message: str) -> Callable[[], None]:
    def action() -> None:
        raise StepFailed(message)
    return action


class _Parser(argparse.ArgumentParser):
    """argparse's parser, except that an argument it refuses exits with status 1, the flasher's
    usage error: argparse's own 2 is the flasher's failed sequence."""

    def error(self, message: str) -> NoReturn:
        self.print_usage(sys.stderr)
        self.exit(1, f"{self.prog}: error: {message}\n")


def _address(text: str) -> int:
    """--address: a number in any base int() reads, of 32 bits (ALFID 0x44)."""
    try:
        value = int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a number: {text!r}") from None
    if not 0 <= value <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError(f"{text} is not an address of 32 bits")
    return value


def main(argv: Sequence[str] | None = None) -> int:
    parser = _Parser(prog="python -m fcn_tools.flasher",
                     description="Update the node over the bus (UDS download).")
    parser.add_argument("image", type=Path, help="image file (python -m fcn_tools.image pack)")
    parser.add_argument("--can", required=True, help="the bus: udp[:<base port>] or "
                                                     "socketcan:<interface>")
    parser.add_argument("--report", type=Path, help="write the JSON report here (default stdout)")
    parser.add_argument("--address", type=_address,
                        help="the slot's start address (default: the slot that does not run)")
    parser.add_argument("--no-reset", action="store_true",
                        help="stop after the check routine: no ECUReset")
    parser.add_argument("--no-confirm", action="store_true",
                        help="stop after reading the trial's version: no confirm routine")
    parser.add_argument("--trial-wait-s", type=float, default=TRIAL_WAIT_S,
                        help=f"how long to wait for the trial after the reset (default "
                             f"{TRIAL_WAIT_S:g})")
    args = parser.parse_args(argv)
    with contextlib.ExitStack() as files:
        out = sys.stdout
        try:
            if args.report is not None:
                # Opened before the first request, so that a report file that cannot be
                # written is a usage error and not a node updated without its report.
                out = files.enter_context(args.report.open("w"))
            report = flash(args.can, args.image, address=args.address, reset=not args.no_reset,
                           confirm=not args.no_confirm, trial_wait_s=args.trial_wait_s)
        except (OSError, ValueError, can.CanError) as error:
            print(f"fcn_tools.flasher: {error}", file=sys.stderr)
            if out is not sys.stdout:       # the image or the bus: nothing was sent, no report
                files.close()
                args.report.unlink(missing_ok=True)
            return 1
        out.write(json.dumps(report.as_json(), indent=1) + "\n")
    # Every record has "ok" and "duration_s" (run_step), whatever ended its step.
    for step in report.steps:
        state = "ok" if step["ok"] else f"FAILED {step.get('nrc_name') or step.get('error')}"
        print(f"{step['name']:24s} {step['duration_s']:10.3f} s  {state}", file=sys.stderr)
    if "error" in report.extra:
        print(f"error: {report.extra['error']}", file=sys.stderr)
    print(f"result: {report.result}", file=sys.stderr)
    return 2 if report.result == "failed" else 0


if __name__ == "__main__":
    sys.exit(main())
