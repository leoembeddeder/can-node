/* SPDX-License-Identifier: MIT */
/*
 * Tests of the CAN controller of the host build (platform/posix/posix_can.c) through the
 * interface the driver uses (plat_can.h): mailboxes and their outcomes, abort, bus off and
 * recovery, the receive FIFO, and what a driver that falls behind is and is not told.
 *
 * No scheduler runs here. The test is the driver: it calls plat_can_tx(), plat_can_rx() and
 * plat_can_event() from its own thread and polls instead of waiting for the interrupt. The
 * hardware thread is the real one and runs in real time at 500 kbit/s. A second participant
 * on the same UDP bus plays the rest of the network, and faults are applied through the
 * simulation control socket, as the system tests do from outside the process.
 *
 * No verdict depends on how fast the host is. Every wait is "until it has happened, at most
 * WAIT_MS", and the assertions are about what happened, in which order, and about times of
 * the modelled bus, which the model computes and the host cannot shorten.
 */
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "can/can_bits.h"
#include "can_backend.h"
#include "plat_can.h"
#include "posix_platform.h"
#include "posix_sim.h"
#include "test_bus_ports.h"
#include "unity.h"

#define BITRATE 500000U
#define WAIT_MS 5000U /* only a failing test ever waits this long */

/* State events the event FIFO takes before it keeps the rest for transmit outcomes. */
#define STATE_EVENTS_QUEUED (PLAT_CAN_EVENT_FIFO_DEPTH - PLAT_CAN_TX_MAILBOXES)

static char s_error[160];
static can_backend_t s_peer;
static bool s_peer_open;
static int s_sim_client = -1;
static uint16_t s_sim_port;
static uint8_t s_chain_state; /* the state of the last state event taken */

void setUp(void)
{
    const uint16_t base = test_bus_fresh_base_port();
    char spec[32];

    (void)snprintf(spec, sizeof(spec), "udp:%u", (unsigned int)base);
    s_error[0] = '\0';
    s_chain_state = PLAT_CAN_ERROR_ACTIVE;
    TEST_ASSERT_TRUE_MESSAGE(posix_can_start(spec, BITRATE, s_error, sizeof(s_error)), s_error);
    TEST_ASSERT_TRUE_MESSAGE(can_backend_udp_open(&s_peer, base, s_error, sizeof(s_error)),
                             s_error);
    s_peer_open = true;
}

void tearDown(void)
{
    posix_can_stop();

    if (s_peer_open) {
        s_peer.close(&s_peer);
        s_peer_open = false;
    }
}

/* ------------------------------------------------------------------------------- helpers */

static void nap(void)
{
    const struct timespec pause = {0, 50000L};

    (void)nanosleep(&pause, NULL);
}

static uint64_t deadline_ns(void)
{
    return posix_monotonic_ns() + ((uint64_t)WAIT_MS * 1000000ULL);
}

static can_frame_t make_frame(uint32_t id, uint8_t flags, uint8_t dlc, uint8_t seed)
{
    can_frame_t frame;

    (void)memset(&frame, 0, sizeof(frame));
    frame.id = id;
    frame.flags = flags;
    frame.dlc = dlc;

    if ((flags & CAN_FLAG_RTR) == 0U) {
        for (uint8_t i = 0U; (i < dlc) && (i < CAN_MAX_DLC); i++) {
            frame.data[i] = (uint8_t)(seed + i);
        }
    }

    return frame;
}

/* A frame that carries a 16-bit number. */
static can_frame_t numbered_frame(uint32_t id, uint32_t number)
{
    can_frame_t frame = make_frame(id, 0U, 2U, 0U);

    frame.data[0] = (uint8_t)(number & 0xFFU);
    frame.data[1] = (uint8_t)((number >> 8U) & 0xFFU);
    return frame;
}

static uint32_t frame_number(const can_frame_t *frame)
{
    return (uint32_t)frame->data[0] | ((uint32_t)frame->data[1] << 8U);
}

static void assert_frames_equal(const can_frame_t *expected, const can_frame_t *actual)
{
    TEST_ASSERT_EQUAL_HEX32(expected->id, actual->id);
    TEST_ASSERT_EQUAL_HEX8(expected->flags, actual->flags);
    TEST_ASSERT_EQUAL_UINT8(expected->dlc, actual->dlc);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected->data, actual->data, CAN_MAX_DLC);
}

/* What the other participant hears next, waiting up to `timeout_ms`: 1 with a frame, else 0. */
static int peer_hears(can_frame_t *frame, int timeout_ms)
{
    struct pollfd fd = {.fd = s_peer.fd, .events = POLLIN, .revents = 0};
    int result = s_peer.recv(&s_peer, frame);

    if ((result == 0) && (poll(&fd, 1U, timeout_ms) > 0)) {
        result = s_peer.recv(&s_peer, frame);
    }

    return result;
}

/* One simulation control command; fails the test unless the reply is "ok" (`ok` true) or
 * "err" (`ok` false). Returns the reply. */
static const char *sim_expect(const char *command, bool ok)
{
    static char reply[POSIX_SIM_MAX_DATAGRAM + 1U];
    static unsigned int serial;
    struct pollfd fd = {.fd = s_sim_client, .events = POLLIN, .revents = 0};
    struct sockaddr_in to;
    char request[128];
    char expected[32];
    ssize_t received;
    int length;

    serial++;
    length = snprintf(request, sizeof(request), "t%u %s", serial, command);
    (void)snprintf(expected, sizeof(expected), "t%u %s", serial, ok ? "ok" : "err");
    TEST_ASSERT_TRUE((length > 0) && ((size_t)length < sizeof(request)));

    (void)memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(s_sim_port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT(length, (int)sendto(s_sim_client, request, (size_t)length, 0,
                                              (const struct sockaddr *)&to, sizeof(to)));
    TEST_ASSERT_TRUE_MESSAGE(poll(&fd, 1U, (int)WAIT_MS) > 0, "simulation control did not answer");
    received = recv(s_sim_client, reply, sizeof(reply) - 1U, 0);
    TEST_ASSERT_TRUE(received > 0);
    reply[received] = '\0';
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(reply, expected, strlen(expected)), reply);
    return reply;
}

static const char *sim(const char *command)
{
    return sim_expect(command, true);
}

/* The number after " <key>=" in a simulation control reply. */
static uint64_t sim_number(const char *reply, const char *key)
{
    char pattern[48];
    const char *found;

    (void)snprintf(pattern, sizeof(pattern), " %s=", key);
    found = strstr(reply, pattern);
    TEST_ASSERT_NOT_NULL_MESSAGE(found, key);
    return strtoull(&found[strlen(pattern)], NULL, 10);
}

