/* SPDX-License-Identifier: MIT */
/* Chooses the transport from a "kind:argument" specification. */
#include "can_backend.h"

#include <errno.h>
#include <linux/sock_diag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

void can_backend_refresh_counters(can_backend_t *backend)
{
    /* SO_MEMINFO returns the socket's memory counters; one of them is the number of packets
     * the kernel had to drop because the receive buffer was full. It is exact at the moment
     * of the call. (The other way to learn it, SO_RXQ_OVFL, reports it with the next packet
     * that is received, so the drops at the end of a burst stay unseen until more arrives.) */
    uint32_t counters[SK_MEMINFO_VARS];
    socklen_t len = (socklen_t)sizeof(counters);

    if ((backend->fd >= 0) &&
        (getsockopt(backend->fd, SOL_SOCKET, SO_MEMINFO, counters, &len) == 0) &&
        (len >= (socklen_t)((SK_MEMINFO_DROPS + 1U) * sizeof(counters[0])))) {
        backend->rx_overflow = counters[SK_MEMINFO_DROPS];
    }
}

/* A port number: decimal digits only, 1 to 65535. strtoul() alone would also take leading
 * spaces, a sign, and a negative number wrapped around to a large one. */
static bool parse_port(const char *digits, uint16_t *out)
{
    bool ok = false;

    if ((digits[0] >= '0') && (digits[0] <= '9')) {
        char *end = NULL;
        unsigned long port;

        errno = 0;
        port = strtoul(digits, &end, 10);

        if ((errno == 0) && (*end == '\0') && (port > 0UL) && (port <= 65535UL)) {
            *out = (uint16_t)port;
            ok = true;
        }
    }

    return ok;
}

bool can_backend_parse_spec(const char *spec, can_backend_spec_t *out, char *error,
                            size_t error_len)
{
    static const char udp_prefix[] = "udp";
    static const char socketcan_prefix[] = "socketcan:";
    bool ok = false;

    (void)memset(out, 0, sizeof(*out));

    if (strncmp(spec, socketcan_prefix, sizeof(socketcan_prefix) - 1U) == 0) {
        out->kind = CAN_BACKEND_SOCKETCAN;
        out->interface = &spec[sizeof(socketcan_prefix) - 1U];
        ok = true;
    } else if (strcmp(spec, udp_prefix) == 0) {
        out->kind = CAN_BACKEND_UDP;
        out->udp_base_port = (uint16_t)CAN_UDP_DEFAULT_PORT;
        ok = true;
    } else if ((strncmp(spec, udp_prefix, sizeof(udp_prefix) - 1U) == 0) &&
               (spec[sizeof(udp_prefix) - 1U] == ':')) {
        out->kind = CAN_BACKEND_UDP;
        ok = parse_port(&spec[sizeof(udp_prefix)], &out->udp_base_port);
    } else {
        ok = false;
    }

    if (!ok) {
        (void)snprintf(
            error, error_len,
            "unknown CAN backend '%s' (use udp, udp:<base port> or socketcan:<interface>)", spec);
    }

    return ok;
}

bool can_backend_open(can_backend_t *backend, const char *spec, char *error, size_t error_len)
{
    can_backend_spec_t parsed;

    if (!can_backend_parse_spec(spec, &parsed, error, error_len)) {
        return false;
    }

    if (parsed.kind == CAN_BACKEND_SOCKETCAN) {
        return can_backend_socketcan_open(backend, parsed.interface, error, error_len);
    }

    return can_backend_udp_open(backend, parsed.udp_base_port, error, error_len);
}
