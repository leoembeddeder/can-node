# SPDX-License-Identifier: MIT
"""canbits, the independent reference for CAN frame lengths, checked against values that do not
come from it: logic-analyser captures, the CRC catalogue's check value, frames worked out by
hand from the Bosch specification, and properties that hold for every frame.
"""
from __future__ import annotations

import itertools
import random

import can
import pytest

from fcn_tools import canbits
from fcn_tools.canbits import FrameBits

# ------------------------------------------------------------------------------------------
# Independent helpers: a second way to the same answer, written differently on purpose.
# ------------------------------------------------------------------------------------------


def crc15_by_long_division(bits: list[int]) -> int:
    """The CRC as section 3.2.1 defines it, before it suggests a shift register: the remainder
    of (the bit stream followed by 15 zeros) divided modulo 2 by the generator polynomial."""
    generator = 0xC599  # x^15 + x^14 + x^10 + x^8 + x^7 + x^4 + x^3 + 1
    dividend = int("".join(str(bit) for bit in bits) or "0", 2) << 15
    for shift in range(max(dividend.bit_length() - 16, -1), -1, -1):
        if (dividend >> (shift + 15)) & 1:
            dividend ^= generator << shift
    return dividend


def destuff(bits: list[int]) -> list[int]:
    """What a receiver does with a stuffed stream: the bit after five equal bits is a stuff
    bit. It has to be there, it has to have the opposite level, and it is discarded."""
    out: list[int] = []
    run_level, run_length = None, 0
    position = 0
    while position < len(bits):
        bit = bits[position]
        out.append(bit)
        run_level, run_length = (bit, run_length + 1) if bit == run_level else (bit, 1)
        if run_length == 5:
            position += 1
            assert position < len(bits), "five equal bits at the end without a stuff bit"
            assert bits[position] != bit, f"bit {position} should be a stuff bit"
            run_level, run_length = bits[position], 1
        position += 1
    return out


def fields_as_text(ident: int, data: bytes, extended: bool, remote: bool, dlc: int) -> str:
    """Start of frame to the end of the data field as '0'/'1' text, composed straight from the
    field lists of sections 3.2.1 and 3.2.2."""
    rtr = "1" if remote else "0"
    if extended:
        arbitration = f"{ident >> 18:011b}" + "1" + "1" + f"{ident & 0x3FFFF:018b}" + rtr
        control = "0" + "0" + f"{dlc:04b}"  # r1, r0, DLC
    else:
        arbitration = f"{ident:011b}" + rtr
        control = "0" + "0" + f"{dlc:04b}"  # IDE, r0, DLC
    data_field = "" if remote else "".join(f"{byte:08b}" for byte in data)
    return "0" + arbitration + control + data_field


def bits_of(data: bytes) -> list[int]:
    return [(byte >> shift) & 1 for byte in data for shift in range(7, -1, -1)]


def longest_run(bits: list[int]) -> int:
    return max((len(list(group)) for _, group in itertools.groupby(bits)), default=0)


# ------------------------------------------------------------------------------------------
# Published values
# ------------------------------------------------------------------------------------------


def test_base_frame_captured_by_a_logic_analyser():
    # sigrok test suite, CAN decoder: identifier 0x222, data 00 11 22 33 44.
    info = canbits.frame_bits(0x222, bytes.fromhex("0011223344"))
    assert info == FrameBits(frame_bits=87, stuff_bits=3, crc15=0x66DA)


def test_extended_frame_captured_by_a_logic_analyser():
    # sigrok test suite, CAN decoder: identifier 0x11223344, data 00 11 22 33 44 55 66.
    info = canbits.frame_bits(0x11223344, bytes.fromhex("00112233445566"), is_extended_id=True)
    assert info == FrameBits(frame_bits=123, stuff_bits=3, crc15=0x0D30)


