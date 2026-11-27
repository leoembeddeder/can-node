/* SPDX-License-Identifier: MIT */
/*
 * Tests of the sensor front-end model of the host build (platform/posix/posix_sensor.c): what
 * it produces, that the same seed gives the same readings, each fault and each reply of the
 * "sensor." commands, and the commands served from another thread while the test reads, as
 * the CAN hardware thread serves them in the node (scripts/test-tsan.sh runs this test under
 * ThreadSanitizer as well).
 *
 * The test plays both sides: it reads through plat_sensor.h, as the sensor task does, and sends
 * commands as datagrams from a client socket, then calls posix_sim_service() itself (or lets a
 * thread of its own do that, in the last test).
 *
 * The parameters of the model are written here as literals (the load profile, the cells'
 * open-circuit voltages and resistances, the temperatures, the noise amplitudes), so that a
 * changed model shows up here and in docs/sensor-model.md, which quotes the same numbers.
 * Readings are taken in the pattern of the application's sensor task: the current in every
 * period, the cells in every second one and the temperatures in every tenth (acquire()).
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bms/bms.h"
#include "plat_sensor.h"
#include "posix_sensor.h"
#include "posix_sim.h"
#include "unity.h"

#define PERIODS 4000U /* two cycles of the load profile */
#define SEED    42U

/* ------------------------------------------------------------------------- the model's numbers */

typedef struct {
    uint32_t steps;
    int32_t current_ma;
} segment_t;

static const segment_t PROFILE[] = {
    {300U, 2000}, {800U, 45000}, {150U, 160000}, {450U, 45000}, {300U, -80000}};
static const int32_t OCV_MV[BMS_CELLS] = {3702, 3698, 3705, 3695, 3700, 3703, 3697, 3701};
static const int32_t R_UOHM[BMS_CELLS] = {1800, 1900, 1750, 2100, 1850, 1950, 1800, 2000};
static const int32_t TEMP_DC[BMS_TEMPS] = {251, 248, 256, 245};
#define CURRENT_NOISE 150
#define CELL_NOISE    2
#define TEMP_NOISE    2

/* The profile's current at `step`, from the table above: 2000 steps per cycle. */
static int32_t profile_ma(uint32_t step)
{
    uint32_t left = step % 2000U;
    size_t i = 0U;

    while (left >= PROFILE[i].steps) {
        left -= PROFILE[i].steps;
        i++;
    }

    return PROFILE[i].current_ma;
}

/* What cell `cell` reads without noise at `current_ma`: OCV - I x R, rounded to the nearest mV,
 * a half away from zero (mA x micro-ohm = 1e-6 mV). */
static int32_t cell_model_mv(uint32_t cell, int32_t current_ma)
{
    const long long product = (long long)current_ma * R_UOHM[cell];
    const long long drop =
        (product >= 0) ? ((product + 500000) / 1000000) : -((500000 - product) / 1000000);

    return OCV_MV[cell] - (int32_t)drop;
}

/* --------------------------------------------------------------------------------- readings */

/* One period of the sensor task: which groups it read and what it got. The outputs are filled
 * with a sentinel before each read, so a read that writes nothing when it should not shows. */
typedef struct {
    bool current_read;
    bool cells_read;
    bool temps_read;
    bool current_answered;
    bool cells_answered;
    bool temps_answered;
    bool current_invalid;
    int32_t current_ma;
    uint16_t cell_mv[BMS_CELLS];
    uint8_t cell_invalid;
    int16_t temp_dc[BMS_TEMPS];
    uint8_t temp_invalid;
} period_t;

#define SENTINEL_I32 0x5A5A5A5A
#define SENTINEL_U16 0x5A5AU
#define SENTINEL_I16 0x5A5A
#define SENTINEL_U8  0x5AU

static period_t s_baseline[PERIODS];
static period_t s_run[PERIODS];

static void acquire(uint32_t period, period_t *out)
{
    (void)memset(out, 0, sizeof(*out));
    out->current_ma = SENTINEL_I32;
    out->current_invalid = true;
    out->cell_invalid = SENTINEL_U8;
    out->temp_invalid = SENTINEL_U8;

    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        out->cell_mv[i] = SENTINEL_U16;
    }

    for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
        out->temp_dc[i] = SENTINEL_I16;
    }

    out->current_read = true;
    out->current_answered = plat_sensor_read_current(&out->current_ma, &out->current_invalid);

    if ((period % 2U) == 0U) {
        out->cells_read = true;
        out->cells_answered = plat_sensor_read_cells(out->cell_mv, &out->cell_invalid);
    }

    if ((period % 10U) == 0U) {
        out->temps_read = true;
        out->temps_answered = plat_sensor_read_temps(out->temp_dc, &out->temp_invalid);
    }
}

static void run(period_t *periods, uint32_t from, uint32_t count)
{
    for (uint32_t p = from; p < (from + count); p++) {
        acquire(p, &periods[p]);
    }
}

/* True if two periods read the same groups and got the same answers. */
static bool same_period(const period_t *a, const period_t *b)
{
    return (a->current_read == b->current_read) && (a->cells_read == b->cells_read) &&
           (a->temps_read == b->temps_read) && (a->current_answered == b->current_answered) &&
           (a->cells_answered == b->cells_answered) && (a->temps_answered == b->temps_answered) &&
           (a->current_invalid == b->current_invalid) && (a->current_ma == b->current_ma) &&
           (memcmp(a->cell_mv, b->cell_mv, sizeof(a->cell_mv)) == 0) &&
           (a->cell_invalid == b->cell_invalid) &&
           (memcmp(a->temp_dc, b->temp_dc, sizeof(a->temp_dc)) == 0) &&
           (a->temp_invalid == b->temp_invalid);
}

/* Message for an assertion inside a loop: which case failed. */
static const char *at(const char *what, long value)
{
    static char text[64];

    (void)snprintf(text, sizeof(text), "%s %ld", what, value);
    return text;
}

/* ------------------------------------------------------------------------ simulation control */

static int s_client = -1;
static uint16_t s_port;
static char s_error[160];

