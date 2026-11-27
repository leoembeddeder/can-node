/* SPDX-License-Identifier: MIT */
/*
 * The frame lengths of can_bits.c, checked against values that do not come from it:
 *
 *  - two frames captured from a real bus with a logic analyser;
 *  - frames worked out by hand from the Bosch CAN Specification 2.0 (Part B, sections 3.2.1,
 *    3.2.2 and 6), with the working in the comments;
 *  - a second model of the transmitter, written for this file in the most literal way there
 *    is (a list of bits, polynomial division, a search for five equal bits). The frames above
 *    vouch for the model as well, and it is then compared with the implementation on some
 *    35 000 further frames.
 *
 * In the comments, bits are written in the order of transmission, 0 for dominant and 1 for
 * recessive, and a stuff bit is written in brackets: 00000[1].
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "can/can_bits.h"
#include "unity.h"

#define BOTH_FLAGS (CAN_FLAG_EXT | CAN_FLAG_RTR)

/* The four kinds of frame: data frames in base and extended format, then remote frames. */
static const uint8_t s_kinds[4] = {0U, CAN_FLAG_EXT, CAN_FLAG_RTR, BOTH_FLAGS};

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
    s_random_state = 0xB175CA9EU;
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------------------- */

static bool is_extended(const can_frame_t *frame)
{
    return (frame->flags & CAN_FLAG_EXT) != 0U;
}

static bool is_remote(const can_frame_t *frame)
{
    return (frame->flags & CAN_FLAG_RTR) != 0U;
}

/* A frame with the first `dlc` bytes of `data` (none if `data` is NULL) and zeros behind. */
static can_frame_t make_frame(uint32_t id, uint8_t flags, uint8_t dlc, const uint8_t *data)
{
    can_frame_t frame = {.id = id, .flags = flags, .dlc = dlc, .data = {0U}};

    if (data != NULL) {
        for (size_t i = 0U; (i < dlc) && (i < sizeof(frame.data)); i++) {
            frame.data[i] = data[i];
        }
    }

    return frame;
}

/* Message for an assertion inside a loop: which case failed. */
static const char *at(const char *what, size_t value)
{
    static char text[48];

    (void)snprintf(text, sizeof(text), "%s %lu", what, (unsigned long)value);
    return text;
}

/* Message for an assertion on a frame: which frame failed. */
static const char *frame_text(const can_frame_t *frame)
{
    static char text[112];

    (void)snprintf(text, sizeof(text),
                   "id 0x%lX flags 0x%02X dlc %u data %02X %02X %02X %02X %02X %02X %02X %02X",
                   (unsigned long)frame->id, (unsigned int)frame->flags, (unsigned int)frame->dlc,
                   (unsigned int)frame->data[0], (unsigned int)frame->data[1],
                   (unsigned int)frame->data[2], (unsigned int)frame->data[3],
                   (unsigned int)frame->data[4], (unsigned int)frame->data[5],
                   (unsigned int)frame->data[6], (unsigned int)frame->data[7]);
    return text;
}

/* can_bits_exact() of a frame that has to be accepted. */
static can_bits_info_t exact(const can_frame_t *frame)
{
    can_bits_info_t info = {.frame_bits = 0U, .stuff_bits = 0U, .crc15 = 0U};

    TEST_ASSERT_TRUE_MESSAGE(can_bits_exact(frame, &info), frame_text(frame));
    return info;
}

/* ------------------------------------------------------------------------------------------
 * The model: a second way to the same answer
 *
 * It shares no code and no method with can_bits.c. The implementation sends a frame bit by
 * bit through a shift register and a run counter; the model writes the whole frame down
 * first and then works on the list, the way one does it on paper.
 * ---------------------------------------------------------------------------------------- */

/* Room for the longest stuffed sequence, 118 bits and 29 stuff bits, and for the 15 zeros the
 * division appends. */
#define MODEL_MAX_BITS 160U

#define MODEL_CRC_BITS  15U
#define MODEL_TAIL_BITS 10U /* CRC delimiter, ACK slot, ACK delimiter, 7 bits end of frame */

typedef struct {
    uint8_t bit[MODEL_MAX_BITS];
    size_t count;
} bit_list_t;

static void list_push(bit_list_t *list, uint32_t bit)
{
    TEST_ASSERT_LESS_THAN_size_t(MODEL_MAX_BITS, list->count);
    list->bit[list->count] = (uint8_t)(bit & 1U);
    list->count++;
}

/* Bits `first` down to `last` of `value`, in that order: the specification numbers the bits
 * of a field from the one sent first downwards (ID-28 ... ID-18, DLC3 ... DLC0). */
static void list_push_field(bit_list_t *list, uint32_t value, unsigned int first, unsigned int last)
{
    for (unsigned int n = first + 1U; n > last; n--) {
        list_push(list, value >> (n - 1U));
    }
}

/* Start of frame to the end of the data field, field by field as sections 3.2.1 (data frame)
 * and 3.2.2 (remote frame: the same without the data field) list them. */
static void model_fields(const can_frame_t *frame, bit_list_t *list)
{
    const uint32_t rtr = is_remote(frame) ? 1U : 0U;

    list->count = 0U;
    list_push(list, 0U); /* start of frame: dominant */

    if (is_extended(frame)) {
        list_push_field(list, frame->id, 28U, 18U); /* base identifier */
        list_push(list, 1U);                        /* SRR: recessive */
        list_push(list, 1U);                        /* IDE: recessive in the extended format */
        list_push_field(list, frame->id, 17U, 0U);  /* identifier extension */
        list_push(list, rtr);
        list_push(list, 0U); /* r1: dominant */
        list_push(list, 0U); /* r0: dominant */
    } else {
        list_push_field(list, frame->id, 10U, 0U); /* identifier, there called ID-28 .. ID-18 */
        list_push(list, rtr);
        list_push(list, 0U); /* IDE: dominant in the base format */
        list_push(list, 0U); /* r0: dominant */
    }

    list_push_field(list, frame->dlc, 3U, 0U);

    if (!is_remote(frame)) {
        for (size_t i = 0U; i < frame->dlc; i++) {
            list_push_field(list, frame->data[i], 7U, 0U);
        }
    }
}

/* The CRC by its definition (section 3.2.1, "CRC SEQUENCE"): the remainder of the division of
 * the bit stream, followed by 15 zeros, by the generator polynomial. Schoolbook long division
 * on a list of coefficients; no shift register. */
static uint16_t model_crc(const bit_list_t *fields)
{
    /* x^15 + x^14 + x^10 + x^8 + x^7 + x^4 + x^3 + 1, coefficient of x^15 first */
    static const uint8_t generator[MODEL_CRC_BITS + 1U] = {1U, 1U, 0U, 0U, 0U, 1U, 0U, 1U,
                                                           1U, 0U, 0U, 1U, 1U, 0U, 0U, 1U};
    uint8_t work[MODEL_MAX_BITS] = {0U};
    uint16_t crc = 0U;

    TEST_ASSERT_LESS_OR_EQUAL_size_t(MODEL_MAX_BITS - MODEL_CRC_BITS, fields->count);

    for (size_t i = 0U; i < fields->count; i++) {
        work[i] = fields->bit[i];
    }

    for (size_t i = 0U; i < fields->count; i++) {
        if (work[i] != 0U) {
            for (size_t j = 0U; j <= MODEL_CRC_BITS; j++) {
                work[i + j] ^= generator[j];
            }
        }
    }

    for (size_t i = 0U; i < MODEL_CRC_BITS; i++) {
        crc = (uint16_t)((crc << 1U) | work[fields->count + i]);
    }

    return crc;
}

