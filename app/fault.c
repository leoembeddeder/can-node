/* SPDX-License-Identifier: MIT */
#include "fault.h"

#include <stddef.h>
#include <string.h>

/*
 * THE TABLE OF FAULTS: per fault its DTC, its debouncing (lib/dtc, the Dem's counter-based
 * algorithm: the counter goes up by step_up for every piece of failing evidence and down by
 * step_down for every piece of passing evidence; at failed_threshold the test has failed, at
 * passed_threshold it has passed) and whether it requests the warning indicator.
 *
 * Common to all:
 *   - confirmation_threshold 1: in phase 3 one run of the node is one operation cycle and the
 *     DTCs live in RAM, so a higher threshold could never be reached (lib/dtc/dtc.h, THE
 *     OPERATION CYCLE). confirmedDTC therefore means "occurred since the start or the last
 *     clear", and BMS_Fault reports it as such (FaultConfirmed);
 *   - warning_indicator for the faults of the pack's limits and of its measurement (0..8):
 *     they are about the safety of the module, which a pack master shows to the driver; the
 *     others are about the node and its network;
 *   - a threshold of 1 (or -1) means that the first piece of evidence decides. lib/dtc has no
 *     jump up (dtc.h): a PREFAILED below 0 only moves the counter towards 0, so after a pass a
 *     failed threshold of 1 would take two failing reports. Evidence for a fault whose failed
 *     threshold is 1 is therefore reported as the matured verdict, DTC_REPORT_FAILED, which
 *     sets the counter to the threshold from wherever it is, and likewise passing evidence
 *     with a passed threshold of -1 as DTC_REPORT_PASSED (report() below);
 *   - jump_down where a fault is set at once but heals over several reports (bus off, error
 *     passive): the first passing report then starts from 0. The limits have none, and their
 *     counter runs between -N and N: from the start N readings decide, after that 2N net
 *     readings move it from one verdict to the other, so that a value that goes beyond its
 *     limit again and again for less than the debounce time each time still adds up to a
 *     fault.
 *
 * THE LIMITS assume a module of eight NMC/graphite Li-ion cells, the chemistry the sensor
 * model's open-circuit voltage of 3.70 V stands for (docs/sensor-model.md). The values are
 * those typical of such cells' datasheets; a product takes them from the datasheet of its own
 * cell. The model's values without a fault stay inside every release value, so that a run
 * without a fault has no DTC: cells 3359 to 3845 mV, temperatures 24.5 to 25.6 degC, current
 * -80 A to 160 A.
 */