/* Take the next event if there is one. Whatever a test is about, the state events it sees
 * must form an unbroken chain: each starts in the state the one before ended in. */
static bool take_event(plat_can_event_t *event)
{
    const bool taken = plat_can_event(event);

    if (taken && (event->kind == PLAT_CAN_EVENT_STATE)) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(s_chain_state, event->old_state, "state chain broken");
        TEST_ASSERT_NOT_EQUAL_MESSAGE(event->old_state, event->state, "state event without change");
        s_chain_state = event->state;
    }

    return taken;
}

/* The next event of any kind; fails the test if none arrives. */
static plat_can_event_t next_event(void)
{
    const uint64_t deadline = deadline_ns();
    plat_can_event_t event;
    bool taken = take_event(&event);

    while (!taken && (posix_monotonic_ns() < deadline)) {
        nap();
        taken = take_event(&event);
    }

    TEST_ASSERT_TRUE_MESSAGE(taken, "no event from the controller");
    return event;
}

/* The next TX_DONE or TX_ABORTED event; state events on the way are taken and checked. */
static plat_can_event_t next_outcome(void)
{
    plat_can_event_t event = next_event();

    while (event.kind == PLAT_CAN_EVENT_STATE) {
        event = next_event();
    }

    return event;
}

/* The next state event; fails the test if a transmit outcome comes first. */
static plat_can_event_t next_state(void)
{
    const plat_can_event_t event = next_event();

    TEST_ASSERT_EQUAL_INT_MESSAGE(PLAT_CAN_EVENT_STATE, event.kind, "expected a state event");
    return event;
}

/* The next received frame; fails the test if none arrives. */
static plat_can_rx_t next_rx(void)
{
    const uint64_t deadline = deadline_ns();
    plat_can_rx_t received;
    bool taken = plat_can_rx(&received);

    while (!taken && (posix_monotonic_ns() < deadline)) {
        nap();
        taken = plat_can_rx(&received);
    }

    TEST_ASSERT_TRUE_MESSAGE(taken, "no frame from the controller");
    return received;
}

static void wait_for_state(uint8_t state)
{
    const uint64_t deadline = deadline_ns();
    plat_can_status_t status;

    plat_can_get_status(&status);

    while ((status.state != state) && (posix_monotonic_ns() < deadline)) {
        nap();
        plat_can_get_status(&status);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(state, status.state, "the controller did not reach the state");
}

static void wait_for_rec(uint16_t rec)
{
    const uint64_t deadline = deadline_ns();
    plat_can_status_t status;

    plat_can_get_status(&status);

    while ((status.rec != rec) && (posix_monotonic_ns() < deadline)) {
        nap();
        plat_can_get_status(&status);
    }

    TEST_ASSERT_EQUAL_UINT16(rec, status.rec);
}

/* Wait until the model has delivered `count` frames to the receive FIFO (or lost them to an
 * overrun of it): the count of the controller model, read through simulation control. */
static void wait_for_model_rx(uint64_t count)
{
    const uint64_t deadline = deadline_ns();
    uint64_t delivered = sim_number(sim("can.status"), "rx");

    while ((delivered != count) && (posix_monotonic_ns() < deadline)) {
        nap();
        delivered = sim_number(sim("can.status"), "rx");
    }

    TEST_ASSERT_EQUAL_UINT64(count, delivered);
}

static void assert_nothing_was_lost_on_the_way_to_the_driver(void)
{
    posix_can_stats_t stats;

    posix_can_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, stats.event_fifo_overruns, "a transmit outcome was lost");
    TEST_ASSERT_EQUAL_UINT32(0U, stats.rx_fifo_overruns);
}

/* --------------------------------------------------------------------------------- tests */

static void test_frames_leave_by_priority_at_bus_speed_and_each_is_confirmed_once(void)
{
    static const uint32_t ids[PLAT_CAN_TX_MAILBOXES] = {0x700U, 0x100U, 0x555U, 0x0F0U,
                                                        0x123U, 0x7FFU, 0x001U, 0x300U};
    const can_frame_t extra = make_frame(0x040U, 0U, 1U, 0xEEU);
    can_frame_t frames[PLAT_CAN_TX_MAILBOXES];
    plat_can_event_t done[PLAT_CAN_TX_MAILBOXES];
    uint32_t keys[PLAT_CAN_TX_MAILBOXES];
    bool confirmed[PLAT_CAN_TX_MAILBOXES] = {false};
    posix_can_stats_t stats;
    plat_can_event_t event;
    can_frame_t heard;

    for (uint32_t i = 0U; i < PLAT_CAN_TX_MAILBOXES; i++) {
        frames[i] = make_frame(ids[i], 0U, (uint8_t)(i + 1U), (uint8_t)(0x10U * i));
        TEST_ASSERT_TRUE(plat_can_tx(&frames[i], 100U + i));
    }

    /* Every mailbox is in use, and stays in use until its outcome has been taken, however
     * many of the frames have left the controller in the meantime. */
    TEST_ASSERT_FALSE(plat_can_tx(&extra, 999U));

    for (uint32_t n = 0U; n < PLAT_CAN_TX_MAILBOXES; n++) {
        can_bits_info_t bits;
        uint64_t heard_ns;
        uint32_t index;

        /* The rest of the network has the frame ... */
        TEST_ASSERT_EQUAL_INT(1, peer_hears(&heard, (int)WAIT_MS));
        heard_ns = posix_monotonic_ns();

        /* ... and the driver is told about the same one, once. */
        done[n] = next_outcome();
        TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, done[n].kind);
        TEST_ASSERT_TRUE((done[n].handle - 100U) < PLAT_CAN_TX_MAILBOXES);
        index = (done[n].handle - 100U) % PLAT_CAN_TX_MAILBOXES;
        TEST_ASSERT_FALSE_MESSAGE(confirmed[index], "a frame was confirmed twice");
        confirmed[index] = true;
        assert_frames_equal(&frames[index], &heard);
        keys[n] = can_frame_arbitration_key(&frames[index]);

        /* On the modelled bus it took exactly its length in bit times, after the
         * intermission that follows the frame before, and nobody had it before its end. */
        TEST_ASSERT_TRUE(can_bits_exact(&frames[index], &bits));
        TEST_ASSERT_TRUE(done[n].submit_ns <= done[n].start_ns);
        TEST_ASSERT_EQUAL_UINT64(can_bits_to_ns(bits.frame_bits, BITRATE),
                                 done[n].eof_ns - done[n].start_ns);
        TEST_ASSERT_TRUE_MESSAGE(heard_ns >= done[n].eof_ns, "on the transport before its end");

        if (n > 0U) {
            TEST_ASSERT_TRUE_MESSAGE(
                done[n].start_ns >=
                    (done[n - 1U].eof_ns + can_bits_to_ns(CAN_BITS_INTERMISSION, BITRATE)),
                "two frames closer than the intermission");
        }
    }

    /* Arbitration: a frame never went out while one with higher priority was waiting in a
     * mailbox. (Which frames were waiting when the first one started is up to the host; the
     * times in the events say it.) */
    for (uint32_t n = 0U; n < PLAT_CAN_TX_MAILBOXES; n++) {
        for (uint32_t later = n + 1U; later < PLAT_CAN_TX_MAILBOXES; later++) {
            if (done[later].submit_ns < done[n].start_ns) {
                TEST_ASSERT_TRUE_MESSAGE(keys[n] < keys[later], "a lower priority went first");
            }
        }
    }

    /* The mailboxes are free again. */
    TEST_ASSERT_TRUE(plat_can_tx(&extra, 999U));
    event = next_outcome();
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, event.kind);
    TEST_ASSERT_EQUAL_UINT32(999U, event.handle);
    TEST_ASSERT_EQUAL_INT(1, peer_hears(&heard, (int)WAIT_MS));
    assert_frames_equal(&extra, &heard);
    TEST_ASSERT_FALSE(take_event(&event));

    posix_can_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(PLAT_CAN_TX_MAILBOXES + 1U, stats.frames_paced);
    TEST_ASSERT_TRUE(stats.pacing_lag_max_ns >= stats.pacing_lag_mean_ns);
    /* The pass that writes a frame starts at or after its end of frame, and the thread's
     * last sleep before it was for that end of frame or an earlier deadline: every frame
     * written is a pass that served a deadline. */
    TEST_ASSERT_TRUE(stats.wakeups >= 1U);
    TEST_ASSERT_TRUE(stats.wakeup_lag_max_ns >= stats.wakeup_lag_mean_ns);
    TEST_ASSERT_EQUAL_UINT64(0U, stats.rx_delivered);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.state_events_merged);
    assert_nothing_was_lost_on_the_way_to_the_driver();
}

