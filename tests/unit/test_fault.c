/* SPDX-License-Identifier: MIT */
/*
 * The fault monitor (app/fault.c): the table of faults, the verdicts on a sample, the health
 * task's monitors, the reset faults, the clear, and the summary BMS_Fault carries.
 *
 * The DTC numbers, the limits and the debounce counts are written here as literals from the
 * table in app/fault.h and the comments of app/fault.c, not as the module's macros, so that a
 * changed value shows up here. Each status byte expected is worked out from the bits of
 * ISO 14229-1 (lib/dtc/dtc.h): TF 0x01, TFTOC 0x02, PDTC 0x04, CDTC 0x08, TNCSLC 0x10, TFSLC
 * 0x20, TNCTOC 0x40, WIR 0x80. With a confirmation threshold of 1 the first FAILED sets TF,
 * TFTOC, PDTC, CDTC and TFSLC (0x2F), and WIR too for a fault that requests the indicator
 * (0xAF); a later PASSED leaves CDTC, PDTC, TFTOC, TFSLC and WIR (0xAE or 0x2E); a PASSED
 * that comes first leaves nothing (0x00); before any test the byte is 0x50.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fault.h"
#include "unity.h"

#define BIT(n) ((uint16_t)(1U << (n)))

/* The status bytes worked out above. */
#define UNTESTED      0x50U /* TNCSLC | TNCTOC */
#define PASSED_FIRST  0x00U
#define FAILED_NO_WIR 0x2FU /* TF | TFTOC | PDTC | CDTC | TFSLC */
#define FAILED_WIR    0xAFU /* and WIR */
#define HEALED_NO_WIR 0x2EU /* FAILED, then PASSED: TF reset */
#define HEALED_WIR    0xAEU

static fault_monitor_t s_monitor;
static bms_sample_t s_sample;
static bms_pack_t s_pack;

/* The sensor model's values without a fault, every channel valid, every group in the sample. */
static void healthy(void)
{
    (void)memset(&s_sample, 0, sizeof(s_sample));
    bms_init(&s_pack);
    s_sample.groups = (uint8_t)(BMS_SAMPLE_CURRENT | BMS_SAMPLE_CELLS | BMS_SAMPLE_TEMPS);
    s_sample.current_ma = 45000;

    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        s_sample.cell_mv[i] = 3700U;
    }

    for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
        s_sample.temp_dc[i] = 251;
    }

    s_pack.cell_valid = 0xFFU;
    s_pack.temp_valid = 0x0FU;
    s_pack.current_valid = true;
}

void setUp(void)
{
    (void)memset(&s_monitor, 0xA5, sizeof(s_monitor));
    TEST_ASSERT_TRUE(fault_init(&s_monitor, PLAT_RESET_POWER_ON, false));
    healthy();
}

void tearDown(void)
{
}

static uint8_t status_of(fault_event_t event)
{
    dtc_entry_t entry;

    TEST_ASSERT_TRUE(dtc_get(&s_monitor.dtc, (uint16_t)event, &entry));
    return entry.status;
}

static int16_t counter_of(fault_event_t event)
{
    dtc_entry_t entry;

    TEST_ASSERT_TRUE(dtc_get(&s_monitor.dtc, (uint16_t)event, &entry));
    return entry.counter;
}

static fault_verdict_t judge(uint8_t unanswered)
{
    fault_verdict_t verdict;

    (void)memset(&verdict, 0xA5, sizeof(verdict));
    fault_judge_sample(&s_sample, unanswered, &s_pack, &verdict);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0U, verdict.failing & verdict.passing, "failing and passing");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, verdict.clear_requests, "the poster tags it");
    return verdict;
}

/* failing (1), passing (-1) or not judged (0) for one fault in a verdict on the sample. */
static int evidence_of(fault_event_t event)
{
    const fault_verdict_t verdict = judge(0U);
    int result = 0;

    if ((verdict.failing & BIT(event)) != 0U) {
        result = 1;
    } else if ((verdict.passing & BIT(event)) != 0U) {
        result = -1;
    } else {
        result = 0;
    }

    return result;
}

/* `count` verdicts with only this fault failing (or passing). */
static void apply(fault_event_t event, bool failing, uint32_t count)
{
    const uint16_t mask = BIT(event);
    const fault_verdict_t verdict = {.failing = failing ? mask : (uint16_t)0U,
                                     .passing = failing ? (uint16_t)0U : mask,
                                     .clear_requests = s_monitor.clear_requests};

    for (uint32_t i = 0U; i < count; i++) {
        fault_apply(&s_monitor, &verdict);
    }
}

/* The summary of the period since the last one, and its changes of testFailed in `changes`. */
static fault_summary_t summary_with(fault_changes_t *changes)
{
    fault_summary_t out;

    (void)memset(&out, 0xA5, sizeof(out));
    (void)memset(changes, 0xA5, sizeof(*changes));
    fault_summary(&s_monitor, &out, changes);
    return out;
}

static fault_summary_t summary(void)
{
    fault_changes_t changes;

    return summary_with(&changes);
}

/* One verdict: `failing` and `passing` as given, tagged with the count of clear requests the
 * processing task had read when it posted it. */
static void apply_tagged(uint16_t failing, uint16_t passing, uint32_t clear_requests)
{
    const fault_verdict_t verdict = {
        .failing = failing, .passing = passing, .clear_requests = clear_requests};

    fault_apply(&s_monitor, &verdict);
}

static void health(uint32_t bus_offs, uint32_t passive_entries, can_fc_state_t state,
                   wdgm_global_status_t supervision, uint32_t cycles)
{
    const fault_health_inputs_t inputs = {.bus_off_count = bus_offs,
                                          .error_passive_entries = passive_entries,
                                          .can_state = state,
                                          .supervision = supervision};

    for (uint32_t i = 0U; i < cycles; i++) {
        fault_health_cycle(&s_monitor, &inputs);
    }
}

/* ------------------------------------------------------------------------------- the table */

