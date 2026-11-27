/* SPDX-License-Identifier: MIT */
/*
 * The measurement front end of the host build (plat_sensor.h) as a model, and its simulation
 * control commands. docs/sensor-model.md describes it for the reader of a result; this comment
 * says how it is built.
 *
 * NO CLOCK. The model has no time of its own: it advances with the readings taken from it.
 * Reading n of a channel (counted from 0 since posix_sensor_start(), answered or not) is a
 * function of the seed, the channel and n alone, and a cell's reading also of how many current
 * readings were taken before it. A run therefore produces the same values whatever the host's
 * timing, a test can rely on them, and a fault on one channel changes the readings of no other.
 * The sensor task reads the current every 10 ms, so one current reading stands for 10 ms of
 * the load profile.
 *
 * THE MODEL.
 *   current        the load profile PROFILE, one step per current reading, repeating every
 *                  2000 readings (20 s at 10 ms per reading), plus noise of up to +-150 mA;
 *   cell n         its open-circuit voltage minus the current times its internal resistance,
 *                  rounded to the nearest mV, plus noise of up to +-2 mV. The current is the
 *                  profile's value (the true current, not the noisy reading) at the latest
 *                  current reading, or at step 0 before the first one;
 *   temperature n  a fixed value per sensor plus noise of up to +-0.2 degC.
 * The noise is triangular, the sum of two uniform draws, which makes the middle values more
 * likely than the extremes, as with real converter noise. Reading n of channel c is SplitMix64's
 * output number n + 1 (Steele, Lea and Flood, 2014) for a key made from the seed and c, which
 * takes the place of SplitMix64's seed: the key plus n + 1 times its constant step, through its
 * mixing function. Nothing else enters the draw.
 *
 * FAULTS. Each channel is in one of three modes, set through simulation control: following the
 * model, held at a value (reported exactly, valid, without noise, whatever the value), or open
 * (flagged invalid; the value reported is the model's, so that an application that ignores the
 * flag publishes a plausible number and a test of the flag cannot pass by way of the range
 * check). Holding the current changes its reading only: the cells keep following the true
 * current. Off line, the front end answers no read at all. The model runs on through every
 * fault: once a fault is cleared, the readings are those of a run without it.
 *
 * THE HANG. "sensor.hang on" or "spin" makes every read wait at its start until "sensor.hang
 * off" (or "sensor.clear"): the hardware fault behind the watchdog tests, a front end that
 * never completes a transfer. "on" is a driver that waits for its transfer-complete interrupt:
 * the read calls the wait function the node set (posix_sensor_set_wait(), one tick of
 * vTaskDelay), so the task is blocked and every task below it runs on. "spin" is a driver that
 * polls a busy flag: the read never gives up the CPU as far as the scheduler can tell, so every
 * task below the caller starves. (On the host the thread sleeps 1 ms between its looks at the
 * flag, so that it does not burn a host core; the scheduler does not see the sleep.) A read
 * that waited is counted in `hung` when it starts to wait and is taken, numbered and answered
 * as any other once the hang ends: the model has no clock, so the readings go on with the next
 * number.
 *
 * THREADS. The read functions run in the sensor task, the commands in the thread that serves
 * simulation control (the CAN hardware thread). They share only atomics: one 64-bit word per
 * channel holds its mode and its held value together, so a reading sees either the setting
 * before a command or the one after it, never a mix of the two; the off-line flag, the hang and
 * the counters are atomics of their own. Nothing here takes a lock, and nothing blocks except
 * a read during a hang, so a FreeRTOS task may call the read functions (posix_platform.h). The
 * seed and the wait function are written before either thread runs, and only read afterwards.
 */
#include "posix_sensor.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "plat_sensor.h"
#include "posix_sim.h"
#include "posix_trace.h"

/* Channels, in the order of the status reply: the current, the cells, the temperatures. */
#define CH_CURRENT 0U
#define CH_CELL(n) (1U + (n))
#define CH_TEMP(n) (1U + BMS_CELLS + (n))
#define CH_COUNT   (1U + BMS_CELLS + BMS_TEMPS)

