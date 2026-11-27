/* SPDX-License-Identifier: MIT */
/*
 * The DTC module, in the order of its header: initialisation and refused input; the debouncing
 * (each report, the thresholds, the largest values, the jump down, the fault detection
 * counter); every rule of every bit of the status byte; the operation cycle and the clear; the
 * records; the read access; and a long random run against a reference model.
 *
 * Most tests use s_events, five events whose parameters differ, so that a rule applied with the
 * parameters of the wrong event shows at the wrong moment:
 *
 *   EV_A         failed 5,   passed -4,  up 2,  down 1,  confirmed after 1 cycle
 *   EV_JUMP      as EV_A, with jump down
 *   EV_CONFIRM3  failed 1,   passed -1,  up 1,  down 1,  confirmed after 3 cycles with a failure
 *   EV_LAMP      failed 3,   passed -3,  up 1,  down 1,  confirmed after 2, jump down, indicator
 *   EV_SLOW      failed 100, passed -50, up 30, down 20, confirmed after 1, indicator
 *
 * Status bytes are written in hex. The bits, from bit 0: TF 0x01, TFTOC 0x02, PDTC 0x04, CDTC
 * 0x08, TNCSLC 0x10, TFSLC 0x20, TNCTOC 0x40, WIR 0x80. The byte expected after each step is
 * worked out in the comment next to it.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dtc/dtc.h"
#include "unity.h"

#define PREFAILED DTC_REPORT_PREFAILED
#define PREPASSED DTC_REPORT_PREPASSED
#define FAILED    DTC_REPORT_FAILED
#define PASSED    DTC_REPORT_PASSED

#define TF     DTC_STATUS_TF
#define TFTOC  DTC_STATUS_TFTOC
#define PDTC   DTC_STATUS_PDTC
#define CDTC   DTC_STATUS_CDTC
#define TNCSLC DTC_STATUS_TNCSLC
#define TFSLC  DTC_STATUS_TFSLC
#define TNCTOC DTC_STATUS_TNCTOC
#define WIR    DTC_STATUS_WIR

enum { EV_A, EV_JUMP, EV_CONFIRM3, EV_LAMP, EV_SLOW, EV_COUNT };

static const dtc_event_config_t s_events[EV_COUNT] = {
    [EV_A] = {.dtc = 0x0A1B17U,
              .failed_threshold = 5,
              .passed_threshold = -4,
              .step_up = 2U,
              .step_down = 1U,
              .confirmation_threshold = 1U,
              .jump_down = false,
              .warning_indicator = false},
    [EV_JUMP] = {.dtc = 0x0A1C16U,
                 .failed_threshold = 5,
                 .passed_threshold = -4,
                 .step_up = 2U,
                 .step_down = 1U,
                 .confirmation_threshold = 1U,
                 .jump_down = true,
                 .warning_indicator = false},
    [EV_CONFIRM3] = {.dtc = 0xD10088U,
                     .failed_threshold = 1,
                     .passed_threshold = -1,
                     .step_up = 1U,
                     .step_down = 1U,
                     .confirmation_threshold = 3U,
                     .jump_down = false,
                     .warning_indicator = false},
    [EV_LAMP] = {.dtc = 0x0A2B4BU,
                 .failed_threshold = 3,
                 .passed_threshold = -3,
                 .step_up = 1U,
                 .step_down = 1U,
                 .confirmation_threshold = 2U,
                 .jump_down = true,
                 .warning_indicator = true},
    [EV_SLOW] = {.dtc = 0xFFFFFEU,
                 .failed_threshold = 100,
                 .passed_threshold = -50,
                 .step_up = 30U,
                 .step_down = 20U,
                 .confirmation_threshold = 1U,
                 .jump_down = false,
                 .warning_indicator = true},
};

/* The state under test, and a copy of it for the tests that show that a call changed nothing
 * at all. */
static dtc_state_t s_state;
static dtc_state_t s_snapshot;

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

static uint32_t random_below(uint32_t bound)
{
    return random_u32() % bound;
}

void setUp(void)
{
    s_random_state = 0x5EED0D7CU;
    /* Nonsense first: dtc_init() must need nothing from the memory it is given. */
    (void)memset(&s_state, 0xA5, sizeof(s_state));
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------------------- */

static const char *at(const char *what, unsigned long value)
{
    static char text[64];

    (void)snprintf(text, sizeof(text), "%s %lu", what, value);
    return text;
}

static void start(void)
{
    TEST_ASSERT_TRUE(dtc_init(&s_state, s_events, EV_COUNT));
}

static void report(uint16_t event, dtc_report_t what)
{
    TEST_ASSERT_TRUE_MESSAGE(dtc_report(&s_state, event, what), at("event", event));
}

static void report_n(uint16_t event, dtc_report_t what, unsigned int count)
{
    for (unsigned int i = 0U; i < count; i++) {
        report(event, what);
    }
}

static void cycle(void)
{
    TEST_ASSERT_TRUE(dtc_start_operation_cycle(&s_state));
}

static void clear(void)
{
    TEST_ASSERT_TRUE(dtc_clear_all(&s_state));
}

static dtc_entry_t entry_of(uint16_t event)
{
    dtc_entry_t entry;

    (void)memset(&entry, 0, sizeof(entry));
    TEST_ASSERT_TRUE_MESSAGE(dtc_get(&s_state, event, &entry), at("event", event));
    return entry;
}

static uint8_t status_of(uint16_t event)
{
    return entry_of(event).status;
}

static int16_t counter_of(uint16_t event)
{
    return entry_of(event).counter;
}

static int8_t fdc_of(uint16_t event)
{
    return entry_of(event).fault_detection_counter;
}

static uint8_t failed_cycles_of(uint16_t event)
{
    return entry_of(event).failed_cycles;
}

#define ASSERT_STATUS(expected, event)                                                             \
    TEST_ASSERT_EQUAL_HEX8_MESSAGE((expected), status_of(event), at("status of event", (event)))
#define ASSERT_COUNTER(expected, event)                                                            \
    TEST_ASSERT_EQUAL_INT16_MESSAGE((expected), counter_of(event), at("counter of event", (event)))

/* The record `which` names event `event`, and the entry carries that event's present state. */
static void expect_record(dtc_record_t which, uint16_t event)
{
    dtc_entry_t entry;

    TEST_ASSERT_TRUE_MESSAGE(dtc_recorded(&s_state, which, &entry),
                             at("record", (unsigned long)which));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(event, entry.event, at("record", (unsigned long)which));
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(s_events[event].dtc, entry.dtc,
                                    at("record", (unsigned long)which));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(status_of(event), entry.status,
                                   at("record", (unsigned long)which));
}

static void expect_no_record(dtc_record_t which)
{
    dtc_entry_t entry;

    (void)memset(&entry, 0x5A, sizeof(entry));
    TEST_ASSERT_FALSE_MESSAGE(dtc_recorded(&s_state, which, &entry),
                              at("record", (unsigned long)which));
    TEST_ASSERT_EACH_EQUAL_HEX8(0x5A, &entry, sizeof(entry)); /* left alone */
}

static void expect_no_records(void)
{
    expect_no_record(DTC_RECORD_FIRST_FAILED);
    expect_no_record(DTC_RECORD_FIRST_CONFIRMED);
    expect_no_record(DTC_RECORD_MOST_RECENT_FAILED);
    expect_no_record(DTC_RECORD_MOST_RECENT_CONFIRMED);
}

/* Every event as dtc_init() leaves it: the status after a clear, counter and FDC 0, no cycle
 * with a failure; and no records. */
static void expect_as_after_a_clear(void)
{
    for (uint16_t e = 0U; e < (uint16_t)EV_COUNT; e++) {
        const dtc_entry_t entry = entry_of(e);

        TEST_ASSERT_EQUAL_HEX8_MESSAGE(TNCSLC | TNCTOC, entry.status, at("event", e));
        TEST_ASSERT_EQUAL_INT16_MESSAGE(0, entry.counter, at("event", e));
        TEST_ASSERT_EQUAL_INT8_MESSAGE(0, entry.fault_detection_counter, at("event", e));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, entry.failed_cycles, at("event", e));
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(e, entry.event, at("event", e));
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(s_events[e].dtc, entry.dtc, at("event", e));
    }

    expect_no_records();
}

/* ------------------------------------------------------------------------------------------
 * Initialisation and refused input
 * ---------------------------------------------------------------------------------------- */

/* The memory held 0xA5 before. Every DTC is as after a clear, 0x50; the count of changes is 0;
 * all DTCs match a mask of every bit, none a mask of TF. */
static void test_init_gives_every_dtc_the_status_after_a_clear(void)
{
    start();
    expect_as_after_a_clear();
    TEST_ASSERT_EQUAL_HEX8(0x50U, DTC_STATUS_AFTER_CLEAR);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, DTC_STATUS_AVAILABILITY_MASK);
    TEST_ASSERT_EQUAL_UINT32(0U, dtc_status_change_count(&s_state));
    TEST_ASSERT_EQUAL_UINT16(EV_COUNT, dtc_count_by_status_mask(&s_state, 0xFFU));
    TEST_ASSERT_EQUAL_UINT16(0U, dtc_count_by_status_mask(&s_state, TF));

    /* The entries beyond the table are cleared too, so that the state holds nothing from
     * before. No function reads them; this looks at the private array. */
    for (uint32_t i = EV_COUNT; i < DTC_EVENTS_MAX; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x50U, s_state.event[i].status, at("entry", i));
        TEST_ASSERT_EQUAL_INT16_MESSAGE(0, s_state.event[i].counter, at("entry", i));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, s_state.event[i].failed_cycles, at("entry", i));
    }
}

/* Tables dtc_init() refuses: each has exactly one fault, made in a copy of s_events. */
#define BAD_TABLES 13U