def test_crc15_has_the_catalogue_check_value():
    # CRC-15/CAN in the catalogue of parametrised CRC algorithms: check = 0x059E for the
    # ASCII string "123456789", fed most significant bit first.
    assert canbits.crc15(bits_of(b"123456789")) == 0x059E


def test_crc15_of_nothing_and_of_dominant_bits_is_zero():
    # The register starts at zero and a dominant bit never sets CRCNXT while it is zero.
    assert canbits.crc15([]) == 0
    assert canbits.crc15([0] * 40) == 0


# ------------------------------------------------------------------------------------------
# Frames worked out by hand
#
# The CRC register is linear, so the CRC of a frame is the XOR of what each of its recessive
# bits would leave behind alone. A lone recessive bit meets an empty register: CRCNXT is 1,
# the shift changes nothing, 4599h is XORed in. Every dominant bit after it shifts the
# register left by one place (15 bits wide) and XORs 4599h in again if the bit that fell out
# of position 14 was a 1. T[k] below is the register after a lone recessive bit followed by k
# dominant bits, each line derived from the one before by that rule:
#
#    k  T[k]   from T[k-1]                    k  T[k]   from T[k-1]
#    0  4599h  (the generator)               14  1C05h  599Ch ^ 4599h
#    1  4EABh  0B32h ^ 4599h                 15  380Ah  shift only
#    2  58CFh  1D56h ^ 4599h                 16  7014h  shift only
#    3  7407h  319Eh ^ 4599h                 17  25B1h  6028h ^ 4599h
#    4  2D97h  680Eh ^ 4599h                 18  4B62h  shift only
#    5  5B2Eh  shift only                    19  535Dh  16C4h ^ 4599h
#    6  73C5h  365Ch ^ 4599h                 20  6323h  26BAh ^ 4599h
#    7  2213h  678Ah ^ 4599h                 21  03DFh  4646h ^ 4599h
#    8  4426h  shift only                    22  07BEh  shift only
#    9  4DD5h  084Ch ^ 4599h                 23  0F7Ch  shift only
#   10  5E33h  1BAAh ^ 4599h                 24  1EF8h  shift only
#   11  79FFh  3C66h ^ 4599h                 25  3DF0h  shift only
#   12  3667h  73FEh ^ 4599h                 26  7BE0h  shift only
#   13  6CCEh  shift only                    27  3259h  77C0h ^ 4599h
#
# For each frame below: the bits before the CRC, the recessive bits among them with the number
# of bits that follow each (its k), the CRC as the XOR of those T[k], and then the whole
# stuffed part with the stuff bits in brackets. The frame is that many bits plus the 10 bits
# of CRC delimiter, ACK field and end of frame.
# ------------------------------------------------------------------------------------------

LONE_BIT_CRC = (
    0x4599, 0x4EAB, 0x58CF, 0x7407, 0x2D97, 0x5B2E, 0x73C5, 0x2213, 0x4426, 0x4DD5,
    0x5E33, 0x79FF, 0x3667, 0x6CCE, 0x1C05, 0x380A, 0x7014, 0x25B1, 0x4B62, 0x535D,
    0x6323, 0x03DF, 0x07BE, 0x0F7C, 0x1EF8, 0x3DF0, 0x7BE0, 0x3259,
)  # fmt: skip


@pytest.mark.parametrize("k", range(len(LONE_BIT_CRC)))
def test_crc15_of_a_lone_recessive_bit_matches_the_hand_computed_table(k):
    assert canbits.crc15([1] + [0] * k) == LONE_BIT_CRC[k]


