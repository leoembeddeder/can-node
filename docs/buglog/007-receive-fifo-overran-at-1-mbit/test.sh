#!/usr/bin/env bash
# The system test that loads the node's bus at 1 Mbit/s with the shortest frames, once with an
# identifier nobody handles and once with the ping, and counts what the receive FIFO lost in
# its measurement window. It is marked timing: a host too busy to load the bus, or one that
# holds the receive task up at a stretch for long enough to cost frames in the window (which
# shows as ticks dropped from the window's start on), misses a tolerance. As
# scripts/test-system.sh does, a run whose failures were all such misses is repeated once, and
# if the repetition misses only timing tolerances too, the status is 75: the run neither shows
# nor rules out the bug (scripts/buglog-verify.sh reports that as inconclusive).
report="$FCN_BUILD_ROOT/buglog-007-timing-report.txt"
run() {
    rm -f "$report"
    "$FCN_PYTHON" -m pytest tests/system/test_node_errors.py -q -p no:cacheprovider -rP \
        -k test_the_receive_path_keeps_up_with_a_full_bus_of_the_shortest_frames_at_1_mbit \
        --timing-report "$report" --artifacts "$FCN_BUILD_ROOT/system-test-logs"
}
only_timing_missed() {
    [ -s "$report" ] && ! grep -qv '^miss ' "$report"
}
run && exit 0
only_timing_missed || exit 1
echo "only timing tolerances were missed; repeating once"
run && exit 0
only_timing_missed && exit 75
exit 1
