/* SPDX-License-Identifier: MIT */
/*
 * The transmit queue, in the order of its header: the two kinds of frame, the choice of the
 * next frame, completion and abort, stale frames in a mailbox, the bus-off policy, and then
 * three scenarios that put all of it together: one bus-off told frame by frame and one fault
 * without bus off told period by period, with every number worked out by hand, and a long
 * random run against a reference model.
 *
 * How to read the tests:
 *
 *   - Every test starts its own queue with start() or start_with(). setUp() fills the queue
 *     with nonsense first, so each test also shows that can_txq_init() needs nothing from the
 *     memory it is given.
 *   - The calls go through thin wrappers that check the accounting identity of the header
 *     after each one. It is therefore asserted after every step of every test, not only in
 *     the random scenario.
 *   - A frame carries a number in its payload, its `tag`, by which a test tells the values of
 *     one message, or the frames of the FIFO, apart.
 *   - Times are nanoseconds; ms() converts from milliseconds.
 *   - Expected numbers are worked out in the comment next to them. The sizes of the queue are
 *     written as the literals of the header (16 slots, a FIFO of 32, at most 8 mailboxes)
 *     rather than as its macros, so that a changed macro shows up here.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "can/can_txq.h"
#include "unity.h"

#define ABORT   CAN_TXQ_REQUEST_ABORT_MAILBOXES
#define RESTART CAN_TXQ_REQUEST_RESTART

/* The queue under test, and a copy of it for the tests that show that a call changed nothing
 * at all. */
static can_txq_t s_queue;
static can_txq_t s_snapshot;

/* xorshift32. The state is set before every test, so a test sees the same "random" numbers on
 * every run and whatever order the tests run in. */
static uint32_t s_random_state;

static uint32_t random_u32(void)
{
    s_random_state ^= s_random_state << 13U;
    s_random_state ^= s_random_state >> 17U;
    s_random_state ^= s_random_state << 5U;
    return s_random_state;
}

void setUp(void)
{
    s_random_state = 0x1CEB00DAU;
    (void)memset(&s_queue, 0xA5, sizeof(s_queue));
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------------------- */

static uint64_t ms(uint64_t milliseconds)
{
    return milliseconds * 1000000ULL;
}

/* A base-format data frame with `tag` as its payload. */
static can_frame_t data_frame(uint32_t id, uint32_t tag)
{
    can_frame_t frame = {.id = id, .flags = 0U, .dlc = 4U, .data = {0U}};

    frame.data[0] = (uint8_t)(tag >> 24U);
    frame.data[1] = (uint8_t)(tag >> 16U);
    frame.data[2] = (uint8_t)(tag >> 8U);
    frame.data[3] = (uint8_t)tag;

    return frame;
}

static uint32_t tag_of(const can_frame_t *frame)
{
    return ((uint32_t)frame->data[0] << 24U) | ((uint32_t)frame->data[1] << 16U) |
           ((uint32_t)frame->data[2] << 8U) | (uint32_t)frame->data[3];
}

static void start_with(uint8_t mailboxes, uint32_t holdoff_initial_ms, uint32_t holdoff_max_ms,
                       uint32_t stable_ms, uint32_t event_timeout_ms)
{
    const can_txq_config_t config = {.mailboxes = mailboxes,
                                     .holdoff_initial_ms = holdoff_initial_ms,
                                     .holdoff_max_ms = holdoff_max_ms,
                                     .stable_ms = stable_ms,
                                     .event_timeout_ms = event_timeout_ms};

    can_txq_init(&s_queue, &config);
}

/* The configuration of most tests: a hold-off of 100 ms that doubles up to 800 ms and returns
 * to 100 ms after one second on the bus. */
static void start(uint8_t mailboxes, uint32_t event_timeout_ms)
{
    start_with(mailboxes, 100U, 800U, 1000U, event_timeout_ms);
}

static const can_txq_stats_t *stats(void)
{
    return can_txq_stats(&s_queue);
}

/* A copy of the counters of one of the 16 slots; a slot without counters fails the test. */
static can_txq_slot_stats_t slot_counts(uint8_t slot)
{
    const can_txq_slot_stats_t *counts = can_txq_slot_stats(&s_queue, slot);
    can_txq_slot_stats_t copy = {0U, 0U, 0U, 0U};

    if (counts == NULL) {
        TEST_FAIL_MESSAGE("a slot below 16 has no counters");
    } else {
        copy = *counts;
    }

    return copy;
}

/* The identities of the header, from the queue's own counters: the queue's, each slot's, and
 * how the two relate. The slots' counters are a share of the queue's: their superseded values
 * add up to the queue's, and the rest of each is a part of the queue's figure, the event
 * frames being the other part. */
static void check_accounting(void)
{
    const can_txq_stats_t *s = stats();
    uint64_t produced = 0U;
    uint64_t sent = 0U;
    uint64_t superseded = 0U;
    uint64_t pending = 0U;

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(s->produced,
                                     s->sent + s->superseded + s->dropped + s->expired + s->pending,
                                     "produced = sent + superseded + dropped + expired + pending");

    for (uint8_t slot = 0U; slot < 16U; slot++) {
        const can_txq_slot_stats_t c = slot_counts(slot);

        TEST_ASSERT_EQUAL_UINT64_MESSAGE(c.produced, c.sent + c.superseded + c.pending,
                                         "slot: produced = sent + superseded + pending");
        TEST_ASSERT_TRUE_MESSAGE(c.pending <= 2U, "a slot holds at most two values");
        produced += c.produced;
        sent += c.sent;
        superseded += c.superseded;
        pending += c.pending;
    }

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(s->superseded, superseded, "superseded: the slots' sum");
    TEST_ASSERT_TRUE_MESSAGE(produced <= s->produced, "produced: the slots' share");
    TEST_ASSERT_TRUE_MESSAGE(sent <= s->sent, "sent: the slots' share");
    TEST_ASSERT_TRUE_MESSAGE(pending <= s->pending, "pending: the slots' share");
}

/* The calls of the queue, each followed by the accounting check. */

static bool publish_frame(uint8_t slot, const can_frame_t *frame, uint64_t stamp_ns)
{
    const bool accepted = can_txq_publish(&s_queue, slot, frame, stamp_ns);

    check_accounting();
    return accepted;
}

static bool publish(uint8_t slot, uint32_t id, uint32_t tag, uint64_t stamp_ns)
{
    const can_frame_t frame = data_frame(id, tag);

    return publish_frame(slot, &frame, stamp_ns);
}

static bool enqueue_frame(const can_frame_t *frame, uint64_t now_ns)
{
    const bool accepted = can_txq_enqueue(&s_queue, frame, now_ns);

    check_accounting();
    return accepted;
}

static bool enqueue(uint32_t id, uint32_t tag, uint64_t now_ns)
{
    const can_frame_t frame = data_frame(id, tag);

    return enqueue_frame(&frame, now_ns);
}

static bool next(uint64_t now_ns, can_frame_t *frame, uint32_t *handle)
{
    const bool handed_out = can_txq_next(&s_queue, now_ns, frame, handle);

    check_accounting();
    return handed_out;
}

/* The header allows a caller that does not want to know about the frame to pass NULL. */
static bool done(uint32_t handle)
{
    const bool known = can_txq_on_tx_done(&s_queue, handle, NULL);

    check_accounting();
    return known;
}

static bool aborted(uint32_t handle)
{
    const bool known = can_txq_on_tx_aborted(&s_queue, handle);

    check_accounting();
    return known;
}

static uint32_t report_state(can_fc_state_t state, uint64_t now_ns)
{
    const uint32_t requests = can_txq_on_state(&s_queue, state, now_ns);

    check_accounting();
    return requests;
}

static uint32_t poll(uint64_t now_ns)
{
    const uint32_t requests = can_txq_poll(&s_queue, now_ns);

    check_accounting();
    return requests;
}

static uint64_t deadline(void)
{
    return can_txq_next_deadline(&s_queue);
}

/* Expectations. Each is a function that takes the line of its caller, behind a macro that
 * supplies it, so that a failure is reported where the test states what it expects. */

/* The six counters of the accounting identity, in its order:
 *     produced = sent + superseded + dropped + expired + pending */
static void expect_counts(UNITY_LINE_TYPE line, uint64_t produced, uint64_t sent,
                          uint64_t superseded, uint64_t dropped, uint64_t expired, uint32_t pending)
{
    const can_txq_stats_t *s = stats();

    UNITY_TEST_ASSERT_EQUAL_UINT64(produced, s->produced, line, "produced");
    UNITY_TEST_ASSERT_EQUAL_UINT64(sent, s->sent, line, "sent");
    UNITY_TEST_ASSERT_EQUAL_UINT64(superseded, s->superseded, line, "superseded");
    UNITY_TEST_ASSERT_EQUAL_UINT64(dropped, s->dropped, line, "dropped");
    UNITY_TEST_ASSERT_EQUAL_UINT64(expired, s->expired, line, "expired");
    UNITY_TEST_ASSERT_EQUAL_UINT32(pending, s->pending, line, "pending");
}

#define EXPECT_COUNTS(produced, sent, superseded, dropped, expired, pending)                       \
    expect_counts(__LINE__, (produced), (sent), (superseded), (dropped), (expired), (pending))

/* The four counters of one slot, in the order of its identity:
 *     produced = sent + superseded + pending */
static void expect_slot_counts(UNITY_LINE_TYPE line, uint8_t slot, uint64_t produced, uint64_t sent,
                               uint64_t superseded, uint32_t pending)
{
    const can_txq_slot_stats_t c = slot_counts(slot);

    UNITY_TEST_ASSERT_EQUAL_UINT64(produced, c.produced, line, "slot produced");
    UNITY_TEST_ASSERT_EQUAL_UINT64(sent, c.sent, line, "slot sent");
    UNITY_TEST_ASSERT_EQUAL_UINT64(superseded, c.superseded, line, "slot superseded");
    UNITY_TEST_ASSERT_EQUAL_UINT32(pending, c.pending, line, "slot pending");
}

#define EXPECT_SLOT_COUNTS(slot, produced, sent, superseded, pending)                              \
    expect_slot_counts(__LINE__, (slot), (produced), (sent), (superseded), (pending))

/* The queue hands out exactly this frame. Returns its handle. */
static uint32_t expect_next_frame(UNITY_LINE_TYPE line, uint64_t now_ns,
                                  const can_frame_t *expected)
{
    can_frame_t frame = data_frame(0x7FFU, 0xFFFFFFFFU);
    uint32_t handle = 0U;
    const bool handed_out = next(now_ns, &frame, &handle);

    UNITY_TEST_ASSERT(handed_out, line, "a frame should have been handed out");
    UNITY_TEST_ASSERT_EQUAL_HEX32(expected->id, frame.id, line, "identifier handed out");
    UNITY_TEST_ASSERT_EQUAL_HEX8(expected->flags, frame.flags, line, "flags handed out");
    UNITY_TEST_ASSERT_EQUAL_UINT8(expected->dlc, frame.dlc, line, "DLC handed out");

    /* A remote frame has no payload to compare. */
    if ((expected->flags & CAN_FLAG_RTR) == 0U) {
        UNITY_TEST_ASSERT_EQUAL_UINT32(tag_of(expected), tag_of(&frame), line,
                                       "payload (tag) handed out");
    }

    return handle;
}

#define EXPECT_NEXT_FRAME(now_ns, expected) expect_next_frame(__LINE__, (now_ns), (expected))

static uint32_t expect_next(UNITY_LINE_TYPE line, uint64_t now_ns, uint32_t id, uint32_t tag)
{
    const can_frame_t expected = data_frame(id, tag);

    return expect_next_frame(line, now_ns, &expected);
}

#define EXPECT_NEXT(now_ns, id, tag) expect_next(__LINE__, (now_ns), (id), (tag))

static void expect_nothing(UNITY_LINE_TYPE line, uint64_t now_ns)
{
    can_frame_t frame = data_frame(0x7FFU, 0xFFFFFFFFU);
    uint32_t handle = 0U;

    UNITY_TEST_ASSERT(!next(now_ns, &frame, &handle), line, "no frame should have been handed out");
}

#define EXPECT_NOTHING(now_ns) expect_nothing(__LINE__, (now_ns))

/* The frame with this handle is confirmed, and what the queue reports about it is this. The
 * structure is filled with the opposite of what is expected first, so a field the queue does
 * not write fails the comparison. That includes the slot of an event frame, which the header
 * documents as 0: it is pre-filled with 0xFF, so a queue that leaves it alone, or reports the
 * slot of whatever the mailbox record held before, fails. */
static void expect_done(UNITY_LINE_TYPE line, uint32_t handle, bool cyclic, uint8_t slot,
                        uint32_t id, uint64_t stamp_ns)
{
    can_txq_done_t info = {
        .cyclic = !cyclic, .slot = (uint8_t)~slot, .id = ~id, .stamp_ns = ~stamp_ns};
    const bool known = can_txq_on_tx_done(&s_queue, handle, &info);

    check_accounting();
    UNITY_TEST_ASSERT(known, line, "the handle should have been accepted");
    UNITY_TEST_ASSERT(info.cyclic == cyclic, line, "cyclic or event frame");
    UNITY_TEST_ASSERT_EQUAL_HEX32(id, info.id, line, "identifier of the frame that went out");
    UNITY_TEST_ASSERT_EQUAL_UINT64(stamp_ns, info.stamp_ns, line,
                                   "stamp of the frame that went out");
    UNITY_TEST_ASSERT_EQUAL_UINT8(slot, info.slot, line, "slot of the frame that went out");
}

#define EXPECT_DONE_CYCLIC(handle, slot, id, stamp_ns)                                             \
    expect_done(__LINE__, (handle), true, (slot), (id), (stamp_ns))
#define EXPECT_DONE_EVENT(handle, id, stamp_ns)                                                    \
    expect_done(__LINE__, (handle), false, 0U, (id), (stamp_ns))

static void take_snapshot(void)
{
    (void)memcpy(&s_snapshot, &s_queue, sizeof(s_queue));
}

/* Every byte of the queue is what it was when the snapshot was taken. */
#define EXPECT_UNCHANGED()                                                                         \
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&s_snapshot, &s_queue, sizeof(s_queue),                       \
                                     "the queue is not exactly as it was")

/* One bus-off from the report to the recovery: the queue serves it with `holdoff_ms`, which
 * means the restart is requested exactly that long after the report and not a nanosecond
 * earlier. The controller is back 3 ms after it was asked. Returns the time of the recovery.
 * For queues without a waiting event frame: its expiry would be a deadline of its own. */
static uint64_t bus_off_and_back(UNITY_LINE_TYPE line, uint64_t at_ns, uint32_t holdoff_ms)
{
    const uint64_t due_ns = at_ns + ms(holdoff_ms);
    const uint64_t back_ns = due_ns + ms(3U);

    UNITY_TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, at_ns), line,
                                  "request when the node goes bus off");
    UNITY_TEST_ASSERT_EQUAL_UINT32(holdoff_ms, stats()->holdoff_ms, line,
                                   "hold-off of this bus-off");
    UNITY_TEST_ASSERT_EQUAL_UINT64(due_ns, deadline(), line, "deadline during the hold-off");
    UNITY_TEST_ASSERT_EQUAL_HEX32(0U, poll(due_ns - 1U), line,
                                  "request one nanosecond before the hold-off is over");
    UNITY_TEST_ASSERT_EQUAL_HEX32(RESTART, poll(due_ns), line, "request when the hold-off is over");
    UNITY_TEST_ASSERT_EQUAL_HEX32(0U, poll(due_ns), line, "request when asked a second time");
    UNITY_TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, back_ns), line,
                                  "request when the node is back");

    return back_ns;
}

#define BUS_OFF_AND_BACK(at_ns, holdoff_ms) bus_off_and_back(__LINE__, (at_ns), (holdoff_ms))

/* ------------------------------------------------------------------------------------------
 * A new queue
 * ---------------------------------------------------------------------------------------- */

static void test_a_new_queue_is_empty_and_on_the_bus(void)
{
    start(3U, 50U);

    EXPECT_COUNTS(0U, 0U, 0U, 0U, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->bus_off_count);
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->restart_requests);
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT32(100U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(5000U)));
    EXPECT_NOTHING(ms(5000U));

    /* "For an error-active controller": what is published is handed out without more ado. */
    TEST_ASSERT_TRUE(publish(0U, 0x100U, 1U, ms(5000U)));
    (void)EXPECT_NEXT(ms(5000U), 0x100U, 1U);
}

/* What a configured number of mailboxes comes to: all 16 messages are published, and the
 * result is how many of them the queue hands out before it declines. */
static uint32_t usable_mailboxes(uint8_t configured)
{
    can_frame_t frame = data_frame(0x7FFU, 0U);
    uint32_t handle = 0U;
    uint32_t count = 0U;

    start(configured, 0U);

    for (uint8_t slot = 0U; slot < 16U; slot++) {
        TEST_ASSERT_TRUE(publish(slot, 0x100U + slot, slot, 0U));
    }

    while (next(0U, &frame, &handle)) {
        count++;
    }

    return count;
}

static void test_init_clamps_the_number_of_mailboxes_to_1_through_8(void)
{
    TEST_ASSERT_EQUAL_UINT32(1U, usable_mailboxes(0U));
    TEST_ASSERT_EQUAL_UINT32(1U, usable_mailboxes(1U));
    TEST_ASSERT_EQUAL_UINT32(5U, usable_mailboxes(5U));
    TEST_ASSERT_EQUAL_UINT32(8U, usable_mailboxes(8U));
    TEST_ASSERT_EQUAL_UINT32(8U, usable_mailboxes(9U));
    TEST_ASSERT_EQUAL_UINT32(8U, usable_mailboxes(255U));
}

static void test_init_raises_a_maximum_holdoff_that_is_below_the_initial_one(void)
{
    uint64_t back_ns;

    /* Initial 300 ms, maximum 100 ms: the maximum becomes 300 ms. The hold-off then has no
     * room to double and stays where it starts. */
    start_with(1U, 300U, 100U, 1000U, 0U);

    back_ns = BUS_OFF_AND_BACK(ms(1000U), 300U);
    TEST_ASSERT_EQUAL_UINT32(300U, stats()->holdoff_ms);
    (void)BUS_OFF_AND_BACK(back_ns + ms(20U), 300U);
}

static void test_init_makes_a_used_queue_new_again(void)
{
    start(2U, 0U);

    /* Use it in every way that leaves something behind: frames waiting and in a mailbox, a
     * recall asked for, a bus-off survived, the hold-off raised and a second bus-off in
     * progress. */
    TEST_ASSERT_TRUE(publish(0U, 0x100U, 1U, 0U));
    TEST_ASSERT_TRUE(publish(1U, 0x200U, 2U, 0U));
    TEST_ASSERT_TRUE(enqueue(0x300U, 3U, 0U));
    (void)EXPECT_NEXT(0U, 0x100U, 1U);
    TEST_ASSERT_TRUE(publish(0U, 0x100U, 4U, 0U));
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(0U));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    (void)BUS_OFF_AND_BACK(ms(1U), 100U);
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(110U)));
    TEST_ASSERT_EQUAL_UINT32(200U, stats()->holdoff_ms);

    start(2U, 50U);

    EXPECT_COUNTS(0U, 0U, 0U, 0U, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->bus_off_count);
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->restart_requests);
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT32(100U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    EXPECT_NOTHING(ms(200U));

    /* Not bus off any more, and both mailboxes are free again. */
    TEST_ASSERT_TRUE(publish(4U, 0x140U, 4U, 0U));
    TEST_ASSERT_TRUE(publish(5U, 0x150U, 5U, 0U));
    (void)EXPECT_NEXT(ms(200U), 0x140U, 4U);
    (void)EXPECT_NEXT(ms(200U), 0x150U, 5U);
    EXPECT_COUNTS(2U, 0U, 0U, 0U, 0U, 2U);
}

/* ------------------------------------------------------------------------------------------
 * Cyclic slots
 * ---------------------------------------------------------------------------------------- */

static void test_publish_refuses_an_invalid_slot_or_frame_and_does_not_count_it(void)
{
    const can_frame_t good = data_frame(0x100U, 1U);
    can_frame_t bad_dlc = good;
    can_frame_t bad_id = good;
    can_frame_t bad_flag = good;

    bad_dlc.dlc = 9U;
    bad_id.id = 0x800U; /* 12 bits in a base frame */
    bad_flag.flags = 0x04U;

    start(3U, 0U);

    TEST_ASSERT_FALSE(publish_frame(16U, &good, 0U)); /* the slots are 0..15 */
    TEST_ASSERT_FALSE(publish_frame(255U, &good, 0U));
    TEST_ASSERT_FALSE(publish_frame(0U, NULL, 0U));
    TEST_ASSERT_FALSE(publish_frame(0U, &bad_dlc, 0U));
    TEST_ASSERT_FALSE(publish_frame(0U, &bad_id, 0U));
    TEST_ASSERT_FALSE(publish_frame(0U, &bad_flag, 0U));
    EXPECT_COUNTS(0U, 0U, 0U, 0U, 0U, 0U);
    EXPECT_NOTHING(0U);

    /* The last slot is a slot. */
    TEST_ASSERT_TRUE(publish_frame(15U, &good, 0U));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);
}

static void test_publishing_again_replaces_the_waiting_value_and_counts_it_as_superseded(void)
{
    uint32_t handle;

    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(3U, 0x120U, 1U, 111U));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);
    TEST_ASSERT_TRUE(publish(3U, 0x120U, 2U, 222U));
    EXPECT_COUNTS(2U, 0U, 1U, 0U, 0U, 1U); /* value 1 will never go out */
    TEST_ASSERT_TRUE(publish(3U, 0x120U, 3U, 333U));
    EXPECT_COUNTS(3U, 0U, 2U, 0U, 0U, 1U);

    /* Another message is not affected by any of this. */
    TEST_ASSERT_TRUE(publish(4U, 0x121U, 7U, 777U));
    EXPECT_COUNTS(4U, 0U, 2U, 0U, 0U, 2U);

    /* One frame per message goes out: its newest value, with the stamp of that value. */
    handle = EXPECT_NEXT(0U, 0x120U, 3U);
    EXPECT_DONE_CYCLIC(handle, 3U, 0x120U, 333U);
    handle = EXPECT_NEXT(0U, 0x121U, 7U);
    EXPECT_DONE_CYCLIC(handle, 4U, 0x121U, 777U);
    EXPECT_NOTHING(0U);
    EXPECT_COUNTS(4U, 2U, 2U, 0U, 0U, 0U);
}

static void test_a_value_in_a_mailbox_is_not_replaced_and_the_new_value_waits_for_it(void)
{
    uint32_t first;
    uint32_t second;

    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(5U, 0x150U, 1U, 1000U));
    first = EXPECT_NEXT(0U, 0x150U, 1U);

    /* Value 1 is in a mailbox. Value 2 does not replace it: it waits in the slot. Two of the
     * three mailboxes are free, but a message never occupies two. */
    TEST_ASSERT_TRUE(publish(5U, 0x150U, 2U, 2000U));
    EXPECT_COUNTS(2U, 0U, 0U, 0U, 0U, 2U);
    EXPECT_NOTHING(0U);

    /* Value 3 replaces the waiting value 2, not value 1 in the mailbox. */
    TEST_ASSERT_TRUE(publish(5U, 0x150U, 3U, 3000U));
    EXPECT_COUNTS(3U, 0U, 1U, 0U, 0U, 2U);
    EXPECT_NOTHING(0U);

    /* What went out is value 1: its stamp comes back, not that of the newest value. */
    EXPECT_DONE_CYCLIC(first, 5U, 0x150U, 1000U);
    EXPECT_COUNTS(3U, 1U, 1U, 0U, 0U, 1U);

    /* Only now is the waiting value loaded. */
    second = EXPECT_NEXT(0U, 0x150U, 3U);
    EXPECT_NOTHING(0U);
    EXPECT_DONE_CYCLIC(second, 5U, 0x150U, 3000U);
    EXPECT_COUNTS(3U, 2U, 1U, 0U, 0U, 0U);
}

static void test_the_waiting_value_is_loaded_once_the_earlier_one_is_aborted(void)
{
    uint32_t first;
    uint32_t second;

    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(5U, 0x150U, 1U, 1000U));
    first = EXPECT_NEXT(0U, 0x150U, 1U);
    TEST_ASSERT_TRUE(publish(5U, 0x150U, 2U, 2000U));
    EXPECT_NOTHING(0U);

    /* The aborted value is not put back in front of the newer one: it is superseded. */
    TEST_ASSERT_TRUE(aborted(first));
    EXPECT_COUNTS(2U, 0U, 1U, 0U, 0U, 1U);

    second = EXPECT_NEXT(0U, 0x150U, 2U);
    EXPECT_DONE_CYCLIC(second, 5U, 0x150U, 2000U);
    EXPECT_NOTHING(0U);
    EXPECT_COUNTS(2U, 1U, 1U, 0U, 0U, 0U);
}

