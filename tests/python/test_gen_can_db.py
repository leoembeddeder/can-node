# SPDX-License-Identifier: MIT
"""The DBC, the generator tools/gen_can_db.py, and the C codec it generates, against cantools.

cantools reads dbc/fcn.dbc on its own and is the oracle here. What is checked:

  the DBC          it loads in the pinned cantools without a warning, and holds the message set
                   of the design, with comments, cycle times, Data IDs and SNA codes;
  the header       every constant of app/gen/can_db.h (identifiers, DLCs, cycle times, Data IDs,
                   positions, raw ranges, value tables, factor and offset as fractions) is what
                   cantools reads from the DBC;
  the codec        the release build of tests/tools/can_db_cli.c, which puts the generated codec
                   behind a pipe, packs random raw values into the bytes cantools encodes from
                   them, unpacks random frames into the values cantools decodes, and masks values
                   wider than their signals, for every message; all lines go to ONE run of it;
  the generator    on a DBC made for the purpose with signals of every length from 1 to 32 bits,
                   signed and unsigned, each starting at two or more bit positions within a
                   byte, and signed and unsigned signals starting at every bit position within
                   a byte: the generated code is compiled here with the project's warning set
                   under AddressSanitizer and UndefinedBehaviorSanitizer and compared with
                   cantools the same way. fcn.dbc
                   has a single signed signal, of 16 bits; sign extension from any other length
                   is only exercised here;
  the rules        each rule of the generator, fed a DBC that breaks it, reports exactly that;
  --check          is silent for current files and fails with a diff for stale ones;
  the document     the tables of docs/can-protocol.md agree with the DBC.
"""
from __future__ import annotations

import importlib.util
import os
import random
import re
import shutil
import subprocess
import sys
import warnings
from collections.abc import Sequence
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path

import cantools
import pytest

from fcn_tools import paths

DBC = paths.REPO_ROOT / "dbc" / "fcn.dbc"
GEN_DIR = paths.REPO_ROOT / "app" / "gen"
TOOLS_DIR = paths.REPO_ROOT / "tests" / "tools"
TEST_TABLE = TOOLS_DIR / "can_db_table.c"
DOC = paths.REPO_ROOT / "docs" / "can-protocol.md"
CLI = "can_db_cli"
CLI_TIMEOUT_S = 120


