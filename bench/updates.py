#!/usr/bin/env python3
"""Measure the update over the bus end to end, many times, and the download of a 256 KiB image.

Everything measured here runs on a Linux host: the node is the FreeRTOS POSIX port started
through the host bootloader fcn_boot, the tester is the flasher (python -m fcn_tools.flasher,
udsoncan 1.26.1 over can-isotp 2.0.7) in Python, and the bus is the node's model of a 500 kbit/s
CAN bus over SocketCAN (vcan0) or the UDP bus.

1. Updates. On each transport of the campaign (--socketcan-updates, default 50, then
   --udp-updates, default 10) a flash starts with the release node, stripped, as the factory
   image 1.0.0 in slot A; then each update, one after the other and never repeated: the board
   is started through fcn_boot (fcn_tools.node.Board, which starts it again after the
   flasher's ECUReset), and once its first status frame is on the bus, the flasher updates it
   with the same node under the next version: into the slot that does not run (B, then A, then
   B, ...), erase, download, check, ECUReset, the trial started by fcn_boot, its version read,
   and the confirm routine. The bench then stops the board in order and reads the trial run's
   statistics and the flash (fcn_flash info and fcn_tools.flash, which must agree, and the
   reference model's invariants), so the next update starts from the image this one confirmed.
   Per update: every step's time from the flasher's report, the total, the flasher's wall time,
   the board's start to its first status frame; the node's restart (reset_to_trial), from the
   download run's log line of the tester's ECUReset to the trial run's line of its hand-off, by
   the node's own log stamps; the node's own counts: the download run's log (the bytes it
   erased for, its erase and check steps and times: a run that ends in a reset writes no
   statistics) and the trial run's statistics (the hand-off it took, who confirmed it, the
   confirm job, its UDS counts); success or the failed step. A failure is kept with the
   flasher's whole report and is never retried; the campaign goes on with the next update if
   the flash allows one (no trial pending), and stops otherwise.
2. The 256 KiB image. --big-downloads (default 10) downloads, each into slot B of a fresh flash
   (slot A the factory image) of a node started without the bootloader, by the flasher with
   --no-reset: erase, transfer, check, committed as the trial; the node is then stopped in order,
   so that its statistics count the download (bytes received, blocks, the flash jobs and their
   times). The image's payload is exactly --big-payload bytes (default 262,144): the stripped
   release node padded with zero bytes (the ELF loader ignores what follows the file's last
   segment), so it is a program, and after the last download the board is started once through
   fcn_boot to show that the trial starts (its hand-off in its statistics). Below the node's
   size (a --quick run) it is a payload of seeded random bytes instead, validated by the check
   routine and never started, as the result's config says.

Builds the release preset first (scripts/build.sh release) unless --no-build, refuses to measure
with binaries that are not the tree's build (the payload's id is checked too, and every trial
run's statistics report it), and writes results/updates.json and results/updates.png.

Exit status: 0 the result was written and every update and download succeeded; 5 the result was
written and holds one that did not (or an update that was never attempted because the campaign
stopped), as power_loss.py does for a failed trial; 1 an error; 2 a usage error; 3 a loaded
host; 4 a binary that is not the tree's build; 128 + N ended by signal N. Nothing is written
unless the status is 0 or 5.

    python bench/updates.py                     # 50 + 10 updates and 10 downloads, ~30 minutes
    python bench/updates.py --socketcan-updates 5 --udp-updates 0 --big-downloads 1
    python bench/updates.py --quick --out /tmp/b   # a test of the bench, under a minute
    python bench/updates.py --replot               # redraw the plot from the JSON
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import subprocess
import sys
import time
import traceback
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence

import numpy as np

from fcn_tools import bench, bus, flasher, image as image_format, paths, plotstyle, results, runner
from fcn_tools.node import EXIT_OK, RESET_REQUESTED, RESETS

NAME = "updates"
VERSION = 1

#: Exit status of a written result that holds an update or a download that did not succeed.
EXIT_FINDINGS = 5

BITRATE = 500_000
DEFAULT_SOCKETCAN_UPDATES = 50
DEFAULT_UDP_UPDATES = 10
DEFAULT_BIG_DOWNLOADS = 10
DEFAULT_BIG_PAYLOAD = 256 * 1024
DEFAULT_BIG_TRANSPORT = "socketcan"
QUICK = {"socketcan_updates": 0, "udp_updates": 1, "big_downloads": 1, "big_payload": 8192,
         "big_transport": "udp"}

#: The versions: the factory image, the updates of each transport (minor = the update's number)
#: and the 256 KiB image.
FACTORY_VERSION = "1.0.0"
CAMPAIGN_MAJOR = {"socketcan": 2, "udp": 3}
BIG_VERSION = "4.0.0"
#: The slot the 256 KiB downloads write (the host's slot B) and the seed of a data payload.
BIG_ADDRESS = "0x102000"
DATA_SEED = 20261124
#: The data of a TransferData block: the node's maxNumberOfBlockLength 1026 less the SID and
#: the counter (the flasher sends blocks of that size).
BLOCK_DATA = 1024

#: The steps of an update, in the flasher's order (fcn_tools.flasher), and those of a download
#: that stops after the check (--no-reset).
UPDATE_STEPS = ("boot_info", "default_session", "extended_session", "programming_session",
                "erase", "request_download", "transfer", "transfer_exit", "check", "ecu_reset",
                "wait_for_trial", "read_version", "extended_session_trial", "confirm",
                "boot_info_confirmed")
DOWNLOAD_STEPS = UPDATE_STEPS[:UPDATE_STEPS.index("check") + 1]

#: Bounds only: the wait for a board's first status frame, for the trial's run after the reset,
#: for the flasher (fcn_tools.tester.FLASHER_TIMEOUT_S).
START_WAIT_S = 10.0
RUN_WAIT_S = 30.0
FLASHER_TIMEOUT_S = 240.0

TIMING_NOTE = (
    "An update's time is set by the node's diagnostic task (2 events per 1 ms tick: about 79 ms "
    "for each block of 1024 bytes, bench/isotp_throughput.py), the modelled flash (45 ms per "
    "sector erase, 0.4 ms per page, each wait ending at a tick) and, after the ECUReset, the "
    "flasher's poll for the trial (wait_for_trial: its first request 50 ms after the reset's "
    "answer, the next 50 ms after each one that did not find it); the restart itself, fcn_boot "
    "as a host process and the trial's start-up, is reset_to_trial, by the node's own log "
    "stamps. The host adds its scheduling to each and the tester's Python start-up and "
    "turnaround, which a tester on a CAN adapter does not have. The bytes, blocks, steps and "
    "slots of an update do not depend on the host's timing; its success can, in one direction "
    "only: a stall of the host past the node's 500 ms watchdog or the 1 s ISO-TP timeout fails "
    "an update, and none makes one succeed. A failed update is kept with its record and "
    "counted, so the updates counted as succeeded did succeed, whatever the host did."
)

# The download run's own account of the download (app/app.c, log_job()), and the trial run's of
# the confirm by the routine.
ERASE_LINE = re.compile(r"download: slot at 0x([0-9A-Fa-f]+) erased for (\d+) bytes, (\d+) steps "
                        r"\((\d+) us\)")
CHECK_LINE = re.compile(r"download: the image validated and is committed as the trial, (\d+) "
                        r"steps \((\d+) us\)")
RESET_LINE = "ECUReset: resetting (requested by the tester)"
CONFIRM_LINE = re.compile(r"confirmed by the tester's confirm routine: slot ([AB]), image "
                          r"(\d+\.\d+\.\d+), is the active image \((\d+) us\)")

# The node's restart after the tester's ECUReset, by its own log stamps (lib/log's
# "[seconds.microseconds]", plat_time_ns(): CLOCK_MONOTONIC of the host, so the stamps of two
# runs on the same host can be subtracted): from the download run's line just before its
# plat_reset() (app/app.c, reset_if_due()) to the trial run's line of its hand-off, at its
# start-up before the scheduler (app/app.c, log_handoff()). Host timing: the old run's end, the
# Board starting fcn_boot again (fcn_tools.node, as a reset into the bootloader would), fcn_boot
# as a host process (memfd and exec, not a jump) and the trial's start-up.
STAMP = r"^\[\s*(\d+)\.(\d{6})\] "
RESET_STAMP = re.compile(STAMP + r"W app: " + re.escape(RESET_LINE) + r"$", re.MULTILINE)
TRIAL_START_STAMP = re.compile(STAMP + r"I app: started by the bootloader: slot [AB] as a trial",
                               re.MULTILINE)
RESET_TO_TRIAL = ("host timing, by the node's own log stamps (CLOCK_MONOTONIC of the host): the "
                  "download run's 'ECUReset: resetting' line to the trial run's 'started by the "
                  "bootloader' line; the old run's end, the Board starting fcn_boot again, "
                  "fcn_boot as a host process (memfd and exec, not a jump) and the trial's "
                  "start-up")


# --------------------------------------------------------------------------- pure analysis


def update_version(transport: str, number: int) -> str:
    """The version of update `number` (1, 2, ...) of the campaign on `transport`."""
    if not 1 <= number <= 255:
        raise ValueError("an update's number is its minor version, 1 to 255")
    return f"{CAMPAIGN_MAJOR[transport]}.{number}.0"


def target_slot(number: int) -> str:
    """The slot update `number` writes while every update before it succeeded: B for the first,
    then A, B, ... (the factory image is in A, and each update goes to the slot that does not
    run)."""
    return "B" if number % 2 == 1 else "A"


def big_payload(node: bytes, size: int, seed: int = DATA_SEED) -> tuple[bytes, str]:
    """The payload of the big image, exactly `size` bytes: the node padded with zero bytes
    ("node, padded") if it fits, else `size` bytes from a generator seeded with `seed` ("data")."""
    if size < 1:
        raise ValueError("a payload has at least one byte")
    if len(node) <= size:
        return node + bytes(size - len(node)), "node, padded"
    return random.Random(seed).randbytes(size), "data"


def parse_download_log(text: str) -> dict[str, Any]:
    """What the log of the run that took a download says of it: the erase (address, bytes, steps,
    us), the check (steps, us) and whether it reset for the tester; None for what it lacks."""
    erase = ERASE_LINE.search(text)
    check = CHECK_LINE.search(text)
    return {
        "erase": None if erase is None else {"address": int(erase.group(1), 16),
                                             "bytes": int(erase.group(2)),
                                             "steps": int(erase.group(3)),
                                             "us": int(erase.group(4))},
        "check": None if check is None else {"steps": int(check.group(1)),
                                             "us": int(check.group(2))},
        "reset_for_the_tester": RESET_LINE in text,
    }


def parse_trial_log(text: str) -> dict[str, Any] | None:
    """The trial run's line of the confirm by the tester's routine: slot, version, us; None."""
    found = CONFIRM_LINE.search(text)
    if found is None:
        return None
    return {"slot": found.group(1), "version": found.group(2), "us": int(found.group(3))}


def log_stamp_us(text: str, line: re.Pattern[str]) -> int | None:
    """The stamp of the first line of the log `text` that `line` matches, in microseconds of the
    node's clock; None if no line does."""
    found = line.search(text)
    return None if found is None else int(found.group(1)) * 1_000_000 + int(found.group(2))


