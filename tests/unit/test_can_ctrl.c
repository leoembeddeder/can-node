/* SPDX-License-Identifier: MIT */
/*
 * The CAN controller model of can_ctrl.h, in model time.
 *
 * Every test builds its times by hand. At 500 kbit/s one bit time is exactly 2000 ns, so an
 * expected time is written t0 + bt(n), with n a number of bit times added up in the comment next
 * to it from the frame lengths below and the bus times of the contract in can_ctrl.h:
 *
 *     intermission 3, suspend 8, error frame 14 (6 flag bits + 8 delimiter bits),
 *     an injected transmit error (L / 2) + 14, a short 1 + 14, no acknowledgment (L - 8) + 14,
 *     a bus held dominant: counter steps 6, 7 and 7 bits apart, then every 8 bits,
 *     recovery from bus off: 128 occurrences of 11 recessive bits.
 *
 * Counter values follow the rules of can_fc.h, numbered as in the specification. The bus-level
 * tests at the end drive random scenarios with a fixed seed.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "can/can_bits.h"
#include "can/can_ctrl.h"
#include "unity.h"

#define BITRATE 500000U
#define BIT_NS  2000U

/* n bit times at 500 kbit/s, in nanoseconds. */
static uint64_t bt(uint32_t n)
{
    return (uint64_t)n * BIT_NS;
}

/* ------------------------------------------------------------------- reference frames */

/* Base-format data frames whose stuffed lengths are worked out by hand in
 * tests/unit/test_can_bits.c; test_the_reference_frames_have_the_lengths_used_here pins them,
 * so that every time below rests on them. Keys: 0x000 < 0x009 < 0x078 < 0x104 < 0x2AA. */
#define LEN_000 50U  /* 0x000, DLC 0: 34 dominant bits up to the end of the CRC, 6 stuff bits */
#define LEN_009 49U  /* 0x009, DLC 0: 5 stuff bits */
#define LEN_078 49U  /* 0x078, DLC 0: 5 stuff bits */
#define LEN_104 47U  /* 0x104, DLC 0: 3 stuff bits */
#define LEN_2AA 108U /* 0x2AA, DLC 8, eight bytes 0xAA: alternating bits, no stuff bit */

static can_frame_t base_frame(uint32_t id, uint8_t dlc, uint8_t fill)
{
    can_frame_t frame;

    (void)memset(&frame, 0, sizeof(frame));
    frame.id = id;
    frame.dlc = dlc;
    (void)memset(frame.data, (int)fill, dlc);

    return frame;
}

static can_frame_t frame_000(void)
{
    return base_frame(0x000U, 0U, 0U);
}

static can_frame_t frame_009(void)
{
    return base_frame(0x009U, 0U, 0U);
}

static can_frame_t frame_078(void)
{
    return base_frame(0x078U, 0U, 0U);
}

static can_frame_t frame_104(void)
{
    return base_frame(0x104U, 0U, 0U);
}

static can_frame_t frame_2aa(void)
{
    return base_frame(0x2AAU, 8U, 0xAAU);
}

/* ------------------------------------------------------------------- helpers */

static void init_ctrl(can_ctrl_t *ctrl, uint32_t bitrate, uint64_t now_ns)
{
    const can_ctrl_config_t config = {.bitrate = bitrate};

    can_ctrl_init(ctrl, &config, now_ns);
}

/* Poll at `now`: there must be an action of `kind` at `time`. Reported at the caller's line. */
#define EXPECT_ACTION(ctrl, now, kind, time) expect_action((ctrl), (now), (kind), (time), __LINE__)

static can_ctrl_action_t expect_action(can_ctrl_t *ctrl, uint64_t now_ns,
                                       can_ctrl_action_kind_t kind, uint64_t time_ns, int line)
{
    can_ctrl_action_t action;

    (void)memset(&action, 0, sizeof(action));
    UNITY_TEST_ASSERT(can_ctrl_poll(ctrl, now_ns, &action), line, "an action is due");
    UNITY_TEST_ASSERT_EQUAL_INT(kind, action.kind, line, "action kind");
    UNITY_TEST_ASSERT_EQUAL_UINT64(time_ns, action.time_ns, line, "action time");

    return action;
}

#define EXPECT_NO_ACTION(ctrl, now) expect_no_action((ctrl), (now), __LINE__)

static void expect_no_action(can_ctrl_t *ctrl, uint64_t now_ns, int line)
{
    can_ctrl_action_t action;

    UNITY_TEST_ASSERT(!can_ctrl_poll(ctrl, now_ns, &action), line, "no action is due");
}

#define EXPECT_WAKEUP(ctrl, time)                                                                  \
    UNITY_TEST_ASSERT_EQUAL_UINT64((time), can_ctrl_next_wakeup(ctrl), __LINE__, "next wake-up")

/* The model's next wake-up is `time`, and polling then produces no action. */
#define EXPECT_SILENT_STEP(ctrl, time) expect_silent_step((ctrl), (time), __LINE__)

static void expect_silent_step(can_ctrl_t *ctrl, uint64_t time_ns, int line)
{
    UNITY_TEST_ASSERT_EQUAL_UINT64(time_ns, can_ctrl_next_wakeup(ctrl), line, "next wake-up");
    expect_no_action(ctrl, time_ns, line);
}

#define EXPECT_STATE(ctrl, state, tec, rec) expect_state((ctrl), (state), (tec), (rec), __LINE__)

static void expect_state(const can_ctrl_t *ctrl, can_fc_state_t state, uint16_t tec, uint16_t rec,
                         int line)
{
    UNITY_TEST_ASSERT_EQUAL_INT(state, can_ctrl_state(ctrl), line, "error state");
    UNITY_TEST_ASSERT_EQUAL_UINT16(tec, can_ctrl_tec(ctrl), line, "transmit error count");
    UNITY_TEST_ASSERT_EQUAL_UINT16(rec, can_ctrl_rec(ctrl), line, "receive error count");
}

#define EXPECT_CHANGE(action, from, to) expect_change((action), (from), (to), __LINE__)

static void expect_change(const can_ctrl_action_t *action, can_fc_state_t from, can_fc_state_t to,
                          int line)
{
    UNITY_TEST_ASSERT_EQUAL_INT(CAN_CTRL_ACTION_STATE_CHANGED, action->kind, line, "kind");
    UNITY_TEST_ASSERT_EQUAL_INT(from, action->old_state, line, "old state");
    UNITY_TEST_ASSERT_EQUAL_INT(to, action->state, line, "new state");
}

#define EXPECT_FRAME(expected, actual) expect_frame((expected), (actual), __LINE__)

static void expect_frame(const can_frame_t *expected, const can_frame_t *actual, int line)
{
    UNITY_TEST_ASSERT_EQUAL_HEX32(expected->id, actual->id, line, "identifier");
    UNITY_TEST_ASSERT_EQUAL_HEX8(expected->flags, actual->flags, line, "flags");
    UNITY_TEST_ASSERT_EQUAL_UINT8(expected->dlc, actual->dlc, line, "dlc");

    if (expected->dlc > 0U) { /* Unity refuses to compare 0 bytes */
        UNITY_TEST_ASSERT_EQUAL_MEMORY(expected->data, actual->data, expected->dlc, line, "data");
    }
}

/* Actions collected while driving the model. */
#define LOG_MAX 512U

typedef struct {
    can_ctrl_action_t action[LOG_MAX];
    uint32_t count;
} action_log_t;

static action_log_t s_log;

static void collect(can_ctrl_t *ctrl, uint64_t now_ns, action_log_t *log)
{
    can_ctrl_action_t action;

    while (can_ctrl_poll(ctrl, now_ns, &action)) {
        TEST_ASSERT_TRUE(log->count < LOG_MAX);
        log->action[log->count] = action;
        log->count++;
    }
}

/* Poll at the model's own wake-up times up to `until_ns`, then at `until_ns`. */
static void run_until(can_ctrl_t *ctrl, uint64_t until_ns, action_log_t *log)
{
    uint64_t wake = can_ctrl_next_wakeup(ctrl);
    uint32_t polls = 0U;

    while ((wake != CAN_CTRL_NO_WAKEUP) && (wake <= until_ns)) {
        collect(ctrl, wake, log);
        wake = can_ctrl_next_wakeup(ctrl);
        polls++;
        TEST_ASSERT_TRUE(polls < 100000U);
    }

    collect(ctrl, until_ns, log);
}

void setUp(void)
{
    (void)memset(&s_log, 0, sizeof(s_log));
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------- the anchors */

static void test_the_reference_frames_have_the_lengths_used_here(void)
{
    const can_frame_t frames[] = {frame_000(), frame_009(), frame_078(), frame_104(), frame_2aa()};
    const uint16_t lengths[] = {LEN_000, LEN_009, LEN_078, LEN_104, LEN_2AA};

    for (size_t i = 0U; i < (sizeof(lengths) / sizeof(lengths[0])); i++) {
        can_bits_info_t info = {0U, 0U, 0U};

        TEST_ASSERT_TRUE(can_bits_exact(&frames[i], &info));
        TEST_ASSERT_EQUAL_UINT16(lengths[i], info.frame_bits);
    }

    TEST_ASSERT_EQUAL_UINT64(2000U, bt(1U));
    TEST_ASSERT_EQUAL_UINT64(bt(1U), can_bits_to_ns(1U, BITRATE));
}

static void test_a_fresh_model_is_error_active_with_no_fault_and_nothing_to_do(void)
{
    can_ctrl_t ctrl;
    can_ctrl_stats_t zero;

    (void)memset(&zero, 0, sizeof(zero));
    (void)memset(&ctrl, 0xA5, sizeof(ctrl)); /* init must not rely on a zeroed object */
    init_ctrl(&ctrl, BITRATE, 12345U);

    EXPECT_STATE(&ctrl, CAN_FC_ERROR_ACTIVE, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(CAN_BUS_FAULT_NONE, can_ctrl_fault(&ctrl));
    TEST_ASSERT_EQUAL_UINT32(0U, can_ctrl_tx_pending(&ctrl));
    TEST_ASSERT_EQUAL_MEMORY(&zero, can_ctrl_stats(&ctrl), sizeof(zero));
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
    EXPECT_NO_ACTION(&ctrl, 1000000000U);
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
}

/* The bus is free from the time of initialisation, not before: a frame handed in 10 bits after
 * it cannot have started 40 bits before it, and is placed [t, t + 50]. */
static void test_nothing_is_placed_on_the_bus_before_the_model_was_initialised(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t = 10000000U;

    init_ctrl(&ctrl, BITRATE, t);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, t + bt(10U)));

    const can_ctrl_action_t got =
        EXPECT_ACTION(&ctrl, t + bt(50U), CAN_CTRL_ACTION_RX_DELIVER, t + bt(50U));

    TEST_ASSERT_EQUAL_UINT64(t, got.start_ns);
}

/* ------------------------------------------------------------------- 1. the timeline */

static void test_a_frame_on_a_free_bus_completes_exactly_its_length_after_its_start(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_2aa();
    const uint64_t t0 = 10000U;

    init_ctrl(&ctrl, BITRATE, 1000U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 41U, t0));
    TEST_ASSERT_EQUAL_UINT32(1U, can_ctrl_tx_pending(&ctrl));

    EXPECT_SILENT_STEP(&ctrl, t0); /* the bus is free: it starts at once */
    EXPECT_WAKEUP(&ctrl, t0 + bt(LEN_2AA));
    EXPECT_NO_ACTION(&ctrl, t0 + bt(LEN_2AA) - 1U);
    TEST_ASSERT_EQUAL_UINT32(1U, can_ctrl_tx_pending(&ctrl)); /* the frame on the wire counts */

    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, t0 + bt(LEN_2AA), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(LEN_2AA));

    TEST_ASSERT_EQUAL_UINT64(t0, done.start_ns);
    TEST_ASSERT_EQUAL_UINT64(t0, done.submit_ns);
    TEST_ASSERT_EQUAL_UINT32(41U, done.handle);
    EXPECT_FRAME(&frame, &done.frame);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, done.state);
    TEST_ASSERT_EQUAL_UINT16(0U, done.tec);

    EXPECT_NO_ACTION(&ctrl, t0 + bt(LEN_2AA));
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
    TEST_ASSERT_EQUAL_UINT32(0U, can_ctrl_tx_pending(&ctrl));
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->tx_frames);
    TEST_ASSERT_EQUAL_UINT64(bt(LEN_2AA), can_ctrl_stats(&ctrl)->busy_ns);
}

static void test_two_frames_back_to_back_are_separated_by_exactly_the_intermission(void)
{
    can_ctrl_t ctrl;
    const can_frame_t first = frame_009();
    const can_frame_t second = frame_078();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &second, 2U, t0));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &first, 1U, t0));

    /* 0x009 wins: [t0, t0 + 49]. 0x078 starts after the 3-bit intermission: [52, 101]. */
    EXPECT_SILENT_STEP(&ctrl, t0);
    EXPECT_WAKEUP(&ctrl, t0 + bt(49U));

    can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, t0 + bt(49U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(49U));

    TEST_ASSERT_EQUAL_UINT32(1U, done.handle);
    TEST_ASSERT_EQUAL_UINT64(t0, done.start_ns);
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(52U));
    done = EXPECT_ACTION(&ctrl, t0 + bt(101U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(101U));
    TEST_ASSERT_EQUAL_UINT32(2U, done.handle);
    TEST_ASSERT_EQUAL_UINT64(t0 + bt(52U), done.start_ns);
    TEST_ASSERT_EQUAL_UINT64(t0, done.submit_ns);
    TEST_ASSERT_EQUAL_UINT64(bt(98U), can_ctrl_stats(&ctrl)->busy_ns); /* intermission excluded */
}

static void test_a_frame_submitted_during_a_transmission_waits_for_the_bus_and_cannot_overtake(void)
{
    can_ctrl_t ctrl;
    const can_frame_t big = frame_078();
    const can_frame_t small = frame_009();
    const can_frame_t later = frame_104();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &big, 1U, t0));
    EXPECT_NO_ACTION(&ctrl, t0);

    /* 0x009 has the smaller key but 0x078 is already on the wire, [t0, 49]: 0x009 follows at
     * 52 and ends at 101. */
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &small, 2U, t0 + bt(20U)));

    can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, t0 + bt(49U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(49U));

    TEST_ASSERT_EQUAL_UINT32(1U, done.handle);
    done = EXPECT_ACTION(&ctrl, t0 + bt(101U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(101U));
    TEST_ASSERT_EQUAL_UINT32(2U, done.handle);
    TEST_ASSERT_EQUAL_UINT64(t0 + bt(52U), done.start_ns);
    TEST_ASSERT_EQUAL_UINT64(t0 + bt(20U), done.submit_ns);

    /* Submitted in the intermission after 101: starts when it ends, at 104; ends at 151. */
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &later, 3U, t0 + bt(102U)));
    done = EXPECT_ACTION(&ctrl, t0 + bt(151U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(151U));
    TEST_ASSERT_EQUAL_UINT64(t0 + bt(104U), done.start_ns);

    /* Submitted on an idle bus: starts at its submission, 200, ends at 247. */
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &later, 4U, t0 + bt(200U)));
    done = EXPECT_ACTION(&ctrl, t0 + bt(247U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(247U));
    TEST_ASSERT_EQUAL_UINT64(t0 + bt(200U), done.start_ns);
}

static void test_the_smallest_arbitration_key_goes_first_whatever_the_submission_order(void)
{
    can_ctrl_t ctrl;
    const can_frame_t busy = frame_2aa();
    const can_frame_t f104 = frame_104();
    const can_frame_t f078 = frame_078();
    const can_frame_t f009 = frame_009();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &busy, 1U, t0));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f104, 2U, t0 + bt(10U)));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f078, 3U, t0 + bt(11U)));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f009, 4U, t0 + bt(12U)));
    run_until(&ctrl, t0 + bt(1000U), &s_log);

    /* 0x2AA [0, 108]; then 0x009 [111, 160], 0x078 [163, 212], 0x104 [215, 262]. */
    const uint32_t handles[] = {1U, 4U, 3U, 2U};
    const uint32_t starts[] = {0U, 111U, 163U, 215U};
    const uint32_t ends[] = {108U, 160U, 212U, 262U};

    TEST_ASSERT_EQUAL_UINT32(4U, s_log.count);

    for (uint32_t i = 0U; i < 4U; i++) {
        TEST_ASSERT_EQUAL_INT(CAN_CTRL_ACTION_TX_COMPLETE, s_log.action[i].kind);
        TEST_ASSERT_EQUAL_UINT32(handles[i], s_log.action[i].handle);
        TEST_ASSERT_EQUAL_UINT64(t0 + bt(starts[i]), s_log.action[i].start_ns);
        TEST_ASSERT_EQUAL_UINT64(t0 + bt(ends[i]), s_log.action[i].time_ns);
    }
}