static void bad_tables(dtc_event_config_t tables[BAD_TABLES][EV_COUNT])
{
    for (size_t i = 0U; i < BAD_TABLES; i++) {
        (void)memcpy(tables[i], s_events, sizeof(s_events));
    }

    tables[0][EV_A].dtc = 0U;
    tables[1][EV_JUMP].dtc = 0xFFFFFFU; /* the groupOfDTC of all groups */
    tables[2][EV_SLOW].dtc = 0x1000000U;
    tables[3][EV_A].failed_threshold = 0;
    tables[4][EV_LAMP].failed_threshold = -1;
    tables[5][EV_A].passed_threshold = 0;
    tables[6][EV_CONFIRM3].passed_threshold = 1;
    tables[7][EV_JUMP].step_up = 0U;
    tables[8][EV_SLOW].step_down = 0U;
    tables[9][EV_LAMP].confirmation_threshold = 0U;
    tables[10][EV_SLOW].dtc = s_events[EV_A].dtc; /* the first and the last are one DTC */
    tables[11][EV_CONFIRM3].dtc = s_events[EV_LAMP].dtc;
    tables[12][EV_LAMP] = (dtc_event_config_t){0}; /* an entry left out */
}

/* Refused, with the state unchanged (a valid one, set up before): NULL pointers, no events,
 * more than DTC_EVENTS_MAX, and every bad table. */
static void test_init_refuses_bad_tables_and_changes_nothing(void)
{
    static dtc_event_config_t tables[BAD_TABLES][EV_COUNT];
    static dtc_event_config_t many[DTC_EVENTS_MAX + 1U];

    bad_tables(tables);

    for (uint32_t i = 0U; i <= DTC_EVENTS_MAX; i++) {
        many[i] = s_events[EV_A];
        many[i].dtc = 0x100000U + i;
    }

    start();
    report(EV_A, FAILED);
    s_snapshot = s_state;

    TEST_ASSERT_FALSE(dtc_init(NULL, s_events, EV_COUNT));
    TEST_ASSERT_FALSE(dtc_init(&s_state, NULL, EV_COUNT));
    TEST_ASSERT_FALSE(dtc_init(&s_state, s_events, 0U));
    TEST_ASSERT_FALSE(dtc_init(&s_state, many, DTC_EVENTS_MAX + 1U));

    for (size_t i = 0U; i < BAD_TABLES; i++) {
        TEST_ASSERT_FALSE_MESSAGE(dtc_init(&s_state, tables[i], EV_COUNT), at("table", i));
    }

    TEST_ASSERT_EQUAL_MEMORY(&s_snapshot, &s_state, sizeof(s_state));

    /* The same tables are fine without their faulty entry where it is the last one. */
    TEST_ASSERT_TRUE(dtc_init(&s_state, tables[2], EV_SLOW));
    TEST_ASSERT_TRUE(dtc_init(&s_state, many, DTC_EVENTS_MAX));
}

/* The boundaries of every range are accepted and work: DTC numbers 0x000001 and 0xFFFFFE,
 * thresholds 32767 and -32768, steps 65535, a confirmation threshold of 255, and a table of
 * DTC_EVENTS_MAX events, whose last one can be used and the one after it cannot. */
static void test_init_accepts_the_boundaries(void)
{
    static dtc_event_config_t many[DTC_EVENTS_MAX];
    const dtc_event_config_t extremes[2] = {{.dtc = DTC_NUMBER_MIN,
                                             .failed_threshold = 32767,
                                             .passed_threshold = -32768,
                                             .step_up = 65535U,
                                             .step_down = 65535U,
                                             .confirmation_threshold = 255U,
                                             .jump_down = true,
                                             .warning_indicator = true},
                                            {.dtc = DTC_NUMBER_MAX,
                                             .failed_threshold = 1,
                                             .passed_threshold = -1,
                                             .step_up = 1U,
                                             .step_down = 1U,
                                             .confirmation_threshold = 1U,
                                             .jump_down = false,
                                             .warning_indicator = false}};

    TEST_ASSERT_EQUAL_HEX32(0x000001U, DTC_NUMBER_MIN);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFEU, DTC_NUMBER_MAX);
    TEST_ASSERT_TRUE(dtc_init(&s_state, extremes, 2U));
    TEST_ASSERT_TRUE(dtc_report(&s_state, 1U, FAILED));
    TEST_ASSERT_EQUAL_HEX8(TF | TFTOC | PDTC | CDTC | TFSLC, status_of(1U));

    for (uint32_t i = 0U; i < DTC_EVENTS_MAX; i++) {
        many[i] = s_events[EV_SLOW];
        many[i].dtc = 0x200000U + (i * 0x100U);
    }

    TEST_ASSERT_TRUE(dtc_init(&s_state, many, DTC_EVENTS_MAX));
    TEST_ASSERT_TRUE(dtc_report(&s_state, DTC_EVENTS_MAX - 1U, FAILED));
    TEST_ASSERT_EQUAL_HEX8(TF | TFTOC | PDTC | CDTC | TFSLC | WIR, status_of(DTC_EVENTS_MAX - 1U));
    TEST_ASSERT_FALSE(dtc_report(&s_state, DTC_EVENTS_MAX, FAILED));
    TEST_ASSERT_EQUAL_UINT16(1U, dtc_count_by_status_mask(&s_state, TF));
}

/* A dtc_state_t that was never initialised (zeroed static memory) has no table: every function
 * refuses it, and none writes to it or to what it was given. */
static void test_a_state_that_was_never_initialised_is_refused(void)
{
    dtc_entry_t entry;
    uint16_t cursor = 3U;

    (void)memset(&s_state, 0, sizeof(s_state));
    (void)memset(&entry, 0x5A, sizeof(entry));
    s_snapshot = s_state;

    TEST_ASSERT_FALSE(dtc_report(&s_state, 0U, FAILED));
    TEST_ASSERT_FALSE(dtc_start_operation_cycle(&s_state));
    TEST_ASSERT_FALSE(dtc_clear_all(&s_state));
    TEST_ASSERT_FALSE(dtc_get(&s_state, 0U, &entry));
    TEST_ASSERT_EQUAL_UINT16(0U, dtc_count_by_status_mask(&s_state, 0xFFU));
    TEST_ASSERT_FALSE(dtc_next_by_status_mask(&s_state, 0xFFU, &cursor, &entry));
    TEST_ASSERT_FALSE(dtc_recorded(&s_state, DTC_RECORD_FIRST_FAILED, &entry));
    TEST_ASSERT_EQUAL_UINT32(0U, dtc_status_change_count(&s_state));

    TEST_ASSERT_EQUAL_MEMORY(&s_snapshot, &s_state, sizeof(s_state));
    TEST_ASSERT_EQUAL_UINT16(3U, cursor);
    TEST_ASSERT_EACH_EQUAL_HEX8(0x5A, &entry, sizeof(entry));
}

/* Refused, with the state unchanged: NULL pointers, an event id at the number of events, a
 * report or a record that is no value of its enumeration, and an event count that no call can
 * have left (0, or more than the arrays hold). */
static void test_wrong_arguments_are_refused_and_change_nothing(void)
{
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): outside it on purpose */
    const dtc_report_t not_a_report = (dtc_report_t)4;
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): outside it on purpose */
    const dtc_record_t not_a_record = (dtc_record_t)4;
    dtc_entry_t entry;
    uint16_t cursor = 0U;

    start();
    report(EV_A, FAILED);
    report(EV_LAMP, PREFAILED);
    (void)memset(&entry, 0x5A, sizeof(entry));
    s_snapshot = s_state;

    TEST_ASSERT_FALSE(dtc_report(NULL, EV_A, FAILED));
    TEST_ASSERT_FALSE(dtc_report(&s_state, EV_COUNT, FAILED));
    TEST_ASSERT_FALSE(dtc_report(&s_state, 0xFFFFU, FAILED));
    TEST_ASSERT_FALSE(dtc_report(&s_state, EV_A, not_a_report));
    TEST_ASSERT_FALSE(dtc_start_operation_cycle(NULL));
    TEST_ASSERT_FALSE(dtc_clear_all(NULL));
    TEST_ASSERT_FALSE(dtc_get(NULL, EV_A, &entry));
    TEST_ASSERT_FALSE(dtc_get(&s_state, EV_A, NULL));
    TEST_ASSERT_FALSE(dtc_get(&s_state, EV_COUNT, &entry));
    TEST_ASSERT_EQUAL_UINT16(0U, dtc_count_by_status_mask(NULL, 0xFFU));
    TEST_ASSERT_FALSE(dtc_next_by_status_mask(NULL, 0xFFU, &cursor, &entry));
    TEST_ASSERT_FALSE(dtc_next_by_status_mask(&s_state, 0xFFU, NULL, &entry));
    TEST_ASSERT_FALSE(dtc_next_by_status_mask(&s_state, 0xFFU, &cursor, NULL));
    TEST_ASSERT_FALSE(dtc_recorded(NULL, DTC_RECORD_FIRST_FAILED, &entry));
    TEST_ASSERT_FALSE(dtc_recorded(&s_state, DTC_RECORD_FIRST_FAILED, NULL));
    TEST_ASSERT_FALSE(dtc_recorded(&s_state, not_a_record, &entry));
    TEST_ASSERT_EQUAL_UINT32(0U, dtc_status_change_count(NULL));

    TEST_ASSERT_EQUAL_MEMORY(&s_snapshot, &s_state, sizeof(s_state));
    TEST_ASSERT_EQUAL_UINT16(0U, cursor);
    TEST_ASSERT_EACH_EQUAL_HEX8(0x5A, &entry, sizeof(entry));

    /* A damaged event count would be an access outside the arrays. */
    s_state.event_count = 0U;
    s_snapshot = s_state;
    TEST_ASSERT_FALSE(dtc_report(&s_state, 0U, FAILED));
    TEST_ASSERT_FALSE(dtc_start_operation_cycle(&s_state));
    TEST_ASSERT_FALSE(dtc_clear_all(&s_state));
    TEST_ASSERT_EQUAL_MEMORY(&s_snapshot, &s_state, sizeof(s_state));

    s_state.event_count = DTC_EVENTS_MAX + 1U;
    s_snapshot = s_state;
    TEST_ASSERT_FALSE(dtc_report(&s_state, DTC_EVENTS_MAX, FAILED));
    TEST_ASSERT_FALSE(dtc_start_operation_cycle(&s_state));
    TEST_ASSERT_FALSE(dtc_clear_all(&s_state));
    TEST_ASSERT_EQUAL_UINT16(0U, dtc_count_by_status_mask(&s_state, 0xFFU));
    TEST_ASSERT_FALSE(dtc_get(&s_state, 0U, &entry));
    TEST_ASSERT_EQUAL_MEMORY(&s_snapshot, &s_state, sizeof(s_state));
}