static void test_every_fault_has_its_documented_dtc_and_name(void)
{
    static const struct {
        uint32_t dtc;
        const char *name;
    } expected[17] = {{0x1A0100U, "CELL_OVERVOLTAGE"},       {0x1A0200U, "CELL_UNDERVOLTAGE"},
                      {0x1A0300U, "OVERTEMPERATURE"},        {0x1A0400U, "UNDERTEMPERATURE"},
                      {0x1A0500U, "OVERCURRENT_DISCHARGE"},  {0x1A0600U, "OVERCURRENT_CHARGE"},
                      {0x1A1000U, "SENSOR_CHANNEL_INVALID"}, {0x1A1100U, "FRONT_END_NO_ANSWER"},
                      {0x1A1200U, "SAMPLE_TIMEOUT"},         {0xD00100U, "CAN_BUS_OFF"},
                      {0xD00200U, "CAN_ERROR_PASSIVE"},      {0x1B0100U, "SUPERVISION_EXPIRED"},
                      {0x1B0200U, "RESET_WATCHDOG"},         {0x1B0300U, "RESET_ASSERT"},
                      {0x1B0400U, "RESET_STACK_OVERFLOW"},   {0x1B0500U, "RESET_MALLOC_FAILED"},
                      {0x1B0600U, "UPDATE_ROLLBACK"}};

    TEST_ASSERT_EQUAL_UINT32(17U, (uint32_t)FAULT_EVENTS);

    for (uint32_t i = 0U; i < 17U; i++) {
        dtc_entry_t entry;

        TEST_ASSERT_EQUAL_HEX32(expected[i].dtc, fault_dtc_number(i));
        TEST_ASSERT_EQUAL_STRING(expected[i].name, fault_name(i));
        TEST_ASSERT_TRUE(dtc_get(&s_monitor.dtc, (uint16_t)i, &entry));
        TEST_ASSERT_EQUAL_HEX32(expected[i].dtc, entry.dtc);
    }

    TEST_ASSERT_EQUAL_HEX32(0U, fault_dtc_number(17U));
    TEST_ASSERT_EQUAL_STRING("unknown", fault_name(17U));
    TEST_ASSERT_EQUAL_STRING("unknown", fault_name(UINT32_MAX));
}

/* After a power-on: the four reset faults and the rollback judged (passed), every other one
 * untested, nothing in the summary. */
static void test_after_a_power_on_nothing_is_active_and_the_reset_faults_have_passed(void)
{
    const fault_summary_t picture = summary();

    for (uint32_t i = 0U; i < (uint32_t)FAULT_EVENTS; i++) {
        /* The reset faults and UPDATE_ROLLBACK, the last five, are judged at once. */
        const bool reset = i >= (uint32_t)FAULT_RESET_WATCHDOG;

        TEST_ASSERT_EQUAL_HEX8_MESSAGE(reset ? PASSED_FIRST : UNTESTED, status_of((fault_event_t)i),
                                       fault_name(i));
    }

    TEST_ASSERT_EQUAL_HEX32(0U, picture.active);
    TEST_ASSERT_EQUAL_HEX32(0U, picture.confirmed);
    TEST_ASSERT_EQUAL_UINT8(31U, picture.most_recent);
    TEST_ASSERT_EQUAL_UINT8(0U, picture.clears);
    TEST_ASSERT_FALSE(picture.warning);
    TEST_ASSERT_EQUAL(PLAT_RESET_POWER_ON, s_monitor.reset_cause);
    TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.verdicts);
}

/* A reset of each faulty kind is the confirmed, active fault of its own DTC from the start,
 * and the most recent; the other three pass. A reset that is no fault passes all four. */
static void test_a_faulty_reset_is_an_active_fault_from_the_start(void)
{
    static const struct {
        plat_reset_reason_t cause;
        int32_t event; /* -1: none */
    } cases[] = {{PLAT_RESET_WATCHDOG, (int32_t)FAULT_RESET_WATCHDOG},
                 {PLAT_RESET_ASSERT, (int32_t)FAULT_RESET_ASSERT},
                 {PLAT_RESET_STACK_OVERFLOW, (int32_t)FAULT_RESET_STACK_OVERFLOW},
                 {PLAT_RESET_MALLOC_FAILED, (int32_t)FAULT_RESET_MALLOC_FAILED},
                 {PLAT_RESET_REQUESTED, -1},
                 {PLAT_RESET_IMAGE_FAULT, -1},
                 {PLAT_RESET_POWER_LOSS, -1},
                 {PLAT_RESET_POWER_ON, -1}};

    for (size_t c = 0U; c < (sizeof(cases) / sizeof(cases[0])); c++) {
        TEST_ASSERT_TRUE(fault_init(&s_monitor, cases[c].cause, false));
        const fault_summary_t picture = summary();

        for (int32_t e = (int32_t)FAULT_RESET_WATCHDOG; e <= (int32_t)FAULT_RESET_MALLOC_FAILED;
             e++) {
            TEST_ASSERT_EQUAL_HEX8((e == cases[c].event) ? FAILED_NO_WIR : PASSED_FIRST,
                                   status_of((fault_event_t)e));
        }

        if (cases[c].event >= 0) {
            TEST_ASSERT_EQUAL_HEX32(BIT(cases[c].event), picture.active);
            TEST_ASSERT_EQUAL_HEX32(BIT(cases[c].event), picture.confirmed);
            TEST_ASSERT_EQUAL_UINT8((uint8_t)cases[c].event, picture.most_recent);
        } else {
            TEST_ASSERT_EQUAL_HEX32(0U, picture.active);
            TEST_ASSERT_EQUAL_UINT8(31U, picture.most_recent);
        }

        TEST_ASSERT_FALSE(picture.warning); /* the reset faults request no indicator */
        TEST_ASSERT_EQUAL(cases[c].cause, s_monitor.reset_cause);
        TEST_ASSERT_EQUAL_HEX8(PASSED_FIRST, status_of(FAULT_UPDATE_ROLLBACK));
    }
}

/* A rollback in the bootloader's hand-off is UPDATE_ROLLBACK, confirmed and active from the
 * start and the most recent, with no warning indicator, whatever the reset cause says; beside a
 * faulty reset both are active. It is judged once: a clear removes it for the run, and nothing
 * the monitors see brings it back. Without a rollback it has passed. */
static void test_a_rollback_in_the_hand_off_is_an_active_fault_from_the_start(void)
{
    const uint32_t rollback = (uint32_t)(1UL << (uint32_t)FAULT_UPDATE_ROLLBACK);
    const uint32_t watchdog = (uint32_t)(1UL << (uint32_t)FAULT_RESET_WATCHDOG);

    TEST_ASSERT_TRUE(fault_init(&s_monitor, PLAT_RESET_POWER_ON, true));
    fault_summary_t picture = summary();

    TEST_ASSERT_EQUAL_HEX8(FAILED_NO_WIR, status_of(FAULT_UPDATE_ROLLBACK));
    TEST_ASSERT_EQUAL_HEX32(rollback, picture.active);
    TEST_ASSERT_EQUAL_HEX32(rollback, picture.confirmed);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FAULT_UPDATE_ROLLBACK, picture.most_recent);
    TEST_ASSERT_FALSE(picture.warning);
    TEST_ASSERT_TRUE(s_monitor.rolled_back);

    for (int32_t e = (int32_t)FAULT_RESET_WATCHDOG; e <= (int32_t)FAULT_RESET_MALLOC_FAILED; e++) {
        TEST_ASSERT_EQUAL_HEX8(PASSED_FIRST, status_of((fault_event_t)e));
    }

    /* A start after a watchdog reset that the bootloader answered with a rollback: both. */
    TEST_ASSERT_TRUE(fault_init(&s_monitor, PLAT_RESET_WATCHDOG, true));
    picture = summary();
    TEST_ASSERT_EQUAL_HEX32(rollback | watchdog, picture.active);
    TEST_ASSERT_EQUAL_HEX32(rollback | watchdog, picture.confirmed);

    /* A clear ends it, and the run's monitors never judge it again. */
    TEST_ASSERT_TRUE(fault_clear_if_requested(&s_monitor, 1U));
    health(0U, 0U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_OK, 60U);
    picture = summary();
    TEST_ASSERT_EQUAL_HEX32(0U, picture.active & rollback);
    TEST_ASSERT_EQUAL_HEX32(0U, picture.confirmed & rollback);

    /* No rollback: passed, nothing active. */
    TEST_ASSERT_TRUE(fault_init(&s_monitor, PLAT_RESET_POWER_ON, false));
    picture = summary();
    TEST_ASSERT_EQUAL_HEX8(PASSED_FIRST, status_of(FAULT_UPDATE_ROLLBACK));
    TEST_ASSERT_EQUAL_HEX32(0U, picture.active);
    TEST_ASSERT_FALSE(s_monitor.rolled_back);
}

