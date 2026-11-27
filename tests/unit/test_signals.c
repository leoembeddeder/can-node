/* SPDX-License-Identifier: MIT */
/*
 * The conversion from the node's units to the raw values of the DBC (app/signals.c).
 *
 * Each conversion is checked three ways: worked numbers at the edges (the rounding halves, the
 * offsets, the saturation limits, SNA), a sweep over its whole input range (or a range far
 * beyond what the sensors deliver) against a reference written differently, and the
 * properties the header promises (odd symmetry, never SNA for a valid value). The DBC's
 * factors are written here as literals (10 mA per bit, 1000 mV offset, -40 degC), so a change
 * of the DBC that signals.c follows still shows up as a failing test.
 */
#include <stdbool.h>
#include <stdint.h>

#include "signals.h"
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* round(x / 10), a half away from zero, by C's truncating division and the remainder. */
static int32_t reference_div10(int32_t x)
{
    int32_t q = x / 10;
    const int32_t r = x % 10;

    if (r >= 5) {
        q++;
    } else if (r <= -5) {
        q--;
    } else {
        /* closer to q */
    }

    return q;
}

static int32_t clamp(int32_t x, int32_t lo, int32_t hi)
{
    int32_t result = x;

    if (x < lo) {
        result = lo;
    } else if (x > hi) {
        result = hi;
    } else {
        /* inside */
    }

    return result;
}

/* ---------------------------------------------------------------------------- PackCurrent */

static void test_the_pack_current_is_counted_in_10_ma_and_rounds_halves_away_from_zero(void)
{
    TEST_ASSERT_EQUAL_INT16(0, signals_pack_current(0));
    TEST_ASSERT_EQUAL_INT16(0, signals_pack_current(4));
    TEST_ASSERT_EQUAL_INT16(1, signals_pack_current(5));   /* 0.5 -> 1 */
    TEST_ASSERT_EQUAL_INT16(-1, signals_pack_current(-5)); /* -0.5 -> -1 */
    TEST_ASSERT_EQUAL_INT16(0, signals_pack_current(-4));
    TEST_ASSERT_EQUAL_INT16(2, signals_pack_current(15));   /* 1.5 -> 2 */
    TEST_ASSERT_EQUAL_INT16(-2, signals_pack_current(-15)); /* -1.5 -> -2 */
    TEST_ASSERT_EQUAL_INT16(123, signals_pack_current(1234));
    TEST_ASSERT_EQUAL_INT16(124, signals_pack_current(1235));
    TEST_ASSERT_EQUAL_INT16(4512, signals_pack_current(45123));
    /* The sensor's range, +-300 A, is inside the signal's. */
    TEST_ASSERT_EQUAL_INT16(30000, signals_pack_current(300000));
    TEST_ASSERT_EQUAL_INT16(-30000, signals_pack_current(-300000));
}

static void test_a_pack_current_beyond_the_signal_saturates(void)
{
    TEST_ASSERT_EQUAL_INT16(32767, signals_pack_current(327674)); /* 32767.4 */
    TEST_ASSERT_EQUAL_INT16(32767, signals_pack_current(327675)); /* 32767.5 -> 32768 */
    TEST_ASSERT_EQUAL_INT16(32767, signals_pack_current(INT32_MAX));
    TEST_ASSERT_EQUAL_INT16(-32768, signals_pack_current(-327684)); /* -32768.4 */
    TEST_ASSERT_EQUAL_INT16(-32768, signals_pack_current(-327685)); /* -32768.5 -> -32769 */
    TEST_ASSERT_EQUAL_INT16(-32768, signals_pack_current(INT32_MIN));
}

static void test_the_pack_current_agrees_with_the_reference_and_is_odd_symmetric(void)
{
    for (int32_t ma = -400000; ma <= 400000; ma++) {
        const int16_t raw = signals_pack_current(ma);
        const int32_t expected = clamp(reference_div10(ma), -32768, 32767);

        if (raw != expected) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(expected, raw, "against the reference");
        }

        if ((ma > -327670) && (ma < 327670) && (signals_pack_current(-ma) != -raw)) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(-raw, signals_pack_current(-ma), "odd symmetry");
        }
    }
}

/* ---------------------------------------------------------------------------- PackVoltage */

static void test_the_pack_voltage_is_counted_in_mv_and_saturates_at_65535(void)
{
    TEST_ASSERT_EQUAL_UINT16(0U, signals_pack_voltage(0U));
    TEST_ASSERT_EQUAL_UINT16(29569U, signals_pack_voltage(29569U));
    TEST_ASSERT_EQUAL_UINT16(40000U, signals_pack_voltage(40000U));
    TEST_ASSERT_EQUAL_UINT16(65535U, signals_pack_voltage(65535U));
    TEST_ASSERT_EQUAL_UINT16(65535U, signals_pack_voltage(65536U));
    TEST_ASSERT_EQUAL_UINT16(65535U, signals_pack_voltage(UINT32_MAX));
}