/* The three groups the read functions take, for the read counters. */
#define GROUP_CURRENT 0U
#define GROUP_CELLS   1U
#define GROUP_TEMPS   2U
#define GROUPS        3U

/* The mode of a channel. */
#define MODE_MODEL 0U /* the model's value */
#define MODE_HELD  1U /* the held value, valid */
#define MODE_OPEN  2U /* flagged invalid */

/* The hang of the front end. */
#define HANG_OFF   0U /* reads return */
#define HANG_BLOCK 1U /* "on": a read blocks its task through the wait function */
#define HANG_SPIN  2U /* "spin": a read keeps the CPU */

/* How long a read sleeps between two looks at the hang without a wait function, or in spin. */
#define HANG_NAP_NS 1000000L

/* Noise amplitudes: a reading lies within +-this of the model's value. */
#define CURRENT_NOISE_MA 150U
#define CELL_NOISE_MV    2U
#define TEMP_NOISE_DC    2U

/* The longest word of a command the parser keeps, and the most words a command has. */
#define WORD_CHARS 24U
#define MAX_WORDS  2U

typedef struct {
    uint32_t steps;     /* current readings the segment lasts */
    int32_t current_ma; /* positive: discharging */
} segment_t;

/* The load profile: 2000 steps, 20 s at the sensor task's 10 ms. */
static const segment_t PROFILE[] = {
    {300U, 2000},   /* 3 s at rest: the module's own electronics */
    {800U, 45000},  /* 8 s of steady discharge */
    {150U, 160000}, /* 1.5 s of peak discharge */
    {450U, 45000},  /* 4.5 s of steady discharge */
    {300U, -80000}, /* 3 s of charging (regenerative braking) */
};
#define PROFILE_STEPS 2000U

#define PROFILE_SEGMENTS (sizeof(PROFILE) / sizeof(PROFILE[0]))

/* Open-circuit voltage and internal resistance of each cell, and each sensor's temperature:
 * a little different per channel, as in a real module, so that a value on the wrong channel
 * shows. */
static const int32_t CELL_OCV_MV[] = {3702, 3698, 3705, 3695, 3700, 3703, 3697, 3701};
static const int32_t CELL_RESISTANCE_UOHM[] = {1800, 1900, 1750, 2100, 1850, 1950, 1800, 2000};
static const int32_t TEMP_DC[] = {251, 248, 256, 245};

_Static_assert((sizeof(CELL_OCV_MV) / sizeof(CELL_OCV_MV[0])) == BMS_CELLS, "one OCV per cell");
_Static_assert((sizeof(CELL_RESISTANCE_UOHM) / sizeof(CELL_RESISTANCE_UOHM[0])) == BMS_CELLS,
               "one resistance per cell");
_Static_assert((sizeof(TEMP_DC) / sizeof(TEMP_DC[0])) == BMS_TEMPS, "one value per sensor");

static const char *const CHANNEL_NAMES[] = {"current", "cell1", "cell2", "cell3", "cell4",
                                            "cell5",   "cell6", "cell7", "cell8", "temp1",
                                            "temp2",   "temp3", "temp4"};

_Static_assert((sizeof(CHANNEL_NAMES) / sizeof(CHANNEL_NAMES[0])) == CH_COUNT,
               "one name per channel");

/* Every name is shorter than a word the parser keeps, so no name can be cut short. */
_Static_assert(sizeof("current") <= WORD_CHARS, "channel names fit a word");

static uint32_t s_seed;
static _Atomic uint64_t s_channels[CH_COUNT]; /* mode << 32 | held value as uint32_t */
static _Atomic bool s_offline;
static _Atomic uint64_t s_reads[GROUPS]; /* readings taken, answered or not */
static _Atomic uint64_t s_unanswered;    /* of those, the ones taken while off line */
static _Atomic uint32_t s_hang;          /* HANG_OFF, HANG_BLOCK or HANG_SPIN */
static _Atomic uint64_t s_hung;          /* reads that had to wait for a hang to end */
static posix_sensor_wait_fn s_wait;      /* set before the threads run; NULL: sleep */

static const char *const HANG_NAMES[] = {"off", "on", "spin"};

_Static_assert((sizeof(HANG_NAMES) / sizeof(HANG_NAMES[0])) == (HANG_SPIN + 1U),
               "one name per hang mode");

