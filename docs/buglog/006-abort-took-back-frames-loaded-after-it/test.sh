#!/usr/bin/env bash
# The tests of the host CAN controller through the driver interface. The regression tests are
# test_a_frame_loaded_right_after_an_abort_is_not_aborted and
# test_an_abort_takes_back_the_frames_loaded_before_it_and_no_other.
"$BUILD/tests/platform/test_posix_can"
