#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the C codec of the node's CAN messages from the DBC.

    tools/gen_can_db.py [--check] [--test-table FILE] DBC OUT_DIR

writes OUT_DIR/can_db.h and OUT_DIR/can_db.c, and with --test-table also FILE, a table of the
codec for the tests (tests/tools/can_db_table.c). With --check nothing is written: the files
are generated in memory and compared with the ones on disk, and every difference is printed
as a unified diff.

    exit status 0   the files were written, or --check found them up to date
                1   --check found a file that differs or is missing, or the DBC breaks a rule
                    below (each problem is printed on stderr)
                2   wrong command line

The project's files are generated, and checked in CI, by scripts/codegen-check.sh.

WHAT IS GENERATED. app/gen/can_db.h explains the generated interface in its first comment:
for every message its identifier, DLC, cycle time and E2E Data ID; for every message with
signals a struct of the RAW signal values with a pack() and an unpack() function; for every
signal its position, raw range, value table and scaling as exact integer fractions.

RULES. The DBC is read with cantools, which is told not to check it (strict=False), so that
every problem is reported here, in the project's words, and all of them at once:

  - base-format (11-bit) identifiers, DLC 0..8, one identifier and one C name per message;
  - signals little-endian, integer (not float), not multiplexed, 1 to 32 bits long, inside
    the DLC, not overlapping another signal of the message;
  - factor and offset representable as fractions of two int32 values, factor not 0, so that
    the generated NUM/DEN constants are exact; value-table entries inside the raw range and
    usable as C identifiers;
  - the attribute FcnE2eDataId (default -1) gives the Data ID of a message protected by AUTOSAR
    E2E Profile 11, variant 11A, or -1 for an unprotected one. A protected message is cyclic
    (GenMsgCycleTime > 0: E2E detects loss only in periodic data) and starts with the E2E
    header: E2E_Crc at bit 0, 8 bits, and E2E_Counter at bit 8, 4 bits, both unsigned and
    unscaled. An unprotected message has neither. The Profile 11 CRC of the two Data ID bytes
    differs between any two protected messages (PRS_E2E_UC_00072: otherwise a frame of one
    message, delivered under the identifier of the other, could pass its check);
  - every generated name is a C identifier, unique, and distinct from every other within the
    significant characters that MISRA C:2012 rules 5.1 and 5.4 count (31 for functions, 63 for
    macros), and every generated line fits the 100 columns of .clang-format.

The output is a function of the DBC alone: no time stamp, no absolute path; the header names
the DBC by its path in the repository and its SHA-256. It is written formatted as clang-format
would format it, so that scripts/static-analysis.sh accepts it as it is.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import re
import sys
import textwrap
import warnings
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

HEADER_NAME = "can_db.h"
SOURCE_NAME = "can_db.c"

E2E_ATTRIBUTE = "FcnE2eDataId"
E2E_UNPROTECTED = -1
E2E_CRC_SIGNAL = "E2E_Crc"
E2E_COUNTER_SIGNAL = "E2E_Counter"
# (start bit, length) of the two header signals of variant 11A (PRS_E2E_00540).
E2E_HEADER = {E2E_CRC_SIGNAL: (0, 8), E2E_COUNTER_SIGNAL: (8, 4)}

CLASSICAL_CAN_MAX_DLC = 8
MAX_SIGNAL_BITS = 32
COLUMN_LIMIT = 100
INT32_MAX = (1 << 31) - 1

# Significant initial characters (MISRA C:2012 rule 5.1: external identifiers, the C99 minimum
# of 31; rule 5.4: macro identifiers, 63).
SIGNIFICANT_EXTERNAL = 31
SIGNIFICANT_MACRO = 63

# Words a generated C name must not be (keywords, and names the C library has taken).
C_RESERVED = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
    "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register",
    "restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch", "typedef",
    "union", "unsigned", "void", "volatile", "while", "bool", "true", "false", "_Bool",
}


class GeneratorError(Exception):
    """The DBC breaks one or more of the rules in the docstring of this module."""

    def __init__(self, dbc: str, problems: list[str]):
        self.problems = problems
        super().__init__("\n".join(f"{dbc}: {problem}" for problem in problems))


def crc8_p11(data: bytes) -> int:
    """The CRC of AUTOSAR E2E Profile 11: polynomial 0x1D, most significant bit first, start
    value 0x00, no final XOR. The specification calls the SAE J1850 routine with a start value
    and a final XOR that cancel its own 0xFF/0xFF, which is why "123456789" gives 0x37 here
    and not the catalogue value 0x4B."""
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1D) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def data_id_crc(data_id: int) -> int:
    """The CRC over the two Data ID bytes, low byte first, as Profile 11 feeds them before the
    data (PRS_E2E_00505). Two messages with the same value here cannot be told apart by the
    CRC when their data agree."""
    return crc8_p11(bytes([data_id & 0xFF, (data_id >> 8) & 0xFF]))


# ------------------------------------------------------------------------------------------
# The model: what the generator takes from the DBC
# ------------------------------------------------------------------------------------------


def snake_case(name: str) -> str:
    """BMS_CellVoltages1 -> bms_cell_voltages1, Cell1Voltage -> cell1_voltage, E2E_Crc ->
    e2e_crc. A word boundary is an underscore, a capital after a small letter or after a small
    letter and a digit, or a capital that starts a word after an acronym (CRCValue)."""
    words = re.sub(r"(?<=[a-z])(?=[A-Z])|(?<=[a-z][0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])",
                   "_", name)
    return words.lower()