def load_generator():
    """tools/gen_can_db.py of this checkout, whatever sys.path holds."""
    spec = importlib.util.spec_from_file_location("gen_can_db",
                                                  paths.REPO_ROOT / "tools" / "gen_can_db.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["gen_can_db"] = module  # dataclasses look their module up here
    spec.loader.exec_module(module)
    return module


gen = load_generator()


def load_dbc(path: Path = DBC) -> cantools.database.can.Database:
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        return cantools.database.load_file(path, database_format="dbc", strict=True)


def by_start(message) -> list:
    return sorted(message.signals, key=lambda signal: signal.start)


# ------------------------------------------------------------------------------------------
# The DBC
# ------------------------------------------------------------------------------------------

# The message set of the design: identifier, name, sender, DLC, cycle time (0: events), Data ID
# (None: not protected end to end).
MESSAGE_SET = [
    (0x010, "BMS_Fault", "BMS", 8, 100, 0x0110),
    (0x020, "BMS_PackStatus", "BMS", 8, 10, 0x0120),
    (0x030, "BMS_Heartbeat", "BMS", 8, 10, None),
    (0x040, "BMS_CellVoltages1", "BMS", 8, 20, 0x0140),
    (0x041, "BMS_CellVoltages2", "BMS", 8, 20, 0x0141),
    (0x050, "BMS_Temperatures", "BMS", 8, 100, 0x0150),
    (0x100, "TESTER_Command", "TESTER", 8, 100, 0x0200),
    (0x600, "BMS_TaskStats", "BMS", 8, 200, None),
    (0x610, "BMS_NodeInfo", "BMS", 8, 1000, None),
    (0x611, "BMS_BootInfo", "BMS", 8, 1000, None),
    (0x6F0, "TESTER_Ping", "TESTER", 8, 0, None),
    (0x6F1, "BMS_Pong", "BMS", 8, 0, None),
    (0x7E0, "UDS_Request", "TESTER", 8, 0, None),
    (0x7E8, "UDS_Response", "BMS", 8, 0, None),
]


def test_the_dbc_loads_in_the_pinned_cantools_without_a_warning():
    pinned = re.search(r"^cantools==(\S+)$",
                       (paths.REPO_ROOT / "requirements.txt").read_text(), re.MULTILINE)
    assert pinned is not None and cantools.__version__ == pinned[1] == "45.1.2"
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        cantools.database.load_file(DBC, database_format="dbc", strict=True)
    assert [str(warning.message) for warning in caught] == []


def test_the_dbc_holds_the_message_set_of_the_design():
    db = load_dbc()
    model = gen.load(DBC)
    got = [(m.frame_id, m.name, m.senders[0], m.length, m.cycle_time or 0)
           for m in sorted(db.messages, key=lambda m: m.frame_id)]
    assert got == [row[:5] for row in MESSAGE_SET]
    assert [m.data_id for m in model.messages] == [row[5] for row in MESSAGE_SET]
    assert all(not m.is_extended_frame and len(m.senders) == 1 for m in db.messages)


def test_every_message_and_every_signal_has_a_comment():
    db = load_dbc()
    assert db.dbc is not None
    for message in db.messages:
        assert message.comment, message.name
        for signal in message.signals:
            assert signal.comment, f"{message.name}.{signal.name}"


def test_the_heartbeat_is_the_phase_1_status_frame():
    """bytes 0..3 the period counter, bytes 4..7 the tick count, both u32 little-endian."""
    heartbeat = load_dbc().get_message_by_frame_id(0x030)
    layout = [(s.name, s.start, s.length, s.is_signed, s.byte_order, s.scale, s.offset)
              for s in by_start(heartbeat)]
    assert layout == [("PeriodCounter", 0, 32, False, "little_endian", 1, 0),
                      ("TickCount", 32, 32, False, "little_endian", 1, 0)]
    frame = heartbeat.encode({"PeriodCounter": 0x04030201, "TickCount": 0x08070605})
    assert frame == bytes(range(1, 9))


def test_cells_and_temperatures_have_their_scaling_and_an_sna_code():
    db = load_dbc()
    for name in ("BMS_CellVoltages1", "BMS_CellVoltages2"):
        cells = [s for s in by_start(db.get_message_by_name(name)) if s.name.startswith("Cell")]
        assert [(s.start, s.length) for s in cells] == [(16, 12), (28, 12), (40, 12), (52, 12)]
        for signal in cells:
            assert (gen.exact(signal.scale), gen.exact(signal.offset)) == (Fraction(1, 1000), 1)
            assert signal.unit == "V" and dict(signal.choices) == {4095: "SNA"}
    temps = [s for s in by_start(db.get_message_by_name("BMS_Temperatures"))
             if s.name.startswith("Temp")]
    assert [(s.start, s.length) for s in temps] == [(16, 8), (24, 8), (32, 8), (40, 8)]
    for signal in temps:
        assert (signal.scale, signal.offset, signal.unit) == (1, -40, "degC")
        assert dict(signal.choices) == {255: "SNA"}


def test_the_e2e_data_ids_have_distinct_crcs():
    model = gen.load(DBC)
    crcs = {m.name: gen.data_id_crc(m.data_id) for m in model.protected}
    assert len(crcs) == 6 and len(set(crcs.values())) == 6, crcs


# ------------------------------------------------------------------------------------------
# The CRC the generator checks the Data IDs with
# ------------------------------------------------------------------------------------------


def test_the_profile_11_crc_reproduces_its_check_value_and_the_published_vectors():
    assert gen.crc8_p11(b"123456789") == 0x37
    # PRS_E2EProtocol R24-11, Tables 6.48 and 6.49: Data ID 0x123, variant 11A, data all zero,
    # counter 0 and 1. The CRC covers the Data ID bytes, low first, then bytes 1..7.
    assert gen.crc8_p11(bytes([0x23, 0x01, 0x00]) + bytes(6)) == 0xCC
    assert gen.crc8_p11(bytes([0x23, 0x01, 0x01]) + bytes(6)) == 0x91


def test_data_ids_whose_crcs_collide_are_found():
    # The 11-bit Data IDs that share the CRC of 0x123, as the E2E research computed them
    # independently (scratch e2e-reproduction.md, section 5d item 2).
    colliding = [i for i in range(0x800) if gen.data_id_crc(i) == gen.data_id_crc(0x123)]
    assert colliding == [0x0A0, 0x123, 0x2BB, 0x338, 0x496, 0x515, 0x68D, 0x70E]


# ------------------------------------------------------------------------------------------
# The generated header against cantools
# ------------------------------------------------------------------------------------------

DEFINE = re.compile(r"^#define (CAN_DB_\w+) +(.+)$", re.MULTILINE)


TYPED = re.compile(r"\(\((int8_t|int16_t|int32_t)\)(.+)\)")


def c_value(text: str) -> int:
    """The value of a constant of the header: 0x020U, 4095U, ((int32_t)1000),
    ((int32_t)(-40)), ((int32_t)(-2147483647 - 1))."""
    typed = TYPED.fullmatch(text.strip())
    if typed is not None:
        text = typed[2]
    if text == "(-2147483647 - 1)":
        return -(1 << 31)
    if re.fullmatch(r"\(-\d+\)|-?\d+|\d+U|0x[0-9A-F]+U", text) is None:
        raise ValueError(f"not a constant of the header: {text!r}")
    return int(text.strip("()").rstrip("U"), 0)


def header_macros(text: str) -> dict[str, int]:
    macros = {}
    for name, value in DEFINE.findall(text):
        assert name not in macros, f"{name} is defined twice"
        macros[name] = c_value(value)
        # The fractions are int32_t; every other constant is unsigned or of a member's type.
        if name.endswith(("_NUM", "_DEN")):
            assert value.startswith("((int32_t)"), (name, value)
    return macros


def expected_macros(db) -> dict[str, int]:
    """Every constant the header has to define, computed from cantools' reading of the DBC."""
    expected = {}
    for message in db.messages:
        prefix = f"CAN_DB_{gen.snake_case(message.name).upper()}"
        attribute = message.dbc.attributes.get("FcnE2eDataId")
        expected[f"{prefix}_ID"] = message.frame_id
        expected[f"{prefix}_DLC"] = message.length
        expected[f"{prefix}_CYCLE_MS"] = message.cycle_time or 0
        expected[f"{prefix}_E2E_PROTECTED"] = 0 if attribute is None else 1
        if attribute is not None:
            expected[f"{prefix}_E2E_DATA_ID"] = attribute.value
        for signal in message.signals:
            name = f"{prefix}_{gen.snake_case(signal.name).upper()}"
            low = -(1 << (signal.length - 1)) if signal.is_signed else 0
            high = (1 << (signal.length - 1)) - 1 if signal.is_signed else (1 << signal.length) - 1
            factor = Fraction(str(signal.scale))
            offset = Fraction(str(signal.offset))
            expected.update({f"{name}_START": signal.start, f"{name}_LENGTH": signal.length,
                             f"{name}_RAW_MIN": low, f"{name}_RAW_MAX": high,
                             f"{name}_FACTOR_NUM": factor.numerator,
                             f"{name}_FACTOR_DEN": factor.denominator,
                             f"{name}_OFFSET_NUM": offset.numerator,
                             f"{name}_OFFSET_DEN": offset.denominator})
            for value, choice in (signal.choices or {}).items():
                expected[f"{name}_{str(choice).upper()}"] = value
    return expected


def test_every_constant_of_the_header_is_what_cantools_reads_from_the_dbc():
    macros = header_macros((GEN_DIR / "can_db.h").read_text())
    expected = expected_macros(load_dbc())
    assert macros == expected
    # Spot checks of the fractions in the units of the design.
    assert macros["CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_FACTOR_DEN"] == 100
    assert macros["CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_OFFSET_NUM"] == 1
    assert macros["CAN_DB_BMS_TEMPERATURES_TEMP3_OFFSET_NUM"] == -40
    assert macros["CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_SNA"] == 4095


def test_the_fractions_are_exact_where_floats_are_not():
    # 0.01 is no binary fraction: the float that cantools holds is not 1/100, and a generator
    # that turned it into a fraction directly would emit 5764607523034235/576460752303423488.
    assert Fraction(0.01) != Fraction(1, 100)
    assert gen.exact(0.01) == Fraction(1, 100)
    assert gen.exact(0.001) == Fraction(1, 1000)
    assert gen.exact(-40) == -40
    assert gen.decimal(Fraction(-32768, 100)) == "-327.68"
    assert gen.decimal(Fraction(2547, 500)) == "5.094"
    assert gen.decimal(Fraction(1, 3)) == "1/3"


# ------------------------------------------------------------------------------------------
# The committed files and --check
# ------------------------------------------------------------------------------------------


def generator_args(out_dir: Path, table: Path) -> list[str]:
    return [str(DBC), str(out_dir), "--test-table", str(table)]


def test_the_committed_files_are_what_the_dbc_gives():
    assert gen.main(["--check", *generator_args(GEN_DIR, TEST_TABLE)]) == 0


def test_the_output_depends_on_the_dbc_alone(tmp_path, monkeypatch):
    """Not on the working directory, nor on how the path to the DBC is written."""
    first = gen.outputs(gen.load(DBC), tmp_path, tmp_path / "table.c")
    monkeypatch.chdir(tmp_path)
    second = gen.outputs(gen.load(Path(os.path.relpath(DBC, tmp_path))), tmp_path,
                         tmp_path / "table.c")
    assert first == second
    header = re.sub(r"\n \* ", " ", first[tmp_path / "can_db.h"])  # the comment, unwrapped
    sha = gen.hashlib.sha256(DBC.read_bytes()).hexdigest()
    assert f"from dbc/fcn.dbc, SHA-256 {sha}." in header
    for text in first.values():
        assert not re.search(r"20\d\d-\d\d-\d\d|\d\d:\d\d:\d\d", text), "a time stamp"
        assert str(paths.REPO_ROOT) not in text, "an absolute path"
        assert all(len(line) <= 100 for line in text.splitlines())
        assert text.isascii() and "\r" not in text


def test_check_is_silent_for_current_files_and_writes_nothing(tmp_path, capsys):
    out, table = tmp_path / "gen", tmp_path / "table.c"
    assert gen.main(generator_args(out, table)) == 0
    stamps = {path: path.stat().st_mtime_ns for path in (out / "can_db.h", out / "can_db.c", table)}
    capsys.readouterr()
    assert gen.main(["--check", *generator_args(out, table)]) == 0
    assert capsys.readouterr() == ("", "")
    assert {path: path.stat().st_mtime_ns for path in stamps} == stamps
    # Writing again leaves unchanged files alone too.
    assert gen.main(generator_args(out, table)) == 0
    assert {path: path.stat().st_mtime_ns for path in stamps} == stamps


def test_check_fails_with_a_diff_for_a_changed_or_a_missing_file(tmp_path, capsys):
    out, table = tmp_path / "gen", tmp_path / "table.c"
    assert gen.main(generator_args(out, table)) == 0
    source = out / "can_db.c"
    source.write_text(source.read_text().replace("put_bits(data, 16U, 16U,",
                                                 "put_bits(data, 17U, 16U,"))
    table.unlink()
    capsys.readouterr()
    assert gen.main(["--check", *generator_args(out, table)]) == 1
    printed = capsys.readouterr()
    assert "-        put_bits(data, 17U, 16U, (uint32_t)msg->pack_current);" in printed.out
    assert "+        put_bits(data, 16U, 16U, (uint32_t)msg->pack_current);" in printed.out
    assert "table.c (on disk, missing)" in printed.out
    assert "2 of 3 generated files differ" in printed.err
    assert not table.exists(), "--check wrote a file"


def test_the_command_line_reports_a_broken_dbc_on_stderr(tmp_path, capsys):
    broken = tmp_path / "broken.dbc"
    broken.write_text(DBC.read_text().replace("SG_ Cell2Voltage : 28|12",
                                              "SG_ Cell2Voltage : 26|12"))
    assert gen.main([str(broken), str(tmp_path / "out")]) == 1
    err = capsys.readouterr().err
    assert ("broken.dbc: message BMS_CellVoltages1: signals Cell1Voltage and Cell2Voltage "
            "overlap") in err
    assert not (tmp_path / "out").exists()


# ------------------------------------------------------------------------------------------
# The rules, each fed a DBC that breaks it
# ------------------------------------------------------------------------------------------

# Valid as it is: one protected and one plain message. Each case below changes it in one way.
TEMPLATE = """VERSION ""

NS_ :
\tCM_
\tBA_DEF_
\tBA_
\tVAL_
\tBA_DEF_DEF_
\tSIG_VALTYPE_

BS_:

BU_: BMS TESTER

BO_ 32 Prot: 8 BMS
 SG_ E2E_Crc : 0|8@1+ (1,0) [0|255] "" TESTER
 SG_ E2E_Counter : 8|4@1+ (1,0) [0|14] "" TESTER
 SG_ Value : 16|12@1+ (0.001,1) [1|5.094] "V" TESTER
 SG_ Other : 28|4@1- (1,0) [-8|7] "" TESTER

BO_ 48 Plain: 8 BMS
 SG_ Counter : 0|32@1+ (1,0) [0|4294967295] "" TESTER

CM_ BO_ 48 "A plain message.";
BA_DEF_ BO_ "GenMsgCycleTime" INT 0 65535;
BA_DEF_ BO_ "FcnE2eDataId" INT -1 65535;
BA_DEF_DEF_ "GenMsgCycleTime" 0;
BA_DEF_DEF_ "FcnE2eDataId" -1;
BA_ "GenMsgCycleTime" BO_ 32 10;
BA_ "FcnE2eDataId" BO_ 32 288;
VAL_ 32 Value 4095 "SNA" ;
"""

PLAIN_COUNTER = ' SG_ Counter : 0|32@1+ (1,0) [0|4294967295] "" TESTER'
SECOND_PROTECTED = """BO_ 64 Prot2: 8 BMS
 SG_ E2E_Crc : 0|8@1+ (1,0) [0|255] "" TESTER
 SG_ E2E_Counter : 8|4@1+ (1,0) [0|14] "" TESTER

CM_ BO_ 48"""


@dataclass(frozen=True)
class Broken:
    """A change of TEMPLATE, as (old, new) replacements, and the problems it must cause: every
    problem reported contains one of `problems`, and each of them is in some problem."""

    edits: tuple[tuple[str, str], ...]
    problems: tuple[str, ...]


BROKEN = {
    "overlapping signals": Broken(
        (("SG_ Other : 28|4@1-", "SG_ Other : 26|4@1-"),),
        ("message Prot: signals Value and Other overlap",)),
    "a signal outside the DLC": Broken(
        (("BO_ 48 Plain: 8 BMS", "BO_ 48 Plain: 3 BMS"),),
        ("message Plain, signal Counter: bits 0..31 are not all inside the 3 data bytes",)),
    "a signal that ends one bit outside the DLC": Broken(
        (("SG_ Other : 28|4@1-", "SG_ Other : 61|4@1-"),),
        ("message Prot, signal Other: bits 61..64 are not all inside the 8 data bytes",)),
    "a big-endian signal": Broken(
        (("SG_ Counter : 0|32@1+", "SG_ Counter : 7|32@0+"),),
        ("signal Counter: big-endian; only little-endian signals are supported",)),
    "a float signal": Broken(
        (("VAL_ 32", "SIG_VALTYPE_ 48 Counter : 1;\nVAL_ 32"),),
        ("signal Counter: a float signal; only integer signals are supported",)),
    "a signal of 33 bits": Broken(
        (("Counter : 0|32@1+ (1,0) [0|4294967295]", "Counter : 0|33@1+ (1,0) [0|1]"),),
        ("signal Counter: 33 bits; 1 to 32 are supported",)),
    "a signal of 0 bits": Broken(
        (("Counter : 0|32@1+ (1,0) [0|4294967295]", "Counter : 0|0@1+ (1,0) [0|1]"),),
        ("cantools cannot read it", "length 0")),
    "multiplexed signals": Broken(
        ((PLAIN_COUNTER, ' SG_ Mux M : 0|8@1+ (1,0) [0|255] "" TESTER\n'
                         ' SG_ Counter m0 : 8|16@1+ (1,0) [0|1] "" TESTER'),),
        ("signal Mux: multiplexed", "signal Counter: multiplexed")),
    "an extended identifier": Broken(
        (("BO_ 48 Plain", "BO_ 2147483696 Plain"), ("CM_ BO_ 48", "CM_ BO_ 2147483696")),
        ("message Plain: identifier 0x30 is not a base-format (11-bit) identifier",)),
    "a DLC above 8": Broken(
        (("BO_ 48 Plain: 8 BMS", "BO_ 48 Plain: 12 BMS"),),
        ("message Plain: DLC 12 is not 0..8 (classical CAN)",)),
    "two messages with one identifier": Broken(
        (("CM_ BO_ 48", 'BO_ 48 Again: 8 BMS\n SG_ X : 0|8@1+ (1,0) [0|255] "" TESTER\n\n'
                        'CM_ BO_ 48'),),
        ("message Again: identifier 0x030 is also used by Plain",)),
    "a protected message without E2E_Crc": Broken(
        ((' SG_ E2E_Crc : 0|8@1+ (1,0) [0|255] "" TESTER\n', ""),),
        ("message Prot: protected end to end but has no signal E2E_Crc",)),
    "E2E_Counter in the high nibble of byte 1": Broken(
        (("E2E_Counter : 8|4", "E2E_Counter : 12|4"),),
        ("message Prot: E2E_Counter must be unsigned and unscaled at bit 8, 4 bits",)),
    "E2E_Counter of 8 bits": Broken(
        (("E2E_Counter : 8|4", "E2E_Counter : 8|8"),),
        ("message Prot: E2E_Counter must be unsigned and unscaled at bit 8, 4 bits",)),
    "a signed E2E_Crc": Broken(
        (("E2E_Crc : 0|8@1+ (1,0) [0|255]", "E2E_Crc : 0|8@1- (1,0) [-128|127]"),),
        ("message Prot: E2E_Crc must be unsigned and unscaled at bit 0, 8 bits",)),
    "a scaled E2E_Crc": Broken(
        (("E2E_Crc : 0|8@1+ (1,0)", "E2E_Crc : 0|8@1+ (2,0)"),),
        ("message Prot: E2E_Crc must be unsigned and unscaled at bit 0, 8 bits",)),
    "two Data IDs whose CRCs collide": Broken(
        (('BA_ "FcnE2eDataId" BO_ 32 288;',
          'BA_ "FcnE2eDataId" BO_ 32 291;\nBA_ "FcnE2eDataId" BO_ 64 160;\n'
          'BA_ "GenMsgCycleTime" BO_ 64 10;'),
         ("CM_ BO_ 48", SECOND_PROTECTED)),
        ("messages Prot (Data ID 0x0123) and Prot2 (Data ID 0x00A0): the CRC of the Data ID "
         "bytes is 0xBC for both",)),
    "a protected message that is not cyclic": Broken(
        (('BA_ "GenMsgCycleTime" BO_ 32 10;\n', ""),),
        ("message Prot: protected end to end but not cyclic",)),
    "an E2E header in an unprotected message": Broken(
        ((PLAIN_COUNTER, ' SG_ E2E_Crc : 0|8@1+ (1,0) [0|255] "" TESTER\n'
                         ' SG_ Counter : 8|32@1+ (1,0) [0|4294967295] "" TESTER'),),
        ("message Plain: has the E2E header signal E2E_Crc but no FcnE2eDataId",)),
    "a Data ID above 16 bits": Broken(
        (("BO_ 32 288;", "BO_ 32 70000;"),),
        ("message Prot: FcnE2eDataId 70000 is not -1 or a 16-bit Data ID",)),
    "no definition of FcnE2eDataId": Broken(
        (('BA_DEF_ BO_ "FcnE2eDataId" INT -1 65535;\n', ""),
         ('BA_DEF_DEF_ "FcnE2eDataId" -1;\n', ""), ('BA_ "FcnE2eDataId" BO_ 32 288;\n', "")),
        ("no attribute definition FcnE2eDataId",
         "message Prot: has the E2E header signal E2E_Crc but no FcnE2eDataId",
         "message Prot: has the E2E header signal E2E_Counter but no FcnE2eDataId")),
    "a default Data ID": Broken(
        (('BA_DEF_DEF_ "FcnE2eDataId" -1;', 'BA_DEF_DEF_ "FcnE2eDataId" 5;'),),
        ("the default of FcnE2eDataId is 5, not -1",
         # ... which makes Plain a protected message that breaks every rule for one:
         "message Plain: protected end to end but not cyclic",
         "message Plain: protected end to end but has no signal E2E_Crc",
         "message Plain: protected end to end but has no signal E2E_Counter")),
    "a factor that is no fraction of int32 numbers": Broken(
        (("(0.001,1)", "(1e-12,1)"),),
        ("signal Value: factor 0.000000000001 is not a fraction of two int32 numbers",)),
    "an offset that is no fraction of int32 numbers": Broken(
        (("(0.001,1)", "(0.001,3000000000)"),),
        ("signal Value: offset 3000000000 is not a fraction of two int32 numbers",)),
    "a factor of 0": Broken(
        (("(0.001,1)", "(0,1)"),),
        ("signal Value: factor 0",)),
    "a value-table entry outside the raw range": Broken(
        (('VAL_ 32 Value 4095 "SNA" ;', 'VAL_ 32 Value 4096 "SNA" ;'),),
        ('signal Value: value table entry 4096 "SNA" is outside the raw range 0..4095',)),
    "a value-table entry below the raw range of a signed signal": Broken(
        (('VAL_ 32 Value 4095 "SNA" ;', 'VAL_ 32 Other -9 "LOW" ;'),),
        ('signal Other: value table entry -9 "LOW" is outside the raw range -8..7',)),
    "a value-table entry that is no C identifier": Broken(
        (('VAL_ 32 Value 4095 "SNA" ;', 'VAL_ 32 Value 4095 "NOT AVAILABLE" ;'),),
        ('signal Value: value table entry "NOT AVAILABLE" is not a C identifier',)),
    "a value-table entry that clashes with a generated name": Broken(
        (('VAL_ 32 Value 4095 "SNA" ;', 'VAL_ 32 Value 4095 "LENGTH" ;'),),
        ("the generated name CAN_DB_PROT_VALUE_LENGTH clashes",)),
    "two signals with one C name": Broken(
        ((PLAIN_COUNTER, ' SG_ CounterX : 0|8@1+ (1,0) [0|255] "" TESTER\n'
                         ' SG_ Counter_X : 8|8@1+ (1,0) [0|255] "" TESTER'),),
        ("signal Counter_X: gives the same C name, counter_x, as signal CounterX",)),
    "two messages with one C name": Broken(
        (("CM_ BO_ 48", 'BO_ 80 PLAIN: 8 BMS\n SG_ X : 0|8@1+ (1,0) [0|255] "" TESTER\n\n'
                        'CM_ BO_ 48'),),
        ("message PLAIN: gives the same C name, plain, as message Plain",)),
    "a name that gives no C identifier": Broken(
        (("BO_ 48 Plain: 8 BMS", "BO_ 48 _Plain: 8 BMS"),),
        ("message _Plain: the name does not give a C identifier (_plain)",)),
    "a name that gives a C keyword": Broken(
        (("SG_ Counter :", "SG_ Static :"),),
        ("signal Static: the name does not give a C identifier (static)",)),
    "functions that are the same in their first 31 characters": Broken(
        (("BO_ 48 Plain: 8 BMS", "BO_ 48 Lengthy_message_name_for_a: 8 BMS"),
         ("CM_ BO_ 48", 'BO_ 80 Lengthy_message_name_for_b: 8 BMS\n'
                        ' SG_ X : 0|8@1+ (1,0) [0|255] "" TESTER\n\nCM_ BO_ 48')),
        # MISRA C:2012 rule 5.1 counts 31 characters of an external identifier; the pack and
        # unpack functions of one long name already clash with each other.
        ("the generated name can_db_lengthy_message_name_for_a_unpack clashes",
         "the generated name can_db_lengthy_message_name_for_b_pack clashes",
         "the generated name can_db_lengthy_message_name_for_b_unpack clashes")),
    "a comment that would end the C comment": Broken(
        (('"A plain message."', '"A */ plain message."'),),
        ("message Plain: a comment contains /* or */",)),
    "a name too long for the line limit": Broken(
        # Short enough for distinct macro names; the entry of the test table is too long.
        (("SG_ Counter :", "SG_ CounterWithAVeryLongDescriptiveName :"),),
        ("columns long, more than 100",)),
    "a name too long for distinct macro names": Broken(
        (("SG_ Counter :", "SG_ CounterWithAnUnusuallyLongDescriptiveName :"),),
        ("CAN_DB_PLAIN_COUNTER_WITH_AN_UNUSUALLY_LONG_DESCRIPTIVE_NAME_RAW_MAX clashes",
         "CAN_DB_PLAIN_COUNTER_WITH_AN_UNUSUALLY_LONG_DESCRIPTIVE_NAME_FACTOR_DEN clashes",
         "CAN_DB_PLAIN_COUNTER_WITH_AN_UNUSUALLY_LONG_DESCRIPTIVE_NAME_OFFSET_DEN clashes")),
    "a file that is not ASCII": Broken(
        (('"A plain message."', '"A pl\u00e4in message."'),),
        ("not an ASCII file",)),
    "a file cantools cannot read": Broken(
        (("BO_ 48 Plain: 8 BMS", "BO_ 48 Plain 8 BMS"),),
        ("cantools cannot read it", "Invalid syntax")),
}


def apply_edits(edits: Sequence[tuple[str, str]]) -> str:
    text = TEMPLATE
    for old, new in edits:
        assert old in text, f"the template has no {old!r}"
        text = text.replace(old, new, 1)
    return text


def problems_of(text: str, tmp_path: Path) -> list[str]:
    """What the generator reports for a DBC with this text; [] if it generates code."""
    path = tmp_path / "case.dbc"
    path.write_bytes(text.encode("utf-8"))
    try:
        gen.outputs(gen.load(path), tmp_path / "out", tmp_path / "table.c")
    except gen.GeneratorError as error:
        return error.problems
    return []


def test_the_template_of_the_broken_dbcs_is_valid(tmp_path):
    assert problems_of(TEMPLATE, tmp_path) == []
    load_dbc_text = cantools.database.load_string(TEMPLATE, database_format="dbc", strict=True)
    assert len(load_dbc_text.messages) == 2


@pytest.mark.parametrize("case", BROKEN.values(), ids=BROKEN.keys())
def test_each_rule_of_the_generator_reports_a_dbc_that_breaks_it(case, tmp_path):
    problems = problems_of(apply_edits(case.edits), tmp_path)
    assert problems, "the generator accepted it"
    for expected in case.problems:
        assert any(expected in problem for problem in problems), (expected, problems)
    for problem in problems:
        assert any(expected in problem for expected in case.problems), (problem, case.problems)


# ------------------------------------------------------------------------------------------
# The C codec against cantools, through can_db_cli
# ------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class CliSignal:
    name: str
    start: int
    length: int
    is_signed: bool
    raw_min: int
    raw_max: int
    field_min: int
    field_max: int


@dataclass(frozen=True)
class CliMessage:
    frame_id: int
    name: str
    dlc: int
    cycle_ms: int
    data_id: int
    signals: tuple[CliSignal, ...]


LIST_LINE = re.compile(r"([0-9A-F]{3}) (\w+) (\d) (\d+) (-1|\d+) (\d+)((?: \S+)*)")


def parse_list(text: str) -> list[CliMessage]:
    """The output of can_db_cli --list; ValueError for anything not of its form."""
    messages = []
    for line in text.splitlines():
        match = LIST_LINE.fullmatch(line)
        if match is None:
            raise ValueError(f"not a line of {CLI} --list: {line!r}")
        signals = []
        for field in match[7].split():
            name, start, length, kind, *numbers = field.split(":")
            if kind not in "us" or len(numbers) != 4:
                raise ValueError(f"not a signal of {CLI} --list: {field!r}")
            signals.append(CliSignal(name, int(start), int(length), kind == "s",
                                     *(int(number) for number in numbers)))
        if len(signals) != int(match[6]):
            raise ValueError(f"signal count {match[6]} but {len(signals)} signals: {line!r}")
        messages.append(CliMessage(int(match[1], 16), match[2], int(match[3]), int(match[4]),
                                   int(match[5]), tuple(signals)))
    return messages


def require_binary(binary: Path) -> Path:
    if not binary.is_file():
        pytest.fail(f"{binary} not found: build it with scripts/build.sh release, with "
                    f"FCN_BUILD_ROOT set as for this test run (build root: {paths.build_root()})",
                    pytrace=False)
    return binary


def run_cli(binary: Path, lines: Sequence[str], *args: str) -> list[str]:
    """One run of the CLI; its answers, one per line of `lines`, in their order."""
    questions = "".join(f"{line}\n" for line in lines).encode("ascii")
    try:
        run = subprocess.run([str(binary), *args], input=questions, capture_output=True,
                             timeout=CLI_TIMEOUT_S, check=False)
    except subprocess.TimeoutExpired:
        pytest.fail(f"{binary} was not done with {len(lines)} lines after {CLI_TIMEOUT_S} s",
                    pytrace=False)
    if run.returncode != 0 or run.stderr:
        pytest.fail(f"{binary} ended with exit status {run.returncode}; on stderr: "
                    f"{run.stderr[:600]!r}", pytrace=False)
    answers = run.stdout.decode("ascii", errors="replace").split("\n")
    if answers.pop() != "" or (not args and len(answers) != len(lines)):
        pytest.fail(f"{binary} gave {len(answers)} complete answers to {len(lines)} lines",
                    pytrace=False)
    return answers


def wrap(value: int, signal: CliSignal) -> int:
    """What a signal of signal.length bits carries for `value`: its low bits, read back in the
    signal's own representation."""
    value &= (1 << signal.length) - 1
    if signal.is_signed and value >= 1 << (signal.length - 1):
        value -= 1 << signal.length
    return value


@dataclass(frozen=True)
class Case:
    """One line for can_db_cli and the answer cantools gives to it."""

    line: str
    expected: str


SEED = 0xDBC2026


def random_values(rng: random.Random, signals: Sequence[CliSignal], wide: bool) -> list[int]:
    """Raw values, a quarter of them at the ends of the range; with `wide`, from the range of
    the C type of the struct member instead of the signal's own range."""
    values = []
    for signal in signals:
        low, high = ((signal.field_min, signal.field_max) if wide
                     else (signal.raw_min, signal.raw_max))
        values.append(rng.choice((low, high)) if rng.random() < 0.25 else rng.randint(low, high))
    return values


def cases_for(db, messages: Sequence[CliMessage], rounds: int) -> dict[str, list[Case]]:
    """Pack, masked pack and unpack cases for every message with signals."""
    rng = random.Random(SEED)
    cases: dict[str, list[Case]] = {"pack": [], "masked pack": [], "unpack": [], "refused": []}
    for message in messages:
        if not message.signals:
            continue
        oracle = db.get_message_by_frame_id(message.frame_id)
        names = [signal.name for signal in message.signals]
        prefix = f"{message.frame_id:X}"

        def encode(values: Sequence[int]) -> str:
            data = oracle.encode(dict(zip(names, values)), scaling=False, strict=False)
            return data.hex().upper()

        extremes = [[s.raw_min for s in message.signals], [s.raw_max for s in message.signals],
                    [0] * len(names)]
        for values in extremes + [random_values(rng, message.signals, False)
                                  for _ in range(rounds)]:
            cases["pack"].append(Case(f"P {prefix} {' '.join(map(str, values))}", encode(values)))
        for _ in range(rounds):
            values = random_values(rng, message.signals, True)
            wrapped = [wrap(value, signal) for value, signal in zip(values, message.signals)]
            cases["masked pack"].append(Case(f"P {prefix} {' '.join(map(str, values))}",
                                             encode(wrapped)))
        for data in [bytes(message.dlc), b"\xff" * message.dlc] + [
                rng.randbytes(message.dlc) for _ in range(rounds)]:
            decoded = oracle.decode(data, scaling=False, decode_choices=False)
            cases["unpack"].append(Case(f"U {prefix} {data.hex().upper()}",
                                        " ".join(str(decoded[name]) for name in names)))
        # Lengths unpack() must refuse, and values outside the range of the struct member.
        for length in range(message.dlc):
            line = f"U {prefix} {rng.randbytes(length).hex().upper()}".rstrip()
            cases["refused"].append(Case(line, "invalid"))
        over = [s.field_max + 1 for s in message.signals]
        cases["refused"].append(Case(f"P {prefix} {' '.join(map(str, over))}", "invalid"))
        cases["refused"].append(Case(f"P {prefix} {' '.join(map(str, [0] * (len(names) + 1)))}",
                                     "invalid"))
    return cases


SHOWN = 10


def require_agreement(cases: Sequence[Case], answers: Sequence[str], what: str) -> None:
    if not cases:
        pytest.fail(f"'{what}' holds no case: nothing was compared", pytrace=False)
    failing = [(case, answer) for case, answer in zip(cases, answers, strict=True)
               if case.expected != answer]
    if failing:
        text = [f"the generated codec and cantools disagree on {len(failing)} of the "
                f"{len(cases)} lines of '{what}'; the first {min(SHOWN, len(failing))}:"]
        for case, answer in failing[:SHOWN]:
            text += ["", f"  {CLI} {case.line}", f"    C         {answer}",
                     f"    cantools  {case.expected}"]
        pytest.fail("\n".join(text), pytrace=False)


def compare(binary: Path, db, rounds: int) -> dict[str, tuple[list[Case], list[str]]]:
    messages = parse_list("\n".join(run_cli(binary, [], "--list")))
    groups = cases_for(db, messages, rounds)
    lines = [case.line for group in groups.values() for case in group]
    answers = iter(run_cli(binary, lines))
    return {name: (group, [next(answers) for _ in group]) for name, group in groups.items()}


def expected_table(db) -> list[CliMessage]:
    """The table can_db_cli --list must print, from cantools' reading of the DBC."""
    table = []
    for message in sorted(db.messages, key=lambda m: m.frame_id):
        attribute = message.dbc.attributes.get("FcnE2eDataId")
        signals = []
        for signal in by_start(message):
            bits = 8 if signal.length <= 8 else 16 if signal.length <= 16 else 32
            if signal.is_signed:
                raw = (-(1 << (signal.length - 1)), (1 << (signal.length - 1)) - 1)
                field = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1)
            else:
                raw, field = (0, (1 << signal.length) - 1), (0, (1 << bits) - 1)
            signals.append(CliSignal(signal.name, signal.start, signal.length, signal.is_signed,
                                     *raw, *field))
        table.append(CliMessage(message.frame_id, message.name, message.length,
                                message.cycle_time or 0,
                                -1 if attribute is None else attribute.value, tuple(signals)))
    return table