/* ------------------------------------------------------------------------------------------
 * Debouncing
 * ---------------------------------------------------------------------------------------- */

/* PREFAILED adds step_up and is FAILED at the threshold, which caps the counter: EV_A goes
 * 2, 4, 5 (not 6) and fails at the third; EV_SLOW goes 30, 60, 90, 100 (not 120) and fails at
 * the fourth. Before that nothing but the counter changes. */
static void test_prefailed_counts_up_and_fails_at_the_threshold(void)
{
    start();
    report(EV_A, PREFAILED);
    ASSERT_COUNTER(2, EV_A);
    ASSERT_STATUS(0x50U, EV_A);
    report(EV_A, PREFAILED);
    ASSERT_COUNTER(4, EV_A);
    ASSERT_STATUS(0x50U, EV_A);
    report(EV_A, PREFAILED);
    ASSERT_COUNTER(5, EV_A);
    ASSERT_STATUS(TF | TFTOC | PDTC | CDTC | TFSLC, EV_A); /* 0x2F */

    for (int expected = 30; expected <= 90; expected += 30) {
        report(EV_SLOW, PREFAILED);
        ASSERT_COUNTER(expected, EV_SLOW);
        ASSERT_STATUS(0x50U, EV_SLOW);
    }

    report(EV_SLOW, PREFAILED);
    ASSERT_COUNTER(100, EV_SLOW);
    ASSERT_STATUS(TF | TFTOC | PDTC | CDTC | TFSLC | WIR, EV_SLOW); /* 0xAF */

    /* The other events have not moved. */
    ASSERT_STATUS(0x50U, EV_JUMP);
    ASSERT_COUNTER(0, EV_JUMP);
}

/* PREPASSED subtracts step_down and is PASSED at the threshold, which caps the counter: EV_A
 * goes -1, -2, -3, -4 and passes at the fourth; EV_SLOW -20, -40, -50 (not -60) at the third.
 * PASSED resets TF, TNCSLC and TNCTOC: 0x00. */
static void test_prepassed_counts_down_and_passes_at_the_threshold(void)
{
    start();

    for (int expected = -1; expected >= -3; expected--) {
        report(EV_A, PREPASSED);
        ASSERT_COUNTER(expected, EV_A);
        ASSERT_STATUS(0x50U, EV_A);
    }

    report(EV_A, PREPASSED);
    ASSERT_COUNTER(-4, EV_A);
    ASSERT_STATUS(0x00U, EV_A);

    report(EV_SLOW, PREPASSED);
    ASSERT_COUNTER(-20, EV_SLOW);
    report(EV_SLOW, PREPASSED);
    ASSERT_COUNTER(-40, EV_SLOW);
    ASSERT_STATUS(0x50U, EV_SLOW);
    report(EV_SLOW, PREPASSED);
    ASSERT_COUNTER(-50, EV_SLOW);
    ASSERT_STATUS(0x00U, EV_SLOW);
}

/* At a threshold the counter stays, and more of the same report changes no status byte. */
static void test_the_counter_stays_at_its_thresholds(void)
{
    start();
    report(EV_A, FAILED);
    TEST_ASSERT_EQUAL_UINT32(1U, dtc_status_change_count(&s_state));
    report_n(EV_A, PREFAILED, 10U);
    ASSERT_COUNTER(5, EV_A);
    ASSERT_STATUS(0x2FU, EV_A);
    report_n(EV_A, FAILED, 3U);
    ASSERT_COUNTER(5, EV_A);
    TEST_ASSERT_EQUAL_UINT32(1U, dtc_status_change_count(&s_state));

    report(EV_A, PASSED);
    ASSERT_COUNTER(-4, EV_A);
    ASSERT_STATUS(TFTOC | PDTC | CDTC | TFSLC, EV_A); /* 0x2E: only TF went */
    report_n(EV_A, PREPASSED, 10U);
    report_n(EV_A, PASSED, 3U);
    ASSERT_COUNTER(-4, EV_A);
    ASSERT_STATUS(0x2EU, EV_A);
    TEST_ASSERT_EQUAL_UINT32(2U, dtc_status_change_count(&s_state));
}

/* FAILED and PASSED set the counter to their threshold at once, from anywhere: EV_SLOW from 30
 * to 100, then to -50; EV_A from -3 to 5. */
static void test_failed_and_passed_set_the_counter_to_the_thresholds(void)
{
    start();
    report(EV_SLOW, PREFAILED);
    report(EV_SLOW, FAILED);
    ASSERT_COUNTER(100, EV_SLOW);
    ASSERT_STATUS(0xAFU, EV_SLOW);
    report(EV_SLOW, PASSED);
    ASSERT_COUNTER(-50, EV_SLOW);
    ASSERT_STATUS(0xAEU, EV_SLOW);

    report_n(EV_A, PREPASSED, 3U);
    ASSERT_COUNTER(-3, EV_A);
    report(EV_A, FAILED);
    ASSERT_COUNTER(5, EV_A);
    ASSERT_STATUS(0x2FU, EV_A);
}

/*
 * The largest values: thresholds 32767 and -32768 and steps 65535, which a 16-bit sum would
 * overflow. One PREFAILED from 0 reaches 32767, one PREPASSED from there -32768 (32767 - 65535
 * is exactly the passed threshold), one PREFAILED from there 32767 again. With jump down the
 * PREPASSED goes to 0 - 65535, which is capped at -32768. The fault detection counter is 127
 * and -128 at the ends. With a confirmation threshold of 255 nothing is confirmed in one cycle.
 */
static void test_the_largest_steps_and_thresholds_do_not_overflow(void)
{
    const dtc_event_config_t big[2] = {{.dtc = 0x000100U,
                                        .failed_threshold = 32767,
                                        .passed_threshold = -32768,
                                        .step_up = 65535U,
                                        .step_down = 65535U,
                                        .confirmation_threshold = 255U,
                                        .jump_down = false,
                                        .warning_indicator = false},
                                       {.dtc = 0x000200U,
                                        .failed_threshold = 32767,
                                        .passed_threshold = -32768,
                                        .step_up = 65535U,
                                        .step_down = 65535U,
                                        .confirmation_threshold = 255U,
                                        .jump_down = true,
                                        .warning_indicator = false}};
    dtc_entry_t entry;

    TEST_ASSERT_TRUE(dtc_init(&s_state, big, 2U));

    for (uint16_t e = 0U; e < 2U; e++) {
        TEST_ASSERT_TRUE(dtc_report(&s_state, e, PREFAILED));
        TEST_ASSERT_TRUE(dtc_get(&s_state, e, &entry));
        TEST_ASSERT_EQUAL_INT16_MESSAGE(32767, entry.counter, at("event", e));
        TEST_ASSERT_EQUAL_INT8_MESSAGE(127, entry.fault_detection_counter, at("event", e));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(TF | TFTOC | PDTC | TFSLC, entry.status, at("event", e));

        TEST_ASSERT_TRUE(dtc_report(&s_state, e, PREPASSED));
        TEST_ASSERT_TRUE(dtc_get(&s_state, e, &entry));
        TEST_ASSERT_EQUAL_INT16_MESSAGE(-32768, entry.counter, at("event", e));
        TEST_ASSERT_EQUAL_INT8_MESSAGE(-128, entry.fault_detection_counter, at("event", e));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(TFTOC | PDTC | TFSLC, entry.status, at("event", e));

        TEST_ASSERT_TRUE(dtc_report(&s_state, e, PREFAILED));
        TEST_ASSERT_TRUE(dtc_get(&s_state, e, &entry));
        TEST_ASSERT_EQUAL_INT16_MESSAGE(32767, entry.counter, at("event", e));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(TF | TFTOC | PDTC | TFSLC, entry.status, at("event", e));
    }
}

/*
 * Jump down. EV_A and EV_JUMP differ only in it. Both fail with three PREFAILED (0, 2, 4, 5).
 * Then EV_JUMP's first PREPASSED jumps to 0 and steps to -1, and it passes at the fourth, as
 * from 0; EV_A steps from 5 and passes at the ninth (4, 3, ..., -4). Until it passes, each
 * keeps TF: a counter on its way down is evidence, not a result.
 */
static void test_jump_down_counts_down_from_zero(void)
{
    start();
    report_n(EV_A, PREFAILED, 3U);
    report_n(EV_JUMP, PREFAILED, 3U);
    ASSERT_STATUS(0x2FU, EV_A);
    ASSERT_STATUS(0x2FU, EV_JUMP);

    report(EV_A, PREPASSED);
    report(EV_JUMP, PREPASSED);
    ASSERT_COUNTER(4, EV_A);
    ASSERT_COUNTER(-1, EV_JUMP);
    ASSERT_STATUS(0x2FU, EV_JUMP);

    report_n(EV_JUMP, PREPASSED, 2U);
    ASSERT_COUNTER(-3, EV_JUMP);
    ASSERT_STATUS(0x2FU, EV_JUMP);
    report(EV_JUMP, PREPASSED);
    ASSERT_COUNTER(-4, EV_JUMP);
    ASSERT_STATUS(0x2EU, EV_JUMP); /* the fourth: PASSED */

    report_n(EV_A, PREPASSED, 7U);
    ASSERT_COUNTER(-3, EV_A);
    ASSERT_STATUS(0x2FU, EV_A);
    report(EV_A, PREPASSED);
    ASSERT_COUNTER(-4, EV_A);
    ASSERT_STATUS(0x2EU, EV_A); /* the ninth */

    /* The jump is taken only from above 0, it does not need a FAILED before (any evidence of a
     * failure is enough), and it is never taken by a PREFAILED. */
    report(EV_JUMP, PREFAILED); /* -4 + 2 */
    ASSERT_COUNTER(-2, EV_JUMP);
    report(EV_JUMP, PREPASSED); /* no jump at -2: -3 */
    ASSERT_COUNTER(-3, EV_JUMP);
    report_n(EV_JUMP, PREFAILED, 2U); /* -1, 1 */
    ASSERT_COUNTER(1, EV_JUMP);
    report(EV_JUMP, PREPASSED); /* 1 -> 0 -> -1; without the jump it would be 0 */
    ASSERT_COUNTER(-1, EV_JUMP);
}

