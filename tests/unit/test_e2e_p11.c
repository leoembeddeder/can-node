/* SPDX-License-Identifier: MIT */
/*
 * E2E Profile 11, variant 11A, in the order of its header: the published test vectors, the
 * CRC, the sender's counter, the receiver's verdicts and its counter, the start after
 * e2e_p11_check_init(), every input error, and the mapping to the state machine.
 *
 * Where the expected bytes come from:
 *
 *   - AUTOSAR_FO_PRS_E2EProtocol R24-11, Tables 6.48 to 6.52: the published vectors, Data ID
 *     0x123 and an all-zero payload. Four of the five are run here. The fifth puts the CRC
 *     into byte 8 of a 16-byte message, a layout that variant 11A on classical CAN does not
 *     have; tests/python/test_e2e.py runs it on the independent Python implementation, which
 *     the C code is compared with on random messages in the same file.
 *   - The receiver frames in s_frames[] and the frames marked "fcn_tools.e2e" were computed
 *     with that Python implementation (tools/fcn_tools/e2e.py), not with the code under test.
 *     s_frames[0] and s_frames[1] are the published vectors.
 *   - 0x37 is the check value of CRC-8/GSM-A in Greg Cook's catalogue of CRC algorithms: the
 *     CRC that the profile computes in effect (see compute_crc() in e2e_p11.c).
 *
 * Statuses are compared by name, so that a failure says "expected OK, was REPEATED".
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crc/crc.h"
#include "e2e/e2e_p11.h"
#include "unity.h"

/* The configuration of the published vectors: Data ID 0x123, 8 bytes. */
static const e2e_p11_config_t s_config = {
    .data_id = 0x0123U, .data_len = 8U, .max_delta_counter = 1U};

/* One message of s_config's Data ID with an all-zero payload for every counter value 0..15
 * (15 is the reserved one): byte 0 and byte 1. Computed with fcn_tools.e2e. */
static const uint8_t s_frames[16][2] = {
    {0xCCU, 0x00U}, {0x91U, 0x01U}, {0x76U, 0x02U}, {0x2BU, 0x03U}, {0xA5U, 0x04U}, {0xF8U, 0x05U},
    {0x1FU, 0x06U}, {0x42U, 0x07U}, {0x1EU, 0x08U}, {0x43U, 0x09U}, {0xA4U, 0x0AU}, {0xF9U, 0x0BU},
    {0x77U, 0x0CU}, {0x2AU, 0x0DU}, {0xCDU, 0x0EU}, {0x90U, 0x0FU},
};

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
    s_random_state = 0xE2E11A11U;
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------------------- */

/* Message for an assertion inside a loop: which case failed. */
static const char *at(const char *what, unsigned long value)
{
    static char text[64];

    (void)snprintf(text, sizeof(text), "%s %lu", what, value);
    return text;
}

static const char *status_name(e2e_p11_status_t status)
{
    const char *name;

    switch (status) {
    case E2E_P11STATUS_OK:
        name = "OK";
        break;
    case E2E_P11STATUS_NONEWDATA:
        name = "NONEWDATA";
        break;
    case E2E_P11STATUS_ERROR:
        name = "ERROR";
        break;
    case E2E_P11STATUS_REPEATED:
        name = "REPEATED";
        break;
    case E2E_P11STATUS_OKSOMELOST:
        name = "OKSOMELOST";
        break;
    case E2E_P11STATUS_WRONGSEQUENCE:
        name = "WRONGSEQUENCE";
        break;
    default:
        name = "(not a status)";
        break;
    }

    return name;
}

#define ASSERT_STATUS(expected, actual)                                                            \
    TEST_ASSERT_EQUAL_STRING(status_name(expected), status_name(actual))
#define ASSERT_STATUS_MESSAGE(expected, actual, message)                                           \
    TEST_ASSERT_EQUAL_STRING_MESSAGE(status_name(expected), status_name(actual), message)

static e2e_p11_config_t config_with(uint16_t data_id, uint8_t data_len, uint8_t max_delta)
{
    const e2e_p11_config_t config = {
        .data_id = data_id, .data_len = data_len, .max_delta_counter = max_delta};

    return config;
}

/* The 8-byte message of s_frames[] with this counter. */
static void frame_of(uint8_t counter, uint8_t *data)
{
    (void)memset(data, 0, 8U);
    data[0] = s_frames[counter][0];
    data[1] = s_frames[counter][1];
}

/*
 * The CRC of a message as the PRS words it (PRS_E2E_00505, Fig. 6.97): the bytes Data ID low,
 * Data ID high, then the message without byte 0, laid out one after the other and run through
 * one CRC-8 with polynomial 0x1D, start 0x00 and no final XOR. tests/unit/test_crc.c checks
 * crc8_j1850_update() itself. Used to build messages the sender cannot make, such as one with
 * the reserved counter 15.
 */
static uint8_t reference_crc(uint16_t data_id, const uint8_t *data, size_t length)
{
    uint8_t stream[2U + E2E_P11_MAX_DATA_LEN];

    TEST_ASSERT_TRUE(length >= 1U);
    TEST_ASSERT_TRUE(length <= E2E_P11_MAX_DATA_LEN);
    stream[0] = (uint8_t)(data_id & 0xFFU);
    stream[1] = (uint8_t)(data_id >> 8U);
    (void)memcpy(&stream[2], &data[1], length - 1U);

    return crc8_j1850_update(0x00U, stream, (length - 1U) + 2U);
}