/* The CRC of a single recessive bit that is followed by `zeros` dominant bits. */
static uint16_t model_single_bit_crc(size_t zeros)
{
    bit_list_t bits = {.count = 0U};

    list_push(&bits, 1U);

    for (size_t i = 0U; i < zeros; i++) {
        list_push(&bits, 0U);
    }

    return model_crc(&bits);
}

static bool last_five_are_equal(const bit_list_t *list)
{
    bool equal = (list->count >= 5U);

    for (size_t back = 2U; equal && (back <= 5U); back++) {
        equal = (list->bit[list->count - back] == list->bit[list->count - 1U]);
    }

    return equal;
}

/* Bit stuffing (section 6): copy the bits, and whenever the last five bits of the copy are
 * equal, add the opposite bit to the copy. The copy is "the actual transmitted bit stream",
 * so a stuff bit is looked at again like any other bit. Returns the number of stuff bits. */
static size_t model_stuff(const bit_list_t *bits, bit_list_t *wire)
{
    size_t stuff_bits = 0U;

    wire->count = 0U;

    for (size_t i = 0U; i < bits->count; i++) {
        list_push(wire, bits->bit[i]);

        if (last_five_are_equal(wire)) {
            list_push(wire, (bits->bit[i] == 0U) ? 1U : 0U);
            stuff_bits++;
        }
    }

    return stuff_bits;
}

static can_bits_info_t model_exact(const can_frame_t *frame)
{
    bit_list_t sequence;
    bit_list_t wire;
    can_bits_info_t info;

    model_fields(frame, &sequence);
    info.crc15 = model_crc(&sequence);
    list_push_field(&sequence, info.crc15, MODEL_CRC_BITS - 1U, 0U);
    info.stuff_bits = (uint8_t)model_stuff(&sequence, &wire);
    info.frame_bits = (uint16_t)(wire.count + MODEL_TAIL_BITS);

    return info;
}

/* Length, stuff bit count and CRC of a frame as they are known from elsewhere. Both the
 * implementation and the model have to reproduce them, which is what entitles the model to
 * judge the implementation on frames nobody worked out. A failure is reported at the line of
 * the caller. */
#define ASSERT_FRAME(bits, stuff, crc, frame)                                                      \
    assert_frame((bits), (stuff), (crc), (frame), __LINE__)

static void assert_frame(uint16_t bits, uint8_t stuff, uint16_t crc, const can_frame_t *frame,
                         int line)
{
    const can_bits_info_t model = model_exact(frame);
    can_bits_info_t info = {.frame_bits = 0U, .stuff_bits = 0U, .crc15 = 0U};

    UNITY_TEST_ASSERT(can_bits_exact(frame, &info), line, "the frame was refused");
    UNITY_TEST_ASSERT_EQUAL_HEX16(crc, info.crc15, line, "CRC");
    UNITY_TEST_ASSERT_EQUAL_UINT8(stuff, info.stuff_bits, line, "stuff bits");
    UNITY_TEST_ASSERT_EQUAL_UINT16(bits, info.frame_bits, line, "frame bits");
    UNITY_TEST_ASSERT_EQUAL_HEX16(crc, model.crc15, line, "CRC of the model");
    UNITY_TEST_ASSERT_EQUAL_UINT8(stuff, model.stuff_bits, line, "stuff bits of the model");
    UNITY_TEST_ASSERT_EQUAL_UINT16(bits, model.frame_bits, line, "frame bits of the model");
}

static void assert_same_as_model(const can_frame_t *frame)
{
    const can_bits_info_t model = model_exact(frame);
    const can_bits_info_t info = exact(frame);
    const char *text = frame_text(frame);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(model.crc15, info.crc15, text);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(model.stuff_bits, info.stuff_bits, text);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(model.frame_bits, info.frame_bits, text);
}

/* Uniformly random bytes hardly ever hold five equal bits in a row. Every second byte is
 * taken from a list of bytes with long runs, so that stuff bits are the rule in the random
 * frames and runs cross from one byte into the next. */
static uint8_t random_data_byte(void)
{
    static const uint8_t runs[12] = {0x00U, 0xFFU, 0x0FU, 0xF0U, 0x07U, 0xE0U,
                                     0x1FU, 0xF8U, 0x83U, 0x7CU, 0x3EU, 0xC1U};
    const uint32_t random = random_u32();
    uint8_t byte = (uint8_t)(random >> 24U);

    if ((random & 1U) != 0U) {
        byte = runs[(random >> 8U) % sizeof(runs)];
    }

    return byte;
}

/* A valid frame of the given kind and DLC. All eight data bytes are filled, also those beyond
 * the DLC and those of a remote frame: they are not transmitted and must not matter. */
static can_frame_t random_frame(uint8_t flags, uint8_t dlc)
{
    const uint32_t id_max = ((flags & CAN_FLAG_EXT) != 0U) ? CAN_ID_EXT_MAX : CAN_ID_STD_MAX;
    can_frame_t frame = make_frame(random_u32() & id_max, flags, dlc, NULL);

    /* One identifier in four is all dominant or all recessive: the longest runs there are. */
    switch (random_u32() % 8U) {
    case 0U:
        frame.id = 0U;
        break;
    case 1U:
        frame.id = id_max;
        break;
    default:
        break;
    }

    for (size_t i = 0U; i < sizeof(frame.data); i++) {
        frame.data[i] = random_data_byte();
    }

    return frame;
}

/* The frame whose identifier and data bits are all dominant, or all recessive: the longest
 * runs that a frame of its kind and DLC can hold. */
static can_frame_t uniform_frame(uint8_t flags, uint8_t dlc, bool recessive)
{
    can_frame_t frame = make_frame(0U, flags, dlc, NULL);

    if (recessive) {
        frame.id = ((flags & CAN_FLAG_EXT) != 0U) ? CAN_ID_EXT_MAX : CAN_ID_STD_MAX;

        for (size_t i = 0U; i < sizeof(frame.data); i++) {
            frame.data[i] = 0xFFU;
        }
    }

    return frame;
}

/* ------------------------------------------------------------------------------------------
 * Frames captured from a bus
 * ---------------------------------------------------------------------------------------- */

/*
 * Logic-analyser capture from the test suite of the sigrok CAN decoder.
 *
 *   SOF ID          RTR IDE r0 DLC   data
 *   0   01000100010 0   0   0  0101  00000000 00010001 00100010 00110011 01000100
 *
 * The stuff bits can be found without knowing the CRC, 66DAh = 110011011011010, because it
 * holds no long run: one after ID-18, RTR, IDE, r0 and DLC3 (00000), one after the first five
 * bits of the first data byte, and one after its other three bits and the first two of the
 * second byte. 19 + 40 + 15 bits, 3 stuff bits and the 10 bits after the CRC sequence: 87.
 */
static void test_base_frame_captured_by_a_logic_analyser(void)
{
    static const uint8_t data[5] = {0x00U, 0x11U, 0x22U, 0x33U, 0x44U};
    const can_frame_t frame = make_frame(0x222U, 0U, 5U, data);

    ASSERT_FRAME(87U, 3U, 0x66DAU, &frame);
}

/*
 * From the same test suite, in extended format. 11223344h is base identifier 448h and
 * identifier extension 23344h:
 *
 *   SOF ID-28..18   SRR IDE ID-17..0           RTR r1 r0 DLC   data
 *   0   10001001000 1   1   100011001101000100 0   0  0  0111  00 11 22 33 44 55 66 (hex)
 *
 * Stuff bits: after ID-1, ID-0, RTR, r1 and r0 (00000), and the same two in the data field as
 * in the base frame. CRC 0D30h = 000110100110000. 39 + 56 + 15 + 3 + 10 = 123.
 */