static void test_a_frame_that_is_not_a_can_frame_comes_back_aborted(void)
{
    can_frame_t bad = make_frame(0x123U, 0U, 8U, 0x01U);
    const can_frame_t good = make_frame(0x124U, 0U, 1U, 0x02U);
    plat_can_event_t event;
    can_frame_t heard;

    bad.dlc = 9U;
    TEST_ASSERT_TRUE(plat_can_tx(&bad, 1U));
    event = next_outcome();
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_ABORTED, event.kind);
    TEST_ASSERT_EQUAL_UINT32(1U, event.handle);

    /* Nothing went out for it, and the controller works as before. */
    TEST_ASSERT_TRUE(plat_can_tx(&good, 2U));
    event = next_outcome();
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, event.kind);
    TEST_ASSERT_EQUAL_UINT32(2U, event.handle);
    TEST_ASSERT_EQUAL_INT(1, peer_hears(&heard, (int)WAIT_MS));
    assert_frames_equal(&good, &heard);
    assert_nothing_was_lost_on_the_way_to_the_driver();
}

static void test_received_frames_reach_the_driver_in_order_with_their_bus_time(void)
{
    const can_frame_t sent[3] = {make_frame(0x123U, 0U, 8U, 0x10U),
                                 make_frame(0x1ABCDE01U, CAN_FLAG_EXT, 3U, 0x20U),
                                 make_frame(0x456U, CAN_FLAG_RTR, 2U, 0U)};
    plat_can_status_t status;
    plat_can_rx_t received;

    for (size_t i = 0U; i < (sizeof(sent) / sizeof(sent[0])); i++) {
        const uint64_t before_ns = posix_monotonic_ns();

        TEST_ASSERT_TRUE(s_peer.send(&s_peer, &sent[i]));
        received = next_rx();
        assert_frames_equal(&sent[i], &received.frame);

        /* Its end of frame lies between the moment it was sent and the moment the driver
         * had it. */
        TEST_ASSERT_TRUE(received.eof_ns >= before_ns);
        TEST_ASSERT_TRUE(received.eof_ns <= posix_monotonic_ns());
    }

    TEST_ASSERT_FALSE(plat_can_rx(&received));
    plat_can_get_status(&status);
    TEST_ASSERT_EQUAL_UINT8(PLAT_CAN_ERROR_ACTIVE, status.state);
    TEST_ASSERT_EQUAL_UINT16(0U, status.tec);
    TEST_ASSERT_EQUAL_UINT16(0U, status.rec);
    TEST_ASSERT_EQUAL_UINT32(0U, status.rx_fifo_overruns);

    /* Each delivery was measured against its end of frame, and nothing is left in the FIFO. */
    {
        posix_can_stats_t stats;

        posix_can_get_stats(&stats);
        TEST_ASSERT_EQUAL_UINT64(sizeof(sent) / sizeof(sent[0]), stats.rx_delivered);
        TEST_ASSERT_TRUE(stats.rx_lag_max_ns >= stats.rx_lag_mean_ns);
        TEST_ASSERT_EQUAL_UINT32(0U, stats.rx_fifo_pending);
    }
}

