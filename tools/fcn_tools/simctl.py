"""Client of the node's simulation control channel.

The simulated hardware around the node (the CAN bus, the sensor front end, the flash and its
supply) is controlled through a UDP socket on the loopback interface, the way a test
bench controls relays and signal generators around a real device. The firmware itself never
sees these commands. The protocol is documented in platform/posix/posix_sim.h:

    request:  <token> <command> [arguments...]
    reply:    <token> ok [key=value ...]     or     <token> err <message>

The node carries out every request it receives, once per datagram, and does not remember
tokens. A request that got no answer in time may still be waiting in the node's socket (a
host that did not run the node), so sending it again can make the node carry it out twice.
The client therefore repeats only a command for which that does no harm; see REPEATABLE.
"""
from __future__ import annotations

import itertools
import re
import socket
import time
from dataclasses import dataclass

# Commands that may reach the node more than once without changing the outcome: they only
# read (ping, can.status, can.trace, sensor.status, flash.status) or set a state to a value
# (can.fault and the other sensor commands: a channel held, opened or cleared twice, or the
# front end taken off line or made to hang twice, ends as it would after once). Any other
# command is sent once and waited for: can.inject above all (it adds to the errors still to
# come), and flash.cut, which counts its operations from the moment it is served, so that a
# second delivery after an operation would move the cut.
REPEATABLE = frozenset({"ping", "can.status", "can.fault", "can.trace",
                        "sensor.status", "sensor.set", "sensor.open", "sensor.offline",
                        "sensor.hang", "sensor.clear", "flash.status"})

# The largest N of flash.cut, and the largest seed (platform/posix/posix_flash.h).
FLASH_CUT_MAX = 2147483647
FLASH_SEED_MAX = 4294967295

# The modes of sensor.hang (platform/posix/posix_sensor.c, docs/sensor-model.md).
HANG_MODES = ("on", "spin", "off")

# The channels of the sensor front-end model (platform/posix/posix_sensor.c).
SENSOR_CHANNELS = ("current",
                   *(f"cell{n}" for n in range(1, 9)),
                   *(f"temp{n}" for n in range(1, 5)))

_INTEGER = re.compile(r"-?[0-9]+")

# Pause before sending again to a port where nothing was listening yet (a node that is still
# starting): the request was not delivered, so it can be sent again whatever it is.
REFUSED_PAUSE_S = 0.01


class SimError(RuntimeError):
    """The node answered a command with an error, or did not answer."""


# ------------------------------------------------------------------ the bus-off trace

#: The kinds of record of can.trace (posix_can_trace_kind_name() in posix_can_trace.c).
TRACE_KINDS = ("state", "counters", "restart", "first_tx", "fault", "inject_tx", "inject_rx")

#: The key of a record in the reply of can.trace: "r<number>".
TRACE_KEY = re.compile(r"r([0-9]+)")

#: The error states in the numbering of the trace (can_fc_state_t).
TRACE_STATES = ("active", "passive", "busoff")


@dataclass(frozen=True)
class TraceRecord:
    """One record of the controller's bus-off trace (platform/posix/posix_can_trace.h, which
    says what each field means for each kind). Times are CLOCK_MONOTONIC nanoseconds, the clock
    of the model, of the node's log and of time.monotonic_ns()."""
    seq: int
    kind: str
    model_ns: int
    pass_ns: int
    start_ns: int
    submit_ns: int
    attempts_failed: int
    value: int
    state: int
    old_state: int
    tec: int
    rec: int
    frames: int


@dataclass(frozen=True)
class TraceRead:
    """What SimControl.can_trace() read: the records in order, how many the ring had already
    overwritten, and the number the next record will get."""
    records: list[TraceRecord]
    lost: int
    newest: int


def parse_trace_record(key: str, value: str) -> TraceRecord:
    """The record of one "r<seq>=<kind>,<model_ns>,...,<frames>" pair of a can.trace reply, its
    fields in the order of TraceRecord. ValueError for anything else."""
    found = TRACE_KEY.fullmatch(key)
    fields = value.split(",")
    if found is None or len(fields) != 12 or fields[0] not in TRACE_KINDS:
        raise ValueError(f"not a trace record: {key}={value}")
    numbers = [int(field) for field in fields[1:]]
    return TraceRecord(int(found.group(1)), fields[0], *numbers)