HAND_VECTORS = [
    # Base format, identifier 0, no data: every bit before the CRC is dominant.
    #   SOF ID          RTR IDE r0 DLC
    #   0   00000000000 0   0   0  0000        CRC = 0000h (no recessive bit) = 000000000000000
    #   00000[1]00000[1]00000[1]00000[1]00000[1]00000[1]0000
    #   34 bits + 6 stuff bits; 40 + 10 = 50. A recessive stuff bit ends the run it follows,
    #   so here it takes five more dominant bits to earn the next one.
    pytest.param(dict(arbitration_id=0x000), FrameBits(50, 6, 0x0000), id="base-all-dominant"),
    # Base format, identifier 0x078, no data: a stuff bit that starts the next run of five.
    #   0   00001111000 0   0   0  0000        recessive: ID bits with k = 13, 12, 11, 10
    #   CRC = 6CCEh ^ 3667h ^ 79FFh ^ 5E33h = 7D65h = 111110101100101
    #   00000[1]1111[0]0000[1]00000[1]0 11111[0]0101100101
    #   The first stuff bit and the four identifier bits after it are five recessive bits, the
    #   stuff bit after those and four dominant bits are five again. 34 + 5 + 10 = 49.
    pytest.param(dict(arbitration_id=0x078), FrameBits(49, 5, 0x7D65), id="base-stuff-cascade"),
    # Base format, identifier 0x009, no data: the CRC sequence ends in five dominant bits.
    #   0   00000001001 0   0   0  0000        recessive: ID bits with k = 10 and 7
    #   CRC = 5E33h ^ 2213h = 7C20h = 111110000100000
    #   00000[1]000 1 00 1 00000[1]00 11111[0]0000[1]1 00000[1]
    #   The last stuff bit follows the last bit of the CRC sequence and precedes the CRC
    #   delimiter: the run of five is complete, so the transmitter inserts it. 34 + 5 + 10 = 49.
    pytest.param(dict(arbitration_id=0x009), FrameBits(49, 5, 0x7C20), id="base-stuff-after-crc"),
    # Base format, identifier 0, one data byte 0x01: bytes go out most significant bit first,
    # so the recessive data bit is the last bit before the CRC.
    #   0   00000000000 0   0   0  0001  00000001   recessive: DLC0 (k = 8), data bit 0 (k = 0)
    #   CRC = 4426h ^ 4599h = 01BFh = 000000110111111
    #   00000[1]00000[1]00000[1]000 1 00000[1]00 1 00000[1]0 11 0 11111[0]1
    #   42 + 6 + 10 = 58.
    pytest.param(dict(arbitration_id=0x000, data=b"\x01"), FrameBits(58, 6, 0x01BF),
                 id="base-data-0x01"),
    # The same with data byte 0x80: now the recessive data bit comes first (k = 7).
    #   0   00000000000 0   0   0  0001  10000000
    #   CRC = 4426h ^ 2213h = 6635h = 110011000110101
    #   00000[1]00000[1]00000[1]000 11 00000[1]00 110011000110101
    #   42 + 4 + 10 = 56.
    pytest.param(dict(arbitration_id=0x000, data=b"\x80"), FrameBits(56, 4, 0x6635),
                 id="base-data-0x80"),
    # Base format remote frame, identifier 0, DLC 0: only the RTR bit is recessive (k = 6).
    #   0   00000000000 1   0   0  0000        CRC = 73C5h = 111001111000101
    #   00000[1]00000[1]00 1 00000[1]0 111001111000101
    #   34 + 3 + 10 = 47.
    pytest.param(dict(arbitration_id=0x000, is_remote_frame=True, dlc=0),
                 FrameBits(47, 3, 0x73C5), id="base-remote-dlc0"),
    # The same remote frame asking for 8 bytes: the DLC changes (DLC3 recessive, k = 3), a
    # data field does not appear.
    #   0   00000000000 1   0   0  1000        CRC = 73C5h ^ 7407h = 07C2h = 000011111000010
    #   00000[1]00000[1]00 1 00 1 000 00[1]00 11111[0]0000[1]1 0
    #   34 + 5 + 10 = 49: two bits more than with DLC 0, both of them stuff bits.
    pytest.param(dict(arbitration_id=0x000, is_remote_frame=True, dlc=8),
                 FrameBits(49, 5, 0x07C2), id="base-remote-dlc8"),
    # Extended format, identifier 0, no data: SRR and IDE are the only recessive bits.
    #   SOF ID          SRR IDE ID extension       RTR r1 r0 DLC
    #   0   00000000000 1   1   000000000000000000 0   0  0  0000     k = 26 (SRR), 25 (IDE)
    #   CRC = 7BE0h ^ 3DF0h = 4610h = 100011000010000
    #   00000[1]00000[1]00 11 00000[1]00000[1]00000[1]00000[1]00000[1] 100011000010000
    #   54 + 7 + 10 = 71.
    pytest.param(dict(arbitration_id=0x00000000, is_extended_id=True),
                 FrameBits(71, 7, 0x4610), id="extended-id-0"),
    # Extended format remote frame, identifier 0, DLC 0: RTR follows the identifier extension
    # (k = 6); the bit where a base frame has its RTR is SRR and stays recessive.
    #   0   00000000000 1   1   000000000000000000 1   0  0  0000
    #   CRC = 4610h ^ 73C5h = 35D5h = 011010111010101
    #   00000[1]00000[1]00 11 00000[1]00000[1]00000[1]000 1 00000[1]0 011010111010101
    #   54 + 6 + 10 = 70.
    pytest.param(dict(arbitration_id=0x00000000, is_extended_id=True, is_remote_frame=True,
                      dlc=0), FrameBits(70, 6, 0x35D5), id="extended-remote"),
    # Extended format, identifier 0x00040000 = ID-18, the last of the 11 bits sent before
    # SRR and IDE (k = 27).
    #   0   00000000001 1   1   000000000000000000 0   0  0  0000
    #   CRC = 3259h ^ 4610h = 7449h = 111010001001001
    #   00000[1]00000[1]0 111 00000[1]00000[1]00000[1]00000[1]00000[1] 111010001001001
    #   54 + 7 + 10 = 71.
    pytest.param(dict(arbitration_id=0x00040000, is_extended_id=True),
                 FrameBits(71, 7, 0x7449), id="extended-id-18"),
    # Extended format, identifier 0x00020000 = ID-17, the first of the 18 bits sent after
    # SRR and IDE (k = 24).
    #   0   00000000000 1   1   100000000000000000 0   0  0  0000
    #   CRC = 4610h ^ 1EF8h = 58E8h = 101100011101000
    #   00000[1]00000[1]00 111 00000[1]00000[1]00000[1]00000[1]0000 101100011101000
    #   54 + 6 + 10 = 70.
    pytest.param(dict(arbitration_id=0x00020000, is_extended_id=True),
                 FrameBits(70, 6, 0x58E8), id="extended-id-17"),
]


