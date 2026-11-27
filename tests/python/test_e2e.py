# SPDX-License-Identifier: MIT
"""AUTOSAR E2E Profile 11: the published test vectors, fcn_tools.e2e on its own, and the C code
of lib/e2e against fcn_tools.e2e.

fcn_tools.e2e was written from the specification text without a look at lib/e2e (see its
docstring). The first part of this file holds it to values that come from neither: the test
vectors of the PRS, the worked examples of the AUTOSAR CRC library, the CRC catalogue, and
rules of the PRS checked case by case. The second part makes the two implementations meet:
tests/tools/e2e_cli.c (release build) puts e2e_p11_protect() and e2e_p11_check() behind a
pipe, every request of this file goes to ONE run of it, and every answer has to equal
Python's in every field, including the state each side leaves behind.

The requests, and what each family is there to expose:

  protect, random        2 000 messages: random Data IDs, lengths 2 to 8, payloads and sender
                         counters 0 to 14. The CRC's input order, the counter nibble, the
                         bits that must stay as they are.
  check, every step      every pair of stored and received counter (225) for every
                         max_delta_counter (14), with a correct CRC: the counter arithmetic,
                         the wrap from 14 to 0, the order of the status rules.
  check, random          3 000 received messages: correct ones with any counter (the reserved
                         15 included), ones with 1 to 3 bits flipped, ones under another Data
                         ID, and random bytes.
  sequences              60 sender-receiver pairs over 200 cycles each, through a channel that
                         loses, repeats, delays and damages messages and sometimes delivers
                         nothing. Each side is asked from the same state at every step, so a
                         wrong state update shows at the step where it happens.
  input errors           lengths that do not match, state counters 15 to 255, max_delta_counter
                         0 and 15 to 255, data_len 0 and 1, data and length that do not agree.

The C module accepts lengths of 2 to 8 bytes only (classical CAN), the Python one up to 32;
data_len above 8 is therefore not compared here. tests/unit/test_e2e_p11.c covers it.
"""
from __future__ import annotations

import itertools
import random
import re
import subprocess
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import pytest

from fcn_tools import e2e, paths
from fcn_tools.e2e import Config, DataIdMode, Status

CLI = "e2e_cli"
CLI_TIMEOUT_S = 60

# ------------------------------------------------------------------------------------------
# Part 1: fcn_tools.e2e against values from elsewhere
# ------------------------------------------------------------------------------------------

#: SWS_Crc_00074 (AUTOSAR_CP_SWS_CRCLibrary R23-11): Crc_CalculateCRC8 of seven messages.
CRC_LIBRARY_EXAMPLES = [
    ("00000000", 0x59), ("F20183", 0x37), ("0FAA0055", 0x79), ("00FF5511", 0xB8),
    ("332255AABBCCDDEEFF", 0xCB), ("926B55", 0x8C), ("FFFFFFFF", 0x74),
]

CHECK_MESSAGE = b"123456789"


def test_crc_library_function_gives_the_library_examples():
    assert e2e.crc_calculate_crc8(CHECK_MESSAGE, 0x00, True) == 0x4B  # SWS_Crc_00073 "Check"
    for message, expected in CRC_LIBRARY_EXAMPLES:
        assert e2e.crc_calculate_crc8(bytes.fromhex(message), 0x00, True) == expected, message


def test_crc_library_chains_calls_through_the_start_value():
    """SWS_Crc_00041: a call with Crc_IsFirstCall FALSE continues the CRC of the call before,
    whose result it is given; split anywhere, the result is the one of a single call."""
    whole = e2e.crc_calculate_crc8(CHECK_MESSAGE, 0x00, True)
    for split in range(len(CHECK_MESSAGE) + 1):
        first = e2e.crc_calculate_crc8(CHECK_MESSAGE[:split], 0x00, True)
        assert e2e.crc_calculate_crc8(CHECK_MESSAGE[split:], first, False) == whole, split


