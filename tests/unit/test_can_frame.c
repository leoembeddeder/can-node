/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "can/can_frame.h"
#include "unity.h"

#define BOTH_FLAGS (CAN_FLAG_EXT | CAN_FLAG_RTR)

/* xorshift32. The state is reset before every test, so a test sees the same "random" frames on
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
    s_random_state = 0xC0FFEE11U;
}

void tearDown(void)
{
}

static can_frame_t make_frame(uint32_t id, uint8_t flags, uint8_t dlc)
{
    const can_frame_t frame = {.id = id, .flags = flags, .dlc = dlc, .data = {0U}};

    return frame;
}

static bool is_valid(uint32_t id, uint8_t flags, uint8_t dlc)
{
    const can_frame_t frame = make_frame(id, flags, dlc);

    return can_frame_is_valid(&frame);
}

static uint32_t key_of(uint32_t id, uint8_t flags)
{
    const can_frame_t frame = make_frame(id, flags, 0U);

    return can_frame_arbitration_key(&frame);
}

/* Message for an assertion on a pair of frames: which pair failed. */
static const char *pair_text(const can_frame_t *a, const can_frame_t *b)
{
    static char text[96];

    (void)snprintf(text, sizeof(text), "a: id 0x%lX flags 0x%02X, b: id 0x%lX flags 0x%02X",
                   (unsigned long)a->id, (unsigned int)a->flags, (unsigned long)b->id,
                   (unsigned int)b->flags);
    return text;
}

/* ------------------------------------------------------------------------------------------
 * Validity
 * ---------------------------------------------------------------------------------------- */

static void test_base_identifier_is_valid_up_to_11_bits(void)
{
    TEST_ASSERT_TRUE(is_valid(0x000U, 0U, 0U));
    TEST_ASSERT_TRUE(is_valid(0x7FFU, 0U, 0U));
    TEST_ASSERT_FALSE(is_valid(0x800U, 0U, 0U));
    TEST_ASSERT_FALSE(is_valid(0x1FFFFFFFU, 0U, 0U));
    TEST_ASSERT_FALSE(is_valid(0xFFFFFFFFU, 0U, 0U));

    /* The limit belongs to the format, not to the frame type. */
    TEST_ASSERT_TRUE(is_valid(0x7FFU, CAN_FLAG_RTR, 0U));
    TEST_ASSERT_FALSE(is_valid(0x800U, CAN_FLAG_RTR, 0U));

    /* 0x7F0 to 0x7FF have their seven most significant bits recessive. The Bosch text
     * excludes them; this project accepts them (see can_frame.c). */
    TEST_ASSERT_TRUE(is_valid(0x7F0U, 0U, 0U));
}

static void test_extended_identifier_is_valid_up_to_29_bits(void)
{
    TEST_ASSERT_TRUE(is_valid(0x00000000U, CAN_FLAG_EXT, 0U));
    TEST_ASSERT_TRUE(is_valid(0x00000800U, CAN_FLAG_EXT, 0U)); /* too wide for a base frame only */
    TEST_ASSERT_TRUE(is_valid(0x1FFFFFFFU, CAN_FLAG_EXT, 0U));
    TEST_ASSERT_FALSE(is_valid(0x20000000U, CAN_FLAG_EXT, 0U));
    TEST_ASSERT_FALSE(is_valid(0x80000000U, CAN_FLAG_EXT, 0U));
    TEST_ASSERT_FALSE(is_valid(0xFFFFFFFFU, CAN_FLAG_EXT, 0U));

    TEST_ASSERT_TRUE(is_valid(0x1FFFFFFFU, BOTH_FLAGS, 0U));
    TEST_ASSERT_FALSE(is_valid(0x20000000U, BOTH_FLAGS, 0U));
}