/* ---------------------------------------------------------------- verdicts: the limits */

/* For each limit: the reading just beyond it fails, the limit itself and the band up to the
 * release value are neither, the release value passes. */
static void test_the_cell_limits_and_their_release_values(void)
{
    static const struct {
        uint16_t mv;
        int over;
        int under;
    } cases[] = {{3700U, -1, -1}, {4251U, 1, -1}, {4250U, 0, -1}, {4201U, 0, -1},
                 {4200U, -1, -1}, {2499U, -1, 1}, {2500U, -1, 0}, {2599U, -1, 0},
                 {2600U, -1, -1}, {5000U, 1, -1}, {0U, -1, 1}};

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        healthy();
        s_sample.cell_mv[5] = cases[i].mv;
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].over, evidence_of(FAULT_CELL_OVERVOLTAGE), "over");
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].under, evidence_of(FAULT_CELL_UNDERVOLTAGE),
                                      "under");
    }
}

static void test_the_temperature_limits_and_their_release_values(void)
{
    static const struct {
        int16_t dc;
        int over;
        int under;
    } cases[] = {{251, -1, -1},  {601, 1, -1},  {600, 0, -1},  {551, 0, -1},
                 {550, -1, -1},  {-201, -1, 1}, {-200, -1, 0}, {-151, -1, 0},
                 {-150, -1, -1}, {1250, 1, -1}, {-400, -1, 1}};

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        healthy();
        s_sample.temp_dc[3] = cases[i].dc;
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].over, evidence_of(FAULT_OVERTEMPERATURE), "over");
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].under, evidence_of(FAULT_UNDERTEMPERATURE), "under");
    }
}

static void test_the_current_limits_and_their_release_values(void)
{
    static const struct {
        int32_t ma;
        int discharge;
        int charge;
    } cases[] = {{45000, -1, -1},  {160000, -1, -1}, {-80000, -1, -1}, {250001, 1, -1},
                 {250000, 0, -1},  {225001, 0, -1},  {225000, -1, -1}, {-100001, -1, 1},
                 {-100000, -1, 0}, {-90001, -1, 0},  {-90000, -1, -1}, {300000, 1, -1},
                 {-300000, -1, 1}};

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        healthy();
        s_sample.current_ma = cases[i].ma;
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].discharge, evidence_of(FAULT_OVERCURRENT_DISCHARGE),
                                      "discharge");
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].charge, evidence_of(FAULT_OVERCURRENT_CHARGE),
                                      "charge");
    }
}

/* A reading that is not valid in the pack (flagged, implausible, unanswered) says nothing about
 * its limit: it neither fails, however far beyond it lies, nor lets the group pass. */
static void test_a_reading_that_is_not_valid_is_no_evidence_about_a_limit(void)
{
    s_sample.cell_mv[2] = 4400U;
    s_pack.cell_valid = (uint8_t) ~(1U << 2U);
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_CELL_OVERVOLTAGE));
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_CELL_UNDERVOLTAGE));

    /* Another, valid cell beyond the limit still fails. */
    s_sample.cell_mv[3] = 4300U;
    TEST_ASSERT_EQUAL_INT(1, evidence_of(FAULT_CELL_OVERVOLTAGE));

    s_sample.temp_dc[0] = 900;
    s_pack.temp_valid = 0x0EU;
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_OVERTEMPERATURE));

    s_sample.current_ma = 280000;
    s_pack.current_valid = false;
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_OVERCURRENT_DISCHARGE));
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_OVERCURRENT_CHARGE));
}

/* Several channels beyond a limit at once are still one piece of evidence each way. */
static void test_several_channels_beyond_their_limits_are_one_verdict(void)
{
    s_sample.temp_dc[0] = 700;
    s_sample.temp_dc[2] = -300;
    s_sample.temp_dc[3] = 650;
    TEST_ASSERT_EQUAL_INT(1, evidence_of(FAULT_OVERTEMPERATURE));
    TEST_ASSERT_EQUAL_INT(1, evidence_of(FAULT_UNDERTEMPERATURE));

    healthy();
    s_sample.cell_mv[1] = 2000U;
    s_sample.cell_mv[6] = 4400U;
    TEST_ASSERT_EQUAL_INT(1, evidence_of(FAULT_CELL_OVERVOLTAGE));
    TEST_ASSERT_EQUAL_INT(1, evidence_of(FAULT_CELL_UNDERVOLTAGE));
}

/* A group the sample does not carry is not judged, whatever its old readings say. */
static void test_only_the_groups_of_the_sample_are_judged(void)
{
    s_sample.cell_mv[0] = 4300U;
    s_sample.temp_dc[0] = 700;
    s_sample.groups = (uint8_t)BMS_SAMPLE_CURRENT;
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_CELL_OVERVOLTAGE));
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_CELL_UNDERVOLTAGE));
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_OVERTEMPERATURE));
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_UNDERTEMPERATURE));
    TEST_ASSERT_EQUAL_INT(-1, evidence_of(FAULT_OVERCURRENT_DISCHARGE));

    s_sample.groups = (uint8_t)(BMS_SAMPLE_CURRENT | BMS_SAMPLE_CELLS);
    TEST_ASSERT_EQUAL_INT(1, evidence_of(FAULT_CELL_OVERVOLTAGE));
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_OVERTEMPERATURE));

    /* Without the current (the node always reads it, lib/bms does not need it). */
    s_sample.current_ma = 400000;
    s_sample.groups = (uint8_t)BMS_SAMPLE_CELLS;
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_OVERCURRENT_DISCHARGE));
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_OVERCURRENT_CHARGE));
    TEST_ASSERT_EQUAL_INT(1, evidence_of(FAULT_CELL_OVERVOLTAGE));
}