static void send_request(const char *text)
{
    struct sockaddr_in to;

    (void)memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(s_port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT((int)strlen(text), (int)sendto(s_client, text, strlen(text), 0,
                                                         (const struct sockaddr *)&to, sizeof(to)));
}

/* The next reply, or NULL if none arrives within `timeout_ms`. */
static const char *receive_reply(int timeout_ms)
{
    static char reply[2U * POSIX_SIM_MAX_DATAGRAM];
    struct pollfd fd = {.fd = s_client, .events = POLLIN, .revents = 0};
    const char *result = NULL;

    if (poll(&fd, 1U, timeout_ms) > 0) {
        const ssize_t n = recv(s_client, reply, sizeof(reply) - 1U, 0);

        if (n >= 0) {
            reply[n] = '\0';
            result = reply;
        }
    }

    return result;
}

/* One request, served by this thread, and its reply; "<no reply>" if none came. */
static const char *exchange(const char *request)
{
    const char *reply;

    send_request(request);
    posix_sim_service();
    reply = receive_reply(200);
    return (reply != NULL) ? reply : "<no reply>";
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/* A command that succeeds with a reply of `expected` followed by " t_ns=<time>", where the time
 * is CLOCK_MONOTONIC at some moment of the exchange. */
static void expect_timed_ok(UNITY_LINE_TYPE line, const char *request, const char *expected)
{
    const uint64_t before = monotonic_ns();
    const char *reply = exchange(request);
    const uint64_t after = monotonic_ns();
    const size_t length = strlen(expected);
    const char *number = NULL;
    char *end = NULL;
    unsigned long long t_ns = 0ULL;

    /* Each assertion ends the test if it fails, so the next one only looks where the reply has
     * characters. */
    UNITY_TEST_ASSERT_EQUAL_STRING_LEN(expected, reply, length, line, request);
    UNITY_TEST_ASSERT_EQUAL_STRING_LEN(" t_ns=", &reply[length], 6U, line, request);
    number = &reply[length + 6U];
    t_ns = strtoull(number, &end, 10);
    UNITY_TEST_ASSERT((end != number) && (*end == '\0'), line, "the reply ends with t_ns=<number>");
    UNITY_TEST_ASSERT((t_ns >= before) && (t_ns <= after), line, "t_ns is the time of the command");
}

#define EXPECT_TIMED_OK(request, expected) expect_timed_ok(__LINE__, (request), (expected))

/* A reply that begins with `expected`. */
static void expect_prefix(UNITY_LINE_TYPE line, const char *request, const char *expected)
{
    UNITY_TEST_ASSERT_EQUAL_STRING_LEN(expected, exchange(request), strlen(expected), line,
                                       request);
}

#define EXPECT_PREFIX(request, expected) expect_prefix(__LINE__, (request), (expected))

/* Start the model from the beginning with `seed`. */
static void start(uint32_t seed)
{
    posix_sensor_start(seed);
}

void setUp(void)
{
    start(SEED);
}

void tearDown(void)
{
}

/* ---------------------------------------------------------------------------- determinism */

static void test_the_same_seed_gives_the_same_readings(void)
{
    uint32_t differing = 0U;

    start(SEED);
    run(s_baseline, 0U, PERIODS);
    start(SEED);
    run(s_run, 0U, PERIODS);

    for (uint32_t p = 0U; p < PERIODS; p++) {
        TEST_ASSERT_TRUE_MESSAGE(same_period(&s_baseline[p], &s_run[p]), at("period", p));
    }

    /* Another seed, other noise: most current readings differ (two equal draws from 151
     * values each are rare). */
    start(SEED + 1U);
    run(s_run, 0U, PERIODS);

    for (uint32_t p = 0U; p < PERIODS; p++) {
        if (s_run[p].current_ma != s_baseline[p].current_ma) {
            differing++;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(differing > (PERIODS * 9U / 10U), at("differing", differing));
}

static void test_a_fresh_model_reports_the_default_seed_and_no_fault(void)
{
    TEST_ASSERT_EQUAL_UINT32(1U, POSIX_SENSOR_DEFAULT_SEED);
    start(POSIX_SENSOR_DEFAULT_SEED);
    TEST_ASSERT_EQUAL_STRING(
        "f ok seed=1 offline=off current_reads=0 cell_reads=0 temp_reads=0 unanswered=0 "
        "current=model cell1=model cell2=model cell3=model cell4=model cell5=model cell6=model "
        "cell7=model cell8=model temp1=model temp2=model temp3=model temp4=model hang=off hung=0",
        exchange("f sensor.status"));

    start(4294967295U);
    EXPECT_PREFIX("g sensor.status", "g ok seed=4294967295 offline=off ");
}

/* ----------------------------------------------------------------------------- the readings */

static void test_the_current_follows_the_load_profile_with_a_little_noise(void)
{
    long long noise_sum = 0;
    int lowest = 0;
    int highest = 0;

    run(s_run, 0U, PERIODS);

    for (uint32_t p = 0U; p < PERIODS; p++) {
        const int noise = (int)(s_run[p].current_ma - profile_ma(p));

        TEST_ASSERT_TRUE(s_run[p].current_answered);
        TEST_ASSERT_FALSE(s_run[p].current_invalid);
        TEST_ASSERT_TRUE_MESSAGE((noise >= -CURRENT_NOISE) && (noise <= CURRENT_NOISE),
                                 at("period", p));
        TEST_ASSERT_TRUE(bms_current_plausible(s_run[p].current_ma));
        noise_sum += noise;
        lowest = (noise < lowest) ? noise : lowest;
        highest = (noise > highest) ? noise : highest;
    }

    /* Noise there is, centred on 0: with this seed it spans most of +-150 mA, and its mean
     * over 4000 readings stays within 10 mA (its standard deviation per reading is about
     * 150 / sqrt(6) = 61 mA, so the mean's is about 1 mA). */
    TEST_ASSERT_TRUE_MESSAGE(lowest <= -100, at("lowest", lowest));
    TEST_ASSERT_TRUE_MESSAGE(highest >= 100, at("highest", highest));
    TEST_ASSERT_TRUE_MESSAGE((noise_sum > -40000) && (noise_sum < 40000), at("sum", noise_sum));

    /* The boundaries of the profile, in readings: 300, 1100, 1250, 1700, and 2000 = 0 again. */
    TEST_ASSERT_INT32_WITHIN(150, 2000, s_run[299].current_ma);
    TEST_ASSERT_INT32_WITHIN(150, 45000, s_run[300].current_ma);
    TEST_ASSERT_INT32_WITHIN(150, 45000, s_run[1099].current_ma);
    TEST_ASSERT_INT32_WITHIN(150, 160000, s_run[1100].current_ma);
    TEST_ASSERT_INT32_WITHIN(150, 160000, s_run[1249].current_ma);
    TEST_ASSERT_INT32_WITHIN(150, 45000, s_run[1250].current_ma);
    TEST_ASSERT_INT32_WITHIN(150, -80000, s_run[1700].current_ma);
    TEST_ASSERT_INT32_WITHIN(150, -80000, s_run[1999].current_ma);
    TEST_ASSERT_INT32_WITHIN(150, 2000, s_run[2000].current_ma);
}

/* The cells see the true current of the latest current reading, which the sensor task takes
 * just before them in the same period (period p is current reading p). Every noise value from
 * -2 to +2 mV, and from -0.2 to +0.2 degC, occurs. */
static void test_the_cells_follow_the_current_through_their_resistance(void)
{
    uint32_t cell_noise_seen[5] = {0U};
    uint32_t temp_noise_seen[5] = {0U};

    run(s_run, 0U, PERIODS);

    for (uint32_t p = 0U; p < PERIODS; p++) {
        if (s_run[p].cells_read) {
            TEST_ASSERT_TRUE(s_run[p].cells_answered);
            TEST_ASSERT_EQUAL_HEX8(0U, s_run[p].cell_invalid);

            for (uint32_t i = 0U; i < BMS_CELLS; i++) {
                const int noise = (int)s_run[p].cell_mv[i] - cell_model_mv(i, profile_ma(p));

                TEST_ASSERT_TRUE_MESSAGE((noise >= -CELL_NOISE) && (noise <= CELL_NOISE),
                                         at("period", p));
                TEST_ASSERT_TRUE(bms_cell_plausible(s_run[p].cell_mv[i]));
                cell_noise_seen[noise + CELL_NOISE]++;
            }
        }

        if (s_run[p].temps_read) {
            TEST_ASSERT_TRUE(s_run[p].temps_answered);
            TEST_ASSERT_EQUAL_HEX8(0U, s_run[p].temp_invalid);

            for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
                const int noise = (int)s_run[p].temp_dc[i] - TEMP_DC[i];

                TEST_ASSERT_TRUE_MESSAGE((noise >= -TEMP_NOISE) && (noise <= TEMP_NOISE),
                                         at("period", p));
                TEST_ASSERT_TRUE(bms_temp_plausible(s_run[p].temp_dc[i]));
                temp_noise_seen[noise + TEMP_NOISE]++;
            }
        }
    }

    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE_MESSAGE(cell_noise_seen[i] > 0U, at("cell noise never", i - 2));
        TEST_ASSERT_TRUE_MESSAGE(temp_noise_seen[i] > 0U, at("temperature noise never", i - 2));
    }

    /* Triangular: 0 is the most frequent value, +-2 the least (1/9 of 16000 readings against
     * 3/9, give or take a little). */
    TEST_ASSERT_TRUE(cell_noise_seen[2] > (2U * cell_noise_seen[0]));
    TEST_ASSERT_TRUE(cell_noise_seen[2] > (2U * cell_noise_seen[4]));

    /* Under the peak current, cell 4 (2100 micro-ohm) drops 336 mV from its 3695 mV; while
     * charging at 80 A, cell 3 (3705 mV, 1750 micro-ohm) is 140 mV up. */
    TEST_ASSERT_INT32_WITHIN(2, 3695 - 336, s_run[1200].cell_mv[3]);
    TEST_ASSERT_INT32_WITHIN(2, 3705 + 140, s_run[1800].cell_mv[2]);
}

/* Each channel has noise of its own: two cells read at the same time show the same noise in
 * about 19 of 81 readings (the chance that two independent triangular draws from -2..2 agree),
 * not in all of them. */
static void test_every_channel_has_noise_of_its_own(void)
{
    uint32_t equal[BMS_CELLS] = {0U};
    uint32_t readings = 0U;

    run(s_run, 0U, PERIODS);

    for (uint32_t p = 0U; p < PERIODS; p += 2U) {
        const int first = (int)s_run[p].cell_mv[0] - cell_model_mv(0U, profile_ma(p));

        for (uint32_t i = 1U; i < BMS_CELLS; i++) {
            if (((int)s_run[p].cell_mv[i] - cell_model_mv(i, profile_ma(p))) == first) {
                equal[i]++;
            }
        }

        readings++;
    }

    for (uint32_t i = 1U; i < BMS_CELLS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(equal[i] < (readings / 2U), at("cell", (long)i + 1L));
    }
}

/* The cells see the profile at the latest current reading. The profile changes at readings 300,
 * 1100, 1250, 1700 and 2000 (the next cycle): cells read right after current reading 299 see
 * the 2 A of rest, right after reading 300 the 45 A of discharge, and so on. */
static void test_the_cells_see_the_current_of_the_latest_current_reading(void)
{
    static const uint32_t boundaries[] = {300U, 1100U, 1250U, 1700U, 2000U};
    uint32_t taken = 0U;

    for (size_t b = 0U; b < (sizeof(boundaries) / sizeof(boundaries[0])); b++) {
        for (uint32_t step = boundaries[b] - 1U; step <= boundaries[b]; step++) {
            int32_t current = 0;
            bool invalid = false;
            uint16_t cells[BMS_CELLS];
            uint8_t mask = SENTINEL_U8;

            while (taken <= step) {
                TEST_ASSERT_TRUE(plat_sensor_read_current(&current, &invalid));
                taken++;
            }

            TEST_ASSERT_TRUE(plat_sensor_read_cells(cells, &mask));

            for (uint32_t i = 0U; i < BMS_CELLS; i++) {
                TEST_ASSERT_INT32_WITHIN_MESSAGE(CELL_NOISE, cell_model_mv(i, profile_ma(step)),
                                                 cells[i], at("after current reading", step));
            }
        }
    }
}

static void test_before_the_first_current_reading_the_cells_see_the_first_step(void)
{
    uint16_t cells[BMS_CELLS];
    uint8_t invalid = SENTINEL_U8;

    TEST_ASSERT_TRUE(plat_sensor_read_cells(cells, &invalid));
    TEST_ASSERT_EQUAL_HEX8(0U, invalid);

    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        TEST_ASSERT_INT32_WITHIN(CELL_NOISE, cell_model_mv(i, 2000), cells[i]);
    }
}

/* ---------------------------------------------------------------------------------- faults */

/* Everything except one channel of a run must be what the baseline run read. */
static void expect_baseline_except(uint32_t from, uint32_t count, int cell, int temp, bool current)
{
    for (uint32_t p = from; p < (from + count); p++) {
        const period_t *b = &s_baseline[p];
        const period_t *r = &s_run[p];

        if (!current) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(b->current_ma, r->current_ma, at("period", p));
            TEST_ASSERT_EQUAL_MESSAGE(b->current_invalid, r->current_invalid, at("period", p));
        }

        if (r->cells_read) {
            for (int i = 0; i < (int)BMS_CELLS; i++) {
                if (i != cell) {
                    TEST_ASSERT_EQUAL_UINT16_MESSAGE(b->cell_mv[i], r->cell_mv[i], at("period", p));
                }
            }
        }

        if (r->temps_read) {
            for (int i = 0; i < (int)BMS_TEMPS; i++) {
                if (i != temp) {
                    TEST_ASSERT_EQUAL_INT16_MESSAGE(b->temp_dc[i], r->temp_dc[i], at("period", p));
                }
            }
        }
    }
}