class SimControl:
    """One client socket. Use as a context manager, or call close()."""

    def __init__(self, port: int, timeout: float = 1.0) -> None:
        self._address = ("127.0.0.1", port)
        self._timeout = timeout
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Connected, so that the kernel reports a datagram nobody received (nothing bound to
        # the port) as ConnectionRefusedError instead of letting it vanish; and so that only
        # the node's port can answer.
        self._socket.connect(self._address)
        self._tokens = (f"t{n}" for n in itertools.count(1))

    def close(self) -> None:
        self._socket.close()

    def __enter__(self) -> "SimControl":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def request(self, command: str, retries: int = 2) -> dict[str, str]:
        """Send one command and return the reply's key=value pairs.

        The client waits up to (retries + 1) x timeout in all. A command in REPEATABLE is sent
        again, with a new token, each time `timeout` passes without an answer, up to `retries`
        times; any other command is sent once and waited for the whole time. A request that
        the kernel reports as not received (the node has not opened its socket yet, right
        after it started) is sent again in any case.
        """
        repeatable = command.split(" ", 1)[0] in REPEATABLE
        deadline = time.monotonic() + self._timeout * (retries + 1)
        unanswered = 0           # requests that may have reached the node
        last_error: Exception | None = None
        while unanswered < (retries + 1 if repeatable else 1):
            now = time.monotonic()
            if now >= deadline:
                break
            token = next(self._tokens)
            until = min(deadline, now + self._timeout) if repeatable else deadline
            try:
                self._socket.send(f"{token} {command}".encode())
                words = self._reply(token, until)
            except ConnectionRefusedError as error:
                last_error = error
                time.sleep(REFUSED_PAUSE_S)
                continue
            except TimeoutError as error:
                last_error = error
                unanswered += 1
                continue
            if len(words) >= 2 and words[1] == "ok":
                return dict(word.split("=", 1) for word in words[2:] if "=" in word)
            raise SimError(f"'{command}' failed: {' '.join(words[2:])}")
        raise SimError(f"no reply to '{command}' from {self._address}: {last_error}")

    def _reply(self, token: str, until: float) -> list[str]:
        """The words of the reply to `token`, read until the monotonic time `until`. Replies
        with another token answer an earlier attempt and are skipped."""
        while True:
            remaining = until - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out")
            self._socket.settimeout(remaining)
            data = self._socket.recv(2048)
            words = data.decode(errors="replace").split()
            if words and words[0] == token:
                return words

    def ping(self) -> int:
        """The node's process id; proves the node is up and serving commands."""
        return int(self.request("ping")["pid"])

    # ------------------------------------------------------------------ CAN controller model

    def can_status(self) -> dict:
        """State, error counters and statistics of the CAN controller model, and the ticks the
        node's tick code has given up so far (`ticks_dropped`, docs/can-simulation.md).

        Numeric values are converted to int; `state` (active, passive, busoff) and `fault`
        stay strings.
        """
        reply = self.request("can.status")
        return {key: (int(value) if value.isdigit() else value) for key, value in reply.items()}

    def can_fault(self, fault: str) -> dict[str, str]:
        """Apply a bus fault: none, short, dominant or noack."""
        return self.request(f"can.fault {fault}")

    def can_inject(self, kind: str, count: int) -> dict[str, str]:
        """Destroy the next `count` transmission attempts ("tx") or received frames ("rx").
        Not repeated on a timeout: the count adds to what is left (see REPEATABLE)."""
        return self.request(f"can.inject {kind} {count}")

    def can_trace(self, start: int = 0) -> TraceRead:
        """The records of the controller's bus-off trace from number `start` on, up to the
        newest at the time of the last request (platform/posix/posix_can_trace.h), read with as
        many can.trace requests as they take. Records the ring no longer keeps are counted in
        TraceRead.lost, not returned."""
        records: list[TraceRecord] = []
        lost = 0
        at = start
        while True:
            reply = self.request(f"can.trace {at}")
            oldest, newest = int(reply["oldest"]), int(reply["newest"])
            if at < oldest:
                lost += oldest - at
                at = oldest
            got = sorted((parse_trace_record(key, value) for key, value in reply.items()
                          if TRACE_KEY.fullmatch(key)), key=lambda record: record.seq)
            for record in got:
                if record.seq != at:
                    raise SimError(f"can.trace from {at} answered with record {record.seq}")
                records.append(record)
                at += 1
            if at >= newest:
                return TraceRead(records=records, lost=lost, newest=newest)
            if not got:
                raise SimError(f"can.trace {at} returned no record below newest={newest}")

    # --------------------------------------------------------------- sensor front-end model
    #
    # Channels are named as in SENSOR_CHANNELS; values are in the units of plat_sensor.h: mA
    # (positive when discharging), mV, and tenths of a degree Celsius. docs/sensor-model.md
    # describes the model and what each fault does.

    def sensor_status(self) -> dict:
        """Seed, off-line state, read counters and every channel's mode.

        Whole numbers are converted to int, negative ones included (a held value); `offline`
        (on, off) and a channel's `model` or `open` stay strings."""
        reply = self.request("sensor.status")
        return {key: (int(value) if _INTEGER.fullmatch(value) else value)
                for key, value in reply.items()}

    def sensor_set(self, channel: str, value: int) -> dict[str, str]:
        """Hold `channel` at `value`: the front end reports exactly that, as a valid reading,
        until the channel is cleared. Any value of the channel's type is accepted, also one
        the real sensor could not deliver."""
        if isinstance(value, bool) or not isinstance(value, int):
            raise TypeError(f"a sensor value is a whole number, not {value!r}")
        return self.request(f"sensor.set {channel} {value}")

    def sensor_open(self, channel: str) -> dict[str, str]:
        """Let the front end declare `channel` invalid (an open sense wire)."""
        return self.request(f"sensor.open {channel}")

    def sensor_offline(self, offline: bool = True) -> dict[str, str]:
        """Take the whole front end off line (it answers no read), or bring it back."""
        return self.request(f"sensor.offline {'on' if offline else 'off'}")

    def sensor_hang(self, mode: str | bool = "on") -> dict[str, str]:
        """Make every read of the front end hang until the hang ends, or end it.

        "on" (or True): a read blocks the sensor task, as a driver that waits for an interrupt
        that never comes; the tasks below it run on. "spin": a read keeps the CPU, as a driver
        polling a flag that never changes; every task below the sensor task starves. "off" (or
        False): reads return again. Anything else raises ValueError and sends nothing."""
        if mode is True or mode is False:
            mode = "on" if mode else "off"
        if mode not in HANG_MODES:
            raise ValueError(f"a hang mode is one of {', '.join(HANG_MODES)}, not {mode!r}")
        return self.request(f"sensor.hang {mode}")

    def sensor_clear(self, channel: str | None = None) -> dict[str, str]:
        """End the fault of `channel`, or with no channel every fault, off line and hang
        included."""
        return self.request("sensor.clear" if channel is None else f"sensor.clear {channel}")

    # ------------------------------------------------------------------- the simulated flash
    #
    # platform/posix/posix_flash.h describes the commands and their replies, and docs/update.md
    # the flash model and what a cut leaves.

    def flash_status(self) -> dict:
        """The flash's counts since the node opened it and the armed cut.

        Whole numbers are converted to int; `flash` (on, off) and `cut` (none, armed, fired)
        stay strings. Without a flash (a node started without --flash-file) the reply is
        {"flash": "off"} alone."""
        reply = self.request("flash.status")
        return {key: (int(value) if value.isdigit() else value) for key, value in reply.items()}

    def flash_cut(self, operation: int, seed: int = 0) -> dict[str, str]:
        """Cut the supply in the `operation`-th flash program or erase that starts after the
        node served the command (1: the next one), torn as `seed` says; replaces a cut armed
        before. The node's run then ends as a loss of supply (exit status 12).

        `operation` is a whole number from 1 to FLASH_CUT_MAX and `seed` one from 0 to
        FLASH_SEED_MAX; anything else raises TypeError or ValueError and sends nothing. Sent
        once and waited for, never repeated (see REPEATABLE)."""
        for name, value, low, high in (("operation", operation, 1, FLASH_CUT_MAX),
                                       ("seed", seed, 0, FLASH_SEED_MAX)):
            if isinstance(value, bool) or not isinstance(value, int):
                raise TypeError(f"a flash cut's {name} is a whole number, not {value!r}")
            if not low <= value <= high:
                raise ValueError(f"a flash cut's {name} is from {low} to {high}, not {value}")
        return self.request(f"flash.cut {operation}:{seed}")

    def flash_cut_off(self) -> dict[str, str]:
        """Disarm a cut that has not fired."""
        return self.request("flash.cut off")
