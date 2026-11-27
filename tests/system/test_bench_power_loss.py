# SPDX-License-Identifier: MIT
"""bench/power_loss.py, the power-loss campaign, run in its --quick mode into a temporary
directory: one trial of each class (a staging cut in its COMMIT record's commit field, a download
over the bus cut in a page, a crashing trial cut in its second attempt), each restarted through
fcn_boot and followed by the next update. The test checks the result's structure and that its
counts agree with each other and with the plan, never a time: the times per trial and the wall
time may be anything. Then one trial is replayed from its row (--replay) and must come out the
same, and --replot redraws the plot. The bench takes ports of its own and does not use the bus
fixtures."""
from __future__ import annotations

import json
import os
import subprocess
import sys

import pytest

from fcn_tools import paths, powerloss

#: A bound for the whole --quick run (about half a minute on the release build: the download's
#: next update is a download of the node with the flasher, 24 s); not a timing tolerance.
QUICK_TIMEOUT_S = 300

#: pytest's limit for the tests of this file, above the bounds of the runs they make (the
#: project's default of 120 s per test would end a quick run on a slow host first, without
#: the bench's output); a bound, not a timing tolerance.
pytestmark = pytest.mark.timeout(480)

MEMBERS = ["schema", "provenance", "host_timing_note", "loaded", "load_limit", "build", "config",
           "summary", "legend", "images", "bases", "sequences", "references", "by_class",
           "failures", "trials"]


@pytest.mark.slow
def test_the_quick_campaign_writes_a_consistent_result(tmp_path):
    bench = paths.REPO_ROOT / "bench" / "power_loss.py"
    run = subprocess.run([sys.executable, str(bench), "--quick", "--out", str(tmp_path),
                          "--allow-loaded", "--no-build"], cwd=paths.REPO_ROOT,
                         capture_output=True, text=True, timeout=QUICK_TIMEOUT_S, check=False,
                         env=dict(os.environ))
    assert run.returncode == 0, f"status {run.returncode}\n{run.stdout}\n{run.stderr}"
    result = json.loads((tmp_path / "power_loss.json").read_text())
    assert list(result) == MEMBERS
    assert result["schema"] == "fcn.result/power_loss/1"
    assert (tmp_path / "power_loss.png").stat().st_size > 0

    # The build: every binary and every node run is the tree's build.
    build = result["build"]
    assert {entry["build_id"] for entry in build["binaries"].values()} == {build["tree"]}
    assert set(build["reported"].values()) == {build["tree"]}

    # The plan: one trial per class, as the config says.
    config, summary = result["config"], result["summary"]
    assert config["quick"] is True
    assert config["trials_per_class"] == {"stage": 1, "download": 1, "boot": 1}
    rows = [powerloss.parse_row(text) for text in result["trials"]["rows"]]
    assert result["trials"]["columns"] == list(powerloss.COLUMNS)
    assert sorted(row["index"] for row in rows) == [0, 1, 2]
    assert summary["trials"]["value"] == len(rows) == 3
    assert {cls: f["value"] for cls, f in summary["trials_per_class"].items()} == \
        config["trials_per_class"]
    for cls in powerloss.CLASSES:
        assert result["by_class"][cls]["trials"] == 1
        assert summary["seconds_per_trial"][cls]["n"] == 1
    roles = {(row["class"], row["sequence"], row["role"]) for row in rows}
    assert roles == {("stage", "fresh", "commit.commit_field"),
                     ("download", "fresh", "slot.page"), ("boot", "rollback", "attempt.2")}

    # Every trial passed every check, and its row says what the cut left and what followed.
    assert result["failures"] == []
    for key in ("invariant_violations", "cuts_not_where_drawn", "follow_up_failures",
                "bench_errors"):
        assert summary[key]["value"] == 0, key
    for row in rows:
        assert row["check"] is None, row
        assert row["left"] in ("before", "after")
        assert row["tear"] in ("none", "some", "all", "nothing")
        assert row["starts"] and row["boot"] == row["starts"][0].rsplit(" ", 1)[0]
        assert row["follow_up"].startswith(("A 1.4.0 confirmed", "B 1.4.0 confirmed"))
        sequence = result["sequences"][f"{row['class']}:{row['sequence']}"]
        op = sequence[row["op"] - 1]
        assert op.startswith(f"{row['op']} ") and f" {row['role']} " in op
        assert row["before"] in op.split(": ", 1)[1]
    # The model of each sequence was checked against a run without a cut.
    assert set(result["references"]) == set(result["sequences"])
    by_role = {(cls, role): entry for cls in powerloss.CLASSES
               for role, entry in result["by_class"][cls]["by_role"].items()}
    assert sum(entry["trials"] for entry in by_role.values()) == 3

    # A trial replayed from its row (class, sequence, op, seed) does the same again: the cut
    # tears the same bits, the board comes back the same way. (The bootloader and the crash
    # payload only: a few seconds.)
    boot = next(row for row in rows if row["class"] == "boot")
    spec = f"boot:{boot['sequence']}:{boot['op']}:{boot['seed']}"
    replayed = subprocess.run([sys.executable, str(bench), "--replay", spec, "--out",
                               str(tmp_path), "--allow-loaded", "--no-build"],
                              cwd=paths.REPO_ROOT, capture_output=True, text=True, timeout=60,
                              check=False)
    assert replayed.returncode == 0, f"{replayed.stdout}\n{replayed.stderr}"
    again = json.loads(next(line for line in replayed.stdout.splitlines()
                            if line.startswith("{")))
    same = ("op", "seed", "role", "cut", "tear", "before", "after", "left", "boot", "starts",
            "follow_up")
    assert {key: again[key] for key in same} == {key: boot[key] for key in same}

    # --replot redraws the plot from the JSON alone.
    (tmp_path / "power_loss.png").unlink()
    again = subprocess.run([sys.executable, str(bench), "--replot", "--out", str(tmp_path)],
                           cwd=paths.REPO_ROOT, capture_output=True, text=True, timeout=60,
                           check=False)
    assert again.returncode == 0, again.stderr
    assert (tmp_path / "power_loss.png").stat().st_size > 0