def test_profile_crc_is_crc8_gsm_a():
    """The boxes of Fig. 6.97 add up to polynomial 0x1D with start 0x00 and no final XOR:
    CRC-8/GSM-A of the CRC catalogue, check value 0x37. Fed "123456789" through the profile:
    Data ID 0x3231 ('1', '2'), byte 1 '3' = 0x33 (counter 3 with payload 3), bytes 2..7 '4'..'9'.
    """
    config = Config(data_id=0x3231)
    message, _ = e2e.protect(config, 3, b"\x00\x30" + CHECK_MESSAGE[3:])
    assert message == b"\x37" + CHECK_MESSAGE[2:]


#: PRS R24-11, Tables 6.48 to 6.52. Data ID 0x123, MaxDeltaCounter 1, payload all zero; the
#: counter is the sender's before the call (0: the first message, 1: the second).
PUBLISHED_VECTORS = [
    ("6.48 BOTH, counter 0", Config(data_id=0x123), 0, "CC 00 00 00 00 00 00 00"),
    ("6.49 BOTH, counter 1", Config(data_id=0x123), 1, "91 01 00 00 00 00 00 00"),
    ("6.50 NIBBLE, counter 0", Config(data_id=0x123, data_id_mode=DataIdMode.NIBBLE), 0,
     "2A 10 00 00 00 00 00 00"),
    ("6.51 NIBBLE, counter 1", Config(data_id=0x123, data_id_mode=DataIdMode.NIBBLE), 1,
     "77 11 00 00 00 00 00 00"),
    ("6.52 NIBBLE, CRC at bit 64", Config(data_id=0x123, data_id_mode=DataIdMode.NIBBLE,
                                          data_length=128, crc_offset=64, counter_offset=72,
                                          data_id_nibble_offset=76), 0,
     "00 00 00 00 00 00 00 00 7D 10 00 00 00 00 00 00"),
]


@pytest.mark.parametrize("name, config, counter, expected", PUBLISHED_VECTORS,
                         ids=[vector[0] for vector in PUBLISHED_VECTORS])
def test_published_vectors(name, config, counter, expected):
    message, next_counter = e2e.protect(config, counter, bytes(config.length_bytes))
    assert message.hex(" ").upper() == expected
    assert next_counter == counter + 1
    # ... and a receiver that saw the message before accepts it.
    status, stored = e2e.check(config, (counter - 1) % 15, message)
    assert (status, stored) == (Status.OK, counter)


def test_sender_counts_0_to_14_and_never_sends_15():
    sender = e2e.Sender(Config(data_id=0x123))
    counters = [sender.protect(bytes(8))[1] & 0x0F for _ in range(46)]
    assert counters == [n % 15 for n in range(46)]


def test_protect_writes_only_the_crc_and_the_counter_nibble():
    config = Config(data_id=0x0123)
    before = bytes.fromhex("5A F0 11 22 33 44 55 66")
    message, _ = e2e.protect(config, 14, before)
    assert message[1] == 0xFE and message[2:] == before[2:]
    nibble = Config(data_id=0x0E55, data_id_mode=DataIdMode.NIBBLE)
    message, _ = e2e.protect(nibble, 9, bytes([0xFF] * 8))
    assert message[1] == 0xE9 and message[2:] == bytes([0xFF] * 6)


@pytest.mark.parametrize("max_delta", [1, 3, 14])
def test_status_is_a_function_of_the_counter_step(max_delta):
    """Fig. 6.110 for every pair of stored and received counter. The step is received minus
    stored, plus 15 when negative (the PRS's "taking into wrap around 0xE", as Profile 1
    spells it out in Fig. 6.60)."""
    config = Config(data_id=0x123, max_delta_counter=max_delta)
    sender_messages = {}
    counter = 0
    for _ in range(15):
        message, next_counter = e2e.protect(config, counter, bytes(8))
        sender_messages[counter] = message
        counter = next_counter
    for stored, received in itertools.product(range(15), repeat=2):
        step = received - stored + (15 if received < stored else 0)
        expected = (Status.REPEATED if step == 0 else Status.OK if step == 1
                    else Status.OKSOMELOST if step <= max_delta else Status.WRONGSEQUENCE)
        assert e2e.check(config, stored, sender_messages[received]) == (expected, received)


