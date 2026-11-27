/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The build id of the host programs (fcn_node, fcn_boot, fcn_flash, plat_selftest): the commit
 * of the source tree they were built from, followed by "-dirty-" and a hash of the uncommitted
 * changes when tracked files differed from that commit, or "unknown" without git. It is
 * computed on every build (cmake/FcnBuildId.cmake, the target fcn_build_id of
 * platform/posix/CMakeLists.txt), so it is never older than the binary. It is made of the
 * characters 0-9, a-f and '-' (or is "unknown"), so it needs no escaping in JSON or a log line.
 *
 * Every program reports it: the node in its statistics ("build_id"), in --version and in a log
 * line at its start; fcn_boot in --version and in a line of its log; fcn_flash in --version;
 * plat_selftest in its JSON reports. A measurement (bench/) compares it with the id of the tree
 * its result names (fcn_tools.results.tree_build_id()) and refuses a binary that carries
 * another one. It is host-only: the application (app/) never reads it, and in the node it is
 * one string of read-only data that main() prints before the scheduler starts and after it has
 * stopped.
 */
#ifndef FCN_BUILD_ID_H
#define FCN_BUILD_ID_H

extern const char fcn_build_id[];

#endif /* FCN_BUILD_ID_H */
