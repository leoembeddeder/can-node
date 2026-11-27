"""Assertions about host timing, kept apart from all other assertions, and the rule for
repeating the tests that make them.

A test that checks how well the host kept time (a tick period, a frame period on the wall
clock) can fail because the machine was busy, without anything being wrong with the code.
Such a test is allowed one repetition, but only when every failure of the run was of that
kind: a crash, a wrong exit status or a wrong value fails at once. To make the distinction,
tolerance checks raise TimingToleranceMissed and nothing else does; tests/conftest.py writes
each failed test to a report as "miss <test>" (a TimingToleranceMissed in the test itself) or
"fail <test>" (anything else), and run_with_one_retry() decides from that report.

scripts/test-system.sh runs the timing tests through the command line of this module:

    python -m fcn_tools.timing --report FILE -- <pytest command line>

where every "{tag}" in the command line becomes "timing" for the first run and
"timing-retry" for the repetition (the names of their JUnit reports and log directories).
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path
from typing import Callable, Sequence

# Exit status when the tests missed only timing tolerances, in the first run and again in its
# repetition: the same status the platform self-tests use for a missed timing tolerance
# (EX_TEMPFAIL, tests/platform/run_selftest.sh). It is a failure, but one that says "the host
# did not keep time", which on an idle machine means a timing regression.
TIMING_MISSED = 75

# pytest's exit statuses: tests ran and some failed; no test was selected (not a failure of
# the timing run).
TESTS_FAILED = 1
NO_TESTS_COLLECTED = 5


class TimingToleranceMissed(AssertionError):
    """A measured time was outside its tolerance; nothing else went wrong."""


def check_timing(ok: bool, message: str) -> None:
    """Assert a timing tolerance. Use plain assert for everything that is not one."""
    if not ok:
        raise TimingToleranceMissed(message)


def report_lines(report: Path) -> list[str]:
    """The lines of a timing report; none if the file does not exist."""
    try:
        return report.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        return []


def only_timing_missed(report: Path) -> bool:
    """True if the report lists at least one failure and every one is a missed timing
    tolerance. An empty or missing report is False: a run can fail without any test failing
    (a collection error, an interrupted session), and nothing then says it was the host."""
    lines = report_lines(report)
    return bool(lines) and all(line.startswith("miss ") for line in lines)


def run_with_one_retry(run: Callable[[str, Sequence[str]], int], report: Path,
                       say: Callable[[str], None] = print) -> int:
    """Run the timing tests and repeat the failed ones once if all they missed was a timing
    tolerance.

    run(tag, extra_args) runs pytest once and returns its exit status; extra_args select the
    report and, for the repetition, --last-failed. Returns 0 if the first run or the
    repetition passed, TIMING_MISSED if both missed timing tolerances and nothing else went
    wrong, and 1 for any other failure: a failure that is not a timing miss is never
    repeated.
    """
    def ran(tag: str, extra: Sequence[str]) -> int:
        report.unlink(missing_ok=True)
        status = run(tag, [*extra, "--timing-report", str(report)])
        return 0 if status == NO_TESTS_COLLECTED else status

    # pytest exits 1 when tests failed; any other non-zero status (interrupted, internal or
    # usage error) is not a verdict on the tests and is never repeated.
    status = ran("timing", [])
    if status == 0:
        return 0
    if status != TESTS_FAILED or not only_timing_missed(report):
        return 1
    say("timing tolerances missed, repeating those tests once:")
    for line in report_lines(report):
        say(f"  {line}")
    status = ran("timing-retry", ["--last-failed"])
    if status == 0:
        return 0
    if status == TESTS_FAILED and only_timing_missed(report):
        say("timing tolerances missed again: either the host is too busy to keep time or the "
            "timing has regressed; repeat on an otherwise idle machine to tell them apart")
        for line in report_lines(report):
            say(f"  {line}")
        return TIMING_MISSED
    return 1


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m fcn_tools.timing",
        description="Run the timing tests with one repetition for missed timing tolerances.")
    parser.add_argument("--report", required=True, type=Path,
                        help="the timing report file (tests/conftest.py --timing-report)")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="-- followed by the pytest command line; {tag} is substituted")
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no pytest command line after --")

    def run(tag: str, extra: Sequence[str]) -> int:
        return subprocess.call([part.replace("{tag}", tag) for part in command] + list(extra))

    return run_with_one_retry(run, args.report, say=lambda text: print(text, flush=True))


if __name__ == "__main__":
    sys.exit(main())