/* A message of s_config's length with this counter and payload byte pattern, and a correct
 * CRC by reference_crc(). */
static void make_frame(uint16_t data_id, uint8_t counter, uint8_t fill, uint8_t *data)
{
    (void)memset(data, fill, 8U);
    data[1] = (uint8_t)((fill & 0xF0U) | (counter & 0x0FU));
    data[0] = reference_crc(data_id, data, 8U);
}

/* One check of `data` (NULL: no new data) from a receiver whose stored counter is `stored`.
 * Asserts E2E_E_OK and returns the status; the counter afterwards goes to `*stored_after`. */
static e2e_p11_status_t check_from(const e2e_p11_config_t *config, uint8_t stored,
                                   const uint8_t *data, uint8_t *stored_after)
{
    e2e_p11_check_state_t state = {.counter = stored, .status = E2E_P11STATUS_ERROR};
    const size_t length = (data != NULL) ? (size_t)config->data_len : 0U;

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(config, &state, data, length));
    *stored_after = state.counter;
    return state.status;
}

/* ------------------------------------------------------------------------------------------
 * Published vectors
 * ---------------------------------------------------------------------------------------- */

/* PRS Tables 6.48 and 6.49 (variant 11A, DataIDMode BOTH): the first two protect calls of a
 * sender after E2E_P11ProtectInit, all-zero payload. */
static void test_published_vectors_of_variant_11a(void)
{
    static const uint8_t expected_counter_0[8] = {0xCCU, 0x00U, 0x00U, 0x00U,
                                                  0x00U, 0x00U, 0x00U, 0x00U};
    static const uint8_t expected_counter_1[8] = {0x91U, 0x01U, 0x00U, 0x00U,
                                                  0x00U, 0x00U, 0x00U, 0x00U};
    e2e_p11_protect_state_t sender = {.counter = 0xA5U};
    uint8_t data[8] = {0U};

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect_init(&sender));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&s_config, &sender, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_counter_0, data, 8);

    (void)memset(data, 0, sizeof(data));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&s_config, &sender, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_counter_1, data, 8);
}

/*
 * PRS Tables 6.50 and 6.51 are variant 11C (DataIDMode NIBBLE), which this module does not
 * implement as such. It does not need to for these two: in NIBBLE mode the CRC runs over the
 * Data ID low byte, then a 0 byte, then the message (PRS_E2E_00506), and the message carries
 * the low nibble of the Data ID high byte in the high nibble of byte 1 (PRS_E2E_00541). With
 * Data ID 0x123 that is the CRC of 11A over Data ID 0x0023 and a message whose byte 1 has 1 in
 * its high nibble, where 11A has payload. The frames on the bus are the published ones, bit
 * for bit.
 */
static void test_published_vectors_of_variant_11c_as_the_equal_11a_message(void)
{
    static const uint8_t expected_counter_0[8] = {0x2AU, 0x10U, 0x00U, 0x00U,
                                                  0x00U, 0x00U, 0x00U, 0x00U};
    static const uint8_t expected_counter_1[8] = {0x77U, 0x11U, 0x00U, 0x00U,
                                                  0x00U, 0x00U, 0x00U, 0x00U};
    const e2e_p11_config_t nibble_as_both = config_with(0x0023U, 8U, 1U);
    e2e_p11_protect_state_t sender;
    uint8_t data[8] = {0x00U, 0x10U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect_init(&sender));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&nibble_as_both, &sender, data, 8U));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_counter_0, data, 8);

    data[0] = 0x00U;
    data[1] = 0x10U;
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&nibble_as_both, &sender, data, 8U));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_counter_1, data, 8);
}

/* The published frames are accepted by the receiver: the first one straight after
 * e2e_p11_check_init(), the second one after it. */
static void test_published_vectors_are_accepted_in_order(void)
{
    e2e_p11_check_state_t receiver;
    uint8_t data[8];

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check_init(&receiver));

    frame_of(0U, data);
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&s_config, &receiver, data, 8U));
    ASSERT_STATUS(E2E_P11STATUS_OK, receiver.status);

    frame_of(1U, data);
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&s_config, &receiver, data, 8U));
    ASSERT_STATUS(E2E_P11STATUS_OK, receiver.status);
    TEST_ASSERT_EQUAL_UINT8(1U, receiver.counter);
}

/* ------------------------------------------------------------------------------------------
 * The CRC
 * ---------------------------------------------------------------------------------------- */

/*
 * The CRC is CRC-8/GSM-A (polynomial 0x1D, start 0x00, no final XOR) over Data ID low byte,
 * Data ID high byte, bytes 1..7. Chosen so that this is the catalogue's message "123456789":
 * Data ID 0x3231 is '1' then '2', and byte 1 is '3' = 0x33, counter 3 in the low nibble and 3
 * as payload in the high one. The check value is 0x37. A CRC with the catalogue parameters of
 * CRC-8/SAE-J1850 (start 0xFF, final XOR 0xFF) would give 0x4B.
 */
static void test_crc_is_crc8_gsm_a_over_data_id_and_message(void)
{
    const e2e_p11_config_t config = config_with(0x3231U, 8U, 1U);
    e2e_p11_protect_state_t sender = {.counter = 3U};
    uint8_t data[8] = {0x00U, 0x30U, '4', '5', '6', '7', '8', '9'};

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&config, &sender, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8(0x33U, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x37U, data[0]);
}