@pytest.mark.parametrize("frame, expected", HAND_VECTORS)
def test_frame_worked_out_by_hand(frame, expected):
    assert canbits.frame_bits(**frame) == expected
    assert len(canbits.wire_bits(**frame)) == expected.frame_bits


def test_a_stuff_bit_follows_the_last_crc_bit_when_that_bit_completes_a_run_of_five():
    # Identifier 0x009 from the hand-worked frames: ... 1 00000 [1] | delimiter and the rest.
    wire = canbits.wire_bits(0x009)
    assert wire[-17:] == [1, 0, 0, 0, 0, 0, 1] + [1] * 10


# ------------------------------------------------------------------------------------------
# Bit stuffing on its own
# ------------------------------------------------------------------------------------------


@pytest.mark.parametrize("unstuffed, stuffed", [
    ("", ""),
    ("0000", "0000"),
    ("00000", "000001"),
    ("000000", "0000010"),
    ("11111", "111110"),
    ("1111111111", "111110111110"),
    # The stuff bit is the first bit of the next run: four more equal bits complete it.
    ("000001111", "00000111110"),
    ("0000011110000", "0000011111000001"),
    # ... and three are not enough.
    ("00000111", "000001111"),
    ("0101010101", "0101010101"),
])
def test_stuffing_of_short_patterns(unstuffed, stuffed):
    assert canbits.stuff([int(c) for c in unstuffed]) == [int(c) for c in stuffed]


