/* SPDX-License-Identifier: MIT */
/*
 * The test as the peer of one lib/isotp instance, shared by test_isotp.c and test_isotp_flow.c:
 * the instance under test (s_node, the node: it sends 0x7E8 and accepts 0x7E0), its
 * configuration (s_cfg, set up by setUp() before each test), the message data (s_data: byte i is
 * (i * 7 + 3) mod 256, so that a byte from the wrong offset shows), and helpers that inject the
 * peer's frames and take, check and confirm the node's. The helpers are static inline so that
 * each test file compiles the ones it uses.
 */
#ifndef FCN_ISOTP_TEST_NODE_H
#define FCN_ISOTP_TEST_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "isotp/isotp.h"
#include "isotp_test_bus.h"
#include "unity.h"

#define T0 1000000U /* the clock at the start of every test, us */
#define CC 0xCCU    /* padding */

static tb_end_t s_node;
static isotp_config_t s_cfg;
static uint8_t s_data[TB_MAX_LENGTH];

/* What setUp() does in both files. */
static inline void node_setup(void)
{
    tb_now_us = T0;
    s_cfg = tb_config(TB_NODE_TX, TB_NODE_RX);
    for (uint32_t i = 0U; i < TB_MAX_LENGTH; i++) {
        s_data[i] = (uint8_t)((i * 7U) + 3U);
    }
}

static inline const char *at_value(const char *what, unsigned long value)
{
    static char text[80];

    (void)snprintf(text, sizeof(text), "%s %lu", what, value);
    return text;
}

static inline void start(void)
{
    tb_start(&s_node, &s_cfg, ISOTP_FF_DL_12BIT_MAX);
}

static inline void start_sized(uint32_t size)
{
    tb_start(&s_node, &s_cfg, size);
}

static inline const isotp_stats_t *stats(void)
{
    return isotp_stats(&s_node.tp);
}

static inline void poll_at(uint64_t t_us)
{
    tb_now_us = t_us;
    isotp_poll(&s_node.tp, tb_now_us);
}

static inline void send(uint32_t length)
{
    TEST_ASSERT_EQUAL(ISOTP_OK, isotp_send(&s_node.tp, s_data, length, tb_now_us));
}

static inline bool inject(uint8_t b0, uint8_t b1, uint8_t b2)
{
    const uint8_t bytes[3] = {b0, b1, b2};

    return tb_inject8(&s_node, 3U, bytes);
}

static inline bool fc_in(uint8_t fs, uint8_t bs, uint8_t stmin)
{
    const uint8_t bytes[8] = {(uint8_t)(0x30U | fs), bs, stmin, CC, CC, CC, CC, CC};

    return s_cfg.can_fd ? tb_inject_fd(&s_node, 8U, bytes)
                        : inject((uint8_t)(0x30U | fs), bs, stmin);
}

/* An FF from the peer for a classical 8-byte message of `length` (<= 4095) bytes of s_data. */
static inline bool ff_in(uint32_t length)
{
    const uint8_t bytes[8] = {(uint8_t)(0x10U | (length >> 8U)),
                              (uint8_t)(length & 0xFFU),
                              s_data[0],
                              s_data[1],
                              s_data[2],
                              s_data[3],
                              s_data[4],
                              s_data[5]};

    return tb_inject(&s_node, 8U, bytes);
}

/* CF number `index` (1 for the first) of a classical 8-byte message of `length` bytes. */
static inline bool cf_in(uint32_t length, uint32_t index)
{
    const uint32_t offset = 6U + ((index - 1U) * 7U);
    uint32_t count = length - offset;
    uint8_t bytes[8];

    if (count > 7U) {
        count = 7U;
    }
    bytes[0] = (uint8_t)(0x20U | (index & 0x0FU));
    (void)memcpy(&bytes[1], &s_data[offset], count);
    return tb_inject8(&s_node, (uint8_t)(1U + count), bytes);
}

static inline bool sf_in(uint8_t length)
{
    uint8_t bytes[8];

    bytes[0] = length;
    (void)memcpy(&bytes[1], s_data, length);
    return tb_inject8(&s_node, (uint8_t)(1U + length), bytes);
}

static inline void expect_fc(uint8_t fs, uint8_t bs, uint8_t stmin)
{
    const uint8_t expected[8] = {(uint8_t)(0x30U | fs), bs, stmin, CC, CC, CC, CC, CC};

    tb_take_expect(&s_node, expected);
}

/* The node starts receiving a classical message of `length` bytes: FF in, FC CTS confirmed. */
static inline void rx_begin(uint32_t length)
{
    TEST_ASSERT_TRUE(ff_in(length));
    expect_fc(0U, s_cfg.rx_bs, s_cfg.rx_stmin);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
}

/* The node starts sending a message of `length` bytes: FF out and confirmed. */
static inline void tx_begin(uint32_t length)
{
    send(length);
    (void)tb_take_confirm(&s_node);
}

/* The node's classical CF number `index` of a message of `length` bytes: checked & confirmed. */
static inline void expect_cf(uint32_t length, uint32_t index)
{
    const uint32_t offset = 6U + ((index - 1U) * 7U);
    uint32_t count = length - offset;
    uint8_t expected[8];

    if (count > 7U) {
        count = 7U;
    }
    (void)memset(expected, CC, sizeof(expected));
    expected[0] = (uint8_t)(0x20U | (index & 0x0FU));
    (void)memcpy(&expected[1], &s_data[offset], count);
    tb_take_expect(&s_node, expected);
    TEST_ASSERT_TRUE(tb_confirm(&s_node));
}

static inline void expect_ind(isotp_result_t result, uint32_t length)
{
    const tb_event_t event = tb_last_ind(&s_node);

    TEST_ASSERT_EQUAL_STRING(isotp_result_name(result), isotp_result_name(event.result));
    TEST_ASSERT_EQUAL_UINT32(length, event.length);
}

static inline void expect_conf(isotp_result_t result)
{
    TEST_ASSERT_EQUAL_STRING(isotp_result_name(result),
                             isotp_result_name(tb_last_conf(&s_node).result));
}

static inline void expect_message(uint32_t length)
{
    TEST_ASSERT_EQUAL_UINT32(length, s_node.message_length);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_data, s_node.message, length);
}

static inline void expect_ignored(isotp_ignored_t reason, uint32_t count)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(count, stats()->ignored[reason], isotp_ignored_name(reason));
}

#endif /* FCN_ISOTP_TEST_NODE_H */