/* A pre-report that reaches no threshold completes no test: the status byte stays, TNCTOC and
 * TNCSLC included, and nothing is counted as a change. */
static void test_a_pre_report_short_of_a_threshold_completes_no_test(void)
{
    start();
    report_n(EV_A, PREFAILED, 2U);
    report(EV_A, PREPASSED);
    report_n(EV_SLOW, PREPASSED, 2U);
    report_n(EV_LAMP, PREFAILED, 2U);
    ASSERT_COUNTER(3, EV_A);
    ASSERT_COUNTER(-40, EV_SLOW);
    ASSERT_COUNTER(2, EV_LAMP);

    for (uint16_t e = 0U; e < (uint16_t)EV_COUNT; e++) {
        ASSERT_STATUS(0x50U, e);
    }

    TEST_ASSERT_EQUAL_UINT32(0U, dtc_status_change_count(&s_state));
}

/*
 * The fault detection counter, rounded toward zero. EV_A (5, -4): 2 -> 254 / 5 = 50, 4 -> 101,
 * 5 -> 127; -1 -> -128 / 4 = -32, -2 -> -64, -3 -> -96, -4 -> -128. EV_SLOW (100, -50): 30 -> 38,
 * 60 -> 76, 90 -> 114, 100 -> 127; -20 -> -51, -40 -> -102, -50 -> -128. With thresholds 1000
 * and -1000 and steps of 999, 999 -> 126 and -999 -> -127: only a counter at its threshold
 * reaches the end of the range.
 */
static void test_the_fault_detection_counter_maps_the_counter(void)
{
    static const int8_t up_a[3] = {50, 101, 127};
    static const int8_t down_a[4] = {-32, -64, -96, -128};
    static const int8_t up_slow[4] = {38, 76, 114, 127};
    static const int8_t down_slow[3] = {-51, -102, -128};
    const dtc_event_config_t fine = {.dtc = 0x123456U,
                                     .failed_threshold = 1000,
                                     .passed_threshold = -1000,
                                     .step_up = 999U,
                                     .step_down = 999U,
                                     .confirmation_threshold = 1U,
                                     .jump_down = false,
                                     .warning_indicator = false};
    dtc_entry_t entry;

    start();
    TEST_ASSERT_EQUAL_INT8(0, fdc_of(EV_A));

    for (size_t i = 0U; i < 3U; i++) {
        report(EV_A, PREFAILED);
        TEST_ASSERT_EQUAL_INT8_MESSAGE(up_a[i], fdc_of(EV_A), at("step", i));
    }

    cycle();
    TEST_ASSERT_EQUAL_INT8(0, fdc_of(EV_A));

    for (size_t i = 0U; i < 4U; i++) {
        report(EV_A, PREPASSED);
        TEST_ASSERT_EQUAL_INT8_MESSAGE(down_a[i], fdc_of(EV_A), at("step", i));
    }

    for (size_t i = 0U; i < 4U; i++) {
        report(EV_SLOW, PREFAILED);
        TEST_ASSERT_EQUAL_INT8_MESSAGE(up_slow[i], fdc_of(EV_SLOW), at("step", i));
    }

    cycle();

    for (size_t i = 0U; i < 3U; i++) {
        report(EV_SLOW, PREPASSED);
        TEST_ASSERT_EQUAL_INT8_MESSAGE(down_slow[i], fdc_of(EV_SLOW), at("step", i));
    }

    TEST_ASSERT_TRUE(dtc_init(&s_state, &fine, 1U));
    TEST_ASSERT_TRUE(dtc_report(&s_state, 0U, PREFAILED));
    TEST_ASSERT_TRUE(dtc_get(&s_state, 0U, &entry));
    TEST_ASSERT_EQUAL_INT16(999, entry.counter);
    TEST_ASSERT_EQUAL_INT8(126, entry.fault_detection_counter);
    TEST_ASSERT_TRUE(dtc_start_operation_cycle(&s_state));
    TEST_ASSERT_TRUE(dtc_report(&s_state, 0U, PREPASSED));
    TEST_ASSERT_TRUE(dtc_get(&s_state, 0U, &entry));
    TEST_ASSERT_EQUAL_INT16(-999, entry.counter);
    TEST_ASSERT_EQUAL_INT8(-127, entry.fault_detection_counter);
}

/* ------------------------------------------------------------------------------------------
 * The status byte, bit by bit
 * ---------------------------------------------------------------------------------------- */

/* Bit 0 testFailed: set by FAILED, kept while the counter falls short of passing, reset by
 * PASSED, set again by FAILED, kept at the start of a cycle (which performs no test), reset by
 * a clear. */
static void test_bit0_test_failed(void)
{
    start();
    report(EV_A, FAILED);
    ASSERT_STATUS(0x2FU, EV_A);
    report(EV_A, PREPASSED); /* 4 */
    ASSERT_STATUS(0x2FU, EV_A);
    report(EV_A, PASSED);
    ASSERT_STATUS(0x2EU, EV_A);
    report(EV_A, FAILED);
    ASSERT_STATUS(0x2FU, EV_A);
    cycle(); /* TFTOC reset, TNCTOC set: 0x2F - 0x02 + 0x40 */
    ASSERT_STATUS(TF | PDTC | CDTC | TFSLC | TNCTOC, EV_A); /* 0x6D */
    clear();
    ASSERT_STATUS(0x50U, EV_A);
}

/* Bit 1 testFailedThisOperationCycle: set by FAILED, kept by PASSED, reset at the start of a
 * cycle, set again, reset by a clear. */
static void test_bit1_test_failed_this_operation_cycle(void)
{
    start();
    report(EV_CONFIRM3, FAILED);
    ASSERT_STATUS(TF | TFTOC | PDTC | TFSLC, EV_CONFIRM3); /* 0x27: not confirmed yet */
    report(EV_CONFIRM3, PASSED);
    ASSERT_STATUS(TFTOC | PDTC | TFSLC, EV_CONFIRM3); /* 0x26 */
    cycle();
    ASSERT_STATUS(PDTC | TFSLC | TNCTOC, EV_CONFIRM3); /* 0x64 */
    report(EV_CONFIRM3, FAILED);
    ASSERT_STATUS(0x27U, EV_CONFIRM3);
    clear();
    ASSERT_STATUS(0x50U, EV_CONFIRM3);
}

/*
 * Bit 2 pendingDTC: set by FAILED and not reset by PASSED. At the end of a cycle it is kept if
 * the test failed in it, if the test did not run at all, and if only pre-reports came that
 * completed nothing; it is reset only at the end of a cycle in which the test completed and
 * never failed. A FAILED and a PASSED in either order in one cycle keep it. A clear resets it.
 */
static void test_bit2_pending(void)
{
    start();
    report(EV_A, FAILED);
    report(EV_A, PASSED);
    ASSERT_STATUS(0x2EU, EV_A);                        /* PDTC kept by PASSED */
    cycle();                                           /* the cycle had a failure */
    ASSERT_STATUS(PDTC | CDTC | TFSLC | TNCTOC, EV_A); /* 0x6C */
    cycle();                                           /* the test did not run */
    ASSERT_STATUS(0x6CU, EV_A);
    report(EV_A, PREFAILED);
    report(EV_A, PREPASSED);
    cycle(); /* evidence only, no test completed */
    ASSERT_STATUS(0x6CU, EV_A);
    report(EV_A, PASSED);
    ASSERT_STATUS(PDTC | CDTC | TFSLC, EV_A);   /* 0x2C */
    cycle();                                    /* completed without a failure: PDTC reset */
    ASSERT_STATUS(CDTC | TFSLC | TNCTOC, EV_A); /* 0x68 */

    report(EV_A, PASSED);
    report(EV_A, FAILED);
    cycle();                                                /* passed, then failed */
    ASSERT_STATUS(TF | PDTC | CDTC | TFSLC | TNCTOC, EV_A); /* 0x6D */
    report(EV_A, FAILED);
    report(EV_A, PASSED);
    cycle(); /* failed, then passed */
    ASSERT_STATUS(0x6CU, EV_A);
    report(EV_A, PASSED);
    clear();
    ASSERT_STATUS(0x50U, EV_A);
}

/* Bit 3 confirmedDTC with a confirmation threshold of 1: set by the first FAILED, kept through
 * PASSED and through any number of cycles that pass (no aging), reset only by a clear. */
static void test_bit3_confirmed_with_a_threshold_of_one(void)
{
    start();
    report(EV_A, FAILED);
    ASSERT_STATUS(0x2FU, EV_A);
    TEST_ASSERT_EQUAL_UINT8(1U, failed_cycles_of(EV_A));

    for (unsigned int i = 0U; i < 5U; i++) {
        report(EV_A, PASSED);
        cycle();
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(CDTC, status_of(EV_A) & CDTC, at("cycle", i));
    }

    ASSERT_STATUS(CDTC | TFSLC | TNCTOC, EV_A); /* 0x68 */
    TEST_ASSERT_EQUAL_UINT8(0U, failed_cycles_of(EV_A));
    clear();
    ASSERT_STATUS(0x50U, EV_A);
}

/*
 * Bit 3 with a threshold of 3 (EV_CONFIRM3, where every PREFAILED is FAILED). A cycle counts
 * once however often it fails; a cycle in which the test does not run keeps the count; the
 * third cycle with a failure confirms. Then, after a clear: two cycles with a failure, one that
 * passes (PDTC reset, the count starts again), and three more with a failure are needed.
 */
