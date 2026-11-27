# The FreeRTOS application: portable code that uses only the FreeRTOS API, lib/ and the
# platform interface in platform/include. The executable that puts it on a platform is defined
# by that platform (platform/posix).
#
# gen/ holds the codec generated from the DBC, a library of its own (fcn_can_db) so that the
# unit tests can link it without the kernel.
add_subdirectory(gen)

# The part of the application that needs no kernel: the conversion to the raw values of the
# DBC (signals.c), the frames the node sends (telemetry.c), the receiver of TESTER_Command
# (command.c), the fault monitor (fault.c), the update's hand-off and confirm (update.c), and
# phase 5's diagnostic connection (diag.c), its UDS backend (diag_backend.c) and the flash task's
# jobs (flash_job.c). A library of its own, so that the unit tests link it without the kernel.
add_library(fcn_app_logic STATIC
    command.c
    diag.c
    diag_backend.c
    fault.c
    flash_job.c
    signals.c
    telemetry.c
    update.c)
target_include_directories(fcn_app_logic PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/platform/include")
target_link_libraries(fcn_app_logic PUBLIC fcn_can fcn_can_db fcn_e2e fcn_bms fcn_dtc fcn_wdgm
                                            fcn_boot fcn_flash fcn_isotp fcn_uds)
fcn_target_defaults(fcn_app_logic)

add_library(fcn_app STATIC
    app.c
    can_io.c
    node_stats.c
    task_table.c)
target_include_directories(fcn_app PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/platform/include")
# The version BMS_NodeInfo carries.
target_compile_definitions(fcn_app PRIVATE
    FCN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    FCN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    FCN_VERSION_PATCH=${PROJECT_VERSION_PATCH})
target_link_libraries(fcn_app PUBLIC freertos_kernel fcn_app_logic fcn_can fcn_can_db fcn_log
    fcn_wdgm fcn_bootimg)
fcn_target_defaults(fcn_app)
