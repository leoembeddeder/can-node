/* SPDX-License-Identifier: MIT */
/*
 * Tests of the CAN transports (platform/posix/can_backend*): the UDP datagram format, the UDP
 * bus between several participants on the loopback interface, and SocketCAN when the machine
 * has a vcan0 interface (FCN_TEST_CAN_INTERFACE names another; the tests are reported as
 * ignored when it does not exist). The SocketCAN tests hold the same lock on the interface as
 * the system tests (tests/conftest.py), so the two never count each other's frames.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "can_backend.h"
#include "test_bus_ports.h"
#include "unity.h"

/* After can_backend.h: linux/can.h defines CAN_MAX_DLC as well, and only a definition that
 * comes second, in a system header, is exempt from the redefinition warning. */
#include <linux/can.h>
#include <linux/can/raw.h>

#define RECEIVE_TIMEOUT_MS 500

static char s_error[160];
static int s_interface_lock; /* the lock on the SocketCAN interface, -1 while not held */

void setUp(void)
{
    s_error[0] = '\0';
    s_interface_lock = -1;
}

void tearDown(void)
{
    if (s_interface_lock >= 0) {
        (void)flock(s_interface_lock, LOCK_UN);
        (void)close(s_interface_lock);
        s_interface_lock = -1;
    }
}

/* Wait until a frame arrives or the timeout passes. */
static int receive_within(can_backend_t *backend, can_frame_t *frame, int timeout_ms)
{
    struct pollfd fd = {.fd = backend->fd, .events = POLLIN, .revents = 0};
    int result = backend->recv(backend, frame);

    if ((result == 0) && (poll(&fd, 1U, timeout_ms) > 0)) {
        result = backend->recv(backend, frame);
    }

    return result;
}

static can_frame_t make_frame(uint32_t id, uint8_t flags, uint8_t dlc, uint8_t seed)
{
    can_frame_t frame;

    (void)memset(&frame, 0, sizeof(frame));
    frame.id = id;
    frame.flags = flags;
    frame.dlc = dlc;

    if ((flags & CAN_FLAG_RTR) == 0U) {
        for (uint8_t i = 0U; i < dlc; i++) {
            frame.data[i] = (uint8_t)(seed + i);
        }
    }

    return frame;
}

static void assert_frames_equal(const can_frame_t *expected, const can_frame_t *actual)
{
    TEST_ASSERT_EQUAL_HEX32(expected->id, actual->id);
    TEST_ASSERT_EQUAL_HEX8(expected->flags, actual->flags);
    TEST_ASSERT_EQUAL_UINT8(expected->dlc, actual->dlc);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected->data, actual->data, CAN_MAX_DLC);
}

/* ------------------------------------------------------------------ the datagram format */