/* --------------------------------------------------------------------------------- the model */

/* SplitMix64's mixing function: a bijection of 64-bit values that spreads every input bit over
 * the whole output. */
static uint64_t mix64(uint64_t z)
{
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

/* The noise of reading n of channel `channel`: triangular in -amplitude..+amplitude. */
static int32_t noise(uint32_t channel, uint64_t n, uint32_t amplitude)
{
    /* The key differs for every pair of seed and channel, since mix64 is a bijection. */
    const uint64_t key = mix64(((uint64_t)s_seed << 32U) | (uint64_t)channel);
    const uint64_t z = mix64(key + ((n + 1U) * 0x9E3779B97F4A7C15ULL));
    const uint64_t span = (uint64_t)amplitude + 1U;
    const uint64_t first = (z & 0xFFFFFFFFULL) % span;
    const uint64_t second = (z >> 32U) % span;

    return (int32_t)(first + second) - (int32_t)amplitude;
}

/* The profile's current at `step`. */
static int32_t profile_current_ma(uint64_t step)
{
    uint64_t left = step % PROFILE_STEPS;
    size_t segment = 0U;

    while ((segment < (PROFILE_SEGMENTS - 1U)) && (left >= PROFILE[segment].steps)) {
        left -= PROFILE[segment].steps;
        segment++;
    }

    return PROFILE[segment].current_ma;
}

/* The true current the cells see: the profile at the latest current reading. */
static int32_t true_current_ma(void)
{
    const uint64_t taken = atomic_load(&s_reads[GROUP_CURRENT]);

    return profile_current_ma((taken > 0U) ? (taken - 1U) : 0U);
}

/* numerator / denominator rounded to the nearest integer, a half away from zero; denominator
 * > 0. */
static int64_t divide_rounded(int64_t numerator, int64_t denominator)
{
    const int64_t half = denominator / 2;

    return (numerator >= 0) ? ((numerator + half) / denominator)
                            : -((half - numerator) / denominator);
}

/* -------------------------------------------------------------------------------- the faults */

static uint64_t channel_word(uint32_t mode, int32_t value)
{
    return ((uint64_t)mode << 32U) | (uint64_t)(uint32_t)value;
}

static uint32_t word_mode(uint64_t word)
{
    return (uint32_t)(word >> 32U);
}

static int32_t word_value(uint64_t word)
{
    const int64_t raw = (int64_t)(word & 0xFFFFFFFFULL);

    return (int32_t)((raw > INT32_MAX) ? (raw - 4294967296LL) : raw);
}

/* What the front end reports for `channel`, whose model value is `model`. */
static int32_t reported(uint32_t channel, int32_t model, bool *invalid)
{
    const uint64_t word = atomic_load(&s_channels[channel]);
    int32_t value = model;

    *invalid = false;

    if (word_mode(word) == MODE_HELD) {
        value = word_value(word);
    } else if (word_mode(word) == MODE_OPEN) {
        *invalid = true;
    } else {
        /* following the model */
    }

    return value;
}

static void nap(void)
{
    const struct timespec pause = {0, HANG_NAP_NS};

    (void)nanosleep(&pause, NULL);
}

/* While the front end hangs, a read does not get past this point (see THE HANG). */
static void wait_while_hanging(void)
{
    uint32_t mode = atomic_load(&s_hang);

    if (mode != HANG_OFF) {
        (void)atomic_fetch_add(&s_hung, 1U);

        while (mode != HANG_OFF) {
            if ((mode == HANG_BLOCK) && (s_wait != NULL)) {
                s_wait();
            } else {
                nap();
            }

            mode = atomic_load(&s_hang);
        }
    }
}

/* Number this reading of `group` in *n and say whether the front end answers it, once the
 * front end is not hanging. */
static bool take_reading(uint32_t group, uint64_t *n)
{
    wait_while_hanging();

    const bool answered = !atomic_load(&s_offline);

    *n = atomic_fetch_add(&s_reads[group], 1U);

    if (!answered) {
        (void)atomic_fetch_add(&s_unanswered, 1U);
    }

    return answered;
}

/* ---------------------------------------------------------------------- plat_sensor.h, task side */

bool plat_sensor_read_current(int32_t *current_ma, bool *invalid)
{
    uint64_t n;
    const bool answered = take_reading(GROUP_CURRENT, &n);

    /* One current reading per sample: its number is the sample's (posix_trace.h; nothing is
     * recorded unless the node runs with --trace-file). */
    posix_trace_sensor(n);

    if (answered) {
        const int32_t model = profile_current_ma(n) + noise(CH_CURRENT, n, CURRENT_NOISE_MA);

        *current_ma = reported(CH_CURRENT, model, invalid);
    }

    return answered;
}

bool plat_sensor_read_cells(uint16_t cell_mv[BMS_CELLS], uint8_t *invalid_mask)
{
    uint64_t n;
    const bool answered = take_reading(GROUP_CELLS, &n);

    if (answered) {
        const int64_t current_ma = true_current_ma();
        uint8_t mask = 0U;

        for (uint32_t i = 0U; i < BMS_CELLS; i++) {
            /* mA x micro-ohm = 1e-9 V = 1e-6 mV */
            const int64_t drop_mv = divide_rounded(current_ma * CELL_RESISTANCE_UOHM[i], 1000000);
            const int32_t model =
                (int32_t)(CELL_OCV_MV[i] - drop_mv) + noise(CH_CELL(i), n, CELL_NOISE_MV);
            bool invalid;

            /* A model value lies between 3.3 and 3.9 V, and a held one was checked to fit a
             * uint16_t when it was set. */
            cell_mv[i] = (uint16_t)reported(CH_CELL(i), model, &invalid);

            if (invalid) {
                mask |= (uint8_t)(1U << i);
            }
        }

        *invalid_mask = mask;
    }

    return answered;
}

bool plat_sensor_read_temps(int16_t temp_dc[BMS_TEMPS], uint8_t *invalid_mask)
{
    uint64_t n;
    const bool answered = take_reading(GROUP_TEMPS, &n);

    if (answered) {
        uint8_t mask = 0U;

        for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
            const int32_t model = TEMP_DC[i] + noise(CH_TEMP(i), n, TEMP_NOISE_DC);
            bool invalid;

            /* A model value lies near 25 degC, a held one was checked to fit an int16_t. */
            temp_dc[i] = (int16_t)reported(CH_TEMP(i), model, &invalid);

            if (invalid) {
                mask |= (uint8_t)(1U << i);
            }
        }

        *invalid_mask = mask;
    }

    return answered;
}

