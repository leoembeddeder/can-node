# SPDX-License-Identifier: MIT
"""The rules the test harness itself enforces, checked by running pytest on scratch tests:

  - the timing report of tests/conftest.py ("miss" only for a missed timing tolerance in the
    test itself, "fail" for everything else) and the repetition rule built on it
    (fcn_tools.timing.run_with_one_retry, the command scripts/test-system.sh runs): a
    failure that is not a timing miss is never repeated;
  - the supervision of nodes by the start_node fixture and the session: a node death the test
    did not look at fails the test, a reset the test did not wait for with wait_for_reset()
    fails it, a node the test left running is stopped in order at its end and fails it if it
    does not stop in order, a run that ended in order with a kick of the hardware watchdog
    missing from its statistics fails it, and a node left running at the end of the session
    fails the session.

Each scratch run gets a copy of the real tests/conftest.py, so the hooks under test are the
ones the system tests use, and stand-in nodes (shell scripts) instead of the node binary.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

import fcn_tools
from fcn_tools.timing import TIMING_MISSED, only_timing_missed, run_with_one_retry

CONFTEST = Path(__file__).resolve().parents[1] / "conftest.py"
TOOLS = Path(fcn_tools.__file__).resolve().parents[1]

SCRATCH_TESTS = '''
import os
from pathlib import Path

import pytest

from fcn_tools.timing import check_timing

RUNS = Path(os.environ["SCRATCH_RUNS"])


def runs_of(name):
    """Count this run of the test `name` and return how many there have been."""
    with RUNS.open("a") as out:
        out.write(name + "\\n")
    return RUNS.read_text().splitlines().count(name)


@pytest.fixture
def broken_setup():
    raise RuntimeError("fixture setup broke")


@pytest.fixture
def broken_teardown():
    yield
    raise RuntimeError("fixture teardown broke")


@pytest.mark.timing
def test_passes():
    runs_of("passes")


@pytest.mark.timing
def test_misses_once():
    check_timing(runs_of("misses_once") > 1, "pretend the host was slow the first time")


@pytest.mark.timing
def test_misses_always():
    runs_of("misses_always")
    check_timing(False, "pretend the host is always slow")


@pytest.mark.timing
def test_wrong_value():
    runs_of("wrong_value")
    assert 1 + 1 == 3


@pytest.mark.timing
def test_crash():
    runs_of("crash")
    raise RuntimeError("a real failure")


@pytest.mark.timing
def test_broken_setup(broken_setup):
    runs_of("broken_setup")


@pytest.mark.timing
def test_broken_teardown(broken_teardown):
    runs_of("broken_teardown")


@pytest.mark.timing
def test_misses_and_breaks_teardown(broken_teardown):
    runs_of("misses_and_breaks_teardown")
    check_timing(False, "pretend the host was slow")


@pytest.mark.timing
def test_misses_then_wrong_value():
    if runs_of("misses_then_wrong_value") == 1:
        check_timing(False, "pretend the host was slow the first time")
    assert 1 + 1 == 3
'''


class Scratch:
    """A directory with the scratch tests and a copy of tests/conftest.py."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.tests = root / "suite"
        self.tests.mkdir()
        (self.tests / "conftest.py").write_text(CONFTEST.read_text())
        self.runs = root / "runs.txt"
        self.runs.touch()
        self.report = root / "timing-report.txt"
        # The fcn_tools under test, the one this test imported.
        path = [str(TOOLS)] + [p for p in os.environ.get("PYTHONPATH", "").split(os.pathsep) if p]
        self.env = dict(os.environ, SCRATCH_RUNS=str(self.runs), PYTHONPATH=os.pathsep.join(path))
        self.env.pop("FCN_NODE", None)

    def write(self, name: str, text: str) -> None:
        (self.tests / name).write_text(text)

    def pytest_command(self, *args: str) -> list[str]:
        return [sys.executable, "-m", "pytest", str(self.tests), "-q", "-ra",
                "-o", f"cache_dir={self.root / 'cache'}",
                "-W", "ignore::pytest.PytestUnknownMarkWarning",
                "--artifacts", str(self.root / "artifacts-{tag}"), *args]

    def pytest(self, *args: str, env: dict | None = None) -> subprocess.CompletedProcess:
        command = [part.replace("{tag}", "run") for part in self.pytest_command(*args)]
        return subprocess.run(command, cwd=self.root, env=env or self.env, capture_output=True,
                              text=True, timeout=120)

    def gate(self, *args: str) -> subprocess.CompletedProcess:
        """The timing tests the way scripts/test-system.sh runs them."""
        return subprocess.run(
            [sys.executable, "-m", "fcn_tools.timing", "--report", str(self.report), "--",
             *self.pytest_command(*args)],
            cwd=self.root, env=self.env, capture_output=True, text=True, timeout=120)

    def runs_of(self, name: str) -> int:
        return self.runs.read_text().splitlines().count(name)


