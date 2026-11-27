# SPDX-License-Identifier: MIT
"""How many bit times a classical CAN frame occupies on the bus: an independent reference.

The firmware works out frame lengths in C (lib/can/can_bits.c) for its bus-load and timing
figures. This module answers the same question a second time. It was written from the Bosch
CAN Specification 2.0, Part B (3.2.1 DATA FRAME, 3.2.2 REMOTE FRAME, 3.2.5 INTERFRAME SPACING,
6 CODING) and from the definition of what is counted in lib/can/can_bits.h, without looking at
the C implementation. The two are compared on random frames, and that comparison is only worth
something while neither is derived from the other: when they disagree, do not port code from
one to the other, find out which one the specification agrees with.

It is written to be read next to the specification, not to be fast. A frame is built as a list
of bits in the order they are transmitted (0 = dominant, 1 = recessive), the CRC is the
specification's shift register clocked once per bit, stuff bits are inserted by walking the
list, and the result is counted.

Fields of a frame and their widths in bits::

    base format      SOF 1 | identifier 11 | RTR 1 | IDE 1 | r0 1 | DLC 4 | data 8n | CRC 15
    extended format  SOF 1 | identifier 11 | SRR 1 | IDE 1 | identifier 18 | RTR 1 | r1 1 |
                     r0 1 | DLC 4 | data 8n | CRC 15
    both, then       CRC delimiter 1 | ACK slot 1 | ACK delimiter 1 | end of frame 7

Everything up to and including the CRC sequence is subject to bit stuffing; the ten bits after
it have a fixed form and are never stuffed. A remote frame has the same fields without the data
field, whatever its DLC says.
"""
from __future__ import annotations

import operator
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:  # only message_bits() and bus_load() see python-can objects, and only duck-typed
    import can

DOMINANT = 0
RECESSIVE = 1

#: Bits of the interframe space that separate two frames (3.2.5: "INTERMISSION consists of
#: three recessive bits"). They belong to no frame, but no other frame can use them either.
INTERMISSION_BITS = 3

_BASE_ID_BITS = 11
_ID_EXTENSION_BITS = 18
_DLC_BITS = 4
_CRC_BITS = 15
_MAX_DATA_BYTES = 8
_BASE_ID_MAX = (1 << _BASE_ID_BITS) - 1
_EXTENDED_ID_MAX = (1 << (_BASE_ID_BITS + _ID_EXTENSION_BITS)) - 1

# Start of frame to the last bit of the DLC: what precedes the data field.
_BASE_HEADER_BITS = 1 + _BASE_ID_BITS + 1 + 1 + 1 + _DLC_BITS  # SOF, ID, RTR, IDE, r0, DLC
_EXTENDED_HEADER_BITS = (
    1 + _BASE_ID_BITS + 1 + 1 + _ID_EXTENSION_BITS + 1 + 1 + 1 + _DLC_BITS
)  # SOF, ID, SRR, IDE, ID extension, RTR, r1, r0, DLC

# CRC delimiter, ACK slot, ACK delimiter and the seven bits of end of frame.
_TAIL_BITS = 1 + 1 + 1 + 7

# The generator polynomial x^15 + x^14 + x^10 + x^8 + x^7 + x^4 + x^3 + 1 without its x^15
# term, which is the bit shifted out of the register: "4599hex" in the specification.
_CRC_GENERATOR = 0x4599

Data = bytes | bytearray | Sequence[int]


@dataclass(frozen=True)
class FrameBits:
    """Length of one frame on the wire; the fields mirror can_bits_info_t of can_bits.h."""

    frame_bits: int  #: start of frame to end of frame, stuff bits included
    stuff_bits: int  #: stuff bits inserted between start of frame and end of CRC
    crc15: int       #: the frame's CRC-15 sequence


def crc15(bits: Iterable[int]) -> int:
    """CRC sequence of a bit stream, by the shift register of section 3.2.1.

    The specification gives the register in pseudo-code: for every bit NXTBIT of the destuffed
    stream from start of frame to the end of the data field,
    ``CRCNXT = NXTBIT EXOR CRC_RG(14); CRC_RG = CRC_RG << 1; IF CRCNXT THEN CRC_RG ^= 4599hex``.
    The register starts at zero and is 15 bits wide.
    """
    register = 0
    for bit in bits:
        feedback = bit ^ (register >> (_CRC_BITS - 1))
        register = (register << 1) & ((1 << _CRC_BITS) - 1)
        if feedback:
            register ^= _CRC_GENERATOR
    return register


