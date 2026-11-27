/* SPDX-License-Identifier: MIT */
/*
 * The battery-module logic, in the order of its header: the plausibility limits, the filter,
 * and the pack state; then a long random run against a reference model.
 *
 * How to read the tests:
 *
 *   - The filter is checked three ways: numbers worked out by hand in the comments, properties
 *     the header promises (symmetry, exact settling, no overshoot, the clamp), and a reference
 *     implementation in 64-bit arithmetic with a rounding written differently, over the whole
 *     int32_t range of inputs. The asan preset runs this file under UndefinedBehaviorSanitizer,
 *     so a signed overflow anywhere in the code under test fails the run, not only a wrong value.
 *   - Every test of the pack starts with setUp() filling it with nonsense, so each also shows
 *     that bms_init() needs nothing from the memory it is given. Samples go through process(),
 *     which checks after every sample the invariants of the summary against values computed
 *     here from the channels (check_summary()).
 *   - A cell's default reading is 3600 + 10 * n mV and a temperature's 200 + n dC, so that a
 *     value that lands on the wrong channel shows. The limits of the header are written as
 *     literals (5000 mV, -400 and 1250 dC, 300000 mA), so that a changed macro shows up here.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bms/bms.h"
#include "unity.h"

#define ALL_GROUPS (BMS_SAMPLE_CURRENT | BMS_SAMPLE_CELLS | BMS_SAMPLE_TEMPS)

/* The filter's input bound and the largest state, as literals (see the header comment). */
#define INPUT_MAX 4194303
#define STATE_MAX (INPUT_MAX * 256)

static bms_pack_t s_pack;
static uint32_t s_sequence; /* sequence number of the next sample built by sample() */

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

/* A number in lo..hi, both included. */
static int32_t random_between(int32_t lo, int32_t hi)
{
    const uint64_t span = (uint64_t)((int64_t)hi - (int64_t)lo) + 1U; /* 1 .. 2^32 */

    return (int32_t)((int64_t)lo + (int64_t)((uint64_t)random_u32() % span));
}

void setUp(void)
{
    s_random_state = 0x2545F491U;
    s_sequence = 0U;
    (void)memset(&s_pack, 0xA5, sizeof(s_pack));
}

void tearDown(void)
{
}

/* Message for an assertion inside a loop: which case failed. */
static const char *at(const char *what, long value)
{
    static char text[64];

    (void)snprintf(text, sizeof(text), "%s %ld", what, value);
    return text;
}

/* ------------------------------------------------------------------------------------------
 * Reference filter: the formula of the header in 64-bit arithmetic, with the rounding written
 * as quotient and remainder instead of the code's "add half the divisor, then truncate".
 * ---------------------------------------------------------------------------------------- */

typedef struct {
    int64_t state;
    bool seeded;
} ref_filter_t;

/* n / d rounded to the nearest integer, a half away from zero; d > 0. */
static int64_t ref_divide(int64_t n, int64_t d)
{
    int64_t q = n / d;
    const int64_t r = n % d;
    const int64_t twice_r = (r < 0) ? (-2 * r) : (2 * r);

    if (twice_r >= d) {
        q += (n < 0) ? -1 : 1;
    }

    return q;
}

static int32_t ref_update(ref_filter_t *filter, int32_t input, unsigned int shift)
{
    int64_t x = input;
    const unsigned int k = (shift > 7U) ? 7U : shift;

    if (x > INPUT_MAX) {
        x = INPUT_MAX;
    }

    if (x < -INPUT_MAX) {
        x = -INPUT_MAX;
    }

    if (filter->seeded) {
        filter->state += ref_divide((x * 256) - filter->state, (int64_t)1 << k);
    } else {
        filter->state = x * 256;
        filter->seeded = true;
    }

    return (int32_t)ref_divide(filter->state, 256);
}

/* One input through the filter under test and the reference; both outputs and both states
 * have to agree. */
static void update_both(UNITY_LINE_TYPE line, bms_filter_t *filter, ref_filter_t *ref,
                        int32_t input, uint8_t shift)
{
    const int32_t expected = ref_update(ref, input, shift);
    const int32_t output = bms_filter_update(filter, input, shift);

    UNITY_TEST_ASSERT_EQUAL_INT32(expected, output, line, "output");
    UNITY_TEST_ASSERT_EQUAL_INT64(ref->state, (int64_t)filter->state, line, "state");
}

#define UPDATE_BOTH(filter, ref, input, shift) update_both(__LINE__, filter, ref, input, shift)

/* ------------------------------------------------------------------------------------------
 * Pack helpers
 * ---------------------------------------------------------------------------------------- */

/* Shifts of 1, 2 and 3: a different one per group, so that a group filtered with another
 * group's shift shows. */
static const bms_config_t s_config = {.cell_shift = 1U, .temp_shift = 2U, .current_shift = 3U};

/* A sample of the given groups with every reading valid and at its default, and the next
 * sequence number. */
static bms_sample_t sample(uint8_t groups)
{
    bms_sample_t s;

    (void)memset(&s, 0, sizeof(s));
    s.stamp_ns = 1000000ULL * (uint64_t)s_sequence;
    s.sequence = s_sequence;
    s.groups = groups;
    s.current_ma = 12000;

    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        s.cell_mv[i] = (uint16_t)(3600U + (10U * i));
    }

    for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
        s.temp_dc[i] = (int16_t)(200 + (int32_t)i);
    }

    s_sequence++;
    return s;
}

/* The summary of the pack, worked out here from its channels: what bms.h promises about it. */
static void check_summary(UNITY_LINE_TYPE line)
{
    const bms_pack_t *p = &s_pack;
    uint32_t sum = 0U;
    bool any = false;
    uint16_t lo = 0U;
    uint16_t hi = 0U;
    uint8_t lo_index = 0U;
    uint8_t hi_index = 0U;

    UNITY_TEST_ASSERT_EQUAL_HEX8(0U, (uint8_t)(p->cell_valid & (uint8_t)~p->cell_seen), line,
                                 "a valid cell has reported");
    UNITY_TEST_ASSERT_EQUAL_HEX8(0U, (uint8_t)(p->temp_valid & (uint8_t)~p->temp_seen), line,
                                 "a valid temperature has reported");
    UNITY_TEST_ASSERT(!p->current_valid || p->current_seen, line, "a valid current has reported");

    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        if ((p->cell_valid & (1U << i)) != 0U) {
            UNITY_TEST_ASSERT(p->cell_mv[i] <= 5000U, line, "a valid cell is plausible");
            sum += p->cell_mv[i];

            if (!any || (p->cell_mv[i] < lo)) {
                lo = p->cell_mv[i];
                lo_index = (uint8_t)i;
            }

            if (!any || (p->cell_mv[i] > hi)) {
                hi = p->cell_mv[i];
                hi_index = (uint8_t)i;
            }

            any = true;
        } else {
            UNITY_TEST_ASSERT_EQUAL_UINT16(0U, p->cell_mv[i], line, "an invalid cell reads 0");
        }
    }

    for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
        if ((p->temp_valid & (1U << i)) != 0U) {
            UNITY_TEST_ASSERT((p->temp_dc[i] >= -400) && (p->temp_dc[i] <= 1250), line,
                              "a valid temperature is plausible");
        } else {
            UNITY_TEST_ASSERT_EQUAL_INT16(0, p->temp_dc[i], line, "an invalid temperature reads 0");
        }
    }

    if (p->current_valid) {
        UNITY_TEST_ASSERT((p->current_ma >= -300000) && (p->current_ma <= 300000), line,
                          "a valid current is plausible");
    } else {
        UNITY_TEST_ASSERT_EQUAL_INT32(0, p->current_ma, line, "an invalid current reads 0");
    }

    UNITY_TEST_ASSERT_EQUAL_UINT32(sum, p->pack_mv, line, "pack voltage = sum of the valid cells");
    UNITY_TEST_ASSERT(p->pack_valid == (p->cell_valid == 0xFFU), line,
                      "the pack voltage is valid when every cell is");
    UNITY_TEST_ASSERT_EQUAL_UINT16(lo, p->min_cell_mv, line, "lowest cell");
    UNITY_TEST_ASSERT_EQUAL_UINT16(hi, p->max_cell_mv, line, "highest cell");
    UNITY_TEST_ASSERT_EQUAL_UINT8(lo_index, p->min_cell, line, "index of the lowest cell");
    UNITY_TEST_ASSERT_EQUAL_UINT8(hi_index, p->max_cell, line, "index of the highest cell");

    const bool invalid = (p->current_seen && !p->current_valid) ||
                         ((p->cell_seen & (uint8_t)~p->cell_valid) != 0U) ||
                         ((p->temp_seen & (uint8_t)~p->temp_valid) != 0U);
    const bool all_seen = p->current_seen && (p->cell_seen == 0xFFU) && (p->temp_seen == 0x0FU);
    bms_state_t state = BMS_STATE_STARTUP;

    if (invalid) {
        state = BMS_STATE_FAULT;
    } else if (all_seen) {
        state = BMS_STATE_NORMAL;
    } else {
        /* some channel has not reported yet */
    }

    UNITY_TEST_ASSERT_EQUAL_INT((int)state, (int)p->state, line, "pack state");
}

