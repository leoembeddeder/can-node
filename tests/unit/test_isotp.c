/* SPDX-License-Identifier: MIT */
/*
 * lib/isotp, one instance at a time, part 1 in the order of its header: initialisation and
 * refused input; the frames it sends, byte by byte; the checks of received frames; the table of
 * unexpected N_PDUs, row by row. Part 2 (test_isotp_flow.c) has the timers, flow control, the
 * confirmations, the callbacks and the statistics.
 *
 * The instance under test is the node: it sends 0x7E8 and accepts 0x7E0. The test is its peer
 * (isotp_test_node.h): it takes the frames the node hands out (tb_take), confirms them
 * (tb_confirm) and injects the peer's frames (tb_inject). The clock is tb_now_us, set by the
 * test; it starts at 1 s so that "just before" exists for every timer. After every step that
 * matters, tb_check() asserts the accounting identities of isotp.h and that the next deadline
 * lies after the time of the call.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "isotp/isotp.h"
#include "isotp_test_bus.h"
#include "isotp_test_node.h"
#include "unity.h"

void setUp(void)
{
    node_setup();
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------------------------
 * Initialisation and refused input
 * ---------------------------------------------------------------------------------------- */

static bool dummy_send(void *context, const can_frame_t *frame)
{
    (void)context;
    (void)frame;
    return true;
}

static bool dummy_indication(void *context, isotp_result_t result, const uint8_t *data,
                             uint32_t length)
{
    (void)context;
    (void)result;
    (void)data;
    (void)length;
    return false;
}

static isotp_t s_raw;
static uint8_t s_raw_buffer[5000];

/* isotp_init() with one change from a valid call; the instance must stay as it was. */
static bool init_with(const isotp_config_t *config, const isotp_callbacks_t *callbacks,
                      uint8_t *buffer, uint32_t size)
{
    static isotp_t before;
    bool ok;

    (void)memset(&s_raw, 0x5A, sizeof(s_raw));
    before = s_raw;
    ok = isotp_init(&s_raw, config, callbacks, buffer, size);
    if (!ok) {
        TEST_ASSERT_EQUAL_MEMORY(&before, &s_raw, sizeof(s_raw));
    }
    return ok;
}

static void test_init_refuses_every_bad_argument(void)
{
    const isotp_callbacks_t good = {.send_frame = dummy_send, .indication = dummy_indication};
    isotp_callbacks_t callbacks = good;
    isotp_config_t config = s_cfg;
    const uint8_t reserved_stmin[] = {0x80U, 0xA0U, 0xF0U, 0xFAU, 0xFFU};

    TEST_ASSERT_TRUE(init_with(&config, &good, s_raw_buffer, 100U));
    TEST_ASSERT_FALSE(isotp_init(NULL, &config, &good, s_raw_buffer, 100U));
    TEST_ASSERT_FALSE(init_with(NULL, &good, s_raw_buffer, 100U));
    TEST_ASSERT_FALSE(init_with(&config, NULL, s_raw_buffer, 100U));
    TEST_ASSERT_FALSE(init_with(&config, &good, NULL, 100U));
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 0U));
    TEST_ASSERT_TRUE(init_with(&config, &good, s_raw_buffer, 1U));

    callbacks.send_frame = NULL;
    TEST_ASSERT_FALSE(init_with(&config, &callbacks, s_raw_buffer, 100U));
    callbacks = good;
    callbacks.indication = NULL;
    TEST_ASSERT_FALSE(init_with(&config, &callbacks, s_raw_buffer, 100U));

    config.tx_id = 0x800U;
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 100U));
    config.tx_id = 0x7FFU;
    TEST_ASSERT_TRUE(init_with(&config, &good, s_raw_buffer, 100U));
    config = s_cfg;
    config.rx_id = 0x800U;
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 100U));
    config.rx_id = config.tx_id;
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 100U));

    config = s_cfg;
    config.extended_ids = true;
    config.tx_id = 0x20000000U;
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 100U));
    config.tx_id = 0x1FFFFFFFU;
    TEST_ASSERT_TRUE(init_with(&config, &good, s_raw_buffer, 100U));
    config.rx_id = 0x20000000U;
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 100U));

    config = s_cfg;
    config.n_as_us = 0U;
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 100U));
    config = s_cfg;
    config.n_ar_us = 0U;
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 100U));
    config = s_cfg;
    config.n_bs_us = 0U;
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 100U));
    config = s_cfg;
    config.n_cr_us = 0U;
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 100U));
    config = s_cfg;
    config.n_br_us = 0U; /* allowed: no wait for a kept buffer */
    config.n_cs_us = 0U; /* allowed: a target only */
    TEST_ASSERT_TRUE(init_with(&config, &good, s_raw_buffer, 100U));

    for (uint32_t i = 0U; i < sizeof(reserved_stmin); i++) {
        config = s_cfg;
        config.rx_stmin = reserved_stmin[i];
        TEST_ASSERT_FALSE_MESSAGE(init_with(&config, &good, s_raw_buffer, 100U),
                                  at_value("reserved STmin", reserved_stmin[i]));
    }
    config.rx_stmin = 0xF9U;
    TEST_ASSERT_TRUE(init_with(&config, &good, s_raw_buffer, 100U));
    config.rx_stmin = 0x7FU;
    TEST_ASSERT_TRUE(init_with(&config, &good, s_raw_buffer, 100U));

    /* CAN FD config validation: tx_dl defaults and legal discrete lengths. */
    config = s_cfg;
    config.can_fd = false;
    config.can_fd_brs = true;
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 100U));
    config = s_cfg;
    config.can_fd = false;
    config.tx_dl = 12U;
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 100U));
    config = s_cfg;
    config.can_fd = true;
    config.tx_dl = 9U;
    TEST_ASSERT_FALSE(init_with(&config, &good, s_raw_buffer, 100U));
    config = s_cfg;
    config.can_fd = true;
    config.can_fd_brs = true;
    config.tx_dl = 0U; /* defaults to 64 */
    TEST_ASSERT_TRUE(init_with(&config, &good, s_raw_buffer, 100U));
    TEST_ASSERT_EQUAL_UINT8(64U, s_raw.config.tx_dl);
    config.tx_dl = 32U;
    TEST_ASSERT_TRUE(init_with(&config, &good, s_raw_buffer, 100U));
}

/* A zeroed instance was never initialised: every call refuses it. NULL likewise. */
static void test_an_uninitialised_instance_is_refused(void)
{
    const uint8_t byte = 1U;
    const can_frame_t frame = {.id = TB_NODE_RX, .dlc = 2U, .data = {0x01U, 0x55U}};

    (void)memset(&s_raw, 0, sizeof(s_raw));
    TEST_ASSERT_EQUAL(ISOTP_E_ARG, isotp_send(&s_raw, &byte, 1U, T0));
    TEST_ASSERT_FALSE(isotp_on_frame(&s_raw, &frame, T0));
    TEST_ASSERT_FALSE(isotp_on_tx_done(&s_raw, T0));
    isotp_poll(&s_raw, T0);
    TEST_ASSERT_FALSE(isotp_rx_release(&s_raw, T0));
    TEST_ASSERT_EQUAL_UINT64(ISOTP_NO_DEADLINE, isotp_next_deadline(&s_raw));
    TEST_ASSERT_FALSE(isotp_tx_busy(&s_raw));
    TEST_ASSERT_FALSE(isotp_rx_busy(&s_raw));
    TEST_ASSERT_FALSE(isotp_rx_held(&s_raw));
    TEST_ASSERT_EQUAL_UINT32(0U, isotp_stats(&s_raw)->confirm_unmatched);

    TEST_ASSERT_EQUAL(ISOTP_E_ARG, isotp_send(NULL, &byte, 1U, T0));
    TEST_ASSERT_FALSE(isotp_on_frame(NULL, &frame, T0));
    TEST_ASSERT_FALSE(isotp_on_tx_done(NULL, T0));
    isotp_poll(NULL, T0);
    TEST_ASSERT_FALSE(isotp_rx_release(NULL, T0));
    TEST_ASSERT_EQUAL_UINT64(ISOTP_NO_DEADLINE, isotp_next_deadline(NULL));
    TEST_ASSERT_FALSE(isotp_tx_busy(NULL));
    TEST_ASSERT_FALSE(isotp_rx_busy(NULL));
    TEST_ASSERT_FALSE(isotp_rx_held(NULL));
    TEST_ASSERT_NULL(isotp_stats(NULL));

    start();
    TEST_ASSERT_FALSE(isotp_on_frame(&s_node.tp, NULL, T0));
    tb_check(&s_node);
}