static void test_bit3_confirmed_after_three_cycles_with_a_failure(void)
{
    start();
    report(EV_CONFIRM3, PREFAILED);
    report(EV_CONFIRM3, FAILED);
    report(EV_CONFIRM3, PASSED);
    report(EV_CONFIRM3, FAILED);
    ASSERT_STATUS(0x27U, EV_CONFIRM3);
    TEST_ASSERT_EQUAL_UINT8(1U, failed_cycles_of(EV_CONFIRM3));
    cycle();                                                /* cycle 2: not tested */
    ASSERT_STATUS(TF | PDTC | TFSLC | TNCTOC, EV_CONFIRM3); /* 0x65 */
    cycle();                                                /* cycle 3 */
    TEST_ASSERT_EQUAL_UINT8(1U, failed_cycles_of(EV_CONFIRM3));
    report(EV_CONFIRM3, FAILED);
    ASSERT_STATUS(0x27U, EV_CONFIRM3);
    TEST_ASSERT_EQUAL_UINT8(2U, failed_cycles_of(EV_CONFIRM3));
    cycle(); /* cycle 4 */
    report(EV_CONFIRM3, FAILED);
    ASSERT_STATUS(0x2FU, EV_CONFIRM3);
    TEST_ASSERT_EQUAL_UINT8(3U, failed_cycles_of(EV_CONFIRM3));

    clear();
    TEST_ASSERT_EQUAL_UINT8(0U, failed_cycles_of(EV_CONFIRM3));
    report(EV_CONFIRM3, FAILED);
    cycle();
    report(EV_CONFIRM3, FAILED);
    TEST_ASSERT_EQUAL_UINT8(2U, failed_cycles_of(EV_CONFIRM3));
    cycle();
    report(EV_CONFIRM3, PASSED);
    ASSERT_STATUS(PDTC | TFSLC, EV_CONFIRM3);   /* 0x24 */
    cycle();                                    /* passed: PDTC reset, the count with it */
    ASSERT_STATUS(TFSLC | TNCTOC, EV_CONFIRM3); /* 0x60 */
    TEST_ASSERT_EQUAL_UINT8(0U, failed_cycles_of(EV_CONFIRM3));

    for (unsigned int i = 1U; i <= 2U; i++) {
        report(EV_CONFIRM3, FAILED);
        ASSERT_STATUS(0x27U, EV_CONFIRM3);
        TEST_ASSERT_EQUAL_UINT8(i, failed_cycles_of(EV_CONFIRM3));
        cycle();
    }

    report(EV_CONFIRM3, FAILED);
    ASSERT_STATUS(0x2FU, EV_CONFIRM3);
}

/* The count of cycles with a failure at its limit: with a confirmation threshold of 255 the DTC
 * is confirmed in the 255th cycle with a failure, not before, and the count stays at 255 after
 * that instead of wrapping to 0. */
static void test_the_count_of_cycles_with_a_failure_saturates(void)
{
    const dtc_event_config_t patient = {.dtc = 0x0B0B0BU,
                                        .failed_threshold = 1,
                                        .passed_threshold = -1,
                                        .step_up = 1U,
                                        .step_down = 1U,
                                        .confirmation_threshold = 255U,
                                        .jump_down = false,
                                        .warning_indicator = true};
    dtc_entry_t entry;

    TEST_ASSERT_TRUE(dtc_init(&s_state, &patient, 1U));

    for (unsigned int n = 1U; n <= 260U; n++) {
        TEST_ASSERT_TRUE(dtc_report(&s_state, 0U, FAILED));
        TEST_ASSERT_TRUE(dtc_get(&s_state, 0U, &entry));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((n < 255U) ? n : 255U, entry.failed_cycles, at("cycle", n));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE((n < 255U) ? 0x27U : 0xAFU, entry.status, at("cycle", n));
        TEST_ASSERT_TRUE(dtc_start_operation_cycle(&s_state));
    }
}

/* Bit 4 testNotCompletedSinceLastClear: set by dtc_init(), kept by pre-reports and by the start
 * of a cycle, reset by PASSED (EV_A) and by FAILED (EV_JUMP), not set again by a cycle, set by
 * a clear. */
static void test_bit4_test_not_completed_since_last_clear(void)
{
    start();
    report(EV_A, PREFAILED);
    report(EV_A, PREPASSED);
    cycle();
    ASSERT_STATUS(0x50U, EV_A);
    report(EV_A, PASSED);
    ASSERT_STATUS(0x00U, EV_A);
    report(EV_JUMP, FAILED);
    ASSERT_STATUS(0x2FU, EV_JUMP);
    cycle();
    ASSERT_STATUS(TNCTOC, EV_A); /* 0x40: TNCSLC stays reset */
    ASSERT_STATUS(0x6DU, EV_JUMP);
    clear();
    ASSERT_STATUS(0x50U, EV_A);
    ASSERT_STATUS(0x50U, EV_JUMP);
}

/* Bit 5 testFailedSinceLastClear: not set by PASSED; set by FAILED; kept through PASSED and
 * through cycles that pass; reset only by a clear. */
static void test_bit5_test_failed_since_last_clear(void)
{
    start();
    report(EV_A, PASSED);
    ASSERT_STATUS(0x00U, EV_A);
    report(EV_A, FAILED);
    ASSERT_STATUS(0x2FU, EV_A);

    for (unsigned int i = 0U; i < 3U; i++) {
        report(EV_A, PASSED);
        cycle();
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(TFSLC, status_of(EV_A) & TFSLC, at("cycle", i));
    }

    clear();
    ASSERT_STATUS(0x50U, EV_A);
}

/* Bit 6 testNotCompletedThisOperationCycle: set by dtc_init(), kept by a pre-report, reset by
 * FAILED, set by the start of a cycle, reset by PASSED, set by a clear in the middle of a cycle,
 * reset by the next FAILED. */
static void test_bit6_test_not_completed_this_operation_cycle(void)
{
    start();
    report(EV_SLOW, PREFAILED);
    ASSERT_STATUS(0x50U, EV_SLOW);
    report(EV_SLOW, FAILED);
    ASSERT_STATUS(0xAFU, EV_SLOW); /* TNCTOC reset */
    cycle();
    ASSERT_STATUS(TF | PDTC | CDTC | TFSLC | TNCTOC | WIR, EV_SLOW); /* 0xED */
    report(EV_SLOW, PASSED);
    ASSERT_STATUS(PDTC | CDTC | TFSLC | WIR, EV_SLOW); /* 0xAC */
    clear();
    ASSERT_STATUS(0x50U, EV_SLOW);
    report(EV_SLOW, FAILED);
    ASSERT_STATUS(0xAFU, EV_SLOW);
}

/* Bit 7 warningIndicatorRequested: set together with CDTC for an event with an indicator, so
 * EV_LAMP (threshold 2) has it from its second cycle with a failure on, not from its first
 * FAILED; kept through PASSED and cycles that pass (no healing); reset by a clear. EV_A, which
 * has no indicator, is confirmed without it. */
static void test_bit7_warning_indicator(void)
{
    start();
    report(EV_LAMP, FAILED);
    ASSERT_STATUS(0x27U, EV_LAMP);
    cycle();
    report(EV_LAMP, FAILED);
    ASSERT_STATUS(TF | TFTOC | PDTC | CDTC | TFSLC | WIR, EV_LAMP); /* 0xAF */
    report(EV_LAMP, PASSED);
    ASSERT_STATUS(0xAEU, EV_LAMP);
    cycle();
    ASSERT_STATUS(PDTC | CDTC | TFSLC | TNCTOC | WIR, EV_LAMP); /* 0xEC */
    report(EV_LAMP, PASSED);
    cycle();
    ASSERT_STATUS(CDTC | TFSLC | TNCTOC | WIR, EV_LAMP); /* 0xE8: PDTC gone, WIR stays */

    report(EV_A, FAILED);
    ASSERT_STATUS(0x2FU, EV_A);

    clear();
    ASSERT_STATUS(0x50U, EV_LAMP);
}

/* ------------------------------------------------------------------------------------------
 * The operation cycle and the clear
 * ---------------------------------------------------------------------------------------- */

/* The start of a cycle and a clear both set every counter, and so every fault detection
 * counter, to 0: EV_A needs its three PREFAILED again afterwards. */
static void test_cycle_and_clear_restart_the_debouncing(void)
{
    start();
    report_n(EV_A, PREFAILED, 2U);
    report_n(EV_JUMP, PREPASSED, 2U);
    report(EV_SLOW, FAILED);
    cycle();

    for (uint16_t e = 0U; e < (uint16_t)EV_COUNT; e++) {
        ASSERT_COUNTER(0, e);
        TEST_ASSERT_EQUAL_INT8_MESSAGE(0, fdc_of(e), at("event", e));
    }

    report_n(EV_A, PREFAILED, 2U);
    ASSERT_STATUS(0x50U, EV_A);
    report(EV_A, PREFAILED);
    ASSERT_STATUS(0x2FU, EV_A);

    report_n(EV_JUMP, PREFAILED, 2U);
    report(EV_SLOW, PREPASSED);
    clear();

    for (uint16_t e = 0U; e < (uint16_t)EV_COUNT; e++) {
        ASSERT_COUNTER(0, e);
    }

    report_n(EV_JUMP, PREFAILED, 2U);
    ASSERT_STATUS(0x50U, EV_JUMP);
    report(EV_JUMP, PREFAILED);
    ASSERT_STATUS(0x2FU, EV_JUMP);
}

/* A clear brings every event back to the state of dtc_init(), whatever it was in, except the
 * count of changes, which counts the clear's own. A fault that is still there comes back. */
static void test_clear_all_restores_the_state_after_init(void)
{
    start();
    report(EV_A, FAILED);
    report(EV_JUMP, PASSED);
    report(EV_CONFIRM3, FAILED);
    cycle();
    report(EV_CONFIRM3, FAILED); /* 2 cycles with a failure */
    report(EV_LAMP, PREFAILED);
    report_n(EV_SLOW, FAILED, 2U);
    TEST_ASSERT_EQUAL_UINT8(2U, failed_cycles_of(EV_CONFIRM3));
    /* EV_A, EV_JUMP, EV_CONFIRM3 once each; the cycle changes the same three; EV_CONFIRM3 and
     * EV_SLOW fail in the new cycle */
    TEST_ASSERT_EQUAL_UINT32(8U, dtc_status_change_count(&s_state));

    clear();
    expect_as_after_a_clear();
    /* changed by the clear: EV_A 0x6D, EV_JUMP 0x40, EV_CONFIRM3 0x27, EV_SLOW 0xAF; EV_LAMP
     * was 0x50 already */
    TEST_ASSERT_EQUAL_UINT32(12U, dtc_status_change_count(&s_state));
    TEST_ASSERT_EQUAL_UINT16(0U, dtc_count_by_status_mask(&s_state, TF | TFSLC | CDTC));

    /* The count of cycles with a failure starts again: one FAILED is one cycle, not three. */
    report(EV_CONFIRM3, FAILED);
    ASSERT_STATUS(0x27U, EV_CONFIRM3);
    report_n(EV_A, PREFAILED, 3U);
    ASSERT_STATUS(0x2FU, EV_A);
    expect_record(DTC_RECORD_FIRST_FAILED, EV_CONFIRM3);
}