@pytest.fixture(scope="module")
def project_codec():
    binary = require_binary(paths.tool_binary(CLI))
    db = load_dbc()
    return binary, db, compare(binary, db, rounds=1500)


def test_the_table_of_the_cli_is_the_dbc_as_cantools_reads_it(project_codec):
    binary, db, _ = project_codec
    assert parse_list("\n".join(run_cli(binary, [], "--list"))) == expected_table(db)


@pytest.mark.parametrize("what", ["pack", "masked pack", "unpack", "refused"])
def test_the_generated_codec_agrees_with_cantools(project_codec, what):
    _, _, groups = project_codec
    require_agreement(*groups[what], what)


def test_every_message_with_signals_was_compared(project_codec):
    _, db, groups = project_codec
    compared = {int(case.line.split()[1], 16) for case in groups["pack"][0]}
    assert compared == {m.frame_id for m in db.messages if m.signals}
    assert len(groups["pack"][0]) == 10 * (1500 + 3)


# ------------------------------------------------------------------------------------------
# The generator on every length and position
# ------------------------------------------------------------------------------------------

LENGTHS = range(1, 33)


def layout(length: int, is_signed: bool, phase: int) -> list[tuple[str, int, int, bool]]:
    """The signals of one message of the test DBC: copies of a signal of `length` bits one after
    the other, behind a filler that moves the first copy to bit `phase`, and fillers of the
    other signedness behind them, so that all 64 bits are taken."""
    signals = [("F0", 0, phase, not is_signed)] if phase else []
    pos, copy = phase, 0
    while pos + length <= 64:
        signals.append((f"S{copy}", pos, length, is_signed))
        pos += length
        copy += 1
    filler = 1
    while pos < 64:
        size = min(32, 64 - pos)
        signals.append((f"F{filler}", pos, size, not is_signed))
        pos += size
        filler += 1
    return signals