static void test_an_aborted_value_returns_to_its_slot_when_no_newer_one_waits(void)
{
    uint32_t handle;

    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(7U, 0x170U, 1U, 4242U));
    handle = EXPECT_NEXT(0U, 0x170U, 1U);
    TEST_ASSERT_TRUE(aborted(handle));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U); /* neither sent nor lost: still pending */

    /* Back in its slot it is a waiting value like any other. It can be handed out again ... */
    handle = EXPECT_NEXT(0U, 0x170U, 1U);
    TEST_ASSERT_TRUE(aborted(handle));

    /* ... and it can be replaced. */
    TEST_ASSERT_TRUE(publish(7U, 0x170U, 2U, 5353U));
    EXPECT_COUNTS(2U, 0U, 1U, 0U, 0U, 1U);
    handle = EXPECT_NEXT(0U, 0x170U, 2U);
    EXPECT_DONE_CYCLIC(handle, 7U, 0x170U, 5353U);
    EXPECT_NOTHING(0U);
    EXPECT_COUNTS(2U, 1U, 1U, 0U, 0U, 0U);
}

/*
 * Each slot keeps the accounts of its own message while another message and an event frame
 * share the queue with it:
 *
 *   slot 2 (0x200)   values 1, 2, 3   1 sent (3) + 2 superseded (1 in its mailbox, 2 in the slot)
 *   slot 5 (0x100)   value 1          1 sent
 *   FIFO   (0x050)   one frame        1 sent, in no slot's accounts
 *
 * which makes the queue's 5 produced = 3 sent + 2 superseded. A slot beyond the 16 has no
 * counters.
 */
static void test_each_slot_accounts_for_the_values_of_its_own_message(void)
{
    uint32_t slot5_value;
    uint32_t slot2_value;
    uint32_t event;

    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(2U, 0x200U, 1U, 1000U));
    TEST_ASSERT_TRUE(publish(5U, 0x100U, 1U, 1100U));
    slot5_value = EXPECT_NEXT(0U, 0x100U, 1U);
    slot2_value = EXPECT_NEXT(0U, 0x200U, 1U);
    EXPECT_SLOT_COUNTS(2U, 1U, 0U, 0U, 1U);
    EXPECT_SLOT_COUNTS(5U, 1U, 0U, 0U, 1U);

    /* Value 2 waits behind value 1, which is in a mailbox: two pending values of one slot.
     * Value 3 then replaces value 2 in the slot. */
    TEST_ASSERT_TRUE(publish(2U, 0x200U, 2U, 2000U));
    EXPECT_SLOT_COUNTS(2U, 2U, 0U, 0U, 2U);
    TEST_ASSERT_TRUE(publish(2U, 0x200U, 3U, 3000U));
    EXPECT_SLOT_COUNTS(2U, 3U, 0U, 1U, 2U);
    TEST_ASSERT_TRUE(enqueue(0x050U, 9U, 3500U));

    EXPECT_DONE_CYCLIC(slot5_value, 5U, 0x100U, 1100U);
    EXPECT_SLOT_COUNTS(5U, 1U, 1U, 0U, 0U);

    /* Value 1 comes back from its mailbox and finds value 3 waiting: it ends there. */
    TEST_ASSERT_TRUE(aborted(slot2_value));
    EXPECT_SLOT_COUNTS(2U, 3U, 0U, 2U, 1U);

    event = EXPECT_NEXT(0U, 0x050U, 9U);
    slot2_value = EXPECT_NEXT(0U, 0x200U, 3U);
    EXPECT_DONE_CYCLIC(slot2_value, 2U, 0x200U, 3000U);
    EXPECT_DONE_EVENT(event, 0x050U, 3500U);

    EXPECT_SLOT_COUNTS(2U, 3U, 1U, 2U, 0U);
    EXPECT_SLOT_COUNTS(5U, 1U, 1U, 0U, 0U);
    for (uint8_t slot = 0U; slot < 16U; slot++) {
        if ((slot != 2U) && (slot != 5U)) {
            EXPECT_SLOT_COUNTS(slot, 0U, 0U, 0U, 0U);
        }
    }
    EXPECT_COUNTS(5U, 3U, 2U, 0U, 0U, 0U);
    TEST_ASSERT_NULL(can_txq_slot_stats(&s_queue, 16U));
    TEST_ASSERT_NULL(can_txq_slot_stats(&s_queue, 255U));
}

/* ------------------------------------------------------------------------------------------
 * The event FIFO
 * ---------------------------------------------------------------------------------------- */

static void test_enqueue_refuses_an_invalid_frame_and_does_not_count_it(void)
{
    const can_frame_t good = data_frame(0x100U, 1U);
    can_frame_t bad_dlc = good;
    can_frame_t bad_id = good;

    bad_dlc.dlc = 9U;
    bad_id.flags = CAN_FLAG_EXT;
    bad_id.id = 0x20000000U; /* 30 bits in an extended frame */

    start(3U, 0U);

    TEST_ASSERT_FALSE(enqueue_frame(NULL, 0U));
    TEST_ASSERT_FALSE(enqueue_frame(&bad_dlc, 0U));
    TEST_ASSERT_FALSE(enqueue_frame(&bad_id, 0U));
    EXPECT_COUNTS(0U, 0U, 0U, 0U, 0U, 0U);
    EXPECT_NOTHING(0U);

    TEST_ASSERT_TRUE(enqueue_frame(&good, 0U));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);
}

static void test_events_come_out_in_the_order_they_went_in(void)
{
    /* Each frame has a better identifier than the one before it: a queue that sorted by
     * priority would send them in exactly the opposite order. */
    static const uint32_t ids[5] = {0x500U, 0x400U, 0x300U, 0x200U, 0x100U};

    start(3U, 0U);

    for (uint32_t i = 0U; i < 5U; i++) {
        TEST_ASSERT_TRUE(enqueue(ids[i], i, ms(i)));
    }

    EXPECT_COUNTS(5U, 0U, 0U, 0U, 0U, 5U);

    for (uint32_t i = 0U; i < 5U; i++) {
        const uint32_t handle = EXPECT_NEXT(ms(10U), ids[i], i);

        EXPECT_DONE_EVENT(handle, ids[i], ms(i)); /* the stamp is the time of the enqueue */
    }

    EXPECT_NOTHING(ms(10U));
    EXPECT_COUNTS(5U, 5U, 0U, 0U, 0U, 0U);
}

static void test_only_one_event_frame_is_in_a_mailbox_at_a_time(void)
{
    uint32_t first;
    uint32_t second;
    uint32_t cyclic;

    start(8U, 0U);

    TEST_ASSERT_TRUE(enqueue(0x400U, 1U, 0U));
    TEST_ASSERT_TRUE(enqueue(0x300U, 2U, 0U)); /* the better identifier of the two */
    TEST_ASSERT_TRUE(publish(0U, 0x600U, 3U, 0U));

    first = EXPECT_NEXT(0U, 0x400U, 1U);

    /* Seven mailboxes are free, and the second event frame would win arbitration against
     * everything else in the queue. It is not offered while the first is out: from a second
     * mailbox it would overtake the first on the bus. The cyclic frame is offered instead,
     * and after it nothing: the fourth reason the header of can_txq_next() gives for
     * declining, "the only frames waiting are event frames and one event frame is already in
     * a mailbox", with six mailboxes free and the node on the bus. */
    cyclic = EXPECT_NEXT(0U, 0x600U, 3U);
    EXPECT_NOTHING(0U);

    EXPECT_DONE_EVENT(first, 0x400U, 0U);
    second = EXPECT_NEXT(0U, 0x300U, 2U);
    EXPECT_DONE_EVENT(second, 0x300U, 0U);
    EXPECT_DONE_CYCLIC(cyclic, 0U, 0x600U, 0U);
    EXPECT_COUNTS(3U, 3U, 0U, 0U, 0U, 0U);
}

static void test_the_33rd_waiting_event_is_refused_and_counted_as_dropped(void)
{
    start(1U, 0U);

    for (uint32_t i = 1U; i <= 32U; i++) {
        TEST_ASSERT_TRUE(enqueue(0x700U, i, ms(i)));
    }

    EXPECT_COUNTS(32U, 0U, 0U, 0U, 0U, 32U);

    TEST_ASSERT_FALSE(enqueue(0x700U, 33U, ms(33U)));
    EXPECT_COUNTS(33U, 0U, 0U, 1U, 0U, 32U); /* counted as produced and as dropped */
    TEST_ASSERT_FALSE(enqueue(0x700U, 34U, ms(34U)));
    EXPECT_COUNTS(34U, 0U, 0U, 2U, 0U, 32U);

    /* The 32 that were accepted come out in order; the two that were refused are nowhere. */
    for (uint32_t i = 1U; i <= 32U; i++) {
        const uint32_t handle = EXPECT_NEXT(ms(40U), 0x700U, i);

        EXPECT_DONE_EVENT(handle, 0x700U, ms(i));
    }

    EXPECT_NOTHING(ms(40U));
    EXPECT_COUNTS(34U, 32U, 0U, 2U, 0U, 0U);

    /* With room again, a frame is accepted again. */
    TEST_ASSERT_TRUE(enqueue(0x700U, 35U, ms(41U)));
    EXPECT_COUNTS(35U, 32U, 0U, 2U, 0U, 1U);
}

static void test_the_fifo_keeps_its_order_across_many_wraps_of_its_ring(void)
{
    start(1U, 0U);

    /* 1000 frames pass through while 20 are kept waiting, so the ring behind the FIFO is
     * gone round some thirty times and every entry is head and tail in turn. Every seventh
     * frame is aborted once before it goes out, which steps the head back, across the wrap
     * as well. */
    for (uint32_t i = 0U; i < 1000U; i++) {
        TEST_ASSERT_TRUE(enqueue(0x700U, i, i));

        if (i >= 20U) {
            const uint32_t oldest = i - 20U;
            uint32_t handle = EXPECT_NEXT(i, 0x700U, oldest);

            if ((oldest % 7U) == 0U) {
                TEST_ASSERT_TRUE(aborted(handle));
                handle = EXPECT_NEXT(i, 0x700U, oldest);
            }

            EXPECT_DONE_EVENT(handle, 0x700U, oldest);
        }
    }

    EXPECT_COUNTS(1000U, 980U, 0U, 0U, 0U, 20U);
}

static void test_an_event_expires_at_exactly_the_timeout_and_not_a_nanosecond_earlier(void)
{
    const uint64_t enqueued_ns = ms(1000U) + 7U; /* not a round number: nothing is rounded */
    const uint64_t over_ns = enqueued_ns + ms(50U);

    start(1U, 50U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, enqueued_ns));
    TEST_ASSERT_EQUAL_UINT64(over_ns, deadline());

    TEST_ASSERT_EQUAL_HEX32(0U, poll(over_ns - 1U));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);
    TEST_ASSERT_EQUAL_UINT64(over_ns, deadline());

    TEST_ASSERT_EQUAL_HEX32(0U, poll(over_ns));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 1U, 0U);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    EXPECT_NOTHING(over_ns);
}

static void test_next_hands_out_an_event_a_nanosecond_before_its_timeout_but_not_at_it(void)
{
    uint32_t handle;

    start(1U, 50U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, ms(2000U)));
    handle = EXPECT_NEXT(ms(2050U) - 1U, 0x700U, 1U);
    EXPECT_DONE_EVENT(handle, 0x700U, ms(2000U));

    /* Discarded by can_txq_next() itself, without a poll in between, and never returned. */
    TEST_ASSERT_TRUE(enqueue(0x700U, 2U, ms(3000U)));
    EXPECT_NOTHING(ms(3050U));
    EXPECT_COUNTS(2U, 1U, 0U, 0U, 1U, 0U);
}

static void test_events_never_expire_when_the_timeout_is_zero(void)
{
    uint32_t handle;

    start(1U, 0U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, 0U));

    /* A frame waits, but nothing is scheduled for it. */
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());

    /* 0 does not mean "expires at once" ... */
    TEST_ASSERT_EQUAL_HEX32(0U, poll(0U));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);

    /* ... and it does not mean "expires in the end": not by a poll, not when another frame
     * is enqueued, not when the frame is asked for. */
    TEST_ASSERT_EQUAL_HEX32(0U, poll(UINT64_MAX));
    TEST_ASSERT_TRUE(enqueue(0x700U, 2U, UINT64_MAX));
    EXPECT_COUNTS(2U, 0U, 0U, 0U, 0U, 2U);
    handle = EXPECT_NEXT(UINT64_MAX, 0x700U, 1U);
    EXPECT_DONE_EVENT(handle, 0x700U, 0U);
    EXPECT_COUNTS(2U, 1U, 0U, 0U, 0U, 1U);
}

static void test_expired_events_are_skipped_without_using_a_mailbox(void)
{
    uint32_t handle;

    start(1U, 50U);

    TEST_ASSERT_TRUE(enqueue(0x701U, 1U, 0U));
    TEST_ASSERT_TRUE(enqueue(0x702U, 2U, ms(10U)));
    TEST_ASSERT_TRUE(enqueue(0x703U, 3U, ms(30U)));

    /* At 60 ms the first frame is 60 ms old and the second exactly 50 ms: both are over. The
     * third is 30 ms old. The one mailbox there is goes to the third. */
    handle = EXPECT_NEXT(ms(60U), 0x703U, 3U);
    EXPECT_COUNTS(3U, 0U, 0U, 0U, 2U, 1U);
    EXPECT_DONE_EVENT(handle, 0x703U, ms(30U));
    EXPECT_COUNTS(3U, 1U, 0U, 0U, 2U, 0U);
}

static void test_an_expired_event_does_not_keep_a_cyclic_frame_from_the_mailbox(void)
{
    uint32_t handle;

    start(1U, 50U);

    TEST_ASSERT_TRUE(enqueue(0x050U, 1U, ms(100U))); /* would beat the cyclic frame */
    TEST_ASSERT_TRUE(publish(0U, 0x300U, 2U, ms(100U)));

    /* At 150 ms the event frame is over, so the mailbox goes to the cyclic frame. */
    handle = EXPECT_NEXT(ms(150U), 0x300U, 2U);
    EXPECT_COUNTS(2U, 0U, 0U, 0U, 1U, 1U);
    EXPECT_DONE_CYCLIC(handle, 0U, 0x300U, ms(100U));
    EXPECT_COUNTS(2U, 1U, 0U, 0U, 1U, 0U);
}

static void test_expiry_does_not_wait_for_a_free_mailbox_or_for_the_bus(void)
{
    start(1U, 50U);

    /* The only mailbox is taken by a cyclic frame. Nothing can be handed out, but the event
     * frame that is over is discarded all the same. */
    TEST_ASSERT_TRUE(publish(0U, 0x100U, 1U, 0U));
    (void)EXPECT_NEXT(0U, 0x100U, 1U);
    TEST_ASSERT_TRUE(enqueue(0x700U, 2U, 0U));
    EXPECT_NOTHING(ms(50U));
    EXPECT_COUNTS(2U, 0U, 0U, 0U, 1U, 1U);

    /* The same while the node is off the bus. */
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(60U)));
    TEST_ASSERT_TRUE(enqueue(0x700U, 3U, ms(60U)));
    EXPECT_NOTHING(ms(110U));
    EXPECT_COUNTS(3U, 0U, 0U, 0U, 2U, 1U);
}

static void test_enqueue_discards_expired_frames_before_it_judges_whether_the_fifo_is_full(void)
{
    uint32_t handle;

    start(1U, 50U);

    for (uint32_t i = 1U; i <= 32U; i++) {
        TEST_ASSERT_TRUE(enqueue(0x700U, i, 0U));
    }

    /* One nanosecond before the timeout the FIFO is full of frames that can still go out. */
    TEST_ASSERT_FALSE(enqueue(0x700U, 33U, ms(50U) - 1U));
    EXPECT_COUNTS(33U, 0U, 0U, 1U, 0U, 32U);

    /* At the timeout all 32 are over, and they no longer keep a new frame out. */
    TEST_ASSERT_TRUE(enqueue(0x700U, 34U, ms(50U)));
    EXPECT_COUNTS(34U, 0U, 0U, 1U, 32U, 1U);

    handle = EXPECT_NEXT(ms(50U), 0x700U, 34U);
    EXPECT_DONE_EVENT(handle, 0x700U, ms(50U));
    EXPECT_COUNTS(34U, 1U, 0U, 1U, 32U, 0U);
}

static void test_an_event_in_a_mailbox_does_not_expire(void)
{
    uint32_t handle;

    start(1U, 50U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, 0U));
    handle = EXPECT_NEXT(ms(10U), 0x700U, 1U);

    /* It was loaded in time, and in its mailbox it is not discarded: only the controller can
     * take it out of there. At 0 + 50 ms the queue asks for it back, once, and moves nothing
     * itself (the recall has tests of its own below). If the attempt that is on the wire
     * succeeds all the same, the frame has been sent, and counts as sent. */
    TEST_ASSERT_EQUAL_UINT64(ms(50U), deadline());
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(500U)));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1000U)));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);
    EXPECT_DONE_EVENT(handle, 0x700U, 0U);
    EXPECT_COUNTS(1U, 1U, 0U, 0U, 0U, 0U);
}

static void test_an_aborted_event_is_still_timed_from_its_enqueue(void)
{
    uint32_t handle;

    start(1U, 50U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, ms(100U)));
    handle = EXPECT_NEXT(ms(110U), 0x700U, 1U);
    TEST_ASSERT_TRUE(aborted(handle));

    /* Back in the FIFO it has its old place and its old time: over at 100 + 50 = 150 ms. */
    TEST_ASSERT_EQUAL_UINT64(ms(150U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(150U) - 1U));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(150U)));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 1U, 0U);

    /* A frame that comes back after its time is discarded without being offered again. */
    TEST_ASSERT_TRUE(enqueue(0x700U, 2U, ms(200U)));
    handle = EXPECT_NEXT(ms(210U), 0x700U, 2U);
    TEST_ASSERT_TRUE(aborted(handle));
    EXPECT_NOTHING(ms(300U));
    EXPECT_COUNTS(2U, 0U, 0U, 0U, 2U, 0U);
}

/* ------------------------------------------------------------------------------------------
 * Which frame next
 * ---------------------------------------------------------------------------------------- */

/*
 * Six frames, published in an order that is neither that of their keys nor that of their
 * identifiers read as numbers. Their arbitration keys (can_frame.c: the 11 base identifier
 * bits from bit 21 up, RTR or SRR in bit 20, IDE in bit 19):
 *
 *   slot 1   base 0x100, data             0x100 << 21                    = 0x20000000
 *   slot 3   base 0x100, remote           the same with RTR              = 0x20100000
 *   slot 2   extended 0x04000000, data    base part 0x100, SRR and IDE   = 0x20180000
 *   event    base 0x180, data             0x180 << 21                    = 0x30000000
 *   slot 4   base 0x200, data                                            = 0x40000000
 *   slot 0   base 0x300, data                                            = 0x60000000
 *
 * By slot number, slot 0 would go first; by identifier value, the extended frame would go
 * last. In arbitration it is third.
 */
static void test_the_frame_with_the_smallest_arbitration_key_goes_first(void)
{
    const can_frame_t base_100 = data_frame(0x100U, 1U);
    can_frame_t remote_100 = data_frame(0x100U, 2U);
    can_frame_t extended = data_frame(0x04000000U, 3U);
    const can_frame_t event_180 = data_frame(0x180U, 4U);
    const can_frame_t base_200 = data_frame(0x200U, 5U);
    const can_frame_t base_300 = data_frame(0x300U, 6U);
    uint32_t handle;

    remote_100.flags = CAN_FLAG_RTR;
    extended.flags = CAN_FLAG_EXT;

    start(1U, 0U); /* one mailbox: the order of the hand-outs is the order of the ranks */

    TEST_ASSERT_TRUE(publish_frame(0U, &base_300, 0U));
    TEST_ASSERT_TRUE(publish_frame(1U, &base_100, 0U));
    TEST_ASSERT_TRUE(publish_frame(2U, &extended, 0U));
    TEST_ASSERT_TRUE(publish_frame(3U, &remote_100, 0U));
    TEST_ASSERT_TRUE(publish_frame(4U, &base_200, 0U));
    TEST_ASSERT_TRUE(enqueue_frame(&event_180, 0U));

    handle = EXPECT_NEXT_FRAME(0U, &base_100);
    EXPECT_DONE_CYCLIC(handle, 1U, 0x100U, 0U);
    handle = EXPECT_NEXT_FRAME(0U, &remote_100);
    EXPECT_DONE_CYCLIC(handle, 3U, 0x100U, 0U);
    handle = EXPECT_NEXT_FRAME(0U, &extended);
    EXPECT_DONE_CYCLIC(handle, 2U, 0x04000000U, 0U);
    handle = EXPECT_NEXT_FRAME(0U, &event_180);
    EXPECT_DONE_EVENT(handle, 0x180U, 0U);
    handle = EXPECT_NEXT_FRAME(0U, &base_200);
    EXPECT_DONE_CYCLIC(handle, 4U, 0x200U, 0U);
    handle = EXPECT_NEXT_FRAME(0U, &base_300);
    EXPECT_DONE_CYCLIC(handle, 0U, 0x300U, 0U);
    EXPECT_NOTHING(0U);
    EXPECT_COUNTS(6U, 6U, 0U, 0U, 0U, 0U);
}

static void test_only_the_oldest_event_competes_with_the_cyclic_slots(void)
{
    uint32_t handle;

    start(1U, 0U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, 0U)); /* the oldest: the worst identifier of all */
    TEST_ASSERT_TRUE(enqueue(0x050U, 2U, 0U)); /* the best of all, but second in the FIFO */
    TEST_ASSERT_TRUE(publish(0U, 0x300U, 3U, 0U));

    /* 0x300 beats the head of the FIFO, 0x700. The frame behind the head takes no part,
     * whatever its identifier: it goes out when its turn in the FIFO has come. */
    handle = EXPECT_NEXT(0U, 0x300U, 3U);
    EXPECT_DONE_CYCLIC(handle, 0U, 0x300U, 0U);
    handle = EXPECT_NEXT(0U, 0x700U, 1U);
    EXPECT_DONE_EVENT(handle, 0x700U, 0U);
    handle = EXPECT_NEXT(0U, 0x050U, 2U);
    EXPECT_DONE_EVENT(handle, 0x050U, 0U);
    EXPECT_NOTHING(0U);
}

static void test_equal_keys_go_to_the_lower_slot_first_and_to_the_event_last(void)
{
    uint32_t handle;

    start(1U, 0U);

    /* Four frames with one identifier. The event frame is the first to arrive and the slots
     * are filled in no particular order, so neither age nor the order of the calls gives the
     * expected result. The stamp is the number of the slot. */
    TEST_ASSERT_TRUE(enqueue(0x123U, 100U, 100U));
    TEST_ASSERT_TRUE(publish(9U, 0x123U, 9U, 9U));
    TEST_ASSERT_TRUE(publish(2U, 0x123U, 2U, 2U));
    TEST_ASSERT_TRUE(publish(5U, 0x123U, 5U, 5U));

    handle = EXPECT_NEXT(200U, 0x123U, 2U);
    EXPECT_DONE_CYCLIC(handle, 2U, 0x123U, 2U);
    handle = EXPECT_NEXT(200U, 0x123U, 5U);
    EXPECT_DONE_CYCLIC(handle, 5U, 0x123U, 5U);
    handle = EXPECT_NEXT(200U, 0x123U, 9U);
    EXPECT_DONE_CYCLIC(handle, 9U, 0x123U, 9U);
    handle = EXPECT_NEXT(200U, 0x123U, 100U);
    EXPECT_DONE_EVENT(handle, 0x123U, 100U);
    EXPECT_NOTHING(200U);
}

/* All 16 messages are published, with identifiers that rise with the slot. The queue is then
 * loaded until it declines, the oldest frame in a mailbox is confirmed, and so on until
 * nothing is left. At no moment may more than `configured` frames be out, and the queue may
 * only decline when that many are out or nothing is waiting. */
static void check_mailbox_limit(uint8_t configured)
{
    uint32_t handles[8] = {0U};
    uint32_t in_use = 0U;
    uint32_t handed_out = 0U;
    uint32_t confirmed = 0U;

    start(configured, 0U);

    for (uint8_t slot = 0U; slot < 16U; slot++) {
        TEST_ASSERT_TRUE(publish(slot, 0x100U + slot, slot, 0U));
    }

    while (confirmed < 16U) {
        const uint32_t left = 16U - confirmed;
        can_frame_t frame = data_frame(0x7FFU, 0U);
        uint32_t handle = 0U;

        while (next(0U, &frame, &handle)) {
            TEST_ASSERT_LESS_THAN_UINT32(configured, in_use);       /* there was a mailbox for it */
            TEST_ASSERT_EQUAL_HEX32(0x100U + handed_out, frame.id); /* best key first */
            handles[in_use] = handle;
            in_use++;
            handed_out++;
        }

        TEST_ASSERT_EQUAL_UINT32((left < configured) ? left : configured, in_use);

        TEST_ASSERT_TRUE(done(handles[0]));
        in_use--;
        confirmed++;

        for (uint32_t i = 0U; i < in_use; i++) {
            handles[i] = handles[i + 1U];
        }
    }

    TEST_ASSERT_EQUAL_UINT32(16U, handed_out);
    EXPECT_COUNTS(16U, 16U, 0U, 0U, 0U, 0U);
}

