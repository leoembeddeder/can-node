"""Decode the node's CAN frames with the message database and check their end-to-end protection.

dbc/fcn.dbc defines every frame on the bus. This module decodes frames with cantools from that
file and checks the messages it marks as protected with fcn_tools.e2e, the Python reference of
AUTOSAR E2E Profile 11. That reference is independent of the node's lib/e2e, so the check here
is a second opinion on what the node sends.

    from fcn_tools.decode import Decoder
    decoder = Decoder()                  # dbc/fcn.dbc of this checkout
    frame = decoder.decode(message)      # a can.Message as python-can received it
    frame.name                           # "BMS_PackStatus"
    frame.signals["PackCurrent"]         # 2.01: physical value in the signal's unit
    frame.signals["PackState"]           # "NORMAL": the name of a value-table entry
    frame.raw["PackCurrent"]             # 201: the raw value on the wire
    frame.e2e.status                     # fcn_tools.e2e.Status.OK; e2e is None if unprotected

A Decoder holds one Profile 11 receiver per protected message and checks the frames of that
message in the order decode() gets them, the way a receiving ECU would. Profile 11 is meant to
be checked once per cycle, also when no frame came ("no new data"). The decoder sees frames
only, so it never makes that check, and a sender that stops shows as frames that stop.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from decimal import Decimal
from pathlib import Path

import can
import cantools
from cantools.database.can import Database, Message
from cantools.database.namedsignalvalue import NamedSignalValue

from . import e2e, paths

#: The message database of this checkout.
DBC_PATH = paths.REPO_ROOT / "dbc" / "fcn.dbc"

#: The DBC attribute that holds a message's 16-bit Data ID; -1 (the default): not protected.
E2E_DATA_ID_ATTRIBUTE = "FcnE2eDataId"

#: Signals of the Profile 11 header, at the start of every protected message.
E2E_SIGNALS = ("E2E_Crc", "E2E_Counter")


def load_database(path: str | Path = DBC_PATH) -> Database:
    """The DBC at `path`, loaded strictly: overlapping signals and the like are errors."""
    return cantools.database.load_file(str(path), database_format="dbc", strict=True)


def data_id(message: Message) -> int | None:
    """The Data ID of a protected message, None for an unprotected one."""
    attribute = message.dbc.attributes.get(E2E_DATA_ID_ATTRIBUTE) if message.dbc else None
    value = attribute.value if attribute is not None else -1
    return None if value < 0 else int(value)


@dataclass(frozen=True)
class E2EVerdict:
    """What the Profile 11 receiver of a message made of one of its frames."""

    status: e2e.Status  # the receiver's status after this frame
    counter: int        # the frame's counter, the low nibble of byte 1
    crc_ok: bool        # the CRC in byte 0 is that of the message's Data ID and payload


@dataclass(frozen=True)
class DecodedFrame:
    """One frame, decoded. A frame whose length is not the DLC of its message has no signals,
    and `error` says why; its E2E check is still made (it gives ERROR)."""

    timestamp: float
    frame_id: int
    name: str
    signals: dict[str, float | int | str] = field(default_factory=dict)
    raw: dict[str, int] = field(default_factory=dict)
    e2e: E2EVerdict | None = None
    error: str | None = None


class Decoder:
    """Decodes frames of the DBC and checks the protected ones, each message with a receiver of
    its own. `max_delta_counter` is the receivers' tolerance (1: every lost frame is
    WRONGSEQUENCE; up to 14)."""

    def __init__(self, db: Database | None = None, max_delta_counter: int = 1) -> None:
        self.db = db if db is not None else load_database()
        self.max_delta_counter = max_delta_counter
        self._receivers: dict[int, e2e.Receiver] = {}
        for message in self.db.messages:
            identifier = data_id(message)
            if identifier is not None:
                self._receivers[message.frame_id] = e2e.Receiver(e2e.Config(
                    data_id=identifier, data_length=8 * message.length,
                    max_delta_counter=max_delta_counter))

    @property
    def protected(self) -> dict[int, int]:
        """Frame identifier -> Data ID of every protected message."""
        return {frame_id: receiver.config.data_id
                for frame_id, receiver in self._receivers.items()}

    def decode(self, message: can.Message) -> DecodedFrame | None:
        """The frame decoded, or None if it is not one of the DBC: another identifier, an
        extended identifier, a remote or an error frame. Protected messages are checked, which
        advances their receiver."""
        if message.is_extended_id or message.is_remote_frame or message.is_error_frame:
            return None
        try:
            definition = self.db.get_message_by_frame_id(message.arbitration_id)
        except KeyError:
            return None

        data = bytes(message.data)
        verdict = self._check(definition.frame_id, data)
        if not definition.signals:
            # Ping, pong and the UDS frames: a payload of 0 to 8 bytes and no signals.
            return DecodedFrame(message.timestamp, definition.frame_id, definition.name,
                                e2e=verdict)
        if len(data) != definition.length:
            return DecodedFrame(message.timestamp, definition.frame_id, definition.name,
                                e2e=verdict,
                                error=f"{len(data)} data bytes, the DBC says {definition.length}")
        physical = definition.decode(data, decode_choices=True, scaling=True)
        raw = definition.decode(data, decode_choices=False, scaling=False)
        return DecodedFrame(
            message.timestamp, definition.frame_id, definition.name,
            signals={name: (value.name if isinstance(value, NamedSignalValue) else value)
                     for name, value in physical.items()},
            raw={name: int(value) for name, value in raw.items()},
            e2e=verdict)

    def _check(self, frame_id: int, data: bytes) -> E2EVerdict | None:
        receiver = self._receivers.get(frame_id)
        if receiver is None:
            return None
        if len(data) != receiver.config.length_bytes:
            # The profile refuses a message of another length (E2E_E_INPUTERR_WRONG); for the
            # receiver's state machine that is an error (SWS_E2E_00558). The state is kept.
            return E2EVerdict(status=e2e.Status.ERROR, counter=-1, crc_ok=False)
        crc_ok = data[0] == e2e.compute_crc(receiver.config, data)
        return E2EVerdict(status=receiver.check(data), counter=data[1] & 0x0F, crc_ok=crc_ok)


def _decimals(signal) -> int:
    """Digits after the point that the signal's factor and offset give (0.01 A: 2)."""
    exponents = [Decimal(repr(value)).normalize().as_tuple().exponent
                 for value in (signal.scale, signal.offset)]
    return max(0, *(-int(exponent) for exponent in exponents))


