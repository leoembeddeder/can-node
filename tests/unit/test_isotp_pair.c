/* SPDX-License-Identifier: MIT */
/*
 * lib/isotp, two instances over one simulated bus (isotp_test_bus.h): the tester (a, sends
 * 0x7E0) and the node (b, sends 0x7E8). Every frame on the bus is judged by the wire checker,
 * which applies the rules of ISO 15765-2 as docs/diagnostics.md states them (PCI layout,
 * padding, SN order, BS, STmin between any two CFs) and compares every message it puts together
 * with the one that was sent.
 *
 *   - Every message length from 0 to 4095, sent by one instance and received by the other, for
 *     several pairs of BS and STmin of the receiver, in both directions. Length 0 is refused by
 *     the sender and puts nothing on the bus (SF_DL 0 is reserved: no encoding).
 *   - Both directions at once (full duplex).
 *   - Confirmations that come after the peer's answer (the order the node's tasks can give).
 *   - Random runs with lost and repeated frames, kept buffers and random parameters, after
 *     which the accounting identities hold and no message was delivered damaged.
 *
 * Two executables share this file so that each stays well inside CTest's timeout in the
 * coverage build: ISOTP_PAIR_PART 0 (test_isotp_pair_lengths) is three of the every-length
 * runs, part 1 (test_isotp_pair) is the rest.
 *
 * A frame takes FRAME_US on the bus: the longest 8-byte base-format data frame, 132 bits with the
 * worst case of stuffing, plus the 3-bit intermission, 135 bits, is 270 us at 500 kbit/s
 * (fcn_tools.canbits.worst_case_bits). The clock is the fake one; a run takes fake time.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "isotp/isotp.h"
#include "isotp_test_bus.h"
#include "unity.h"

#define FRAME_US  270U
#define TESTER_TX 0x7E0U
#define NODE_TX   0x7E8U

/* The longest a single transfer may take in fake time: 585 CFs at the longest STmin (127 ms)
 * are 75 s, plus the FCs. */
#define TRANSFER_LIMIT_US 200000000U

static tb_end_t s_a; /* the tester */
static tb_end_t s_b; /* the node */
static tb_checker_t s_checker;
static tb_link_t s_link;
static uint8_t s_payload[2][TB_MAX_LENGTH];

/* xorshift32: the same "random" numbers on every run. */
static uint32_t s_random;

#ifndef ISOTP_PAIR_PART
#error "ISOTP_PAIR_PART selects the part of the tests: see tests/unit/CMakeLists.txt"
#endif

#if ISOTP_PAIR_PART == 1
static uint32_t random_u32(void)
{
    s_random ^= s_random << 13U;
    s_random ^= s_random >> 17U;
    s_random ^= s_random << 5U;
    return s_random;
}

static uint32_t random_below(uint32_t bound)
{
    return random_u32() % bound;
}
#endif

void setUp(void)
{
    tb_now_us = 1000000U;
    s_random = 0x150715U; /* read by the random runs of part 1 */
    for (uint32_t i = 0U; i < TB_MAX_LENGTH; i++) {
        s_payload[0][i] = (uint8_t)((i * 13U) + 1U);
        s_payload[1][i] = (uint8_t)((i * 29U) ^ 0x5AU);
    }
    s_link = (tb_link_t){
        .frame_us = FRAME_US, .monitor = tb_checker_monitor, .monitor_context = &s_checker};
}

void tearDown(void)
{
}

static const char *at_value(const char *what, unsigned long value)
{
    static char text[80];

    (void)snprintf(text, sizeof(text), "%s %lu", what, value);
    return text;
}

/* The tester and the node; each one's receive parameters are its BS and STmin. */
static void pair_start(uint8_t a_bs, uint8_t a_stmin, uint8_t b_bs, uint8_t b_stmin)
{
    isotp_config_t a = tb_config(TESTER_TX, NODE_TX);
    isotp_config_t b = tb_config(NODE_TX, TESTER_TX);

    a.rx_bs = a_bs;
    a.rx_stmin = a_stmin;
    b.rx_bs = b_bs;
    b.rx_stmin = b_stmin;
    tb_start(&s_a, &a, ISOTP_FF_DL_12BIT_MAX);
    tb_start(&s_b, &b, ISOTP_FF_DL_12BIT_MAX);
    tb_checker_init(&s_checker, TESTER_TX, NODE_TX);
}

/* One message of `length` bytes from `from` (0: the tester, 1: the node) to the other, and the
 * bus run until it settles. */