/* ------------------------------------------------------------------------ simulation control */

typedef struct {
    char word[MAX_WORDS][WORD_CHARS];
    size_t count;  /* words in the arguments, also those beyond MAX_WORDS */
    bool too_long; /* a word did not fit WORD_CHARS - 1 characters */
} words_t;

/* Split the arguments at spaces. The first MAX_WORDS words are kept. */
static void split_words(const char *args, words_t *out)
{
    const char *p = args;

    (void)memset(out, 0, sizeof(*out));

    for (;;) {
        while (*p == ' ') {
            p++;
        }

        if (*p == '\0') {
            break;
        }

        const char *start = p;

        while ((*p != ' ') && (*p != '\0')) {
            p++;
        }

        const size_t length = (size_t)(p - start);

        if (out->count < MAX_WORDS) {
            if (length < WORD_CHARS) {
                (void)memcpy(out->word[out->count], start, length);
                out->word[out->count][length] = '\0';
            } else {
                out->too_long = true;
            }
        }

        out->count++;
    }
}

static bool find_channel(const char *name, uint32_t *channel)
{
    bool found = false;

    for (uint32_t i = 0U; (i < CH_COUNT) && !found; i++) {
        if (strcmp(name, CHANNEL_NAMES[i]) == 0) {
            *channel = i;
            found = true;
        }
    }

    return found;
}

/* The values a channel can report: its type's range. */
static void channel_range(uint32_t channel, long long *lo, long long *hi)
{
    if (channel == CH_CURRENT) {
        *lo = INT32_MIN;
        *hi = INT32_MAX;
    } else if (channel < CH_TEMP(0U)) {
        *lo = 0;
        *hi = UINT16_MAX;
    } else {
        *lo = INT16_MIN;
        *hi = INT16_MAX;
    }
}