def stuff(bits: Iterable[int]) -> list[int]:
    """The bit stream with stuff bits inserted (section 6, BIT STREAM CODING).

    "Whenever a transmitter detects five consecutive bits of identical value in the bit stream
    to be transmitted it automatically inserts a complementary bit." The stream to be
    transmitted includes the stuff bits already inserted, so a stuff bit is the first bit of
    the next run: 00000 1111 goes out as 00000[1]1111[0]. A run that completes on the very last
    bit still gets its stuff bit: for a frame that is the last bit of the CRC sequence, and the
    stuff bit is sent before the CRC delimiter.
    """
    out: list[int] = []
    run_level: int | None = None
    run_length = 0
    for bit in bits:
        out.append(bit)
        if bit == run_level:
            run_length += 1
        else:
            run_level, run_length = bit, 1
        if run_length == 5:
            run_level, run_length = 1 - bit, 1
            out.append(run_level)
    return out


def wire_bits(
    arbitration_id: int,
    data: Data = b"",
    is_extended_id: bool = False,
    is_remote_frame: bool = False,
    dlc: int | None = None,
) -> list[int]:
    """The frame as its transmitter drives it: one element per bit time, start of frame first.

    The ACK slot is recessive here because that is what the transmitter sends; a receiver
    overwrites it with a dominant bit, which does not change the length. Arguments and errors
    are those of frame_bits().
    """
    _, _, stuffed = _build(arbitration_id, data, is_extended_id, is_remote_frame, dlc)
    return stuffed + [RECESSIVE] * _TAIL_BITS


def frame_bits(
    arbitration_id: int,
    data: Data = b"",
    is_extended_id: bool = False,
    is_remote_frame: bool = False,
    dlc: int | None = None,
) -> FrameBits:
    """Exact length of a data or remote frame on the wire, with its stuff bit count and CRC.

    `dlc` defaults to the number of data bytes. For a data frame it has to equal that number.
    A remote frame transmits its DLC but no data field, so `data` is not looked at beyond
    supplying that default: pass the DLC of the frame that is requested with `dlc`.

    Raises ValueError for a frame that cannot exist: an identifier that does not fit its
    format, more than 8 data bytes, or a DLC above 8 (3.2.1: "admissible numbers of data
    bytes: {0,1,....,7,8}. Other values may not be used"). Every identifier that fits its
    format is accepted. 3.2.1 also asks that the seven most significant bits of a base
    identifier are not all recessive; like can_frame_is_valid(), this module does not apply
    that rule, so 0x7F0 to 0x7FF are identifiers like any other.
    """
    unstuffed, crc, stuffed = _build(arbitration_id, data, is_extended_id, is_remote_frame, dlc)
    return FrameBits(
        frame_bits=len(stuffed) + _TAIL_BITS,
        stuff_bits=len(stuffed) - len(unstuffed) - _CRC_BITS,
        crc15=crc,
    )


def unstuffed_bits(is_extended_id: bool, is_remote_frame: bool, dlc: int) -> int:
    """Length without any stuff bit: 44 + 8n bits in base format, 64 + 8n in extended format.

    n is the number of data bytes: `dlc` for a data frame, 0 for a remote frame. A `dlc` above
    8 is treated as 8, as can_bits_unstuffed() does. The arguments are in the order of the C
    function.
    """
    header = _EXTENDED_HEADER_BITS if is_extended_id else _BASE_HEADER_BITS
    return header + 8 * _data_bytes(is_remote_frame, dlc) + _CRC_BITS + _TAIL_BITS


def worst_case_bits(is_extended_id: bool, is_remote_frame: bool, dlc: int) -> int:
    """Upper bound of the length for any content: the figure for worst-case bus-load sums.

    Of the s bits between start of frame and the end of the CRC sequence (s = 34 + 8n in base
    format, 54 + 8n in extended format), the first stuff bit can follow bit 5 at the earliest.
    From then on a stuff bit can follow every 4 further bits, because it starts the next run
    of five itself. That allows floor((s - 1) / 4) stuff bits. No real frame reaches the bound
    for every DLC, because the CRC cannot be chosen freely.
    """
    unstuffed = unstuffed_bits(is_extended_id, is_remote_frame, dlc)
    stuffable = unstuffed - _TAIL_BITS
    return unstuffed + (stuffable - 1) // 4


def duration_s(bits: int, bitrate: float) -> float:
    """Time that `bits` bit times take at `bitrate` bit/s, in seconds."""
    if bits < 0:
        raise ValueError(f"a bit count cannot be negative: {bits}")
    if bitrate <= 0:
        raise ValueError(f"the bit rate must be positive: {bitrate}")
    return bits / bitrate


def message_bits(msg: can.Message) -> FrameBits:
    """frame_bits() of a python-can message.

    Raises ValueError for a message that is not a classical data or remote frame: CAN FD
    frames have another format, and an error frame is not a frame with a length to look up.
    """
    if msg.is_fd or msg.is_error_frame:
        raise ValueError(f"not a classical CAN data or remote frame: {msg!r}")
    return frame_bits(
        msg.arbitration_id,
        msg.data,
        is_extended_id=msg.is_extended_id,
        is_remote_frame=msg.is_remote_frame,
        dlc=msg.dlc,
    )