@pytest.fixture
def scratch(tmp_path) -> Scratch:
    suite = Scratch(tmp_path)
    suite.write("test_scratch.py", SCRATCH_TESTS)
    return suite


# ------------------------------------------------------------------------- timing report


def test_only_a_timing_tolerance_missed_in_the_test_itself_is_a_miss(scratch):
    out = scratch.pytest("--timing-report", str(scratch.report))
    assert out.returncode == 1, out.stdout
    lines = sorted(scratch.report.read_text().splitlines())
    prefix = "suite/test_scratch.py::test_"
    assert lines == sorted([
        f"miss {prefix}misses_once",
        f"miss {prefix}misses_always",
        f"fail {prefix}wrong_value",
        f"fail {prefix}crash",
        f"fail {prefix}broken_setup",
        f"fail {prefix}broken_teardown",
        # A timing miss in the test and an error after it: both are reported.
        f"miss {prefix}misses_and_breaks_teardown",
        f"fail {prefix}misses_and_breaks_teardown",
        f"miss {prefix}misses_then_wrong_value",
    ]), out.stdout


def test_without_the_option_no_report_is_written(scratch):
    out = scratch.pytest("-k", "misses_always")
    assert out.returncode == 1
    assert not scratch.report.exists()


# --------------------------------------------------------------------- repetition rule


def test_a_run_that_only_missed_timing_is_repeated_and_can_pass(scratch):
    out = scratch.gate("-k", "passes or misses_once")
    assert out.returncode == 0, out.stdout + out.stderr
    assert scratch.runs_of("misses_once") == 2
    assert scratch.runs_of("passes") == 1          # only what failed is repeated
    assert "repeating those tests once" in out.stdout


def test_a_run_that_misses_timing_twice_ends_with_the_timing_status(scratch):
    out = scratch.gate("-k", "misses_always")
    assert out.returncode == TIMING_MISSED, out.stdout + out.stderr
    assert scratch.runs_of("misses_always") == 2
    assert "missed again" in out.stdout


@pytest.mark.parametrize("other, body_runs", [
    ("wrong_value", 1), ("crash", 1), ("broken_setup", 0), ("broken_teardown", 1)])
def test_any_other_failure_is_never_repeated(scratch, other, body_runs):
    out = scratch.gate("-k", f"misses_once or {other}")
    assert out.returncode == 1, out.stdout + out.stderr
    assert scratch.runs_of("misses_once") == 1
    assert scratch.runs_of(other) == body_runs     # a broken setup never reaches the test


def test_a_miss_with_an_error_after_it_is_not_repeated(scratch):
    out = scratch.gate("-k", "misses_and_breaks_teardown")
    assert out.returncode == 1
    assert scratch.runs_of("misses_and_breaks_teardown") == 1


def test_a_repetition_that_fails_on_a_value_is_a_plain_failure(scratch):
    out = scratch.gate("-k", "misses_then_wrong_value")
    assert out.returncode == 1, out.stdout + out.stderr
    assert scratch.runs_of("misses_then_wrong_value") == 2


def test_a_run_that_passes_or_selects_nothing_is_not_repeated(scratch):
    assert scratch.gate("-k", "passes").returncode == 0
    assert scratch.runs_of("passes") == 1
    assert scratch.gate("-k", "no_such_test").returncode == 0


def test_a_run_that_fails_without_a_failed_test_is_not_repeated(scratch):
    scratch.write("test_broken_module.py", "import no_such_module\n")
    out = scratch.gate("-k", "misses_once")
    assert out.returncode == 1, out.stdout + out.stderr
    assert scratch.runs_of("misses_once") == 0      # a collection error stops the run first


def test_the_retry_decision_reads_the_report(tmp_path):
    report = tmp_path / "report.txt"
    assert not only_timing_missed(report)                  # no report: nothing says "host"
    report.write_text("")
    assert not only_timing_missed(report)
    report.write_text("miss a.py::t1\nmiss a.py::t2\n")
    assert only_timing_missed(report)
    report.write_text("miss a.py::t1\nfail a.py::t2\n")
    assert not only_timing_missed(report)


