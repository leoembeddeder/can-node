#!/usr/bin/env bash
# The controller model's unit test; the tests of "2b. arbitration against received frames" are
# the regression tests, test_a_frame_of_this_node_goes_before_a_waiting_received_frame_with_a_larger_key
# the smallest of them.
"$BUILD/tests/unit/test_can_ctrl"