static void test_udp_datagram_has_the_documented_layout(void)
{
    /* Base-format identifier 0x123, three data bytes: can_id 0x00000123 little-endian,
     * length 3, three zero bytes, then the data and zero fill. */
    const uint8_t expected_base[CAN_UDP_DATAGRAM_LEN] = {0x23, 0x01, 0x00, 0x00, 0x03, 0x00,
                                                         0x00, 0x00, 0x11, 0x22, 0x33, 0x00,
                                                         0x00, 0x00, 0x00, 0x00};
    /* Extended identifier 0x1ABCDEF0: bit 31 marks the format, so can_id is 0x9ABCDEF0. */
    const uint8_t expected_extended[CAN_UDP_DATAGRAM_LEN] = {0xF0, 0xDE, 0xBC, 0x9A, 0x01, 0x00,
                                                             0x00, 0x00, 0x7F, 0x00, 0x00, 0x00,
                                                             0x00, 0x00, 0x00, 0x00};
    /* Remote request for identifier 0x7FF with DLC 8: bit 30 set, no data on the wire. */
    const uint8_t expected_remote[CAN_UDP_DATAGRAM_LEN] = {0xFF, 0x07, 0x00, 0x40, 0x08, 0x00,
                                                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                           0x00, 0x00, 0x00, 0x00};
    uint8_t datagram[CAN_UDP_DATAGRAM_LEN];
    can_frame_t frame;

    (void)memset(&frame, 0, sizeof(frame));
    frame.id = 0x123U;
    frame.dlc = 3U;
    frame.data[0] = 0x11U;
    frame.data[1] = 0x22U;
    frame.data[2] = 0x33U;
    frame.data[3] = 0xEEU; /* beyond the length: must not reach the wire */
    can_udp_encode(&frame, datagram);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_base, datagram, CAN_UDP_DATAGRAM_LEN);

    (void)memset(&frame, 0, sizeof(frame));
    frame.id = 0x1ABCDEF0U;
    frame.flags = CAN_FLAG_EXT;
    frame.dlc = 1U;
    frame.data[0] = 0x7FU;
    can_udp_encode(&frame, datagram);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_extended, datagram, CAN_UDP_DATAGRAM_LEN);

    (void)memset(&frame, 0, sizeof(frame));
    frame.id = 0x7FFU;
    frame.flags = CAN_FLAG_RTR;
    frame.dlc = 8U;
    frame.data[0] = 0xAAU; /* a remote frame carries no data */
    can_udp_encode(&frame, datagram);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_remote, datagram, CAN_UDP_DATAGRAM_LEN);
}

static void test_udp_decoding_restores_what_was_encoded(void)
{
    const struct {
        uint32_t id;
        uint8_t flags;
    } kinds[] = {{0x000U, 0U},           {0x7FFU, 0U},
                 {0x000U, CAN_FLAG_EXT}, {0x1FFFFFFFU, CAN_FLAG_EXT},
                 {0x456U, CAN_FLAG_RTR}, {0x12345678U, (uint8_t)(CAN_FLAG_EXT | CAN_FLAG_RTR)}};

    for (size_t k = 0U; k < (sizeof(kinds) / sizeof(kinds[0])); k++) {
        for (uint8_t dlc = 0U; dlc <= CAN_MAX_DLC; dlc++) {
            const can_frame_t original =
                make_frame(kinds[k].id, kinds[k].flags, dlc, (uint8_t)(0x40U + dlc));
            uint8_t datagram[CAN_UDP_DATAGRAM_LEN];
            can_frame_t decoded;

            can_udp_encode(&original, datagram);
            TEST_ASSERT_TRUE(can_udp_decode(datagram, sizeof(datagram), &decoded));
            assert_frames_equal(&original, &decoded);
        }
    }
}

static void test_udp_decoding_rejects_everything_that_is_not_a_frame(void)
{
    const can_frame_t valid = make_frame(0x123U, 0U, 4U, 0x10U);
    uint8_t good[CAN_UDP_DATAGRAM_LEN];
    uint8_t bad[CAN_UDP_DATAGRAM_LEN + 1U];
    can_frame_t decoded;

    can_udp_encode(&valid, good);
    TEST_ASSERT_TRUE(can_udp_decode(good, sizeof(good), &decoded));

    (void)memset(bad, 0, sizeof(bad));
    (void)memcpy(bad, good, sizeof(good));
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN - 1U, &decoded), "short");
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN + 1U, &decoded), "long");
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, 0U, &decoded), "empty");

    (void)memcpy(bad, good, sizeof(good));
    bad[3] |= 0x20U; /* bit 29 of can_id: an error frame */
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN, &decoded), "error flag");

    (void)memcpy(bad, good, sizeof(good));
    bad[4] = 9U;
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN, &decoded), "length 9");

    (void)memcpy(bad, good, sizeof(good));
    bad[1] = 0x08U; /* identifier 0x823 without the extended flag */
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN, &decoded),
                              "identifier too large for the base format");

    for (size_t pad = 5U; pad <= 7U; pad++) {
        (void)memcpy(bad, good, sizeof(good));
        bad[pad] = 1U;
        TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN, &decoded),
                                  "non-zero padding");
    }

    (void)memcpy(bad, good, sizeof(good));
    bad[8U + 4U] = 0x55U; /* a data byte beyond the length of 4 */
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN, &decoded),
                              "data beyond the length");

    (void)memcpy(bad, good, sizeof(good));
    bad[3] |= 0x40U; /* remote request, but the data bytes are still there */
    TEST_ASSERT_FALSE_MESSAGE(can_udp_decode(bad, CAN_UDP_DATAGRAM_LEN, &decoded),
                              "remote frame with data");
}