static void test_never_more_than_1_mailbox_in_use_when_1_is_configured(void)
{
    check_mailbox_limit(1U);
}

static void test_never_more_than_3_mailboxes_in_use_when_3_are_configured(void)
{
    check_mailbox_limit(3U);
}

static void test_never_more_than_8_mailboxes_in_use_when_8_are_configured(void)
{
    check_mailbox_limit(8U);
}

static void test_an_event_frame_takes_a_mailbox_like_a_cyclic_one(void)
{
    uint32_t event;
    uint32_t cyclic;

    start(2U, 0U);

    TEST_ASSERT_TRUE(enqueue(0x100U, 1U, 0U));
    TEST_ASSERT_TRUE(publish(0U, 0x200U, 2U, 0U));
    TEST_ASSERT_TRUE(publish(1U, 0x300U, 3U, 0U));

    event = EXPECT_NEXT(0U, 0x100U, 1U);
    cyclic = EXPECT_NEXT(0U, 0x200U, 2U);
    EXPECT_NOTHING(0U); /* 0x300 waits: the event frame has one of the two mailboxes */

    EXPECT_DONE_EVENT(event, 0x100U, 0U);
    (void)EXPECT_NEXT(0U, 0x300U, 3U);
    EXPECT_NOTHING(0U);
    EXPECT_DONE_CYCLIC(cyclic, 0U, 0x200U, 0U);
}

static void test_nothing_is_handed_out_while_the_controller_is_bus_off(void)
{
    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(0U, 0x100U, 1U, 0U));
    TEST_ASSERT_TRUE(enqueue(0x200U, 2U, 0U));
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(1U)));
    EXPECT_NOTHING(ms(1U));

    /* Frames are still accepted: that is the buffering. */
    TEST_ASSERT_TRUE(publish(1U, 0x300U, 3U, ms(2U)));
    TEST_ASSERT_TRUE(enqueue(0x400U, 4U, ms(2U)));
    EXPECT_COUNTS(4U, 0U, 0U, 0U, 0U, 4U);
    EXPECT_NOTHING(ms(2U));

    /* Nor once the restart has been requested: having asked is not being back. */
    TEST_ASSERT_EQUAL_HEX32(RESTART, poll(ms(101U)));
    EXPECT_NOTHING(ms(101U));

    /* Back on the bus, the three mailboxes are filled best key first: slot 0, the oldest
     * event frame, slot 1. The second event frame waits for the first. */
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(104U)));
    (void)EXPECT_NEXT(ms(104U), 0x100U, 1U);
    (void)EXPECT_NEXT(ms(104U), 0x200U, 2U);
    (void)EXPECT_NEXT(ms(104U), 0x300U, 3U);
    EXPECT_NOTHING(ms(104U));
    EXPECT_COUNTS(4U, 0U, 0U, 0U, 0U, 4U);
}

static void test_next_needs_both_outputs_and_leaves_them_alone_when_it_declines(void)
{
    can_frame_t frame = data_frame(0x7FFU, 0xFFFFFFFFU);
    uint32_t handle = 0xDEADBEEFU;

    start(3U, 0U);

    /* Nothing is waiting: nothing is written. */
    TEST_ASSERT_FALSE(next(0U, &frame, &handle));
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFU, handle);
    TEST_ASSERT_EQUAL_HEX32(0x7FFU, frame.id);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFU, tag_of(&frame));

    /* A caller that cannot take the frame or the handle gets neither, and the frame stays
     * in the queue for a caller that can. */
    TEST_ASSERT_TRUE(publish(0U, 0x100U, 1U, 0U));
    TEST_ASSERT_FALSE(next(0U, NULL, &handle));
    TEST_ASSERT_FALSE(next(0U, &frame, NULL));
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFU, handle);
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);
    (void)EXPECT_NEXT(0U, 0x100U, 1U);
}

/* "If `frame` or `handle` is NULL, the call returns false and changes nothing at all, expiry
 * included." An event frame that is over by the time of the call stays where it is, byte for
 * byte, until a call that can take a frame comes. */
static void test_next_with_a_null_output_changes_nothing_not_even_expiry(void)
{
    can_frame_t frame = data_frame(0x7FFU, 0xFFFFFFFFU);
    uint32_t handle = 0xDEADBEEFU;

    start(1U, 10U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, 0U));
    take_snapshot();

    /* At 20 ms the frame is 10 ms past its timeout of 10 ms. */
    TEST_ASSERT_FALSE(next(ms(20U), NULL, &handle));
    EXPECT_UNCHANGED();
    TEST_ASSERT_FALSE(next(ms(20U), &frame, NULL));
    EXPECT_UNCHANGED();
    TEST_ASSERT_FALSE(next(ms(20U), NULL, NULL));
    EXPECT_UNCHANGED();
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFU, handle);
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);

    /* A call with both outputs at the same time discards it, which shows that there was
     * something the calls above could have discarded. */
    EXPECT_NOTHING(ms(20U));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 1U, 0U);
}

/* ------------------------------------------------------------------------------------------
 * Completion and abort
 * ---------------------------------------------------------------------------------------- */

static void test_unknown_and_completed_handles_are_refused_and_change_nothing(void)
{
    can_txq_done_t info = {.cyclic = true, .slot = 0xEEU, .id = 0xEEEEEEEEU, .stamp_ns = 0xEEEEU};
    uint32_t first;
    uint32_t second;
    uint32_t again;

    start(3U, 0U);

    /* Nothing was ever handed out: every handle is unknown. */
    take_snapshot();
    TEST_ASSERT_FALSE(can_txq_on_tx_done(&s_queue, 0U, &info));
    TEST_ASSERT_FALSE(can_txq_on_tx_aborted(&s_queue, 0U));
    TEST_ASSERT_FALSE(can_txq_on_tx_done(&s_queue, 0xFFFFFFFFU, NULL));
    TEST_ASSERT_FALSE(can_txq_on_tx_aborted(&s_queue, 0xFFFFFFFFU));
    EXPECT_UNCHANGED();

    TEST_ASSERT_TRUE(publish(0U, 0x100U, 1U, 0U));
    TEST_ASSERT_TRUE(publish(1U, 0x200U, 2U, 0U));
    first = EXPECT_NEXT(0U, 0x100U, 1U);
    second = EXPECT_NEXT(0U, 0x200U, 2U);

    /* Two handles are outstanding. Every other number is refused: the 2048 smallest, which
     * include whatever lies next to the two, and 2048 more spread over all 32 bits. */
    take_snapshot();

    for (uint32_t i = 0U; i < 4096U; i++) {
        const uint32_t candidate = (i < 2048U) ? i : (i * 0x9E3779B1U);

        if ((candidate != first) && (candidate != second)) {
            TEST_ASSERT_FALSE(can_txq_on_tx_done(&s_queue, candidate, &info));
            TEST_ASSERT_FALSE(can_txq_on_tx_aborted(&s_queue, candidate));
        }
    }

    EXPECT_UNCHANGED();

    /* A frame is confirmed once. */
    TEST_ASSERT_TRUE(done(first));
    take_snapshot();
    TEST_ASSERT_FALSE(can_txq_on_tx_done(&s_queue, first, &info));
    TEST_ASSERT_FALSE(can_txq_on_tx_aborted(&s_queue, first));
    EXPECT_UNCHANGED();

    /* A frame is aborted once, and cannot be confirmed afterwards. */
    TEST_ASSERT_TRUE(aborted(second));
    take_snapshot();
    TEST_ASSERT_FALSE(can_txq_on_tx_aborted(&s_queue, second));
    TEST_ASSERT_FALSE(can_txq_on_tx_done(&s_queue, second, &info));
    EXPECT_UNCHANGED();

    /* Handed out again, the aborted frame has a new handle; the old one stays refused. */
    again = EXPECT_NEXT(0U, 0x200U, 2U);
    TEST_ASSERT_NOT_EQUAL_UINT32(second, again);
    take_snapshot();
    TEST_ASSERT_FALSE(can_txq_on_tx_done(&s_queue, second, &info));
    TEST_ASSERT_FALSE(can_txq_on_tx_aborted(&s_queue, second));
    EXPECT_UNCHANGED();

    /* None of the refused calls wrote the caller's structure. */
    TEST_ASSERT_TRUE(info.cyclic);
    TEST_ASSERT_EQUAL_HEX8(0xEEU, info.slot);
    TEST_ASSERT_EQUAL_HEX32(0xEEEEEEEEU, info.id);
    TEST_ASSERT_EQUAL_HEX64(0xEEEEU, info.stamp_ns);

    EXPECT_DONE_CYCLIC(again, 1U, 0x200U, 0U);
    EXPECT_COUNTS(2U, 2U, 0U, 0U, 0U, 0U);
}

/* "slot: cyclic: the slot; event frame: 0". One mailbox, so that the event frame is noted in
 * the very record that held a value of slot 9 a moment before: a queue that left the slot of
 * the record as it was would report 9. */
static void test_an_event_frame_reports_slot_0_from_a_record_that_held_another_slot(void)
{
    uint32_t handle;

    start(1U, 0U);

    TEST_ASSERT_TRUE(publish(9U, 0x190U, 1U, 111U));
    handle = EXPECT_NEXT(0U, 0x190U, 1U);
    EXPECT_DONE_CYCLIC(handle, 9U, 0x190U, 111U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 2U, 222U));
    handle = EXPECT_NEXT(300U, 0x700U, 2U);
    EXPECT_DONE_EVENT(handle, 0x700U, 222U); /* asserts slot 0 */

    /* The same after an abort, which returns the frame through the same record. */
    TEST_ASSERT_TRUE(publish(9U, 0x190U, 3U, 333U));
    handle = EXPECT_NEXT(400U, 0x190U, 3U);
    TEST_ASSERT_TRUE(aborted(handle));
    TEST_ASSERT_TRUE(enqueue(0x080U, 4U, 444U)); /* beats the value of slot 9 */
    handle = EXPECT_NEXT(500U, 0x080U, 4U);
    EXPECT_DONE_EVENT(handle, 0x080U, 444U);
    handle = EXPECT_NEXT(500U, 0x190U, 3U);
    EXPECT_DONE_CYCLIC(handle, 9U, 0x190U, 333U);
    EXPECT_COUNTS(4U, 4U, 0U, 0U, 0U, 0U);
}

static void test_an_aborted_event_goes_back_to_the_head_of_the_fifo(void)
{
    uint32_t handle;

    start(2U, 0U);

    TEST_ASSERT_TRUE(enqueue(0x701U, 1U, ms(1U)));
    TEST_ASSERT_TRUE(enqueue(0x702U, 2U, ms(2U)));
    TEST_ASSERT_TRUE(enqueue(0x703U, 3U, ms(3U)));

    handle = EXPECT_NEXT(ms(5U), 0x701U, 1U);
    TEST_ASSERT_TRUE(aborted(handle));
    EXPECT_COUNTS(3U, 0U, 0U, 0U, 0U, 3U); /* it was pending in its mailbox and still is */

    /* Not behind the two that were enqueued after it: the order is what it was. Twice, to
     * show that the place at the head is there every time. */
    handle = EXPECT_NEXT(ms(5U), 0x701U, 1U);
    TEST_ASSERT_TRUE(aborted(handle));
    handle = EXPECT_NEXT(ms(5U), 0x701U, 1U);
    EXPECT_DONE_EVENT(handle, 0x701U, ms(1U));
    handle = EXPECT_NEXT(ms(5U), 0x702U, 2U);
    EXPECT_DONE_EVENT(handle, 0x702U, ms(2U));
    handle = EXPECT_NEXT(ms(5U), 0x703U, 3U);
    EXPECT_DONE_EVENT(handle, 0x703U, ms(3U));
    EXPECT_NOTHING(ms(5U));
    EXPECT_COUNTS(3U, 3U, 0U, 0U, 0U, 0U);
}

/*
 * The FIFO fills up while its oldest frame is in a mailbox, and then that frame is aborted.
 * What the queue does: the frame returns to the head all the same. The FIFO then holds 33
 * frames, one more than it admits, and refuses new ones until fewer than 32 are waiting.
 * Nothing that was accepted is lost, and no counter moves when the frame returns.
 */
static void test_an_aborted_event_returns_to_the_head_even_when_the_fifo_has_filled_up(void)
{
    uint32_t handle;

    start(1U, 0U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 0U, 0U));
    handle = EXPECT_NEXT(0U, 0x700U, 0U);

    /* Frame 0 is in the mailbox and takes no place in the FIFO: 32 more are accepted. */
    for (uint32_t i = 1U; i <= 32U; i++) {
        TEST_ASSERT_TRUE(enqueue(0x700U, i, i));
    }

    EXPECT_COUNTS(33U, 0U, 0U, 0U, 0U, 33U);
    TEST_ASSERT_FALSE(enqueue(0x700U, 33U, 33U)); /* the 33rd waiting frame */
    EXPECT_COUNTS(34U, 0U, 0U, 1U, 0U, 33U);

    /* Frame 0 comes back. 34 produced = 1 dropped + 33 pending, as before. */
    TEST_ASSERT_TRUE(aborted(handle));
    EXPECT_COUNTS(34U, 0U, 0U, 1U, 0U, 33U);

    /* 33 are waiting: no room. */
    TEST_ASSERT_FALSE(enqueue(0x700U, 34U, 34U));
    EXPECT_COUNTS(35U, 0U, 0U, 2U, 0U, 33U);

    /* Frame 0 is at the head. With it in the mailbox again 32 are waiting: still no room. */
    handle = EXPECT_NEXT(34U, 0x700U, 0U);
    TEST_ASSERT_FALSE(enqueue(0x700U, 35U, 35U));
    EXPECT_COUNTS(36U, 0U, 0U, 3U, 0U, 33U);

    /* It can come back as often as it is aborted. */
    TEST_ASSERT_TRUE(aborted(handle));
    EXPECT_COUNTS(36U, 0U, 0U, 3U, 0U, 33U);
    handle = EXPECT_NEXT(35U, 0x700U, 0U);
    EXPECT_DONE_EVENT(handle, 0x700U, 0U);
    EXPECT_COUNTS(36U, 1U, 0U, 3U, 0U, 32U);

    /* Frame 1 goes to the mailbox: 31 are waiting, and the FIFO takes a frame again. */
    handle = EXPECT_NEXT(35U, 0x700U, 1U);
    TEST_ASSERT_TRUE(enqueue(0x700U, 36U, 36U));
    EXPECT_COUNTS(37U, 1U, 0U, 3U, 0U, 33U);
    EXPECT_DONE_EVENT(handle, 0x700U, 1U);

    /* The rest in order: 2 .. 32, and then 36. Frames 33, 34 and 35 were refused. */
    for (uint32_t i = 2U; i <= 32U; i++) {
        handle = EXPECT_NEXT(36U, 0x700U, i);
        EXPECT_DONE_EVENT(handle, 0x700U, i);
    }

    handle = EXPECT_NEXT(36U, 0x700U, 36U);
    EXPECT_DONE_EVENT(handle, 0x700U, 36U);
    EXPECT_NOTHING(36U);

    /* 37 produced = 34 sent (0 .. 32 and 36) + 3 dropped. */
    EXPECT_COUNTS(37U, 34U, 0U, 3U, 0U, 0U);
}

static void test_handles_are_never_reused_while_outstanding(void)
{
    uint32_t outstanding[8] = {0U};
    uint32_t retired[32] = {0U};
    uint32_t count = 0U;
    uint32_t retired_count = 0U;
    uint32_t retired_next = 0U;
    uint32_t handed_out = 0U;

    start(8U, 0U);

    /* Frames are produced, loaded and completed in a random order for a few thousand rounds.
     * A new handle must differ from every handle that is out, and also from the 32 that were
     * completed last, so that a late or repeated report cannot hit a new frame. */
    for (uint32_t round = 0U; round < 4000U; round++) {
        can_frame_t frame = data_frame(0x7FFU, 0U);
        uint32_t handle = 0U;
        uint32_t completions;

        if ((random_u32() % 4U) == 0U) {
            (void)enqueue(0x300U, round, 0U);
        } else {
            const uint8_t slot = (uint8_t)(random_u32() % 16U);
            const uint32_t id = 0x100U + (random_u32() % 8U);

            (void)publish(slot, id, round, 0U);
        }

        while (next(0U, &frame, &handle)) {
            for (uint32_t i = 0U; i < count; i++) {
                TEST_ASSERT_NOT_EQUAL_UINT32(outstanding[i], handle);
            }

            for (uint32_t i = 0U; i < retired_count; i++) {
                TEST_ASSERT_NOT_EQUAL_UINT32(retired[i], handle);
            }

            TEST_ASSERT_LESS_THAN_UINT32(8U, count);
            outstanding[count] = handle;
            count++;
            handed_out++;
        }

        completions = random_u32() % (count + 1U);

        for (uint32_t n = 0U; n < completions; n++) {
            const uint32_t pick = random_u32() % count;
            const uint32_t completed = outstanding[pick];

            if ((random_u32() % 2U) == 0U) {
                TEST_ASSERT_TRUE(done(completed));
            } else {
                TEST_ASSERT_TRUE(aborted(completed));
            }

            /* From here on the handle is refused. */
            TEST_ASSERT_FALSE(done(completed));
            TEST_ASSERT_FALSE(aborted(completed));

            count--;
            outstanding[pick] = outstanding[count];
            retired[retired_next] = completed;
            retired_next = (retired_next + 1U) % 32U;

            if (retired_count < 32U) {
                retired_count++;
            }
        }
    }

    /* The loop above is worth something only if it handed out a few thousand frames. */
    TEST_ASSERT_GREATER_THAN_UINT32(3000U, handed_out);
}

/* ------------------------------------------------------------------------------------------
 * Stale frames in a mailbox
 * ---------------------------------------------------------------------------------------- */

static void test_a_value_published_while_the_earlier_one_is_in_a_mailbox_recalls_it(void)
{
    uint32_t first;
    uint32_t second;
    uint32_t third;

    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(5U, 0x150U, 1U, 1000U));
    first = EXPECT_NEXT(0U, 0x150U, 1U);

    /* A value in a mailbox with nothing behind it is not stale. */
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1U)));
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);

    /* Value 2 comes while value 1 is still out: value 1 has missed its period. The recall is
     * due at once (deadline 0), and the publish itself moves nothing. */
    TEST_ASSERT_TRUE(publish(5U, 0x150U, 2U, 2000U));
    EXPECT_COUNTS(2U, 0U, 0U, 0U, 0U, 2U);
    TEST_ASSERT_EQUAL_UINT64(0U, deadline());

    /* The next poll asks for the abort, once. */
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(10U)));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(10U)));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());

    /* Asking moved nothing either: value 2 still waits for value 1. */
    EXPECT_COUNTS(2U, 0U, 0U, 0U, 0U, 2U);
    EXPECT_NOTHING(ms(10U));

    /* The controller gives value 1 back. It finds value 2 waiting and ends there, superseded;
     * value 2 goes out. */
    TEST_ASSERT_TRUE(aborted(first));
    EXPECT_COUNTS(2U, 0U, 1U, 0U, 0U, 1U);
    second = EXPECT_NEXT(ms(10U), 0x150U, 2U);
    EXPECT_NOTHING(ms(10U));

    /* Value 2 is not confirmed either before value 3 comes: a second request. And value 4
     * comes before the abort is reported: the situation is still there, so a third. Value 4
     * replaces value 3 in the slot as usual. */
    TEST_ASSERT_TRUE(publish(5U, 0x150U, 3U, 3000U));
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(20U)));
    TEST_ASSERT_TRUE(publish(5U, 0x150U, 4U, 4000U));
    EXPECT_COUNTS(4U, 0U, 2U, 0U, 0U, 2U); /* superseded: values 1 and 3 */
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(21U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(22U)));
    TEST_ASSERT_EQUAL_UINT64(3U, stats()->recall_requests);

    TEST_ASSERT_TRUE(aborted(second));
    third = EXPECT_NEXT(ms(22U), 0x150U, 4U);
    EXPECT_DONE_CYCLIC(third, 5U, 0x150U, 4000U);

    /* 4 produced = 1 sent (value 4) + 3 superseded (values 1, 2, 3) */
    EXPECT_COUNTS(4U, 1U, 3U, 0U, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT64(3U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->bus_off_count);
}

/*
 * The earlier value is confirmed between the publish and the poll. The queue withdraws the note
 * (can_txq.c, recall_due(), says why): the poll asks for nothing and counts nothing, and the
 * deadline stops saying "at once" as soon as the confirmation is in.
 */
static void test_a_recall_is_withdrawn_when_the_earlier_value_is_confirmed_before_the_poll(void)
{
    uint32_t first;
    uint32_t second;
    uint32_t third;

    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(5U, 0x150U, 1U, 1000U));
    first = EXPECT_NEXT(0U, 0x150U, 1U);
    TEST_ASSERT_TRUE(publish(5U, 0x150U, 2U, 2000U));
    TEST_ASSERT_EQUAL_UINT64(0U, deadline());

    /* Value 1 went out after all. Value 2 waits for a mailbox like any value. */
    EXPECT_DONE_CYCLIC(first, 5U, 0x150U, 1000U);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(10U)));
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);
    EXPECT_COUNTS(2U, 1U, 0U, 0U, 0U, 1U);

    /* The same when the newer value has already taken the mailbox by the time of the poll:
     * what is out is the newest value, nothing is stale. */
    second = EXPECT_NEXT(ms(10U), 0x150U, 2U);
    TEST_ASSERT_TRUE(publish(5U, 0x150U, 3U, 3000U));
    TEST_ASSERT_TRUE(done(second));
    third = EXPECT_NEXT(ms(11U), 0x150U, 3U);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(12U)));
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);

    /* A withdrawn note is gone, not postponed. */
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(13U)));
    EXPECT_DONE_CYCLIC(third, 5U, 0x150U, 3000U);
    EXPECT_COUNTS(3U, 3U, 0U, 0U, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);
}

/*
 * Three messages in three mailboxes; one of them is published again. The bus is held dominant,
 * so no attempt is on the wire and all three frames come back at once, reported in an order
 * that is neither that of the slots nor that of the keys. Slots and identifiers:
 *
 *   slot 0  0x300   tag 1 (stamp 10), then tag 4 (stamp 40): the stale one
 *   slot 1  0x100   tag 2 (stamp 20)
 *   slot 2  0x200   tag 3 (stamp 30)
 *
 * Afterwards the two others go out again as they were, and in priority order, followed by the
 * newest value of slot 0. Only tag 1 is superseded.
 */
static void test_a_recall_hands_out_the_frames_of_the_other_slots_again_unchanged(void)
{
    uint32_t h100;
    uint32_t h200;
    uint32_t h300;

    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(0U, 0x300U, 1U, 10U));
    TEST_ASSERT_TRUE(publish(1U, 0x100U, 2U, 20U));
    TEST_ASSERT_TRUE(publish(2U, 0x200U, 3U, 30U));
    h100 = EXPECT_NEXT(ms(1U), 0x100U, 2U);
    h200 = EXPECT_NEXT(ms(1U), 0x200U, 3U);
    h300 = EXPECT_NEXT(ms(1U), 0x300U, 1U);

    TEST_ASSERT_TRUE(publish(0U, 0x300U, 4U, 40U));
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(2U)));
    EXPECT_COUNTS(4U, 0U, 0U, 0U, 0U, 4U);

    TEST_ASSERT_TRUE(aborted(h200));
    TEST_ASSERT_TRUE(aborted(h300));
    TEST_ASSERT_TRUE(aborted(h100));
    EXPECT_COUNTS(4U, 0U, 1U, 0U, 0U, 3U); /* tag 1, and nothing else */

    h100 = EXPECT_NEXT(ms(2U), 0x100U, 2U);
    h200 = EXPECT_NEXT(ms(2U), 0x200U, 3U);
    h300 = EXPECT_NEXT(ms(2U), 0x300U, 4U);
    EXPECT_NOTHING(ms(2U));

    /* With the stamps they were published with. */
    EXPECT_DONE_CYCLIC(h100, 1U, 0x100U, 20U);
    EXPECT_DONE_CYCLIC(h200, 2U, 0x200U, 30U);
    EXPECT_DONE_CYCLIC(h300, 0U, 0x300U, 40U);
    EXPECT_COUNTS(4U, 3U, 1U, 0U, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
}

