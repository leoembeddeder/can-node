# SPDX-License-Identifier: MIT
"""AUTOSAR E2E Profile 11, sender and receiver side: an independent reference for the tools.

The firmware protects its cyclic CAN messages with lib/e2e (variant 11A only). The bus tools
need the same protection to check what the node sends and to send it frames it accepts, and
the C code needs a second implementation to be compared with. This module is both. It was
written from the specification text, without looking at lib/e2e:

  - PRS  AUTOSAR_FO_PRS_E2EProtocol R24-11, section 6.12 (Profile 11): the layout
         (PRS_E2E_00540, PRS_E2E_00541), the counter (PRS_E2E_00504), the Data ID modes
         (PRS_E2E_00583, PRS_E2E_00505, PRS_E2E_00506) and the flow charts of protect and
         check, Fig. 6.93 to 6.110, step by step;
  - LIB  AUTOSAR_CP_SWS_E2ELibrary R24-11: the initial states (SWS_E2E_00555,
         SWS_E2E_00556) and the numeric status values (SWS_E2E_00564);
  - CRC  AUTOSAR_CP_SWS_CRCLibrary R23-11: Crc_CalculateCRC8 (SWS_Crc_00014, SWS_Crc_00041,
         SWS_Crc_00073), which the profile calls.

It implements the profile as the PRS defines it, with configurable offsets and both Data ID
modes, so that every test vector the PRS publishes can be run, including the one whose CRC is
byte 8 of a 16-byte message. The firmware's module is the special case 11A (Data ID mode BOTH,
CRC in byte 0, counter in the low nibble of byte 1) for messages of 2 to 8 bytes.
tests/python/test_e2e.py compares the two through tests/tools/e2e_cli.c, and that comparison
is only worth something while neither is derived from the other: when they disagree, do not
port code from one to the other, find out which one the specification agrees with.

Bit offsets count from the least significant bit of byte 0: byte = offset >> 3, shift within
the byte = offset & 7 (Fig. 6.96, 6.108). Fig. 6.91 draws the bits of a byte the other way
round; the formulas and the published vectors decide.

Two rules are this project's own, where the specification leaves a case open. Both are made
in the same way here and in lib/e2e, and both are documented there:

  - A received counter of 0xF (reserved, PRS 6.12.1.1, never sent) gives ERROR, and the
    stored counter is kept. Fig. 6.110 does not test for it at all.
  - max_delta_counter must be 1 to 14: 0 makes OK impossible, and above 14 it means the same
    as 14. A configuration outside that range raises ValueError instead of being used.

Errors of the caller (a wrong length, an impossible state, a configuration that does not fit
the message) raise ValueError. In the AUTOSAR library they are the return codes
E2E_E_INPUTERR_NULL and E2E_E_INPUTERR_WRONG, and the receiver's status is left as it was.
"""
from __future__ import annotations

import enum
from collections.abc import Sequence
from dataclasses import dataclass

# ------------------------------------------------------------------------------------------
# The CRC: Crc_CalculateCRC8 of the AUTOSAR CRC library
# ------------------------------------------------------------------------------------------

#: CRC-8-SAE J1850 as the CRC library defines it (SWS_Crc_00073): x^8 + x^4 + x^3 + x^2 + 1,
#: not reflected, initial value 0xFF, final XOR 0xFF, check value 0x4B.
J1850_POLY = 0x1D
J1850_INIT = 0xFF
J1850_XOR = 0xFF


def crc_calculate_crc8(data: bytes | Sequence[int], start_value: int, is_first_call: bool) -> int:
    """Crc_CalculateCRC8(Crc_DataPtr, Crc_Length, Crc_StartValue8, Crc_IsFirstCall).

    SWS_Crc_00014: a first call starts from the polynomial's initial value. SWS_Crc_00041: a
    later call starts from Crc_StartValue8, "the CRC result of the last call", XORed with the
    XOR value, which undoes the final XOR that call applied. Then every byte is divided in,
    most significant bit first, and the result is XORed with the XOR value.
    """
    register = J1850_INIT if is_first_call else (start_value ^ J1850_XOR)
    for byte in bytes(data):
        register ^= byte
        for _ in range(8):
            carry = register & 0x80
            register = (register << 1) & 0xFF
            if carry:
                register ^= J1850_POLY
    return register ^ J1850_XOR


