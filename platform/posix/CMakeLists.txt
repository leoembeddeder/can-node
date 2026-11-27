# Host simulation layer under the FreeRTOS POSIX port.
find_package(Threads REQUIRED)

# The parts of the host layer that know nothing about FreeRTOS: the CAN transports (SocketCAN,
# UDP), the simulation control socket, the model of the sensor front end (plat_sensor.h), the
# retained-RAM cell (plat_retained.h), the flash (plat_flash.h), the ring of the CAN
# controller's bus-off trace (posix_can_trace.h) and the decisions of the locked tick
# (posix_tick_plan.h: pure functions, which posix_tick.c carries out). A library of their own,
# so that their tests, the bootloader and the tools link without a kernel. The host-only trace
# (posix_trace.h) is here as well: it records for the models and for the kernel's hooks alike,
# and the objects of fcn_platform_posix, which every program with a kernel links whole, always
# refer to it, so the kernel's references to its hooks are always resolved.
add_library(fcn_host_io STATIC
    can_backend.c
    can_backend_socketcan.c
    can_backend_udp.c
    posix_can_trace.c
    posix_flash.c
    posix_retained.c
    posix_sensor.c
    posix_sim.c
    posix_tick_plan.c
    posix_trace.c)
target_include_directories(fcn_host_io PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/platform/include")
# struct ifreq and the SO_RXQ_OVFL socket option.
target_compile_definitions(fcn_host_io PRIVATE _GNU_SOURCE)
target_link_libraries(fcn_host_io PUBLIC fcn_can fcn_bms fcn_crc fcn_flash)
fcn_target_defaults(fcn_host_io)

# An OBJECT library on purpose: the kernel calls back into this code (hooks, the assert
# handler, the wrapped usleep), and with a static archive whether those definitions get linked
# would depend on the order of the archives on the link line. Objects are always linked whole.
add_library(fcn_platform_posix OBJECT
    posix_can.c
    posix_clock.c
    posix_hooks.c
    posix_irq.c
    posix_lifecycle.c
    posix_log.c
    posix_stack.c
    posix_tick.c
    posix_wdg.c)

target_include_directories(fcn_platform_posix PUBLIC
    "${CMAKE_SOURCE_DIR}/platform/include"
    "${CMAKE_CURRENT_SOURCE_DIR}")

# pthread_getattr_np, pthread_setattr_default_np, pthread_setname_np, clock_nanosleep, ppoll.
target_compile_definitions(fcn_platform_posix PRIVATE _GNU_SOURCE)

target_link_libraries(fcn_platform_posix PUBLIC
    freertos_kernel fcn_host_io fcn_can fcn_ring fcn_log Threads::Threads)

# Two calls of the port are intercepted at link time: the tick thread's sleep (posix_tick.c)
# and the signal mask changes (posix_irq.c). Each file says why.
target_link_options(fcn_platform_posix INTERFACE -Wl,--wrap=usleep -Wl,--wrap=pthread_sigmask)

fcn_target_defaults(fcn_platform_posix)

# The build id of the source tree (build_id.h: the commit and, for a tree with uncommitted
# changes, a hash of those changes), for the host programs that report it: the node, the
# bootloader, the flash tool and plat_selftest. It is written into a C file on every build, not
# when CMake configures, so that a binary built after a commit or an edit carries it
# (cmake/FcnBuildId.cmake); the file is only rewritten when the id changed, so an unchanged tree
# does not relink. The generated file lives in the build tree, outside the analysed sources.
set(_fcn_build_id_c "${CMAKE_CURRENT_BINARY_DIR}/fcn_build_id.c")
add_custom_target(fcn_build_id_source
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DOUTPUT=${_fcn_build_id_c}"
            -P "${CMAKE_SOURCE_DIR}/cmake/FcnBuildId.cmake"
    BYPRODUCTS "${_fcn_build_id_c}"
    COMMENT "Build id of the source tree"
    VERBATIM)
add_library(fcn_build_id STATIC "${_fcn_build_id_c}")
add_dependencies(fcn_build_id fcn_build_id_source)
target_include_directories(fcn_build_id PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
fcn_target_defaults(fcn_build_id NO_COVERAGE)

# What the bootloader and the flash tool share (boot_host.h): no kernel, the host's flash and
# retained cell, lib/boot with lib/image's validation behind it, lib/log into a file.
add_library(fcn_boot_host STATIC boot_host.c)
target_include_directories(fcn_boot_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fcn_boot_host PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(fcn_boot_host PUBLIC fcn_host_io fcn_bootimg fcn_boot fcn_image fcn_log)
fcn_target_defaults(fcn_boot_host)

# The bootloader of the host build (boot_main.c): select, hand-off, and the jump by memfd and
# fexecve. memfd_create and fexecve need _GNU_SOURCE. The targets are named after their
# programs with "_program": fcn_boot and fcn_flash are the libraries of lib/boot and lib/flash.
add_executable(fcn_boot_program boot_main.c)
target_compile_definitions(fcn_boot_program PRIVATE _GNU_SOURCE FCN_VERSION="${PROJECT_VERSION}")
target_link_libraries(fcn_boot_program PRIVATE fcn_boot_host fcn_build_id)
set_target_properties(fcn_boot_program PROPERTIES OUTPUT_NAME fcn_boot
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(fcn_boot_program)

# The flash tool (flash_tool.c): factory, stage, info.
add_executable(fcn_flash_program flash_tool.c)
target_compile_definitions(fcn_flash_program PRIVATE _POSIX_C_SOURCE=200809L
                                                     FCN_VERSION="${PROJECT_VERSION}")
target_link_libraries(fcn_flash_program PRIVATE fcn_boot_host fcn_build_id)
set_target_properties(fcn_flash_program PROPERTIES OUTPUT_NAME fcn_flash
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(fcn_flash_program)

# The node as a Linux process: the application on top of this platform layer.
add_executable(fcn_node node_main.c)
target_compile_definitions(fcn_node PRIVATE _POSIX_C_SOURCE=200809L
                                            FCN_VERSION="${PROJECT_VERSION}")
target_link_libraries(fcn_node PRIVATE fcn_app fcn_platform_posix fcn_build_id)
set_target_properties(fcn_node PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(fcn_node)