static const dtc_event_config_t s_events[FAULT_EVENTS] = {
    /* A cell above 4250 mV. The end-of-charge voltage of the chemistry is 4.20 V; 50 mV above
     * it, beyond the error of a cell-monitor IC (a few mV), is where a charger that has gone
     * wrong is told from one that has merely reached the end of charge. Released at 4200 mV.
     * Debounce: the counter runs between -5 and 5, one step per cell reading (20 ms): from the
     * start 5 readings beyond the limit (100 ms) set it, after a pass 10, and 10 within the
     * release value heal it. One disturbed conversion cannot set it. */
    [FAULT_CELL_OVERVOLTAGE] = {.dtc = 0x1A0100U,
                                .failed_threshold = 5,
                                .passed_threshold = -5,
                                .step_up = 1U,
                                .step_down = 1U,
                                .confirmation_threshold = 1U,
                                .jump_down = false,
                                .warning_indicator = true},
    /* A cell below 2500 mV, the discharge cut-off of the chemistry: below it the cell takes
     * damage (its copper current collector begins to dissolve). Released at 2600 mV, 100 mV of
     * hysteresis. Debounce as for the over-voltage. */
    [FAULT_CELL_UNDERVOLTAGE] = {.dtc = 0x1A0200U,
                                 .failed_threshold = 5,
                                 .passed_threshold = -5,
                                 .step_up = 1U,
                                 .step_down = 1U,
                                 .confirmation_threshold = 1U,
                                 .jump_down = false,
                                 .warning_indicator = true},
    /* A temperature above 60.0 degC, the upper end of the discharge range of such cells (-20
     * to 60 degC); above it they age fast. Released at 55.0 degC. Debounce: -3 to 3 at a
     * reading every 100 ms, 3 readings (300 ms) from the start, 6 after that; a module's
     * temperature changes over minutes, so the debounce only has to outlast a disturbed
     * conversion. */
    [FAULT_OVERTEMPERATURE] = {.dtc = 0x1A0300U,
                               .failed_threshold = 3,
                               .passed_threshold = -3,
                               .step_up = 1U,
                               .step_down = 1U,
                               .confirmation_threshold = 1U,
                               .jump_down = false,
                               .warning_indicator = true},
    /* A temperature below -20.0 degC, the lower end of the discharge range. Released at -15.0
     * degC. Charging has a narrower range (typically 0 to 45 degC: charging below 0 degC plates
     * lithium on the anode), which these limits do not tell apart from discharging. Debounce as
     * for the over-temperature. */
    [FAULT_UNDERTEMPERATURE] = {.dtc = 0x1A0400U,
                                .failed_threshold = 3,
                                .passed_threshold = -3,
                                .step_up = 1U,
                                .step_down = 1U,
                                .confirmation_threshold = 1U,
                                .jump_down = false,
                                .warning_indicator = true},
    /* A discharge current above 250 A. The sensor model gives the module no capacity; its
     * profile (45 A steady, 160 A for 1.5 s, 80 A of regenerative charge) is that of cells of
     * about 50 Ah, and 250 A is 5C, the peak discharge rate typical of high-power NMC cells.
     * Released at 225 A, 10 % below. Debounce: -50 to 50 at a reading every 10 ms, 50 readings
     * (500 ms) from the start, 100 after that: this is the limit for a current above the cells'
     * peak rating that lasts; a short circuit is a matter for the hardware's own protection,
     * which acts in microseconds. */
    [FAULT_OVERCURRENT_DISCHARGE] = {.dtc = 0x1A0500U,
                                     .failed_threshold = 50,
                                     .passed_threshold = -50,
                                     .step_up = 1U,
                                     .step_down = 1U,
                                     .confirmation_threshold = 1U,
                                     .jump_down = false,
                                     .warning_indicator = true},
    /* A charge current above 100 A, 2C for the same cells: the peak charge rate typical of
     * them (a cell takes charge more slowly than it gives it). Released at 90 A. Debounce as
     * for the discharge. */
    [FAULT_OVERCURRENT_CHARGE] = {.dtc = 0x1A0600U,
                                  .failed_threshold = 50,
                                  .passed_threshold = -50,
                                  .step_up = 1U,
                                  .step_down = 1U,
                                  .confirmation_threshold = 1U,
                                  .jump_down = false,
                                  .warning_indicator = true},
    /* A channel that the front end flags invalid (an open sense wire) or whose reading lies
     * outside what its sensor can deliver (lib/bms). The pack state makes the channel invalid
     * with the first such reading (SNA on the bus, PackState FAULT), and the DTC follows it:
     * set at the first, healed at the first sample in which every channel is valid. */
    [FAULT_SENSOR_CHANNEL_INVALID] = {.dtc = 0x1A1000U,
                                      .failed_threshold = 1,
                                      .passed_threshold = -1,
                                      .step_up = 1U,
                                      .step_down = 1U,
                                      .confirmation_threshold = 1U,
                                      .jump_down = true,
                                      .warning_indicator = true},
    /* The front end did not answer a read (plat_sensor.h: the read returned false). Set at the
     * first, healed at the first sample whose reads were all answered, as the pack state does
     * with the channels. */
    [FAULT_FRONT_END_NO_ANSWER] = {.dtc = 0x1A1100U,
                                   .failed_threshold = 1,
                                   .passed_threshold = -1,
                                   .step_up = 1U,
                                   .step_down = 1U,
                                   .confirmation_threshold = 1U,
                                   .jump_down = true,
                                   .warning_indicator = true},
    /* The processing task waited APP_SAMPLE_TIMEOUT_MS (three sensor periods) for a sample in
     * vain: the sensor task has stopped delivering. The timeout is the debounce: set at the
     * first, healed at the next sample. */
    [FAULT_SAMPLE_TIMEOUT] = {.dtc = 0x1A1200U,
                              .failed_threshold = 1,
                              .passed_threshold = -1,
                              .step_up = 1U,
                              .step_down = 1U,
                              .confirmation_threshold = 1U,
                              .jump_down = true,
                              .warning_indicator = true},
    /* The controller went bus off. Bus off is already a matured verdict of the CAN fault
     * confinement (a transmit error counter beyond 255), so it is set at once. Healed after
     * 50 health cycles in a row without a bus-off, 5 s: the stable time of the transmit queue
     * (can_io.c, TX_STABLE_MS), after which it too takes the bus as healthy again and returns
     * its recovery hold-off to the initial value. A fault that keeps taking the node bus off
     * keeps the DTC set through every recovery. */
    [FAULT_CAN_BUS_OFF] = {.dtc = 0xD00100U,
                           .failed_threshold = 1,
                           .passed_threshold = -50,
                           .step_up = 1U,
                           .step_down = 1U,
                           .confirmation_threshold = 1U,
                           .jump_down = true,
                           .warning_indicator = false},
    /* The controller went error passive (an error counter above 127): the bus has errors, or
     * nobody acknowledges the node's frames. It still communicates, but can no longer signal
     * errors actively and must wait before every transmission, and a node that stays error
     * passive does so because of a wiring or transceiver fault: worth a DTC. Set at once (the
     * error counters are the controller's own debouncing); healed after 10 health cycles in a
     * row error active, 1 s, in which the node sent every message of up to 1 s cycle time. */
    [FAULT_CAN_ERROR_PASSIVE] = {.dtc = 0xD00200U,
                                 .failed_threshold = 1,
                                 .passed_threshold = -10,
                                 .step_up = 1U,
                                 .step_down = 1U,
                                 .confirmation_threshold = 1U,
                                 .jump_down = true,
                                 .warning_indicator = false},
    /* The watchdog manager found a supervised task beyond its tolerance (global status EXPIRED
     * or STOPPED, lib/wdgm). The tolerances are the debouncing, so it is set at once; it does
     * not heal, because EXPIRED is final and the hardware watchdog resets the node. */
    [FAULT_SUPERVISION_EXPIRED] = {.dtc = 0x1B0100U,
                                   .failed_threshold = 1,
                                   .passed_threshold = -1,
                                   .step_up = 1U,
                                   .step_down = 1U,
                                   .confirmation_threshold = 1U,
                                   .jump_down = true,
                                   .warning_indicator = false},
    /* The last reset was one of the hardware watchdog, of a failed assertion, of a stack
     * overflow or of a failed allocation (plat_reset_cause()): the previous run ended in a
     * fault. Judged once per run, at its start, so set or passed with the one report: the
     * result of that test stands for the whole run, and with it PackFaultActive and PackState
     * FAULT, until a tester clears the DTCs. A node that was reset by a fault tells the pack's
     * master so until the master has taken note; the clear removes it for the rest of the run. */
    [FAULT_RESET_WATCHDOG] = {.dtc = 0x1B0200U,
                              .failed_threshold = 1,
                              .passed_threshold = -1,
                              .step_up = 1U,
                              .step_down = 1U,
                              .confirmation_threshold = 1U,
                              .jump_down = true,
                              .warning_indicator = false},
    [FAULT_RESET_ASSERT] = {.dtc = 0x1B0300U,
                            .failed_threshold = 1,
                            .passed_threshold = -1,
                            .step_up = 1U,
                            .step_down = 1U,
                            .confirmation_threshold = 1U,
                            .jump_down = true,
                            .warning_indicator = false},
    [FAULT_RESET_STACK_OVERFLOW] = {.dtc = 0x1B0400U,
                                    .failed_threshold = 1,
                                    .passed_threshold = -1,
                                    .step_up = 1U,
                                    .step_down = 1U,
                                    .confirmation_threshold = 1U,
                                    .jump_down = true,
                                    .warning_indicator = false},
    [FAULT_RESET_MALLOC_FAILED] = {.dtc = 0x1B0500U,
                                   .failed_threshold = 1,
                                   .passed_threshold = -1,
                                   .step_up = 1U,
                                   .step_down = 1U,
                                   .confirmation_threshold = 1U,
                                   .jump_down = true,
                                   .warning_indicator = false},
    /* The bootloader rolled back before this start (its hand-off, plat_retained.h): a trial
     * that used its three attempts without confirming itself, a trial that did not validate, or
     * a confirmed image that stopped validating. The node now runs the image it was meant to
     * replace, or the one before it, and the update that was tried has failed: the tester has
     * to know, and the pack's master too (the image may be an old one). Judged once per run, at
     * its start, like the reset faults and for the same reason: the hand-off speaks only to the
     * start right after the rollback, so the one report decides, and the DTC stands for the run
     * until a tester clears it. It is a fault of the node's software, not of the module: no
     * warning indicator. */
    [FAULT_UPDATE_ROLLBACK] = {.dtc = 0x1B0600U,
                               .failed_threshold = 1,
                               .passed_threshold = -1,
                               .step_up = 1U,
                               .step_down = 1U,
                               .confirmation_threshold = 1U,
                               .jump_down = true,
                               .warning_indicator = false}};