/*
 * Three event frames, the first in a mailbox, with a timeout of 50 ms:
 *
 *   E1  0x700  enqueued at t0          over at t0 + 50 ms   in the mailbox
 *   E2  0x701  enqueued at t0 + 1 ms   over at t0 + 51 ms   head of the FIFO
 *   E3  0x702  enqueued at t0 + 2 ms   over at t0 + 52 ms
 *
 * t0 is 1000 ms + 7 ns, so that nothing is a round number.
 */
static void test_an_event_in_a_mailbox_is_recalled_at_exactly_its_timeout(void)
{
    const uint64_t t0 = ms(1000U) + 7U;
    const uint64_t over_ns = t0 + ms(50U);
    uint32_t handle;

    start(2U, 50U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, t0));
    TEST_ASSERT_TRUE(enqueue(0x701U, 2U, t0 + ms(1U)));
    TEST_ASSERT_TRUE(enqueue(0x702U, 3U, t0 + ms(2U)));
    handle = EXPECT_NEXT(t0 + ms(5U), 0x700U, 1U);

    /* The next thing to do is the recall of E1, before the expiry of E2 in the FIFO. */
    TEST_ASSERT_EQUAL_UINT64(over_ns, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(over_ns - 1U));
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT64(over_ns, deadline());

    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(over_ns));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    EXPECT_COUNTS(3U, 0U, 0U, 0U, 0U, 3U); /* in its mailbox it does not expire */

    /* Once for this frame. The deadline is now the expiry of E2. */
    TEST_ASSERT_EQUAL_HEX32(0U, poll(over_ns));
    TEST_ASSERT_EQUAL_UINT64(t0 + ms(51U), deadline());

    /* E1 comes back to the head of the FIFO. It is over, so the deadline is its expiry, which
     * has passed, and the next look at the FIFO discards it. */
    TEST_ASSERT_TRUE(aborted(handle));
    EXPECT_COUNTS(3U, 0U, 0U, 0U, 0U, 3U);
    TEST_ASSERT_EQUAL_UINT64(over_ns, deadline());
    handle = EXPECT_NEXT(over_ns, 0x701U, 2U); /* E2 is 49 ms old */
    EXPECT_COUNTS(3U, 0U, 0U, 0U, 1U, 2U);

    /* E2 has a recall of its own, at its own timeout. */
    TEST_ASSERT_EQUAL_UINT64(t0 + ms(51U), deadline());
    EXPECT_DONE_EVENT(handle, 0x701U, t0 + ms(1U));

    /* E1 is never handed out again; E3 follows E2. */
    handle = EXPECT_NEXT(over_ns, 0x702U, 3U);
    EXPECT_DONE_EVENT(handle, 0x702U, t0 + ms(2U));
    EXPECT_NOTHING(over_ns);

    /* 3 produced = 2 sent (E2, E3) + 1 expired (E1) */
    EXPECT_COUNTS(3U, 2U, 0U, 0U, 1U, 0U);
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
}

static void test_an_event_in_a_mailbox_is_never_recalled_when_the_timeout_is_zero(void)
{
    uint32_t handle;

    start(1U, 0U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, 0U));
    handle = EXPECT_NEXT(0U, 0x700U, 1U);

    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(0U));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(3600000U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(UINT64_MAX));
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);
    EXPECT_DONE_EVENT(handle, 0x700U, 0U);
    EXPECT_COUNTS(1U, 1U, 0U, 0U, 0U, 0U);
}

/*
 * Bus off asks for the abort itself; nothing that happens while the controller is bus off asks
 * again. Two mailboxes, timeout 50 ms. The caller is slow to carry the abort out, so the frames
 * stay in their mailboxes for a while:
 *
 *    0 ms   V1 (slot 0) and the event frame E (enqueued at 0) are loaded
 *    1 ms   V2 published: a recall is noted
 *    2 ms   BUS OFF: the abort is requested here, the note is dropped
 *    3 ms   V3 published while bus off: V2 superseded, no note
 *   50 ms   E has been in its mailbox for its timeout: no recall while bus off
 *   61 ms   the aborts are reported: V1 superseded by V3, E back at the head and expired
 */
static void test_while_bus_off_a_stale_frame_does_not_ask_for_a_second_abort(void)
{
    uint32_t value;
    uint32_t event;

    start(2U, 50U);

    TEST_ASSERT_TRUE(publish(0U, 0x100U, 1U, 0U));
    TEST_ASSERT_TRUE(enqueue(0x700U, 2U, 0U));
    value = EXPECT_NEXT(0U, 0x100U, 1U);
    event = EXPECT_NEXT(0U, 0x700U, 2U);

    TEST_ASSERT_TRUE(publish(0U, 0x100U, 3U, ms(1U)));
    TEST_ASSERT_EQUAL_UINT64(0U, deadline());

    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(2U)));
    TEST_ASSERT_EQUAL_UINT64(ms(102U), deadline()); /* the restart, and nothing before it */
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(2U)));

    TEST_ASSERT_TRUE(publish(0U, 0x100U, 4U, ms(3U)));
    TEST_ASSERT_EQUAL_UINT64(ms(102U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(3U)));

    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(50U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(60U)));
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);
    EXPECT_COUNTS(4U, 0U, 1U, 0U, 0U, 3U); /* superseded: V2 */

    TEST_ASSERT_TRUE(aborted(value));
    TEST_ASSERT_TRUE(aborted(event));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(61U)));

    /* 4 produced = 2 superseded (V1, V2) + 1 expired (E) + 1 pending (V3) */
    EXPECT_COUNTS(4U, 0U, 2U, 0U, 1U, 1U);
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_HEX32(RESTART, poll(ms(102U)));
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);
}

/*
 * "The poll asks once for that frame", also when the frame outlives a bus-off in its mailbox.
 * One mailbox, timeout 50 ms. The caller carries out neither abort, so the frame stays where it
 * is throughout:
 *
 *      0 ms   E enqueued and loaded
 *     50 ms   E's timeout: the poll asks for the abort (recall 1)
 *     60 ms   BUS OFF: the abort is asked for again, as on every bus-off
 *    160 ms   the hold-off of 100 ms is over: restart
 *    163 ms   back on the bus with E still in its mailbox and long over. It has been asked back
 *             already, so nothing more is asked; the only deadline is the reset of the raised
 *             hold-off (200 ms) at 163 + 1000 = 1163 ms
 *
 * When E comes back at last it is expired at the head of the FIFO and never handed out.
 */
static void test_an_event_frame_is_recalled_once_even_when_it_outlives_a_bus_off(void)
{
    uint32_t handle;

    start(1U, 50U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, 0U));
    handle = EXPECT_NEXT(0U, 0x700U, 1U);
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(50U)));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);

    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(60U)));
    TEST_ASSERT_EQUAL_HEX32(RESTART, poll(ms(160U)));
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(163U)));

    TEST_ASSERT_EQUAL_UINT64(ms(1163U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(163U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1000U)));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);

    /* 1 produced = 1 expired */
    TEST_ASSERT_TRUE(aborted(handle));
    EXPECT_NOTHING(ms(1000U));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 1U, 0U);
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
}

/*
 * The timeout of the event frame in a mailbox falls on the very nanosecond at which the node
 * goes bus off, or comes back. Timeout 50 ms, E enqueued at 0 and loaded; the caller carries out
 * no abort, so E stays in its mailbox throughout.
 *
 *   - Bus off reported at 50 ms, then the poll at 50 ms: the bus-off has asked for the abort,
 *     the poll asks for nothing more, and the next deadline is the restart at 150 ms.
 *   - The other way round: the poll asks first (recall 1), and the bus-off asks as it always
 *     does.
 *   - Bus off at 10 ms, and the controller is back by itself at exactly 50 ms: the poll at
 *     50 ms asks (recall 1). The next deadline is then the reset of the raised hold-off
 *     (200 ms) at 50 + 1000 = 1050 ms.
 */
static void test_an_event_timeout_at_the_moment_of_a_bus_off_or_recovery_asks_once(void)
{
    start(1U, 50U);
    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, 0U));
    (void)EXPECT_NEXT(0U, 0x700U, 1U);
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(50U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(50U)));
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT64(ms(150U), deadline());

    start(1U, 50U);
    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, 0U));
    (void)EXPECT_NEXT(0U, 0x700U, 1U);
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(50U)));
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(50U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(50U)));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->bus_off_count);

    start(1U, 50U);
    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, 0U));
    (void)EXPECT_NEXT(0U, 0x700U, 1U);
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(10U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(50U) - 1U));
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(50U)));
    TEST_ASSERT_EQUAL_UINT64(ms(50U), deadline());
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(50U)));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT64(ms(1050U), deadline());
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);
}

/*
 * The recall of an event frame in a mailbox is timed as its expiry is: a clock reading from
 * before the enqueue is not "after the timeout", although in unsigned arithmetic it would be
 * 584 years after it. Here the producer read the clock 5 us after the transmit task did, and
 * the transmit task's polls carry the older readings.
 */
static void test_a_clock_reading_from_before_the_enqueue_does_not_recall_an_event_frame(void)
{
    const uint64_t enqueued_ns = ms(1000U);
    uint32_t handle;

    start(1U, 50U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, enqueued_ns));
    handle = EXPECT_NEXT(enqueued_ns - 5000U, 0x700U, 1U);
    TEST_ASSERT_EQUAL_UINT64(ms(1050U), deadline());

    TEST_ASSERT_EQUAL_HEX32(0U, poll(enqueued_ns - 3000U));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(0U));
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT64(ms(1050U), deadline());

    /* At 1000 + 50 ms it is over, and not before. */
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1050U) - 1U));
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(1050U)));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    EXPECT_DONE_EVENT(handle, 0x700U, enqueued_ns);
    EXPECT_COUNTS(1U, 1U, 0U, 0U, 0U, 0U);
}

/*
 * A caller that sleeps until the deadline has to read it again after every call that can make
 * it earlier. A publish and an enqueue can, and so can the controller's reports. Two cases, one
 * mailbox, timeout 50 ms, an event frame E enqueued at 0 and loaded:
 *
 *   - E is asked back at 50 ms; nothing is scheduled after that. When it comes back at 70 ms,
 *     its expiry, at 50 ms, is due at once.
 *   - The node goes bus off at 10 ms and the caller does not carry out the abort. After the
 *     restart at 110 ms nothing is scheduled. When the controller reports that it is back, at
 *     113 ms, E is still in its mailbox and over: its recall is due at once, at 50 ms, before the
 *     reset of the hold-off at 1113 ms.
 */
static void test_the_reports_of_the_controller_can_make_the_deadline_earlier(void)
{
    uint32_t handle;

    start(1U, 50U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, 0U));
    handle = EXPECT_NEXT(0U, 0x700U, 1U);
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(50U)));
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_TRUE(aborted(handle));
    TEST_ASSERT_EQUAL_UINT64(ms(50U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(70U)));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 1U, 0U);

    start(1U, 50U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 2U, 0U));
    handle = EXPECT_NEXT(0U, 0x700U, 2U);
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(10U)));
    TEST_ASSERT_EQUAL_HEX32(RESTART, poll(ms(110U)));
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(113U)));
    TEST_ASSERT_EQUAL_UINT64(ms(50U), deadline());
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(113U)));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT64(ms(1113U), deadline());

    /* 1 produced = 1 expired */
    TEST_ASSERT_TRUE(aborted(handle));
    EXPECT_NOTHING(ms(114U));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 1U, 0U);
}

/*
 * Only a value whose own predecessor is in a mailbox is stale, and a caller that is slow to carry
 * out a request is not asked again for the same stale frame. Two mailboxes, timeout 50 ms. A is
 * slot 0 (0x200), B is slot 1 (0x100), E an event frame (0x080):
 *
 *    0 ms   B#1 and E (enqueued at 0) are loaded; both mailboxes are in use
 *    1 ms   A#1, then A#2 published: A has nothing out, so nothing is stale; A#1 superseded
 *    2 ms   B#2 published: B#1 is stale, the poll asks (recall 1). The caller carries nothing
 *           out for now.
 *    3 ms   A#3 published: A#2 superseded. B#1 is still stale, but this publish did not find
 *           its own predecessor out, so polls at 3, 4 and 49.999999 ms ask for nothing
 *   50 ms   E's timeout: asked once more (recall 2), for E
 *   51 ms   the aborts arrive: B#1 finds B#2 and is superseded, E returns to the head and is
 *           expired by the next hand-out. B#2 and A#3 go out, best key first.
 *
 *   6 produced (B#1, E, A#1, A#2, B#2, A#3) = 2 sent (B#2, A#3) + 3 superseded (A#1, A#2, B#1)
 *                                            + 1 expired (E)
 */
static void test_only_a_value_behind_its_own_predecessor_asks_and_only_once(void)
{
    uint32_t a;
    uint32_t b;
    uint32_t e;

    start(2U, 50U);

    TEST_ASSERT_TRUE(publish(1U, 0x100U, 1U, 0U));
    TEST_ASSERT_TRUE(enqueue(0x080U, 9U, 0U));
    e = EXPECT_NEXT(0U, 0x080U, 9U);
    b = EXPECT_NEXT(0U, 0x100U, 1U);

    TEST_ASSERT_TRUE(publish(0U, 0x200U, 1U, ms(1U)));
    TEST_ASSERT_TRUE(publish(0U, 0x200U, 2U, ms(1U)));
    TEST_ASSERT_EQUAL_UINT64(ms(50U), deadline()); /* E's recall, nothing earlier */
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1U)));
    EXPECT_NOTHING(ms(1U));
    EXPECT_COUNTS(4U, 0U, 1U, 0U, 0U, 3U);

    TEST_ASSERT_TRUE(publish(1U, 0x100U, 2U, ms(2U)));
    TEST_ASSERT_EQUAL_UINT64(0U, deadline());
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(2U)));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);

    TEST_ASSERT_TRUE(publish(0U, 0x200U, 3U, ms(3U)));
    TEST_ASSERT_EQUAL_UINT64(ms(50U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(3U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(4U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(50U) - 1U));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    EXPECT_COUNTS(6U, 0U, 2U, 0U, 0U, 4U);

    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(50U)));
    TEST_ASSERT_EQUAL_UINT64(2U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());

    TEST_ASSERT_TRUE(aborted(b));
    TEST_ASSERT_TRUE(aborted(e));
    EXPECT_COUNTS(6U, 0U, 3U, 0U, 0U, 3U);
    b = EXPECT_NEXT(ms(51U), 0x100U, 2U);
    a = EXPECT_NEXT(ms(51U), 0x200U, 3U);
    EXPECT_NOTHING(ms(51U));
    EXPECT_DONE_CYCLIC(b, 1U, 0x100U, ms(2U));
    EXPECT_DONE_CYCLIC(a, 0U, 0x200U, ms(3U));
    EXPECT_COUNTS(6U, 2U, 3U, 0U, 1U, 0U);
    TEST_ASSERT_EQUAL_UINT64(2U, stats()->recall_requests);
}

/*
 * One mailbox: the recall of a value makes room for a better frame that came meanwhile, and the
 * value then waits in its slot like any other, without asking again. Timeout 50 ms; V is slot 0
 * (0x200), E an event frame (0x100):
 *
 *    0 ms   V#1 loaded
 *   10 ms   V#2 published (recall noted) and E enqueued; the poll asks (recall 1). V#1 comes
 *           back and is superseded; E has the better key and takes the mailbox
 *   20 ms   V#3 published: V#2 superseded. V has nothing out: nothing is asked
 *   60 ms   E's timeout: asked (recall 2). E comes back and is expired; V#3 goes out
 *
 *   4 produced (V#1, V#2, E, V#3) = 1 sent (V#3) + 2 superseded (V#1, V#2) + 1 expired (E)
 */
static void test_a_recall_with_one_mailbox(void)
{
    uint32_t handle;

    start(1U, 50U);

    TEST_ASSERT_TRUE(publish(0U, 0x200U, 1U, 0U));
    handle = EXPECT_NEXT(0U, 0x200U, 1U);

    TEST_ASSERT_TRUE(publish(0U, 0x200U, 2U, ms(10U)));
    TEST_ASSERT_TRUE(enqueue(0x100U, 3U, ms(10U)));
    TEST_ASSERT_EQUAL_UINT64(0U, deadline());
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(10U)));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT64(ms(60U), deadline()); /* E's expiry in the FIFO */

    TEST_ASSERT_TRUE(aborted(handle));
    EXPECT_COUNTS(3U, 0U, 1U, 0U, 0U, 2U);
    handle = EXPECT_NEXT(ms(10U), 0x100U, 3U);
    EXPECT_NOTHING(ms(10U));
    TEST_ASSERT_EQUAL_UINT64(ms(60U), deadline()); /* now E's recall */

    TEST_ASSERT_TRUE(publish(0U, 0x200U, 4U, ms(20U)));
    TEST_ASSERT_EQUAL_UINT64(ms(60U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(20U)));
    EXPECT_COUNTS(4U, 0U, 2U, 0U, 0U, 2U);

    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(60U) - 1U));
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(60U)));
    TEST_ASSERT_EQUAL_UINT64(2U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());

    TEST_ASSERT_TRUE(aborted(handle));
    TEST_ASSERT_EQUAL_UINT64(ms(60U), deadline()); /* its expiry, passed */
    handle = EXPECT_NEXT(ms(60U), 0x200U, 4U);
    EXPECT_COUNTS(4U, 0U, 2U, 0U, 1U, 1U);
    EXPECT_DONE_CYCLIC(handle, 0U, 0x200U, ms(20U));
    EXPECT_COUNTS(4U, 1U, 2U, 0U, 1U, 0U);
    TEST_ASSERT_EQUAL_UINT64(2U, stats()->recall_requests);
}

/*
 * The event frame is timed from its own enqueue, whichever mailbox record holds it. Three
 * mailboxes, timeout 50 ms. The cyclic value C (slot 0, 0x100) carries a sensor stamp of 7 ms
 * and takes record 0; E (0x700), enqueued at 20 ms, takes record 1. E's recall is at 70 ms, not
 * at 7 + 50 = 57 ms. C is then confirmed and record 0 goes to D (slot 1, stamp 30 ms): E's
 * recall stays at 70 ms.
 */
static void test_an_event_frame_in_a_later_mailbox_is_recalled_at_its_own_timeout(void)
{
    uint32_t c;
    uint32_t d;
    uint32_t e;

    start(3U, 50U);

    TEST_ASSERT_TRUE(publish(0U, 0x100U, 1U, ms(7U)));
    TEST_ASSERT_TRUE(enqueue(0x700U, 2U, ms(20U)));
    c = EXPECT_NEXT(ms(20U), 0x100U, 1U);
    e = EXPECT_NEXT(ms(20U), 0x700U, 2U);
    TEST_ASSERT_EQUAL_UINT64(ms(70U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(57U)));

    EXPECT_DONE_CYCLIC(c, 0U, 0x100U, ms(7U));
    TEST_ASSERT_TRUE(publish(1U, 0x200U, 3U, ms(30U)));
    d = EXPECT_NEXT(ms(60U), 0x200U, 3U);
    TEST_ASSERT_EQUAL_UINT64(ms(70U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(70U) - 1U));
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(70U)));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);

    /* Both attempts on the wire succeed. */
    EXPECT_DONE_EVENT(e, 0x700U, ms(20U));
    EXPECT_DONE_CYCLIC(d, 1U, 0x200U, ms(30U));
    EXPECT_COUNTS(3U, 3U, 0U, 0U, 0U, 0U);
}

/*
 * "While the controller is bus off none of it applies" holds until the node is back, not only
 * until the restart has been asked for. Two mailboxes, timeout 50 ms, hold-off 100 ms. The
 * caller carries out no abort, so V#1 and E stay in their mailboxes throughout:
 *
 *     0 ms   V#1 (slot 0, 0x100) and E (0x700, enqueued at 0) loaded
 *    10 ms   BUS OFF: abort requested
 *   110 ms   restart requested; the node is still bus off
 *   120 ms   V#2 published, and E is long over: neither asks, nothing is scheduled
 *   123 ms   back on the bus. E is in its mailbox, over and not yet asked back: due at once
 *            (deadline 50 ms), asked for (recall 1). V#2 has no note: it was published while
 *            bus off. The next deadline is the reset of the hold-off at 1123 ms.
 *
 *   3 produced (V#1, E, V#2) = 1 sent (V#2) + 1 superseded (V#1) + 1 expired (E)
 */
static void test_the_bus_off_exception_lasts_until_the_node_is_back(void)
{
    uint32_t value;
    uint32_t event;

    start(2U, 50U);

    TEST_ASSERT_TRUE(publish(0U, 0x100U, 1U, 0U));
    TEST_ASSERT_TRUE(enqueue(0x700U, 2U, 0U));
    value = EXPECT_NEXT(0U, 0x100U, 1U);
    event = EXPECT_NEXT(0U, 0x700U, 2U);

    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(10U)));
    TEST_ASSERT_EQUAL_HEX32(RESTART, poll(ms(110U)));

    TEST_ASSERT_TRUE(publish(0U, 0x100U, 3U, ms(120U)));
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(120U)));
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);

    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(123U)));
    TEST_ASSERT_EQUAL_UINT64(ms(50U), deadline());
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(123U)));
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT64(ms(1123U), deadline());

    TEST_ASSERT_TRUE(aborted(value));
    TEST_ASSERT_TRUE(aborted(event));
    value = EXPECT_NEXT(ms(124U), 0x100U, 3U);
    EXPECT_NOTHING(ms(124U));
    EXPECT_DONE_CYCLIC(value, 0U, 0x100U, ms(120U));
    EXPECT_COUNTS(3U, 1U, 1U, 0U, 1U, 0U);
    TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
}

/* The permutation of 0 .. 3 with this number, 0 .. 23, read in the factorial number system. */
static void permutation_of_4(uint32_t number, uint32_t *order)
{
    uint32_t left[4] = {0U, 1U, 2U, 3U};
    uint32_t count = 4U;
    uint32_t rest = number;

    for (uint32_t i = 0U; i < 4U; i++) {
        const uint32_t pick = rest % count;

        rest /= count;
        order[i] = left[pick];
        count--;

        for (uint32_t j = pick; j < count; j++) {
            left[j] = left[j + 1U];
        }
    }
}

/*
 * The four frames of a recall come back in each of the 24 orders there are, and every handle is
 * refused as soon as its report is in. Four mailboxes, no timeout:
 *
 *   E   event   0x050   tag 9, enqueued at 0
 *   B   slot 1  0x100   tag 2, stamp 20
 *   C   slot 2  0x200   tag 3, stamp 30
 *   A   slot 0  0x300   tag 1, stamp 10, then tag 4, stamp 40: the stale one
 *
 * Whatever the order, the outcome is the same: A#1 superseded and nothing else, then E, B, C
 * and A#4 handed out in that order with their stamps. 5 produced = 4 sent + 1 superseded.
 */
static void test_a_recall_comes_out_the_same_in_every_order_of_the_abort_reports(void)
{
    for (uint32_t number = 0U; number < 24U; number++) {
        uint32_t order[4] = {0U};
        uint32_t handles[4] = {0U};

        permutation_of_4(number, order);
        start(4U, 0U);

        TEST_ASSERT_TRUE(publish(0U, 0x300U, 1U, 10U));
        TEST_ASSERT_TRUE(publish(1U, 0x100U, 2U, 20U));
        TEST_ASSERT_TRUE(publish(2U, 0x200U, 3U, 30U));
        TEST_ASSERT_TRUE(enqueue(0x050U, 9U, 0U));
        handles[0] = EXPECT_NEXT(ms(1U), 0x050U, 9U);
        handles[1] = EXPECT_NEXT(ms(1U), 0x100U, 2U);
        handles[2] = EXPECT_NEXT(ms(1U), 0x200U, 3U);
        handles[3] = EXPECT_NEXT(ms(1U), 0x300U, 1U);
        TEST_ASSERT_TRUE(publish(0U, 0x300U, 4U, 40U));
        TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(2U)));

        for (uint32_t i = 0U; i < 4U; i++) {
            TEST_ASSERT_TRUE(aborted(handles[order[i]]));

            /* This one and all reported before it are no longer outstanding. */
            for (uint32_t k = 0U; k <= i; k++) {
                TEST_ASSERT_FALSE(aborted(handles[order[k]]));
                TEST_ASSERT_FALSE(done(handles[order[k]]));
            }
        }

        EXPECT_COUNTS(5U, 0U, 1U, 0U, 0U, 4U);
        TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(2U)));

        handles[0] = EXPECT_NEXT(ms(2U), 0x050U, 9U);
        handles[1] = EXPECT_NEXT(ms(2U), 0x100U, 2U);
        handles[2] = EXPECT_NEXT(ms(2U), 0x200U, 3U);
        handles[3] = EXPECT_NEXT(ms(2U), 0x300U, 4U);
        EXPECT_NOTHING(ms(2U));
        EXPECT_DONE_EVENT(handles[0], 0x050U, 0U);
        EXPECT_DONE_CYCLIC(handles[1], 1U, 0x100U, 20U);
        EXPECT_DONE_CYCLIC(handles[2], 2U, 0x200U, 30U);
        EXPECT_DONE_CYCLIC(handles[3], 0U, 0x300U, 40U);
        EXPECT_COUNTS(5U, 4U, 1U, 0U, 0U, 0U);
        TEST_ASSERT_EQUAL_UINT64(1U, stats()->recall_requests);
    }
}