# ------------------------------------------------------------------------------------------
# Configuration and status
# ------------------------------------------------------------------------------------------


class DataIdMode(enum.IntEnum):
    """E2E_P11DataIDMode (SWS_E2E_00566)."""

    BOTH = 0    #: both bytes of the 16-bit Data ID enter the CRC; nothing of it is sent
    NIBBLE = 3  #: 12-bit Data ID; its high nibble is sent, its low byte enters the CRC


class Status(enum.IntEnum):
    """E2E_P11CheckStatusType, with the values of the library (SWS_E2E_00564)."""

    OK = 0x00
    NONEWDATA = 0x01
    ERROR = 0x07
    REPEATED = 0x08
    OKSOMELOST = 0x20
    WRONGSEQUENCE = 0x40


#: The counter runs 0, 1, ..., 14, 0, ... (PRS_E2E_00504; Fig. 6.99: "State->Counter %= 15").
COUNTER_MODULUS = 15
COUNTER_MAX = COUNTER_MODULUS - 1
#: "the counter value 0x0F is reserved as a special invalid value" (PRS 6.12.1.1).
COUNTER_RESERVED = 0x0F
#: E2E_P11ProtectInit sets the counter to 0 (SWS_E2E_00555): the first frame carries 0.
SENDER_INITIAL_COUNTER = 0
#: E2E_P11CheckInit sets Counter to 0xE and Status to ERROR (SWS_E2E_00556), so that a first
#: frame with counter 0 is one step ahead.
RECEIVER_INITIAL_COUNTER = 0x0E
RECEIVER_INITIAL_STATUS = Status.ERROR

#: The profile allows up to 256 bits (PRS_E2E_00893: DataLength "shall be <= 256").
MAX_DATA_LENGTH_BITS = 256


@dataclass(frozen=True)
class Config:
    """E2E_P11ConfigType (PRS_E2E_00893). Lengths and offsets are in bits, as there.

    The defaults are variant 11A (PRS_E2E_00540): CRC in byte 0, counter in the low nibble of
    byte 1, Data ID mode BOTH. data_id_nibble_offset is only used in NIBBLE mode (11C puts the
    nibble in the high nibble of byte 1, offset 12).
    """

    data_id: int
    data_length: int = 64
    max_delta_counter: int = 1
    data_id_mode: DataIdMode = DataIdMode.BOTH
    crc_offset: int = 0
    counter_offset: int = 8
    data_id_nibble_offset: int = 12

    def __post_init__(self) -> None:
        id_bits = 16 if self.data_id_mode == DataIdMode.BOTH else 12
        if not 0 <= self.data_id < (1 << id_bits):
            raise ValueError(f"Data ID {self.data_id:#x} does not fit {id_bits} bits "
                             f"(mode {self.data_id_mode.name})")
        if self.data_length % 8 or not 8 <= self.data_length <= MAX_DATA_LENGTH_BITS:
            raise ValueError(f"data_length is {self.data_length} bits: it has to be a multiple "
                             f"of 8 from 8 to {MAX_DATA_LENGTH_BITS}")
        if not 1 <= self.max_delta_counter <= COUNTER_MAX:
            raise ValueError(f"max_delta_counter {self.max_delta_counter} is outside 1..14")
        if self.crc_offset % 8:
            raise ValueError(f"crc_offset {self.crc_offset} is not a multiple of 8")
        # The CRC byte and the nibbles must lie inside the message and must not overlap.
        fields = [("CRC", self.crc_offset, 8), ("counter", self.counter_offset, 4)]
        if self.data_id_mode == DataIdMode.NIBBLE:
            fields.append(("Data ID nibble", self.data_id_nibble_offset, 4))
        for name, offset, width in fields:
            if width == 4 and offset % 4:
                raise ValueError(f"{name} offset {offset} is not a multiple of 4")
            if not 0 <= offset <= self.data_length - width:
                raise ValueError(f"{name} at bit {offset} is outside a message of "
                                 f"{self.data_length} bits")
        for i, (name_a, offset_a, width_a) in enumerate(fields):
            for name_b, offset_b, width_b in fields[i + 1:]:
                if offset_a < offset_b + width_b and offset_b < offset_a + width_a:
                    raise ValueError(f"{name_a} and {name_b} overlap")

    @property
    def length_bytes(self) -> int:
        return self.data_length // 8


