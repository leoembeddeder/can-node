# Unity unit tests for the portable modules under lib/. One executable per module.
#
# Each test file carries its own main() with an explicit RUN_TEST list. The test functions are
# static, so a test that is written but not listed fails the build (-Wunused-function).
find_package(Threads REQUIRED)

fcn_add_unit_test(test_spsc_ring SOURCES test_spsc_ring.c LIBS fcn_ring Threads::Threads)
fcn_add_unit_test(test_log SOURCES test_log.c LIBS fcn_log)
fcn_add_unit_test(test_crc SOURCES test_crc.c LIBS fcn_crc)
fcn_add_unit_test(test_can_frame SOURCES test_can_frame.c LIBS fcn_can)
fcn_add_unit_test(test_can_bits SOURCES test_can_bits.c LIBS fcn_can)
fcn_add_unit_test(test_can_fc SOURCES test_can_fc.c LIBS fcn_can)
fcn_add_unit_test(test_can_ctrl SOURCES test_can_ctrl.c LIBS fcn_can)
fcn_add_unit_test(test_can_txq SOURCES test_can_txq.c LIBS fcn_can)
# The codec generated from the DBC (app/gen), reached generically through the generated table
# of tests/tools/can_db_table.h.
fcn_add_unit_test(test_can_db SOURCES test_can_db.c LIBS fcn_can_db_table)
fcn_add_unit_test(test_e2e_p11 SOURCES test_e2e_p11.c LIBS fcn_e2e)
fcn_add_unit_test(test_e2e_sm SOURCES test_e2e_sm.c LIBS fcn_e2e)
fcn_add_unit_test(test_bms SOURCES test_bms.c LIBS fcn_bms)
fcn_add_unit_test(test_dtc SOURCES test_dtc.c LIBS fcn_dtc)
# The watchdog manager; one test reports checkpoints from several threads (also run under
# ThreadSanitizer by scripts/test-tsan.sh, which selects unit tests that create threads).
fcn_add_unit_test(test_wdgm SOURCES test_wdgm.c LIBS fcn_wdgm Threads::Threads)
# The flash interface on the NOR model, power cuts included.
fcn_add_unit_test(test_flash SOURCES test_flash.c LIBS fcn_flash)
fcn_add_unit_test(test_sha256 SOURCES test_sha256.c LIBS fcn_sha256)
# The image format and the validation of a slot, over a flash device in the test's RAM.
fcn_add_unit_test(test_image SOURCES test_image.c LIBS fcn_image)
# The parts of the application that need no kernel (app/signals.c, app/telemetry.c,
# app/command.c, app/fault.c, app/update.c).
fcn_add_unit_test(test_signals SOURCES test_signals.c LIBS fcn_app_logic)
fcn_add_unit_test(test_telemetry SOURCES test_telemetry.c LIBS fcn_app_logic)
fcn_add_unit_test(test_command SOURCES test_command.c LIBS fcn_app_logic)
fcn_add_unit_test(test_fault SOURCES test_fault.c LIBS fcn_app_logic)
fcn_add_unit_test(test_update SOURCES test_update.c LIBS fcn_app_logic)
# Phase 5's parts of the application that need no kernel: the diagnostic connection and its UDS
# backend (app/diag.c, app/diag_backend.c) on lib/isotp and lib/uds with a fake node, and the
# flash task's jobs (app/flash_job.c) on lib/boot's slot writer over lib/flash's NOR model with
# lib/bootimg as the validator.
fcn_add_unit_test(test_diag SOURCES test_diag.c LIBS fcn_app_logic)
fcn_add_unit_test(test_flash_job SOURCES test_flash_job.c
                  LIBS fcn_app_logic fcn_bootimg fcn_image fcn_sha256 fcn_flash)
# lib/image's validation as lib/boot's validator, over a RAM flash and lib/flash's model.
fcn_add_unit_test(test_bootimg SOURCES test_bootimg.c LIBS fcn_bootimg fcn_flash)
# Boot control and the slot writer (lib/boot), on the tests' own NOR model (boot_test_nor.c,
# behind boot_test_flash.h). test_boot_cuts.c is the power-cut campaign, in three parts that are
# three tests (BOOT_CUTS_PART): the host geometry with an empty log, the host geometry with the
# log filled first, and two other geometries.
set(FCN_BOOT_TEST_SUPPORT boot_test_support.c boot_test_nor.c)
fcn_add_unit_test(test_boot SOURCES test_boot.c ${FCN_BOOT_TEST_SUPPORT} LIBS fcn_boot)
fcn_add_unit_test(test_boot_cuts_host SOURCES test_boot_cuts.c ${FCN_BOOT_TEST_SUPPORT}
                  LIBS fcn_boot)