/* Length 0 has no encoding (SF_DL 0 is reserved): refused, like a NULL pointer. Nothing goes
 * out and nothing is counted. */
static void test_send_refuses_length_0_and_null(void)
{
    start();
    TEST_ASSERT_EQUAL(ISOTP_E_ARG, isotp_send(&s_node.tp, s_data, 0U, tb_now_us));
    TEST_ASSERT_EQUAL(ISOTP_E_ARG, isotp_send(&s_node.tp, NULL, 5U, tb_now_us));
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->tx_requests);
    TEST_ASSERT_FALSE(isotp_tx_busy(&s_node.tp));
    tb_check(&s_node);
}

/* One message at a time: a second request while one is in progress is refused and changes
 * nothing; after the confirmation the next one is taken. */
static void test_send_refuses_a_second_message_while_one_is_sent(void)
{
    start();
    send(20U);
    TEST_ASSERT_TRUE(isotp_tx_busy(&s_node.tp));
    TEST_ASSERT_EQUAL(ISOTP_E_BUSY, isotp_send(&s_node.tp, s_data, 3U, tb_now_us));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->tx_requests);
    TEST_ASSERT_EQUAL_UINT32(1U, tb_waiting(&s_node));
    (void)tb_take_confirm(&s_node);
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    expect_cf(20U, 1U);
    expect_cf(20U, 2U);
    expect_conf(ISOTP_RESULT_OK);
    TEST_ASSERT_FALSE(isotp_tx_busy(&s_node.tp));
    send(3U);
    TEST_ASSERT_EQUAL_UINT32(2U, stats()->tx_requests);
    tb_check(&s_node);
}

/* A buffer above 4095 bytes receives a 32-bit escape FF message without touching guard bytes. */
static uint32_t s_raw_messages;
static uint32_t s_raw_last_length;

static bool raw_indication(void *context, isotp_result_t result, const uint8_t *data,
                           uint32_t length)
{
    (void)context;
    (void)data;
    if (result == ISOTP_RESULT_OK) {
        s_raw_messages++;
        s_raw_last_length = length;
    }
    return false;
}

static void test_a_buffer_above_4095_bytes_receives_escape_ff(void)
{
    const isotp_callbacks_t callbacks = {.send_frame = dummy_send, .indication = raw_indication};
    const uint8_t ff[8] = {0x10U, 0x00U, 0x00U, 0x00U, 0x10U, 0x00U, 1U, 2U};
    can_frame_t frame = {.id = TB_NODE_RX, .dlc = 8U};

    s_raw_messages = 0U;
    s_raw_last_length = 0U;
    (void)memset(s_raw_buffer, 0xA5, sizeof(s_raw_buffer));
    TEST_ASSERT_TRUE(isotp_init(&s_raw, &s_cfg, &callbacks, s_raw_buffer, 5000U));
    TEST_ASSERT_EQUAL_UINT32(5000U, s_raw.buffer_size);
    (void)memcpy(frame.data, ff, 8U);
    TEST_ASSERT_TRUE(isotp_on_frame(&s_raw, &frame, T0));
    TEST_ASSERT_TRUE(isotp_on_tx_done(&s_raw, T0));
    for (uint32_t i = 1U; i <= 585U; i++) {
        frame.data[0] = (uint8_t)(0x20U | (i & 0x0FU));
        (void)memset(&frame.data[1], 0x77, 7U);
        TEST_ASSERT_TRUE(isotp_on_frame(&s_raw, &frame, T0 + i));
    }
    TEST_ASSERT_EQUAL_UINT32(1U, s_raw_messages);
    TEST_ASSERT_EQUAL_UINT32(4096U, s_raw_last_length);
    for (uint32_t i = 4096U; i < sizeof(s_raw_buffer); i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xA5U, s_raw_buffer[i], "a byte beyond 4096 was written");
    }
}

/* ff_indication and confirm may be NULL: a message goes both ways without them. */
static void test_the_optional_callbacks_may_be_null(void)
{
    const isotp_callbacks_t callbacks = {.send_frame = dummy_send, .indication = raw_indication};
    const uint8_t ff[8] = {0x1FU, 0xFFU, 1U, 2U, 3U, 4U, 5U, 6U};
    const uint8_t cts[8] = {0x30U, 0U, 0U, CC, CC, CC, CC, CC};
    can_frame_t frame = {.id = TB_NODE_RX, .dlc = 8U};

    s_raw_messages = 0U;
    TEST_ASSERT_TRUE(isotp_init(&s_raw, &s_cfg, &callbacks, s_raw_buffer, 4095U));
    TEST_ASSERT_EQUAL(ISOTP_OK, isotp_send(&s_raw, s_data, 3U, T0));
    TEST_ASSERT_TRUE(isotp_on_tx_done(&s_raw, T0));
    TEST_ASSERT_FALSE(isotp_tx_busy(&s_raw));
    TEST_ASSERT_EQUAL_UINT32(1U, s_raw.stats.tx_results[ISOTP_RESULT_OK]);
    TEST_ASSERT_EQUAL(ISOTP_OK, isotp_send(&s_raw, s_data, 9U, T0));
    TEST_ASSERT_TRUE(isotp_on_tx_done(&s_raw, T0));
    (void)memcpy(frame.data, cts, 8U);
    TEST_ASSERT_TRUE(isotp_on_frame(&s_raw, &frame, T0));
    TEST_ASSERT_TRUE(isotp_on_tx_done(&s_raw, T0));
    TEST_ASSERT_EQUAL_UINT32(2U, s_raw.stats.tx_results[ISOTP_RESULT_OK]);
    (void)memcpy(frame.data, ff, 8U);
    TEST_ASSERT_TRUE(isotp_on_frame(&s_raw, &frame, T0));
    TEST_ASSERT_EQUAL_UINT32(1U, s_raw.stats.ff_indications);
    TEST_ASSERT_TRUE(isotp_rx_busy(&s_raw));
}

/* ------------------------------------------------------------------------------------------
 * The frames the node sends, byte by byte (FRAMES in isotp.h)
 * ---------------------------------------------------------------------------------------- */

static void test_a_single_frame_of_every_length_1_to_7(void)
{
    start();
    for (uint8_t length = 1U; length <= 7U; length++) {
        uint8_t expected[8];

        (void)memset(expected, CC, sizeof(expected));
        expected[0] = length;
        (void)memcpy(&expected[1], s_data, length);
        send(length);
        TEST_ASSERT_EQUAL_UINT32(1U, tb_waiting(&s_node));
        tb_take_expect(&s_node, expected);
        TEST_ASSERT_TRUE(isotp_tx_busy(&s_node.tp));
        TEST_ASSERT_TRUE(tb_confirm(&s_node));
        expect_conf(ISOTP_RESULT_OK);
        TEST_ASSERT_FALSE(isotp_tx_busy(&s_node.tp));
        tb_check(&s_node);
    }
    TEST_ASSERT_EQUAL_UINT32(7U, stats()->frames_out[ISOTP_FRAME_SF]);
    TEST_ASSERT_EQUAL_UINT32(7U, stats()->tx_results[ISOTP_RESULT_OK]);
}