static const char *const s_names[FAULT_EVENTS] = {
    [FAULT_CELL_OVERVOLTAGE] = "CELL_OVERVOLTAGE",
    [FAULT_CELL_UNDERVOLTAGE] = "CELL_UNDERVOLTAGE",
    [FAULT_OVERTEMPERATURE] = "OVERTEMPERATURE",
    [FAULT_UNDERTEMPERATURE] = "UNDERTEMPERATURE",
    [FAULT_OVERCURRENT_DISCHARGE] = "OVERCURRENT_DISCHARGE",
    [FAULT_OVERCURRENT_CHARGE] = "OVERCURRENT_CHARGE",
    [FAULT_SENSOR_CHANNEL_INVALID] = "SENSOR_CHANNEL_INVALID",
    [FAULT_FRONT_END_NO_ANSWER] = "FRONT_END_NO_ANSWER",
    [FAULT_SAMPLE_TIMEOUT] = "SAMPLE_TIMEOUT",
    [FAULT_CAN_BUS_OFF] = "CAN_BUS_OFF",
    [FAULT_CAN_ERROR_PASSIVE] = "CAN_ERROR_PASSIVE",
    [FAULT_SUPERVISION_EXPIRED] = "SUPERVISION_EXPIRED",
    [FAULT_RESET_WATCHDOG] = "RESET_WATCHDOG",
    [FAULT_RESET_ASSERT] = "RESET_ASSERT",
    [FAULT_RESET_STACK_OVERFLOW] = "RESET_STACK_OVERFLOW",
    [FAULT_RESET_MALLOC_FAILED] = "RESET_MALLOC_FAILED",
    [FAULT_UPDATE_ROLLBACK] = "UPDATE_ROLLBACK"};

