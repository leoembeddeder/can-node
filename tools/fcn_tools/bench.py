# SPDX-License-Identifier: MIT
"""What a test or a bench does to a flash file, to the images in it and to the bus around a node
during an update: the bench of the update tests (tests/system/update_bench.py imports it) and of
the measurements (bench/). It moved here from tests/system/update_bench.py in phase 6 unchanged
but for one thing: where a wait or a missing tool used to call pytest.fail(), it raises
BenchFailure with the same message, and update_bench.py turns that back into pytest.fail(). No
module of fcn_tools imports pytest.

A test makes its flash with the flash tool (fcn_flash factory and stage, with --no-wait: the
tool's waits are not under test there), starts the node through the bootloader (NodeProcess
with a flash file, tests/conftest.py, or fcn_tools.runner outside pytest) and looks at it from
outside: the bootloader's log, the node's log and statistics, the frames on the bus
(BMS_NodeInfo, BMS_BootInfo, BMS_Fault), and the flash afterwards, read twice: by `fcn_flash
info` (the C code: lib/boot's boot_describe(), lib/image, lib/bootimg's id) and by
fcn_tools.flash, a second implementation from the text of docs/update.md. info() fails when the
two disagree on any member, so every reading of a flash is that cross-check as well. What the
bootloader must decide for a flash, and what every flash must satisfy, is the reference model of
fcn_tools.update_model (expected_boot(), check_invariants()), re-exported here.

The helpers further down follow a trial on the bus for the tests of its confirm: the start of
its window in BMS_BootInfo, its own seconds in its log, its CAN controller's state through
simulation control, and two other nodes, a tester that switches the telemetry off and the flood
of test_can_flood.py.

The images (make_images(), the `images` fixture of tests/system/conftest.py) are packed from the
release build of the node, stripped, and from the test payloads of tests/tools: an image of the
asan node does not fit a slot.

The checks here are assert statements, as in the tests, and fail with AssertionError; so the
module refuses to be imported under python -O, which would remove them.
"""
from __future__ import annotations

import json
import math
import random
import re
import shutil
import subprocess
import threading
import time
from pathlib import Path
from typing import Callable

import can

from . import bus as bus_module
from . import e2e
from . import flash as flash_decoder
from . import image as image_format
from . import paths
from .canbits import INTERMISSION_BITS, message_bits
from .decode import DecodedFrame, Decoder, data_id, load_database
from .node import EXIT_OK, NodeProcess
from .tester import DATA_PAYLOAD_SIZE
from .update_model import MAX_ATTEMPTS, Boot, check_invariants, expected_boot, slot_index, states

if not __debug__:  # pragma: no cover - only under python -O
    raise ImportError("fcn_tools.bench checks with assert statements, which python -O removes: "
                      "run it without -O")

__all__ = [
    "ALIVE_LINE", "BENCH_SEED", "BenchFailure", "Boot", "CONFIRM_WAIT_S", "DATA_SEED",
    "FLOOD_BITRATE", "FLOOD_FRAME", "FLOOD_LOAD", "FLOOD_MOST_PASSES", "FLOOD_PASS_S", "Flood",
    "MAX_ATTEMPTS", "NO_SELF_CONFIRM", "ROLLBACK_BIT", "RUN_WAIT_S", "TRIAL_WINDOW_PERIODS",
    "TelemetryOff", "alive_seconds", "check_frames_of", "check_handoff", "check_invariants",
    "decoded", "expected_boot", "factory", "flash_tool", "flip_bit", "info", "listen",
    "make_images", "pack_image", "slot_index", "stage", "states", "stripped_copy",
    "until_confirmed", "wait_for_alive", "wait_for_can_state", "wait_for_confirm_seconds",
    "wait_until_confirmed",
]


class BenchFailure(AssertionError):
    """A wait that ran out or a tool that is missing: the test, or the bench, fails with this
    message. tests/system/update_bench.py turns it into pytest.fail(message, pytrace=False), as
    the helpers called before they moved here."""


#: UPDATE_ROLLBACK, fault 16 (app/fault.h), as a bit of BMS_Fault's masks.
ROLLBACK_BIT = 1 << 16

#: How long a test waits for a trial's confirm on the bus: the confirm comes after 5 s of good
#: health; the rest is room for a slow host. Not a timing tolerance: a node that does not
#: confirm in this time fails the test (the window itself is timed in test_update.py).
CONFIRM_WAIT_S = 30.0

