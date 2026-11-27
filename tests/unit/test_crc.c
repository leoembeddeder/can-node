/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdio.h>

#include "crc/crc.h"
#include "unity.h"

/* "123456789" in ASCII: the message for which the CRC catalogues publish the check value of
 * every algorithm. */
static const uint8_t s_check[9] = {0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U, 0x39U};

/* xorshift32. The state is reset before every test, so a test sees the same "random" data on
 * every run and whatever order the tests run in. */
static uint32_t s_random_state;

static uint32_t random_u32(void)
{
    s_random_state ^= s_random_state << 13U;
    s_random_state ^= s_random_state >> 17U;
    s_random_state ^= s_random_state << 5U;
    return s_random_state;
}

static void random_fill(uint8_t *buffer, size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        buffer[i] = (uint8_t)(random_u32() >> 24U);
    }
}

/* Message for an assertion inside a loop: which case failed. */
static const char *at(const char *what, size_t value)
{
    static char text[48];

    (void)snprintf(text, sizeof(text), "%s %lu", what, (unsigned long)value);
    return text;
}

void setUp(void)
{
    s_random_state = 0x1234ABCDU;
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------------------------
 * Reference implementations. Each follows the definition of its CRC bit by bit and uses no
 * table, so that it shares neither data nor structure with the code under test.
 * ---------------------------------------------------------------------------------------- */

/* One byte through an 8-bit CRC that works most significant bit first: XOR the byte into the
 * register, then eight times shift left and subtract the generator whenever a 1 falls out. */
static uint8_t ref_crc8_byte(uint8_t poly, uint8_t crc, uint8_t byte)
{
    uint8_t reg = (uint8_t)(crc ^ byte);

    for (unsigned int i = 0U; i < 8U; i++) {
        const bool carry = (reg & 0x80U) != 0U;

        reg = (uint8_t)(reg << 1U);

        if (carry) {
            reg ^= poly;
        }
    }

    return reg;
}

static uint8_t ref_crc8_update(uint8_t poly, uint8_t crc, const uint8_t *data, size_t len)
{
    uint8_t reg = crc;

    for (size_t i = 0U; i < len; i++) {
        reg = ref_crc8_byte(poly, reg, data[i]);
    }

    return reg;
}

static uint32_t reverse_bits32(uint32_t value)
{
    uint32_t reversed = 0U;

    for (unsigned int i = 0U; i < 32U; i++) {
        if ((value & ((uint32_t)1U << i)) != 0U) {
            reversed |= (uint32_t)1U << (31U - i);
        }
    }

    return reversed;
}

/* CRC-32 the way the catalogue states it, with the generator in its normal form 0x04C11DB7
 * and a register that shifts left. "refin" means that every byte enters least significant
 * bit first, "refout" that the register is read bit-reversed. The code under test keeps its
 * register bit-reversed all the time, so the register is reversed on the way in and out. */
static uint32_t ref_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    uint32_t reg = reverse_bits32(crc);

    for (size_t n = 0U; n < len; n++) {
        for (unsigned int i = 0U; i < 8U; i++) {
            const uint32_t in = ((uint32_t)data[n] >> i) & 1U;
            const uint32_t out = reg >> 31U;

            reg <<= 1U;

            if (in != out) {
                reg ^= 0x04C11DB7U;
            }
        }
    }

    return reverse_bits32(reg);
}

#define REF_CRC15_MAX_BYTES 24U

/* CRC-15 by its definition in the Bosch specification, section 3.2.1: the remainder of the
 * division of the message bits, followed by 15 zero bits, by the generator polynomial. This
 * is schoolbook long division on an array of bits, not a shift register. */