static void record_baseline(void)
{
    start(SEED);
    run(s_baseline, 0U, PERIODS);
    start(SEED);
}

static void test_a_held_channel_reads_exactly_its_value_and_nothing_else_changes(void)
{
    record_baseline();
    EXPECT_TIMED_OK("h1 sensor.set cell3 4321", "h1 ok cell3=4321");
    run(s_run, 0U, 200U);

    for (uint32_t p = 0U; p < 200U; p += 2U) {
        TEST_ASSERT_EQUAL_UINT16(4321U, s_run[p].cell_mv[2]);
        TEST_ASSERT_EQUAL_HEX8(0U, s_run[p].cell_invalid);
    }

    expect_baseline_except(0U, 200U, 2, -1, false);

    /* Cleared, the cell reads what it would have read without the fault. */
    EXPECT_TIMED_OK("h2 sensor.clear cell3", "h2 ok cell3=model");
    run(s_run, 200U, 100U);
    expect_baseline_except(200U, 100U, -1, -1, false);

    /* Values outside what the sensor can deliver are held as well: that is what the
     * application's plausibility check is for. Holding the current changes its reading only;
     * the cells keep following the true current (they equal the baseline). */
    EXPECT_TIMED_OK("h3 sensor.set current -2147483648", "h3 ok current=-2147483648");
    EXPECT_TIMED_OK("h4 sensor.set temp4 32767", "h4 ok temp4=32767");
    EXPECT_TIMED_OK("h5 sensor.set cell8 65535", "h5 ok cell8=65535");
    EXPECT_TIMED_OK("h6 sensor.set temp1 -32768", "h6 ok temp1=-32768");
    run(s_run, 300U, 100U);

    for (uint32_t p = 300U; p < 400U; p++) {
        TEST_ASSERT_EQUAL_INT32(INT32_MIN, s_run[p].current_ma);
        TEST_ASSERT_FALSE(s_run[p].current_invalid);

        if (s_run[p].cells_read) {
            TEST_ASSERT_EQUAL_UINT16(65535U, s_run[p].cell_mv[7]);
            TEST_ASSERT_EQUAL_MEMORY(s_baseline[p].cell_mv, s_run[p].cell_mv, 7U * 2U);
        }

        if (s_run[p].temps_read) {
            TEST_ASSERT_EQUAL_INT16(32767, s_run[p].temp_dc[3]);
            TEST_ASSERT_EQUAL_INT16(-32768, s_run[p].temp_dc[0]);
            TEST_ASSERT_EQUAL_HEX8(0U, s_run[p].temp_invalid);
        }
    }

    /* A new value replaces the held one. */
    EXPECT_TIMED_OK("h7 sensor.set current 300001", "h7 ok current=300001");
    run(s_run, 400U, 1U);
    TEST_ASSERT_EQUAL_INT32(300001, s_run[400].current_ma);
}

