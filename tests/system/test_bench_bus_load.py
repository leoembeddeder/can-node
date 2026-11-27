"""bench/bus_load.py in its --quick mode, into a temporary directory, on each transport: it writes
a result of the right schema whose counts agree with each other and with the node's own, and
whose frame lengths lie within what CAN allows. No timing figure is judged here. The bench takes
the vcan0 lock itself, so this test does not use the can_spec fixture."""
from __future__ import annotations

import pytest

from bench_quick import TRANSPORTS, run_bench, usable
from fcn_tools import canbits, results

#: The node's cyclic messages in the DBC (its BMS messages with a cycle time).
#: pytest's limit for the tests of this file, above the bounds of the runs they make (the
#: project's default of 120 s per test would end a quick run on a slow host first, without
#: the bench's output); a bound, not a timing tolerance.
pytestmark = pytest.mark.timeout(360)

CYCLIC = {"BMS_Fault", "BMS_PackStatus", "BMS_Heartbeat", "BMS_CellVoltages1",
          "BMS_CellVoltages2", "BMS_Temperatures", "BMS_TaskStats", "BMS_NodeInfo",
          "BMS_BootInfo"}


@pytest.mark.parametrize("transport", TRANSPORTS)
def test_the_quick_run_writes_a_consistent_result(tmp_path, transport):
    usable(transport)
    result = run_bench("bus_load", tmp_path, "--transport", transport)
    assert result["schema"] == "fcn.result/bus_load/1"
    results.check_summary(result["summary"])
    assert set(result["build"]["reported"].values()) == {result["build"]["tree"]}
    nominal = result["nominal"]
    assert set(nominal["messages"]) == CYCLIC
    assert nominal["load_typical"] < nominal["load_worst_case"]

    conditions = result["transports"][transport]
    idle, download = conditions["idle"], conditions["download"]
    counters = idle["node_counters"]
    # The observer saw exactly the frames the node's controller completed (the bench checks it
    # too), and on an idle bus nothing else.
    assert counters["captured_node_frames"] == counters["tx_frames"] > 0
    assert counters["captured_other_frames"] == 0 and counters["rx_frames"] == 0
    assert set(idle["nominal"]) == CYCLIC
    for entry in (idle, *download["runs"]):
        assert entry["window_s"] > 0
        total = sum(item["load"] for item in entry["by_id"].values())
        assert abs(entry["load"] - total) < 1e-5
        assert sum(item["frames"] for item in entry["by_id"].values()) == entry["frames_in_window"]
        for key, item in entry["by_id"].items():
            assert item["frames"] > 0 and item["id"] == int(key, 16)
            # Every frame of the bus has 0 to 8 bytes: its length lies between the shortest
            # frame and the worst case of 8 bytes.
            assert canbits.unstuffed_bits(False, False, 0) <= item["bits_min"]
            assert item["bits_max"] <= canbits.worst_case_bits(False, False, 8)
    # During the download the tester's requests and the node's responses are both there.
    assert download["by_id"]["0x7e0"]["frames"] > 0 and download["by_id"]["0x7e8"]["frames"] > 0
    assert download["groups"]["tester"]["load"] > 0
    run = download["runs"][0]
    assert run["run"]["result"] == "committed"
    assert run["node_counters"]["captured_node_frames"] == run["node_counters"]["tx_frames"]
    assert run["node_counters"]["captured_other_frames"] == \
        run["node_counters"]["transport_rx_frames"]