/* A remote frame has no data, but its DLC field is still a DLC: the limit is the same. */
static void test_dlc_is_valid_up_to_8_in_every_kind_of_frame(void)
{
    static const uint8_t kinds[] = {0U, CAN_FLAG_EXT, CAN_FLAG_RTR, BOTH_FLAGS};

    for (size_t k = 0U; k < sizeof(kinds); k++) {
        for (uint8_t dlc = 0U; dlc <= 8U; dlc++) {
            TEST_ASSERT_TRUE(is_valid(0x123U, kinds[k], dlc));
        }

        TEST_ASSERT_FALSE(is_valid(0x123U, kinds[k], 9U));
        TEST_ASSERT_FALSE(is_valid(0x123U, kinds[k], 15U));
        TEST_ASSERT_FALSE(is_valid(0x123U, kinds[k], 16U));
        TEST_ASSERT_FALSE(is_valid(0x123U, kinds[k], 255U));
    }
}

static void test_unknown_flag_makes_a_frame_invalid(void)
{
    TEST_ASSERT_TRUE(is_valid(0x123U, BOTH_FLAGS, 8U));

    for (unsigned int bit = 4U; bit < 8U; bit++) {
        const uint8_t unknown = (uint8_t)(1U << bit);

        TEST_ASSERT_FALSE(is_valid(0x123U, unknown, 8U));
        TEST_ASSERT_FALSE(is_valid(0x123U, (uint8_t)(unknown | CAN_FLAG_EXT), 8U));
        TEST_ASSERT_FALSE(is_valid(0x123U, (uint8_t)(unknown | CAN_FLAG_RTR), 8U));
        TEST_ASSERT_FALSE(is_valid(0x123U, (uint8_t)(unknown | BOTH_FLAGS), 8U));
    }

    TEST_ASSERT_FALSE(is_valid(0x123U, 0xFFU, 8U));
}

static void test_null_frame_is_invalid(void)
{
    TEST_ASSERT_FALSE(can_frame_is_valid(NULL));
}

/* ------------------------------------------------------------------------------------------
 * Arbitration key: exact values
 * ---------------------------------------------------------------------------------------- */

/*
 * The arbitration field written out by hand in wire order (Bosch CAN Specification 2.0, Part
 * B, section 3.2.1), the first transmitted bit on the left and in bit 31 of the key:
 *
 *   base 0x123, data        001 0010 0011 | RTR 0 | IDE 0 | nothing more
 *                           0010 0100 0110 0000 0000 0000 0000 0000              = 0x24600000
 *   base 0x123, remote      the same with RTR 1                                 = 0x24700000
 *   base 0x7FF, data        111 1111 1111 | 0 | 0                               = 0xFFE00000
 *
 *   extended 0x12345678     base part  0x12345678 >> 18     = 0x48D   = 100 1000 1101
 *                           extension  0x12345678 & 0x3FFFF = 0x05678 = 00 0101 0110 0111 1000
 *     data                  100 1000 1101 | SRR 1 | IDE 1 | 00 0101 0110 0111 1000 | RTR 0
 *                           1001 0001 1011 1000 1010 1100 1111 0000              = 0x91B8ACF0
 *     remote                the same with RTR 1                                 = 0x91B8ACF1
 *   extended 0, data        only SRR and IDE are recessive                      = 0x00180000
 *   extended 0x1FFFFFFF, remote   every bit recessive                           = 0xFFFFFFFF
 */
static void test_key_is_the_arbitration_field_with_the_first_bit_on_top(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x00000000U, key_of(0x000U, 0U));
    TEST_ASSERT_EQUAL_HEX32(0x24600000U, key_of(0x123U, 0U));
    TEST_ASSERT_EQUAL_HEX32(0x24700000U, key_of(0x123U, CAN_FLAG_RTR));
    TEST_ASSERT_EQUAL_HEX32(0xFFE00000U, key_of(0x7FFU, 0U));

    TEST_ASSERT_EQUAL_HEX32(0x91B8ACF0U, key_of(0x12345678U, CAN_FLAG_EXT));
    TEST_ASSERT_EQUAL_HEX32(0x91B8ACF1U, key_of(0x12345678U, BOTH_FLAGS));
    TEST_ASSERT_EQUAL_HEX32(0x00180000U, key_of(0x00000000U, CAN_FLAG_EXT));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFU, key_of(0x1FFFFFFFU, BOTH_FLAGS));
}

