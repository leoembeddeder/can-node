#!/usr/bin/env bash
# The system test of the running node: a 0x700 flood at 1.2 times the bus capacity, on the UDP
# bus and on SocketCAN (skipped where vcan0 is missing, unless FCN_REQUIRE_SOCKETCAN is set).
# The functional test only; its timing twin asserts on the wall clock and is not run here.
"$FCN_PYTHON" -m pytest tests/system/test_can_flood.py -q -p no:cacheprovider -m "not timing" \
    --artifacts "$FCN_BUILD_ROOT/system-test-logs"
