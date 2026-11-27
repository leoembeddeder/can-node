# SPDX-License-Identifier: MIT
"""The tester's diagnostic connection to the node: UDS (udsoncan 1.26.1) over ISO-TP (can-isotp
2.0.7) over python-can, on a SocketCAN interface or on the project's UDP bus, with the same bus
specifications as everything else ("socketcan:vcan0", "udp:29500"; fcn_tools.bus).

Both packages are independent implementations of the two standards, so a tester built on them is
the oracle for the node's C code (lib/isotp, lib/uds). This module only joins them to the node's
identifiers and timing:

  identifiers   UDS_Request 0x7E0 (tester to node) and UDS_Response 0x7E8, normal addressing,
                11 bits, physical only (dbc/fcn.dbc)
  ISO-TP        frames of 8 bytes padded with 0xCC, as the node pads its own; the tester's own
                flow control asks for no further one (block size 0) and no separation time,
                which the node's transmitter handles (lib/isotp); a flow control WAIT from the
                node is refused (wftmax 0), as can-isotp does by default: the node never sends
                one (app/diag.h); N_Bs and N_Cr of the tester 1000 ms, the standard's
  UDS timing    P2client P2_CLIENT_S, P2*client P2_STAR_CLIENT_S, never the server's own
                figures (use_server_timing off): the node's P2server is 50 ms, which a tester on
                a host cannot keep to (fcn_tools.flasher's wait for the trial uses it only as
                the time after which a poll counts as "not yet", and asks again; an answer that
                comes later is taken there); its P2*server is 5 s, and it sends responsePending
                every 2 s during a long operation (lib/uds/uds.h), so P2*client is P2*server and a
                second for the hosts, which allows every operation however long it is; the
                request timeout of an operation is computed from the flash part's MAXIMUM times
                (erase_timeout_s(), check_timeout_s()), so that an operation that the node keeps
                answering with responsePending for longer than the part can take is reported

DIDs (lib/uds/uds.h) have codecs here: 0xF186 (the session, 1 byte), 0xF189 (the running image's
version as ASCII) and 0xFD00 (the boot information, 12 bytes, decoded by decode_boot_info()).
"""
from __future__ import annotations

import contextlib
import logging
from dataclasses import dataclass
from typing import Any, Iterator

import can
import isotp
import udsoncan
from udsoncan.client import Client
from udsoncan.connections import PythonIsoTpConnection

from . import bus as bus_module

#: UDS_Request and UDS_Response (dbc/fcn.dbc).
REQUEST_ID = 0x7E0
RESPONSE_ID = 0x7E8

#: The node's server (lib/uds/uds.h).
P2_SERVER_S = 0.050
P2_STAR_SERVER_S = 5.0
PENDING_EVERY_S = 2.0

#: The tester's timing (see the module's text).
P2_CLIENT_S = 1.0
P2_STAR_CLIENT_S = P2_STAR_SERVER_S + 1.0
#: The request timeout of a request that does not wait for the flash.
DEFAULT_REQUEST_TIMEOUT_S = 10.0

#: The flash part's maximum times (W25Q128JV, platform/include/plat_flash.h; the host waits the
#: typical ones, 45 ms and 0.4 ms) and its geometry on the host (plat_flash.h, the layout).
SECTOR_ERASE_MAX_S = 0.400
PAGE_PROGRAM_MAX_S = 0.003
SECTOR_SIZE = 4096
PAGE_SIZE = 256
#: A record of lib/boot at the part's maximum times: a sector erase and three page programs.
RECORD_MAX_S = SECTOR_ERASE_MAX_S + 3 * PAGE_PROGRAM_MAX_S
#: What the hosts and the node's processor add to a request at most, on top of the part.
HOST_MARGIN_S = 10.0
#: The design figure of a validation step on a Cortex-M3 (64 bytes read and two SHA-256
#: compressions; app/task_table.c, flash): an assumption, generous on the host.
VALIDATION_STEP_S = 0.0001

#: The DIDs and routines of the node (lib/uds/uds.h).
DID_ACTIVE_SESSION = 0xF186
DID_SOFTWARE_VERSION = 0xF189
DID_BOOT_INFO = 0xFD00
RID_ERASE_MEMORY = 0xFF00
RID_CHECK_PROGRAMMING = 0xFF01
RID_CONFIRM_TRIAL = 0xF000
BOOT_INFO_SIZE = 12

#: can-isotp's parameters for the node's connection (see the module's text).
ISOTP_PARAMS: dict[str, Any] = {
    "stmin": 0,
    "blocksize": 0,
    "tx_padding": 0xCC,
    "tx_data_length": 8,
    "rx_flowcontrol_timeout": 1000,
    "rx_consecutive_frame_timeout": 1000,
    "wftmax": 0,
    "max_frame_size": 4095,
    "can_fd": False,
}