static void test_extended_frame_captured_by_a_logic_analyser(void)
{
    static const uint8_t data[7] = {0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U};
    const can_frame_t frame = make_frame(0x11223344U, CAN_FLAG_EXT, 7U, data);

    ASSERT_FRAME(123U, 3U, 0x0D30U, &frame);
}

/* ------------------------------------------------------------------------------------------
 * Frames worked out by hand
 *
 * THE CRC. The register of section 3.2.1 starts at zero and does nothing but shift and XOR,
 * so it is linear: the CRC of a frame is the XOR of the CRCs that each of its recessive bits
 * would give alone. A recessive bit alone meets an empty register and leaves 4599h in it.
 * Every dominant bit after it shifts the 15-bit register left by one place and XORs 4599h in
 * if the bit that fell out was a 1. S[k] is the register after one recessive bit and k
 * dominant bits, each entry worked out from the one before it:
 *
 *      k  S[k]      k  S[k]      k  S[k]      k  S[k]      k  S[k]
 *      0  4599h     8  4426h    16  7014h    24  1EF8h    32  67E8h
 *      1  4EABh     9  4DD5h    17  25B1h    25  3DF0h    33  0A49h
 *      2  58CFh    10  5E33h    18  4B62h    26  7BE0h    34  1492h
 *      3  7407h    11  79FFh    19  535Dh    27  3259h    35  2924h
 *      4  2D97h    12  3667h    20  6323h    28  64B2h    36  5248h
 *      5  5B2Eh    13  6CCEh    21  03DFh    29  0CFDh    37  6109h
 *      6  73C5h    14  1C05h    22  07BEh    30  19FAh
 *      7  2213h    15  380Ah    23  0F7Ch    31  33F4h
 *
 *   S[1]: 4599h shifted is 0B32h and a 1 fell out, 0B32h ^ 4599h = 4EABh
 *   S[5]: 2D97h shifted is 5B2Eh and a 0 fell out, so 5B2Eh
 *
 * A recessive bit with k bits between it and the CRC sequence therefore contributes S[k].
 * Without a data field the k of every bit is:
 *
 *   base format      SOF  ID-28 .. ID-18  RTR  IDE  r0  DLC3 .. DLC0
 *                    18   17    ..  7     6    5    4   3    ..  0
 *
 *   extended format  SOF  ID-28 .. ID-18  SRR  IDE  ID-17 .. ID-0  RTR  r1  r0  DLC3 .. DLC0
 *                    38   37    .. 27     26   25   24    ..  7    6    5   4   3    ..  0
 *
 * Every data byte adds 8 to these. Bit b of a data byte (bit 7 is sent first) has k = b, plus
 * 8 for every data byte behind it.
 *
 * THE LENGTH. The bits before the CRC sequence and the CRC sequence are written down and the
 * stuff bits put in by hand; the frame is that many bits plus 10 for the CRC delimiter, the
 * ACK field and the end of frame.
 * ---------------------------------------------------------------------------------------- */

#define SINGLE_BIT_CRCS 38U

static const uint16_t s_single_bit_crc[SINGLE_BIT_CRCS] = {
    0x4599U, 0x4EABU, 0x58CFU, 0x7407U, 0x2D97U, 0x5B2EU, 0x73C5U, 0x2213U, /* k = 0 */
    0x4426U, 0x4DD5U, 0x5E33U, 0x79FFU, 0x3667U, 0x6CCEU, 0x1C05U, 0x380AU, /* k = 8 */
    0x7014U, 0x25B1U, 0x4B62U, 0x535DU, 0x6323U, 0x03DFU, 0x07BEU, 0x0F7CU, /* k = 16 */
    0x1EF8U, 0x3DF0U, 0x7BE0U, 0x3259U, 0x64B2U, 0x0CFDU, 0x19FAU, 0x33F4U, /* k = 24 */
    0x67E8U, 0x0A49U, 0x1492U, 0x2924U, 0x5248U, 0x6109U,                   /* k = 32 */
};

/* k of the bits that are recessive in every frame of their kind. */
#define K_RTR 6U
#define K_SRR 26U
#define K_IDE 25U

/* A table filled in by hand can hold a slip of the pen. The same numbers are the remainders
 * of x^(k+15) divided by the generator polynomial, which the model computes by long division:
 * this ties the table, the division and the shift-register rule it was derived with together
 * before anything is built on them. */
static void test_hand_computed_crc_table_agrees_with_polynomial_division(void)
{
    for (size_t k = 0U; k < SINGLE_BIT_CRCS; k++) {
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(s_single_bit_crc[k], model_single_bit_crc(k), at("k", k));
    }
}

/*
 * Base format, identifier 0, no data: all 19 bits before the CRC sequence are dominant, so
 * the CRC is 0000h and the sequence to stuff is 34 dominant bits. A recessive stuff bit
 * follows every fifth of them, and because it ends the run the count starts again at one:
 *
 *   00000[1]00000[1]00000[1]00000[1]00000[1]00000[1]0000      34 + 6, and 10: 50 bits
 */
static void test_all_dominant_frame_has_a_stuff_bit_after_every_fifth_bit(void)
{
    const can_frame_t frame = make_frame(0x000U, 0U, 0U, NULL);

    ASSERT_FRAME(50U, 6U, 0x0000U, &frame);
}

/*
 * Base format, identifier 078h, no data.
 *
 *   SOF ID          RTR IDE r0 DLC
 *   0   00001111000 0   0   0  0000        recessive: ID-24 .. ID-21, k = 13, 12, 11, 10
 *   CRC = 6CCEh ^ 3667h ^ 79FFh ^ 5E33h = 7D65h = 111110101100101
 *
 *   00000[1]1111[0]0000[1]00000[1]0 11111[0]0101100101       34 + 5, and 10: 49 bits
 *
 * The first stuff bit and the four recessive identifier bits behind it are five equal bits
 * and call for the second stuff bit; that one and the four dominant bits behind it (ID-20 to
 * RTR) call for the third. A transmitter that started its count afresh after a stuff bit
 * would find its runs of five elsewhere, send four stuff bits and end up with 48 bits.
 */
static void test_stuff_bit_counts_towards_the_next_run_of_five(void)
{
    const can_frame_t frame = make_frame(0x078U, 0U, 0U, NULL);

    ASSERT_FRAME(49U, 5U, 0x7D65U, &frame);
}

/*
 * The last bit of the CRC sequence can complete a run of five. The stuff bit is then sent
 * between the CRC sequence and the CRC delimiter, in both polarities:
 *
 * Base format, identifier 009h, no data.
 *
 *   0   00000001001 0   0   0  0000        recessive: ID-21 and ID-18, k = 10 and 7
 *   CRC = 5E33h ^ 2213h = 7C20h = 111110000100000
 *
 *   00000[1]000 1 00 1 00000[1]00 11111[0]0000[1]1 00000[1]    34 + 5, and 10: 49 bits
 *
 * Base format, identifier 104h, no data.
 *
 *   0   00100000100 0   0   0  0000        recessive: ID-26 and ID-20, k = 15 and 9
 *   CRC = 380Ah ^ 4DD5h = 75DFh = 111010111011111
 *
 *   000 1 00000[1]1 00000[1]0000 1110101110 11111[0]           34 + 3, and 10: 47 bits
 *
 * The CRC delimiter that follows is recessive, like the stuff bit of the first frame and
 * unlike that of the second: the stuff bit is not there for the sake of an edge, it is there
 * because five equal bits were sent in a field that is stuffed.
 */
static void test_stuff_bit_follows_the_last_crc_bit_if_that_bit_completes_a_run(void)
{
    const can_frame_t ends_dominant = make_frame(0x009U, 0U, 0U, NULL);
    const can_frame_t ends_recessive = make_frame(0x104U, 0U, 0U, NULL);

    ASSERT_FRAME(49U, 5U, 0x7C20U, &ends_dominant);
    ASSERT_FRAME(47U, 3U, 0x75DFU, &ends_recessive);
}

