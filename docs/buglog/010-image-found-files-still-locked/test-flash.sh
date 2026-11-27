#!/usr/bin/env bash
# The regression test added with the fix for the flash file: the platform test of the host's
# flash, whose test of the release forks a process that tries to lock the flash file while it
# is still open and mapped. It fails if the bootloader's release (posix_flash_unlock()) leaves
# the lock in place.
"$BUILD/tests/platform/test_posix_flash"