/* The Data ID enters low byte first (PRS_E2E_00505). 0x05A3 and its byte-swapped twin 0xA305
 * give different frames; the expected one was computed with fcn_tools.e2e. The stale 0xAA in
 * byte 0 is overwritten and does not enter the CRC. */
static void test_crc_takes_the_data_id_low_byte_first(void)
{
    static const uint8_t expected[8] = {0xE7U, 0x37U, 0xDEU, 0xADU, 0xBEU, 0xEFU, 0x01U, 0x02U};
    const e2e_p11_config_t config = config_with(0x05A3U, 8U, 1U);
    const e2e_p11_config_t swapped = config_with(0xA305U, 8U, 1U);
    e2e_p11_protect_state_t sender = {.counter = 7U};
    uint8_t data[8] = {0xAAU, 0x30U, 0xDEU, 0xADU, 0xBEU, 0xEFU, 0x01U, 0x02U};
    uint8_t twin[8];

    (void)memcpy(twin, data, sizeof(twin));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&config, &sender, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, data, 8);

    sender.counter = 7U;
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&swapped, &sender, twin, sizeof(twin)));
    TEST_ASSERT_NOT_EQUAL(expected[0], twin[0]);
}

/* Whatever byte 0 held before, protect writes the same CRC: byte 0 is not covered. */
static void test_crc_does_not_cover_byte_0(void)
{
    uint8_t reference[8] = {0x00U, 0x50U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U};
    e2e_p11_protect_state_t sender = {.counter = 5U};

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&s_config, &sender, reference, 8U));

    for (unsigned int old = 0U; old < 256U; old++) {
        uint8_t data[8] = {0x00U, 0x50U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U};

        data[0] = (uint8_t)old;
        sender.counter = 5U;
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&s_config, &sender, data, 8U));
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(reference, data, 8, at("old byte 0", old));
    }
}

/* Every single bit of the message, the CRC byte included, is covered: a flip of any one of the
 * 64 bits of a good message gives ERROR, and the stored counter stays. */
static void test_every_single_bit_error_is_detected(void)
{
    uint8_t good[8];

    frame_of(3U, good);

    for (unsigned int bit = 0U; bit < 64U; bit++) {
        uint8_t data[8];
        uint8_t stored = 0xFFU;

        (void)memcpy(data, good, sizeof(data));
        data[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
        ASSERT_STATUS_MESSAGE(E2E_P11STATUS_ERROR, check_from(&s_config, 2U, data, &stored),
                              at("bit", bit));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(2U, stored, at("bit", bit));
    }
}

/* A message checked with another Data ID is an ERROR: the Data ID is what tells a receiver
 * that a message meant for another receiver, or sent under the wrong identifier, is not its
 * own. */
static void test_message_of_another_data_id_is_an_error(void)
{
    const e2e_p11_config_t other = config_with(0x0124U, 8U, 1U);
    uint8_t data[8];
    uint8_t stored = 0U;

    frame_of(0U, data);
    ASSERT_STATUS(E2E_P11STATUS_ERROR, check_from(&other, 14U, data, &stored));
    TEST_ASSERT_EQUAL_UINT8(14U, stored);
}

/*
 * The limit of the Data ID: two Data IDs whose two bytes leave the same CRC register protect
 * every message identically. 0x123 and 0x0A0 are such a pair (fcn_tools.e2e.data_id_crc()
 * gives 0xBC for both): a message of one is accepted as a message of the other. This is why
 * the Data IDs of one bus must differ in that register (PRS_E2E_UC_00072), which the
 * generator of the message table checks.
 */
static void test_data_ids_with_the_same_id_crc_cannot_be_told_apart(void)
{
    const e2e_p11_config_t twin = config_with(0x00A0U, 8U, 1U);
    uint8_t data[8];
    uint8_t stored = 0U;

    frame_of(0U, data);
    ASSERT_STATUS(E2E_P11STATUS_OK, check_from(&twin, 14U, data, &stored));
}

/* ------------------------------------------------------------------------------------------
 * The sender
 * ---------------------------------------------------------------------------------------- */

/* 0, 1, ..., 14, 0, 1, ...: 15 is never sent (PRS_E2E_00504). Each frame is the one of
 * s_frames[] for its counter. */
static void test_sender_counts_0_to_14_and_starts_again_at_0(void)
{
    e2e_p11_protect_state_t sender;

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect_init(&sender));

    for (unsigned int n = 0U; n < 46U; n++) {
        const uint8_t expected_counter = (uint8_t)(n % 15U);
        uint8_t data[8] = {0U};
        uint8_t expected[8];

        frame_of(expected_counter, expected);
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&s_config, &sender, data, 8U));
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(expected, data, 8, at("message", n));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)((n + 1U) % 15U), sender.counter,
                                        at("message", n));
    }
}

/* The high nibble of byte 1 is payload in 11A and keeps what the caller put there; so do
 * bytes 2..7. A stale counter nibble is overwritten. Expected frames from fcn_tools.e2e. */