static void test_frames_with_equal_keys_go_in_submission_order_even_from_a_lower_mailbox(void)
{
    can_ctrl_t ctrl;
    const can_frame_t busy = frame_2aa();
    const can_frame_t f009 = frame_009();
    const can_frame_t q = base_frame(0x300U, 1U, 0x11U); /* three frames with one key */
    const can_frame_t r = base_frame(0x300U, 1U, 0x22U);
    const can_frame_t s = base_frame(0x300U, 1U, 0x33U);
    const uint64_t t0 = 1000000U;

    TEST_ASSERT_EQUAL_HEX32(can_frame_arbitration_key(&q), can_frame_arbitration_key(&r));
    init_ctrl(&ctrl, BITRATE, 0U);

    /* Mailboxes: 0x2AA in 0 (on the wire), 0x009 in 1, q in 2. */
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &busy, 1U, t0));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f009, 2U, t0 + bt(10U)));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &q, 3U, t0 + bt(11U)));

    /* 0x2AA ends at 108 and frees mailbox 0; 0x009 starts at 111. r then goes into mailbox 0,
     * s into mailbox 3: q, in a higher mailbox than r, was submitted first and goes first. */
    EXPECT_ACTION(&ctrl, t0 + bt(108U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(108U));
    EXPECT_NO_ACTION(&ctrl, t0 + bt(120U));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &r, 4U, t0 + bt(120U)));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &s, 5U, t0 + bt(121U)));
    TEST_ASSERT_EQUAL_UINT32(4U, can_ctrl_tx_pending(&ctrl));
    TEST_ASSERT_EQUAL_UINT32(4U, ctrl.mailboxes[0].handle); /* the setup: r reused mailbox 0 */
    run_until(&ctrl, t0 + bt(2000U), &s_log);

    TEST_ASSERT_EQUAL_UINT32(4U, s_log.count);
    TEST_ASSERT_EQUAL_UINT32(2U, s_log.action[0].handle);
    TEST_ASSERT_EQUAL_UINT32(3U, s_log.action[1].handle);
    TEST_ASSERT_EQUAL_UINT32(4U, s_log.action[2].handle);
    TEST_ASSERT_EQUAL_UINT32(5U, s_log.action[3].handle);
    EXPECT_FRAME(&q, &s_log.action[1].frame);
    EXPECT_FRAME(&r, &s_log.action[2].frame);
    EXPECT_FRAME(&s, &s_log.action[3].frame);
}

/* At 83333 bit/s a bit lasts 12000.048 ns. Each duration is converted as a whole (rounded to
 * nearest), never as a sum of rounded bits:
 *   50 bits: 50e9 / 83333 = 600002.40 -> 600002 (50 rounded bits would make 600000),
 *   53 bits: 53e9 / 83333 = 636002.54 -> 636003 (frame plus intermission, the next start). */
static void test_durations_are_rounded_once_per_frame_not_once_per_bit(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();

    init_ctrl(&ctrl, 83333U, 0U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, 0U));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 2U, 0U));

    can_ctrl_action_t done = EXPECT_ACTION(&ctrl, 600002U, CAN_CTRL_ACTION_TX_COMPLETE, 600002U);

    TEST_ASSERT_EQUAL_UINT32(1U, done.handle);
    done = EXPECT_ACTION(&ctrl, 636003U + 600002U, CAN_CTRL_ACTION_TX_COMPLETE, 636003U + 600002U);
    TEST_ASSERT_EQUAL_UINT64(636003U, done.start_ns);
}

/* ------------------------------------------------------------------- 2. reception */

static void test_a_frame_arriving_on_a_free_bus_is_delivered_at_its_arrival(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_2aa();
    const uint64_t arrival = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, arrival));
    EXPECT_WAKEUP(&ctrl, arrival);

    /* It ended at its arrival, so it started one frame length earlier. */
    const can_ctrl_action_t got =
        EXPECT_ACTION(&ctrl, arrival, CAN_CTRL_ACTION_RX_DELIVER, arrival);

    TEST_ASSERT_EQUAL_UINT64(arrival - bt(LEN_2AA), got.start_ns);
    EXPECT_FRAME(&frame, &got.frame);
    TEST_ASSERT_EQUAL_UINT16(0U, got.rec);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_frames);
    TEST_ASSERT_EQUAL_UINT64(bt(LEN_2AA), can_ctrl_stats(&ctrl)->busy_ns);
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
}

static void test_frames_arriving_faster_than_the_bit_rate_are_delivered_at_bus_speed_in_order(void)
{
    can_ctrl_t ctrl;
    const can_frame_t big = frame_2aa();
    const can_frame_t small = frame_000();
    const uint64_t a = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &big, a));
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &small, a + 1000U));
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &big, a + 2000U));

    /* First: ends at a. Second: the bus is free from a + 3 bits, so [a + 3, a + 53]. Third:
     * [a + 56, a + 164]. The first is already due when the third arrives. */
    EXPECT_WAKEUP(&ctrl, a + 2000U);

    can_ctrl_action_t got = EXPECT_ACTION(&ctrl, a + 2000U, CAN_CTRL_ACTION_RX_DELIVER, a);

    TEST_ASSERT_EQUAL_UINT64(a - bt(LEN_2AA), got.start_ns);
    EXPECT_FRAME(&big, &got.frame);
    EXPECT_WAKEUP(&ctrl, a + bt(53U));
    EXPECT_NO_ACTION(&ctrl, a + bt(53U) - 1U);
    got = EXPECT_ACTION(&ctrl, a + bt(53U), CAN_CTRL_ACTION_RX_DELIVER, a + bt(53U));
    TEST_ASSERT_EQUAL_UINT64(a + bt(3U), got.start_ns);
    EXPECT_FRAME(&small, &got.frame);
    got = EXPECT_ACTION(&ctrl, a + bt(164U), CAN_CTRL_ACTION_RX_DELIVER, a + bt(164U));
    TEST_ASSERT_EQUAL_UINT64(a + bt(56U), got.start_ns);
    EXPECT_FRAME(&big, &got.frame);
    TEST_ASSERT_EQUAL_UINT64(bt(266U), can_ctrl_stats(&ctrl)->busy_ns);
}

static void test_a_frame_arriving_during_a_transmission_is_delivered_after_it(void)
{
    can_ctrl_t ctrl;
    const can_frame_t mine = frame_2aa();
    const can_frame_t theirs = frame_000();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &mine, 1U, t0));

    /* Mine is on the wire [0, 108]; theirs follows the intermission: [111, 161]. */
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &theirs, t0 + bt(60U)));
    EXPECT_ACTION(&ctrl, t0 + bt(108U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(108U));

    const can_ctrl_action_t got =
        EXPECT_ACTION(&ctrl, t0 + bt(161U), CAN_CTRL_ACTION_RX_DELIVER, t0 + bt(161U));

    TEST_ASSERT_EQUAL_UINT64(t0 + bt(111U), got.start_ns);
}

static void test_the_wire_queue_holds_64_frames_and_counts_those_beyond_as_overruns(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t a = 10000000U;

    init_ctrl(&ctrl, BITRATE, 0U);

    for (uint32_t i = 0U; i < 64U; i++) {
        TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    }

    TEST_ASSERT_FALSE(can_ctrl_wire_rx(&ctrl, &frame, a));
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->wire_overruns);

    /* Frame k ends at a + 53 k bit times (50 bits and the intermission each). Taking the first
     * makes room for one more, which goes to the end of the line, as the 65th. */
    EXPECT_ACTION(&ctrl, a, CAN_CTRL_ACTION_RX_DELIVER, a);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    TEST_ASSERT_FALSE(can_ctrl_wire_rx(&ctrl, &frame, a));
    TEST_ASSERT_EQUAL_UINT64(2U, can_ctrl_stats(&ctrl)->wire_overruns);

    for (uint32_t k = 1U; k <= 64U; k++) {
        const uint64_t eof = a + bt(53U * k);
        const can_ctrl_action_t got = EXPECT_ACTION(&ctrl, eof, CAN_CTRL_ACTION_RX_DELIVER, eof);

        TEST_ASSERT_EQUAL_UINT64(eof - bt(LEN_000), got.start_ns);
    }

    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
    TEST_ASSERT_EQUAL_UINT64(65U, can_ctrl_stats(&ctrl)->rx_frames);
}

/* ------------------------------------------------- 2b. arbitration against received frames */

/* s_log.action[i] is an action of `kind` for a frame on the bus [start, end]. */
#define EXPECT_LOGGED(i, kind, start, end) expect_logged((i), (kind), (start), (end), __LINE__)

static void expect_logged(uint32_t i, can_ctrl_action_kind_t kind, uint64_t start_ns,
                          uint64_t end_ns, int line)
{
    UNITY_TEST_ASSERT(i < s_log.count, line, "the action was logged");
    UNITY_TEST_ASSERT_EQUAL_INT(kind, s_log.action[i].kind, line, "action kind");
    UNITY_TEST_ASSERT_EQUAL_UINT64(start_ns, s_log.action[i].start_ns, line, "start of frame");
    UNITY_TEST_ASSERT_EQUAL_UINT64(end_ns, s_log.action[i].time_ns, line, "end of frame");
}

/* Two received frames 0x2AA handed in at a: the first passed on a free bus, [a - 108, a]; the
 * second could not, and waits for the bus, which is free at a + 3. Then `second`, a frame of
 * another node that waits behind it, if not NULL; then `mine` is submitted at a + 1000 ns,
 * during the intermission. */
static void wait_behind_a_received_frame(can_ctrl_t *ctrl, const can_frame_t *second,
                                         const can_frame_t *mine, uint64_t a)
{
    const can_frame_t theirs = frame_2aa();

    init_ctrl(ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(ctrl, &theirs, a));
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(ctrl, &theirs, a));

    if (second != NULL) {
        TEST_ASSERT_TRUE(can_ctrl_wire_rx(ctrl, second, a));
    }

    TEST_ASSERT_TRUE(can_ctrl_tx_submit(ctrl, mine, 1U, a + 1000U));
}

/* When the bus becomes free at a + 3, the waiting 0x2AA and this node's 0x009 arbitrate, as
 * on a real bus, and the smaller key wins: 0x009 [a + 3, a + 52], then 0x2AA [a + 55, a + 163].
 * (Until bug 005 was fixed, received frames went first whatever their key: 0x009 [114, 163].) */
static void test_a_frame_of_this_node_goes_before_a_waiting_received_frame_with_a_larger_key(void)
{
    can_ctrl_t ctrl;
    const can_frame_t mine = frame_009();
    const uint64_t a = 1000000U;

    wait_behind_a_received_frame(&ctrl, NULL, &mine, a);
    run_until(&ctrl, a + bt(1000U), &s_log);

    TEST_ASSERT_EQUAL_UINT32(3U, s_log.count);
    EXPECT_LOGGED(0U, CAN_CTRL_ACTION_RX_DELIVER, a - bt(LEN_2AA), a);
    EXPECT_LOGGED(1U, CAN_CTRL_ACTION_TX_COMPLETE, a + bt(3U), a + bt(52U));
    EXPECT_LOGGED(2U, CAN_CTRL_ACTION_RX_DELIVER, a + bt(55U), a + bt(163U));
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
}

/* The mirror case: the waiting received frame has the smaller key and goes first. 0x2AA
 * passed, [a - 108, a]; a received 0x000 waits; this node's 0x009 is submitted at a + 1000 ns.
 * 0x000 [a + 3, a + 53], then 0x009 [a + 56, a + 105]. */
static void test_a_waiting_received_frame_with_a_smaller_key_goes_before_this_node(void)
{
    can_ctrl_t ctrl;
    const can_frame_t theirs = frame_2aa();
    const can_frame_t first = frame_000();
    const can_frame_t mine = frame_009();
    const uint64_t a = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &theirs, a));
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &first, a));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &mine, 1U, a + 1000U));
    run_until(&ctrl, a + bt(1000U), &s_log);

    TEST_ASSERT_EQUAL_UINT32(3U, s_log.count);
    EXPECT_LOGGED(0U, CAN_CTRL_ACTION_RX_DELIVER, a - bt(LEN_2AA), a);
    EXPECT_LOGGED(1U, CAN_CTRL_ACTION_RX_DELIVER, a + bt(3U), a + bt(53U));
    EXPECT_FRAME(&first, &s_log.action[1].frame);
    EXPECT_LOGGED(2U, CAN_CTRL_ACTION_TX_COMPLETE, a + bt(56U), a + bt(105U));
}

/* Equal keys: the received frame goes first (ARBITRATION in can_ctrl.h). 0x2AA passed,
 * [a - 108, a]; a received 0x009 waits; this node's 0x009 is submitted at a + 1000 ns. The
 * received one [a + 3, a + 52], this node's [a + 55, a + 104]. */
static void test_with_equal_keys_the_waiting_received_frame_goes_first(void)
{
    can_ctrl_t ctrl;
    const can_frame_t theirs = frame_2aa();
    const can_frame_t same = frame_009();
    const uint64_t a = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &theirs, a));
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &same, a));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &same, 1U, a + 1000U));
    run_until(&ctrl, a + bt(1000U), &s_log);

    TEST_ASSERT_EQUAL_UINT32(3U, s_log.count);
    EXPECT_LOGGED(1U, CAN_CTRL_ACTION_RX_DELIVER, a + bt(3U), a + bt(52U));
    EXPECT_LOGGED(2U, CAN_CTRL_ACTION_TX_COMPLETE, a + bt(55U), a + bt(104U));
}

/*
 * Only the oldest waiting received frame competes, so received frames keep their arrival
 * order. Behind the waiting 0x2AA a received 0x000 waits; this node submits 0x078 and 0x104 at
 * a + 1000 ns. Both win against the 0x2AA at the head: 0x078 [a + 3, a + 52], 0x104
 * [a + 55, a + 102]. Then the received frames, in order: 0x2AA [a + 105, a + 213] and 0x000
 * [a + 216, a + 266], although 0x000 has the smallest key of all four.
 */
static void test_a_received_frame_with_a_small_key_waits_behind_an_older_received_frame(void)
{
    can_ctrl_t ctrl;
    const can_frame_t f000 = frame_000();
    const can_frame_t f078 = frame_078();
    const can_frame_t f104 = frame_104();
    const uint64_t a = 1000000U;

    wait_behind_a_received_frame(&ctrl, &f000, &f078, a);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f104, 2U, a + 1000U));
    run_until(&ctrl, a + bt(1000U), &s_log);

    TEST_ASSERT_EQUAL_UINT32(5U, s_log.count);
    EXPECT_LOGGED(1U, CAN_CTRL_ACTION_TX_COMPLETE, a + bt(3U), a + bt(52U));
    EXPECT_LOGGED(2U, CAN_CTRL_ACTION_TX_COMPLETE, a + bt(55U), a + bt(102U));
    TEST_ASSERT_EQUAL_UINT32(2U, s_log.action[2].handle);
    EXPECT_LOGGED(3U, CAN_CTRL_ACTION_RX_DELIVER, a + bt(105U), a + bt(213U));
    EXPECT_LOGGED(4U, CAN_CTRL_ACTION_RX_DELIVER, a + bt(216U), a + bt(266U));
    EXPECT_FRAME(&f000, &s_log.action[4].frame);
}

/* It is this node's best mailbox that competes, and the frames of both nodes interleave by
 * key. A received 0x078 waits behind the 0x2AA that passed; this node submits 0x104 (mailbox 0)
 * and 0x009 (mailbox 1) at a + 1000 ns. At a + 3, 0x009 wins against 0x078 [a + 3, a + 52];
 * at a + 55, 0x078 wins against 0x104 [a + 55, a + 104]; 0x104 goes last [a + 107, a + 154]. */
static void test_the_best_mailbox_competes_and_the_frames_of_both_nodes_interleave_by_key(void)
{
    can_ctrl_t ctrl;
    const can_frame_t theirs = frame_2aa();
    const can_frame_t f078 = frame_078();
    const can_frame_t f104 = frame_104();
    const can_frame_t f009 = frame_009();
    const uint64_t a = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &theirs, a));
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &f078, a));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f104, 1U, a + 1000U));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f009, 2U, a + 1000U));
    run_until(&ctrl, a + bt(1000U), &s_log);

    TEST_ASSERT_EQUAL_UINT32(4U, s_log.count);
    EXPECT_LOGGED(1U, CAN_CTRL_ACTION_TX_COMPLETE, a + bt(3U), a + bt(52U));
    TEST_ASSERT_EQUAL_UINT32(2U, s_log.action[1].handle);
    EXPECT_LOGGED(2U, CAN_CTRL_ACTION_RX_DELIVER, a + bt(55U), a + bt(104U));
    EXPECT_LOGGED(3U, CAN_CTRL_ACTION_TX_COMPLETE, a + bt(107U), a + bt(154U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_log.action[3].handle);
}

/* A frame handed in at the very instant the bus becomes free did not have the bus before that
 * instant: it waits and arbitrates with what is due then. This node's 0x009 and 0x078 are
 * submitted at t0: 0x009 [t0, t0 + 49], and the bus is free again at t0 + 52, where 0x078 is
 * due. A received 0x2AA handed in at t0 + 52 loses to it: 0x078 [t0 + 52, t0 + 101], then
 * 0x2AA [t0 + 104, t0 + 212]. */
static void test_a_frame_handed_in_at_the_instant_the_bus_becomes_free_arbitrates(void)
{
    can_ctrl_t ctrl;
    const can_frame_t theirs = frame_2aa();
    const can_frame_t f009 = frame_009();
    const can_frame_t f078 = frame_078();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f009, 1U, t0));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f078, 2U, t0));
    EXPECT_ACTION(&ctrl, t0 + bt(49U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(49U));
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &theirs, t0 + bt(52U)));
    run_until(&ctrl, t0 + bt(1000U), &s_log);

    TEST_ASSERT_EQUAL_UINT32(2U, s_log.count);
    EXPECT_LOGGED(0U, CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(52U), t0 + bt(101U));
    EXPECT_LOGGED(1U, CAN_CTRL_ACTION_RX_DELIVER, t0 + bt(104U), t0 + bt(212U));
}

/* The late-caller rule moves the start of this node's frame that won, and the received frame
 * that lost waits for it. As in the first test of this section, 0x009 wins the free bus at
 * a + 3; but the first poll comes at L = a + 3 + 49 bit times + 1 ns, more than a frame length
 * later. 0x009 starts at L, [L, L + 49]; the 0x2AA follows it, [L + 52, L + 160]. */