static void process_with(UNITY_LINE_TYPE line, const bms_config_t *config, const bms_sample_t *s)
{
    bms_process(&s_pack, config, s);
    check_summary(line);
}

#define PROCESS(s)              process_with(__LINE__, &s_config, (s))
#define PROCESS_WITH(config, s) process_with(__LINE__, (config), (s))

/* A fresh pack that has seen one complete, valid sample: NORMAL, every value at its default. */
static void start_normal(void)
{
    const bms_sample_t s = sample(ALL_GROUPS);

    bms_init(&s_pack);
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_NORMAL, s_pack.state);
}

/* ------------------------------------------------------------------------------------------
 * Plausibility
 * ---------------------------------------------------------------------------------------- */

static void test_a_cell_reading_is_plausible_from_0_to_5000_mv(void)
{
    TEST_ASSERT_EQUAL_UINT(0U, BMS_CELL_SENSOR_MIN_MV);
    TEST_ASSERT_EQUAL_UINT(5000U, BMS_CELL_SENSOR_MAX_MV);
    TEST_ASSERT_TRUE(bms_cell_plausible(0U));
    TEST_ASSERT_TRUE(bms_cell_plausible(1U));
    TEST_ASSERT_TRUE(bms_cell_plausible(3700U));
    TEST_ASSERT_TRUE(bms_cell_plausible(5000U));
    TEST_ASSERT_FALSE(bms_cell_plausible(5001U));
    TEST_ASSERT_FALSE(bms_cell_plausible(65535U));
}

static void test_a_temperature_is_plausible_from_minus_40_to_125_degrees(void)
{
    TEST_ASSERT_EQUAL_INT(-400, BMS_TEMP_SENSOR_MIN_DC);
    TEST_ASSERT_EQUAL_INT(1250, BMS_TEMP_SENSOR_MAX_DC);
    TEST_ASSERT_TRUE(bms_temp_plausible(-400));
    TEST_ASSERT_TRUE(bms_temp_plausible(0));
    TEST_ASSERT_TRUE(bms_temp_plausible(1250));
    TEST_ASSERT_FALSE(bms_temp_plausible(-401));
    TEST_ASSERT_FALSE(bms_temp_plausible(1251));
    TEST_ASSERT_FALSE(bms_temp_plausible(INT16_MIN));
    TEST_ASSERT_FALSE(bms_temp_plausible(INT16_MAX));
}

static void test_a_current_is_plausible_from_minus_300_to_300_amperes(void)
{
    TEST_ASSERT_EQUAL_INT(300000, BMS_CURRENT_SENSOR_MAX_MA);
    TEST_ASSERT_TRUE(bms_current_plausible(-300000));
    TEST_ASSERT_TRUE(bms_current_plausible(0));
    TEST_ASSERT_TRUE(bms_current_plausible(300000));
    TEST_ASSERT_FALSE(bms_current_plausible(-300001));
    TEST_ASSERT_FALSE(bms_current_plausible(300001));
    TEST_ASSERT_FALSE(bms_current_plausible(INT32_MIN));
    TEST_ASSERT_FALSE(bms_current_plausible(INT32_MAX));
}

/* ------------------------------------------------------------------------------------------
 * The filter
 * ---------------------------------------------------------------------------------------- */

static void test_the_first_input_after_a_reset_is_the_output_at_once(void)
{
    static const int32_t inputs[] = {0, 1234, -1234, 5000, INPUT_MAX, -INPUT_MAX};

    for (uint8_t shift = 0U; shift <= 7U; shift++) {
        for (size_t i = 0U; i < (sizeof(inputs) / sizeof(inputs[0])); i++) {
            bms_filter_t filter;

            bms_filter_reset(&filter);
            TEST_ASSERT_EQUAL_INT32_MESSAGE(inputs[i], bms_filter_update(&filter, inputs[i], shift),
                                            at("shift", shift));
            TEST_ASSERT_EQUAL_INT32(inputs[i] * 256, filter.state);
            TEST_ASSERT_TRUE(filter.seeded);
        }
    }
}

static void test_shift_0_passes_every_input_through(void)
{
    bms_filter_t filter;

    bms_filter_reset(&filter);

    for (int i = 0; i < 1000; i++) {
        const int32_t input = random_between(-INPUT_MAX, INPUT_MAX);

        TEST_ASSERT_EQUAL_INT32(input, bms_filter_update(&filter, input, 0U));
        TEST_ASSERT_EQUAL_INT32(input * 256, filter.state);
    }
}

/* A step from 0 to 1000 with shift 1: half the distance per sample. In sub-units, X = 256000
 * and each update adds round((X - s) / 2), halves away from zero:
 *     s: 0 -> 128000 -> 192000 -> 224000 -> 240000 -> 248000 -> 252000 -> 254000 -> 255000
 *          -> 255500 -> 255750 -> 255875
 *     y = round(s / 256): 500, 750, 875, 937.5 -> 938, 968.75 -> 969, 984.375 -> 984,
 *          992.19 -> 992, 996.09 -> 996, 998.05 -> 998, 999.02 -> 999, 999.51 -> 1000 */
static void test_a_step_follows_the_numbers_worked_out_by_hand(void)
{
    static const int32_t states[] = {128000, 192000, 224000, 240000, 248000, 252000,
                                     254000, 255000, 255500, 255750, 255875};
    static const int32_t outputs[] = {500, 750, 875, 938, 969, 984, 992, 996, 998, 999, 1000};
    bms_filter_t filter;

    bms_filter_reset(&filter);
    TEST_ASSERT_EQUAL_INT32(0, bms_filter_update(&filter, 0, 1U));

    for (size_t i = 0U; i < (sizeof(states) / sizeof(states[0])); i++) {
        TEST_ASSERT_EQUAL_INT32_MESSAGE(outputs[i], bms_filter_update(&filter, 1000, 1U),
                                        at("step", (long)i + 1L));
        TEST_ASSERT_EQUAL_INT32_MESSAGE(states[i], filter.state, at("step", (long)i + 1L));
    }

    /* The same step with shift 2, a quarter of the distance: (256000 + 2) / 4 = 64000 -> 250;
     * then 64000 + (192000 + 2) / 4 = 112000 -> 437.5 -> 438; 112000 + 36000 = 148000 -> 578.1. */
    bms_filter_reset(&filter);
    (void)bms_filter_update(&filter, 0, 2U);
    TEST_ASSERT_EQUAL_INT32(250, bms_filter_update(&filter, 1000, 2U));
    TEST_ASSERT_EQUAL_INT32(438, bms_filter_update(&filter, 1000, 2U));
    TEST_ASSERT_EQUAL_INT32(578, bms_filter_update(&filter, 1000, 2U));
    TEST_ASSERT_EQUAL_INT32(148000, filter.state);
}

