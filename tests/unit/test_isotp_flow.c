/* SPDX-License-Identifier: MIT */
/*
 * lib/isotp, one instance at a time, part 2 in the order of its header: the timers, each at the
 * microsecond it runs out and the one before; flow control (BS and STmin from every CTS, STmin
 * across a block boundary, WAIT at the sender and at the receiver up to their limits and one
 * more, OVFLW); the confirmations (in order, an answer before the confirmation, frames given up,
 * the pipeline when it is full); refused frames; the callbacks; the statistics of N_Br and N_Cs;
 * the names; the ends of the clock.
 *
 * The timer tests give the four timeouts four different values (TIMERS below), so that a timer
 * taken for another one shows. The test is the node's peer, as in test_isotp.c
 * (isotp_test_node.h).
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

/* The timers of the timer tests, all different. */
#define N_AS 1000000U
#define N_AR 600000U
#define N_BS 700000U
#define N_CR 800000U
#define N_BR 50000U

void setUp(void)
{
    node_setup();
}

void tearDown(void)
{
}

static void distinct_timers(void)
{
    s_cfg.n_as_us = N_AS;
    s_cfg.n_ar_us = N_AR;
    s_cfg.n_bs_us = N_BS;
    s_cfg.n_cr_us = N_CR;
    s_cfg.n_br_us = N_BR;
}

static uint64_t deadline(void)
{
    return isotp_next_deadline(&s_node.tp);
}

/* The node keeps its buffer: an SF delivered and kept. */
static void keep_the_buffer(void)
{
    s_node.keep = true;
    TEST_ASSERT_TRUE(sf_in(1U));
    TEST_ASSERT_TRUE(isotp_rx_held(&s_node.tp));
    s_node.keep = false;
}

/* ------------------------------------------------------------------------------------------
 * Timers: N_As, N_Ar, N_Bs, N_Cr
 * ---------------------------------------------------------------------------------------- */

/* N_As from the hand-out of an SF to its confirmation. */
static void test_n_as_runs_out_for_a_single_frame(void)
{
    distinct_timers();
    start();
    send(5U);
    (void)tb_take(&s_node);
    TEST_ASSERT_EQUAL_UINT64(T0 + N_AS, deadline());
    poll_at(T0 + N_AS - 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.conf_count);
    TEST_ASSERT_TRUE(isotp_tx_busy(&s_node.tp));
    poll_at(T0 + N_AS);
    expect_conf(ISOTP_RESULT_TIMEOUT_A);
    TEST_ASSERT_FALSE(isotp_tx_busy(&s_node.tp));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->unconfirmed);
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->outstanding);
    tb_check(&s_node);
    /* A confirmation after the frame was given up matches nothing. */
    TEST_ASSERT_FALSE(tb_confirm(&s_node));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->confirm_unmatched);
    tb_check(&s_node);
}

/* N_As of an FF, and of a CF. */
static void test_n_as_runs_out_for_a_first_and_a_consecutive_frame(void)
{
    distinct_timers();
    start();
    send(20U);
    (void)tb_take(&s_node);
    poll_at(T0 + N_AS - 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.conf_count);
    poll_at(T0 + N_AS);
    expect_conf(ISOTP_RESULT_TIMEOUT_A);

    tx_begin(20U);
    const uint64_t t1 = tb_now_us + 500U;
    tb_now_us = t1;
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    (void)tb_take(&s_node); /* CF 1, never confirmed */
    TEST_ASSERT_EQUAL_UINT64(t1 + N_AS, deadline());
    poll_at(t1 + N_AS - 1U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.conf_count);
    poll_at(t1 + N_AS);
    expect_conf(ISOTP_RESULT_TIMEOUT_A);
    TEST_ASSERT_EQUAL_UINT32(2U, stats()->tx_results[ISOTP_RESULT_TIMEOUT_A]);
    tb_check(&s_node);
}

/* N_Ar from the hand-out of an FC CTS to its confirmation: after the FF, and at a block
 * boundary. */
static void test_n_ar_runs_out_for_a_cts(void)
{
    distinct_timers();
    s_cfg.rx_bs = 1U;
    start();
    TEST_ASSERT_TRUE(ff_in(20U));
    expect_fc(0U, 1U, 0U);
    TEST_ASSERT_EQUAL_UINT64(T0 + N_AR, deadline());
    poll_at(T0 + N_AR - 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.ind_count);
    poll_at(T0 + N_AR);
    expect_ind(ISOTP_RESULT_TIMEOUT_A, 20U);
    TEST_ASSERT_FALSE(isotp_rx_busy(&s_node.tp));

    rx_begin(20U);
    const uint64_t t1 = tb_now_us + 1000U;
    tb_now_us = t1;
    TEST_ASSERT_TRUE(cf_in(20U, 1U)); /* the end of a block of 1: the next CTS goes out */
    expect_fc(0U, 1U, 0U);
    poll_at(t1 + N_AR - 1U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.ind_count);
    poll_at(t1 + N_AR);
    expect_ind(ISOTP_RESULT_TIMEOUT_A, 20U);
    TEST_ASSERT_EQUAL_UINT32(2U, stats()->unconfirmed);
    tb_check(&s_node);
}

/* N_Ar of an FC WAIT. */
static void test_n_ar_runs_out_for_a_wait(void)
{
    distinct_timers();
    s_cfg.rx_wft_max = 1U;
    start();
    keep_the_buffer();
    TEST_ASSERT_TRUE(ff_in(20U));
    poll_at(T0 + N_BR);
    expect_fc(1U, 0U, 0U);
    poll_at(T0 + N_BR + N_AR - 1U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.ind_count); /* the SF's */
    poll_at(T0 + N_BR + N_AR);
    expect_ind(ISOTP_RESULT_TIMEOUT_A, 20U);
    tb_check(&s_node);
}

/* N_Bs from the confirmation of the FF to the arrival of the FC. */
static void test_n_bs_runs_out_after_the_first_frame(void)
{
    distinct_timers();
    start();
    send(20U);
    (void)tb_take(&s_node);
    const uint64_t t1 = T0 + 300U;
    tb_now_us = t1;
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_EQUAL_UINT64(t1 + N_BS, deadline());
    poll_at(t1 + N_BS - 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.conf_count);
    poll_at(t1 + N_BS);
    expect_conf(ISOTP_RESULT_TIMEOUT_BS);
    tb_check(&s_node);
}