/* ------------------------------------------------------------------------------------------
 * Arbitration key: who wins
 * ---------------------------------------------------------------------------------------- */

static void test_lower_identifier_wins(void)
{
    TEST_ASSERT_LESS_THAN_UINT32(key_of(0x101U, 0U), key_of(0x100U, 0U));
    TEST_ASSERT_LESS_THAN_UINT32(key_of(0x7FFU, 0U), key_of(0x000U, 0U));
    TEST_ASSERT_LESS_THAN_UINT32(key_of(0x400U, 0U), key_of(0x3FFU, 0U)); /* first bit decides */

    TEST_ASSERT_LESS_THAN_UINT32(key_of(0x00000001U, CAN_FLAG_EXT),
                                 key_of(0x00000000U, CAN_FLAG_EXT));
    TEST_ASSERT_LESS_THAN_UINT32(key_of(0x1FFFFFFFU, CAN_FLAG_EXT),
                                 key_of(0x1FFFFFFEU, CAN_FLAG_EXT));

    /* The base part of an extended identifier is sent before the extension and outranks it:
     * all extension bits recessive still beats the next base part. */
    TEST_ASSERT_LESS_THAN_UINT32(key_of(0x04840000U, CAN_FLAG_EXT),
                                 key_of(0x0483FFFFU, CAN_FLAG_EXT));
}

/* Section 3.2.1, "SRR BIT": the standard frame prevails. The extended frame sends the
 * recessive SRR where the base data frame sends its dominant RTR. Against a base REMOTE frame
 * SRR and RTR are equal, and the next bit decides: IDE, dominant in the base frame. */
static void test_base_frame_beats_extended_frame_with_the_same_base_identifier(void)
{
    const uint32_t base_id = 0x123U;
    const uint32_t ext_lowest = base_id << 18U; /* extension all dominant: its best case */
    const uint32_t ext_highest = (base_id << 18U) | 0x3FFFFU;

    TEST_ASSERT_LESS_THAN_UINT32(key_of(ext_lowest, CAN_FLAG_EXT), key_of(base_id, 0U));
    TEST_ASSERT_LESS_THAN_UINT32(key_of(ext_highest, BOTH_FLAGS), key_of(base_id, 0U));

    TEST_ASSERT_LESS_THAN_UINT32(key_of(ext_lowest, CAN_FLAG_EXT), key_of(base_id, CAN_FLAG_RTR));
    TEST_ASSERT_LESS_THAN_UINT32(key_of(ext_lowest, BOTH_FLAGS), key_of(base_id, CAN_FLAG_RTR));

    /* Only the same base identifier gives the base frame this advantage: an extended frame
     * with a lower base part is decided earlier and wins, with the worst possible rest. */
    TEST_ASSERT_LESS_THAN_UINT32(key_of(base_id, 0U),
                                 key_of(((base_id - 1U) << 18U) | 0x3FFFFU, BOTH_FLAGS));
}

static void test_data_frame_beats_remote_frame_with_the_same_identifier(void)
{
    TEST_ASSERT_LESS_THAN_UINT32(key_of(0x123U, CAN_FLAG_RTR), key_of(0x123U, 0U));
    TEST_ASSERT_LESS_THAN_UINT32(key_of(0x12345678U, BOTH_FLAGS),
                                 key_of(0x12345678U, CAN_FLAG_EXT));

    /* RTR follows the identifier it belongs to, so it only decides between equal
     * identifiers: the remote frame of a lower identifier beats the data frame of a higher. */
    TEST_ASSERT_LESS_THAN_UINT32(key_of(0x124U, 0U), key_of(0x123U, CAN_FLAG_RTR));
    TEST_ASSERT_LESS_THAN_UINT32(key_of(0x12345679U, CAN_FLAG_EXT),
                                 key_of(0x12345678U, BOTH_FLAGS));
}