/* The output is round(s / 256), a half away from zero. */
static void test_the_output_rounds_halves_away_from_zero(void)
{
    bms_filter_t filter;

    /* 0 -> 1 with shift 1: s = 128, 0.5 units: 1. And the mirror image: -0.5 -> -1. */
    bms_filter_reset(&filter);
    (void)bms_filter_update(&filter, 0, 1U);
    TEST_ASSERT_EQUAL_INT32(1, bms_filter_update(&filter, 1, 1U));
    TEST_ASSERT_EQUAL_INT32(128, filter.state);

    bms_filter_reset(&filter);
    (void)bms_filter_update(&filter, 0, 1U);
    TEST_ASSERT_EQUAL_INT32(-1, bms_filter_update(&filter, -1, 1U));
    TEST_ASSERT_EQUAL_INT32(-128, filter.state);

    /* 1 -> 2: s = 384, 1.5 units: 2; and -1.5 -> -2. */
    bms_filter_reset(&filter);
    (void)bms_filter_update(&filter, 1, 1U);
    TEST_ASSERT_EQUAL_INT32(2, bms_filter_update(&filter, 2, 1U));
    TEST_ASSERT_EQUAL_INT32(384, filter.state);

    bms_filter_reset(&filter);
    (void)bms_filter_update(&filter, -1, 1U);
    TEST_ASSERT_EQUAL_INT32(-2, bms_filter_update(&filter, -2, 1U));

    /* Just below a half: from s = 128, shift 7 towards 0 adds round(-128 / 128) = -1, so
     * s = 127, 0.496 units: 0. */
    bms_filter_reset(&filter);
    (void)bms_filter_update(&filter, 0, 1U);
    (void)bms_filter_update(&filter, 1, 1U);
    TEST_ASSERT_EQUAL_INT32(0, bms_filter_update(&filter, 0, 7U));
    TEST_ASSERT_EQUAL_INT32(127, filter.state);
}

/* The increment is round((X - s) / 2^shift), a half away from zero, too. */
static void test_the_update_rounds_halves_away_from_zero(void)
{
    bms_filter_t filter;

    /* From s = 64 (0 -> 1 with shift 2 adds (256 + 2) / 4 = 64), shift 7 towards 0 adds
     * round(-64 / 128) = round(-0.5) = -1: s = 63. */
    bms_filter_reset(&filter);
    (void)bms_filter_update(&filter, 0, 2U);
    (void)bms_filter_update(&filter, 1, 2U);
    TEST_ASSERT_EQUAL_INT32(64, filter.state);
    (void)bms_filter_update(&filter, 0, 7U);
    TEST_ASSERT_EQUAL_INT32(63, filter.state);

    /* From s = 64 towards 1 (X = 256): round(192 / 128) = round(1.5) = 2: s = 66. */
    bms_filter_reset(&filter);
    (void)bms_filter_update(&filter, 0, 2U);
    (void)bms_filter_update(&filter, 1, 2U);
    (void)bms_filter_update(&filter, 1, 7U);
    TEST_ASSERT_EQUAL_INT32(66, filter.state);

    /* The mirror images: -64 -> -63 and -66. */
    bms_filter_reset(&filter);
    (void)bms_filter_update(&filter, 0, 2U);
    (void)bms_filter_update(&filter, -1, 2U);
    TEST_ASSERT_EQUAL_INT32(-64, filter.state);
    (void)bms_filter_update(&filter, 0, 7U);
    TEST_ASSERT_EQUAL_INT32(-63, filter.state);

    bms_filter_reset(&filter);
    (void)bms_filter_update(&filter, 0, 2U);
    (void)bms_filter_update(&filter, -1, 2U);
    (void)bms_filter_update(&filter, -1, 7U);
    TEST_ASSERT_EQUAL_INT32(-66, filter.state);
}

static void test_negating_every_input_negates_every_state_and_output(void)
{
    bms_filter_t plus;
    bms_filter_t minus;

    bms_filter_reset(&plus);
    bms_filter_reset(&minus);

    for (int i = 0; i < 20000; i++) {
        const int32_t input = random_between(-INPUT_MAX, INPUT_MAX);
        const uint8_t shift = (uint8_t)(random_u32() % 9U);

        TEST_ASSERT_EQUAL_INT32_MESSAGE(-bms_filter_update(&plus, input, shift),
                                        bms_filter_update(&minus, -input, shift), at("step", i));
        TEST_ASSERT_EQUAL_INT32_MESSAGE(-plus.state, minus.state, at("step", i));

        if ((random_u32() % 64U) == 0U) {
            bms_filter_reset(&plus);
            bms_filter_reset(&minus);
        }
    }
}

/* Inputs from all of int32_t (clamped), from the sensor ranges and from the extremes, shifts
 * from 0 to 15 (above 7 they count as 7), and a reset now and then. */
static void test_the_filter_agrees_with_the_reference_after_every_input(void)
{
    bms_filter_t filter;
    ref_filter_t ref = {0, false};

    bms_filter_reset(&filter);

    for (int i = 0; i < 200000; i++) {
        const uint32_t kind = random_u32() % 4U;
        int32_t input;

        if (kind == 0U) {
            input = (int32_t)random_u32();
        } else if (kind == 1U) {
            input = random_between(-300000, 300000);
        } else if (kind == 2U) {
            input = random_between(-INPUT_MAX, INPUT_MAX);
        } else {
            static const int32_t extremes[] = {
                INT32_MIN,     INT32_MAX, -INPUT_MAX, INPUT_MAX, -INPUT_MAX - 1,
                INPUT_MAX + 1, 0,         1,          -1};

            input = extremes[random_u32() % (sizeof(extremes) / sizeof(extremes[0]))];
        }

        UPDATE_BOTH(&filter, &ref, input, (uint8_t)(random_u32() % 16U));

        if ((random_u32() % 128U) == 0U) {
            bms_filter_reset(&filter);
            ref.seeded = false;
        }
    }
}

/* From one end of the input range to the other and back, with every shift: the output moves
 * towards the input and never past it, reaches it exactly, and stays there. The bound on the
 * number of samples is generous: about 22 time constants (the distance is 2^31 sub-units and
 * the state stops within 2^(shift - 1)), plus the last 128 sub-units, which shift 7 crosses one
 * per sample. */