/* A monitor whose DTC state was never set up (zeroed memory, or fault_init() refused) is
 * refused by lib/dtc everywhere: its summary is empty, a clear request clears nothing. */
static void test_a_monitor_that_was_never_set_up_reports_and_clears_nothing(void)
{
    (void)memset(&s_monitor, 0, sizeof(s_monitor));
    apply(FAULT_SAMPLE_TIMEOUT, true, 1U);

    const fault_summary_t picture = summary();
    TEST_ASSERT_EQUAL_HEX32(0U, picture.active);
    TEST_ASSERT_EQUAL_HEX32(0U, picture.confirmed);
    TEST_ASSERT_EQUAL_UINT8(31U, picture.most_recent);
    TEST_ASSERT_FALSE(fault_clear_if_requested(&s_monitor, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.clears);
}

/* ------------------------------------------------------- verdicts: channels and front end */

static void test_an_invalid_channel_of_an_answered_group_fails_and_all_valid_passes(void)
{
    TEST_ASSERT_EQUAL_INT(-1, evidence_of(FAULT_SENSOR_CHANNEL_INVALID));

    /* Each group: one invalid channel in a group the front end answered fails. */
    s_pack.cell_valid = 0x7FU;
    TEST_ASSERT_EQUAL_INT(1, evidence_of(FAULT_SENSOR_CHANNEL_INVALID));
    healthy();
    s_pack.temp_valid = 0x0BU;
    TEST_ASSERT_EQUAL_INT(1, evidence_of(FAULT_SENSOR_CHANNEL_INVALID));
    healthy();
    s_pack.current_valid = false;
    TEST_ASSERT_EQUAL_INT(1, evidence_of(FAULT_SENSOR_CHANNEL_INVALID));

    /* The same group not answered: that is the front end's fault, not a channel's; and the
     * pack is not all valid, so nothing passes either. */
    fault_verdict_t verdict = judge((uint8_t)BMS_SAMPLE_CURRENT);
    TEST_ASSERT_EQUAL_HEX32(0U, verdict.failing & BIT(FAULT_SENSOR_CHANNEL_INVALID));
    TEST_ASSERT_EQUAL_HEX32(0U, verdict.passing & BIT(FAULT_SENSOR_CHANNEL_INVALID));

    /* A group still invalid from an earlier sample, not in this one: neither. */
    healthy();
    s_pack.temp_valid = 0x00U;
    s_sample.groups = (uint8_t)(BMS_SAMPLE_CURRENT | BMS_SAMPLE_CELLS);
    TEST_ASSERT_EQUAL_INT(0, evidence_of(FAULT_SENSOR_CHANNEL_INVALID));
}

static void test_a_group_without_an_answer_fails_the_front_end_and_answers_pass_it(void)
{
    static const uint8_t groups[] = {BMS_SAMPLE_CURRENT, BMS_SAMPLE_CELLS, BMS_SAMPLE_TEMPS};

    TEST_ASSERT_EQUAL_INT(-1, evidence_of(FAULT_FRONT_END_NO_ANSWER));

    for (size_t i = 0U; i < (sizeof(groups) / sizeof(groups[0])); i++) {
        const fault_verdict_t verdict = judge(groups[i]);

        TEST_ASSERT_EQUAL_HEX32(BIT(FAULT_FRONT_END_NO_ANSWER),
                                verdict.failing & BIT(FAULT_FRONT_END_NO_ANSWER));
    }

    /* A mark for a group the sample does not carry is no evidence: the sample's reads were
     * answered. */
    s_sample.groups = (uint8_t)BMS_SAMPLE_CURRENT;
    const fault_verdict_t verdict = judge((uint8_t)BMS_SAMPLE_TEMPS);
    TEST_ASSERT_EQUAL_HEX32(BIT(FAULT_FRONT_END_NO_ANSWER),
                            verdict.passing & BIT(FAULT_FRONT_END_NO_ANSWER));
}

static void test_a_sample_passes_the_timeout_and_a_timeout_fails_only_it(void)
{
    fault_verdict_t verdict;

    TEST_ASSERT_EQUAL_INT(-1, evidence_of(FAULT_SAMPLE_TIMEOUT));

    (void)memset(&verdict, 0xA5, sizeof(verdict));
    fault_judge_timeout(&verdict);
    TEST_ASSERT_EQUAL_HEX32(BIT(FAULT_SAMPLE_TIMEOUT), verdict.failing);
    TEST_ASSERT_EQUAL_HEX32(0U, verdict.passing);
    TEST_ASSERT_EQUAL_UINT32(0U, verdict.clear_requests);

    /* A sample judges nothing of the health task's or the reset faults. */
    const fault_verdict_t sample = judge(0U);
    TEST_ASSERT_EQUAL_HEX32(0U, (sample.failing | sample.passing) & 0xFE00U);
}

/* -------------------------------------------------------------------------- debouncing */

/*
 * Over-voltage: failed threshold 5, passed -5, steps of 1, no jump down. Four failing
 * readings leave the test open (counter 4); the fifth fails it. From 5 it takes ten passing
 * readings to reach -5 and pass. An excursion interrupted by passing readings still adds up:
 * 4 failing, 1 passing, 2 failing is 4 - 1 + 2 = 5.
 */
static void test_an_over_voltage_matures_after_five_readings_and_heals_after_ten(void)
{
    apply(FAULT_CELL_OVERVOLTAGE, true, 4U);
    TEST_ASSERT_EQUAL_HEX8(UNTESTED, status_of(FAULT_CELL_OVERVOLTAGE));
    TEST_ASSERT_EQUAL_INT16(4, counter_of(FAULT_CELL_OVERVOLTAGE));
    apply(FAULT_CELL_OVERVOLTAGE, true, 1U);
    TEST_ASSERT_EQUAL_HEX8(FAILED_WIR, status_of(FAULT_CELL_OVERVOLTAGE));

    fault_summary_t picture = summary();
    TEST_ASSERT_EQUAL_HEX32(BIT(FAULT_CELL_OVERVOLTAGE), picture.active);
    TEST_ASSERT_TRUE(picture.warning);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FAULT_CELL_OVERVOLTAGE, picture.most_recent);

    apply(FAULT_CELL_OVERVOLTAGE, false, 9U);
    TEST_ASSERT_EQUAL_HEX8(FAILED_WIR, status_of(FAULT_CELL_OVERVOLTAGE));
    apply(FAULT_CELL_OVERVOLTAGE, false, 1U);
    TEST_ASSERT_EQUAL_HEX8(HEALED_WIR, status_of(FAULT_CELL_OVERVOLTAGE));

    picture = summary();
    TEST_ASSERT_EQUAL_HEX32(0U, picture.active);
    TEST_ASSERT_EQUAL_HEX32(BIT(FAULT_CELL_OVERVOLTAGE), picture.confirmed);
    TEST_ASSERT_TRUE(picture.warning);

    /* The interrupted excursion. */
    TEST_ASSERT_TRUE(fault_init(&s_monitor, PLAT_RESET_POWER_ON, false));
    apply(FAULT_CELL_OVERVOLTAGE, true, 4U);
    apply(FAULT_CELL_OVERVOLTAGE, false, 1U);
    apply(FAULT_CELL_OVERVOLTAGE, true, 1U);
    TEST_ASSERT_EQUAL_HEX8(UNTESTED, status_of(FAULT_CELL_OVERVOLTAGE));
    apply(FAULT_CELL_OVERVOLTAGE, true, 1U);
    TEST_ASSERT_EQUAL_HEX8(FAILED_WIR, status_of(FAULT_CELL_OVERVOLTAGE));
}

