# SPDX-License-Identifier: MIT
"""The C and the Python implementation of the length of a CAN frame, asked the same questions.

lib/can/can_bits.c is what the firmware works out bus load and timing with. fcn_tools.canbits
answers the same question a second time and was written from the Bosch specification without
a look at the C code. Each has its own tests against frames worked out by hand
(tests/unit/test_can_bits.c, tests/python/test_canbits.py). This file makes the two meet: more
than 100 000 frames go through both, and every answer has to be the same in each of its three
fields, frame_bits, stuff_bits and crc15.

The C side is the release build of tests/tools/can_bits_cli.c, which puts can_bits_exact()
behind a pipe. All frames of this file go to ONE run of it, one line per frame, and the
answers come back in the order of the questions.

The frames, and what each family is there to expose:

  random                    21 600 seeded frames, 600 for each combination of format, kind of
                            frame and DLC. Anything systematic: the order and the width of the
                            fields, the bit order within them, the CRC, the data field that a
                            remote frame does not have.
  one level                 Identifier and data all dominant or all recessive: the longest
                            runs there are, through every field and across every boundary.
  quartets                  0000 1111 0000 ... in identifier and data, at every phase: the
                            densest stuffing there is, because a stuff bit is the first bit of
                            the next run of five. An implementation that starts counting behind
                            the stuff bit finds nothing to stuff in four equal bits, and frames
                            of one level cannot tell it from a correct one.
  alternating               0101 ...: nothing to stuff in identifier and data. What is stuffed
                            all the same comes from the fixed bits and from the CRC.
  walking identifier bit    One identifier bit unlike all the others, at each of the 11 and 29
                            places: a bit sent at the wrong place changes the CRC.
  walking data bit          The same for every bit of the data field, for every DLC.
  base frames without data  All 20 480 of them: each identifier as a data frame with DLC 0 and
                            as a remote frame with each DLC. Every way in which arbitration and
                            control field can meet the CRC.
  two data bytes            All 65 536 values of the data field of one frame. The CRC is a
                            one-to-one function of the last 15 bits it covers, so every one of
                            the 32 768 CRC sequences is sent exactly twice: all that begin or
                            end with a run of equal bits, the two that are one single run, and
                            every case of a stuff bit behind the last bit of the CRC sequence.

Agreement alone would be worth little if both were wrong together, so three frames whose
answers are known from elsewhere go through the same pipe, and so do frames that cannot exist,
which both sides have to refuse.

What the pipe does not reach: can_bits_cli answers with can_bits_exact() only. The C functions
can_bits_unstuffed() and can_bits_worst_case() are not behind it, so this file cannot call
them. It checks what it can see of them from here: that the C side's exact answers are built
on the unstuffed length that Python's formula gives for every format, kind and DLC, that this
formula and Python's worst case are the closed forms of lib/can/can_bits.h, and that no frame
of the sample is longer than the worst case on either side. The C functions themselves are
held to the same numbers by tests/unit/test_can_bits.c.
"""
from __future__ import annotations

import random
import re
import subprocess
from collections import Counter
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

import pytest

from fcn_tools import canbits, paths
from fcn_tools.canbits import FrameBits

CLI = "can_bits_cli"
CLI_TIMEOUT_S = 60

FIELDS = ("frame_bits", "stuff_bits", "crc15")

# Every combination of format, kind of frame and DLC, as (extended, remote, dlc): the order
# of the arguments of can_bits_unstuffed() and can_bits_worst_case().
CASES = [(extended, remote, dlc)
         for extended in (False, True) for remote in (False, True) for dlc in range(9)]

# ------------------------------------------------------------------------------------------
# Frames
# ------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Frame:
    """A frame as both implementations are asked about it."""

    identifier: int
    extended: bool
    remote: bool
    dlc: int
    data: bytes  # the data field: `dlc` bytes in a data frame, nothing in a remote frame

    @property
    def case(self) -> tuple[bool, bool, int]:
        return (self.extended, self.remote, self.dlc)

    def line(self) -> str:
        """The frame as can_bits_cli reads it; tests/tools/can_bits_cli.c describes the form."""
        fields = (f"{self.identifier:X} {'E' if self.extended else 'S'} "
                  f"{'R' if self.remote else 'D'} {self.dlc}")
        return f"{fields} {self.data.hex().upper()}" if self.data else fields

    def __str__(self) -> str:
        data = f"data {self.data.hex(' ').upper()}" if self.data else "no data field"
        return (f"{'extended' if self.extended else 'base'} format "
                f"{'remote' if self.remote else 'data'} frame, identifier "
                f"0x{self.identifier:0{8 if self.extended else 3}X}, DLC {self.dlc}, {data}")


