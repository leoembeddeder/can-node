# SPDX-License-Identifier: MIT
"""bench/bus_off.py runs: its --quick mode, into a temporary directory, against the binaries of
this test run (FCN_NODE and the others under the asan pass of scripts/test-system.sh). The test
checks the result's structure and its counts, and the figures the controller model fixes (1408
bit times, 24 and 32 destroyed attempts, TEC 128 and 256, and 416 bit times from error passive to
bus off under a short in a trial that no host time entered), never a figure of the host's clock:
those may be anything on a loaded test machine. The bench itself checks the run's counts against
the node's statistics and refuses to write a result where they do not fit."""
from __future__ import annotations

import json
import os
import subprocess
import sys

import pytest

from fcn_tools import busoff, paths, results

#: A quick run takes about 31 s on an idle machine (two trials of each fault with the 5.2 s
#: stable wait after each, and a short held through three bus offs); a bound, not a tolerance.
RUN_TIMEOUT_S = 300


#: pytest's limit for the tests of this file, above the bounds of the runs they make (the
#: project's default of 120 s per test would end a quick run on a slow host first, without
#: the bench's output); a bound, not a timing tolerance.
pytestmark = pytest.mark.timeout(480)


def test_a_quick_run_writes_a_result_whose_counts_fit(tmp_path):
    out = tmp_path / "out"
    run = subprocess.run(
        [sys.executable, str(paths.REPO_ROOT / "bench" / "bus_off.py"), "--quick", "--out",
         str(out), "--allow-loaded", "--no-build"],
        cwd=paths.REPO_ROOT, capture_output=True, text=True, timeout=RUN_TIMEOUT_S,
        env=dict(os.environ))
    assert run.returncode == 0, f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}"
    assert (out / "bus_off.png").stat().st_size > 0
    data = json.loads((out / "bus_off.json").read_text())

    assert data["schema"] == "fcn.result/bus_off/1"
    assert list(data)[:8] == ["schema", "provenance", "host_timing_note", "loaded", "load_limit",
                              "build", "config", "summary"]
    assert data["build"]["tree"] == results.tree_build_id(data["provenance"])
    assert set(data["build"]["reported"].values()) == {data["build"]["tree"]}
    config = data["config"]
    assert config["quick"] is True and config["bitrates"] == [500000]
    assert config["faults"] == list(busoff.FAULTS) and config["trials_per_fault"] == 2
    assert config["standard"]["recovery_bits"] == 1408
    results.check_summary(data["summary"])

    (run_data,) = data["runs"]
    assert run_data["bitrate"] == 500000
    assert all(check["ok"] for check in run_data["checks"]), run_data["checks"]
    trials = len(run_data["trials"]["short"]["holdoff"]) + len(
        run_data["trials"]["tx_errors"]["holdoff"])
    repeated = run_data["repeated_trials"]
    persisting = run_data["persisting_short"]
    assert trials == 4
    assert run_data["bus_offs_caused"] == trials + sum(r["bus_offs"] for r in repeated) + 3
    assert run_data["node"]["queue"]["bus_off_count"] == run_data["bus_offs_caused"]
    assert run_data["node"]["controller"]["bus_off_events"] == run_data["bus_offs_caused"]
    assert run_data["node"]["dtcs"]["CAN_BUS_OFF"]["status"] == busoff.DTC_HEALED_STATUS

    for fault, columns in run_data["trials"].items():
        assert {len(values) for values in columns.values()} == {2}, fault
        # Fixed by the model and the rules, whatever the host did.
        assert columns["recovery_sequence_bits"] == [1408.0, 1408.0]
        assert columns["recovery_sequence"] == [2_816_000, 2_816_000]
        assert columns["attempts_to_bus_off"] == [busoff.ATTEMPTS_TO_BUS_OFF[fault]] * 2
        assert columns["tec_at_passive"] == [128, 128] and columns["tec_at_bus_off"] == [256, 256]
        # Whether the first BMS_Fault after the recovery already carries the DTCs depends on
        # when the host ran the health task (fcn_tools.busoff.observer_figures()): a figure,
        # not a count. That each episode set and healed them once is a check of the run.
        assert all(isinstance(value, bool) for value in columns["dtc_bus_off_after"])
        assert all(isinstance(value, bool) for value in columns["dtc_passive_after"])
        assert all(lost >= 0 for lost in columns["heartbeats_lost"])
        for attempts, tec, count in run_data["tec_samples"][fault]:
            assert tec == busoff.tec_after_attempts(fault, attempts) and count >= 1
        # Whether a host time entered error passive to bus off is the host's; that the summary
        # keeps such a trial out of the figure labelled the model's is not.
        exact = columns["passive_to_bus_off_exact"]
        assert all(isinstance(value, bool) for value in exact)
        entry = data["summary"]["500_kbit_s"][fault]
        assert entry["passive_to_bus_off_bits_not_exact"]["n"] == 2 - sum(exact)
        if fault == "short":
            assert entry["passive_to_bus_off_bits"]["part"] == "model"
            assert entry["passive_to_bus_off_bits"]["n"] == sum(exact)
            assert all(bits == busoff.SHORT_PASSIVE_TO_BUS_OFF_BITS for bits, ok in zip(
                columns["passive_to_bus_off_bits"], exact) if ok)
        else:
            assert entry["passive_to_bus_off_bits"]["part"] == "mixed"
            assert entry["passive_to_bus_off_bits"]["n"] == 2
            assert entry["passive_to_bus_off_bits_exact"]["n"] == sum(exact)
    assert persisting["attempts"] == [24, 24, 24]
    assert persisting["recovery_sequence_bits"] == [1408.0] * 3
    assert persisting["recoveries_under_short"] == 2
    assert persisting["expected_ms"] == [100, 200, 400] and len(persisting["holdoffs_ms"]) == 3

    # --replot redraws the figure from the JSON alone.
    (out / "bus_off.png").unlink()
    replot = subprocess.run(
        [sys.executable, str(paths.REPO_ROOT / "bench" / "bus_off.py"), "--replot", "--out",
         str(out)], cwd=paths.REPO_ROOT, capture_output=True, text=True, timeout=60)
    assert replot.returncode == 0, replot.stderr
    assert (out / "bus_off.png").stat().st_size > 0


def test_a_quick_run_needs_an_output_directory():
    run = subprocess.run([sys.executable, str(paths.REPO_ROOT / "bench" / "bus_off.py"),
                          "--quick"], cwd=paths.REPO_ROOT, capture_output=True, text=True,
                         timeout=60)
    assert run.returncode == 2 and "--out" in run.stderr