@pytest.mark.parametrize("length", range(1, 15))
def test_no_pattern_takes_more_stuff_bits_than_the_worst_case_formula_allows(length):
    # Every pattern of this length, not only frames: the formula floor((s - 1) / 4) of
    # can_bits.h is the exact maximum, reached by 00000 1111 0000 1111 ...
    most = max(
        len(canbits.stuff(list(pattern))) - length
        for pattern in itertools.product((0, 1), repeat=length)
    )
    assert most == (length - 1) // 4


@pytest.mark.parametrize("stuffable, stuff_bits", [(34, 8), (98, 24), (54, 13), (118, 29)])
def test_the_worst_pattern_for_the_stuffed_span_of_a_frame(stuffable, stuff_bits):
    # 34 and 98 bits: base format without data and with 8 bytes; 54 and 118: extended format.
    # After the first five bits every stuff bit is followed by four bits of its own level.
    pattern = [0] * 5
    while len(pattern) < stuffable:
        pattern += [1 - pattern[-1]] * 4
    pattern = pattern[:stuffable]
    assert len(canbits.stuff(pattern)) - stuffable == stuff_bits


# ------------------------------------------------------------------------------------------
# Lengths without stuffing and worst case
# ------------------------------------------------------------------------------------------


def test_lengths_for_eight_data_bytes():
    assert canbits.unstuffed_bits(False, False, 8) == 108
    assert canbits.unstuffed_bits(True, False, 8) == 128
    assert canbits.worst_case_bits(False, False, 8) == 132
    assert canbits.worst_case_bits(True, False, 8) == 157


def test_unstuffed_and_worst_case_lengths_for_every_dlc():
    # 44 + 8n and 64 + 8n; worst case adds floor((34 + 8n - 1) / 4) = 8 + 2n in base format
    # and floor((54 + 8n - 1) / 4) = 13 + 2n in extended format.
    dlcs = range(9)
    assert [canbits.unstuffed_bits(False, False, n) for n in dlcs] == [
        44, 52, 60, 68, 76, 84, 92, 100, 108]
    assert [canbits.unstuffed_bits(True, False, n) for n in dlcs] == [
        64, 72, 80, 88, 96, 104, 112, 120, 128]
    assert [canbits.worst_case_bits(False, False, n) for n in dlcs] == [
        52, 62, 72, 82, 92, 102, 112, 122, 132]
    assert [canbits.worst_case_bits(True, False, n) for n in dlcs] == [
        77, 87, 97, 107, 117, 127, 137, 147, 157]


@pytest.mark.parametrize("dlc", range(9))
def test_a_remote_frame_has_the_length_of_a_frame_without_data_whatever_its_dlc(dlc):
    assert canbits.unstuffed_bits(False, True, dlc) == 44
    assert canbits.unstuffed_bits(True, True, dlc) == 64
    assert canbits.worst_case_bits(False, True, dlc) == 52
    assert canbits.worst_case_bits(True, True, dlc) == 77


def test_a_dlc_above_eight_counts_as_eight_in_the_formulas():
    for dlc in (9, 12, 15):
        assert canbits.unstuffed_bits(False, False, dlc) == 108
        assert canbits.worst_case_bits(True, False, dlc) == 157


def test_the_formulas_refuse_arguments_in_the_wrong_order():
    # dlc first is the natural mistake; a bool where the DLC belongs must not pass as 0 or 1.
    with pytest.raises(TypeError):
        canbits.unstuffed_bits(8, False, False)
    with pytest.raises(TypeError):
        canbits.worst_case_bits(8, True, True)
    with pytest.raises(ValueError):
        canbits.unstuffed_bits(False, False, -1)


# ------------------------------------------------------------------------------------------
# Properties over seeded random frames
# ------------------------------------------------------------------------------------------