/* ------------------------------------------------------------------------------------------
 * Records
 * ---------------------------------------------------------------------------------------- */

/* The records follow the rising edges of TF and CDTC since the last clear: a FAILED of a DTC
 * whose TF is still set, also from an earlier cycle, is no rising edge. */
static void test_records_follow_the_rising_edges(void)
{
    start();
    expect_no_records();

    report(EV_A, FAILED); /* TF and CDTC rise */
    expect_record(DTC_RECORD_FIRST_FAILED, EV_A);
    expect_record(DTC_RECORD_MOST_RECENT_FAILED, EV_A);
    expect_record(DTC_RECORD_FIRST_CONFIRMED, EV_A);
    expect_record(DTC_RECORD_MOST_RECENT_CONFIRMED, EV_A);

    report(EV_CONFIRM3, FAILED); /* TF rises, not confirmed */
    expect_record(DTC_RECORD_FIRST_FAILED, EV_A);
    expect_record(DTC_RECORD_MOST_RECENT_FAILED, EV_CONFIRM3);
    expect_record(DTC_RECORD_MOST_RECENT_CONFIRMED, EV_A);

    report(EV_A, FAILED); /* TF still set: no edge */
    expect_record(DTC_RECORD_MOST_RECENT_FAILED, EV_CONFIRM3);
    report(EV_A, PASSED); /* the record keeps the DTC and shows its status now, 0x2E */
    expect_record(DTC_RECORD_FIRST_FAILED, EV_A);
    report(EV_A, FAILED); /* an edge again */
    expect_record(DTC_RECORD_MOST_RECENT_FAILED, EV_A);

    report(EV_LAMP, FAILED); /* first cycle of two: TF rises, not confirmed */
    expect_record(DTC_RECORD_MOST_RECENT_FAILED, EV_LAMP);
    cycle();
    report(EV_A, PASSED);
    report(EV_A, FAILED);
    expect_record(DTC_RECORD_MOST_RECENT_FAILED, EV_A);
    report(EV_LAMP, FAILED); /* TF kept from the last cycle: no edge; CDTC rises */
    expect_record(DTC_RECORD_MOST_RECENT_FAILED, EV_A);
    expect_record(DTC_RECORD_FIRST_CONFIRMED, EV_A);
    expect_record(DTC_RECORD_MOST_RECENT_CONFIRMED, EV_LAMP);

    clear();
    expect_no_records();
    report(EV_CONFIRM3, FAILED);
    expect_record(DTC_RECORD_FIRST_FAILED, EV_CONFIRM3);
    expect_record(DTC_RECORD_MOST_RECENT_FAILED, EV_CONFIRM3);
    expect_no_record(DTC_RECORD_FIRST_CONFIRMED);
    expect_no_record(DTC_RECORD_MOST_RECENT_CONFIRMED);
}

/* A record that names no event of the table, which only damaged memory can hold, is no record:
 * it is not used as an index. */
static void test_a_damaged_record_is_not_used(void)
{
    dtc_entry_t entry;

    start();
    report(EV_A, FAILED);
    s_state.most_recent_failed = EV_COUNT;
    TEST_ASSERT_FALSE(dtc_recorded(&s_state, DTC_RECORD_MOST_RECENT_FAILED, &entry));
    expect_record(DTC_RECORD_FIRST_FAILED, EV_A);
}

/* ------------------------------------------------------------------------------------------
 * Read access
 * ---------------------------------------------------------------------------------------- */

/*
 * Counting and listing by status mask, (status & mask) != 0, in the order of the table. The
 * DTCs: EV_A 0x2F (FAILED), EV_JUMP 0x00 (PASSED), EV_CONFIRM3 0x27 (FAILED, not confirmed),
 * EV_LAMP and EV_SLOW 0x50 (untested; EV_SLOW has a counter).
 */
static void test_count_and_list_by_status_mask(void)
{
    static const struct {
        uint8_t mask;
        uint16_t count;
    } counts[] = {
        {TF, 2U},     {TFTOC, 2U}, {PDTC, 2U},  {CDTC, 1U},  {TNCSLC, 2U},   {TFSLC, 2U},
        {TNCTOC, 2U}, {WIR, 0U},   {0x00U, 0U}, {0xFFU, 4U}, {TF | WIR, 2U}, {TFSLC | TNCSLC, 4U},
    };
    dtc_entry_t entry;
    uint16_t cursor = 0U;

    start();
    report(EV_A, FAILED);
    report(EV_JUMP, PASSED);
    report(EV_CONFIRM3, FAILED);
    report(EV_SLOW, PREFAILED);

    for (size_t i = 0U; i < (sizeof(counts) / sizeof(counts[0])); i++) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(counts[i].count,
                                         dtc_count_by_status_mask(&s_state, counts[i].mask),
                                         at("mask", counts[i].mask));
    }

    /* testFailed: EV_A, then EV_CONFIRM3, then nothing, the cursor at the end of the table. */
    TEST_ASSERT_TRUE(dtc_next_by_status_mask(&s_state, TF, &cursor, &entry));
    TEST_ASSERT_EQUAL_UINT16(EV_A, entry.event);
    TEST_ASSERT_EQUAL_HEX32(s_events[EV_A].dtc, entry.dtc);
    TEST_ASSERT_EQUAL_HEX8(0x2FU, entry.status);
    TEST_ASSERT_EQUAL_UINT16(EV_A + 1U, cursor);
    TEST_ASSERT_TRUE(dtc_next_by_status_mask(&s_state, TF, &cursor, &entry));
    TEST_ASSERT_EQUAL_UINT16(EV_CONFIRM3, entry.event);
    TEST_ASSERT_EQUAL_HEX32(s_events[EV_CONFIRM3].dtc, entry.dtc);
    TEST_ASSERT_EQUAL_HEX8(0x27U, entry.status);
    TEST_ASSERT_EQUAL_UINT16(EV_CONFIRM3 + 1U, cursor);
    (void)memset(&entry, 0x5A, sizeof(entry));
    TEST_ASSERT_FALSE(dtc_next_by_status_mask(&s_state, TF, &cursor, &entry));
    TEST_ASSERT_EQUAL_UINT16(EV_COUNT, cursor);
    TEST_ASSERT_EACH_EQUAL_HEX8(0x5A, &entry, sizeof(entry));
    TEST_ASSERT_FALSE(dtc_next_by_status_mask(&s_state, TF, &cursor, &entry));
    TEST_ASSERT_EQUAL_UINT16(EV_COUNT, cursor);

    /* Every bit: all but EV_JUMP, in table order, with their counters. */
    {
        static const uint16_t expected[4] = {EV_A, EV_CONFIRM3, EV_LAMP, EV_SLOW};

        cursor = 0U;
        for (size_t i = 0U; i < 4U; i++) {
            TEST_ASSERT_TRUE_MESSAGE(dtc_next_by_status_mask(&s_state, 0xFFU, &cursor, &entry),
                                     at("match", i));
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(expected[i], entry.event, at("match", i));
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(status_of(expected[i]), entry.status, at("match", i));
            TEST_ASSERT_EQUAL_INT16_MESSAGE(counter_of(expected[i]), entry.counter, at("match", i));
        }
        TEST_ASSERT_FALSE(dtc_next_by_status_mask(&s_state, 0xFFU, &cursor, &entry));
    }

    /* A mask of 0 matches nothing; a cursor beyond the table stays where it is. */
    cursor = 0U;
    TEST_ASSERT_FALSE(dtc_next_by_status_mask(&s_state, 0x00U, &cursor, &entry));
    TEST_ASSERT_EQUAL_UINT16(EV_COUNT, cursor);
    cursor = 7U;
    TEST_ASSERT_FALSE(dtc_next_by_status_mask(&s_state, 0xFFU, &cursor, &entry));
    TEST_ASSERT_EQUAL_UINT16(7U, cursor);
    /* Starting in the middle: from EV_CONFIRM3 on, the first DTC with TNCTOC is EV_LAMP. */
    cursor = EV_CONFIRM3;
    TEST_ASSERT_TRUE(dtc_next_by_status_mask(&s_state, TNCTOC, &cursor, &entry));
    TEST_ASSERT_EQUAL_UINT16(EV_LAMP, entry.event);
}

/*
 * The count of status changes counts status bytes, not calls: a pre-report or a repeated result
 * adds nothing; a cycle start or a clear adds one for every byte it changes.
 *
 *   FAILED of EV_A after two PREFAILED                     1
 *   FAILED again, PREPASSED                                1
 *   PASSED of EV_A, PASSED of EV_JUMP                      3
 *   cycle: EV_A 0x2E -> 0x6C, EV_JUMP 0x00 -> 0x40         5
 *   cycle: nothing changes                                 5
 *   clear: EV_A and EV_JUMP to 0x50                        7
 *   clear again                                            7
 */
static void test_the_count_of_status_changes(void)
{
    start();
    report_n(EV_A, PREFAILED, 2U);
    TEST_ASSERT_EQUAL_UINT32(0U, dtc_status_change_count(&s_state));
    report(EV_A, PREFAILED);
    TEST_ASSERT_EQUAL_UINT32(1U, dtc_status_change_count(&s_state));
    report(EV_A, FAILED);
    report(EV_A, PREPASSED);
    TEST_ASSERT_EQUAL_UINT32(1U, dtc_status_change_count(&s_state));
    report(EV_A, PASSED);
    report(EV_JUMP, PASSED);
    TEST_ASSERT_EQUAL_UINT32(3U, dtc_status_change_count(&s_state));
    cycle();
    TEST_ASSERT_EQUAL_UINT32(5U, dtc_status_change_count(&s_state));
    cycle();
    TEST_ASSERT_EQUAL_UINT32(5U, dtc_status_change_count(&s_state));
    clear();
    TEST_ASSERT_EQUAL_UINT32(7U, dtc_status_change_count(&s_state));
    clear();
    TEST_ASSERT_EQUAL_UINT32(7U, dtc_status_change_count(&s_state));
}