static uint16_t ref_crc15_division(const uint8_t *data, size_t len)
{
    /* x^15 + x^14 + x^10 + x^8 + x^7 + x^4 + x^3 + 1, coefficient of x^15 first */
    static const uint8_t generator[16] = {1U, 1U, 0U, 0U, 0U, 1U, 0U, 1U,
                                          1U, 0U, 0U, 1U, 1U, 0U, 0U, 1U};
    uint8_t bits[(REF_CRC15_MAX_BYTES * 8U) + 15U] = {0U};
    const size_t message_bits = len * 8U;
    uint16_t crc = 0U;

    TEST_ASSERT_LESS_OR_EQUAL_size_t(REF_CRC15_MAX_BYTES, len);

    for (size_t i = 0U; i < message_bits; i++) {
        bits[i] = (uint8_t)(((uint32_t)data[i / 8U] >> (7U - (i % 8U))) & 1U);
    }

    for (size_t i = 0U; i < message_bits; i++) {
        if (bits[i] != 0U) {
            for (size_t j = 0U; j < 16U; j++) {
                bits[i + j] ^= generator[j];
            }
        }
    }

    for (size_t i = 0U; i < 15U; i++) {
        crc = (uint16_t)((crc << 1U) | bits[message_bits + i]);
    }

    return crc;
}

/* Complete CRCs, with the initial value and final XOR that the catalogue gives. */
static uint8_t j1850(const uint8_t *data, size_t len)
{
    return (uint8_t)(crc8_j1850_update(CRC8_J1850_INIT, data, len) ^ CRC8_J1850_XOROUT);
}

static uint8_t h2f(const uint8_t *data, size_t len)
{
    return (uint8_t)(crc8_h2f_update(CRC8_H2F_INIT, data, len) ^ CRC8_H2F_XOROUT);
}

/* ------------------------------------------------------------------------------------------
 * Published values
 * ---------------------------------------------------------------------------------------- */

/* The "check" entries of Greg Cook's Catalogue of parametrised CRC algorithms for
 * CRC-8/SAE-J1850, CRC-8/AUTOSAR, CRC-15/CAN and CRC-32/ISO-HDLC. */
static void test_check_values_match_the_crc_catalogue(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x4BU, j1850(s_check, sizeof(s_check)));
    TEST_ASSERT_EQUAL_HEX8(0xDFU, h2f(s_check, sizeof(s_check)));
    TEST_ASSERT_EQUAL_HEX16(0x059EU, crc15_can_update(CRC15_CAN_INIT, s_check, sizeof(s_check)));
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, crc32_compute(s_check, sizeof(s_check)));
}

/* The worked examples of the AUTOSAR "Specification of CRC Routines" (SWS CRC Library) for
 * Crc_CalculateCRC8, Crc_CalculateCRC8H2F and Crc_CalculateCRC32: the same seven messages
 * for each of the three. They reach table entries that "123456789" does not. */
static void test_results_match_the_autosar_crc_library_examples(void)
{
    static const struct {
        uint8_t data[9];
        uint8_t len;
        uint8_t j1850;
        uint8_t h2f;
        uint32_t crc32;
    } examples[] = {
        {{0x00U, 0x00U, 0x00U, 0x00U}, 4U, 0x59U, 0x12U, 0x2144DF1CU},
        {{0xF2U, 0x01U, 0x83U}, 3U, 0x37U, 0xC2U, 0x24AB9D77U},
        {{0x0FU, 0xAAU, 0x00U, 0x55U}, 4U, 0x79U, 0xC6U, 0xB6C9B287U},
        {{0x00U, 0xFFU, 0x55U, 0x11U}, 4U, 0xB8U, 0x77U, 0x32A06212U},
        {{0x33U, 0x22U, 0x55U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU},
         9U,
         0xCBU,
         0x11U,
         0xB0AE863DU},
        {{0x92U, 0x6BU, 0x55U}, 3U, 0x8CU, 0x33U, 0x9CDEA29BU},
        {{0xFFU, 0xFFU, 0xFFU, 0xFFU}, 4U, 0x74U, 0x6CU, 0xFFFFFFFFU},
    };

    for (size_t i = 0U; i < (sizeof(examples) / sizeof(examples[0])); i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(examples[i].j1850, j1850(examples[i].data, examples[i].len),
                                       at("example", i));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(examples[i].h2f, h2f(examples[i].data, examples[i].len),
                                       at("example", i));
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(
            examples[i].crc32, crc32_compute(examples[i].data, examples[i].len), at("example", i));
    }
}

