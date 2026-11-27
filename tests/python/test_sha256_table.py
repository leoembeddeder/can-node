# SPDX-License-Identifier: MIT
"""The table of SHA-256 digests in tests/unit/test_sha256.c, made and checked with hashlib.

tests/unit/test_sha256.c compares lib/sha256 with a digest for every message length from 0 to
320 bytes. The digests come from Python's hashlib, not from the code under test: running this
file as a program writes them into the C file, between the BEGIN and END marker lines,

    python3 tests/python/test_sha256_table.py

and the test below fails if the table in the C file is not exactly what hashlib computes, so
the table cannot drift from its source. The message of length n is message(n), which the C
test builds with the same formula (message_byte() there).
"""
from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

C_TEST = Path(__file__).resolve().parents[1] / "unit" / "test_sha256.c"
BEGIN = "/* BEGIN DIGEST TABLE (tests/python/test_sha256_table.py) */"
END = "/* END DIGEST TABLE */"
MAX_LENGTH = 320


def message_byte(i: int) -> int:
    """Byte i of every table message: the top byte of i * 2654435761 modulo 2^32 (Knuth's
    multiplicative hash), so that the bytes of a block differ from each other and from those of
    the blocks around it."""
    return ((i * 2654435761) & 0xFFFFFFFF) >> 24


def message(length: int) -> bytes:
    return bytes(message_byte(i) for i in range(length))


def table_lines() -> list[str]:
    return [f'    "{hashlib.sha256(message(n)).hexdigest()}", /* {n} */'
            for n in range(MAX_LENGTH + 1)]


def split_c_file(text: str) -> tuple[str, list[str], str]:
    """The C file before the table (up to the end of the BEGIN marker), the table's lines,
    and the rest from the start of the END marker's line."""
    start = text.index(BEGIN) + len(BEGIN)
    stop = text.rindex("\n", 0, text.index(END)) + 1
    head, body, tail = text[:start], text[start:stop], text[stop:]
    lines = [line for line in body.split("\n") if line.strip()]
    return head, lines, tail


def test_the_c_table_is_what_hashlib_computes():
    _, lines, _ = split_c_file(C_TEST.read_text(encoding="utf-8"))
    assert lines == table_lines()


def test_the_c_test_builds_the_messages_with_the_same_formula():
    text = C_TEST.read_text(encoding="utf-8")
    assert re.search(r"\(uint8_t\)\(\(\(uint32_t\)i \* 2654435761U\) >> 24U\)", text)
    assert f"#define TABLE_LENGTHS {MAX_LENGTH + 1}U" in text


def test_the_first_digests_are_known_values():
    """hashlib itself, against values published elsewhere: the empty message (the SHA-256 of
    nothing, quoted everywhere) and "abc" (FIPS 180-4's first example)."""
    assert hashlib.sha256(b"").hexdigest() == (
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
    assert hashlib.sha256(b"abc").hexdigest() == (
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")


def main() -> int:
    head, _, tail = split_c_file(C_TEST.read_text(encoding="utf-8"))
    C_TEST.write_text(head + "\n" + "\n".join(table_lines()) + "\n" + tail, encoding="utf-8",
                      newline="\n")
    print(f"wrote {MAX_LENGTH + 1} digests into {C_TEST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