static void test_a_receive_fifo_that_is_not_drained_overruns_and_keeps_the_oldest_frames(void)
{
    enum { SENT = (int)PLAT_CAN_RX_FIFO_DEPTH + 88, BURST = 16 };
    plat_can_status_t status;
    plat_can_rx_t received;
    uint32_t drained = 0U;

    for (uint32_t n = 0U; n < (uint32_t)SENT; n++) {
        const can_frame_t frame = numbered_frame(0x200U, n);

        TEST_ASSERT_TRUE(s_peer.send(&s_peer, &frame));

        /* The modelled bus carries one frame at a time and the model holds only a few that
         * arrive early, so the sender stays in step with it. */
        if ((n % (uint32_t)BURST) == ((uint32_t)BURST - 1U)) {
            wait_for_model_rx((uint64_t)n + 1U);
        }
    }

    wait_for_model_rx((uint64_t)SENT);
    plat_can_get_status(&status);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SENT - PLAT_CAN_RX_FIFO_DEPTH, status.rx_fifo_overruns);

    /* Every frame the model delivered is accounted for: lost to the overrun or in the FIFO. */
    {
        posix_can_stats_t stats;

        posix_can_get_stats(&stats);
        TEST_ASSERT_EQUAL_UINT64((uint64_t)SENT, stats.rx_delivered);
        TEST_ASSERT_EQUAL_UINT32(PLAT_CAN_RX_FIFO_DEPTH, stats.rx_fifo_pending);
    }

    /* What the FIFO kept is the beginning of the sequence: the frames that found it full were
     * the ones dropped, as on a controller whose FIFO is locked against overwriting. */
    while (plat_can_rx(&received)) {
        TEST_ASSERT_EQUAL_UINT32(drained, frame_number(&received.frame));
        drained++;
    }

    TEST_ASSERT_EQUAL_UINT32(PLAT_CAN_RX_FIFO_DEPTH, drained);

    {
        posix_can_stats_t stats;

        posix_can_get_stats(&stats);
        TEST_ASSERT_EQUAL_UINT32(0U, stats.rx_fifo_pending);
    }

    /* The overrun is over as soon as there is room. */
    {
        const can_frame_t frame = numbered_frame(0x200U, 0xBEEFU);

        TEST_ASSERT_TRUE(s_peer.send(&s_peer, &frame));
        received = next_rx();
        TEST_ASSERT_EQUAL_UINT32(0xBEEFU, frame_number(&received.frame));
    }
}

static void test_a_frame_nobody_acknowledges_stays_until_the_driver_aborts_it(void)
{
    const can_frame_t stuck = make_frame(0x111U, 0U, 4U, 0x30U);
    const can_frame_t fresh = make_frame(0x321U, 0U, 2U, 0x40U);
    bool aborted[PLAT_CAN_TX_MAILBOXES] = {false};
    plat_can_event_t event;
    can_frame_t heard;

    (void)sim("can.fault noack");
    TEST_ASSERT_TRUE(plat_can_tx(&stuck, 7U));

    /* The controller retries by itself. Sixteen failed attempts make it error passive, and
     * there it stays: a node that is alone on the bus never goes bus off. */
    event = next_state();
    TEST_ASSERT_EQUAL_UINT8(PLAT_CAN_ERROR_PASSIVE, event.state);
    TEST_ASSERT_EQUAL_UINT16(128U, event.tec);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, peer_hears(&heard, 20), "an unacknowledged frame arrived");
    TEST_ASSERT_FALSE_MESSAGE(take_event(&event), "an outcome without an abort");

    /* The abort gets the frame back: one event, whether the frame was waiting or in the
     * middle of an attempt. */
    plat_can_abort_tx();
    event = next_outcome();
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_ABORTED, event.kind);
    TEST_ASSERT_EQUAL_UINT32(7U, event.handle);

    /* The same with every mailbox loaded. */
    for (uint32_t i = 0U; i < PLAT_CAN_TX_MAILBOXES; i++) {
        const can_frame_t frame = make_frame(0x400U + i, 0U, 1U, (uint8_t)i);

        TEST_ASSERT_TRUE(plat_can_tx(&frame, 20U + i));
    }

    plat_can_abort_tx();

    for (uint32_t n = 0U; n < PLAT_CAN_TX_MAILBOXES; n++) {
        uint32_t index;

        event = next_outcome();
        TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_ABORTED, event.kind);
        TEST_ASSERT_TRUE((event.handle - 20U) < PLAT_CAN_TX_MAILBOXES);
        index = (event.handle - 20U) % PLAT_CAN_TX_MAILBOXES;
        TEST_ASSERT_FALSE_MESSAGE(aborted[index], "a frame was reported twice");
        aborted[index] = true;
    }

    /* With the bus back, the first and only thing the network hears is what the driver
     * sends now: nothing that was aborted comes out late. */
    (void)sim("can.fault none");
    TEST_ASSERT_TRUE(plat_can_tx(&fresh, 50U));
    event = next_outcome();
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, event.kind);
    TEST_ASSERT_EQUAL_UINT32(50U, event.handle);
    TEST_ASSERT_EQUAL_INT(1, peer_hears(&heard, (int)WAIT_MS));
    assert_frames_equal(&fresh, &heard);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, peer_hears(&heard, 50), "a stale frame arrived");
    assert_nothing_was_lost_on_the_way_to_the_driver();
}

static void test_a_shorted_bus_ends_in_bus_off_and_recovery_waits_for_the_request(void)
{
    const can_frame_t frame = make_frame(0x222U, 0U, 8U, 0x50U);
    plat_can_status_t status;
    plat_can_event_t event;
    can_frame_t heard;
    uint64_t requested_ns;

    (void)sim("can.fault short");
    TEST_ASSERT_TRUE(plat_can_tx(&frame, 5U));

    event = next_state();
    TEST_ASSERT_EQUAL_UINT8(PLAT_CAN_ERROR_PASSIVE, event.state);
    TEST_ASSERT_TRUE(event.tec >= 128U);
    event = next_state();
    TEST_ASSERT_EQUAL_UINT8(PLAT_CAN_BUS_OFF, event.state);
    TEST_ASSERT_TRUE(event.tec >= 256U);
    wait_for_state(PLAT_CAN_BUS_OFF);

    /* The frame is still in its mailbox; the driver takes it back. */
    plat_can_abort_tx();
    event = next_outcome();
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_ABORTED, event.kind);
    TEST_ASSERT_EQUAL_UINT32(5U, event.handle);

    /* A healthy bus alone does not bring the node back. */
    (void)sim("can.fault none");
    nap();
    (void)sim("can.status"); /* one more pass of the hardware thread has run for certain */
    plat_can_get_status(&status);
    TEST_ASSERT_EQUAL_UINT8(PLAT_CAN_BUS_OFF, status.state);
    TEST_ASSERT_FALSE(take_event(&event));

    /* The request does, after 128 occurrences of 11 recessive bits and not earlier. */
    requested_ns = posix_monotonic_ns();
    plat_can_request_restart();
    event = next_state();
    TEST_ASSERT_EQUAL_UINT8(PLAT_CAN_ERROR_ACTIVE, event.state);
    TEST_ASSERT_EQUAL_UINT16(0U, event.tec);
    TEST_ASSERT_EQUAL_UINT16(0U, event.rec);
    TEST_ASSERT_TRUE_MESSAGE((event.eof_ns - requested_ns) >= can_bits_to_ns(128U * 11U, BITRATE),
                             "recovered faster than the standard allows");

    TEST_ASSERT_TRUE(plat_can_tx(&frame, 6U));
    event = next_outcome();
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, event.kind);
    TEST_ASSERT_EQUAL_UINT32(6U, event.handle);
    TEST_ASSERT_EQUAL_INT(1, peer_hears(&heard, (int)WAIT_MS));
    assert_frames_equal(&frame, &heard);
    assert_nothing_was_lost_on_the_way_to_the_driver();
}