target_compile_definitions(test_boot_cuts_host PRIVATE BOOT_CUTS_PART=0)
fcn_add_unit_test(test_boot_cuts_host_log SOURCES test_boot_cuts.c ${FCN_BOOT_TEST_SUPPORT}
                  LIBS fcn_boot)
target_compile_definitions(test_boot_cuts_host_log PRIVATE BOOT_CUTS_PART=1)
fcn_add_unit_test(test_boot_cuts_small SOURCES test_boot_cuts.c ${FCN_BOOT_TEST_SUPPORT}
                  LIBS fcn_boot)
target_compile_definitions(test_boot_cuts_small PRIVATE BOOT_CUTS_PART=2)
# The same tests on lib/flash's NOR model (flash/nor.h, through boot_test_flash_lib.c), which
# tears with its own seeded model: lib/boot is shown safe on both models, not only on the one
# written for its tests.
set(FCN_BOOT_TEST_SUPPORT_LIBFLASH boot_test_support.c boot_test_flash_lib.c)
fcn_add_unit_test(test_boot_libflash SOURCES test_boot.c ${FCN_BOOT_TEST_SUPPORT_LIBFLASH}
                  LIBS fcn_boot fcn_flash)
fcn_add_unit_test(test_boot_cuts_host_libflash SOURCES test_boot_cuts.c
                  ${FCN_BOOT_TEST_SUPPORT_LIBFLASH} LIBS fcn_boot fcn_flash)
target_compile_definitions(test_boot_cuts_host_libflash PRIVATE BOOT_CUTS_PART=0)
fcn_add_unit_test(test_boot_cuts_host_log_libflash SOURCES test_boot_cuts.c
                  ${FCN_BOOT_TEST_SUPPORT_LIBFLASH} LIBS fcn_boot fcn_flash)
target_compile_definitions(test_boot_cuts_host_log_libflash PRIVATE BOOT_CUTS_PART=1)
fcn_add_unit_test(test_boot_cuts_small_libflash SOURCES test_boot_cuts.c
                  ${FCN_BOOT_TEST_SUPPORT_LIBFLASH} LIBS fcn_boot fcn_flash)
target_compile_definitions(test_boot_cuts_small_libflash PRIVATE BOOT_CUTS_PART=2)
# ISO-TP (lib/isotp): one instance against the test as its peer (test_isotp.c: frames, checks,
# the table of unexpected N_PDUs; test_isotp_flow.c: timers, flow control, confirmations,
# callbacks), and two instances over a simulated bus with a wire checker (test_isotp_pair.c:
# every length, full duplex, late confirmations, random faults; in two parts, ISOTP_PAIR_PART,
# so that each stays well inside the timeout in the coverage build). isotp_test_bus.c is the fake
# clock, the endpoints, the bus and the checker.
fcn_add_unit_test(test_isotp SOURCES test_isotp.c isotp_test_bus.c LIBS fcn_isotp)
fcn_add_unit_test(test_isotp_flow SOURCES test_isotp_flow.c isotp_test_bus.c LIBS fcn_isotp)
fcn_add_unit_test(test_isotp_pair_lengths SOURCES test_isotp_pair.c isotp_test_bus.c
                  LIBS fcn_isotp)
target_compile_definitions(test_isotp_pair_lengths PRIVATE ISOTP_PAIR_PART=0)
fcn_add_unit_test(test_isotp_pair SOURCES test_isotp_pair.c isotp_test_bus.c LIBS fcn_isotp)
target_compile_definitions(test_isotp_pair PRIVATE ISOTP_PAIR_PART=1)
# The UDS server (lib/uds) on a fake clock and fake backends (uds_test_support.c): the core,
# the sessions and their timing and the services but the download (test_uds.c); the download
# and its state machine, on the fake backend and on lib/boot's real slot writer over lib/flash's
# NOR model with lib/bootimg as the validator (test_uds_download.c).
fcn_add_unit_test(test_uds SOURCES test_uds.c uds_test_support.c LIBS fcn_uds fcn_dtc)
fcn_add_unit_test(test_uds_download SOURCES test_uds_download.c uds_test_support.c
                  LIBS fcn_uds fcn_boot fcn_bootimg fcn_flash fcn_image fcn_sha256)