/* ------------------------------------------------------------------------- the UDP bus */

static void test_three_udp_participants_hear_each_other_but_not_themselves(void)
{
    const uint16_t base = test_bus_fresh_base_port();
    can_backend_t a;
    can_backend_t b;
    can_backend_t c;
    can_frame_t received = {0};

    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&a, base, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&b, base, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&c, base, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_EQUAL_STRING("udp", a.kind);

    const can_frame_t from_a = make_frame(0x100U, 0U, 8U, 0xA0U);
    const can_frame_t from_c = make_frame(0x1ABCDE00U, CAN_FLAG_EXT, 2U, 0xC0U);

    TEST_ASSERT_TRUE(a.send(&a, &from_a));
    TEST_ASSERT_EQUAL_INT(1, receive_within(&b, &received, RECEIVE_TIMEOUT_MS));
    assert_frames_equal(&from_a, &received);
    TEST_ASSERT_EQUAL_INT(1, receive_within(&c, &received, RECEIVE_TIMEOUT_MS));
    assert_frames_equal(&from_a, &received);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, receive_within(&a, &received, 50), "a heard itself");

    TEST_ASSERT_TRUE(c.send(&c, &from_c));
    TEST_ASSERT_EQUAL_INT(1, receive_within(&a, &received, RECEIVE_TIMEOUT_MS));
    assert_frames_equal(&from_c, &received);
    TEST_ASSERT_EQUAL_INT(1, receive_within(&b, &received, RECEIVE_TIMEOUT_MS));
    assert_frames_equal(&from_c, &received);

    TEST_ASSERT_EQUAL_UINT64(1U, a.tx_frames);
    TEST_ASSERT_EQUAL_UINT64(1U, a.rx_frames);
    TEST_ASSERT_EQUAL_UINT64(2U, b.rx_frames);
    TEST_ASSERT_EQUAL_UINT64(0U, b.rx_malformed);

    a.close(&a);
    b.close(&b);
    c.close(&c);
    TEST_ASSERT_EQUAL_INT(-1, a.fd);
}

static void test_a_burst_arrives_complete_and_in_order(void)
{
    const uint16_t base = test_bus_fresh_base_port();
    can_backend_t sender;
    can_backend_t receiver;
    can_frame_t received = {0};

    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&sender, base, s_error, sizeof(s_error)),
                             s_error);
    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&receiver, base, s_error, sizeof(s_error)),
                             s_error);

    /* 100 frames with nobody reading: they wait in the receiver's socket buffer. */
    for (uint32_t n = 0U; n < 100U; n++) {
        can_frame_t frame = make_frame(0x200U, 0U, 4U, 0U);

        frame.data[0] = (uint8_t)n;
        TEST_ASSERT_TRUE(sender.send(&sender, &frame));
    }

    for (uint32_t n = 0U; n < 100U; n++) {
        TEST_ASSERT_EQUAL_INT(1, receive_within(&receiver, &received, RECEIVE_TIMEOUT_MS));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)n, received.data[0]);
    }

    TEST_ASSERT_EQUAL_INT(0, receiver.recv(&receiver, &received));
    can_backend_refresh_counters(&receiver);
    TEST_ASSERT_EQUAL_UINT64(0U, receiver.rx_overflow);
    sender.close(&sender);
    receiver.close(&receiver);
}