/* The faults the processing task judges are the first ones, up to SAMPLE_TIMEOUT: the masks
 * of a verdict hold exactly those. */
#define SAMPLE_FAULTS ((uint32_t)FAULT_SAMPLE_TIMEOUT + 1U)
#define SAMPLE_MASK   0x01FFU

_Static_assert(SAMPLE_MASK == ((uint32_t)UINT16_MAX >> (16U - SAMPLE_FAULTS)),
               "a bit for each sample fault");

/* The limits and their release values lie on the right side of each other and inside what the
 * sensors can deliver (lib/bms), so that every limit can be reached by a valid reading. */
_Static_assert(FAULT_CELL_OV_RELEASE_MV < FAULT_CELL_OV_MV, "over-voltage: release below");
_Static_assert(FAULT_CELL_UV_RELEASE_MV > FAULT_CELL_UV_MV, "under-voltage: release above");
_Static_assert(FAULT_CELL_OV_MV < BMS_CELL_SENSOR_MAX_MV, "over-voltage: inside the sensor");
_Static_assert(FAULT_CELL_UV_RELEASE_MV < FAULT_CELL_OV_RELEASE_MV, "the voltage window");
_Static_assert(FAULT_TEMP_OT_RELEASE_DC < FAULT_TEMP_OT_DC, "over-temperature: release below");
_Static_assert(FAULT_TEMP_UT_RELEASE_DC > FAULT_TEMP_UT_DC, "under-temperature: release above");
_Static_assert(FAULT_TEMP_OT_DC < BMS_TEMP_SENSOR_MAX_DC, "over-temperature: inside the sensor");
_Static_assert(FAULT_TEMP_UT_DC > BMS_TEMP_SENSOR_MIN_DC, "under-temperature: inside the sensor");
_Static_assert(FAULT_CURRENT_OCD_RELEASE_MA < FAULT_CURRENT_OCD_MA, "discharge: release below");
_Static_assert(FAULT_CURRENT_OCC_RELEASE_MA > FAULT_CURRENT_OCC_MA, "charge: release above");
_Static_assert(FAULT_CURRENT_OCD_MA < BMS_CURRENT_SENSOR_MAX_MA, "discharge: inside the sensor");
_Static_assert(FAULT_CURRENT_OCC_MA > -BMS_CURRENT_SENSOR_MAX_MA, "charge: inside the sensor");
_Static_assert((sizeof(s_events) / sizeof(s_events[0])) == (size_t)FAULT_EVENTS, "one per fault");
_Static_assert((sizeof(s_names) / sizeof(s_names[0])) == (size_t)FAULT_EVENTS, "one name each");