/*
 * A data byte is sent most significant bit first. Base format, identifier 0, one data byte:
 *
 *   SOF ID          RTR IDE r0 DLC   data
 *   0   00000000000 0   0   0  0001  00000001      recessive: DLC0 (k = 0 + 8), data bit 0 (k = 0)
 *   CRC = 4426h ^ 4599h = 01BFh = 000000110111111
 *
 *   00000[1]00000[1]00000[1]000 1 00000[1]00 1 00000[1]0 11 0 11111[0]1     42 + 6, and 10: 58
 *
 *   0   00000000000 0   0   0  0001  10000000      recessive: DLC0 (k = 8), data bit 7 (k = 7)
 *   CRC = 4426h ^ 2213h = 6635h = 110011000110101
 *
 *   00000[1]00000[1]00000[1]000 1 1 00000[1]00 110011000110101              42 + 4, and 10: 56
 *
 * Sent the other way round, each byte would give the other one's result.
 */
static void test_data_byte_is_sent_most_significant_bit_first(void)
{
    const uint8_t last_bit = 0x01U;
    const uint8_t first_bit = 0x80U;
    const can_frame_t ends_recessive = make_frame(0x000U, 0U, 1U, &last_bit);
    const can_frame_t starts_recessive = make_frame(0x000U, 0U, 1U, &first_bit);

    ASSERT_FRAME(58U, 6U, 0x01BFU, &ends_recessive);
    ASSERT_FRAME(56U, 4U, 0x6635U, &starts_recessive);
}

/*
 * Runs do not stop at the end of a field. Base format, identifier 078h, data byte E1h:
 *
 *   SOF ID          RTR IDE r0 DLC   data
 *   0   00001111000 0   0   0  0001  11100001
 *
 *   recessive          k                  contribution
 *   ID-24 .. ID-21     21, 20, 19, 18     03DFh ^ 6323h ^ 535Dh ^ 4B62h = 78C3h
 *   DLC0               8                  4426h
 *   data bits 7, 6, 5  7, 6, 5            2213h ^ 73C5h ^ 5B2Eh         = 0AF8h
 *   data bit 0         0                  4599h
 *   CRC = 78C3h ^ 4426h ^ 0AF8h ^ 4599h = 7384h = 111001110000100
 *
 *   00000[1]1111[0]0000[1]00000[1]1111[0]0000[1]1 111[0]001110000100    42 + 7, and 10: 59 bits
 *
 * The fourth stuff bit, DLC0 and the first three data bits are five recessive bits; the sixth
 * stuff bit, the last data bit and the first three CRC bits are five again.
 */
static void test_run_of_five_crosses_field_boundaries(void)
{
    const uint8_t data = 0xE1U;
    const can_frame_t frame = make_frame(0x078U, 0U, 1U, &data);

    ASSERT_FRAME(59U, 7U, 0x7384U, &frame);
}

/*
 * A remote frame sends RTR recessive and its DLC, but no data field (section 3.2.2). Base
 * format, identifier 0:
 *
 *   SOF ID          RTR IDE r0 DLC
 *   0   00000000000 1   0   0  0000        recessive: RTR, k = 6
 *   CRC = 73C5h = 111001111000101
 *
 *   00000[1]00000[1]00 1 00000[1]0 111001111000101              34 + 3, and 10: 47 bits
 *
 *   0   00000000000 1   0   0  1000        recessive: RTR (k = 6) and DLC3 (k = 3)
 *   CRC = 73C5h ^ 7407h = 07C2h = 000011111000010
 *
 *   00000[1]00000[1]00 1 00 1 000 00[1]00 11111[0]0000[1]1 0    34 + 5, and 10: 49 bits
 *
 * The DLC of 8 changes the CRC and with it the stuff bits (DLC2 to DLC0 and the first two CRC
 * bits are five dominant bits), but no data byte is sent: with a data field of 8 bytes the
 * frame would be at least 44 + 64 = 108 bits long.
 */
static void test_remote_frame_sends_rtr_recessive_and_its_dlc_but_no_data(void)
{
    const can_frame_t asks_for_none = make_frame(0x000U, CAN_FLAG_RTR, 0U, NULL);
    const can_frame_t asks_for_eight = make_frame(0x000U, CAN_FLAG_RTR, 8U, NULL);

    ASSERT_FRAME(47U, 3U, 0x73C5U, &asks_for_none);
    ASSERT_FRAME(49U, 5U, 0x07C2U, &asks_for_eight);
}

/*
 * Extended format, identifier 0, no data: SRR and IDE are recessive, r1 and r0 dominant.
 *
 *   SOF ID-28..18   SRR IDE ID-17..0           RTR r1 r0 DLC
 *   0   00000000000 1   1   000000000000000000 0   0  0  0000    recessive: k = 26 and 25
 *   CRC = 7BE0h ^ 3DF0h = 4610h = 100011000010000
 *
 *   00000[1]00000[1]00 11 00000[1]00000[1]00000[1]00000[1]00000[1] 100011000010000
 *                                                                54 + 7, and 10: 71 bits
 *
 * The 25 dominant bits from ID-17 to DLC0 take five stuff bits, and the fifth one stands at
 * the very end of the control field, between DLC0 and the first bit of the CRC sequence.
 *
 * The remote frame of the same identifier has RTR recessive as well. In the extended format
 * RTR follows the identifier extension (k = 6):
 *
 *   0   00000000000 1   1   000000000000000000 1   0  0  0000
 *   CRC = 4610h ^ 73C5h = 35D5h = 011010111010101
 *
 *   00000[1]00000[1]00 11 00000[1]00000[1]00000[1]000 1 00000[1]0 011010111010101
 *                                                                54 + 6, and 10: 70 bits
 */
static void test_extended_frame_sends_srr_and_ide_recessive_and_rtr_after_the_extension(void)
{
    const can_frame_t data_frame = make_frame(0x00000000U, CAN_FLAG_EXT, 0U, NULL);
    const can_frame_t remote_frame = make_frame(0x00000000U, BOTH_FLAGS, 0U, NULL);

    ASSERT_FRAME(71U, 7U, 0x4610U, &data_frame);
    ASSERT_FRAME(70U, 6U, 0x35D5U, &remote_frame);
}

/* One identifier bit recessive at a time. The CRC shows where in the frame the bit was sent:
 * it differs from the CRC of identifier 0 by S[k], and no two k have the same S[k]. In the
 * base format bit n of the identifier has k = 7 + n. In the extended format the 18 low bits
 * (ID-17 .. ID-0) have k = 7 + n as well, and the 11 high bits (ID-28 .. ID-18) are two
 * places further from the CRC, k = 9 + n, because SRR and IDE stand in between. */
static void test_every_identifier_bit_is_sent_at_its_place(void)
{
    const uint16_t extended_zero = s_single_bit_crc[K_SRR] ^ s_single_bit_crc[K_IDE];

    for (unsigned int n = 0U; n <= 10U; n++) {
        const can_frame_t frame = make_frame((uint32_t)1U << n, 0U, 0U, NULL);

        TEST_ASSERT_EQUAL_HEX16_MESSAGE(s_single_bit_crc[7U + n], exact(&frame).crc15,
                                        at("base identifier bit", n));
    }

    for (unsigned int n = 0U; n <= 28U; n++) {
        const unsigned int k = (n >= 18U) ? (9U + n) : (7U + n);
        const can_frame_t frame = make_frame((uint32_t)1U << n, CAN_FLAG_EXT, 0U, NULL);

        TEST_ASSERT_EQUAL_HEX16_MESSAGE(extended_zero ^ s_single_bit_crc[k], exact(&frame).crc15,
                                        at("extended identifier bit", n));
    }
}

