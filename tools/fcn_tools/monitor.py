"""Print the frames on a bus as they arrive, decoded with the DBC and checked end to end.

    python -m fcn_tools.monitor udp:29500
    python -m fcn_tools.monitor socketcan:vcan0 --count 100
    python -m fcn_tools.monitor udp --seconds 5 --id 0x020 --id 0x050

One line per frame (fcn_tools.decode.format_frame): the receive time, the identifier, the
message, for a protected message the verdict of its Profile 11 receiver and the counter, then
every signal with its unit. Frames the DBC does not define are printed as bytes. The monitor
runs until Ctrl-C, or until --count frames or --seconds have passed, and exits with 0.
"""
from __future__ import annotations

import argparse
import sys
import time
from collections.abc import Sequence
from typing import TextIO

from .bus import open_bus
from .decode import DBC_PATH, Decoder, format_frame, format_unknown, load_database


def _identifier(text: str) -> int:
    value = int(text, 0)
    if not 0 <= value <= 0x7FF:
        raise argparse.ArgumentTypeError(f"{text} is not an 11-bit identifier")
    return value


def monitor(bus, decoder: Decoder, out: TextIO, count: int | None = None,
            seconds: float | None = None, identifiers: frozenset[int] = frozenset()) -> int:
    """Print the frames `bus` receives, only those of `identifiers` if it is not empty, until
    `count` frames have been printed or `seconds` have passed. Returns the number of frames
    printed."""
    deadline = None if seconds is None else time.monotonic() + seconds
    printed = 0
    while count is None or printed < count:
        timeout = 0.5 if deadline is None else deadline - time.monotonic()
        if timeout <= 0:
            break
        message = bus.recv(timeout=timeout)
        if message is None:
            continue
        frame = decoder.decode(message)
        if identifiers and message.arbitration_id not in identifiers:
            continue
        line = format_unknown(message) if frame is None else format_frame(frame, decoder.db)
        print(line, file=out, flush=True)
        printed += 1
    return printed


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m fcn_tools.monitor",
        description="Print the frames on a bus, decoded with the DBC and checked end to end.")
    parser.add_argument("bus", help="udp[:<base port>] or socketcan:<interface>")
    parser.add_argument("--count", type=int, default=None, help="stop after this many frames")
    parser.add_argument("--seconds", type=float, default=None, help="stop after this long")
    parser.add_argument("--id", dest="ids", type=_identifier, action="append", default=[],
                        help="print only this identifier (repeatable), e.g. 0x020")
    parser.add_argument("--max-delta-counter", type=int, default=1, choices=range(1, 15),
                        metavar="1..14",
                        help="largest counter step the E2E receivers accept (default 1)")
    parser.add_argument("--dbc", default=str(DBC_PATH),
                        help="message database (default: %(default)s)")
    args = parser.parse_args(argv)
    if args.count is not None and args.count < 1:
        parser.error("--count must be at least 1")

    decoder = Decoder(load_database(args.dbc), max_delta_counter=args.max_delta_counter)
    try:
        bus = open_bus(args.bus)
    except (ValueError, OSError) as error:
        print(f"monitor: cannot open {args.bus}: {error}", file=sys.stderr)
        return 1
    try:
        monitor(bus, decoder, sys.stdout, count=args.count, seconds=args.seconds,
                identifiers=frozenset(args.ids))
    except KeyboardInterrupt:
        pass
    finally:
        bus.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