def node_restart(download_log: str, trial_log: str) -> dict[str, int | None]:
    """The node's restart after the tester's ECUReset by its own log stamps (RESET_STAMP and
    TRIAL_START_STAMP): the download run's reset line (reset_us), the trial run's line of its
    hand-off (trial_start_us) and the time between them (us). "us" is None, the update refused
    for reset_to_trial and counted, if either line is missing or the trial's is not after the
    reset's."""
    reset = log_stamp_us(download_log, RESET_STAMP)
    start = log_stamp_us(trial_log, TRIAL_START_STAMP)
    us = start - reset if reset is not None and start is not None and start > reset else None
    return {"reset_us": reset, "trial_start_us": start, "us": us}


def erase_steps(image_bytes: int, sector: int = 4096) -> int:
    """The erase job's steps for an image of `image_bytes`: the BEGIN record, then one sector a
    step (docs/update.md, the slot writer)."""
    return 1 + -(-image_bytes // sector)


def check_steps(image_bytes: int, header: int = image_format.HEADER_SIZE) -> int:
    """The check job's steps: 8 validator steps of the header, one per 64 bytes of payload, and
    the COMMIT record (docs/update.md, the slot writer)."""
    return 8 + -(-(image_bytes - header) // 64) + 1


def download_log_problems(logged: dict, image_bytes: int) -> list[str]:
    """What the download run's log (parse_download_log()) says that does not fit a download of
    an image of `image_bytes` that the tester saw complete: the erase for that size in its steps,
    the check's steps, the reset for the tester."""
    problems = []
    erase, check = logged["erase"], logged["check"]
    if erase is None or (erase["bytes"], erase["steps"]) != (image_bytes, erase_steps(image_bytes)):
        problems.append(f"the node's log has the erase {erase}, not of {image_bytes} bytes in "
                        f"{erase_steps(image_bytes)} steps")
    if check is None or check["steps"] != check_steps(image_bytes):
        problems.append(f"the node's log has the check {check}, not in "
                        f"{check_steps(image_bytes)} steps")
    if not logged["reset_for_the_tester"]:
        problems.append("the node's log has no reset for the tester")
    return problems


def can_continue(slot_states: Sequence[str]) -> bool:
    """Whether the next update can begin on a flash in these slot states: one image is confirmed
    and no trial is pending (lib/boot refuses a download while one is)."""
    return "confirmed" in slot_states and "trial" not in slot_states


def step_durations(records: Iterable[dict], steps: Sequence[str]) -> dict[str, list[float]]:
    """Each step's durations (s) over the successful records, in their order."""
    out: dict[str, list[float]] = {step: [] for step in steps}
    for record in records:
        if not record["success"]:
            continue
        for step in steps:
            if step in record["steps"]:
                out[step].append(record["steps"][step])
    return out


def campaign_counts(records: Sequence[dict], requested: int) -> dict[str, int]:
    """How a campaign went: requested, attempted, succeeded, failed, not attempted."""
    succeeded = sum(1 for record in records if record["success"])
    return {"requested": requested, "attempted": len(records), "succeeded": succeeded,
            "failed": len(records) - succeeded, "not_attempted": requested - len(records)}


def distributions(samples: dict[str, list[float]], unit: str = "s") -> dict[str, dict]:
    return {name: results.distribution(values, unit) for name, values in samples.items()}


# ---------------------------------------------------------------------------- measuring


@dataclass
class Images:
    """The image files of the bench and what they hold."""
    directory: Path
    node_payload: bytes
    factory: Path
    big: Path
    big_kind: str
    big_payload_bytes: int

    def update(self, transport: str, number: int) -> tuple[Path, str]:
        version = update_version(transport, number)
        path = self.directory / f"node-{version}.img"
        if not path.exists():
            path.write_bytes(bench.pack_image(self.node_payload, version))
        return path, version


def make_images(directory: Path, release_node: Path, big_size: int) -> Images:
    """The images: the release node of `release_node`, stripped, as the factory image and as
    every update, and the big image of `big_size` payload bytes."""
    directory.mkdir(parents=True, exist_ok=True)
    stripped = bench.stripped_copy(release_node, directory / "fcn_node.stripped")
    payload = stripped.read_bytes()
    factory = directory / f"node-{FACTORY_VERSION}.img"
    factory.write_bytes(bench.pack_image(payload, FACTORY_VERSION))
    big_bytes, kind = big_payload(payload, big_size)
    big = directory / f"big-{BIG_VERSION}.img"
    big.write_bytes(bench.pack_image(big_bytes, BIG_VERSION))
    return Images(directory, payload, factory, big, kind, len(big_bytes))


def wait_for_status(listener: Any, run_alive: Any, start: float) -> float:
    """Wait on `listener` (a bus opened before the node was started) for the first status frame
    (the node's tasks run, the diagnostic task among them); the seconds from `start`
    (time.monotonic(), taken just before the start) to its arrival as the bench read it."""
    deadline = time.monotonic() + START_WAIT_S
    while time.monotonic() < deadline:
        message = listener.recv(timeout=0.05)
        if message is not None and bus.is_status(message):
            return time.monotonic() - start
        if not run_alive():
            break
    raise runner.BenchFailure(f"no status frame within {START_WAIT_S} s of the node's start")


def start_ready(session: runner.Session, start_run: Any, alive: Any) -> tuple[Any, float]:
    """start_run() (a node or a board) with a listener on the session's bus opened before it;
    what it returned and the seconds to its first status frame (wait_for_status())."""
    listener = session.open_bus()
    try:
        start = time.monotonic()
        started = start_run()
        return started, wait_for_status(listener, lambda: alive(started), start)
    finally:
        listener.shutdown()


def run_flasher(spec: str, image: Path, report: Path, *options: str) -> tuple[int, dict, float]:
    """python -m fcn_tools.flasher of this tree on the bus `spec`; its exit status, its report
    (empty if it wrote none) and its wall time. Its stderr goes next to the report."""
    env = dict(os.environ)
    tools = str(paths.REPO_ROOT / "tools")
    env["PYTHONPATH"] = tools + (os.pathsep + env["PYTHONPATH"] if env.get("PYTHONPATH") else "")
    start = time.monotonic()
    with open(report.with_suffix(".stderr.txt"), "w") as err:
        done = subprocess.run([sys.executable, "-m", "fcn_tools.flasher", "--can", spec,
                               "--report", str(report), *options, str(image)],
                              stdout=subprocess.DEVNULL, stderr=err, timeout=FLASHER_TIMEOUT_S,
                              check=False, env=env, cwd=paths.REPO_ROOT)
    wall = time.monotonic() - start
    loaded = json.loads(report.read_text()) if report.exists() else {}
    return done.returncode, loaded, wall


def compact_report(report: dict) -> dict:
    """What a record keeps of a successful update's report: steps, transfer, blocks' times."""
    blocks = report.get("blocks_s", [])
    return {"result": report.get("result"), "failed_step": report.get("failed_step"),
            "slot": report.get("slot"), "address": report.get("address"),
            "max_block_length": report.get("max_block_length"), "blocks": report.get("blocks"),
            "transfer": report.get("transfer"), "confirmed_by": report.get("confirmed_by"),
            "boot_info_trial": report.get("boot_info_trial"),
            "block_ms": results.distribution(blocks, "ms", scale=1e3) if blocks else None,
            "versions": report.get("versions")}


def note_ends(board: Any) -> list[dict]:
    """Stop what still runs of `board` in order, take note of how every run ended (the bench
    records it, so the session's end checks need not report it again) and return the runs."""
    if board.current.running:
        try:
            board.stop()
        except subprocess.TimeoutExpired:
            pass
    runs = []
    for run in board.runs:
        status = run.wait(timeout=5.0) if run.running else run._proc.returncode  # noqa: SLF001
        run.wait(timeout=0.1)
        if status in RESETS:
            run.note_reset_awaited()
        runs.append({"name": run.name, "status": status,
                     "stderr_tail": run.stderr().strip().splitlines()[-10:]})
    return runs


def trial_counts(stats: dict) -> dict[str, Any]:
    """What the trial run's statistics say of its start and its confirm."""
    handoff = stats["handoff"]
    confirm = stats["confirm"]
    uds = stats["diag"]["uds"]
    return {
        "build_id": stats["build_id"],
        "reset_cause": stats["reset"]["cause"],
        "handoff": {key: handoff[key] for key in ("verdict", "slot", "mode", "attempt",
                                                  "image_version", "event")},
        "confirm_state": confirm["state"],
        "confirmed_by_routine": confirm["flash_task"]["confirmed_by_routine"],
        "confirm_job": {key: confirm["flash_task"]["jobs"]["confirm"][key]
                        for key in ("run", "last_status", "last_ns")},
        "uds": {"requests": {k: v for k, v in uds["requests"].items() if v},
                "positive": uds["positive"], "nrc": {k: v for k, v in uds["nrc"].items() if v},
                "response_pending": uds["response_pending"]},
    }


def flash_state(flash: Path) -> dict[str, Any]:
    """The flash read both ways (bench.info(): fcn_flash info and fcn_tools.flash must agree)
    and judged by the reference model's invariants."""
    described = bench.info(flash)
    bench.check_invariants(described)
    return {"states": list(bench.states(described)),
            "versions": [slot["header"]["version"] if slot.get("header") else None
                         for slot in described["slots"]]}


def one_update(session: runner.Session, images: Images, flash: Path, cell: Path, number: int,
               out: Path) -> tuple[dict, dict[str, str]]:
    """Update `number` of the campaign on the session's transport (see the module's text); its
    record and the build ids its runs reported."""
    transport = session.transport
    image, version = images.update(transport, number)
    name = f"{transport}-u{number:02d}"
    record: dict[str, Any] = {"number": number, "transport": transport, "version": version,
                              "image_bytes": image.stat().st_size,
                              "target_slot": target_slot(number)}
    reported: dict[str, str] = {}
    boards_before = len(session.boards)
    try:
        board, ready = start_ready(
            session, lambda: session.start_board(flash_file=flash, retained_file=cell, name=name),
            lambda started: started.current.running)
        record["start_to_ready_s"] = round(ready, 6)
    except runner.BenchFailure as error:
        record.update(success=False, failed_step="start", problems=[str(error)], steps={})
        if len(session.boards) > boards_before:
            record["runs"] = note_ends(session.boards[-1])
        record["flash"] = flash_state(flash)
        return record, reported
    board.expect_resets(1)
    report_path = out / f"{name}.report.json"
    status, report, wall = run_flasher(session.can_spec, image, report_path)
    record.update(flasher_status=status, flasher_wall_s=round(wall, 6),
                  steps={step["name"]: step["duration_s"] for step in report.get("steps", [])},
                  total_s=report.get("total_s"), result=report.get("result"),
                  failed_step=report.get("failed_step"), confirmed_by=report.get("confirmed_by"))
    # The slot the flasher chose (the one that does not run); after a failure it need not be the
    # one the alternation from the factory image gives.
    record["target_slot"] = report.get("slot") or record["target_slot"]
    problems = []
    trial = None
    if status != 0 or report.get("result") != "confirmed":
        problems.append(f"the flasher exited {status} with result {report.get('result')}, "
                        f"failed step {report.get('failed_step')}")
    else:
        try:
            trial = board.wait_for_run(2, timeout=RUN_WAIT_S)
        except TimeoutError as error:
            problems.append(f"no run of the trial: {error}")
    runs = note_ends(board)
    record["runs"] = runs
    record["node_download"] = parse_download_log(board.runs[0].log())
    if trial is not None:
        record["node_restart"] = node_restart(board.runs[0].log(), trial.log())
    if not problems:
        problems += download_log_problems(record["node_download"], record["image_bytes"])
    if runs[0]["status"] != RESET_REQUESTED:
        problems.append(f"the download's run ended {runs[0]['status']}, not by the reset")
    if trial is not None:
        if len(runs) != 2 or runs[1]["status"] != EXIT_OK:
            problems.append(f"the runs ended {[run['status'] for run in runs]}")
        else:
            stats = trial.stats()
            reported[f"{name} trial run"] = stats["build_id"]
            record["node_trial"] = trial_counts(stats)
            record["node_trial"]["confirm_line"] = parse_trial_log(trial.log())
            handoff = record["node_trial"]["handoff"]
            if (handoff["slot"], handoff["mode"], handoff["attempt"], handoff["image_version"]) \
                    != (record["target_slot"], "trial", 1, version):
                problems.append(f"the trial run took the hand-off {handoff}")
            if record["node_trial"]["confirm_state"] != "CONFIRMED":
                problems.append(f"the trial run ended {record['node_trial']['confirm_state']}")
    try:
        record["flash"] = flash_state(flash)
    except AssertionError as error:
        record["flash"] = {"error": str(error)}
        problems.append("the flash failed its check")
    if trial is not None and not problems and record["flash"]["versions"][
            "AB".index(record["target_slot"])] != version:
        problems.append(f"the flash holds {record['flash']['versions']}")
    record["success"] = not problems
    record["problems"] = problems
    record["report"] = compact_report(report) if record["success"] else report
    return record, reported


def campaign(session: runner.Session, images: Images, updates: int, out: Path
             ) -> tuple[dict, dict[str, str]]:
    """The updates of one transport, one after the other (see the module's text)."""
    transport = session.transport
    flash, cell = out / f"{transport}-flash.bin", out / f"{transport}-cell.bin"
    bench.factory(flash, images.factory)
    records: list[dict] = []
    reported: dict[str, str] = {}
    stopped = None
    for number in range(1, updates + 1):
        record, ids = one_update(session, images, flash, cell, number, out)
        records.append(record)
        reported.update(ids)
        steps = record.get("steps", {})
        print(f"{transport} update {number}/{updates} to {record['version']} in slot "
              f"{record['target_slot']}: {'ok' if record['success'] else 'FAILED'} "
              f"total {record.get('total_s')} s (transfer {steps.get('transfer')} s)"
              + ("" if record["success"] else f": {'; '.join(record['problems'])}"), flush=True)
        states = record.get("flash", {}).get("states")
        if not record["success"] and (states is None or not can_continue(states)):
            stopped = (f"after update {number}: the flash holds {states}, from which no update "
                       "can begin")
            break
    return {"transport": transport, "counts": campaign_counts(records, updates),
            "stopped": stopped, "records": records}, reported


def big_downloads(session: runner.Session, images: Images, count: int, out: Path
                  ) -> tuple[dict, dict[str, str]]:
    """The downloads of the big image (see the module's text), and its start through fcn_boot
    after the last one if it is a program."""
    records = []
    reported: dict[str, str] = {}
    last_flash = None
    for number in range(1, count + 1):
        flash = out / f"big-{number:02d}-flash.bin"
        bench.factory(flash, images.factory)
        name = f"big-{number:02d}"
        node, ready = start_ready(session, lambda: session.start_node(
            "--flash-file", str(flash), name=name), lambda started: started.running)
        record: dict[str, Any] = {"number": number, "image_bytes": images.big.stat().st_size,
                                  "start_to_ready_s": round(ready, 6)}
        report_path = out / f"{name}.report.json"
        status, report, wall = run_flasher(session.can_spec, images.big, report_path,
                                           "--address", BIG_ADDRESS, "--no-reset")
        stats = session.finish(node)
        reported[f"{name} node"] = stats["build_id"]
        record.update(flasher_status=status, flasher_wall_s=round(wall, 6),
                      steps={step["name"]: step["duration_s"] for step in report.get("steps", [])},
                      total_s=report.get("total_s"), result=report.get("result"),
                      failed_step=report.get("failed_step"))
        jobs = stats["confirm"]["flash_task"]["jobs"]
        record["node"] = {
            "download": {key: stats["diag"]["uds"]["download"][key] for key in (
                "state", "size", "received", "blocks", "repeated_blocks", "erases", "committed",
                "erase_us", "transfer_us", "check_us")},
            "jobs": {kind: {key: jobs[kind][key] for key in ("run", "last_status", "last_steps",
                                                             "total_ns", "max_ns")}
                     for kind in ("erase", "write", "exit", "check")},
            "task": {key: stats["diag"]["task"][key] for key in ("events", "budget_waits",
                                                                  "rx_lost", "tx_done_lost")},
        }
        record["flash"] = flash_state(flash)
        problems = []
        if status != 0 or report.get("result") != "committed":
            problems.append(f"the flasher exited {status} with result {report.get('result')}")
        if record["flash"]["states"] != ["confirmed", "trial"]:
            problems.append(f"the flash holds {record['flash']['states']}")
        download = record["node"]["download"]
        jobs = record["node"]["jobs"]
        blocks = -(-record["image_bytes"] // BLOCK_DATA)
        if (download["received"], download["blocks"], jobs["write"]["run"]) != (
                record["image_bytes"], blocks, blocks):
            problems.append(f"the node received {download['received']} bytes in "
                            f"{download['blocks']} blocks with {jobs['write']['run']} write jobs, "
                            f"not {record['image_bytes']} in {blocks}")
        if (jobs["erase"]["last_steps"], jobs["check"]["last_steps"]) != (
                erase_steps(record["image_bytes"]), check_steps(record["image_bytes"])):
            problems.append(f"the node's erase and check took {jobs['erase']['last_steps']} and "
                            f"{jobs['check']['last_steps']} steps")
        record["success"] = not problems
        record["problems"] = problems
        record["report"] = compact_report(report) if record["success"] else report
        records.append(record)
        print(f"big download {number}/{count} ({record['image_bytes']} bytes): "
              f"{'ok' if record['success'] else 'FAILED'} total {record['total_s']} s, transfer "
              f"{record['steps'].get('transfer')} s", flush=True)
        if record["success"]:
            last_flash = flash
    booted = None
    if images.big_kind == "node, padded" and last_flash is not None:
        cell = out / "big-boot-cell.bin"
        run, ready = start_ready(session, lambda: session.start_node(
            flash_file=last_flash, retained_file=cell, name="big-boot"),
            lambda started: started.running)
        stats = session.finish(run)
        reported["big image, booted"] = stats["build_id"]
        booted = {"start_to_ready_s": round(ready, 6),
                  "handoff": {key: stats["handoff"][key] for key in (
                      "verdict", "slot", "mode", "attempt", "image_version")},
                  "build_id": stats["build_id"]}
    return {"payload_bytes": images.big_payload_bytes, "payload_kind": images.big_kind,
            "image_bytes": images.big.stat().st_size, "transport": session.transport,
            "counts": campaign_counts(records, count), "booted_afterwards": booted,
            "records": records}, reported


# ------------------------------------------------------------------------------ the figures


def restart_times_us(good: Sequence[dict]) -> tuple[list[int], int]:
    """The node's restart (node_restart()) of each successful update that has both of its log
    lines, in microseconds, and how many have not (refused, not dropped unseen)."""
    times = [r.get("node_restart", {}).get("us") for r in good]
    return [us for us in times if us is not None], sum(1 for us in times if us is None)


def campaign_figures(part: dict) -> dict:
    """The distributions of a campaign's successful updates: every step, the total, the
    flasher's wall time, the start to the first status frame, the node's restart by its log
    stamps (with the count refused for a missing line), the throughput, the median block."""
    records = part["records"]
    good = [r for r in records if r["success"]]
    steps = step_durations(records, UPDATE_STEPS)
    restarts, refused = restart_times_us(good)
    return {
        "steps_s": distributions(steps),
        "total_s": results.distribution([r["total_s"] for r in good], "s"),
        "reset_to_trial_ms": results.distribution(restarts, "ms", scale=1e-3),
        "reset_to_trial_refused": refused,
        "flasher_wall_s": results.distribution([r["flasher_wall_s"] for r in good], "s"),
        "start_to_ready_s": results.distribution([r["start_to_ready_s"] for r in good], "s"),
        "transfer_bytes_per_s": results.distribution(
            [r["report"]["transfer"]["bytes_per_s"] for r in good], "bytes/s"),
        "block_p50_ms": results.distribution([r["report"]["block_ms"]["p50"] for r in good
                                              if r["report"]["block_ms"]["p50"] is not None],
                                             "ms"),
        "erase_us_node": results.distribution(
            [r["node_download"]["erase"]["us"] for r in good if r["node_download"]["erase"]],
            "us"),
        "confirmed_by": {name: sum(1 for r in good if r.get("confirmed_by") == name)
                         for name in ("routine", "node")},
        "raw_total_s": results.raw_samples([r["total_s"] for r in good], "s", decimals=6),
    }


def big_figures(part: dict) -> dict:
    records = part["records"]
    good = [r for r in records if r["success"]]
    steps = step_durations(records, DOWNLOAD_STEPS)
    return {
        "steps_s": distributions(steps),
        "total_s": results.distribution([r["total_s"] for r in good], "s"),
        "transfer_bytes_per_s": results.distribution(
            [r["report"]["transfer"]["bytes_per_s"] for r in good], "bytes/s"),
        "node_transfer_bytes_per_s": results.distribution(
            [r["node"]["download"]["received"] / (r["node"]["download"]["transfer_us"] / 1e6)
             for r in good if r["node"]["download"]["transfer_us"]], "bytes/s"),
        "write_job_mean_ms": results.distribution(
            [r["node"]["jobs"]["write"]["total_ns"] / r["node"]["jobs"]["write"]["run"] / 1e6
             for r in good if r["node"]["jobs"]["write"]["run"]], "ms"),
    }


def summarise(data: dict) -> dict:
    summary: dict = {}
    for name, part in data["campaigns"].items():
        figures = part["figures"]
        counts = part["counts"]
        summary[f"updates_{name}"] = {
            "requested": results.figure(counts["requested"], "updates"),
            "succeeded": results.figure(counts["succeeded"], "updates"),
            "attempted": results.figure(counts["attempted"], "updates"),
            "image_bytes": results.figure(part["records"][0]["image_bytes"]
                                          if part["records"] else None, "bytes"),
            "total": figures["total_s"],
            "erase": figures["steps_s"]["erase"],
            "transfer": figures["steps_s"]["transfer"],
            "check": figures["steps_s"]["check"],
            "wait_for_trial": figures["steps_s"]["wait_for_trial"],
            "reset_to_trial": {**figures["reset_to_trial_ms"], "measured": RESET_TO_TRIAL},
            "reset_to_trial_refused": results.figure(figures["reset_to_trial_refused"],
                                                     "updates"),
            "confirm": figures["steps_s"]["confirm"],
            "transfer_throughput": figures["transfer_bytes_per_s"],
        }
    big = data.get("big_image")
    if big is not None:
        figures = big["figures"]
        summary["big_image"] = {
            "payload_bytes": results.figure(big["payload_bytes"], "bytes"),
            "image_bytes": results.figure(big["image_bytes"], "bytes"),
            "downloads": results.figure(big["counts"]["attempted"], "downloads"),
            "succeeded": results.figure(big["counts"]["succeeded"], "downloads"),
            "erase": figures["steps_s"]["erase"],
            "transfer": figures["steps_s"]["transfer"],
            "check": figures["steps_s"]["check"],
            "total": figures["total_s"],
            "transfer_throughput": figures["transfer_bytes_per_s"],
        }
    return summary


# --------------------------------------------------------------------------------- the plot


def plot(data: dict, path: Path) -> None:
    import matplotlib.pyplot as plt  # noqa: PLC0415

    plotstyle.apply()
    fig, (left, right) = plt.subplots(1, 2, figsize=(10.5, 4.6),
                                      gridspec_kw={"width_ratios": [1.25, 1.0]})
    fig.subplots_adjust(left=0.07, right=0.985, top=0.72, bottom=0.16, wspace=0.3)
    campaigns = data["campaigns"]
    for index, (name, part) in enumerate(campaigns.items()):
        records = part["records"]
        good = [r for r in records if r["success"]]
        bad = [r for r in records if not r["success"]]
        total = part["figures"]["total_s"]
        middle = (f", median {total['p50']:.2f} s" if total["p50"] is not None
                  else f", mean {total['mean']:.2f} s" if total["mean"] is not None else "")
        left.plot([r["number"] for r in good], [r["total_s"] for r in good], marker="o",
                  markersize=3.2, linestyle="none", color=plotstyle.SERIES[index],
                  label=f"{name}: {part['counts']['succeeded']} of {part['counts']['requested']} "
                        f"confirmed{middle}")
        if bad:
            left.plot([r["number"] for r in bad], [r.get("total_s") or 0.0 for r in bad],
                      marker="x", markersize=6, linestyle="none",
                      color=plotstyle.STATUS["critical"], label=f"{name}: failed")
    left.set_xlabel("update number (each from the image the one before confirmed)")
    left.set_ylabel("flasher's total of its steps (s)")
    left.set_title("Updates end to end, one after the other")
    plotstyle.legend_above(left)

    rows, labels = [], []
    groups = (("erase", ("erase",)), ("transfer", ("transfer",)),
              ("ecu_reset and wait_for_trial", ("ecu_reset", "wait_for_trial")),
              ("the rest", None))
    for name, part in campaigns.items():
        if part["counts"]["succeeded"]:
            rows.append(part["figures"]["steps_s"])
            labels.append(f"update, {name}")
    big = data.get("big_image")
    if big is not None and big["counts"]["succeeded"]:
        rows.append(big["figures"]["steps_s"])
        labels.append(f"{big['payload_bytes'] // 1024} KiB download, {big['transport']}")
    y = np.arange(len(rows))
    lefts = np.zeros(len(rows))
    for index, (group, members) in enumerate(groups):
        widths = []
        for steps in rows:
            named = [s for s in steps if members is None
                     and not any(s in m for _, m in groups if m)] if members is None else members
            widths.append(sum((steps[s]["p50"] if steps[s]["p50"] is not None
                               else steps[s]["mean"] or 0.0) for s in named if s in steps))
        right.barh(y, widths, left=lefts, height=0.5, color=plotstyle.SERIES[index], label=group)
        lefts += np.array(widths)
    right.set_yticks(y, labels)
    right.set_xlabel("median of each step (s), stacked")
    right.set_title("Where an update's time goes")
    right.grid(axis="x", color=plotstyle.GRID, linewidth=0.6)
    right.grid(axis="y", visible=False)
    plotstyle.legend_above(right)
    host = "WSL2" if data["provenance"]["host"]["is_wsl"] else "Linux"
    plotstyle.footnote(fig, f"bench/updates.py; FreeRTOS POSIX port through fcn_boot on a {host} "
                            "host, the flasher in Python, 500 kbit/s bus model. Host timing, not "
                            "MCU timing.")
    plotstyle.save(fig, path)


# ------------------------------------------------------------------------------- the bench


def config_of(settings: dict, images: Images | None) -> dict:
    return {
        "bitrate_bit_per_s": BITRATE,
        "updates": {"socketcan": settings["socketcan_updates"], "udp": settings["udp_updates"]},
        "order": "socketcan then udp, each from a factory flash; the updates of a campaign one "
                 "after the other, each from the image the one before confirmed",
        "factory_version": FACTORY_VERSION,
        "update_versions": {transport: f"{major}.<update number>.0"
                            for transport, major in CAMPAIGN_MAJOR.items()},
        "node_image": "the release node (<build root>/release/bin/fcn_node), stripped "
                      "(strip -o), packed with fcn_tools.image",
        "node_payload_bytes": len(images.node_payload) if images else None,
        "node_options": [],
        "trial_window": "the product's (5 s of good health): no --confirm-periods",
        "flasher_options": {"updates": [], "big_image": ["--address", BIG_ADDRESS, "--no-reset"]},
        "flasher_wait_for_trial": {"first_request_and_interval_s": flasher.TRIAL_POLL_S,
                                   "request_timeout_s": flasher.TRIAL_POLL_TIMEOUT_S},
        "big_image": {"downloads": settings["big_downloads"],
                      "payload_bytes": settings["big_payload"],
                      "payload_kind": images.big_kind if images else None,
                      "version": BIG_VERSION, "transport": settings["big_transport"],
                      "data_seed": DATA_SEED,
                      "node": "started directly with --flash-file (no bootloader), one fresh "
                              "flash per download; booted through fcn_boot once afterwards if "
                              "the payload is the node"},
        "flasher_timeout_s": FLASHER_TIMEOUT_S,
    }


def measure(settings: dict, binaries: runner.Binaries, release_node: Path
            ) -> tuple[dict, dict[str, str], Images]:
    workdir = runner.bench_workdir(NAME)
    images = make_images(workdir / "images", release_node, settings["big_payload"])
    reported: dict[str, str] = {}
    data: dict = {"campaigns": {}, "workdir": str(workdir)}
    for transport, count in (("socketcan", settings["socketcan_updates"]),
                             ("udp", settings["udp_updates"])):
        if count == 0:
            continue
        with runner.Session(workdir / transport, transport, binaries=binaries) as session:
            part, ids = campaign(session, images, count, workdir / transport)
        reported.update(ids)
        part["figures"] = campaign_figures(part)
        data["campaigns"][transport] = part
    if settings["big_downloads"]:
        transport = settings["big_transport"]
        with runner.Session(workdir / f"big-{transport}", transport,
                            binaries=binaries) as session:
            part, ids = big_downloads(session, images, settings["big_downloads"],
                                      workdir / f"big-{transport}")
        reported.update(ids)
        part["figures"] = big_figures(part)
        data["big_image"] = part
    return data, reported, images


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    results.add_bench_arguments(parser, quick="one update over UDP and one download of a "
                                              f"{QUICK['big_payload']}-byte data payload")
    parser.add_argument("--socketcan-updates", type=int, default=DEFAULT_SOCKETCAN_UPDATES,
                        help="updates over SocketCAN (default %(default)s)")
    parser.add_argument("--udp-updates", type=int, default=DEFAULT_UDP_UPDATES,
                        help="updates over the UDP bus (default %(default)s)")
    parser.add_argument("--big-downloads", type=int, default=DEFAULT_BIG_DOWNLOADS,
                        help="downloads of the big image (default %(default)s)")
    parser.add_argument("--big-payload", type=int, default=DEFAULT_BIG_PAYLOAD,
                        help="its payload in bytes (default %(default)s)")
    parser.add_argument("--big-transport", choices=runner.TRANSPORTS,
                        default=DEFAULT_BIG_TRANSPORT, help="its bus (default %(default)s)")
    args = parser.parse_args(argv)
    out = results.output_dir(args)
    json_path = out / f"{NAME}.json"
    if args.replot:
        if not json_path.exists():
            print(f"nothing to replot: {json_path} does not exist", file=sys.stderr)
            return 1
        plot(json.loads(json_path.read_text()), json_path.with_suffix(".png"))
        print(f"redrew {json_path.with_suffix('.png')}")
        return 0
    settings = dict(QUICK) if args.quick else {
        "socketcan_updates": args.socketcan_updates, "udp_updates": args.udp_updates,
        "big_downloads": args.big_downloads, "big_payload": args.big_payload,
        "big_transport": args.big_transport}
    if (not 0 <= settings["socketcan_updates"] <= 255 or not 0 <= settings["udp_updates"] <= 255
            or settings["big_downloads"] < 0
            or not 1 <= settings["big_payload"] <= 1024 * 1024 - 256
            or settings["socketcan_updates"] + settings["udp_updates"]
            + settings["big_downloads"] == 0):
        parser.error("0 to 255 updates per transport, a payload of 1 byte to the slot less its "
                     "header, and something to do")
    try:
        load = results.check_load(args.max_load, args.allow_loaded)
        binaries = runner.prepare_binaries(build=not args.no_build)
        # The images are packed from the release node, whatever node FCN_NODE names (an asan
        # node does not fit a slot): its build id is checked as well.
        release_node = paths.build_root() / "release" / "bin" / "fcn_node"
        if not release_node.exists():
            raise FileNotFoundError(f"{release_node} not found: the images are packed from the "
                                    "release node (scripts/build.sh release)")
        checked = {**binaries.as_dict(), "fcn_node (image payload)": release_node}
        results.require_tree_build(checked)
        sha256 = {**binaries.sha256, "fcn_node (image payload)": results.sha256_of(release_node)}
        data, reported, images = measure(settings, binaries, release_node)
        path = results.write_result(NAME, data, summarise(data), config_of(settings, images),
                                    out, version=VERSION, timing_note=TIMING_NOTE, load=load,
                                    binaries=checked, reported_build_ids=reported,
                                    binary_sha256=sha256)
    except results.HostLoaded as error:
        print(error, file=sys.stderr)
        return results.EXIT_HOST_LOADED
    except results.BuildIdMismatch as error:
        print(f"no result written: {error}", file=sys.stderr)
        return results.EXIT_BUILD_MISMATCH
    except KeyboardInterrupt as error:
        return runner.interrupted_status(error)
    except AssertionError as error:      # runner.BenchFailure, the checks of the flash
        print(f"no result written: {error}", file=sys.stderr)
        return 1
    except Exception as error:  # noqa: BLE001 (the tools, the files: say what)
        traceback.print_exc()
        print(f"no result written: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    plot(json.loads(path.read_text()), path.with_suffix(".png"))
    failed = 0
    for name, group in json.loads(path.read_text())["summary"].items():
        total = group["total"]
        attempted = group["attempted" if "attempted" in group else "downloads"]["value"]
        requested = group["requested"]["value"] if "requested" in group else attempted
        failed += requested - group["succeeded"]["value"]
        print(f"{name}: {group['succeeded']['value']} of {requested} succeeded, total median "
              f"{total['p50']} s (mean {total['mean']} s, max {total['max']} s)")
    print(f"wrote {path} and {path.with_suffix('.png')}")
    if failed:
        # A failed (or never attempted) update is a finding, not a measurement to pass over:
        # the result holds it with its report, and the status says so (bench/README.md).
        print(f"{failed} update(s) or download(s) did not succeed: see the result's records",
              file=sys.stderr)
        return EXIT_FINDINGS
    return 0


if __name__ == "__main__":
    sys.exit(main())