def every_length_dbc() -> str:
    """Two messages for every length and signedness, whose first copies start at two different
    bits of byte 0."""
    lines = [TEMPLATE.split("BO_ 32")[0].rstrip(), ""]
    variants = [(n, s, v) for n in LENGTHS for s in (False, True) for v in ("A", "B")]
    for index, (length, is_signed, variant) in enumerate(variants):
        phase = (3 * length + (5 if is_signed else 0) + (3 if variant == "B" else 0)) % 8
        lines.append(f"BO_ {0x100 + index} T_W{length}{'S' if is_signed else 'U'}{variant}: "
                     f"8 BMS")
        for name, start, size, signed in layout(length, is_signed, phase):
            low = -(1 << (size - 1)) if signed else 0
            high = (1 << (size - 1)) - 1 if signed else (1 << size) - 1
            lines.append(f' SG_ {name} : {start}|{size}@1{"-" if signed else "+"} (1,0) '
                         f'[{low}|{high}] "" TESTER')
        lines.append("")
    lines += ['BA_DEF_ BO_ "FcnE2eDataId" INT -1 65535;', 'BA_DEF_DEF_ "FcnE2eDataId" -1;', ""]
    return "\n".join(lines)


def project_warning_flags() -> list[str]:
    """The warning set of every project target, from cmake/FcnHelpers.cmake."""
    text = (paths.REPO_ROOT / "cmake" / "FcnHelpers.cmake").read_text()
    block = re.search(r"set\(FCN_WARNING_FLAGS(.*?)\)", text, re.DOTALL)
    assert block is not None
    flags = block[1].split()
    assert "-Wconversion" in flags and "-Wsign-conversion" in flags
    return flags + ["-Werror"]


