/* SPDX-License-Identifier: MIT */
/* The virtual CAN bus over UDP on the loopback interface. Protocol: see can_backend.h. */
#include "can_backend.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define UDP_ID_MASK  0x1FFFFFFFU
#define UDP_ERR_FLAG 0x20000000U
#define UDP_RTR_FLAG 0x40000000U
#define UDP_EFF_FLAG 0x80000000U

/* Asked of the kernel for the receive buffer. The kernel grants at most net.core.rmem_max,
 * which on a stock system is far less than this (room for a few hundred frames). The
 * controller thread reads promptly, so that is enough for a bus that is paced at its bit
 * rate, and whatever the kernel has to drop is counted (can_backend_refresh_counters). */
#define UDP_RECEIVE_BUFFER_BYTES (4 * 1024 * 1024)

void can_udp_encode(const can_frame_t *frame, uint8_t datagram[CAN_UDP_DATAGRAM_LEN])
{
    uint32_t can_id = frame->id & UDP_ID_MASK;
    const uint8_t len = (frame->dlc <= CAN_MAX_DLC) ? frame->dlc : (uint8_t)CAN_MAX_DLC;
    const bool remote = (frame->flags & CAN_FLAG_RTR) != 0U;

    if ((frame->flags & CAN_FLAG_EXT) != 0U) {
        can_id |= UDP_EFF_FLAG;
    }

    if (remote) {
        can_id |= UDP_RTR_FLAG;
    }

    (void)memset(datagram, 0, CAN_UDP_DATAGRAM_LEN);
    datagram[0] = (uint8_t)(can_id & 0xFFU);
    datagram[1] = (uint8_t)((can_id >> 8U) & 0xFFU);
    datagram[2] = (uint8_t)((can_id >> 16U) & 0xFFU);
    datagram[3] = (uint8_t)((can_id >> 24U) & 0xFFU);
    datagram[4] = len;

    if (!remote) {
        (void)memcpy(&datagram[8], frame->data, len);
    }
}

bool can_udp_decode(const uint8_t *datagram, size_t len, can_frame_t *frame)
{
    if (len != CAN_UDP_DATAGRAM_LEN) {
        return false;
    }

    const uint32_t can_id = (uint32_t)datagram[0] | ((uint32_t)datagram[1] << 8U) |
                            ((uint32_t)datagram[2] << 16U) | ((uint32_t)datagram[3] << 24U);
    const bool extended = (can_id & UDP_EFF_FLAG) != 0U;
    const bool remote = (can_id & UDP_RTR_FLAG) != 0U;
    const uint32_t id = can_id & UDP_ID_MASK;
    const uint8_t dlc = datagram[4];

    if (((can_id & UDP_ERR_FLAG) != 0U) || (dlc > CAN_MAX_DLC) ||
        (!extended && (id > CAN_ID_STD_MAX)) || (datagram[5] != 0U) || (datagram[6] != 0U) ||
        (datagram[7] != 0U)) {
        return false;
    }

    /* Bytes beyond the length, and all data bytes of a remote frame, have to be zero: the
     * format has exactly one encoding per frame. */
    const uint8_t used = remote ? 0U : dlc;

    for (size_t i = used; i < CAN_MAX_DLC; i++) {
        if (datagram[8U + i] != 0U) {
            return false;
        }
    }

    (void)memset(frame, 0, sizeof(*frame));
    frame->id = id;
    frame->flags = (uint8_t)((extended ? CAN_FLAG_EXT : 0U) | (remote ? CAN_FLAG_RTR : 0U));
    frame->dlc = dlc;
    (void)memcpy(frame->data, &datagram[8], used);
    return true;
}