/* A decimal integer within lo..hi with nothing after it. */
static bool parse_integer(const char *text, long long lo, long long hi, long long *out)
{
    char *end = NULL;
    bool ok = false;

    errno = 0;
    const long long value = strtoll(text, &end, 10);

    if ((errno == 0) && (end != text) && (*end == '\0') && (value >= lo) && (value <= hi)) {
        *out = value;
        ok = true;
    }

    return ok;
}

/* CLOCK_MONOTONIC, the clock of plat_time_ns(): the time a command took effect. */
static unsigned long long now_ns(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((unsigned long long)ts.tv_sec * 1000000000ULL) + (unsigned long long)ts.tv_nsec;
}

static bool unknown_channel(const char *name, char *reply, size_t reply_len)
{
    (void)snprintf(reply, reply_len, "unknown channel '%s' (current, cell1..cell8, temp1..temp4)",
                   name);
    return false;
}

/* The status: seed, off-line state, counters, then every channel as <name>=model, =open or
 * =<held value>, and last the hang and the reads that waited for one. */
static bool status_reply(char *reply, size_t reply_len)
{
    size_t used = 0U;
    int length = snprintf(reply, reply_len,
                          "seed=%lu offline=%s current_reads=%llu cell_reads=%llu temp_reads=%llu "
                          "unanswered=%llu",
                          (unsigned long)s_seed, atomic_load(&s_offline) ? "on" : "off",
                          (unsigned long long)atomic_load(&s_reads[GROUP_CURRENT]),
                          (unsigned long long)atomic_load(&s_reads[GROUP_CELLS]),
                          (unsigned long long)atomic_load(&s_reads[GROUP_TEMPS]),
                          (unsigned long long)atomic_load(&s_unanswered));
    bool fits = (length >= 0) && ((size_t)length < reply_len);

    for (uint32_t i = 0U; (i < CH_COUNT) && fits; i++) {
        const uint64_t word = atomic_load(&s_channels[i]);

        used += (size_t)length;

        if (word_mode(word) == MODE_HELD) {
            length = snprintf(&reply[used], reply_len - used, " %s=%ld", CHANNEL_NAMES[i],
                              (long)word_value(word));
        } else {
            length = snprintf(&reply[used], reply_len - used, " %s=%s", CHANNEL_NAMES[i],
                              (word_mode(word) == MODE_OPEN) ? "open" : "model");
        }

        fits = (length >= 0) && ((size_t)length < (reply_len - used));
    }

    if (fits) {
        const uint32_t hang = atomic_load(&s_hang);

        used += (size_t)length;
        length = snprintf(&reply[used], reply_len - used, " hang=%s hung=%llu",
                          HANG_NAMES[(hang <= HANG_SPIN) ? hang : HANG_OFF],
                          (unsigned long long)atomic_load(&s_hung));
        fits = (length >= 0) && ((size_t)length < (reply_len - used));
    }

    /* A reply cut short would lose the channels at its end without anyone noticing. */
    if (!fits) {
        (void)snprintf(reply, reply_len, "the status does not fit into a reply");
    }

    return fits;
}

/* Commands under "sensor.": status | set <channel> <value> | open <channel> | offline on|off |
 * hang on|spin|off | clear [<channel>]. Runs in the thread that serves simulation control. */
