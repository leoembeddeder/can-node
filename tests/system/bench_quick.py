"""What the functional tests of the benches share (tests/system/test_bench_*.py): the transports,
the check that SocketCAN is there, and a --quick run of a bench into a temporary directory."""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from fcn_tools import bus, paths, runner

TRANSPORTS = ["udp", pytest.param("socketcan", marks=pytest.mark.socketcan)]

#: A bound, not a tolerance: a quick run takes about 20 s on the development laptop, longer while
#: another bench or test run holds vcan0.
BENCH_TIMEOUT_S = 300


def usable(transport: str) -> None:
    """Skip (or fail, with FCN_REQUIRE_SOCKETCAN) a SocketCAN test without the interface."""
    if transport == "socketcan" and not bus.socketcan_available(runner.DEFAULT_INTERFACE):
        message = f"SocketCAN interface {runner.DEFAULT_INTERFACE} is not available"
        if os.environ.get("FCN_REQUIRE_SOCKETCAN"):
            pytest.fail(message, pytrace=False)
        pytest.skip(message)


def run_bench(name: str, out: Path, *args: str) -> dict:
    """bench/<name>.py --quick --out OUT --allow-loaded --no-build ARGS; its result. The binaries
    are those of the test run (FCN_NODE and the others in the asan pass)."""
    done = subprocess.run([sys.executable, str(paths.REPO_ROOT / "bench" / f"{name}.py"),
                           "--quick", "--out", str(out), "--allow-loaded", "--no-build", *args],
                          capture_output=True, text=True, timeout=BENCH_TIMEOUT_S, check=False,
                          cwd=paths.REPO_ROOT)
    assert done.returncode == 0, f"{name} exited {done.returncode}\n{done.stdout}\n{done.stderr}"
    assert (out / f"{name}.png").stat().st_size > 0
    return json.loads((out / f"{name}.json").read_text())