/* ------------------------------------------------------------------------------------------
 * Against a reference model
 * ---------------------------------------------------------------------------------------- */

/*
 * The module as the norm's text describes it, kept another way. Each bit of the status byte is
 * a bool named after its definition in ISO 14229-1 Annex D and changed by the sentences of that
 * definition, as dtc.h quotes them; the byte is put together from the bools only to compare it,
 * with the bit positions written out. The debounce counter is a long, capped by comparisons;
 * the fault detection counter is computed from magnitudes; the records are logs of every rising
 * edge since the last clear; and the status changes are counted by comparing every byte before
 * and after each step. Nothing is shared with dtc.c.
 */
#define MODEL_LOG 4096U

typedef struct {
    bool test_failed;
    bool test_failed_this_operation_cycle;
    bool pending_dtc;
    bool confirmed_dtc;
    bool test_not_completed_since_last_clear;
    bool test_failed_since_last_clear;
    bool test_not_completed_this_operation_cycle;
    bool warning_indicator_requested;
    long counter;
    unsigned int failing_cycles; /* operation cycles with a failure since pendingDTC was reset */
} model_dtc_t;

typedef struct {
    const dtc_event_config_t *config;
    uint16_t count;
    model_dtc_t dtc[DTC_EVENTS_MAX];
    uint16_t failed_log[MODEL_LOG]; /* events whose testFailed rose, in order */
    size_t failed_length;
    uint16_t confirmed_log[MODEL_LOG]; /* events whose confirmedDTC rose, in order */
    size_t confirmed_length;
    unsigned long changes;
} model_t;

/* What the random run saw, so that it cannot pass by never reaching a rule. */
typedef struct {
    unsigned long jumps;              /* PREPASSED that jumped down */
    unsigned long capped_up;          /* PREFAILED capped at the failed threshold */
    unsigned long capped_down;        /* PREPASSED capped at the passed threshold */
    unsigned long pending_kept;       /* cycle ends with PDTC kept */
    unsigned long pending_reset;      /* cycle ends that reset PDTC */
    unsigned long late_confirmations; /* CDTC rose in the second or a later failing cycle */
    unsigned long indicators;         /* WIR rose */
    unsigned long clears;
} visits_t;

static model_t s_model;
static visits_t s_visits;

static uint8_t model_byte(const model_dtc_t *d)
{
    unsigned int byte = 0U;

    byte |= d->test_failed ? (1U << 0U) : 0U;
    byte |= d->test_failed_this_operation_cycle ? (1U << 1U) : 0U;
    byte |= d->pending_dtc ? (1U << 2U) : 0U;
    byte |= d->confirmed_dtc ? (1U << 3U) : 0U;
    byte |= d->test_not_completed_since_last_clear ? (1U << 4U) : 0U;
    byte |= d->test_failed_since_last_clear ? (1U << 5U) : 0U;
    byte |= d->test_not_completed_this_operation_cycle ? (1U << 6U) : 0U;
    byte |= d->warning_indicator_requested ? (1U << 7U) : 0U;
    return (uint8_t)byte;
}

/* Every bit as the norm has it after ClearDiagnosticInformation; no evidence, no count. */
static void model_clear_dtc(model_dtc_t *d)
{
    d->test_failed = false;
    d->test_failed_this_operation_cycle = false;
    d->pending_dtc = false;
    d->confirmed_dtc = false;
    d->test_not_completed_since_last_clear = true;
    d->test_failed_since_last_clear = false;
    d->test_not_completed_this_operation_cycle = true;
    d->warning_indicator_requested = false;
    d->counter = 0;
    d->failing_cycles = 0U;
}

static void model_clear(model_t *m)
{
    for (uint16_t e = 0U; e < m->count; e++) {
        model_clear_dtc(&m->dtc[e]);
    }

    m->failed_length = 0U;
    m->confirmed_length = 0U;
}

static void model_init(model_t *m, const dtc_event_config_t *config, uint16_t count)
{
    m->config = config;
    m->count = count;
    m->changes = 0U;
    model_clear(m);
}

static void model_log(uint16_t *log, size_t *length, uint16_t event)
{
    TEST_ASSERT_TRUE(*length < MODEL_LOG);
    log[*length] = event;
    (*length)++;
}

/* A completed test that failed, bit by bit. */
static void model_test_failed(model_t *m, uint16_t e)
{
    model_dtc_t *d = &m->dtc[e];

    /* confirmedDTC: the confirmation counts the cycles in which the test failed */
    if (!d->test_failed_this_operation_cycle && (d->failing_cycles < 255U)) {
        d->failing_cycles++;
    }

    if (!d->test_failed) {
        model_log(m->failed_log, &m->failed_length, e);
    }

    d->test_failed = true;
    d->test_failed_this_operation_cycle = true;
    d->pending_dtc = true;
    d->test_failed_since_last_clear = true;
    d->test_not_completed_since_last_clear = false;
    d->test_not_completed_this_operation_cycle = false;

    if (d->failing_cycles >= m->config[e].confirmation_threshold) {
        if (!d->confirmed_dtc) {
            model_log(m->confirmed_log, &m->confirmed_length, e);
            s_visits.late_confirmations += (d->failing_cycles >= 2U) ? 1U : 0U;
        }

        d->confirmed_dtc = true;

        if (m->config[e].warning_indicator) {
            s_visits.indicators += d->warning_indicator_requested ? 0U : 1U;
            d->warning_indicator_requested = true;
        }
    }
}

/* A completed test that passed. */
static void model_test_passed(model_dtc_t *d)
{
    d->test_failed = false;
    d->test_not_completed_since_last_clear = false;
    d->test_not_completed_this_operation_cycle = false;
}

static void model_report(model_t *m, uint16_t e, dtc_report_t what)
{
    const dtc_event_config_t *c = &m->config[e];
    model_dtc_t *d = &m->dtc[e];
    const long failed = c->failed_threshold;
    const long passed = c->passed_threshold;

    if (what == PREFAILED) {
        d->counter += (long)c->step_up;
        if (d->counter > failed) {
            d->counter = failed;
            s_visits.capped_up++;
        }
        if (d->counter == failed) {
            model_test_failed(m, e);
        }
    } else if (what == PREPASSED) {
        if (c->jump_down && (d->counter > 0)) {
            d->counter = 0;
            s_visits.jumps++;
        }
        d->counter -= (long)c->step_down;
        if (d->counter < passed) {
            d->counter = passed;
            s_visits.capped_down++;
        }
        if (d->counter == passed) {
            model_test_passed(d);
        }
    } else if (what == FAILED) {
        d->counter = failed;
        model_test_failed(m, e);
    } else {
        d->counter = passed;
        model_test_passed(d);
    }
}

/* The end of one operation cycle and the start of the next. */
static void model_cycle(model_t *m)
{
    for (uint16_t e = 0U; e < m->count; e++) {
        model_dtc_t *d = &m->dtc[e];
        const bool completed = !d->test_not_completed_this_operation_cycle;

        if (completed && !d->test_failed_this_operation_cycle) {
            s_visits.pending_reset += d->pending_dtc ? 1U : 0U;
            d->pending_dtc = false;
            d->failing_cycles = 0U;
        } else {
            s_visits.pending_kept += d->pending_dtc ? 1U : 0U;
        }

        d->test_failed_this_operation_cycle = false;
        d->test_not_completed_this_operation_cycle = true;
        d->counter = 0;
    }
}

static int model_fdc(const dtc_event_config_t *c, long counter)
{
    int fdc;

    if (counter >= 0) {
        fdc = (int)((counter * 127L) / (long)c->failed_threshold);
    } else {
        fdc = -(int)(((-counter) * 128L) / (-(long)c->passed_threshold));
    }

    return fdc;
}

#define IMPLIES(a, b) (!(a) || (b))

/* What dtc.h says follows from the rules, on the module's own byte. */
static void expect_consistent(const dtc_entry_t *entry, const dtc_event_config_t *c,
                              unsigned long where)
{
    const uint8_t s = entry->status;
    const bool tf = (s & TF) != 0U;
    const bool tftoc = (s & TFTOC) != 0U;
    const bool pdtc = (s & PDTC) != 0U;
    const bool cdtc = (s & CDTC) != 0U;
    const bool tncslc = (s & TNCSLC) != 0U;
    const bool tfslc = (s & TFSLC) != 0U;
    const bool tnctoc = (s & TNCTOC) != 0U;
    const bool wir = (s & WIR) != 0U;
    const char *text = at("step", where);

    TEST_ASSERT_TRUE_MESSAGE(IMPLIES(tf || tftoc || pdtc || cdtc, tfslc), text);
    TEST_ASSERT_TRUE_MESSAGE(IMPLIES(tftoc, pdtc && !tnctoc), text);
    TEST_ASSERT_TRUE_MESSAGE(IMPLIES(wir, cdtc && c->warning_indicator), text);
    TEST_ASSERT_TRUE_MESSAGE(IMPLIES(tncslc, tnctoc && !tfslc), text);
    TEST_ASSERT_TRUE_MESSAGE(IMPLIES(!pdtc, entry->failed_cycles == 0U), text);
    TEST_ASSERT_TRUE_MESSAGE(IMPLIES(tftoc, entry->failed_cycles >= 1U), text);
    TEST_ASSERT_TRUE_MESSAGE(
        (entry->counter >= c->passed_threshold) && (entry->counter <= c->failed_threshold), text);
}