static void transfer(uint32_t from, uint32_t length)
{
    tb_end_t *sender = (from == 0U) ? &s_a : &s_b;
    const tb_end_t *receiver = (from == 0U) ? &s_b : &s_a;
    const uint32_t messages = receiver->messages;
    const uint32_t confirmations = sender->conf_count;
    const uint32_t frames = s_checker.frames;

    s_checker.expected[from] = s_payload[from];
    s_checker.expected_length[from] = length;
    if (length == 0U) {
        TEST_ASSERT_EQUAL(ISOTP_E_ARG, isotp_send(&sender->tp, s_payload[from], length, tb_now_us));
        TEST_ASSERT_TRUE(tb_run(&s_a, &s_b, &s_link, tb_now_us + TRANSFER_LIMIT_US));
        TEST_ASSERT_EQUAL_UINT32(frames, s_checker.frames);
        TEST_ASSERT_EQUAL_UINT32(messages, receiver->messages);
    } else {
        TEST_ASSERT_EQUAL(ISOTP_OK, isotp_send(&sender->tp, s_payload[from], length, tb_now_us));
        TEST_ASSERT_TRUE_MESSAGE(tb_run(&s_a, &s_b, &s_link, tb_now_us + TRANSFER_LIMIT_US),
                                 at_value("did not settle, length", length));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(confirmations + 1U, sender->conf_count,
                                         at_value("length", length));
        TEST_ASSERT_EQUAL(ISOTP_RESULT_OK, tb_last_conf(sender).result);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(messages + 1U, receiver->messages,
                                         at_value("length", length));
        TEST_ASSERT_EQUAL_UINT32(length, receiver->message_length);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(s_payload[from], receiver->message, length);
    }
    tb_check(&s_a);
    tb_check(&s_b);
}

/* Every length from 0 to 4095 in one direction with the receiver's BS and STmin. */
static void every_length(uint32_t from, uint8_t bs, uint8_t stmin)
{
    if (from == 0U) {
        pair_start(0U, 0U, bs, stmin);
    } else {
        pair_start(bs, stmin, 0U, 0U);
    }
    for (uint32_t length = 0U; length <= ISOTP_FF_DL_12BIT_MAX; length++) {
        transfer(from, length);
    }
    TEST_ASSERT_EQUAL_UINT32(ISOTP_FF_DL_12BIT_MAX, s_checker.messages[from]);
    TEST_ASSERT_EQUAL_UINT32(0U, s_checker.violations);
}

/* ------------------------------------------------------------------------------------------
 * Every length
 * ---------------------------------------------------------------------------------------- */

#if ISOTP_PAIR_PART == 0

static void test_every_length_to_the_node_with_bs_0_and_stmin_0(void)
{
    every_length(0U, 0U, 0x00U);
}

static void test_every_length_to_the_node_with_bs_1_and_stmin_100_us(void)
{
    every_length(0U, 1U, 0xF1U);
}

static void test_every_length_to_the_node_with_bs_8_and_stmin_1_ms(void)
{
    every_length(0U, 8U, 0x01U);
}

#else /* ISOTP_PAIR_PART 1 */

static void test_every_length_to_the_node_with_bs_255_and_stmin_900_us(void)
{
    every_length(0U, 255U, 0xF9U);
}

static void test_every_length_from_the_node_with_bs_3_and_stmin_500_us(void)
{
    every_length(1U, 3U, 0xF5U);
}