/* The debounce counts of every fault the processing task judges: the reports that fail it from
 * the start, and those that heal it from FAILED. */
static void test_every_sample_fault_matures_and_heals_after_its_debounce_count(void)
{
    static const struct {
        fault_event_t event;
        uint32_t to_fail;
        uint32_t to_heal;
    } cases[] = {{FAULT_CELL_OVERVOLTAGE, 5U, 10U},
                 {FAULT_CELL_UNDERVOLTAGE, 5U, 10U},
                 {FAULT_OVERTEMPERATURE, 3U, 6U},
                 {FAULT_UNDERTEMPERATURE, 3U, 6U},
                 {FAULT_OVERCURRENT_DISCHARGE, 50U, 100U},
                 {FAULT_OVERCURRENT_CHARGE, 50U, 100U},
                 {FAULT_SENSOR_CHANNEL_INVALID, 1U, 1U},
                 {FAULT_FRONT_END_NO_ANSWER, 1U, 1U},
                 {FAULT_SAMPLE_TIMEOUT, 1U, 1U}};

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        const fault_event_t event = cases[i].event;

        TEST_ASSERT_TRUE(fault_init(&s_monitor, PLAT_RESET_POWER_ON, false));
        apply(event, true, cases[i].to_fail - 1U);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNTESTED, status_of(event), fault_name(event));
        apply(event, true, 1U);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(FAILED_WIR, status_of(event), fault_name(event));
        apply(event, false, cases[i].to_heal - 1U);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(FAILED_WIR, status_of(event), fault_name(event));
        apply(event, false, 1U);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(HEALED_WIR, status_of(event), fault_name(event));

        /* From the start, passing evidence passes at the passed threshold. */
        TEST_ASSERT_TRUE(fault_init(&s_monitor, PLAT_RESET_POWER_ON, false));
        apply(event, false, cases[i].to_heal / ((cases[i].to_fail == 1U) ? 1U : 2U));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(PASSED_FIRST, status_of(event), fault_name(event));

        /* After a pass the counter has the whole way to go: as many failing reports as it
         * takes to heal, one for a fault decided by the first piece of evidence. */
        apply(event, true, cases[i].to_heal - 1U);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(PASSED_FIRST, status_of(event), fault_name(event));
        apply(event, true, 1U);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(FAILED_WIR, status_of(event), fault_name(event));
    }
}

/* A verdict works in every fault it names, in one call; bits beyond the processing task's faults
 * are ignored, and a fault named in both masks counts as failing. */
static void test_a_verdict_reports_each_of_its_faults_once(void)
{
    const fault_verdict_t verdict = {
        .failing = (uint16_t)(BIT(FAULT_SAMPLE_TIMEOUT) | BIT(FAULT_CAN_BUS_OFF) |
                              BIT(FAULT_RESET_WATCHDOG) | BIT(FAULT_FRONT_END_NO_ANSWER)),
        .passing = (uint16_t)(BIT(FAULT_SENSOR_CHANNEL_INVALID) | BIT(FAULT_FRONT_END_NO_ANSWER) |
                              BIT(FAULT_CAN_ERROR_PASSIVE))};

    fault_apply(&s_monitor, &verdict);
    TEST_ASSERT_EQUAL_HEX8(FAILED_WIR, status_of(FAULT_SAMPLE_TIMEOUT));
    TEST_ASSERT_EQUAL_HEX8(FAILED_WIR, status_of(FAULT_FRONT_END_NO_ANSWER));
    TEST_ASSERT_EQUAL_HEX8(PASSED_FIRST, status_of(FAULT_SENSOR_CHANNEL_INVALID));
    TEST_ASSERT_EQUAL_HEX8(UNTESTED, status_of(FAULT_CAN_BUS_OFF));
    TEST_ASSERT_EQUAL_HEX8(UNTESTED, status_of(FAULT_CAN_ERROR_PASSIVE));
    TEST_ASSERT_EQUAL_HEX8(PASSED_FIRST, status_of(FAULT_RESET_WATCHDOG));
    TEST_ASSERT_EQUAL_INT16(0, counter_of(FAULT_CELL_OVERVOLTAGE));
    TEST_ASSERT_EQUAL_UINT32(1U, s_monitor.verdicts);
}

/* A whole sample through the two steps: a cell held at 4300 mV for five cell readings is an
 * over-voltage; the other faults of a healthy sample pass. */
static void test_judged_samples_make_the_dtcs(void)
{
    s_sample.cell_mv[0] = 4300U;

    for (uint32_t i = 0U; i < 5U; i++) {
        const fault_verdict_t verdict = judge(0U);

        fault_apply(&s_monitor, &verdict);
    }

    const fault_summary_t picture = summary();
    TEST_ASSERT_EQUAL_HEX32(BIT(FAULT_CELL_OVERVOLTAGE), picture.active);
    TEST_ASSERT_EQUAL_HEX8(PASSED_FIRST, status_of(FAULT_CELL_UNDERVOLTAGE));
    /* The current's passed threshold is -50: five readings are not yet a pass. */
    TEST_ASSERT_EQUAL_HEX8(UNTESTED, status_of(FAULT_OVERCURRENT_DISCHARGE));
    TEST_ASSERT_EQUAL_INT16(-5, counter_of(FAULT_OVERCURRENT_DISCHARGE));
    TEST_ASSERT_EQUAL_HEX8(PASSED_FIRST, status_of(FAULT_SENSOR_CHANNEL_INVALID));
    TEST_ASSERT_EQUAL_HEX8(PASSED_FIRST, status_of(FAULT_FRONT_END_NO_ANSWER));
    TEST_ASSERT_EQUAL_HEX8(PASSED_FIRST, status_of(FAULT_SAMPLE_TIMEOUT));
    TEST_ASSERT_EQUAL_HEX8(UNTESTED, status_of(FAULT_CAN_BUS_OFF));
}