/* N_Bs from the confirmation of the last CF of a block. */
static void test_n_bs_runs_out_after_the_last_cf_of_a_block(void)
{
    distinct_timers();
    start();
    tx_begin(40U);
    TEST_ASSERT_TRUE(fc_in(0U, 2U, 0U));
    expect_cf(40U, 1U);
    (void)tb_take(&s_node);
    const uint64_t t2 = tb_now_us + 700U;
    tb_now_us = t2;
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_UINT64(t2 + N_BS, deadline());
    poll_at(t2 + N_BS - 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.conf_count);
    poll_at(t2 + N_BS);
    expect_conf(ISOTP_RESULT_TIMEOUT_BS);
    tb_check(&s_node);
}

/* Every FC WAIT starts N_Bs again. */
static void test_n_bs_restarts_with_every_wait(void)
{
    distinct_timers();
    s_cfg.tx_wft_max = 5U;
    start();
    tx_begin(20U);
    const uint64_t t1 = T0 + N_BS - 100U;
    tb_now_us = t1;
    TEST_ASSERT_TRUE(fc_in(1U, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT64(t1 + N_BS, deadline());
    poll_at(T0 + N_BS);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.conf_count);
    const uint64_t t2 = t1 + N_BS - 1U;
    tb_now_us = t2;
    TEST_ASSERT_TRUE(fc_in(1U, 0U, 0U));
    poll_at(t2 + N_BS - 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.conf_count);
    poll_at(t2 + N_BS);
    expect_conf(ISOTP_RESULT_TIMEOUT_BS);
    TEST_ASSERT_EQUAL_UINT32(2U, stats()->frames_in[ISOTP_FRAME_FC_WAIT]);
    tb_check(&s_node);
}

/* N_Cr from the confirmation of the CTS, then from every CF. */
static void test_n_cr_runs_out_after_the_cts_and_between_cfs(void)
{
    distinct_timers();
    start();
    rx_begin(30U);
    TEST_ASSERT_EQUAL_UINT64(T0 + N_CR, deadline());
    poll_at(T0 + N_CR - 1U);
    TEST_ASSERT_TRUE(cf_in(30U, 1U));
    const uint64_t t1 = tb_now_us;
    TEST_ASSERT_EQUAL_UINT64(t1 + N_CR, deadline());
    poll_at(t1 + N_CR - 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.ind_count);
    poll_at(t1 + N_CR);
    expect_ind(ISOTP_RESULT_TIMEOUT_CR, 30U);
    TEST_ASSERT_FALSE(isotp_rx_busy(&s_node.tp));
    tb_check(&s_node);
}

/* A frame that arrives at the very microsecond its timer runs out is too late: the timer is
 * acted on first. One microsecond earlier it is in time. */
static void test_a_frame_at_the_moment_its_timer_runs_out_is_too_late(void)
{
    distinct_timers();
    start();
    rx_begin(20U);
    tb_now_us = T0 + N_CR;
    TEST_ASSERT_FALSE(cf_in(20U, 1U));
    expect_ind(ISOTP_RESULT_TIMEOUT_CR, 20U);
    expect_ignored(ISOTP_IGNORED_UNEXPECTED_CF, 1U);

    rx_begin(20U);
    const uint64_t t1 = tb_now_us;
    tb_now_us = t1 + N_CR - 1U;
    TEST_ASSERT_TRUE(cf_in(20U, 1U));
    TEST_ASSERT_TRUE(cf_in(20U, 2U));
    expect_ind(ISOTP_RESULT_OK, 20U);

    tx_begin(20U);
    const uint64_t t2 = tb_now_us;
    tb_now_us = t2 + N_BS;
    TEST_ASSERT_FALSE(fc_in(0U, 0U, 0U));
    expect_conf(ISOTP_RESULT_TIMEOUT_BS);
    expect_ignored(ISOTP_IGNORED_UNEXPECTED_FC, 1U);

    tx_begin(20U);
    tb_now_us += N_BS - 1U;
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    expect_cf(20U, 1U);
    expect_cf(20U, 2U);
    expect_conf(ISOTP_RESULT_OK);

    /* A confirmation at the moment N_As runs out is too late as well: the frame was given up. */
    send(5U);
    (void)tb_take(&s_node);
    tb_now_us += N_AS;
    TEST_ASSERT_FALSE(tb_confirm(&s_node));
    expect_conf(ISOTP_RESULT_TIMEOUT_A);
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->confirm_unmatched);
    tb_check(&s_node);
}

/* ------------------------------------------------------------------------------------------
 * Flow control
 * ---------------------------------------------------------------------------------------- */