static bool sim_sensor_handler(const char *verb, const char *args, char *reply, size_t reply_len)
{
    words_t words;
    uint32_t channel = 0U;

    split_words(args, &words);

    if (strcmp(verb, "status") == 0) {
        return status_reply(reply, reply_len);
    }

    if (strcmp(verb, "set") == 0) {
        long long lo = 0;
        long long hi = 0;
        long long value = 0;

        if ((words.count != 2U) || words.too_long) {
            (void)snprintf(reply, reply_len, "usage: sensor.set <channel> <value>");
            return false;
        }

        if (!find_channel(words.word[0], &channel)) {
            return unknown_channel(words.word[0], reply, reply_len);
        }

        channel_range(channel, &lo, &hi);

        if (!parse_integer(words.word[1], lo, hi, &value)) {
            (void)snprintf(reply, reply_len,
                           "a value for %s is a whole number from %lld to %lld, "
                           "not '%s'",
                           CHANNEL_NAMES[channel], lo, hi, words.word[1]);
            return false;
        }

        atomic_store(&s_channels[channel], channel_word(MODE_HELD, (int32_t)value));
        (void)snprintf(reply, reply_len, "%s=%lld t_ns=%llu", CHANNEL_NAMES[channel], value,
                       now_ns());
        return true;
    }

    if (strcmp(verb, "open") == 0) {
        if ((words.count != 1U) || words.too_long) {
            (void)snprintf(reply, reply_len, "usage: sensor.open <channel>");
            return false;
        }

        if (!find_channel(words.word[0], &channel)) {
            return unknown_channel(words.word[0], reply, reply_len);
        }

        atomic_store(&s_channels[channel], channel_word(MODE_OPEN, 0));
        (void)snprintf(reply, reply_len, "%s=open t_ns=%llu", CHANNEL_NAMES[channel], now_ns());
        return true;
    }

    if (strcmp(verb, "offline") == 0) {
        const bool on = (words.count == 1U) && (strcmp(words.word[0], "on") == 0);
        const bool off = (words.count == 1U) && (strcmp(words.word[0], "off") == 0);

        if ((!on && !off) || words.too_long) {
            (void)snprintf(reply, reply_len, "usage: sensor.offline on|off");
            return false;
        }

        atomic_store(&s_offline, on);
        (void)snprintf(reply, reply_len, "offline=%s t_ns=%llu", on ? "on" : "off", now_ns());
        return true;
    }

    if (strcmp(verb, "hang") == 0) {
        uint32_t mode = HANG_SPIN + 1U;

        for (uint32_t m = 0U; (m <= HANG_SPIN) && (words.count == 1U) && !words.too_long; m++) {
            if (strcmp(words.word[0], HANG_NAMES[m]) == 0) {
                mode = m;
            }
        }

        if (mode > HANG_SPIN) {
            (void)snprintf(reply, reply_len, "usage: sensor.hang on|spin|off");
            return false;
        }

        atomic_store(&s_hang, mode);
        (void)snprintf(reply, reply_len, "hang=%s t_ns=%llu", HANG_NAMES[mode], now_ns());
        return true;
    }

    if (strcmp(verb, "clear") == 0) {
        if (words.too_long || (words.count > 1U)) {
            (void)snprintf(reply, reply_len, "usage: sensor.clear [<channel>]");
            return false;
        }

        if (words.count == 0U) {
            for (uint32_t i = 0U; i < CH_COUNT; i++) {
                atomic_store(&s_channels[i], channel_word(MODE_MODEL, 0));
            }

            atomic_store(&s_offline, false);
            atomic_store(&s_hang, HANG_OFF);
            (void)snprintf(reply, reply_len, "cleared=all offline=off hang=off t_ns=%llu",
                           now_ns());
            return true;
        }

        if (!find_channel(words.word[0], &channel)) {
            return unknown_channel(words.word[0], reply, reply_len);
        }

        atomic_store(&s_channels[channel], channel_word(MODE_MODEL, 0));
        (void)snprintf(reply, reply_len, "%s=model t_ns=%llu", CHANNEL_NAMES[channel], now_ns());
        return true;
    }

    (void)snprintf(reply, reply_len, "unknown command 'sensor.%s'", verb);
    return false;
}

/* ------------------------------------------------------------------------------- lifecycle */

void posix_sensor_start(uint32_t seed)
{
    s_seed = seed;

    for (uint32_t i = 0U; i < CH_COUNT; i++) {
        atomic_store(&s_channels[i], channel_word(MODE_MODEL, 0));
    }

    for (uint32_t i = 0U; i < GROUPS; i++) {
        atomic_store(&s_reads[i], 0U);
    }

    atomic_store(&s_unanswered, 0U);
    atomic_store(&s_offline, false);
    atomic_store(&s_hang, HANG_OFF);
    atomic_store(&s_hung, 0U);
    posix_sim_register("sensor.", sim_sensor_handler);
}

void posix_sensor_set_wait(posix_sensor_wait_fn wait)
{
    s_wait = wait;
}