static void test_extended_frames_that_differ_only_in_the_extension_rank_by_it(void)
{
    const uint32_t base_part = 0x123U << 18U;

    TEST_ASSERT_LESS_THAN_UINT32(key_of(base_part | 0x00002U, CAN_FLAG_EXT),
                                 key_of(base_part | 0x00001U, CAN_FLAG_EXT));
    TEST_ASSERT_LESS_THAN_UINT32(key_of(base_part | 0x20000U, CAN_FLAG_EXT),
                                 key_of(base_part | 0x1FFFFU, CAN_FLAG_EXT)); /* ID-17 decides */
    TEST_ASSERT_LESS_THAN_UINT32(key_of(base_part | 0x3FFFFU, CAN_FLAG_EXT),
                                 key_of(base_part | 0x3FFFEU, CAN_FLAG_EXT)); /* ID-0 decides */
}

/* Arbitration ends with the arbitration field. Frames that are equal up to there are not
 * ranked by what follows. */
static void test_key_does_not_depend_on_dlc_or_data(void)
{
    static const uint8_t kinds[] = {0U, CAN_FLAG_EXT, CAN_FLAG_RTR, BOTH_FLAGS};

    for (size_t k = 0U; k < sizeof(kinds); k++) {
        const can_frame_t empty = make_frame(0x155U, kinds[k], 0U);
        can_frame_t full = make_frame(0x155U, kinds[k], 8U);

        for (size_t i = 0U; i < sizeof(full.data); i++) {
            full.data[i] = 0xFFU;
        }

        TEST_ASSERT_EQUAL_HEX32(can_frame_arbitration_key(&empty),
                                can_frame_arbitration_key(&full));
    }
}

/* The header leaves the key of an invalid frame open. The implementation documents what it
 * does with one: identifier bits beyond the format are dropped and flags it does not know are
 * not looked at, so that the result is still the key of some frame and no stray bit lands in
 * a neighbouring field of the key. */
static void test_key_of_an_invalid_frame_is_that_of_the_frame_cut_down_to_its_format(void)
{
    TEST_ASSERT_EQUAL_HEX32(key_of(0x123U, 0U), key_of(0xFFFFF923U, 0U));
    TEST_ASSERT_EQUAL_HEX32(key_of(0x7FFU, CAN_FLAG_RTR), key_of(0xFFFFFFFFU, CAN_FLAG_RTR));
    TEST_ASSERT_EQUAL_HEX32(key_of(0x12345678U, CAN_FLAG_EXT), key_of(0xF2345678U, CAN_FLAG_EXT));
    TEST_ASSERT_EQUAL_HEX32(key_of(0x1FFFFFFFU, BOTH_FLAGS), key_of(0xFFFFFFFFU, BOTH_FLAGS));

    TEST_ASSERT_EQUAL_HEX32(key_of(0x123U, 0U), key_of(0x123U, 0xFCU));
    TEST_ASSERT_EQUAL_HEX32(key_of(0x123U, CAN_FLAG_RTR), key_of(0x123U, 0x04U | CAN_FLAG_RTR));
    TEST_ASSERT_EQUAL_HEX32(key_of(0x12345678U, BOTH_FLAGS), key_of(0x12345678U, 0xFFU));
}

/* ------------------------------------------------------------------------------------------
 * Arbitration key: properties
 * ---------------------------------------------------------------------------------------- */

/* A key that grows with every step of the identifier orders all 2048 base data frames the way
 * their identifiers are ordered. */
static void test_key_of_base_data_frames_grows_with_every_identifier(void)
{
    for (uint32_t id = 0U; id < CAN_ID_STD_MAX; id++) {
        TEST_ASSERT_LESS_THAN_UINT32(key_of(id + 1U, 0U), key_of(id, 0U));
    }
}