static void test_protect_writes_only_the_crc_and_the_counter_nibble(void)
{
    static const struct {
        uint8_t counter;
        uint8_t before[8];
        uint8_t after[8];
    } cases[] = {
        {14U,
         {0x00U, 0xF0U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U},
         {0x75U, 0xFEU, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U}},
        {5U,
         {0x00U, 0xF0U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU},
         {0x9AU, 0xF5U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU}},
    };

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        e2e_p11_protect_state_t sender = {.counter = cases[i].counter};
        uint8_t data[8];

        (void)memcpy(data, cases[i].before, sizeof(data));
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&s_config, &sender, data, 8U));
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(cases[i].after, data, 8, at("case", i));
    }

    /* Random payloads, with a stale counter nibble: only byte 0 and bits 0..3 of byte 1. */
    for (unsigned int trial = 0U; trial < 500U; trial++) {
        e2e_p11_protect_state_t sender = {.counter = (uint8_t)(trial % 15U)};
        uint8_t data[8];
        uint8_t before[8];

        for (size_t k = 0U; k < sizeof(data); k++) {
            data[k] = (uint8_t)(random_u32() >> 24U);
        }
        (void)memcpy(before, data, sizeof(before));

        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&s_config, &sender, data, 8U));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(reference_crc(s_config.data_id, data, 8U), data[0],
                                       at("trial", trial));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)((before[1] & 0xF0U) | (trial % 15U)), data[1],
                                       at("trial", trial));
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(&before[2], &data[2], 6, at("trial", trial));
    }
}

/* Every length from 2 to 8: exactly `length` bytes are covered and written, nothing behind
 * them is read or written (the guard bytes stay, and the CRC does not depend on them).
 * The 2-byte frame D7 03 was computed with fcn_tools.e2e. */
static void test_every_length_from_2_to_8(void)
{
    for (uint8_t length = 2U; length <= 8U; length++) {
        const e2e_p11_config_t config = config_with(0x0123U, length, 1U);
        uint8_t buffer[12];
        e2e_p11_protect_state_t sender = {.counter = 3U};
        e2e_p11_check_state_t receiver = {.counter = 2U, .status = E2E_P11STATUS_ERROR};

        (void)memset(buffer, 0, sizeof(buffer));
        (void)memset(&buffer[length], 0x5A, sizeof(buffer) - length);

        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&config, &sender, buffer, length));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(reference_crc(0x0123U, buffer, length), buffer[0],
                                       at("length", length));
        TEST_ASSERT_EACH_EQUAL_HEX8_MESSAGE(0x5AU, &buffer[length], sizeof(buffer) - length,
                                            at("length", length));

        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&config, &receiver, buffer, length));
        ASSERT_STATUS_MESSAGE(E2E_P11STATUS_OK, receiver.status, at("length", length));

        if (length == 2U) {
            TEST_ASSERT_EQUAL_HEX8(0xD7U, buffer[0]);
            TEST_ASSERT_EQUAL_HEX8(0x03U, buffer[1]);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * The receiver
 * ---------------------------------------------------------------------------------------- */

/* Every status, each from a receiver whose stored counter is 5, with max_delta_counter 3,
 * and the stored counter afterwards. */
static void test_every_status_and_the_counter_it_leaves(void)
{
    const e2e_p11_config_t config = config_with(0x0123U, 8U, 3U);
    static const struct {
        int counter; /* received counter; -1: no message; -2: message with a bad CRC */
        e2e_p11_status_t status;
        uint8_t stored_after;
    } cases[] = {
        {-1, E2E_P11STATUS_NONEWDATA, 5U},
        {-2, E2E_P11STATUS_ERROR, 5U},
        {15, E2E_P11STATUS_ERROR, 5U},
        {5, E2E_P11STATUS_REPEATED, 5U},
        {6, E2E_P11STATUS_OK, 6U},
        {7, E2E_P11STATUS_OKSOMELOST, 7U},
        {8, E2E_P11STATUS_OKSOMELOST, 8U},
        {9, E2E_P11STATUS_WRONGSEQUENCE, 9U},
        {4, E2E_P11STATUS_WRONGSEQUENCE, 4U},
        {0, E2E_P11STATUS_WRONGSEQUENCE, 0U},
        {14, E2E_P11STATUS_WRONGSEQUENCE, 14U},
    };

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        uint8_t data[8];
        uint8_t stored = 0xFFU;
        e2e_p11_status_t status;

        if (cases[i].counter == -1) {
            status = check_from(&config, 5U, NULL, &stored);
        } else if (cases[i].counter == -2) {
            make_frame(0x0123U, 6U, 0x00U, data);
            data[0] ^= 0x01U;
            status = check_from(&config, 5U, data, &stored);
        } else {
            make_frame(0x0123U, (uint8_t)cases[i].counter, 0x00U, data);
            status = check_from(&config, 5U, data, &stored);
        }

        ASSERT_STATUS_MESSAGE(cases[i].status, status, at("case", i));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(cases[i].stored_after, stored, at("case", i));
    }
}

/* The wrap from 14 to 0 is one step forward, not fourteen back; 13 -> 0 and 14 -> 1 are two. */
static void test_counter_wraps_from_14_to_0(void)
{
    const e2e_p11_config_t tolerant = config_with(0x0123U, 8U, 2U);
    uint8_t data[8];
    uint8_t stored = 0U;

    frame_of(0U, data);
    ASSERT_STATUS(E2E_P11STATUS_OK, check_from(&s_config, 14U, data, &stored));
    TEST_ASSERT_EQUAL_UINT8(0U, stored);
    ASSERT_STATUS(E2E_P11STATUS_WRONGSEQUENCE, check_from(&s_config, 13U, data, &stored));
    ASSERT_STATUS(E2E_P11STATUS_OKSOMELOST, check_from(&tolerant, 13U, data, &stored));

    frame_of(1U, data);
    ASSERT_STATUS(E2E_P11STATUS_OKSOMELOST, check_from(&tolerant, 14U, data, &stored));

    /* 0 -> 14 is fourteen steps forward: a message from the past. */
    frame_of(14U, data);
    ASSERT_STATUS(E2E_P11STATUS_WRONGSEQUENCE, check_from(&tolerant, 0U, data, &stored));
}