/* What a receiver does: run the CRC over the message and the CRC that came with it. For an
 * undamaged codeword the register always ends at the same value, the "residue" entry of the
 * catalogue (the register before the final XOR). The transmitted CRC-32 goes least
 * significant byte first, because that CRC works least significant bit first. */
static void test_message_followed_by_its_crc_leaves_the_catalogue_residue(void)
{
    uint8_t codeword[sizeof(s_check) + 4U];
    const size_t len = sizeof(s_check);

    for (size_t i = 0U; i < len; i++) {
        codeword[i] = s_check[i];
    }

    codeword[len] = j1850(s_check, len);
    TEST_ASSERT_EQUAL_HEX8(0xC4U, crc8_j1850_update(CRC8_J1850_INIT, codeword, len + 1U));

    codeword[len] = h2f(s_check, len);
    TEST_ASSERT_EQUAL_HEX8(0x42U, crc8_h2f_update(CRC8_H2F_INIT, codeword, len + 1U));

    const uint32_t crc32 = crc32_compute(s_check, len);

    for (size_t i = 0U; i < 4U; i++) {
        codeword[len + i] = (uint8_t)(crc32 >> (8U * i));
    }

    TEST_ASSERT_EQUAL_HEX32(0xDEBB20E3U, crc32_update(CRC32_INIT, codeword, len + 4U));
}

/* ------------------------------------------------------------------------------------------
 * Tables
 * ---------------------------------------------------------------------------------------- */

/* Starting from an all-zero register, one byte b leaves exactly table entry b in the
 * register, so the public functions expose every entry. A check value proves nothing about
 * the entries its message does not use; this test would catch a single mistyped digit in any
 * of the 768. */
static void test_every_table_entry_equals_the_bitwise_computation(void)
{
    for (unsigned int value = 0U; value < 256U; value++) {
        const uint8_t byte = (uint8_t)value;

        TEST_ASSERT_EQUAL_HEX8_MESSAGE(ref_crc8_byte(0x1DU, 0x00U, byte),
                                       crc8_j1850_update(0x00U, &byte, 1U),
                                       at("J1850 entry", value));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(ref_crc8_byte(0x2FU, 0x00U, byte),
                                       crc8_h2f_update(0x00U, &byte, 1U), at("H2F entry", value));
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(ref_crc32_update(0U, &byte, 1U),
                                        crc32_update(0U, &byte, 1U), at("CRC-32 entry", value));
    }
}

/* The table lookup is only half of an update step: the old register has to be combined with
 * the input and, for CRC-32, with the entry. Random data on random register values. */
static void test_updates_match_the_bitwise_computation_for_random_data(void)
{
    for (size_t trial = 0U; trial < 300U; trial++) {
        uint8_t data[REF_CRC15_MAX_BYTES];
        const size_t len = (size_t)(random_u32() % (sizeof(data) + 1U));
        const uint32_t start = random_u32();
        const uint8_t start8 = (uint8_t)(start >> 8U);

        random_fill(data, sizeof(data));

        TEST_ASSERT_EQUAL_HEX8_MESSAGE(ref_crc8_update(0x1DU, start8, data, len),
                                       crc8_j1850_update(start8, data, len), at("trial", trial));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(ref_crc8_update(0x2FU, start8, data, len),
                                       crc8_h2f_update(start8, data, len), at("trial", trial));
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(ref_crc32_update(start, data, len),
                                        crc32_update(start, data, len), at("trial", trial));
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(ref_crc15_division(data, len),
                                        crc15_can_update(CRC15_CAN_INIT, data, len),
                                        at("trial", trial));
    }
}

/* ------------------------------------------------------------------------------------------
 * The register can be carried from one call to the next
 * ---------------------------------------------------------------------------------------- */