/* BS from every CTS: 3 CFs, then 1, then all that are left. 100 bytes: 14 CFs. */
static void test_the_block_size_of_every_cts(void)
{
    start();
    tx_begin(100U);
    TEST_ASSERT_TRUE(fc_in(0U, 3U, 0U));
    for (uint32_t i = 1U; i <= 3U; i++) {
        expect_cf(100U, i);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_UINT64(tb_now_us + ISOTP_TIMEOUT_US, deadline()); /* N_Bs */
    TEST_ASSERT_TRUE(fc_in(0U, 1U, 0U));
    expect_cf(100U, 4U);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    for (uint32_t i = 5U; i <= 14U; i++) {
        expect_cf(100U, i);
    }
    expect_conf(ISOTP_RESULT_OK);
    TEST_ASSERT_EQUAL_UINT32(3U, stats()->frames_in[ISOTP_FRAME_FC_CTS]);
    tb_check(&s_node);
}

/* The receiver sends a CTS after every rx_bs CFs, never after the last one. 34 bytes: 4 CFs in
 * two blocks of 2. */
static void test_the_receiver_sends_a_cts_after_every_block(void)
{
    s_cfg.rx_bs = 2U;
    start();
    rx_begin(34U);
    TEST_ASSERT_TRUE(cf_in(34U, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_TRUE(cf_in(34U, 2U));
    expect_fc(0U, 2U, 0U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_TRUE(cf_in(34U, 3U));
    TEST_ASSERT_TRUE(cf_in(34U, 4U));
    expect_ind(ISOTP_RESULT_OK, 34U);
    expect_message(34U);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_UINT32(2U, stats()->frames_out[ISOTP_FRAME_FC_CTS]);
    tb_check(&s_node);
}

/* After a CF's confirmation the next one waits for STmin: not 1 us earlier. */
static void stmin_gap(uint8_t stmin, uint32_t gap_us)
{
    tx_begin(30U);
    TEST_ASSERT_TRUE(fc_in(0U, 0U, stmin));
    (void)tb_take(&s_node); /* CF 1 at once: the FF has no STmin before it */
    tb_now_us += 100U;
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    const uint64_t t1 = tb_now_us;
    TEST_ASSERT_EQUAL_UINT64(t1 + gap_us, deadline());
    poll_at(t1 + gap_us - 1U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, tb_waiting(&s_node), at_value("STmin", stmin));
    poll_at(t1 + gap_us);
    expect_cf(30U, 2U);
    tb_now_us += gap_us;
    isotp_poll(&s_node.tp, tb_now_us);
    expect_cf(30U, 3U);
    tb_now_us += gap_us;
    isotp_poll(&s_node.tp, tb_now_us);
    expect_cf(30U, 4U);
    expect_conf(ISOTP_RESULT_OK);
}

static void test_stmin_in_milliseconds(void)
{
    start();
    stmin_gap(0x01U, 1000U);
    stmin_gap(0x05U, 5000U);
    stmin_gap(0x7FU, 127000U);
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->stmin_reserved);
    tb_check(&s_node);
}

static void test_stmin_in_100_microsecond_steps(void)
{
    start();
    for (uint8_t stmin = 0xF1U; stmin <= 0xF9U; stmin++) {
        stmin_gap(stmin, (uint32_t)(stmin - 0xF0U) * 100U);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->stmin_reserved);
    tb_check(&s_node);
}

/* A reserved STmin is read as the longest, 127 ms, and counted. */
static void test_a_reserved_stmin_means_127_ms(void)
{
    const uint8_t reserved[] = {0x80U, 0xA5U, 0xF0U, 0xFAU, 0xFFU};

    start();
    for (uint32_t i = 0U; i < sizeof(reserved); i++) {
        stmin_gap(reserved[i], 127000U);
    }
    TEST_ASSERT_EQUAL_UINT32(5U, stats()->stmin_reserved);
    tb_check(&s_node);
}

/* STmin 0: the next CF goes out in the call that confirms the one before. */
static void test_stmin_0_sends_the_next_cf_with_the_confirmation(void)
{
    start();
    tx_begin(20U);
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    (void)tb_take(&s_node);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_EQUAL_UINT32(1U, tb_waiting(&s_node));
    expect_cf(20U, 2U);
    tb_check(&s_node);
}

/* STmin also holds between the last CF of a block and the first of the next: counted from the
 * previous CF's confirmation, or from the FC's arrival when the FC came first. */
static void test_stmin_holds_across_a_block_boundary(void)
{
    start();
    tx_begin(40U);
    TEST_ASSERT_TRUE(fc_in(0U, 1U, 10U));
    expect_cf(40U, 1U); /* confirmed at T0 */
    tb_now_us = T0 + 2000U;
    TEST_ASSERT_TRUE(fc_in(0U, 1U, 10U)); /* a fast receiver */
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_UINT64(T0 + 10000U, deadline());
    poll_at(T0 + 9999U);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    poll_at(T0 + 10000U);
    expect_cf(40U, 2U);
    const uint64_t t2 = tb_now_us;
    tb_now_us = t2 + 20000U;
    TEST_ASSERT_TRUE(fc_in(0U, 1U, 10U)); /* a slow one: no wait */
    (void)tb_take(&s_node);               /* CF 3, not confirmed */
    const uint64_t t3 = tb_now_us + 50U;
    tb_now_us = t3;
    TEST_ASSERT_TRUE(fc_in(0U, 1U, 10U)); /* before CF 3's confirmation: it proves CF 3 */
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    poll_at(t3 + 9999U);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_TRUE(tb_confirm(&s_node)); /* CF 3's confirmation: changes nothing */
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    poll_at(t3 + 10000U);
    expect_cf(40U, 4U);
    tb_check(&s_node);
}

/* The sender accepts tx_wft_max WAITs in a row; one more ends the transmission. A CTS starts
 * the count again. */
static void test_the_sender_accepts_tx_wft_max_waits_and_not_one_more(void)
{
    s_cfg.tx_wft_max = 3U;
    start();
    tx_begin(20U);
    for (uint32_t i = 0U; i < 3U; i++) {
        TEST_ASSERT_TRUE(fc_in(1U, 0U, 0U));
        TEST_ASSERT_TRUE(isotp_tx_busy(&s_node.tp));
    }
    TEST_ASSERT_TRUE(fc_in(1U, 0U, 0U));
    expect_conf(ISOTP_RESULT_WFT_OVRN);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));

    tx_begin(40U);
    for (uint32_t i = 0U; i < 3U; i++) {
        TEST_ASSERT_TRUE(fc_in(1U, 0U, 0U));
    }
    TEST_ASSERT_TRUE(fc_in(0U, 1U, 0U));
    expect_cf(40U, 1U);
    for (uint32_t i = 0U; i < 3U; i++) {
        TEST_ASSERT_TRUE(fc_in(1U, 0U, 0U));
    }
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    for (uint32_t i = 2U; i <= 5U; i++) {
        expect_cf(40U, i);
    }
    expect_conf(ISOTP_RESULT_OK);
    TEST_ASSERT_EQUAL_UINT32(10U, stats()->frames_in[ISOTP_FRAME_FC_WAIT]);
    tb_check(&s_node);
}

static void test_with_tx_wft_max_0_the_first_wait_ends_the_transmission(void)
{
    start();
    tx_begin(20U);
    TEST_ASSERT_TRUE(fc_in(1U, 0U, 0U));
    expect_conf(ISOTP_RESULT_WFT_OVRN);
    tb_check(&s_node);
}

/* The receiver with a kept buffer: it waits N_Br, sends WAIT, again N_Br after each WAIT's
 * confirmation, rx_wft_max times; when one more would be needed, the reception ends. */
static void test_the_receiver_sends_n_wftmax_waits_and_not_one_more(void)
{
    distinct_timers();
    s_cfg.rx_wft_max = 3U;
    start();
    keep_the_buffer();
    TEST_ASSERT_TRUE(ff_in(20U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.ff_count);
    TEST_ASSERT_TRUE(isotp_rx_busy(&s_node.tp));
    uint64_t t = T0;
    for (uint32_t i = 0U; i < 3U; i++) {
        TEST_ASSERT_EQUAL_UINT64(t + N_BR, deadline());
        poll_at(t + N_BR - 1U);
        TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
        poll_at(t + N_BR);
        expect_fc(1U, 0U, 0U);
        tb_now_us += 200U;
        TEST_ASSERT_TRUE(tb_confirm(&s_node));
        t = tb_now_us;
    }
    poll_at(t + N_BR - 1U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.ind_count);
    poll_at(t + N_BR);
    expect_ind(ISOTP_RESULT_WFT_OVRN, 20U);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_UINT32(3U, stats()->frames_out[ISOTP_FRAME_FC_WAIT]);
    TEST_ASSERT_FALSE(isotp_rx_busy(&s_node.tp));
    TEST_ASSERT_TRUE(isotp_rx_held(&s_node.tp));
    tb_check(&s_node);
}

static void test_with_n_wftmax_0_a_kept_buffer_ends_the_reception_after_n_br(void)
{
    distinct_timers();
    start();
    keep_the_buffer();
    TEST_ASSERT_TRUE(ff_in(20U));
    poll_at(T0 + N_BR - 1U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.ind_count);
    poll_at(T0 + N_BR);
    expect_ind(ISOTP_RESULT_WFT_OVRN, 20U);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));

    /* With N_Br 0, at once. */
    s_cfg.n_br_us = 0U;
    start();
    keep_the_buffer();
    TEST_ASSERT_TRUE(ff_in(20U));
    expect_ind(ISOTP_RESULT_WFT_OVRN, 20U);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    tb_check(&s_node);
}

