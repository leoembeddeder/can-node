/* SPDX-License-Identifier: MIT */
/* Linux SocketCAN backend: a raw CAN socket on one interface (vcan0 in tests and CI). */
#include "can_backend.h"

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define SOCKETCAN_RECEIVE_BUFFER_BYTES (4 * 1024 * 1024)

static bool socketcan_send(can_backend_t *self, const can_frame_t *frame)
{
    struct can_frame wire;
    ssize_t written;

    (void)memset(&wire, 0, sizeof(wire));
    wire.can_id = frame->id & CAN_EFF_MASK;

    if ((frame->flags & CAN_FLAG_EXT) != 0U) {
        wire.can_id |= CAN_EFF_FLAG;
    }

    if ((frame->flags & CAN_FLAG_RTR) != 0U) {
        wire.can_id |= CAN_RTR_FLAG;
    } else {
        (void)memcpy(wire.data, frame->data,
                     (frame->dlc <= CAN_MAX_DLC) ? frame->dlc : CAN_MAX_DLC);
    }

    wire.len = (frame->dlc <= CAN_MAX_DLC) ? frame->dlc : (uint8_t)CAN_MAX_DLC;

    do {
        written = write(self->fd, &wire, sizeof(wire));
    } while ((written < 0) && (errno == EINTR));

    if (written == (ssize_t)sizeof(wire)) {
        self->tx_frames++;
        return true;
    }

    /* A raw CAN socket writes a whole frame or nothing; EIO stands for a short write. */
    self->last_error = (written < 0) ? errno : EIO;
    self->tx_failed++;
    return false;
}

static int socketcan_recv(can_backend_t *self, can_frame_t *frame)
{
    for (;;) {
        struct can_frame wire;
        const ssize_t received = recv(self->fd, &wire, sizeof(wire), MSG_DONTWAIT);

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

        const bool extended = (wire.can_id & CAN_EFF_FLAG) != 0U;
        const bool remote = (wire.can_id & CAN_RTR_FLAG) != 0U;

        /* Dropped: a short read; an error frame (a status report of a real controller's
         * driver, which a virtual interface never produces); a length above 8; and a
         * base-format identifier above 0x7FF. The kernel passes the last one on unchanged
         * (cansend vcan0 EF0#55 sends it), and masking it to 11 bits would turn it into
         * another, valid identifier: 0xEF0 would become the ping 0x6F0. The UDP transport
         * drops the same frame, so it means nothing on either. */
        if ((received != (ssize_t)sizeof(wire)) || ((wire.can_id & CAN_ERR_FLAG) != 0U) ||
            (wire.len > CAN_MAX_DLC) ||
            (!extended && ((wire.can_id & CAN_EFF_MASK) > CAN_SFF_MASK))) {
            self->rx_malformed++;
            continue;
        }

        (void)memset(frame, 0, sizeof(*frame));
        frame->id = wire.can_id & (extended ? CAN_EFF_MASK : CAN_SFF_MASK);
        frame->flags = (uint8_t)((extended ? CAN_FLAG_EXT : 0U) | (remote ? CAN_FLAG_RTR : 0U));
        frame->dlc = wire.len;

        if (!remote) {
            (void)memcpy(frame->data, wire.data, wire.len);
        }

        self->rx_frames++;
        return 1;
    }
}

static void socketcan_close(can_backend_t *self)
{
    if (self->fd >= 0) {
        can_backend_refresh_counters(self); /* the last word on what the kernel dropped */
        (void)close(self->fd);
        self->fd = -1;
    }
}

bool can_backend_socketcan_open(can_backend_t *backend, const char *interface, char *error,
                                size_t error_len)
{
    const int receive_buffer = SOCKETCAN_RECEIVE_BUFFER_BYTES;
    struct sockaddr_can address;
    struct ifreq request;
    int fd;

    if ((interface == NULL) || (interface[0] == '\0') || (strlen(interface) >= IFNAMSIZ)) {
        (void)snprintf(error, error_len, "socketcan: invalid interface name");
        return false;
    }

    fd = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);

    if (fd < 0) {
        (void)snprintf(error, error_len,
                       "socketcan: socket: %s (is CAN support loaded? see docs/wsl-vcan.md)",
                       strerror(errno));
        return false;
    }

    (void)memset(&request, 0, sizeof(request));
    (void)strncpy(request.ifr_name, interface, IFNAMSIZ - 1U);

    if (ioctl(fd, SIOCGIFINDEX, &request) < 0) {
        (void)snprintf(error, error_len,
                       "socketcan: no interface '%s': %s (create it with scripts/vcan-up.sh)",
                       interface, strerror(errno));
        (void)close(fd);
        return false;
    }

    (void)memset(&address, 0, sizeof(address));
    address.can_family = AF_CAN;
    address.can_ifindex = request.ifr_ifindex;

    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        (void)snprintf(error, error_len, "socketcan: bind to '%s': %s", interface, strerror(errno));
        (void)close(fd);
        return false;
    }

    /* The defaults are what a node needs: frames sent here are looped back to the other
     * sockets on the interface (so local tools see them) but not to this socket. */
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));

    (void)memset(backend, 0, sizeof(*backend));
    backend->kind = "socketcan";
    backend->fd = fd;
    backend->send = socketcan_send;
    backend->recv = socketcan_recv;
    backend->close = socketcan_close;
    return true;
}
