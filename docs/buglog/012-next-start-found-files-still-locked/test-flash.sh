#!/usr/bin/env bash
# The regression test added with the fix for the end without a kernel and for the flash file:
# the platform test of the host's flash. A child with the flash and a retained cell open ends at
# a power cut with status 12, while a process it forked keeps mappings of both files, and with
# them the two open file descriptions the child locked, alive. The test fails if another
# process cannot lock either file once the child's exit has been seen, and if the close of the
# flash leaves the lock in place while such a mapping exists.
"$BUILD/tests/platform/test_posix_flash"