static void test_a_late_poll_moves_a_start_won_against_a_received_frame_and_that_frame_waits(void)
{
    can_ctrl_t ctrl;
    const can_frame_t mine = frame_009();
    const uint64_t a = 1000000U;
    const uint64_t late = a + bt(3U + LEN_009) + 1U;

    wait_behind_a_received_frame(&ctrl, NULL, &mine, a);
    collect(&ctrl, late, &s_log);
    TEST_ASSERT_EQUAL_UINT32(1U, s_log.count);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->late_resyncs);
    run_until(&ctrl, late + bt(1000U), &s_log);

    TEST_ASSERT_EQUAL_UINT32(3U, s_log.count);
    EXPECT_LOGGED(1U, CAN_CTRL_ACTION_TX_COMPLETE, late, late + bt(49U));
    EXPECT_LOGGED(2U, CAN_CTRL_ACTION_RX_DELIVER, late + bt(52U), late + bt(160U));
}

/*
 * A flood from other nodes that the bus cannot carry. 64 received frames with identifiers from
 * 0x700 (8 data bytes) are handed in at once, and 5 more every 600 bit times; this node submits
 * a frame 0x030 every 600 bit times, 20 in all. A frame takes about 130 bit times, so the wire
 * queue stays full and frames beyond it are overruns. Each frame of this node starts at the
 * first start of frame after its submission: it waits at most for the received frame on the
 * bus at that moment and its intermission, never for the backlog. The received frames still
 * come out in the order they were handed in, and every one is either delivered or an overrun.
 */
#define FLOOD_PERIOD_BITS 600U
#define FLOOD_OWN_FRAMES  20U
#define FLOOD_REFILL      5U
#define FLOOD_FRAMES_MAX  (CAN_CTRL_WIRE_QUEUE + (FLOOD_OWN_FRAMES * FLOOD_REFILL))

typedef struct {
    uint32_t id[FLOOD_FRAMES_MAX]; /* identifiers of the accepted frames, in order */
    uint32_t accepted;
    uint32_t handed;
    uint32_t longest_bits; /* the longest frame handed in */
} flood_t;

static void flood_hand_in(can_ctrl_t *ctrl, flood_t *flood, uint64_t now_ns)
{
    const can_frame_t frame = base_frame(0x700U + flood->handed, 8U, 0x55U);
    can_bits_info_t info = {0U, 0U, 0U};

    TEST_ASSERT_TRUE(can_bits_exact(&frame, &info));

    if (info.frame_bits > flood->longest_bits) {
        flood->longest_bits = info.frame_bits;
    }

    if (can_ctrl_wire_rx(ctrl, &frame, now_ns)) {
        flood->id[flood->accepted] = frame.id;
        flood->accepted++;
    }

    flood->handed++;
}

static void test_a_flood_with_larger_keys_delays_this_node_by_one_frame_at_most(void)
{
    can_ctrl_t ctrl;
    flood_t flood;
    const can_frame_t mine = base_frame(0x030U, 8U, 0x5AU);
    const uint64_t a = 1000000U;
    uint32_t sent = 0U;
    uint32_t received = 0U;
    uint32_t waited = 0U;

    (void)memset(&flood, 0, sizeof(flood));
    init_ctrl(&ctrl, BITRATE, 0U);

    for (uint32_t i = 0U; i < CAN_CTRL_WIRE_QUEUE; i++) {
        flood_hand_in(&ctrl, &flood, a);
    }

    for (uint32_t p = 0U; p < FLOOD_OWN_FRAMES; p++) {
        const uint64_t t = a + bt(37U + (FLOOD_PERIOD_BITS * p));

        run_until(&ctrl, t, &s_log);
        /* The backlog is there: received frames are waiting for the bus (private fields, read
         * only to check that the scenario is what it claims to be). */
        TEST_ASSERT_TRUE(ctrl.wire_count > ctrl.wire_placed);
        TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &mine, p, t));

        for (uint32_t i = 0U; i < FLOOD_REFILL; i++) {
            flood_hand_in(&ctrl, &flood, t);
        }
    }

    run_until(&ctrl, a + 100000000U, &s_log);

    for (uint32_t i = 0U; i < s_log.count; i++) {
        const can_ctrl_action_t *const action = &s_log.action[i];

        if (action->kind == CAN_CTRL_ACTION_TX_COMPLETE) {
            TEST_ASSERT_EQUAL_UINT32(sent, action->handle);
            TEST_ASSERT_TRUE(action->start_ns >= action->submit_ns);
            TEST_ASSERT_TRUE((action->start_ns - action->submit_ns) <=
                             bt(flood.longest_bits + CAN_BITS_INTERMISSION));
            waited += (action->start_ns > action->submit_ns) ? 1U : 0U;
            sent++;
        } else {
            TEST_ASSERT_EQUAL_INT(CAN_CTRL_ACTION_RX_DELIVER, action->kind);
            TEST_ASSERT_TRUE(received < flood.accepted);
            TEST_ASSERT_EQUAL_HEX32(flood.id[received], action->frame.id);
            received++;
        }
    }

    TEST_ASSERT_EQUAL_UINT32(FLOOD_OWN_FRAMES, sent);
    TEST_ASSERT_TRUE(waited > 0U);
    TEST_ASSERT_EQUAL_UINT32(flood.accepted, received);
    TEST_ASSERT_EQUAL_UINT64(flood.accepted, can_ctrl_stats(&ctrl)->rx_frames);
    TEST_ASSERT_EQUAL_UINT64(flood.handed - flood.accepted, can_ctrl_stats(&ctrl)->wire_overruns);
    TEST_ASSERT_TRUE(can_ctrl_stats(&ctrl)->wire_overruns > 0U);
}

/* ------------------------------------------------------------------- 3. late callers */

static void test_a_poll_one_frame_length_after_the_possible_start_is_not_late(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_2aa();

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, 0U));

    /* It could start at 0; 216000 ns later is exactly one frame length, not more. */
    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, bt(LEN_2AA), CAN_CTRL_ACTION_TX_COMPLETE, bt(LEN_2AA));

    TEST_ASSERT_EQUAL_UINT64(0U, done.start_ns);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->late_resyncs);
}

static void test_a_later_poll_moves_the_start_to_the_poll_and_counts_a_resync(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_2aa();
    const uint64_t poll_at = bt(LEN_2AA) + 1U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, 0U));

    /* One nanosecond more than a frame length: the start moves to the poll, and the frame is
     * reported one frame length after that, not at once. */
    EXPECT_NO_ACTION(&ctrl, poll_at);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->late_resyncs);
    EXPECT_WAKEUP(&ctrl, poll_at + bt(LEN_2AA));

    const can_ctrl_action_t done = EXPECT_ACTION(
        &ctrl, poll_at + bt(LEN_2AA), CAN_CTRL_ACTION_TX_COMPLETE, poll_at + bt(LEN_2AA));

    TEST_ASSERT_EQUAL_UINT64(poll_at, done.start_ns);
    TEST_ASSERT_EQUAL_UINT64(0U, done.submit_ns);
}

static void test_a_late_poll_after_a_failed_attempt_moves_the_retransmission(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_009();
    const uint64_t late = bt(38U) + 1000000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_inject_tx_errors(&ctrl, 1U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, 0U));

    /* The destroyed attempt: [0, 49 / 2 + 14 = 38]. The retry could start at 41; a poll one
     * second later starts it then. */
    EXPECT_SILENT_STEP(&ctrl, 0U);
    EXPECT_SILENT_STEP(&ctrl, bt(38U));
    EXPECT_WAKEUP(&ctrl, bt(41U));
    EXPECT_NO_ACTION(&ctrl, late);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->late_resyncs);

    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, late + bt(49U), CAN_CTRL_ACTION_TX_COMPLETE, late + bt(49U));

    TEST_ASSERT_EQUAL_UINT64(late, done.start_ns);
}

/* ------------------------------------------------------------------- 4. next wake-up */

static void test_next_wakeup_is_the_last_poll_time_while_actions_wait_to_be_taken(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    can_ctrl_action_t action;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, 5000000U));

    /* Advancing without taking: the action stays queued, so the model is due at once. */
    TEST_ASSERT_TRUE(can_ctrl_poll(&ctrl, 7000000U, NULL));
    EXPECT_WAKEUP(&ctrl, 7000000U);
    TEST_ASSERT_TRUE(can_ctrl_poll(&ctrl, 7000000U, NULL));
    TEST_ASSERT_TRUE(can_ctrl_poll(&ctrl, 7000000U, &action));
    TEST_ASSERT_EQUAL_UINT64(5000000U, action.time_ns);
    TEST_ASSERT_FALSE(can_ctrl_poll(&ctrl, 7000000U, NULL));
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
}

/* A frame read from the transport may carry an arrival time before the last poll (the glue
 * stamps it when it reads it). 0x000 arriving at 5 ms, handed in after a poll at 10 ms: it ended
 * at 5 ms, but the wake-up is the last poll time, never earlier, as can_ctrl.h promises. */
static void test_next_wakeup_is_never_earlier_than_the_last_poll_for_a_frame_dated_before_it(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t arrival = 5000000U;
    const uint64_t poll_at = 10000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    EXPECT_NO_ACTION(&ctrl, poll_at);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, arrival));
    EXPECT_WAKEUP(&ctrl, poll_at);

    const can_ctrl_action_t got =
        EXPECT_ACTION(&ctrl, poll_at, CAN_CTRL_ACTION_RX_DELIVER, arrival);

    TEST_ASSERT_EQUAL_UINT64(arrival - bt(LEN_000), got.start_ns);
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
}

/* ------------------------------------------------------------------- 5. faults */

/*
 * CAN_BUS_FAULT_SHORT from a clean node, frame 0x000 (50 bits) submitted at t0 (by the caller).
 * Every attempt is a bit error on the start-of-frame bit: 1 + 14 = 15 bits on the bus.
 *
 *   Error active: rule 3 (+8) and, because the dominant bits of the active error flag read back
 *   recessive, rule 4 (+8): 16 per attempt. Attempt k (k = 0..7) starts at 18 k (15 + 3) and
 *   leaves TEC 16 (k + 1); attempt 7 ends at 126 + 15 = 141 with TEC 128: error passive.
 *   Error passive: rule 3 only (+8), and the suspend: 15 + 3 + 8 = 26 bits per attempt.
 *   Attempt k (k = 8..23) starts at 141 + 11 + 26 (k - 8) = 152 + 26 (k - 8) and leaves
 *   TEC 128 + 8 (k - 7); attempt 23 starts at 542, ends at 557 with TEC 256: bus off.
 *
 * Checks every start and end time as the model's next wake-up, the counter after each attempt
 * and the two state changes. `started`: the first attempt was already started by the poll that
 * took the caller's last action. Returns the time of bus off, t0 + 557 bit times.
 */
static uint64_t expect_short_to_bus_off(can_ctrl_t *ctrl, uint64_t t0, bool started)
{
    for (uint32_t k = 0U; k < 24U; k++) {
        const uint32_t start = (k <= 7U) ? (18U * k) : (152U + (26U * (k - 8U)));
        const uint32_t tec = (k <= 7U) ? (16U * (k + 1U)) : (128U + (8U * (k - 7U)));
        const uint64_t end_ns = t0 + bt(start + 15U);

        if ((k > 0U) || !started) {
            EXPECT_SILENT_STEP(ctrl, t0 + bt(start));
        }

        EXPECT_WAKEUP(ctrl, end_ns);

        if (k == 7U) {
            const can_ctrl_action_t change =
                EXPECT_ACTION(ctrl, end_ns, CAN_CTRL_ACTION_STATE_CHANGED, end_ns);

            EXPECT_CHANGE(&change, CAN_FC_ERROR_ACTIVE, CAN_FC_ERROR_PASSIVE);
            TEST_ASSERT_EQUAL_UINT16(128U, change.tec);
        } else if (k == 23U) {
            const can_ctrl_action_t change =
                EXPECT_ACTION(ctrl, end_ns, CAN_CTRL_ACTION_STATE_CHANGED, end_ns);

            EXPECT_CHANGE(&change, CAN_FC_ERROR_PASSIVE, CAN_FC_BUS_OFF);
            TEST_ASSERT_EQUAL_UINT16(256U, change.tec);
        } else {
            /* no action for this attempt */
        }

        EXPECT_NO_ACTION(ctrl, end_ns);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)tec, can_ctrl_tec(ctrl));
    }

    return t0 + bt(557U);
}

static void
test_a_short_takes_a_clean_node_error_passive_after_8_attempts_and_bus_off_after_24(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, t0);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 7U, t0));

    TEST_ASSERT_EQUAL_UINT64(t0 + bt(557U), expect_short_to_bus_off(&ctrl, t0, false));

    EXPECT_STATE(&ctrl, CAN_FC_BUS_OFF, 256U, 0U);
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
    TEST_ASSERT_EQUAL_UINT32(1U, can_ctrl_tx_pending(&ctrl)); /* still in its mailbox */
    TEST_ASSERT_EQUAL_UINT64(24U, can_ctrl_stats(&ctrl)->tx_attempts_failed);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->tx_frames);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->bus_off_events);
    TEST_ASSERT_EQUAL_UINT64(bt(24U * 15U), can_ctrl_stats(&ctrl)->busy_ns);
}

/*
 * CAN_BUS_FAULT_NO_ACK, frame 0x000 (50 bits): an attempt lasts (50 - 8) + 14 = 56 bits.
 *   Error active: rule 3 (+8). Attempt k (k = 0..15) starts at 59 k; attempt 15 ends at
 *   885 + 56 = 941 with TEC 128: error passive.
 *   Error passive: exception 1, TEC unchanged; with the suspend 56 + 3 + 8 = 67 bits per
 *   attempt: attempt k (k >= 16) starts at 941 + 11 + 67 (k - 16) = 952 + 67 (k - 16).
 *   200 attempts later (k = 215) the last one ends at 952 + 67 * 199 + 56 = 14341. The fault is
 *   removed then; it takes effect after the intermission, at 14344, and the next attempt starts
 *   after the suspend, at 14341 + 11 = 14352. It succeeds (rule 7): TEC 127, error active,
 *   at 14352 + 50 = 14402.
 */
static void test_a_node_never_acknowledged_stays_at_128_and_recovers_with_the_next_good_frame(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NO_ACK, t0);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 9U, t0));

    for (uint32_t k = 0U; k < 216U; k++) {
        const uint32_t start = (k < 16U) ? (59U * k) : (952U + (67U * (k - 16U)));
        const uint64_t end_ns = t0 + bt(start + 56U);

        EXPECT_SILENT_STEP(&ctrl, t0 + bt(start));
        EXPECT_WAKEUP(&ctrl, end_ns);

        if (k == 15U) {
            const can_ctrl_action_t change =
                EXPECT_ACTION(&ctrl, end_ns, CAN_CTRL_ACTION_STATE_CHANGED, end_ns);

            EXPECT_CHANGE(&change, CAN_FC_ERROR_ACTIVE, CAN_FC_ERROR_PASSIVE);
        }

        EXPECT_NO_ACTION(&ctrl, end_ns);
        TEST_ASSERT_EQUAL_UINT16((k < 16U) ? (uint16_t)(8U * (k + 1U)) : 128U, can_ctrl_tec(&ctrl));
        TEST_ASSERT_NOT_EQUAL_INT(CAN_FC_BUS_OFF, can_ctrl_state(&ctrl));
    }

    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NONE, t0 + bt(14341U));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(14344U)); /* the fault ends */
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(14352U)); /* the start */

    const can_ctrl_action_t change =
        EXPECT_ACTION(&ctrl, t0 + bt(14402U), CAN_CTRL_ACTION_STATE_CHANGED, t0 + bt(14402U));

    EXPECT_CHANGE(&change, CAN_FC_ERROR_PASSIVE, CAN_FC_ERROR_ACTIVE);
    TEST_ASSERT_EQUAL_UINT16(127U, change.tec);

    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, t0 + bt(14402U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(14402U));

    TEST_ASSERT_EQUAL_UINT64(t0 + bt(14352U), done.start_ns);
    TEST_ASSERT_EQUAL_UINT32(9U, done.handle);
    TEST_ASSERT_EQUAL_UINT16(127U, done.tec);
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_ACTIVE, 127U, 0U);
    TEST_ASSERT_EQUAL_UINT64(216U, can_ctrl_stats(&ctrl)->tx_attempts_failed);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->bus_off_events);
}

/*
 * CAN_BUS_FAULT_STUCK_DOMINANT set at B on an idle bus, the node error active. Steps, in bit
 * times after B:
 *   6: stuff error, rule 1, REC 1. The active error flag takes bits 7 to 12.
 *   13: the first bit after the flag is dominant, rule 2, REC 9.
 *   Rule 6 counts the dominant bits after the flag, bit 13 being the first: the 14th is bit 26.
 *   So rule 6 at 26, 34, ..., 26 + 8 (m - 1) for the m-th run (m >= 1), REC 9 + 8 m.
 *   Error passive at the first REC >= 128: m = 15 (129), at 26 + 8 * 14 = 138. Saturation:
 *   m = 31 gives 257, held at 255, at 26 + 8 * 30 = 266; then nothing more. 33 steps in all.
 * After the fault is removed at R the bus is free at R + 11 and the frame [R + 11, R + 61] goes.
 */
