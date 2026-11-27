"""bench/updates.py runs: its --quick mode (one update over the UDP bus, one download of a small
data payload) into a temporary directory writes a result of the documented schema whose counts
agree with each other, with the flasher's report, with the node's log and statistics and with
the flash, and whose node's restart is the difference of the two runs' log stamps. Structure
and counts only: no figure of the host's clock is judged here (bench/README.md)."""
from __future__ import annotations

import json
import subprocess
import sys

import pytest

from fcn_tools import paths, results

BENCH = paths.REPO_ROOT / "bench" / "updates.py"
#: A bound on the quick run (it takes about 30 s, most of it the download of the node image at
#: 2 events of the diagnostic task per tick), not a timing tolerance.
QUICK_TIMEOUT_S = 300

#: pytest's limit for the tests of this file, above the bounds of the runs they make (the
#: project's default of 120 s per test would end a quick run on a slow host first, without
#: the bench's output); a bound, not a timing tolerance.
pytestmark = pytest.mark.timeout(360)

UPDATE_STEPS = ["boot_info", "default_session", "extended_session", "programming_session",
                "erase", "request_download", "transfer", "transfer_exit", "check", "ecu_reset",
                "wait_for_trial", "read_version", "extended_session_trial", "confirm",
                "boot_info_confirmed"]


@pytest.mark.slow
def test_a_quick_run_writes_a_consistent_result(tmp_path):
    run = subprocess.run([sys.executable, str(BENCH), "--quick", "--out", str(tmp_path),
                          "--allow-loaded", "--no-build"], capture_output=True, text=True,
                         timeout=QUICK_TIMEOUT_S, cwd=paths.REPO_ROOT, check=False)
    assert run.returncode == 0, run.stdout + run.stderr
    data = json.loads((tmp_path / "updates.json").read_text())
    assert (tmp_path / "updates.png").stat().st_size > 0
    assert list(data)[:8] == list(results.RESERVED)
    assert data["schema"] == "fcn.result/updates/1"
    results.check_summary(data["summary"])
    build = data["build"]
    assert set(build["reported"].values()) == {build["tree"]}
    assert "fcn_node (image payload)" in build["binaries"]

    config = data["config"]
    assert config["updates"] == {"socketcan": 0, "udp": 1}
    assert list(data["campaigns"]) == ["udp"]
    campaign = data["campaigns"]["udp"]
    assert campaign["counts"] == {"requested": 1, "attempted": 1, "succeeded": 1, "failed": 0,
                                  "not_attempted": 0}
    (update,) = campaign["records"]
    assert update["success"] and update["problems"] == []
    assert list(update["steps"]) == UPDATE_STEPS
    assert update["total_s"] == pytest.approx(sum(update["steps"].values()), abs=1e-5)
    assert (update["version"], update["target_slot"], update["result"]) == ("3.1.0", "B",
                                                                          "confirmed")
    assert update["image_bytes"] == config["node_payload_bytes"] + 256
    report = update["report"]
    assert report["blocks"] == -(-update["image_bytes"] // 1024) == report["block_ms"]["n"]
    assert report["transfer"]["bytes"] == update["image_bytes"]
    assert [run_["status"] for run_ in update["runs"]] == [10, 0]
    erase = update["node_download"]["erase"]
    assert (erase["address"], erase["bytes"]) == (0x102000, update["image_bytes"])
    # The node's restart from its own log stamps, both lines found, the trial's after the
    # reset's: the summary's reset_to_trial is that, and wait_for_trial the flasher's step.
    restart = update["node_restart"]
    assert restart["us"] == restart["trial_start_us"] - restart["reset_us"] > 0
    summary = data["summary"]["updates_udp"]
    assert summary["reset_to_trial"]["n"] == 1 and summary["reset_to_trial"]["unit"] == "ms"
    assert summary["reset_to_trial"]["max"] == pytest.approx(restart["us"] / 1000, abs=1e-3)
    assert summary["reset_to_trial_refused"]["value"] == 0
    assert summary["wait_for_trial"]["max"] == pytest.approx(update["steps"]["wait_for_trial"],
                                                             abs=1e-3)
    trial = update["node_trial"]
    assert trial["handoff"]["slot"] == "B" and trial["handoff"]["image_version"] == "3.1.0"
    assert trial["confirm_state"] == "CONFIRMED"
    assert trial["confirmed_by_routine"] == (update["confirmed_by"] == "routine")
    assert update["flash"]["states"] == ["previous", "confirmed"]
    assert update["flash"]["versions"] == ["1.0.0", "3.1.0"]
    assert build["reported"]["udp-u01 trial run"] == trial["build_id"]

    big = data["big_image"]
    assert (big["payload_bytes"], big["payload_kind"]) == (8192, "data")
    assert big["booted_afterwards"] is None
    assert big["counts"]["succeeded"] == big["counts"]["requested"] == 1
    (download,) = big["records"]
    assert list(download["steps"]) == UPDATE_STEPS[:UPDATE_STEPS.index("check") + 1]
    node = download["node"]
    assert node["download"]["received"] == download["image_bytes"] == 8192 + 256
    assert node["download"]["blocks"] == node["jobs"]["write"]["run"] == 9
    assert download["flash"]["states"] == ["confirmed", "trial"]
    assert data["summary"]["updates_udp"]["succeeded"]["value"] == 1
