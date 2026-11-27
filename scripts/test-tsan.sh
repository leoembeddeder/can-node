#!/usr/bin/env bash
# Build the tsan preset and run every test of lock-free code between threads under
# ThreadSanitizer.
#   scripts/test-tsan.sh
# A JUnit report is written to <build>/test-results/ctest-tsan.xml and CTest's output to
# <build>/test-results/ctest-tsan.log.
#
# Why: the lock-free ring is correct only with its acquire and release orderings, and no test
# can see them on x86-64, where every load and store is ordered that strongly anyway.
# ThreadSanitizer checks what the C11 memory model requires instead: with an ordering missing,
# the element copies of producer and consumer are no longer ordered, and it reports a data race.
#
# Which tests:
#  - every tests/unit/*.c that calls pthread_create() (test_spsc_ring: the ring itself);
#  - every Unity test in tests/platform that starts the CAN controller's hardware thread with
#    posix_can_start() (test_posix_can: the rings, atomics, eventfd and abort and restart
#    commands between the driver and the hardware thread, and simulation control served by
#    that thread). Its threads are started inside platform code, so a search for
#    pthread_create() in the test's source does not find it;
#  - every Unity test in tests/platform that calls pthread_create() itself (test_posix_sensor:
#    the sensor model read by one thread while another serves its simulation control commands).
# The other Unity tests have one thread. plat_selftest is not run: it runs the FreeRTOS POSIX
# port, and under ThreadSanitizer programs on that port report races inside the port's own
# scheduler code (port.c and tasks.c: vTaskSwitchContext against xTaskGetCurrentTaskHandle,
# vPortEndScheduler), outside this project's code. Only the selected targets are built; the
# rest of the preset's build tree is unused.
#
# Address-space randomisation: the ThreadSanitizer runtime that gcc 13 uses on Ubuntu 24.04
# expects the program's memory in fixed ranges. On a kernel that randomises mmap with more than
# 28 bits, a test run then fails at random: most runs stop with "unexpected memory mapping",
# some die from SIGSEGV without a message, and some pass. The tests are therefore run with
# randomisation off for their process tree (setarch -R, which needs no privileges). Where that
# is not permitted, as in a container whose seccomp profile forbids personality(), they run with
# randomisation on, which works when vm.mmap_rnd_bits is at most 28 and is unreliable above.
# In that case the script reads vm.mmap_rnd_bits (the kernel lets only root read it) and stops
# before building if it is above 28. If the value is 28 or less, or cannot be read, the tests
# run, and a failure with "unexpected memory mapping" or a test that died from a signal is
# reported with a hint that the address-space layout may be the cause.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

preset=tsan
dir="$FCN_BUILD_ROOT/$preset"
mkdir -p "$FCN_BUILD_ROOT" "$dir/test-results"
layout_fix="run 'sudo sysctl vm.mmap_rnd_bits=28' or allow setarch -R"

# Build targets are named after the source file; CTest names a unit test like its target and a
# platform test platform_<name without test_>.
mapfile -t unit_tests < <(grep -l 'pthread_create(' "$FCN_ROOT"/tests/unit/*.c | xargs -r -n1 basename | sed 's/\.c$//' | sort)
mapfile -t platform_tests < <(grep -l -e 'posix_can_start(' -e 'pthread_create(' "$FCN_ROOT"/tests/platform/test_*.c | xargs -r -n1 basename | sed 's/\.c$//' | sort)
if [[ ${#unit_tests[@]} -eq 0 || ${#platform_tests[@]} -eq 0 ]]; then
    echo "tsan: found unit tests '${unit_tests[*]}' and platform tests '${platform_tests[*]}';" \
        "each list should have at least one, so the selection is broken"
    exit 1
fi
tests=("${unit_tests[@]}" "${platform_tests[@]}")
ctest_names=("${unit_tests[@]}")
for t in "${platform_tests[@]}"; do
    ctest_names+=("platform_${t#test_}")
done
echo "==> threaded tests: ${tests[*]}"

no_aslr=()
rnd_bits=unreadable
if setarch "$(uname -m)" -R true 2>/dev/null; then
    no_aslr=(setarch "$(uname -m)" -R)
    echo "==> address-space randomisation off for the test run (setarch -R)"
else
    if bits="$(cat /proc/sys/vm/mmap_rnd_bits 2>/dev/null)" && [[ "$bits" =~ ^[0-9]+$ ]]; then
        rnd_bits="$bits"
    fi
    echo "==> setarch -R is not permitted here; running with randomisation on" \
        "(vm.mmap_rnd_bits = $rnd_bits)"
    if [[ "$rnd_bits" != unreadable ]] && ((10#$rnd_bits > 28)); then
        echo "tsan: setarch -R is not permitted and vm.mmap_rnd_bits is $rnd_bits, above the 28" \
            "bits this runtime accepts, so the tests would fail at random; $layout_fix"
        exit 1
    fi
fi

cmake --preset "$preset" -S "$FCN_ROOT" -B "$dir" >"$dir.configure.log" 2>&1 || {
    cat "$dir.configure.log" >&2
    exit 1
}
cmake --build "$dir" --target "${tests[@]}"

# halt_on_error: the first report ends the test with ThreadSanitizer's exit status (66).
export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1"
regex="^($(IFS='|' && echo "${ctest_names[*]}"))\$"
log="$dir/test-results/ctest-tsan.log"
status=0
# CTest's output is kept because LastTest.log records "Test Failed." both for a test that died
# from a signal and for one that failed; the terminal output tells them apart ("***Exception:
# SegFault" against "***Failed"), as does the JUnit report's failure message. CTest's status is
# taken from the pipeline explicitly, so that the verdict does not depend on whether the caller
# set pipefail.
set +e
"${no_aslr[@]}" ctest --test-dir "$dir" -R "$regex" --no-tests=error --output-on-failure \
    --output-junit "$dir/test-results/ctest-tsan.xml" 2>&1 | tee "$log"
ctest_status=${PIPESTATUS[0]}
set -e
[[ "$ctest_status" -eq 0 ]] || status=1

if [[ "$status" -ne 0 ]]; then
    if grep -qs 'unexpected memory mapping' "$dir/Testing/Temporary/LastTest.log"; then
        echo "tsan: the runtime does not accept this kernel's address-space layout; $layout_fix"
    elif [[ ${#no_aslr[@]} -eq 0 ]] && grep -qs '\*\*\*Exception' "$log"; then
        echo "tsan: a test died from a signal with address-space randomisation on" \
            "(vm.mmap_rnd_bits = $rnd_bits); the runtime fails this way at random on a kernel" \
            "that randomises mmap with more than 28 bits; if this one does, $layout_fix"
    fi
fi
exit "$status"