/* One record of the bus-off trace as can.trace gives it (posix_can_trace.h). */
typedef struct {
    unsigned long seq;
    char kind[16];
    unsigned long long model_ns;
    unsigned long long pass_ns;
    unsigned long long start_ns;
    unsigned long long submit_ns;
    unsigned long long attempts_failed;
    unsigned int value;
    unsigned int state;
    unsigned int old_state;
    unsigned int tec;
    unsigned int rec;
    unsigned long long frames;
} trace_entry_t;

#define TRACE_ENTRIES 128U

/* Every record of the trace, read with as many can.trace requests as it takes; returns the
 * number read. The numbers are checked to follow each other from 0 on. */
static uint32_t read_trace(trace_entry_t *entries)
{
    uint32_t count = 0U;
    unsigned long newest = 1UL;

    while (count < newest) {
        char command[32];
        const char *reply;
        const char *at;

        (void)snprintf(command, sizeof(command), "can.trace %u", (unsigned int)count);
        reply = sim(command);
        newest = (unsigned long)sim_number(reply, "newest");
        TEST_ASSERT_TRUE_MESSAGE(newest <= TRACE_ENTRIES, "more trace records than expected");
        TEST_ASSERT_EQUAL_UINT64(0U, sim_number(reply, "oldest"));
        at = strstr(reply, " r");

        while ((at != NULL) && (count < newest)) {
            trace_entry_t *entry = &entries[count];
            int fields;

            /* A record that does not convert fails the count of fields below. */
            /* NOLINTBEGIN(cert-err34-c) */
            fields =
                sscanf(at, " r%lu=%15[^,],%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%llu",
                       &entry->seq, entry->kind, &entry->model_ns, &entry->pass_ns,
                       &entry->start_ns, &entry->submit_ns, &entry->attempts_failed, &entry->value,
                       &entry->state, &entry->old_state, &entry->tec, &entry->rec, &entry->frames);
            /* NOLINTEND(cert-err34-c) */

            TEST_ASSERT_EQUAL_INT_MESSAGE(13, fields, at);
            TEST_ASSERT_EQUAL_UINT32(count, (uint32_t)entry->seq);
            count++;
            at = strstr(&at[2], " r");
        }

        TEST_ASSERT_TRUE_MESSAGE((count == newest) || (at == NULL), "a reply without records");
    }

    return count;
}

/* The transmit error counter after `attempts` attempts destroyed by a short from error active
 * on: +16 each while error active (rule 3 and the bit error in the active flag, rule 4), +8 each
 * from error passive on (rule 3); error passive at 8 attempts (128), bus off at 24 (256). */
static unsigned int tec_after_shorted_attempts(unsigned long long attempts)
{
    return (attempts <= 8U) ? (unsigned int)(16U * attempts)
                            : (unsigned int)(128U + (8U * (attempts - 8U)));
}