static void test_frames_that_do_not_fit_the_socket_buffer_are_dropped_and_counted(void)
{
    /* UDP has no back-pressure: a participant that does not read loses what its socket
     * buffer cannot hold. What matters is that every loss is counted, so that a run which
     * dropped frames can be recognised and discarded. The kernel grants at most
     * net.core.rmem_max of the buffer this backend asks for; 20,000 frames exceed what even
     * a generous setting holds (a queued datagram costs the kernel close to a kilobyte). */
    enum { SENT = 20000 };
    const uint16_t base = test_bus_fresh_base_port();
    can_backend_t sender;
    can_backend_t receiver;
    can_frame_t received = {0};
    uint32_t arrived = 0U;
    uint32_t expected_next = 0U;
    bool in_order = true;

    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&sender, base, s_error, sizeof(s_error)),
                             s_error);
    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&receiver, base, s_error, sizeof(s_error)),
                             s_error);

    for (uint32_t n = 0U; n < (uint32_t)SENT; n++) {
        can_frame_t frame = make_frame(0x300U, 0U, 4U, 0U);

        frame.data[0] = (uint8_t)(n & 0xFFU);
        frame.data[1] = (uint8_t)((n >> 8U) & 0xFFU);
        frame.data[2] = (uint8_t)((n >> 16U) & 0xFFU);
        frame.data[3] = 0U;
        TEST_ASSERT_TRUE(sender.send(&sender, &frame));
    }

    /* The kernel knows at once how many it dropped, before anything is read. */
    can_backend_refresh_counters(&receiver);
    TEST_ASSERT_TRUE_MESSAGE(receiver.rx_overflow > 0U, "20,000 unread frames fitted the buffer");

    /* What did arrive is the beginning of the sequence, unbroken: the buffer filled up and
     * everything after that point was dropped, since nobody was reading. */
    while (receiver.recv(&receiver, &received) == 1) {
        const uint32_t n = (uint32_t)received.data[0] | ((uint32_t)received.data[1] << 8U) |
                           ((uint32_t)received.data[2] << 16U);

        in_order = in_order && (n == expected_next);
        expected_next = n + 1U;
        arrived++;
    }

    TEST_ASSERT_TRUE(in_order);
    TEST_ASSERT_TRUE(arrived > 0U);
    receiver.close(&receiver);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)SENT, (uint64_t)arrived + receiver.rx_overflow);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)arrived, receiver.rx_frames);
    sender.close(&sender);
}

static void test_a_seventeenth_udp_participant_is_refused_and_a_freed_slot_is_reused(void)
{
    const uint16_t base = test_bus_fresh_base_port();
    can_backend_t slots[CAN_UDP_SLOTS];
    can_backend_t extra;

    for (uint32_t i = 0U; i < CAN_UDP_SLOTS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&slots[i], base, s_error, sizeof(s_error)),
                                 s_error);
    }

    TEST_ASSERT_FALSE(can_backend_udp_open(&extra, base, s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL(strstr(s_error, "slots"));

    slots[5].close(&slots[5]);
    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&extra, base, s_error, sizeof(s_error)), s_error);
    extra.close(&extra);

    for (uint32_t i = 0U; i < CAN_UDP_SLOTS; i++) {
        slots[i].close(&slots[i]);
    }
}

static void test_udp_drops_datagrams_that_do_not_come_from_the_bus(void)
{
    const uint16_t base = test_bus_fresh_base_port();
    const can_frame_t valid = make_frame(0x321U, 0U, 2U, 0x77U);
    uint8_t datagram[CAN_UDP_DATAGRAM_LEN];
    struct sockaddr_in to;
    can_backend_t node;
    can_frame_t received = {0};

    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&node, base, s_error, sizeof(s_error)), s_error);
    can_udp_encode(&valid, datagram);

    /* A perfectly formed datagram, but sent from a port that is not a slot of this bus. */
    const int outsider = socket(AF_INET, SOCK_DGRAM, 0);

    TEST_ASSERT_TRUE(outsider >= 0);
    (void)memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(base); /* the node took the first slot */
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT((int)sizeof(datagram),
                          (int)sendto(outsider, datagram, sizeof(datagram), 0,
                                      (const struct sockaddr *)&to, sizeof(to)));
    (void)close(outsider);

    TEST_ASSERT_EQUAL_INT(0, receive_within(&node, &received, 100));
    TEST_ASSERT_EQUAL_UINT64(1U, node.rx_malformed);
    TEST_ASSERT_EQUAL_UINT64(0U, node.rx_frames);
    node.close(&node);
}