def bus_load(
    messages_or_bit_counts: Iterable[int | FrameBits | can.Message],
    window_s: float,
    bitrate: float,
    include_intermission: bool = True,
) -> float:
    """Fraction of a time window during which the frames given occupy the bus.

    Each item is a python-can message (its exact length is computed), the FrameBits of a frame,
    or a number of bits, for example worst_case_bits() of a frame that is only planned. With
    `include_intermission` every frame also accounts for the three recessive bits that must
    follow it before the next frame may start: that is the load to compare against 100 %,
    because a bus whose frames follow each other without any other gap is full.

    The result is not clamped. A value above 1 says the frames cannot all have been on a bus
    of this bit rate within the window.
    """
    if window_s <= 0:
        raise ValueError(f"the window must be longer than zero: {window_s}")
    per_frame_gap = INTERMISSION_BITS if include_intermission else 0
    occupied_bits = 0
    for item in messages_or_bit_counts:
        if isinstance(item, FrameBits):
            length = item.frame_bits
        elif hasattr(item, "arbitration_id"):
            length = message_bits(item).frame_bits
        else:
            # An integer of any kind, numpy's included. A float is refused with a TypeError:
            # there is no such thing as 87.5 bits, and 87.0 is more likely a duration.
            length = operator.index(item)
        if length < 0:
            raise ValueError(f"a bit count cannot be negative: {length}")
        occupied_bits += length + per_frame_gap
    return duration_s(occupied_bits, bitrate) / window_s


def _build(
    arbitration_id: int,
    data: Data,
    is_extended_id: bool,
    is_remote_frame: bool,
    dlc: int | None,
) -> tuple[list[int], int, list[int]]:
    """Returns (bits from start of frame to end of data, CRC, the same bits plus the CRC
    sequence after stuffing)."""
    id_max = _EXTENDED_ID_MAX if is_extended_id else _BASE_ID_MAX
    if not 0 <= arbitration_id <= id_max:
        raise ValueError(
            f"identifier {arbitration_id:#x} does not fit the "
            f"{'extended' if is_extended_id else 'base'} format (0..{id_max:#x})"
        )
    if isinstance(data, int):
        # bytes(5) is five zero bytes: frame_bits(identifier, dlc) would quietly be a frame.
        raise TypeError(f"data must be bytes or a sequence of byte values, not the number {data}")
    payload = bytes(data)
    if dlc is None:
        dlc = len(payload)
    if not 0 <= dlc <= _MAX_DATA_BYTES:
        raise ValueError(f"a classical CAN frame has a DLC of 0..8, not {dlc}")
    if is_remote_frame:
        payload = b""
    elif len(payload) != dlc:
        raise ValueError(f"DLC {dlc} does not match {len(payload)} data byte(s)")

    rtr = RECESSIVE if is_remote_frame else DOMINANT
    bits = [DOMINANT]  # start of frame
    if is_extended_id:
        bits += _msb_first(arbitration_id >> _ID_EXTENSION_BITS, _BASE_ID_BITS)  # ID-28..ID-18
        bits += [RECESSIVE, RECESSIVE]  # SRR, IDE
        bits += _msb_first(arbitration_id, _ID_EXTENSION_BITS)  # ID-17..ID-0
        bits += [rtr, DOMINANT, DOMINANT]  # RTR, r1, r0
    else:
        bits += _msb_first(arbitration_id, _BASE_ID_BITS)  # ID-28..ID-18
        bits += [rtr, DOMINANT, DOMINANT]  # RTR, IDE, r0
    bits += _msb_first(dlc, _DLC_BITS)
    for byte in payload:
        bits += _msb_first(byte, 8)

    crc = crc15(bits)
    return bits, crc, stuff(bits + _msb_first(crc, _CRC_BITS))


def _msb_first(value: int, width: int) -> list[int]:
    """The low `width` bits of `value`, most significant first: the order of every field."""
    return [(value >> shift) & 1 for shift in range(width - 1, -1, -1)]


def _data_bytes(is_remote_frame: bool, dlc: int) -> int:
    # bool is an int in Python: without this check a call with the arguments in another order,
    # unstuffed_bits(8, False, False), would quietly be a frame with DLC 0.
    if isinstance(dlc, bool) or not isinstance(dlc, int):
        raise TypeError(f"dlc must be an integer, not {dlc!r}")
    if dlc < 0:
        raise ValueError(f"a DLC cannot be negative: {dlc}")
    return 0 if is_remote_frame else min(dlc, _MAX_DATA_BYTES)