@pytest.fixture(scope="module")
def every_length_codec(tmp_path_factory):
    out = tmp_path_factory.mktemp("every_length")
    dbc = out / "every_length.dbc"
    dbc.write_text(every_length_dbc())
    assert gen.main([str(dbc), str(out), "--test-table", str(out / "can_db_table.c")]) == 0
    compiler = shutil.which("gcc") or shutil.which("cc")
    if compiler is None:
        pytest.fail("no C compiler (gcc or cc) on PATH", pytrace=False)
    binary = out / CLI
    command = [compiler, "-std=c11", "-O1", "-g", *project_warning_flags(),
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
               "-I", str(out), "-I", str(TOOLS_DIR), str(out / "can_db.c"),
               str(out / "can_db_table.c"), str(TOOLS_DIR / "can_db_cli.c"), "-o", str(binary)]
    build = subprocess.run(command, capture_output=True, text=True, check=False)
    if build.returncode != 0:
        pytest.fail(f"the generated code does not compile:\n{' '.join(command)}\n"
                    f"{build.stderr[:3000]}", pytrace=False)
    db = load_dbc(dbc)
    return binary, db, compare(binary, db, rounds=100)


def test_the_test_dbc_has_every_length_signed_and_unsigned_at_several_phases(every_length_codec):
    _, db, _ = every_length_codec
    phases: dict[tuple[int, bool], set[int]] = {}
    for message in db.messages:
        assert sum(signal.length for signal in message.signals) == 64, message.name
        for signal in message.signals:
            phases.setdefault((signal.length, signal.is_signed), set()).add(signal.start % 8)
    for length in LENGTHS:
        for is_signed in (False, True):
            assert (length, is_signed) in phases
    # Every length, signed and unsigned, sits at two phases at least, and every phase occurs
    # for signed and for unsigned signals.
    for key, seen in phases.items():
        assert len(seen) >= 2, (key, seen)
    for is_signed in (False, True):
        assert set().union(*(seen for (_, s), seen in phases.items() if s == is_signed)) == \
            set(range(8))


