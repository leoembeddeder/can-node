# Header checks: every header that project code is written against has to compile on its
# own, with the project's warning set, as the first thing a translation unit includes.
#
# One object file per header proves that. It matters most for the headers that are otherwise
# only ever reached through another one: FreeRTOSConfig.h is included by the kernel's
# FreeRTOS.h, and platform.h by FreeRTOSConfig.h, so in every other translation unit the
# compiler and clang-tidy first meet them from inside a third-party header. Here they are
# project code like any other: compiled with -Werror, and analysed by scripts/static-analysis.sh
# through the compile database.
add_library(fcn_header_check OBJECT
    check_freertos_config_h.c
    check_plat_can_h.c
    check_plat_flash_h.c
    check_plat_retained_h.c
    check_plat_sensor_h.c
    check_plat_wdg_h.c
    check_platform_h.c)
# For the include directories only: an object library is never linked.
target_link_libraries(fcn_header_check PRIVATE freertos_config freertos_kernel fcn_can fcn_bms
                                               fcn_flash)
fcn_target_defaults(fcn_header_check NO_COVERAGE)

# An object library that nothing links is not built by default; this makes it part of "all".
add_custom_target(fcn_header_check_all ALL DEPENDS $<TARGET_OBJECTS:fcn_header_check>)