static void test_the_trace_records_the_way_into_bus_off_and_out_with_both_times(void)
{
    const can_frame_t first = make_frame(0x233U, 0U, 8U, 0x10U);
    const can_frame_t after = make_frame(0x234U, 0U, 4U, 0x20U);
    trace_entry_t trace[TRACE_ENTRIES];
    plat_can_event_t event;
    plat_can_event_t done;
    can_frame_t heard;
    const trace_entry_t *passive = NULL;
    const trace_entry_t *bus_off = NULL;
    const trace_entry_t *restart = NULL;
    const trace_entry_t *recovery = NULL;
    const trace_entry_t *first_tx = NULL;
    uint64_t fault_ns;
    uint32_t count;
    unsigned int last_tec = 0U;

    (void)memset(trace, 0, sizeof(trace));

    /* A healthy bus leaves no record. */
    TEST_ASSERT_TRUE(plat_can_tx(&first, 1U));
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, next_outcome().kind);
    TEST_ASSERT_EQUAL_INT(1, peer_hears(&heard, (int)WAIT_MS));
    TEST_ASSERT_NOT_NULL(strstr(sim("can.trace"), "oldest=0 newest=0"));

    fault_ns = sim_number(sim("can.fault short"), "t_ns");
    TEST_ASSERT_TRUE(plat_can_tx(&first, 2U));
    TEST_ASSERT_EQUAL_UINT8(PLAT_CAN_ERROR_PASSIVE, next_state().state);
    TEST_ASSERT_EQUAL_UINT8(PLAT_CAN_BUS_OFF, next_state().state);
    plat_can_abort_tx();
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_ABORTED, next_outcome().kind);
    (void)sim("can.fault none");
    plat_can_request_restart();
    event = next_state();
    TEST_ASSERT_EQUAL_UINT8(PLAT_CAN_ERROR_ACTIVE, event.state);
    TEST_ASSERT_TRUE(plat_can_tx(&after, 3U));
    done = next_outcome();
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, done.kind);
    TEST_ASSERT_EQUAL_INT(1, peer_hears(&heard, (int)WAIT_MS));
    assert_frames_equal(&after, &heard);

    count = read_trace(trace);
    TEST_ASSERT_EQUAL_STRING("fault", trace[0].kind);
    TEST_ASSERT_EQUAL_UINT(1U, trace[0].value); /* CAN_BUS_FAULT_SHORT */
    TEST_ASSERT_EQUAL_UINT64(fault_ns, trace[0].pass_ns);
    TEST_ASSERT_EQUAL_UINT64(0U, trace[0].attempts_failed);

    for (uint32_t i = 1U; i < count; i++) {
        const trace_entry_t *entry = &trace[i];

        /* Every time of a step lies at or before the pass that carried it out. */
        TEST_ASSERT_TRUE(entry->model_ns <= entry->pass_ns);
        TEST_ASSERT_TRUE(entry->pass_ns >= trace[i - 1U].pass_ns);

        if (bus_off == NULL) {
            /* On the way up the counter follows the rules attempt by attempt, wherever the
             * passes of the thread happen to look at it. */
            TEST_ASSERT_TRUE(entry->tec > last_tec);
            last_tec = entry->tec;

            if (strcmp(entry->kind, "counters") == 0) {
                TEST_ASSERT_EQUAL_UINT(tec_after_shorted_attempts(entry->attempts_failed),
                                       entry->tec);
            }
        }

        if (strcmp(entry->kind, "state") == 0) {
            if ((entry->old_state == 0U) && (entry->state == 1U)) {
                passive = entry;
            } else if ((entry->old_state == 1U) && (entry->state == 2U)) {
                bus_off = entry;
            } else if ((entry->old_state == 2U) && (entry->state == 0U)) {
                recovery = entry;
            } else {
                TEST_FAIL_MESSAGE("a state change the test did not cause");
            }
        } else if (strcmp(entry->kind, "restart") == 0) {
            restart = entry;
        } else if (strcmp(entry->kind, "first_tx") == 0) {
            first_tx = entry;
        } else {
            TEST_ASSERT_TRUE_MESSAGE((strcmp(entry->kind, "counters") == 0) ||
                                         (strcmp(entry->kind, "fault") == 0),
                                     entry->kind);
        }
    }

    TEST_ASSERT_NOT_NULL(passive);
    TEST_ASSERT_NOT_NULL(bus_off);
    TEST_ASSERT_NOT_NULL(restart);
    TEST_ASSERT_NOT_NULL(recovery);
    TEST_ASSERT_NOT_NULL(first_tx);
    TEST_ASSERT_EQUAL_UINT(128U, passive->tec);
    TEST_ASSERT_EQUAL_UINT(256U, bus_off->tec);
    TEST_ASSERT_EQUAL_UINT64(24U, bus_off->attempts_failed);
    TEST_ASSERT_TRUE(passive->model_ns >= fault_ns);

    /* From error passive to bus off: 16 attempts of 15 bits (a bit error on the start-of-frame
     * bit, then the error frame), each after the intermission and the suspend of an
     * error-passive transmitter, 26 bit times apart. Exact, unless a late pass of the thread
     * moved a start (late_resyncs). */
    if (sim_number(sim("can.status"), "late_resyncs") == 0U) {
        TEST_ASSERT_EQUAL_UINT64(can_bits_to_ns(16U * 26U, BITRATE),
                                 bus_off->model_ns - passive->model_ns);
    } else {
        TEST_ASSERT_TRUE(bus_off->model_ns - passive->model_ns >= can_bits_to_ns(416U, BITRATE));
    }

    /* The request was served while bus off, and the recovery came 128 x 11 bit times after it,
     * to the nanosecond: the model counts from the pass that served the request. */
    TEST_ASSERT_EQUAL_UINT(2U, restart->state);
    TEST_ASSERT_EQUAL_UINT64(restart->pass_ns, restart->model_ns);
    TEST_ASSERT_TRUE(restart->model_ns >= bus_off->model_ns);
    TEST_ASSERT_EQUAL_UINT64(can_bits_to_ns(128U * 11U, BITRATE),
                             recovery->model_ns - restart->model_ns);
    TEST_ASSERT_EQUAL_UINT64(event.eof_ns, recovery->model_ns);
    TEST_ASSERT_EQUAL_UINT(0U, recovery->tec);
    TEST_ASSERT_EQUAL_UINT(0U, recovery->rec);

    /* The first frame after the recovery, with the times its outcome gave the driver, and its
     * number among the frames transmitted: the second (the first went out before the short,
     * the one loaded under it was destroyed and aborted). Every record before it counts one. */
    TEST_ASSERT_EQUAL_UINT(after.id, first_tx->value);
    TEST_ASSERT_EQUAL_UINT64(2U, first_tx->frames);
    TEST_ASSERT_EQUAL_UINT64(1U, trace[0].frames);
    TEST_ASSERT_EQUAL_UINT64(1U, recovery->frames);
    TEST_ASSERT_EQUAL_UINT64(done.eof_ns, first_tx->model_ns);
    TEST_ASSERT_EQUAL_UINT64(done.start_ns, first_tx->start_ns);
    TEST_ASSERT_EQUAL_UINT64(done.submit_ns, first_tx->submit_ns);
    TEST_ASSERT_TRUE(first_tx->submit_ns >= recovery->model_ns);
    TEST_ASSERT_TRUE(first_tx == &trace[count - 1U]);
    assert_nothing_was_lost_on_the_way_to_the_driver();
}

/* A receive error counter at 127 makes the error state follow every single frame: a destroyed
 * one takes the node to error passive (128), a good one back to error active (127). */
static void change_state_once(uint32_t change)
{
    const can_frame_t probe = numbered_frame(0x150U, change);

    if ((change % 2U) == 1U) {
        (void)sim("can.inject rx 1");
        TEST_ASSERT_TRUE(s_peer.send(&s_peer, &probe));
        wait_for_state(PLAT_CAN_ERROR_PASSIVE);
    } else {
        TEST_ASSERT_TRUE(s_peer.send(&s_peer, &probe));
        wait_for_state(PLAT_CAN_ERROR_ACTIVE);
    }
}