def format_frame(frame: DecodedFrame, db: Database) -> str:
    """One line for a decoded frame: time, identifier, name, the E2E verdict and the signals
    with their units. The E2E header signals are shown in the verdict."""
    line = [f"{frame.timestamp:17.6f}", f"{frame.frame_id:03X}", f"{frame.name:<18}"]
    if frame.e2e is not None:
        if frame.e2e.counter < 0:
            verdict = "ERROR (length)"
        elif not frame.e2e.crc_ok:
            verdict = "ERROR (CRC)"
        else:
            verdict = frame.e2e.status.name
        counter = "-" if frame.e2e.counter < 0 else str(frame.e2e.counter)
        line.append(f"e2e {verdict:<14} c={counter:<2}")
    if frame.error is not None:
        line.append(f"malformed: {frame.error}")
    message = db.get_message_by_frame_id(frame.frame_id)
    for signal in message.signals:
        if signal.name in E2E_SIGNALS or signal.name not in frame.signals:
            continue
        value = frame.signals[signal.name]
        if isinstance(value, float):
            text = f"{value:.{_decimals(signal)}f}"
        else:
            text = str(value)
        unit = f" {signal.unit}" if signal.unit and not isinstance(value, str) else ""
        line.append(f"{signal.name}={text}{unit}")
    return "  ".join(line)


def format_unknown(message: can.Message) -> str:
    """One line for a frame the DBC does not define."""
    kind = "extended" if message.is_extended_id else "base"
    remote = " remote" if message.is_remote_frame else ""
    width = 8 if message.is_extended_id else 3
    return (f"{message.timestamp:17.6f}  {message.arbitration_id:0{width}X}  "
            f"{'(not in the DBC)':<18}  {kind}{remote}, {message.dlc} bytes: "
            f"{bytes(message.data).hex(' ')}")
