/* SPDX-License-Identifier: MIT */
/**
 * @file
 * What the node knows about its battery module, and the arithmetic that gets it there from
 * raw samples: plausibility of each reading, filtering, and the summary of the pack.
 *
 * Pure logic in integer arithmetic. No floating point: a Cortex-M3 has no FPU, and every
 * quantity here has a natural integer unit. No state of its own and no OS calls: the
 * processing task owns the data (a bms_pack_t) and passes it in. Every function may therefore
 * be called from any task or interrupt; the caller serialises the calls that work on one
 * bms_pack_t or one bms_filter_t (the application does so with the pack mutex), and readers of
 * a pack copy it under the same lock.
 *
 * UNITS. Cell voltages in millivolts (mV), temperatures in tenths of a degree Celsius (dC:
 * 251 is 25.1 degC), current in milliamperes (mA), positive when the pack is discharging.
 * This module knows nothing about how the values are encoded on the bus: mapping them to the
 * raw signal values of the DBC (and to "signal not available") is the caller's job.
 *
 * VALID AND INVALID READINGS. A reading can be missing for two reasons, and the difference
 * matters downstream:
 *   - the measurement front end says so itself (open sense wire, converter fault): the sample
 *     carries an invalid flag for that channel ("flagged"). Its value is then not looked at;
 *   - the value lies outside what the sensor can physically deliver (BMS_*_SENSOR_MIN/MAX):
 *     the reading is implausible, whatever the front end claims. A reading at a limit is
 *     plausible, one beyond it is not.
 * Either way the channel is invalid in the pack state, its filtered value is 0, its filter is
 * reset, and the caller publishes "signal not available" for it, never a made-up number. A
 * channel becomes valid again with the next plausible reading, and its filter restarts from
 * that reading. Whether a valid value is also SAFE (over-voltage, over-temperature ...) is not
 * decided here; that is the fault monitor's job (app/fault.h).
 *
 * A group of readings that the front end did not deliver at all (plat_sensor.h: the read
 * returned false) is passed in as read, with every channel of the group flagged invalid. It is
 * then invalid here, not stale: a group that is simply left out of a sample keeps its previous
 * values (see bms_process()).
 */
#ifndef FCN_BMS_H
#define FCN_BMS_H

#include <stdbool.h>
#include <stdint.h>

#define BMS_CELLS 8U
#define BMS_TEMPS 4U

/** Masks with one bit per cell and per temperature sensor (bit n: cell n, sensor n). */
#define BMS_ALL_CELLS 0xFFU
#define BMS_ALL_TEMPS 0x0FU

/* The masks are uint8_t, so the module has room for at most 8 channels of each kind, and the
 * mask constants have to have exactly one bit per channel. */
_Static_assert((BMS_CELLS >= 1U) && (BMS_CELLS <= 8U), "cell masks are 8 bits wide");
_Static_assert((BMS_TEMPS >= 1U) && (BMS_TEMPS <= 8U), "temperature masks are 8 bits wide");
_Static_assert(BMS_ALL_CELLS == (((uint32_t)1U << BMS_CELLS) - 1U), "one bit per cell");
_Static_assert(BMS_ALL_TEMPS == (((uint32_t)1U << BMS_TEMPS) - 1U),
               "one bit per temperature sensor");

/* What the sensors can deliver; a reading outside is implausible. The limits are inclusive. */
#define BMS_CELL_SENSOR_MIN_MV    0U
#define BMS_CELL_SENSOR_MAX_MV    5000U
#define BMS_TEMP_SENSOR_MIN_DC    (-400) /* -40.0 degC */
#define BMS_TEMP_SENSOR_MAX_DC    1250   /* 125.0 degC */
#define BMS_CURRENT_SENSOR_MAX_MA 300000 /* +-300 A: the range is -300000..300000 mA */

/** The largest pack voltage a sum of plausible cells can reach (8 x 5000 mV = 40000 mV), for
 *  the caller's checks that the bus encoding has room for it. */
#define BMS_PACK_SENSOR_MAX_MV (BMS_CELLS * BMS_CELL_SENSOR_MAX_MV)

/* bms_cell_plausible() compares with the maximum only: a uint16_t cannot lie below 0. */
_Static_assert(BMS_CELL_SENSOR_MIN_MV == 0U, "the minimum cell reading is the type's minimum");
_Static_assert(BMS_CELL_SENSOR_MAX_MV <= 65535U, "a plausible cell reading fits a uint16_t");
_Static_assert((BMS_TEMP_SENSOR_MIN_DC >= -32768) && (BMS_TEMP_SENSOR_MAX_DC <= 32767),
               "a plausible temperature fits an int16_t");

/** Which measurements a sample carries (the three groups are sampled at different rates). */
#define BMS_SAMPLE_CURRENT 0x01U
#define BMS_SAMPLE_CELLS   0x02U
#define BMS_SAMPLE_TEMPS   0x04U