static void test_a_driver_that_falls_behind_loses_no_outcome_and_learns_the_last_state(void)
{
    /* Five state changes more than the event FIFO takes; the last one leaves the node in a
     * state other than the last one queued. */
    enum { CHANGES = (int)STATE_EVENTS_QUEUED + 5 };
    _Static_assert((STATE_EVENTS_QUEUED % 2U) == 0U, "the scenario needs an even number");
    bool confirmed[PLAT_CAN_TX_MAILBOXES] = {false};
    posix_can_stats_t stats;
    plat_can_event_t event;
    plat_can_rx_t received;
    can_frame_t heard;
    uint32_t states = 0U;
    uint32_t outcomes = 0U;
    uint32_t good_frames = 0U;

    /* 127 destroyed frames bring the receive error counter to the edge of error passive. */
    (void)sim("can.inject rx 127");

    for (uint32_t n = 1U; n <= 127U; n++) {
        const can_frame_t probe = numbered_frame(0x150U, n);

        TEST_ASSERT_TRUE(s_peer.send(&s_peer, &probe));
        wait_for_rec((uint16_t)n);
    }

    TEST_ASSERT_FALSE_MESSAGE(take_event(&event), "an event before any state change");

    /* The driver takes no events from here on. */
    for (uint32_t change = 1U; change <= (uint32_t)CHANGES; change++) {
        change_state_once(change);
    }

    /* The event FIFO now holds all the state events it takes. Every mailbox is loaded and
     * every frame goes out: each outcome still finds room. */
    for (uint32_t i = 0U; i < PLAT_CAN_TX_MAILBOXES; i++) {
        const can_frame_t frame = make_frame(0x500U + i, 0U, 2U, (uint8_t)i);

        TEST_ASSERT_TRUE(plat_can_tx(&frame, 500U + i));
    }

    for (uint32_t i = 0U; i < PLAT_CAN_TX_MAILBOXES; i++) {
        TEST_ASSERT_EQUAL_INT(1, peer_hears(&heard, (int)WAIT_MS));
    }

    /* Now the driver catches up: first the state changes that were queued, in an unbroken
     * chain (take_event checks that), then all eight outcomes. */
    while (outcomes < PLAT_CAN_TX_MAILBOXES) {
        event = next_event();

        if (event.kind == PLAT_CAN_EVENT_STATE) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, outcomes, "a state event among the outcomes");
            states++;
        } else {
            const uint32_t index = (event.handle - 500U) % PLAT_CAN_TX_MAILBOXES;

            TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, event.kind);
            TEST_ASSERT_TRUE((event.handle - 500U) < PLAT_CAN_TX_MAILBOXES);
            TEST_ASSERT_FALSE(confirmed[index]);
            confirmed[index] = true;
            outcomes++;
        }
    }

    TEST_ASSERT_EQUAL_UINT32(STATE_EVENTS_QUEUED, states);
    TEST_ASSERT_EQUAL_UINT8(PLAT_CAN_ERROR_ACTIVE, s_chain_state);

    /* The changes that found no room were never queued one by one. The state the controller
     * is in arrives as soon as there is room, as one change from the last state reported. */
    event = next_state();
    TEST_ASSERT_EQUAL_UINT8(PLAT_CAN_ERROR_ACTIVE, event.old_state);
    TEST_ASSERT_EQUAL_UINT8(PLAT_CAN_ERROR_PASSIVE, event.state);
    TEST_ASSERT_EQUAL_UINT16(128U, event.rec);
    TEST_ASSERT_EQUAL_UINT16(0U, event.tec);

    /* That was all: the controller has nothing further to say. */
    (void)sim("can.status");
    nap();
    (void)sim("can.status");
    TEST_ASSERT_FALSE(take_event(&event));

    posix_can_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.event_fifo_overruns);
    /* Of the five changes beyond the mark, the 1st, 3rd and 5th found the FIFO at the mark
     * and the driver in another state; the 2nd and 4th led back to the state it knew. */
    TEST_ASSERT_EQUAL_UINT32(3U, stats.state_events_merged);

    /* Every good probe frame was received on the way. */
    while (plat_can_rx(&received)) {
        good_frames++;
        TEST_ASSERT_EQUAL_UINT32(2U * good_frames, frame_number(&received.frame));
    }

    TEST_ASSERT_EQUAL_UINT32((uint32_t)CHANGES / 2U, good_frames);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.rx_fifo_overruns);

    /* And the mailboxes are free again. */
    TEST_ASSERT_TRUE(plat_can_tx(&heard, 600U));
    event = next_outcome();
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, event.kind);
    TEST_ASSERT_EQUAL_UINT32(600U, event.handle);
}

/* docs/bug-log.md, 006: the abort used to take back every request still in the transmit ring
 * when the hardware thread served it, including requests the driver made after the abort had
 * returned. A driver that aborts and loads a fresh frame at once, as can_io's transmit task
 * does, lost the fresh one. */
static void test_a_frame_loaded_right_after_an_abort_is_not_aborted(void)
{
    enum { ROUNDS = 50 };
    uint32_t aborted = 0U;
    can_frame_t heard;

    for (uint32_t round = 0U; round < (uint32_t)ROUNDS; round++) {
        const can_frame_t fresh = numbered_frame(0x321U, round);
        plat_can_event_t event;

        /* Nothing is loaded: the abort has nothing to take back. */
        plat_can_abort_tx();
        TEST_ASSERT_TRUE(plat_can_tx(&fresh, 1000U + round));
        event = next_outcome();
        TEST_ASSERT_EQUAL_UINT32(1000U + round, event.handle);

        if (event.kind == PLAT_CAN_EVENT_TX_ABORTED) {
            aborted++;
        } else {
            TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, event.kind);
            TEST_ASSERT_EQUAL_INT(1, peer_hears(&heard, (int)WAIT_MS));
            assert_frames_equal(&fresh, &heard);
        }
    }

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, aborted, "frames loaded after the abort were aborted");
    assert_nothing_was_lost_on_the_way_to_the_driver();
}

static void test_an_abort_takes_back_the_frames_loaded_before_it_and_no_other(void)
{
    enum { LOADED = 3 };
    const can_frame_t fresh = make_frame(0x0A0U, 0U, 3U, 0x60U);
    bool aborted[LOADED] = {false};
    plat_can_event_t event;
    can_frame_t heard;

    /* Nobody acknowledges: the loaded frames stay in their mailboxes, one of them always in
     * the middle of an attempt. */
    (void)sim("can.fault noack");

    for (uint32_t i = 0U; i < (uint32_t)LOADED; i++) {
        const can_frame_t frame = make_frame(0x410U + i, 0U, 1U, (uint8_t)i);

        TEST_ASSERT_TRUE(plat_can_tx(&frame, 30U + i));
    }

    plat_can_abort_tx();
    TEST_ASSERT_TRUE(plat_can_tx(&fresh, 60U));

    /* The three come back aborted, and only they. */
    for (uint32_t n = 0U; n < (uint32_t)LOADED; n++) {
        uint32_t index;

        event = next_outcome();
        TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_ABORTED, event.kind);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(60U, event.handle, "the frame loaded after the abort");
        TEST_ASSERT_TRUE((event.handle - 30U) < (uint32_t)LOADED);
        index = (event.handle - 30U) % (uint32_t)LOADED;
        TEST_ASSERT_FALSE_MESSAGE(aborted[index], "a frame was reported twice");
        aborted[index] = true;
    }

    /* The fresh frame waits in its mailbox for an acknowledgement, and goes out once there
     * is one: the first and only frame the network hears. */
    (void)sim("can.status"); /* a pass of the hardware thread after the abort has run */

    while (take_event(&event)) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(PLAT_CAN_EVENT_STATE, event.kind, "an outcome too early");
    }

    (void)sim("can.fault none");
    event = next_outcome();
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, event.kind);
    TEST_ASSERT_EQUAL_UINT32(60U, event.handle);
    TEST_ASSERT_EQUAL_INT(1, peer_hears(&heard, (int)WAIT_MS));
    assert_frames_equal(&fresh, &heard);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, peer_hears(&heard, 50), "an aborted frame arrived");
    assert_nothing_was_lost_on_the_way_to_the_driver();
}