/* FF_DL in 12 bits, the high nibble in byte 0, and the first 6 data bytes. */
static void test_the_first_frame_carries_ff_dl_and_6_bytes(void)
{
    const uint32_t lengths[] = {8U, 0x123U, 0x2FEU, ISOTP_FF_DL_12BIT_MAX};

    start();
    for (uint32_t i = 0U; i < (sizeof(lengths) / sizeof(lengths[0])); i++) {
        const uint32_t length = lengths[i];
        const uint8_t expected[8] = {(uint8_t)(0x10U | (length >> 8U)),
                                     (uint8_t)(length & 0xFFU),
                                     s_data[0],
                                     s_data[1],
                                     s_data[2],
                                     s_data[3],
                                     s_data[4],
                                     s_data[5]};

        send(length);
        tb_take_expect(&s_node, expected);
        TEST_ASSERT_TRUE(tb_confirm(&s_node));
        TEST_ASSERT_TRUE(fc_in(2U, 0U, 0U)); /* OVFLW ends it: the next one can begin */
        expect_conf(ISOTP_RESULT_BUFFER_OVFLW);
    }
    TEST_ASSERT_EQUAL_UINT32(4U, stats()->frames_out[ISOTP_FRAME_FF]);
    tb_check(&s_node);
}

/* CFs carry SN 1, 2, ... and 7 bytes each; the last one carries what is left, padded. Lengths
 * 20 (6 + 7 + 7: the last CF full) and 21 (one byte in a third CF). */
static void test_consecutive_frames_carry_sn_and_the_last_one_is_padded(void)
{
    const uint8_t last21[8] = {0x23U, s_data[20], CC, CC, CC, CC, CC, CC};

    start();
    tx_begin(20U);
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    expect_cf(20U, 1U);
    expect_cf(20U, 2U);
    expect_conf(ISOTP_RESULT_OK);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));

    tx_begin(21U);
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    expect_cf(21U, 1U);
    expect_cf(21U, 2U);
    tb_take_expect(&s_node, last21);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    expect_conf(ISOTP_RESULT_OK);
    TEST_ASSERT_EQUAL_UINT32(5U, stats()->frames_out[ISOTP_FRAME_CF]);
    tb_check(&s_node);
}

/* 6 + 17 * 7 bytes: SN 1..15, then 0 and 1. */
static void test_the_sequence_number_wraps_after_15(void)
{
    const uint16_t length = (uint16_t)(6U + (17U * 7U));

    start();
    tx_begin(length);
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    for (uint32_t i = 1U; i <= 17U; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x20U | (i & 0x0FU), s_node.out[s_node.out_head].frame.data[0]);
        expect_cf(length, i);
    }
    expect_conf(ISOTP_RESULT_OK);

    /* And as receiver: CF 16 carries SN 0 and is accepted. */
    rx_begin(length);
    for (uint32_t i = 1U; i <= 17U; i++) {
        TEST_ASSERT_TRUE_MESSAGE(cf_in(length, i), at_value("CF", i));
    }
    expect_ind(ISOTP_RESULT_OK, length);
    expect_message(length);
    tb_check(&s_node);
}

/* The receiver's FCs: CTS with the configured BS and STmin, WAIT and OVFLW with 0 and 0. */
static void test_flow_control_frames_byte_by_byte(void)
{
    s_cfg.rx_bs = 5U;
    s_cfg.rx_stmin = 0xF3U;
    s_cfg.rx_wft_max = 1U;
    start_sized(100U);
    TEST_ASSERT_TRUE(ff_in(50U));
    expect_fc(0U, 5U, 0xF3U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));

    TEST_ASSERT_TRUE(ff_in(101U)); /* ends the first (UNEXP_PDU) and does not fit */
    expect_ind(ISOTP_RESULT_UNEXP_PDU, 50U);
    expect_fc(2U, 0U, 0U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));

    TEST_ASSERT_TRUE(sf_in(1U));
    s_node.keep = false;
    TEST_ASSERT_TRUE(ff_in(8U));
    expect_fc(0U, 5U, 0xF3U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_TRUE(cf_in(8U, 1U));
    expect_ind(ISOTP_RESULT_OK, 8U);

    s_node.keep = true;
    TEST_ASSERT_TRUE(sf_in(2U)); /* kept */
    TEST_ASSERT_TRUE(ff_in(8U));
    poll_at(tb_now_us + s_cfg.n_br_us);
    expect_fc(1U, 0U, 0U);
    tb_check(&s_node);
}

/* 29-bit identifiers: frames out carry CAN_FLAG_EXT; a base-format frame with the same number
 * is someone else's. */
static void test_extended_identifiers(void)
{
    const uint8_t sf_ext[8] = {0x02U, s_data[0], s_data[1], CC, CC, CC, CC, CC};
    const can_frame_t base = {.id = 0x18DAF110U, .dlc = 2U, .data = {0x01U, 0x42U}};

    s_cfg.extended_ids = true;
    s_cfg.tx_id = 0x18DA10F1U;
    s_cfg.rx_id = 0x18DAF110U;
    start();
    send(2U);
    tb_take_expect(&s_node, sf_ext);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_TRUE(sf_in(3U));
    expect_message(3U);
    s_node.given++;
    TEST_ASSERT_FALSE(isotp_on_frame(&s_node.tp, &base, tb_now_us));
    expect_ignored(ISOTP_IGNORED_FOREIGN, 1U);
    tb_check(&s_node);
}

/* ------------------------------------------------------------------------------------------
 * The checks of received frames (RECEIVED FRAMES ARE CHECKED in isotp.h)
 * ---------------------------------------------------------------------------------------- */

/* SF_DL 1..7 with exactly 1 + SF_DL bytes, and with padding to 8. */
static void test_a_single_frame_of_every_length_is_received(void)
{
    start();
    for (uint8_t length = 1U; length <= 7U; length++) {
        uint8_t bytes[8];

        bytes[0] = length;
        (void)memcpy(&bytes[1], s_data, length);
        TEST_ASSERT_TRUE(tb_inject(&s_node, (uint8_t)(1U + length), bytes));
        expect_ind(ISOTP_RESULT_OK, length);
        expect_message(length);
        TEST_ASSERT_TRUE(sf_in(length));
        expect_message(length);
    }
    TEST_ASSERT_EQUAL_UINT32(14U, stats()->rx_results[ISOTP_RESULT_OK]);
    tb_check(&s_node);
}

