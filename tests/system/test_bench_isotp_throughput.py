"""bench/isotp_throughput.py runs: its --quick mode (UDP, two block sizes, a few blocks and
responses) into a temporary directory writes a result of the documented schema whose counts
agree with each other and with the node's own, and --replot redraws its plot. Structure and
counts only: no figure of the host's clock is judged here (bench/README.md)."""
from __future__ import annotations

import json
import math
import subprocess
import sys

import pytest

from fcn_tools import paths, results

BENCH = paths.REPO_ROOT / "bench" / "isotp_throughput.py"
#: A bound on the quick run (it took 4 s in a pilot), not a timing tolerance.
QUICK_TIMEOUT_S = 300


#: pytest's limit for the tests of this file, above the bounds of the runs they make (the
#: project's default of 120 s per test would end a quick run on a slow host first, without
#: the bench's output); a bound, not a timing tolerance.
pytestmark = pytest.mark.timeout(480)


@pytest.mark.slow
def test_a_quick_run_writes_a_consistent_result(tmp_path):
    run = subprocess.run([sys.executable, str(BENCH), "--quick", "--out", str(tmp_path),
                          "--allow-loaded", "--no-build"], capture_output=True, text=True,
                         timeout=QUICK_TIMEOUT_S, cwd=paths.REPO_ROOT, check=False)
    assert run.returncode == 0, run.stdout + run.stderr
    path = tmp_path / "isotp_throughput.json"
    data = json.loads(path.read_text())
    assert (tmp_path / "isotp_throughput.png").stat().st_size > 0
    assert list(data)[:8] == list(results.RESERVED)
    assert data["schema"] == "fcn.result/isotp_throughput/1"
    assert data["host_timing_note"].startswith(results.HOST_TIMING_NOTE)
    results.check_summary(data["summary"])
    build = data["build"]
    assert set(build["reported"].values()) == {build["tree"]}
    assert set(build["binaries"]) == {"fcn_node", "fcn_boot", "fcn_flash"}

    config = data["config"]
    assert config["transports"] == ["udp"] and list(data["transports"]) == ["udp"]
    part = data["transports"]["udp"]
    assert [s["data_bytes"] for s in part["transfer"]] == config["block_sizes_data_bytes"]
    assert len(build["reported"]) == len(config["block_sizes_data_bytes"]) + 1
    for size in part["transfer"]:
        n = size["data_bytes"]
        blocks = math.ceil(size["image_bytes"] / n)
        assert size["image_bytes"] == max(config["blocks_per_size"] * n,
                                          config["min_image_bytes"])
        assert size["blocks"] == blocks
        assert size["full_blocks"] == size["image_bytes"] // n
        assert (size["full_blocks_decomposed"] + size["clock_disagreements"]["blocks"]
                <= size["full_blocks"])
        assert size["stray_frames"] == 0
        assert size["tester_block_ms"]["n"] == size["full_blocks"]
        assert size["raw"]["tester_block_ms"]["n"] == blocks
        assert size["wire"]["period_ms"]["n"] == size["full_blocks_decomposed"]
        assert size["events"] == size["frames_to_node"] + size["flow_controls"]
        node = size["node"]
        assert (node["isotp_bs"], node["events_per_tick"], node["tick_rate_hz"]) == (16, 2, 1000)
        download = node["download"]
        assert (download["received"], download["size"], download["blocks"]) == (
            size["image_bytes"], size["image_bytes"], blocks)
        assert (download["committed"], download["repeated_blocks"]) == (1, 0)
        assert node["write_jobs"]["run"] == blocks
        assert node["write_jobs"]["total_ns"] >= node["write_jobs"]["max_ns"] > 0
        assert node["task"]["rx_lost"] == 0 and node["task"]["tx_done_lost"] == 0
    response = part["response"]
    assert response["request"] == "19 0A"
    assert response["requests"] == config["responses"]
    assert response["responses_on_the_wire"] == config["responses"]
    assert (response["responses_decomposed"] + response["clock_disagreements"]
            == config["responses"])
    assert response["wire"]["total_ms"]["n"] == response["responses_decomposed"]
    assert response["response_bytes"] == 71 and response["response_lengths_seen"] == [71]
    assert (response["frames"], response["consecutive_frames"]) == (11, 10)
    assert response["node"]["read_dtc_requests"] == config["responses"]
    assert part["projection_2048"]["projection"] is True
    assert data["transport_difference"] is None

    before = (tmp_path / "isotp_throughput.png").stat().st_mtime_ns
    replot = subprocess.run([sys.executable, str(BENCH), "--replot", "--out", str(tmp_path)],
                            capture_output=True, text=True, timeout=60, cwd=paths.REPO_ROOT,
                            check=False)
    assert replot.returncode == 0, replot.stderr
    assert (tmp_path / "isotp_throughput.png").stat().st_mtime_ns >= before
    assert json.loads(path.read_text()) == data


def test_a_quick_run_without_out_is_refused(tmp_path):
    run = subprocess.run([sys.executable, str(BENCH), "--quick"], capture_output=True,
                         text=True, timeout=60, cwd=paths.REPO_ROOT, check=False)
    assert run.returncode == 2
    assert "--out" in run.stderr