/* The bit of a fault in the monitor's and the summary's masks. */
static uint32_t bit_of(fault_event_t event)
{
    return (uint32_t)(1UL << (uint32_t)event);
}

/* The bit of one of the processing task's faults (0..FAULT_SAMPLE_TIMEOUT) in a verdict. */
static uint16_t sample_bit(fault_event_t event)
{
    return (uint16_t)(1UL << (uint32_t)event);
}

static bool channel_valid(uint8_t mask, uint32_t channel)
{
    return (((uint32_t)mask >> channel) & 1U) != 0U;
}

/* Evidence for a fault from a group: failing if `beyond`, passing if every channel of the
 * group is valid and `within`, otherwise none. */
static void evidence(fault_event_t event, bool beyond, bool within, bool all_valid,
                     fault_verdict_t *verdict)
{
    if (beyond) {
        verdict->failing = (uint16_t)(verdict->failing | sample_bit(event));
    } else if (all_valid && within) {
        verdict->passing = (uint16_t)(verdict->passing | sample_bit(event));
    } else {
        /* not judged */
    }
}

static void judge_cells(const bms_sample_t *sample, const bms_pack_t *pack,
                        fault_verdict_t *verdict)
{
    bool over = false;
    bool under = false;
    bool below_release = true;
    bool above_release = true;

    for (uint32_t cell = 0U; cell < BMS_CELLS; cell++) {
        if (channel_valid(pack->cell_valid, cell)) {
            const uint16_t mv = sample->cell_mv[cell];

            over = over || (mv > FAULT_CELL_OV_MV);
            under = under || (mv < FAULT_CELL_UV_MV);
            below_release = below_release && (mv <= FAULT_CELL_OV_RELEASE_MV);
            above_release = above_release && (mv >= FAULT_CELL_UV_RELEASE_MV);
        }
    }

    const bool all_valid = (pack->cell_valid == (uint8_t)BMS_ALL_CELLS);

    evidence(FAULT_CELL_OVERVOLTAGE, over, below_release, all_valid, verdict);
    evidence(FAULT_CELL_UNDERVOLTAGE, under, above_release, all_valid, verdict);
}

