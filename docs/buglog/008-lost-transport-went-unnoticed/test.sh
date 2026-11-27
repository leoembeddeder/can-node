#!/usr/bin/env bash
# The system test that deletes the node's SocketCAN interface under it, in a user and network
# namespace of its own (unshare -rn), and expects the dedicated exit status, the log lines and
# the counters. Where the namespace or the vcan module is not available the test is skipped;
# here that counts as a failure, since a skipped test cannot show the bug.
out="$("$FCN_PYTHON" -m pytest tests/system/test_node_errors.py -q -p no:cacheprovider -rs \
    -k test_a_transport_that_goes_away_stops_the_node_with_its_own_exit_status \
    --artifacts "$FCN_BUILD_ROOT/system-test-logs" 2>&1)"
status=$?
echo "$out"
if [ "$status" -eq 0 ] && ! grep -q '1 passed' <<<"$out"; then
    echo "the test did not run here, so it cannot show the bug"
    exit 1
fi
exit "$status"