/* The four DLC bits are the last bits before the CRC sequence of a remote frame, DLC3 first:
 * bit b of the DLC has k = b. With identifier 0 the only other recessive bits are RTR and, in
 * the extended format, SRR and IDE. */
static void test_dlc_is_sent_as_four_bits_with_dlc3_first(void)
{
    const uint16_t base_zero = s_single_bit_crc[K_RTR];
    const uint16_t extended_zero =
        s_single_bit_crc[K_SRR] ^ s_single_bit_crc[K_IDE] ^ s_single_bit_crc[K_RTR];

    for (uint8_t dlc = 0U; dlc <= 8U; dlc++) {
        const can_frame_t base = make_frame(0x000U, CAN_FLAG_RTR, dlc, NULL);
        const can_frame_t extended = make_frame(0x00000000U, BOTH_FLAGS, dlc, NULL);
        uint16_t dlc_part = 0U;

        for (unsigned int b = 0U; b <= 3U; b++) {
            if ((dlc & (1U << b)) != 0U) {
                dlc_part ^= s_single_bit_crc[b];
            }
        }

        TEST_ASSERT_EQUAL_HEX16_MESSAGE(base_zero ^ dlc_part, exact(&base).crc15, at("DLC", dlc));
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(extended_zero ^ dlc_part, exact(&extended).crc15,
                                        at("DLC", dlc));
    }
}

/*
 * A frame computed by a colleague: base format, identifier 2AAh, eight data bytes AAh, said
 * to have CRC 43D6h and no stuff bit. Confirmed:
 *
 *   SOF ID          RTR IDE r0 DLC   data
 *   0   01010101010 0   0   0  1000  10101010 (eight times)
 *
 * Before the CRC sequence the longest runs are the four dominant bits from ID-18 to r0 and
 * the three of DLC2 to DLC0. The CRC sequence 43D6h = 100001111010110 follows a dominant data
 * bit and holds runs of four at most. No stuff bit, so the frame has its unstuffed length of
 * 44 + 64 = 108 bits. The CRC has 38 terms, with k up to 80, which is too many for the table
 * above: here it is the remainder that the model's polynomial division gives.
 */
static void test_frame_of_alternating_bits_needs_no_stuff_bit(void)
{
    static const uint8_t data[8] = {0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU};
    const can_frame_t frame = make_frame(0x2AAU, 0U, 8U, data);

    ASSERT_FRAME(108U, 0U, 0x43D6U, &frame);
    TEST_ASSERT_EQUAL_UINT16(can_bits_unstuffed(false, false, 8U), exact(&frame).frame_bits);
}

/* ------------------------------------------------------------------------------------------
 * Properties
 * ---------------------------------------------------------------------------------------- */

#define FRAMES_PER_CASE 400U

/* For every kind of frame and every DLC: the extremes (everything dominant, everything
 * recessive) and random frames. Counts how many of the frames needed a stuff bit at all, so
 * that a test can tell whether it looked at anything. */
static void test_exact_length_lies_between_the_unstuffed_length_and_the_worst_case(void)
{
    unsigned int frames = 0U;
    unsigned int frames_with_stuff_bits = 0U;
    unsigned int frames_with_ten_or_more = 0U;

    for (size_t kind = 0U; kind < sizeof(s_kinds); kind++) {
        const bool extended = (s_kinds[kind] & CAN_FLAG_EXT) != 0U;
        const bool remote = (s_kinds[kind] & CAN_FLAG_RTR) != 0U;

        for (uint8_t dlc = 0U; dlc <= 8U; dlc++) {
            const uint16_t unstuffed = can_bits_unstuffed(extended, remote, dlc);
            const uint16_t worst = can_bits_worst_case(extended, remote, dlc);

            for (unsigned int trial = 0U; trial < FRAMES_PER_CASE; trial++) {
                can_frame_t frame = random_frame(s_kinds[kind], dlc);

                if (trial < 2U) {
                    frame = uniform_frame(s_kinds[kind], dlc, trial == 1U);
                }

                const can_bits_info_t info = exact(&frame);
                const char *text = frame_text(&frame);

                TEST_ASSERT_GREATER_OR_EQUAL_UINT16_MESSAGE(unstuffed, info.frame_bits, text);
                TEST_ASSERT_LESS_OR_EQUAL_UINT16_MESSAGE(worst, info.frame_bits, text);
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(unstuffed + info.stuff_bits, info.frame_bits,
                                                 text);
                TEST_ASSERT_BITS_LOW_MESSAGE(0x8000U, info.crc15, text);

                frames++;
                frames_with_stuff_bits += (info.stuff_bits > 0U) ? 1U : 0U;
                frames_with_ten_or_more += (info.stuff_bits >= 10U) ? 1U : 0U;
            }
        }
    }

    /* The bounds are only tested by frames that have stuff bits, and the upper one by frames
     * that have many. If the generator stopped producing them, this test would pass blind. */
    TEST_ASSERT_EQUAL_UINT(4U * 9U * FRAMES_PER_CASE, frames);
    TEST_ASSERT_GREATER_THAN_UINT(frames / 2U, frames_with_stuff_bits);
    TEST_ASSERT_GREATER_THAN_UINT(frames / 50U, frames_with_ten_or_more);
}

/* Every base frame without a data field there is: 2048 identifiers as a data frame with DLC 0
 * and as a remote frame with each DLC, 20 480 frames. Small enough to try them all, and they
 * contain every arrangement of identifier, RTR and DLC around the CRC. */
static void test_every_base_frame_without_data_matches_the_model(void)
{
    const uint16_t unstuffed = can_bits_unstuffed(false, false, 0U);
    const uint16_t worst = can_bits_worst_case(false, false, 0U);
    uint8_t most_stuff_bits = 0U;

    for (uint32_t id = 0U; id <= CAN_ID_STD_MAX; id++) {
        for (unsigned int variant = 0U; variant <= 9U; variant++) {
            /* variant 0: data frame with DLC 0; variants 1 to 9: remote frame with DLC 0 to 8 */
            const uint8_t flags = (variant == 0U) ? 0U : CAN_FLAG_RTR;
            const uint8_t dlc = (variant == 0U) ? 0U : (uint8_t)(variant - 1U);
            const can_frame_t frame = make_frame(id, flags, dlc, NULL);
            const can_bits_info_t info = exact(&frame);

            assert_same_as_model(&frame);
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(unstuffed + info.stuff_bits, info.frame_bits,
                                             frame_text(&frame));

            if (info.stuff_bits > most_stuff_bits) {
                most_stuff_bits = info.stuff_bits;
            }
        }
    }

    /* The worst case allows 8 stuff bits in 34 bits. The all-dominant frame worked out above
     * has 6, and the search shows that no frame has more: the 15 CRC bits cannot be chosen,
     * which is what can_bits.h means by "no real frame reaches it for every DLC". */
    TEST_ASSERT_EQUAL_UINT16(8U, worst - unstuffed);
    TEST_ASSERT_EQUAL_UINT8(6U, most_stuff_bits);
}

static void test_random_frames_of_every_kind_and_dlc_match_the_model(void)
{
    for (size_t kind = 0U; kind < sizeof(s_kinds); kind++) {
        for (uint8_t dlc = 0U; dlc <= 8U; dlc++) {
            for (unsigned int trial = 0U; trial < FRAMES_PER_CASE; trial++) {
                const can_frame_t frame = random_frame(s_kinds[kind], dlc);

                assert_same_as_model(&frame);
            }
        }
    }
}

