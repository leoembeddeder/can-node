/* SPDX-License-Identifier: MIT */
/**
 * @file
 * Simulation control: the test bench's hand on the simulated hardware.
 *
 * On a real bench, faults are applied to the hardware around the device under test: a relay
 * shorts the bus, a cell simulator drives a voltage out of range, the supply is cut. None of
 * that goes through the device's own communication interface, and none of it needs support
 * in the firmware. This channel is the same thing for the simulated hardware: a UDP socket on
 * the loopback interface, owned by the host platform layer, through which a test applies
 * faults to the models (CAN bus, sensors, flash, power) and reads their state. The
 * application has no hook for these faults and never sees these commands.
 *
 * Protocol: one request per datagram, one reply datagram to the sender, both ASCII.
 *
 *     request:  <token> <command> [arguments...]
 *     reply:    <token> ok [key=value ...]        or        <token> err <message>
 *
 * The token is a word of 1 to POSIX_SIM_MAX_TOKEN characters chosen by the client; it comes
 * back unchanged so the client can match replies to requests. A request whose token is longer,
 * or that has no command, cannot be matched and is answered with "? err malformed request".
 * Commands are named <model>.<verb>, for example "can.fault short"; a model registers a
 * handler for its prefix. "ping" is built in.
 *
 * Requests are served by the hardware thread (posix_can.c), in the same thread that runs the
 * models, so a command takes effect at a well-defined point of the simulated timeline.
 */
#ifndef FCN_POSIX_SIM_H
#define FCN_POSIX_SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Longest request or reply, in bytes. A request is cut to this length; a handler's reply has
 *  this less the token and " ok " to fit into. */
#define POSIX_SIM_MAX_DATAGRAM 1024U

/** Longest token, in characters. */
#define POSIX_SIM_MAX_TOKEN 31U

/**
 * Handles the commands of one model. `verb` is the part of the command after the prefix,
 * `args` the rest of the request (possibly empty). On success write the reply's key=value
 * list (possibly empty) to `reply` and return true; on failure write a message and return
 * false. Runs in the hardware thread: it must not call any FreeRTOS function.
 */
typedef bool (*posix_sim_handler_t)(const char *verb, const char *args, char *reply,
                                    size_t reply_len);

/** Open the control socket on 127.0.0.1:port. Port 0 leaves simulation control disabled and
 *  succeeds. */
bool posix_sim_open(uint16_t port, char *error, size_t error_len);

/** True while the control socket is open. */
bool posix_sim_enabled(void);

/** File descriptor to poll for requests. Only meaningful while posix_sim_enabled(). */
int posix_sim_fd(void);

/** Register the handler for commands that start with `prefix` (for example "can."),
 *  replacing the handler the prefix had, if any. The prefix string must outlive the program.
 *  Call before the hardware thread starts. */
void posix_sim_register(const char *prefix, posix_sim_handler_t handler);

/** Serve every request that is waiting. Called by the hardware thread. */
void posix_sim_service(void);

void posix_sim_close(void);

#endif /* FCN_POSIX_SIM_H */