/* ------------------------------------------------------------- the health task's monitors */

/*
 * Bus off: set by a bus-off counted since the last cycle, or by the controller being bus off
 * now; healed after 50 cycles in a row without either. The count is compared with the last
 * cycle's, so a count that stays where it is (3) is no new bus-off.
 */
static void test_a_bus_off_is_set_at_once_and_heals_after_fifty_cycles_without_one(void)
{
    health(0U, 0U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_OK, 1U);
    TEST_ASSERT_EQUAL_HEX8(UNTESTED, status_of(FAULT_CAN_BUS_OFF));
    TEST_ASSERT_EQUAL_INT16(-1, counter_of(FAULT_CAN_BUS_OFF));

    health(3U, 3U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_OK, 1U);
    TEST_ASSERT_EQUAL_HEX8(FAILED_NO_WIR, status_of(FAULT_CAN_BUS_OFF));
    health(3U, 3U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_OK, 49U);
    TEST_ASSERT_EQUAL_HEX8(FAILED_NO_WIR, status_of(FAULT_CAN_BUS_OFF));
    health(3U, 3U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_OK, 1U);
    TEST_ASSERT_EQUAL_HEX8(HEALED_NO_WIR, status_of(FAULT_CAN_BUS_OFF));

    /* Bus off now, without a new count (the event chain merged): failing as well. */
    health(3U, 3U, CAN_FC_BUS_OFF, WDGM_GLOBAL_STATUS_OK, 1U);
    TEST_ASSERT_EQUAL_HEX8(0x01U, status_of(FAULT_CAN_BUS_OFF) & 0x01U);
    TEST_ASSERT_EQUAL_UINT32(53U, s_monitor.health_cycles);
}

/* Error passive: set by an entry since the last cycle or a state that is not error active now;
 * healed after 10 cycles in a row error active. */
static void test_error_passive_is_set_at_once_and_heals_after_ten_cycles_error_active(void)
{
    health(0U, 1U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_OK, 1U);
    TEST_ASSERT_EQUAL_HEX8(FAILED_NO_WIR, status_of(FAULT_CAN_ERROR_PASSIVE));
    health(0U, 1U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_OK, 9U);
    TEST_ASSERT_EQUAL_HEX8(FAILED_NO_WIR, status_of(FAULT_CAN_ERROR_PASSIVE));
    health(0U, 1U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_OK, 1U);
    TEST_ASSERT_EQUAL_HEX8(HEALED_NO_WIR, status_of(FAULT_CAN_ERROR_PASSIVE));

    /* Still error passive, no new entry: failing; bus off: failing too. */
    health(0U, 1U, CAN_FC_ERROR_PASSIVE, WDGM_GLOBAL_STATUS_OK, 1U);
    TEST_ASSERT_EQUAL_HEX8(0x01U, status_of(FAULT_CAN_ERROR_PASSIVE) & 0x01U);
    TEST_ASSERT_TRUE(fault_init(&s_monitor, PLAT_RESET_POWER_ON, false));
    health(0U, 0U, CAN_FC_BUS_OFF, WDGM_GLOBAL_STATUS_OK, 1U);
    TEST_ASSERT_EQUAL_HEX8(FAILED_NO_WIR, status_of(FAULT_CAN_ERROR_PASSIVE));
    TEST_ASSERT_EQUAL_HEX8(FAILED_NO_WIR, status_of(FAULT_CAN_BUS_OFF));
}

/* Supervision: OK and FAILED pass, EXPIRED and STOPPED fail, DEACTIVATED judges nothing. */
static void test_the_supervision_fault_follows_the_global_status(void)
{
    health(0U, 0U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_DEACTIVATED, 1U);
    TEST_ASSERT_EQUAL_HEX8(UNTESTED, status_of(FAULT_SUPERVISION_EXPIRED));
    health(0U, 0U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_FAILED, 1U);
    TEST_ASSERT_EQUAL_HEX8(PASSED_FIRST, status_of(FAULT_SUPERVISION_EXPIRED));
    health(0U, 0U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_OK, 1U);
    TEST_ASSERT_EQUAL_HEX8(PASSED_FIRST, status_of(FAULT_SUPERVISION_EXPIRED));
    health(0U, 0U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_STOPPED, 1U);
    TEST_ASSERT_EQUAL_HEX8(FAILED_NO_WIR, status_of(FAULT_SUPERVISION_EXPIRED));

    TEST_ASSERT_TRUE(fault_init(&s_monitor, PLAT_RESET_POWER_ON, false));
    health(0U, 0U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_EXPIRED, 1U);
    TEST_ASSERT_EQUAL_HEX8(FAILED_NO_WIR, status_of(FAULT_SUPERVISION_EXPIRED));

    const fault_summary_t picture = summary();
    TEST_ASSERT_EQUAL_HEX32(BIT(FAULT_SUPERVISION_EXPIRED), picture.active);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FAULT_SUPERVISION_EXPIRED, picture.most_recent);
    TEST_ASSERT_FALSE(picture.warning);
}

/* ------------------------------------------------------------------------------ the clear */