/* A release lets the waiting reception go on: the CTS goes out at once, or after the
 * confirmation of a WAIT that is out. */
static void test_a_release_sends_the_cts(void)
{
    distinct_timers();
    s_cfg.rx_wft_max = 2U;
    start();
    keep_the_buffer();
    TEST_ASSERT_TRUE(ff_in(20U));
    tb_now_us = T0 + 1000U;
    TEST_ASSERT_TRUE(isotp_rx_release(&s_node.tp, tb_now_us));
    TEST_ASSERT_FALSE(isotp_rx_held(&s_node.tp));
    expect_fc(0U, 0U, 0U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_TRUE(cf_in(20U, 1U));
    TEST_ASSERT_TRUE(cf_in(20U, 2U));
    expect_ind(ISOTP_RESULT_OK, 20U);
    expect_message(20U); /* the FF's 6 bytes waited in the instance and came first */
    TEST_ASSERT_EQUAL_UINT32(1000U, stats()->n_br_max_us);

    keep_the_buffer();
    TEST_ASSERT_TRUE(ff_in(20U));
    poll_at(tb_now_us + N_BR);
    expect_fc(1U, 0U, 0U); /* a WAIT out, not confirmed */
    TEST_ASSERT_TRUE(isotp_rx_release(&s_node.tp, tb_now_us));
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    tb_now_us += 300U;
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    expect_fc(0U, 0U, 0U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_TRUE(cf_in(20U, 1U));
    TEST_ASSERT_TRUE(cf_in(20U, 2U));
    expect_ind(ISOTP_RESULT_OK, 20U);
    expect_message(20U);

    TEST_ASSERT_FALSE(isotp_rx_release(&s_node.tp, tb_now_us)); /* not kept */
    tb_check(&s_node);
}

/* While the buffer is kept an SF has nowhere to go: dropped, counted. */
static void test_a_single_frame_is_dropped_while_the_buffer_is_kept(void)
{
    start();
    keep_the_buffer();
    TEST_ASSERT_TRUE(sf_in(5U));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->rx_sf_busy);
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.ind_count);
    TEST_ASSERT_TRUE(isotp_rx_release(&s_node.tp, tb_now_us));
    TEST_ASSERT_TRUE(sf_in(5U));
    expect_ind(ISOTP_RESULT_OK, 5U);
    expect_message(5U);
    tb_check(&s_node);
}

/* OVFLW from the receiver ends the transmission, after the FF and at a later block boundary. */
static void test_ovflw_ends_the_transmission(void)
{
    start();
    tx_begin(40U);
    TEST_ASSERT_TRUE(fc_in(2U, 0U, 0U));
    expect_conf(ISOTP_RESULT_BUFFER_OVFLW);
    tx_begin(40U);
    TEST_ASSERT_TRUE(fc_in(0U, 1U, 0U));
    expect_cf(40U, 1U);
    TEST_ASSERT_TRUE(fc_in(2U, 0U, 0U));
    expect_conf(ISOTP_RESULT_BUFFER_OVFLW);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_UINT32(2U, stats()->frames_in[ISOTP_FRAME_FC_OVFLW]);
    tb_check(&s_node);
}

/* ------------------------------------------------------------------------------------------
 * Confirmations and the pipeline
 * ---------------------------------------------------------------------------------------- */

static void test_a_confirmation_with_nothing_out_is_counted(void)
{
    start();
    TEST_ASSERT_FALSE(tb_confirm(&s_node));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->confirm_unmatched);
    tb_check(&s_node);
}