static void test_incremental_update_equals_one_shot_at_every_split_point(void)
{
    uint8_t data[40];

    random_fill(data, sizeof(data));

    const uint8_t whole_j1850 = crc8_j1850_update(CRC8_J1850_INIT, data, sizeof(data));
    const uint8_t whole_h2f = crc8_h2f_update(CRC8_H2F_INIT, data, sizeof(data));
    const uint16_t whole_crc15 = crc15_can_update(CRC15_CAN_INIT, data, sizeof(data));
    const uint32_t whole_crc32 = crc32_update(CRC32_INIT, data, sizeof(data));

    for (size_t split = 0U; split <= sizeof(data); split++) {
        const uint8_t *second = &data[split];
        const size_t rest = sizeof(data) - split;

        TEST_ASSERT_EQUAL_HEX8_MESSAGE(
            whole_j1850,
            crc8_j1850_update(crc8_j1850_update(CRC8_J1850_INIT, data, split), second, rest),
            at("split", split));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(
            whole_h2f, crc8_h2f_update(crc8_h2f_update(CRC8_H2F_INIT, data, split), second, rest),
            at("split", split));
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(
            whole_crc15,
            crc15_can_update(crc15_can_update(CRC15_CAN_INIT, data, split), second, rest),
            at("split", split));
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(
            whole_crc32, crc32_update(crc32_update(CRC32_INIT, data, split), second, rest),
            at("split", split));
    }
}

static void test_zero_length_update_leaves_the_register_unchanged(void)
{
    static const uint32_t registers[] = {0x00000000U, 0xFFFFFFFFU, 0x5A5A5A5AU, 0x00003C96U};
    const uint8_t byte = 0xA5U;

    for (size_t i = 0U; i < (sizeof(registers) / sizeof(registers[0])); i++) {
        const uint32_t reg32 = registers[i];
        const uint8_t reg8 = (uint8_t)reg32;
        const uint16_t reg15 = (uint16_t)(reg32 & 0x7FFFU);

        TEST_ASSERT_EQUAL_HEX8(reg8, crc8_j1850_update(reg8, &byte, 0U));
        TEST_ASSERT_EQUAL_HEX8(reg8, crc8_h2f_update(reg8, &byte, 0U));
        TEST_ASSERT_EQUAL_HEX16(reg15, crc15_can_update(reg15, &byte, 0U));
        TEST_ASSERT_EQUAL_HEX32(reg32, crc32_update(reg32, &byte, 0U));

        /* An empty buffer may also be given as NULL: the data is never looked at. */
        TEST_ASSERT_EQUAL_HEX8(reg8, crc8_j1850_update(reg8, NULL, 0U));
        TEST_ASSERT_EQUAL_HEX8(reg8, crc8_h2f_update(reg8, NULL, 0U));
        TEST_ASSERT_EQUAL_HEX16(reg15, crc15_can_update(reg15, NULL, 0U));
        TEST_ASSERT_EQUAL_HEX32(reg32, crc32_update(reg32, NULL, 0U));
    }
}

/* ------------------------------------------------------------------------------------------
 * CRC-32 one-shot helper
 * ---------------------------------------------------------------------------------------- */

/* Initial value XOR final XOR with nothing in between: 0, as zlib.crc32(b"") gives. */
static void test_crc32_of_an_empty_buffer_is_zero(void)
{
    const uint8_t unused = 0x55U;

    TEST_ASSERT_EQUAL_HEX32(0x00000000U, crc32_compute(&unused, 0U));
    TEST_ASSERT_EQUAL_HEX32(0x00000000U, crc32_compute(NULL, 0U));
}

/* Each 0xFF byte cancels eight bits of the initial value 0xFFFFFFFF, so after four of them the
 * register is 0, and the final XOR turns that into 0xFFFFFFFF. An implementation that lacks
 * the initial value or the final XOR cannot produce this result. */
static void test_crc32_of_four_ff_bytes_is_all_ones(void)
{
    static const uint8_t ones[4] = {0xFFU, 0xFFU, 0xFFU, 0xFFU};

    TEST_ASSERT_EQUAL_HEX32(0x00000000U, crc32_update(CRC32_INIT, ones, sizeof(ones)));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFU, crc32_compute(ones, sizeof(ones)));
}