/* The module and the model agree on everything that can be read. */
static void expect_as_model(const model_t *m, unsigned long where)
{
    const char *text;
    uint8_t mask = (uint8_t)random_below(256U);
    uint16_t matching = 0U;
    uint16_t cursor = 0U;
    dtc_entry_t entry;

    for (uint16_t e = 0U; e < m->count; e++) {
        const model_dtc_t *d = &m->dtc[e];

        TEST_ASSERT_TRUE(dtc_get(&s_state, e, &entry));
        text = at("event * 1000000 + step", (e * 1000000UL) + where);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(model_byte(d), entry.status, text);
        TEST_ASSERT_EQUAL_INT16_MESSAGE((int16_t)d->counter, entry.counter, text);
        TEST_ASSERT_EQUAL_INT8_MESSAGE((int8_t)model_fdc(&m->config[e], d->counter),
                                       entry.fault_detection_counter, text);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)d->failing_cycles, entry.failed_cycles, text);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(m->config[e].dtc, entry.dtc, text);
        expect_consistent(&entry, &m->config[e], where);

        if ((model_byte(d) & mask) != 0U) {
            matching++;
        }
    }

    text = at("step", where);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(m->changes, dtc_status_change_count(&s_state), text);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(matching, dtc_count_by_status_mask(&s_state, mask), text);

    /* The list for the same mask, against the model's bytes in table order. */
    for (uint16_t e = 0U; e < m->count; e++) {
        if ((model_byte(&m->dtc[e]) & mask) != 0U) {
            TEST_ASSERT_TRUE_MESSAGE(dtc_next_by_status_mask(&s_state, mask, &cursor, &entry),
                                     text);
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(e, entry.event, text);
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(dtc_next_by_status_mask(&s_state, mask, &cursor, &entry), text);

    /* The records: the ends of the logs. */
    {
        const struct {
            const uint16_t *log;
            size_t length;
            dtc_record_t record;
            bool first;
        } records[4] = {
            {m->failed_log, m->failed_length, DTC_RECORD_FIRST_FAILED, true},
            {m->failed_log, m->failed_length, DTC_RECORD_MOST_RECENT_FAILED, false},
            {m->confirmed_log, m->confirmed_length, DTC_RECORD_FIRST_CONFIRMED, true},
            {m->confirmed_log, m->confirmed_length, DTC_RECORD_MOST_RECENT_CONFIRMED, false},
        };

        for (size_t r = 0U; r < 4U; r++) {
            const bool found = dtc_recorded(&s_state, records[r].record, &entry);

            TEST_ASSERT_TRUE_MESSAGE((records[r].length > 0U) == found, text);
            if (found) {
                const uint16_t expected =
                    records[r].first ? records[r].log[0] : records[r].log[records[r].length - 1U];

                TEST_ASSERT_EQUAL_UINT16_MESSAGE(expected, entry.event, text);
            }
        }
    }
}

static int16_t random_failed_threshold(void)
{
    const uint32_t kind = random_below(10U);
    int16_t threshold = (int16_t)(1U + random_below(6U));

    if (kind == 0U) {
        threshold = 32767;
    } else if (kind == 1U) {
        threshold = 1;
    } else {
        /* 1..6 */
    }

    return threshold;
}

static int16_t random_passed_threshold(void)
{
    const uint32_t kind = random_below(10U);
    int16_t threshold = (int16_t) - (int16_t)(1U + random_below(6U));

    if (kind == 0U) {
        threshold = -32768;
    } else if (kind == 1U) {
        threshold = -1;
    } else {
        /* -6..-1 */
    }

    return threshold;
}

static uint16_t random_step(void)
{
    const uint32_t kind = random_below(12U);
    uint16_t step = (uint16_t)(1U + random_below(3U));

    if (kind == 0U) {
        step = 65535U;
    } else if (kind == 1U) {
        step = 32767U;
    } else {
        /* 1..3 */
    }

    return step;
}

/* Mostly small tables, in which every event is tested often; now and then a large one. */
static uint16_t random_table(dtc_event_config_t table[DTC_EVENTS_MAX])
{
    const uint16_t count = (uint16_t)((random_below(4U) == 0U) ? (1U + random_below(DTC_EVENTS_MAX))
                                                               : (1U + random_below(4U)));

    for (uint16_t i = 0U; i < count; i++) {
        dtc_event_config_t *e = &table[i];
        bool unique = false;

        while (!unique) {
            e->dtc =
                (random_below(16U) == 0U) ? DTC_NUMBER_MAX : (1U + (random_u32() % DTC_NUMBER_MAX));
            unique = true;
            for (uint16_t j = 0U; j < i; j++) {
                unique = unique && (table[j].dtc != e->dtc);
            }
        }

        e->failed_threshold = random_failed_threshold();
        e->passed_threshold = random_passed_threshold();
        e->step_up = random_step();
        e->step_down = random_step();
        e->confirmation_threshold = (uint8_t)(1U + random_below(4U));
        e->jump_down = random_below(2U) == 0U;
        e->warning_indicator = random_below(2U) == 0U;
    }

    return count;
}

static dtc_report_t random_report(void)
{
    const uint32_t r = random_below(100U);
    dtc_report_t what = PASSED;

    if (r < 35U) {
        what = PREFAILED;
    } else if (r < 70U) {
        what = PREPASSED;
    } else if (r < 85U) {
        what = FAILED;
    } else {
        /* PASSED */
    }

    return what;
}

/*
 * 300 random tables, 1000 random steps each: a report (now and then a burst of one report to
 * one event, so that counters reach their thresholds), the start of an operation cycle (8 %)
 * or a clear (1 %). After every step everything readable equals the model, and the module's
 * own bytes satisfy the implications of dtc.h. At the end, every rule must have been reached
 * many times.
 */
static void test_random_runs_match_the_reference_model(void)
{
    static dtc_event_config_t table[DTC_EVENTS_MAX];

    (void)memset(&s_visits, 0, sizeof(s_visits));

    for (unsigned long run = 0U; run < 300U; run++) {
        const uint16_t count = random_table(table);
        uint16_t burst_event = 0U;
        dtc_report_t burst_report = PASSED;
        unsigned int burst_left = 0U;

        TEST_ASSERT_TRUE(dtc_init(&s_state, table, count));
        model_init(&s_model, table, count);
        expect_as_model(&s_model, run * 10000UL);

        for (unsigned long n = 0U; n < 1000U; n++) {
            const unsigned long where = (run * 10000UL) + n + 1U;
            const uint32_t op = random_below(100U);
            uint8_t before[DTC_EVENTS_MAX];

            for (uint16_t e = 0U; e < count; e++) {
                before[e] = model_byte(&s_model.dtc[e]);
            }

            if (op < 1U) {
                TEST_ASSERT_TRUE(dtc_clear_all(&s_state));
                model_clear(&s_model);
                s_visits.clears++;
            } else if (op < 9U) {
                TEST_ASSERT_TRUE(dtc_start_operation_cycle(&s_state));
                model_cycle(&s_model);
            } else {
                uint16_t event = (uint16_t)random_below(count);
                dtc_report_t what = random_report();

                if (burst_left > 0U) {
                    event = burst_event;
                    what = burst_report;
                    burst_left--;
                } else if (random_below(15U) == 0U) {
                    burst_event = event;
                    burst_report = what;
                    burst_left = random_below(12U);
                } else {
                    /* a single random report */
                }

                TEST_ASSERT_TRUE(dtc_report(&s_state, event, what));
                model_report(&s_model, event, what);
            }

            for (uint16_t e = 0U; e < count; e++) {
                s_model.changes += (model_byte(&s_model.dtc[e]) != before[e]) ? 1U : 0U;
            }

            expect_as_model(&s_model, where);
        }
    }

    (void)printf("random runs reached: %lu jumps down, %lu PREFAILED and %lu PREPASSED capped at "
                 "a threshold, %lu cycle ends that kept PDTC and %lu that reset it, %lu "
                 "confirmations in a later failing cycle, %lu indicators, %lu clears\n",
                 s_visits.jumps, s_visits.capped_up, s_visits.capped_down, s_visits.pending_kept,
                 s_visits.pending_reset, s_visits.late_confirmations, s_visits.indicators,
                 s_visits.clears);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, s_visits.jumps);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, s_visits.capped_up);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, s_visits.capped_down);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, s_visits.pending_kept);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, s_visits.pending_reset);
    TEST_ASSERT_GREATER_THAN_UINT32(500U, s_visits.late_confirmations);
    TEST_ASSERT_GREATER_THAN_UINT32(500U, s_visits.indicators);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, s_visits.clears);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_gives_every_dtc_the_status_after_a_clear);
    RUN_TEST(test_init_refuses_bad_tables_and_changes_nothing);
    RUN_TEST(test_init_accepts_the_boundaries);
    RUN_TEST(test_a_state_that_was_never_initialised_is_refused);
    RUN_TEST(test_wrong_arguments_are_refused_and_change_nothing);
    RUN_TEST(test_prefailed_counts_up_and_fails_at_the_threshold);
    RUN_TEST(test_prepassed_counts_down_and_passes_at_the_threshold);
    RUN_TEST(test_the_counter_stays_at_its_thresholds);
    RUN_TEST(test_failed_and_passed_set_the_counter_to_the_thresholds);
    RUN_TEST(test_the_largest_steps_and_thresholds_do_not_overflow);
    RUN_TEST(test_jump_down_counts_down_from_zero);
    RUN_TEST(test_a_pre_report_short_of_a_threshold_completes_no_test);
    RUN_TEST(test_the_fault_detection_counter_maps_the_counter);
    RUN_TEST(test_bit0_test_failed);
    RUN_TEST(test_bit1_test_failed_this_operation_cycle);
    RUN_TEST(test_bit2_pending);
    RUN_TEST(test_bit3_confirmed_with_a_threshold_of_one);
    RUN_TEST(test_bit3_confirmed_after_three_cycles_with_a_failure);
    RUN_TEST(test_the_count_of_cycles_with_a_failure_saturates);
    RUN_TEST(test_bit4_test_not_completed_since_last_clear);
    RUN_TEST(test_bit5_test_failed_since_last_clear);
    RUN_TEST(test_bit6_test_not_completed_this_operation_cycle);
    RUN_TEST(test_bit7_warning_indicator);
    RUN_TEST(test_cycle_and_clear_restart_the_debouncing);
    RUN_TEST(test_clear_all_restores_the_state_after_init);
    RUN_TEST(test_records_follow_the_rising_edges);
    RUN_TEST(test_a_damaged_record_is_not_used);
    RUN_TEST(test_count_and_list_by_status_mask);
    RUN_TEST(test_the_count_of_status_changes);
    RUN_TEST(test_random_runs_match_the_reference_model);
    return UNITY_END();
}