# Bytes with long runs of equal bits: uniformly random data almost never needs many stuff bits.
RUNNY_BYTES = bytes([0x00, 0xFF, 0x0F, 0xF0, 0x07, 0xE0, 0x1F, 0xF8, 0x83, 0x7C, 0x3E, 0xC1])
FRAMES_PER_CASE = 40


def random_frames(seed: int, extended: bool, remote: bool, dlc: int):
    """Identifiers and data for one (format, kind, DLC) case: the corners of the identifier
    range, then seeded random frames, half of them with data chosen to provoke stuffing."""
    rng = random.Random(seed)
    id_max = 0x1FFFFFFF if extended else 0x7FF
    for index in range(FRAMES_PER_CASE):
        ident = (0, id_max)[index] if index < 2 else rng.randint(0, id_max)
        source = RUNNY_BYTES if index % 2 else bytes(range(256))
        data = b"" if remote else bytes(rng.choice(source) for _ in range(dlc))
        yield ident, data


ALL_CASES = [
    pytest.param(extended, remote, dlc,
                 id=f"{'ext' if extended else 'base'}-{'remote' if remote else 'data'}-dlc{dlc}")
    for extended in (False, True) for remote in (False, True) for dlc in range(9)
]


@pytest.mark.parametrize("extended, remote, dlc", ALL_CASES)
def test_exact_length_lies_between_unstuffed_and_worst_case(extended, remote, dlc):
    seed = 0xCA9B175 + 100 * dlc + 10 * extended + remote
    for ident, data in random_frames(seed, extended, remote, dlc):
        info = canbits.frame_bits(ident, data, extended, remote, dlc)
        unstuffed = canbits.unstuffed_bits(extended, remote, dlc)
        worst = canbits.worst_case_bits(extended, remote, dlc)
        assert unstuffed <= info.frame_bits <= worst, (hex(ident), data.hex())
        assert info.frame_bits == unstuffed + info.stuff_bits, (hex(ident), data.hex())


@pytest.mark.parametrize("extended, remote, dlc", ALL_CASES)
def test_the_bits_on_the_wire_are_the_stuffed_fields_and_crc_and_a_recessive_tail(
        extended, remote, dlc):
    seed = 0x57FF + 100 * dlc + 10 * extended + remote
    for ident, data in random_frames(seed, extended, remote, dlc):
        wire = canbits.wire_bits(ident, data, extended, remote, dlc)
        info = canbits.frame_bits(ident, data, extended, remote, dlc)
        context = (hex(ident), data.hex())

        assert len(wire) == info.frame_bits, context
        # CRC delimiter, ACK slot as the transmitter sends it, ACK delimiter, end of frame.
        assert wire[-10:] == [1] * 10, context

        stuffed = wire[:-10]
        # A receiver must never see six equal bits before the CRC delimiter (stuff error).
        assert longest_run(stuffed) <= 5, context
        fields = [int(c) for c in fields_as_text(ident, data, extended, remote, dlc)]
        crc = crc15_by_long_division(fields)
        assert info.crc15 == crc, context
        assert destuff(stuffed) == fields + [int(c) for c in f"{crc:015b}"], context
        assert info.stuff_bits == len(stuffed) - len(fields) - 15, context


def test_the_shift_register_agrees_with_polynomial_division_on_random_bit_streams():
    rng = random.Random(0xC4C15)
    for _ in range(300):
        bits = [rng.getrandbits(1) for _ in range(rng.randint(0, 130))]
        assert canbits.crc15(bits) == crc15_by_long_division(bits), bits