static void loopback_address(struct sockaddr_in *address, uint16_t port)
{
    (void)memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

static bool udp_send(can_backend_t *self, const can_frame_t *frame)
{
    uint8_t datagram[CAN_UDP_DATAGRAM_LEN];
    int error = 0;

    can_udp_encode(frame, datagram);

    for (uint16_t slot = 0U; slot < CAN_UDP_SLOTS; slot++) {
        const uint16_t port = (uint16_t)(self->udp_base_port + slot);

        if (port != self->udp_own_port) {
            struct sockaddr_in to;
            ssize_t sent;

            loopback_address(&to, port);

            do {
                sent = sendto(self->fd, datagram, sizeof(datagram), MSG_DONTWAIT,
                              (const struct sockaddr *)&to, sizeof(to));
            } while ((sent < 0) && (errno == EINTR));

            /* A slot nobody has bound is not an error: the datagram is simply not
             * delivered, like a frame on a bus segment with no node attached. The first
             * real error is the one reported. */
            if ((sent < 0) && (errno != ECONNREFUSED) && (error == 0)) {
                error = errno;
            }
        }
    }

    if (error == 0) {
        self->tx_frames++;
    } else {
        self->last_error = error;
        self->tx_failed++;
    }

    return (error == 0);
}

static int udp_recv(can_backend_t *self, can_frame_t *frame)
{
    for (;;) {
        uint8_t datagram[CAN_UDP_DATAGRAM_LEN + 1U]; /* one extra byte reveals oversize input */
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t received;

        /* Cleared first: the kernel may store less than a full address, and what it did not
         * store must not look like one. */
        (void)memset(&from, 0, sizeof(from));
        received = recvfrom(self->fd, datagram, sizeof(datagram), MSG_DONTWAIT,
                            (struct sockaddr *)&from, &from_len);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                return 0;
            }

            self->last_error = errno;
            self->rx_failed++;
            return -1;
        }

        const uint16_t port = ntohs(from.sin_port);
        const bool from_bus =
            (from_len >= sizeof(from)) && (from.sin_family == AF_INET) &&
            (from.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) && (port >= self->udp_base_port) &&
            (port < (self->udp_base_port + CAN_UDP_SLOTS)) && (port != self->udp_own_port);

        if (from_bus && can_udp_decode(datagram, (size_t)received, frame)) {
            self->rx_frames++;
            return 1;
        }

        self->rx_malformed++;
    }
}

static void udp_close(can_backend_t *self)
{
    if (self->fd >= 0) {
        can_backend_refresh_counters(self); /* the last word on what the kernel dropped */
        (void)close(self->fd);
        self->fd = -1;
    }
}

bool can_backend_udp_open(can_backend_t *backend, uint16_t base_port, char *error, size_t error_len)
{
    const int receive_buffer = UDP_RECEIVE_BUFFER_BYTES;
    int fd;

    /* A bus is the ports B .. B+15, so the last possible base port is 65520. */
    if ((base_port == 0U) || (base_port > (uint16_t)(UINT16_MAX - (CAN_UDP_SLOTS - 1U)))) {
        (void)snprintf(error, error_len, "udp: base port %u is out of range (1..%u)",
                       (unsigned int)base_port, (unsigned int)(UINT16_MAX - (CAN_UDP_SLOTS - 1U)));
        return false;
    }

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        (void)snprintf(error, error_len, "udp: socket: %s", strerror(errno));
        return false;
    }

    (void)memset(backend, 0, sizeof(*backend));
    backend->udp_own_port = 0U;

    /* Take the first free slot. No SO_REUSEADDR: a slot belongs to exactly one participant,
     * and binding a taken slot has to fail. */
    for (uint16_t slot = 0U; (slot < CAN_UDP_SLOTS) && (backend->udp_own_port == 0U); slot++) {
        struct sockaddr_in address;

        loopback_address(&address, (uint16_t)(base_port + slot));

        if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
            backend->udp_own_port = (uint16_t)(base_port + slot);
        }
    }

    if (backend->udp_own_port == 0U) {
        (void)snprintf(error, error_len, "udp: all %u slots of the bus at port %u are taken",
                       (unsigned int)CAN_UDP_SLOTS, (unsigned int)base_port);
        (void)close(fd);
        return false;
    }

    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));

    backend->kind = "udp";
    backend->fd = fd;
    backend->send = udp_send;
    backend->recv = udp_recv;
    backend->close = udp_close;
    backend->udp_base_port = base_port;
    return true;
}