def test_check_after_init_accepts_only_counter_0_as_ok():
    for first in range(15):
        receiver = e2e.Receiver(Config(data_id=0x123))
        assert (receiver.counter, receiver.status) == (14, Status.ERROR)
        message, _ = e2e.protect(receiver.config, first, bytes(8))
        expected = (Status.OK if first == 0 else Status.REPEATED if first == 14
                    else Status.WRONGSEQUENCE)
        assert receiver.check(message) == expected, first


def test_a_sequence_through_every_rule():
    """The sequence of tests/unit/test_e2e_p11.c, test_a_sequence_through_every_rule, with the
    statuses worked out there by hand."""
    frames = {c: e2e.protect(Config(data_id=0x123), c, bytes(8))[0] for c in range(15)}
    damaged = bytearray(frames[2])
    damaged[5] ^= 0x10
    reserved = bytearray(8)
    reserved[1] = 0x0F
    reserved[0] = e2e.compute_crc(Config(data_id=0x123), reserved)
    sequence = [frames[0], frames[0], frames[1], None, frames[3], frames[7], frames[8],
                frames[14], frames[0], frames[2], bytes(damaged), frames[3], bytes(reserved),
                frames[0], frames[1]]
    strict = "OK REPEATED OK NONEWDATA WRONGSEQUENCE WRONGSEQUENCE OK WRONGSEQUENCE OK " \
             "WRONGSEQUENCE ERROR OK ERROR WRONGSEQUENCE OK".split()
    tolerant = list(strict)
    tolerant[4] = tolerant[9] = "OKSOMELOST"
    for max_delta, expected in ((1, strict), (3, tolerant)):
        receiver = e2e.Receiver(Config(data_id=0x123, max_delta_counter=max_delta))
        assert [receiver.check(frame).name for frame in sequence] == expected


def test_reserved_counter_15_is_an_error_and_keeps_the_counter():
    config = Config(data_id=0x123)
    message = bytearray(8)
    message[1] = 0x0F
    message[0] = e2e.compute_crc(config, message)
    assert e2e.check(config, 14, bytes(message)) == (Status.ERROR, 14)


def test_check_without_a_message_is_nonewdata_and_keeps_the_counter():
    assert e2e.check(Config(data_id=0x123), 9, None) == (Status.NONEWDATA, 9)


def test_wrong_input_raises():
    config = Config(data_id=0x123)
    with pytest.raises(ValueError):
        e2e.protect(config, 0, bytes(7))
    with pytest.raises(ValueError):
        e2e.protect(config, 15, bytes(8))
    with pytest.raises(ValueError):
        e2e.check(config, 0, bytes(9))
    with pytest.raises(ValueError):
        e2e.check(config, 15, None)
    for bad in (dict(max_delta_counter=0), dict(max_delta_counter=15), dict(data_length=12),
                dict(data_length=8), dict(data_length=264), dict(data_id=0x10000),
                dict(crc_offset=4), dict(counter_offset=6), dict(counter_offset=0),
                dict(data_id=0x1000, data_id_mode=DataIdMode.NIBBLE),
                dict(data_id_mode=DataIdMode.NIBBLE, data_id_nibble_offset=8)):
        with pytest.raises(ValueError):
            Config(**{"data_id": 0x123, **bad})


# Undetected errors of an 8-byte message, all 64 bits transmitted: counted by brute force.
UNDETECTED_3_BIT_ERRORS = 198