/* ------------------------------------------------------------------------------------------
 * Bus off
 * ---------------------------------------------------------------------------------------- */

static void test_going_bus_off_asks_once_for_the_mailboxes_to_be_aborted(void)
{
    uint32_t handle;

    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(0U, 0x100U, 1U, 0U));
    handle = EXPECT_NEXT(0U, 0x100U, 1U);

    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(10U)));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->bus_off_count);

    /* The request is all that happens. The frame stays noted as being in its mailbox until
     * the caller reports what became of it. */
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);
    TEST_ASSERT_TRUE(aborted(handle));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);

    /* The controller repeats its report: nothing new to do, it is the same bus-off, and the
     * hold-off still counts from the first report (10 + 100 ms). */
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_BUS_OFF, ms(11U)));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->bus_off_count);
    TEST_ASSERT_EQUAL_UINT64(ms(110U), deadline());

    /* Back on the bus and off again: a new bus-off, and a new request. */
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(20U)));
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(30U)));
    TEST_ASSERT_EQUAL_UINT32(2U, stats()->bus_off_count);
}

static void test_restart_is_requested_exactly_once_when_the_holdoff_has_elapsed(void)
{
    const uint64_t off_ns = ms(5000U) + 3U;
    const uint64_t due_ns = off_ns + ms(100U);

    start(3U, 0U);

    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, off_ns));
    TEST_ASSERT_EQUAL_UINT64(due_ns, deadline());

    TEST_ASSERT_EQUAL_HEX32(0U, poll(off_ns));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(due_ns - 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->restart_requests);

    TEST_ASSERT_EQUAL_HEX32(RESTART, poll(due_ns));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->restart_requests);

    /* Once. The node is still bus off, but there is nothing more to ask for or to wait for. */
    TEST_ASSERT_EQUAL_HEX32(0U, poll(due_ns));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(due_ns + ms(10000U)));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->restart_requests);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
}

static void test_holdoff_doubles_with_every_further_bus_off_up_to_the_maximum(void)
{
    /* 100 ms initial, 800 ms maximum: 100, 200, 400, 800, and 800 again. */
    static const uint32_t expected_ms[5] = {100U, 200U, 400U, 800U, 800U};
    uint64_t now_ns = ms(10000U);

    start(3U, 0U);

    for (uint32_t i = 0U; i < 5U; i++) {
        /* Bus off again 20 ms after each recovery: far inside the stable time of 1 s. */
        now_ns = BUS_OFF_AND_BACK(now_ns, expected_ms[i]) + ms(20U);
    }

    TEST_ASSERT_EQUAL_UINT32(5U, stats()->bus_off_count);
    TEST_ASSERT_EQUAL_UINT32(5U, stats()->restart_requests);
    TEST_ASSERT_EQUAL_UINT32(800U, stats()->holdoff_ms);
}

static void test_holdoff_returns_to_its_initial_value_after_the_stable_time(void)
{
    uint64_t back_ns;

    start(3U, 0U);

    /* Two bus-offs in a row: off at 0 ms, back at 0 + 100 + 3 = 103 ms; off at 123 ms, back
     * at 123 + 200 + 3 = 326 ms. A third would have to wait 400 ms. */
    back_ns = BUS_OFF_AND_BACK(0U, 100U);
    back_ns = BUS_OFF_AND_BACK(back_ns + ms(20U), 200U);
    TEST_ASSERT_EQUAL_UINT64(ms(326U), back_ns);
    TEST_ASSERT_EQUAL_UINT32(400U, stats()->holdoff_ms);

    /* The node now stays on the bus. One second after it came back, at 1326 ms and not a
     * nanosecond earlier, the hold-off is 100 ms again. */
    TEST_ASSERT_EQUAL_UINT64(ms(1326U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1326U) - 1U));
    TEST_ASSERT_EQUAL_UINT32(400U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1326U)));
    TEST_ASSERT_EQUAL_UINT32(100U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());

    /* So the next bus-off is treated like a first one. */
    (void)BUS_OFF_AND_BACK(ms(5000U), 100U);
}

static void test_holdoff_stays_raised_when_the_node_goes_bus_off_before_the_stable_time(void)
{
    const uint64_t again_ns = ms(1103U) - 1U;
    uint64_t back_ns;

    start(3U, 0U);

    back_ns = BUS_OFF_AND_BACK(0U, 100U);
    TEST_ASSERT_EQUAL_UINT64(ms(103U), back_ns);
    TEST_ASSERT_EQUAL_UINT32(200U, stats()->holdoff_ms);

    /* One nanosecond before the stable time is over (103 + 1000 = 1103 ms) the node is bus
     * off again: this bus-off is served with 200 ms. The helper polls at again + 200 ms,
     * which is after 1103 ms: the stable time that had been running is void, nothing was
     * reset in the middle of the bus-off. */
    back_ns = BUS_OFF_AND_BACK(again_ns, 200U);

    /* The stable time starts afresh with the new recovery, and the hold-off is raised once
     * more. */
    TEST_ASSERT_EQUAL_UINT32(400U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_UINT64(back_ns + ms(1000U), deadline());
}

static void test_the_stable_time_counts_at_the_next_bus_off_even_if_nobody_polled(void)
{
    uint64_t back_ns;

    start(3U, 0U);

    back_ns = BUS_OFF_AND_BACK(0U, 100U);
    TEST_ASSERT_EQUAL_UINT32(200U, stats()->holdoff_ms);

    /* Nobody polls. Exactly one second after the recovery the node goes bus off. It has been
     * on the bus for the stable time, so this is a first bus-off again: 100 ms. */
    (void)BUS_OFF_AND_BACK(back_ns + ms(1000U), 100U);
}

static void test_error_passive_is_not_bus_off_and_changes_nothing(void)
{
    uint32_t handle;

    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(0U, 0x100U, 1U, 0U));

    take_snapshot();
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_PASSIVE, ms(5U)));
    EXPECT_UNCHANGED();
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(6U)));
    EXPECT_UNCHANGED();

    /* An error-passive node still transmits, and nothing is counted or scheduled for it. */
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_PASSIVE, ms(7U)));
    handle = EXPECT_NEXT(ms(7U), 0x100U, 1U);
    EXPECT_DONE_CYCLIC(handle, 0U, 0x100U, 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->bus_off_count);
    TEST_ASSERT_EQUAL_UINT32(100U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(5000U)));
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->restart_requests);
}

static void test_error_passive_does_not_start_the_stable_time_again(void)
{
    uint64_t back_ns;

    start(3U, 0U);

    back_ns = BUS_OFF_AND_BACK(0U, 100U);
    TEST_ASSERT_EQUAL_UINT64(ms(103U), back_ns);
    TEST_ASSERT_EQUAL_UINT32(200U, stats()->holdoff_ms);

    /* Error passive half a second later, and error active again: the node was on the bus all
     * the time, so the stable time runs on from 103 ms. */
    take_snapshot();
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_PASSIVE, ms(600U)));
    EXPECT_UNCHANGED();
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(700U)));
    EXPECT_UNCHANGED();

    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1103U) - 1U));
    TEST_ASSERT_EQUAL_UINT32(200U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1103U)));
    TEST_ASSERT_EQUAL_UINT32(100U, stats()->holdoff_ms);
}

static void test_a_node_that_comes_back_error_passive_is_back(void)
{
    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(0U, 0x100U, 1U, 0U));
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, 0U));
    TEST_ASSERT_EQUAL_HEX32(RESTART, poll(ms(100U)));

    /* Any state that is not bus off ends the bus-off. */
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_PASSIVE, ms(103U)));
    (void)EXPECT_NEXT(ms(103U), 0x100U, 1U);
    TEST_ASSERT_EQUAL_UINT32(200U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_UINT64(ms(1103U), deadline());
}

static void test_a_controller_that_comes_back_by_itself_is_not_asked_to_restart(void)
{
    start(3U, 0U);

    /* Back after half the hold-off, without having been asked. */
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, 0U));
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(50U)));

    /* The request that was pending is void: nothing at 100 ms, nothing later. */
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(100U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1049U)));
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->restart_requests);

    /* It was a bus-off all the same: the next one within the stable time, which runs from
     * 50 ms to 1050 ms, would wait twice as long. */
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->bus_off_count);
    TEST_ASSERT_EQUAL_UINT32(200U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_UINT64(ms(1050U), deadline());
}

/*
 * A timeline with all three kinds of deadline (hold-off 100 ms, stable time 1000 ms, event
 * timeout 250 ms):
 *
 *   1000 ms  event A enqueued               expires at 1250
 *   1100 ms  bus off                        restart due at 1200
 *   1200 ms  restart requested
 *   1203 ms  back on the bus                hold-off 200 ms, to be reset at 2203
 *   1250 ms  A expires
 *   2000 ms  event B enqueued               expires at 2250
 *   2203 ms  hold-off reset
 *   2250 ms  B expires
 */
static void test_next_deadline_is_the_earliest_of_restart_reset_expiry_and_recall(void)
{
    uint32_t handle;

    start(1U, 250U);

    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, ms(1000U)));
    TEST_ASSERT_EQUAL_UINT64(ms(1250U), deadline());

    /* The restart comes before the expiry. */
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(1100U)));
    TEST_ASSERT_EQUAL_UINT64(ms(1200U), deadline());
    TEST_ASSERT_EQUAL_HEX32(RESTART, poll(ms(1200U)));
    TEST_ASSERT_EQUAL_UINT64(ms(1250U), deadline());

    /* The expiry comes before the reset of the hold-off. */
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(1203U)));
    TEST_ASSERT_EQUAL_UINT64(ms(1250U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1250U)));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 1U, 0U);
    TEST_ASSERT_EQUAL_UINT64(ms(2203U), deadline());

    /* The reset comes before the expiry of a frame that was enqueued later. */
    TEST_ASSERT_TRUE(enqueue(0x700U, 2U, ms(2000U)));
    TEST_ASSERT_EQUAL_UINT64(ms(2203U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(2203U)));
    TEST_ASSERT_EQUAL_UINT32(100U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_UINT64(ms(2250U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(2250U)));
    EXPECT_COUNTS(2U, 0U, 0U, 0U, 2U, 0U);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());

    /* Of two waiting frames the deadline is that of the older. Once that one is in a mailbox
     * it no longer expires, but it is called back at the moment it would have, 3000 + 250.
     * After that the deadline is that of the frame behind it: 3010 + 250. */
    TEST_ASSERT_TRUE(enqueue(0x700U, 3U, ms(3000U)));
    TEST_ASSERT_TRUE(enqueue(0x700U, 4U, ms(3010U)));
    TEST_ASSERT_EQUAL_UINT64(ms(3250U), deadline());
    handle = EXPECT_NEXT(ms(3020U), 0x700U, 3U);
    TEST_ASSERT_EQUAL_UINT64(ms(3250U), deadline());
    TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(3250U)));
    TEST_ASSERT_EQUAL_UINT64(ms(3260U), deadline());
    EXPECT_DONE_EVENT(handle, 0x700U, ms(3000U));
}

static void test_a_zero_initial_holdoff_asks_for_recovery_at_once_and_never_grows(void)
{
    start_with(1U, 0U, 800U, 1000U, 0U);

    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(7U)));
    TEST_ASSERT_EQUAL_UINT64(ms(7U), deadline());
    TEST_ASSERT_EQUAL_HEX32(RESTART, poll(ms(7U)));
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(10U)));

    /* Twice nothing is nothing: the hold-off stays 0, and with nothing raised there is
     * nothing to reset and nothing to wake up for. */
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());

    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(20U)));
    TEST_ASSERT_EQUAL_HEX32(RESTART, poll(ms(20U)));
    TEST_ASSERT_EQUAL_UINT32(2U, stats()->restart_requests);
}

static void test_the_maximum_is_reached_exactly_when_doubling_would_pass_it(void)
{
    uint64_t back_ns;

    /* 100, 200, then 250 rather than 400, and 250 from there on. */
    start_with(1U, 100U, 250U, 1000U, 0U);

    back_ns = BUS_OFF_AND_BACK(ms(1000U), 100U);
    back_ns = BUS_OFF_AND_BACK(back_ns + ms(20U), 200U);
    back_ns = BUS_OFF_AND_BACK(back_ns + ms(20U), 250U);
    (void)BUS_OFF_AND_BACK(back_ns + ms(20U), 250U);

    /* A maximum of 201 ms: twice 100 still fits under it, by one. The second hold-off is
     * 200 ms and not yet the maximum, the third is the maximum. */
    start_with(1U, 100U, 201U, 1000U, 0U);

    back_ns = BUS_OFF_AND_BACK(ms(1000U), 100U);
    back_ns = BUS_OFF_AND_BACK(back_ns + ms(20U), 200U);
    (void)BUS_OFF_AND_BACK(back_ns + ms(20U), 201U);
}

static void test_doubling_the_holdoff_cannot_overflow(void)
{
    uint64_t back_ns;

    /* Twice 3 000 000 000 ms does not fit 32 bits; cut off, it would be 1 705 032 704 ms.
     * The hold-off goes to the maximum instead. */
    start_with(1U, 3000000000U, 4000000000U, 1000U, 0U);

    back_ns = BUS_OFF_AND_BACK(ms(1000U), 3000000000U);
    (void)BUS_OFF_AND_BACK(back_ns + ms(20U), 4000000000U);
}

/* Two tasks may read the clock and then reach the queue in the other order, so a call can
 * carry a time slightly before that of an earlier call. Nothing may happen early because of
 * it; in unsigned arithmetic "before" would otherwise look like "584 years after". */
static void test_a_clock_reading_from_before_a_timer_started_triggers_nothing(void)
{
    uint32_t handle;

    start(1U, 50U);

    /* An event frame is not over at a time before its enqueue. */
    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, ms(1000U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(900U)));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);
    handle = EXPECT_NEXT(ms(900U), 0x700U, 1U);
    EXPECT_DONE_EVENT(handle, 0x700U, ms(1000U));

    /* A restart is not requested at a time before the bus-off. */
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(2000U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1999U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(0U));
    TEST_ASSERT_EQUAL_HEX32(RESTART, poll(ms(2100U)));

    /* The hold-off is not reset at a time before the recovery, neither by a poll nor by the
     * next bus-off. */
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(3000U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(2999U)));
    TEST_ASSERT_EQUAL_UINT32(200U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(2999U)));
    TEST_ASSERT_EQUAL_UINT32(200U, stats()->holdoff_ms);
}

static void test_a_deadline_beyond_the_range_of_the_clock_is_no_deadline(void)
{
    const uint64_t late_ns = UINT64_MAX - ms(10U);

    start(1U, 50U);

    /* 10 ms before the clock ends, with a timeout of 50 ms: this frame cannot expire any
     * more. A sum that wrapped around would name a deadline near zero instead. */
    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, late_ns));
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(UINT64_MAX));
    EXPECT_COUNTS(1U, 0U, 0U, 0U, 0U, 1U);

    /* The same for the hold-off of 100 ms ... */
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, late_ns));
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(UINT64_MAX));

    /* ... and for the stable time of one second. */
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, late_ns));
    TEST_ASSERT_EQUAL_UINT32(200U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(UINT64_MAX));
    TEST_ASSERT_EQUAL_UINT32(200U, stats()->holdoff_ms);
}

/* The same for the recall of an event frame in a mailbox: enqueued 10 ms before the clock ends,
 * with a timeout of 50 ms, it is never called back, not even at the last nanosecond there is. A
 * recall decided as "stamp + timeout <= now" would be made there, as the sum is cut off at
 * UINT64_MAX. */
static void test_an_event_frame_in_a_mailbox_beyond_the_range_of_the_clock_is_never_recalled(void)
{
    const uint64_t late_ns = UINT64_MAX - ms(10U);
    uint32_t handle;

    start(1U, 50U);

    TEST_ASSERT_TRUE(enqueue(0x700U, 1U, late_ns));
    handle = EXPECT_NEXT(late_ns, 0x700U, 1U);
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(UINT64_MAX - 1U));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(UINT64_MAX));
    TEST_ASSERT_EQUAL_UINT64(0U, stats()->recall_requests);
    EXPECT_DONE_EVENT(handle, 0x700U, late_ns);
    EXPECT_COUNTS(1U, 1U, 0U, 0U, 0U, 0U);
}

/* ------------------------------------------------------------------------------------------
 * Buffering across a bus-off, frame by frame
 * ---------------------------------------------------------------------------------------- */

#define ID_ALARM  0x080U /* event frame; beats every cyclic message in arbitration */
#define ID_SPEED  0x100U /* cyclic, slot 0 */
#define ID_TEMP   0x200U /* cyclic, slot 1 */
#define ID_STATUS 0x300U /* cyclic, slot 2 */
#define ID_DIAG   0x7E8U /* event frame; loses to every cyclic message */

/*
 * Two mailboxes, a hold-off of 100 ms, event frames expire after 100 ms. "SPEED#2" is the
 * second value of the message SPEED; the event frames are numbered in the same way.
 *
 *    time     what happens                                    what it leads to
 *   --------  ----------------------------------------------  ---------------------------------
 *      0 ms   SPEED#1, TEMP#1, STATUS#1 published; DIAG#1     all four sent by 1 ms, two at a
 *             enqueued; the bus is healthy                    time
 *    990 ms   SPEED#2 published, DIAG#2 enqueued; both are    the bus is disturbed: neither is
 *             loaded                                          confirmed
 *    995 ms   TEMP#2 published, DIAG#3 enqueued               both wait, no mailbox is free
 *   1000 ms   BUS OFF. SPEED#3 is published before the two    SPEED#2 superseded; DIAG#2 back at
 *             aborts are reported                             the head, in front of DIAG#3
 *   1010 ms   SPEED#4 published                               SPEED#3 superseded
 *   1050 ms   SPEED#5, TEMP#3 published; ALARM#1, DIAG#4      SPEED#4 and TEMP#2 superseded
 *             enqueued
 *   1080 ms   a burst of 30 DIAG frames, #101 .. #130         the FIFO holds 4: it takes 28 and
 *                                                             refuses #129 and #130
 *   1090 ms   DIAG#2 is 100 ms old; SPEED#6 and STATUS#2      DIAG#2 expired; SPEED#5 superseded
 *             published
 *   1095 ms   DIAG#3 is 100 ms old                            DIAG#3 expired
 *   1100 ms   the hold-off is over                            restart requested
 *   1103 ms   the controller is back                          33 frames are waiting
 *
 * The node was off the bus for 103 ms. What comes out afterwards, and in which order:
 *
 *   ALARM#1    the head of the FIFO, and the best key of all
 *   SPEED#6    sampled at 1090 ms: 13 ms old, not 113
 *   TEMP#3     sampled at 1050 ms
 *   STATUS#2   sampled at 1090 ms
 *   DIAG#4, then DIAG#101 .. DIAG#128 in order, one at a time
 *
 * Over the whole scenario 46 frames were produced:
 *
 *   SPEED    6    2 sent (#1, #6)                     4 superseded (#2 .. #5)
 *   TEMP     3    2 sent (#1, #3)                     1 superseded (#2)
 *   STATUS   2    2 sent
 *   ALARM    1    1 sent
 *   DIAG    34   30 sent (#1, #4, #101 .. #128)       2 expired (#2, #3), 2 dropped (#129, #130)
 *           --   --
 *           46 = 37 sent + 5 superseded + 2 dropped + 2 expired
 */
static void test_buffering_across_a_bus_off(void)
{
    uint32_t speed;
    uint32_t temp;
    uint32_t status;
    uint32_t event;

    start_with(2U, 100U, 800U, 1000U, 100U);

    /* 0 ms: a healthy bus. Two mailboxes, so four frames go out two at a time, best key
     * first. */
    TEST_ASSERT_TRUE(publish(0U, ID_SPEED, 1U, 0U));
    TEST_ASSERT_TRUE(publish(1U, ID_TEMP, 1U, 0U));
    TEST_ASSERT_TRUE(publish(2U, ID_STATUS, 1U, 0U));
    TEST_ASSERT_TRUE(enqueue(ID_DIAG, 1U, 0U));
    speed = EXPECT_NEXT(0U, ID_SPEED, 1U);
    temp = EXPECT_NEXT(0U, ID_TEMP, 1U);
    EXPECT_NOTHING(0U);
    EXPECT_DONE_CYCLIC(speed, 0U, ID_SPEED, 0U);
    EXPECT_DONE_CYCLIC(temp, 1U, ID_TEMP, 0U);
    status = EXPECT_NEXT(ms(1U), ID_STATUS, 1U);
    event = EXPECT_NEXT(ms(1U), ID_DIAG, 1U);
    EXPECT_DONE_CYCLIC(status, 2U, ID_STATUS, 0U);
    EXPECT_DONE_EVENT(event, ID_DIAG, 0U);
    EXPECT_COUNTS(4U, 4U, 0U, 0U, 0U, 0U);

    /* 990 ms: two frames are loaded. The bus is disturbed and no confirmation comes. */
    TEST_ASSERT_TRUE(publish(0U, ID_SPEED, 2U, ms(990U)));
    TEST_ASSERT_TRUE(enqueue(ID_DIAG, 2U, ms(990U)));
    speed = EXPECT_NEXT(ms(990U), ID_SPEED, 2U);
    event = EXPECT_NEXT(ms(990U), ID_DIAG, 2U);

    /* 995 ms: both mailboxes are in use, so these two wait. */
    TEST_ASSERT_TRUE(publish(1U, ID_TEMP, 2U, ms(995U)));
    TEST_ASSERT_TRUE(enqueue(ID_DIAG, 3U, ms(995U)));
    EXPECT_NOTHING(ms(995U));
    EXPECT_COUNTS(8U, 4U, 0U, 0U, 0U, 4U);

    /* 1000 ms: bus off. The producer of SPEED runs before the aborts are reported, so the
     * aborted SPEED#2 finds a newer value in its slot and ends there. DIAG#2 returns to the
     * head of the FIFO. */
    TEST_ASSERT_EQUAL_HEX32(ABORT, report_state(CAN_FC_BUS_OFF, ms(1000U)));
    TEST_ASSERT_TRUE(publish(0U, ID_SPEED, 3U, ms(1000U)));
    EXPECT_COUNTS(9U, 4U, 0U, 0U, 0U, 5U);
    TEST_ASSERT_TRUE(aborted(speed));
    EXPECT_COUNTS(9U, 4U, 1U, 0U, 0U, 4U);
    TEST_ASSERT_TRUE(aborted(event));
    EXPECT_COUNTS(9U, 4U, 1U, 0U, 0U, 4U);
    EXPECT_NOTHING(ms(1000U));

    /* The next thing to happen is the expiry of DIAG#2 at 990 + 100 ms, before the restart at
     * 1000 + 100 ms. */
    TEST_ASSERT_EQUAL_UINT64(ms(1090U), deadline());

    /* 1010 ms and 1050 ms: the producers carry on. */
    TEST_ASSERT_TRUE(publish(0U, ID_SPEED, 4U, ms(1010U)));
    EXPECT_COUNTS(10U, 4U, 2U, 0U, 0U, 4U);
    TEST_ASSERT_TRUE(publish(0U, ID_SPEED, 5U, ms(1050U)));
    TEST_ASSERT_TRUE(publish(1U, ID_TEMP, 3U, ms(1050U)));
    EXPECT_COUNTS(12U, 4U, 4U, 0U, 0U, 4U);
    TEST_ASSERT_TRUE(enqueue(ID_ALARM, 1U, ms(1050U)));
    TEST_ASSERT_TRUE(enqueue(ID_DIAG, 4U, ms(1050U)));
    EXPECT_COUNTS(14U, 4U, 4U, 0U, 0U, 6U);

    /* 1080 ms: 30 more DIAG frames. DIAG#2, DIAG#3, ALARM#1 and DIAG#4 are waiting, so there
     * is room for 32 - 4 = 28. */
    for (uint32_t tag = 101U; tag <= 130U; tag++) {
        TEST_ASSERT_EQUAL(tag <= 128U, enqueue(ID_DIAG, tag, ms(1080U)));
    }

    EXPECT_COUNTS(44U, 4U, 4U, 2U, 0U, 34U);

    /* 1090 ms: DIAG#2 expires, not a nanosecond earlier. Then two more values. */
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1090U) - 1U));
    EXPECT_COUNTS(44U, 4U, 4U, 2U, 0U, 34U);
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1090U)));
    EXPECT_COUNTS(44U, 4U, 4U, 2U, 1U, 33U);
    TEST_ASSERT_TRUE(publish(0U, ID_SPEED, 6U, ms(1090U)));
    TEST_ASSERT_TRUE(publish(2U, ID_STATUS, 2U, ms(1090U)));
    EXPECT_COUNTS(46U, 4U, 5U, 2U, 1U, 34U);

    /* 1095 ms: DIAG#3 expires. The head of the FIFO is now ALARM#1, good until 1150 ms. */
    TEST_ASSERT_EQUAL_UINT64(ms(1095U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1095U)));
    EXPECT_COUNTS(46U, 4U, 5U, 2U, 2U, 33U);

    /* 1100 ms: the hold-off is over. */
    TEST_ASSERT_EQUAL_UINT64(ms(1100U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1100U) - 1U));
    TEST_ASSERT_EQUAL_HEX32(RESTART, poll(ms(1100U)));
    TEST_ASSERT_EQUAL_UINT64(ms(1150U), deadline());
    EXPECT_NOTHING(ms(1100U));

    /* 1103 ms: back on the bus. Nothing was lost by the recovery itself. */
    TEST_ASSERT_EQUAL_HEX32(0U, report_state(CAN_FC_ERROR_ACTIVE, ms(1103U)));
    EXPECT_COUNTS(46U, 4U, 5U, 2U, 2U, 33U);

    /* First the best two keys: the alarm, then the newest SPEED. */
    event = EXPECT_NEXT(ms(1103U), ID_ALARM, 1U);
    speed = EXPECT_NEXT(ms(1103U), ID_SPEED, 6U);
    EXPECT_NOTHING(ms(1103U));
    EXPECT_DONE_EVENT(event, ID_ALARM, ms(1050U));
    EXPECT_DONE_CYCLIC(speed, 0U, ID_SPEED, ms(1090U));

    /* Then the other two messages, each with its newest value. */
    temp = EXPECT_NEXT(ms(1104U), ID_TEMP, 3U);
    status = EXPECT_NEXT(ms(1104U), ID_STATUS, 2U);
    EXPECT_DONE_CYCLIC(temp, 1U, ID_TEMP, ms(1050U));
    EXPECT_DONE_CYCLIC(status, 2U, ID_STATUS, ms(1090U));

    /* Then the event frames that are left, in the order of their enqueue and one at a time:
     * the second mailbox stays empty. The last of them is loaded at 1133 ms, before the
     * burst expires at 1180 ms. */
    event = EXPECT_NEXT(ms(1105U), ID_DIAG, 4U);
    EXPECT_NOTHING(ms(1105U));
    EXPECT_DONE_EVENT(event, ID_DIAG, ms(1050U));

    for (uint32_t k = 0U; k < 28U; k++) {
        event = EXPECT_NEXT(ms(1106U + k), ID_DIAG, 101U + k);
        EXPECT_NOTHING(ms(1106U + k));
        EXPECT_DONE_EVENT(event, ID_DIAG, ms(1080U));
    }

    EXPECT_NOTHING(ms(1134U));
    EXPECT_COUNTS(46U, 37U, 5U, 2U, 2U, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->bus_off_count);
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->restart_requests);

    /* A second bus-off within the next second would wait 200 ms. After that second, at
     * 1103 + 1000 ms, the hold-off is 100 ms again. */
    TEST_ASSERT_EQUAL_UINT32(200U, stats()->holdoff_ms);
    TEST_ASSERT_EQUAL_UINT64(ms(2103U), deadline());
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(2103U)));
    TEST_ASSERT_EQUAL_UINT32(100U, stats()->holdoff_ms);
}