def test_the_generated_code_for_every_length_matches_its_table(every_length_codec):
    binary, db, _ = every_length_codec
    assert parse_list("\n".join(run_cli(binary, [], "--list"))) == expected_table(db)


@pytest.mark.parametrize("what", ["pack", "masked pack", "unpack", "refused"])
def test_the_generated_code_for_every_length_agrees_with_cantools(every_length_codec, what):
    _, _, groups = every_length_codec
    require_agreement(*groups[what], what)


# ------------------------------------------------------------------------------------------
# The harness itself: a comparison that cannot fail proves nothing
# ------------------------------------------------------------------------------------------


def stand_in(directory: Path, script: str) -> Path:
    program = directory / "stand_in_for_can_db_cli"
    program.write_text(f"#!/bin/sh\ncat >/dev/null\n{script}\n")
    program.chmod(0o755)
    return program


def test_a_missing_binary_fails_the_test_and_says_how_to_build_it(monkeypatch, tmp_path):
    monkeypatch.setenv("FCN_BUILD_ROOT", str(tmp_path))
    with pytest.raises(pytest.fail.Exception) as failure:
        require_binary(paths.tool_binary(CLI))
    assert "scripts/build.sh release" in str(failure.value)


@pytest.mark.parametrize("script, complaint", [
    ("echo 00", "1 complete answers to 2 lines"),
    ("echo 00; echo 00; echo 00", "3 complete answers to 2 lines"),
    ("echo 00; printf 00", "1 complete answers to 2 lines"),
    ("echo 00; echo 00; exit 1", "exit status 1"),
    ("echo 00; echo 00; echo oops >&2", "on stderr: b'oops"),
], ids=["too-few", "too-many", "cut-off", "exit-status", "stderr"])
def test_a_run_that_did_not_answer_every_line_properly_fails(tmp_path, script, complaint):
    with pytest.raises(pytest.fail.Exception) as failure:
        run_cli(stand_in(tmp_path, script), ["P 20 0", "U 20 00"])
    assert complaint in str(failure.value)