static void judge_temps(const bms_sample_t *sample, const bms_pack_t *pack,
                        fault_verdict_t *verdict)
{
    bool over = false;
    bool under = false;
    bool below_release = true;
    bool above_release = true;

    for (uint32_t sensor = 0U; sensor < BMS_TEMPS; sensor++) {
        if (channel_valid(pack->temp_valid, sensor)) {
            const int16_t dc = sample->temp_dc[sensor];

            over = over || (dc > FAULT_TEMP_OT_DC);
            under = under || (dc < FAULT_TEMP_UT_DC);
            below_release = below_release && (dc <= FAULT_TEMP_OT_RELEASE_DC);
            above_release = above_release && (dc >= FAULT_TEMP_UT_RELEASE_DC);
        }
    }

    const bool all_valid = (pack->temp_valid == (uint8_t)BMS_ALL_TEMPS);

    evidence(FAULT_OVERTEMPERATURE, over, below_release, all_valid, verdict);
    evidence(FAULT_UNDERTEMPERATURE, under, above_release, all_valid, verdict);
}

static void judge_current(const bms_sample_t *sample, const bms_pack_t *pack,
                          fault_verdict_t *verdict)
{
    const bool valid = pack->current_valid;
    const int32_t ma = sample->current_ma;

    evidence(FAULT_OVERCURRENT_DISCHARGE, valid && (ma > FAULT_CURRENT_OCD_MA),
             ma <= FAULT_CURRENT_OCD_RELEASE_MA, valid, verdict);
    evidence(FAULT_OVERCURRENT_CHARGE, valid && (ma < FAULT_CURRENT_OCC_MA),
             ma >= FAULT_CURRENT_OCC_RELEASE_MA, valid, verdict);
}

/* A group the front end answered in this sample that has an invalid channel in the pack. */
static bool answered_group_invalid(uint8_t answered, const bms_pack_t *pack)
{
    const bool cells =
        ((answered & BMS_SAMPLE_CELLS) != 0U) && (pack->cell_valid != (uint8_t)BMS_ALL_CELLS);
    const bool temps =
        ((answered & BMS_SAMPLE_TEMPS) != 0U) && (pack->temp_valid != (uint8_t)BMS_ALL_TEMPS);
    const bool current = ((answered & BMS_SAMPLE_CURRENT) != 0U) && !pack->current_valid;

    return cells || temps || current;
}

static bool every_channel_valid(const bms_pack_t *pack)
{
    return (pack->cell_valid == (uint8_t)BMS_ALL_CELLS) &&
           (pack->temp_valid == (uint8_t)BMS_ALL_TEMPS) && pack->current_valid;
}

void fault_judge_sample(const bms_sample_t *sample, uint8_t unanswered, const bms_pack_t *pack,
                        fault_verdict_t *verdict)
{
    const uint8_t groups =
        (uint8_t)(sample->groups & (BMS_SAMPLE_CURRENT | BMS_SAMPLE_CELLS | BMS_SAMPLE_TEMPS));
    const uint8_t answered = (uint8_t)(groups & (uint8_t)~unanswered);

    verdict->failing = 0U;
    verdict->passing = sample_bit(FAULT_SAMPLE_TIMEOUT);
    verdict->clear_requests = 0U;

    if ((groups & BMS_SAMPLE_CELLS) != 0U) {
        judge_cells(sample, pack, verdict);
    }

    if ((groups & BMS_SAMPLE_TEMPS) != 0U) {
        judge_temps(sample, pack, verdict);
    }

    if ((groups & BMS_SAMPLE_CURRENT) != 0U) {
        judge_current(sample, pack, verdict);
    }

    evidence(FAULT_SENSOR_CHANNEL_INVALID, answered_group_invalid(answered, pack), true,
             every_channel_valid(pack), verdict);
    evidence(FAULT_FRONT_END_NO_ANSWER, (groups & unanswered) != 0U, true, true, verdict);
}

void fault_judge_timeout(fault_verdict_t *verdict)
{
    verdict->failing = sample_bit(FAULT_SAMPLE_TIMEOUT);
    verdict->passing = 0U;
    verdict->clear_requests = 0U;
}

