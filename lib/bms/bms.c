/* SPDX-License-Identifier: MIT */
#include "bms/bms.h"

#include <string.h>

/*
 * What the module does is described in bms.h. This file adds how, and the proof that the
 * filter's arithmetic cannot overflow.
 *
 * NO OVERFLOW IN THE FILTER. Let M = BMS_FILTER_INPUT_MAX = 2^22 - 1 and S = 256 M = 2^30 - 256
 * (STATE_MAX below), and write D = 2^k for the divisor of the update, 1 <= D <= 128.
 *
 *   1. The clamped input x has |x| <= M, so X = 256 x has |X| <= S.
 *   2. Every state that takes part in an update has |s| <= S. A seed is some X. An update adds
 *      q = round((X - s) / D), and |q| <= |X - s| with q of the same sign as X - s or 0 (the
 *      lemma below), so the new state lies between the old one and X, both within the bound.
 *      A state found outside the bound (memory that was never reset) is not used: the filter
 *      seeds again instead (filter_seeded()).
 *   3. So d = X - s has |d| <= 2 S = 2^31 - 512, which an int32_t holds, -d exists, and the
 *      largest intermediate of round(d / D), |d| + D / 2, is at most 2^31 - 512 + 64.
 *   4. The output round(s / 256) computes |s| + 128 <= 2^30 - 128.
 *
 * Lemma: for D = 1, q = d. For D >= 2 and |d| >= 1, |q| = floor((|d| + D/2) / D) <= |d|,
 * because |d| + D/2 <= D |d| whenever (D - 1) |d| >= D/2, which holds for D >= 2 and |d| >= 1.
 *
 * WHY THE OUTPUT SETTLES EXACTLY. q is 0 only when |d| < D/2 (for D >= 2; d = 0 for D = 1),
 * so on a constant input the state stops within D/2 - 1 <= 63 sub-units of X. That is less
 * than half of one unit (128 sub-units), and round(s / 256) is then x itself.
 */

/* One unit of filter output in sub-units of the state, as the signed value the arithmetic
 * needs, and the largest state an update can produce (step 2 of the proof). */
#define FILTER_ONE 256
#define STATE_MAX  (BMS_FILTER_INPUT_MAX * FILTER_ONE)

_Static_assert((uint32_t)FILTER_ONE == ((uint32_t)1U << BMS_FILTER_FRACTION_BITS),
               "unit of the state");
_Static_assert(BMS_FILTER_INPUT_MAX == 4194303, "the overflow proof assumes M = 2^22 - 1");
_Static_assert(BMS_FILTER_MAX_SHIFT <= 7U, "the overflow proof assumes D <= 128");
_Static_assert(BMS_FILTER_MAX_SHIFT < BMS_FILTER_FRACTION_BITS,
               "D/2 - 1 sub-units must stay below half a unit for the output to settle exactly");

/* 2^shift for every shift the filter accepts. */
static const int32_t DIVISORS[] = {1, 2, 4, 8, 16, 32, 64, 128};

_Static_assert((sizeof(DIVISORS) / sizeof(DIVISORS[0])) == (BMS_FILTER_MAX_SHIFT + 1U),
               "one divisor per shift");

#define SEQUENCE_BACKWARDS 0x80000000U /* 2^31 numbers skipped or more: a step back */

/* ------------------------------------------------------------------------------ plausibility */

bool bms_cell_plausible(uint16_t cell_mv)
{
    /* The minimum is 0, which every uint16_t satisfies (_Static_assert in bms.h). */
    return (cell_mv <= BMS_CELL_SENSOR_MAX_MV);
}

bool bms_temp_plausible(int16_t temp_dc)
{
    return (temp_dc >= BMS_TEMP_SENSOR_MIN_DC) && (temp_dc <= BMS_TEMP_SENSOR_MAX_DC);
}

bool bms_current_plausible(int32_t current_ma)
{
    return (current_ma >= -BMS_CURRENT_SENSOR_MAX_MA) && (current_ma <= BMS_CURRENT_SENSOR_MAX_MA);
}

/* ------------------------------------------------------------------------------------ filter */

/* value / divisor rounded to the nearest integer, a half away from zero. divisor >= 1, and
 * |value| + divisor / 2 must fit an int32_t (step 3 and 4 of the proof). C11 division
 * truncates towards zero, so adding half the divisor to the magnitude first rounds it. */