def erase_timeout_s(size: int) -> float:
    """The request timeout of eraseMemory for an image of `size` bytes: every sector it needs at
    the part's maximum erase time, the BEGIN record at its maximum, and HOST_MARGIN_S."""
    sectors = -(-size // SECTOR_SIZE)
    return sectors * SECTOR_ERASE_MAX_S + RECORD_MAX_S + HOST_MARGIN_S


def check_timeout_s(size: int) -> float:
    """The request timeout of checkProgrammingDependencies for an image of `size` bytes: a
    validation step per 64 bytes (VALIDATION_STEP_S, a design figure), the COMMIT record at the
    part's maximum, and HOST_MARGIN_S."""
    return (size // 64 + 8) * VALIDATION_STEP_S + RECORD_MAX_S + HOST_MARGIN_S


def write_timeout_s(length: int) -> float:
    """The request timeout of a TransferData block of `length` data bytes: a page program at the
    part's maximum for every page it can complete, one more, and HOST_MARGIN_S."""
    return (length // PAGE_SIZE + 1) * PAGE_PROGRAM_MAX_S + HOST_MARGIN_S


class _Raw(udsoncan.DidCodec):
    """A DID of `length` raw bytes."""

    def __init__(self, length: int) -> None:
        self.length = length

    def encode(self, *args: Any, **kwargs: Any) -> bytes:  # the tester never writes a DID
        raise NotImplementedError

    def decode(self, payload: bytes) -> bytes:
        return bytes(payload)

    def __len__(self) -> int:
        return self.length


class _Ascii(udsoncan.DidCodec):
    """A DID of ASCII text up to the end of the response (one DID per request)."""

    def encode(self, *args: Any, **kwargs: Any) -> bytes:
        raise NotImplementedError

    def decode(self, payload: bytes) -> str:
        return bytes(payload).decode("ascii")

    def __len__(self) -> int:
        raise udsoncan.DidCodec.ReadAllRemainingData


def client_config(request_timeout: float | None = DEFAULT_REQUEST_TIMEOUT_S) -> dict:
    """udsoncan's configuration for the node (see the module's text)."""
    config = dict(udsoncan.configs.default_client_config)
    config.update({
        "exception_on_negative_response": True,
        "exception_on_invalid_response": True,
        "exception_on_unexpected_response": True,
        "p2_timeout": P2_CLIENT_S,
        "p2_star_timeout": P2_STAR_CLIENT_S,
        "request_timeout": request_timeout,
        "use_server_timing": False,
        "data_identifiers": {DID_ACTIVE_SESSION: _Raw(1), DID_SOFTWARE_VERSION: _Ascii(),
                             DID_BOOT_INFO: _Raw(BOOT_INFO_SIZE)},
    })
    return config


@contextlib.contextmanager
def connect(spec: str, request_timeout: float | None = DEFAULT_REQUEST_TIMEOUT_S,
            isotp_params: dict[str, Any] | None = None,
            name: str = "tester") -> Iterator[Client]:
    """An open udsoncan client of the node on the bus `spec`, closed with everything under it
    when the block ends. `isotp_params` replace entries of ISOTP_PARAMS."""
    params = dict(ISOTP_PARAMS)
    params.update(isotp_params or {})
    logging.getLogger("isotp").setLevel(logging.WARNING)
    bus = bus_module.open_bus(spec)
    notifier = can.Notifier(bus, [], timeout=0.05)
    try:
        address = isotp.Address(isotp.AddressingMode.Normal_11bits, txid=REQUEST_ID,
                                rxid=RESPONSE_ID)
        stack = isotp.NotifierBasedCanStack(bus, notifier, address=address, params=params)
        connection = PythonIsoTpConnection(stack, name=name)
        with Client(connection, config=client_config(request_timeout)) as client:
            yield client
    finally:
        notifier.stop()
        bus.shutdown()


@dataclass(frozen=True)
class BootInfo:
    """DID 0xFD00, layout version 1 (lib/uds/uds.h, docs/diagnostics.md)."""
    layout: int
    handoff: bool
    slot: str | None          # "A", "B", or None without a hand-off
    mode: str | None          # "confirmed", "trial", or None
    attempt: int
    confirm_state: str        # NOT_NEEDED, WAITING, CONFIRMING, CONFIRMED, FAILED
    event: int
    event_slot: str | None
    image_version: str | None  # "major.minor.patch", or None without a hand-off

    def as_dict(self) -> dict:
        return dict(self.__dict__)


CONFIRM_STATES = ("NOT_NEEDED", "WAITING", "CONFIRMING", "CONFIRMED", "FAILED")


def version_text(code: int) -> str:
    """lib/image's version code (major << 24 | minor << 16 | patch) as "major.minor.patch"."""
    return f"{code >> 24}.{(code >> 16) & 0xFF}.{code & 0xFFFF}"


def _slot_name(value: int) -> str | None:
    return {0: "A", 1: "B"}.get(value)


def decode_boot_info(data: bytes) -> BootInfo:
    """The 12 bytes of DID 0xFD00, decoded; ValueError for another length or layout."""
    if len(data) != BOOT_INFO_SIZE or data[0] != 1:
        raise ValueError(f"not a boot information record of layout 1: {data.hex(' ')}")
    handoff = data[1] == 1
    mode = {1: "confirmed", 2: "trial"}.get(data[3]) if handoff else None
    state = data[5]
    return BootInfo(layout=data[0], handoff=handoff, slot=_slot_name(data[2]) if handoff else None,
                    mode=mode, attempt=data[4],
                    confirm_state=CONFIRM_STATES[state] if state < len(CONFIRM_STATES) else str(state),
                    event=data[6], event_slot=_slot_name(data[7]),
                    image_version=version_text(int.from_bytes(data[8:12], "big")) if handoff
                    else None)


def read_boot_info(client: Client) -> BootInfo:
    """ReadDataByIdentifier 0xFD00."""
    response = client.read_data_by_identifier(DID_BOOT_INFO)
    return decode_boot_info(response.service_data.values[DID_BOOT_INFO])


def read_version(client: Client) -> str:
    """ReadDataByIdentifier 0xF189: the running image's version."""
    response = client.read_data_by_identifier(DID_SOFTWARE_VERSION)
    return response.service_data.values[DID_SOFTWARE_VERSION]