# ------------------------------------------------------------------------------------------
# The steps of the flow charts
# ------------------------------------------------------------------------------------------


def _read_nibble(data: bytes | bytearray, offset: int) -> int:
    """Fig. 6.107, 6.108: (Data[offset >> 3] >> (offset & 7)) & 0xF."""
    return (data[offset >> 3] >> (offset & 7)) & 0x0F


def _write_nibble(data: bytearray, offset: int, value: int) -> None:
    """Fig. 6.95, 6.96: the four bits at `offset` become `value`; the other four bits of the
    byte keep what the caller put there."""
    shift = offset & 7
    data[offset >> 3] = (data[offset >> 3] & ~(0x0F << shift) & 0xFF) | ((value & 0x0F) << shift)


def compute_crc(config: Config, data: bytes | bytearray) -> int:
    """The step "Compute CRC" of protect and check, Fig. 6.97, box by box.

    Every call is a "subsequent" call (Crc_IsFirstCall FALSE), the first one with
    Crc_StartValue8 0xFF, and the result is XORed with 0xFF at the end. Worked through the
    library's rules this is CRC-8 with polynomial 0x1D, initial value 0x00 and no final XOR
    (CRC-8/GSM-A in the CRC catalogues); tests/python/test_e2e.py shows it.
    """
    offset = config.crc_offset // 8
    length = config.length_bytes
    if config.data_id_mode == DataIdMode.BOTH:
        crc = crc_calculate_crc8([config.data_id & 0xFF], 0xFF, False)
        crc = crc_calculate_crc8([(config.data_id >> 8) & 0xFF], crc, False)
    else:
        crc = crc_calculate_crc8([config.data_id & 0xFF], 0xFF, False)
        crc = crc_calculate_crc8([0], crc, False)
    if offset > 0:
        crc = crc_calculate_crc8(data[0:offset], crc, False)
        if length > offset + 1:
            crc = crc_calculate_crc8(data[offset + 1:length], crc, False)
    else:
        crc = crc_calculate_crc8(data[1:length], crc, False)
    return crc ^ 0xFF


def data_id_crc(data_id: int) -> int:
    """The CRC after the two Data ID bytes of mode BOTH, before any byte of the message.

    Two Data IDs with the same value here protect every message in exactly the same way: a
    frame of one is accepted as a frame of the other (incorrect addressing goes unnoticed).
    The Data IDs of the protected messages on one bus therefore have to differ in this value
    (PRS_E2E_UC_00072), and the generator of the message table checks that with this function.
    """
    if not 0 <= data_id <= 0xFFFF:
        raise ValueError(f"Data ID {data_id:#x} does not fit 16 bits")
    # The first two boxes of Fig. 6.97 (mode BOTH), and the final XOR of the last box: the
    # register of the effective CRC (initial value 0, no final XOR) after the two bytes.
    crc = crc_calculate_crc8([data_id & 0xFF, data_id >> 8], 0xFF, False)
    return crc ^ 0xFF


def _require_length(config: Config, data: bytes | bytearray) -> None:
    if len(data) != config.length_bytes:
        raise ValueError(f"{len(data)} byte(s) given, the configuration says "
                         f"{config.length_bytes} (data_length {config.data_length} bits)")