/* ------------------------------------------------------------------------------------------
 * A fault without bus off, period by period
 * ---------------------------------------------------------------------------------------- */

/* At most how long after the abort the attempt on the wire ends. A frame with four data bytes
 * up to its acknowledgment slot, the error frame, the intermission and the suspension of an
 * error-passive transmitter are about 106 bit times, 0.21 ms at 500 kbit/s; rounded up. */
#define ATTEMPT_NS 250000U /* 0.25 ms */

/*
 * Three mailboxes, one more than the two messages need: the one that stays free shows that a
 * new value waits for its predecessor and does not take a second mailbox. SPEED (slot 0) is
 * published every 10 ms, TEMP (slot 1) every 100 ms. The tag
 * of a value is the time it was published in milliseconds, and so is its stamp: SPEED@1490 is
 * the value published at 1490 ms. Event frames take no part; their timeout is off.
 *
 * From 1000 ms until 1495 ms nobody acknowledges (CAN_BUS_FAULT_NO_ACK): the node goes error
 * passive and stays there, it never goes bus off, and nothing is confirmed. The controller
 * answers an abort as lib/can/can_ctrl.h says: a frame that only waits in its mailbox comes
 * back at once, the frame whose attempt is on the wire comes back at the end of that failed
 * attempt, ATTEMPT_NS later. The controller always attempts the best key among its mailboxes,
 * and the transmit task reloads a mailbox as soon as its frame comes back, before the next
 * attempt starts (the error frame and the intermission alone are 17 bit times), so the frame
 * on the wire is always SPEED's.
 *
 *    time          what happens                                    what the queue does
 *   ------------  ----------------------------------------------  -------------------------------
 *   1000 ms       SPEED@1000 and TEMP@1000 published and loaded;   no slot had a value out:
 *                 the fault begins, SPEED@1000's attempts fail     nothing to recall
 *   1010 ms       SPEED@1010 published                             the poll asks for an abort (1)
 *                 TEMP@1000 comes back at once                     back to its slot, loaded again
 *   1010.25 ms    SPEED@1000 comes back after its attempt          superseded; SPEED@1010 loaded
 *   1020 .. 1490  the same every 10 ms                             one request each: (2) .. (49)
 *   1100 .. 1400  TEMP@t is published at the same time as          the same single request; the
 *                 SPEED@t                                          old TEMP is superseded as well
 *   1495 ms       the fault ends: SPEED@1490 goes out, then
 *                 TEMP@1400
 *
 * The first frame on the bus after the fault is SPEED@1490, 5 ms old: less than its period of
 * 10 ms. TEMP@1400 is 95 ms old, less than its period of 100 ms. Without the recall the first
 * would have been SPEED@1000, 495 ms old, with TEMP@1000 behind it.
 *
 *   SPEED   50 published (1000, 1010 .. 1490)   1 sent (@1490)   49 superseded
 *   TEMP     5 published (1000, 1100 .. 1400)   1 sent (@1400)    4 superseded
 *           --                                   -               --
 *           55 produced                       =  2 sent        + 53 superseded
 *
 *   49 recall requests, one per period from 1010 to 1490 ms; no bus-off.
 */
static void test_a_fault_without_bus_off_leaves_no_stale_value_in_the_mailboxes(void)
{
    uint32_t speed;
    uint32_t temp;
    uint32_t temp_tag = 1000U;

    start(3U, 0U);

    TEST_ASSERT_TRUE(publish(0U, ID_SPEED, 1000U, ms(1000U)));
    TEST_ASSERT_TRUE(publish(1U, ID_TEMP, 1000U, ms(1000U)));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1000U)));
    speed = EXPECT_NEXT(ms(1000U), ID_SPEED, 1000U);
    temp = EXPECT_NEXT(ms(1000U), ID_TEMP, 1000U);
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1000U)));
    EXPECT_COUNTS(2U, 0U, 0U, 0U, 0U, 2U);

    for (uint32_t t = 1010U; t <= 1490U; t += 10U) {
        /* Periods so far, counting this one: n = (t - 1000) / 10 values of SPEED after the
         * first, and k = (t - 1000) / 100 of TEMP. Each of them supersedes its predecessor,
         * one of each message is pending, nothing is sent:
         *     produced = 2 + n + k = 0 sent + (n + k) superseded + 2 pending */
        const uint32_t n = (t - 1000U) / 10U;
        const uint32_t k = (t - 1000U) / 100U;

        TEST_ASSERT_TRUE(publish(0U, ID_SPEED, t, ms(t)));

        if ((t % 100U) == 0U) {
            TEST_ASSERT_TRUE(publish(1U, ID_TEMP, t, ms(t)));
            temp_tag = t;
        }

        /* Due at once, asked for once. */
        TEST_ASSERT_EQUAL_UINT64(0U, deadline());
        TEST_ASSERT_EQUAL_HEX32(ABORT, poll(ms(t)));
        TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(t)));
        TEST_ASSERT_EQUAL_UINT64(n, stats()->recall_requests);

        /* TEMP only waits in its mailbox: it comes back at once and is loaded again, its
         * newest value either way. SPEED's newest value waits for the old one. */
        TEST_ASSERT_TRUE(aborted(temp));
        temp = EXPECT_NEXT(ms(t), ID_TEMP, temp_tag);
        EXPECT_NOTHING(ms(t));

        /* The attempt on the wire ends, and SPEED's old value comes back. */
        TEST_ASSERT_TRUE(aborted(speed));
        speed = EXPECT_NEXT(ms(t) + ATTEMPT_NS, ID_SPEED, t);
        TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(t) + ATTEMPT_NS));
        TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
        EXPECT_COUNTS(2U + n + k, 0U, n + k, 0U, 0U, 2U);
    }

    /* 1495 ms: the fault is gone, and the next attempts succeed. What goes out is at most one
     * period old. */
    EXPECT_DONE_CYCLIC(speed, 0U, ID_SPEED, ms(1490U));
    EXPECT_DONE_CYCLIC(temp, 1U, ID_TEMP, ms(1400U));
    TEST_ASSERT_EQUAL_HEX32(0U, poll(ms(1496U)));
    TEST_ASSERT_EQUAL_UINT64(CAN_TXQ_NO_DEADLINE, deadline());
    EXPECT_NOTHING(ms(1496U));

    EXPECT_COUNTS(55U, 2U, 53U, 0U, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT64(49U, stats()->recall_requests);
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->bus_off_count);
}

/* ------------------------------------------------------------------------------------------
 * A random scenario against a reference model
 *
 * The model below is the header written down in the most obvious way and nothing more: whole
 * frames in plain lists, searched from the start every time. It keeps no count of what is
 * pending (it counts its lists when asked), has no ring (the FIFO is an array whose first
 * element is the oldest frame), knows nothing of how a handle is made (it stores the one the
 * queue gave) and does not rely on the order of the FIFO when frames expire. The queue is
 * written for short, constant-time steps instead. The scenario makes the same random call on
 * both, tens of thousands of times, and after every call compares everything the queue lets
 * a caller see.
 * ---------------------------------------------------------------------------------------- */

#define SCENARIO_STEPS 40000U

/* Short times, so that 40000 steps see hundreds of bus-offs of every length. */
#define SCENARIO_HOLDOFF_INITIAL_MS 4U
#define SCENARIO_HOLDOFF_MAX_MS     32U
#define SCENARIO_STABLE_MS          20U

/* Where a frame comes from when it is handed out: one of the 16 slots, or the FIFO. */
#define SOURCE_EVENT 16U
#define SOURCE_NONE  17U

typedef struct {
    uint64_t stamp_ns;
    can_frame_t frame;
    uint32_t handle; /* while the frame is in a mailbox */
    bool present;
} model_frame_t;

typedef struct {
    /* The counters of the header. */
    uint64_t produced;
    uint64_t sent;
    uint64_t superseded;
    uint64_t dropped;
    uint64_t expired;
    /* The counters of each slot. */
    uint64_t slot_produced[16];
    uint64_t slot_sent[16];
    uint64_t slot_superseded[16];

    uint64_t bus_off_since_ns;
    uint64_t recovered_at_ns;

    /* Every frame that was accepted and is neither sent nor lost is in one of these. */
    model_frame_t waiting[16];  /* per slot: the value that waits to be loaded */
    model_frame_t loaded[16];   /* per slot: the value that is in a mailbox */
    model_frame_t fifo[33];     /* the event frames that wait, oldest first */
    model_frame_t event_loaded; /* the event frame that is in a mailbox */
    uint32_t fifo_count;

    uint64_t recall_requests;
    uint32_t mailboxes;
    uint32_t event_timeout_ms;
    uint32_t holdoff_ms;
    uint32_t bus_off_count;
    uint32_t restart_requests;
    bool bus_off;
    bool restart_owed;        /* bus off, and the restart has not been requested yet */
    bool stable_time_running; /* back on the bus since recovered_at_ns */
    bool recall_noted;        /* a value was published while its predecessor was out */
    bool event_recalled;      /* the event frame that is in a mailbox was asked back */
} model_t;

/* How often the scenario got into the situations it is there for. */
typedef struct {
    uint32_t calls;               /* calls into the queue, each followed by the full check */
    uint32_t handed_out;          /* frames loaded into a mailbox */
    uint32_t ties;                /* of those, with another candidate that had the same key */
    uint32_t declined_full;       /* can_txq_next() declined although a frame could have gone */
    uint32_t refused_handles;     /* done or aborted with a handle that is not outstanding */
    uint32_t invalid_calls;       /* publish or enqueue with an invalid argument */
    uint32_t aborts_to_slot;      /* aborted cyclic value went back to its slot */
    uint32_t aborts_superseded;   /* aborted cyclic value found a newer one there */
    uint32_t aborts_to_fifo;      /* aborted event frame went back to the head */
    uint32_t aborts_to_full_fifo; /* of those, into a FIFO that held 32 already */
    uint32_t holdoff_resets;      /* raised hold-off returned to its initial value */
    uint32_t holdoff_at_maximum;  /* bus-offs that were served with the maximum hold-off */
    uint32_t early_recoveries;    /* controller came back before it was asked to */
    uint32_t recalls_for_values;  /* polls that asked back a value with a successor waiting */
    uint32_t recalls_for_events;  /* polls that asked back an event frame at its timeout */
    uint32_t recalls_withdrawn;   /* notes that were dropped: the value had gone out meanwhile */
    uint32_t recalls_carried_out; /* recalls after which every frame came back at once */
} seen_t;

static model_t s_model;
static seen_t s_seen;
static uint64_t s_now_ns;
static uint32_t s_serial;
static uint32_t s_retired[16];
static uint32_t s_retired_count;
static char s_step_text[64];

static uint32_t model_mailboxes_in_use(void)
{
    uint32_t in_use = s_model.event_loaded.present ? 1U : 0U;

    for (uint32_t slot = 0U; slot < 16U; slot++) {
        if (s_model.loaded[slot].present) {
            in_use++;
        }
    }

    return in_use;
}

/* "pending counts the frames waiting in slots, in the FIFO and in mailboxes" */
static uint32_t model_pending(void)
{
    uint32_t pending = s_model.fifo_count + model_mailboxes_in_use();

    for (uint32_t slot = 0U; slot < 16U; slot++) {
        if (s_model.waiting[slot].present) {
            pending++;
        }
    }

    return pending;
}

/* An event frame that has waited for the timeout is gone, wherever in the FIFO it is. */
static void model_expire(uint64_t now_ns)
{
    const uint64_t timeout_ns = (uint64_t)s_model.event_timeout_ms * 1000000ULL;

    if (s_model.event_timeout_ms != 0U) {
        uint32_t kept = 0U;

        for (uint32_t i = 0U; i < s_model.fifo_count; i++) {
            if ((now_ns - s_model.fifo[i].stamp_ns) >= timeout_ns) {
                s_model.expired++;
            } else {
                s_model.fifo[kept] = s_model.fifo[i];
                kept++;
            }
        }

        s_model.fifo_count = kept;
    }
}

static void model_publish(uint32_t slot, const can_frame_t *frame, uint64_t stamp_ns)
{
    s_model.produced++;
    s_model.slot_produced[slot]++;

    if (s_model.waiting[slot].present) {
        s_model.superseded++; /* the value that waited is replaced */
        s_model.slot_superseded[slot]++;
    }

    s_model.waiting[slot].stamp_ns = stamp_ns;
    s_model.waiting[slot].frame = *frame;
    s_model.waiting[slot].handle = 0U;
    s_model.waiting[slot].present = true;

    /* "If an earlier value of the message is still in a mailbox and the controller is not bus
     * off, the next can_txq_poll() asks for the mailboxes to be aborted" */
    if (s_model.loaded[slot].present && !s_model.bus_off) {
        s_model.recall_noted = true;
    }
}

static bool model_enqueue(const can_frame_t *frame, uint64_t now_ns)
{
    bool accepted = false;

    model_expire(now_ns);
    s_model.produced++;

    if (s_model.fifo_count < 32U) {
        model_frame_t *entry = &s_model.fifo[s_model.fifo_count];

        entry->stamp_ns = now_ns;
        entry->frame = *frame;
        entry->handle = 0U;
        entry->present = true;
        s_model.fifo_count++;
        accepted = true;
    } else {
        s_model.dropped++;
    }

    return accepted;
}

/* The frame that `source` offers for the next free mailbox, or NULL. */
static const model_frame_t *model_candidate(uint32_t source)
{
    const model_frame_t *candidate = NULL;

    if (source == SOURCE_EVENT) {
        /* the oldest event frame, unless an event frame is in a mailbox already */
        if ((s_model.fifo_count > 0U) && !s_model.event_loaded.present) {
            candidate = &s_model.fifo[0];
        }
    } else if (s_model.waiting[source].present && !s_model.loaded[source].present) {
        /* a waiting value, unless an earlier value of the message is in a mailbox */
        candidate = &s_model.waiting[source];
    } else {
        /* this slot has nothing to offer */
    }

    return candidate;
}

/* Which source the next frame has to come from at `now_ns`, or SOURCE_NONE. The rule of the
 * header as one comparison: the smallest key wins, and of equal keys the lowest source, where
 * the slots are sources 0 to 15 and the FIFO is source 16. */
static uint32_t model_choose(uint64_t now_ns)
{
    uint32_t chosen = SOURCE_NONE;

    model_expire(now_ns);

    if (!s_model.bus_off && (model_mailboxes_in_use() < s_model.mailboxes)) {
        uint64_t chosen_rank = UINT64_MAX;

        for (uint32_t source = 0U; source <= SOURCE_EVENT; source++) {
            const model_frame_t *candidate = model_candidate(source);

            if (candidate != NULL) {
                const uint64_t key = can_frame_arbitration_key(&candidate->frame);
                const uint64_t rank = (key << 8U) | source;

                if (rank < chosen_rank) {
                    chosen = source;
                    chosen_rank = rank;
                }
            }
        }
    }

    return chosen;
}

/* True if another source offers a frame with the same key as `winner`, the chosen one. */
static bool model_choice_was_a_tie(uint32_t chosen, const model_frame_t *winner)
{
    const uint32_t key = can_frame_arbitration_key(&winner->frame);
    bool tie = false;

    for (uint32_t source = 0U; source <= SOURCE_EVENT; source++) {
        const model_frame_t *candidate = model_candidate(source);

        if ((source != chosen) && (candidate != NULL) &&
            (can_frame_arbitration_key(&candidate->frame) == key)) {
            tie = true;
        }
    }

    return tie;
}

static void model_load(uint32_t source, uint32_t handle)
{
    if (source == SOURCE_EVENT) {
        s_model.event_loaded = s_model.fifo[0];
        s_model.event_loaded.handle = handle;
        s_model.event_recalled = false; /* "once for that frame": this is another one */
        s_model.fifo_count--;

        for (uint32_t i = 0U; i < s_model.fifo_count; i++) {
            s_model.fifo[i] = s_model.fifo[i + 1U];
        }
    } else {
        s_model.loaded[source] = s_model.waiting[source];
        s_model.loaded[source].handle = handle;
        s_model.waiting[source].present = false;
    }
}

/* The frame of `source` that is in a mailbox. */
static model_frame_t *model_loaded(uint32_t source)
{
    return (source == SOURCE_EVENT) ? &s_model.event_loaded : &s_model.loaded[source];
}

/* The source whose frame is in a mailbox under `handle`, or SOURCE_NONE. */
static uint32_t model_find(uint32_t handle)
{
    uint32_t found = SOURCE_NONE;

    for (uint32_t source = 0U; source <= SOURCE_EVENT; source++) {
        const model_frame_t *frame = model_loaded(source);

        if (frame->present && (frame->handle == handle)) {
            found = source;
        }
    }

    return found;
}

static void model_done(uint32_t source)
{
    s_model.sent++;

    if (source < SOURCE_EVENT) {
        s_model.slot_sent[source]++;
    }

    model_loaded(source)->present = false;
}

static void model_aborted(uint32_t source)
{
    if (source == SOURCE_EVENT) {
        /* Back in front of everything that was enqueued after it, room or no room. */
        s_seen.aborts_to_fifo++;

        if (s_model.fifo_count >= 32U) {
            s_seen.aborts_to_full_fifo++;
        }

        for (uint32_t i = s_model.fifo_count; i > 0U; i--) {
            s_model.fifo[i] = s_model.fifo[i - 1U];
        }

        s_model.fifo[0] = s_model.event_loaded;
        s_model.fifo_count++;
    } else if (s_model.waiting[source].present) {
        /* A newer value waits: the aborted one is not worth sending any more. */
        s_model.superseded++;
        s_model.slot_superseded[source]++;
        s_seen.aborts_superseded++;
    } else {
        s_model.waiting[source] = s_model.loaded[source];
        s_seen.aborts_to_slot++;
    }

    model_loaded(source)->present = false;
}

/* "Once the node has stayed out of bus-off for the configured stable time, the hold-off
 * returns to its initial value." */
static void model_settle_holdoff(uint64_t now_ns)
{
    const uint64_t stable_ns = (uint64_t)SCENARIO_STABLE_MS * 1000000ULL;

    if (s_model.stable_time_running && ((now_ns - s_model.recovered_at_ns) >= stable_ns)) {
        s_model.stable_time_running = false;

        if (s_model.holdoff_ms != SCENARIO_HOLDOFF_INITIAL_MS) {
            s_model.holdoff_ms = SCENARIO_HOLDOFF_INITIAL_MS;
            s_seen.holdoff_resets++;
        }
    }
}

/* Returns the requests the queue has to make. */
static uint32_t model_state(can_fc_state_t state, uint64_t now_ns)
{
    uint32_t requests = 0U;

    if ((state == CAN_FC_BUS_OFF) && !s_model.bus_off) {
        model_settle_holdoff(now_ns);        /* the stable time may have passed unnoticed */
        s_model.stable_time_running = false; /* and from here on it does not run */
        s_model.bus_off = true;
        s_model.restart_owed = true;
        s_model.bus_off_since_ns = now_ns;
        s_model.bus_off_count++;
        s_model.recall_noted = false; /* "the abort was requested on the way in" */
        requests = ABORT;

        if (s_model.holdoff_ms == SCENARIO_HOLDOFF_MAX_MS) {
            s_seen.holdoff_at_maximum++;
        }
    } else if ((state != CAN_FC_BUS_OFF) && s_model.bus_off) {
        const uint64_t doubled = 2ULL * s_model.holdoff_ms;

        if (s_model.restart_owed) {
            s_seen.early_recoveries++;
        }

        s_model.bus_off = false;
        s_model.restart_owed = false;
        s_model.holdoff_ms =
            (doubled > SCENARIO_HOLDOFF_MAX_MS) ? SCENARIO_HOLDOFF_MAX_MS : (uint32_t)doubled;
        s_model.stable_time_running = true;
        s_model.recovered_at_ns = now_ns;
    } else {
        /* no change between bus off and not bus off: nothing happens */
    }

    return requests;
}

/* Some message has a value waiting behind an earlier one that is in a mailbox. */
static bool model_value_behind_a_mailbox(void)
{
    bool found = false;

    for (uint32_t slot = 0U; slot < 16U; slot++) {
        if (s_model.waiting[slot].present && s_model.loaded[slot].present) {
            found = true;
        }
    }

    return found;
}

/* "An event frame in a mailbox reaches its timeout. The poll asks once for that frame";
 * "While the controller is bus off none of it applies". */
static bool model_event_recall_armed(void)
{
    return s_model.event_loaded.present && !s_model.event_recalled && !s_model.bus_off &&
           (s_model.event_timeout_ms != 0U);
}