static void test_an_open_channel_is_flagged_and_reports_the_model_value(void)
{
    record_baseline();
    EXPECT_TIMED_OK("o1 sensor.open temp2", "o1 ok temp2=open");
    EXPECT_TIMED_OK("o2 sensor.open cell8", "o2 ok cell8=open");
    EXPECT_TIMED_OK("o3 sensor.open current", "o3 ok current=open");
    run(s_run, 0U, 100U);

    for (uint32_t p = 0U; p < 100U; p++) {
        TEST_ASSERT_TRUE(s_run[p].current_answered);
        TEST_ASSERT_TRUE(s_run[p].current_invalid);
        TEST_ASSERT_EQUAL_INT32(s_baseline[p].current_ma, s_run[p].current_ma);

        if (s_run[p].cells_read) {
            TEST_ASSERT_EQUAL_HEX8(0x80U, s_run[p].cell_invalid);
            TEST_ASSERT_EQUAL_MEMORY(s_baseline[p].cell_mv, s_run[p].cell_mv,
                                     sizeof(s_run[p].cell_mv));
        }

        if (s_run[p].temps_read) {
            TEST_ASSERT_EQUAL_HEX8(0x02U, s_run[p].temp_invalid);
            TEST_ASSERT_EQUAL_MEMORY(s_baseline[p].temp_dc, s_run[p].temp_dc,
                                     sizeof(s_run[p].temp_dc));
        }
    }

    /* An open channel that is then held is held, and the other way round. */
    EXPECT_TIMED_OK("o4 sensor.set temp2 100", "o4 ok temp2=100");
    EXPECT_TIMED_OK("o5 sensor.open cell8", "o5 ok cell8=open");
    run(s_run, 100U, 11U);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_run[110].temp_invalid);
    TEST_ASSERT_EQUAL_INT16(100, s_run[110].temp_dc[1]);
    TEST_ASSERT_EQUAL_HEX8(0x80U, s_run[110].cell_invalid);
}

static void test_an_offline_front_end_answers_no_read_and_writes_nothing(void)
{
    record_baseline();
    run(s_run, 0U, 50U);
    EXPECT_TIMED_OK("x1 sensor.offline on", "x1 ok offline=on");
    run(s_run, 50U, 50U);

    for (uint32_t p = 50U; p < 100U; p++) {
        TEST_ASSERT_FALSE(s_run[p].current_answered);
        TEST_ASSERT_EQUAL_INT32(SENTINEL_I32, s_run[p].current_ma);
        TEST_ASSERT_TRUE(s_run[p].current_invalid); /* the sentinel: untouched */

        if (s_run[p].cells_read) {
            TEST_ASSERT_FALSE(s_run[p].cells_answered);
            TEST_ASSERT_EQUAL_UINT16(SENTINEL_U16, s_run[p].cell_mv[0]);
            TEST_ASSERT_EQUAL_UINT16(SENTINEL_U16, s_run[p].cell_mv[7]);
            TEST_ASSERT_EQUAL_HEX8(SENTINEL_U8, s_run[p].cell_invalid);
        }

        if (s_run[p].temps_read) {
            TEST_ASSERT_FALSE(s_run[p].temps_answered);
            TEST_ASSERT_EQUAL_INT16(SENTINEL_I16, s_run[p].temp_dc[0]);
            TEST_ASSERT_EQUAL_HEX8(SENTINEL_U8, s_run[p].temp_invalid);
        }
    }

    /* Every read counted, answered or not: 100 current, 50 cell and 10 temperature readings,
     * of which 50 + 25 + 5 found the front end off line. */
    EXPECT_PREFIX("x2 sensor.status", "x2 ok seed=42 offline=on current_reads=100 cell_reads=50 "
                                      "temp_reads=10 unanswered=80 current=model ");

    /* Back on line, the readings are those of a run that never went off line: the model ran
     * on in the meantime. */
    EXPECT_TIMED_OK("x3 sensor.offline off", "x3 ok offline=off");
    run(s_run, 100U, 100U);

    for (uint32_t p = 100U; p < 200U; p++) {
        TEST_ASSERT_TRUE_MESSAGE(same_period(&s_baseline[p], &s_run[p]), at("period", p));
    }
}