/* Both sides busy: the confirmations are matched in the order the frames went out. */
static void test_confirmations_are_matched_in_order_with_both_sides_busy(void)
{
    distinct_timers();
    start();
    tx_begin(20U);
    TEST_ASSERT_TRUE(ff_in(20U));        /* CTS out: the oldest */
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U)); /* CF 1 out */
    TEST_ASSERT_EQUAL_UINT32(2U, tb_waiting(&s_node));
    expect_fc(0U, 0U, 0U);
    tb_now_us += 100U;
    TEST_ASSERT_TRUE(tb_confirm(&s_node)); /* the CTS: N_Cr runs, CF 2 not yet */
    TEST_ASSERT_EQUAL_UINT32(1U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_UINT64(tb_now_us + N_CR, deadline()); /* before CF 1's N_As */
    expect_cf(20U, 1U);                                     /* its confirmation sends CF 2 */
    expect_cf(20U, 2U);
    expect_conf(ISOTP_RESULT_OK);
    TEST_ASSERT_TRUE(cf_in(20U, 1U));
    TEST_ASSERT_TRUE(cf_in(20U, 2U));
    expect_ind(ISOTP_RESULT_OK, 20U);
    tb_check(&s_node);
}

/* An FC before the confirmation of the FF answers the FF: the CF goes out at once, and the FF's
 * confirmation, when it comes, changes nothing. */
static void test_an_fc_before_the_confirmation_of_the_ff_counts_as_it(void)
{
    start();
    send(20U);
    tb_now_us += 10U;
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT32(2U, tb_waiting(&s_node));
    (void)tb_take(&s_node); /* the FF */
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_EQUAL_UINT32(1U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->confirmed);
    expect_cf(20U, 1U);
    expect_cf(20U, 2U);
    expect_conf(ISOTP_RESULT_OK);
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->outstanding);
    tb_check(&s_node);
}

/* A CF before the confirmation of the CTS answers the CTS: taken, and N_Cr runs from it. The
 * CTS's confirmation changes nothing; if it never comes, the CTS is given up silently. */
static void test_a_cf_before_the_confirmation_of_the_cts_counts_as_it(void)
{
    distinct_timers();
    start();
    TEST_ASSERT_TRUE(ff_in(30U));
    expect_fc(0U, 0U, 0U);
    tb_now_us = T0 + 100U;
    TEST_ASSERT_TRUE(cf_in(30U, 1U));
    TEST_ASSERT_EQUAL_UINT64(T0 + N_AR, deadline()); /* the CTS's give-up comes first */
    tb_now_us = T0 + 200U;
    TEST_ASSERT_TRUE(cf_in(30U, 2U));
    poll_at(T0 + N_AR);
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->unconfirmed);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.ind_count); /* nobody waited for it */
    TEST_ASSERT_EQUAL_UINT64(T0 + 200U + N_CR, deadline());
    TEST_ASSERT_TRUE(cf_in(30U, 3U));
    TEST_ASSERT_TRUE(cf_in(30U, 4U));
    expect_ind(ISOTP_RESULT_OK, 30U);
    expect_message(30U);
    tb_check(&s_node);
}

/* With N_Ar shorter than N_As, a later frame is given up first; the earlier one still takes its
 * confirmation. */
static void test_a_later_frame_can_be_given_up_before_an_earlier_one(void)
{
    distinct_timers();
    start();
    send(5U);                     /* SF out at T0: N_As */
    TEST_ASSERT_TRUE(ff_in(20U)); /* CTS out at T0: N_Ar, shorter */
    poll_at(T0 + N_AR);
    expect_ind(ISOTP_RESULT_TIMEOUT_A, 20U);
    TEST_ASSERT_TRUE(isotp_tx_busy(&s_node.tp));
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->outstanding);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    expect_conf(ISOTP_RESULT_OK);
    tb_check(&s_node);
}

/*
 * The pipeline: frames proven by early FCs fill it (the FF and three CFs of blocks of 1, all
 * handed out, none confirmed). A frame that is due then waits for a confirmation; an OVFLW that
 * is due is dropped by a new FF; the receiver's FC goes before the sender's CF when both are due.
 */
static void test_a_full_pipeline_makes_frames_wait(void)
{
    start_sized(100U);
    send(60U);
    for (uint32_t i = 0U; i < 3U; i++) {
        TEST_ASSERT_TRUE(fc_in(0U, 1U, 0U)); /* proves the newest frame, CF i + 1 goes out */
    }
    TEST_ASSERT_EQUAL_UINT32(ISOTP_PIPELINE_DEPTH, stats()->outstanding);
    TEST_ASSERT_TRUE(fc_in(0U, 1U, 0U)); /* proves CF 3: CF 4 is due, no room */
    TEST_ASSERT_EQUAL_UINT32(4U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_UINT64(T0 + ISOTP_TIMEOUT_US, deadline()); /* only the give-ups */

    TEST_ASSERT_TRUE(ff_in(101U)); /* OVFLW due, no room */
    TEST_ASSERT_TRUE(ff_in(20U));  /* drops it; CTS due, no room */
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->fc_dropped);
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->frames_out[ISOTP_FRAME_FC_OVFLW]);
    TEST_ASSERT_FALSE(cf_in(20U, 1U)); /* the CTS is not out: a CF cannot answer it */
    TEST_ASSERT_EQUAL_UINT32(4U, tb_waiting(&s_node));

    tb_now_us += 500U;
    (void)tb_take_confirm(&s_node); /* the FF: room for one, and the CTS takes it */
    TEST_ASSERT_EQUAL_UINT32(4U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_HEX8(0x30U, s_node.out[(s_node.out_head + 3U) % TB_QUEUE].frame.data[0]);
    TEST_ASSERT_EQUAL_UINT32(500U, stats()->n_br_max_us);
    (void)tb_take_confirm(&s_node); /* CF 1: room for CF 4 */
    TEST_ASSERT_EQUAL_UINT32(4U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_HEX8(0x24U, s_node.out[(s_node.out_head + 3U) % TB_QUEUE].frame.data[0]);
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U)); /* proves CF 4 too: CF 5..8 when there is room */
    for (uint32_t i = 0U; i < 8U; i++) {
        (void)tb_take_confirm(&s_node);
    }
    expect_conf(ISOTP_RESULT_OK);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_TRUE(cf_in(20U, 1U));
    TEST_ASSERT_TRUE(cf_in(20U, 2U));
    expect_ind(ISOTP_RESULT_OK, 20U);
    tb_check(&s_node);
}