@pytest.mark.parametrize("extended", [False, True])
@pytest.mark.parametrize("dlc", range(9))
def test_remote_frames_carry_no_data_bits(extended, dlc):
    rng = random.Random(0x2E307E + dlc + 16 * extended)
    without_stuffing = 64 if extended else 44
    for _ in range(FRAMES_PER_CASE):
        ident = rng.randint(0, 0x1FFFFFFF if extended else 0x7FF)
        info = canbits.frame_bits(ident, b"", extended, True, dlc)
        assert info.frame_bits - info.stuff_bits == without_stuffing
        # Whatever is passed as data is not transmitted: only the DLC is.
        ignored = bytes(rng.randrange(256) for _ in range(dlc))
        assert canbits.frame_bits(ident, ignored, extended, True, dlc) == info
        assert canbits.frame_bits(ident, ignored, extended, True) == info
        # The corresponding data frame is longer by exactly its data field, stuffing aside.
        data_frame = canbits.frame_bits(ident, ignored, extended, False, dlc)
        assert data_frame.frame_bits - data_frame.stuff_bits == without_stuffing + 8 * dlc


# ------------------------------------------------------------------------------------------
# Arguments
# ------------------------------------------------------------------------------------------


def test_the_dlc_defaults_to_the_number_of_data_bytes():
    data = bytes.fromhex("0011223344")
    assert canbits.frame_bits(0x222, data, dlc=5) == canbits.frame_bits(0x222, data)
    assert canbits.frame_bits(0x222, bytearray(data)) == canbits.frame_bits(0x222, data)
    assert canbits.frame_bits(0x222, list(data)) == canbits.frame_bits(0x222, data)


@pytest.mark.parametrize("frame", [
    dict(arbitration_id=0x800),                                  # 12 bits in base format
    dict(arbitration_id=0x20000000, is_extended_id=True),       # 30 bits
    dict(arbitration_id=-1),
    dict(arbitration_id=0x123, data=bytes(9)),                   # more than 8 data bytes
    dict(arbitration_id=0x123, data=bytes(3), dlc=4),            # DLC and data disagree
    dict(arbitration_id=0x123, data=bytes(3), dlc=2),
    dict(arbitration_id=0x123, is_remote_frame=True, dlc=9),     # "other values may not be used"
    dict(arbitration_id=0x123, is_remote_frame=True, dlc=-1),
], ids=["base-0x800", "extended-30-bits", "negative-identifier", "9-data-bytes", "dlc-above-data",
        "dlc-below-data", "remote-dlc-9", "remote-negative-dlc"])
def test_a_frame_that_cannot_exist_is_refused(frame):
    with pytest.raises(ValueError):
        canbits.frame_bits(**frame)
    with pytest.raises(ValueError):
        canbits.wire_bits(**frame)


def test_a_number_where_the_data_belongs_is_refused():
    # frame_bits(identifier, dlc) is the natural slip, and bytes(8) would be eight zero bytes:
    # a plausible frame of the wrong length.
    with pytest.raises(TypeError):
        canbits.frame_bits(0x123, 8)
    with pytest.raises(TypeError):
        canbits.wire_bits(0x123, 0)


def test_the_largest_identifiers_are_accepted():
    assert canbits.frame_bits(0x7FF).frame_bits >= 44
    assert canbits.frame_bits(0x1FFFFFFF, is_extended_id=True).frame_bits >= 64


# ------------------------------------------------------------------------------------------
# Durations, python-can messages and bus load
# ------------------------------------------------------------------------------------------


def test_duration_is_bits_over_bit_rate():
    assert canbits.duration_s(87, 500_000) == pytest.approx(174e-6)     # 87 bits of 2 us
    assert canbits.duration_s(135, 125_000) == pytest.approx(1.08e-3)   # 135 bits of 8 us
    assert canbits.duration_s(0, 1_000_000) == 0.0
    with pytest.raises(ValueError):
        canbits.duration_s(87, 0)
    with pytest.raises(ValueError):
        canbits.duration_s(-1, 500_000)


def test_message_bits_measures_a_python_can_message():
    data_frame = can.Message(arbitration_id=0x222, is_extended_id=False,
                             data=bytes.fromhex("0011223344"))
    assert canbits.message_bits(data_frame) == FrameBits(87, 3, 0x66DA)

    extended = can.Message(arbitration_id=0x11223344, is_extended_id=True,
                           data=bytes.fromhex("00112233445566"))
    assert canbits.message_bits(extended) == FrameBits(123, 3, 0x0D30)

    # A remote message has no data; its DLC still reaches the frame (hand-worked above).
    remote = can.Message(arbitration_id=0x000, is_extended_id=False, is_remote_frame=True,
                         dlc=8)
    assert canbits.message_bits(remote) == FrameBits(49, 5, 0x07C2)