static int32_t divide_rounded(int32_t value, int32_t divisor)
{
    const int32_t half = divisor / 2;
    int32_t quotient;

    if (value >= 0) {
        quotient = (value + half) / divisor;
    } else {
        quotient = -((half - value) / divisor);
    }

    return quotient;
}

static int32_t clamp_input(int32_t input)
{
    int32_t clamped = input;

    if (clamped > BMS_FILTER_INPUT_MAX) {
        clamped = BMS_FILTER_INPUT_MAX;
    } else if (clamped < -BMS_FILTER_INPUT_MAX) {
        clamped = -BMS_FILTER_INPUT_MAX;
    } else {
        /* within the range: unchanged */
    }

    return clamped;
}

/* True if the filter has a state an update can work from: seeded, and within the bound of
 * step 2 of the proof. */
static bool filter_seeded(const bms_filter_t *filter)
{
    return filter->seeded && (filter->state <= STATE_MAX) && (filter->state >= -STATE_MAX);
}

void bms_filter_reset(bms_filter_t *filter)
{
    filter->state = 0;
    filter->seeded = false;
}

int32_t bms_filter_update(bms_filter_t *filter, int32_t input, uint8_t shift)
{
    const int32_t target = clamp_input(input) * FILTER_ONE;
    const uint8_t k = (shift > BMS_FILTER_MAX_SHIFT) ? (uint8_t)BMS_FILTER_MAX_SHIFT : shift;

    if (filter_seeded(filter)) {
        filter->state += divide_rounded(target - filter->state, DIVISORS[k]);
    } else {
        filter->state = target;
        filter->seeded = true;
    }

    return divide_rounded(filter->state, FILTER_ONE);
}

/* -------------------------------------------------------------------------------- the pack */

/* a + b, stopping at UINT32_MAX. */
static uint32_t add_saturating(uint32_t a, uint32_t b)
{
    return (b > (UINT32_MAX - a)) ? UINT32_MAX : (a + b);
}

/* Decide whether one reading is valid and count why not: a reading the front end flagged is
 * counted as flagged only, whatever its value; one it did not flag and that lies outside the
 * sensor's range is counted as implausible. */
static bool reading_valid(bms_pack_t *pack, bool flagged, bool plausible)
{
    bool valid = false;

    if (flagged) {
        pack->flagged_readings = add_saturating(pack->flagged_readings, 1U);
    } else if (!plausible) {
        pack->implausible_readings = add_saturating(pack->implausible_readings, 1U);
    } else {
        valid = true;
    }

    return valid;
}

static void take_current(bms_pack_t *pack, const bms_config_t *config, const bms_sample_t *sample)
{
    const bool valid =
        reading_valid(pack, sample->current_invalid, bms_current_plausible(sample->current_ma));

    if (valid) {
        pack->current_ma =
            bms_filter_update(&pack->current_filter, sample->current_ma, config->current_shift);
    } else {
        bms_filter_reset(&pack->current_filter);
        pack->current_ma = 0;
    }

    pack->current_valid = valid;
    pack->current_seen = true;
}

static void take_cells(bms_pack_t *pack, const bms_config_t *config, const bms_sample_t *sample)
{
    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        const uint8_t bit = (uint8_t)(1U << i);
        const bool valid = reading_valid(pack, (sample->cell_invalid & bit) != 0U,
                                         bms_cell_plausible(sample->cell_mv[i]));

        if (valid) {
            /* The output lies between the inputs since the last reset, which were all
             * plausible (0..5000 mV), so it fits the uint16_t. */
            pack->cell_mv[i] = (uint16_t)bms_filter_update(
                &pack->cell_filter[i], (int32_t)sample->cell_mv[i], config->cell_shift);
            pack->cell_valid |= bit;
        } else {
            bms_filter_reset(&pack->cell_filter[i]);
            pack->cell_mv[i] = 0U;
            pack->cell_valid &= (uint8_t)~bit;
        }

        pack->cell_seen |= bit;
    }
}