static void test_a_constant_input_is_reached_exactly_without_overshoot(void)
{
    for (uint8_t shift = 0U; shift <= 7U; shift++) {
        for (int direction = 0; direction < 2; direction++) {
            const int32_t from = (direction == 0) ? -INPUT_MAX : INPUT_MAX;
            const int32_t to = -from;
            const int limit = (32 << shift) + 256;
            bms_filter_t filter;
            int32_t previous = from;
            int reached = -1;

            bms_filter_reset(&filter);
            (void)bms_filter_update(&filter, from, shift);

            for (int n = 1; (n <= limit) && (reached < 0); n++) {
                const int32_t output = bms_filter_update(&filter, to, shift);

                if (to > from) {
                    TEST_ASSERT_TRUE_MESSAGE((output >= previous) && (output <= to), at("n", n));
                    TEST_ASSERT_TRUE(filter.state <= to * 256);
                } else {
                    TEST_ASSERT_TRUE_MESSAGE((output <= previous) && (output >= to), at("n", n));
                    TEST_ASSERT_TRUE(filter.state >= to * 256);
                }

                previous = output;

                if (output == to) {
                    reached = n;
                }
            }

            TEST_ASSERT_TRUE_MESSAGE(reached > 0, at("never reached with shift", shift));

            /* And it stays, within 2^(shift - 1) - 1 sub-units of the input. */
            for (int n = 0; n < 1000; n++) {
                TEST_ASSERT_EQUAL_INT32(to, bms_filter_update(&filter, to, shift));
            }

            const int32_t residual = (filter.state > to * 256) ? (filter.state - (to * 256))
                                                               : ((to * 256) - filter.state);

            TEST_ASSERT_TRUE_MESSAGE(residual <= ((shift == 0U) ? 0 : ((1 << (shift - 1U)) - 1)),
                                     at("residual with shift", shift));
        }
    }
}

static void test_inputs_beyond_the_range_count_as_its_ends(void)
{
    bms_filter_t clamped;
    bms_filter_t at_limit;

    bms_filter_reset(&clamped);
    bms_filter_reset(&at_limit);

    for (int i = 0; i < 2000; i++) {
        const bool high = (random_u32() % 2U) == 0U;
        const int32_t beyond = high ? random_between(INPUT_MAX + 1, INT32_MAX)
                                    : random_between(INT32_MIN, -INPUT_MAX - 1);
        const uint8_t shift = (uint8_t)(random_u32() % 8U);

        TEST_ASSERT_EQUAL_INT32(bms_filter_update(&at_limit, high ? INPUT_MAX : -INPUT_MAX, shift),
                                bms_filter_update(&clamped, beyond, shift));
        TEST_ASSERT_EQUAL_INT32(at_limit.state, clamped.state);
    }
}

/* The largest swings there are: every update between the two ends of the range and the two
 * ends of int32_t, with every shift. Nothing overflows (UndefinedBehaviorSanitizer would stop
 * the run), and the state stays within +-(2^22 - 1) * 256. */
static void test_the_extremes_never_overflow(void)
{
    static const int32_t extremes[] = {INT32_MAX, INT32_MIN, INPUT_MAX, -INPUT_MAX};

    for (uint8_t shift = 0U; shift <= 8U; shift++) {
        for (size_t first = 0U; first < 4U; first++) {
            bms_filter_t filter;
            ref_filter_t ref = {0, false};

            bms_filter_reset(&filter);

            for (int n = 0; n < 400; n++) {
                UPDATE_BOTH(&filter, &ref, extremes[(first + (size_t)n) % 4U], shift);
                TEST_ASSERT_TRUE((filter.state <= STATE_MAX) && (filter.state >= -STATE_MAX));
            }

            /* The longest run in one direction. */
            for (int n = 0; n < 400; n++) {
                UPDATE_BOTH(&filter, &ref, extremes[first], shift);
            }
        }
    }
}

static void test_a_shift_above_7_counts_as_7(void)
{
    static const uint8_t shifts[] = {8U, 9U, 100U, 255U};

    for (size_t s = 0U; s < (sizeof(shifts) / sizeof(shifts[0])); s++) {
        bms_filter_t seven;
        bms_filter_t above;

        bms_filter_reset(&seven);
        bms_filter_reset(&above);

        for (int i = 0; i < 300; i++) {
            const int32_t input = random_between(-5000, 5000);

            TEST_ASSERT_EQUAL_INT32(bms_filter_update(&seven, input, 7U),
                                    bms_filter_update(&above, input, shifts[s]));
            TEST_ASSERT_EQUAL_INT32(seven.state, above.state);
        }
    }
}

static void test_a_reset_forgets_the_past(void)
{
    bms_filter_t filter;

    bms_filter_reset(&filter);
    (void)bms_filter_update(&filter, 4000, 3U);
    (void)bms_filter_update(&filter, 3000, 3U);
    bms_filter_reset(&filter);
    TEST_ASSERT_FALSE(filter.seeded);
    TEST_ASSERT_EQUAL_INT32(0, filter.state);
    TEST_ASSERT_EQUAL_INT32(-17, bms_filter_update(&filter, -17, 3U));
}

/* A filter whose memory was never reset can hold any state. One beyond what an update can
 * produce is not worked from (it could overflow the arithmetic): the filter seeds again. The
 * largest state an update can produce, (2^22 - 1) * 256, is still worked from. */
static void test_a_state_no_update_can_produce_counts_as_not_seeded(void)
{
    static const int32_t impossible[] = {INT32_MAX, INT32_MIN, STATE_MAX + 1, -STATE_MAX - 1};
    bms_filter_t filter;

    for (size_t i = 0U; i < (sizeof(impossible) / sizeof(impossible[0])); i++) {
        filter.seeded = true;
        filter.state = impossible[i];
        TEST_ASSERT_EQUAL_INT32(5, bms_filter_update(&filter, 5, 1U));
        TEST_ASSERT_EQUAL_INT32(1280, filter.state);
    }

    /* From the largest state towards 0 with shift 1: half of it, 2^29 - 128 = 536870784. */
    filter.seeded = true;
    filter.state = STATE_MAX;
    TEST_ASSERT_EQUAL_INT32(2097152, bms_filter_update(&filter, 0, 1U));
    TEST_ASSERT_EQUAL_INT32(536870784, filter.state);

    filter.seeded = true;
    filter.state = -STATE_MAX;
    TEST_ASSERT_EQUAL_INT32(-2097152, bms_filter_update(&filter, 0, 1U));
    TEST_ASSERT_EQUAL_INT32(-536870784, filter.state);
}

/* ------------------------------------------------------------------------------------------
 * The pack
 * ---------------------------------------------------------------------------------------- */

static void test_init_leaves_an_empty_pack_in_startup(void)
{
    bms_init(&s_pack);

    TEST_ASSERT_EQUAL_INT(BMS_STATE_STARTUP, s_pack.state);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.updates);
    TEST_ASSERT_EQUAL_UINT64(0U, s_pack.stamp_ns);
    TEST_ASSERT_FALSE(s_pack.current_valid);
    TEST_ASSERT_FALSE(s_pack.current_seen);
    TEST_ASSERT_FALSE(s_pack.pack_valid);
    TEST_ASSERT_EQUAL_HEX8(0U, s_pack.cell_valid);
    TEST_ASSERT_EQUAL_HEX8(0U, s_pack.cell_seen);
    TEST_ASSERT_EQUAL_HEX8(0U, s_pack.temp_valid);
    TEST_ASSERT_EQUAL_HEX8(0U, s_pack.temp_seen);
    TEST_ASSERT_EQUAL_INT32(0, s_pack.current_ma);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.pack_mv);
    TEST_ASSERT_EQUAL_UINT16(0U, s_pack.min_cell_mv);
    TEST_ASSERT_EQUAL_UINT16(0U, s_pack.max_cell_mv);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.flagged_readings);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.implausible_readings);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.lost_samples);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.sequence_errors);
    TEST_ASSERT_FALSE(s_pack.current_filter.seeded);

    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        TEST_ASSERT_EQUAL_UINT16(0U, s_pack.cell_mv[i]);
        TEST_ASSERT_FALSE(s_pack.cell_filter[i].seeded);
    }

    for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
        TEST_ASSERT_EQUAL_INT16(0, s_pack.temp_dc[i]);
        TEST_ASSERT_FALSE(s_pack.temp_filter[i].seeded);
    }

    /* The first sample sets the sequence reference, whatever its number: nothing is lost. */
    s_sequence = 1000U;
    const bms_sample_t s = sample(BMS_SAMPLE_CURRENT);

    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.lost_samples);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.sequence_errors);
}