/* After a report of `event`: its testFailed as lib/dtc has it now, and the edge, if any, for
 * the summary of the period (fault.h, THE SUMMARY OF A PERIOD). */
static void note_test_failed(fault_monitor_t *monitor, fault_event_t event)
{
    const uint32_t mask = bit_of(event);
    const bool was = (monitor->failed & mask) != 0U;
    dtc_entry_t entry;
    bool is = false;

    if (dtc_get(&monitor->dtc, (uint16_t)event, &entry)) {
        is = (entry.status & DTC_STATUS_TF) != 0U;
    }

    if (is && !was) {
        monitor->failed |= mask;
        monitor->rose |= mask;
        monitor->latched |= mask;
    } else if (was && !is) {
        monitor->failed &= ~mask;
        monitor->fell |= mask;
    } else {
        /* no change of testFailed */
    }
}

/* One piece of evidence for one fault, or nothing: PREFAILED or PREPASSED, or the matured
 * FAILED or PASSED for a threshold of 1 or -1 (see THE TABLE OF FAULTS). */
static void report(fault_monitor_t *monitor, fault_event_t event, bool failing, bool passing)
{
    const dtc_event_config_t *config = &s_events[event];

    if (failing) {
        (void)dtc_report(&monitor->dtc, (uint16_t)event,
                         (config->failed_threshold == 1) ? DTC_REPORT_FAILED
                                                         : DTC_REPORT_PREFAILED);
        note_test_failed(monitor, event);
    } else if (passing) {
        (void)dtc_report(&monitor->dtc, (uint16_t)event,
                         (config->passed_threshold == -1) ? DTC_REPORT_PASSED
                                                          : DTC_REPORT_PREPASSED);
        note_test_failed(monitor, event);
    } else {
        /* not judged */
    }
}

/* Whether the receiver's count of clear requests `requests` is ahead of `carried_out`, the
 * count the monitor has carried out. The count only grows, modulo 2^32, and the monitor is never
 * 2^31 requests behind it, so a difference of 1 to 2^31 - 1 is ahead and any other, behind or
 * equal. */
static bool newer_requests(uint32_t requests, uint32_t carried_out)
{
    const uint32_t ahead = requests - carried_out;

    return (ahead != 0U) && (ahead < 0x80000000UL);
}

bool fault_init(fault_monitor_t *monitor, plat_reset_reason_t cause, bool rolled_back)
{
    static const struct {
        fault_event_t event;
        plat_reset_reason_t reason;
    } resets[] = {{FAULT_RESET_WATCHDOG, PLAT_RESET_WATCHDOG},
                  {FAULT_RESET_ASSERT, PLAT_RESET_ASSERT},
                  {FAULT_RESET_STACK_OVERFLOW, PLAT_RESET_STACK_OVERFLOW},
                  {FAULT_RESET_MALLOC_FAILED, PLAT_RESET_MALLOC_FAILED}};
    bool ok;

    (void)memset(monitor, 0, sizeof(*monitor));
    monitor->reset_cause = cause;
    monitor->rolled_back = rolled_back;
    ok = dtc_init(&monitor->dtc, s_events, (uint16_t)FAULT_EVENTS);

    for (size_t i = 0U; ok && (i < (sizeof(resets) / sizeof(resets[0]))); i++) {
        const bool this_one = (cause == resets[i].reason);

        report(monitor, resets[i].event, this_one, !this_one);
    }

    if (ok) {
        report(monitor, FAULT_UPDATE_ROLLBACK, rolled_back, !rolled_back);
    }

    return ok;
}

void fault_apply(fault_monitor_t *monitor, const fault_verdict_t *verdict)
{
    const uint16_t failing = (uint16_t)(verdict->failing & SAMPLE_MASK);
    const uint16_t passing = (uint16_t)(verdict->passing & SAMPLE_MASK & (uint16_t)~failing);

    /* A request the processing task had seen when it handed the verdict over is carried out
     * before the verdict's evidence, so that the clear erases what came before the request
     * and nothing after it. */
    (void)fault_clear_if_requested(monitor, verdict->clear_requests);

    for (uint32_t event = 0U; event < SAMPLE_FAULTS; event++) {
        const uint16_t mask = (uint16_t)(1UL << event);

        report(monitor, (fault_event_t)event, (failing & mask) != 0U, (passing & mask) != 0U);
    }

    monitor->verdicts++;
}