/* The CRC-15 detects every single-bit error, so no change of one data bit may leave the CRC
 * as it was. By linearity the change is known exactly: the CRC of the changed bit alone, with
 * as many bits behind it as lie between it and the CRC sequence (bit b of byte i of n: the b
 * lower bits of its byte and the n - 1 - i bytes after it). This is also what proves that the
 * data bytes are sent in order and each of them most significant bit first. */
static void test_changing_one_data_bit_changes_the_crc_by_that_bits_contribution(void)
{
    for (unsigned int trial = 0U; trial < 200U; trial++) {
        const uint8_t flags = s_kinds[trial % 2U]; /* base and extended data frames in turn */
        const uint8_t dlc = (uint8_t)(1U + (trial % 8U));
        const can_frame_t frame = random_frame(flags, dlc);
        const uint16_t crc = exact(&frame).crc15;

        for (size_t byte = 0U; byte < dlc; byte++) {
            for (unsigned int bit = 0U; bit <= 7U; bit++) {
                const size_t bits_behind = (8U * ((size_t)dlc - 1U - byte)) + bit;
                can_frame_t changed = frame;

                changed.data[byte] ^= (uint8_t)(1U << bit);

                const uint16_t changed_crc = exact(&changed).crc15;

                TEST_ASSERT_NOT_EQUAL_HEX16_MESSAGE(crc, changed_crc, frame_text(&changed));
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(model_single_bit_crc(bits_behind),
                                                crc ^ changed_crc, frame_text(&changed));
            }
        }
    }
}

/* can_frame.h: "data bytes 0..dlc-1; the rest is ignored". A remote frame sends no data at
 * all, so there every byte is "the rest". */
static void test_bytes_that_are_not_transmitted_do_not_change_the_result(void)
{
    for (size_t kind = 0U; kind < sizeof(s_kinds); kind++) {
        const bool remote = (s_kinds[kind] & CAN_FLAG_RTR) != 0U;

        for (uint8_t dlc = 0U; dlc <= 8U; dlc++) {
            const can_frame_t frame = random_frame(s_kinds[kind], dlc);
            const can_bits_info_t info = exact(&frame);
            const size_t transmitted = remote ? 0U : (size_t)dlc;
            can_frame_t other = frame;

            for (size_t i = transmitted; i < sizeof(other.data); i++) {
                other.data[i] = (uint8_t)~frame.data[i];
            }

            const can_bits_info_t other_info = exact(&other);

            TEST_ASSERT_EQUAL_HEX16_MESSAGE(info.crc15, other_info.crc15, frame_text(&frame));
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(info.stuff_bits, other_info.stuff_bits,
                                            frame_text(&frame));
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(info.frame_bits, other_info.frame_bits,
                                             frame_text(&frame));
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * Frames that are refused
 * ---------------------------------------------------------------------------------------- */

static void assert_refused_and_untouched(const can_frame_t *frame)
{
    can_bits_info_t info = {.frame_bits = 0xA5A5U, .stuff_bits = 0x5AU, .crc15 = 0xC3C3U};

    TEST_ASSERT_FALSE(can_bits_exact(frame, &info));
    TEST_ASSERT_EQUAL_HEX16(0xA5A5U, info.frame_bits);
    TEST_ASSERT_EQUAL_HEX8(0x5AU, info.stuff_bits);
    TEST_ASSERT_EQUAL_HEX16(0xC3C3U, info.crc15);
}

static void test_invalid_frame_is_refused_and_the_result_left_untouched(void)
{
    static const can_frame_t invalid[] = {
        {.id = 0x800U, .flags = 0U, .dlc = 0U},                /* 12 bits in base format */
        {.id = 0x20000000U, .flags = CAN_FLAG_EXT, .dlc = 0U}, /* 30 bits */
        {.id = 0x123U, .flags = 0U, .dlc = 9U},                /* DLC above 8 */
        {.id = 0x123U, .flags = CAN_FLAG_RTR, .dlc = 9U},      /* also without a data field */
        {.id = 0x123U, .flags = BOTH_FLAGS, .dlc = 15U},       /* the largest 4-bit DLC */
        {.id = 0x123U, .flags = 0x04U, .dlc = 0U},             /* a flag nobody defined */
        {.id = 0x123U, .flags = 0x80U | CAN_FLAG_EXT, .dlc = 8U},
    };
    static const can_frame_t still_valid[] = {
        {.id = 0x7FFU, .flags = 0U, .dlc = 8U},
        {.id = 0x1FFFFFFFU, .flags = CAN_FLAG_EXT, .dlc = 8U},
        {.id = 0x7FFU, .flags = CAN_FLAG_RTR, .dlc = 8U},
        {.id = 0x1FFFFFFFU, .flags = BOTH_FLAGS, .dlc = 8U},
    };

    for (size_t i = 0U; i < (sizeof(invalid) / sizeof(invalid[0])); i++) {
        assert_refused_and_untouched(&invalid[i]);
    }

    /* The refusals above are worth something only if the nearest valid frames pass. */
    for (size_t i = 0U; i < (sizeof(still_valid) / sizeof(still_valid[0])); i++) {
        assert_same_as_model(&still_valid[i]);
    }
}

/* The header promises nothing for a missing result; the implementation refuses the call, as
 * it does for a missing frame, instead of writing through a null pointer. */
static void test_missing_frame_or_result_is_refused(void)
{
    const can_frame_t frame = make_frame(0x123U, 0U, 0U, NULL);

    assert_refused_and_untouched(NULL);
    TEST_ASSERT_FALSE(can_bits_exact(&frame, NULL));
    TEST_ASSERT_FALSE(can_bits_exact(NULL, NULL));
}

/* ------------------------------------------------------------------------------------------
 * Lengths that do not depend on the content
 * ---------------------------------------------------------------------------------------- */

/*
 * Counted from the field lists of section 3.2.1, without the data field:
 *
 *   base      SOF 1 + ID 11 + RTR 1 + IDE 1 + r0 1 + DLC 4 + CRC 15                   = 34
 *   extended  SOF 1 + ID 11 + SRR 1 + IDE 1 + ID 18 + RTR 1 + r1 1 + r0 1 + DLC 4
 *             + CRC 15                                                                = 54
 *   both      CRC delimiter 1 + ACK slot 1 + ACK delimiter 1 + end of frame 7         = 10
 */
static void test_unstuffed_length_is_44_or_64_bits_and_8_per_data_byte(void)
{
    static const uint16_t base[9] = {44U, 52U, 60U, 68U, 76U, 84U, 92U, 100U, 108U};
    static const uint16_t extended[9] = {64U, 72U, 80U, 88U, 96U, 104U, 112U, 120U, 128U};

    for (uint8_t dlc = 0U; dlc <= 8U; dlc++) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(base[dlc], can_bits_unstuffed(false, false, dlc),
                                         at("DLC", dlc));
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(extended[dlc], can_bits_unstuffed(true, false, dlc),
                                         at("DLC", dlc));
    }
}

/*
 * The stuffed span has s = 34 + 8n bits (base) or 54 + 8n (extended), and at most
 * floor((s - 1) / 4) stuff bits:
 *
 *   base      floor((33 + 8n) / 4) = 8 + 2n      44 + 8n + 8 + 2n  = 52 + 10n
 *   extended  floor((53 + 8n) / 4) = 13 + 2n     64 + 8n + 13 + 2n = 77 + 10n
 *
 * With 8 data bytes that is 132 and 157 bits. With the 3 bits of intermission it is the 135
 * and 160 bit times per frame of the response-time analysis of CAN (Davis, Burns, Bril and
 * Lukkien, "Controller Area Network (CAN) schedulability analysis: refuted, revisited and
 * revised", Real-Time Systems 35, 2007).
 */
static void test_worst_case_length_is_52_or_77_bits_and_10_per_data_byte(void)
{
    static const uint16_t base[9] = {52U, 62U, 72U, 82U, 92U, 102U, 112U, 122U, 132U};
    static const uint16_t extended[9] = {77U, 87U, 97U, 107U, 117U, 127U, 137U, 147U, 157U};

    for (uint8_t dlc = 0U; dlc <= 8U; dlc++) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(base[dlc], can_bits_worst_case(false, false, dlc),
                                         at("DLC", dlc));
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(extended[dlc], can_bits_worst_case(true, false, dlc),
                                         at("DLC", dlc));
    }

    TEST_ASSERT_EQUAL_UINT16(135U, can_bits_worst_case(false, false, 8U) + CAN_BITS_INTERMISSION);
    TEST_ASSERT_EQUAL_UINT16(160U, can_bits_worst_case(true, false, 8U) + CAN_BITS_INTERMISSION);
}

/* Where floor((s - 1) / 4) comes from: the first stuff bit needs five equal bits, every
 * further one only four, because the stuff bit before it is the first bit of their run. The
 * pattern 00000 1111 0000 1111 ... therefore earns a stuff bit after bit 5, 9, 13 and so on.
 * Stuffed by the model, it has exactly as many stuff bits as the worst case allows for. */
static void test_worst_case_is_what_the_worst_bit_pattern_needs(void)
{
    for (unsigned int format = 0U; format <= 1U; format++) {
        const bool extended = (format == 1U);

        for (uint8_t dlc = 0U; dlc <= 8U; dlc++) {
            const uint16_t unstuffed = can_bits_unstuffed(extended, false, dlc);
            const size_t span = (size_t)unstuffed - MODEL_TAIL_BITS;
            bit_list_t pattern = {.count = 0U};
            bit_list_t wire;
            uint32_t level = 0U;
            size_t left_in_group = 5U;

            for (size_t i = 0U; i < span; i++) {
                list_push(&pattern, level);
                left_in_group--;

                if (left_in_group == 0U) {
                    level ^= 1U;
                    left_in_group = 4U;
                }
            }

            TEST_ASSERT_EQUAL_size_t_MESSAGE((size_t)can_bits_worst_case(extended, false, dlc) -
                                                 unstuffed,
                                             model_stuff(&pattern, &wire), at("DLC", dlc));
        }
    }
}

/* A remote frame has no data field, so its DLC does not enter the formulas. */
static void test_remote_frame_has_the_lengths_of_a_frame_without_data_for_every_dlc(void)
{
    for (uint8_t dlc = 0U; dlc <= 8U; dlc++) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(44U, can_bits_unstuffed(false, true, dlc), at("DLC", dlc));
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(64U, can_bits_unstuffed(true, true, dlc), at("DLC", dlc));
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(52U, can_bits_worst_case(false, true, dlc),
                                         at("DLC", dlc));
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(77U, can_bits_worst_case(true, true, dlc), at("DLC", dlc));
    }

    /* The exact length agrees: whatever DLC a remote frame carries, it is its stuff bits
     * longer than 44 or 64 bits. */
    for (size_t trial = 0U; trial < 200U; trial++) {
        const uint8_t flags = s_kinds[2U + (trial % 2U)]; /* the two kinds of remote frame */
        const can_frame_t frame = random_frame(flags, (uint8_t)(trial % 9U));
        const can_bits_info_t info = exact(&frame);

        TEST_ASSERT_EQUAL_UINT16_MESSAGE(is_extended(&frame) ? 64U : 44U,
                                         info.frame_bits - info.stuff_bits, frame_text(&frame));
    }
}