static void test_a_new_clear_request_clears_every_dtc_once(void)
{
    TEST_ASSERT_TRUE(fault_init(&s_monitor, PLAT_RESET_WATCHDOG, false));
    apply(FAULT_SAMPLE_TIMEOUT, true, 1U);
    TEST_ASSERT_EQUAL_HEX32(BIT(FAULT_SAMPLE_TIMEOUT) | BIT(FAULT_RESET_WATCHDOG),
                            summary().active);

    /* The count the receiver started with: nothing to do. */
    TEST_ASSERT_FALSE(fault_clear_if_requested(&s_monitor, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.clears);

    /* Three requests since: one clear. Every DTC as after dtc_init(), the reset faults too. */
    TEST_ASSERT_TRUE(fault_clear_if_requested(&s_monitor, 3U));
    TEST_ASSERT_FALSE(fault_clear_if_requested(&s_monitor, 3U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_monitor.clears);

    for (uint32_t i = 0U; i < (uint32_t)FAULT_EVENTS; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNTESTED, status_of((fault_event_t)i), fault_name(i));
    }

    fault_summary_t picture = summary();
    TEST_ASSERT_EQUAL_HEX32(0U, picture.active);
    TEST_ASSERT_EQUAL_HEX32(0U, picture.confirmed);
    TEST_ASSERT_EQUAL_UINT8(31U, picture.most_recent);
    TEST_ASSERT_EQUAL_UINT8(1U, picture.clears);

    /* A fault still present comes back with its monitor's next evidence. */
    apply(FAULT_SAMPLE_TIMEOUT, true, 1U);
    picture = summary();
    TEST_ASSERT_EQUAL_HEX32(BIT(FAULT_SAMPLE_TIMEOUT), picture.active);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FAULT_SAMPLE_TIMEOUT, picture.most_recent);

    /* A count behind the one carried out is no request: it comes from before the last clear. */
    TEST_ASSERT_FALSE(fault_clear_if_requested(&s_monitor, 2U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_monitor.clears);

    /* The receiver's count wraps: ahead by up to 2^31 - 1 is newer, also across 2^32. */
    TEST_ASSERT_TRUE(fault_clear_if_requested(&s_monitor, 3U + 0x7FFFFFFFU));
    TEST_ASSERT_TRUE(fault_clear_if_requested(&s_monitor, 0xFFFFFFFFU));
    TEST_ASSERT_TRUE(fault_clear_if_requested(&s_monitor, 0U));
    TEST_ASSERT_FALSE(fault_clear_if_requested(&s_monitor, 0xFFFFFFFFU));
    TEST_ASSERT_EQUAL_UINT32(4U, s_monitor.clears);
}

/*
 * The order of a clear and the evidence (the health task works in a period's verdicts, then
 * carries out a request no verdict carried). Before the request: a channel goes invalid and the
 * front end does not answer; the receiver then counts the request (1). After it: the channel is
 * valid again, and a sample has a read the front end did not answer, and the next one is whole
 * again. The clear comes before the first verdict that carries the request: what came before
 * it is gone, the failure after it survives the clear with its whole record.
 */
static void test_a_failure_detected_after_a_clear_request_survives_the_clear(void)
{
    const uint16_t invalid = BIT(FAULT_SENSOR_CHANNEL_INVALID);
    const uint16_t no_answer = BIT(FAULT_FRONT_END_NO_ANSWER);
    fault_changes_t changes;

    apply_tagged((uint16_t)(invalid | no_answer), 0U, 0U); /* before the request */
    apply_tagged(0U, (uint16_t)(invalid | no_answer), 1U); /* after it: the clear first */
    TEST_ASSERT_EQUAL_UINT32(1U, s_monitor.clears);
    apply_tagged(no_answer, invalid, 1U);
    apply_tagged(0U, no_answer, 1U);
    TEST_ASSERT_FALSE(fault_clear_if_requested(&s_monitor, 1U)); /* carried out already */
    TEST_ASSERT_EQUAL_UINT32(1U, s_monitor.clears);

    TEST_ASSERT_EQUAL_HEX8(PASSED_FIRST, status_of(FAULT_SENSOR_CHANNEL_INVALID));
    TEST_ASSERT_EQUAL_HEX8(HEALED_WIR, status_of(FAULT_FRONT_END_NO_ANSWER));

    /* The frame of the period: the failure after the request, active (it rose in the period)
     * and confirmed, the most recent; nothing from before the request. */
    const fault_summary_t picture = summary_with(&changes);
    TEST_ASSERT_EQUAL_HEX32(no_answer, picture.active);
    TEST_ASSERT_EQUAL_HEX32(no_answer, picture.confirmed);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FAULT_FRONT_END_NO_ANSWER, picture.most_recent);
    TEST_ASSERT_EQUAL_UINT8(1U, picture.clears);
    /* For the log: both rose before the clear, the clear ended both, the front end rose after
     * it and passed again. */
    TEST_ASSERT_EQUAL_HEX32(invalid | no_answer, changes.rose);
    TEST_ASSERT_EQUAL_HEX32(invalid | no_answer, changes.fell);
    TEST_ASSERT_EQUAL_HEX32(0U, changes.failed);

    /* A request that no verdict carries is carried out when the health task gets to it, after
     * the verdicts that came before it. */
    apply_tagged(no_answer, 0U, 1U);
    TEST_ASSERT_TRUE(fault_clear_if_requested(&s_monitor, 2U));
    TEST_ASSERT_EQUAL_HEX8(UNTESTED, status_of(FAULT_FRONT_END_NO_ANSWER));
    TEST_ASSERT_EQUAL_HEX32(0U, summary().active);
}

/* FaultClearCount is the count of clears modulo 256. */
static void test_the_count_of_clears_in_the_summary_wraps_at_256(void)
{
    for (uint32_t i = 1U; i <= 257U; i++) {
        TEST_ASSERT_TRUE(fault_clear_if_requested(&s_monitor, i));
    }

    TEST_ASSERT_EQUAL_UINT32(257U, s_monitor.clears);
    TEST_ASSERT_EQUAL_UINT8(1U, summary().clears);
}

/* ---------------------------------------------------------------------------- the summary */

/*
 * A fault decided by one piece of evidence can come and go between two summaries (one health
 * period). The summary of that period has it active, and its changes say it rose, then fell;
 * the summary after that has it no longer active and no change. The other way round, a fault
 * that was active, passed and failed again in one period: active, fell then rose.
 */
static void test_a_fault_that_comes_and_goes_within_a_period_is_active_in_its_summary(void)
{
    const uint16_t mask = BIT(FAULT_FRONT_END_NO_ANSWER);
    fault_changes_t changes;

    apply(FAULT_FRONT_END_NO_ANSWER, true, 1U);
    apply(FAULT_FRONT_END_NO_ANSWER, false, 1U);
    TEST_ASSERT_EQUAL_HEX8(HEALED_WIR, status_of(FAULT_FRONT_END_NO_ANSWER));

    fault_summary_t picture = summary_with(&changes);
    TEST_ASSERT_EQUAL_HEX32(mask, picture.active);
    TEST_ASSERT_EQUAL_HEX32(mask, picture.confirmed);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FAULT_FRONT_END_NO_ANSWER, picture.most_recent);
    TEST_ASSERT_EQUAL_HEX32(mask, changes.rose);
    TEST_ASSERT_EQUAL_HEX32(mask, changes.fell);
    TEST_ASSERT_EQUAL_HEX32(0U, changes.failed);

    picture = summary_with(&changes);
    TEST_ASSERT_EQUAL_HEX32(0U, picture.active);
    TEST_ASSERT_EQUAL_HEX32(mask, picture.confirmed);
    TEST_ASSERT_EQUAL_HEX32(0U, changes.rose | changes.fell | changes.failed);

    /* Active at the start of the period, passed and failed again within it. */
    apply(FAULT_FRONT_END_NO_ANSWER, true, 1U);
    picture = summary_with(&changes);
    TEST_ASSERT_EQUAL_HEX32(mask, picture.active);
    TEST_ASSERT_EQUAL_HEX32(mask, changes.rose);
    TEST_ASSERT_EQUAL_HEX32(0U, changes.fell);
    apply(FAULT_FRONT_END_NO_ANSWER, false, 1U);
    apply(FAULT_FRONT_END_NO_ANSWER, true, 1U);
    picture = summary_with(&changes);
    TEST_ASSERT_EQUAL_HEX32(mask, picture.active);
    TEST_ASSERT_EQUAL_HEX32(mask, changes.rose & changes.fell);
    TEST_ASSERT_EQUAL_HEX32(mask, changes.failed);

    /* A pre-report that leaves testFailed alone is no change. */
    apply(FAULT_CELL_OVERVOLTAGE, true, 4U);
    (void)summary_with(&changes);
    TEST_ASSERT_EQUAL_HEX32(0U, (changes.rose | changes.fell) & BIT(FAULT_CELL_OVERVOLTAGE));
}