/** One acquisition, as the sensor task hands it to the processing task. */
typedef struct {
    uint64_t stamp_ns;           /**< when the measurements were taken (plat_time_ns()) */
    uint32_t sequence;           /**< counts acquisitions; a gap means samples were lost */
    int32_t current_ma;          /**< meaningful if BMS_SAMPLE_CURRENT is set */
    uint16_t cell_mv[BMS_CELLS]; /**< meaningful if BMS_SAMPLE_CELLS is set */
    int16_t temp_dc[BMS_TEMPS];  /**< meaningful if BMS_SAMPLE_TEMPS is set */
    uint8_t groups;              /**< BMS_SAMPLE_*: which of the groups above are fresh */
    uint8_t cell_invalid;        /**< bit n: the front end has no valid reading for cell n */
    uint8_t temp_invalid;        /**< bit n: likewise for temperature sensor n */
    bool current_invalid;        /**< likewise for the current */
} bms_sample_t;

/* ------------------------------------------------------------------------------ plausibility */

/** True if `cell_mv` lies within BMS_CELL_SENSOR_MIN_MV..BMS_CELL_SENSOR_MAX_MV. Any context. */
bool bms_cell_plausible(uint16_t cell_mv);

/** True if `temp_dc` lies within BMS_TEMP_SENSOR_MIN_DC..BMS_TEMP_SENSOR_MAX_DC. Any context. */
bool bms_temp_plausible(int16_t temp_dc);

/** True if `current_ma` lies within -BMS_CURRENT_SENSOR_MAX_MA..BMS_CURRENT_SENSOR_MAX_MA.
 *  Any context. */
bool bms_current_plausible(int32_t current_ma);

/* ------------------------------------------------------------------------------------ filter */

/**
 * First-order low-pass filter (an exponential moving average) in fixed point:
 *
 *     y[n] = y[n-1] + (x[n] - y[n-1]) / 2^shift
 *
 * so that 2^shift samples is roughly its time constant (exactly -1 / ln(1 - 2^-shift) samples:
 * 1.44 for shift 1, 127.5 for shift 7). shift 0 passes the input through.
 *
 * The state keeps the output with BMS_FILTER_FRACTION_BITS bits of fraction, s = y * 256.
 * EXACT ROUNDING. With X = x * 256 and D = 2^shift, one update is
 *
 *     s := s + round(( X - s ) / D)        output := round(s / 256)
 *
 * where round() is to the nearest integer and a half goes away from zero (2.5 -> 3, -2.5 -> -3).
 * Consequences, each checked by the unit tests:
 *   - negating every input negates every state and output (the rounding is symmetric);
 *   - the state never passes the input: it moves from s towards X and stops at most
 *     D / 2 - 1 <= 63 sub-units short of it, less than half a unit, so the output settles
 *     exactly on a constant input instead of stopping short by a rounding remainder;
 *   - the output always lies between the smallest and the largest input since the last reset.
 *
 * Inputs are clamped to +-BMS_FILTER_INPUT_MAX first; every quantity of this module is far
 * inside that range (_Static_assert below), and the clamp makes the arithmetic total: no
 * input and no state can overflow it (the proof is in bms.c).
 */
#define BMS_FILTER_FRACTION_BITS 8U
#define BMS_FILTER_MAX_SHIFT     7U
#define BMS_FILTER_INPUT_MAX     4194303 /* 2^22 - 1 */

_Static_assert(BMS_CURRENT_SENSOR_MAX_MA <= BMS_FILTER_INPUT_MAX, "current fits the filter");
_Static_assert(BMS_CELL_SENSOR_MAX_MV <= (uint32_t)BMS_FILTER_INPUT_MAX, "cells fit the filter");
_Static_assert((-BMS_TEMP_SENSOR_MIN_DC <= BMS_FILTER_INPUT_MAX) &&
                   (BMS_TEMP_SENSOR_MAX_DC <= BMS_FILTER_INPUT_MAX),
               "temperatures fit the filter");

/** State of one filter. Set up with bms_filter_reset(); the members belong to bms.c, and the
 *  unit tests read `state` to check the rounding. */
typedef struct {
    int32_t state; /**< output * 2^BMS_FILTER_FRACTION_BITS; meaningful while `seeded` */
    bool seeded;   /**< false: the next input sets the state directly */
} bms_filter_t;

/** Forget the past: the next input becomes the output at once. Any context. */
void bms_filter_reset(bms_filter_t *filter);

/**
 * Feed one input and return the new output (rounding as above). The first input after a reset
 * becomes the output at once, with no start-up transient from zero. `input` may be any int32_t
 * (it is clamped to +-BMS_FILTER_INPUT_MAX), `shift` any value (above BMS_FILTER_MAX_SHIFT it
 * counts as BMS_FILTER_MAX_SHIFT). A state outside what an update can produce (memory that
 * was never reset) counts as not seeded. Any context.
 */