static void test_udp_drops_datagrams_from_a_bus_port_of_another_loopback_address(void)
{
    /* Every 127.x.y.z address is the local machine, but the bus is 127.0.0.1 only: a datagram
     * from a bus port number on 127.0.0.2 comes from outside the bus. */
    const uint16_t base = test_bus_fresh_base_port();
    const can_frame_t valid = make_frame(0x322U, 0U, 2U, 0x78U);
    uint8_t datagram[CAN_UDP_DATAGRAM_LEN];
    struct sockaddr_in address;
    can_backend_t node;
    can_frame_t received = {0};
    const int outsider = socket(AF_INET, SOCK_DGRAM, 0);

    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&node, base, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_TRUE(outsider >= 0);
    (void)memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)(base + 1U)); /* a slot of the bus, by number */
    address.sin_addr.s_addr = htonl(0x7F000002U);    /* 127.0.0.2 */
    TEST_ASSERT_EQUAL_INT(0, bind(outsider, (const struct sockaddr *)&address, sizeof(address)));

    can_udp_encode(&valid, datagram);
    address.sin_port = htons(base); /* the node took the first slot */
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT((int)sizeof(datagram),
                          (int)sendto(outsider, datagram, sizeof(datagram), 0,
                                      (const struct sockaddr *)&address, sizeof(address)));
    (void)close(outsider);

    TEST_ASSERT_EQUAL_INT(0, receive_within(&node, &received, 100));
    TEST_ASSERT_EQUAL_UINT64(1U, node.rx_malformed);
    TEST_ASSERT_EQUAL_UINT64(0U, node.rx_frames);
    node.close(&node);
}

static void test_a_send_and_a_receive_the_socket_refuses_are_counted_with_their_error(void)
{
    const uint16_t base = test_bus_fresh_base_port();
    const can_frame_t frame = make_frame(0x323U, 0U, 1U, 0x79U);
    can_backend_t node;
    can_frame_t received = {0};
    int fd;

    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&node, base, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_EQUAL_INT(0, node.last_error);

    /* A descriptor that is not open: every system call on it fails with EBADF. */
    fd = node.fd;
    node.fd = -1;
    TEST_ASSERT_FALSE(node.send(&node, &frame));
    TEST_ASSERT_EQUAL_UINT64(1U, node.tx_failed);
    TEST_ASSERT_EQUAL_UINT64(0U, node.tx_frames);
    TEST_ASSERT_EQUAL_INT(EBADF, node.last_error);

    node.last_error = 0;
    TEST_ASSERT_EQUAL_INT(-1, node.recv(&node, &received));
    TEST_ASSERT_EQUAL_UINT64(1U, node.rx_failed);
    TEST_ASSERT_EQUAL_UINT64(0U, node.rx_frames);
    TEST_ASSERT_EQUAL_INT(EBADF, node.last_error);

    /* Nothing pending is not a failure. */
    node.fd = fd;
    TEST_ASSERT_EQUAL_INT(0, node.recv(&node, &received));
    TEST_ASSERT_EQUAL_UINT64(1U, node.rx_failed);
    node.close(&node);
}

