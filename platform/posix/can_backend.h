/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The transport under the CAN controller model: where frames physically go on the host.
 *
 * Two backends implement the same small interface:
 *
 *  - "socketcan:<interface>", for example socketcan:vcan0: a raw CAN socket on a Linux
 *    SocketCAN interface. This is what CI uses, and what can-utils and python-can speak.
 *  - "udp:<base port>", for example udp:29500: a virtual bus over UDP on the loopback
 *    interface, for machines whose kernel has no vcan (the stock WSL2 kernel). The same
 *    protocol is implemented for python-can in tools/fcn_tools/udpbus.py.
 *
 * Both deliver every frame to every other participant, immediately. Bit timing, bus load and
 * bus faults are not the backend's business; the controller model adds them
 * (lib/can/can_ctrl.h). A failure of the transport itself (an interface that disappears, a
 * send buffer that is full) is reported by send() and recv(), counted with its errno, and left
 * to the caller.
 *
 * Both accept only well-formed classical CAN frames: input with a wrong size, the error flag
 * set, a length above 8, or a base-format identifier above 0x7FF is dropped and counted
 * (rx_malformed), never passed on, so that a frame means the same on either transport.
 *
 * The UDP bus. A bus is 16 UDP ports on 127.0.0.1: base .. base+15. A participant binds the
 * first of them that is free and sends each frame as one datagram to each of the other 15.
 * There is no broker and nothing leaves the machine. A datagram is 16 bytes laid out like
 * Linux's struct can_frame, little-endian:
 *
 *     0..3   can_id   bits 0..28 identifier, bit 29 error frame (never sent),
 *                     bit 30 remote request, bit 31 extended identifier
 *     4      length   0..8
 *     5..7   zero
 *     8..15  data     unused bytes zero
 *
 * A receiver drops, and counts, any datagram that is not exactly that: wrong size, error
 * flag set, length above 8, identifier too large for its format, non-zero padding, or a
 * source that is not another slot of the same bus.
 *
 * A backend is used by one thread only (the controller thread). All calls are non-blocking.
 */
#ifndef FCN_CAN_BACKEND_H
#define FCN_CAN_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can/can_frame.h"

#define CAN_UDP_SLOTS        16U
#define CAN_UDP_DATAGRAM_LEN 16U
#define CAN_UDP_DEFAULT_PORT 29500U

typedef struct can_backend can_backend_t;

struct can_backend {
    const char *kind; /**< "socketcan" or "udp" */

    /** Hand one frame to the transport. Returns false if it could not be sent. */
    bool (*send)(can_backend_t *self, const can_frame_t *frame);

    /** Read one frame. Returns 1 with a frame, 0 if nothing is pending, -1 on a socket error.
     *  Malformed input is consumed, counted and skipped, never returned. */
    int (*recv)(can_backend_t *self, can_frame_t *frame);

    void (*close)(can_backend_t *self);

    /* Counters, readable by the owner at any time. */
    uint64_t tx_frames;
    uint64_t tx_failed; /**< send() calls the transport refused */
    uint64_t rx_frames;
    uint64_t rx_failed;    /**< recv() calls that returned -1 (a socket error) */
    uint64_t rx_malformed; /**< input dropped by validation */
    uint64_t rx_overflow;  /**< packets the kernel dropped because the socket buffer was full;
                                as of the last can_backend_refresh_counters() or close() */
    int last_error;        /**< errno of the last failed send() or recv(); 0 if none failed */

    int fd; /**< for poll(); -1 when closed */

    /* Private to the implementation. */
    uint16_t udp_base_port;
    uint16_t udp_own_port;
};

typedef enum { CAN_BACKEND_UDP = 0, CAN_BACKEND_SOCKETCAN = 1 } can_backend_kind_t;

/** A transport specification taken apart. */
typedef struct {
    can_backend_kind_t kind;
    uint16_t udp_base_port; /**< UDP: the bus's first port */
    const char *interface;  /**< SocketCAN: points into the specification string */
} can_backend_spec_t;

/**
 * Take apart a transport specification: "udp" (the default port), "udp:<base port>" or
 * "socketcan:<interface>". The port is a decimal number from 1 to 65535 with nothing before or
 * after it: no sign, no spaces. (Whether a bus fits above the port is checked when it is
 * opened.) On failure returns false and writes a message for the user into `error`.
 */
bool can_backend_parse_spec(const char *spec, can_backend_spec_t *out, char *error,
                            size_t error_len);

/**
 * Open the backend named by `spec` (see can_backend_parse_spec). On failure returns false and
 * writes a message for the user into `error`.
 */
bool can_backend_open(can_backend_t *backend, const char *spec, char *error, size_t error_len);

/** Bring rx_overflow up to date with the kernel's count for the backend's socket. close()
 *  does this as well, so the counter is final once a backend is closed. */
void can_backend_refresh_counters(can_backend_t *backend);

/* The two implementations; can_backend_open() dispatches to them. */
bool can_backend_udp_open(can_backend_t *backend, uint16_t base_port, char *error,
                          size_t error_len);
bool can_backend_socketcan_open(can_backend_t *backend, const char *interface, char *error,
                                size_t error_len);

/* The UDP datagram format, exposed for the unit test of the encoding. */
void can_udp_encode(const can_frame_t *frame, uint8_t datagram[CAN_UDP_DATAGRAM_LEN]);
bool can_udp_decode(const uint8_t *datagram, size_t len, can_frame_t *frame);

#endif /* FCN_CAN_BACKEND_H */