/* The longest STmin and a block of 15, with the lengths around every block boundary. */
static void test_block_boundaries_with_the_longest_stmin(void)
{
    pair_start(0U, 0U, 15U, 0x7FU);
    for (uint32_t block = 1U; block <= 4U; block++) {
        const uint32_t end_of_block = 6U + (block * 15U * 7U);
        for (uint32_t length = end_of_block - 1U; length <= end_of_block + 1U; length++) {
            transfer(0U, (uint16_t)length);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(12U, s_checker.messages[0]);
    TEST_ASSERT_EQUAL_UINT32(0U, s_b.tp.stats.stmin_reserved);
}

/* ------------------------------------------------------------------------------------------
 * Both directions at once, and late confirmations
 * ---------------------------------------------------------------------------------------- */

static void test_both_directions_at_once(void)
{
    const uint32_t lengths[] = {1U, 7U, 8U, 100U, 1000U, ISOTP_FF_DL_12BIT_MAX};

    pair_start(2U, 0xF3U, 8U, 0x00U);
    for (uint32_t i = 0U; i < (sizeof(lengths) / sizeof(lengths[0])); i++) {
        for (uint32_t j = 0U; j < (sizeof(lengths) / sizeof(lengths[0])); j++) {
            s_checker.expected[0] = s_payload[0];
            s_checker.expected_length[0] = lengths[i];
            s_checker.expected[1] = s_payload[1];
            s_checker.expected_length[1] = lengths[j];
            TEST_ASSERT_EQUAL(ISOTP_OK, isotp_send(&s_a.tp, s_payload[0], lengths[i], tb_now_us));
            TEST_ASSERT_EQUAL(ISOTP_OK, isotp_send(&s_b.tp, s_payload[1], lengths[j], tb_now_us));
            TEST_ASSERT_TRUE(tb_run(&s_a, &s_b, &s_link, tb_now_us + TRANSFER_LIMIT_US));
            TEST_ASSERT_EQUAL(ISOTP_RESULT_OK, tb_last_conf(&s_a).result);
            TEST_ASSERT_EQUAL(ISOTP_RESULT_OK, tb_last_conf(&s_b).result);
            TEST_ASSERT_EQUAL_UINT32(lengths[i], s_b.message_length);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(s_payload[0], s_b.message, lengths[i]);
            TEST_ASSERT_EQUAL_UINT32(lengths[j], s_a.message_length);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(s_payload[1], s_a.message, lengths[j]);
            tb_check(&s_a);
            tb_check(&s_b);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(36U, s_checker.messages[0]);
    TEST_ASSERT_EQUAL_UINT32(36U, s_checker.messages[1]);
}

/*
 * The sender's confirmation comes after the receiver has seen the frame (lag 0, answer first),
 * or so late that the answer to it arrives first (lag of three frames): the FC before the FF's
 * or the last CF's confirmation, the CF before the CTS's. Nothing may be lost or repeated, and
 * STmin must still hold on the wire.
 */
static void test_confirmations_after_the_answer(void)
{
    const uint32_t lags[] = {0U, FRAME_US / 2U, 3U * FRAME_US, 20U * FRAME_US};
    const uint32_t lengths[] = {1U, 7U, 8U, 13U, 14U, 20U, 111U, 112U, 500U, ISOTP_FF_DL_12BIT_MAX};

    for (uint32_t l = 0U; l < (sizeof(lags) / sizeof(lags[0])); l++) {
        pair_start(1U, 0xF2U, 2U, 0x02U);
        s_link.answer_first = true;
        s_link.confirm_lag_us = lags[l];
        for (uint32_t i = 0U; i < (sizeof(lengths) / sizeof(lengths[0])); i++) {
            transfer(0U, lengths[i]);
            transfer(1U, lengths[i]);
        }
        /* With a lag shorter than a frame every confirmation comes before the answer to it;
         * with a longer one the answers prove the frames: the FF and the last CF of each block
         * at the sender, the CTS at the receiver. Either way every frame is confirmed. */
        if (lags[l] < FRAME_US) {
            TEST_ASSERT_EQUAL_UINT32(0U, s_a.tp.stats.proven + s_b.tp.stats.proven);
        } else {
            TEST_ASSERT_TRUE_MESSAGE(s_a.tp.stats.proven > 100U, at_value("lag", lags[l]));
            TEST_ASSERT_TRUE_MESSAGE(s_b.tp.stats.proven > 100U, at_value("lag", lags[l]));
        }
        TEST_ASSERT_EQUAL_UINT32(0U, s_a.tp.stats.unconfirmed + s_b.tp.stats.unconfirmed);
        TEST_ASSERT_EQUAL_UINT32(0U,
                                 s_a.tp.stats.confirm_unmatched + s_b.tp.stats.confirm_unmatched);
    }
}

/* ------------------------------------------------------------------------------------------
 * Random runs with faults
 * ---------------------------------------------------------------------------------------- */

typedef struct {
    uint32_t drop_per_mille;
    uint32_t duplicate_per_mille;
    uint32_t release_per_mille;
} faults_t;

static faults_t s_faults;

static tb_fate_t random_fate(void *context, const tb_end_t *from, const can_frame_t *frame)
{
    const uint32_t roll = random_below(1000U);
    tb_fate_t fate = TB_DELIVER;

    (void)context;
    (void)from;
    (void)frame;
    if (roll < s_faults.drop_per_mille) {
        fate = TB_DROP;
    } else if (roll < (s_faults.drop_per_mille + s_faults.duplicate_per_mille)) {
        fate = TB_DUPLICATE;
    }
    return fate;
}

static void random_release(void *context)
{
    (void)context;
    if ((random_below(1000U) < s_faults.release_per_mille) && isotp_rx_held(&s_b.tp)) {
        TEST_ASSERT_TRUE(isotp_rx_release(&s_b.tp, tb_now_us));
    }
}

static uint8_t random_stmin(void)
{
    const uint8_t choices[] = {0x00U, 0x00U, 0x01U, 0x05U, 0xF1U, 0xF5U, 0xF9U};

    return choices[random_below(sizeof(choices))];
}

static uint32_t random_length(void)
{
    const uint32_t kind = random_below(4U);
    uint32_t length;

    if (kind == 0U) {
        length = 1U + random_below(7U);
    } else if (kind == 1U) {
        length = 8U + random_below(100U);
    } else {
        length = 1U + random_below(ISOTP_FF_DL_12BIT_MAX);
    }
    return length;
}

/* A delivered message must be the one that was sent: no fault may damage a message that is
 * delivered (ISO-TP has no checksum, but a lost or repeated CF breaks the SN order). */
static void expect_intact(const tb_end_t *receiver, const uint8_t *sent, uint32_t delivered)
{
    if (receiver->messages != delivered) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY(sent, receiver->message, receiver->message_length);
    }
}

static void test_random_runs_with_lost_and_repeated_frames(void)
{
    uint32_t results[ISOTP_RESULTS] = {0U};
    uint32_t sends = 0U;

    for (uint32_t round = 0U; round < 600U; round++) {
        isotp_config_t a = tb_config(TESTER_TX, NODE_TX);
        isotp_config_t b = tb_config(NODE_TX, TESTER_TX);

        a.rx_bs = (uint8_t)random_below(5U);
        a.rx_stmin = random_stmin();
        b.rx_bs = (random_below(3U) == 0U) ? 0U : (uint8_t)(1U + random_below(16U));
        b.rx_stmin = random_stmin();
        b.rx_wft_max = (uint8_t)random_below(4U);
        a.tx_wft_max = (uint8_t)random_below(4U);
        b.n_br_us = 20000U + random_below(100000U);
        tb_start(&s_a, &a, ISOTP_FF_DL_12BIT_MAX);
        tb_start(&s_b, &b, 64U + random_below(ISOTP_FF_DL_12BIT_MAX - 64U));
        s_link = (tb_link_t){.frame_us = FRAME_US,
                             .confirm_lag_us = random_below(4U) * FRAME_US,
                             .answer_first = random_below(2U) == 0U,
                             .fate = random_fate,
                             .tick = random_release};
        s_faults.drop_per_mille = random_below(3U) * 10U;
        s_faults.duplicate_per_mille = random_below(3U) * 10U;
        s_faults.release_per_mille = 5U + random_below(50U);

        for (uint32_t message = 0U; message < 4U; message++) {
            const uint32_t delivered_a = s_a.messages;
            const uint32_t delivered_b = s_b.messages;
            const uint32_t conf_a = s_a.conf_count;
            const uint32_t conf_b = s_b.conf_count;
            const bool a_sends = random_below(4U) != 0U;
            const bool b_sends = random_below(3U) == 0U;

            s_b.keep = random_below(4U) == 0U;
            if (a_sends) {
                TEST_ASSERT_EQUAL(ISOTP_OK,
                                  isotp_send(&s_a.tp, s_payload[0], random_length(), tb_now_us));
                sends++;
            }
            if (b_sends) {
                TEST_ASSERT_EQUAL(ISOTP_OK,
                                  isotp_send(&s_b.tp, s_payload[1], random_length(), tb_now_us));
                sends++;
            }
            TEST_ASSERT_TRUE_MESSAGE(tb_run(&s_a, &s_b, &s_link, tb_now_us + TRANSFER_LIMIT_US),
                                     at_value("round", round));
            /* Settled: nothing in progress any more, every request has its confirmation. */
            TEST_ASSERT_FALSE(isotp_tx_busy(&s_a.tp));
            TEST_ASSERT_FALSE(isotp_tx_busy(&s_b.tp));
            TEST_ASSERT_FALSE(isotp_rx_busy(&s_a.tp));
            TEST_ASSERT_FALSE(isotp_rx_busy(&s_b.tp));
            TEST_ASSERT_EQUAL_UINT32(conf_a + (a_sends ? 1U : 0U), s_a.conf_count);
            TEST_ASSERT_EQUAL_UINT32(conf_b + (b_sends ? 1U : 0U), s_b.conf_count);
            TEST_ASSERT_EQUAL_UINT32(0U, s_a.tp.stats.outstanding);
            TEST_ASSERT_EQUAL_UINT32(0U, s_b.tp.stats.outstanding);
            expect_intact(&s_b, s_payload[0], delivered_b);
            expect_intact(&s_a, s_payload[1], delivered_a);
            tb_check(&s_a);
            tb_check(&s_b);
            /* Half the time a kept buffer stays kept, so that the next FF meets it. */
            if (isotp_rx_held(&s_b.tp) && (random_below(2U) == 0U)) {
                TEST_ASSERT_TRUE(isotp_rx_release(&s_b.tp, tb_now_us));
            }
        }
        for (uint32_t r = 0U; r < ISOTP_RESULTS; r++) {
            results[r] += s_a.tp.stats.tx_results[r] + s_b.tp.stats.tx_results[r] +
                          s_a.tp.stats.rx_results[r] + s_b.tp.stats.rx_results[r];
        }
    }
    /* The runs reached the outcomes they are meant to exercise. */
    TEST_ASSERT_TRUE(sends > 1000U);
    TEST_ASSERT_TRUE(results[ISOTP_RESULT_OK] > 1000U);
    TEST_ASSERT_TRUE(results[ISOTP_RESULT_TIMEOUT_A] > 0U);
    TEST_ASSERT_TRUE(results[ISOTP_RESULT_TIMEOUT_BS] > 0U);
    TEST_ASSERT_TRUE(results[ISOTP_RESULT_TIMEOUT_CR] > 0U);
    TEST_ASSERT_TRUE(results[ISOTP_RESULT_WRONG_SN] > 0U);
    TEST_ASSERT_TRUE(results[ISOTP_RESULT_UNEXP_PDU] > 0U);
    TEST_ASSERT_TRUE(results[ISOTP_RESULT_WFT_OVRN] > 0U);
    TEST_ASSERT_TRUE(results[ISOTP_RESULT_BUFFER_OVFLW] > 0U);
}

static void test_canfd_pair_all_tx_dl_and_escape_ff(void)
{
    static const uint8_t tx_dls[] = {8U, 12U, 16U, 20U, 24U, 32U, 48U, 64U};
    static const uint32_t extra_lengths[] = {1U,  7U,   8U,    9U,    10U,   11U,   14U,  15U,
                                             18U, 22U,  30U,   31U,   46U,   47U,   62U,  63U,
                                             64U, 127U, 1000U, 4095U, 4096U, 5000U, 8192U};

    for (size_t d = 0U; d < (sizeof(tx_dls) / sizeof(tx_dls[0])); d++) {
        isotp_config_t a = tb_config(TESTER_TX, NODE_TX);
        isotp_config_t b = tb_config(NODE_TX, TESTER_TX);

        a.can_fd = true;
        a.can_fd_brs = true;
        a.tx_dl = tx_dls[d];
        a.rx_bs = 4U;
        a.rx_stmin = 0xF2U;

        b.can_fd = true;
        b.can_fd_brs = true;
        b.tx_dl = tx_dls[d];
        b.rx_bs = 3U;
        b.rx_stmin = 0x01U;

        tb_start(&s_a, &a, TB_MAX_LENGTH);
        tb_start(&s_b, &b, TB_MAX_LENGTH);
        tb_checker_init_fd(&s_checker, TESTER_TX, NODE_TX, a.tx_dl, b.tx_dl, true);

        for (size_t i = 0U; i < (sizeof(extra_lengths) / sizeof(extra_lengths[0])); i++) {
            transfer(0U, extra_lengths[i]);
            transfer(1U, extra_lengths[i]);
        }
        TEST_ASSERT_EQUAL_UINT32(0U, s_checker.violations);
    }
}

#endif /* ISOTP_PAIR_PART */

int main(void)
{
    UNITY_BEGIN();
#if ISOTP_PAIR_PART == 0
    RUN_TEST(test_every_length_to_the_node_with_bs_0_and_stmin_0);
    RUN_TEST(test_every_length_to_the_node_with_bs_1_and_stmin_100_us);
    RUN_TEST(test_every_length_to_the_node_with_bs_8_and_stmin_1_ms);
#else
    RUN_TEST(test_every_length_to_the_node_with_bs_255_and_stmin_900_us);
    RUN_TEST(test_every_length_from_the_node_with_bs_3_and_stmin_500_us);
    RUN_TEST(test_block_boundaries_with_the_longest_stmin);
    RUN_TEST(test_both_directions_at_once);
    RUN_TEST(test_confirmations_after_the_answer);
    RUN_TEST(test_random_runs_with_lost_and_repeated_frames);
    RUN_TEST(test_canfd_pair_all_tx_dl_and_escape_ff);
#endif
    return UNITY_END();
}