static void test_key_order_of_random_base_data_frames_is_identifier_order(void)
{
    for (size_t trial = 0U; trial < 5000U; trial++) {
        const uint32_t id_a = random_u32() & CAN_ID_STD_MAX;
        uint32_t id_b = random_u32() & CAN_ID_STD_MAX;

        /* Two independent draws are hardly ever equal or close, and close identifiers are the
         * pairs that only the last identifier bits decide. One pair in eight is made equal and
         * one in eight 1 to 3 apart. */
        switch (random_u32() % 8U) {
        case 0U:
            id_b = id_a;
            break;
        case 1U:
            id_b = (id_a + 1U + (random_u32() % 3U)) & CAN_ID_STD_MAX;
            break;
        default:
            break;
        }

        const can_frame_t a = make_frame(id_a, 0U, (uint8_t)(random_u32() % 9U));
        const can_frame_t b = make_frame(id_b, 0U, (uint8_t)(random_u32() % 9U));
        const uint32_t key_a = can_frame_arbitration_key(&a);
        const uint32_t key_b = can_frame_arbitration_key(&b);

        TEST_ASSERT_EQUAL_MESSAGE(id_a < id_b, key_a < key_b, pair_text(&a, &b));
        TEST_ASSERT_EQUAL_MESSAGE(id_a == id_b, key_a == key_b, pair_text(&a, &b));
    }
}

#define ARBITRATION_BITS_MAX 32U

/* The bits that decide arbitration, one array element per bit in wire order, taken from the
 * frame formats of section 3.2.1 independently of the key. Returns how many there are. */
static size_t arbitration_bits(const can_frame_t *frame, uint8_t bits[ARBITRATION_BITS_MAX])
{
    const uint8_t rtr = ((frame->flags & CAN_FLAG_RTR) != 0U) ? 1U : 0U;
    size_t count = 0U;

    if ((frame->flags & CAN_FLAG_EXT) != 0U) {
        for (unsigned int n = 28U; n >= 18U; n--) { /* base identifier ID-28 .. ID-18 */
            bits[count] = (uint8_t)((frame->id >> n) & 1U);
            count++;
        }

        bits[count] = 1U; /* SRR: recessive */
        count++;
        bits[count] = 1U; /* IDE: recessive in the extended format */
        count++;

        for (unsigned int n = 18U; n > 0U; n--) { /* identifier extension ID-17 .. ID-0 */
            bits[count] = (uint8_t)((frame->id >> (n - 1U)) & 1U);
            count++;
        }

        bits[count] = rtr;
        count++;
    } else {
        for (unsigned int n = 11U; n > 0U; n--) { /* identifier, here numbered 10 .. 0 */
            bits[count] = (uint8_t)((frame->id >> (n - 1U)) & 1U);
            count++;
        }

        bits[count] = rtr;
        count++;
        bits[count] = 0U; /* IDE: dominant in the base format */
        count++;
    }

    return count;
}

/* Two nodes start a frame at the same time and each sends its bits onto a wired-AND bus. At
 * the first position where they differ the bus is dominant (0) and the node that sent
 * recessive (1) stops. Returns -1 if `a` keeps the bus, 1 if `b` does, and 0 if nothing
 * separates them. A base and an extended frame always differ in IDE at the latest, so
 * comparing as many bits as the shorter one has is enough. */
static int arbitrate(const can_frame_t *a, const can_frame_t *b)
{
    uint8_t bits_a[ARBITRATION_BITS_MAX];
    uint8_t bits_b[ARBITRATION_BITS_MAX];
    const size_t count_a = arbitration_bits(a, bits_a);
    const size_t count_b = arbitration_bits(b, bits_b);
    const size_t count = (count_a < count_b) ? count_a : count_b;
    int winner = 0;

    for (size_t i = 0U; (i < count) && (winner == 0); i++) {
        if (bits_a[i] != bits_b[i]) {
            winner = (bits_a[i] == 0U) ? -1 : 1;
        }
    }

    return winner;
}

static int compare_keys(const can_frame_t *a, const can_frame_t *b)
{
    const uint32_t key_a = can_frame_arbitration_key(a);
    const uint32_t key_b = can_frame_arbitration_key(b);
    int order;

    if (key_a < key_b) {
        order = -1;
    } else if (key_a > key_b) {
        order = 1;
    } else {
        order = 0;
    }

    return order;
}

static can_frame_t random_frame(void)
{
    const uint8_t flags = (uint8_t)(random_u32() & BOTH_FLAGS);
    const uint32_t id_max = ((flags & CAN_FLAG_EXT) != 0U) ? CAN_ID_EXT_MAX : CAN_ID_STD_MAX;

    return make_frame(random_u32() & id_max, flags, (uint8_t)(random_u32() % 9U));
}