def test_hamming_distance_of_an_8_byte_message_is_3():
    """Every error of 1 or 2 bits anywhere in the 64 bits is detected; of the C(64, 3) = 41 664
    errors of 3 bits, 198 are not. The CRC is linear and starts at 0, so whether an error
    pattern goes undetected does not depend on the message; two messages show it."""
    config = Config(data_id=0x0123)
    messages = [e2e.protect(config, 3, bytes.fromhex("00 50 11 22 33 44 55 66"))[0],
                e2e.protect(config, 9, bytes.fromhex("00 A0 FF 00 5A C3 81 7E"))[0]]
    for message in messages:
        undetected = Counter()
        for weight in (1, 2, 3):
            for bits in itertools.combinations(range(64), weight):
                damaged = bytearray(message)
                for bit in bits:
                    damaged[bit // 8] ^= 1 << (bit % 8)
                if e2e.compute_crc(config, damaged) == damaged[0]:
                    undetected[weight] += 1
        assert (undetected[1], undetected[2], undetected[3]) == (0, 0, UNDETECTED_3_BIT_ERRORS)


def hamming_distance(generator: int, length: int) -> int:
    """The Hamming distance (2, 3, or 4 meaning "4 or more") of an 8-bit CRC with generator
    x^8 + `generator` over code words of `length` bits, CRC included, from the polynomials
    alone: an error pattern E(x) goes undetected exactly when g(x) divides it, that is when the
    remainders x^p mod g(x) of its bit positions p add up to 0."""
    remainders, r = [], 1
    for _ in range(length):
        remainders.append(r)
        r = (r << 1) ^ (0x100 | generator) if r & 0x80 else r << 1
    if len(set(remainders)) < length:
        return 2
    position = {value: p for p, value in enumerate(remainders)}
    for i, j in itertools.combinations(range(length), 2):
        k = position.get(remainders[i] ^ remainders[j])
        if k is not None and k > j:
            return 3
    return 4


def test_hamming_distance_limits_of_the_two_profile_polynomials():
    """0x1D (Profiles 1 and 11) keeps a distance of 4 only up to 13 data bits and of 3 up to
    247; 0x2F (Profiles 2 and 22) keeps 4 up to 119 data bits, so over an 8-byte message it
    detects every error of 3 bits, which 0x1D does not (see the test above)."""
    def longest(generator: int, wanted: int) -> int:
        return max(n for n in range(9, 300) if hamming_distance(generator, n) >= wanted) - 8

    assert hamming_distance(0x1D, 64) == 3
    assert hamming_distance(0x2F, 64) == 4
    assert (longest(0x1D, 4), longest(0x1D, 3)) == (13, 247)
    assert longest(0x2F, 4) == 119


def test_data_id_crc_classes():
    """Data IDs with the same CRC over their two bytes protect identically. Every value of that
    CRC is shared by exactly 8 of the 2 048 11-bit identifiers (the register after the high byte
    0..7 and any low byte takes each value 8 times); 0x123 shares it with seven others, and a
    message of 0x123 is accepted as one of 0x0A0."""
    classes = Counter(e2e.data_id_crc(data_id) for data_id in range(0x800))
    assert set(classes.values()) == {8} and len(classes) == 256
    same = [data_id for data_id in range(0x800)
            if data_id != 0x123 and e2e.data_id_crc(data_id) == e2e.data_id_crc(0x123)]
    assert same == [0x0A0, 0x2BB, 0x338, 0x496, 0x515, 0x68D, 0x70E]
    message, _ = e2e.protect(Config(data_id=0x123), 0, bytes(8))
    assert e2e.check(Config(data_id=0x0A0), 14, message) == (Status.OK, 0)
    assert e2e.check(Config(data_id=0x124), 14, message) == (Status.ERROR, 14)


# ------------------------------------------------------------------------------------------
# Part 2: lib/e2e against fcn_tools.e2e
# ------------------------------------------------------------------------------------------

RETURN_CODES = ("E_OK", "E_INPUTERR_NULL", "E_INPUTERR_WRONG")


@dataclass(frozen=True)
class Outcome:
    """What one call did: its return code and the state (and message) it left. Python's
    ValueError is E_INPUTERR_WRONG with everything as it was."""

    ret: str
    counter: int
    result: str  # protect: the message in upper-case hex or "-"; check: the status name


@dataclass(frozen=True)
class ProtectRequest:
    data_id: int
    data_len: int
    counter: int
    data: bytes

    def line(self) -> str:
        return f"P {self.data_id:X} {self.data_len} {self.counter} {self.data.hex().upper()}"

    def python(self) -> Outcome:
        try:
            config = Config(data_id=self.data_id, data_length=8 * self.data_len)
            message, counter = e2e.protect(config, self.counter, self.data)
        except ValueError:
            return Outcome("E_INPUTERR_WRONG", self.counter, self.data.hex().upper())
        return Outcome("E_OK", counter, message.hex().upper())


@dataclass(frozen=True)
class CheckRequest:
    data_id: int
    data_len: int
    max_delta: int
    counter: int
    status: Status
    data: bytes | None

    def line(self) -> str:
        data = self.data.hex().upper() if self.data is not None else "-"
        return (f"C {self.data_id:X} {self.data_len} {self.max_delta} {self.counter} "
                f"{self.status.name} {data}")

    def python(self) -> Outcome:
        try:
            config = Config(data_id=self.data_id, data_length=8 * self.data_len,
                            max_delta_counter=self.max_delta)
            status, counter = e2e.check(config, self.counter, self.data)
        except ValueError:
            return Outcome("E_INPUTERR_WRONG", self.counter, self.status.name)
        return Outcome("E_OK", counter, status.name)


Request = ProtectRequest | CheckRequest

ANSWER = re.compile(r"(E_\w+) (\d+) ([0-9A-F]+|-|[A-Z]+)")


def parse_answer(text: str) -> Outcome:
    match = ANSWER.fullmatch(text)
    if match is None or match[1] not in RETURN_CODES:
        raise ValueError(f"not an answer of {CLI}: {text!r}")
    return Outcome(match[1], int(match[2]), match[3])


def require_cli() -> Path:
    """The release build of the CLI. A missing binary fails the test: a comparison that is
    skipped would read as one that passed."""
    binary = paths.tool_binary(CLI)
    if not binary.is_file():
        pytest.fail(f"{binary} not found: build it with scripts/build.sh release, with "
                    f"FCN_BUILD_ROOT set as for this test run (build root: {paths.build_root()})",
                    pytrace=False)
    return binary


def ask_cli(binary: Path, lines: Sequence[str]) -> list[str]:
    """The answers of one run of e2e_cli to all `lines`, in their order. A run that fails, or
    that does not answer every line with exactly one line, is not compared at all."""
    questions = "".join(f"{line}\n" for line in lines).encode("ascii")
    try:
        run = subprocess.run([str(binary)], input=questions, capture_output=True,
                             timeout=CLI_TIMEOUT_S, check=False)
    except subprocess.TimeoutExpired:
        pytest.fail(f"{binary} was not done with {len(lines)} lines after {CLI_TIMEOUT_S} s",
                    pytrace=False)
    if run.returncode != 0 or run.stderr:
        pytest.fail(f"{binary} ended with exit status {run.returncode}; on stderr: "
                    f"{run.stderr[:300]!r}", pytrace=False)
    answers = run.stdout.decode("ascii", errors="replace").split("\n")
    if answers.pop() != "" or len(answers) != len(lines):
        pytest.fail(f"{binary} gave {len(answers)} complete answers to {len(lines)} lines",
                    pytrace=False)
    return answers


# ---------------------------------------------------------------------------- the requests

RANDOM_SEED = 0xE2E0011A


def random_payload(rng: random.Random, length: int) -> bytes:
    return bytes(rng.getrandbits(8) for _ in range(length))


def protect_random(rng: random.Random) -> list[ProtectRequest]:
    return [ProtectRequest(rng.getrandbits(16), length, rng.randrange(15),
                           random_payload(rng, length))
            for length in (rng.randint(2, 8) for _ in range(2000))]


def published_through_the_pipe() -> list[ProtectRequest]:
    """The four published vectors the C module can make: 11A as such, 11C as the equal 11A
    message (Data ID low byte with a 0 high byte, the Data ID nibble as payload; see
    tests/unit/test_e2e_p11.c)."""
    return [ProtectRequest(0x123, 8, 0, bytes(8)), ProtectRequest(0x123, 8, 1, bytes(8)),
            ProtectRequest(0x023, 8, 0, bytes([0, 0x10]) + bytes(6)),
            ProtectRequest(0x023, 8, 1, bytes([0, 0x10]) + bytes(6))]


def good_message(data_id: int, length: int, counter: int, payload: bytes) -> bytes:
    """A message with a correct CRC and any counter nibble, 15 included, built with the
    Python CRC (the Python sender refuses counter 15)."""
    message = bytearray(payload[:length])
    message[1] = (message[1] & 0xF0) | counter
    message[0] = e2e.compute_crc(Config(data_id=data_id, data_length=8 * length), message)
    return bytes(message)


def check_every_step() -> list[CheckRequest]:
    requests = []
    for max_delta in range(1, 15):
        for stored, received in itertools.product(range(15), repeat=2):
            message = good_message(0x0123, 8, received, bytes(8))
            requests.append(CheckRequest(0x0123, 8, max_delta, stored, Status.ERROR, message))
    return requests


def check_random(rng: random.Random) -> list[CheckRequest]:
    requests = []
    for n in range(3000):
        data_id = rng.getrandbits(16)
        length = rng.randint(2, 8)
        payload = random_payload(rng, length)
        message = good_message(data_id, length, rng.randrange(16), payload)
        kind = n % 4
        if kind == 1:  # damaged: 1 to 3 bits flipped
            damaged = bytearray(message)
            for bit in rng.sample(range(8 * length), rng.randint(1, 3)):
                damaged[bit // 8] ^= 1 << (bit % 8)
            message = bytes(damaged)
        elif kind == 2:  # checked under another Data ID
            data_id ^= 1 << rng.randrange(16)
        elif kind == 3:  # random bytes
            message = payload
        requests.append(CheckRequest(data_id, length, rng.randint(1, 14), rng.randrange(15),
                                     rng.choice(list(Status)), message))
    return requests


def check_sequences(rng: random.Random) -> list[CheckRequest]:
    """Sender and receiver pairs through a faulty channel; the requests carry the Python
    receiver's state before each step."""
    requests = []
    for _ in range(60):
        config = Config(data_id=rng.getrandbits(16), data_length=8 * rng.randint(2, 8),
                        max_delta_counter=rng.randint(1, 4))
        sender = e2e.Sender(config)
        receiver = e2e.Receiver(config)
        delayed: list[bytes] = []
        last: bytes | None = None
        for _ in range(200):
            message: bytes | None = sender.protect(random_payload(rng, config.length_bytes))
            event = rng.random()
            if event < 0.10:      # lost
                message = None
            elif event < 0.15:    # held back, delivered later instead of a newer one
                delayed.append(message)
                message = None
            elif event < 0.20 and delayed:
                message = delayed.pop(0)
            elif event < 0.25 and last is not None:  # repeated by a gateway
                message = last
            elif event < 0.30:    # damaged
                damaged = bytearray(message)
                damaged[rng.randrange(len(damaged))] ^= 1 << rng.randrange(8)
                message = bytes(damaged)
            if rng.random() < 0.05:  # a gap in the sender: nothing in this cycle at all
                message = None
            requests.append(CheckRequest(config.data_id, config.length_bytes,
                                         config.max_delta_counter, receiver.counter,
                                         receiver.status, message))
            receiver.check(message)
            last = message if message is not None else last
    return requests


def input_errors() -> list[Request]:
    good = good_message(0x0123, 8, 4, bytes(8))
    requests: list[Request] = []
    for length in (2, 7, 8, 9, 16):
        requests.append(ProtectRequest(0x0123, 8, 3, bytes(length)))
        requests.append(CheckRequest(0x0123, 8, 1, 3, Status.OKSOMELOST, (good * 2)[:length]))
    for counter in (15, 16, 100, 255):
        requests.append(ProtectRequest(0x0123, 8, counter, bytes(8)))
        requests.append(CheckRequest(0x0123, 8, 1, counter, Status.REPEATED, good))
        requests.append(CheckRequest(0x0123, 8, 1, counter, Status.REPEATED, None))
    for max_delta in (0, 15, 99, 255):
        requests.append(CheckRequest(0x0123, 8, max_delta, 3, Status.OK, good))
        requests.append(CheckRequest(0x0123, 8, max_delta, 3, Status.OK, None))
    for data_len in (0, 1):
        requests.append(ProtectRequest(0x0123, data_len, 3, bytes(max(data_len, 1))))
        requests.append(CheckRequest(0x0123, data_len, 1, 3, Status.OK, bytes(max(data_len, 1))))
        requests.append(CheckRequest(0x0123, data_len, 1, 3, Status.OK, None))
    # The boundaries that are accepted, next to the refused ones.
    requests.append(CheckRequest(0x0123, 8, 14, 3, Status.OK, good))
    requests.append(CheckRequest(0x0123, 2, 1, 3, Status.OK, good[:2]))
    requests.append(ProtectRequest(0x0123, 2, 14, bytes(2)))
    return requests


def families() -> dict[str, list[Request]]:
    rng = random.Random(RANDOM_SEED)
    return {
        "published vectors": list(published_through_the_pipe()),
        "protect, random": list(protect_random(rng)),
        "check, every step": list(check_every_step()),
        "check, random": list(check_random(rng)),
        "sequences": list(check_sequences(rng)),
        "input errors": input_errors(),
    }


@dataclass(frozen=True)
class Compared:
    request: Request
    c: Outcome
    python: Outcome

    @property
    def agree(self) -> bool:
        return self.c == self.python


@dataclass(frozen=True)
class Run:
    """The outcome of the one run of e2e_cli: every request with both answers, by family, and
    the answer to "I", the initial states."""

    families: dict[str, list[Compared]]
    init: str


@pytest.fixture(scope="module")
def run() -> Run:
    """This is where the one run of e2e_cli takes place; the tests only look at what it
    returned."""
    groups = families()
    requests = [request for group in groups.values() for request in group]
    answers = iter(ask_cli(require_cli(), [request.line() for request in requests] + ["I"]))
    compared = {}
    for name, group in groups.items():
        items = []
        for request in group:
            answer = next(answers)
            try:
                outcome = parse_answer(answer)
            except ValueError:
                pytest.fail(f"{CLI} answered {request.line()!r} with {answer!r}", pytrace=False)
            items.append(Compared(request, outcome, request.python()))
        compared[name] = items
    return Run(families=compared, init=next(answers))


@pytest.fixture(scope="module")
def compared(run) -> dict[str, list[Compared]]:
    return run.families


def test_c_and_python_start_from_the_same_states(run):
    """E2E_P11ProtectInit: counter 0; E2E_P11CheckInit: counter 0xE, status ERROR."""
    assert run.init == (f"E_OK {e2e.SENDER_INITIAL_COUNTER} E_OK {e2e.RECEIVER_INITIAL_COUNTER}"
                        f" {e2e.RECEIVER_INITIAL_STATUS.name}")


def disagreements(items: Sequence[Compared]) -> str:
    wrong = [item for item in items if not item.agree]
    lines = [f"{item.request.line()}\n    C      {item.c}\n    Python {item.python}"
             for item in wrong[:10]]
    return f"{len(wrong)} of {len(items)} disagree:\n" + "\n".join(lines)


@pytest.mark.parametrize("family", ["published vectors", "protect, random", "check, every step",
                                    "check, random", "sequences", "input errors"])
def test_c_and_python_agree(compared, family):
    items = compared[family]
    assert all(item.agree for item in items), disagreements(items)


def test_published_vectors_through_the_pipe(compared):
    """The C side's answers to the published vectors are the published bytes: the pipe compares
    the C code with the reference, not something else with something else."""
    got = [item.c.result for item in compared["published vectors"]]
    assert got == ["CC00000000000000", "9101000000000000", "2A10000000000000",
                   "7711000000000000"]


def test_the_comparison_covers_every_outcome(compared):
    """A comparison that never meets a status, or never a refusal, proves nothing about it."""
    statuses = Counter(item.c.result for name in ("check, every step", "check, random",
                                                  "sequences")
                       for item in compared[name] if item.c.ret == "E_OK")
    assert set(statuses) == {status.name for status in Status}, statuses
    assert min(statuses.values()) >= 20, statuses
    returns = Counter(item.c.ret for item in compared["input errors"])
    assert returns["E_INPUTERR_WRONG"] >= 20 and returns["E_OK"] >= 3, returns
    lengths = Counter(item.request.data_len for item in compared["protect, random"])
    assert set(lengths) == set(range(2, 9)), lengths


def test_the_comparison_notices_a_difference(compared):
    """The fields that are compared are the ones that differ when an implementation is wrong: a
    planted change in each field of one answer is reported."""
    item = compared["sequences"][0]
    for planted in (Outcome("E_INPUTERR_WRONG", item.c.counter, item.c.result),
                    Outcome(item.c.ret, (item.c.counter + 1) % 15, item.c.result),
                    Outcome(item.c.ret, item.c.counter, "REPEATED" if item.c.result != "REPEATED"
                            else "OK")):
        assert not Compared(item.request, planted, item.python).agree
