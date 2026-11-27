# SPDX-License-Identifier: MIT
"""The bench of the update tests (test_boot.py, test_update.py, test_update_cuts.py and phase 5's
download tests): what a test does to a flash file and how it reads one, kept in one place.

It lives in fcn_tools since phase 6, so that the benches of bench/ use the same helpers outside
pytest: fcn_tools.bench (the flash tool, the images, the frames, the waits that follow a trial,
TelemetryOff and Flood) and fcn_tools.update_model (the reference: expected_boot() and
check_invariants(), written from the state machine in docs/update.md). Their module texts say
what each helper does. This module keeps the names the tests import. Where a helper of
fcn_tools.bench gives up (a wait that ran out, the flash tool missing) it raises
fcn_tools.bench.BenchFailure; the helpers here turn that into pytest.fail(message,
pytrace=False), the failure they gave before they moved, and are otherwise those functions
themselves.

The images (the `images` fixture, tests/system/conftest.py) are packed from the release build of
the node, stripped, and from the test payloads of tests/tools: an image of the asan node does
not fit a slot.
"""
from __future__ import annotations

import functools
from typing import Callable, TypeVar

import pytest

from fcn_tools import bench as _bench
from fcn_tools.bench import (  # noqa: F401 - the names the tests import
    ALIVE_LINE, BENCH_SEED, CONFIRM_WAIT_S, FLOOD_BITRATE, FLOOD_FRAME, FLOOD_LOAD,
    FLOOD_MOST_PASSES, FLOOD_PASS_S, NO_SELF_CONFIRM, ROLLBACK_BIT, RUN_WAIT_S,
    TRIAL_WINDOW_PERIODS, Flood, TelemetryOff, alive_seconds, check_frames_of, check_handoff,
    decoded, flip_bit)
from fcn_tools.update_model import (  # noqa: F401
    MAX_ATTEMPTS, Boot, check_invariants, expected_boot, slot_index, states)

_F = TypeVar("_F", bound=Callable)


def _failing_the_test(function: _F) -> _F:
    """`function`, with a BenchFailure it raises turned into the test's failure."""
    @functools.wraps(function)
    def wrapped(*args, **kwargs):
        try:
            return function(*args, **kwargs)
        except _bench.BenchFailure as failure:
            pytest.fail(str(failure), pytrace=False)
    return wrapped  # type: ignore[return-value]


flash_tool = _failing_the_test(_bench.flash_tool)
factory = _failing_the_test(_bench.factory)
stage = _failing_the_test(_bench.stage)
info = _failing_the_test(_bench.info)
wait_until_confirmed = _failing_the_test(_bench.wait_until_confirmed)
listen = _failing_the_test(_bench.listen)
until_confirmed = _failing_the_test(_bench.until_confirmed)
wait_for_alive = _failing_the_test(_bench.wait_for_alive)
wait_for_confirm_seconds = _failing_the_test(_bench.wait_for_confirm_seconds)
wait_for_can_state = _failing_the_test(_bench.wait_for_can_state)