static void test_transport_specifications_are_taken_apart(void)
{
    can_backend_spec_t spec;

    TEST_ASSERT_TRUE(can_backend_parse_spec("udp", &spec, s_error, sizeof(s_error)));
    TEST_ASSERT_EQUAL_INT(CAN_BACKEND_UDP, spec.kind);
    TEST_ASSERT_EQUAL_UINT16(CAN_UDP_DEFAULT_PORT, spec.udp_base_port);

    TEST_ASSERT_TRUE(can_backend_parse_spec("udp:31000", &spec, s_error, sizeof(s_error)));
    TEST_ASSERT_EQUAL_INT(CAN_BACKEND_UDP, spec.kind);
    TEST_ASSERT_EQUAL_UINT16(31000U, spec.udp_base_port);

    TEST_ASSERT_TRUE(can_backend_parse_spec("udp:65535", &spec, s_error, sizeof(s_error)));
    TEST_ASSERT_EQUAL_UINT16(65535U, spec.udp_base_port);

    TEST_ASSERT_TRUE(can_backend_parse_spec("socketcan:vcan7", &spec, s_error, sizeof(s_error)));
    TEST_ASSERT_EQUAL_INT(CAN_BACKEND_SOCKETCAN, spec.kind);
    TEST_ASSERT_EQUAL_STRING("vcan7", spec.interface);

    /* A port is digits and nothing else. strtoul() would take every one of these. */
    static const char *const refused[] = {"udp: 1",
                                          "udp:+1",
                                          "udp:-1",
                                          "udp:-18446744073709512516",
                                          "udp:1 ",
                                          "udp:0x10",
                                          "udp:99999999999999999999999",
                                          "udp:65536",
                                          "udp:0",
                                          "udp:",
                                          "udpx",
                                          "UDP:1000",
                                          "udp1000"};

    for (size_t i = 0U; i < (sizeof(refused) / sizeof(refused[0])); i++) {
        s_error[0] = '\0';
        TEST_ASSERT_FALSE_MESSAGE(
            can_backend_parse_spec(refused[i], &spec, s_error, sizeof(s_error)), refused[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_error, "unknown CAN backend"), refused[i]);
    }
}