static void test_a_bus_held_dominant_raises_the_receive_counter_on_schedule_and_blocks_sending(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t b = 1000000U;
    const uint64_t r = b + 2000000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_STUCK_DOMINANT, b);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 3U, b + 1000U));

    for (uint32_t n = 0U; n < 33U; n++) {
        uint32_t offset = 6U;
        uint16_t rec = 1U;

        if (n >= 1U) {
            offset = (n == 1U) ? 13U : (26U + (8U * (n - 2U)));
            rec = (uint16_t)(9U + (8U * (n - 1U)));
        }

        if (rec > 255U) {
            rec = 255U;
        }

        EXPECT_WAKEUP(&ctrl, b + bt(offset));

        if (n == 16U) {
            const can_ctrl_action_t change =
                EXPECT_ACTION(&ctrl, b + bt(offset), CAN_CTRL_ACTION_STATE_CHANGED, b + bt(138U));

            EXPECT_CHANGE(&change, CAN_FC_ERROR_ACTIVE, CAN_FC_ERROR_PASSIVE);
            TEST_ASSERT_EQUAL_UINT16(129U, change.rec);
        }

        EXPECT_NO_ACTION(&ctrl, b + bt(offset));
        TEST_ASSERT_EQUAL_UINT16(rec, can_ctrl_rec(&ctrl));
    }

    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
    EXPECT_NO_ACTION(&ctrl, b + 1000000000U);
    TEST_ASSERT_FALSE(can_ctrl_wire_rx(&ctrl, &frame, b + 1000000000U));
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_lost_fault);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->tx_frames);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->tx_attempts_failed);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->busy_ns);
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_PASSIVE, 0U, 255U);

    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NONE, r);
    EXPECT_SILENT_STEP(&ctrl, r);
    EXPECT_SILENT_STEP(&ctrl, r + bt(11U)); /* no suspend: the dominant bus came last */

    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, r + bt(61U), CAN_CTRL_ACTION_TX_COMPLETE, r + bt(61U));

    TEST_ASSERT_EQUAL_UINT64(r + bt(11U), done.start_ns);
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_PASSIVE, 0U, 255U); /* no frame received: REC stays */
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->bus_off_events);
}

/*
 * Rule 9 ends: the error that makes a node error passive is still signalled with an ACTIVE error
 * flag, so on a shorted bus that error also brings the bit error in the flag (rule 4). To start
 * an attempt at TEC 120, one injected error first: 0x000, [0, 39] TEC 8. The short is set at 39
 * and takes effect after the intermission, 42. Attempts k = 1..7 at 42 + 18 (k - 1) leave
 * TEC 8 + 16 k; the 7th ends at 165 with 120. Attempt 8, [168, 183]: rule 3 gives 128 and the
 * node error passive, rule 4 then 136, decided by the state in which the error was signalled.
 */
static void test_the_error_that_makes_a_node_error_passive_is_signalled_with_an_active_flag(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_inject_tx_errors(&ctrl, 1U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, t0));
    EXPECT_SILENT_STEP(&ctrl, t0);
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(39U));
    TEST_ASSERT_EQUAL_UINT16(8U, can_ctrl_tec(&ctrl));
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, t0 + bt(39U));
    run_until(&ctrl, t0 + bt(182U), &s_log);
    TEST_ASSERT_EQUAL_UINT32(0U, s_log.count);
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_ACTIVE, 120U, 0U);
    EXPECT_WAKEUP(&ctrl, t0 + bt(183U));

    const can_ctrl_action_t change =
        EXPECT_ACTION(&ctrl, t0 + bt(183U), CAN_CTRL_ACTION_STATE_CHANGED, t0 + bt(183U));

    EXPECT_CHANGE(&change, CAN_FC_ERROR_ACTIVE, CAN_FC_ERROR_PASSIVE);
    TEST_ASSERT_EQUAL_UINT16(136U, change.tec);
}

/*
 * Two injected errors on 0x009 (49 bits): an attempt destroyed halfway lasts 49 / 2 + 14 = 38
 * bits (integer half, 24). Attempts [0, 38] TEC 8, [41, 79] TEC 16 (rule 3), then the frame
 * [82, 131] with TEC 15 (rule 7).
 */
static void test_injected_transmit_errors_destroy_one_attempt_each_and_the_frame_is_resent(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_009();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_inject_tx_errors(&ctrl, 1U);
    can_ctrl_inject_tx_errors(&ctrl, 1U); /* adds to what is left */
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 5U, t0));

    EXPECT_SILENT_STEP(&ctrl, t0);
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(38U));
    TEST_ASSERT_EQUAL_UINT16(8U, can_ctrl_tec(&ctrl));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(41U));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(79U));
    TEST_ASSERT_EQUAL_UINT16(16U, can_ctrl_tec(&ctrl));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(82U));

    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, t0 + bt(131U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(131U));

    TEST_ASSERT_EQUAL_UINT64(t0 + bt(82U), done.start_ns);
    TEST_ASSERT_EQUAL_UINT16(15U, done.tec);
    TEST_ASSERT_EQUAL_UINT64(2U, can_ctrl_stats(&ctrl)->tx_attempts_failed);
    TEST_ASSERT_EQUAL_UINT64(bt(38U + 38U + 49U), can_ctrl_stats(&ctrl)->busy_ns);
}

/*
 * A short destroys the start-of-frame bit, before the middle of the frame where an injected
 * error would strike, so the injection is still there for the next attempt. 0x009:
 * [0, 15] short, TEC 16; the short is removed at 15 and ends after the intermission, at 18;
 * [18, 56] the injected error, TEC 24; [59, 108] the frame, TEC 23.
 */
static void test_an_injected_error_waits_while_a_short_destroys_the_attempt_first(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_009();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, t0);
    can_ctrl_inject_tx_errors(&ctrl, 1U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, t0));

    EXPECT_SILENT_STEP(&ctrl, t0);
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(15U));
    TEST_ASSERT_EQUAL_UINT16(16U, can_ctrl_tec(&ctrl));
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NONE, t0 + bt(15U));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(18U));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(56U));
    TEST_ASSERT_EQUAL_UINT16(24U, can_ctrl_tec(&ctrl));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(59U));

    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, t0 + bt(108U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(108U));

    TEST_ASSERT_EQUAL_UINT64(t0 + bt(59U), done.start_ns);
    TEST_ASSERT_EQUAL_UINT16(23U, done.tec);
}

/*
 * A missing acknowledgment is detected in the ACK slot, after the middle of the frame, so an
 * injected error strikes first. 0x009 without acknowledgment: [0, 38] injected (rule 3, TEC 8),
 * [41, 41 + 49 - 8 + 14 = 96] no acknowledgment (TEC 16, the node is error active).
 */
static void test_an_injected_error_strikes_before_a_missing_acknowledgment(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_009();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NO_ACK, t0);
    can_ctrl_inject_tx_errors(&ctrl, 1U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, t0));

    EXPECT_SILENT_STEP(&ctrl, t0);
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(38U));
    TEST_ASSERT_EQUAL_UINT16(8U, can_ctrl_tec(&ctrl));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(41U));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(96U));
    TEST_ASSERT_EQUAL_UINT16(16U, can_ctrl_tec(&ctrl));
    TEST_ASSERT_EQUAL_UINT64(2U, can_ctrl_stats(&ctrl)->tx_attempts_failed);
}

/*
 * Two injected receive errors and three 0x000 frames arriving at a: [.., a], [a + 3, a + 53],
 * [a + 56, a + 106]. The first two are destroyed at their end (rule 1: REC 1, 2), the third is
 * delivered (rule 8: REC 1).
 */
static void test_injected_receive_errors_destroy_one_frame_each(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t a = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_inject_rx_errors(&ctrl, 2U);

    for (uint32_t i = 0U; i < 3U; i++) {
        TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    }

    EXPECT_NO_ACTION(&ctrl, a);
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_ACTIVE, 0U, 1U);
    EXPECT_NO_ACTION(&ctrl, a + bt(53U));
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_ACTIVE, 0U, 2U);

    const can_ctrl_action_t got =
        EXPECT_ACTION(&ctrl, a + bt(106U), CAN_CTRL_ACTION_RX_DELIVER, a + bt(106U));

    TEST_ASSERT_EQUAL_UINT64(a + bt(56U), got.start_ns);
    TEST_ASSERT_EQUAL_UINT16(1U, got.rec);
    TEST_ASSERT_EQUAL_UINT64(2U, can_ctrl_stats(&ctrl)->rx_errors);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_frames);
    TEST_ASSERT_EQUAL_UINT64(bt(150U), can_ctrl_stats(&ctrl)->busy_ns);
}

/*
 * A short set during a good frame [0, 50] takes effect after its intermission (53): that frame
 * is transmitted, the next one [53, 68] is not (TEC 0 - 1 stays 0, then 16). The short removed
 * during that attempt ends at 71: [71, 121] goes through, TEC 15.
 */
static void test_a_fault_set_or_removed_during_a_frame_takes_effect_from_the_next_frame(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, t0));
    EXPECT_NO_ACTION(&ctrl, t0);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, t0 + bt(10U));
    TEST_ASSERT_EQUAL_INT(CAN_BUS_FAULT_SHORT, can_ctrl_fault(&ctrl)); /* reported at once */
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 2U, t0 + bt(20U)));

    can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, t0 + bt(50U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(50U));

    TEST_ASSERT_EQUAL_UINT32(1U, done.handle);
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(53U));
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NONE, t0 + bt(60U));
    TEST_ASSERT_EQUAL_INT(CAN_BUS_FAULT_NONE, can_ctrl_fault(&ctrl));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(68U));
    TEST_ASSERT_EQUAL_UINT16(16U, can_ctrl_tec(&ctrl));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(71U));
    done = EXPECT_ACTION(&ctrl, t0 + bt(121U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(121U));
    TEST_ASSERT_EQUAL_UINT32(2U, done.handle);
    TEST_ASSERT_EQUAL_UINT64(t0 + bt(71U), done.start_ns);
    TEST_ASSERT_EQUAL_UINT16(15U, done.tec);
}

/*
 * Three 0x000 frames arrive at a: [.., a], [a + 3, a + 53], [a + 56, a + 106]. At a + 10, while
 * the second is on the wire, the bus is held dominant. The fault takes effect after the second
 * one's intermission, at a + 56: the second is delivered, the third never reaches the bus, and
 * the first counter step comes 6 bits later, at a + 62.
 */
static void test_a_dominant_fault_set_during_a_reception_starts_after_it_and_drops_the_rest(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t a = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);

    for (uint32_t i = 0U; i < 3U; i++) {
        TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    }

    EXPECT_ACTION(&ctrl, a, CAN_CTRL_ACTION_RX_DELIVER, a);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_STUCK_DOMINANT, a + bt(10U));
    EXPECT_ACTION(&ctrl, a + bt(53U), CAN_CTRL_ACTION_RX_DELIVER, a + bt(53U));
    EXPECT_SILENT_STEP(&ctrl, a + bt(56U));
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_lost_fault);
    EXPECT_SILENT_STEP(&ctrl, a + bt(62U));
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_ACTIVE, 0U, 1U);
    TEST_ASSERT_EQUAL_UINT64(2U, can_ctrl_stats(&ctrl)->rx_frames);
}

/* Two 0x2AA frames: [.., a] and [a + 3, a + 111]. A dominant fault set at a + 50, during the
 * second, the last frame queued, takes effect at a + 114: first step at a + 120. */
static void test_a_dominant_fault_set_during_the_last_queued_frame_waits_for_its_intermission(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_2aa();
    const uint64_t a = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    EXPECT_ACTION(&ctrl, a, CAN_CTRL_ACTION_RX_DELIVER, a);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_STUCK_DOMINANT, a + bt(50U));
    EXPECT_ACTION(&ctrl, a + bt(111U), CAN_CTRL_ACTION_RX_DELIVER, a + bt(111U));
    EXPECT_SILENT_STEP(&ctrl, a + bt(114U));
    EXPECT_SILENT_STEP(&ctrl, a + bt(120U));
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_ACTIVE, 0U, 1U);
}

/*
 * Whether a frame handed in is refused goes by the fault last set, not by the one in force on
 * the timeline. 0x000 (1) is on the wire [0, 50] when a short is set at 10; it takes effect at 53,
 * but a frame handed in at 20 is refused already (that it would arrive before 53 is an accident
 * of the arrival time). 0x000 (2) then fails under the short, [53, 68]; the short is removed at
 * 60 and ends at 71. A frame handed in at 62 is accepted: it goes on the bus at 71, [71, 121],
 * and the retransmission follows, [124, 174].
 */
static void test_a_frame_handed_in_is_judged_by_the_fault_last_set_before_it_takes_effect(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, t0));
    EXPECT_NO_ACTION(&ctrl, t0);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, t0 + bt(10U));
    TEST_ASSERT_FALSE(can_ctrl_wire_rx(&ctrl, &frame, t0 + bt(20U)));
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_lost_fault);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 2U, t0 + bt(20U)));
    EXPECT_ACTION(&ctrl, t0 + bt(50U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(50U));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(53U));
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NONE, t0 + bt(60U));
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, t0 + bt(62U)));

    const can_ctrl_action_t got =
        EXPECT_ACTION(&ctrl, t0 + bt(121U), CAN_CTRL_ACTION_RX_DELIVER, t0 + bt(121U));

    TEST_ASSERT_EQUAL_UINT64(t0 + bt(71U), got.start_ns);

    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, t0 + bt(174U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(174U));

    TEST_ASSERT_EQUAL_UINT32(2U, done.handle);
    TEST_ASSERT_EQUAL_UINT64(t0 + bt(124U), done.start_ns);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_lost_fault);
}

/* A queued received frame that starts exactly when a short is set has not started: the short is
 * in force at its start of frame. Frames [.., a] and [a + 3, a + 53]; the short is set at a + 3. */
static void test_a_received_frame_due_to_start_at_the_instant_of_a_short_never_passes(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t a = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    EXPECT_ACTION(&ctrl, a, CAN_CTRL_ACTION_RX_DELIVER, a);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, a + bt(3U));
    EXPECT_SILENT_STEP(&ctrl, a + bt(3U));
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_lost_fault);
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_frames);
}

/* The same with a poll at a + 3 before the short is set at that instant: the poll has put the
 * second frame on the timeline, due to start at a + 3, and it has still not started when the
 * short is set (free_after_started). Since received frames wait for the bus instead of being
 * placed in advance (bug 005), this is the case of bug 004. */
static void test_a_received_frame_placed_at_the_instant_of_a_short_never_passes(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t a = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    EXPECT_ACTION(&ctrl, a + bt(3U), CAN_CTRL_ACTION_RX_DELIVER, a);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, a + bt(3U));
    EXPECT_SILENT_STEP(&ctrl, a + bt(3U));
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_lost_fault);
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_frames);
}

/* A fault set in the intermission after a received frame takes effect at its end. One 0x000
 * ends at a; held dominant from a + 1, the fault takes effect at a + 3 and the first counter
 * step comes 6 bits later, at a + 9. */
static void test_a_fault_set_in_the_intermission_after_a_received_frame_waits_for_its_end(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t a = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    EXPECT_ACTION(&ctrl, a, CAN_CTRL_ACTION_RX_DELIVER, a);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_STUCK_DOMINANT, a + bt(1U));
    EXPECT_SILENT_STEP(&ctrl, a + bt(3U));
    EXPECT_SILENT_STEP(&ctrl, a + bt(9U));
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_ACTIVE, 0U, 1U);
}

/*
 * The same fault on a node that is error passive when it detects the first error: its error
 * flag is passive, and rule 6 counts 8 dominant bits after a passive flag, not 14. The node is
 * made error passive by 16 injected transmit errors on 0x000 (TEC 128 after the attempt that
 * ends at 669, as in the next test). Held dominant from 669, in force from the end of the
 * intermission, 672. Steps after 672: 6 rule 1 (REC 1), 13 rule 2 (REC 9), then rule 6 at 20
 * (7 bits after the rule-2 bit, REC 17) and 28 (REC 25).
 */
static void test_after_a_passive_error_flag_rule_6_counts_eight_dominant_bits(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t0 = 1000000U;
    const uint64_t e = t0 + bt(672U);

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_inject_tx_errors(&ctrl, 16U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, t0));
    run_until(&ctrl, t0 + bt(669U), &s_log);
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_PASSIVE, 128U, 0U);

    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_STUCK_DOMINANT, t0 + bt(669U));
    EXPECT_SILENT_STEP(&ctrl, e);
    EXPECT_WAKEUP(&ctrl, e + bt(6U));
    EXPECT_SILENT_STEP(&ctrl, e + bt(6U));
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_PASSIVE, 128U, 1U);
    EXPECT_WAKEUP(&ctrl, e + bt(13U));
    EXPECT_SILENT_STEP(&ctrl, e + bt(13U));
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_PASSIVE, 128U, 9U);
    EXPECT_WAKEUP(&ctrl, e + bt(20U));
    EXPECT_SILENT_STEP(&ctrl, e + bt(20U));
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_PASSIVE, 128U, 17U);
    EXPECT_WAKEUP(&ctrl, e + bt(28U));
    EXPECT_SILENT_STEP(&ctrl, e + bt(28U));
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_PASSIVE, 128U, 25U);
}

/*
 * A period of dominant bus ends the suspend of the node that sent the last frame before it.
 * 16 injected errors on 0x000 (39 bits each, 42 apart): attempt 15 ends at 669 and the node is
 * error passive, so its next attempt would wait for 669 + 11 = 680. Held dominant from 669, in
 * force from the end of the intermission, 672: no attempt. Released at r: the bus is free at
 * r + 11 and the frame goes then, [r + 11, r + 61]; not at r (the stale suspend time would let
 * it start at the release itself).
 */