/* Returns the requests the queue has to make. */
static uint32_t model_poll(uint64_t now_ns)
{
    const uint64_t holdoff_ns = (uint64_t)s_model.holdoff_ms * 1000000ULL;
    const uint64_t timeout_ns = (uint64_t)s_model.event_timeout_ms * 1000000ULL;
    uint32_t requests = 0U;
    bool recall_value;
    bool recall_event;

    model_expire(now_ns);
    model_settle_holdoff(now_ns);

    if (s_model.restart_owed && ((now_ns - s_model.bus_off_since_ns) >= holdoff_ns)) {
        s_model.restart_owed = false;
        s_model.restart_requests++;
        requests = RESTART;
    }

    /* "If the earlier value is confirmed before the poll, nothing stale is left and the
     * request is withdrawn." */
    recall_value = s_model.recall_noted && model_value_behind_a_mailbox();
    recall_event =
        model_event_recall_armed() && ((now_ns - s_model.event_loaded.stamp_ns) >= timeout_ns);

    if (s_model.recall_noted && !recall_value) {
        s_seen.recalls_withdrawn++;
    }

    s_model.recall_noted = false;

    if (recall_value) {
        s_seen.recalls_for_values++;
    }

    if (recall_event) {
        s_model.event_recalled = true;
        s_seen.recalls_for_events++;
    }

    if (recall_value || recall_event) {
        s_model.recall_requests++;
        requests |= ABORT;
    }

    return requests;
}

/* The earliest moment at which model_poll() would do something. */
static uint64_t model_deadline(void)
{
    const uint64_t timeout_ns = (uint64_t)s_model.event_timeout_ms * 1000000ULL;
    uint64_t deadline_ns = CAN_TXQ_NO_DEADLINE;

    if (s_model.restart_owed) {
        deadline_ns = s_model.bus_off_since_ns + ((uint64_t)s_model.holdoff_ms * 1000000ULL);
    }

    if (s_model.stable_time_running && (s_model.holdoff_ms != SCENARIO_HOLDOFF_INITIAL_MS)) {
        deadline_ns = s_model.recovered_at_ns + ((uint64_t)SCENARIO_STABLE_MS * 1000000ULL);
    }

    if (s_model.event_timeout_ms != 0U) {
        for (uint32_t i = 0U; i < s_model.fifo_count; i++) {
            const uint64_t expiry_ns = s_model.fifo[i].stamp_ns + timeout_ns;

            if (expiry_ns < deadline_ns) {
                deadline_ns = expiry_ns;
            }
        }
    }

    if (model_event_recall_armed() &&
        ((s_model.event_loaded.stamp_ns + timeout_ns) < deadline_ns)) {
        deadline_ns = s_model.event_loaded.stamp_ns + timeout_ns;
    }

    /* "0, which has always passed, while the recall of a cyclic value is due" */
    if (s_model.recall_noted && model_value_behind_a_mailbox()) {
        deadline_ns = 0U;
    }

    return deadline_ns;
}

/* Everything a caller can see of the queue, against the model. Called after every call. */
static void check_against_model(void)
{
    const can_txq_stats_t *s = stats();

    s_seen.calls++;

    /* The identity of the header, from the queue's own counters alone. */
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        s->produced, s->sent + s->superseded + s->dropped + s->expired + s->pending, s_step_text);

    /* Each counter against the model's, and pending against the frames the model holds. */
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(s_model.produced, s->produced, s_step_text);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(s_model.sent, s->sent, s_step_text);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(s_model.superseded, s->superseded, s_step_text);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(s_model.dropped, s->dropped, s_step_text);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(s_model.expired, s->expired, s_step_text);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(model_pending(), s->pending, s_step_text);

    /* The same for each slot: a value waiting in it and one in a mailbox are its pending. */
    for (uint8_t slot = 0U; slot < 16U; slot++) {
        const can_txq_slot_stats_t c = slot_counts(slot);
        const uint32_t pending =
            (s_model.waiting[slot].present ? 1U : 0U) + (s_model.loaded[slot].present ? 1U : 0U);

        TEST_ASSERT_EQUAL_UINT64_MESSAGE(s_model.slot_produced[slot], c.produced, s_step_text);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(s_model.slot_sent[slot], c.sent, s_step_text);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(s_model.slot_superseded[slot], c.superseded, s_step_text);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(pending, c.pending, s_step_text);
    }

    /* The bus-off policy. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(s_model.bus_off_count, s->bus_off_count, s_step_text);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(s_model.restart_requests, s->restart_requests, s_step_text);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(s_model.holdoff_ms, s->holdoff_ms, s_step_text);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(s_model.recall_requests, s->recall_requests, s_step_text);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(model_deadline(), deadline(), s_step_text);
}

/* A frame for the scenario. Six arbitration fields are enough: with 16 slots and a FIFO
 * drawing from them, frames with equal keys meet all the time, which is what the tie rule
 * needs. A serial number in the payload makes every frame different from every other. */
static can_frame_t scenario_frame(void)
{
    static const uint32_t ids[6] = {0x090U, 0x100U, 0x100U, 0x04000123U, 0x200U, 0x7E8U};
    static const uint8_t flags[6] = {0U, 0U, CAN_FLAG_RTR, CAN_FLAG_EXT, 0U, 0U};
    const uint32_t pick = random_u32() % 6U;
    const uint32_t more = s_serial * 0x9E3779B1U;
    can_frame_t frame = data_frame(ids[pick], s_serial);

    frame.flags = flags[pick];
    frame.dlc = (uint8_t)(4U + (random_u32() % 5U));
    frame.data[4] = (uint8_t)(more >> 24U);
    frame.data[5] = (uint8_t)(more >> 16U);
    frame.data[6] = (uint8_t)(more >> 8U);
    frame.data[7] = (uint8_t)more;
    s_serial++;

    return frame;
}

/* One of the frames that are in a mailbox, picked at random, or SOURCE_NONE if none is. */
static uint32_t pick_loaded(void)
{
    uint32_t sources[17] = {0U};
    uint32_t count = 0U;

    for (uint32_t source = 0U; source <= SOURCE_EVENT; source++) {
        if (model_loaded(source)->present) {
            sources[count] = source;
            count++;
        }
    }

    return (count == 0U) ? SOURCE_NONE : sources[random_u32() % count];
}

static void remember_retired(uint32_t handle)
{
    s_retired[s_retired_count % 16U] = handle;
    s_retired_count++;
}

/* The frame of `source` that is in a mailbox comes back without having been sent. */
static void abort_one(uint32_t source)
{
    const uint32_t handle = model_loaded(source)->handle;

    model_aborted(source);
    TEST_ASSERT_TRUE_MESSAGE(can_txq_on_tx_aborted(&s_queue, handle), s_step_text);
    remember_retired(handle);
    check_against_model();
}

static void step_publish(void)
{
    const can_frame_t frame = scenario_frame();
    const uint8_t slot = (uint8_t)(random_u32() % 16U);

    if ((random_u32() % 40U) == 0U) {
        /* Now and then a call that must be refused and must not count. */
        can_frame_t bad_frame = frame;

        bad_frame.dlc = 9U;

        if ((random_u32() % 2U) == 0U) {
            TEST_ASSERT_FALSE_MESSAGE(can_txq_publish(&s_queue, slot, &bad_frame, s_now_ns),
                                      s_step_text);
        } else {
            const uint8_t bad_slot = (uint8_t)(16U + (random_u32() % 240U));

            TEST_ASSERT_FALSE_MESSAGE(can_txq_publish(&s_queue, bad_slot, &frame, s_now_ns),
                                      s_step_text);
        }

        s_seen.invalid_calls++;
    } else {
        model_publish(slot, &frame, s_now_ns);
        TEST_ASSERT_TRUE_MESSAGE(can_txq_publish(&s_queue, slot, &frame, s_now_ns), s_step_text);
    }

    check_against_model();
}

static void enqueue_one(void)
{
    const can_frame_t frame = scenario_frame();
    const bool expected = model_enqueue(&frame, s_now_ns);

    TEST_ASSERT_EQUAL_MESSAGE(expected, can_txq_enqueue(&s_queue, &frame, s_now_ns), s_step_text);
    check_against_model();
}

static void step_enqueue(void)
{
    if ((random_u32() % 40U) == 0U) {
        TEST_ASSERT_FALSE_MESSAGE(can_txq_enqueue(&s_queue, NULL, s_now_ns), s_step_text);
        s_seen.invalid_calls++;
        check_against_model();
    } else {
        enqueue_one();
    }
}

/* Up to 64 event frames within a few microseconds, twice what the FIFO takes. If an event
 * frame is in a mailbox at that moment, it is aborted right afterwards: this is how the
 * scenario gets to see a frame come back to a FIFO that has filled up behind it. */
static void step_enqueue_burst(void)
{
    const uint32_t count = 1U + (random_u32() % 64U);

    for (uint32_t i = 0U; i < count; i++) {
        s_now_ns += 1U + (random_u32() % 1000U);
        enqueue_one();
    }

    if (s_model.event_loaded.present) {
        abort_one(SOURCE_EVENT);
    }
}

static void step_load(void)
{
    can_frame_t frame = data_frame(0x7FFU, 0xFFFFFFFFU);
    uint32_t handle = 0U;
    const uint32_t source = model_choose(s_now_ns);
    const model_frame_t *expected = (source == SOURCE_NONE) ? NULL : model_candidate(source);
    const bool handed_out = can_txq_next(&s_queue, s_now_ns, &frame, &handle);

    /* A frame is handed out exactly when the model has one to hand out. */
    TEST_ASSERT_EQUAL_MESSAGE(expected != NULL, handed_out, s_step_text);

    if (expected != NULL) {
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(expected->frame.id, frame.id, s_step_text);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(expected->frame.flags, frame.flags, s_step_text);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(expected->frame.dlc, frame.dlc, s_step_text);

        /* The payload of a data frame, which holds the serial number: this very frame. */
        if ((expected->frame.flags & CAN_FLAG_RTR) == 0U) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(expected->frame.data, frame.data,
                                                 expected->frame.dlc, s_step_text);
        }

        /* Never a handle that is still out. */
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(SOURCE_NONE, model_find(handle), s_step_text);

        s_seen.handed_out++;

        if (model_choice_was_a_tie(source, expected)) {
            s_seen.ties++;
        }

        model_load(source, handle);
    } else if (!s_model.bus_off && (model_pending() > model_mailboxes_in_use()) &&
               (model_mailboxes_in_use() >= s_model.mailboxes)) {
        s_seen.declined_full++;
    } else {
        /* declined for another reason: bus off, or nothing that may go now */
    }

    check_against_model();
}

static void step_done(void)
{
    const uint32_t source = pick_loaded();

    if (source != SOURCE_NONE) {
        const model_frame_t expected = *model_loaded(source);
        const bool cyclic = (source != SOURCE_EVENT);
        can_txq_done_t info = {.cyclic = !cyclic,
                               .slot = 0xEEU,
                               .id = ~expected.frame.id,
                               .stamp_ns = ~expected.stamp_ns};

        TEST_ASSERT_TRUE_MESSAGE(can_txq_on_tx_done(&s_queue, expected.handle, &info), s_step_text);
        TEST_ASSERT_EQUAL_MESSAGE(cyclic, info.cyclic, s_step_text);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(expected.frame.id, info.id, s_step_text);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(expected.stamp_ns, info.stamp_ns, s_step_text);

        /* "cyclic: the slot; event frame: 0" */
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cyclic ? source : 0U, info.slot, s_step_text);

        model_done(source);
        remember_retired(expected.handle);
        check_against_model();
    }
}

static void step_abort(void)
{
    const uint32_t source = pick_loaded();

    if (source != SOURCE_NONE) {
        abort_one(source);
    }
}

/* A report with a handle that is not outstanding: any number, a handle that was completed a
 * moment ago, or a handle that is out with one bit flipped. */
static void step_wrong_handle(void)
{
    const uint32_t kind = random_u32() % 3U;
    const uint32_t source = pick_loaded();
    uint32_t handle = random_u32();

    if (kind == 1U) {
        handle = s_retired[random_u32() % 16U];
    } else if ((kind == 2U) && (source != SOURCE_NONE)) {
        handle = model_loaded(source)->handle ^ (1U << (random_u32() % 32U));
    } else {
        /* any number */
    }

    if (model_find(handle) == SOURCE_NONE) {
        take_snapshot();

        if ((random_u32() % 2U) == 0U) {
            TEST_ASSERT_FALSE_MESSAGE(can_txq_on_tx_done(&s_queue, handle, NULL), s_step_text);
        } else {
            TEST_ASSERT_FALSE_MESSAGE(can_txq_on_tx_aborted(&s_queue, handle), s_step_text);
        }

        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&s_snapshot, &s_queue, sizeof(s_queue), s_step_text);
        s_seen.refused_handles++;
        check_against_model();
    }
}

/* What the driver does when the queue asks for an abort, after a state report or a poll. Three
 * times out of four it does as it was asked, and every frame comes back from its mailbox, in
 * no particular order. Otherwise the frames stay where they are until a later step confirms or
 * aborts them, as the frame on the wire does when its attempt is still running. */
static void answer_abort_request(uint32_t requests)
{
    if (((requests & ABORT) != 0U) && ((random_u32() % 4U) != 0U)) {
        uint32_t source = pick_loaded();

        while (source != SOURCE_NONE) {
            abort_one(source);
            source = pick_loaded();
        }
    }
}

static void step_state(void)
{
    const uint32_t dice = random_u32() % 8U;
    can_fc_state_t state = CAN_FC_ERROR_ACTIVE;
    uint32_t expected;

    if (!s_model.bus_off) {
        /* On the bus. A node that has just come back goes bus off again seven times out of
         * eight, as it does when the fault is still there: that is what makes the hold-off
         * climb. A node that has been stable does so one time in eight. The other reports
         * say error passive or error active, which must change nothing. */
        const uint32_t bus_off_below = s_model.stable_time_running ? 7U : 1U;

        if (dice < bus_off_below) {
            state = CAN_FC_BUS_OFF;
        } else if (dice < 5U) {
            state = CAN_FC_ERROR_PASSIVE;
        } else {
            /* error active */
        }
    } else if (s_model.restart_owed) {
        /* Bus off and not yet asked to restart: the controller repeats its report. Once in
         * a while it comes back by itself. */
        if ((random_u32() % 16U) != 0U) {
            state = CAN_FC_BUS_OFF;
        }
    } else {
        /* Asked to restart: it comes back, error active as a rule. */
        if (dice == 6U) {
            state = CAN_FC_ERROR_PASSIVE;
        } else if (dice == 7U) {
            state = CAN_FC_BUS_OFF;
        } else {
            /* error active */
        }
    }

    expected = model_state(state, s_now_ns);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(expected, can_txq_on_state(&s_queue, state, s_now_ns),
                                    s_step_text);
    check_against_model();
    answer_abort_request(expected);
}

/* Time passes, and the queue is polled. One time in four the scenario goes straight to the
 * next deadline: one nanosecond short of it, exactly to it, or one nanosecond past it. */
static void step_time(void)
{
    const uint64_t deadline_before_ns = deadline();
    const uint64_t expired_before = stats()->expired;
    const uint32_t holdoff_before_ms = stats()->holdoff_ms;
    const uint32_t dice = random_u32() % 12U;
    uint32_t expected;
    uint32_t requests;
    bool acted;

    if ((dice < 3U) && (deadline_before_ns != CAN_TXQ_NO_DEADLINE) &&
        (deadline_before_ns > s_now_ns)) {
        s_now_ns = (deadline_before_ns - 1U) + dice;
    } else if (dice == 4U) {
        s_now_ns += random_u32() % 10000000U; /* a pause of up to 10 ms */
    } else {
        s_now_ns += random_u32() % 500000U; /* up to 0.5 ms */
    }

    expected = model_poll(s_now_ns);
    requests = can_txq_poll(&s_queue, s_now_ns);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(expected, requests, s_step_text);

    /* What can_txq_next_deadline() promises, checked without the model: a poll before the
     * deadline does nothing, a poll at or after it does something, and afterwards the
     * deadline lies in the future. */
    acted = (requests != 0U) || (stats()->expired != expired_before) ||
            (stats()->holdoff_ms != holdoff_before_ms);
    TEST_ASSERT_EQUAL_MESSAGE(s_now_ns >= deadline_before_ns, acted, s_step_text);
    TEST_ASSERT_TRUE_MESSAGE(deadline() > s_now_ns, s_step_text);

    check_against_model();

    if ((requests & ABORT) != 0U) {
        const uint32_t aborts_before =
            s_seen.aborts_to_slot + s_seen.aborts_superseded + s_seen.aborts_to_fifo;

        answer_abort_request(requests);

        if ((s_seen.aborts_to_slot + s_seen.aborts_superseded + s_seen.aborts_to_fifo) !=
            aborts_before) {
            s_seen.recalls_carried_out++;
        }
    }
}

static void begin_step(uint32_t step, const char *what)
{
    (void)snprintf(s_step_text, sizeof(s_step_text), "step %lu: %s", (unsigned long)step, what);
}

/* The end of a scenario: the node comes back if it was off the bus, and the queue is emptied.
 * The model says which frames must still come out and in which order; step_load() and
 * step_done() compare each of them as before. */
static void drain(void)
{
    begin_step(SCENARIO_STEPS, "drain");

    if (s_model.bus_off) {
        TEST_ASSERT_EQUAL_HEX32(0U, model_state(CAN_FC_ERROR_ACTIVE, s_now_ns));
        TEST_ASSERT_EQUAL_HEX32(0U, can_txq_on_state(&s_queue, CAN_FC_ERROR_ACTIVE, s_now_ns));
        check_against_model();
    }

    /* At most 57 frames can be pending: 16 waiting values, 33 event frames, 8 mailboxes. Each
     * round confirms what is out and then asks for a frame nine times, once more than there
     * are mailboxes, so that the queue is also seen to decline. */
    for (uint32_t round = 0U; (round < 64U) && (model_pending() > 0U); round++) {
        while (pick_loaded() != SOURCE_NONE) {
            step_done();
        }

        for (uint32_t i = 0U; i < 9U; i++) {
            step_load();
        }
    }

    TEST_ASSERT_EQUAL_UINT32(0U, model_pending());
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->pending);
    TEST_ASSERT_EQUAL_UINT64(stats()->produced, stats()->sent + stats()->superseded +
                                                    stats()->dropped + stats()->expired);
}

static void run_scenario(uint32_t seed, uint8_t mailboxes, uint32_t event_timeout_ms)
{
    static char summary[768];

    s_random_state = seed;
    start_with(mailboxes, SCENARIO_HOLDOFF_INITIAL_MS, SCENARIO_HOLDOFF_MAX_MS, SCENARIO_STABLE_MS,
               event_timeout_ms);

    (void)memset(&s_model, 0, sizeof(s_model));
    s_model.mailboxes = mailboxes;
    s_model.event_timeout_ms = event_timeout_ms;
    s_model.holdoff_ms = SCENARIO_HOLDOFF_INITIAL_MS;
    (void)memset(&s_seen, 0, sizeof(s_seen));
    (void)memset(s_retired, 0, sizeof(s_retired));
    s_retired_count = 0U;
    s_serial = 1U;
    s_now_ns = ms(1000U);

    /* The mix, in thousandths. It is hard on the queue on purpose: the node is off the bus
     * for much of the time, so only about a third of the frames go out and the rest are
     * superseded, dropped or expired. The line each scenario prints at its end says how often
     * each case occurred. */
    for (uint32_t step = 0U; step < SCENARIO_STEPS; step++) {
        const uint32_t dice = random_u32() % 1000U;

        /* Time never stands still, so every frame has a stamp of its own. */
        s_now_ns += 1U + (random_u32() % 100000U);

        if (dice < 150U) {
            begin_step(step, "publish");
            step_publish();
        } else if (dice < 200U) {
            begin_step(step, "enqueue");
            step_enqueue();
        } else if (dice < 203U) {
            begin_step(step, "burst of event frames");
            step_enqueue_burst();
        } else if (dice < 503U) {
            begin_step(step, "next");
            step_load();
        } else if (dice < 753U) {
            begin_step(step, "done");
            step_done();
        } else if (dice < 783U) {
            begin_step(step, "aborted");
            step_abort();
        } else if (dice < 813U) {
            begin_step(step, "report with a handle that is not outstanding");
            step_wrong_handle();
        } else if (dice < 853U) {
            begin_step(step, "state");
            step_state();
        } else {
            begin_step(step, "time passes, poll");
            step_time();
        }
    }

    drain();

    (void)snprintf(
        summary, sizeof(summary),
        "%lu calls in %lu ms: %lu produced, %lu sent, %lu superseded, %lu dropped, "
        "%lu expired; "
        "%lu handed out (%lu ties, %lu declined for lack of a mailbox); "
        "aborts: %lu to slot, %lu superseded, %lu to FIFO (%lu when full); "
        "%lu bus-offs (%lu at maximum hold-off, %lu early recoveries), "
        "%lu restart requests, %lu hold-off resets; "
        "%lu recall requests (%lu for a value, %lu for an event frame, %lu carried "
        "out, %lu notes withdrawn); "
        "%lu refused handles, %lu invalid calls",
        (unsigned long)s_seen.calls, (unsigned long)((s_now_ns - ms(1000U)) / 1000000ULL),
        (unsigned long)stats()->produced, (unsigned long)stats()->sent,
        (unsigned long)stats()->superseded, (unsigned long)stats()->dropped,
        (unsigned long)stats()->expired, (unsigned long)s_seen.handed_out,
        (unsigned long)s_seen.ties, (unsigned long)s_seen.declined_full,
        (unsigned long)s_seen.aborts_to_slot, (unsigned long)s_seen.aborts_superseded,
        (unsigned long)s_seen.aborts_to_fifo, (unsigned long)s_seen.aborts_to_full_fifo,
        (unsigned long)stats()->bus_off_count, (unsigned long)s_seen.holdoff_at_maximum,
        (unsigned long)s_seen.early_recoveries, (unsigned long)stats()->restart_requests,
        (unsigned long)s_seen.holdoff_resets, (unsigned long)stats()->recall_requests,
        (unsigned long)s_seen.recalls_for_values, (unsigned long)s_seen.recalls_for_events,
        (unsigned long)s_seen.recalls_carried_out, (unsigned long)s_seen.recalls_withdrawn,
        (unsigned long)s_seen.refused_handles, (unsigned long)s_seen.invalid_calls);
    TEST_MESSAGE(summary);

    /* Agreement with the model means little if the scenario never got into the situations
     * that are hard. The numbers above are the same on every run. Each has to stay clear of
     * its floor here, which is about half of the smallest value the three scenarios reach. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(20000U, s_seen.calls, "checked calls");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64_MESSAGE(1800U, stats()->sent, "sent");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64_MESSAGE(1200U, stats()->superseded, "superseded");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64_MESSAGE(500U, stats()->dropped, "dropped");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(600U, s_seen.ties, "equal keys");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(30U, s_seen.declined_full, "all mailboxes in use");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(200U, s_seen.aborts_to_slot, "aborted to slot");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(5U, s_seen.aborts_superseded,
                                                "aborted and superseded");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(20U, s_seen.aborts_to_fifo, "aborted to FIFO");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(3U, s_seen.aborts_to_full_fifo,
                                                "aborted to a full FIFO");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(100U, stats()->bus_off_count, "bus-offs");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(10U, s_seen.holdoff_at_maximum,
                                                "bus-offs at the maximum hold-off");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(5U, s_seen.early_recoveries, "early recoveries");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(100U, stats()->restart_requests,
                                                "restart requests");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(40U, s_seen.holdoff_resets, "hold-off resets");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(15U, s_seen.recalls_for_values,
                                                "recalls for a value");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(15U, s_seen.recalls_carried_out,
                                                "recalls carried out");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(30U, s_seen.recalls_withdrawn,
                                                "recall notes withdrawn");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(500U, s_seen.refused_handles, "refused handles");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(100U, s_seen.invalid_calls, "invalid calls");

    if (event_timeout_ms == 0U) {
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(0U, stats()->expired, "expired without a timeout");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_seen.recalls_for_events,
                                         "event frames recalled without a timeout");
    } else {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT64_MESSAGE(1500U, stats()->expired, "expired");
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(50U, s_seen.recalls_for_events,
                                                    "recalls for an event frame");
    }
}

static void test_random_scenario_with_1_mailbox_agrees_with_the_model_after_every_step(void)
{
    run_scenario(0x13579BDFU, 1U, 0U);
}

static void test_random_scenario_with_3_mailboxes_agrees_with_the_model_after_every_step(void)
{
    run_scenario(0x2468ACE1U, 3U, 15U);
}

static void test_random_scenario_with_8_mailboxes_agrees_with_the_model_after_every_step(void)
{
    run_scenario(0x0F1E2D3CU, 8U, 6U);
}

/* ------------------------------------------------------------------------------------------
 * A caller that sleeps until the deadline
 *
 * The transmit task of the application does not poll on a timer: it sleeps until
 * can_txq_next_deadline(), reads the deadline again after each of its own calls, and is woken
 * early only by the calls of others (a publish, an enqueue). That is safe only if the deadline
 * never lies. step_time() above checks the two halves of that one poll at a time; the scenario
 * here checks the consequence the task relies on, with two queues that receive the same calls:
 *
 *   - the eager queue is polled at every opportunity;
 *   - the lazy queue is polled at an opportunity only if its time has reached the deadline,
 *     as read after the previous call.
 *
 * Where the lazy caller sleeps, the eager poll must have had nothing to do: no request, no
 * counter moved. Where it wakes, both polls return the same, and its deadline is then later
 * than the time of the poll. Every other call returns the same on both queues, frame for frame
 * and handle for handle, and their counters and deadlines are compared after every call, so the
 * two cannot drift apart unnoticed. One opportunity in three is skipped by both (the task is
 * busy elsewhere), so that a recall can be withdrawn before anyone polls. A request to abort is
 * carried out on both alike: each frame comes back at once three times in four, and otherwise
 * stays in its mailbox until a later step confirms or aborts it, as a frame on the wire does.
 *
 * One call in eight carries a clock reading up to 5 us older than the time of the step, as when
 * two tasks read the clock and reach the queue in the other order.
 * ---------------------------------------------------------------------------------------- */

#define TWIN_STEPS 30000U

static can_txq_t s_lazy;

typedef struct {
    uint32_t handles[8]; /* the frames in mailboxes, the same on both queues */
    uint32_t count;
    bool bus_off;          /* as last reported */
    bool restart_asked;    /* bus off, and the queue has asked for the recovery */
    uint64_t lazy_wake_ns; /* the lazy queue's deadline, read after its last call */

    /* How often the scenario got into the situations it is there for. */
    uint32_t opportunities;    /* polls of the eager queue */
    uint32_t slept;            /* of those, with the lazy caller asleep */
    uint32_t woke_at_deadline; /* lazy polls exactly at the deadline */
    uint32_t abort_requests;   /* polls that asked for an abort */
    uint32_t event_recalls;    /* of those, with a deadline other than 0: an event frame's */
    uint32_t withdrawn;        /* a deadline of 0 that went away without a poll */
    uint32_t steps_back;       /* calls with a clock reading from before the step */
} twin_t;

static twin_t s_twin;

static uint64_t twin_time(void)
{
    uint64_t at_ns = s_now_ns;

    if ((random_u32() % 8U) == 0U) {
        at_ns -= 1U + (random_u32() % 5000U);
        s_twin.steps_back++;
    }

    return at_ns;
}

/* The two queues are the same to a caller: same counters, same deadline. */
static void check_twins(bool polled)
{
    const can_txq_stats_t *eager = can_txq_stats(&s_queue);
    const can_txq_stats_t *lazy = can_txq_stats(&s_lazy);
    const uint64_t deadline_ns = can_txq_next_deadline(&s_lazy);

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(eager->produced,
                                     eager->sent + eager->superseded + eager->dropped +
                                         eager->expired + eager->pending,
                                     s_step_text);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(eager, lazy, sizeof(*eager), s_step_text);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(can_txq_next_deadline(&s_queue), deadline_ns, s_step_text);

    if (!polled && (s_twin.lazy_wake_ns == 0U) && (deadline_ns != 0U)) {
        s_twin.withdrawn++;
    }

    s_twin.lazy_wake_ns = deadline_ns;
}

static void twin_forget(uint32_t index)
{
    s_twin.count--;
    s_twin.handles[index] = s_twin.handles[s_twin.count];
}

static void twin_abort(uint32_t index)
{
    const uint32_t handle = s_twin.handles[index];

    TEST_ASSERT_TRUE_MESSAGE(can_txq_on_tx_aborted(&s_queue, handle), s_step_text);
    TEST_ASSERT_TRUE_MESSAGE(can_txq_on_tx_aborted(&s_lazy, handle), s_step_text);
    twin_forget(index);
    check_twins(false);
}

static void twin_carry_out(uint32_t requests)
{
    if ((requests & RESTART) != 0U) {
        s_twin.restart_asked = true;
    }

    if ((requests & ABORT) != 0U) {
        for (uint32_t i = s_twin.count; i > 0U; i--) {
            if ((random_u32() % 4U) != 0U) {
                twin_abort(i - 1U);
            }
        }
    }
}

static void twin_poll_opportunity(void)
{
    if ((random_u32() % 3U) != 0U) {
        const uint64_t at_ns = twin_time();
        const can_txq_stats_t before = *can_txq_stats(&s_queue);
        const uint32_t requests = can_txq_poll(&s_queue, at_ns);

        s_twin.opportunities++;

        if (at_ns >= s_twin.lazy_wake_ns) {
            if (at_ns == s_twin.lazy_wake_ns) {
                s_twin.woke_at_deadline++;
            }

            TEST_ASSERT_EQUAL_HEX32_MESSAGE(requests, can_txq_poll(&s_lazy, at_ns), s_step_text);
            TEST_ASSERT_TRUE_MESSAGE(can_txq_next_deadline(&s_lazy) > at_ns, s_step_text);
        } else {
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(0U, requests, s_step_text);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, can_txq_stats(&s_queue), sizeof(before),
                                             s_step_text);
            s_twin.slept++;
        }

        if ((requests & ABORT) != 0U) {
            s_twin.abort_requests++;

            /* The recall of a value is due with a deadline of 0, that of an event frame at its
             * timeout. */
            if (s_twin.lazy_wake_ns != 0U) {
                s_twin.event_recalls++;
            }
        }

        check_twins(true);
        twin_carry_out(requests);
    }
}

