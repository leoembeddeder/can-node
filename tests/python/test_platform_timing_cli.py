"""bench/platform_timing.py's command line since phase 6: --out, and the load check of every
bench (--max-load, --allow-loaded, exit status 3). Nothing is measured here: the measurement is
the self-test's, and its figures are the host's."""
from __future__ import annotations

import subprocess
import sys

from fcn_tools import paths, results

BENCH = paths.REPO_ROOT / "bench" / "platform_timing.py"


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(BENCH), *args], capture_output=True, text=True,
                          timeout=60, cwd=paths.REPO_ROOT, check=False)


def test_the_help_names_the_output_directory_and_the_load_check():
    done = run("--help")
    assert done.returncode == 0
    for option in ("--seconds", "--replot", "--out", "--max-load", "--allow-loaded"):
        assert option in done.stdout
    assert f"default {results.DEFAULT_LOAD_LIMIT}" in done.stdout


def test_a_replot_reads_the_output_directory_it_is_given(tmp_path):
    # Nothing in tmp_path to redraw: it says so and writes nothing, whatever results/ holds.
    done = run("--replot", "--out", str(tmp_path))
    assert done.returncode == 1 and "nothing to replot" in done.stderr
    assert list(tmp_path.iterdir()) == []


def test_a_load_limit_must_be_a_positive_number():
    # A usage error, before anything is built or measured.
    done = run("--max-load", "0", "--out", "/nonexistent-never-written")
    assert done.returncode == 2 and "positive" in done.stderr