def data_frame(identifier: int, data: bytes = b"", extended: bool = False) -> Frame:
    return Frame(identifier, extended, remote=False, dlc=len(data), data=bytes(data))


def remote_frame(identifier: int, dlc: int, extended: bool = False) -> Frame:
    return Frame(identifier, extended, remote=True, dlc=dlc, data=b"")


def identifier_bits(extended: bool) -> int:
    return 29 if extended else 11


def ones(width: int) -> int:
    return (1 << width) - 1


def as_bytes(bits: int, count: int) -> bytes:
    """A number of 8 * count bits as `count` data bytes. The byte that is sent first holds the
    top bits, so that the number reads like the data field on the wire."""
    return bits.to_bytes(count, "big")


def alternating(width: int, first: int) -> int:
    """`width` bits that change level at every bit, the one sent first being `first`."""
    return int("".join(str((first + place) % 2) for place in range(width)), 2)


def quartets(width: int, phase: int) -> int:
    """`width` bits cut out of 0000 1111 0000 1111 ..., beginning `phase` bits (0 to 7) into
    it. For 11 bits, phase 0 is 0000 1111 000: identifier 0x078."""
    return int("".join(str(((place + phase) // 4) % 2) for place in range(width)), 2)


# ------------------------------------------------------------------------------------------
# The sample
# ------------------------------------------------------------------------------------------

RANDOM_SEED = 0x1B15C0DE
RANDOM_FRAMES_PER_CASE = 600  # times the 36 cases: 21 600 frames


def runs(rng: random.Random, width: int) -> int:
    """`width` bits in runs of 1 to 9 equal bits. Bits drawn one by one hold five equal ones in
    a row too rarely to say much about stuffing; these hold them at every offset, not only at
    the byte boundaries a list of chosen bytes would give."""
    bits = 0
    level = rng.getrandbits(1)
    filled = 0
    while filled < width:
        length = min(1 + rng.randrange(9), width - filled)
        bits = (bits << length) | (ones(length) if level else 0)
        level ^= 1
        filled += length
    return bits


def random_field(rng: random.Random, width: int) -> int:
    """Half of the fields are runs, half are uniformly random."""
    return runs(rng, width) if rng.getrandbits(1) else rng.getrandbits(width)


def random_frames() -> list[Frame]:
    rng = random.Random(RANDOM_SEED)
    frames = []
    for extended, remote, dlc in CASES:
        for _ in range(RANDOM_FRAMES_PER_CASE):
            identifier = random_field(rng, identifier_bits(extended))
            if remote:
                frames.append(remote_frame(identifier, dlc, extended))
            else:
                data = as_bytes(random_field(rng, 8 * dlc), dlc)
                frames.append(data_frame(identifier, data, extended))
    return frames


def frames_of_one_level() -> list[Frame]:
    frames = []
    for extended in (False, True):
        for identifier in (0, ones(identifier_bits(extended))):
            for dlc in range(9):
                frames.append(remote_frame(identifier, dlc, extended))
                for byte in (0x00, 0xFF) if dlc > 0 else (0x00,):
                    frames.append(data_frame(identifier, bytes([byte] * dlc), extended))
    return frames


def frames_of_quartets() -> list[Frame]:
    """Identifier and data from the quartet pattern, each at all of its eight phases. In the
    extended format SRR and IDE stand between the two parts of the identifier, so the parts
    get their phases independently of each other."""
    base = [quartets(11, phase) for phase in range(8)]
    extended = [(quartets(11, high) << 18) | quartets(18, low)
                for high in range(8) for low in range(8)]
    frames = []
    for is_extended, identifiers in ((False, base), (True, extended)):
        for identifier in identifiers:
            frames.append(data_frame(identifier, b"", is_extended))
            frames += [remote_frame(identifier, dlc, is_extended) for dlc in range(9)]
            frames += [data_frame(identifier, as_bytes(quartets(8 * dlc, phase), dlc), is_extended)
                       for dlc in range(1, 9) for phase in range(8)]
    return frames


def alternating_frames() -> list[Frame]:
    frames = []
    for extended in (False, True):
        for first in (0, 1):
            identifier = alternating(identifier_bits(extended), first)
            frames.append(data_frame(identifier, b"", extended))
            frames += [remote_frame(identifier, dlc, extended) for dlc in range(9)]
            frames += [data_frame(identifier, bytes([byte] * dlc), extended)
                       for dlc in range(1, 9) for byte in (0x55, 0xAA)]
    return frames


def frames_with_a_walking_identifier_bit() -> list[Frame]:
    """One recessive bit in a dominant identifier and one dominant bit in a recessive one, at
    every place; each as a data frame without and with data and as a remote frame."""
    frames = []
    for extended in (False, True):
        width = identifier_bits(extended)
        for place in range(width):
            for identifier in (1 << place, ones(width) ^ (1 << place)):
                frames += [data_frame(identifier, b"", extended),
                           data_frame(identifier, bytes([0x55] * 8), extended),
                           remote_frame(identifier, 8, extended)]
    return frames


def frames_with_a_walking_data_bit() -> list[Frame]:
    """One recessive bit in a dominant data field and one dominant bit in a recessive one, at
    every place of every length of data field. The identifier alternates and adds no run."""
    frames = []
    for extended in (False, True):
        identifier = alternating(identifier_bits(extended), first=1)
        for dlc in range(1, 9):
            width = 8 * dlc
            for place in range(width):
                for bits in (1 << place, ones(width) ^ (1 << place)):
                    frames.append(data_frame(identifier, as_bytes(bits, dlc), extended))
    return frames


def base_frames_without_data() -> list[Frame]:
    frames = []
    for identifier in range(1 << 11):
        frames.append(data_frame(identifier))
        frames += [remote_frame(identifier, dlc) for dlc in range(9)]
    return frames


def frames_with_every_two_data_bytes() -> list[Frame]:
    identifier = alternating(11, first=1)
    return [data_frame(identifier, as_bytes(value, 2)) for value in range(1 << 16)]


FAMILIES: dict[str, Callable[[], list[Frame]]] = {
    "random": random_frames,
    "one level": frames_of_one_level,
    "quartets": frames_of_quartets,
    "alternating": alternating_frames,
    "walking identifier bit": frames_with_a_walking_identifier_bit,
    "walking data bit": frames_with_a_walking_data_bit,
    "base frames without data": base_frames_without_data,
    "two data bytes": frames_with_every_two_data_bytes,
}

# Frames whose answers do not come from either implementation.
#
# The first two are logic-analyser captures from the test suite of the sigrok CAN decoder, as
# quoted in both implementations' own tests.
#
# The third was traced for this file through the register that section 3.2.1 of the
# specification gives for the CRC (CRCNXT = NXTBIT EXOR CRC_RG(14); shift left; if CRCNXT then
# EXOR 4599h). Base format, identifier 0x009, no data:
#
#   SOF ID          RTR IDE r0 DLC
#   0   00000001001 0   0   0  0000
#
#   bit            CRC_RG after it
#   SOF .. ID-22   0000h   eight dominant bits leave an empty register empty
#   ID-21 = 1      4599h   CRCNXT = 1: 0000h shifted, EXOR 4599h
#   ID-20 = 0      4EABh   bit 14 was 1, CRCNXT = 1: 0B32h EXOR 4599h
#   ID-19 = 0      58CFh   bit 14 was 1, CRCNXT = 1: 1D56h EXOR 4599h
#   ID-18 = 1      319Eh   bit 14 was 1, CRCNXT = 0: shifted only
#   RTR   = 0      633Ch   shifted only
#   IDE   = 0      03E1h   bit 14 was 1, CRCNXT = 1: 4678h EXOR 4599h
#   r0, DLC3..0    07C2h, 0F84h, 1F08h, 3E10h, 7C20h   shifted only
#
#   CRC sequence 7C20h = 111110000100000. With the stuff bits in brackets:
#
#   00000[1]000 1 00 1 00000[1]00 11111[0]0000[1]1 00000[1]
#
# 34 bits and 5 stuff bits, then the 10 bits from CRC delimiter to end of frame: 49. The last
# stuff bit stands behind the last bit of the CRC sequence. Section 6: the CRC sequence is
# coded by bit stuffing, and "whenever a transmitter detects five consecutive bits of
# identical value in the bit stream to be transmitted it automatically inserts a complementary
# bit". The five bits are all sent in a stuffed field, so the bit is inserted; that the field
# behind them, the CRC delimiter, is of fixed form changes nothing about them.
KNOWN_ANSWERS = [
    (data_frame(0x222, bytes.fromhex("0011223344")), FrameBits(87, 3, 0x66DA)),
    (data_frame(0x11223344, bytes.fromhex("00112233445566"), extended=True),
     FrameBits(123, 3, 0x0D30)),
    (data_frame(0x009), FrameBits(49, 5, 0x7C20)),
]

# What cannot be a frame, each next to the nearest thing that can. A DLC above 8 is tried on
# remote frames only: can_bits_cli wants as many data bytes as the DLC says and has room for
# eight, so a data frame with such a DLC is turned away by its parser and never reaches
# can_bits_exact().
NO_FRAMES = [
    data_frame(0x800),                      # 12 bits in the base format
    data_frame(0x20000000, extended=True),  # 30 bits in the extended format
    data_frame(0xFFFFFFFF, extended=True),
    remote_frame(0x123, 9),
    remote_frame(0x123, 15),
    remote_frame(0x12345678, 9, extended=True),
]
NEAREST_FRAMES = [
    data_frame(0x7FF),
    data_frame(0x1FFFFFFF, extended=True),
    remote_frame(0x123, 8),
    remote_frame(0x12345678, 8, extended=True),
]

# ------------------------------------------------------------------------------------------
# The one run of can_bits_cli
# ------------------------------------------------------------------------------------------

ANSWER = re.compile(r"(\d{1,5}) (\d{1,3}) ([0-9A-F]{4})")


def require_cli() -> Path:
    """The release build of can_bits_cli. Without it nothing here can be compared, and a test
    that then skipped would report a comparison that never took place: it fails instead."""
    binary = paths.tool_binary(CLI)
    if not binary.is_file():
        pytest.fail(f"{binary} not found: build it with scripts/build.sh release, with "
                    f"FCN_BUILD_ROOT set as for this test run (build root: {paths.build_root()})",
                    pytrace=False)
    return binary


def parse_answer(text: str) -> FrameBits | None:
    """One line of can_bits_cli's output: frame_bits, stuff_bits and the CRC in four upper-case
    hexadecimal digits, or "invalid", which is None here. Anything else raises ValueError: a
    parser that made the best of it would have Python compared with a guess."""
    if text == "invalid":
        return None
    match = ANSWER.fullmatch(text)
    if match is None:
        raise ValueError(f"not an answer of {CLI}: {text!r}")
    return FrameBits(frame_bits=int(match[1]), stuff_bits=int(match[2]), crc15=int(match[3], 16))


def ask_can_bits_cli(binary: Path, frames: Sequence[Frame]) -> list[FrameBits | None]:
    """The answers of one run of can_bits_cli to all `frames`, in their order. A run that
    fails, or that does not answer every line with exactly one line, is not compared at all:
    answers that have slipped against their questions would be compared with the wrong frames.
    """
    lines = [frame.line() for frame in frames]
    questions = "".join(f"{line}\n" for line in lines).encode("ascii")
    try:
        run = subprocess.run([str(binary)], input=questions, capture_output=True,
                             timeout=CLI_TIMEOUT_S, check=False)
    except subprocess.TimeoutExpired:
        pytest.fail(f"{binary} was not done with {len(lines)} lines after {CLI_TIMEOUT_S} s",
                    pytrace=False)
    if run.returncode != 0 or run.stderr:
        pytest.fail(f"{binary} ended with exit status {run.returncode} after {len(lines)} "
                    f"lines; on stderr: {run.stderr[:300]!r}", pytrace=False)

    answers = run.stdout.decode("ascii", errors="replace").split("\n")
    # Every answer ends with a newline, so a complete output leaves one empty string behind.
    if answers.pop() != "" or len(answers) != len(lines):
        pytest.fail(f"{binary} gave {len(answers)} complete answers to {len(lines)} lines",
                    pytrace=False)

    parsed = []
    for number, (line, answer) in enumerate(zip(lines, answers), start=1):
        try:
            parsed.append(parse_answer(answer))
        except ValueError:
            pytest.fail(f"{binary} answered line {number}, {line!r}, with {answer!r}",
                        pytrace=False)
    return parsed


def ask_canbits(frame: Frame) -> FrameBits | None:
    """Python's answer; None if it refuses the frame."""
    try:
        return canbits.frame_bits(frame.identifier, frame.data, frame.extended, frame.remote,
                                  frame.dlc)
    except ValueError:
        return None


# ------------------------------------------------------------------------------------------
# Comparing
# ------------------------------------------------------------------------------------------

SHOWN = 10  # frames written out in the report of a disagreement


@dataclass(frozen=True)
class Compared:
    """One frame and what each side said about it. None: that side refused the frame."""

    frame: Frame
    c: FrameBits | None
    python: FrameBits | None

    def same_answer(self) -> bool:
        """Both sides took the frame and gave the same three numbers."""
        return self.c is not None and self.c == self.python

    def differing_fields(self) -> list[str]:
        if self.c is None or self.python is None:
            return []
        return [name for name in FIELDS if getattr(self.c, name) != getattr(self.python, name)]


def describe(answer: FrameBits | None) -> str:
    if answer is None:
        return "refuses the frame"
    return (f"frame_bits {answer.frame_bits}, stuff_bits {answer.stuff_bits}, "
            f"crc15 0x{answer.crc15:04X}")


def disagreement(failing: Sequence[Compared], total: int, what: str) -> str:
    """The message for a failed comparison: how many frames, in which fields, and the first
    few frames with both answers and with the line that reproduces the C side's."""
    counts = Counter(name for item in failing for name in item.differing_fields())
    refused = sum(1 for item in failing if item.c is None or item.python is None)
    summary = [f"{name} in {counts[name]}" for name in FIELDS if counts[name]]
    if refused:
        summary.append(f"{refused} refused by at least one side")
    text = [
        f"lib/can/can_bits.c (behind {CLI}) and fcn_tools.canbits do not give the same answer "
        f"for {len(failing)} of the {total} frames of '{what}'.",
        f"They differ in: {', '.join(summary)}. The first {min(len(failing), SHOWN)}:",
    ]
    for item in failing[:SHOWN]:
        if item.c is None and item.python is None:
            differ = "nothing, but both refuse a frame that was meant to be valid"
        elif item.c is None or item.python is None:
            differ = "whether this is a frame at all"
        else:
            differ = ", ".join(item.differing_fields())
        text += [
            "",
            f"  {item.frame}",
            f"    asked of {CLI} as:  {item.frame.line()}",
            f"    C       {describe(item.c)}",
            f"    Python  {describe(item.python)}",
            f"    differ in: {differ}",
        ]
    return "\n".join(text)


def require_agreement(compared: Sequence[Compared], what: str) -> None:
    """Fails unless both sides took every frame and gave the same answer in every field."""
    if not compared:
        pytest.fail(f"'{what}' holds no frame: nothing was compared", pytrace=False)
    failing = [item for item in compared if not item.same_answer()]
    if failing:
        pytest.fail(disagreement(failing, len(compared), what), pytrace=False)


@pytest.fixture(scope="module")
def compared() -> dict[str, list[Compared]]:
    """Every frame of this file with both answers, by family. This is where the one run of
    can_bits_cli takes place; the tests only look at what it returned."""
    groups = {name: build() for name, build in FAMILIES.items()}
    groups["known answers"] = [frame for frame, _ in KNOWN_ANSWERS]
    groups["no frames"] = NO_FRAMES
    groups["nearest frames"] = NEAREST_FRAMES

    frames = [frame for group in groups.values() for frame in group]
    answers = iter(ask_can_bits_cli(require_cli(), frames))
    return {name: [Compared(frame, next(answers), ask_canbits(frame)) for frame in group]
            for name, group in groups.items()}


def sample(compared: dict[str, list[Compared]]) -> list[Compared]:
    """The frames of all families together."""
    return [item for name in FAMILIES for item in compared[name]]


def stuffed_span(extended: bool, remote: bool, dlc: int) -> int:
    """s of lib/can/can_bits.h: the bits from start of frame to the end of the CRC sequence,
    34 + 8n in the base format and 54 + 8n in the extended format, n = 0 for a remote frame."""
    return (54 if extended else 34) + 8 * (0 if remote else dlc)


TAIL_BITS = 10  # CRC delimiter, ACK slot, ACK delimiter, 7 bits end of frame: never stuffed

# ------------------------------------------------------------------------------------------
# The harness itself: a comparison that cannot fail proves nothing
# ------------------------------------------------------------------------------------------


def test_the_tool_is_looked_for_in_the_build_tree_of_its_preset(monkeypatch, tmp_path):
    monkeypatch.setenv("FCN_BUILD_ROOT", str(tmp_path))
    assert paths.tool_binary(CLI) == tmp_path / "release" / "bin" / CLI
    assert paths.tool_binary(CLI, preset="asan") == tmp_path / "asan" / "bin" / CLI


def test_a_missing_binary_fails_the_test_and_says_how_to_build_it(monkeypatch, tmp_path):
    monkeypatch.setenv("FCN_BUILD_ROOT", str(tmp_path))
    with pytest.raises(pytest.fail.Exception) as failure:
        require_cli()
    assert "scripts/build.sh release" in str(failure.value)
    assert str(tmp_path / "release" / "bin" / CLI) in str(failure.value)


def test_a_frame_is_written_as_the_line_that_can_bits_cli_reads():
    # The first line is the example in tests/tools/can_bits_cli.c.
    assert data_frame(0x222, bytes.fromhex("0011223344")).line() == "222 S D 5 0011223344"
    assert data_frame(0x1ABCDEF0, bytes.fromhex("0a"), extended=True).line() == "1ABCDEF0 E D 1 0A"
    assert data_frame(0x009).line() == "9 S D 0"
    assert remote_frame(0x7FF, 8).line() == "7FF S R 8"
    assert remote_frame(0, 0, extended=True).line() == "0 E R 0"


def test_an_answer_is_read_as_three_numbers_or_as_a_refusal():
    assert parse_answer("87 3 66DA") == FrameBits(frame_bits=87, stuff_bits=3, crc15=0x66DA)
    assert parse_answer("157 29 0000") == FrameBits(157, 29, 0)
    assert parse_answer("invalid") is None


@pytest.mark.parametrize("text", [
    "", " ", "87 3", "87 3 66DA ", " 87 3 66DA", "87 3 66da", "87 3 6DA", "87 3 066DA",
    "87  3 66DA", "87 3 66DA 0", "-87 3 66DA", "0x57 3 66DA", "Invalid", "invalid ",
])
def test_anything_else_is_not_taken_for_an_answer(text):
    with pytest.raises(ValueError):
        parse_answer(text)


def stand_in(directory: Path, script: str) -> Path:
    """A program that reads its input like can_bits_cli and then does what `script` says."""
    program = directory / "stand_in_for_can_bits_cli"
    program.write_text(f"#!/bin/sh\ncat >/dev/null\n{script}\n")
    program.chmod(0o755)
    return program


def test_one_answer_per_line_is_taken_in_the_order_of_the_lines(tmp_path):
    program = stand_in(tmp_path, "echo '87 3 66DA'; echo invalid; echo '49 5 7C20'")
    frames = [data_frame(0x222, bytes.fromhex("0011223344")), data_frame(0x800), data_frame(9)]
    assert ask_can_bits_cli(program, frames) == [FrameBits(87, 3, 0x66DA), None,
                                                 FrameBits(49, 5, 0x7C20)]


@pytest.mark.parametrize("script, complaint", [
    ("echo '87 3 66DA'", "1 complete answers to 2 lines"),               # an answer is missing
    ("echo '87 3 66DA'; echo invalid; echo invalid", "3 complete answers to 2 lines"),
    ("echo '87 3 66DA'; printf '49 5 7C20'", "1 complete answers to 2 lines"),  # cut off
    ("echo '87 3 66DA'; echo '49 5 7c20'", "answered line 2, '9 S D 0', with '49 5 7c20'"),
    ("echo '87 3 66DA'; echo invalid; exit 1", "exit status 1"),
    ("echo '87 3 66DA'; echo invalid; echo oops >&2", "on stderr: b'oops"),
], ids=["too-few", "too-many", "cut-off", "not-an-answer", "exit-status", "stderr"])
def test_a_run_that_did_not_answer_every_line_properly_is_not_compared(tmp_path, script,
                                                                         complaint):
    program = stand_in(tmp_path, script)
    frames = [data_frame(0x222, bytes.fromhex("0011223344")), data_frame(0x009)]
    with pytest.raises(pytest.fail.Exception) as failure:
        ask_can_bits_cli(program, frames)
    assert complaint in str(failure.value)


def test_a_disagreement_is_reported_with_the_frame_and_both_answers():
    capture = data_frame(0x222, bytes.fromhex("0011223344"))
    same = Compared(capture, FrameBits(87, 3, 0x66DA), FrameBits(87, 3, 0x66DA))
    # As if the C side had left out the stuff bit behind the CRC sequence of the frame above.
    shorter = Compared(data_frame(0x009), FrameBits(48, 4, 0x7C20), FrameBits(49, 5, 0x7C20))
    other_crc = Compared(remote_frame(0x1ABCDEF0, 8, extended=True),
                         FrameBits(70, 6, 0x35D5), FrameBits(70, 6, 0x35D4))
    refused = Compared(data_frame(0x7FF), None, FrameBits(47, 3, 0x1234))

    require_agreement([same], "one frame")
    require_agreement([same, same], "two frames")

    with pytest.raises(pytest.fail.Exception) as failure:
        require_agreement([same, shorter, other_crc, refused], "four frames")
    message = str(failure.value)
    assert "3 of the 4 frames of 'four frames'" in message
    assert "frame_bits in 1, stuff_bits in 1, crc15 in 1, 1 refused by at least one side" in message
    # The frame, in words and as the line that reproduces the C side's answer ...
    assert "base format data frame, identifier 0x009, DLC 0, no data field" in message
    assert f"asked of {CLI} as:  9 S D 0" in message
    # ... both answers, and where they part.
    assert "C       frame_bits 48, stuff_bits 4, crc15 0x7C20" in message
    assert "Python  frame_bits 49, stuff_bits 5, crc15 0x7C20" in message
    assert "differ in: frame_bits, stuff_bits" in message
    assert "extended format remote frame, identifier 0x1ABCDEF0, DLC 8, no data field" in message
    assert "C       frame_bits 70, stuff_bits 6, crc15 0x35D5" in message
    assert "Python  frame_bits 70, stuff_bits 6, crc15 0x35D4" in message
    assert "differ in: crc15" in message
    assert "C       refuses the frame" in message
    assert "differ in: whether this is a frame at all" in message
    # The frame on which they agree is not in the report.
    assert "0x222" not in message


def test_a_frame_that_both_refuse_is_no_agreement_and_neither_is_an_empty_family():
    with pytest.raises(pytest.fail.Exception, match="both refuse a frame that was meant"):
        require_agreement([Compared(data_frame(0x123), None, None)], "one frame")
    with pytest.raises(pytest.fail.Exception, match="nothing was compared"):
        require_agreement([], "no frame")


def test_a_long_list_of_disagreements_is_counted_in_full_and_shown_in_part():
    failing = [Compared(data_frame(identifier), FrameBits(50, 6, 0), FrameBits(50, 6, 1))
               for identifier in range(25)]
    with pytest.raises(pytest.fail.Exception) as failure:
        require_agreement(failing, "many")
    message = str(failure.value)
    assert "25 of the 25 frames" in message and "crc15 in 25" in message
    assert message.count("asked of") == SHOWN


# ------------------------------------------------------------------------------------------
# C and Python on the same frames
# ------------------------------------------------------------------------------------------


@pytest.mark.parametrize("family", FAMILIES)
def test_c_and_python_give_the_same_length_stuff_bit_count_and_crc(compared, family):
    require_agreement(compared[family], family)


def test_the_random_frames_cover_every_format_kind_and_dlc_and_most_of_them_are_stuffed(compared):
    frames = compared["random"]
    assert len(frames) >= 20000
    assert Counter(item.frame.case for item in frames) == {
        case: RANDOM_FRAMES_PER_CASE for case in CASES}

    # A sample without stuff bits would compare little more than the CRC. These numbers are
    # the C side's; the test above is what makes them Python's as well. With the seed above
    # the sample has 20 252 stuffed frames, 898 of the 6000 data frames with DLC 4 to 8 have
    # ten or more stuff bits, and 1348 frames have none; the bars leave room for a new seed.
    stuff_bits = [item.c.stuff_bits for item in frames]
    assert sum(1 for count in stuff_bits if count > 0) > 0.9 * len(frames)
    assert 0 in stuff_bits
    long_data = [item.c.stuff_bits for item in frames
                 if not item.frame.remote and item.frame.dlc >= 4]
    assert sum(1 for count in long_data if count >= 10) > 0.1 * len(long_data)
    # In every case some frame comes at least halfway to the bound of the worst case.
    for case in CASES:
        most = max(item.c.stuff_bits for item in frames if item.frame.case == case)
        assert 2 * most >= (stuffed_span(*case) - 1) // 4, case

    # The generator did not get stuck. A base frame without a data field can be one of only
    # 2048 frames, and 600 draws from them repeat by chance (373 to 402 distinct ones with this
    # seed); every other case has at least 2^19 frames to draw from and repeats almost never.
    for case in CASES:
        drawn = {item.frame for item in frames if item.frame.case == case}
        extended, remote, dlc = case
        small = not extended and (remote or dlc == 0)
        assert len(drawn) >= (300 if small else 0.95 * RANDOM_FRAMES_PER_CASE), case


def test_every_crc_sequence_is_on_the_wire_twice_when_two_data_bytes_take_every_value(compared):
    """This is what puts every beginning and every end of a CRC sequence into the sample.

    Up to the last 15 bits that the CRC covers, 2 of the 65 536 frames are the same frame, and
    those 15 bits decide the CRC one-to-one. If a CRC sequence were missing here, the family
    would not be what the top of this file says it is; it would also mean that one side's CRC
    is not the remainder of a division by the generator polynomial."""
    frames = compared["two data bytes"]
    assert len(frames) == 1 << 16
    for side in ("c", "python"):
        seen = Counter(getattr(item, side).crc15 for item in frames)
        assert len(seen) == 1 << 15, side
        assert set(seen.values()) == {2}, side


def test_the_families_have_the_sizes_that_the_top_of_this_file_gives(compared):
    assert len(compared["base frames without data"]) == 20480
    assert len({item.frame for item in compared["base frames without data"]}) == 20480
    assert len(sample(compared)) > 100000


def test_both_reproduce_the_frames_whose_answers_are_known_from_elsewhere(compared):
    for item, (frame, expected) in zip(compared["known answers"], KNOWN_ANSWERS, strict=True):
        assert item.frame == frame
        assert item.c == expected, f"{CLI}: {frame}"
        assert item.python == expected, f"fcn_tools.canbits: {frame}"


def test_both_refuse_what_cannot_be_a_frame_and_take_the_nearest_thing_that_can(compared):
    for item in compared["no frames"]:
        assert item.c is None, f"{CLI} accepts: {item.frame}"
        assert item.python is None, f"fcn_tools.canbits accepts: {item.frame}"
    # The refusals are worth something only if the nearest valid frames pass.
    require_agreement(compared["nearest frames"], "nearest frames")


# ------------------------------------------------------------------------------------------
# Unstuffed length and worst case
# ------------------------------------------------------------------------------------------


@pytest.mark.parametrize("extended, remote, dlc", CASES)
def test_the_formulas_are_the_closed_forms_of_can_bits_h(extended, remote, dlc):
    span = stuffed_span(extended, remote, dlc)
    assert canbits.unstuffed_bits(extended, remote, dlc) == span + TAIL_BITS
    # After the first five bits of the span, every further four can earn a stuff bit.
    assert canbits.worst_case_bits(extended, remote, dlc) == span + (span - 1) // 4 + TAIL_BITS


def test_every_exact_length_is_the_unstuffed_length_plus_the_stuff_bits_on_both_sides(compared):
    """The C side cannot be asked for can_bits_unstuffed(), but every answer of
    can_bits_exact() shows what it took the unstuffed length to be: the frame's bits minus its
    stuff bits. For every format, kind and DLC that has to be what Python's formula says."""
    by_case: dict[tuple[bool, bool, int], list[Compared]] = {case: [] for case in CASES}
    for item in sample(compared):
        by_case[item.frame.case].append(item)

    for case, items in by_case.items():
        unstuffed = canbits.unstuffed_bits(*case)
        assert len(items) >= RANDOM_FRAMES_PER_CASE, case
        for side in ("c", "python"):
            answers = [getattr(item, side) for item in items]
            wrong = [item.frame.line() for item, answer in zip(items, answers)
                     if answer is None or answer.frame_bits - answer.stuff_bits != unstuffed]
            assert not wrong, (f"{side}: {len(wrong)} frames are not {unstuffed} bits long "
                               f"without their stuff bits, the first: {wrong[0]}")


def test_no_frame_is_longer_than_the_worst_case_and_some_come_close_to_it(compared, capsys):
    frames = sample(compared)
    most = {case: 0 for case in CASES}
    too_long = []
    for item in frames:
        worst = canbits.worst_case_bits(*item.frame.case)
        for side in (item.c, item.python):
            if side is not None:
                most[item.frame.case] = max(most[item.frame.case], side.stuff_bits)
                if side.frame_bits > worst:
                    too_long.append(item)

    allowed = {case: (stuffed_span(*case) - 1) // 4 for case in CASES}
    differing = sum(1 for item in frames if not item.same_answer())
    summary = [f"{len(frames)} frames in one run of {CLI}, {differing} with different answers "
               f"from C and Python",
               "most stuff bits seen / most that the worst case allows, for DLC 0 to 8:"]
    for extended in (False, True):
        for remote in (False, True):
            row = " ".join(f"{most[extended, remote, dlc]:2}/{allowed[extended, remote, dlc]:<2}"
                           for dlc in range(9))
            summary.append(f"  {'extended' if extended else 'base':8} "
                           f"{'remote' if remote else 'data':6} {row}")
    with capsys.disabled():
        print()
        for line in summary:
            print(f"canbits cross-check: {line}")

    assert not too_long, disagreement(too_long, len(frames), "longer than the worst case")