/*
 * All 225 pairs of stored and received counter, for every max_delta_counter from 1 to 14.
 * The status is a function of the step d = (received - stored) mod 15 alone: 0 REPEATED,
 * 1 OK, 2..max OKSOMELOST, above max WRONGSEQUENCE; the received counter is stored.
 */
static void test_status_depends_on_the_counter_step_alone(void)
{
    for (uint8_t max = 1U; max <= 14U; max++) {
        const e2e_p11_config_t config = config_with(0x0123U, 8U, max);

        for (uint8_t stored = 0U; stored <= 14U; stored++) {
            for (uint8_t received = 0U; received <= 14U; received++) {
                const unsigned int step = ((unsigned int)received + 15U - stored) % 15U;
                e2e_p11_status_t expected = E2E_P11STATUS_OKSOMELOST;
                uint8_t data[8];
                uint8_t after = 0xFFU;

                if (step == 0U) {
                    expected = E2E_P11STATUS_REPEATED;
                } else if (step == 1U) {
                    expected = E2E_P11STATUS_OK;
                } else if (step > max) {
                    expected = E2E_P11STATUS_WRONGSEQUENCE;
                } else {
                    /* 2..max */
                }

                frame_of(received, data);
                ASSERT_STATUS_MESSAGE(expected, check_from(&config, stored, data, &after),
                                      at("max * 256 + stored * 16 + received",
                                         (max * 256UL) + (stored * 16UL) + received));
                TEST_ASSERT_EQUAL_UINT8(received, after);
            }
        }
    }
}

/* A WRONGSEQUENCE is followed: the receiver stores the counter of every message whose CRC is
 * right, so a sender that jumped and then goes on normally is OK again at once. Stored 3, then
 * 4, 9, 10, 10, 11. */
static void test_receiver_follows_a_sender_that_jumped(void)
{
    static const struct {
        uint8_t counter;
        e2e_p11_status_t status;
    } steps[] = {{4U, E2E_P11STATUS_OK},
                 {9U, E2E_P11STATUS_WRONGSEQUENCE},
                 {10U, E2E_P11STATUS_OK},
                 {10U, E2E_P11STATUS_REPEATED},
                 {11U, E2E_P11STATUS_OK}};
    e2e_p11_check_state_t receiver = {.counter = 3U, .status = E2E_P11STATUS_ERROR};

    for (size_t i = 0U; i < (sizeof(steps) / sizeof(steps[0])); i++) {
        uint8_t data[8];

        frame_of(steps[i].counter, data);
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&s_config, &receiver, data, 8U));
        ASSERT_STATUS_MESSAGE(steps[i].status, receiver.status, at("step", i));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(steps[i].counter, receiver.counter, at("step", i));
    }
}

/*
 * After e2e_p11_check_init() the stored counter is 14 (SWS_E2E_00556), so only a first message
 * with counter 0 is OK. With max_delta_counter 1, 14 is REPEATED and 1..13 are WRONGSEQUENCE;
 * with 3, 1 and 2 are OKSOMELOST.
 */
static void test_after_check_init_only_counter_0_is_ok(void)
{
    const e2e_p11_config_t tolerant = config_with(0x0123U, 8U, 3U);

    for (uint8_t first = 0U; first <= 14U; first++) {
        e2e_p11_check_state_t strict_rx;
        e2e_p11_check_state_t tolerant_rx;
        e2e_p11_status_t strict_expected = E2E_P11STATUS_WRONGSEQUENCE;
        e2e_p11_status_t tolerant_expected = E2E_P11STATUS_WRONGSEQUENCE;
        uint8_t data[8];

        if (first == 0U) {
            strict_expected = E2E_P11STATUS_OK;
            tolerant_expected = E2E_P11STATUS_OK;
        } else if (first == 14U) {
            strict_expected = E2E_P11STATUS_REPEATED;
            tolerant_expected = E2E_P11STATUS_REPEATED;
        } else if (first <= 2U) {
            tolerant_expected = E2E_P11STATUS_OKSOMELOST;
        } else {
            /* 3..13: WRONGSEQUENCE for both */
        }

        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check_init(&strict_rx));
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check_init(&tolerant_rx));
        frame_of(first, data);
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&s_config, &strict_rx, data, 8U));
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&tolerant, &tolerant_rx, data, 8U));
        ASSERT_STATUS_MESSAGE(strict_expected, strict_rx.status, at("first counter", first));
        ASSERT_STATUS_MESSAGE(tolerant_expected, tolerant_rx.status, at("first counter", first));
    }
}

/*
 * The reserved counter 15 with a correct CRC (s_frames[15]) is an ERROR and leaves the stored
 * counter alone (this project's rule, e2e_p11.h RULE 3). Taken literally, the PRS chart would
 * call it OK after a stored 14 and then judge the sender's next message, counter 0, REPEATED.
 */