/* can_bits.h says so for the unstuffed length; the implementation documents that it treats
 * the worst case the same way. A frame with such a DLC is invalid and has no exact length. */
static void test_dlc_above_8_counts_as_8_in_the_formulas(void)
{
    static const uint8_t too_large[] = {9U, 15U, 16U, 255U};

    for (size_t i = 0U; i < sizeof(too_large); i++) {
        TEST_ASSERT_EQUAL_UINT16(108U, can_bits_unstuffed(false, false, too_large[i]));
        TEST_ASSERT_EQUAL_UINT16(128U, can_bits_unstuffed(true, false, too_large[i]));
        TEST_ASSERT_EQUAL_UINT16(132U, can_bits_worst_case(false, false, too_large[i]));
        TEST_ASSERT_EQUAL_UINT16(157U, can_bits_worst_case(true, false, too_large[i]));
        TEST_ASSERT_EQUAL_UINT16(44U, can_bits_unstuffed(false, true, too_large[i]));
        TEST_ASSERT_EQUAL_UINT16(77U, can_bits_worst_case(true, true, too_large[i]));
    }
}

/* ------------------------------------------------------------------------------------------
 * Duration
 * ---------------------------------------------------------------------------------------- */

/* The standard bit rates divide a second without remainder: 2 us per bit at 500 kbit/s. */
static void test_duration_at_500_kbit_per_second_is_exactly_2000_ns_per_bit(void)
{
    TEST_ASSERT_EQUAL_UINT64(2000U, can_bits_to_ns(1U, 500000U));
    TEST_ASSERT_EQUAL_UINT64(222000U, can_bits_to_ns(111U, 500000U));
    TEST_ASSERT_EQUAL_UINT64(0U, can_bits_to_ns(0U, 500000U));

    /* 135 bit times, the worst case of an 8-byte base frame with its intermission: 270 us. */
    TEST_ASSERT_EQUAL_UINT64(270000U, can_bits_to_ns(135U, 500000U));

    TEST_ASSERT_EQUAL_UINT64(8000U, can_bits_to_ns(1U, 125000U));
    TEST_ASSERT_EQUAL_UINT64(4000U, can_bits_to_ns(1U, 250000U));
    TEST_ASSERT_EQUAL_UINT64(1250U, can_bits_to_ns(1U, 800000U));
    TEST_ASSERT_EQUAL_UINT64(1000U, can_bits_to_ns(1U, 1000000U));
}

/*
 * 83 333 bit/s does not divide a second: 1 000 000 000 = 83 333 x 12 000 + 4 000, so a bit
 * lasts 12 000 ns and 4 000 / 83 333 = 0.048 ns.
 *
 *     1 bit     12 000.048 ns  ->   12 000    down
 *    10 bits   120 000.48  ns  ->  120 000    down, 0.48 is still below one half
 *    11 bits   132 000.528 ns  ->  132 001    up
 *    20 bits   240 000.96  ns  ->  240 001    up
 *    21 bits   252 001.008 ns  ->  252 001    down again
 *
 * A result that is cut off instead of rounded fails at 11 and 20 bits; one that is always
 * rounded up fails at 1, 10 and 21.
 */
static void test_duration_is_rounded_to_the_nearest_nanosecond(void)
{
    TEST_ASSERT_EQUAL_UINT64(12000U, can_bits_to_ns(1U, 83333U));
    TEST_ASSERT_EQUAL_UINT64(120000U, can_bits_to_ns(10U, 83333U));
    TEST_ASSERT_EQUAL_UINT64(132001U, can_bits_to_ns(11U, 83333U));
    TEST_ASSERT_EQUAL_UINT64(240001U, can_bits_to_ns(20U, 83333U));
    TEST_ASSERT_EQUAL_UINT64(252001U, can_bits_to_ns(21U, 83333U));

    /* The same with numbers that need no calculator: thirds of a second. */
    TEST_ASSERT_EQUAL_UINT64(333333333U, can_bits_to_ns(1U, 3U)); /* 333 333 333.33 */
    TEST_ASSERT_EQUAL_UINT64(666666667U, can_bits_to_ns(2U, 3U)); /* 666 666 666.67 */
    TEST_ASSERT_EQUAL_UINT64(1000000000U, can_bits_to_ns(3U, 3U));
}