static void twin_publish(void)
{
    const uint8_t slot = (uint8_t)(random_u32() % 6U);
    const can_frame_t frame = data_frame(0x100U + (random_u32() % 4U), s_serial);
    const uint64_t at_ns = twin_time();

    s_serial++;
    TEST_ASSERT_TRUE_MESSAGE(can_txq_publish(&s_queue, slot, &frame, at_ns), s_step_text);
    TEST_ASSERT_TRUE_MESSAGE(can_txq_publish(&s_lazy, slot, &frame, at_ns), s_step_text);
    check_twins(false);
}

static void twin_enqueue(void)
{
    const can_frame_t frame = data_frame(((random_u32() % 2U) == 0U) ? 0x080U : 0x700U, s_serial);
    const uint64_t at_ns = twin_time();
    const bool accepted = can_txq_enqueue(&s_queue, &frame, at_ns);

    s_serial++;
    TEST_ASSERT_EQUAL_MESSAGE(accepted, can_txq_enqueue(&s_lazy, &frame, at_ns), s_step_text);
    check_twins(false);
}

static void twin_load(void)
{
    const uint64_t at_ns = twin_time();
    bool more = true;

    while (more) {
        can_frame_t eager = data_frame(0x7FFU, 0xFFFFFFFFU);
        can_frame_t lazy = eager;
        uint32_t eager_handle = 0U;
        uint32_t lazy_handle = 0U;

        more = can_txq_next(&s_queue, at_ns, &eager, &eager_handle);
        TEST_ASSERT_EQUAL_MESSAGE(more, can_txq_next(&s_lazy, at_ns, &lazy, &lazy_handle),
                                  s_step_text);

        if (more) {
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(eager_handle, lazy_handle, s_step_text);
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(eager.id, lazy.id, s_step_text);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(tag_of(&eager), tag_of(&lazy), s_step_text);
            TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(8U, s_twin.count, s_step_text);
            s_twin.handles[s_twin.count] = eager_handle;
            s_twin.count++;
        }

        check_twins(false);
    }
}

static void twin_done(void)
{
    if (s_twin.count > 0U) {
        const uint32_t index = random_u32() % s_twin.count;
        can_txq_done_t eager = {.cyclic = false, .slot = 0xEEU, .id = 0U, .stamp_ns = 0U};
        can_txq_done_t lazy = eager;

        TEST_ASSERT_TRUE_MESSAGE(can_txq_on_tx_done(&s_queue, s_twin.handles[index], &eager),
                                 s_step_text);
        TEST_ASSERT_TRUE_MESSAGE(can_txq_on_tx_done(&s_lazy, s_twin.handles[index], &lazy),
                                 s_step_text);
        TEST_ASSERT_MESSAGE(eager.cyclic == lazy.cyclic, s_step_text);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(eager.slot, lazy.slot, s_step_text);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(eager.id, lazy.id, s_step_text);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(eager.stamp_ns, lazy.stamp_ns, s_step_text);
        twin_forget(index);
        check_twins(false);
    }
}

/* Bus off now and then; back once asked to restart, and now and then before. */
static void twin_state(void)
{
    const uint32_t dice = random_u32() % 8U;
    can_fc_state_t state = CAN_FC_ERROR_ACTIVE;
    uint64_t at_ns;
    uint32_t requests;

    if (!s_twin.bus_off) {
        if (dice == 0U) {
            state = CAN_FC_BUS_OFF;
        } else if (dice < 4U) {
            state = CAN_FC_ERROR_PASSIVE;
        } else {
            /* error active */
        }
    } else if (!s_twin.restart_asked && (dice != 0U)) {
        state = CAN_FC_BUS_OFF;
    } else {
        /* back on the bus */
    }

    at_ns = twin_time();
    requests = can_txq_on_state(&s_queue, state, at_ns);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(requests, can_txq_on_state(&s_lazy, state, at_ns), s_step_text);

    if ((state == CAN_FC_BUS_OFF) && !s_twin.bus_off) {
        s_twin.restart_asked = false;
    }

    s_twin.bus_off = (state == CAN_FC_BUS_OFF);
    check_twins(false);
    twin_carry_out(requests);
}

/* Time passes: a pause of up to 10 ms, or, one time in two, straight to the lazy deadline:
 * one nanosecond short of it, exactly to it, or one nanosecond past it. */
static void twin_time_passes(void)
{
    const uint32_t dice = random_u32() % 6U;

    if ((dice < 3U) && (s_twin.lazy_wake_ns != CAN_TXQ_NO_DEADLINE) &&
        (s_twin.lazy_wake_ns > s_now_ns)) {
        s_now_ns = (s_twin.lazy_wake_ns - 1U) + dice;
    } else {
        s_now_ns += random_u32() % 10000000U;
    }
}

static void run_twins(uint32_t seed, uint8_t mailboxes, uint32_t event_timeout_ms)
{
    static char summary[384];
    const can_txq_config_t config = {.mailboxes = mailboxes,
                                     .holdoff_initial_ms = SCENARIO_HOLDOFF_INITIAL_MS,
                                     .holdoff_max_ms = SCENARIO_HOLDOFF_MAX_MS,
                                     .stable_ms = SCENARIO_STABLE_MS,
                                     .event_timeout_ms = event_timeout_ms};

    s_random_state = seed;
    (void)memset(&s_lazy, 0x5A, sizeof(s_lazy));
    can_txq_init(&s_queue, &config);
    can_txq_init(&s_lazy, &config);
    (void)memset(&s_twin, 0, sizeof(s_twin));
    s_twin.lazy_wake_ns = CAN_TXQ_NO_DEADLINE;
    s_serial = 1U;
    s_now_ns = ms(1000U);

    for (uint32_t step = 0U; step < TWIN_STEPS; step++) {
        const uint32_t dice = random_u32() % 1000U;

        s_now_ns += 1U + (random_u32() % 100000U);

        if (dice < 250U) {
            begin_step(step, "publish");
            twin_publish();
        } else if (dice < 330U) {
            begin_step(step, "enqueue");
            twin_enqueue();
        } else if (dice < 580U) {
            begin_step(step, "next");
            twin_load();
        } else if (dice < 780U) {
            begin_step(step, "done");
            twin_done();
        } else if (dice < 810U) {
            begin_step(step, "aborted");

            if (s_twin.count > 0U) {
                twin_abort(random_u32() % s_twin.count);
            }
        } else if (dice < 850U) {
            begin_step(step, "state");
            twin_state();
        } else {
            begin_step(step, "time passes");
            twin_time_passes();
        }

        begin_step(step, "poll");
        twin_poll_opportunity();
    }

    (void)snprintf(summary, sizeof(summary),
                   "%lu polls, %lu with the lazy caller asleep, %lu at its deadline; "
                   "%lu abort requests from polls (%lu for an event frame; %lu recall requests), "
                   "%lu recalls withdrawn; %lu bus-offs, %lu expired; "
                   "%lu clock readings from the past",
                   (unsigned long)s_twin.opportunities, (unsigned long)s_twin.slept,
                   (unsigned long)s_twin.woke_at_deadline, (unsigned long)s_twin.abort_requests,
                   (unsigned long)s_twin.event_recalls, (unsigned long)stats()->recall_requests,
                   (unsigned long)s_twin.withdrawn, (unsigned long)stats()->bus_off_count,
                   (unsigned long)stats()->expired, (unsigned long)s_twin.steps_back);
    TEST_MESSAGE(summary);

    /* Every abort a poll asked for is a recall: a restart is asked for while bus off, the
     * recall only on the bus, so the two never come together. */
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(s_twin.abort_requests, stats()->recall_requests,
                                     "abort requests from polls");

    /* The floors are about half of the smallest value the three scenarios reach. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(10000U, s_twin.opportunities, "polls");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(8000U, s_twin.slept, "polls slept through");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(20U, s_twin.woke_at_deadline,
                                                "polls exactly at the deadline");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(250U, s_twin.abort_requests, "abort requests");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(25U, s_twin.withdrawn, "recalls withdrawn");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(50U, stats()->bus_off_count, "bus-offs");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(2000U, s_twin.steps_back,
                                                "clock readings from the past");

    if (event_timeout_ms == 0U) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_twin.event_recalls,
                                         "event frames recalled without a timeout");
    } else {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(75U, s_twin.event_recalls,
                                                    "recalls for an event frame");
        TEST_ASSERT_GREATER_OR_EQUAL_UINT64_MESSAGE(900U, stats()->expired, "expired");
    }
}

static void test_a_caller_that_sleeps_until_the_deadline_misses_nothing_with_1_mailbox(void)
{
    run_twins(0x7A11E5U, 1U, 4U);
}

static void test_a_caller_that_sleeps_until_the_deadline_misses_nothing_with_3_mailboxes(void)
{
    run_twins(0xDEAD11U, 3U, 15U);
}

static void test_a_caller_that_sleeps_until_the_deadline_misses_nothing_with_8_mailboxes(void)
{
    run_twins(0x5EED08U, 8U, 0U);
}

/* can_txq_events_held(): the event frames in the FIFO and the one in a mailbox, through every
 * way in and out (enqueue, hand-out, sent, aborted, expired, dropped), with cyclic frames around
 * that must not count. */
static void test_events_held_counts_the_fifo_and_the_event_frame_in_a_mailbox(void)
{
    can_frame_t frame;
    uint32_t first = 0U;
    uint32_t second = 0U;
    uint32_t cyclic = 0U;

    start(3U, 1000U);
    TEST_ASSERT_EQUAL_UINT32(0U, can_txq_events_held(&s_queue));
    TEST_ASSERT_TRUE(publish(0U, 0x010U, 1U, ms(0)));
    TEST_ASSERT_EQUAL_UINT32(0U, can_txq_events_held(&s_queue));
    TEST_ASSERT_TRUE(enqueue(0x7E8U, 2U, ms(0)));
    TEST_ASSERT_TRUE(enqueue(0x7E8U, 3U, ms(0)));
    TEST_ASSERT_EQUAL_UINT32(2U, can_txq_events_held(&s_queue));

    /* The cyclic frame and the first event frame go into mailboxes; the second event waits. */
    TEST_ASSERT_TRUE(next(ms(1), &frame, &cyclic));
    TEST_ASSERT_EQUAL_HEX32(0x010U, frame.id);
    TEST_ASSERT_TRUE(next(ms(1), &frame, &first));
    TEST_ASSERT_EQUAL_UINT32(2U, tag_of(&frame));
    TEST_ASSERT_FALSE(next(ms(1), &frame, &second));
    TEST_ASSERT_EQUAL_UINT32(2U, can_txq_events_held(&s_queue));
    TEST_ASSERT_TRUE(done(cyclic));
    TEST_ASSERT_EQUAL_UINT32(2U, can_txq_events_held(&s_queue));

    /* Sent: one left. Aborted: back in the FIFO, still held. */
    TEST_ASSERT_TRUE(done(first));
    TEST_ASSERT_EQUAL_UINT32(1U, can_txq_events_held(&s_queue));
    TEST_ASSERT_TRUE(next(ms(2), &frame, &second));
    TEST_ASSERT_EQUAL_UINT32(1U, can_txq_events_held(&s_queue));
    TEST_ASSERT_TRUE(aborted(second));
    TEST_ASSERT_EQUAL_UINT32(1U, can_txq_events_held(&s_queue));

    /* Expired at its timeout: nothing held any more. */
    (void)poll(ms(1000));
    TEST_ASSERT_EQUAL_UINT32(0U, can_txq_events_held(&s_queue));
    EXPECT_COUNTS(3U, 2U, 0U, 0U, 1U, 0U);

    /* A frame the full FIFO refuses is not held. */
    for (uint32_t i = 0U; i < CAN_TXQ_EVENT_DEPTH; i++) {
        TEST_ASSERT_TRUE(enqueue(0x7E8U, i, ms(1001)));
    }

    TEST_ASSERT_FALSE(enqueue(0x7E8U, 99U, ms(1001)));
    TEST_ASSERT_EQUAL_UINT32(CAN_TXQ_EVENT_DEPTH, can_txq_events_held(&s_queue));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_events_held_counts_the_fifo_and_the_event_frame_in_a_mailbox);
    RUN_TEST(test_a_new_queue_is_empty_and_on_the_bus);
    RUN_TEST(test_init_clamps_the_number_of_mailboxes_to_1_through_8);
    RUN_TEST(test_init_raises_a_maximum_holdoff_that_is_below_the_initial_one);
    RUN_TEST(test_init_makes_a_used_queue_new_again);

    RUN_TEST(test_publish_refuses_an_invalid_slot_or_frame_and_does_not_count_it);
    RUN_TEST(test_publishing_again_replaces_the_waiting_value_and_counts_it_as_superseded);
    RUN_TEST(test_a_value_in_a_mailbox_is_not_replaced_and_the_new_value_waits_for_it);
    RUN_TEST(test_the_waiting_value_is_loaded_once_the_earlier_one_is_aborted);
    RUN_TEST(test_an_aborted_value_returns_to_its_slot_when_no_newer_one_waits);
    RUN_TEST(test_each_slot_accounts_for_the_values_of_its_own_message);

    RUN_TEST(test_enqueue_refuses_an_invalid_frame_and_does_not_count_it);
    RUN_TEST(test_events_come_out_in_the_order_they_went_in);
    RUN_TEST(test_only_one_event_frame_is_in_a_mailbox_at_a_time);
    RUN_TEST(test_the_33rd_waiting_event_is_refused_and_counted_as_dropped);
    RUN_TEST(test_the_fifo_keeps_its_order_across_many_wraps_of_its_ring);
    RUN_TEST(test_an_event_expires_at_exactly_the_timeout_and_not_a_nanosecond_earlier);
    RUN_TEST(test_next_hands_out_an_event_a_nanosecond_before_its_timeout_but_not_at_it);
    RUN_TEST(test_events_never_expire_when_the_timeout_is_zero);
    RUN_TEST(test_expired_events_are_skipped_without_using_a_mailbox);
    RUN_TEST(test_an_expired_event_does_not_keep_a_cyclic_frame_from_the_mailbox);
    RUN_TEST(test_expiry_does_not_wait_for_a_free_mailbox_or_for_the_bus);
    RUN_TEST(test_enqueue_discards_expired_frames_before_it_judges_whether_the_fifo_is_full);
    RUN_TEST(test_an_event_in_a_mailbox_does_not_expire);
    RUN_TEST(test_an_aborted_event_is_still_timed_from_its_enqueue);

    RUN_TEST(test_the_frame_with_the_smallest_arbitration_key_goes_first);
    RUN_TEST(test_only_the_oldest_event_competes_with_the_cyclic_slots);
    RUN_TEST(test_equal_keys_go_to_the_lower_slot_first_and_to_the_event_last);
    RUN_TEST(test_never_more_than_1_mailbox_in_use_when_1_is_configured);
    RUN_TEST(test_never_more_than_3_mailboxes_in_use_when_3_are_configured);
    RUN_TEST(test_never_more_than_8_mailboxes_in_use_when_8_are_configured);
    RUN_TEST(test_an_event_frame_takes_a_mailbox_like_a_cyclic_one);
    RUN_TEST(test_nothing_is_handed_out_while_the_controller_is_bus_off);
    RUN_TEST(test_next_needs_both_outputs_and_leaves_them_alone_when_it_declines);
    RUN_TEST(test_next_with_a_null_output_changes_nothing_not_even_expiry);

    RUN_TEST(test_unknown_and_completed_handles_are_refused_and_change_nothing);
    RUN_TEST(test_an_event_frame_reports_slot_0_from_a_record_that_held_another_slot);
    RUN_TEST(test_an_aborted_event_goes_back_to_the_head_of_the_fifo);
    RUN_TEST(test_an_aborted_event_returns_to_the_head_even_when_the_fifo_has_filled_up);
    RUN_TEST(test_handles_are_never_reused_while_outstanding);

    RUN_TEST(test_a_value_published_while_the_earlier_one_is_in_a_mailbox_recalls_it);
    RUN_TEST(test_a_recall_is_withdrawn_when_the_earlier_value_is_confirmed_before_the_poll);
    RUN_TEST(test_a_recall_hands_out_the_frames_of_the_other_slots_again_unchanged);
    RUN_TEST(test_an_event_in_a_mailbox_is_recalled_at_exactly_its_timeout);
    RUN_TEST(test_an_event_in_a_mailbox_is_never_recalled_when_the_timeout_is_zero);
    RUN_TEST(test_while_bus_off_a_stale_frame_does_not_ask_for_a_second_abort);
    RUN_TEST(test_an_event_frame_is_recalled_once_even_when_it_outlives_a_bus_off);
    RUN_TEST(test_an_event_timeout_at_the_moment_of_a_bus_off_or_recovery_asks_once);
    RUN_TEST(test_a_clock_reading_from_before_the_enqueue_does_not_recall_an_event_frame);
    RUN_TEST(test_the_reports_of_the_controller_can_make_the_deadline_earlier);
    RUN_TEST(test_only_a_value_behind_its_own_predecessor_asks_and_only_once);
    RUN_TEST(test_a_recall_with_one_mailbox);
    RUN_TEST(test_an_event_frame_in_a_later_mailbox_is_recalled_at_its_own_timeout);
    RUN_TEST(test_the_bus_off_exception_lasts_until_the_node_is_back);
    RUN_TEST(test_a_recall_comes_out_the_same_in_every_order_of_the_abort_reports);

    RUN_TEST(test_going_bus_off_asks_once_for_the_mailboxes_to_be_aborted);
    RUN_TEST(test_restart_is_requested_exactly_once_when_the_holdoff_has_elapsed);
    RUN_TEST(test_holdoff_doubles_with_every_further_bus_off_up_to_the_maximum);
    RUN_TEST(test_holdoff_returns_to_its_initial_value_after_the_stable_time);
    RUN_TEST(test_holdoff_stays_raised_when_the_node_goes_bus_off_before_the_stable_time);
    RUN_TEST(test_the_stable_time_counts_at_the_next_bus_off_even_if_nobody_polled);
    RUN_TEST(test_error_passive_is_not_bus_off_and_changes_nothing);
    RUN_TEST(test_error_passive_does_not_start_the_stable_time_again);
    RUN_TEST(test_a_node_that_comes_back_error_passive_is_back);
    RUN_TEST(test_a_controller_that_comes_back_by_itself_is_not_asked_to_restart);
    RUN_TEST(test_next_deadline_is_the_earliest_of_restart_reset_expiry_and_recall);
    RUN_TEST(test_a_zero_initial_holdoff_asks_for_recovery_at_once_and_never_grows);
    RUN_TEST(test_the_maximum_is_reached_exactly_when_doubling_would_pass_it);
    RUN_TEST(test_doubling_the_holdoff_cannot_overflow);
    RUN_TEST(test_a_clock_reading_from_before_a_timer_started_triggers_nothing);
    RUN_TEST(test_a_deadline_beyond_the_range_of_the_clock_is_no_deadline);
    RUN_TEST(test_an_event_frame_in_a_mailbox_beyond_the_range_of_the_clock_is_never_recalled);

    RUN_TEST(test_buffering_across_a_bus_off);
    RUN_TEST(test_a_fault_without_bus_off_leaves_no_stale_value_in_the_mailboxes);
    RUN_TEST(test_random_scenario_with_1_mailbox_agrees_with_the_model_after_every_step);
    RUN_TEST(test_random_scenario_with_3_mailboxes_agrees_with_the_model_after_every_step);
    RUN_TEST(test_random_scenario_with_8_mailboxes_agrees_with_the_model_after_every_step);
    RUN_TEST(test_a_caller_that_sleeps_until_the_deadline_misses_nothing_with_1_mailbox);
    RUN_TEST(test_a_caller_that_sleeps_until_the_deadline_misses_nothing_with_3_mailboxes);
    RUN_TEST(test_a_caller_that_sleeps_until_the_deadline_misses_nothing_with_8_mailboxes);

    return UNITY_END();
}