/* A proven frame whose confirmation never comes is given up silently when its time has run out,
 * also while its side waits for a later frame; the later frame keeps its own timer. */
static void test_a_proven_frame_that_is_never_confirmed_is_given_up_silently(void)
{
    distinct_timers();
    s_cfg.rx_bs = 1U;
    start();
    send(40U); /* FF out at T0, never confirmed */
    tb_now_us = T0 + 500000U;
    TEST_ASSERT_TRUE(fc_in(0U, 1U, 0U)); /* proves the FF; CF 1 out */
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->proven);
    poll_at(T0 + N_AS); /* the FF is given up; the sender waits for CF 1 */
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->unconfirmed);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.conf_count);
    TEST_ASSERT_TRUE(isotp_tx_busy(&s_node.tp));
    (void)tb_take(&s_node);
    expect_cf(40U, 1U); /* confirmed in time: the block of 1 is done, an FC is waited for */
    TEST_ASSERT_EQUAL_UINT64(tb_now_us + N_BS, deadline());

    const uint64_t t1 = tb_now_us;
    TEST_ASSERT_TRUE(ff_in(20U)); /* CTS 1 out at t1, never confirmed */
    (void)tb_take(&s_node);
    tb_now_us = t1 + 100U;
    TEST_ASSERT_TRUE(cf_in(20U, 1U)); /* proves CTS 1, ends a block of 1: CTS 2 out */
    TEST_ASSERT_EQUAL_UINT32(2U, stats()->proven);
    (void)tb_take(&s_node);
    poll_at(t1 + N_AR); /* CTS 1 given up; the receiver waits for CTS 2 */
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.ind_count);
    TEST_ASSERT_TRUE(isotp_rx_busy(&s_node.tp));
    poll_at(t1 + 100U + N_AR); /* CTS 2's own N_Ar */
    expect_ind(ISOTP_RESULT_TIMEOUT_A, 20U);
    TEST_ASSERT_EQUAL_UINT32(3U, stats()->unconfirmed);
    tb_check(&s_node);
}

/* An FF proven by an early WAIT and never confirmed is given up silently while the sender waits
 * for the next FC; the transmission goes on when the CTS comes. */
static void test_a_proven_ff_is_given_up_silently_while_the_sender_waits(void)
{
    distinct_timers();
    s_cfg.tx_wft_max = 1U;
    start();
    send(20U); /* FF out at T0, never confirmed */
    tb_now_us = T0 + 400000U;
    TEST_ASSERT_TRUE(fc_in(1U, 0U, 0U)); /* WAIT: proves the FF, N_Bs from now */
    TEST_ASSERT_EQUAL_UINT64(T0 + N_AS, deadline());
    poll_at(T0 + N_AS);
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->unconfirmed);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.conf_count);
    TEST_ASSERT_EQUAL_UINT64(T0 + 400000U + N_BS, deadline());
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    (void)tb_take(&s_node); /* the FF */
    expect_cf(20U, 1U);
    expect_cf(20U, 2U);
    expect_conf(ISOTP_RESULT_OK);
    tb_check(&s_node);
}

/* Fill the pipeline with frames of the sender proven by early FCs: the FF and CFs 1..3 of
 * blocks of 1 are out, none confirmed. */
static void fill_the_pipeline(void)
{
    send(60U);
    for (uint32_t i = 0U; i < 3U; i++) {
        TEST_ASSERT_TRUE(fc_in(0U, 1U, 0U));
    }
    TEST_ASSERT_EQUAL_UINT32(ISOTP_PIPELINE_DEPTH, stats()->outstanding);
}

/* A WAIT that is due but not out (no room) is not needed once the buffer is released: the CTS
 * goes out in its place. An OVFLW that is due stays an OVFLW. */
static void test_a_release_while_an_fc_is_due_but_not_out(void)
{
    s_cfg.rx_wft_max = 1U;
    start_sized(100U);
    keep_the_buffer();
    fill_the_pipeline();
    TEST_ASSERT_TRUE(ff_in(20U));
    poll_at(tb_now_us + s_cfg.n_br_us); /* the WAIT is due, no room */
    TEST_ASSERT_TRUE(isotp_rx_busy(&s_node.tp));
    TEST_ASSERT_TRUE(isotp_rx_release(&s_node.tp, tb_now_us));
    TEST_ASSERT_EQUAL_UINT32(4U, tb_waiting(&s_node));
    (void)tb_take_confirm(&s_node); /* the FF: room, and the CTS takes it */
    TEST_ASSERT_EQUAL_HEX8(0x30U, s_node.out[(s_node.out_head + 3U) % TB_QUEUE].frame.data[0]);
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->frames_out[ISOTP_FRAME_FC_WAIT]);
    tb_check(&s_node);

    start_sized(100U);
    keep_the_buffer();
    fill_the_pipeline();
    TEST_ASSERT_TRUE(ff_in(101U)); /* OVFLW due, no room */
    TEST_ASSERT_FALSE(isotp_rx_busy(&s_node.tp));
    TEST_ASSERT_TRUE(isotp_rx_release(&s_node.tp, tb_now_us));
    (void)tb_take_confirm(&s_node);
    TEST_ASSERT_EQUAL_HEX8(0x32U, s_node.out[(s_node.out_head + 3U) % TB_QUEUE].frame.data[0]);
    tb_check(&s_node);
}

/* N_Cs and N_Br saturate at UINT32_MAX microseconds (71 minutes). */
static void test_the_measurements_saturate(void)
{
    start();
    tx_begin(20U);
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 1U));
    expect_cf(20U, 1U);
    poll_at(tb_now_us + 5000000000U);
    expect_cf(20U, 2U);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, stats()->n_cs_max_us);
    tb_check(&s_node);
}

/* A frame that send_frame refuses ends its side at once with N_ERROR; an OVFLW is just not
 * sent. */