def test_the_repetition_rule_with_a_stand_in_for_pytest(tmp_path):
    report = tmp_path / "report.txt"
    calls = []

    def run(script):
        def _run(tag, extra):
            calls.append((tag, list(extra)))
            status, lines = script[len(calls) - 1]
            report.write_text("".join(f"{line}\n" for line in lines))
            return status
        return _run

    calls.clear()
    assert run_with_one_retry(run([(1, ["miss t"]), (0, [])]), report, say=lambda _: None) == 0
    assert calls == [("timing", ["--timing-report", str(report)]),
                     ("timing-retry", ["--last-failed", "--timing-report", str(report)])]
    calls.clear()
    # pytest exit status 2 (interrupted) is no verdict on the tests, whatever the report says.
    assert run_with_one_retry(run([(2, ["miss t"])]), report, say=lambda _: None) == 1
    assert len(calls) == 1
    calls.clear()
    assert run_with_one_retry(run([(1, ["miss t"]), (1, ["miss t"])]), report,
                              say=lambda _: None) == TIMING_MISSED
    calls.clear()
    assert run_with_one_retry(run([(1, ["miss t"]), (1, ["fail t"])]), report,
                              say=lambda _: None) == 1


# --------------------------------------------------------------------- node supervision


SUPERVISED_TESTS = '''
import time

from fcn_tools.node import NodeProcess


def wait_until_ready(node):
    deadline = time.monotonic() + 10
    while "ready" not in node.stdout_path.read_text():
        assert time.monotonic() < deadline, "the stand-in did not get ready"
        time.sleep(0.01)


def test_does_not_look(start_node):
    node = start_node()
    while node.running:
        time.sleep(0.01)                   # the node is dead; the test does not look


def test_leaves_it_running(start_node):
    wait_until_ready(start_node())         # the end of the test stops it


def test_stops_it(start_node):
    node = start_node()
    wait_until_ready(node)
    assert node.stop(timeout=10) == 0


def test_looks(start_node):
    node = start_node()
    assert node.wait_for_reset(timeout=10) == 20     # expected: the test's business


def test_looks_but_expects_no_reset(start_node):
    node = start_node()
    assert node.wait(timeout=10) == 20     # seen, but a reset the test did not wait for


def test_leaves_a_node_running(workdir, node_binary):
    NodeProcess(workdir=workdir, binary=node_binary, name="orphan").start()
'''


def stand_in_node(root: Path, body: str) -> Path:
    path = root / "stand_in_node.sh"
    path.write_text("#!/bin/sh\n" + textwrap.dedent(body))
    path.chmod(0o755)
    return path


def test_a_node_death_the_test_did_not_look_at_fails_the_test(scratch):
    scratch.write("test_supervised.py", SUPERVISED_TESTS)
    node = stand_in_node(scratch.root, 'echo "FATAL assertion failed" >&2\nexit 20\n')
    out = scratch.pytest("-k", "does_not_look or (test_looks and not expects_no_reset)",
                         env=dict(scratch.env, FCN_NODE=str(node)))
    assert out.returncode == 1, out.stdout
    # Both tests pass their own assertions; the one that did not look gets an error.
    assert re.search(r"\b2 passed, \d+ deselected, 1 error\b", out.stdout), out.stdout
    assert re.search(r"^ERROR \S*test_supervised\.py::test_does_not_look", out.stdout,
                     re.MULTILINE), out.stdout
    assert "ended by itself with status 20 while the test was not looking" in out.stdout


def test_a_reset_the_test_did_not_wait_for_fails_the_test(scratch):
    """A system test that does not wait for a reset with wait_for_reset() expects none: a node
    that was reset fails it, even when the test saw the status."""
    scratch.write("test_supervised.py", SUPERVISED_TESTS)
    node = stand_in_node(scratch.root, 'echo "FATAL watchdog: not kicked" >&2\nexit 20\n')
    out = scratch.pytest("-k", "test_looks", env=dict(scratch.env, FCN_NODE=str(node)))
    assert out.returncode == 1, out.stdout
    assert re.search(r"\b2 passed, \d+ deselected, 1 error\b", out.stdout), out.stdout
    assert re.search(r"^ERROR \S*test_supervised\.py::test_looks_but_expects_no_reset",
                     out.stdout, re.MULTILINE), out.stdout
    assert "was reset (status 20) in a test that does not expect a reset" in out.stdout