def _require_counter(counter: int, whose: str) -> None:
    if not 0 <= counter <= COUNTER_MAX:
        raise ValueError(f"{whose} counter {counter} is outside 0..{COUNTER_MAX}")


# ------------------------------------------------------------------------------------------
# E2E_P11Protect and E2E_P11Check
# ------------------------------------------------------------------------------------------


def protect(config: Config, counter: int, data: bytes | bytearray) -> tuple[bytes, int]:
    """E2E_P11Protect (PRS_E2E_00509, Fig. 6.93 to 6.99) for a sender whose state holds
    `counter`. Returns the protected message and the counter the next message will carry.

    `data` is the whole message with the payload in place. Only the CRC byte, the counter
    nibble and, in NIBBLE mode, the Data ID nibble are written.
    """
    _require_length(config, data)  # Fig. 6.94: Length == Config->DataLength / 8
    _require_counter(counter, "the sender's")
    out = bytearray(data)
    if config.data_id_mode == DataIdMode.NIBBLE:
        _write_nibble(out, config.data_id_nibble_offset, config.data_id >> 8)
    _write_nibble(out, config.counter_offset, counter)
    out[config.crc_offset // 8] = compute_crc(config, out)
    return bytes(out), (counter + 1) % COUNTER_MODULUS


def check(config: Config, counter: int, data: bytes | bytearray | None) -> tuple[Status, int]:
    """E2E_P11Check (PRS_E2E_00516, Fig. 6.105 to 6.110) for a receiver whose state holds
    `counter`. Returns the status and the counter the state holds afterwards.

    `data` is the message that arrived since the previous check, or None if none did: the
    check is meant to be called every cycle either way (NewDataAvailable FALSE gives
    NONEWDATA).
    """
    _require_counter(counter, "the receiver's")
    if data is None:
        return Status.NONEWDATA, counter
    _require_length(config, data)  # Fig. 6.106

    received_crc = data[config.crc_offset // 8]
    received_counter = _read_nibble(data, config.counter_offset)
    if received_crc != compute_crc(config, data):
        return Status.ERROR, counter
    if (config.data_id_mode == DataIdMode.NIBBLE
            and _read_nibble(data, config.data_id_nibble_offset) != (config.data_id >> 8) & 0x0F):
        return Status.ERROR, counter
    if received_counter == COUNTER_RESERVED:
        return Status.ERROR, counter  # this project's rule, see the module docstring

    # "ReceivedCounter - State->Counter (taking into wrap around 0xE)": both lie in 0..14, so
    # the difference modulo 15 is the number of steps the sender has gone forward.
    delta = (received_counter - counter) % COUNTER_MODULUS
    if delta > config.max_delta_counter:
        status = Status.WRONGSEQUENCE
    elif delta == 0:
        status = Status.REPEATED
    elif delta == 1:
        status = Status.OK
    else:
        status = Status.OKSOMELOST
    return status, received_counter  # Fig. 6.110: State->Counter = ReceivedCounter


# ------------------------------------------------------------------------------------------
# Both ends with their state, for the bus tools
# ------------------------------------------------------------------------------------------


class Sender:
    """The sending end of one protected message: protect() every message before it is sent."""

    def __init__(self, config: Config) -> None:
        self.config = config
        self.counter = SENDER_INITIAL_COUNTER

    def protect(self, data: bytes | bytearray) -> bytes:
        message, self.counter = protect(self.config, self.counter, data)
        return message


class Receiver:
    """The receiving end of one protected message: check() once per cycle, with the message
    that arrived in it or None."""

    def __init__(self, config: Config) -> None:
        self.config = config
        self.counter = RECEIVER_INITIAL_COUNTER
        self.status = RECEIVER_INITIAL_STATUS

    def check(self, data: bytes | bytearray | None) -> Status:
        self.status, self.counter = check(self.config, self.counter, data)
        return self.status