/* The sensor task samples the current every period, the cells every second one and the
 * temperatures every tenth: the pack is NORMAL once the temperatures have come in. The first
 * reading of each channel is its value at once. */
static void test_the_state_is_startup_until_every_channel_has_reported(void)
{
    bms_sample_t s;

    bms_init(&s_pack);

    s = sample(BMS_SAMPLE_CURRENT);
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_STARTUP, s_pack.state);
    TEST_ASSERT_TRUE(s_pack.current_valid);
    TEST_ASSERT_EQUAL_INT32(12000, s_pack.current_ma);

    s = sample(BMS_SAMPLE_CURRENT | BMS_SAMPLE_CELLS);
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_STARTUP, s_pack.state);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, s_pack.cell_valid);
    TEST_ASSERT_EQUAL_UINT16(3600U, s_pack.cell_mv[0]);
    TEST_ASSERT_EQUAL_UINT16(3670U, s_pack.cell_mv[7]);
    TEST_ASSERT_TRUE(s_pack.pack_valid);
    /* 8 x 3600 + 10 x (0 + 1 + ... + 7) = 28800 + 280 */
    TEST_ASSERT_EQUAL_UINT32(29080U, s_pack.pack_mv);

    s = sample(BMS_SAMPLE_TEMPS);
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_NORMAL, s_pack.state);
    TEST_ASSERT_EQUAL_HEX8(0x0FU, s_pack.temp_valid);
    TEST_ASSERT_EQUAL_INT16(200, s_pack.temp_dc[0]);
    TEST_ASSERT_EQUAL_INT16(203, s_pack.temp_dc[3]);
    TEST_ASSERT_EQUAL_UINT32(3U, s_pack.updates);
}

static void test_a_missing_temperature_keeps_the_pack_in_startup(void)
{
    bms_sample_t s;

    bms_init(&s_pack);
    s = sample(BMS_SAMPLE_CURRENT | BMS_SAMPLE_CELLS);
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_STARTUP, s_pack.state);

    /* A cell that has not reported keeps it there as well; so does a missing current. */
    bms_init(&s_pack);
    s = sample(BMS_SAMPLE_CURRENT | BMS_SAMPLE_TEMPS);
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_STARTUP, s_pack.state);

    bms_init(&s_pack);
    s = sample(BMS_SAMPLE_CELLS | BMS_SAMPLE_TEMPS);
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_STARTUP, s_pack.state);
    s = sample(BMS_SAMPLE_CURRENT);
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_NORMAL, s_pack.state);
}

/* FAULT wins over STARTUP: an invalid reading is a fault at once, even before every channel
 * has reported. */
static void test_an_invalid_reading_during_startup_is_a_fault_at_once(void)
{
    bms_sample_t s;

    bms_init(&s_pack);
    s = sample(BMS_SAMPLE_CURRENT);
    s.current_invalid = true;
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_FAULT, s_pack.state);
    TEST_ASSERT_TRUE(s_pack.current_seen);
    TEST_ASSERT_FALSE(s_pack.current_valid);

    bms_init(&s_pack);
    s = sample(BMS_SAMPLE_TEMPS);
    s.temp_dc[2] = 1300; /* implausible */
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_FAULT, s_pack.state);
    TEST_ASSERT_EQUAL_HEX8(0x0BU, s_pack.temp_valid);
}

static void test_a_flagged_reading_makes_its_channel_invalid_whatever_its_value(void)
{
    bms_sample_t s;

    start_normal();
    s = sample(BMS_SAMPLE_CELLS);
    s.cell_invalid = 0x08U; /* cell 3, with a plausible value */
    PROCESS(&s);

    TEST_ASSERT_EQUAL_INT(BMS_STATE_FAULT, s_pack.state);
    TEST_ASSERT_EQUAL_HEX8(0xF7U, s_pack.cell_valid);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, s_pack.cell_seen);
    TEST_ASSERT_EQUAL_UINT16(0U, s_pack.cell_mv[3]);
    TEST_ASSERT_FALSE(s_pack.cell_filter[3].seeded);
    TEST_ASSERT_EQUAL_UINT16(3620U, s_pack.cell_mv[2]);
    TEST_ASSERT_EQUAL_UINT16(3640U, s_pack.cell_mv[4]);
    TEST_ASSERT_FALSE(s_pack.pack_valid);
    TEST_ASSERT_EQUAL_UINT32(29080U - 3630U, s_pack.pack_mv);
    TEST_ASSERT_EQUAL_UINT32(1U, s_pack.flagged_readings);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.implausible_readings);

    /* A flagged reading outside the range is counted as flagged only. */
    s = sample(BMS_SAMPLE_CURRENT | BMS_SAMPLE_TEMPS);
    s.current_invalid = true;
    s.current_ma = 400000;
    s.temp_invalid = 0x01U;
    s.temp_dc[0] = -1000;
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(3U, s_pack.flagged_readings);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.implausible_readings);
    TEST_ASSERT_FALSE(s_pack.current_valid);
    TEST_ASSERT_EQUAL_INT32(0, s_pack.current_ma);
    TEST_ASSERT_EQUAL_HEX8(0x0EU, s_pack.temp_valid);
}

static void test_a_reading_outside_the_sensor_range_is_implausible(void)
{
    bms_sample_t s;

    start_normal();
    s = sample(ALL_GROUPS);
    s.cell_mv[0] = 5000U; /* the limit itself is plausible */
    s.cell_mv[5] = 5001U;
    s.temp_dc[0] = -400;
    s.temp_dc[1] = -401;
    s.temp_dc[2] = 1250;
    s.temp_dc[3] = 1251;
    s.current_ma = -300001;
    PROCESS(&s);

    TEST_ASSERT_EQUAL_UINT32(4U, s_pack.implausible_readings);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.flagged_readings);
    TEST_ASSERT_EQUAL_HEX8(0xDFU, s_pack.cell_valid);
    TEST_ASSERT_EQUAL_HEX8(0x05U, s_pack.temp_valid);
    TEST_ASSERT_FALSE(s_pack.current_valid);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_FAULT, s_pack.state);

    /* 3600 -> 5000 with shift 1: 4300. -> -400 and 1250 with shift 2 from 200 and 202:
     * 200 + round(-600 / 4) = 50, 202 + round(1048 / 4) = 464. */
    TEST_ASSERT_EQUAL_UINT16(4300U, s_pack.cell_mv[0]);
    TEST_ASSERT_EQUAL_INT16(50, s_pack.temp_dc[0]);
    TEST_ASSERT_EQUAL_INT16(464, s_pack.temp_dc[2]);

    s = sample(BMS_SAMPLE_CURRENT);
    s.current_ma = 300001;
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(5U, s_pack.implausible_readings);
    s = sample(BMS_SAMPLE_CURRENT);
    s.current_ma = -300000;
    PROCESS(&s);
    TEST_ASSERT_TRUE(s_pack.current_valid);
    TEST_ASSERT_EQUAL_INT32(-300000, s_pack.current_ma);
}

/* After an invalid reading the filter starts again from the next plausible one: its value is
 * that reading, not a blend with what came before the gap. */