/* "Rounded to nearest" leaves open what happens exactly in the middle; the implementation
 * rounds a half up. 1 000 000 000 / 1024 = 976 562.5 is such a case, and at 2 000 000 000
 * bit/s a bit lasts half a nanosecond. */
static void test_duration_of_exactly_half_a_nanosecond_is_rounded_up(void)
{
    TEST_ASSERT_EQUAL_UINT64(976563U, can_bits_to_ns(1U, 1024U));  /*   976 562.5 */
    TEST_ASSERT_EQUAL_UINT64(1953125U, can_bits_to_ns(2U, 1024U)); /* 1 953 125 exactly */
    TEST_ASSERT_EQUAL_UINT64(2929688U, can_bits_to_ns(3U, 1024U)); /* 2 929 687.5 */

    TEST_ASSERT_EQUAL_UINT64(1U, can_bits_to_ns(1U, 2000000000U)); /* 0.5 */
    TEST_ASSERT_EQUAL_UINT64(1U, can_bits_to_ns(2U, 2000000000U)); /* 1.0 */
    TEST_ASSERT_EQUAL_UINT64(2U, can_bits_to_ns(3U, 2000000000U)); /* 1.5 */
}

/* The definition of rounding, for arguments nobody picked: the result times the bit rate is
 * at most half a bit rate away from bits x 1 000 000 000, and if it is exactly that far away,
 * it is above. Both products stay below 2^63. */
static void test_duration_is_within_half_a_nanosecond_for_random_arguments(void)
{
    /* The standard rates, two that do not divide a second, two at which every odd number of
     * bits ends on half a nanosecond, and the ends of the range. */
    static const uint32_t bitrates[] = {10000U,  20000U,      50000U,   125000U,    250000U,
                                        500000U, 800000U,     1000000U, 33333U,     83333U,
                                        1024U,   2000000000U, 1U,       4294967295U};
    unsigned int ties = 0U;

    for (size_t trial = 0U; trial < 6000U; trial++) {
        uint32_t bits = random_u32();
        uint32_t bitrate = random_u32();
        const uint32_t shift = random_u32() % 32U;

        if ((trial % 2U) == 0U) {
            /* frame-sized bit counts at the bit rates of the list */
            bits %= 200U;
            bitrate = bitrates[bitrate % (sizeof(bitrates) / sizeof(bitrates[0]))];
        } else {
            /* the whole range of both arguments, small bit rates as likely as large ones */
            bitrate = (bitrate >> shift) | 1U;
        }

        const uint64_t ns = can_bits_to_ns(bits, bitrate);
        const uint64_t exact_product = (uint64_t)bits * 1000000000ULL;
        const uint64_t product = ns * (uint64_t)bitrate;
        const uint64_t error =
            (product > exact_product) ? (product - exact_product) : (exact_product - product);

        TEST_ASSERT_LESS_OR_EQUAL_UINT64_MESSAGE((uint64_t)bitrate, 2U * error, at("trial", trial));

        if ((2U * error) == (uint64_t)bitrate) {
            TEST_ASSERT_GREATER_THAN_UINT64_MESSAGE(exact_product, product, at("trial", trial));
            ties++;
        }
    }

    /* Without the two bit rates chosen for it, the case of the exact half would not occur. */
    TEST_ASSERT_GREATER_THAN_UINT(100U, ties);
}

/* 4 294 967 295 bits at 1 bit/s are 4.29e18 ns, which still fits 64 bits; an intermediate
 * result in 32 bits, or a rounding term added carelessly, would not survive these. */
static void test_duration_of_the_largest_arguments_does_not_overflow(void)
{
    TEST_ASSERT_EQUAL_UINT64(4294967295000000000ULL, can_bits_to_ns(UINT32_MAX, 1U));
    TEST_ASSERT_EQUAL_UINT64(2147483647500000000ULL, can_bits_to_ns(UINT32_MAX, 2U));
    TEST_ASSERT_EQUAL_UINT64(1000000000ULL, can_bits_to_ns(UINT32_MAX, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT64(0U, can_bits_to_ns(1U, UINT32_MAX)); /* 0.23 ns */
    TEST_ASSERT_EQUAL_UINT64(1U, can_bits_to_ns(3U, UINT32_MAX)); /* 0.70 ns */
}

/* can_bits.h forbids a bit rate of 0. The implementation documents that it answers 0 rather
 * than divide by zero, which would end this test program with a signal. */
static void test_duration_at_bit_rate_zero_is_zero(void)
{
    TEST_ASSERT_EQUAL_UINT64(0U, can_bits_to_ns(0U, 0U));
    TEST_ASSERT_EQUAL_UINT64(0U, can_bits_to_ns(1U, 0U));
    TEST_ASSERT_EQUAL_UINT64(0U, can_bits_to_ns(UINT32_MAX, 0U));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_base_frame_captured_by_a_logic_analyser);
    RUN_TEST(test_extended_frame_captured_by_a_logic_analyser);
    RUN_TEST(test_hand_computed_crc_table_agrees_with_polynomial_division);
    RUN_TEST(test_all_dominant_frame_has_a_stuff_bit_after_every_fifth_bit);
    RUN_TEST(test_stuff_bit_counts_towards_the_next_run_of_five);
    RUN_TEST(test_stuff_bit_follows_the_last_crc_bit_if_that_bit_completes_a_run);
    RUN_TEST(test_data_byte_is_sent_most_significant_bit_first);
    RUN_TEST(test_run_of_five_crosses_field_boundaries);
    RUN_TEST(test_remote_frame_sends_rtr_recessive_and_its_dlc_but_no_data);
    RUN_TEST(test_extended_frame_sends_srr_and_ide_recessive_and_rtr_after_the_extension);
    RUN_TEST(test_every_identifier_bit_is_sent_at_its_place);
    RUN_TEST(test_dlc_is_sent_as_four_bits_with_dlc3_first);
    RUN_TEST(test_frame_of_alternating_bits_needs_no_stuff_bit);
    RUN_TEST(test_exact_length_lies_between_the_unstuffed_length_and_the_worst_case);
    RUN_TEST(test_every_base_frame_without_data_matches_the_model);
    RUN_TEST(test_random_frames_of_every_kind_and_dlc_match_the_model);
    RUN_TEST(test_changing_one_data_bit_changes_the_crc_by_that_bits_contribution);
    RUN_TEST(test_bytes_that_are_not_transmitted_do_not_change_the_result);
    RUN_TEST(test_invalid_frame_is_refused_and_the_result_left_untouched);
    RUN_TEST(test_missing_frame_or_result_is_refused);
    RUN_TEST(test_unstuffed_length_is_44_or_64_bits_and_8_per_data_byte);
    RUN_TEST(test_worst_case_length_is_52_or_77_bits_and_10_per_data_byte);
    RUN_TEST(test_worst_case_is_what_the_worst_bit_pattern_needs);
    RUN_TEST(test_remote_frame_has_the_lengths_of_a_frame_without_data_for_every_dlc);
    RUN_TEST(test_dlc_above_8_counts_as_8_in_the_formulas);
    RUN_TEST(test_duration_at_500_kbit_per_second_is_exactly_2000_ns_per_bit);
    RUN_TEST(test_duration_is_rounded_to_the_nearest_nanosecond);
    RUN_TEST(test_duration_of_exactly_half_a_nanosecond_is_rounded_up);
    RUN_TEST(test_duration_is_within_half_a_nanosecond_for_random_arguments);
    RUN_TEST(test_duration_of_the_largest_arguments_does_not_overflow);
    RUN_TEST(test_duration_at_bit_rate_zero_is_zero);
    return UNITY_END();
}