def c_literal_unsigned(value: int) -> str:
    return f"{value}U"


def c_literal_signed(value: int, c_type: str = "int32_t") -> str:
    """A constant of a signed fixed-width type, e.g. ((int32_t)(-40)).

    The cast gives the constant the type it is used with, and it keeps clang-tidy's
    misc-redundant-expression quiet on _Static_assert(X == -40): with X defined as (-40), both
    sides are the same expression to it. INT32_MIN has no literal of type int: -2147483648 is
    the negation of a long."""
    if value == -(1 << 31):
        text = "(-2147483647 - 1)"
    else:
        text = f"({value})" if value < 0 else str(value)
    return f"(({c_type}){text})"


def decimal(value: Fraction) -> str:
    """A fraction as a decimal number (0.01, -327.68) if it has a finite one, else as num/den."""
    places = 0
    while (10**places) % value.denominator != 0:
        places += 1
        if places > 40:  # the denominator has a prime factor other than 2 and 5
            return f"{value.numerator}/{value.denominator}"
    if places == 0:
        return str(value.numerator)
    digits = str(abs(value.numerator) * (10**places // value.denominator)).rjust(places + 1, "0")
    return f"{'-' if value < 0 else ''}{digits[:-places]}.{digits[-places:]}"


@dataclass
class Signal:
    name: str
    c_name: str
    start: int
    length: int
    is_signed: bool
    factor: Fraction
    offset: Fraction
    unit: str
    minimum: str | None
    maximum: str | None
    choices: list[tuple[int, str]]
    comment: str
    receivers: list[str]
    supported: bool = True  # False: a rule on the signal alone failed; its bits are not checked

    @property
    def end(self) -> int:
        return self.start + self.length - 1

    @property
    def raw_min(self) -> int:
        return -(1 << (self.length - 1)) if self.is_signed else 0

    @property
    def raw_max(self) -> int:
        return (1 << (self.length - 1)) - 1 if self.is_signed else (1 << self.length) - 1

    @property
    def c_bits(self) -> int:
        """Width of the smallest fixed-width type that holds the raw value."""
        return 8 if self.length <= 8 else 16 if self.length <= 16 else 32

    @property
    def c_type(self) -> str:
        return f"{'int' if self.is_signed else 'uint'}{self.c_bits}_t"

    @property
    def field_min(self) -> int:
        return -(1 << (self.c_bits - 1)) if self.is_signed else 0

    @property
    def field_max(self) -> int:
        return (1 << (self.c_bits - 1)) - 1 if self.is_signed else (1 << self.c_bits) - 1

    def raw_literal(self, value: int) -> str:
        """A raw value as a constant: unsigned with a U, signed with the member's type."""
        return c_literal_signed(value, self.c_type) if self.is_signed else c_literal_unsigned(value)


@dataclass
class Message:
    name: str
    c_name: str
    frame_id: int
    dlc: int
    cycle_ms: int
    data_id: int | None
    comment: str
    senders: list[str]
    signals: list[Signal] = field(default_factory=list)
    e2e_known: bool = True  # False: the Data ID attribute was unusable; the E2E rules are skipped

    @property
    def macro(self) -> str:
        return f"CAN_DB_{self.c_name.upper()}"

    @property
    def c_type(self) -> str:
        return f"can_db_{self.c_name}_t"

    @property
    def pack_name(self) -> str:
        return f"can_db_{self.c_name}_pack"

    @property
    def unpack_name(self) -> str:
        return f"can_db_{self.c_name}_unpack"

    @property
    def receivers(self) -> list[str]:
        return sorted({r for signal in self.signals for r in signal.receivers})


@dataclass
class Database:
    path: str     # as shown in the generated files: relative to the repository if inside it
    sha256: str
    messages: list[Message]

    @property
    def protected(self) -> list[Message]:
        return [message for message in self.messages if message.data_id is not None]


def display_path(path: Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.name


def exact(value: float | int) -> Fraction:
    """The number as written in the DBC. cantools reads "0.01" into the nearest float; the
    shortest decimal that gives back that float is what the file said."""
    return Fraction(repr(value)) if isinstance(value, float) else Fraction(value)


def range_text(value: float | int | None) -> str | None:
    if value is None:
        return None
    return decimal(exact(value))


def text_of(comment: str | None) -> str:
    return " ".join((comment or "").split())


def load(dbc_path: Path) -> Database:
    """Read the DBC, apply every rule of the module docstring and return the model. Raises
    GeneratorError with all problems found."""
    import cantools  # imported here so that the rest of the module works without it

    raw = dbc_path.read_bytes()
    shown = display_path(dbc_path)
    problems: list[str] = []
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            db = cantools.database.load_string(raw.decode("ascii"), database_format="dbc",
                                               strict=False, sort_signals=None)
    except UnicodeDecodeError as error:
        raise GeneratorError(shown, [f"not an ASCII file ({error})"]) from None
    except Exception as error:  # cantools raises several types for an unreadable file
        raise GeneratorError(shown, [f"cantools cannot read it: {error}"]) from None

    definition = db.dbc.attribute_definitions.get(E2E_ATTRIBUTE) if db.dbc else None
    if definition is None:
        problems.append(f"no attribute definition {E2E_ATTRIBUTE} (BA_DEF_ BO_ \"{E2E_ATTRIBUTE}\" "
                        f"INT -1 65535;)")
        default_id = E2E_UNPROTECTED
    else:
        default_id = definition.default_value
        if default_id != E2E_UNPROTECTED:
            problems.append(f"the default of {E2E_ATTRIBUTE} is {default_id!r}, not -1: a "
                            f"message is protected only when the DBC says so")

    messages = []
    for message in sorted(db.messages, key=lambda m: m.frame_id):
        where = f"message {message.name}"
        attribute = message.dbc.attributes.get(E2E_ATTRIBUTE) if message.dbc else None
        data_id = attribute.value if attribute is not None else default_id
        data_id_valid = isinstance(data_id, int) and E2E_UNPROTECTED <= data_id <= 0xFFFF
        if not data_id_valid:
            problems.append(f"{where}: {E2E_ATTRIBUTE} {data_id!r} is not -1 or a 16-bit Data ID")
            data_id = E2E_UNPROTECTED
        model = Message(name=message.name, c_name=snake_case(message.name),
                        frame_id=message.frame_id, dlc=message.length,
                        cycle_ms=message.cycle_time or 0,
                        data_id=None if data_id == E2E_UNPROTECTED else data_id,
                        comment=text_of(message.comment), senders=list(message.senders or []),
                        e2e_known=data_id_valid)
        if message.is_extended_frame or message.frame_id > 0x7FF:
            problems.append(f"{where}: identifier 0x{message.frame_id:X} is not a base-format "
                            f"(11-bit) identifier")
        if not 0 <= message.length <= CLASSICAL_CAN_MAX_DLC:
            problems.append(f"{where}: DLC {message.length} is not 0..8 (classical CAN)")
        for signal in sorted(message.signals, key=lambda s: s.start):
            model.signals.append(signal_model(signal, f"{where}, signal {signal.name}",
                                              problems))
        messages.append(model)

    database = Database(path=shown, sha256=hashlib.sha256(raw).hexdigest(), messages=messages)
    check(database, problems)
    if problems:
        raise GeneratorError(shown, problems)
    return database


def signal_model(signal, where: str, problems: list[str]) -> Signal:
    found = len(problems)
    if signal.byte_order != "little_endian":
        problems.append(f"{where}: big-endian; only little-endian signals are supported")
    if signal.is_float:
        problems.append(f"{where}: a float signal; only integer signals are supported")
    if signal.is_multiplexer or signal.multiplexer_ids is not None:
        problems.append(f"{where}: multiplexed; multiplexing is not supported")
    if not 1 <= signal.length <= MAX_SIGNAL_BITS:
        problems.append(f"{where}: {signal.length} bits; 1 to {MAX_SIGNAL_BITS} are supported")

    factor, offset = exact(signal.scale), exact(signal.offset)
    for what, value in (("factor", factor), ("offset", offset)):
        if abs(value.numerator) > INT32_MAX or value.denominator > INT32_MAX:
            problems.append(f"{where}: {what} {decimal(value)} is not a fraction of two int32 "
                            f"numbers")
    if factor == 0:
        problems.append(f"{where}: factor 0")

    model = Signal(name=signal.name, c_name=snake_case(signal.name), start=signal.start,
                   length=signal.length, is_signed=signal.is_signed, factor=factor,
                   offset=offset, unit=signal.unit or "",
                   minimum=range_text(signal.minimum), maximum=range_text(signal.maximum),
                   choices=[], comment=text_of(signal.comment),
                   receivers=list(signal.receivers or []), supported=len(problems) == found)
    for value, name in sorted((signal.choices or {}).items(), key=lambda item: int(item[0])):
        name = getattr(name, "name", str(name))
        model.choices.append((int(value), name))
        if model.supported and not model.raw_min <= value <= model.raw_max:
            problems.append(f"{where}: value table entry {value} \"{name}\" is outside the raw "
                            f"range {model.raw_min}..{model.raw_max}")
    return model


def bits_of(signal: Signal) -> set[int]:
    """The frame bits of a little-endian signal: start is its least significant bit."""
    return set(range(signal.start, signal.start + signal.length))


def check(db: Database, problems: list[str]) -> None:
    """The rules that look at more than one signal or message."""
    seen_ids: dict[int, str] = {}
    for message in db.messages:
        where = f"message {message.name}"
        if message.frame_id in seen_ids:
            problems.append(f"{where}: identifier 0x{message.frame_id:03X} is also used by "
                            f"{seen_ids[message.frame_id]}")
        seen_ids.setdefault(message.frame_id, message.name)

        owner: dict[int, str] = {}
        for signal in message.signals:
            if not signal.supported:
                continue
            if signal.start < 0 or signal.end >= 8 * message.dlc:
                problems.append(f"{where}, signal {signal.name}: bits {signal.start}..{signal.end}"
                                f" are not all inside the {message.dlc} data bytes")
            overlapping = sorted({owner[bit] for bit in bits_of(signal) if bit in owner})
            for other in overlapping:
                problems.append(f"{where}: signals {other} and {signal.name} overlap")
            for bit in bits_of(signal):
                owner.setdefault(bit, signal.name)
        check_e2e_header(message, problems)

    by_crc: dict[int, Message] = {}
    for message in db.protected:
        crc = data_id_crc(message.data_id)
        if crc in by_crc:
            other = by_crc[crc]
            problems.append(f"messages {other.name} (Data ID 0x{other.data_id:04X}) and "
                            f"{message.name} (Data ID 0x{message.data_id:04X}): the CRC of the "
                            f"Data ID bytes is 0x{crc:02X} for both")
        by_crc.setdefault(crc, message)

    check_names(db, problems)


def check_e2e_header(message: Message, problems: list[str]) -> None:
    where = f"message {message.name}"
    by_name = {signal.name: signal for signal in message.signals}
    if not message.e2e_known:
        return
    if message.data_id is None:
        for name in E2E_HEADER:
            if name in by_name:
                problems.append(f"{where}: has the E2E header signal {name} but no "
                                f"{E2E_ATTRIBUTE}")
        return
    if message.cycle_ms <= 0:
        problems.append(f"{where}: protected end to end but not cyclic (GenMsgCycleTime 0): "
                        f"E2E detects lost frames only in periodic data")
    for name, (start, length) in E2E_HEADER.items():
        signal = by_name.get(name)
        if signal is None:
            problems.append(f"{where}: protected end to end but has no signal {name}")
        elif (signal.start, signal.length, signal.is_signed, signal.factor,
              signal.offset) != (start, length, False, 1, 0):
            problems.append(f"{where}: {name} must be unsigned and unscaled at bit {start}, "
                            f"{length} bits (E2E Profile 11A)")


def check_names(db: Database, problems: list[str]) -> None:
    """Every generated identifier is a C identifier, unique, and distinct from every other in
    its significant initial characters."""
    def valid(name: str) -> bool:
        return (re.fullmatch(r"[a-z][a-z0-9]*(_[a-z0-9]+)*", name) is not None
                and name not in C_RESERVED)

    # Every generated name is claimed exactly once; a second claim of the same significant
    # characters is a clash, whoever makes it.
    macros: dict[str, str] = {}
    functions: dict[str, str] = {}  # external linkage: rule 5.1
    types: dict[str, str] = {}      # no linkage: rule 5.2/5.6, 63 characters

    def claim(table: dict[str, str], significant: int, name: str, owner: str) -> None:
        key = name[:significant]
        if key in table:
            problems.append(f"{owner}: the generated name {name} clashes with a name of "
                            f"{table[key]} (the first {significant} characters are the same)")
        table.setdefault(key, owner)

    messages: dict[str, str] = {}
    for message in db.messages:
        owner = f"message {message.name}"
        if not valid(message.c_name):
            problems.append(f"{owner}: the name does not give a C identifier ({message.c_name})")
            continue
        if message.c_name in messages:
            problems.append(f"{owner}: gives the same C name, {message.c_name}, as message "
                            f"{messages[message.c_name]}")
            continue
        messages[message.c_name] = message.name
        for suffix in ("ID", "DLC", "CYCLE_MS", "E2E_PROTECTED", "E2E_DATA_ID"):
            claim(macros, SIGNIFICANT_MACRO, f"{message.macro}_{suffix}", owner)
        if message.signals:
            claim(functions, SIGNIFICANT_EXTERNAL, message.pack_name, owner)
            claim(functions, SIGNIFICANT_EXTERNAL, message.unpack_name, owner)
            claim(types, SIGNIFICANT_MACRO, message.c_type, owner)
        fields: dict[str, str] = {}
        for signal in message.signals:
            where = f"{owner}, signal {signal.name}"
            if not valid(signal.c_name):
                problems.append(f"{where}: the name does not give a C identifier "
                                f"({signal.c_name})")
                continue
            if signal.c_name in fields:
                problems.append(f"{where}: gives the same C name, {signal.c_name}, as signal "
                                f"{fields[signal.c_name]}")
                continue
            fields[signal.c_name] = signal.name
            for suffix in SIGNAL_MACRO_SUFFIXES:
                claim(macros, SIGNIFICANT_MACRO, signal_macro(message, signal, suffix), where)
            for _, choice in signal.choices:
                if re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", choice) is None:
                    problems.append(f"{where}: value table entry \"{choice}\" is not a C "
                                    f"identifier")
                else:
                    claim(macros, SIGNIFICANT_MACRO, signal_macro(message, signal, choice.upper()),
                          where)
        for text in (message.comment, *(signal.comment for signal in message.signals)):
            if "*/" in text or "/*" in text:
                problems.append(f"{owner}: a comment contains /* or */, which would end the C "
                                f"comment it is copied into")


SIGNAL_MACRO_SUFFIXES = ("START", "LENGTH", "RAW_MIN", "RAW_MAX", "FACTOR_NUM", "FACTOR_DEN",
                         "OFFSET_NUM", "OFFSET_DEN")


def signal_macro(message: Message, signal: Signal, suffix: str) -> str:
    return f"{message.macro}_{signal.c_name.upper()}_{suffix}"


# ------------------------------------------------------------------------------------------
# Writing C: the layout clang-format would give it
# ------------------------------------------------------------------------------------------


class Code:
    """Lines of C. Blocks of consecutive #defines are aligned as AlignConsecutiveMacros does;
    every line is checked against the column limit."""

    def __init__(self) -> None:
        self.lines: list[str] = []

    def line(self, text: str = "") -> None:
        self.lines.append(text)

    def comment(self, text: str, indent: str = "", doc: bool = False) -> None:
        """A block comment. Paragraphs are separated by blank lines; the lines of a paragraph
        are joined and wrapped at the column limit, except lines that begin with a space,
        which are kept as written (tables, code)."""
        self.line(f"{indent}/**" if doc else f"{indent}/*")
        prefix = f"{indent} * "

        def flush(words: list[str]) -> None:
            for wrapped in textwrap.wrap(" ".join(words), width=COLUMN_LIMIT - len(prefix),
                                         break_long_words=False, break_on_hyphens=False):
                self.line(f"{prefix}{wrapped}")
            words.clear()

        for number, paragraph in enumerate(text.split("\n\n")):
            if number:
                self.line(f"{indent} *")
            words: list[str] = []
            for raw_line in paragraph.split("\n"):
                if raw_line.startswith((" ", "@")):  # kept as written: tables, code, @file
                    flush(words)
                    self.line(f"{prefix}{raw_line}".rstrip())
                else:
                    words.append(raw_line)
            flush(words)
        self.line(f"{indent} */")

    def defines(self, pairs: list[tuple[str, str]]) -> None:
        width = max(len(name) for name, _ in pairs)
        for name, value in pairs:
            self.line(f"#define {name.ljust(width)} {value}")

    def trailing(self, pairs: list[tuple[str, str]]) -> None:
        """Consecutive lines of code with a comment behind each, the comments aligned one
        column after the longest line, as AlignTrailingComments does."""
        width = max(len(statement) for statement, _ in pairs)
        for statement, comment in pairs:
            self.line(f"{statement.ljust(width)} {comment}")

    def signature(self, head: str, params: list[str], tail: str) -> None:
        """`head(params)tail`, broken after a comma and aligned after the parenthesis where
        it does not fit, filling each line as far as it goes."""
        opening = f"{head}("
        current = opening
        for index, param in enumerate(params):
            piece = param + (f"){tail}" if index == len(params) - 1 else ",")
            if current == opening:
                current += piece
            elif len(current) + 1 + len(piece) <= COLUMN_LIMIT:
                current += " " + piece
            else:
                self.line(current)
                current = " " * len(opening) + piece
        self.line(current)

    def text(self) -> str:
        for number, text in enumerate(self.lines, start=1):
            if len(text) > COLUMN_LIMIT:
                raise GeneratorError("generated code", [
                    f"line {number} is {len(text)} columns long, more than {COLUMN_LIMIT}: "
                    f"{text.strip()[:60]}... (shorten the names in the DBC)"])
        return "\n".join(self.lines) + "\n"


def banner(db: Database, what: str) -> str:
    return (f"GENERATED FILE, DO NOT EDIT. {what} Made by tools/gen_can_db.py from {db.path}, "
            f"SHA-256 {db.sha256}.\n\n"
            "After a change of the DBC, run scripts/codegen-check.sh --update. Without --update "
            "the script fails while this file is out of date; CI and scripts/check.sh run it.")


def pack_params(message: Message) -> list[str]:
    return [f"const {message.c_type} *msg", "uint8_t *data", "size_t capacity"]


def unpack_params(message: Message) -> list[str]:
    return [f"{message.c_type} *msg", "const uint8_t *data", "size_t len"]


def e2e_text(message: Message) -> str:
    if message.data_id is None:
        return "Not protected end to end."
    return (f"Protected end to end (E2E Profile 11A), Data ID 0x{message.data_id:04X}; the CRC "
            f"of the Data ID bytes is 0x{data_id_crc(message.data_id):02X}.")


def timing_text(message: Message) -> str:
    return f"every {message.cycle_ms} ms" if message.cycle_ms else "sent on events"


def scaling_text(signal: Signal) -> str:
    unit = f" {signal.unit}" if signal.unit else ""
    if (signal.factor, signal.offset) == (1, 0):
        formula = f"physical = raw{unit}"
    else:
        formula = f"physical = raw * {decimal(signal.factor)}"
        if signal.offset != 0:
            sign = "-" if signal.offset < 0 else "+"
            formula += f" {sign} {decimal(abs(signal.offset))}"
        formula += unit
    parts = [formula]
    if signal.minimum is not None and signal.maximum is not None:
        parts.append(f"range {signal.minimum}..{signal.maximum}{unit}")
    return ", ".join(parts)


def signal_text(signal: Signal) -> str:
    kind = "signed" if signal.is_signed else "unsigned"
    where = (f"bit {signal.start} (1 bit" if signal.length == 1
             else f"bits {signal.start}..{signal.end} ({signal.length} bits")
    text = (f"{signal.name}: {where}, {kind}); raw {signal.raw_min}..{signal.raw_max}; "
            f"{scaling_text(signal)}.")
    if signal.choices:
        text += " Value table: " + ", ".join(f"{value} {name}" for value, name in signal.choices)
        text += "."
    if signal.comment:
        text += f" {signal.comment}"
    return text


HEADER_INTRO = """\
The node's CAN messages: identifiers, timing, end-to-end protection, and a codec for the
signals of each message. docs/can-protocol.md describes the messages.

WHAT IS HERE. For every message of the DBC:
    CAN_DB_<MSG>_ID             identifier, base format (11 bits)
    CAN_DB_<MSG>_DLC            number of data bytes
    CAN_DB_<MSG>_CYCLE_MS       cycle time in ms; 0 for a message sent on events
    CAN_DB_<MSG>_E2E_PROTECTED  1U if protected end to end (AUTOSAR E2E Profile 11A), else 0U
    CAN_DB_<MSG>_E2E_DATA_ID    the Data ID; defined for protected messages only, so that
                                using it for any other message does not compile
and for every message with signals a struct can_db_<msg>_t of the raw signal values, with
can_db_<msg>_pack() and can_db_<msg>_unpack(). For every signal:
    CAN_DB_<MSG>_<SIG>_START, _LENGTH   first bit and number of bits; bit 0 is the least
                                        significant bit of byte 0, and bit i of the raw value
                                        is frame bit START + i (little-endian)
    _RAW_MIN, _RAW_MAX                  range of the raw value
    _FACTOR_NUM, _FACTOR_DEN            physical = raw * FACTOR_NUM / FACTOR_DEN
    _OFFSET_NUM, _OFFSET_DEN                       + OFFSET_NUM / OFFSET_DEN, exactly;
                                        int32_t constants, the fractions in lowest terms
    _<NAME>                             raw value of an entry of the signal's value table;
                                        _SNA ("signal not available") where the table has it
Raw values of unsigned signals are unsigned constants (4095U), those of signed signals
constants of the struct member's type (((int16_t)(-32768))).

RAW VALUES. The structs hold the bits on the wire as an integer, in the smallest fixed-width
type that holds them; a signed signal is a two's complement number. Converting to and from
physical units is left to the code that produces or uses a value, which knows how to round
and saturate and when to send SNA. That code can check its assumptions about the DBC at
compile time, one relation per assertion, for example
    _Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP1_OFFSET_NUM == -40, "offset -40 degC");
so that a change of the DBC it does not follow breaks the build instead of the data.

PACK AND UNPACK. pack() writes all DLC data bytes: every signal from its struct member, every
bit that belongs to no signal as 0. A member's bits above the signal's length are masked off,
not saturated: a value cannot reach another signal's bits, but 4096 in a 12-bit signal goes
out as 0. unpack() reads every signal into its member; a signed signal is sign-extended. Both
refuse NULL pointers and a buffer of the wrong size, and then return false without touching
the output. The E2E header signals (E2E_Crc, E2E_Counter) are packed like any other; the
end-to-end protection overwrites them after pack().

CONTEXT. No state, no allocation, no operating system: every function may be called from any
task or interrupt, also concurrently, as long as no two calls write the same buffer."""


def render_header(db: Database) -> str:
    code = Code()
    code.line("/* SPDX-License-Identifier: MIT */")
    code.comment(f"@file\n{HEADER_INTRO}\n\n{banner(db, 'The interface of the codec.')}", doc=True)
    code.line("#ifndef FCN_CAN_DB_H")
    code.line("#define FCN_CAN_DB_H")
    code.line()
    code.line("#include <stdbool.h>")
    code.line("#include <stddef.h>")
    code.line("#include <stdint.h>")
    for message in db.messages:
        code.line()
        code.line("/* " + "-" * (COLUMN_LIMIT - 3))
        receivers = ", ".join(message.receivers) or "-"
        senders = ", ".join(message.senders) or "-"
        intro = (f"{message.name}: 0x{message.frame_id:03X}, {message.dlc} bytes, "
                 f"{timing_text(message)}; from {senders} to {receivers}. {e2e_text(message)}")
        for text in (intro, message.comment):
            for wrapped in textwrap.wrap(text, width=COLUMN_LIMIT - 3, break_long_words=False,
                                         break_on_hyphens=False):
                code.line(f" * {wrapped}")
        code.line(" * " + "-" * (COLUMN_LIMIT - 6) + " */")
        pairs = [(f"{message.macro}_ID", f"0x{message.frame_id:03X}U"),
                 (f"{message.macro}_DLC", c_literal_unsigned(message.dlc)),
                 (f"{message.macro}_CYCLE_MS", c_literal_unsigned(message.cycle_ms)),
                 (f"{message.macro}_E2E_PROTECTED",
                  c_literal_unsigned(0 if message.data_id is None else 1))]
        if message.data_id is not None:
            pairs.append((f"{message.macro}_E2E_DATA_ID", f"0x{message.data_id:04X}U"))
        code.defines(pairs)
        if not message.signals:
            code.line()
            code.line("/* No signals: the payload is passed on as raw bytes. */")
            continue
        for signal in message.signals:
            code.line()
            code.comment(signal_text(signal))
            pairs = [(signal_macro(message, signal, "START"), c_literal_unsigned(signal.start)),
                     (signal_macro(message, signal, "LENGTH"), c_literal_unsigned(signal.length)),
                     (signal_macro(message, signal, "RAW_MIN"), signal.raw_literal(signal.raw_min)),
                     (signal_macro(message, signal, "RAW_MAX"), signal.raw_literal(signal.raw_max)),
                     (signal_macro(message, signal, "FACTOR_NUM"),
                      c_literal_signed(signal.factor.numerator)),
                     (signal_macro(message, signal, "FACTOR_DEN"),
                      c_literal_signed(signal.factor.denominator)),
                     (signal_macro(message, signal, "OFFSET_NUM"),
                      c_literal_signed(signal.offset.numerator)),
                     (signal_macro(message, signal, "OFFSET_DEN"),
                      c_literal_signed(signal.offset.denominator))]
            pairs += [(signal_macro(message, signal, name.upper()), signal.raw_literal(value))
                      for value, name in signal.choices]
            code.defines(pairs)
        code.line()
        code.line(f"/** The raw signal values of {message.name}, in the order of their start "
                  f"bits. */")
        code.line("typedef struct {")
        code.trailing([(f"    {signal.c_type} {signal.c_name};", f"/**< {signal.name} */")
                       for signal in message.signals])
        code.line(f"}} {message.c_type};")
        code.line()
        code.comment(f"Write the {message.macro}_DLC data bytes of {message.name} into data[0] "
                     f"to data[{message.dlc - 1}]. Returns false, writing nothing, if a pointer "
                     f"is NULL or `capacity`, the size of `data` in bytes, is less than the DLC.",
                     doc=True)
        code.signature(f"bool {message.pack_name}", pack_params(message), ";")
        code.line()
        code.comment(f"Read the signals of {message.name} from `len` data bytes. Returns false, "
                     f"leaving *msg as it was, if a pointer is NULL or `len` is not "
                     f"{message.macro}_DLC.", doc=True)
        code.signature(f"bool {message.unpack_name}", unpack_params(message), ";")
    code.line()
    code.line("#endif /* FCN_CAN_DB_H */")
    return code.text()


SOURCE_HELPERS = """\
/* The number of bits from frame bit `pos` up to the end of its byte, at most `remaining`. */
static uint32_t chunk_bits(uint32_t pos, uint32_t remaining)
{
    uint32_t chunk = 8U - (pos % 8U);

    if (chunk > remaining) {
        chunk = remaining;
    }

    return chunk;
}

/*
 * Write the `length` low bits of `raw` into the frame from frame bit `start` on: bit i of
 * `raw` goes to bit (start + i) % 8 of data[(start + i) / 8]. Bits of `raw` above `length` are
 * not written. The bits are ORed in, so the frame must hold 0 there; pack() clears it first.
 * The generator has checked 1 <= length <= 32 and that the bits lie inside the DLC.
 */
static void put_bits(uint8_t *data, uint32_t start, uint32_t length, uint32_t raw)
{
    uint32_t done = 0U;

    while (done < length) {
        const uint32_t pos = start + done;
        const uint32_t chunk = chunk_bits(pos, length - done);
        const uint32_t part = (raw >> done) & (0xFFU >> (8U - chunk));

        data[pos / 8U] |= (uint8_t)(part << (pos % 8U));
        done += chunk;
    }
}

/* The `length`-bit value from frame bit `start` on, as put_bits() wrote it; 0 above `length`. */
static uint32_t get_bits(const uint8_t *data, uint32_t start, uint32_t length)
{
    uint32_t raw = 0U;
    uint32_t done = 0U;

    while (done < length) {
        const uint32_t pos = start + done;
        const uint32_t chunk = chunk_bits(pos, length - done);
        const uint32_t part = ((uint32_t)data[pos / 8U] >> (pos % 8U)) & (0xFFU >> (8U - chunk));

        raw |= part << done;
        done += chunk;
    }

    return raw;
}
"""

SOURCE_SIGN_HELPER = """\
/*
 * The value of the `length`-bit two's complement number in `raw`, whose bits above `length`
 * are 0. A negative number is computed as -magnitude - 1, with magnitude the complement of its
 * low length - 1 bits, so that no unsigned value outside the range of int32_t is ever
 * converted to it (C leaves the result of that conversion to the implementation).
 */
static int32_t to_signed(uint32_t raw, uint32_t length)
{
    const uint32_t sign = (uint32_t)1U << (length - 1U);
    int32_t value;

    if ((raw & sign) != 0U) {
        const uint32_t magnitude = (~raw) & (sign - 1U);

        value = (-(int32_t)magnitude) - 1;
    } else {
        value = (int32_t)raw;
    }

    return value;
}
"""


def render_source(db: Database) -> str:
    code = Code()
    code.line("/* SPDX-License-Identifier: MIT */")
    code.comment(
        "The codec of can_db.h. Every pack and unpack function is a list of calls of the helpers "
        "below, one per signal, with the signal's start bit and length as constants; the "
        "helpers do the bit work for every signal alike.\n\n"
        + banner(db, "The implementation of the codec."))
    code.line('#include "can_db.h"')
    code.line()
    code.line("#include <string.h>")
    with_signals = [message for message in db.messages if message.signals]
    if with_signals:
        code.line()
        code.lines += SOURCE_HELPERS.rstrip("\n").split("\n")
    if any(signal.is_signed for message in with_signals for signal in message.signals):
        code.line()
        code.lines += SOURCE_SIGN_HELPER.rstrip("\n").split("\n")
    for message in with_signals:
        code.line()
        code.signature(f"bool {message.pack_name}", pack_params(message), "")
        code.line("{")
        code.line("    bool packed = false;")
        code.line()
        code.line(f"    if ((msg != NULL) && (data != NULL) && "
                  f"(capacity >= {message.macro}_DLC)) {{")
        code.line(f"        (void)memset(data, 0, {message.macro}_DLC);")
        for signal in message.signals:
            value = (f"(uint32_t)msg->{signal.c_name}" if signal.is_signed
                     else f"msg->{signal.c_name}")
            code.line(f"        put_bits(data, {signal.start}U, {signal.length}U, {value});")
        code.line("        packed = true;")
        code.line("    }")
        code.line()
        code.line("    return packed;")
        code.line("}")
        code.line()
        code.signature(f"bool {message.unpack_name}", unpack_params(message), "")
        code.line("{")
        code.line("    bool unpacked = false;")
        code.line()
        code.line(f"    if ((msg != NULL) && (data != NULL) && (len == {message.macro}_DLC)) {{")
        for signal in message.signals:
            bits = f"get_bits(data, {signal.start}U, {signal.length}U)"
            if signal.is_signed:
                value = f"({signal.c_type})to_signed({bits}, {signal.length}U)"
            elif signal.c_bits == 32:
                value = bits
            else:
                value = f"({signal.c_type}){bits}"
            code.line(f"        msg->{signal.c_name} = {value};")
        code.line("        unpacked = true;")
        code.line("    }")
        code.line()
        code.line("    return unpacked;")
        code.line("}")
    return code.text()


def render_test_table(db: Database) -> str:
    code = Code()
    code.line("/* SPDX-License-Identifier: MIT */")
    code.comment(
        "The table of can_db_table.h for the codec generated from the same DBC: every message, "
        "every signal, and the adapters between arrays of raw values and the structs.\n\n"
        + banner(db, "Test support, not part of the node."))
    code.line('#include "can_db_table.h"')
    code.line()
    code.line('#include "can_db.h"')
    with_signals = [message for message in db.messages if message.signals]
    for message in with_signals:
        code.line()
        code.signature(f"static bool pack_{message.c_name}",
                       ["const int64_t *raw", "uint8_t *data", "size_t capacity"], "")
        code.line("{")
        code.line(f"    {message.c_type} msg;")
        code.line()
        for index, signal in enumerate(message.signals):
            code.line(f"    msg.{signal.c_name} = ({signal.c_type})raw[{index}];")
        code.line(f"    return {message.pack_name}(&msg, data, capacity);")
        code.line("}")
        code.line()
        code.signature(f"static bool unpack_{message.c_name}",
                       ["const uint8_t *data", "size_t len", "int64_t *raw"], "")
        code.line("{")
        code.line(f"    {message.c_type} msg = {{0}};")
        code.line(f"    const bool unpacked = {message.unpack_name}(&msg, data, len);")
        code.line()
        code.line("    if (unpacked) {")
        for index, signal in enumerate(message.signals):
            code.line(f"        raw[{index}] = msg.{signal.c_name};")
        code.line("    }")
        code.line()
        code.line("    return unpacked;")
        code.line("}")
        code.line()
        code.line(f"static const can_db_table_signal_t SIGNALS_{message.c_name.upper()}[] = {{")
        # One entry per line: the comment behind each keeps clang-format from packing them.
        entries = []
        for signal in message.signals:
            values = ", ".join([f"\"{signal.name}\"", f"{signal.start}U", f"{signal.length}U",
                                "true" if signal.is_signed else "false",
                                str(signal.raw_min), str(signal.raw_max), str(signal.field_min),
                                str(signal.field_max)])
            entries.append((f"    {{{values}}},", f"/* {signal.c_type} {signal.c_name} */"))
        code.trailing(entries)
        code.line("};")
    code.line()
    code.line("const can_db_table_message_t can_db_table[] = {")
    for message in db.messages:
        data_id = -1 if message.data_id is None else message.data_id
        code.line("    {")
        code.line(f"        .name = \"{message.name}\",")
        code.line(f"        .id = {message.macro}_ID,")
        code.line(f"        .dlc = {message.macro}_DLC,")
        code.line(f"        .cycle_ms = {message.macro}_CYCLE_MS,")
        code.line(f"        .e2e_data_id = {data_id},")
        code.line(f"        .signal_count = {len(message.signals)}U,")
        if message.signals:
            code.line(f"        .signals = SIGNALS_{message.c_name.upper()},")
            code.line(f"        .pack = pack_{message.c_name},")
            code.line(f"        .unpack = unpack_{message.c_name},")
        else:
            code.line("        .signals = NULL,")
            code.line("        .pack = NULL,")
            code.line("        .unpack = NULL,")
        code.line("    },")
    code.line("};")
    code.line()
    code.line("const size_t can_db_table_count = sizeof(can_db_table) / sizeof(can_db_table[0]);")
    return code.text()


# ------------------------------------------------------------------------------------------
# Command line
# ------------------------------------------------------------------------------------------


def outputs(db: Database, out_dir: Path, test_table: Path | None) -> dict[Path, str]:
    files = {out_dir / HEADER_NAME: render_header(db), out_dir / SOURCE_NAME: render_source(db)}
    if test_table is not None:
        files[test_table] = render_test_table(db)
    return files


def check_files(files: dict[Path, str]) -> list[str]:
    """Unified diffs of every file whose content on disk is not what was generated."""
    diffs = []
    for path, content in files.items():
        current = path.read_text(encoding="utf-8") if path.is_file() else ""
        if current != content:
            shown = display_path(path) if path.is_absolute() else path.as_posix()
            diffs.append("".join(difflib.unified_diff(
                current.splitlines(keepends=True), content.splitlines(keepends=True),
                fromfile=f"{shown} (on disk{'' if path.is_file() else ', missing'})",
                tofile=f"{shown} (generated)")))
    return diffs


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="gen_can_db.py", description="Generate the C codec of the CAN messages from a DBC.")
    parser.add_argument("dbc", type=Path, help="the DBC, e.g. dbc/fcn.dbc")
    parser.add_argument("out_dir", type=Path, help="where can_db.h and can_db.c go, e.g. app/gen")
    parser.add_argument("--test-table", type=Path, default=None,
                        help="also generate this table of the codec for the tests "
                             "(tests/tools/can_db_table.c)")
    parser.add_argument("--check", action="store_true",
                        help="write nothing; exit 1 with a diff if a file is not up to date")
    args = parser.parse_args(argv)

    try:
        db = load(args.dbc)
        files = outputs(db, args.out_dir, args.test_table)
    except GeneratorError as error:
        print(error, file=sys.stderr)
        return 1
    except OSError as error:
        print(f"gen_can_db.py: {error}", file=sys.stderr)
        return 1

    if args.check:
        diffs = check_files(files)
        for diff in diffs:
            sys.stdout.write(diff)
        if diffs:
            print(f"gen_can_db.py: {len(diffs)} of {len(files)} generated files differ from what "
                  f"{db.path} gives; run the same command without --check to regenerate them",
                  file=sys.stderr)
            return 1
        return 0

    for path, content in files.items():
        if not path.is_file() or path.read_text(encoding="utf-8") != content:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