/* SF_DL 0 is the CAN FD escape and 8..15 do not fit an 8-byte frame: ignored. */
static void test_single_frames_with_sf_dl_0_or_8_to_15_are_ignored(void)
{
    start();
    for (uint8_t low = 0U; low <= 15U; low++) {
        if ((low == 0U) || (low >= 8U)) {
            const uint8_t bytes[8] = {low, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
            TEST_ASSERT_FALSE_MESSAGE(tb_inject(&s_node, 8U, bytes), at_value("SF_DL", low));
        }
    }
    expect_ignored(ISOTP_IGNORED_LENGTH, 9U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.ind_count);
    tb_check(&s_node);
}

/* An SF needs 1 + SF_DL bytes: one fewer is ignored. */
static void test_single_frames_shorter_than_their_length_are_ignored(void)
{
    start();
    for (uint8_t length = 1U; length <= 7U; length++) {
        uint8_t bytes[8];

        bytes[0] = length;
        (void)memcpy(&bytes[1], s_data, 7U);
        TEST_ASSERT_FALSE(tb_inject(&s_node, length, bytes));
    }
    expect_ignored(ISOTP_IGNORED_SHORT, 7U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.ind_count);
    TEST_ASSERT_FALSE(tb_inject(&s_node, 0U, NULL)); /* no N_PCI at all */
    expect_ignored(ISOTP_IGNORED_SHORT, 8U);
    tb_check(&s_node);
}

/* An FF always fills the frame: DLC 1..7 is ignored. */
static void test_first_frames_shorter_than_8_bytes_are_ignored(void)
{
    const uint8_t bytes[8] = {0x10U, 0x20U, 1U, 2U, 3U, 4U, 5U, 6U};

    start();
    for (uint8_t dlc = 1U; dlc <= 7U; dlc++) {
        TEST_ASSERT_FALSE(tb_inject(&s_node, dlc, bytes));
    }
    expect_ignored(ISOTP_IGNORED_SHORT, 7U);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_FALSE(isotp_rx_busy(&s_node.tp));
    tb_check(&s_node);
}

/* FF_DL 1..7 would fit an SF: ignored. FF_DL 8 is the shortest FF. */
static void test_first_frames_with_ff_dl_below_8_are_ignored(void)
{
    start();
    for (uint16_t length = 1U; length <= 7U; length++) {
        TEST_ASSERT_FALSE(ff_in(length));
    }
    expect_ignored(ISOTP_IGNORED_LENGTH, 7U);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_TRUE(ff_in(8U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.ff_count);
    TEST_ASSERT_EQUAL_UINT16(8U, s_node.ff_length);
    expect_fc(0U, 0U, 0U);
    tb_check(&s_node);
}

/* The escape FF: FF_DL 0 in 12 bits, then 32 bits. Above 4095 it never fits: OVFLW. At 4095
 * or below the escape is not allowed: ignored. It needs 8 bytes like any FF. */
static void test_the_escape_first_frame(void)
{
    const uint32_t too_long[] = {4096U, 0x00012345U, 0xFFFFFFFFU};
    const uint32_t not_allowed[] = {0U, 7U, 8U, 4095U};

    start();
    for (uint32_t i = 0U; i < (sizeof(too_long) / sizeof(too_long[0])); i++) {
        const uint32_t ff_dl = too_long[i];
        const uint8_t bytes[8] = {0x10U,
                                  0x00U,
                                  (uint8_t)(ff_dl >> 24U),
                                  (uint8_t)(ff_dl >> 16U),
                                  (uint8_t)(ff_dl >> 8U),
                                  (uint8_t)ff_dl,
                                  0xAAU,
                                  0xBBU};
        TEST_ASSERT_TRUE(tb_inject(&s_node, 8U, bytes));
        expect_fc(2U, 0U, 0U);
        TEST_ASSERT_TRUE(tb_confirm(&s_node));
        TEST_ASSERT_FALSE(isotp_rx_busy(&s_node.tp));
    }
    TEST_ASSERT_EQUAL_UINT32(3U, stats()->rx_overflow);
    TEST_ASSERT_EQUAL_UINT32(3U, stats()->frames_out[ISOTP_FRAME_FC_OVFLW]);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.ff_count);

    for (uint32_t i = 0U; i < (sizeof(not_allowed) / sizeof(not_allowed[0])); i++) {
        const uint32_t ff_dl = not_allowed[i];
        const uint8_t bytes[8] = {0x10U,
                                  0x00U,
                                  (uint8_t)(ff_dl >> 24U),
                                  (uint8_t)(ff_dl >> 16U),
                                  (uint8_t)(ff_dl >> 8U),
                                  (uint8_t)ff_dl,
                                  0xAAU,
                                  0xBBU};
        TEST_ASSERT_FALSE(tb_inject(&s_node, 8U, bytes));
    }
    expect_ignored(ISOTP_IGNORED_LENGTH, 4U);
    {
        const uint8_t bytes[8] = {0x10U, 0x00U, 0x00U, 0x00U, 0x10U, 0x00U, 0xAAU, 0xBBU};
        TEST_ASSERT_FALSE(tb_inject(&s_node, 7U, bytes));
        expect_ignored(ISOTP_IGNORED_SHORT, 1U);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    tb_check(&s_node);
}

/* An FF longer than the buffer is answered with OVFLW and nothing is received; one that fits
 * exactly is received. */
static void test_a_first_frame_longer_than_the_buffer_gets_ovflw(void)
{
    start_sized(100U);
    TEST_ASSERT_TRUE(ff_in(101U));
    expect_fc(2U, 0U, 0U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_FALSE(isotp_rx_busy(&s_node.tp));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->rx_overflow);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.ff_count);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.ind_count);
    TEST_ASSERT_FALSE(cf_in(101U, 1U));
    expect_ignored(ISOTP_IGNORED_UNEXPECTED_CF, 1U);

    rx_begin(100U);
    for (uint32_t i = 1U; i <= 14U; i++) {
        TEST_ASSERT_TRUE(cf_in(100U, i));
    }
    expect_ind(ISOTP_RESULT_OK, 100U);
    expect_message(100U);
    tb_check(&s_node);
}

/* An SF longer than a tiny buffer is dropped, counted. */
static void test_a_single_frame_longer_than_the_buffer_is_dropped(void)
{
    start_sized(3U);
    TEST_ASSERT_TRUE(sf_in(3U));
    expect_message(3U);
    TEST_ASSERT_TRUE(sf_in(4U));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->rx_sf_too_long);
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.messages);
    TEST_ASSERT_TRUE(ff_in(8U)); /* and every FF overflows */
    expect_fc(2U, 0U, 0U);
    tb_check(&s_node);
}

/* A CF must carry the data it has to: 7 bytes, or what is left for the last one. Message of 15
 * bytes: FF 6, CF1 7, CF2 2. */
static void test_consecutive_frames_shorter_than_their_data_are_ignored(void)
{
    uint8_t cf1[8] = {0x21U};
    uint8_t cf2[8] = {0x22U};

    (void)memcpy(&cf1[1], &s_data[6], 7U);
    (void)memcpy(&cf2[1], &s_data[13], 2U);
    start();
    rx_begin(15U);
    TEST_ASSERT_FALSE(tb_inject(&s_node, 7U, cf1));
    TEST_ASSERT_FALSE(tb_inject(&s_node, 1U, cf1));
    expect_ignored(ISOTP_IGNORED_SHORT, 2U);
    TEST_ASSERT_TRUE(isotp_rx_busy(&s_node.tp));
    TEST_ASSERT_TRUE(tb_inject(&s_node, 8U, cf1));
    TEST_ASSERT_FALSE(tb_inject(&s_node, 2U, cf2));
    expect_ignored(ISOTP_IGNORED_SHORT, 3U);
    TEST_ASSERT_TRUE(tb_inject(&s_node, 3U, cf2)); /* exactly what it needs */
    expect_ind(ISOTP_RESULT_OK, 15U);
    expect_message(15U);
    tb_check(&s_node);
}

/* An FC needs its 3 bytes: a shorter one is ignored and the sender keeps waiting. */
static void test_flow_control_frames_shorter_than_3_bytes_are_ignored(void)
{
    const uint8_t cts[3] = {0x30U, 0U, 0U};

    start();
    tx_begin(20U);
    TEST_ASSERT_FALSE(tb_inject(&s_node, 1U, cts));
    TEST_ASSERT_FALSE(tb_inject(&s_node, 2U, cts));
    expect_ignored(ISOTP_IGNORED_SHORT, 2U);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_UINT64(T0 + ISOTP_TIMEOUT_US, isotp_next_deadline(&s_node.tp));
    TEST_ASSERT_TRUE(tb_inject(&s_node, 3U, cts));
    expect_cf(20U, 1U);
    tb_check(&s_node);
}

/* N_PCI types 0x4..0xF are reserved: ignored when idle, during a transmission and during a
 * reception, which all go on as if nothing had come. */
static void test_reserved_pci_types_are_ignored_in_every_state(void)
{
    uint32_t ignored = 0U;

    start();
    for (uint32_t type = 4U; type <= 15U; type++) {
        TEST_ASSERT_FALSE(inject((uint8_t)(type << 4U), 0U, 0U));
        ignored++;
    }
    tx_begin(20U);
    rx_begin(20U);
    TEST_ASSERT_TRUE(cf_in(20U, 1U));
    for (uint32_t type = 4U; type <= 15U; type++) {
        TEST_ASSERT_FALSE(inject((uint8_t)((type << 4U) | 0x02U), 0U, 0U));
        ignored++;
    }
    expect_ignored(ISOTP_IGNORED_RESERVED_TYPE, ignored);
    TEST_ASSERT_TRUE(cf_in(20U, 2U));
    expect_ind(ISOTP_RESULT_OK, 20U);
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    expect_cf(20U, 1U);
    expect_cf(20U, 2U);
    expect_conf(ISOTP_RESULT_OK);
    tb_check(&s_node);
}

