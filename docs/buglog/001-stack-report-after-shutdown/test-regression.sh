#!/usr/bin/env bash
# The regression test added with the fix: the platform self-test of the stack report after an
# orderly stop.
"$BUILD/tests/platform/plat_selftest" stack