static void test_reserved_counter_15_is_an_error_and_keeps_the_state(void)
{
    e2e_p11_check_state_t receiver;
    uint8_t data[8];

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check_init(&receiver));

    frame_of(15U, data);
    TEST_ASSERT_EQUAL_HEX8(reference_crc(0x0123U, data, 8U), data[0]); /* the CRC is right */
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&s_config, &receiver, data, 8U));
    ASSERT_STATUS(E2E_P11STATUS_ERROR, receiver.status);
    TEST_ASSERT_EQUAL_UINT8(14U, receiver.counter);

    frame_of(0U, data);
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&s_config, &receiver, data, 8U));
    ASSERT_STATUS(E2E_P11STATUS_OK, receiver.status);
}

/*
 * A sequence from e2e_p11_check_init() that passes every rule: counters 0, 0, 1, no message,
 * 3, 7, 8, 14, 0, 2, 2 with one bit flipped, 3, 15, 0, 1. Worked out by hand from the rules
 * of the header (stored counter in brackets), for max_delta_counter 1:
 *
 *     0 OK [0], 0 REPEATED [0], 1 OK [1], - NONEWDATA [1], 3 WRONGSEQUENCE [3],
 *     7 WRONGSEQUENCE [7], 8 OK [8], 14 WRONGSEQUENCE [14], 0 OK [0], 2 WRONGSEQUENCE [2],
 *     2* ERROR [2], 3 OK [3], 15 ERROR [3], 0 WRONGSEQUENCE (step 12) [0], 1 OK [1]
 *
 * and for max_delta_counter 3 the same, except that the steps of 2 (1 -> 3, 0 -> 2) are
 * OKSOMELOST.
 */
static void test_a_sequence_through_every_rule(void)
{
    static const int counters[15] = {0, 0, 1, -1, 3, 7, 8, 14, 0, 2, -2, 3, 15, 0, 1};
    static const e2e_p11_status_t strict[15] = {E2E_P11STATUS_OK,
                                                E2E_P11STATUS_REPEATED,
                                                E2E_P11STATUS_OK,
                                                E2E_P11STATUS_NONEWDATA,
                                                E2E_P11STATUS_WRONGSEQUENCE,
                                                E2E_P11STATUS_WRONGSEQUENCE,
                                                E2E_P11STATUS_OK,
                                                E2E_P11STATUS_WRONGSEQUENCE,
                                                E2E_P11STATUS_OK,
                                                E2E_P11STATUS_WRONGSEQUENCE,
                                                E2E_P11STATUS_ERROR,
                                                E2E_P11STATUS_OK,
                                                E2E_P11STATUS_ERROR,
                                                E2E_P11STATUS_WRONGSEQUENCE,
                                                E2E_P11STATUS_OK};
    const e2e_p11_config_t tolerant = config_with(0x0123U, 8U, 3U);
    e2e_p11_check_state_t strict_rx;
    e2e_p11_check_state_t tolerant_rx;

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check_init(&strict_rx));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check_init(&tolerant_rx));

    for (size_t i = 0U; i < 15U; i++) {
        uint8_t data[8];
        const uint8_t *message = data;
        const size_t length = (counters[i] == -1) ? 0U : 8U;
        e2e_p11_status_t tolerant_expected = strict[i];

        if (counters[i] == -1) {
            message = NULL;
        } else if (counters[i] == -2) {
            frame_of(2U, data);
            data[5] ^= 0x10U;
        } else {
            frame_of((uint8_t)counters[i], data);
        }

        if ((i == 4U) || (i == 9U)) {
            tolerant_expected = E2E_P11STATUS_OKSOMELOST;
        }

        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&s_config, &strict_rx, message, length));
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&tolerant, &tolerant_rx, message, length));
        ASSERT_STATUS_MESSAGE(strict[i], strict_rx.status, at("step", i));
        ASSERT_STATUS_MESSAGE(tolerant_expected, tolerant_rx.status, at("step", i));
    }
}

/* A sender and a receiver over 100 messages, through the wrap six times: every message OK.
 * Then the receiver misses two cycles (NONEWDATA, counter kept) and the next message is
 * OKSOMELOST with max_delta_counter 3. */
static void test_sender_and_receiver_agree_across_the_wrap(void)
{
    const e2e_p11_config_t config = config_with(0x0123U, 8U, 3U);
    e2e_p11_protect_state_t sender;
    e2e_p11_check_state_t receiver;
    uint8_t data[8];

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect_init(&sender));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check_init(&receiver));

    for (unsigned int n = 0U; n < 100U; n++) {
        (void)memset(data, (int)(n & 0xFFU), sizeof(data));
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&config, &sender, data, 8U));
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&config, &receiver, data, 8U));
        ASSERT_STATUS_MESSAGE(E2E_P11STATUS_OK, receiver.status, at("message", n));
    }

    for (unsigned int lost = 0U; lost < 2U; lost++) {
        (void)memset(data, 0, sizeof(data));
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&config, &sender, data, 8U));
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&config, &receiver, NULL, 0U));
        ASSERT_STATUS(E2E_P11STATUS_NONEWDATA, receiver.status);
        TEST_ASSERT_EQUAL_UINT8(9U, receiver.counter); /* message 99 carried 99 % 15 = 9 */
    }

    (void)memset(data, 0, sizeof(data));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&config, &sender, data, 8U));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&config, &receiver, data, 8U));
    ASSERT_STATUS(E2E_P11STATUS_OKSOMELOST, receiver.status);
    TEST_ASSERT_EQUAL_UINT8(12U, receiver.counter);
}

/* ------------------------------------------------------------------------------------------
 * Initialisation and input errors
 * ---------------------------------------------------------------------------------------- */