static void take_temps(bms_pack_t *pack, const bms_config_t *config, const bms_sample_t *sample)
{
    for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
        const uint8_t bit = (uint8_t)(1U << i);
        const bool valid = reading_valid(pack, (sample->temp_invalid & bit) != 0U,
                                         bms_temp_plausible(sample->temp_dc[i]));

        if (valid) {
            /* Between plausible inputs (-400..1250 dC), so it fits the int16_t. */
            pack->temp_dc[i] = (int16_t)bms_filter_update(
                &pack->temp_filter[i], (int32_t)sample->temp_dc[i], config->temp_shift);
            pack->temp_valid |= bit;
        } else {
            bms_filter_reset(&pack->temp_filter[i]);
            pack->temp_dc[i] = 0;
            pack->temp_valid &= (uint8_t)~bit;
        }

        pack->temp_seen |= bit;
    }
}

/* Pack voltage, lowest and highest cell, pack_valid and the state, from the channels. */
static void summarize(bms_pack_t *pack)
{
    uint32_t sum = 0U;
    bool any_valid = false;

    pack->min_cell_mv = 0U;
    pack->max_cell_mv = 0U;
    pack->min_cell = 0U;
    pack->max_cell = 0U;

    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        if ((pack->cell_valid & (uint8_t)(1U << i)) != 0U) {
            const uint16_t cell = pack->cell_mv[i];

            sum += (uint32_t)cell;

            /* Strictly lower or higher: of equal cells the lower index keeps its place. */
            if (!any_valid || (cell < pack->min_cell_mv)) {
                pack->min_cell_mv = cell;
                pack->min_cell = (uint8_t)i;
            }

            if (!any_valid || (cell > pack->max_cell_mv)) {
                pack->max_cell_mv = cell;
                pack->max_cell = (uint8_t)i;
            }

            any_valid = true;
        }
    }

    pack->pack_mv = sum;
    pack->pack_valid = (pack->cell_valid == BMS_ALL_CELLS);

    /* A valid channel has always reported, so `seen & ~valid` are the channels whose latest
     * reading was invalid. */
    const bool any_invalid = (pack->current_seen && !pack->current_valid) ||
                             ((pack->cell_seen & (uint8_t)~pack->cell_valid) != 0U) ||
                             ((pack->temp_seen & (uint8_t)~pack->temp_valid) != 0U);
    const bool all_seen = pack->current_seen && (pack->cell_seen == BMS_ALL_CELLS) &&
                          (pack->temp_seen == BMS_ALL_TEMPS);

    if (any_invalid) {
        pack->state = BMS_STATE_FAULT;
    } else if (all_seen) {
        pack->state = BMS_STATE_NORMAL;
    } else {
        pack->state = BMS_STATE_STARTUP;
    }
}

static void track_sequence(bms_pack_t *pack, uint32_t sequence)
{
    if (pack->have_sequence) {
        /* Modulo 2^32: the numbers between the expected one and this one. */
        const uint32_t skipped = sequence - pack->next_sequence;

        if (skipped < SEQUENCE_BACKWARDS) {
            pack->lost_samples = add_saturating(pack->lost_samples, skipped);
        } else {
            pack->sequence_errors = add_saturating(pack->sequence_errors, 1U);
        }
    }

    pack->have_sequence = true;
    pack->next_sequence = sequence + 1U;
}

void bms_init(bms_pack_t *pack)
{
    /* Every value and counter 0, nothing seen, nothing valid, no sequence yet. */
    (void)memset(pack, 0, sizeof(*pack));
    pack->state = BMS_STATE_STARTUP;
    bms_filter_reset(&pack->current_filter);

    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        bms_filter_reset(&pack->cell_filter[i]);
    }

    for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
        bms_filter_reset(&pack->temp_filter[i]);
    }
}

void bms_process(bms_pack_t *pack, const bms_config_t *config, const bms_sample_t *sample)
{
    track_sequence(pack, sample->sequence);
    pack->updates++;
    pack->stamp_ns = sample->stamp_ns;

    if ((sample->groups & BMS_SAMPLE_CURRENT) != 0U) {
        take_current(pack, config, sample);
    }

    if ((sample->groups & BMS_SAMPLE_CELLS) != 0U) {
        take_cells(pack, config, sample);
    }

    if ((sample->groups & BMS_SAMPLE_TEMPS) != 0U) {
        take_temps(pack, config, sample);
    }

    summarize(pack);
}
