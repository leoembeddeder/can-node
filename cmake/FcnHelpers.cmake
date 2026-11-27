# Helpers that give every project target the same language settings and warning set.

set(FCN_WARNING_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wold-style-definition
    -Wcast-qual
    -Wformat=2
    -Wundef
    -Wdouble-promotion
    -Wvla
    -Wwrite-strings
    -Wimplicit-fallthrough
    -Wnull-dereference)

if(FCN_WERROR)
    list(APPEND FCN_WARNING_FLAGS -Werror)
endif()

# Apply the project's defaults to a target that holds project code.
#   fcn_target_defaults(<target> [NO_COVERAGE])
function(fcn_target_defaults target)
    cmake_parse_arguments(ARG "NO_COVERAGE" "" "" ${ARGN})
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE ${FCN_WARNING_FLAGS})
    if(FCN_COVERAGE AND NOT ARG_NO_COVERAGE)
        # -fprofile-abs-path keeps source paths resolvable when gcovr runs from another directory.
        # -fprofile-update=atomic: code that several threads run at once (the watchdog manager's
        # checkpoint in test_wdgm) would otherwise lose counter updates, and gcov then reports
        # branches taken a negative number of times.
        target_compile_options(${target} PRIVATE --coverage -fprofile-abs-path
                                                 -fprofile-update=atomic)
        target_link_options(${target} PUBLIC --coverage)
        target_compile_definitions(${target} PRIVATE FCN_COVERAGE=1)
    endif()
endfunction()

# A portable module under lib/: a static library named fcn_<name>.
#   fcn_add_module(<name> SOURCES <files...> [DEPENDS <modules...>])
# Headers are included as "<name>/<header>.h", relative to lib/.
function(fcn_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
    add_library(fcn_${name} STATIC ${ARG_SOURCES})
    target_include_directories(fcn_${name} PUBLIC "${CMAKE_SOURCE_DIR}/lib")
    foreach(dep IN LISTS ARG_DEPENDS)
        target_link_libraries(fcn_${name} PUBLIC fcn_${dep})
    endforeach()
    fcn_target_defaults(fcn_${name})
endfunction()

# A Unity test executable registered with CTest under the label "unit".
#   fcn_add_unit_test(<name> SOURCES <files...> [LIBS <targets...>])
function(fcn_add_unit_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE unity ${ARG_LIBS})
    fcn_target_defaults(${name} NO_COVERAGE)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        LABELS "unit"
        TIMEOUT 60
        ENVIRONMENT
        "ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1:detect_leaks=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endfunction()