static void test_a_channel_restarts_from_its_first_plausible_reading_after_an_invalid_one(void)
{
    const bms_config_t slow = {.cell_shift = 3U, .temp_shift = 3U, .current_shift = 3U};
    bms_sample_t s;

    bms_init(&s_pack);

    for (int i = 0; i < 5; i++) {
        s = sample(ALL_GROUPS);
        s.cell_mv[6] = 3700U;
        PROCESS_WITH(&slow, &s);
    }

    TEST_ASSERT_EQUAL_UINT16(3700U, s_pack.cell_mv[6]);

    s = sample(BMS_SAMPLE_CELLS);
    s.cell_invalid = 0x40U;
    PROCESS_WITH(&slow, &s);
    TEST_ASSERT_EQUAL_HEX8(0xBFU, s_pack.cell_valid);

    s = sample(BMS_SAMPLE_CELLS);
    s.cell_mv[6] = 3000U;
    PROCESS_WITH(&slow, &s);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, s_pack.cell_valid);
    TEST_ASSERT_EQUAL_UINT16(3000U, s_pack.cell_mv[6]);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_NORMAL, s_pack.state);

    /* And it filters from there: s = 768000, X = 793600, (25600 + 4) / 8 = 3200 -> 771200,
     * 3012.5 -> 3013. */
    s = sample(BMS_SAMPLE_CELLS);
    s.cell_mv[6] = 3100U;
    PROCESS_WITH(&slow, &s);
    TEST_ASSERT_EQUAL_UINT16(3013U, s_pack.cell_mv[6]);

    /* The same for an implausible reading, a temperature and the current. */
    s = sample(BMS_SAMPLE_TEMPS | BMS_SAMPLE_CURRENT);
    s.temp_dc[1] = 2000;
    s.current_ma = -400000;
    PROCESS_WITH(&slow, &s);
    TEST_ASSERT_FALSE(s_pack.temp_filter[1].seeded);
    TEST_ASSERT_FALSE(s_pack.current_filter.seeded);
    s = sample(BMS_SAMPLE_TEMPS | BMS_SAMPLE_CURRENT);
    s.temp_dc[1] = -150;
    s.current_ma = -45000;
    PROCESS_WITH(&slow, &s);
    TEST_ASSERT_EQUAL_INT16(-150, s_pack.temp_dc[1]);
    TEST_ASSERT_EQUAL_INT32(-45000, s_pack.current_ma);
}

static void test_groups_not_in_the_sample_are_left_as_they_are(void)
{
    bms_sample_t s;
    bms_pack_t before;

    start_normal();
    s = sample(BMS_SAMPLE_CELLS | BMS_SAMPLE_TEMPS);
    s.cell_mv[1] = 3500U;
    s.temp_dc[1] = 300;
    PROCESS(&s);
    before = s_pack;

    /* Only the current is fresh. Whatever the other fields say, flags included, nothing of the
     * cells and temperatures changes, their filters included. */
    s = sample(BMS_SAMPLE_CURRENT);
    s.current_ma = 20000;
    (void)memset(s.cell_mv, 0xFF, sizeof(s.cell_mv));
    s.cell_invalid = 0xFFU;
    s.temp_invalid = 0x0FU;
    s.temp_dc[0] = -30000;
    PROCESS(&s);

    TEST_ASSERT_EQUAL_MEMORY(before.cell_mv, s_pack.cell_mv, sizeof(before.cell_mv));
    TEST_ASSERT_EQUAL_MEMORY(before.temp_dc, s_pack.temp_dc, sizeof(before.temp_dc));
    TEST_ASSERT_EQUAL_MEMORY(before.cell_filter, s_pack.cell_filter, sizeof(before.cell_filter));
    TEST_ASSERT_EQUAL_MEMORY(before.temp_filter, s_pack.temp_filter, sizeof(before.temp_filter));
    TEST_ASSERT_EQUAL_HEX8(before.cell_valid, s_pack.cell_valid);
    TEST_ASSERT_EQUAL_HEX8(before.temp_valid, s_pack.temp_valid);
    TEST_ASSERT_EQUAL_UINT32(before.pack_mv, s_pack.pack_mv);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.flagged_readings);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_NORMAL, s_pack.state);
    /* 12000 -> 20000 with shift 3: 12000 + 1000 */
    TEST_ASSERT_EQUAL_INT32(13000, s_pack.current_ma);

    /* Only the temperatures: the current and the cells stay. */
    before = s_pack;
    s = sample(BMS_SAMPLE_TEMPS);
    s.current_ma = -1;
    s.current_invalid = true;
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT32(before.current_ma, s_pack.current_ma);
    TEST_ASSERT_TRUE(s_pack.current_valid);
    TEST_ASSERT_EQUAL_MEMORY(before.cell_mv, s_pack.cell_mv, sizeof(before.cell_mv));
}

/* Shifts 1, 2 and 3 for cells, temperatures and current (s_config). From the defaults:
 *   cell 0: 3600 -> 3700, shift 1: 3650;
 *   temperature 0: 200 -> 300, shift 2: s = 51200 + (25600 + 2) / 4 = 57600, 225;
 *   current: 12000 -> 20000, shift 3: 13000. */
static void test_each_group_is_filtered_with_its_own_shift(void)
{
    bms_sample_t s;

    start_normal();
    s = sample(ALL_GROUPS);
    s.cell_mv[0] = 3700U;
    s.temp_dc[0] = 300;
    s.current_ma = 20000;
    PROCESS(&s);

    TEST_ASSERT_EQUAL_UINT16(3650U, s_pack.cell_mv[0]);
    TEST_ASSERT_EQUAL_INT16(225, s_pack.temp_dc[0]);
    TEST_ASSERT_EQUAL_INT32(13000, s_pack.current_ma);
}

static void test_the_pack_voltage_is_the_sum_of_the_valid_cells(void)
{
    bms_sample_t s;
    const bms_config_t direct = {.cell_shift = 0U, .temp_shift = 0U, .current_shift = 0U};

    bms_init(&s_pack);
    s = sample(ALL_GROUPS);

    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        s.cell_mv[i] = 5000U;
    }

    PROCESS_WITH(&direct, &s);
    TEST_ASSERT_EQUAL_UINT32(40000U, s_pack.pack_mv); /* the largest there is */
    TEST_ASSERT_EQUAL_UINT32(40000U, BMS_PACK_SENSOR_MAX_MV);
    TEST_ASSERT_TRUE(s_pack.pack_valid);

    s = sample(BMS_SAMPLE_CELLS);
    s.cell_invalid = 0x81U; /* cells 0 and 7 (3600 and 3670 mV) */
    PROCESS_WITH(&direct, &s);
    TEST_ASSERT_EQUAL_UINT32(29080U - 3600U - 3670U, s_pack.pack_mv);
    TEST_ASSERT_FALSE(s_pack.pack_valid);

    s = sample(BMS_SAMPLE_CELLS);
    s.cell_invalid = 0xFFU;
    PROCESS_WITH(&direct, &s);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.pack_mv);
    TEST_ASSERT_FALSE(s_pack.pack_valid);
}