static void test_a_refused_frame_ends_its_side_with_n_error(void)
{
    s_cfg.rx_wft_max = 1U;
    start_sized(100U);
    s_node.refuse = true;
    send(5U);
    expect_conf(ISOTP_RESULT_ERROR);
    TEST_ASSERT_FALSE(isotp_tx_busy(&s_node.tp));
    send(20U);
    expect_conf(ISOTP_RESULT_ERROR);
    s_node.refuse = false;
    tx_begin(20U);
    s_node.refuse = true;
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    expect_conf(ISOTP_RESULT_ERROR);

    TEST_ASSERT_TRUE(ff_in(20U));
    expect_ind(ISOTP_RESULT_ERROR, 20U);
    TEST_ASSERT_FALSE(isotp_rx_busy(&s_node.tp));
    TEST_ASSERT_TRUE(ff_in(101U)); /* OVFLW refused: not sent, no indication */
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->fc_dropped);
    TEST_ASSERT_EQUAL_UINT32(1U, s_node.ind_count);
    TEST_ASSERT_FALSE(isotp_rx_busy(&s_node.tp));
    s_node.refuse = false;
    keep_the_buffer();
    s_node.refuse = true;
    TEST_ASSERT_TRUE(ff_in(20U));
    poll_at(tb_now_us + s_cfg.n_br_us); /* the WAIT is refused */
    expect_ind(ISOTP_RESULT_ERROR, 20U);
    TEST_ASSERT_EQUAL_UINT32(6U, stats()->send_refused);
    TEST_ASSERT_EQUAL_UINT32(3U, stats()->tx_results[ISOTP_RESULT_ERROR]);
    TEST_ASSERT_EQUAL_UINT32(2U, stats()->rx_results[ISOTP_RESULT_ERROR]);
    tb_check(&s_node);
}

/* ------------------------------------------------------------------------------------------
 * Callbacks and statistics
 * ---------------------------------------------------------------------------------------- */

/* Every mutating call from inside any callback is refused and counted; the exchange is not
 * disturbed by the attempts. */
static void test_callbacks_cannot_call_back_into_the_instance(void)
{
    s_cfg.rx_wft_max = 1U;
    start();
    s_node.probe = true;
    send(5U);
    (void)tb_take_confirm(&s_node);
    tx_begin(20U);
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 0U));
    expect_cf(20U, 1U);
    expect_cf(20U, 2U);
    rx_begin(20U);
    TEST_ASSERT_TRUE(cf_in(20U, 1U));
    TEST_ASSERT_TRUE(cf_in(20U, 2U));
    expect_ind(ISOTP_RESULT_OK, 20U);
    expect_message(20U);
    TEST_ASSERT_TRUE(sf_in(3U));
    expect_message(3U);
    TEST_ASSERT_TRUE(s_node.probes_refused >= 40U);
    tb_check(&s_node);
}

/* The indication keeps the buffer when it returns true, gives it back when it returns false. */
static void test_the_indication_keeps_or_gives_back_the_buffer(void)
{
    start();
    TEST_ASSERT_TRUE(sf_in(2U));
    TEST_ASSERT_FALSE(isotp_rx_held(&s_node.tp));
    s_node.keep = true;
    TEST_ASSERT_TRUE(sf_in(2U));
    TEST_ASSERT_TRUE(isotp_rx_held(&s_node.tp));
    TEST_ASSERT_TRUE(isotp_rx_release(&s_node.tp, tb_now_us));
    TEST_ASSERT_FALSE(isotp_rx_held(&s_node.tp));
    tb_check(&s_node);
}

/* N_Cs: from a CF's confirmation to the next CF's hand-out. A late call makes a late CF. */
static void test_n_cs_is_measured_and_late_cfs_are_counted(void)
{
    s_cfg.n_cs_us = 1000U;
    start();
    tx_begin(30U);
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 1U));
    expect_cf(30U, 1U); /* 0 after the FC */
    poll_at(tb_now_us + 1000U);
    expect_cf(30U, 2U); /* exactly the target: not late */
    poll_at(tb_now_us + 1500U);
    expect_cf(30U, 3U); /* late */
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->n_cs_late);
    TEST_ASSERT_EQUAL_UINT32(1500U, stats()->n_cs_max_us);
    tb_check(&s_node);
}

/* N_Br: WAITs on their schedule are on time; a late call makes a late FC. */
static void test_n_br_is_measured_and_late_fcs_are_counted(void)
{
    distinct_timers();
    s_cfg.rx_wft_max = 2U;
    start();
    keep_the_buffer();
    TEST_ASSERT_TRUE(ff_in(20U));
    poll_at(T0 + N_BR);
    expect_fc(1U, 0U, 0U);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    TEST_ASSERT_EQUAL_UINT32(0U, stats()->n_br_late);
    TEST_ASSERT_EQUAL_UINT32(N_BR, stats()->n_br_max_us);
    poll_at(tb_now_us + N_BR + 10000U);
    expect_fc(1U, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, stats()->n_br_late);
    TEST_ASSERT_EQUAL_UINT32(N_BR + 10000U, stats()->n_br_max_us);
    tb_check(&s_node);
}