static void test_a_bus_held_dominant_ends_the_suspend_of_the_last_transmitter(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t0 = 1000000U;
    const uint64_t r = t0 + bt(669U) + 10000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_inject_tx_errors(&ctrl, 16U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, t0));
    run_until(&ctrl, t0 + bt(669U), &s_log);
    TEST_ASSERT_EQUAL_UINT32(1U, s_log.count);
    EXPECT_CHANGE(&s_log.action[0], CAN_FC_ERROR_ACTIVE, CAN_FC_ERROR_PASSIVE);
    TEST_ASSERT_EQUAL_UINT64(t0 + bt(669U), s_log.action[0].time_ns);

    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_STUCK_DOMINANT, t0 + bt(669U));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(672U));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(678U)); /* the first counter step, not an attempt */
    run_until(&ctrl, r - 1U, &s_log);
    TEST_ASSERT_EQUAL_UINT64(16U, can_ctrl_stats(&ctrl)->tx_attempts_failed);

    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NONE, r);

    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, r + bt(61U), CAN_CTRL_ACTION_TX_COMPLETE, r + bt(61U));

    TEST_ASSERT_EQUAL_UINT64(r + bt(11U), done.start_ns);
}

/* Two 0x000 frames: [.., a] and [a + 3, a + 53]. A short set at a, in the first one's
 * intermission, takes effect at a + 3: the second never starts. While the short lasts nothing
 * is received at all, and when it ends at r nothing can have passed before r: a frame handed in
 * at r + 10 is placed [r, r + 50], not [r - 40, r + 10]. */
static void test_a_short_drops_the_received_frames_that_have_not_started(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t a = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, a);
    EXPECT_ACTION(&ctrl, a, CAN_CTRL_ACTION_RX_DELIVER, a);
    EXPECT_SILENT_STEP(&ctrl, a + bt(3U));
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_lost_fault);
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
    TEST_ASSERT_FALSE(can_ctrl_wire_rx(&ctrl, &frame, a + bt(100U)));
    TEST_ASSERT_EQUAL_UINT64(2U, can_ctrl_stats(&ctrl)->rx_lost_fault);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_frames);
    TEST_ASSERT_EQUAL_UINT64(bt(LEN_000), can_ctrl_stats(&ctrl)->busy_ns);

    const uint64_t r = a + 1000000U;

    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NONE, r);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, r + bt(10U)));

    const can_ctrl_action_t got =
        EXPECT_ACTION(&ctrl, r + bt(50U), CAN_CTRL_ACTION_RX_DELIVER, r + bt(50U));

    TEST_ASSERT_EQUAL_UINT64(r, got.start_ns);
}

/*
 * A fault set while a full action queue holds the model up still takes effect from the next
 * frame, also for a received frame that had to wait and starts before the fault. 20 frames
 * 0x000 arrive at a: frame k is [a + 53 k - 50, a + 53 k]; the first passed on a free bus, the
 * others waited. A poll at p = a + 760 bit times without taking actions stops after 15 of
 * them, before frame 15 [a + 745, a + 795] has started. A short set then takes effect at p, in
 * the middle of frame 15: frame 15 passes, frames 16 to 19 never start.
 */
static void test_a_fault_set_while_the_model_is_held_up_spares_a_frame_that_started_before_it(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t a = 1000000U;
    const uint64_t p = a + bt(760U);

    init_ctrl(&ctrl, BITRATE, 0U);

    for (uint32_t i = 0U; i < 20U; i++) {
        TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    }

    TEST_ASSERT_TRUE(can_ctrl_poll(&ctrl, p, NULL));
    TEST_ASSERT_EQUAL_UINT64(15U, can_ctrl_stats(&ctrl)->rx_frames);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, p);
    run_until(&ctrl, p + bt(1000U), &s_log);

    TEST_ASSERT_EQUAL_UINT32(16U, s_log.count);
    EXPECT_LOGGED(15U, CAN_CTRL_ACTION_RX_DELIVER, a + bt(745U), a + bt(795U));
    TEST_ASSERT_EQUAL_UINT64(4U, can_ctrl_stats(&ctrl)->rx_lost_fault);
    TEST_ASSERT_EQUAL_UINT64(16U, can_ctrl_stats(&ctrl)->rx_frames);
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
}

/*
 * A frame handed in at the very instant the removal of a dominant bus takes effect waits for
 * the bus to be released, 11 recessive bits: [r + 11, r + 61]. The bus became free long before
 * the fault, so without it the frame would have ended at its arrival, r, inside the period of
 * dominant bus.
 */
static void test_a_frame_handed_in_as_a_dominant_bus_is_released_waits_for_the_release(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t b = 1000000U;
    const uint64_t r = b + 10000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_STUCK_DOMINANT, b);
    run_until(&ctrl, r, &s_log); /* the counter steps; one change to error passive */
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NONE, r);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, r));
    EXPECT_SILENT_STEP(&ctrl, r); /* the release takes effect */

    /* The frame is received with REC 255: rule 8 takes the counter below 128 first. */
    const can_ctrl_action_t change =
        EXPECT_ACTION(&ctrl, r + bt(61U), CAN_CTRL_ACTION_STATE_CHANGED, r + bt(61U));

    EXPECT_CHANGE(&change, CAN_FC_ERROR_PASSIVE, CAN_FC_ERROR_ACTIVE);

    const can_ctrl_action_t got =
        EXPECT_ACTION(&ctrl, r + bt(61U), CAN_CTRL_ACTION_RX_DELIVER, r + bt(61U));

    TEST_ASSERT_EQUAL_UINT64(r + bt(11U), got.start_ns);
}

/*
 * Suspend transmission applies to the transmitter of the LAST frame on the bus. Using the
 * trajectory of the no-acknowledgment test: the node is error passive after attempt 15 (ends at
 * 941); attempt 16 is [952, 1008]. A frame received during it waits for the bus and starts at
 * the end of the intermission, where this node, in its suspend, does not compete: [1011, 1061].
 * It is then the last frame: attempt 17 starts after its intermission, at 1064, not at
 * 1008 + 11 = 1019 (which would overlap it) nor 8 bits later.
 */
static void test_suspend_transmission_ends_when_another_node_sends_the_last_frame(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NO_ACK, t0);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, t0));
    run_until(&ctrl, t0 + bt(952U), &s_log);
    TEST_ASSERT_EQUAL_UINT32(1U, s_log.count); /* the change to error passive */
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_PASSIVE, 128U, 0U);

    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, t0 + bt(960U)));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(1008U));

    const can_ctrl_action_t got =
        EXPECT_ACTION(&ctrl, t0 + bt(1061U), CAN_CTRL_ACTION_RX_DELIVER, t0 + bt(1061U));

    TEST_ASSERT_EQUAL_UINT64(t0 + bt(1011U), got.start_ns);
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(1064U));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(1064U + 56U));
}

/* ------------------------------------------------------------------- 6. bus off, recovery */

/* A node taken bus off by a short (see expect_short_to_bus_off), with frame 0x000, handle 7,
 * in its mailbox. With `remove_short` the short is lifted at the moment of bus off. */
static uint64_t bus_off_node(can_ctrl_t *ctrl, uint64_t t0, bool remove_short)
{
    const can_frame_t frame = frame_000();

    init_ctrl(ctrl, BITRATE, 0U);
    can_ctrl_set_fault(ctrl, CAN_BUS_FAULT_SHORT, t0);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(ctrl, &frame, 7U, t0));

    const uint64_t bus_off_ns = expect_short_to_bus_off(ctrl, t0, false);

    if (remove_short) {
        can_ctrl_set_fault(ctrl, CAN_BUS_FAULT_NONE, bus_off_ns);
        EXPECT_SILENT_STEP(ctrl, bus_off_ns + bt(3U)); /* after the intermission */
    }

    EXPECT_WAKEUP(ctrl, CAN_CTRL_NO_WAKEUP);
    return bus_off_ns;
}

static void test_a_bus_off_node_neither_sends_nor_receives_and_keeps_its_frames_until_aborted(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_009();
    uint32_t handles[CAN_CTRL_TX_MAILBOXES] = {0U};
    const uint64_t off = bus_off_node(&ctrl, 1000000U, true);

    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 8U, off + 1000U));
    TEST_ASSERT_EQUAL_UINT32(2U, can_ctrl_tx_pending(&ctrl));
    TEST_ASSERT_FALSE(can_ctrl_wire_rx(&ctrl, &frame, off + bt(100U)));
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_lost_fault);

    run_until(&ctrl, off + 10000000U, &s_log);
    TEST_ASSERT_EQUAL_UINT32(0U, s_log.count);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_lost_fault); /* counted once */
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, can_ctrl_state(&ctrl));
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->tx_frames);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->rx_frames);
    /* The refused frame still passed on the bus: 24 attempts of 15 bits and 49 bits. */
    TEST_ASSERT_EQUAL_UINT64(bt(360U + LEN_009), can_ctrl_stats(&ctrl)->busy_ns);
    TEST_ASSERT_EQUAL_UINT32(2U, can_ctrl_tx_pending(&ctrl));

    TEST_ASSERT_EQUAL_UINT32(2U, can_ctrl_tx_abort_all(&ctrl, handles, CAN_CTRL_TX_MAILBOXES));
    TEST_ASSERT_EQUAL_UINT32(7U, handles[0]);
    TEST_ASSERT_EQUAL_UINT32(8U, handles[1]);
    TEST_ASSERT_EQUAL_UINT32(0U, can_ctrl_tx_pending(&ctrl));
    TEST_ASSERT_EQUAL_UINT64(2U, can_ctrl_stats(&ctrl)->tx_aborted);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, can_ctrl_state(&ctrl));
}

/* On an idle bus the 128 occurrences of 11 recessive bits take 1408 bit times = 2816000 ns
 * from the request; then the frame left in the mailbox starts at once, [0, 50] after it. */
static void test_recovery_starts_only_on_request_and_takes_1408_bit_times_on_an_idle_bus(void)
{
    can_ctrl_t ctrl;
    const uint64_t off = bus_off_node(&ctrl, 1000000U, true);
    const uint64_t request = off + 1000000U;

    EXPECT_NO_ACTION(&ctrl, request - 1U); /* no request, no recovery */
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, can_ctrl_state(&ctrl));

    can_ctrl_request_restart(&ctrl, request);
    EXPECT_WAKEUP(&ctrl, request + 2816000U);
    can_ctrl_request_restart(&ctrl, request + 1000000U); /* asked again: changes nothing */
    EXPECT_WAKEUP(&ctrl, request + 2816000U);
    EXPECT_NO_ACTION(&ctrl, request + 2816000U - 1U);

    const can_ctrl_action_t change =
        EXPECT_ACTION(&ctrl, request + 2816000U, CAN_CTRL_ACTION_STATE_CHANGED, request + 2816000U);

    EXPECT_CHANGE(&change, CAN_FC_BUS_OFF, CAN_FC_ERROR_ACTIVE);
    TEST_ASSERT_EQUAL_UINT16(0U, change.tec);
    TEST_ASSERT_EQUAL_UINT16(0U, change.rec);

    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, request + 2816000U + bt(50U), CAN_CTRL_ACTION_TX_COMPLETE,
                      request + 2816000U + bt(50U));

    TEST_ASSERT_EQUAL_UINT32(7U, done.handle);
    TEST_ASSERT_EQUAL_UINT16(0U, done.tec);
}

static void test_a_restart_requested_before_bus_off_is_not_remembered(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_request_restart(&ctrl, 1000U); /* error active: ignored */
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_ACTIVE, 0U, 0U);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, t0);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 7U, t0));

    const uint64_t off = expect_short_to_bus_off(&ctrl, t0, false);

    EXPECT_NO_ACTION(&ctrl, off + 100000000U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, can_ctrl_state(&ctrl));
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
}

/*
 * Recovery with frames passing, idle time and frames combined as described in can_ctrl.c
 * (recovery_count_until). Two 0x000 frames (50 bits) handed in at request + 200 while the node
 * is bus off: refused, but on the bus at [150, 200] and [203, 253] bit times after the request.
 *   Idle run from the request: occurrences end at 11, 22, ..., 143; the 14th would end at 154,
 *   after the first start of frame at 150: 13.
 *   The first frame starts a run at its ACK delimiter, 150 + 50 - 8 = 192. Its first occurrence
 *   ends at 203, exactly the second frame's start of frame: it counts, 14.
 *   The second frame starts a run at 203 + 42 = 245; 114 more occurrences: 245 + 1254 = 1499.
 * Cross-check, one occurrence per frame: 13 idle + 1 + 1 = 15 by 256 (the end of the second
 * frame's intermission), then 113 x 11 = 1243 more: 1499 again. On an idle bus it is 1408.
 */
static void test_each_frame_passing_during_recovery_counts_as_one_occurrence(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t off = bus_off_node(&ctrl, 1000000U, true);
    const uint64_t request = off + 1000000U;
    const uint64_t busy_before = can_ctrl_stats(&ctrl)->busy_ns;

    can_ctrl_request_restart(&ctrl, request);
    TEST_ASSERT_FALSE(can_ctrl_wire_rx(&ctrl, &frame, request + bt(200U)));
    TEST_ASSERT_FALSE(can_ctrl_wire_rx(&ctrl, &frame, request + bt(200U)));
    TEST_ASSERT_EQUAL_UINT64(2U, can_ctrl_stats(&ctrl)->rx_lost_fault);

    run_until(&ctrl, request + bt(1499U) - 1U, &s_log);
    TEST_ASSERT_EQUAL_UINT32(0U, s_log.count);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, can_ctrl_state(&ctrl));
    EXPECT_WAKEUP(&ctrl, request + bt(1499U));

    const can_ctrl_action_t change = EXPECT_ACTION(
        &ctrl, request + bt(1499U), CAN_CTRL_ACTION_STATE_CHANGED, request + bt(1499U));

    EXPECT_CHANGE(&change, CAN_FC_BUS_OFF, CAN_FC_ERROR_ACTIVE);
    TEST_ASSERT_EQUAL_UINT64(busy_before + bt(100U), can_ctrl_stats(&ctrl)->busy_ns);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->rx_frames);
    TEST_ASSERT_EQUAL_UINT64(2U, can_ctrl_stats(&ctrl)->rx_lost_fault); /* once each */
}

/*
 * A burst handed in during recovery: 11 frames 0x000 at q + 1350, while the node is bus off.
 * They queue back to back: frame k on [1300 + 53 k, 1350 + 53 k] after the request q.
 *   Idle from q to the first start of frame at 1300: 118 occurrences (1298 <= 1300).
 *   Each frame k = 0..9 starts a run at its ACK delimiter, 1342 + 53 k; its first occurrence
 *   ends 11 bits later, at 1353 + 53 k, exactly the next frame's start of frame: one each.
 *   118 + 10 = 128 at the start of frame 10, 1300 + 530 = 1830. The recovery is complete there,
 *   before frame 10 passes, and is reported at that instant.
 * Frame 10 still passes (it was refused, so it is not delivered); the frame in the mailbox
 * follows it, [1883, 1933].
 */
static void test_a_recovery_completed_by_a_burst_of_frames_ends_at_a_start_of_frame(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t off = bus_off_node(&ctrl, 1000000U, true);
    const uint64_t q = off + 1000000U;

    can_ctrl_request_restart(&ctrl, q);

    for (uint32_t k = 0U; k < 11U; k++) {
        TEST_ASSERT_FALSE(can_ctrl_wire_rx(&ctrl, &frame, q + bt(1350U)));
    }

    run_until(&ctrl, q + bt(1830U) - 1U, &s_log);
    TEST_ASSERT_EQUAL_UINT32(0U, s_log.count);
    EXPECT_WAKEUP(&ctrl, q + bt(1830U));

    const can_ctrl_action_t change =
        EXPECT_ACTION(&ctrl, q + bt(1830U), CAN_CTRL_ACTION_STATE_CHANGED, q + bt(1830U));

    EXPECT_CHANGE(&change, CAN_FC_BUS_OFF, CAN_FC_ERROR_ACTIVE);

    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, q + bt(1933U), CAN_CTRL_ACTION_TX_COMPLETE, q + bt(1933U));

    TEST_ASSERT_EQUAL_UINT64(q + bt(1883U), done.start_ns);
    TEST_ASSERT_EQUAL_UINT64(11U, can_ctrl_stats(&ctrl)->rx_lost_fault); /* once each */
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->rx_frames);
}

/* The 128 occurrences were counted on an idle bus up to the moment of recovery, so a frame
 * handed in just after it cannot have started before it: arriving at 10 bits after the
 * recovery, it is placed [recovery, recovery + 50]. */
static void test_a_frame_arriving_just_after_a_recovery_is_placed_after_it(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t off = bus_off_node(&ctrl, 1000000U, true);
    const uint64_t done_ns = off + 1000000U + bt(1408U);

    TEST_ASSERT_EQUAL_UINT32(1U, can_ctrl_tx_abort_all(&ctrl, NULL, 0U));
    can_ctrl_request_restart(&ctrl, off + 1000000U);
    EXPECT_ACTION(&ctrl, done_ns, CAN_CTRL_ACTION_STATE_CHANGED, done_ns);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, done_ns + bt(10U)));

    const can_ctrl_action_t got =
        EXPECT_ACTION(&ctrl, done_ns + bt(50U), CAN_CTRL_ACTION_RX_DELIVER, done_ns + bt(50U));

    TEST_ASSERT_EQUAL_UINT64(done_ns, got.start_ns);
}

/* Frames refused while the node is bus off stay on the timeline; when a short then drops the
 * ones that have not started, they are not counted a second time. Three 0x000 at off + 200:
 * [150, 200], [203, 253], [256, 306]. The short is set at 210, during the second one, and takes
 * effect at 256: the third never passes. */