#: A trial's window of good health for a test that leaves the confirm to the tester's routine
#: (--confirm-periods, a test hook of the host's node, app/update.h): 36,000 health periods of
#: 100 ms, an hour, so that the trial cannot confirm itself before the routine: every wait of the
#: test and of the flasher ends long before (RUN_WAIT_S, the flasher's timeouts). Every other
#: test runs with the product's window, 50 periods.
TRIAL_WINDOW_PERIODS = 36_000
NO_SELF_CONFIRM = ("--confirm-periods", str(TRIAL_WINDOW_PERIODS))

#: How long a test waits for a run of the node that ends by itself (--duration-s, a reset). Not
#: a timing tolerance either: it only bounds the wait.
RUN_WAIT_S = 30.0

# The test's random choices (bits to flip, operations to cut, seeds) come from generators seeded
# with this, so that a run repeats exactly.
BENCH_SEED = 20261121


# ------------------------------------------------------------------------------ the flash tool


def flash_tool(*args: str | Path, expect: int = 0) -> dict:
    """Run fcn_flash; its JSON output. The exit status must be `expect`."""
    tool = paths.flash_tool_binary()
    if not tool.exists():
        raise BenchFailure(f"{tool} not found: build it with scripts/build.sh release")
    run = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True,
                         timeout=120, check=False)
    assert run.returncode == expect, f"fcn_flash {args}: {run.returncode}\n{run.stdout}{run.stderr}"
    return json.loads(run.stdout) if run.stdout.strip() else {}


def factory(flash: Path, image: Path) -> dict:
    """A new flash file with `image` in slot A, adopted as the CONFIRMED image."""
    return flash_tool("factory", "--flash-file", flash, "--no-wait", image)


def stage(flash: Path, image: Path, *options: str, expect: int = 0) -> dict:
    """An update with `image`, as phase 5's download will make it: committed as the TRIAL."""
    return flash_tool("stage", "--flash-file", flash, "--no-wait", *options, image, expect=expect)


def info(flash: Path) -> dict:
    """`fcn_flash info`, after checking that fcn_tools.flash reads the same from the file."""
    reported = flash_tool("info", "--flash-file", flash)
    decoded = flash_decoder.describe(flash.read_bytes())
    reported_without_counts = {key: value for key, value in reported.items() if key != "flash"}
    assert decoded == reported_without_counts, (
        "fcn_tools.flash and fcn_flash info disagree:\n"
        f"decoder {json.dumps(decoded, indent=1)}\ntool {json.dumps(reported_without_counts, indent=1)}")
    return reported


def flip_bit(flash: Path, slot: str, where: str, seed: int) -> tuple[int, int]:
    """The bench flips one bit of the image in `slot`, as a bit that changed in the flash would:
    in its 256-byte header (`where` "header") or in its payload ("payload"), at a place drawn
    from `seed`. Returns the file offset and the bit."""
    offset, _ = flash_decoder.SLOTS[slot_index(slot)]
    data = bytearray(flash.read_bytes())
    header = data[offset:offset + 256]
    payload_size = int.from_bytes(header[16:20], "little")   # docs/update.md, the header
    draw = random.Random(seed)
    start, end = (0, 256) if where == "header" else (256, 256 + payload_size)
    at = offset + draw.randrange(start, end)
    bit = 1 << draw.randrange(8)
    data[at] ^= bit
    flash.write_bytes(bytes(data))
    return at, bit


# ---------------------------------------------------------------------------------- the images


#: The seed of the "data" images' payload (its size: fcn_tools.tester.DATA_PAYLOAD_SIZE).
DATA_SEED = 20261122


def stripped_copy(binary: Path, out: Path) -> Path:
    """`binary` without its symbols at `out` (`strip -o`), as a slot holds the node; a plain copy
    on a machine without strip. Returns `out`."""
    strip = shutil.which("strip")
    if strip:
        subprocess.run([strip, "-o", str(out), str(binary)], check=True)
    else:
        shutil.copyfile(binary, out)
    return out


def pack_image(payload: bytes, version: str, target: str = "host") -> bytes:
    """An image file (fcn_tools.image pack): the 256-byte header for `payload`, `version`
    (major.minor.patch) and `target`, and the payload."""
    return image_format.pack(payload, image_format.parse_version(version),
                             image_format.TARGETS[target])