/* ---------------------------------------------------------------------------------- cells */

static void test_a_cell_is_counted_in_mv_from_1000_mv(void)
{
    TEST_ASSERT_EQUAL_UINT16(0U, signals_cell_voltage(1000U, true));
    TEST_ASSERT_EQUAL_UINT16(1U, signals_cell_voltage(1001U, true));
    TEST_ASSERT_EQUAL_UINT16(2700U, signals_cell_voltage(3700U, true));
    TEST_ASSERT_EQUAL_UINT16(4000U, signals_cell_voltage(5000U, true));
    TEST_ASSERT_EQUAL_UINT16(4094U, signals_cell_voltage(5094U, true));
}

static void test_a_valid_cell_outside_the_signal_is_sent_as_its_nearest_value_never_as_sna(void)
{
    /* Below 1000 mV: 1000 mV. A deep-discharged cell must not look like a missing one. */
    TEST_ASSERT_EQUAL_UINT16(0U, signals_cell_voltage(999U, true));
    TEST_ASSERT_EQUAL_UINT16(0U, signals_cell_voltage(0U, true));
    /* Above 5094 mV: 5094 mV, one below the SNA code. */
    TEST_ASSERT_EQUAL_UINT16(4094U, signals_cell_voltage(5095U, true));
    TEST_ASSERT_EQUAL_UINT16(4094U, signals_cell_voltage(65535U, true));

    for (uint32_t mv = 0U; mv <= 65535U; mv++) {
        const uint16_t raw = signals_cell_voltage((uint16_t)mv, true);
        const int32_t expected = clamp((int32_t)mv - 1000, 0, 4094);

        if ((int32_t)raw != expected) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(expected, raw, "against the reference");
        }
    }
}

static void test_an_invalid_cell_is_sna_whatever_its_value(void)
{
    TEST_ASSERT_EQUAL_UINT16(4095U, SIGNALS_CELL_SNA);
    TEST_ASSERT_EQUAL_UINT16(4095U, signals_cell_voltage(0U, false));
    TEST_ASSERT_EQUAL_UINT16(4095U, signals_cell_voltage(3700U, false));
    TEST_ASSERT_EQUAL_UINT16(4095U, signals_cell_voltage(65535U, false));
}

/* --------------------------------------------------------------------------- temperatures */

static void test_a_temperature_is_counted_in_degrees_from_minus_40_and_rounds_halves_away(void)
{
    TEST_ASSERT_EQUAL_UINT8(40U, signals_temperature(0, true));
    TEST_ASSERT_EQUAL_UINT8(65U, signals_temperature(251, true));   /* 25.1 -> 25 */
    TEST_ASSERT_EQUAL_UINT8(66U, signals_temperature(255, true));   /* 25.5 -> 26 */
    TEST_ASSERT_EQUAL_UINT8(65U, signals_temperature(245, true));   /* 24.5 -> 25 */
    TEST_ASSERT_EQUAL_UINT8(65U, signals_temperature(254, true));   /* 25.4 -> 25 */
    TEST_ASSERT_EQUAL_UINT8(14U, signals_temperature(-255, true));  /* -25.5 -> -26 */
    TEST_ASSERT_EQUAL_UINT8(15U, signals_temperature(-254, true));  /* -25.4 -> -25 */
    TEST_ASSERT_EQUAL_UINT8(0U, signals_temperature(-400, true));   /* -40, the sensor minimum */
    TEST_ASSERT_EQUAL_UINT8(165U, signals_temperature(1250, true)); /* 125, the sensor maximum */
    TEST_ASSERT_EQUAL_UINT8(254U, signals_temperature(2140, true)); /* 214, the largest */
}

static void test_a_valid_temperature_outside_the_signal_saturates_and_is_never_sna(void)
{
    TEST_ASSERT_EQUAL_UINT8(0U, signals_temperature(-405, true)); /* -40.5 -> -41 */
    TEST_ASSERT_EQUAL_UINT8(0U, signals_temperature(INT16_MIN, true));
    TEST_ASSERT_EQUAL_UINT8(254U, signals_temperature(2145, true)); /* 214.5 -> 215 */
    TEST_ASSERT_EQUAL_UINT8(254U, signals_temperature(INT16_MAX, true));

    for (int32_t dc = INT16_MIN; dc <= INT16_MAX; dc++) {
        const uint8_t raw = signals_temperature((int16_t)dc, true);
        const int32_t expected = clamp(reference_div10(dc) + 40, 0, 254);

        if ((int32_t)raw != expected) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(expected, raw, "against the reference");
        }
    }
}