static void test_crc32_compute_is_update_with_initial_value_and_final_xor(void)
{
    uint8_t data[33];

    random_fill(data, sizeof(data));

    for (size_t len = 0U; len <= sizeof(data); len++) {
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(crc32_update(CRC32_INIT, data, len) ^ CRC32_XOROUT,
                                        crc32_compute(data, len), at("length", len));
    }
}

/* ------------------------------------------------------------------------------------------
 * CRC-15/CAN, bit by bit
 * ---------------------------------------------------------------------------------------- */

/*
 * The pseudo-code of the Bosch specification (section 3.2.1, "CRC SEQUENCE"), worked by hand
 * for a 1 followed by seven 0 bits. CRC_RG has 15 bits, so the shift drops bit 14:
 *
 *     input   CRC_RG before   CRCNXT   shifted   XOR 0x4599   CRC_RG after
 *       1        0x0000         1      0x0000       yes          0x4599
 *       0        0x4599         1      0x0B32       yes          0x4EAB
 *       0        0x4EAB         1      0x1D56       yes          0x58CF
 *       0        0x58CF         1      0x319E       yes          0x7407
 *       0        0x7407         1      0x680E       yes          0x2D97
 *       0        0x2D97         0      0x5B2E       no           0x5B2E
 *       0        0x5B2E         1      0x365C       yes          0x73C5
 *       0        0x73C5         1      0x678A       yes          0x2213
 */
static void test_crc15_shift_register_follows_the_specification_step_by_step(void)
{
    static const uint16_t expected[8] = {0x4599U, 0x4EABU, 0x58CFU, 0x7407U,
                                         0x2D97U, 0x5B2EU, 0x73C5U, 0x2213U};
    uint16_t reg = CRC15_CAN_INIT;

    for (size_t step = 0U; step < 8U; step++) {
        const uint8_t bit = (step == 0U) ? 1U : 0U;

        reg = crc15_can_update_bit(reg, bit);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(expected[step], reg, at("step", step));
    }
}

/* CRCNXT is the input bit XOR the bit that leaves the register: all four combinations.
 * 0x4599 has bit 14 set, 0x2D97 has it clear; 0x5B2E XOR 0x4599 is 0x1EB7. */
static void test_crc15_feedback_is_input_bit_xor_outgoing_register_bit(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x5B2EU, crc15_can_update_bit(0x2D97U, 0U)); /* 0 xor 0: shift only */
    TEST_ASSERT_EQUAL_HEX16(0x4EABU, crc15_can_update_bit(0x4599U, 0U)); /* 0 xor 1: subtract */
    TEST_ASSERT_EQUAL_HEX16(0x1EB7U, crc15_can_update_bit(0x2D97U, 1U)); /* 1 xor 0: subtract */
    TEST_ASSERT_EQUAL_HEX16(0x0B32U, crc15_can_update_bit(0x4599U, 1U)); /* 1 xor 1: shift only */
}

/* The byte 0x80 is the bit sequence of the table above and ends at 0x2213. The byte 0x01 is
 * seven 0 bits, which leave an all-zero register alone, and then the 1: 0x4599. Feeding the
 * least significant bit first would swap the two results. */
static void test_crc15_byte_update_feeds_the_most_significant_bit_first(void)
{
    const uint8_t first_bit_set = 0x80U;
    const uint8_t last_bit_set = 0x01U;

    TEST_ASSERT_EQUAL_HEX16(0x2213U, crc15_can_update(CRC15_CAN_INIT, &first_bit_set, 1U));
    TEST_ASSERT_EQUAL_HEX16(0x4599U, crc15_can_update(CRC15_CAN_INIT, &last_bit_set, 1U));
}

static void test_crc15_byte_update_equals_eight_single_bit_updates(void)
{
    uint8_t data[16];
    uint16_t by_bits = 0x1234U;

    random_fill(data, sizeof(data));

    for (size_t i = 0U; i < sizeof(data); i++) {
        for (unsigned int bit = 8U; bit > 0U; bit--) {
            const uint8_t level = (uint8_t)(((uint32_t)data[i] >> (bit - 1U)) & 1U);

            by_bits = crc15_can_update_bit(by_bits, level);
        }
    }

    TEST_ASSERT_EQUAL_HEX16(by_bits, crc15_can_update(0x1234U, data, sizeof(data)));
}

