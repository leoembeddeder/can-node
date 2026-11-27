#!/usr/bin/env bash
# The interrupt storm: the line is raised in a tight loop for 2 s with signal delivery. The
# self-test fails if a handler ran on top of another one, if a task's stack grew beyond what
# a single handler needs, if a raise was never served, or if the process died.
"$BUILD/tests/platform/plat_selftest" irq signal 0 2 1