static void test_an_invalid_temperature_is_sna_whatever_its_value(void)
{
    TEST_ASSERT_EQUAL_UINT8(255U, SIGNALS_TEMPERATURE_SNA);
    TEST_ASSERT_EQUAL_UINT8(255U, signals_temperature(0, false));
    TEST_ASSERT_EQUAL_UINT8(255U, signals_temperature(251, false));
    TEST_ASSERT_EQUAL_UINT8(255U, signals_temperature(INT16_MIN, false));
}

/* ----------------------------------------------------------------- state, share, counters */

static void test_the_pack_state_keeps_its_number_and_an_unknown_one_is_a_fault(void)
{
    TEST_ASSERT_EQUAL_UINT8(0U, signals_pack_state(BMS_STATE_STARTUP));
    TEST_ASSERT_EQUAL_UINT8(1U, signals_pack_state(BMS_STATE_NORMAL));
    TEST_ASSERT_EQUAL_UINT8(2U, signals_pack_state(BMS_STATE_FAULT));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): memory that is damaged */
    TEST_ASSERT_EQUAL_UINT8(2U, signals_pack_state((bms_state_t)3));
}

static void test_the_cpu_share_is_counted_in_hundredths_of_a_percent(void)
{
    TEST_ASSERT_EQUAL_UINT16(0U, signals_cpu_share(0U, 0U));
    TEST_ASSERT_EQUAL_UINT16(0U, signals_cpu_share(5U, 0U));
    TEST_ASSERT_EQUAL_UINT16(0U, signals_cpu_share(0U, 1000U));
    TEST_ASSERT_EQUAL_UINT16(10000U, signals_cpu_share(1000U, 1000U));
    TEST_ASSERT_EQUAL_UINT16(10000U, signals_cpu_share(2000U, 1000U));      /* never above 100 % */
    TEST_ASSERT_EQUAL_UINT16(3333U, signals_cpu_share(1U, 3U));             /* 33.333 % */
    TEST_ASSERT_EQUAL_UINT16(6667U, signals_cpu_share(2U, 3U));             /* 66.667 % */
    TEST_ASSERT_EQUAL_UINT16(1U, signals_cpu_share(1U, 20000U));            /* 0.005 % -> 0.01 */
    TEST_ASSERT_EQUAL_UINT16(0U, signals_cpu_share(1U, 20001U));            /* just below half */
    TEST_ASSERT_EQUAL_UINT16(7745U, signals_cpu_share(1549000U, 2000000U)); /* 77.45 % */
}

static void test_the_cpu_share_of_a_run_of_any_length_does_not_overflow(void)
{
    TEST_ASSERT_EQUAL_UINT16(5000U, signals_cpu_share(UINT64_MAX / 2U, UINT64_MAX));
    TEST_ASSERT_EQUAL_UINT16(10000U, signals_cpu_share(UINT64_MAX - 1U, UINT64_MAX));
    TEST_ASSERT_EQUAL_UINT16(2500U, signals_cpu_share(UINT64_MAX / 4U, UINT64_MAX));
    TEST_ASSERT_EQUAL_UINT16(0U, signals_cpu_share(1U, UINT64_MAX));
}

static void test_saturate_returns_the_value_up_to_the_maximum(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, signals_saturate(0U, 255U));
    TEST_ASSERT_EQUAL_UINT32(255U, signals_saturate(255U, 255U));
    TEST_ASSERT_EQUAL_UINT32(255U, signals_saturate(256U, 255U));
    TEST_ASSERT_EQUAL_UINT32(65535U, signals_saturate(UINT32_MAX, 65535U));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_pack_current_is_counted_in_10_ma_and_rounds_halves_away_from_zero);
    RUN_TEST(test_a_pack_current_beyond_the_signal_saturates);
    RUN_TEST(test_the_pack_current_agrees_with_the_reference_and_is_odd_symmetric);
    RUN_TEST(test_the_pack_voltage_is_counted_in_mv_and_saturates_at_65535);
    RUN_TEST(test_a_cell_is_counted_in_mv_from_1000_mv);
    RUN_TEST(test_a_valid_cell_outside_the_signal_is_sent_as_its_nearest_value_never_as_sna);
    RUN_TEST(test_an_invalid_cell_is_sna_whatever_its_value);
    RUN_TEST(test_a_temperature_is_counted_in_degrees_from_minus_40_and_rounds_halves_away);
    RUN_TEST(test_a_valid_temperature_outside_the_signal_saturates_and_is_never_sna);
    RUN_TEST(test_an_invalid_temperature_is_sna_whatever_its_value);
    RUN_TEST(test_the_pack_state_keeps_its_number_and_an_unknown_one_is_a_fault);
    RUN_TEST(test_the_cpu_share_is_counted_in_hundredths_of_a_percent);
    RUN_TEST(test_the_cpu_share_of_a_run_of_any_length_does_not_overflow);
    RUN_TEST(test_saturate_returns_the_value_up_to_the_maximum);
    return UNITY_END();
}
