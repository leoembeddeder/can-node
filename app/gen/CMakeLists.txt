# The codec of the CAN messages, generated from dbc/fcn.dbc by tools/gen_can_db.py and checked
# in: the build needs neither Python nor cantools. scripts/codegen-check.sh (run by CI and by
# scripts/check.sh) fails when the files here are not what the DBC gives.
#
# It is code of the node like the rest of app/: compiled with the project's warning set and
# checked by the MISRA addon, so the generator writes it to pass both. It uses nothing but the
# C library, so the unit tests link it without the kernel.
add_library(fcn_can_db STATIC can_db.c)
target_include_directories(fcn_can_db PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
fcn_target_defaults(fcn_can_db)