/* A frame that is close to `frame`: independent random frames almost always differ in one of
 * their first identifier bits, which would leave SRR, IDE, the extension and RTR undecided
 * in nearly every pair. */
static can_frame_t related_frame(const can_frame_t *frame)
{
    can_frame_t other = *frame;
    const bool extended = (frame->flags & CAN_FLAG_EXT) != 0U;

    switch (random_u32() % 5U) {
    case 0U: /* the same base identifier in the other format */
        if (extended) {
            other.id = frame->id >> 18U;
        } else {
            other.id = (frame->id << 18U) | (random_u32() & 0x3FFFFU);
        }
        other.flags ^= CAN_FLAG_EXT;
        break;
    case 1U: /* the other frame type */
        other.flags ^= CAN_FLAG_RTR;
        break;
    case 2U: /* one identifier bit different */
        other.id ^= (uint32_t)1U << (random_u32() % (extended ? 29U : 11U));
        break;
    case 3U: /* the same base identifier in the other format, and the other frame type */
        if (extended) {
            other.id = frame->id >> 18U;
        } else {
            other.id = frame->id << 18U;
        }
        other.flags ^= BOTH_FLAGS;
        break;
    default: /* the same arbitration field */
        other.dlc = (uint8_t)(random_u32() % 9U);
        break;
    }

    return other;
}

/* The claim of the header, for all four kinds of frame against each other: comparing keys
 * gives the same winner as putting the two arbitration fields on the bus bit by bit. */
static void test_key_order_equals_bitwise_arbitration_for_random_frames(void)
{
    unsigned int ties = 0U;

    for (size_t trial = 0U; trial < 20000U; trial++) {
        const can_frame_t a = random_frame();
        const can_frame_t b = ((trial % 4U) == 0U) ? random_frame() : related_frame(&a);
        const int winner = arbitrate(&a, &b);

        TEST_ASSERT_TRUE_MESSAGE(can_frame_is_valid(&a) && can_frame_is_valid(&b),
                                 pair_text(&a, &b));
        TEST_ASSERT_EQUAL_INT_MESSAGE(winner, compare_keys(&a, &b), pair_text(&a, &b));
        TEST_ASSERT_EQUAL_INT_MESSAGE(-winner, compare_keys(&b, &a), pair_text(&a, &b));

        if (winner == 0) {
            ties++;
        }
    }

    /* The generator has to produce ties as well as decisions, or half of the claim (equal
     * arbitration fields give equal keys) would go untested. */
    TEST_ASSERT_GREATER_THAN_UINT(1000U, ties);
}

static void test_can_fd_frames_validate_flags_and_discrete_lengths(void)
{
    static const uint8_t valid_fd_lens[] = {0U, 1U,  2U,  3U,  4U,  5U,  6U,  7U,
                                            8U, 12U, 16U, 20U, 24U, 32U, 48U, 64U};
    static const uint8_t bad_fd_lens[] = {9U, 10U, 11U, 13U, 15U, 17U, 25U, 33U, 49U, 65U, 255U};

    for (size_t i = 0U; i < (sizeof(valid_fd_lens) / sizeof(valid_fd_lens[0])); i++) {
        TEST_ASSERT_TRUE(is_valid(0x123U, CAN_FLAG_FD, valid_fd_lens[i]));
        TEST_ASSERT_TRUE(is_valid(0x123U, CAN_FLAG_FD | CAN_FLAG_BRS, valid_fd_lens[i]));
        TEST_ASSERT_TRUE(
            is_valid(0x1FFFFFFFU, CAN_FLAG_EXT | CAN_FLAG_FD | CAN_FLAG_BRS, valid_fd_lens[i]));
    }

    for (size_t i = 0U; i < (sizeof(bad_fd_lens) / sizeof(bad_fd_lens[0])); i++) {
        TEST_ASSERT_FALSE(is_valid(0x123U, CAN_FLAG_FD, bad_fd_lens[i]));
        TEST_ASSERT_FALSE(is_valid(0x123U, CAN_FLAG_FD | CAN_FLAG_BRS, bad_fd_lens[i]));
    }

    /* CAN FD never allows RTR; BRS is invalid without CAN_FLAG_FD. */
    TEST_ASSERT_FALSE(is_valid(0x123U, CAN_FLAG_FD | CAN_FLAG_RTR, 8U));
    TEST_ASSERT_FALSE(is_valid(0x123U, CAN_FLAG_BRS, 8U));
}