static void test_frames_refused_while_bus_off_are_counted_once_even_if_a_short_drops_them(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t off = bus_off_node(&ctrl, 1000000U, true);
    const uint64_t busy_before = can_ctrl_stats(&ctrl)->busy_ns;

    for (uint32_t k = 0U; k < 3U; k++) {
        TEST_ASSERT_FALSE(can_ctrl_wire_rx(&ctrl, &frame, off + bt(200U)));
    }

    TEST_ASSERT_EQUAL_UINT64(3U, can_ctrl_stats(&ctrl)->rx_lost_fault);
    EXPECT_NO_ACTION(&ctrl, off + bt(200U));
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, off + bt(210U));
    EXPECT_SILENT_STEP(&ctrl, off + bt(253U));
    EXPECT_SILENT_STEP(&ctrl, off + bt(256U));
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
    TEST_ASSERT_EQUAL_UINT64(3U, can_ctrl_stats(&ctrl)->rx_lost_fault);
    TEST_ASSERT_EQUAL_UINT64(busy_before + bt(100U), can_ctrl_stats(&ctrl)->busy_ns);
}

/*
 * Recovery counting stops while the bus is held dominant and resumes from the release.
 * Request at q; the bus is held dominant from q + 555: by then 50 occurrences have ended
 * (550 <= 555 < 561). Nothing happens while it is held. Released at r: a new run from r, 78
 * more occurrences: r + 858 bit times. The frame in the mailbox then goes [0, 50] after that.
 */
static void test_a_bus_held_dominant_holds_the_recovery_until_it_is_released(void)
{
    can_ctrl_t ctrl;
    const uint64_t off = bus_off_node(&ctrl, 1000000U, true);
    const uint64_t q = off + 1000000U;
    const uint64_t r = q + bt(555U) + 20000000U;

    can_ctrl_request_restart(&ctrl, q);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_STUCK_DOMINANT, q + bt(555U));
    EXPECT_SILENT_STEP(&ctrl, q + bt(555U));
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP); /* no counter steps either: the node is bus off */
    EXPECT_NO_ACTION(&ctrl, r - 1U);
    EXPECT_STATE(&ctrl, CAN_FC_BUS_OFF, 256U, 0U);

    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NONE, r);
    EXPECT_SILENT_STEP(&ctrl, r);
    EXPECT_WAKEUP(&ctrl, r + bt(858U));

    const can_ctrl_action_t change =
        EXPECT_ACTION(&ctrl, r + bt(858U), CAN_CTRL_ACTION_STATE_CHANGED, r + bt(858U));

    EXPECT_CHANGE(&change, CAN_FC_BUS_OFF, CAN_FC_ERROR_ACTIVE);

    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, r + bt(908U), CAN_CTRL_ACTION_TX_COMPLETE, r + bt(908U));

    TEST_ASSERT_EQUAL_UINT64(r + bt(858U), done.start_ns);
}

/* A shorted bus reads recessive, so the recovery completes 1408 bit times after the request;
 * the frame in the mailbox then meets the short again and the node goes bus off again,
 * on exactly the trajectory of the first time. */
static void test_a_node_on_a_bus_still_shorted_recovers_and_goes_bus_off_again(void)
{
    can_ctrl_t ctrl;
    const uint64_t off = bus_off_node(&ctrl, 1000000U, false);
    const uint64_t done_ns = off + 1000000U + bt(1408U);

    can_ctrl_request_restart(&ctrl, off + 1000000U);

    const can_ctrl_action_t change =
        EXPECT_ACTION(&ctrl, done_ns, CAN_CTRL_ACTION_STATE_CHANGED, done_ns);

    EXPECT_CHANGE(&change, CAN_FC_BUS_OFF, CAN_FC_ERROR_ACTIVE);
    TEST_ASSERT_EQUAL_UINT64(done_ns + bt(557U), expect_short_to_bus_off(&ctrl, done_ns, true));
    TEST_ASSERT_EQUAL_UINT64(2U, can_ctrl_stats(&ctrl)->bus_off_events);
    TEST_ASSERT_EQUAL_UINT32(1U, can_ctrl_tx_pending(&ctrl));
}

/*
 * 32 injected errors on 0x000: 25 + 14 = 39 bits each. Error active: attempt k at 42 k
 * (k = 0..15); attempt 15 ends at 669 with TEC 128. Error passive with suspend, 50 per
 * attempt: attempt k at 680 + 50 (k - 16); attempt 31 is [1430, 1469] and leaves TEC 256.
 * A frame received during it is placed after it, [1472, 1522], and ends while the node is bus
 * off: it is not delivered and counts as lost.
 */
static void test_a_frame_that_ends_after_the_node_went_bus_off_is_lost(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_inject_tx_errors(&ctrl, 32U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, t0));
    run_until(&ctrl, t0 + bt(1440U), &s_log);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, t0 + bt(1440U)));
    run_until(&ctrl, t0 + bt(3000U), &s_log);

    TEST_ASSERT_EQUAL_UINT32(2U, s_log.count);
    EXPECT_CHANGE(&s_log.action[0], CAN_FC_ERROR_ACTIVE, CAN_FC_ERROR_PASSIVE);
    TEST_ASSERT_EQUAL_UINT64(t0 + bt(669U), s_log.action[0].time_ns);
    EXPECT_CHANGE(&s_log.action[1], CAN_FC_ERROR_PASSIVE, CAN_FC_BUS_OFF);
    TEST_ASSERT_EQUAL_UINT64(t0 + bt(1469U), s_log.action[1].time_ns);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->rx_lost_fault);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->rx_frames);
    TEST_ASSERT_EQUAL_UINT64(bt((32U * 39U) + 50U), can_ctrl_stats(&ctrl)->busy_ns);
}

/* ------------------------------------------------------------------- aborting */

static void
test_abort_removes_the_waiting_frames_in_submission_order_and_spares_the_one_on_the_wire(void)
{
    can_ctrl_t ctrl;
    const can_frame_t busy = frame_2aa();
    uint32_t handles[CAN_CTRL_TX_MAILBOXES] = {0U};
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);

    /* 0x2AA (1) in mailbox 0 [0, 108], 0x009 (2) in 1, 0x078 (3) in 2. 0x009 starts at 111;
     * then 0x104 (4) takes mailbox 0. Waiting: 3 in mailbox 2, 4 in mailbox 0. */
    const can_frame_t f009 = frame_009();
    const can_frame_t f078 = frame_078();
    const can_frame_t f104 = frame_104();

    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &busy, 1U, t0));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f009, 2U, t0 + bt(1U)));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f078, 3U, t0 + bt(2U)));
    EXPECT_ACTION(&ctrl, t0 + bt(108U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(108U));
    EXPECT_SILENT_STEP(&ctrl, t0 + bt(111U));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f104, 4U, t0 + bt(112U)));

    TEST_ASSERT_EQUAL_UINT32(2U, can_ctrl_tx_abort_all(&ctrl, handles, CAN_CTRL_TX_MAILBOXES));
    TEST_ASSERT_EQUAL_UINT32(3U, handles[0]);
    TEST_ASSERT_EQUAL_UINT32(4U, handles[1]);
    TEST_ASSERT_EQUAL_UINT32(1U, can_ctrl_tx_pending(&ctrl));
    TEST_ASSERT_EQUAL_UINT64(2U, can_ctrl_stats(&ctrl)->tx_aborted);

    /* The marked frame's attempt succeeds: it is transmitted and reported as usual. */
    const can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, t0 + bt(160U), CAN_CTRL_ACTION_TX_COMPLETE, t0 + bt(160U));

    TEST_ASSERT_EQUAL_UINT32(2U, done.handle);
    TEST_ASSERT_EQUAL_UINT64(2U, can_ctrl_stats(&ctrl)->tx_aborted);
    TEST_ASSERT_EQUAL_UINT32(0U, can_ctrl_tx_pending(&ctrl));
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);

    /* A frame submitted after the abort is not affected, even after a failed attempt. */
    can_ctrl_inject_tx_errors(&ctrl, 1U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f104, 5U, t0 + bt(200U)));
    run_until(&ctrl, t0 + bt(400U), &s_log);
    TEST_ASSERT_EQUAL_UINT32(1U, s_log.count);
    TEST_ASSERT_EQUAL_UINT32(5U, s_log.action[0].handle);
    TEST_ASSERT_EQUAL_INT(CAN_CTRL_ACTION_TX_COMPLETE, s_log.action[0].kind);
}

/* The short trajectory: attempt 7 is [126, 141] and makes the node error passive. Aborted while
 * it is on the wire, the frame is removed when the attempt fails, after its error counting:
 * STATE_CHANGED first, then TX_ABORTED; and it is not tried again. */
static void test_an_aborted_frame_whose_attempt_fails_is_removed_after_the_error_counting(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t0 = 1000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, t0);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 6U, t0));
    run_until(&ctrl, t0 + bt(126U), &s_log);
    TEST_ASSERT_EQUAL_UINT16(112U, can_ctrl_tec(&ctrl));

    TEST_ASSERT_EQUAL_UINT32(0U, can_ctrl_tx_abort_all(&ctrl, NULL, 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, can_ctrl_tx_pending(&ctrl));
    EXPECT_WAKEUP(&ctrl, t0 + bt(141U));

    const can_ctrl_action_t change =
        EXPECT_ACTION(&ctrl, t0 + bt(141U), CAN_CTRL_ACTION_STATE_CHANGED, t0 + bt(141U));

    EXPECT_CHANGE(&change, CAN_FC_ERROR_ACTIVE, CAN_FC_ERROR_PASSIVE);

    const can_ctrl_action_t aborted =
        EXPECT_ACTION(&ctrl, t0 + bt(141U), CAN_CTRL_ACTION_TX_ABORTED, t0 + bt(141U));

    TEST_ASSERT_EQUAL_UINT32(6U, aborted.handle);
    EXPECT_FRAME(&frame, &aborted.frame);
    TEST_ASSERT_EQUAL_UINT16(128U, aborted.tec);
    TEST_ASSERT_EQUAL_UINT32(0U, can_ctrl_tx_pending(&ctrl));
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->tx_aborted);
    TEST_ASSERT_EQUAL_UINT64(8U, can_ctrl_stats(&ctrl)->tx_attempts_failed);
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
}

static void test_abort_writes_no_more_handles_than_asked_and_needs_no_buffer(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    uint32_t handles[2] = {0xDEADU, 0xBEEFU};

    init_ctrl(&ctrl, BITRATE, 0U);

    /* Not polled since: nothing has started, all three wait. */
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 11U, 1000U));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 12U, 1000U));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 13U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(3U, can_ctrl_tx_abort_all(&ctrl, handles, 1U));
    TEST_ASSERT_EQUAL_UINT32(11U, handles[0]);
    TEST_ASSERT_EQUAL_UINT32(0xBEEFU, handles[1]);
    TEST_ASSERT_EQUAL_UINT32(0U, can_ctrl_tx_pending(&ctrl));

    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 14U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(1U, can_ctrl_tx_abort_all(&ctrl, NULL, 8U));
    TEST_ASSERT_EQUAL_UINT64(4U, can_ctrl_stats(&ctrl)->tx_aborted);
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
    EXPECT_NO_ACTION(&ctrl, 10000000U);
}

/* ------------------------------------------------------------------- 7. bookkeeping */

static void test_invalid_frames_are_refused_by_both_inputs_and_counted_nowhere(void)
{
    can_ctrl_t ctrl;
    can_ctrl_stats_t zero;
    can_frame_t bad[4];

    (void)memset(&zero, 0, sizeof(zero));
    bad[0] = base_frame(0x800U, 0U, 0U); /* beyond 11 bits */
    bad[1] = base_frame(0x20000000U, 0U, 0U);
    bad[1].flags = CAN_FLAG_EXT; /* beyond 29 bits */
    bad[2] = base_frame(0x100U, 8U, 0U);
    bad[2].dlc = 9U;
    bad[3] = base_frame(0x100U, 0U, 0U);
    bad[3].flags = 0x04U; /* unknown flag */

    init_ctrl(&ctrl, BITRATE, 0U);

    for (uint32_t i = 0U; i < 4U; i++) {
        TEST_ASSERT_FALSE(can_ctrl_tx_submit(&ctrl, &bad[i], i, 1000U));
        TEST_ASSERT_FALSE(can_ctrl_wire_rx(&ctrl, &bad[i], 1000U));
    }

    TEST_ASSERT_FALSE(can_ctrl_tx_submit(&ctrl, NULL, 9U, 1000U));
    TEST_ASSERT_FALSE(can_ctrl_wire_rx(&ctrl, NULL, 1000U));
    TEST_ASSERT_EQUAL_UINT32(0U, can_ctrl_tx_pending(&ctrl));
    TEST_ASSERT_EQUAL_MEMORY(&zero, can_ctrl_stats(&ctrl), sizeof(zero));
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
}

static void test_a_ninth_frame_is_refused_while_eight_mailboxes_are_in_use(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();

    init_ctrl(&ctrl, BITRATE, 0U);

    for (uint32_t i = 0U; i < 8U; i++) {
        TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, i, 0U));
    }

    TEST_ASSERT_FALSE(can_ctrl_tx_submit(&ctrl, &frame, 8U, 0U));
    TEST_ASSERT_EQUAL_UINT32(8U, can_ctrl_tx_pending(&ctrl));

    /* The first one ends at 50: its mailbox is free again. */
    EXPECT_ACTION(&ctrl, bt(50U), CAN_CTRL_ACTION_TX_COMPLETE, bt(50U));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 8U, bt(50U)));
    TEST_ASSERT_FALSE(can_ctrl_tx_submit(&ctrl, &frame, 9U, bt(50U)));
}

/* 40 frames arriving together: frame k ends at a + 53 k. Advanced far ahead without taking the
 * actions, the model stops when the queue cannot take the two actions one event may produce:
 * after 15 frames. Nothing is lost: all 40 come out in order with their own times. */
static void test_a_full_action_queue_stops_the_model_without_losing_an_action(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t a = 1000000U;
    const uint64_t late = a + 1000000000U;

    init_ctrl(&ctrl, BITRATE, 0U);

    for (uint32_t i = 0U; i < 40U; i++) {
        TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    }

    TEST_ASSERT_TRUE(can_ctrl_poll(&ctrl, late, NULL));
    TEST_ASSERT_EQUAL_UINT64(15U, can_ctrl_stats(&ctrl)->rx_frames);
    EXPECT_WAKEUP(&ctrl, late);

    for (uint32_t k = 0U; k < 40U; k++) {
        EXPECT_ACTION(&ctrl, late, CAN_CTRL_ACTION_RX_DELIVER, a + bt(53U * k));
    }

    EXPECT_NO_ACTION(&ctrl, late);
    TEST_ASSERT_EQUAL_UINT64(40U, can_ctrl_stats(&ctrl)->rx_frames);
}

/*
 * While the model is held up by a full action queue, an input is applied to the model as it
 * stands; a frame submitted then still competes only from its submission time on.
 *   20 frames 0x000 arrive at a: frame k ends at a + 53 k, the last at a + 1007; 0x078 (1),
 *   submitted at a + 1 ns, can start at the end of that intermission, s = a + 1010.
 *   A poll at s + 10 without taking actions stops after 15 queued actions, before s. 0x009 (2)
 *   submitted then has the smaller key, but was not there at s: 0x078 starts at s (the poll is
 *   10 bit times late, less than a frame length: no resync) and 0x009 follows at s + 52.
 */
#define HELD_UP_A 1000000U

/* The situation above, up to the second submission. Returns s. */
static uint64_t hold_up_with_two_frames(can_ctrl_t *ctrl)
{
    const can_frame_t theirs = frame_000();
    const can_frame_t f078 = frame_078();
    const can_frame_t f009 = frame_009();
    const uint64_t s = HELD_UP_A + bt(1010U);

    init_ctrl(ctrl, BITRATE, 0U);

    for (uint32_t i = 0U; i < 20U; i++) {
        TEST_ASSERT_TRUE(can_ctrl_wire_rx(ctrl, &theirs, HELD_UP_A));
    }

    TEST_ASSERT_TRUE(can_ctrl_tx_submit(ctrl, &f078, 1U, HELD_UP_A + 1U));
    TEST_ASSERT_TRUE(can_ctrl_poll(ctrl, s + bt(10U), NULL));
    TEST_ASSERT_EQUAL_UINT64(15U, can_ctrl_stats(ctrl)->rx_frames); /* held up before s */
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(ctrl, &f009, 2U, s + bt(10U)));

    return s;
}

static void test_a_frame_submitted_while_the_model_is_held_up_competes_from_its_submission(void)
{
    can_ctrl_t ctrl;
    const uint64_t s = hold_up_with_two_frames(&ctrl);

    for (uint32_t k = 0U; k < 20U; k++) {
        EXPECT_ACTION(&ctrl, s + bt(10U), CAN_CTRL_ACTION_RX_DELIVER, HELD_UP_A + bt(53U * k));
    }

    can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, s + bt(49U), CAN_CTRL_ACTION_TX_COMPLETE, s + bt(49U));

    TEST_ASSERT_EQUAL_UINT32(1U, done.handle);
    TEST_ASSERT_EQUAL_UINT64(s, done.start_ns);
    done = EXPECT_ACTION(&ctrl, s + bt(101U), CAN_CTRL_ACTION_TX_COMPLETE, s + bt(101U));
    TEST_ASSERT_EQUAL_UINT32(2U, done.handle);
    TEST_ASSERT_EQUAL_UINT64(s + bt(10U), done.submit_ns);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->late_resyncs);
}

/* Received frames keep their arrival order also while a full action queue holds the model up:
 * a frame handed in then goes behind the frames still waiting, although the bus, as the model
 * stands, has been free for it since long before. 20 frames 0x000 arrive at a (frame k ends at
 * a + 53 k); a poll at `late` without taking actions stops after 15 of them. 0x009 handed in
 * at `late` comes out after the other five, [late, late + 49]: it competes from its hand-in. */