static void test_init_functions(void)
{
    e2e_p11_protect_state_t sender = {.counter = 0xA5U};
    e2e_p11_check_state_t receiver = {.counter = 0xA5U, .status = E2E_P11STATUS_OK};

    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_p11_protect_init(NULL));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_p11_check_init(NULL));

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect_init(&sender));
    TEST_ASSERT_EQUAL_UINT8(0U, sender.counter);

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check_init(&receiver));
    TEST_ASSERT_EQUAL_UINT8(14U, receiver.counter);
    ASSERT_STATUS(E2E_P11STATUS_ERROR, receiver.status);
}

/* Configurations that are refused: lengths 0, 1 and 9 and above, max_delta_counter 0 and 15
 * and above. The boundaries 2, 8, 1 and 14 are accepted. */
static const e2e_p11_config_t s_bad_configs[] = {
    {.data_id = 0x0123U, .data_len = 0U, .max_delta_counter = 1U},
    {.data_id = 0x0123U, .data_len = 1U, .max_delta_counter = 1U},
    {.data_id = 0x0123U, .data_len = 9U, .max_delta_counter = 1U},
    {.data_id = 0x0123U, .data_len = 255U, .max_delta_counter = 1U},
    {.data_id = 0x0123U, .data_len = 8U, .max_delta_counter = 0U},
    {.data_id = 0x0123U, .data_len = 8U, .max_delta_counter = 15U},
    {.data_id = 0x0123U, .data_len = 8U, .max_delta_counter = 255U},
};

/*
 * E2E_P11Protect: E2E_E_INPUTERR_NULL for any NULL pointer, E2E_E_INPUTERR_WRONG for a length
 * other than data_len (Fig. 6.94), a configuration out of range or a state counter above 14.
 * Nothing is written, neither the message nor the state.
 */
static void test_protect_refuses_wrong_input_and_writes_nothing(void)
{
    static const uint8_t pristine[9] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U,
                                        0x66U, 0x77U, 0x88U, 0x99U};
    e2e_p11_protect_state_t sender = {.counter = 4U};
    uint8_t data[9];

    (void)memcpy(data, pristine, sizeof(data));

    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_p11_protect(NULL, &sender, data, 8U));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_p11_protect(&s_config, NULL, data, 8U));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_p11_protect(&s_config, &sender, NULL, 8U));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_p11_protect(&s_config, &sender, NULL, 0U));

    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_WRONG, e2e_p11_protect(&s_config, &sender, data, 0U));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_WRONG, e2e_p11_protect(&s_config, &sender, data, 7U));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_WRONG, e2e_p11_protect(&s_config, &sender, data, 9U));

    for (size_t i = 0U; i < (sizeof(s_bad_configs) / sizeof(s_bad_configs[0])); i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(
            E2E_E_INPUTERR_WRONG,
            e2e_p11_protect(&s_bad_configs[i], &sender, data, s_bad_configs[i].data_len),
            at("configuration", i));
    }

    for (unsigned int counter = 15U; counter < 256U; counter++) {
        e2e_p11_protect_state_t broken = {.counter = (uint8_t)counter};

        TEST_ASSERT_EQUAL_HEX8_MESSAGE(E2E_E_INPUTERR_WRONG,
                                       e2e_p11_protect(&s_config, &broken, data, 8U),
                                       at("state counter", counter));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)counter, broken.counter);
    }

    TEST_ASSERT_EQUAL_UINT8(4U, sender.counter);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pristine, data, sizeof(data));

    /* The boundaries of the configuration are accepted. */
    {
        const e2e_p11_config_t shortest = config_with(0x0123U, 2U, 14U);
        const e2e_p11_config_t longest = config_with(0x0123U, 8U, 14U);

        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&shortest, &sender, data, 2U));
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&longest, &sender, data, 8U));
        TEST_ASSERT_EQUAL_UINT8(6U, sender.counter);
    }
}