def test_a_disagreement_is_reported_with_the_line_and_both_answers():
    cases = [Case("P 30 1 2", "0100000002000000"), Case("U 30 0000000000000000", "0 0")]
    require_agreement(cases, ["0100000002000000", "0 0"], "two")
    with pytest.raises(pytest.fail.Exception) as failure:
        require_agreement(cases, ["0100000002000000", "0 1"], "two")
    message = str(failure.value)
    assert "1 of the 2 lines of 'two'" in message
    assert f"{CLI} U 30 0000000000000000" in message
    assert "C         0 1" in message and "cantools  0 0" in message
    with pytest.raises(pytest.fail.Exception, match="nothing was compared"):
        require_agreement([], [], "none")


@pytest.mark.parametrize("line", [
    "020 BMS_PackStatus 8 10 288 1 E2E_Crc:0:8:x:0:255:0:255",
    "020 BMS_PackStatus 8 10 288 2 E2E_Crc:0:8:u:0:255:0:255",
    "20 BMS_PackStatus 8 10 288 0",
    "020 BMS_PackStatus 8 10 288 1 E2E_Crc:0:8:u:0:255:0",
])
def test_a_table_line_not_of_the_form_is_refused(line):
    with pytest.raises(ValueError):
        parse_list(line)


def test_masking_is_worked_out_as_the_low_bits_read_back():
    unsigned12 = CliSignal("U", 0, 12, False, 0, 4095, 0, 65535)
    signed12 = CliSignal("S", 0, 12, True, -2048, 2047, -32768, 32767)
    signed1 = CliSignal("B", 0, 1, True, -1, 0, -128, 127)
    assert wrap(0xF123, unsigned12) == 0x123
    assert wrap(4096, unsigned12) == 0
    assert wrap(2048, signed12) == -2048
    assert wrap(-2049, signed12) == 2047
    assert wrap(-32768, signed12) == 0
    assert wrap(127, signed1) == -1 and wrap(-128, signed1) == 0