/* Frames of another connection: another identifier (the node's own included), the other
 * format, a remote frame, an unknown flag, DLC above 8. */
static void test_frames_of_another_connection_are_ignored(void)
{
    can_frame_t frames[] = {
        {.id = 0x7E1U, .dlc = 2U, .data = {0x01U, 0x11U}},
        {.id = TB_NODE_TX, .dlc = 2U, .data = {0x01U, 0x11U}},
        {.id = TB_NODE_RX, .flags = CAN_FLAG_EXT, .dlc = 2U, .data = {0x01U, 0x11U}},
        {.id = TB_NODE_RX, .flags = CAN_FLAG_RTR, .dlc = 2U, .data = {0x01U, 0x11U}},
        {.id = TB_NODE_RX, .flags = 0x80U, .dlc = 2U, .data = {0x01U, 0x11U}},
        {.id = TB_NODE_RX, .dlc = 9U, .data = {0x01U, 0x11U}},
    };

    start();
    for (uint32_t i = 0U; i < (sizeof(frames) / sizeof(frames[0])); i++) {
        s_node.given++;
        TEST_ASSERT_FALSE_MESSAGE(isotp_on_frame(&s_node.tp, &frames[i], tb_now_us),
                                  at_value("frame", i));
    }
    expect_ignored(ISOTP_IGNORED_FOREIGN, 6U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.ind_count);
    tb_check(&s_node);
}

/* FS 3..15 are reserved: the transmission ends with N_INVALID_FS. */
static void test_a_reserved_flow_status_ends_the_transmission(void)
{
    start();
    for (uint8_t fs = 3U; fs <= 15U; fs++) {
        tx_begin(20U);
        TEST_ASSERT_TRUE(fc_in(fs, 0U, 0U));
        expect_conf(ISOTP_RESULT_INVALID_FS);
        TEST_ASSERT_FALSE(isotp_tx_busy(&s_node.tp));
        TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    }
    TEST_ASSERT_EQUAL_UINT32(13U, stats()->tx_results[ISOTP_RESULT_INVALID_FS]);
    TEST_ASSERT_EQUAL_UINT32(13U, stats()->frames_in[ISOTP_FRAME_FC_RESERVED]);
    tb_check(&s_node);
}

/* Every STmin byte, as the standard's table reads it. */
static void test_stmin_every_value(void)
{
    for (uint32_t value = 0U; value <= 0xFFU; value++) {
        uint32_t expected = 127000U;
        bool valid = false;

        if (value <= 0x7FU) {
            expected = value * 1000U;
            valid = true;
        } else if ((value >= 0xF1U) && (value <= 0xF9U)) {
            expected = (value - 0xF0U) * 100U;
            valid = true;
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected, isotp_stmin_us((uint8_t)value),
                                         at_value("STmin", value));
        TEST_ASSERT_EQUAL_MESSAGE(valid, isotp_stmin_valid((uint8_t)value),
                                  at_value("STmin", value));
    }
    TEST_ASSERT_EQUAL_UINT32(100U, isotp_stmin_us(0xF1U));
    TEST_ASSERT_EQUAL_UINT32(900U, isotp_stmin_us(0xF9U));
    TEST_ASSERT_EQUAL_UINT32(127000U, isotp_stmin_us(0x7FU));
    TEST_ASSERT_EQUAL_UINT32(127000U, isotp_stmin_us(0xF0U));
}

/* ------------------------------------------------------------------------------------------
 * The table of unexpected N_PDUs, row by row (UNEXPECTED PDUS in isotp.h). "Reception" is a
 * reception of 20 bytes with its FC CTS confirmed and one CF in; "transmission" is one of 20
 * bytes whose FF is confirmed and which waits for an FC.
 * ---------------------------------------------------------------------------------------- */

static void reception_in_progress(void)
{
    rx_begin(20U);
    TEST_ASSERT_TRUE(cf_in(20U, 1U));
    TEST_ASSERT_TRUE(isotp_rx_busy(&s_node.tp));
}

static void transmission_in_progress(void)
{
    tx_begin(20U);
    TEST_ASSERT_TRUE(isotp_tx_busy(&s_node.tp));
}

/* The transmission goes on undisturbed: FC, two CFs, OK. */
static void transmission_completes(void)
{
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    expect_cf(20U, 1U);
    expect_cf(20U, 2U);
    expect_conf(ISOTP_RESULT_OK);
}

static void test_table_sf_when_idle_is_a_new_reception(void)
{
    start();
    TEST_ASSERT_TRUE(sf_in(5U));
    expect_ind(ISOTP_RESULT_OK, 5U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.ind_count);
    tb_check(&s_node);
}

static void test_table_sf_during_a_reception_ends_it_and_is_received(void)
{
    start();
    reception_in_progress();
    TEST_ASSERT_TRUE(sf_in(4U));
    TEST_ASSERT_EQUAL_UINT32(2U, s_node.ind_count);
    TEST_ASSERT_EQUAL(ISOTP_RESULT_UNEXP_PDU, s_node.ind[0].result);
    TEST_ASSERT_EQUAL_UINT16(20U, s_node.ind[0].length);
    expect_ind(ISOTP_RESULT_OK, 4U);
    expect_message(4U);
    TEST_ASSERT_FALSE(cf_in(20U, 2U)); /* the old message's next CF: nobody expects it */
    expect_ignored(ISOTP_IGNORED_UNEXPECTED_CF, 1U);
    tb_check(&s_node);
}

static void test_table_sf_during_a_transmission_is_received_and_the_transmission_goes_on(void)
{
    start();
    transmission_in_progress();
    TEST_ASSERT_TRUE(sf_in(6U));
    expect_ind(ISOTP_RESULT_OK, 6U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.ind_count);
    transmission_completes();
    tb_check(&s_node);
}

static void test_table_sf_during_both_ends_the_reception_only(void)
{
    start();
    transmission_in_progress();
    reception_in_progress();
    TEST_ASSERT_TRUE(sf_in(2U));
    TEST_ASSERT_EQUAL(ISOTP_RESULT_UNEXP_PDU, s_node.ind[0].result);
    expect_ind(ISOTP_RESULT_OK, 2U);
    transmission_completes();
    tb_check(&s_node);
}