def make_images(out: Path, release_bin: Path | None = None) -> dict[str, Path]:
    """The image files of the update tests in the directory `out`, by name (the `images`
    fixture of tests/system/conftest.py, which says what each is): "node-1.0.0", "node-1.1.0",
    "node-1.4.0" (the release node of `release_bin`, default <build root>/release/bin,
    stripped), "crash" (1.2.0) and "watchdog" (1.3.0), the test payloads of tests/tools, and
    phase 5's "crash-stripped", "data", "data-damaged" and "data-mps2". The binaries must exist
    (the fixture checks that first)."""
    release = release_bin if release_bin is not None else paths.build_root() / "release" / "bin"
    node = release / "fcn_node"
    payloads = {"crash": release / "fcn_payload_crash", "watchdog": release / "fcn_payload_watchdog"}
    stripped = stripped_copy(node, out / "fcn_node.stripped")
    made = {}
    for name, payload, version in (("node-1.0.0", stripped, "1.0.0"),
                                   ("node-1.1.0", stripped, "1.1.0"),
                                   ("node-1.4.0", stripped, "1.4.0"),
                                   ("crash", payloads["crash"], "1.2.0"),
                                   ("watchdog", payloads["watchdog"], "1.3.0")):
        path = out / f"{name}.img"
        path.write_bytes(pack_image(payload.read_bytes(), version))
        made[name] = path
    # Phase 5's downloads (fcn_tools.tester), where a short transfer saves time:
    #   "crash-stripped" (1.2.0), fcn_payload_crash stripped, the smallest image that runs;
    #   "data" (1.5.0), DATA_PAYLOAD_SIZE bytes drawn from a seeded generator: an image that
    #   validates but is no program, for downloads the bootloader never starts;
    #   "data-damaged", the same with one bit of its payload flipped after the header was made,
    #   as a bit that changed in transit: its digest no longer matches;
    #   "data-mps2", the same payload packed for the other target (mps2-an385).
    crash = stripped_copy(payloads["crash"], out / "fcn_payload_crash.stripped")
    made["crash-stripped"] = out / "crash-stripped.img"
    made["crash-stripped"].write_bytes(pack_image(crash.read_bytes(), "1.2.0"))
    payload = random.Random(DATA_SEED).randbytes(DATA_PAYLOAD_SIZE)
    data = pack_image(payload, "1.5.0")
    damaged = bytearray(data)
    damaged[image_format.HEADER_SIZE + DATA_PAYLOAD_SIZE // 2] ^= 0x10
    for name, content in (("data", data), ("data-damaged", bytes(damaged)),
                          ("data-mps2", pack_image(payload, "1.5.0", "mps2-an385"))):
        made[name] = out / f"{name}.img"
        made[name].write_bytes(content)
    return made


# -------------------------------------------------------------------------------- the frames


def decoded(frames, name: str) -> list[DecodedFrame]:
    """The frames of message `name` among `frames`, decoded."""
    decoder = Decoder()
    out = []
    for message in list(frames):
        frame = decoder.decode(message)
        if frame is not None and frame.name == name:
            out.append(frame)
    return out


def wait_until_confirmed(reader: bus_module.BusReader, node: NodeProcess) -> None:
    """Wait for a BMS_BootInfo that says CONFIRMED, while the node runs."""
    deadline = time.monotonic() + CONFIRM_WAIT_S
    while time.monotonic() < deadline and node.running:
        if any(frame.signals["ConfirmState"] == "CONFIRMED"
               for frame in decoded(reader.frames, "BMS_BootInfo")):
            return
        time.sleep(0.1)
    raise BenchFailure(f"no BMS_BootInfo with ConfirmState CONFIRMED within {CONFIRM_WAIT_S} s\n"
                       f"--- log\n{node.log()}\n--- stderr\n{node.stderr()}")


def listen(bus_spec: str, start: Callable[[], NodeProcess],
           run: Callable[[NodeProcess, bus_module.BusReader], object] | None = None
           ) -> tuple[NodeProcess, list[can.Message]]:
    """Open the bus, start a node with `start()` and return it with every frame it sent.
    `run(node, reader)` waits for its end, or does what the test needs meanwhile; without one
    the node must end by itself in order (status 0: a run of --duration-s)."""
    bus = bus_module.open_bus(bus_spec)
    try:
        with bus_module.BusReader(bus) as reader:
            node = start()
            if run is None:
                status = node.wait(timeout=RUN_WAIT_S)
                assert status == EXIT_OK, f"{node.name} ended with {status}\n{node.stderr()}"
            else:
                run(node, reader)
    finally:
        bus.shutdown()
    return node, reader.frames


def until_confirmed(node: NodeProcess, reader: bus_module.BusReader) -> None:
    """For listen(): wait until the trial says CONFIRMED on the bus, then stop it in order."""
    wait_until_confirmed(reader, node)
    assert node.stop() == EXIT_OK


def check_frames_of(boot: Boot, frames: list[can.Message], version: str,
                    reset_cause: str | None = None) -> None:
    """The frames of a run started as `boot`: BMS_NodeInfo names the slot (and the reset cause
    of the start, if given), BMS_BootInfo the mode, the attempt, the bootloader's event and the
    image's `version` (major.minor.patch)."""
    infos = decoded(frames, "BMS_NodeInfo")
    boots = decoded(frames, "BMS_BootInfo")
    assert infos and boots, f"{len(infos)} BMS_NodeInfo, {len(boots)} BMS_BootInfo"
    for frame in infos:
        assert frame.signals["ActiveSlot"] == boot.slot, frame
        if reset_cause is not None:
            assert frame.signals["ResetCause"] == reset_cause, frame
    major, minor, patch = (int(part) for part in version.split("."))
    for frame in boots:
        assert frame.signals["BootMode"] == boot.mode.upper(), frame
        assert frame.raw["BootAttempt"] == boot.attempt, frame
        assert frame.signals["BootEvent"] == boot.event, frame
        assert (frame.raw["ImageVersionMajor"], frame.raw["ImageVersionMinor"],
                frame.raw["ImageVersionPatch"]) == (major, minor, patch), frame


def check_handoff(stats: dict, boot: Boot) -> None:
    """The hand-off the node took, in its statistics, is `boot`, and no write of the select
    failed on the way (none fails in these tests: a cut ends the run instead, and a select whose
    write fails starts the confirmed image as well, so that only the hand-off tells)."""
    handoff = stats["handoff"]
    assert handoff["verdict"] == "taken", handoff
    assert (handoff["slot"], handoff["mode"], handoff["attempt"], handoff["event"]) == (
        boot.slot, boot.mode, boot.attempt, boot.event), handoff
    assert handoff["rolled_back"] == (boot.event in ("ROLLBACK", "REJECT", "FALLBACK"))
    assert handoff["select_write_error"] == 0, handoff


# ------------------------------------------------------------------ the bus during a trial


#: A line of the node's log that ends a second of its uptime (app.c, log_alive()): the seconds
#: its health task has counted, ten health periods each.
ALIVE_LINE = re.compile(r"\balive: (\d+) s, ")


def alive_seconds(node: NodeProcess) -> int:
    """The node's uptime in its own seconds, as the last such line of its log says; -1 before
    the first one."""
    found = ALIVE_LINE.findall(node.log())
    return int(found[-1]) if found else -1


def wait_for_alive(node: NodeProcess, seconds: int) -> None:
    """Wait until the node's log says that it has run `seconds` of its own seconds, while it
    runs. The node's count decides; RUN_WAIT_S only bounds the wait."""
    deadline = time.monotonic() + RUN_WAIT_S
    while alive_seconds(node) < seconds:
        if not node.running or time.monotonic() >= deadline:
            raise BenchFailure(f"the node's log did not reach {seconds} s of its uptime within "
                               f"{RUN_WAIT_S} s (its last line says {alive_seconds(node)} s)\n"
                               f"--- stderr\n{node.stderr()}")
        time.sleep(0.1)


def wait_for_confirm_seconds(reader: bus_module.BusReader, node: NodeProcess,
                             seconds: int) -> None:
    """Wait for a BMS_BootInfo with at least `seconds` of good health counted (ten healthy
    periods in a row each): the trial's window has begun. RUN_WAIT_S only bounds the wait. A
    test process that fell behind may find the trial confirming or confirmed by then (its
    seconds are 5 from the request on); the caller decides that from the node's counts."""
    deadline = time.monotonic() + RUN_WAIT_S
    while time.monotonic() < deadline and node.running:
        for frame in decoded(reader.frames, "BMS_BootInfo"):
            if frame.raw["ConfirmSeconds"] >= seconds:
                return
        time.sleep(0.05)
    raise BenchFailure(f"no BMS_BootInfo with ConfirmSeconds {seconds} within {RUN_WAIT_S} s\n"
                       f"--- log\n{node.log()}\n--- stderr\n{node.stderr()}")


def wait_for_can_state(node: NodeProcess, state: str) -> dict:
    """Wait until simulation control reports the node's CAN controller in `state` (active,
    passive or busoff, SimControl.can_status()) and return that report. RUN_WAIT_S only bounds
    the wait."""
    deadline = time.monotonic() + RUN_WAIT_S
    while True:
        with node.sim() as sim:
            status = sim.can_status()
        if status["state"] == state:
            return status
        if not node.running or time.monotonic() >= deadline:
            raise BenchFailure(f"the controller did not become {state} within {RUN_WAIT_S} s: "
                               f"{status}")
        time.sleep(0.05)


_DB = load_database()
_COMMAND = _DB.get_message_by_name("TESTER_Command")


class TelemetryOff:
    """A tester that switches the node's telemetry off: TESTER_Command with CmdTelemetryOn 0,
    protected with fcn_tools.e2e, every 20 ms from a thread and a bus of its own, until stop().
    The node acts from the third frame on and keeps the switch off afterwards, also when the
    commands stop (app/command.h)."""

    def __init__(self, spec: str) -> None:
        self.sent = 0
        self._bus = bus_module.open_bus(spec)
        self._sender = e2e.Sender(e2e.Config(data_id=data_id(_COMMAND), data_length=64))
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="TelemetryOff", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            payload = _DB.encode_message(_COMMAND.name, {"E2E_Crc": 0, "E2E_Counter": 0,
                                                         "CmdTelemetryOn": 0,
                                                         "CmdClearFaultsSeq": 0})
            self._bus.send(can.Message(arbitration_id=_COMMAND.frame_id,
                                       data=self._sender.protect(payload), is_extended_id=False))
            self.sent += 1
            self._stop.wait(0.02)

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=5)
        self._bus.shutdown()


