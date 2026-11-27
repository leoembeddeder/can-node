#!/usr/bin/env bash
# The regression test added with the fix: the platform self-test of the stack report, whose
# "body" task keeps a 16 KiB buffer in the frame of its own task function. The self-test fails
# if that buffer is not in the task's reported depth.
"$BUILD/tests/platform/plat_selftest" stack