static void test_the_names(void)
{
    const char *const results[] = {"OK",           "TIMEOUT_A",  "TIMEOUT_BS", "TIMEOUT_CR",
                                   "WRONG_SN",     "INVALID_FS", "UNEXP_PDU",  "WFT_OVRN",
                                   "BUFFER_OVFLW", "ERROR"};
    const char *const kinds[] = {"SF", "FF", "CF", "FC_CTS", "FC_WAIT", "FC_OVFLW", "FC_RESERVED"};
    const char *const reasons[] = {"FOREIGN",       "SHORT",         "LENGTH",
                                   "RESERVED_TYPE", "UNEXPECTED_CF", "UNEXPECTED_FC"};

    TEST_ASSERT_EQUAL_UINT32(ISOTP_RESULTS, sizeof(results) / sizeof(results[0]));
    TEST_ASSERT_EQUAL_UINT32(ISOTP_FRAME_KINDS, sizeof(kinds) / sizeof(kinds[0]));
    TEST_ASSERT_EQUAL_UINT32(ISOTP_IGNORED_REASONS, sizeof(reasons) / sizeof(reasons[0]));
    for (uint32_t i = 0U; i < ISOTP_RESULTS; i++) {
        TEST_ASSERT_EQUAL_STRING(results[i], isotp_result_name((isotp_result_t)i));
    }
    for (uint32_t i = 0U; i < ISOTP_FRAME_KINDS; i++) {
        TEST_ASSERT_EQUAL_STRING(kinds[i], isotp_frame_kind_name((isotp_frame_kind_t)i));
    }
    for (uint32_t i = 0U; i < ISOTP_IGNORED_REASONS; i++) {
        TEST_ASSERT_EQUAL_STRING(reasons[i], isotp_ignored_name((isotp_ignored_t)i));
    }
    TEST_ASSERT_EQUAL_STRING("?", isotp_result_name(ISOTP_RESULTS));
    TEST_ASSERT_EQUAL_STRING("?", isotp_frame_kind_name(ISOTP_FRAME_KINDS));
    TEST_ASSERT_EQUAL_STRING("?", isotp_ignored_name(ISOTP_IGNORED_REASONS));
}

/* Near the end of the clock a timer that would run out beyond it never runs out. */
static void test_timers_near_the_end_of_the_clock(void)
{
    start();
    tb_now_us = UINT64_MAX - 10U;
    send(5U);
    (void)tb_take(&s_node);
    TEST_ASSERT_EQUAL_UINT64(ISOTP_NO_DEADLINE, deadline());
    poll_at(UINT64_MAX - 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_node.conf_count);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
    expect_conf(ISOTP_RESULT_OK);
}

/* A call with an earlier time than the one before (two readings of a clock that crossed) fires
 * nothing early and measures nothing negative. */
static void test_a_clock_read_out_of_order_fires_nothing_early(void)
{
    s_cfg.n_cs_us = 0U;
    start();
    tx_begin(20U);
    TEST_ASSERT_TRUE(fc_in(0U, 0U, 1U));
    tb_now_us = T0 + 500U;
    (void)tb_take(&s_node);
    TEST_ASSERT_TRUE(tb_confirm(&s_node)); /* CF 2 due at T0 + 1500 */
    poll_at(T0 + 100U);
    TEST_ASSERT_EQUAL_UINT32(0U, tb_waiting(&s_node));
    TEST_ASSERT_EQUAL_UINT32(2U, s_node.handed_out); /* FF and CF 1 only */
    TEST_ASSERT_EQUAL_UINT64(T0 + 1500U, deadline());
    poll_at(T0 + 1500U);
    expect_cf(20U, 2U);
    TEST_ASSERT_EQUAL_UINT32(1000U, stats()->n_cs_max_us);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_n_as_runs_out_for_a_single_frame);
    RUN_TEST(test_n_as_runs_out_for_a_first_and_a_consecutive_frame);
    RUN_TEST(test_n_ar_runs_out_for_a_cts);
    RUN_TEST(test_n_ar_runs_out_for_a_wait);
    RUN_TEST(test_n_bs_runs_out_after_the_first_frame);
    RUN_TEST(test_n_bs_runs_out_after_the_last_cf_of_a_block);
    RUN_TEST(test_n_bs_restarts_with_every_wait);
    RUN_TEST(test_n_cr_runs_out_after_the_cts_and_between_cfs);
    RUN_TEST(test_a_frame_at_the_moment_its_timer_runs_out_is_too_late);
    RUN_TEST(test_the_block_size_of_every_cts);
    RUN_TEST(test_the_receiver_sends_a_cts_after_every_block);
    RUN_TEST(test_stmin_in_milliseconds);
    RUN_TEST(test_stmin_in_100_microsecond_steps);
    RUN_TEST(test_a_reserved_stmin_means_127_ms);
    RUN_TEST(test_stmin_0_sends_the_next_cf_with_the_confirmation);
    RUN_TEST(test_stmin_holds_across_a_block_boundary);
    RUN_TEST(test_the_sender_accepts_tx_wft_max_waits_and_not_one_more);
    RUN_TEST(test_with_tx_wft_max_0_the_first_wait_ends_the_transmission);
    RUN_TEST(test_the_receiver_sends_n_wftmax_waits_and_not_one_more);
    RUN_TEST(test_with_n_wftmax_0_a_kept_buffer_ends_the_reception_after_n_br);
    RUN_TEST(test_a_release_sends_the_cts);
    RUN_TEST(test_a_single_frame_is_dropped_while_the_buffer_is_kept);
    RUN_TEST(test_ovflw_ends_the_transmission);
    RUN_TEST(test_a_confirmation_with_nothing_out_is_counted);
    RUN_TEST(test_confirmations_are_matched_in_order_with_both_sides_busy);
    RUN_TEST(test_an_fc_before_the_confirmation_of_the_ff_counts_as_it);
    RUN_TEST(test_a_cf_before_the_confirmation_of_the_cts_counts_as_it);
    RUN_TEST(test_a_later_frame_can_be_given_up_before_an_earlier_one);
    RUN_TEST(test_a_full_pipeline_makes_frames_wait);
    RUN_TEST(test_a_proven_frame_that_is_never_confirmed_is_given_up_silently);
    RUN_TEST(test_a_proven_ff_is_given_up_silently_while_the_sender_waits);
    RUN_TEST(test_a_release_while_an_fc_is_due_but_not_out);
    RUN_TEST(test_the_measurements_saturate);
    RUN_TEST(test_a_refused_frame_ends_its_side_with_n_error);
    RUN_TEST(test_callbacks_cannot_call_back_into_the_instance);
    RUN_TEST(test_the_indication_keeps_or_gives_back_the_buffer);
    RUN_TEST(test_n_cs_is_measured_and_late_cfs_are_counted);
    RUN_TEST(test_n_br_is_measured_and_late_fcs_are_counted);
    RUN_TEST(test_the_names);
    RUN_TEST(test_timers_near_the_end_of_the_clock);
    RUN_TEST(test_a_clock_read_out_of_order_fires_nothing_early);
    return UNITY_END();
}