void fault_health_cycle(fault_monitor_t *monitor, const fault_health_inputs_t *inputs)
{
    const bool bus_off =
        (inputs->bus_off_count != monitor->bus_off_count) || (inputs->can_state == CAN_FC_BUS_OFF);
    const bool passive = (inputs->error_passive_entries != monitor->error_passive_entries) ||
                         (inputs->can_state != CAN_FC_ERROR_ACTIVE);
    const bool expired = (inputs->supervision == WDGM_GLOBAL_STATUS_EXPIRED) ||
                         (inputs->supervision == WDGM_GLOBAL_STATUS_STOPPED);
    const bool supervised = (inputs->supervision == WDGM_GLOBAL_STATUS_OK) ||
                            (inputs->supervision == WDGM_GLOBAL_STATUS_FAILED);

    report(monitor, FAULT_CAN_BUS_OFF, bus_off, !bus_off);
    report(monitor, FAULT_CAN_ERROR_PASSIVE, passive, !passive);
    report(monitor, FAULT_SUPERVISION_EXPIRED, expired, supervised);

    monitor->bus_off_count = inputs->bus_off_count;
    monitor->error_passive_entries = inputs->error_passive_entries;
    monitor->health_cycles++;
}

bool fault_clear_if_requested(fault_monitor_t *monitor, uint32_t requests)
{
    bool cleared = false;

    if (newer_requests(requests, monitor->clear_requests)) {
        monitor->clear_requests = requests;
        cleared = dtc_clear_all(&monitor->dtc);

        if (cleared) {
            /* Every testFailed ends here; the rises before the clear stay in the log's edges
             * but no longer count for the summary. */
            monitor->fell |= monitor->failed;
            monitor->failed = 0U;
            monitor->latched = 0U;
            monitor->clears++;
        }
    }

    return cleared;
}

void fault_summary(fault_monitor_t *monitor, fault_summary_t *summary, fault_changes_t *changes)
{
    dtc_entry_t entry;

    (void)memset(summary, 0, sizeof(*summary));

    for (uint32_t event = 0U; event < (uint32_t)FAULT_EVENTS; event++) {
        if (dtc_get(&monitor->dtc, (uint16_t)event, &entry)) {
            const uint32_t mask = bit_of((fault_event_t)event);

            if (((entry.status & DTC_STATUS_TF) != 0U) || ((monitor->latched & mask) != 0U)) {
                summary->active |= mask;
            }

            if ((entry.status & DTC_STATUS_CDTC) != 0U) {
                summary->confirmed |= mask;
            }

            if ((entry.status & DTC_STATUS_WIR) != 0U) {
                summary->warning = true;
            }
        }
    }

    summary->most_recent = (uint8_t)FAULT_NONE;

    if (dtc_recorded(&monitor->dtc, DTC_RECORD_MOST_RECENT_FAILED, &entry)) {
        summary->most_recent = (uint8_t)entry.event;
    }

    summary->clears = (uint8_t)(monitor->clears & 0xFFU);

    changes->rose = monitor->rose;
    changes->fell = monitor->fell;
    changes->failed = monitor->failed;
    monitor->rose = 0U;
    monitor->fell = 0U;
    monitor->latched = 0U;
}

const char *fault_name(uint32_t event)
{
    const char *name = "unknown";

    if (event < (uint32_t)FAULT_EVENTS) {
        name = s_names[event];
    }

    return name;
}

uint32_t fault_dtc_number(uint32_t event)
{
    uint32_t dtc = 0U;

    if (event < (uint32_t)FAULT_EVENTS) {
        dtc = s_events[event].dtc;
    }

    return dtc;
}
