#!/usr/bin/env bash
# The system test that caught the bug: the statistics the node writes after a 2 s run.
"$FCN_PYTHON" -m pytest tests/system/test_smoke.py -q -p no:cacheprovider \
    -k test_runs_for_the_requested_time_and_reports_statistics \
    --artifacts "$FCN_BUILD_ROOT/system-test-logs"