def kick_stats(hw_kicks: int) -> str:
    """The statistics lost_kicks() reads, of a run whose 20 supervision cycles each ended with
    a kick, of which the hardware watchdog counted `hw_kicks`."""
    return json.dumps({"hw_watchdog": {"started": True, "kicks": hw_kicks},
                       "watchdog_manager": {"kicks": 20, "kicks_withheld": 0, "cycles": 20}})


def stopping_stand_in(root: Path, stats: str) -> Path:
    """A stand-in node that runs until SIGTERM, then writes `stats` as its statistics and ends
    in order (status 0), as the node does."""
    source = root / "stand_in_stats.json"
    source.write_text(stats)
    return stand_in_node(root, f"""\
        while [ $# -gt 0 ]; do
          if [ "$1" = --stats-json ]; then stats="$2"; fi
          shift
        done
        trap 'cp "{source}" "$stats"; exit 0' TERM
        echo ready
        while :; do sleep 0.02; done
        """)


def test_every_run_that_ended_in_order_has_its_kicks_checked(scratch):
    """A node the test left running is stopped in order (SIGTERM) when the test ends, so that
    it writes its statistics; those of every run that ended in order, also of one the test
    stopped itself, fail the test if a kick of the hardware watchdog went missing."""
    scratch.write("test_supervised.py", SUPERVISED_TESTS)
    lost = stopping_stand_in(scratch.root, kick_stats(hw_kicks=0))
    out = scratch.pytest("-k", "leaves_it_running or stops_it",
                         env=dict(scratch.env, FCN_NODE=str(lost)))
    assert out.returncode == 1, out.stdout
    assert re.search(r"\b2 passed, \d+ deselected, 2 errors\b", out.stdout), out.stdout
    for test in ("leaves_it_running", "stops_it"):
        assert re.search(rf"^ERROR \S*test_supervised\.py::test_{test}\b", out.stdout,
                         re.MULTILINE), out.stdout
    assert ("node lost kicks of the hardware watchdog: started True, 0 kicks counted by the "
            "watchdog, 20 decided on and 0 withheld by the health task in 20 supervision "
            "cycles") in out.stdout, out.stdout

    kept = stopping_stand_in(scratch.root, kick_stats(hw_kicks=20))
    out = scratch.pytest("-k", "leaves_it_running or stops_it",
                         env=dict(scratch.env, FCN_NODE=str(kept)))
    assert out.returncode == 0, out.stdout
    assert re.search(r"\b2 passed, \d+ deselected in\b", out.stdout), out.stdout


@pytest.mark.parametrize("body, shown", [
    # Ignores SIGTERM: killed when the wait runs out.
    ("trap '' TERM\necho ready\nexec sleep 30\n",
     "node had not ended 0.5 s after the end of the test asked it to stop (SIGTERM), and was "
     "killed (SIGKILL): it wrote no statistics, so its kicks were not checked"),
    # Reports a stack overflow on the way down.
    ("trap 'echo \"FATAL stack overflow\" >&2; exit 21' TERM\necho ready\n"
     "while :; do sleep 0.02; done\n",
     "node ended with status 21, not in order, when the end of the test asked it to stop "
     "(SIGTERM)\n--- stderr (last lines)\nFATAL stack overflow"),
], ids=["ignores_sigterm", "status_21"])
def test_a_node_that_does_not_stop_in_order_at_the_end_of_the_test_fails_it(scratch, body,
                                                                             shown):
    scratch.write("test_supervised.py", SUPERVISED_TESTS)
    # The wait of the copy shortened from 5 s, so that this test does not take 5 s.
    conftest = scratch.tests / "conftest.py"
    text = conftest.read_text()
    assert "\nSTOP_TIMEOUT_S = 5.0\n" in text
    conftest.write_text(text.replace("\nSTOP_TIMEOUT_S = 5.0\n", "\nSTOP_TIMEOUT_S = 0.5\n"))
    node = stand_in_node(scratch.root, body)
    out = scratch.pytest("-k", "leaves_it_running", env=dict(scratch.env, FCN_NODE=str(node)))
    assert out.returncode == 1, out.stdout
    assert re.search(r"\b1 passed, \d+ deselected, 1 error\b", out.stdout), out.stdout
    assert shown in out.stdout, out.stdout


def test_a_node_left_running_at_the_end_fails_the_session(scratch):
    scratch.write("test_supervised.py", SUPERVISED_TESTS)
    node = stand_in_node(scratch.root, "exec sleep 30\n")
    out = scratch.pytest("-k", "leaves_a_node_running",
                         env=dict(scratch.env, FCN_NODE=str(node)))
    assert out.returncode == 1, out.stdout
    assert "were still running at session end" in out.stdout, out.stdout