int32_t bms_filter_update(bms_filter_t *filter, int32_t input, uint8_t shift);

/** Filter coefficients per group: 2^shift samples is roughly the time constant, counted in
 *  the samples of that group (cells and temperatures are sampled less often than the current). */
typedef struct {
    uint8_t cell_shift;
    uint8_t temp_shift;
    uint8_t current_shift;
} bms_config_t;

/* -------------------------------------------------------------------------------- the pack */

/** The pack state. The numbers are those of the PackState signal on the bus. */
typedef enum {
    BMS_STATE_STARTUP = 0, /**< no channel is invalid, but not every one has reported yet */
    BMS_STATE_NORMAL = 1,  /**< every channel has reported, and every one is valid */
    BMS_STATE_FAULT = 2    /**< at least one channel is invalid (the node also sends FAULT while
                                a DTC is active: app/telemetry.h, telemetry_pack_status()) */
} bms_state_t;

/**
 * The pack as the node currently knows it: the shared state behind the application's mutex.
 * The members up to `sequence_errors` are the summary readers use; the rest belongs to bms.c.
 *
 * A channel "has reported" once a sample carried a reading for it, valid or not; `*_seen`
 * records that. `*_valid` says whether its latest reading was valid. FAULT wins over STARTUP:
 * a channel that reports an invalid reading is a fault at once, even while other channels
 * have not reported yet.
 */
typedef struct {
    uint64_t stamp_ns; /**< stamp of the newest sample that went into it */
    bms_state_t state;
    uint32_t updates; /**< samples processed (wraps); lets a reader see it is fresh */

    int32_t current_ma;          /**< filtered; 0 while not valid */
    uint32_t pack_mv;            /**< sum of the filtered values of the valid cells; it is the
                                      pack voltage only while pack_valid */
    uint16_t cell_mv[BMS_CELLS]; /**< filtered; 0 while not valid */
    int16_t temp_dc[BMS_TEMPS];  /**< filtered; 0 while not valid */
    uint16_t min_cell_mv;        /**< lowest valid cell; 0 if no cell is valid */
    uint16_t max_cell_mv;        /**< highest valid cell; 0 if no cell is valid */
    uint8_t min_cell;            /**< its index, the lowest index of equal ones; 0 if none */
    uint8_t max_cell;            /**< likewise */
    uint8_t cell_valid;          /**< bit n: cell n has a valid, filtered value */
    uint8_t temp_valid;          /**< bit n: temperature n has a valid, filtered value */
    uint8_t cell_seen;           /**< bit n: cell n has reported since bms_init() */
    uint8_t temp_seen;           /**< bit n: temperature n has reported since bms_init() */
    bool current_valid;
    bool current_seen;
    bool pack_valid; /**< every cell is valid, so pack_mv is the pack voltage */

    /* Counters; each stops at UINT32_MAX instead of wrapping. */
    uint32_t flagged_readings;     /**< readings the front end itself marked invalid */
    uint32_t implausible_readings; /**< readings rejected by the sensor-range check */
    uint32_t lost_samples;         /**< sequence numbers skipped between two samples */
    uint32_t sequence_errors;      /**< samples whose sequence number repeated or went back */

    /* Private to bms.c. */
    uint32_t next_sequence; /* the sequence number the next sample should have */
    bool have_sequence;     /* a sample has been processed, so next_sequence is meaningful */
    bms_filter_t current_filter;
    bms_filter_t cell_filter[BMS_CELLS];
    bms_filter_t temp_filter[BMS_TEMPS];
} bms_pack_t;

/** An empty pack: STARTUP, nothing seen, nothing valid, every value and counter 0, every
 *  filter reset. Needs nothing from the memory it is given. Any context. */
void bms_init(bms_pack_t *pack);

/**
 * Work one sample into the pack state. For each group the sample carries (sample->groups),
 * each channel's reading is checked (flagged by the front end, or implausible); a valid one
 * goes through the channel's filter with the group's shift from `config`, an invalid one
 * resets that filter and makes the channel invalid. Groups the sample does not carry, and bits
 * of `groups` other than BMS_SAMPLE_*, change nothing. Then the summary is recomputed (pack
 * voltage, lowest and highest cell, pack_valid, state), `updates` counts the sample and
 * `stamp_ns` takes its stamp.
 *
 * The sequence number is checked against the previous sample's: the numbers skipped between
 * the two, counted modulo 2^32 (0xFFFFFFFF -> 0 skips none), are added to lost_samples if
 * there are fewer than 2^31 of them. A sequence number that repeats or goes back reads as 2^31
 * or more skipped and counts one sequence_error instead. Either way the next sample is expected
 * to follow this one. The first sample after bms_init() sets the reference.
 *
 * Any context; the caller serialises calls on one pack.
 */
void bms_process(bms_pack_t *pack, const bms_config_t *config, const bms_sample_t *sample);

#endif /* FCN_BMS_H */
