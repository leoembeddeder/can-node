# SPDX-License-Identifier: MIT
"""The reference model of the update: what the bootloader's select must decide for a flash, and
what every flash must satisfy, written from the state machine in docs/update.md.

It is the oracle of the update tests (tests/system/update_bench.py) and of the benches (bench/),
and stays independent of the code under test: it reads only the description of a flash that
`fcn_flash info` and fcn_tools.flash agree on (fcn_tools.bench.info()), and never lib/boot,
lib/image, fcn_boot or the node. It moved here from tests/system/update_bench.py in phase 6
unchanged, so that a bench outside pytest judges a flash by the same rules as the tests.

expected_boot() is what the bootloader's select must decide for the metadata a test has just
read, and check_invariants() what every flash must satisfy whatever happened to it, a power cut
included (docs/update.md, "Decision 5's invariants" in the power-cut campaign). Both check with
assert statements, as the tests do: they fail with AssertionError.
"""
from __future__ import annotations

import json
from dataclasses import dataclass

if not __debug__:  # pragma: no cover - only under python -O
    raise ImportError("fcn_tools.update_model checks with assert statements, which python -O "
                      "removes: run it without -O")

#: BOOT_MAX_ATTEMPTS (lib/boot/boot.h): the starts a trial gets before it is rolled back.
MAX_ATTEMPTS = 3


def states(flash_info: dict) -> tuple[str, ...]:
    """The state of slot A and of slot B."""
    return tuple(slot["state"] for slot in flash_info["slots"])


def slot_index(name: str) -> int:
    return "AB".index(name)


@dataclass(frozen=True)
class Boot:
    """What the bootloader's select starts: a slot, as a trial (attempt 1 to 3) or as the
    confirmed image (attempt 0), and the state change it writes on the way."""
    slot: str
    mode: str          # "trial" or "confirmed", as in the node's statistics
    attempt: int
    event: str         # "NONE", "ROLLBACK", ... as in the node's statistics and BMS_BootInfo


def expected_boot(flash_info: dict) -> Boot:
    """What select must do with the flash `flash_info` describes, from the state machine of
    docs/update.md, for flashes whose images all validate as recorded (a trial with attempts
    left is started as its next attempt; one that has used them is rolled back and the
    confirmed image started; otherwise the confirmed image) or have been damaged by the bench
    (a trial that does not validate is rejected; a confirmed image that does not validate falls
    back to the previous one). Fails the test for a factory flash: every test adopts first."""
    md = flash_info["metadata"]
    assert md["have_record"], "a flash in the factory state"
    slots = {slot["name"]: slot for slot in flash_info["slots"]}
    active, trial = md["active_slot"], md["trial_slot"]
    if trial != "none":
        if slots[trial]["validation"] != "ok" or not slots[trial]["id_matches_record"]:
            return Boot(active, "confirmed", 0, "REJECT")
        if md["attempts_used"] < MAX_ATTEMPTS:
            return Boot(trial, "trial", md["attempts_used"] + 1, "NONE")
        return Boot(active, "confirmed", 0, "ROLLBACK")
    if slots[active]["validation"] == "ok" and slots[active]["id_matches_record"]:
        return Boot(active, "confirmed", 0, "NONE")
    other = "B" if active == "A" else "A"
    assert slots[other]["state"] == "previous" and slots[other]["id_matches_record"], (
        f"nothing bootable: {json.dumps(flash_info['slots'], indent=1)}")
    return Boot(other, "confirmed", 0, "FALLBACK")


def check_invariants(flash_info: dict) -> None:
    """What every flash of these tests satisfies that the bench has not damaged, whatever else
    happened to it, power cuts included (docs/update.md, boot control): the metadata have a
    newest record (the factory adoption is never undone); exactly one slot is CONFIRMED, and it
    holds the image the record names, intact; at most one slot is a TRIAL, with 0 to 3 attempts
    used; and every slot whose state names an image (TRIAL, CONFIRMED, PREVIOUS) holds that
    image, intact."""
    md = flash_info["metadata"]
    assert md["have_record"], "the metadata lost every record"
    by_state = {}
    for slot in flash_info["slots"]:
        by_state.setdefault(slot["state"], []).append(slot["name"])
    assert len(by_state.get("confirmed", [])) == 1, by_state
    assert len(by_state.get("trial", [])) <= 1, by_state
    active = flash_info["slots"][slot_index(md["active_slot"])]
    assert active["state"] == "confirmed"
    assert active["validation"] == "ok" and active["id_matches_record"], active
    assert 0 <= md["attempts_used"] <= MAX_ATTEMPTS
    for slot in flash_info["slots"]:
        if slot["state"] in ("trial", "confirmed", "previous"):
            assert slot["validation"] == "ok" and slot["id_matches_record"], slot