#: The flood of test_can_flood.py, the heaviest load the system tests put on a bus: frames of
#: another node at 0x700, below every periodic frame of the node in priority, at FLOOD_LOAD
#: times what a bus of FLOOD_BITRATE can carry of them.
FLOOD_BITRATE = 125_000
FLOOD_LOAD = 1.2
FLOOD_FRAME = can.Message(arbitration_id=0x700, data=bytes([0x55] * 8), is_extended_id=False)

#: A Flood's pass: it sleeps this long between two batches of frames, and a batch holds no more
#: frames than FLOOD_MOST_PASSES of them ask for.
FLOOD_PASS_S = 0.001
FLOOD_MOST_PASSES = 5


class Flood:
    """Sends `frame` at `load` times what a bus of `bitrate` can carry of it (by default
    FLOOD_FRAME, FLOOD_LOAD and FLOOD_BITRATE), from a thread and a bus of its own, until stop().
    It follows the clock: every FLOOD_PASS_S it sends the frames that the rate and the time since
    its start ask for. A sender that the host delays does not make up for the time it lost: it
    sends at most FLOOD_MOST_PASSES passes' worth of frames at once and gives up the rest
    (`given_up`), so the flood never reaches the node as a burst. (A burst would also come out of
    the bus model as one: the host transport stamps every frame of one read pass with the same
    time, and a burst on top of the frames already waiting overflows the model's wire queue of 64
    frames, lib/can/can_ctrl.h. test_can_flood.py's flood, above the bus's capacity, catches up
    instead: it counts what the model drops.)"""

    def __init__(self, spec: str, frame: can.Message = FLOOD_FRAME, load: float = FLOOD_LOAD,
                 bitrate: int = FLOOD_BITRATE) -> None:
        self.frame = frame
        self.rate = load * bitrate / (message_bits(frame).frame_bits + INTERMISSION_BITS)
        self.sent = 0
        self.given_up = 0
        self._bus = bus_module.open_bus(spec)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="Flood", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        most = math.ceil(self.rate * FLOOD_PASS_S * FLOOD_MOST_PASSES)
        start = time.monotonic()
        while not self._stop.is_set():
            due = int((time.monotonic() - start) * self.rate) - self.given_up
            if due - self.sent > most:
                self.given_up += due - self.sent - most
                due = self.sent + most
            while self.sent < due:
                self._bus.send(self.frame)
                self.sent += 1
            time.sleep(FLOOD_PASS_S)

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=5)
        self._bus.shutdown()