static void test_a_frame_handed_in_while_the_model_is_held_up_goes_behind_the_waiting_ones(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const can_frame_t f009 = frame_009();
    const uint64_t a = 1000000U;
    const uint64_t late = a + 1000000000U;

    init_ctrl(&ctrl, BITRATE, 0U);

    for (uint32_t i = 0U; i < 20U; i++) {
        TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, a));
    }

    TEST_ASSERT_TRUE(can_ctrl_poll(&ctrl, late, NULL));
    TEST_ASSERT_EQUAL_UINT64(15U, can_ctrl_stats(&ctrl)->rx_frames);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &f009, late));
    run_until(&ctrl, late + bt(1000U), &s_log);

    TEST_ASSERT_EQUAL_UINT32(21U, s_log.count);

    for (uint32_t k = 1U; k < 20U; k++) {
        EXPECT_LOGGED(k, CAN_CTRL_ACTION_RX_DELIVER, a + bt((53U * k) - 50U), a + bt(53U * k));
    }

    EXPECT_LOGGED(20U, CAN_CTRL_ACTION_RX_DELIVER, late, late + bt(LEN_009));
    EXPECT_FRAME(&f009, &s_log.action[20].frame);
}

/* The same, but the queue is read 100 bit times after s, more than a frame length: the start
 * moves to that poll (late-caller rule), and by then 0x009 is there and wins arbitration. */
static void test_a_start_moved_by_a_late_poll_is_arbitrated_among_the_frames_there_by_then(void)
{
    can_ctrl_t ctrl;
    const uint64_t s = hold_up_with_two_frames(&ctrl);
    const uint64_t late = s + bt(100U);

    for (uint32_t k = 0U; k < 20U; k++) {
        EXPECT_ACTION(&ctrl, late, CAN_CTRL_ACTION_RX_DELIVER, HELD_UP_A + bt(53U * k));
    }

    can_ctrl_action_t done =
        EXPECT_ACTION(&ctrl, late + bt(49U), CAN_CTRL_ACTION_TX_COMPLETE, late + bt(49U));

    TEST_ASSERT_EQUAL_UINT32(2U, done.handle);
    TEST_ASSERT_EQUAL_UINT64(late, done.start_ns);
    done = EXPECT_ACTION(&ctrl, late + bt(101U), CAN_CTRL_ACTION_TX_COMPLETE, late + bt(101U));
    TEST_ASSERT_EQUAL_UINT32(1U, done.handle);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->late_resyncs);
}

/* An injected count that would wrap is held at its maximum: after UINT32_MAX and 5 more, the
 * first 5 attempts all fail (wrapped, only 4 would). 0x009: attempts of 38 bits, 41 apart. */
static void test_injected_error_counts_saturate_instead_of_wrapping(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_009();

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_inject_tx_errors(&ctrl, UINT32_MAX);
    can_ctrl_inject_tx_errors(&ctrl, 5U);
    can_ctrl_inject_rx_errors(&ctrl, UINT32_MAX);
    can_ctrl_inject_rx_errors(&ctrl, 5U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, 0U));
    run_until(&ctrl, bt((41U * 4U) + 38U), &s_log);
    TEST_ASSERT_EQUAL_UINT64(5U, can_ctrl_stats(&ctrl)->tx_attempts_failed);
    TEST_ASSERT_EQUAL_UINT16(40U, can_ctrl_tec(&ctrl));

    /* The sixth attempt would start at 205: the frame waits, and is aborted. */
    TEST_ASSERT_EQUAL_UINT32(1U, can_ctrl_tx_abort_all(&ctrl, NULL, 0U));
    run_until(&ctrl, bt(1000U), &s_log);

    for (uint32_t i = 0U; i < 5U; i++) {
        TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, bt(2000U)));
    }

    run_until(&ctrl, bt(3000U), &s_log);
    TEST_ASSERT_EQUAL_UINT64(5U, can_ctrl_stats(&ctrl)->rx_errors);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->rx_frames);
    TEST_ASSERT_EQUAL_UINT32(0U, s_log.count);
}

/* A frame handed in with an arrival time before the last event processed is placed so that it
 * ends no earlier than that event. Here a no-acknowledgment fault takes effect at f on an idle
 * bus; a frame stamped f - 500 us then ends at f, starting one length before. A time that goes
 * back is taken as the latest time seen, for submissions as for polls. */
static void test_an_input_dated_before_what_was_processed_does_not_go_back_in_time(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t f = 2000000U;

    init_ctrl(&ctrl, BITRATE, 0U);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NO_ACK, f);
    EXPECT_NO_ACTION(&ctrl, f);
    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &frame, f - 500000U));

    const can_ctrl_action_t got = EXPECT_ACTION(&ctrl, f - 1U, CAN_CTRL_ACTION_RX_DELIVER, f);

    TEST_ASSERT_EQUAL_UINT64(f - bt(LEN_000), got.start_ns);

    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NONE, f);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, f - 2U));

    const can_ctrl_action_t done = EXPECT_ACTION(
        &ctrl, f + bt(3U) + bt(LEN_000), CAN_CTRL_ACTION_TX_COMPLETE, f + bt(3U) + bt(LEN_000));

    TEST_ASSERT_EQUAL_UINT64(f, done.submit_ns);
    TEST_ASSERT_EQUAL_UINT64(f + bt(3U), done.start_ns); /* after the received frame */
}

/* ------------------------------------------------------------------- pacing off */

static void test_with_pacing_off_frames_complete_at_the_poll_that_lets_them_start(void)
{
    can_ctrl_t ctrl;
    const can_frame_t f078 = frame_078();
    const can_frame_t f009 = frame_009();
    const uint64_t t = 5000U;

    init_ctrl(&ctrl, 0U, 0U);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f078, 1U, t));
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f009, 2U, t));
    EXPECT_WAKEUP(&ctrl, t);

    /* Both at t, still in arbitration order. */
    can_ctrl_action_t got = EXPECT_ACTION(&ctrl, t, CAN_CTRL_ACTION_TX_COMPLETE, t);

    TEST_ASSERT_EQUAL_UINT32(2U, got.handle);
    TEST_ASSERT_EQUAL_UINT64(t, got.start_ns);
    got = EXPECT_ACTION(&ctrl, t, CAN_CTRL_ACTION_TX_COMPLETE, t);
    TEST_ASSERT_EQUAL_UINT32(1U, got.handle);

    TEST_ASSERT_TRUE(can_ctrl_wire_rx(&ctrl, &f009, t + 100U));
    got = EXPECT_ACTION(&ctrl, t + 100U, CAN_CTRL_ACTION_RX_DELIVER, t + 100U);
    TEST_ASSERT_EQUAL_UINT64(t + 100U, got.start_ns);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->late_resyncs);

    /* Polled 100 ns after it could have started: it starts and completes at the poll. */
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &f078, 3U, t + 200U));
    got = EXPECT_ACTION(&ctrl, t + 300U, CAN_CTRL_ACTION_TX_COMPLETE, t + 300U);
    TEST_ASSERT_EQUAL_UINT64(t + 300U, got.start_ns);
    TEST_ASSERT_EQUAL_UINT64(1U, can_ctrl_stats(&ctrl)->late_resyncs);
    TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&ctrl)->busy_ns);
}

static void test_with_pacing_off_a_node_never_acknowledged_does_not_hang_the_poll(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t = 5000U;

    init_ctrl(&ctrl, 0U, 0U);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NO_ACK, t);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, t));

    /* Every retry happens at t. The poll returns all the same, and asks to be called again. */
    const can_ctrl_action_t change = EXPECT_ACTION(&ctrl, t, CAN_CTRL_ACTION_STATE_CHANGED, t);

    EXPECT_CHANGE(&change, CAN_FC_ERROR_ACTIVE, CAN_FC_ERROR_PASSIVE);
    EXPECT_NO_ACTION(&ctrl, t);

    const uint64_t attempts = can_ctrl_stats(&ctrl)->tx_attempts_failed;

    TEST_ASSERT_TRUE(attempts > 16U);
    EXPECT_WAKEUP(&ctrl, t);
    EXPECT_NO_ACTION(&ctrl, t);
    TEST_ASSERT_TRUE(can_ctrl_stats(&ctrl)->tx_attempts_failed > attempts);
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_PASSIVE, 128U, 0U);

    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NONE, t);
    run_until(&ctrl, t, &s_log);
    TEST_ASSERT_EQUAL_UINT32(2U, s_log.count);
    EXPECT_CHANGE(&s_log.action[0], CAN_FC_ERROR_PASSIVE, CAN_FC_ERROR_ACTIVE);
    TEST_ASSERT_EQUAL_INT(CAN_CTRL_ACTION_TX_COMPLETE, s_log.action[1].kind);
    TEST_ASSERT_EQUAL_UINT64(t, s_log.action[1].time_ns);
}

static void test_with_pacing_off_bus_off_recovery_and_a_dominant_bus_take_no_time(void)
{
    can_ctrl_t ctrl;
    const can_frame_t frame = frame_000();
    const uint64_t t = 5000U;

    init_ctrl(&ctrl, 0U, 0U);
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_SHORT, t);
    TEST_ASSERT_TRUE(can_ctrl_tx_submit(&ctrl, &frame, 1U, t));
    run_until(&ctrl, t, &s_log);
    TEST_ASSERT_EQUAL_UINT32(2U, s_log.count); /* passive, then bus off, both at t */
    TEST_ASSERT_EQUAL_UINT64(t, s_log.action[1].time_ns);
    EXPECT_STATE(&ctrl, CAN_FC_BUS_OFF, 256U, 0U);

    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_NONE, t + 1U);
    can_ctrl_request_restart(&ctrl, t + 1U);

    const can_ctrl_action_t change =
        EXPECT_ACTION(&ctrl, t + 1U, CAN_CTRL_ACTION_STATE_CHANGED, t + 1U);

    EXPECT_CHANGE(&change, CAN_FC_BUS_OFF, CAN_FC_ERROR_ACTIVE);
    EXPECT_ACTION(&ctrl, t + 1U, CAN_CTRL_ACTION_TX_COMPLETE, t + 1U);

    /* Held dominant: all 33 counter steps at once, the change to error passive among them. */
    can_ctrl_set_fault(&ctrl, CAN_BUS_FAULT_STUCK_DOMINANT, t + 2U);

    const can_ctrl_action_t passive =
        EXPECT_ACTION(&ctrl, t + 2U, CAN_CTRL_ACTION_STATE_CHANGED, t + 2U);

    EXPECT_CHANGE(&passive, CAN_FC_ERROR_ACTIVE, CAN_FC_ERROR_PASSIVE);
    EXPECT_NO_ACTION(&ctrl, t + 2U);
    EXPECT_STATE(&ctrl, CAN_FC_ERROR_PASSIVE, 0U, 255U);
    EXPECT_WAKEUP(&ctrl, CAN_CTRL_NO_WAKEUP);
}

/* ------------------------------------------------------------------- random scenarios */

/* xorshift32, re-seeded by every scenario. */
static uint32_t s_rng;

static uint32_t rng_next(void)
{
    uint32_t x = s_rng;

    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    s_rng = x;

    return x;
}

static uint32_t rng_below(uint32_t n)
{
    return rng_next() % n;
}

typedef enum {
    INPUT_SUBMIT = 0,
    INPUT_WIRE_RX,
    INPUT_BURST,
    INPUT_FAULT,
    INPUT_INJECT_TX,
    INPUT_INJECT_RX,
    INPUT_RESTART,
    INPUT_ABORT
} input_kind_t;

typedef struct {
    uint64_t time_ns;
    can_frame_t frame;
    uint32_t value; /* handle, fault, count or burst length */
    input_kind_t kind;
} input_t;

/* What both drivers record, actions and the results of inputs alike, compared field by field. */
typedef struct {
    uint64_t time_ns;
    uint64_t start_ns;
    uint64_t submit_ns;
    uint32_t handle;
    uint32_t id;
    uint32_t kind; /* an action kind, or 100 + an input kind */
    uint32_t value;
    uint16_t tec;
    uint16_t rec;
    uint8_t state;
    uint8_t old_state;
} record_t;

#define SCRIPT_MAX    800U
#define RECORDS_MAX   8000U
#define SCENARIO_NS   60000000U /* 60 ms of model time */
#define POLL_GRID_NS  1000U     /* the brute-force driver polls every microsecond */
#define FRAME_PENDING 1U
#define FRAME_DONE    2U
#define FRAME_ABORTED 3U

typedef struct {
    record_t record[RECORDS_MAX];
    uint32_t count;
    uint8_t fate[SCRIPT_MAX]; /* by handle (the script index): FRAME_* or 0 if never accepted */
    uint64_t start_ns;
    uint64_t free_ns; /* end of the intermission after the last frame reported */
} trace_t;

static input_t s_script[SCRIPT_MAX];
static trace_t s_trace_a;
static trace_t s_trace_b;
static can_ctrl_t s_ctrl_a;
static can_ctrl_t s_ctrl_b;

static can_frame_t random_frame(void)
{
    static const uint32_t common_ids[] = {0x100U, 0x101U, 0x7FFU, 0x000U};
    can_frame_t frame;

    (void)memset(&frame, 0, sizeof(frame));

    if (rng_below(4U) == 0U) {
        frame.flags = CAN_FLAG_EXT;
        frame.id = rng_next() & CAN_ID_EXT_MAX;
    } else if (rng_below(3U) == 0U) {
        frame.id = common_ids[rng_below(4U)]; /* equal keys */
    } else {
        frame.id = rng_next() & CAN_ID_STD_MAX;
    }

    if (rng_below(8U) == 0U) {
        frame.flags |= CAN_FLAG_RTR;
    }

    frame.dlc = (uint8_t)rng_below(9U);

    for (uint32_t i = 0U; i < 8U; i++) {
        frame.data[i] = (uint8_t)rng_next();
    }

    if (rng_below(32U) == 0U) {
        frame.dlc = 9U; /* invalid */
    }

    return frame;
}

static uint32_t make_script(uint32_t seed)
{
    uint64_t t = 0U;
    uint32_t count = 0U;

    s_rng = seed;

    while (count < SCRIPT_MAX) {
        t += (uint64_t)(1U + rng_below(250U)) * POLL_GRID_NS;

        if (t >= SCENARIO_NS) {
            break;
        }

        /* Two inputs at the same time now and then: both drivers must apply them together. */
        if ((count > 0U) && (rng_below(10U) == 0U)) {
            t = s_script[count - 1U].time_ns;
        }

        input_t *const in = &s_script[count];
        const uint32_t pick = rng_below(100U);

        in->time_ns = t;
        in->frame = random_frame();
        in->value = count;

        /* Received traffic is kept well below the bus capacity apart from rare bursts: a
         * waiting received frame with a smaller key goes ahead of a transmission, so a
         * sustained flood would leave few transmissions with large keys to compare. */
        if (pick < 55U) {
            in->kind = INPUT_SUBMIT;
        } else if (pick < 75U) {
            in->kind = INPUT_WIRE_RX;
        } else if (pick < 77U) {
            in->kind = INPUT_BURST;
            in->value = 1U + rng_below(50U);
        } else if (pick < 81U) {
            in->kind = INPUT_FAULT;
            in->value = (rng_below(2U) == 0U) ? 0U : rng_below(4U);
        } else if (pick < 85U) {
            in->kind = INPUT_INJECT_TX;
            in->value = 1U + rng_below(3U);
        } else if (pick < 89U) {
            in->kind = INPUT_INJECT_RX;
            in->value = 1U + rng_below(3U);
        } else if (pick < 98U) {
            in->kind = INPUT_RESTART;
        } else {
            in->kind = INPUT_ABORT;
        }

        count++;
    }

    return count;
}

static void add_record(trace_t *trace, const record_t *record)
{
    TEST_ASSERT_TRUE(trace->count < RECORDS_MAX);
    trace->record[trace->count] = *record;
    trace->count++;
}

/* Take every due action, record it, and keep the books on each frame's fate. */
static void drain(can_ctrl_t *ctrl, uint64_t now_ns, trace_t *trace)
{
    can_ctrl_action_t action;

    while (can_ctrl_poll(ctrl, now_ns, &action)) {
        record_t record;

        (void)memset(&record, 0, sizeof(record));
        record.time_ns = action.time_ns;
        record.start_ns = action.start_ns;
        record.submit_ns = action.submit_ns;
        record.handle = action.handle;
        record.id = action.frame.id;
        record.kind = (uint32_t)action.kind;
        record.tec = action.tec;
        record.rec = action.rec;
        record.state = (uint8_t)action.state;
        record.old_state = (uint8_t)action.old_state;
        add_record(trace, &record);

        TEST_ASSERT_TRUE(action.time_ns <= now_ns);

        /* One frame at a time: whoever sent them, the frames reported follow one another on
         * the bus, each after the intermission of the one before. */
        if ((action.kind == CAN_CTRL_ACTION_TX_COMPLETE) ||
            (action.kind == CAN_CTRL_ACTION_RX_DELIVER)) {
            TEST_ASSERT_TRUE(action.start_ns >= trace->free_ns);
            trace->free_ns = action.time_ns + bt(CAN_BITS_INTERMISSION);
        }

        if ((action.kind == CAN_CTRL_ACTION_TX_COMPLETE) ||
            (action.kind == CAN_CTRL_ACTION_TX_ABORTED)) {
            if (action.handle >= SCRIPT_MAX) {
                TEST_FAIL_MESSAGE("an outcome for a handle that was never submitted");
            } else {
                TEST_ASSERT_EQUAL_UINT8(FRAME_PENDING, trace->fate[action.handle]);
                trace->fate[action.handle] =
                    (action.kind == CAN_CTRL_ACTION_TX_COMPLETE) ? FRAME_DONE : FRAME_ABORTED;
            }
        }
    }

    /* The bus never carries more than the time that has passed. */
    TEST_ASSERT_TRUE(can_ctrl_stats(ctrl)->busy_ns <= (now_ns - trace->start_ns));
}