static void test_clear_ends_every_fault_at_once(void)
{
    record_baseline();
    EXPECT_TIMED_OK("c1 sensor.set cell1 1", "c1 ok cell1=1");
    EXPECT_TIMED_OK("c2 sensor.open temp3", "c2 ok temp3=open");
    EXPECT_TIMED_OK("c3 sensor.set current 5", "c3 ok current=5");
    EXPECT_TIMED_OK("c4 sensor.offline on", "c4 ok offline=on");
    run(s_run, 0U, 20U);
    EXPECT_TIMED_OK("c5 sensor.clear", "c5 ok cleared=all offline=off hang=off");
    run(s_run, 20U, 100U);

    for (uint32_t p = 20U; p < 120U; p++) {
        TEST_ASSERT_TRUE_MESSAGE(same_period(&s_baseline[p], &s_run[p]), at("period", p));
    }

    /* The 20 periods off line left 20 + 10 + 2 readings unanswered. */
    TEST_ASSERT_EQUAL_STRING(
        "c6 ok seed=42 offline=off current_reads=120 cell_reads=60 temp_reads=12 unanswered=32 "
        "current=model cell1=model cell2=model cell3=model cell4=model cell5=model cell6=model "
        "cell7=model cell8=model temp1=model temp2=model temp3=model temp4=model hang=off hung=0",
        exchange("c6 sensor.status"));
}

/* ------------------------------------------------------------------------------- the replies */

static void test_the_status_names_every_counter_and_every_channel(void)
{
    int32_t current;
    bool invalid;
    uint16_t cells[BMS_CELLS];
    uint8_t mask;

    (void)plat_sensor_read_current(&current, &invalid);
    (void)plat_sensor_read_current(&current, &invalid);
    (void)plat_sensor_read_cells(cells, &mask);
    (void)exchange("s1 sensor.set cell2 4300");
    (void)exchange("s2 sensor.set temp1 -123");
    (void)exchange("s3 sensor.open temp4");
    (void)exchange("s4 sensor.set current -7");
    (void)exchange("s5 sensor.offline on");
    (void)plat_sensor_read_cells(cells, &mask);

    TEST_ASSERT_EQUAL_STRING(
        "s6 ok seed=42 offline=on current_reads=2 cell_reads=2 temp_reads=0 unanswered=1 "
        "current=-7 cell1=model cell2=4300 cell3=model cell4=model cell5=model cell6=model "
        "cell7=model cell8=model temp1=-123 temp2=model temp3=model temp4=open hang=off hung=0",
        exchange("s6 sensor.status"));

    /* Arguments after "status" are ignored, as by can.status. */
    EXPECT_PREFIX("s7 sensor.status now", "s7 ok seed=42 offline=on ");
}

static void test_every_command_answers_with_the_state_it_set(void)
{
    EXPECT_TIMED_OK("a1 sensor.set cell1 0", "a1 ok cell1=0");
    EXPECT_TIMED_OK("a2 sensor.set cell8 65535", "a2 ok cell8=65535");
    EXPECT_TIMED_OK("a3 sensor.set temp1 -32768", "a3 ok temp1=-32768");
    EXPECT_TIMED_OK("a4 sensor.set temp4 32767", "a4 ok temp4=32767");
    EXPECT_TIMED_OK("a5 sensor.set current 2147483647", "a5 ok current=2147483647");
    EXPECT_TIMED_OK("a6 sensor.set current +12", "a6 ok current=12");
    EXPECT_TIMED_OK("a7   sensor.set    cell5    3333  ", "a7 ok cell5=3333");
    EXPECT_TIMED_OK("a8 sensor.open cell5", "a8 ok cell5=open");
    EXPECT_TIMED_OK("a9 sensor.clear cell5", "a9 ok cell5=model");
    EXPECT_TIMED_OK("b1 sensor.offline on", "b1 ok offline=on");
    EXPECT_TIMED_OK("b2 sensor.offline off", "b2 ok offline=off");
    EXPECT_TIMED_OK("b3 sensor.clear", "b3 ok cleared=all offline=off hang=off");
}