/* The register has 15 bits. Whatever goes in, bit 15 of the result is clear, and bit 15 of
 * the register passed in is not part of the register. */
static void test_crc15_register_stays_within_15_bits(void)
{
    for (size_t trial = 0U; trial < 2000U; trial++) {
        const uint32_t random = random_u32();
        const uint16_t reg = (uint16_t)random;
        const uint8_t bit = (uint8_t)((random >> 16U) & 1U);
        const uint16_t next = crc15_can_update_bit(reg, bit);

        TEST_ASSERT_BITS_LOW_MESSAGE(0x8000U, next, at("trial", trial));
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(crc15_can_update_bit((uint16_t)(reg & 0x7FFFU), bit), next,
                                        at("trial", trial));
    }
}

/* The header asks for 0 or 1. The implementation documents that any other non-zero value
 * counts as a 1 (and not, say, only its least significant bit). */
static void test_crc15_takes_any_non_zero_bit_argument_as_one(void)
{
    static const uint8_t non_zero[] = {0x02U, 0x80U, 0xFEU, 0xFFU};

    for (size_t i = 0U; i < sizeof(non_zero); i++) {
        TEST_ASSERT_EQUAL_HEX16(crc15_can_update_bit(0x0000U, 1U),
                                crc15_can_update_bit(0x0000U, non_zero[i]));
        TEST_ASSERT_EQUAL_HEX16(crc15_can_update_bit(0x4599U, 1U),
                                crc15_can_update_bit(0x4599U, non_zero[i]));
    }
}

/* A CAN receiver runs the register over the frame and over the CRC sequence that follows it,
 * most significant CRC bit first. Without an error it ends at zero, because the transmitted
 * codeword is a multiple of the generator. This only holds because the initial value is 0
 * and there is no final XOR. */
static void test_crc15_over_a_message_and_its_crc_sequence_is_zero(void)
{
    for (size_t trial = 0U; trial < 200U; trial++) {
        uint8_t data[REF_CRC15_MAX_BYTES];
        const size_t len = (size_t)(random_u32() % (sizeof(data) + 1U));

        random_fill(data, sizeof(data));

        const uint16_t crc = crc15_can_update(CRC15_CAN_INIT, data, len);
        uint16_t reg = crc;

        for (unsigned int bit = 15U; bit > 0U; bit--) {
            reg = crc15_can_update_bit(reg, (uint8_t)(((uint32_t)crc >> (bit - 1U)) & 1U));
        }

        TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x0000U, reg, at("trial", trial));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_values_match_the_crc_catalogue);
    RUN_TEST(test_results_match_the_autosar_crc_library_examples);
    RUN_TEST(test_message_followed_by_its_crc_leaves_the_catalogue_residue);
    RUN_TEST(test_every_table_entry_equals_the_bitwise_computation);
    RUN_TEST(test_updates_match_the_bitwise_computation_for_random_data);
    RUN_TEST(test_incremental_update_equals_one_shot_at_every_split_point);
    RUN_TEST(test_zero_length_update_leaves_the_register_unchanged);
    RUN_TEST(test_crc32_of_an_empty_buffer_is_zero);
    RUN_TEST(test_crc32_of_four_ff_bytes_is_all_ones);
    RUN_TEST(test_crc32_compute_is_update_with_initial_value_and_final_xor);
    RUN_TEST(test_crc15_shift_register_follows_the_specification_step_by_step);
    RUN_TEST(test_crc15_feedback_is_input_bit_xor_outgoing_register_bit);
    RUN_TEST(test_crc15_byte_update_feeds_the_most_significant_bit_first);
    RUN_TEST(test_crc15_byte_update_equals_eight_single_bit_updates);
    RUN_TEST(test_crc15_register_stays_within_15_bits);
    RUN_TEST(test_crc15_takes_any_non_zero_bit_argument_as_one);
    RUN_TEST(test_crc15_over_a_message_and_its_crc_sequence_is_zero);
    return UNITY_END();
}
