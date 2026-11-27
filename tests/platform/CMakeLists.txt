# Self-tests of the POSIX platform layer: small FreeRTOS programs run by CTest.
# Labels: "platform" for all of them, "timing" for those that can miss a timing tolerance on a
# host that does not run them well. Such a test exits with status 75 and only that status is
# repeated, once, by run_selftest.sh; any other failure counts at once.

# plat_selftest reports the build id of the tree it was built from in every JSON report
# (fcn_build_id, platform/posix/build_id.h), as the node and the bootloader do.
add_executable(plat_selftest plat_selftest.c)
target_include_directories(plat_selftest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(plat_selftest PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(plat_selftest PRIVATE fcn_platform_posix fcn_build_id)
fcn_target_defaults(plat_selftest NO_COVERAGE)

set(_fcn_sanitizer_env
    "ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1:detect_leaks=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
set(_fcn_run_selftest "${CMAKE_CURRENT_SOURCE_DIR}/run_selftest.sh")

# The self-tests of the hardware watchdog, the retained cell across resets and the sensor's
# hang under the scheduler (wdg_selftest.c), a program of their own. map_keeper.c is the
# process that keeps a child's mapping of a file alive past the child's end, for this program,
# flash_selftest and the two Unity tests of the files' locks.
add_executable(wdg_selftest wdg_selftest.c map_keeper.c)
target_compile_definitions(wdg_selftest PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(wdg_selftest PRIVATE fcn_platform_posix)
fcn_target_defaults(wdg_selftest NO_COVERAGE)

# The self-tests of the host's flash that need real time or the scheduler (flash_selftest.c).
add_executable(flash_selftest flash_selftest.c flash_script.c map_keeper.c)
target_include_directories(flash_selftest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(flash_selftest PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(flash_selftest PRIVATE fcn_platform_posix)
# Every _exit() and every flock() of the program and of the platform goes through
# flash_selftest.c's own, where end-while-writing holds the end of a run open and sees the
# order of its release.
target_link_options(flash_selftest PRIVATE -Wl,--wrap=_exit -Wl,--wrap=flock)
fcn_target_defaults(flash_selftest NO_COVERAGE)

# fcn_add_platform_test(<name> STATUS <exit status> LABELS <labels...>
#                       [STDERR <regex>] [FILE <path> <regex>] [TIMEOUT <seconds>]
#                       [PROGRAM <target>] ARGS <selftest arguments...>)
# STDERR and FILE: a line of stderr, or of the file, has to match the extended regex.
# A test labelled "timing" may be repeated once if it exits with status 75.
# PROGRAM: the self-test program to run, plat_selftest by default.
function(fcn_add_platform_test name)
    cmake_parse_arguments(ARG "" "STATUS;STDERR;TIMEOUT;PROGRAM" "LABELS;FILE;ARGS" ${ARGN})
    if(NOT DEFINED ARG_PROGRAM)
        set(ARG_PROGRAM plat_selftest)
    endif()
    set(_options "")
    if("timing" IN_LIST ARG_LABELS)
        list(APPEND _options --retry-timing)
    endif()
    if(DEFINED ARG_STDERR)
        list(APPEND _options --stderr "${ARG_STDERR}")
    endif()
    if(DEFINED ARG_FILE)
        list(APPEND _options --file ${ARG_FILE})
    endif()
    if(NOT DEFINED ARG_TIMEOUT)
        set(ARG_TIMEOUT 60)
    endif()
    add_test(NAME platform_${name}
             COMMAND sh "${_fcn_run_selftest}" ${_options} ${ARG_STATUS} --
                     "$<TARGET_FILE:${ARG_PROGRAM}>" ${ARG_ARGS})
    set_tests_properties(platform_${name} PROPERTIES
        LABELS "${ARG_LABELS}"
        TIMEOUT ${ARG_TIMEOUT}
        ENVIRONMENT "${_fcn_sanitizer_env}")
endfunction()

# ---------------------------------------------------------------- host I/O without a kernel
# Unity tests of the parts of the host layer that do not need FreeRTOS.
#   test_can_backend   the UDP datagram format, the UDP bus between participants, and
#                      SocketCAN on vcan0 (reported as "ignored" where the machine has no
#                      vcan0, as a failure where FCN_REQUIRE_SOCKETCAN is set, as in CI)
#   test_posix_sim     the simulation control protocol
#   test_posix_sensor  the sensor front-end model and its "sensor." commands, also served
#                      from a thread of their own while the test reads (run under TSan too),
#                      and the hang of "sensor.hang" with a reader thread of its own
#   test_posix_retained the retained-RAM cell: starts and resets played by the test, and its
#                      lock against other processes (with a keeper, map_keeper.h)
#   test_posix_can_trace the ring and the text of the CAN controller's bus-off trace, and the
#                      cost of a record (printed, not judged)
#   test_posix_tick_plan the locked tick's decisions (posix_tick_plan.h) against a model of the
#                      tick thread and the kernel: lost, late and given-back ticks, stalls,
#                      random orders of events (fixed seeds), independent of the host's timing
#                      (docs/bug-log.md, entry 011)
find_package(Threads REQUIRED)
foreach(_test IN ITEMS can_backend posix_sim posix_sensor posix_retained posix_can_trace
                       posix_tick_plan)
    add_executable(test_${_test} test_${_test}.c)
    target_compile_definitions(test_${_test} PRIVATE _POSIX_C_SOURCE=200809L)
    target_link_libraries(test_${_test} PRIVATE fcn_host_io unity Threads::Threads)
    fcn_target_defaults(test_${_test} NO_COVERAGE)
    add_test(NAME platform_${_test} COMMAND test_${_test})
    set_tests_properties(platform_${_test} PROPERTIES
        LABELS "platform" TIMEOUT 60 ENVIRONMENT "${_fcn_sanitizer_env}")
endforeach()
target_sources(test_posix_retained PRIVATE map_keeper.c)

# The flash of the host (posix_flash.c) without a kernel: the file, the device, the simulation
# commands, cuts that end a child process with the model's tear, and the commands served from
# a thread of their own while operations run (run under TSan too). flash_script.c is the
# sequence of operations it shares with flash_selftest below; map_keeper.c the process that
# keeps a mapping of the file alive for the tests of the lock (docs/bug-log.md, entry 012).
add_executable(test_posix_flash test_posix_flash.c flash_script.c map_keeper.c)
target_compile_definitions(test_posix_flash PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(test_posix_flash PRIVATE fcn_host_io unity Threads::Threads)
fcn_target_defaults(test_posix_flash NO_COVERAGE)
add_test(NAME platform_posix_flash COMMAND test_posix_flash)
set_tests_properties(platform_posix_flash PROPERTIES
    LABELS "platform" TIMEOUT 120 ENVIRONMENT "${_fcn_sanitizer_env}")

# ------------------------------------------------- the CAN controller without a scheduler
# The test plays the driver: it calls plat_can.h itself and polls, against the real hardware
# thread on a UDP bus of its own, with faults applied through simulation control.
add_executable(test_posix_can test_posix_can.c)
target_compile_definitions(test_posix_can PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(test_posix_can PRIVATE fcn_platform_posix unity)
fcn_target_defaults(test_posix_can NO_COVERAGE)
add_test(NAME platform_posix_can COMMAND test_posix_can)
set_tests_properties(platform_posix_can PROPERTIES
    LABELS "platform" TIMEOUT 60 ENVIRONMENT "${_fcn_sanitizer_env}")

# ---------------------------------------------------------------- self-tests under FreeRTOS
set(_dir "${CMAKE_CURRENT_BINARY_DIR}")

# The tick: locked mode keeps time and drops few ticks; stock mode leaves the tick alone.
fcn_add_platform_test(tick_locked STATUS 0 LABELS platform timing ARGS tick locked 3)
fcn_add_platform_test(tick_stock STATUS 0 LABELS platform ARGS tick stock 1)
fcn_add_platform_test(tick_suspended_scheduler STATUS 0 LABELS platform timing ARGS suspend 20)
# A tick pending on a thread that was switched out, made up and then taken late, is given back
# at the next boundary: never two ticks early in a row (docs/bug-log.md, entry 011). Timing:
# whether the late tick is made up at all depends on when the host delivers the tick signal,
# and a run that provoked too few is status 75.
fcn_add_platform_test(tick_late STATUS 0 LABELS platform timing ARGS tick-late 50)

# Simulated interrupts from a free-running source, with the load task. "signal-only" switches
# the tick hook's safety net off: the signal path has to serve every interrupt alone. Period 0
# is an interrupt storm: the line is raised in a tight loop. In poll mode the median latency
# has to stay below 0.88 tick, a bound a slow host can miss (status 75): hence "timing".
fcn_add_platform_test(irq_signal STATUS 0 LABELS platform ARGS irq signal 250 3 1)
fcn_add_platform_test(irq_signal_only STATUS 0 LABELS platform ARGS irq signal-only 250 3 1)
fcn_add_platform_test(irq_poll STATUS 0 LABELS platform timing ARGS irq poll 250 3 1)
fcn_add_platform_test(irq_storm_signal STATUS 0 LABELS platform ARGS irq signal 0 2 1)
fcn_add_platform_test(irq_storm_signal_only STATUS 0 LABELS platform ARGS irq signal-only 0 2 1)
fcn_add_platform_test(irq_storm_poll STATUS 0 LABELS platform ARGS irq poll 0 2 1)

# Ping-pong: no interrupt lost, and with signals the task runs within the tick of its ISR.
fcn_add_platform_test(irq_rounds_signal STATUS 0 LABELS platform timing
                      ARGS irq-rounds signal 3000)
fcn_add_platform_test(irq_rounds_signal_only STATUS 0 LABELS platform timing
                      ARGS irq-rounds signal-only 3000)
fcn_add_platform_test(irq_rounds_poll STATUS 0 LABELS platform timing ARGS irq-rounds poll 3000)

# Several lines with their own contexts, singly and together; empty and invalid lines.
fcn_add_platform_test(irq_lines_signal STATUS 0 LABELS platform ARGS irq-lines signal)
fcn_add_platform_test(irq_lines_poll STATUS 0 LABELS platform ARGS irq-lines poll)

fcn_add_platform_test(stack STATUS 0 LABELS platform ARGS stack)
# Both log tests arm the shutdown guard: producers stuck in the log sink end the run with
# status 70 about 5 s later. A ring damaged by two producers at once can also hang the logger
# thread, which runs the guard; then the TIMEOUT ends the test (30 s, 60 s by default).
fcn_add_platform_test(log STATUS 0 LABELS platform ARGS log "${_dir}/selftest_log.txt")
fcn_add_platform_test(log_preempt STATUS 0 LABELS platform TIMEOUT 30
                      ARGS log-preempt "${_dir}/selftest_log_preempt.txt")

# Fatal paths: the documented exit status and the message on stderr.
fcn_add_platform_test(assert STATUS 20 LABELS platform
                      STDERR "^FATAL assertion failed: .*plat_selftest\\.c [0-9]+$" ARGS assert)
fcn_add_platform_test(malloc_failed STATUS 22 LABELS platform
                      STDERR "^FATAL pvPortMalloc failed: free heap bytes [0-9]+$"
                      ARGS malloc-fail)
fcn_add_platform_test(stack_overflow STATUS 21 LABELS platform
                      STDERR "^FATAL stack overflow in task: ovf 0$" ARGS stack-overflow)
fcn_add_platform_test(reset STATUS 10 LABELS platform ARGS reset)
# The line logged right before plat_reset() reaches the file: the reset flushes the log.
fcn_add_platform_test(reset_log STATUS 10 LABELS platform
                      FILE "${_dir}/selftest_reset_log.txt" " I reset: the last line before the reset$"
                      ARGS reset-log "${_dir}/selftest_reset_log.txt")
# A scheduler that never stops is ended by the shutdown guard, armed for 300 ms (the run takes
# about 0.3 s). The TIMEOUT fails a guard that never fires and one that falls back to the
# default POSIX_SHUTDOWN_TIMEOUT_MS of 5 s; a guard that is late by less than about 2.5 s is
# not caught.
fcn_add_platform_test(shutdown_guard STATUS 70 LABELS platform TIMEOUT 3
                      STDERR "^FATAL the scheduler did not stop in time$" ARGS guard)

# The hardware watchdog. Not kicked after six kicks, it resets with status 11 (the self-test
# exits 0 when that came no earlier than the timeout after the last kick and at most 50 ms
# later; 75 if later); kicked every quarter timeout for 3 s, it lets the run end orderly.
fcn_add_platform_test(wdg_expire STATUS 0 LABELS platform timing PROGRAM wdg_selftest
                      STDERR "^FATAL watchdog: not kicked for [0-9]+ ms \\(timeout 200 ms\\)$"
                      ARGS expire 200)
fcn_add_platform_test(wdg_kicked STATUS 0 LABELS platform timing PROGRAM wdg_selftest
                      ARGS kicked 200 3)
fcn_add_platform_test(wdg_start_rules STATUS 0 LABELS platform PROGRAM wdg_selftest
                      ARGS start-rules)
fcn_add_platform_test(wdg_kick_cost STATUS 0 LABELS platform PROGRAM wdg_selftest
                      ARGS kick-cost 100000)
# The retained cell across real resets of a FreeRTOS program, a watchdog reset among them.
# Every child leaves a keeper of its mapping of the cell behind (map_keeper.h), so each next
# start, which follows at once, opens the cell only if the run before it released the cell's
# lock itself, at its reset or at its close (docs/bug-log.md, entry 012).
fcn_add_platform_test(retained_resets STATUS 0 LABELS platform PROGRAM wdg_selftest
                      ARGS retained "${_dir}/selftest_retained_cell.bin")
fcn_add_platform_test(retained_watchdog STATUS 0 LABELS platform PROGRAM wdg_selftest
                      STDERR "^FATAL watchdog: not kicked for [0-9]+ ms \\(timeout 100 ms\\)$"
                      ARGS watchdog-cause 100 "${_dir}/selftest_retained_wdg.bin")
# sensor.hang under the scheduler: "on" blocks the reading task, "spin" starves the ones below.
fcn_add_platform_test(sensor_hang_on STATUS 0 LABELS platform PROGRAM wdg_selftest
                      ARGS hang on)
fcn_add_platform_test(sensor_hang_spin STATUS 0 LABELS platform PROGRAM wdg_selftest
                      ARGS hang spin)

# The flash. Each program and erase takes its modelled time (0.4 ms, 45 ms) and at most 20 ms
# more (else status 75), without a kernel and under the scheduler with the node's wait, which
# blocks the flash task (a task below it runs during every erase).
fcn_add_platform_test(flash_waits STATUS 0 LABELS platform timing PROGRAM flash_selftest
                      ARGS waits "${_dir}/selftest_flash_waits.bin")
fcn_add_platform_test(flash_task_wait STATUS 0 LABELS platform timing PROGRAM flash_selftest
                      ARGS task-wait "${_dir}/selftest_flash_task_wait.bin")
# A cut under the scheduler ends the child through plat_reset() with status 12 and the model's
# tear: the first, a middle and the last write of the script, and one past its end. A keeper of
# the child's mappings outlives it, and the parent opens the cell and the flash at once: both
# locks were released by the child itself (entry 012).
foreach(_cut IN ITEMS 1:5 6:77 10:4242 11:1)
    string(REPLACE ":" "_" _name "${_cut}")
    fcn_add_platform_test(flash_task_cut_${_name} STATUS 0 LABELS platform PROGRAM flash_selftest
                          ARGS task-cut "${_dir}/selftest_flash_cut_${_name}.bin"
                          "${_dir}/selftest_flash_cut_${_name}.cell" ${_cut})
endforeach()
# A run ended from outside the scheduler while a task programs the flash, by the hardware
# watchdog's thread (status 11) and by the shutdown guard (70): the lock is removed only once
# the mapping of the file is private, nothing the task programs after that reaches the file,
# and the parent opens the cell and the flash at once while a keeper still has the child's
# mappings (entry 012).
foreach(_end IN ITEMS watchdog guard)
    fcn_add_platform_test(flash_end_while_writing_${_end} STATUS 0 LABELS platform
                          PROGRAM flash_selftest
                          ARGS end-while-writing ${_end} "${_dir}/selftest_flash_end_${_end}.bin"
                          "${_dir}/selftest_flash_end_${_end}.cell")
endforeach()

# An orderly stop on SIGTERM must leave through main() with status 0.
add_test(NAME platform_sigterm
         COMMAND timeout --preserve-status -s TERM 1 $<TARGET_FILE:plat_selftest> run)
set_tests_properties(platform_sigterm PROPERTIES
    LABELS "platform" TIMEOUT 30 ENVIRONMENT "${_fcn_sanitizer_env}")