static void test_a_wrong_command_is_refused_with_what_was_wrong(void)
{
    char request[96];

    TEST_ASSERT_EQUAL_STRING("e1 err usage: sensor.set <channel> <value>",
                             exchange("e1 sensor.set"));
    TEST_ASSERT_EQUAL_STRING("e2 err usage: sensor.set <channel> <value>",
                             exchange("e2 sensor.set cell1"));
    TEST_ASSERT_EQUAL_STRING("e3 err usage: sensor.set <channel> <value>",
                             exchange("e3 sensor.set cell1 1 2"));
    TEST_ASSERT_EQUAL_STRING("e4 err unknown channel 'cell9' (current, cell1..cell8, temp1..temp4)",
                             exchange("e4 sensor.set cell9 1"));
    TEST_ASSERT_EQUAL_STRING("e5 err unknown channel 'cell0' (current, cell1..cell8, temp1..temp4)",
                             exchange("e5 sensor.set cell0 1"));
    TEST_ASSERT_EQUAL_STRING("e6 err unknown channel 'Cell1' (current, cell1..cell8, temp1..temp4)",
                             exchange("e6 sensor.set Cell1 1"));
    TEST_ASSERT_EQUAL_STRING("e7 err a value for cell1 is a whole number from 0 to 65535, not "
                             "'65536'",
                             exchange("e7 sensor.set cell1 65536"));
    TEST_ASSERT_EQUAL_STRING("e8 err a value for cell1 is a whole number from 0 to 65535, not '-1'",
                             exchange("e8 sensor.set cell1 -1"));
    TEST_ASSERT_EQUAL_STRING("e9 err a value for temp3 is a whole number from -32768 to 32767, "
                             "not '32768'",
                             exchange("e9 sensor.set temp3 32768"));
    TEST_ASSERT_EQUAL_STRING("f1 err a value for temp3 is a whole number from -32768 to 32767, "
                             "not '-32769'",
                             exchange("f1 sensor.set temp3 -32769"));
    TEST_ASSERT_EQUAL_STRING("f2 err a value for current is a whole number from -2147483648 to "
                             "2147483647, not '2147483648'",
                             exchange("f2 sensor.set current 2147483648"));
    TEST_ASSERT_EQUAL_STRING("f3 err a value for current is a whole number from -2147483648 to "
                             "2147483647, not '-2147483649'",
                             exchange("f3 sensor.set current -2147483649"));
    TEST_ASSERT_EQUAL_STRING("f4 err a value for cell2 is a whole number from 0 to 65535, not "
                             "'12x'",
                             exchange("f4 sensor.set cell2 12x"));
    TEST_ASSERT_EQUAL_STRING("f5 err a value for cell2 is a whole number from 0 to 65535, not "
                             "'3.7'",
                             exchange("f5 sensor.set cell2 3.7"));
    TEST_ASSERT_EQUAL_STRING("f6 err a value for cell2 is a whole number from 0 to 65535, not "
                             "'-'",
                             exchange("f6 sensor.set cell2 -"));
    /* A number too large even for strtoll's range. */
    TEST_ASSERT_EQUAL_STRING("f7 err a value for current is a whole number from -2147483648 to "
                             "2147483647, not '99999999999999999999'",
                             exchange("f7 sensor.set current 99999999999999999999"));

    TEST_ASSERT_EQUAL_STRING("g1 err usage: sensor.open <channel>", exchange("g1 sensor.open"));
    TEST_ASSERT_EQUAL_STRING("g2 err usage: sensor.open <channel>",
                             exchange("g2 sensor.open cell1 cell2"));
    TEST_ASSERT_EQUAL_STRING(
        "g3 err unknown channel 'voltage' (current, cell1..cell8, temp1..temp4)",
        exchange("g3 sensor.open voltage"));
    TEST_ASSERT_EQUAL_STRING("g4 err usage: sensor.offline on|off", exchange("g4 sensor.offline"));
    TEST_ASSERT_EQUAL_STRING("g5 err usage: sensor.offline on|off",
                             exchange("g5 sensor.offline yes"));
    TEST_ASSERT_EQUAL_STRING("g6 err usage: sensor.offline on|off",
                             exchange("g6 sensor.offline on off"));
    TEST_ASSERT_EQUAL_STRING("g7 err usage: sensor.clear [<channel>]",
                             exchange("g7 sensor.clear cell1 cell2"));
    TEST_ASSERT_EQUAL_STRING("g8 err unknown channel 'temp5' (current, cell1..cell8, temp1..temp4)",
                             exchange("g8 sensor.clear temp5"));
    TEST_ASSERT_EQUAL_STRING("g9 err unknown command 'sensor.reset'", exchange("g9 sensor.reset"));
    TEST_ASSERT_EQUAL_STRING("h1 err unknown command 'sensor.'", exchange("h1 sensor."));

    /* A word of 24 characters or more is not taken apart: the request is a usage error. */
    (void)snprintf(request, sizeof(request), "h2 sensor.set cell1 %s", "000000000000000000000001");
    TEST_ASSERT_EQUAL_STRING("h2 err usage: sensor.set <channel> <value>", exchange(request));
    (void)snprintf(request, sizeof(request), "h3 sensor.open %s", "cell1xxxxxxxxxxxxxxxxxxxx");
    TEST_ASSERT_EQUAL_STRING("h3 err usage: sensor.open <channel>", exchange(request));
    (void)snprintf(request, sizeof(request), "h4 sensor.clear %s", "cell1xxxxxxxxxxxxxxxxxxxx");
    TEST_ASSERT_EQUAL_STRING("h4 err usage: sensor.clear [<channel>]", exchange(request));
    (void)snprintf(request, sizeof(request), "h5 sensor.offline %s", "onnnnnnnnnnnnnnnnnnnnnnnn");
    TEST_ASSERT_EQUAL_STRING("h5 err usage: sensor.offline on|off", exchange(request));
    /* 23 characters still are: */
    EXPECT_TIMED_OK("h6 sensor.set cell1 00000000000000000000001", "h6 ok cell1=1");

    /* None of the refused commands changed anything but the last one, which set cell1. */
    TEST_ASSERT_EQUAL_STRING(
        "h7 ok seed=42 offline=off current_reads=0 cell_reads=0 temp_reads=0 unanswered=0 "
        "current=model cell1=1 cell2=model cell3=model cell4=model cell5=model cell6=model "
        "cell7=model cell8=model temp1=model temp2=model temp3=model temp4=model hang=off hung=0",
        exchange("h7 sensor.status"));
}

/* --------------------------------------------------------------------------------- the hang */

/* A reader of its own: one current reading, as the sensor task would take it, while this
 * thread plays the bench. */
typedef struct {
    atomic_bool done;
    bool answered;
    bool invalid;
    int32_t current_ma;
} hang_reader_t;

static hang_reader_t s_reader;
static atomic_uint s_waits; /* calls of the wait function of "sensor.hang on" */

static void counting_wait(void)
{
    const struct timespec pause = {0, 1000000L};

    (void)atomic_fetch_add(&s_waits, 1U);
    (void)nanosleep(&pause, NULL);
}

static void *read_once(void *arg)
{
    hang_reader_t *reader = arg;

    reader->answered = plat_sensor_read_current(&reader->current_ma, &reader->invalid);
    atomic_store(&reader->done, true);
    return NULL;
}

/* The value of `key`=<number> in the status reply, or -1. */
static long status_number(const char *key)
{
    const char *reply = exchange("q sensor.status");
    char pattern[32];
    const char *at_key;

    (void)snprintf(pattern, sizeof(pattern), " %s=", key);
    at_key = strstr(reply, pattern);
    return (at_key != NULL) ? strtol(at_key + strlen(pattern), NULL, 10) : -1L;
}

/* Wait (up to 5 s) until the status says that a read is waiting for the hang to end. */
static void wait_until_hung(long count)
{
    const struct timespec pause = {0, 1000000L};

    for (uint32_t i = 0U; (i < 5000U) && (status_number("hung") < count); i++) {
        (void)nanosleep(&pause, NULL);
    }

    TEST_ASSERT_EQUAL_INT32_MESSAGE(count, status_number("hung"), "the reader is waiting");
}

/* Join the reader once the hang has been ended. It returns within 5 s if the end of the hang
 * reached it; a defect that keeps it waiting fails here instead of hanging the test (the
 * reader is then left behind, blocked, until the process ends). */