static void test_an_injection_needs_a_kind_and_a_count_above_zero(void)
{
    static const char *const refused[] = {"can.inject tx 0", "can.inject rx 0",   "can.inject tx",
                                          "can.inject tx x", "can.inject both 3", "can.inject"};
    const char *reply;

    for (size_t i = 0U; i < (sizeof(refused) / sizeof(refused[0])); i++) {
        reply = sim_expect(refused[i], false);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "usage: can.inject <tx|rx> <count>"),
                                     refused[i]);
    }

    /* None of them destroyed anything: a frame goes out and comes in without an error. */
    {
        const can_frame_t frame = make_frame(0x1A0U, 0U, 1U, 0x70U);
        plat_can_event_t event;
        can_frame_t heard;

        TEST_ASSERT_TRUE(plat_can_tx(&frame, 70U));
        event = next_outcome();
        TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, event.kind);
        TEST_ASSERT_EQUAL_INT(1, peer_hears(&heard, (int)WAIT_MS));
        TEST_ASSERT_TRUE(s_peer.send(&s_peer, &frame));
        (void)next_rx();
    }

    reply = sim("can.status");
    TEST_ASSERT_EQUAL_UINT64(0U, sim_number(reply, "tx_failed"));
    TEST_ASSERT_EQUAL_UINT64(0U, sim_number(reply, "rx_errors"));
    TEST_ASSERT_EQUAL_UINT64(0U, sim_number(reply, "tec"));
}

static void test_the_status_reports_the_transport_and_the_host_lateness_in_full(void)
{
    const can_frame_t frame = make_frame(0x2B0U, 0U, 8U, 0x80U);
    plat_can_event_t event;
    can_frame_t heard;
    posix_can_stats_t stats;
    posix_tick_stats_t tick;
    const char *reply;

    TEST_ASSERT_TRUE(plat_can_tx(&frame, 80U));
    event = next_outcome();
    TEST_ASSERT_EQUAL_INT(PLAT_CAN_EVENT_TX_DONE, event.kind);
    TEST_ASSERT_EQUAL_INT(1, peer_hears(&heard, (int)WAIT_MS));
    /* Two received frames, so that the largest receive lag and the mean differ (they are in
     * nanoseconds and two deliveries are never exactly as late). */
    TEST_ASSERT_TRUE(s_peer.send(&s_peer, &frame));
    (void)next_rx();
    TEST_ASSERT_TRUE(s_peer.send(&s_peer, &frame));
    (void)next_rx();

    /* The keys added last are the ones a reply cut short would lose. */
    reply = sim("can.status");
    TEST_ASSERT_EQUAL_UINT64(1U, sim_number(reply, "transport_tx"));
    TEST_ASSERT_EQUAL_UINT64(0U, sim_number(reply, "transport_tx_failed"));
    TEST_ASSERT_EQUAL_UINT64(2U, sim_number(reply, "transport_rx"));
    TEST_ASSERT_EQUAL_UINT64(0U, sim_number(reply, "transport_rx_failed"));
    TEST_ASSERT_TRUE(sim_number(reply, "wakeup_lag_max_ns") >=
                     sim_number(reply, "wakeup_lag_mean_ns"));
    TEST_ASSERT_TRUE(sim_number(reply, "rx_lag_max_ns") >= sim_number(reply, "rx_lag_mean_ns"));
    TEST_ASSERT_NOT_NULL(strstr(reply, " rx_lag_mean_ns="));
    /* The receive lag is recorded before the frame enters the FIFO, and nothing has been
     * received since: the figures in the reply are the final ones, exactly. */
    posix_can_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(stats.rx_lag_max_ns, sim_number(reply, "rx_lag_max_ns"));
    TEST_ASSERT_EQUAL_UINT64(stats.rx_lag_mean_ns, sim_number(reply, "rx_lag_mean_ns"));
    TEST_ASSERT_EQUAL_UINT64(2U, stats.rx_delivered);
    /* The last key, the ticks the tick code has given up (posix_tick.c). No scheduler runs in
     * this test, so the count cannot change between the reply and this check (it is 0 here).
     * A count that is not 0 is checked against the node's statistics by
     * test_a_stalled_host_is_accounted_as_dropped_ticks in tests/system/test_smoke.py. */
    posix_tick_get_stats(&tick);
    TEST_ASSERT_EQUAL_UINT64(tick.dropped_ticks, sim_number(reply, "ticks_dropped"));
    TEST_ASSERT_TRUE(strlen(reply) < POSIX_SIM_MAX_DATAGRAM);
}

int main(void)
{
    struct sockaddr_in any;
    socklen_t len = sizeof(any);
    int probe;
    int failures;

    UNITY_BEGIN();

    /* Let the kernel pick a free port for simulation control. */
    probe = socket(AF_INET, SOCK_DGRAM, 0);
    (void)memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if ((probe < 0) || (bind(probe, (const struct sockaddr *)&any, sizeof(any)) != 0) ||
        (getsockname(probe, (struct sockaddr *)&any, &len) != 0)) {
        (void)printf("cannot find a free port\n");
        return 1;
    }

    s_sim_port = ntohs(any.sin_port);
    (void)close(probe);

    if (!posix_sim_open(s_sim_port, s_error, sizeof(s_error))) {
        (void)printf("%s\n", s_error);
        return 1;
    }

    s_sim_client = socket(AF_INET, SOCK_DGRAM, 0);

    RUN_TEST(test_frames_leave_by_priority_at_bus_speed_and_each_is_confirmed_once);
    RUN_TEST(test_a_frame_that_is_not_a_can_frame_comes_back_aborted);
    RUN_TEST(test_received_frames_reach_the_driver_in_order_with_their_bus_time);
    RUN_TEST(test_a_receive_fifo_that_is_not_drained_overruns_and_keeps_the_oldest_frames);
    RUN_TEST(test_a_frame_nobody_acknowledges_stays_until_the_driver_aborts_it);
    RUN_TEST(test_a_shorted_bus_ends_in_bus_off_and_recovery_waits_for_the_request);
    RUN_TEST(test_the_trace_records_the_way_into_bus_off_and_out_with_both_times);
    RUN_TEST(test_a_driver_that_falls_behind_loses_no_outcome_and_learns_the_last_state);
    RUN_TEST(test_a_frame_loaded_right_after_an_abort_is_not_aborted);
    RUN_TEST(test_an_abort_takes_back_the_frames_loaded_before_it_and_no_other);
    RUN_TEST(test_an_injection_needs_a_kind_and_a_count_above_zero);
    RUN_TEST(test_the_status_reports_the_transport_and_the_host_lateness_in_full);

    failures = UNITY_END();
    (void)close(s_sim_client);
    posix_sim_close();
    return failures;
}