static void test_the_lowest_and_highest_cell_come_with_their_index(void)
{
    const bms_config_t direct = {.cell_shift = 0U, .temp_shift = 0U, .current_shift = 0U};
    bms_sample_t s;

    bms_init(&s_pack);
    s = sample(ALL_GROUPS);
    s.cell_mv[5] = 3100U;
    s.cell_mv[2] = 4100U;
    PROCESS_WITH(&direct, &s);
    TEST_ASSERT_EQUAL_UINT16(3100U, s_pack.min_cell_mv);
    TEST_ASSERT_EQUAL_UINT8(5U, s_pack.min_cell);
    TEST_ASSERT_EQUAL_UINT16(4100U, s_pack.max_cell_mv);
    TEST_ASSERT_EQUAL_UINT8(2U, s_pack.max_cell);

    /* Equal cells: the lowest index. */
    s = sample(BMS_SAMPLE_CELLS);
    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        s.cell_mv[i] = 3333U;
    }
    s.cell_mv[1] = 3000U;
    s.cell_mv[6] = 3000U;
    s.cell_mv[3] = 3900U;
    s.cell_mv[4] = 3900U;
    PROCESS_WITH(&direct, &s);
    TEST_ASSERT_EQUAL_UINT8(1U, s_pack.min_cell);
    TEST_ASSERT_EQUAL_UINT8(3U, s_pack.max_cell);

    /* Invalid cells do not count: without cells 1 and 3 the extremes are cells 6 and 4. */
    s.sequence = s_sequence++;
    s.cell_invalid = 0x0AU;
    PROCESS_WITH(&direct, &s);
    TEST_ASSERT_EQUAL_UINT8(6U, s_pack.min_cell);
    TEST_ASSERT_EQUAL_UINT16(3000U, s_pack.min_cell_mv);
    TEST_ASSERT_EQUAL_UINT8(4U, s_pack.max_cell);
    TEST_ASSERT_EQUAL_UINT16(3900U, s_pack.max_cell_mv);

    /* A single valid cell is both; none at all leaves zeros. */
    s.sequence = s_sequence++;
    s.cell_invalid = 0x7FU;
    PROCESS_WITH(&direct, &s);
    TEST_ASSERT_EQUAL_UINT8(7U, s_pack.min_cell);
    TEST_ASSERT_EQUAL_UINT8(7U, s_pack.max_cell);
    TEST_ASSERT_EQUAL_UINT16(3333U, s_pack.min_cell_mv);

    s.sequence = s_sequence++;
    s.cell_invalid = 0xFFU;
    PROCESS_WITH(&direct, &s);
    TEST_ASSERT_EQUAL_UINT16(0U, s_pack.min_cell_mv);
    TEST_ASSERT_EQUAL_UINT16(0U, s_pack.max_cell_mv);
    TEST_ASSERT_EQUAL_UINT8(0U, s_pack.min_cell);
    TEST_ASSERT_EQUAL_UINT8(0U, s_pack.max_cell);
}

static void test_the_fault_ends_when_every_channel_is_valid_again(void)
{
    bms_sample_t s;

    start_normal();
    s = sample(BMS_SAMPLE_TEMPS);
    s.temp_invalid = 0x08U;
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_FAULT, s_pack.state);

    /* Another channel recovering is not enough. */
    s = sample(BMS_SAMPLE_CURRENT);
    s.current_invalid = true;
    PROCESS(&s);
    s = sample(BMS_SAMPLE_CURRENT);
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_FAULT, s_pack.state);

    s = sample(BMS_SAMPLE_TEMPS);
    PROCESS(&s);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_NORMAL, s_pack.state);
}

static void test_lost_samples_are_counted_from_gaps_in_the_sequence(void)
{
    bms_sample_t s = sample(0U);

    bms_init(&s_pack);

    /* 10, 11, 14: two lost. */
    s.sequence = 10U;
    PROCESS(&s);
    s.sequence = 11U;
    PROCESS(&s);
    s.sequence = 14U;
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(2U, s_pack.lost_samples);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.sequence_errors);

    /* A repeat and a step back are errors, not losses; counting goes on from them. */
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(1U, s_pack.sequence_errors);
    s.sequence = 5U;
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(2U, s_pack.sequence_errors);
    s.sequence = 6U;
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(2U, s_pack.lost_samples);
    TEST_ASSERT_EQUAL_UINT32(2U, s_pack.sequence_errors);

    /* Across the wrap: 0xFFFFFFFE, 0xFFFFFFFF, 0, 1 lose nothing. Then 0xFFFFFFFF is a step
     * back, and 2 after it skips 0 and 1. */
    bms_init(&s_pack);
    s.sequence = 0xFFFFFFFEU;
    PROCESS(&s);
    s.sequence = 0xFFFFFFFFU;
    PROCESS(&s);
    s.sequence = 0U;
    PROCESS(&s);
    s.sequence = 1U;
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.lost_samples);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.sequence_errors);
    s.sequence = 0xFFFFFFFFU;
    PROCESS(&s);
    s.sequence = 2U;
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(2U, s_pack.lost_samples);
    TEST_ASSERT_EQUAL_UINT32(1U, s_pack.sequence_errors);

    /* The boundary: 2^31 - 1 skipped is a loss, 2^31 skipped an error. */
    bms_init(&s_pack);
    s.sequence = 0U;
    PROCESS(&s);
    s.sequence = 0x80000000U; /* 1 .. 0x7FFFFFFF skipped */
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(0x7FFFFFFFU, s_pack.lost_samples);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.sequence_errors);
    s.sequence = 0U; /* 0x80000001 .. 0xFFFFFFFF skipped: 2^31 - 1 again */
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFEU, s_pack.lost_samples);
    s.sequence = 0x80000001U; /* 1 .. 0x80000000 skipped: 2^31 */
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFEU, s_pack.lost_samples);
    TEST_ASSERT_EQUAL_UINT32(1U, s_pack.sequence_errors);
}

static void test_updates_and_stamp_follow_every_sample(void)
{
    bms_sample_t s;

    bms_init(&s_pack);
    s = sample(0U); /* no group at all: still a sample */
    s.stamp_ns = 123456789ULL;
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(1U, s_pack.updates);
    TEST_ASSERT_EQUAL_UINT64(123456789ULL, s_pack.stamp_ns);
    TEST_ASSERT_EQUAL_INT(BMS_STATE_STARTUP, s_pack.state);
    TEST_ASSERT_FALSE(s_pack.current_seen);

    s = sample(BMS_SAMPLE_CURRENT);
    s.stamp_ns = 0xFFFFFFFFFFFFFFF0ULL;
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(2U, s_pack.updates);
    TEST_ASSERT_EQUAL_UINT64(0xFFFFFFFFFFFFFFF0ULL, s_pack.stamp_ns);

    /* The count wraps. */
    s_pack.updates = UINT32_MAX;
    s = sample(0U);
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.updates);
}

static void test_bits_of_groups_that_do_not_exist_change_nothing(void)
{
    bms_sample_t s;

    bms_init(&s_pack);
    s = sample(0xF8U);
    PROCESS(&s);
    TEST_ASSERT_FALSE(s_pack.current_seen);
    TEST_ASSERT_EQUAL_HEX8(0U, s_pack.cell_seen);
    TEST_ASSERT_EQUAL_HEX8(0U, s_pack.temp_seen);
    TEST_ASSERT_EQUAL_UINT32(1U, s_pack.updates);
}

static void test_the_counters_stop_at_their_maximum(void)
{
    bms_sample_t s;

    start_normal();
    s_pack.flagged_readings = UINT32_MAX - 1U;
    s_pack.implausible_readings = UINT32_MAX - 1U;
    s_pack.sequence_errors = UINT32_MAX;
    s_pack.lost_samples = UINT32_MAX - 3U;

    s = sample(BMS_SAMPLE_CELLS);
    s.cell_invalid = 0x07U;
    s.cell_mv[7] = 9000U;
    s.cell_mv[6] = 9000U;
    s.sequence += 10U; /* 10 lost */
    PROCESS(&s);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s_pack.flagged_readings);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s_pack.implausible_readings);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s_pack.lost_samples);

    PROCESS(&s); /* a repeat */
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s_pack.sequence_errors);
}

/* ------------------------------------------------------------------------------------------
 * A long random run against a reference model of the channels
 * ---------------------------------------------------------------------------------------- */

typedef struct {
    ref_filter_t filter;
    bool seen;
    bool valid;
    int32_t value;
} ref_channel_t;

/* One reading into a reference channel: valid if not flagged and within lo..hi. */
static void ref_take(ref_channel_t *channel, bool flagged, int32_t reading, int32_t lo, int32_t hi,
                     uint8_t shift, uint32_t *flagged_count, uint32_t *implausible_count)
{
    channel->seen = true;
    channel->valid = !flagged && (reading >= lo) && (reading <= hi);

    if (flagged) {
        (*flagged_count)++;
    } else if (!channel->valid) {
        (*implausible_count)++;
    } else {
        /* valid */
    }

    if (channel->valid) {
        channel->value = ref_update(&channel->filter, reading, shift);
    } else {
        channel->filter.seeded = false;
        channel->value = 0;
    }
}