static void join_reader(pthread_t thread)
{
    const struct timespec pause = {0, 1000000L};

    for (uint32_t i = 0U; (i < 5000U) && !atomic_load(&s_reader.done); i++) {
        (void)nanosleep(&pause, NULL);
    }

    if (!atomic_load(&s_reader.done)) {
        TEST_FAIL_MESSAGE("the read did not return after the hang ended");
    }

    TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
}

/* "on" and "spin": a read that starts while the front end hangs does not return, takes no
 * reading and is counted in `hung`; "on" waits through the wait function, "spin" never calls
 * it. Once the hang ends, the read returns the very reading a run without the hang would have
 * given first: the model has no clock, so nothing was skipped. */
static void hang_holds_the_read(const char *mode, bool calls_wait)
{
    const struct timespec settle = {0, 30000000L};
    pthread_t thread;
    char request[48];
    char expected[48];

    record_baseline();
    posix_sensor_set_wait(counting_wait);
    atomic_store(&s_waits, 0U);
    (void)memset(&s_reader, 0, sizeof(s_reader));
    atomic_store(&s_reader.done, false);

    (void)snprintf(request, sizeof(request), "k1 sensor.hang %s", mode);
    (void)snprintf(expected, sizeof(expected), "k1 ok hang=%s", mode);
    EXPECT_TIMED_OK(request, expected);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, read_once, &s_reader));
    wait_until_hung(1L);

    /* Held: nothing returned and nothing read 30 ms later, the hang shown in the status. */
    (void)nanosleep(&settle, NULL);
    TEST_ASSERT_FALSE(atomic_load(&s_reader.done));
    TEST_ASSERT_EQUAL_INT32(0, status_number("current_reads"));
    (void)snprintf(expected, sizeof(expected), " hang=%s hung=1", mode);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(exchange("k2 sensor.status"), expected), expected);

    if (calls_wait) {
        TEST_ASSERT_GREATER_THAN_UINT32(0U, atomic_load(&s_waits));
    } else {
        TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_waits));
    }

    EXPECT_TIMED_OK("k3 sensor.hang off", "k3 ok hang=off");
    join_reader(thread);
    TEST_ASSERT_TRUE(atomic_load(&s_reader.done));
    TEST_ASSERT_TRUE(s_reader.answered);
    TEST_ASSERT_FALSE(s_reader.invalid);
    TEST_ASSERT_EQUAL_INT32(s_baseline[0].current_ma, s_reader.current_ma);
    TEST_ASSERT_EQUAL_INT32(1, status_number("current_reads"));
    TEST_ASSERT_EQUAL_INT32(1, status_number("hung"));

    /* A read after the hang returns at once, and is not counted as hung. */
    period_t next;

    acquire(1U, &next);
    TEST_ASSERT_TRUE(same_period(&s_baseline[1], &next));
    TEST_ASSERT_EQUAL_INT32(1, status_number("hung"));
    posix_sensor_set_wait(NULL);
}

static void test_a_hanging_front_end_blocks_the_read_until_it_is_released(void)
{
    hang_holds_the_read("on", true);
}

static void test_a_spinning_front_end_holds_the_read_without_the_wait_function(void)
{
    hang_holds_the_read("spin", false);
}

/* Without a wait function, "on" sleeps the thread, as in this test without a kernel. */
static void test_without_a_wait_function_a_hang_still_holds_the_read(void)
{
    pthread_t thread;

    posix_sensor_set_wait(NULL);
    (void)memset(&s_reader, 0, sizeof(s_reader));
    atomic_store(&s_reader.done, false);
    EXPECT_TIMED_OK("w1 sensor.hang on", "w1 ok hang=on");
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, read_once, &s_reader));
    wait_until_hung(1L);
    TEST_ASSERT_FALSE(atomic_load(&s_reader.done));
    /* sensor.clear ends the hang with every other fault. */
    EXPECT_TIMED_OK("w2 sensor.clear", "w2 ok cleared=all offline=off hang=off");
    join_reader(thread);
    TEST_ASSERT_TRUE(s_reader.answered);
}

/* A hang does not change what the front end answers once it ends: off line before the hang
 * ends is off line after it. */
static void test_the_hang_and_the_offline_state_are_independent(void)
{
    pthread_t thread;

    (void)memset(&s_reader, 0, sizeof(s_reader));
    atomic_store(&s_reader.done, false);
    EXPECT_TIMED_OK("v1 sensor.hang spin", "v1 ok hang=spin");
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, read_once, &s_reader));
    wait_until_hung(1L);
    EXPECT_TIMED_OK("v2 sensor.offline on", "v2 ok offline=on");
    EXPECT_TIMED_OK("v3 sensor.hang off", "v3 ok hang=off");
    join_reader(thread);
    TEST_ASSERT_FALSE(s_reader.answered);
    TEST_ASSERT_EQUAL_INT32(1, status_number("unanswered"));
}

static void test_a_wrong_hang_command_is_refused(void)
{
    char request[96];

    TEST_ASSERT_EQUAL_STRING("j1 err usage: sensor.hang on|spin|off", exchange("j1 sensor.hang"));
    TEST_ASSERT_EQUAL_STRING("j2 err usage: sensor.hang on|spin|off",
                             exchange("j2 sensor.hang yes"));
    TEST_ASSERT_EQUAL_STRING("j3 err usage: sensor.hang on|spin|off",
                             exchange("j3 sensor.hang on off"));
    TEST_ASSERT_EQUAL_STRING("j4 err usage: sensor.hang on|spin|off",
                             exchange("j4 sensor.hang ON"));
    (void)snprintf(request, sizeof(request), "j5 sensor.hang %s", "onnnnnnnnnnnnnnnnnnnnnnnn");
    TEST_ASSERT_EQUAL_STRING("j5 err usage: sensor.hang on|spin|off", exchange(request));
    /* Nothing changed; then a hang without a reader is shown, and no read counted as hung. */
    EXPECT_PREFIX("j6 sensor.status", "j6 ok seed=42 offline=off ");
    TEST_ASSERT_NOT_NULL(strstr(exchange("j7 sensor.status"), " hang=off hung=0"));
    EXPECT_TIMED_OK("j8 sensor.hang spin", "j8 ok hang=spin");
    TEST_ASSERT_NOT_NULL(strstr(exchange("j9 sensor.status"), " hang=spin hung=0"));
    EXPECT_TIMED_OK("k9 sensor.hang off", "k9 ok hang=off");
}

/* ------------------------------------------------------------------- from another thread */

static atomic_bool s_serving;