static void test_backend_specification_strings(void)
{
    can_backend_t backend;

    TEST_ASSERT_FALSE(can_backend_open(&backend, "carrier-pigeon", s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL(strstr(s_error, "unknown CAN backend"));
    TEST_ASSERT_FALSE(can_backend_open(&backend, "udp:", s_error, sizeof(s_error)));
    TEST_ASSERT_FALSE(can_backend_open(&backend, "udp:0", s_error, sizeof(s_error)));
    TEST_ASSERT_FALSE(can_backend_open(&backend, "udp:70000", s_error, sizeof(s_error)));
    TEST_ASSERT_FALSE(can_backend_open(&backend, "udp:12x", s_error, sizeof(s_error)));
    TEST_ASSERT_FALSE(
        can_backend_open(&backend, "udp:-18446744073709512516", s_error, sizeof(s_error)));
    TEST_ASSERT_FALSE(can_backend_open(&backend, "udp: 29500", s_error, sizeof(s_error)));
    /* A bus is 16 consecutive ports: 65520 is the last base port that has them. */
    TEST_ASSERT_FALSE(can_backend_open(&backend, "udp:65521", s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL(strstr(s_error, "out of range"));
    TEST_ASSERT_TRUE_MESSAGE(can_backend_open(&backend, "udp:65520", s_error, sizeof(s_error)),
                             s_error);
    backend.close(&backend);
    TEST_ASSERT_FALSE(can_backend_open(&backend, "socketcan:", s_error, sizeof(s_error)));
    TEST_ASSERT_FALSE(
        can_backend_open(&backend, "socketcan:no-such-interface0", s_error, sizeof(s_error)));

    char spec[32];

    (void)snprintf(spec, sizeof(spec), "udp:%u", (unsigned int)test_bus_fresh_base_port());
    TEST_ASSERT_TRUE_MESSAGE(can_backend_open(&backend, spec, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_EQUAL_STRING("udp", backend.kind);
    backend.close(&backend);
}

/* ---------------------------------------------------------------------------- SocketCAN */

/* The interface the SocketCAN tests use: FCN_TEST_CAN_INTERFACE, as for the system tests, or
 * vcan0. */
static const char *test_interface(void)
{
    const char *name = getenv("FCN_TEST_CAN_INTERFACE");

    return ((name != NULL) && (name[0] != '\0')) ? name : "vcan0";
}

/*
 * One interface is shared by everything on the machine, and the tests on it count frames, so
 * only one test may use it at a time. tests/conftest.py takes an exclusive flock() on
 * <temporary directory>/fcn-test-<interface>.lock around every system test on it; this test
 * takes the same lock. The directory is the one Python's tempfile.gettempdir() picks in any
 * ordinary setup: the first of TMPDIR, TEMP and TMP that names a directory, else /tmp.
 */
static int lock_interface(const char *interface)
{
    static const char *const variables[] = {"TMPDIR", "TEMP", "TMP"};
    const char *directory = "/tmp";
    char path[512];
    int fd;

    for (size_t i = 0U; i < (sizeof(variables) / sizeof(variables[0])); i++) {
        const char *value = getenv(variables[i]);
        struct stat info;

        if ((value != NULL) && (value[0] != '\0') && (stat(value, &info) == 0) &&
            S_ISDIR(info.st_mode)) {
            directory = value;
            break;
        }
    }

    (void)snprintf(path, sizeof(path), "%s/fcn-test-%s.lock", directory, interface);
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flock(fd, LOCK_EX), path);
    return fd;
}

/* Send one raw frame on the interface, as any program on the machine may. */
static void send_raw(const char *interface, uint32_t can_id, uint8_t len, uint8_t byte)
{
    struct sockaddr_can address;
    struct can_frame wire;
    const int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);

    TEST_ASSERT_TRUE(fd >= 0);
    (void)memset(&address, 0, sizeof(address));
    address.can_family = AF_CAN;
    address.can_ifindex = (int)if_nametoindex(interface);
    TEST_ASSERT_TRUE(address.can_ifindex > 0);
    TEST_ASSERT_EQUAL_INT(0, bind(fd, (const struct sockaddr *)&address, sizeof(address)));

    (void)memset(&wire, 0, sizeof(wire));
    wire.can_id = can_id;
    wire.len = len;
    wire.data[0] = byte;
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)sizeof(wire), (int)write(fd, &wire, sizeof(wire)),
                                  "the kernel refused the raw frame");
    (void)close(fd);
}

static void test_socketcan_participants_exchange_frames_on_vcan0(void)
{
    const char *const interface = test_interface();
    can_backend_t a;
    can_backend_t b;
    can_frame_t received = {0};

    /* First the lock, released in tearDown(): a socket opened before it would collect what
     * the test holding the interface sends meanwhile. */
    s_interface_lock = lock_interface(interface);

    if (!can_backend_socketcan_open(&a, interface, s_error, sizeof(s_error))) {
        /* CI sets FCN_REQUIRE_SOCKETCAN: there the interface exists and must be tested. */
        if (getenv("FCN_REQUIRE_SOCKETCAN") != NULL) {
            TEST_FAIL_MESSAGE(s_error);
        }

        TEST_IGNORE_MESSAGE(
            "the interface is not available on this machine (see docs/wsl-vcan.md)");
    }

    TEST_ASSERT_TRUE_MESSAGE(can_backend_socketcan_open(&b, interface, s_error, sizeof(s_error)),
                             s_error);
    TEST_ASSERT_EQUAL_STRING("socketcan", a.kind);

    /* Other programs may be using vcan0; pick identifiers of our own and skip the rest. */
    const can_frame_t frames[] = {
        make_frame(0x5A1U, 0U, 8U, 0x01U),
        make_frame(0x5A2U, 0U, 0U, 0x00U),
        make_frame(0x15A5A5A5U, CAN_FLAG_EXT, 5U, 0x30U),
        make_frame(0x5A3U, CAN_FLAG_RTR, 4U, 0x00U),
    };

    for (size_t i = 0U; i < (sizeof(frames) / sizeof(frames[0])); i++) {
        bool found = false;

        TEST_ASSERT_TRUE(a.send(&a, &frames[i]));

        for (int attempt = 0; (attempt < 50) && !found; attempt++) {
            TEST_ASSERT_EQUAL_INT(1, receive_within(&b, &received, RECEIVE_TIMEOUT_MS));
            found = (received.id == frames[i].id) && (received.flags == frames[i].flags);
        }

        TEST_ASSERT_TRUE_MESSAGE(found, "the frame did not arrive on the second socket");
        assert_frames_equal(&frames[i], &received);
    }

    /* The sender does not receive its own frames. */
    TEST_ASSERT_EQUAL_INT(0, receive_within(&a, &received, 50));
    a.close(&a);
    b.close(&b);
}

static void test_socketcan_drops_a_base_identifier_above_0x7ff(void)
{
    const char *const interface = test_interface();
    const can_frame_t marker = make_frame(0x5A6U, 0U, 1U, 0x11U);
    can_backend_t node;
    can_frame_t received = {0};
    bool found = false;

    s_interface_lock = lock_interface(interface);

    if (!can_backend_socketcan_open(&node, interface, s_error, sizeof(s_error))) {
        if (getenv("FCN_REQUIRE_SOCKETCAN") != NULL) {
            TEST_FAIL_MESSAGE(s_error);
        }

        TEST_IGNORE_MESSAGE(
            "the interface is not available on this machine (see docs/wsl-vcan.md)");
    }

    /* Base format (no CAN_EFF_FLAG), but with bits above the 11 of a base identifier. The
     * kernel passes them on. Masked to 11 bits, 0xEF0 would be 0x6F0, the node's ping, and
     * 0x12345 would be 0x345. Then a valid frame from the same sender, which arrives after
     * them. */
    send_raw(interface, 0xEF0U, 1U, 0x55U);
    send_raw(interface, 0x12345U, 1U, 0x56U);
    send_raw(interface, marker.id, marker.dlc, marker.data[0]);

    for (int attempt = 0; (attempt < 50) && !found; attempt++) {
        TEST_ASSERT_EQUAL_INT(1, receive_within(&node, &received, RECEIVE_TIMEOUT_MS));
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0x6F0U, received.id, "0xEF0 became the ping 0x6F0");
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0x345U, received.id, "0x12345 became 0x345");
        found = (received.id == marker.id) && (received.flags == 0U);
    }

    TEST_ASSERT_TRUE_MESSAGE(found, "the valid frame did not arrive");
    assert_frames_equal(&marker, &received);
    TEST_ASSERT_EQUAL_UINT64(2U, node.rx_malformed);
    node.close(&node);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_udp_datagram_has_the_documented_layout);
    RUN_TEST(test_udp_decoding_restores_what_was_encoded);
    RUN_TEST(test_udp_decoding_rejects_everything_that_is_not_a_frame);
    RUN_TEST(test_three_udp_participants_hear_each_other_but_not_themselves);
    RUN_TEST(test_a_burst_arrives_complete_and_in_order);
    RUN_TEST(test_frames_that_do_not_fit_the_socket_buffer_are_dropped_and_counted);
    RUN_TEST(test_a_seventeenth_udp_participant_is_refused_and_a_freed_slot_is_reused);
    RUN_TEST(test_udp_drops_datagrams_that_do_not_come_from_the_bus);
    RUN_TEST(test_udp_drops_datagrams_from_a_bus_port_of_another_loopback_address);
    RUN_TEST(test_a_send_and_a_receive_the_socket_refuses_are_counted_with_their_error);
    RUN_TEST(test_transport_specifications_are_taken_apart);
    RUN_TEST(test_backend_specification_strings);
    RUN_TEST(test_socketcan_participants_exchange_frames_on_vcan0);
    RUN_TEST(test_socketcan_drops_a_base_identifier_above_0x7ff);
    return UNITY_END();
}