/* A reading: mostly plausible and close to `centre`, sometimes anywhere in the type. */
static int32_t random_reading(int32_t centre, int32_t spread, int32_t type_lo, int32_t type_hi)
{
    int32_t value;

    if ((random_u32() % 16U) == 0U) {
        value = random_between(type_lo, type_hi);
    } else {
        value = centre + random_between(-spread, spread);
    }

    return value;
}

static void test_a_long_random_run_agrees_with_the_reference_model(void)
{
    ref_channel_t current;
    ref_channel_t cells[BMS_CELLS];
    ref_channel_t temps[BMS_TEMPS];
    uint32_t flagged = 0U;
    uint32_t implausible = 0U;
    bms_config_t config;

    (void)memset(&current, 0, sizeof(current));
    (void)memset(cells, 0, sizeof(cells));
    (void)memset(temps, 0, sizeof(temps));
    config.cell_shift = 2U;
    config.temp_shift = 5U;
    config.current_shift = 1U;
    bms_init(&s_pack);

    for (int n = 0; n < 50000; n++) {
        bms_sample_t s;

        (void)memset(&s, 0, sizeof(s));
        s.sequence = (uint32_t)n;
        s.groups = (uint8_t)(random_u32() % 8U);
        s.current_ma = random_reading(0, 310000, INT32_MIN, INT32_MAX);
        s.current_invalid = (random_u32() % 20U) == 0U;

        for (uint32_t i = 0U; i < BMS_CELLS; i++) {
            s.cell_mv[i] = (uint16_t)random_reading(3700, 1400, 0, 65535);

            if ((random_u32() % 40U) == 0U) {
                s.cell_invalid = (uint8_t)(s.cell_invalid | (1U << i));
            }
        }

        for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
            s.temp_dc[i] = (int16_t)random_reading(400, 900, INT16_MIN, INT16_MAX);

            if ((random_u32() % 40U) == 0U) {
                s.temp_invalid = (uint8_t)(s.temp_invalid | (1U << i));
            }
        }

        PROCESS_WITH(&config, &s);

        if ((s.groups & BMS_SAMPLE_CURRENT) != 0U) {
            ref_take(&current, s.current_invalid, s.current_ma, -300000, 300000,
                     config.current_shift, &flagged, &implausible);
        }

        if ((s.groups & BMS_SAMPLE_CELLS) != 0U) {
            for (uint32_t i = 0U; i < BMS_CELLS; i++) {
                ref_take(&cells[i], (s.cell_invalid & (1U << i)) != 0U, s.cell_mv[i], 0, 5000,
                         config.cell_shift, &flagged, &implausible);
            }
        }

        if ((s.groups & BMS_SAMPLE_TEMPS) != 0U) {
            for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
                ref_take(&temps[i], (s.temp_invalid & (1U << i)) != 0U, s.temp_dc[i], -400, 1250,
                         config.temp_shift, &flagged, &implausible);
            }
        }

        TEST_ASSERT_EQUAL_INT32_MESSAGE(current.value, s_pack.current_ma, at("sample", n));
        TEST_ASSERT_EQUAL_MESSAGE(current.valid, s_pack.current_valid, at("sample", n));
        TEST_ASSERT_EQUAL_MESSAGE(current.seen, s_pack.current_seen, at("sample", n));

        for (uint32_t i = 0U; i < BMS_CELLS; i++) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(cells[i].value, (int32_t)s_pack.cell_mv[i],
                                            at("sample", n));
            TEST_ASSERT_EQUAL_MESSAGE(cells[i].valid, (s_pack.cell_valid & (1U << i)) != 0U,
                                      at("sample", n));
            TEST_ASSERT_EQUAL_MESSAGE(cells[i].seen, (s_pack.cell_seen & (1U << i)) != 0U,
                                      at("sample", n));
        }

        for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(temps[i].value, (int32_t)s_pack.temp_dc[i],
                                            at("sample", n));
            TEST_ASSERT_EQUAL_MESSAGE(temps[i].valid, (s_pack.temp_valid & (1U << i)) != 0U,
                                      at("sample", n));
            TEST_ASSERT_EQUAL_MESSAGE(temps[i].seen, (s_pack.temp_seen & (1U << i)) != 0U,
                                      at("sample", n));
        }

        TEST_ASSERT_EQUAL_UINT32_MESSAGE(flagged, s_pack.flagged_readings, at("sample", n));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(implausible, s_pack.implausible_readings, at("sample", n));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)n + 1U, s_pack.updates);
    }

    /* The run went through every state. */
    TEST_ASSERT_TRUE(flagged > 1000U);
    TEST_ASSERT_TRUE(implausible > 1000U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pack.lost_samples);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_a_cell_reading_is_plausible_from_0_to_5000_mv);
    RUN_TEST(test_a_temperature_is_plausible_from_minus_40_to_125_degrees);
    RUN_TEST(test_a_current_is_plausible_from_minus_300_to_300_amperes);

    RUN_TEST(test_the_first_input_after_a_reset_is_the_output_at_once);
    RUN_TEST(test_shift_0_passes_every_input_through);
    RUN_TEST(test_a_step_follows_the_numbers_worked_out_by_hand);
    RUN_TEST(test_the_output_rounds_halves_away_from_zero);
    RUN_TEST(test_the_update_rounds_halves_away_from_zero);
    RUN_TEST(test_negating_every_input_negates_every_state_and_output);
    RUN_TEST(test_the_filter_agrees_with_the_reference_after_every_input);
    RUN_TEST(test_a_constant_input_is_reached_exactly_without_overshoot);
    RUN_TEST(test_inputs_beyond_the_range_count_as_its_ends);
    RUN_TEST(test_the_extremes_never_overflow);
    RUN_TEST(test_a_shift_above_7_counts_as_7);
    RUN_TEST(test_a_reset_forgets_the_past);
    RUN_TEST(test_a_state_no_update_can_produce_counts_as_not_seeded);

    RUN_TEST(test_init_leaves_an_empty_pack_in_startup);
    RUN_TEST(test_the_state_is_startup_until_every_channel_has_reported);
    RUN_TEST(test_a_missing_temperature_keeps_the_pack_in_startup);
    RUN_TEST(test_an_invalid_reading_during_startup_is_a_fault_at_once);
    RUN_TEST(test_a_flagged_reading_makes_its_channel_invalid_whatever_its_value);
    RUN_TEST(test_a_reading_outside_the_sensor_range_is_implausible);
    RUN_TEST(test_a_channel_restarts_from_its_first_plausible_reading_after_an_invalid_one);
    RUN_TEST(test_groups_not_in_the_sample_are_left_as_they_are);
    RUN_TEST(test_each_group_is_filtered_with_its_own_shift);
    RUN_TEST(test_the_pack_voltage_is_the_sum_of_the_valid_cells);
    RUN_TEST(test_the_lowest_and_highest_cell_come_with_their_index);
    RUN_TEST(test_the_fault_ends_when_every_channel_is_valid_again);
    RUN_TEST(test_lost_samples_are_counted_from_gaps_in_the_sequence);
    RUN_TEST(test_updates_and_stamp_follow_every_sample);
    RUN_TEST(test_bits_of_groups_that_do_not_exist_change_nothing);
    RUN_TEST(test_the_counters_stop_at_their_maximum);

    RUN_TEST(test_a_long_random_run_agrees_with_the_reference_model);

    return UNITY_END();
}
