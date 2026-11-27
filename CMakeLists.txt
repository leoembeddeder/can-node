# freertos-can-node: FreeRTOS telemetry node on CAN with fail-safe A/B firmware update.
#
# Targets are grouped by how portable they are:
#   lib/             pure C11, no OS, no malloc: unit-tested on the host, reused on the MCU
#   platform/posix/  the host simulation layer under the FreeRTOS POSIX port
#   app/             the FreeRTOS application (tasks and their IPC)
#   tests/           Unity unit tests and platform self-tests, run by CTest
cmake_minimum_required(VERSION 3.25)
project(freertos_can_node VERSION 0.1.0 LANGUAGES C)

# Project targets are strict C11 (set per target in cmake/FcnHelpers.cmake). Third-party code
# keeps the compiler's default dialect: the POSIX port relies on GNU extensions.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FCN_WERROR "Treat warnings in project code as errors" ON)
option(FCN_SANITIZE "Build everything with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(FCN_COVERAGE "Instrument project code (not tests, not third-party) for gcov" OFF)

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/FreeRTOS-Kernel/tasks.c")
    message(FATAL_ERROR "The FreeRTOS-Kernel submodule is missing. Run: git submodule update --init")
endif()

include(cmake/FcnHelpers.cmake)

if(FCN_SANITIZE)
    # Sanitizers instrument every translation unit, the kernel included, so that a bad access
    # made through a kernel object is caught where it happens.
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------- portable modules
add_subdirectory(lib)

# ---------------------------------------------------------------- FreeRTOS kernel (POSIX port)
# The kernel's own CMake consumes a target named freertos_config that carries FreeRTOSConfig.h.
# Both directories hold project code and are ordinary include directories on purpose: marked
# SYSTEM, their headers would be exempt from the warning set and from clang-tidy in every
# project file that includes them. tests/headers compiles each of them on its own as well.
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE
    "${CMAKE_SOURCE_DIR}/config/posix"
    "${CMAKE_SOURCE_DIR}/platform/include") # FreeRTOSConfig.h includes platform.h
set(FREERTOS_PORT GCC_POSIX CACHE STRING "FreeRTOS port")
set(FREERTOS_HEAP 4 CACHE STRING "FreeRTOS heap implementation")
add_subdirectory(third_party/FreeRTOS-Kernel EXCLUDE_FROM_ALL)

# The kernel is third-party code: it is compiled with its own defaults, not with the project's
# warning set, and its headers are seen as system headers by project code.
get_target_property(_fcn_kernel_inc freertos_kernel_include INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(freertos_kernel_include PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_fcn_kernel_inc}")
get_target_property(_fcn_port_inc freertos_kernel_port_headers INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(freertos_kernel_port_headers PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_fcn_port_inc}")

add_subdirectory(app)
add_subdirectory(platform/posix)

# ---------------------------------------------------------------- tests
enable_testing()
add_library(unity STATIC third_party/Unity/src/unity.c)
target_include_directories(unity SYSTEM PUBLIC third_party/Unity/src)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE UNITY_SUPPORT_64)
add_subdirectory(tests/headers)
add_subdirectory(tests/unit)
add_subdirectory(tests/platform)
add_subdirectory(tests/tools)