static void test_can_fd_length_and_dlc_helpers(void)
{
    static const uint8_t expected_lens[16] = {0U, 1U,  2U,  3U,  4U,  5U,  6U,  7U,
                                              8U, 12U, 16U, 20U, 24U, 32U, 48U, 64U};

    for (uint8_t dlc = 0U; dlc < 16U; dlc++) {
        TEST_ASSERT_EQUAL_UINT8(expected_lens[dlc], can_fd_dlc_to_len(dlc));
        TEST_ASSERT_TRUE(can_fd_len_is_valid(expected_lens[dlc]));
        TEST_ASSERT_EQUAL_UINT8(expected_lens[dlc], can_fd_pad_len(expected_lens[dlc]));
        TEST_ASSERT_EQUAL_UINT8(dlc, can_fd_len_to_dlc(expected_lens[dlc]));
    }
    TEST_ASSERT_EQUAL_UINT8(64U, can_fd_dlc_to_len(16U));
    TEST_ASSERT_EQUAL_UINT8(64U, can_fd_dlc_to_len(255U));

    TEST_ASSERT_EQUAL_UINT8(12U, can_fd_pad_len(9U));
    TEST_ASSERT_EQUAL_UINT8(12U, can_fd_pad_len(10U));
    TEST_ASSERT_EQUAL_UINT8(16U, can_fd_pad_len(13U));
    TEST_ASSERT_EQUAL_UINT8(32U, can_fd_pad_len(25U));
    TEST_ASSERT_EQUAL_UINT8(48U, can_fd_pad_len(33U));
    TEST_ASSERT_EQUAL_UINT8(64U, can_fd_pad_len(49U));
    TEST_ASSERT_EQUAL_UINT8(64U, can_fd_pad_len(100U));

    TEST_ASSERT_EQUAL_UINT8(9U, can_fd_len_to_dlc(9U));
    TEST_ASSERT_EQUAL_UINT8(15U, can_fd_len_to_dlc(65U));

    TEST_ASSERT_FALSE(can_fd_len_is_valid(9U));
    TEST_ASSERT_FALSE(can_fd_len_is_valid(63U));
    TEST_ASSERT_FALSE(can_fd_len_is_valid(65U));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_base_identifier_is_valid_up_to_11_bits);
    RUN_TEST(test_extended_identifier_is_valid_up_to_29_bits);
    RUN_TEST(test_dlc_is_valid_up_to_8_in_every_kind_of_frame);
    RUN_TEST(test_unknown_flag_makes_a_frame_invalid);
    RUN_TEST(test_null_frame_is_invalid);
    RUN_TEST(test_can_fd_frames_validate_flags_and_discrete_lengths);
    RUN_TEST(test_can_fd_length_and_dlc_helpers);
    RUN_TEST(test_key_is_the_arbitration_field_with_the_first_bit_on_top);
    RUN_TEST(test_lower_identifier_wins);
    RUN_TEST(test_base_frame_beats_extended_frame_with_the_same_base_identifier);
    RUN_TEST(test_data_frame_beats_remote_frame_with_the_same_identifier);
    RUN_TEST(test_extended_frames_that_differ_only_in_the_extension_rank_by_it);
    RUN_TEST(test_key_does_not_depend_on_dlc_or_data);
    RUN_TEST(test_key_of_an_invalid_frame_is_that_of_the_frame_cut_down_to_its_format);
    RUN_TEST(test_key_of_base_data_frames_grows_with_every_identifier);
    RUN_TEST(test_key_order_of_random_base_data_frames_is_identifier_order);
    RUN_TEST(test_key_order_equals_bitwise_arbitration_for_random_frames);
    return UNITY_END();
}