/* A receiver state that is checked to be unchanged after a refused call. */
static void expect_check_refused(e2e_result_t expected, const e2e_p11_config_t *config,
                                 const uint8_t *data, size_t length, const char *what)
{
    e2e_p11_check_state_t receiver = {.counter = 6U, .status = E2E_P11STATUS_OKSOMELOST};

    TEST_ASSERT_EQUAL_HEX8_MESSAGE(expected, e2e_p11_check(config, &receiver, data, length), what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(6U, receiver.counter, what);
    ASSERT_STATUS_MESSAGE(E2E_P11STATUS_OKSOMELOST, receiver.status, what);
}

/*
 * E2E_P11Check (Fig. 6.106): E2E_E_INPUTERR_NULL for a NULL configuration or state;
 * E2E_E_INPUTERR_WRONG when only one of "data NULL" and "length 0" holds, for a length other
 * than data_len, and, this module's additions, for a configuration out of range (also on a
 * call without data) and a stored counter above 14. Neither the counter nor the status of the
 * previous check is touched.
 */
static void test_check_refuses_wrong_input_and_keeps_the_state(void)
{
    e2e_p11_check_state_t receiver = {.counter = 6U, .status = E2E_P11STATUS_OKSOMELOST};
    uint8_t data[9];

    make_frame(0x0123U, 7U, 0x00U, data);
    data[8] = 0x00U;

    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_p11_check(NULL, &receiver, data, 8U));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_p11_check(&s_config, NULL, data, 8U));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_p11_check(NULL, NULL, NULL, 0U));

    expect_check_refused(E2E_E_INPUTERR_WRONG, &s_config, NULL, 8U, "NULL with length 8");
    expect_check_refused(E2E_E_INPUTERR_WRONG, &s_config, NULL, 1U, "NULL with length 1");
    expect_check_refused(E2E_E_INPUTERR_WRONG, &s_config, data, 0U, "data with length 0");
    expect_check_refused(E2E_E_INPUTERR_WRONG, &s_config, data, 7U, "length 7");
    expect_check_refused(E2E_E_INPUTERR_WRONG, &s_config, data, 9U, "length 9");

    for (size_t i = 0U; i < (sizeof(s_bad_configs) / sizeof(s_bad_configs[0])); i++) {
        expect_check_refused(E2E_E_INPUTERR_WRONG, &s_bad_configs[i], data,
                             s_bad_configs[i].data_len, at("configuration", i));
        expect_check_refused(E2E_E_INPUTERR_WRONG, &s_bad_configs[i], NULL, 0U,
                             at("configuration without data", i));
    }

    for (unsigned int counter = 15U; counter < 256U; counter++) {
        e2e_p11_check_state_t broken = {.counter = (uint8_t)counter,
                                        .status = E2E_P11STATUS_REPEATED};

        TEST_ASSERT_EQUAL_HEX8_MESSAGE(E2E_E_INPUTERR_WRONG,
                                       e2e_p11_check(&s_config, &broken, data, 8U),
                                       at("state counter", counter));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(E2E_E_INPUTERR_WRONG,
                                       e2e_p11_check(&s_config, &broken, NULL, 0U),
                                       at("state counter, no data", counter));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)counter, broken.counter);
        ASSERT_STATUS(E2E_P11STATUS_REPEATED, broken.status);
    }

    /* The same receiver accepts the message when the call is right. */
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check(&s_config, &receiver, data, 8U));
    ASSERT_STATUS(E2E_P11STATUS_OK, receiver.status);
}

/* ------------------------------------------------------------------------------------------
 * Mapping to the state machine
 * ---------------------------------------------------------------------------------------- */

/* SWS_E2E_00608 and SWS_E2E_00558. */
static void test_map_status_to_sm(void)
{
    static const struct {
        e2e_p11_status_t status;
        e2e_p_check_status_t mapped;
    } table[] = {
        {E2E_P11STATUS_OK, E2E_P_OK},
        {E2E_P11STATUS_OKSOMELOST, E2E_P_OK},
        {E2E_P11STATUS_ERROR, E2E_P_ERROR},
        {E2E_P11STATUS_REPEATED, E2E_P_REPEATED},
        {E2E_P11STATUS_NONEWDATA, E2E_P_NONEWDATA},
        {E2E_P11STATUS_WRONGSEQUENCE, E2E_P_WRONGSEQUENCE},
    };
    static const e2e_result_t failures[] = {E2E_E_INPUTERR_NULL, E2E_E_INPUTERR_WRONG,
                                            E2E_E_WRONGSTATE};

    for (size_t i = 0U; i < (sizeof(table) / sizeof(table[0])); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(table[i].mapped,
                                      e2e_p11_map_status_to_sm(E2E_E_OK, table[i].status),
                                      status_name(table[i].status));

        for (size_t k = 0U; k < (sizeof(failures) / sizeof(failures[0])); k++) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(E2E_P_ERROR,
                                          e2e_p11_map_status_to_sm(failures[k], table[i].status),
                                          status_name(table[i].status));
        }
    }

    /* A value that is no status at all, from a damaged state: an error. */
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): outside it on purpose */
    TEST_ASSERT_EQUAL_INT(E2E_P_ERROR, e2e_p11_map_status_to_sm(E2E_E_OK, (e2e_p11_status_t)0x02));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_published_vectors_of_variant_11a);
    RUN_TEST(test_published_vectors_of_variant_11c_as_the_equal_11a_message);
    RUN_TEST(test_published_vectors_are_accepted_in_order);
    RUN_TEST(test_crc_is_crc8_gsm_a_over_data_id_and_message);
    RUN_TEST(test_crc_takes_the_data_id_low_byte_first);
    RUN_TEST(test_crc_does_not_cover_byte_0);
    RUN_TEST(test_every_single_bit_error_is_detected);
    RUN_TEST(test_message_of_another_data_id_is_an_error);
    RUN_TEST(test_data_ids_with_the_same_id_crc_cannot_be_told_apart);
    RUN_TEST(test_sender_counts_0_to_14_and_starts_again_at_0);
    RUN_TEST(test_protect_writes_only_the_crc_and_the_counter_nibble);
    RUN_TEST(test_every_length_from_2_to_8);
    RUN_TEST(test_every_status_and_the_counter_it_leaves);
    RUN_TEST(test_counter_wraps_from_14_to_0);
    RUN_TEST(test_status_depends_on_the_counter_step_alone);
    RUN_TEST(test_receiver_follows_a_sender_that_jumped);
    RUN_TEST(test_after_check_init_only_counter_0_is_ok);
    RUN_TEST(test_reserved_counter_15_is_an_error_and_keeps_the_state);
    RUN_TEST(test_a_sequence_through_every_rule);
    RUN_TEST(test_sender_and_receiver_agree_across_the_wrap);
    RUN_TEST(test_init_functions);
    RUN_TEST(test_protect_refuses_wrong_input_and_writes_nothing);
    RUN_TEST(test_check_refuses_wrong_input_and_keeps_the_state);
    RUN_TEST(test_map_status_to_sm);
    return UNITY_END();
}
