# Small command-line programs that expose library functions to the Python tests, so that an
# independent Python implementation can be compared with the C code on many inputs.
add_executable(can_bits_cli can_bits_cli.c)
target_link_libraries(can_bits_cli PRIVATE fcn_can)
set_target_properties(can_bits_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(can_bits_cli NO_COVERAGE)

# The generated CAN codec as a table (can_db_table.h; can_db_table.c is generated with the codec
# by tools/gen_can_db.py). Used by can_db_cli and by tests/unit/test_can_db.c.
add_library(fcn_can_db_table STATIC can_db_table.c)
target_include_directories(fcn_can_db_table PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fcn_can_db_table PUBLIC fcn_can_db)
fcn_target_defaults(fcn_can_db_table NO_COVERAGE)

add_executable(can_db_cli can_db_cli.c)
target_link_libraries(can_db_cli PRIVATE fcn_can_db_table)
set_target_properties(can_db_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(can_db_cli NO_COVERAGE)

add_executable(e2e_cli e2e_cli.c)
target_link_libraries(e2e_cli PRIVATE fcn_e2e)
set_target_properties(e2e_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(e2e_cli NO_COVERAGE)

# lib/image: packs images and validates files, for tests/python/test_image.py.
add_executable(image_cli image_cli.c)
target_link_libraries(image_cli PRIVATE fcn_image)
set_target_properties(image_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(image_cli NO_COVERAGE)

# The test payloads (payload.c): stand-ins for broken firmware that fcn_boot starts from an
# image, one that crashes at once (13) and one that ends as a watchdog reset (11).
foreach(kind crash watchdog)
    add_executable(fcn_payload_${kind} payload.c)
    target_link_libraries(fcn_payload_${kind} PRIVATE fcn_boot_host)
    set_target_properties(fcn_payload_${kind} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    fcn_target_defaults(fcn_payload_${kind} NO_COVERAGE)
endforeach()
target_compile_definitions(fcn_payload_crash PRIVATE PAYLOAD_KIND=1 _POSIX_C_SOURCE=200809L)
target_compile_definitions(fcn_payload_watchdog PRIVATE PAYLOAD_KIND=2 _POSIX_C_SOURCE=200809L)

# The held end (exit_held.c): linked into a program, it keeps the program's memory map alive
# after the program has ended, until the test lets it go, when the environment names the test's
# FIFO (FCN_HELD_EXIT); without that the program ends as the real one. An object library, so
# that the real programs' sources below are compiled exactly as for the real programs; every
# program that links it gets -Wl,--wrap=_exit with it.
add_library(fcn_exit_held OBJECT exit_held.c)
target_compile_definitions(fcn_exit_held PRIVATE _GNU_SOURCE)
target_link_options(fcn_exit_held INTERFACE -Wl,--wrap=_exit)
fcn_target_defaults(fcn_exit_held NO_COVERAGE)

# fcn_boot_held: the bootloader (the same boot_main.c as fcn_boot) whose exec keeps its old
# memory map, and with it the open file descriptions of the flash and the cell, alive until the
# image has ended (boot_held.c, by -Wl,--wrap=fexecve), for tests/system/test_boot.py; and
# whose own ends are held ends, for tests/system/test_held_ends.py.
add_executable(fcn_boot_held "${CMAKE_SOURCE_DIR}/platform/posix/boot_main.c" boot_held.c)
target_compile_definitions(fcn_boot_held PRIVATE _GNU_SOURCE FCN_VERSION="${PROJECT_VERSION}")
target_link_libraries(fcn_boot_held PRIVATE fcn_boot_host fcn_build_id fcn_exit_held)
target_link_options(fcn_boot_held PRIVATE -Wl,--wrap=fexecve)
set_target_properties(fcn_boot_held PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(fcn_boot_held NO_COVERAGE)

# The flash tool, the node and the crash payload with held ends, for
# tests/system/test_held_ends.py (docs/bug-log.md, 012): the sources, definitions and libraries
# of fcn_flash, fcn_node and fcn_payload_crash, and the held end.
add_executable(fcn_flash_held "${CMAKE_SOURCE_DIR}/platform/posix/flash_tool.c")
target_compile_definitions(fcn_flash_held PRIVATE _POSIX_C_SOURCE=200809L
                                                  FCN_VERSION="${PROJECT_VERSION}")
target_link_libraries(fcn_flash_held PRIVATE fcn_boot_host fcn_build_id fcn_exit_held)

add_executable(fcn_node_held "${CMAKE_SOURCE_DIR}/platform/posix/node_main.c")
target_compile_definitions(fcn_node_held PRIVATE _POSIX_C_SOURCE=200809L
                                                 FCN_VERSION="${PROJECT_VERSION}")
target_link_libraries(fcn_node_held PRIVATE fcn_app fcn_platform_posix fcn_build_id fcn_exit_held)

add_executable(fcn_payload_crash_held payload.c)
target_compile_definitions(fcn_payload_crash_held PRIVATE PAYLOAD_KIND=1 _POSIX_C_SOURCE=200809L)
target_link_libraries(fcn_payload_crash_held PRIVATE fcn_boot_host fcn_exit_held)

foreach(program fcn_flash_held fcn_node_held fcn_payload_crash_held)
    set_target_properties(${program} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    fcn_target_defaults(${program} NO_COVERAGE)
endforeach()