/*
 * A clear ends every testFailed (a fall, for the log) and forgets the rises of the period
 * before it, so that the summary of the period of a clear holds nothing from before it. A rise
 * after the clear counts, also when the fault has passed again by the summary.
 */
static void test_a_clear_forgets_the_rises_before_it_and_keeps_those_after_it(void)
{
    const uint16_t timeout = BIT(FAULT_SAMPLE_TIMEOUT);
    const uint16_t no_answer = BIT(FAULT_FRONT_END_NO_ANSWER);
    const uint16_t invalid = BIT(FAULT_SENSOR_CHANNEL_INVALID);
    fault_changes_t changes;

    TEST_ASSERT_TRUE(fault_init(&s_monitor, PLAT_RESET_WATCHDOG, false));
    (void)summary(); /* the start: the reset fault is active, not a change of the run */
    apply(FAULT_SAMPLE_TIMEOUT, true, 1U);
    apply(FAULT_FRONT_END_NO_ANSWER, true, 1U);
    apply(FAULT_FRONT_END_NO_ANSWER, false, 1U);
    TEST_ASSERT_TRUE(fault_clear_if_requested(&s_monitor, 1U));
    apply(FAULT_SENSOR_CHANNEL_INVALID, true, 1U);
    apply(FAULT_SENSOR_CHANNEL_INVALID, false, 1U);

    const fault_summary_t picture = summary_with(&changes);
    TEST_ASSERT_EQUAL_HEX32(invalid, picture.active);
    TEST_ASSERT_EQUAL_HEX32(invalid, picture.confirmed);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FAULT_SENSOR_CHANNEL_INVALID, picture.most_recent);
    TEST_ASSERT_EQUAL_HEX32(timeout | no_answer | invalid, changes.rose);
    TEST_ASSERT_EQUAL_HEX32(timeout | no_answer | invalid | BIT(FAULT_RESET_WATCHDOG),
                            changes.fell);
    TEST_ASSERT_EQUAL_HEX32(0U, changes.failed);
    TEST_ASSERT_EQUAL_HEX32(0U, summary().active);
}

/* The masks hold every fault at its bit, the most recent is the last whose testFailed rose, and
 * the indicator is requested by the faults of the pack only. */
static void test_the_summary_has_a_bit_per_fault_and_the_most_recent_one(void)
{
    apply(FAULT_FRONT_END_NO_ANSWER, true, 1U);
    health(1U, 1U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_OK, 1U);

    fault_summary_t picture = summary();
    TEST_ASSERT_EQUAL_HEX32(BIT(7) | BIT(9) | BIT(10), picture.active);
    TEST_ASSERT_EQUAL_HEX32(BIT(7) | BIT(9) | BIT(10), picture.confirmed);
    /* Bus off and error passive rose in the same call after the front end: the later of the
     * two in the order of the faults is the most recent. */
    TEST_ASSERT_EQUAL_UINT8(10U, picture.most_recent);
    TEST_ASSERT_TRUE(picture.warning);

    apply(FAULT_FRONT_END_NO_ANSWER, false, 1U);
    apply(FAULT_UNDERTEMPERATURE, true, 3U);
    picture = summary();
    TEST_ASSERT_EQUAL_HEX32(BIT(3) | BIT(9) | BIT(10), picture.active);
    TEST_ASSERT_EQUAL_HEX32(BIT(3) | BIT(7) | BIT(9) | BIT(10), picture.confirmed);
    TEST_ASSERT_EQUAL_UINT8(3U, picture.most_recent);

    /* Only the CAN faults: no indicator. */
    TEST_ASSERT_TRUE(fault_init(&s_monitor, PLAT_RESET_POWER_ON, false));
    health(1U, 1U, CAN_FC_ERROR_ACTIVE, WDGM_GLOBAL_STATUS_OK, 1U);
    TEST_ASSERT_FALSE(summary().warning);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_fault_has_its_documented_dtc_and_name);
    RUN_TEST(test_after_a_power_on_nothing_is_active_and_the_reset_faults_have_passed);
    RUN_TEST(test_a_faulty_reset_is_an_active_fault_from_the_start);
    RUN_TEST(test_a_rollback_in_the_hand_off_is_an_active_fault_from_the_start);
    RUN_TEST(test_the_cell_limits_and_their_release_values);
    RUN_TEST(test_the_temperature_limits_and_their_release_values);
    RUN_TEST(test_the_current_limits_and_their_release_values);
    RUN_TEST(test_a_reading_that_is_not_valid_is_no_evidence_about_a_limit);
    RUN_TEST(test_several_channels_beyond_their_limits_are_one_verdict);
    RUN_TEST(test_only_the_groups_of_the_sample_are_judged);
    RUN_TEST(test_a_monitor_that_was_never_set_up_reports_and_clears_nothing);
    RUN_TEST(test_an_invalid_channel_of_an_answered_group_fails_and_all_valid_passes);
    RUN_TEST(test_a_group_without_an_answer_fails_the_front_end_and_answers_pass_it);
    RUN_TEST(test_a_sample_passes_the_timeout_and_a_timeout_fails_only_it);
    RUN_TEST(test_an_over_voltage_matures_after_five_readings_and_heals_after_ten);
    RUN_TEST(test_every_sample_fault_matures_and_heals_after_its_debounce_count);
    RUN_TEST(test_a_verdict_reports_each_of_its_faults_once);
    RUN_TEST(test_judged_samples_make_the_dtcs);
    RUN_TEST(test_a_bus_off_is_set_at_once_and_heals_after_fifty_cycles_without_one);
    RUN_TEST(test_error_passive_is_set_at_once_and_heals_after_ten_cycles_error_active);
    RUN_TEST(test_the_supervision_fault_follows_the_global_status);
    RUN_TEST(test_a_new_clear_request_clears_every_dtc_once);
    RUN_TEST(test_a_failure_detected_after_a_clear_request_survives_the_clear);
    RUN_TEST(test_the_count_of_clears_in_the_summary_wraps_at_256);
    RUN_TEST(test_the_summary_has_a_bit_per_fault_and_the_most_recent_one);
    RUN_TEST(test_a_fault_that_comes_and_goes_within_a_period_is_active_in_its_summary);
    RUN_TEST(test_a_clear_forgets_the_rises_before_it_and_keeps_those_after_it);
    return UNITY_END();
}