# ------------------------------------------------------------------------------------------
# docs/can-protocol.md
# ------------------------------------------------------------------------------------------


def table_rows(text: str) -> list[list[str]]:
    """The body rows of the markdown tables in `text`, as lists of stripped cells."""
    rows = []
    for line in text.splitlines():
        if line.startswith("|") and not re.fullmatch(r"\|[-| :]+\|", line):
            rows.append([cell.strip() for cell in line.strip("|").split("|")])
    return rows


def test_the_message_table_of_the_document_is_the_dbc():
    text = DOC.read_text()
    section = text.split("## Messages", 1)[1].split("\n## ", 1)[0]
    rows = [row for row in table_rows(section) if row[0].startswith("0x")]
    expected = [[f"0x{frame_id:03X}", name, sender, str(dlc),
                 f"{cycle} ms" if cycle else "event",
                 f"0x{data_id:04X}" if data_id is not None else "none"]
                for frame_id, name, sender, dlc, cycle, data_id in MESSAGE_SET]
    assert [row[:6] for row in rows] == expected


def test_every_signal_of_the_dbc_is_in_the_document_as_it_is_in_the_dbc():
    text = DOC.read_text()
    model = gen.load(DBC)
    for message in model.messages:
        if not message.signals:
            continue
        heading = f"### 0x{message.frame_id:03X} {message.name}"
        assert heading in text, heading
        section = text.split(heading, 1)[1].split("\n#", 1)[0]
        rows = [row for row in table_rows(section) if row[0] != "Signal"]
        expected = []
        for signal in message.signals:
            choices = ", ".join(f"{value} {name}" for value, name in signal.choices)
            expected.append([signal.name, str(signal.start), str(signal.length),
                             "signed" if signal.is_signed else "unsigned",
                             gen.decimal(signal.factor), gen.decimal(signal.offset),
                             signal.unit, f"{signal.minimum}..{signal.maximum}", choices])
        assert [row[:9] for row in rows] == expected, message.name