@pytest.mark.parametrize("message", [
    can.Message(arbitration_id=0x123, is_extended_id=False, is_fd=True, data=bytes(12)),
    can.Message(arbitration_id=0x123, is_extended_id=False, is_fd=True, data=bytes(2)),
    can.Message(arbitration_id=0x004, is_extended_id=True, is_error_frame=True, dlc=8,
                data=bytes(8)),
])
def test_message_bits_refuses_fd_and_error_frames(message):
    with pytest.raises(ValueError):
        canbits.message_bits(message)


def test_bus_load_of_three_frames_worked_out_by_hand():
    # 87 + 123 + 50 = 260 bits of frames (the two captures and the all-dominant frame above),
    # plus 3 bits of intermission after each: 269 bit times. At 250 kbit/s a 2 ms window is
    # 500 bit times: 269 / 500 = 0.538 with intermission, 260 / 500 = 0.52 without.
    messages = [
        can.Message(arbitration_id=0x222, is_extended_id=False,
                    data=bytes.fromhex("0011223344")),
        can.Message(arbitration_id=0x11223344, is_extended_id=True,
                    data=bytes.fromhex("00112233445566")),
        can.Message(arbitration_id=0x000, is_extended_id=False),
    ]
    assert canbits.bus_load(messages, 0.002, 250_000) == pytest.approx(0.538)
    assert canbits.bus_load(messages, 0.002, 250_000, include_intermission=False) == \
        pytest.approx(0.52)

    # The same frames as bit counts, as FrameBits, and mixed, from an iterator.
    assert canbits.bus_load([87, 123, 50], 0.002, 250_000) == pytest.approx(0.538)
    mixed = iter([canbits.message_bits(messages[0]), 123, messages[2]])
    assert canbits.bus_load(mixed, 0.002, 250_000) == pytest.approx(0.538)


def test_bus_load_of_a_worst_case_schedule():
    # Ten 8-byte base frames every 10 ms at 500 kbit/s, each taken at its worst case:
    # 10 * (132 + 3) = 1350 bit times of 5000.
    worst = canbits.worst_case_bits(False, False, 8)
    assert canbits.bus_load([worst] * 10, 0.010, 500_000) == pytest.approx(0.27)


def test_bus_load_of_an_idle_bus_is_zero_and_an_impossible_schedule_exceeds_one():
    assert canbits.bus_load([], 1.0, 500_000) == 0.0
    # 100 frames of 135 bit times cannot fit into 10,000 bit times; the answer says so.
    assert canbits.bus_load([132] * 100, 0.020, 500_000) == pytest.approx(1.35)


def test_bus_load_takes_integers_of_any_kind_and_refuses_what_is_no_bit_count():
    class Count:
        """An integer that is not an int, as the elements of a numpy array are."""

        def __init__(self, value: int) -> None:
            self._value = value

        def __index__(self) -> int:
            return self._value

    assert canbits.bus_load([Count(87), Count(123), 50], 0.002, 250_000) == pytest.approx(0.538)
    for not_a_bit_count in (87.0, "87", None):
        with pytest.raises(TypeError):
            canbits.bus_load([not_a_bit_count], 0.002, 250_000)


def test_bus_load_refuses_a_window_or_bit_rate_that_is_not_positive():
    with pytest.raises(ValueError):
        canbits.bus_load([87], 0.0, 500_000)
    with pytest.raises(ValueError):
        canbits.bus_load([87], 0.01, 0)
    with pytest.raises(ValueError):
        canbits.bus_load([-5], 0.01, 500_000)


def test_the_intermission_is_three_bits():
    assert canbits.INTERMISSION_BITS == 3