static void test_table_ff_when_idle_is_a_new_reception(void)
{
    start();
    TEST_ASSERT_TRUE(ff_in(30U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.ff_count);
    expect_fc(0U, 0U, 0U);
    tb_check(&s_node);
}

static void test_table_ff_during_a_reception_ends_it_and_starts_a_new_one(void)
{
    start();
    reception_in_progress();
    TEST_ASSERT_TRUE(ff_in(30U));
    expect_ind(ISOTP_RESULT_UNEXP_PDU, 20U);
    TEST_ASSERT_EQUAL_UINT32(2U, s_node.ff_count);
    TEST_ASSERT_EQUAL_UINT16(30U, s_node.ff_length);
    expect_fc(0U, 0U, 0U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    for (uint32_t i = 1U; i <= 4U; i++) {
        TEST_ASSERT_TRUE(cf_in(30U, i));
    }
    expect_ind(ISOTP_RESULT_OK, 30U);
    expect_message(30U);
    tb_check(&s_node);
}

static void test_table_ff_during_a_transmission_is_received_and_the_transmission_goes_on(void)
{
    start();
    transmission_in_progress();
    TEST_ASSERT_TRUE(ff_in(9U));
    expect_fc(0U, 0U, 0U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_TRUE(cf_in(9U, 1U));
    expect_ind(ISOTP_RESULT_OK, 9U);
    transmission_completes();
    tb_check(&s_node);
}

static void test_table_ff_during_both_ends_the_reception_only(void)
{
    start();
    transmission_in_progress();
    reception_in_progress();
    TEST_ASSERT_TRUE(ff_in(9U));
    expect_ind(ISOTP_RESULT_UNEXP_PDU, 20U);
    expect_fc(0U, 0U, 0U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    transmission_completes();
    TEST_ASSERT_TRUE(cf_in(9U, 1U));
    expect_ind(ISOTP_RESULT_OK, 9U);
    tb_check(&s_node);
}

static void test_table_cf_when_idle_is_ignored(void)
{
    start();
    TEST_ASSERT_FALSE(cf_in(20U, 1U));
    expect_ignored(ISOTP_IGNORED_UNEXPECTED_CF, 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.ind_count);
    tb_check(&s_node);
}

static void test_table_cf_during_a_transmission_only_is_ignored(void)
{
    start();
    transmission_in_progress();
    TEST_ASSERT_FALSE(cf_in(20U, 1U));
    expect_ignored(ISOTP_IGNORED_UNEXPECTED_CF, 1U);
    transmission_completes();
    tb_check(&s_node);
}

static void test_table_cf_during_a_reception_is_processed_with_its_sn_checked(void)
{
    start();
    reception_in_progress();
    TEST_ASSERT_TRUE(cf_in(20U, 2U));
    expect_ind(ISOTP_RESULT_OK, 20U);

    reception_in_progress();
    TEST_ASSERT_TRUE(cf_in(20U, 3U)); /* SN 3 where 2 is due */
    expect_ind(ISOTP_RESULT_WRONG_SN, 20U);
    TEST_ASSERT_FALSE(isotp_rx_busy(&s_node.tp));
    TEST_ASSERT_FALSE(cf_in(20U, 2U)); /* the reception is over */
    expect_ignored(ISOTP_IGNORED_UNEXPECTED_CF, 1U);

    reception_in_progress(); /* during both, the transmission is not touched */
    transmission_in_progress();
    TEST_ASSERT_TRUE(cf_in(20U, 1U)); /* a repeated CF has the wrong SN */
    expect_ind(ISOTP_RESULT_WRONG_SN, 20U);
    transmission_completes();
    TEST_ASSERT_EQUAL_UINT32(2U, stats()->rx_results[ISOTP_RESULT_WRONG_SN]);
    tb_check(&s_node);
}

/* Every wrong SN, at every position of a 40-byte message. */
static void test_every_wrong_sequence_number_ends_the_reception(void)
{
    start();
    for (uint32_t position = 1U; position <= 5U; position++) {
        for (uint8_t sn = 0U; sn <= 15U; sn++) {
            uint8_t bytes[8];

            if (sn == (position & 0x0FU)) {
                continue;
            }
            rx_begin(40U);
            for (uint32_t i = 1U; i < position; i++) {
                TEST_ASSERT_TRUE(cf_in(40U, i));
            }
            (void)memset(bytes, CC, sizeof(bytes));
            bytes[0] = (uint8_t)(0x20U | sn);
            TEST_ASSERT_TRUE(tb_inject(&s_node, 8U, bytes));
            expect_ind(ISOTP_RESULT_WRONG_SN, 40U);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(75U, stats()->rx_results[ISOTP_RESULT_WRONG_SN]);
    tb_check(&s_node);
}

static void test_table_fc_when_idle_is_ignored(void)
{
    start();
    TEST_ASSERT_FALSE(fc_in(0U, 0U, 0U));
    TEST_ASSERT_FALSE(fc_in(2U, 0U, 0U));
    expect_ignored(ISOTP_IGNORED_UNEXPECTED_FC, 2U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.conf_count);
    tb_check(&s_node);
}

static void test_table_fc_during_a_reception_only_is_ignored(void)
{
    start();
    reception_in_progress();
    TEST_ASSERT_FALSE(fc_in(0U, 0U, 0U));
    expect_ignored(ISOTP_IGNORED_UNEXPECTED_FC, 1U);
    TEST_ASSERT_TRUE(cf_in(20U, 2U));
    expect_ind(ISOTP_RESULT_OK, 20U);
    tb_check(&s_node);
}

/* An FC is processed only where the sender waits for one: not during an SF, not between the
 * CFs of a block, not during the STmin wait. */
static void test_table_fc_during_a_transmission_that_does_not_wait_for_one_is_ignored(void)
{
    start();
    send(5U); /* an SF */
    TEST_ASSERT_FALSE(fc_in(0U, 0U, 0U));
    (void)tb_take_confirm(&s_node);
    expect_conf(ISOTP_RESULT_OK);

    tx_begin(40U);
    TEST_ASSERT_TRUE(fc_in(0U, 3U, 10U)); /* BS 3, STmin 10 ms */
    (void)tb_take(&s_node);               /* CF 1 out, not confirmed */
    TEST_ASSERT_FALSE(fc_in(0U, 0U, 0U)); /* not the last of its block */
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_FALSE(fc_in(0U, 0U, 0U)); /* during STmin */
    expect_ignored(ISOTP_IGNORED_UNEXPECTED_FC, 3U);
    poll_at(tb_now_us + 10000U);
    expect_cf(40U, 2U);
    poll_at(tb_now_us + 10000U);
    expect_cf(40U, 3U); /* the block is complete: now an FC is waited for */
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    tb_check(&s_node);
}

/* Only a valid frame ends a reception: an SF with SF_DL 0 or an FF that is too short does not. */
static void test_an_ignored_frame_does_not_end_a_reception(void)
{
    const uint8_t sf0[8] = {0x00U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    const uint8_t short_ff[7] = {0x10U, 0x30U, 1U, 2U, 3U, 4U, 5U};

    start();
    reception_in_progress();
    TEST_ASSERT_FALSE(tb_inject(&s_node, 8U, sf0));
    TEST_ASSERT_FALSE(tb_inject(&s_node, 7U, short_ff));
    TEST_ASSERT_FALSE(ff_in(5U));
    TEST_ASSERT_TRUE(isotp_rx_busy(&s_node.tp));
    TEST_ASSERT_TRUE(cf_in(20U, 2U));
    expect_ind(ISOTP_RESULT_OK, 20U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.ind_count);
    tb_check(&s_node);
}

/* A CF before the receiver's FC CTS is out cannot be an answer to it: ignored. Likewise during
 * the wait for a kept buffer. */
static void test_a_consecutive_frame_before_the_cts_is_out_is_ignored(void)
{
    s_cfg.rx_wft_max = 2U;
    start();
    s_node.keep = true;
    TEST_ASSERT_TRUE(sf_in(1U));
    TEST_ASSERT_TRUE(ff_in(20U)); /* waits for the buffer */
    TEST_ASSERT_FALSE(cf_in(20U, 1U));
    poll_at(tb_now_us + s_cfg.n_br_us);
    expect_fc(1U, 0U, 0U); /* WAIT out, not confirmed */
    TEST_ASSERT_FALSE(cf_in(20U, 1U));
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_FALSE(cf_in(20U, 1U));
    expect_ignored(ISOTP_IGNORED_UNEXPECTED_CF, 3U);
    s_node.keep = false;
    TEST_ASSERT_TRUE(isotp_rx_release(&s_node.tp, tb_now_us));
    expect_fc(0U, 0U, 0U);
    TEST_ASSERT_TRUE(cf_in(20U, 1U)); /* before the CTS's confirmation: it proves the CTS */
    TEST_ASSERT_TRUE(cf_in(20U, 2U));
    expect_ind(ISOTP_RESULT_OK, 20U);
    expect_message(20U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node)); /* the CTS's late confirmation changes nothing */
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->outstanding);
    tb_check(&s_node);
}

/* ------------------------------------------------------------------------------------------
 * ISO 15765-2:2016 CAN FD and 32-bit escape FF tests
 * ---------------------------------------------------------------------------------------- */

static void test_canfd_single_frames_tx_1_to_62_conform_to_table_12_and_13(void)
{
    s_cfg.can_fd = true;
    s_cfg.can_fd_brs = true;
    s_cfg.tx_dl = 64U;
    start_sized(TB_MAX_LENGTH);

    for (uint8_t len = 1U; len <= ISOTP_FD_MAX_SF_DATA; len++) {
        uint8_t expected[CAN_FD_MAX_DL];
        const uint8_t can_dl = (len <= 7U) ? 8U : can_fd_pad_len((uint8_t)(2U + len));

        (void)memset(expected, CC, sizeof(expected));
        if (len <= 7U) {
            expected[0] = len;
            (void)memcpy(&expected[1], s_data, len);
        } else {
            expected[0] = 0x00U;
            expected[1] = len;
            (void)memcpy(&expected[2], s_data, len);
        }
        send(len);
        tb_take_expect_len(&s_node, can_dl, expected);
        TEST_ASSERT_TRUE(tb_confirm(&s_node));
        expect_conf(ISOTP_RESULT_OK);
    }
    TEST_ASSERT_EQUAL_UINT32(62U, stats()->frames_out[ISOTP_FRAME_SF]);

    /* 63 bytes exceeds (TX_DL - 2) for TX_DL = 64 -> must promote to FF (Table 14). */
    send(63U);
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->frames_out[ISOTP_FRAME_FF]);
    {
        const can_frame_t ff = tb_take(&s_node);
        TEST_ASSERT_EQUAL_UINT8(64U, ff.dlc);
        TEST_ASSERT_EQUAL_HEX8(0x10U, ff.data[0]);
        TEST_ASSERT_EQUAL_HEX8(63U, ff.data[1]);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(s_data, &ff.data[2], 62U);
    }
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    tb_check(&s_node);
}

static void test_canfd_single_frames_rx_table_13_bounds(void)
{
    static const uint8_t fd_dls[] = {12U, 16U, 20U, 24U, 32U, 48U, 64U};
    static const uint8_t min_sf[] = {8U, 11U, 15U, 19U, 23U, 31U, 47U};
    uint8_t buf[CAN_FD_MAX_DL];
    uint32_t rejected = 0U;

    /* A classical instance rejects CAN_FLAG_FD frames as FOREIGN. */
    start_sized(TB_MAX_LENGTH);
    (void)memset(buf, CC, sizeof(buf));
    buf[0] = 0x00U;
    buf[1] = 8U;
    {
        can_frame_t fd_frame = {.id = TB_NODE_RX, .flags = CAN_FLAG_FD, .dlc = 12U};
        (void)memcpy(fd_frame.data, buf, 12U);
        s_node.given++;
        TEST_ASSERT_FALSE(isotp_on_frame(&s_node.tp, &fd_frame, tb_now_us));
        expect_ignored(ISOTP_IGNORED_FOREIGN, 1U);
    }

    s_cfg.can_fd = true;
    s_cfg.tx_dl = 64U;
    start_sized(TB_MAX_LENGTH);

    for (size_t i = 0U; i < (sizeof(fd_dls) / sizeof(fd_dls[0])); i++) {
        const uint8_t dlc = fd_dls[i];
        const uint8_t lo = min_sf[i];
        const uint8_t hi = (uint8_t)(dlc - 2U);

        /* Non-zero low nibble is invalid when CAN_DL > 8. */
        (void)memset(buf, CC, sizeof(buf));
        buf[0] = 0x01U;
        buf[1] = lo;
        TEST_ASSERT_FALSE(tb_inject_fd(&s_node, dlc, buf));
        rejected++;

        /* Below minimum SF_DL for this CAN_DL (wrong container size, Table 13). */
        buf[0] = 0x00U;
        buf[1] = (uint8_t)(lo - 1U);
        TEST_ASSERT_FALSE(tb_inject_fd(&s_node, dlc, buf));
        rejected++;

        /* Above maximum SF_DL for this CAN_DL. */
        buf[1] = (uint8_t)(hi + 1U);
        TEST_ASSERT_FALSE(tb_inject_fd(&s_node, dlc, buf));
        rejected++;

        /* Valid min and max SF_DL for this CAN_DL. */
        for (uint32_t k = 0U; k < 2U; k++) {
            const uint8_t sf_dl = (k == 0U) ? lo : hi;

            (void)memset(buf, CC, sizeof(buf));
            buf[0] = 0x00U;
            buf[1] = sf_dl;
            (void)memcpy(&buf[2], s_data, sf_dl);
            TEST_ASSERT_TRUE(tb_inject_fd(&s_node, dlc, buf));
            expect_ind(ISOTP_RESULT_OK, sf_dl);
            expect_message(sf_dl);
        }
    }
    expect_ignored(ISOTP_IGNORED_LENGTH, rejected);
    tb_check(&s_node);
}

static void test_canfd_first_frame_and_cf_tx_and_rx(void)
{
    /* TX_DL = 64: 135-byte message = FF (62 B) + CF1 (63 B, dlc 64) + CF2 (10 B -> dlc 12). */
    const uint32_t len = 135U;
    uint8_t buf[CAN_FD_MAX_DL];

    s_cfg.can_fd = true;
    s_cfg.can_fd_brs = true;
    s_cfg.tx_dl = 64U;
    s_cfg.rx_stmin = 0x00U;
    start_sized(TB_MAX_LENGTH);

    send(len);
    {
        uint8_t exp_ff[64];

        exp_ff[0] = (uint8_t)(0x10U | (len >> 8U));
        exp_ff[1] = (uint8_t)(len & 0xFFU);
        (void)memcpy(&exp_ff[2], s_data, 62U);
        tb_take_expect_len(&s_node, 64U, exp_ff);
        TEST_ASSERT_TRUE(tb_confirm(&s_node));
    }
    /* Receive FC CTS with STmin = 10 ms: STmin MUST NOT delay CF1 right after the FF. */
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 10U));
    {
        uint8_t exp_cf1[64];
        uint8_t exp_cf2[12];

        exp_cf1[0] = 0x21U;
        (void)memcpy(&exp_cf1[1], &s_data[62], 63U);
        tb_take_expect_len(&s_node, 64U, exp_cf1);
        TEST_ASSERT_TRUE(tb_confirm(&s_node));

        poll_at(tb_now_us + 10000U);
        (void)memset(exp_cf2, CC, sizeof(exp_cf2));
        exp_cf2[0] = 0x22U;
        (void)memcpy(&exp_cf2[1], &s_data[125], 10U);
        tb_take_expect_len(&s_node, 12U, exp_cf2);
        TEST_ASSERT_TRUE(tb_confirm(&s_node));
        expect_conf(ISOTP_RESULT_OK);
    }

    /* RX Table 14 check: 64-byte FF with FF_DL = 62 (< RX_DL - 1) must be ignored. */
    (void)memset(buf, 0xAA, sizeof(buf));
    buf[0] = 0x10U;
    buf[1] = 62U;
    TEST_ASSERT_FALSE(tb_inject_fd(&s_node, 64U, buf));
    expect_ignored(ISOTP_IGNORED_LENGTH, 1U);

    /* RX 135-byte CAN FD message -> FF(64) -> FC(8) -> CF1(64) -> CF2(12). */
    buf[0] = (uint8_t)(0x10U | (len >> 8U));
    buf[1] = (uint8_t)(len & 0xFFU);
    (void)memcpy(&buf[2], s_data, 62U);
    TEST_ASSERT_TRUE(tb_inject_fd(&s_node, 64U, buf));
    expect_fc(0U, 0U, 0U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));

    buf[0] = 0x21U;
    (void)memcpy(&buf[1], &s_data[62], 63U);
    TEST_ASSERT_TRUE(tb_inject_fd(&s_node, 64U, buf));

    (void)memset(buf, CC, sizeof(buf));
    buf[0] = 0x22U;
    (void)memcpy(&buf[1], &s_data[125], 10U);
    TEST_ASSERT_TRUE(tb_inject_fd(&s_node, 12U, buf));
    expect_ind(ISOTP_RESULT_OK, len);
    expect_message(len);
    tb_check(&s_node);
}

static void test_escape_first_frame_tx_and_rx_classical_and_canfd(void)
{
    const uint32_t len = 5000U;
    uint8_t buf[64];

    /* 1) Classical CAN (TX_DL = 8): 32-bit escape FF carries 2 data bytes + 714 CFs. */
    start_sized(TB_MAX_LENGTH);
    send(len);
    {
        const uint8_t exp_ff[8] = {0x10U,
                                   0x00U,
                                   (uint8_t)(len >> 24U),
                                   (uint8_t)(len >> 16U),
                                   (uint8_t)(len >> 8U),
                                   (uint8_t)len,
                                   s_data[0],
                                   s_data[1]};
        tb_take_expect(&s_node, exp_ff);
        TEST_ASSERT_TRUE(tb_confirm(&s_node));
    }
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    {
        uint32_t offset = 2U;
        uint32_t sn = 1U;

        while (offset < len) {
            const can_frame_t cf = tb_take_confirm(&s_node);
            const uint32_t rem = len - offset;
            const uint32_t chunk = (rem > 7U) ? 7U : rem;

            TEST_ASSERT_EQUAL_UINT8(8U, cf.dlc);
            TEST_ASSERT_EQUAL_HEX8((uint8_t)(0x20U | (sn & 0x0FU)), cf.data[0]);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(&s_data[offset], &cf.data[1], chunk);
            offset += chunk;
            sn++;
        }
        expect_conf(ISOTP_RESULT_OK);
    }

    /* 2) CAN FD (TX_DL = 64): 32-bit escape FF carries 58 data bytes + 79 CFs. */
    s_cfg.can_fd = true;
    s_cfg.tx_dl = 64U;
    start_sized(TB_MAX_LENGTH);
    send(len);
    {
        uint8_t exp_ff[64];

        exp_ff[0] = 0x10U;
        exp_ff[1] = 0x00U;
        exp_ff[2] = (uint8_t)(len >> 24U);
        exp_ff[3] = (uint8_t)(len >> 16U);
        exp_ff[4] = (uint8_t)(len >> 8U);
        exp_ff[5] = (uint8_t)len;
        (void)memcpy(&exp_ff[6], s_data, 58U);
        tb_take_expect_len(&s_node, 64U, exp_ff);
        TEST_ASSERT_TRUE(tb_confirm(&s_node));
    }
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    {
        uint32_t offset = 58U;
        uint32_t sn = 1U;

        while (offset < len) {
            const can_frame_t cf = tb_take_confirm(&s_node);
            const uint32_t rem = len - offset;
            const uint32_t chunk = (rem > 63U) ? 63U : rem;
            const uint8_t exp_dlc =
                (chunk == 63U) ? 64U : ((chunk <= 7U) ? 8U : can_fd_pad_len((uint8_t)(1U + chunk)));

            TEST_ASSERT_EQUAL_UINT8(exp_dlc, cf.dlc);
            TEST_ASSERT_EQUAL_HEX8((uint8_t)(0x20U | (sn & 0x0FU)), cf.data[0]);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(&s_data[offset], &cf.data[1], chunk);
            offset += chunk;
            sn++;
        }
        expect_conf(ISOTP_RESULT_OK);
    }

    /* 3) CAN FD RX of 5000-byte 32-bit escape FF message. */
    buf[0] = 0x10U;
    buf[1] = 0x00U;
    buf[2] = (uint8_t)(len >> 24U);
    buf[3] = (uint8_t)(len >> 16U);
    buf[4] = (uint8_t)(len >> 8U);
    buf[5] = (uint8_t)len;
    (void)memcpy(&buf[6], s_data, 58U);
    TEST_ASSERT_TRUE(tb_inject_fd(&s_node, 64U, buf));
    expect_fc(0U, 0U, 0U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    {
        uint32_t offset = 58U;
        uint32_t sn = 1U;

        while (offset < len) {
            const uint32_t rem = len - offset;
            const uint32_t chunk = (rem > 63U) ? 63U : rem;
            const uint8_t dlc =
                (chunk == 63U) ? 64U : ((chunk <= 7U) ? 8U : can_fd_pad_len((uint8_t)(1U + chunk)));

            (void)memset(buf, CC, dlc);
            buf[0] = (uint8_t)(0x20U | (sn & 0x0FU));
            (void)memcpy(&buf[1], &s_data[offset], chunk);
            TEST_ASSERT_TRUE(tb_inject_fd(&s_node, dlc, buf));
            offset += chunk;
            sn++;
        }
    }
    expect_ind(ISOTP_RESULT_OK, len);
    expect_message(len);
    tb_check(&s_node);
}

/* ------------------------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_refuses_every_bad_argument);
    RUN_TEST(test_an_uninitialised_instance_is_refused);
    RUN_TEST(test_send_refuses_length_0_and_null);
    RUN_TEST(test_send_refuses_a_second_message_while_one_is_sent);
    RUN_TEST(test_a_buffer_above_4095_bytes_receives_escape_ff);
    RUN_TEST(test_the_optional_callbacks_may_be_null);
    RUN_TEST(test_a_single_frame_of_every_length_1_to_7);
    RUN_TEST(test_the_first_frame_carries_ff_dl_and_6_bytes);
    RUN_TEST(test_consecutive_frames_carry_sn_and_the_last_one_is_padded);
    RUN_TEST(test_the_sequence_number_wraps_after_15);
    RUN_TEST(test_flow_control_frames_byte_by_byte);
    RUN_TEST(test_extended_identifiers);
    RUN_TEST(test_a_single_frame_of_every_length_is_received);
    RUN_TEST(test_single_frames_with_sf_dl_0_or_8_to_15_are_ignored);
    RUN_TEST(test_single_frames_shorter_than_their_length_are_ignored);
    RUN_TEST(test_first_frames_shorter_than_8_bytes_are_ignored);
    RUN_TEST(test_first_frames_with_ff_dl_below_8_are_ignored);
    RUN_TEST(test_the_escape_first_frame);
    RUN_TEST(test_a_first_frame_longer_than_the_buffer_gets_ovflw);
    RUN_TEST(test_a_single_frame_longer_than_the_buffer_is_dropped);
    RUN_TEST(test_consecutive_frames_shorter_than_their_data_are_ignored);
    RUN_TEST(test_flow_control_frames_shorter_than_3_bytes_are_ignored);
    RUN_TEST(test_reserved_pci_types_are_ignored_in_every_state);
    RUN_TEST(test_frames_of_another_connection_are_ignored);
    RUN_TEST(test_a_reserved_flow_status_ends_the_transmission);
    RUN_TEST(test_stmin_every_value);
    RUN_TEST(test_table_sf_when_idle_is_a_new_reception);
    RUN_TEST(test_table_sf_during_a_reception_ends_it_and_is_received);
    RUN_TEST(test_table_sf_during_a_transmission_is_received_and_the_transmission_goes_on);
    RUN_TEST(test_table_sf_during_both_ends_the_reception_only);
    RUN_TEST(test_table_ff_when_idle_is_a_new_reception);
    RUN_TEST(test_table_ff_during_a_reception_ends_it_and_starts_a_new_one);
    RUN_TEST(test_table_ff_during_a_transmission_is_received_and_the_transmission_goes_on);
    RUN_TEST(test_table_ff_during_both_ends_the_reception_only);
    RUN_TEST(test_table_cf_when_idle_is_ignored);
    RUN_TEST(test_table_cf_during_a_transmission_only_is_ignored);
    RUN_TEST(test_table_cf_during_a_reception_is_processed_with_its_sn_checked);
    RUN_TEST(test_every_wrong_sequence_number_ends_the_reception);
    RUN_TEST(test_table_fc_when_idle_is_ignored);
    RUN_TEST(test_table_fc_during_a_reception_only_is_ignored);
    RUN_TEST(test_table_fc_during_a_transmission_that_does_not_wait_for_one_is_ignored);
    RUN_TEST(test_an_ignored_frame_does_not_end_a_reception);
    RUN_TEST(test_a_consecutive_frame_before_the_cts_is_out_is_ignored);
    RUN_TEST(test_canfd_single_frames_tx_1_to_62_conform_to_table_12_and_13);
    RUN_TEST(test_canfd_single_frames_rx_table_13_bounds);
    RUN_TEST(test_canfd_first_frame_and_cf_tx_and_rx);
    RUN_TEST(test_escape_first_frame_tx_and_rx_classical_and_canfd);
    return UNITY_END();
}