/* The hardware thread's part: serve simulation control until told to stop. */
static void *serve(void *arg)
{
    struct pollfd fd = {.fd = posix_sim_fd(), .events = POLLIN, .revents = 0};

    (void)arg;

    while (atomic_load(&s_serving)) {
        (void)poll(&fd, 1U, 1);
        posix_sim_service();
    }

    return NULL;
}

typedef enum { SEEN_MODEL, SEEN_HELD, SEEN_OPEN, SEEN_OFFLINE } seen_t;

/* What one reading of the cells says about cell 5: held values are 1000..1499, far from what
 * the model gives (3.3 to 3.9 V), so the four cases cannot be confused. */
static seen_t look_at_cell5(uint16_t *value)
{
    uint16_t cells[BMS_CELLS];
    uint8_t invalid = 0U;
    seen_t seen;

    if (!plat_sensor_read_cells(cells, &invalid)) {
        seen = SEEN_OFFLINE;
    } else if ((invalid & 0x10U) != 0U) {
        seen = SEEN_OPEN;
    } else if ((cells[4] >= 1000U) && (cells[4] < 1500U)) {
        seen = SEEN_HELD;
    } else {
        TEST_ASSERT_TRUE_MESSAGE((cells[4] >= 3300U) && (cells[4] <= 3900U),
                                 at("cell 5", cells[4]));
        seen = SEEN_MODEL;
    }

    *value = cells[4];
    return seen;
}

/* Commands go to a thread that serves them while this one keeps reading. Every reading taken
 * while a command is on its way shows the state before it or the one after it, and the first
 * reading after its reply shows the one after it. */
static void test_commands_from_another_thread_reach_the_reader(void)
{
    static const struct {
        const char *command;
        seen_t after;
    } cycle[] = {{"sensor.set cell5", SEEN_HELD},
                 {"sensor.open cell5", SEEN_OPEN},
                 {"sensor.offline on", SEEN_OFFLINE},
                 {"sensor.offline off", SEEN_OPEN},
                 {"sensor.clear cell5", SEEN_MODEL}};
    pthread_t thread;
    seen_t state = SEEN_MODEL;
    uint16_t held = 0U;
    unsigned long readings_in_flight = 0UL;

    atomic_store(&s_serving, true);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, serve, NULL));

    for (uint32_t n = 0U; n < 500U; n++) {
        const size_t step = n % (sizeof(cycle) / sizeof(cycle[0]));
        const seen_t after = cycle[step].after;
        const uint16_t value = (uint16_t)(1000U + (n % 500U));
        char request[64];
        const char *reply = NULL;
        uint16_t read_value = 0U;

        if (after == SEEN_HELD) {
            (void)snprintf(request, sizeof(request), "r%u %s %u", (unsigned int)n,
                           cycle[step].command, (unsigned int)value);
        } else {
            (void)snprintf(request, sizeof(request), "r%u %s", (unsigned int)n,
                           cycle[step].command);
        }

        send_request(request);

        while (reply == NULL) {
            const seen_t seen = look_at_cell5(&read_value);

            TEST_ASSERT_TRUE_MESSAGE((seen == state) || (seen == after), at("command", n));

            if ((seen == SEEN_HELD) && (state == SEEN_HELD)) {
                TEST_ASSERT_TRUE((read_value == held) || (read_value == value));
            }

            readings_in_flight++;
            reply = receive_reply(0);
        }

        /* The reply to this request (its token), and a success. */
        TEST_ASSERT_EQUAL_STRING_LEN(request, reply,
                                     (uint32_t)(strchr(request, ' ') - request) + 1U);
        TEST_ASSERT_TRUE_MESSAGE(strstr(reply, " ok ") != NULL, reply);
        TEST_ASSERT_EQUAL_INT_MESSAGE(after, look_at_cell5(&read_value), at("command", n));

        if (after == SEEN_HELD) {
            TEST_ASSERT_EQUAL_UINT16(value, read_value);
            held = value;
        }

        state = after;
    }

    atomic_store(&s_serving, false);
    TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
    TEST_ASSERT_TRUE(readings_in_flight >= 500UL);
}

int main(void)
{
    struct sockaddr_in any;
    socklen_t len = sizeof(any);
    int probe;

    UNITY_BEGIN();

    /* Let the kernel pick a free port, then give it to simulation control. */
    probe = socket(AF_INET, SOCK_DGRAM, 0);
    (void)memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if ((probe < 0) || (bind(probe, (const struct sockaddr *)&any, sizeof(any)) != 0) ||
        (getsockname(probe, (struct sockaddr *)&any, &len) != 0)) {
        (void)printf("cannot find a free port\n");
        return 1;
    }

    s_port = ntohs(any.sin_port);
    (void)close(probe);

    if (!posix_sim_open(s_port, s_error, sizeof(s_error))) {
        (void)printf("%s\n", s_error);
        return 1;
    }

    s_client = socket(AF_INET, SOCK_DGRAM, 0);

    RUN_TEST(test_the_same_seed_gives_the_same_readings);
    RUN_TEST(test_a_fresh_model_reports_the_default_seed_and_no_fault);
    RUN_TEST(test_the_current_follows_the_load_profile_with_a_little_noise);
    RUN_TEST(test_the_cells_follow_the_current_through_their_resistance);
    RUN_TEST(test_every_channel_has_noise_of_its_own);
    RUN_TEST(test_the_cells_see_the_current_of_the_latest_current_reading);
    RUN_TEST(test_before_the_first_current_reading_the_cells_see_the_first_step);
    RUN_TEST(test_a_held_channel_reads_exactly_its_value_and_nothing_else_changes);
    RUN_TEST(test_an_open_channel_is_flagged_and_reports_the_model_value);
    RUN_TEST(test_an_offline_front_end_answers_no_read_and_writes_nothing);
    RUN_TEST(test_clear_ends_every_fault_at_once);
    RUN_TEST(test_the_status_names_every_counter_and_every_channel);
    RUN_TEST(test_every_command_answers_with_the_state_it_set);
    RUN_TEST(test_a_wrong_command_is_refused_with_what_was_wrong);
    RUN_TEST(test_a_hanging_front_end_blocks_the_read_until_it_is_released);
    RUN_TEST(test_a_spinning_front_end_holds_the_read_without_the_wait_function);
    RUN_TEST(test_without_a_wait_function_a_hang_still_holds_the_read);
    RUN_TEST(test_the_hang_and_the_offline_state_are_independent);
    RUN_TEST(test_a_wrong_hang_command_is_refused);
    RUN_TEST(test_commands_from_another_thread_reach_the_reader);

    (void)close(s_client);
    posix_sim_close();
    return UNITY_END();
}