static void apply_input(can_ctrl_t *ctrl, const input_t *in, trace_t *trace)
{
    record_t record;
    uint32_t handles[CAN_CTRL_TX_MAILBOXES];

    (void)memset(&record, 0, sizeof(record));
    record.kind = 100U + (uint32_t)in->kind;
    record.time_ns = in->time_ns;

    switch (in->kind) {
    case INPUT_SUBMIT:
        record.value = can_ctrl_tx_submit(ctrl, &in->frame, in->value, in->time_ns) ? 1U : 0U;

        if (record.value == 1U) {
            trace->fate[in->value] = FRAME_PENDING;
        }
        break;

    case INPUT_WIRE_RX:
        record.value = can_ctrl_wire_rx(ctrl, &in->frame, in->time_ns) ? 1U : 0U;
        break;

    case INPUT_BURST:
        for (uint32_t i = 0U; i < in->value; i++) {
            can_frame_t frame = in->frame;

            frame.id = (frame.id + i) & CAN_ID_STD_MAX;
            frame.flags = 0U;
            frame.dlc = (uint8_t)(i % 9U);
            record.value += can_ctrl_wire_rx(ctrl, &frame, in->time_ns) ? 1U : 0U;
        }
        break;

    case INPUT_FAULT:
        can_ctrl_set_fault(ctrl, (can_bus_fault_t)in->value, in->time_ns);
        break;

    case INPUT_INJECT_TX:
        can_ctrl_inject_tx_errors(ctrl, in->value);
        break;

    case INPUT_INJECT_RX:
        can_ctrl_inject_rx_errors(ctrl, in->value);
        break;

    case INPUT_RESTART:
        can_ctrl_request_restart(ctrl, in->time_ns);
        break;

    case INPUT_ABORT:
    default:
        record.value = can_ctrl_tx_abort_all(ctrl, handles, CAN_CTRL_TX_MAILBOXES);

        for (uint32_t i = 0U; i < record.value; i++) {
            TEST_ASSERT_EQUAL_UINT8(FRAME_PENDING, trace->fate[handles[i]]);
            trace->fate[handles[i]] = FRAME_ABORTED;
            record.handle = (record.handle * 31U) + handles[i];
        }
        break;
    }

    add_record(trace, &record);
}

static void start_trace(can_ctrl_t *ctrl, trace_t *trace)
{
    (void)memset(trace, 0, sizeof(*trace));
    trace->start_ns = 0U;
    init_ctrl(ctrl, BITRATE, 0U);
}

/* Poll every microsecond; inputs are applied right after the poll at their time. */
static void drive_every_microsecond(can_ctrl_t *ctrl, trace_t *trace, uint32_t inputs)
{
    uint32_t next = 0U;

    start_trace(ctrl, trace);

    for (uint64_t t = 0U; t <= SCENARIO_NS; t += POLL_GRID_NS) {
        drain(ctrl, t, trace);

        while ((next < inputs) && (s_script[next].time_ns == t)) {
            apply_input(ctrl, &s_script[next], trace);
            next++;
        }
    }
}

/* Poll only at the model's wake-up times and at input times (once per input time, then all
 * inputs of that time). */
static void drive_at_wakeups(can_ctrl_t *ctrl, trace_t *trace, uint32_t inputs)
{
    uint32_t next = 0U;
    uint32_t polls = 0U;

    start_trace(ctrl, trace);

    while (next <= inputs) {
        const uint64_t t = (next < inputs) ? s_script[next].time_ns : SCENARIO_NS;
        uint64_t wake = can_ctrl_next_wakeup(ctrl);

        while ((wake != CAN_CTRL_NO_WAKEUP) && (wake < t)) {
            drain(ctrl, wake, trace);
            wake = can_ctrl_next_wakeup(ctrl);
            polls++;
            TEST_ASSERT_TRUE(polls < 1000000U);
        }

        drain(ctrl, t, trace);

        if (next == inputs) {
            break;
        }

        while ((next < inputs) && (s_script[next].time_ns == t)) {
            apply_input(ctrl, &s_script[next], trace);
            next++;
        }
    }
}

static void expect_same_trace(const trace_t *a, const trace_t *b, uint32_t seed)
{
    char message[64];

    (void)snprintf(message, sizeof(message), "seed %u", (unsigned int)seed);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(a->count, b->count, message);

    for (uint32_t i = 0U; i < a->count; i++) {
        const record_t *const x = &a->record[i];
        const record_t *const y = &b->record[i];

        (void)snprintf(message, sizeof(message), "seed %u, record %u", (unsigned int)seed,
                       (unsigned int)i);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(x->kind, y->kind, message);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(x->time_ns, y->time_ns, message);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(x->start_ns, y->start_ns, message);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(x->submit_ns, y->submit_ns, message);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(x->handle, y->handle, message);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(x->id, y->id, message);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(x->value, y->value, message);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(x->tec, y->tec, message);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(x->rec, y->rec, message);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(x->state, y->state, message);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(x->old_state, y->old_state, message);
    }
}

#define SCENARIOS 12U

static void
test_polling_only_at_wakeups_and_inputs_gives_the_same_actions_as_every_microsecond(void)
{
    uint64_t bus_offs = 0U;
    uint64_t sent = 0U;
    uint64_t recoveries = 0U;
    uint64_t passive = 0U;
    uint64_t delivered = 0U;
    uint64_t aborted_on_wire = 0U;
    uint64_t overruns = 0U;
    uint64_t lost = 0U;

    for (uint32_t seed = 1U; seed <= SCENARIOS; seed++) {
        const uint32_t inputs = make_script(seed * 2654435761U);

        drive_every_microsecond(&s_ctrl_a, &s_trace_a, inputs);
        drive_at_wakeups(&s_ctrl_b, &s_trace_b, inputs);
        expect_same_trace(&s_trace_a, &s_trace_b, seed);
        TEST_ASSERT_EQUAL_MEMORY(can_ctrl_stats(&s_ctrl_a), can_ctrl_stats(&s_ctrl_b),
                                 sizeof(can_ctrl_stats_t));
        TEST_ASSERT_EQUAL_UINT64(0U, can_ctrl_stats(&s_ctrl_b)->late_resyncs);

        for (uint32_t i = 0U; i < s_trace_b.count; i++) {
            const record_t *const r = &s_trace_b.record[i];

            if (r->kind == (uint32_t)CAN_CTRL_ACTION_STATE_CHANGED) {
                recoveries += (r->old_state == (uint8_t)CAN_FC_BUS_OFF) ? 1U : 0U;
                passive += (r->state == (uint8_t)CAN_FC_ERROR_PASSIVE) ? 1U : 0U;
            }

            aborted_on_wire += (r->kind == (uint32_t)CAN_CTRL_ACTION_TX_ABORTED) ? 1U : 0U;
        }

        bus_offs += can_ctrl_stats(&s_ctrl_b)->bus_off_events;
        sent += can_ctrl_stats(&s_ctrl_b)->tx_frames;
        delivered += can_ctrl_stats(&s_ctrl_b)->rx_frames;
        overruns += can_ctrl_stats(&s_ctrl_b)->wire_overruns;
        lost += can_ctrl_stats(&s_ctrl_b)->rx_lost_fault;
    }

    /* The scenarios reach the parts of the model that matter. */
    TEST_ASSERT_TRUE(bus_offs > 0U);
    TEST_ASSERT_TRUE(sent > 100U);
    TEST_ASSERT_TRUE(recoveries > 0U);
    TEST_ASSERT_TRUE(passive > 0U);
    TEST_ASSERT_TRUE(delivered > 100U);
    TEST_ASSERT_TRUE(aborted_on_wire > 0U);
    TEST_ASSERT_TRUE(overruns > 0U);
    TEST_ASSERT_TRUE(lost > 0U);
}

static void test_every_submitted_frame_is_completed_aborted_or_still_pending(void)
{
    for (uint32_t seed = 101U; seed < (101U + SCENARIOS); seed++) {
        const uint32_t inputs = make_script(seed * 2246822519U);
        uint32_t accepted = 0U;
        uint32_t done = 0U;
        uint32_t aborted = 0U;
        uint32_t pending = 0U;

        drive_at_wakeups(&s_ctrl_b, &s_trace_b, inputs);

        for (uint32_t i = 0U; i < inputs; i++) {
            const uint8_t fate = s_trace_b.fate[i];

            accepted += (fate != 0U) ? 1U : 0U;
            done += (fate == FRAME_DONE) ? 1U : 0U;
            aborted += (fate == FRAME_ABORTED) ? 1U : 0U;
            pending += (fate == FRAME_PENDING) ? 1U : 0U;

            if ((s_script[i].kind != INPUT_SUBMIT) || (s_script[i].frame.dlc > 8U)) {
                TEST_ASSERT_EQUAL_UINT8(0U, fate); /* no outcome for what was never accepted */
            }
        }

        /* drain() and apply_input() already refused a second outcome for any frame. */
        TEST_ASSERT_TRUE(accepted > 0U);
        TEST_ASSERT_EQUAL_UINT32(accepted, done + aborted + pending);
        TEST_ASSERT_EQUAL_UINT32(pending, can_ctrl_tx_pending(&s_ctrl_b));
        TEST_ASSERT_EQUAL_UINT64(done, can_ctrl_stats(&s_ctrl_b)->tx_frames);
        TEST_ASSERT_EQUAL_UINT64(aborted, can_ctrl_stats(&s_ctrl_b)->tx_aborted);
        TEST_ASSERT_TRUE(can_ctrl_stats(&s_ctrl_b)->busy_ns <= SCENARIO_NS);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_reference_frames_have_the_lengths_used_here);
    RUN_TEST(test_a_fresh_model_is_error_active_with_no_fault_and_nothing_to_do);
    RUN_TEST(test_nothing_is_placed_on_the_bus_before_the_model_was_initialised);
    RUN_TEST(test_a_frame_on_a_free_bus_completes_exactly_its_length_after_its_start);
    RUN_TEST(test_two_frames_back_to_back_are_separated_by_exactly_the_intermission);
    RUN_TEST(test_a_frame_submitted_during_a_transmission_waits_for_the_bus_and_cannot_overtake);
    RUN_TEST(test_the_smallest_arbitration_key_goes_first_whatever_the_submission_order);
    RUN_TEST(test_frames_with_equal_keys_go_in_submission_order_even_from_a_lower_mailbox);
    RUN_TEST(test_durations_are_rounded_once_per_frame_not_once_per_bit);
    RUN_TEST(test_a_frame_arriving_on_a_free_bus_is_delivered_at_its_arrival);
    RUN_TEST(test_frames_arriving_faster_than_the_bit_rate_are_delivered_at_bus_speed_in_order);
    RUN_TEST(test_a_frame_arriving_during_a_transmission_is_delivered_after_it);
    RUN_TEST(test_the_wire_queue_holds_64_frames_and_counts_those_beyond_as_overruns);
    RUN_TEST(test_a_frame_of_this_node_goes_before_a_waiting_received_frame_with_a_larger_key);
    RUN_TEST(test_a_waiting_received_frame_with_a_smaller_key_goes_before_this_node);
    RUN_TEST(test_with_equal_keys_the_waiting_received_frame_goes_first);
    RUN_TEST(test_a_received_frame_with_a_small_key_waits_behind_an_older_received_frame);
    RUN_TEST(test_the_best_mailbox_competes_and_the_frames_of_both_nodes_interleave_by_key);
    RUN_TEST(test_a_frame_handed_in_at_the_instant_the_bus_becomes_free_arbitrates);
    RUN_TEST(test_a_late_poll_moves_a_start_won_against_a_received_frame_and_that_frame_waits);
    RUN_TEST(test_a_flood_with_larger_keys_delays_this_node_by_one_frame_at_most);
    RUN_TEST(test_a_poll_one_frame_length_after_the_possible_start_is_not_late);
    RUN_TEST(test_a_later_poll_moves_the_start_to_the_poll_and_counts_a_resync);
    RUN_TEST(test_a_late_poll_after_a_failed_attempt_moves_the_retransmission);
    RUN_TEST(test_next_wakeup_is_the_last_poll_time_while_actions_wait_to_be_taken);
    RUN_TEST(test_next_wakeup_is_never_earlier_than_the_last_poll_for_a_frame_dated_before_it);
    RUN_TEST(test_a_short_takes_a_clean_node_error_passive_after_8_attempts_and_bus_off_after_24);
    RUN_TEST(test_a_node_never_acknowledged_stays_at_128_and_recovers_with_the_next_good_frame);
    RUN_TEST(test_a_bus_held_dominant_raises_the_receive_counter_on_schedule_and_blocks_sending);
    RUN_TEST(test_the_error_that_makes_a_node_error_passive_is_signalled_with_an_active_flag);
    RUN_TEST(test_injected_transmit_errors_destroy_one_attempt_each_and_the_frame_is_resent);
    RUN_TEST(test_an_injected_error_waits_while_a_short_destroys_the_attempt_first);
    RUN_TEST(test_an_injected_error_strikes_before_a_missing_acknowledgment);
    RUN_TEST(test_injected_receive_errors_destroy_one_frame_each);
    RUN_TEST(test_a_fault_set_or_removed_during_a_frame_takes_effect_from_the_next_frame);
    RUN_TEST(test_a_dominant_fault_set_during_a_reception_starts_after_it_and_drops_the_rest);
    RUN_TEST(test_a_dominant_fault_set_during_the_last_queued_frame_waits_for_its_intermission);
    RUN_TEST(test_a_frame_handed_in_is_judged_by_the_fault_last_set_before_it_takes_effect);
    RUN_TEST(test_a_received_frame_due_to_start_at_the_instant_of_a_short_never_passes);
    RUN_TEST(test_a_received_frame_placed_at_the_instant_of_a_short_never_passes);
    RUN_TEST(test_a_fault_set_in_the_intermission_after_a_received_frame_waits_for_its_end);
    RUN_TEST(test_after_a_passive_error_flag_rule_6_counts_eight_dominant_bits);
    RUN_TEST(test_a_bus_held_dominant_ends_the_suspend_of_the_last_transmitter);
    RUN_TEST(test_a_short_drops_the_received_frames_that_have_not_started);
    RUN_TEST(test_a_fault_set_while_the_model_is_held_up_spares_a_frame_that_started_before_it);
    RUN_TEST(test_a_frame_handed_in_as_a_dominant_bus_is_released_waits_for_the_release);
    RUN_TEST(test_suspend_transmission_ends_when_another_node_sends_the_last_frame);
    RUN_TEST(test_a_bus_off_node_neither_sends_nor_receives_and_keeps_its_frames_until_aborted);
    RUN_TEST(test_recovery_starts_only_on_request_and_takes_1408_bit_times_on_an_idle_bus);
    RUN_TEST(test_a_restart_requested_before_bus_off_is_not_remembered);
    RUN_TEST(test_each_frame_passing_during_recovery_counts_as_one_occurrence);
    RUN_TEST(test_a_recovery_completed_by_a_burst_of_frames_ends_at_a_start_of_frame);
    RUN_TEST(test_a_frame_arriving_just_after_a_recovery_is_placed_after_it);
    RUN_TEST(test_frames_refused_while_bus_off_are_counted_once_even_if_a_short_drops_them);
    RUN_TEST(test_a_bus_held_dominant_holds_the_recovery_until_it_is_released);
    RUN_TEST(test_a_node_on_a_bus_still_shorted_recovers_and_goes_bus_off_again);
    RUN_TEST(test_a_frame_that_ends_after_the_node_went_bus_off_is_lost);
    RUN_TEST(
        test_abort_removes_the_waiting_frames_in_submission_order_and_spares_the_one_on_the_wire);
    RUN_TEST(test_an_aborted_frame_whose_attempt_fails_is_removed_after_the_error_counting);
    RUN_TEST(test_abort_writes_no_more_handles_than_asked_and_needs_no_buffer);
    RUN_TEST(test_invalid_frames_are_refused_by_both_inputs_and_counted_nowhere);
    RUN_TEST(test_a_ninth_frame_is_refused_while_eight_mailboxes_are_in_use);
    RUN_TEST(test_a_full_action_queue_stops_the_model_without_losing_an_action);
    RUN_TEST(test_a_frame_submitted_while_the_model_is_held_up_competes_from_its_submission);
    RUN_TEST(test_a_frame_handed_in_while_the_model_is_held_up_goes_behind_the_waiting_ones);
    RUN_TEST(test_a_start_moved_by_a_late_poll_is_arbitrated_among_the_frames_there_by_then);
    RUN_TEST(test_injected_error_counts_saturate_instead_of_wrapping);
    RUN_TEST(test_an_input_dated_before_what_was_processed_does_not_go_back_in_time);
    RUN_TEST(test_with_pacing_off_frames_complete_at_the_poll_that_lets_them_start);
    RUN_TEST(test_with_pacing_off_a_node_never_acknowledged_does_not_hang_the_poll);
    RUN_TEST(test_with_pacing_off_bus_off_recovery_and_a_dominant_bus_take_no_time);
    RUN_TEST(test_polling_only_at_wakeups_and_inputs_gives_the_same_actions_as_every_microsecond);
    RUN_TEST(test_every_submitted_frame_is_completed_aborted_or_still_pending);
    return UNITY_END();
}
