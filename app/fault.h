/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The fault monitor: every fault the node can detect, as an event of lib/dtc with its DTC, its
 * limits and its debouncing in one table (fault.c), the monitors that judge the evidence, and
 * the summary the node publishes (BMS_Fault, PackFaultActive).
 *
 * THE FAULTS. One event per fault; its number is its bit in BMS_Fault's FaultActive and
 * FaultConfirmed and its entry in the value table of FaultMostRecent (dbc/fcn.dbc;
 * telemetry.c checks every number against the generated header). The DTC numbers are the
 * project's: the two high bytes a code in the SAE J2012 format, in the range a manufacturer
 * defines (P1Axx the battery module, U1xxx the network, P1Bxx the control module itself), the
 * low byte, the failure type, 0x00 (no subtype information) for every one, so that every fault
 * has a code of its own.
 *
 *   n   fault                    DTC        J2012      judged by       evidence
 *   0   CELL_OVERVOLTAGE         0x1A0100   P1A01-00   process task    every sample with cells
 *   1   CELL_UNDERVOLTAGE        0x1A0200   P1A02-00   process task    every sample with cells
 *   2   OVERTEMPERATURE          0x1A0300   P1A03-00   process task    every sample with temps
 *   3   UNDERTEMPERATURE         0x1A0400   P1A04-00   process task    every sample with temps
 *   4   OVERCURRENT_DISCHARGE    0x1A0500   P1A05-00   process task    every sample
 *   5   OVERCURRENT_CHARGE       0x1A0600   P1A06-00   process task    every sample
 *   6   SENSOR_CHANNEL_INVALID   0x1A1000   P1A10-00   process task    every sample
 *   7   FRONT_END_NO_ANSWER      0x1A1100   P1A11-00   process task    every sample
 *   8   SAMPLE_TIMEOUT           0x1A1200   P1A12-00   process task    every sample, every timeout
 *   9   CAN_BUS_OFF              0xD00100   U1001-00   health task     every health cycle
 *   10  CAN_ERROR_PASSIVE        0xD00200   U1002-00   health task     every health cycle
 *   11  SUPERVISION_EXPIRED      0x1B0100   P1B01-00   health task     every health cycle
 *   12  RESET_WATCHDOG           0x1B0200   P1B02-00   fault_init()    once, at the start
 *   13  RESET_ASSERT             0x1B0300   P1B03-00   fault_init()    once, at the start
 *   14  RESET_STACK_OVERFLOW     0x1B0400   P1B04-00   fault_init()    once, at the start
 *   15  RESET_MALLOC_FAILED      0x1B0500   P1B05-00   fault_init()    once, at the start
 *   16  UPDATE_ROLLBACK          0x1B0600   P1B06-00   fault_init()    once, at the start
 *
 * UPDATE_ROLLBACK comes from the bootloader's hand-off (plat_retained.h): the bootloader
 * rolled back before this start, so the node runs an image other than the one it was to run
 * (a trial that used its attempts or did not validate, or a confirmed image that stopped
 * validating). The hand-off says so only to the start right after the rollback, as the reset
 * cause speaks only to the start after the reset.
 *
 * Not a fault of the node, and so no DTC: a TESTER_Command stream that times out. The tester is
 * another node; that it went silent says nothing about this one, which keeps the last command
 * it carried out and goes on working (command.h). The state machine of the stream is in the
 * statistics ("command.sm_state").
 *
 * WHO DOES WHAT. The DTC state (lib/dtc) has one owner, the health task: every call on a
 * fault_monitor_t is made there (in the node, app.c), with one exception, fault_init(), which
 * runs before the scheduler starts. The other tasks never touch it:
 *   - the processing task judges each sample with fault_judge_sample() (and each timeout of
 *     its wait for one with fault_judge_timeout()), functions without state, and hands the
 *     verdict to the health task through a queue; the health task works the verdicts into the
 *     DTCs in the order of the samples (fault_apply()), so that the debouncing counts samples.
 *     A fault of the pack values therefore reaches its DTC in the health task's first period
 *     after the sample that decided it, at most one health period (100 ms) later;
 *   - the health task judges the CAN controller and the watchdog manager itself, once per
 *     cycle (fault_health_cycle());
 *   - the receive task counts the clear requests of TESTER_Command (command.h); the health task
 *     carries them out (fault_clear_if_requested()), in order with the evidence: the
 *     processing task tags every verdict with the count of requests when it posts it, and a
 *     verdict that carries a request not yet carried out clears the DTCs before it is worked
 *     in (fault_apply()). Evidence handed over after a request was counted survives its
 *     clear; evidence handed over before it does not;
 *   - a task that wants to know the state reads the summary (fault_summary_t) that the health
 *     task publishes after every cycle (in the node, PackFaultActive of the processing task).
 * The DTCs change only in the health task's job, and the health task sends BMS_Fault at the
 * end of every job, after the last change: the frame goes out at once on every change, and
 * every 100 ms without one. A fault whose testFailed rose in the job is active in that job's
 * summary even if it has passed again since (THE SUMMARY OF A PERIOD below), so that a fault
 * decided by one piece of evidence that comes and goes within one job still reaches
 * FaultActive, PackFaultActive and the log.
 *
 * THE SUMMARY OF A PERIOD. The monitor keeps, between two calls of fault_summary(), which DTCs
 * had testFailed rise (0 to 1) and fall (1 to 0, by a pass or by a clear). The summary reports
 * a fault active if its testFailed is set at the end of the period or rose during it, and hands
 * the edges to the caller for its log (fault_changes_t); then it forgets them. A clear forgets
 * the rises before it for the summary, so that the summary of a period with a clear holds
 * nothing from before the clear; the rises after the clear are kept.
 *
 * WHAT THE NODE DOES ABOUT A FAULT IN PHASE 3: it reports it. A DTC, BMS_Fault, PackFaultActive
 * and PackState FAULT, a line in the log. It does not act on the pack: the node has no
 * contactor or current limit to command; opening the contactors is the decision of the pack's
 * master, which reads BMS_Fault.
 *
 * CONTEXT. No operating system, no locks; see WHO DOES WHAT.
 */
#ifndef FCN_FAULT_H
#define FCN_FAULT_H

#include <stdbool.h>
#include <stdint.h>

#include "bms/bms.h"
#include "can/can_fc.h"
#include "dtc/dtc.h"
#include "platform.h"
#include "wdgm/wdgm.h"

/** The faults, in the numbering described above. */
typedef enum {
    FAULT_CELL_OVERVOLTAGE = 0,
    FAULT_CELL_UNDERVOLTAGE,
    FAULT_OVERTEMPERATURE,
    FAULT_UNDERTEMPERATURE,
    FAULT_OVERCURRENT_DISCHARGE,
    FAULT_OVERCURRENT_CHARGE,
    FAULT_SENSOR_CHANNEL_INVALID,
    FAULT_FRONT_END_NO_ANSWER,
    FAULT_SAMPLE_TIMEOUT,
    FAULT_CAN_BUS_OFF,
    FAULT_CAN_ERROR_PASSIVE,
    FAULT_SUPERVISION_EXPIRED,
    FAULT_RESET_WATCHDOG,
    FAULT_RESET_ASSERT,
    FAULT_RESET_STACK_OVERFLOW,
    FAULT_RESET_MALLOC_FAILED,
    FAULT_UPDATE_ROLLBACK,
    FAULT_EVENTS
} fault_event_t;

/** fault_summary_t.most_recent when no DTC went to testFailed since the start or a clear. */
#define FAULT_NONE 31U

/** One bit per fault in a 32-bit mask (BMS_Fault's masks have a bit for each of the 17). The
 *  verdicts of the processing task hold only its own faults, 0..FAULT_SAMPLE_TIMEOUT, in 16 bits. */
_Static_assert((uint32_t)FAULT_EVENTS <= 32U, "a 32-bit mask holds every fault");
_Static_assert((uint32_t)FAULT_SAMPLE_TIMEOUT < 16U, "a verdict's 16-bit masks hold its faults");
_Static_assert((uint32_t)FAULT_EVENTS <= DTC_EVENTS_MAX, "lib/dtc has room for every fault");
_Static_assert((uint32_t)FAULT_EVENTS < FAULT_NONE, "FAULT_NONE is no fault");

/**
 * The limits of the pack values, in the units of lib/bms (mV, tenths of a degree Celsius, mA
 * positive while discharging). A reading beyond a limit is evidence of the fault; a reading
 * within its release value is evidence that it is gone; a reading between the two is neither,
 * so that a value that sits at a limit with noise on it does not make the DTC come and go
 * (hysteresis). Each value is justified at its entry in fault.c.
 */
#define FAULT_CELL_OV_MV             4250U
#define FAULT_CELL_OV_RELEASE_MV     4200U
#define FAULT_CELL_UV_MV             2500U
#define FAULT_CELL_UV_RELEASE_MV     2600U
#define FAULT_TEMP_OT_DC             600
#define FAULT_TEMP_OT_RELEASE_DC     550
#define FAULT_TEMP_UT_DC             (-200)
#define FAULT_TEMP_UT_RELEASE_DC     (-150)
#define FAULT_CURRENT_OCD_MA         250000
#define FAULT_CURRENT_OCD_RELEASE_MA 225000
#define FAULT_CURRENT_OCC_MA         (-100000)
#define FAULT_CURRENT_OCC_RELEASE_MA (-90000)

/**
 * The verdict on one sample, or on one timeout of the wait for a sample, for the faults the
 * processing task judges (0..FAULT_SAMPLE_TIMEOUT): bit n of `failing` is evidence of fault n,
 * bit n of `passing` evidence that it is absent. A fault in neither mask was not judged (the
 * sample did not carry its measurements, or the evidence was not conclusive). No bit is in
 * both. `clear_requests` is the receiver's count of clear requests (command_clear_requests())
 * at the moment the verdict was handed over: the poster sets it, the judges leave it 0.
 */
typedef struct {
    uint16_t failing;
    uint16_t passing;
    uint32_t clear_requests;
} fault_verdict_t;

/** What the health task reads, once per cycle, for the faults it judges itself. */
typedef struct {
    uint32_t bus_off_count;           /**< bus-off events since the start (can_txq_stats_t) */
    uint32_t error_passive_entries;   /**< changes of the controller to error passive since the
                                         start (can_io_stats_t) */
    can_fc_state_t can_state;         /**< the controller's error state now */
    wdgm_global_status_t supervision; /**< the watchdog manager's global status now */
} fault_health_inputs_t;

/** What BMS_Fault reports: a picture of the DTCs at the end of a period (THE SUMMARY OF A
 *  PERIOD). */
typedef struct {
    uint32_t active;     /**< bit n: the DTC of fault n has testFailed, or its testFailed rose
                              in the period (after its clear, if the period had one) */
    uint32_t confirmed;  /**< bit n: the DTC of fault n has confirmedDTC */
    uint8_t most_recent; /**< the fault of DTC_RECORD_MOST_RECENT_FAILED, or FAULT_NONE */
    uint8_t clears;      /**< clears carried out since the start, modulo 256 */
    bool warning;        /**< a DTC has warningIndicatorRequested */
} fault_summary_t;

/** The changes of testFailed in a period, for a log: a fault in both `rose` and `fell` changed
 *  both ways, and `failed` tells in which order it ended (set: it fell, then rose). */
typedef struct {
    uint32_t rose;   /**< bit n: testFailed of fault n went from 0 to 1 */
    uint32_t fell;   /**< bit n: it went from 1 to 0, by a pass or by a clear */
    uint32_t failed; /**< bit n: the DTC of fault n has testFailed at the end of the period */
} fault_changes_t;

/** The monitor: the DTC state and what the monitors remember. Set up with fault_init(); read
 *  the members for diagnosis, write none. */
typedef struct {
    dtc_state_t dtc;                 /**< the DTCs, one per fault_event_t */
    uint32_t verdicts;               /**< verdicts worked in by fault_apply() */
    uint32_t health_cycles;          /**< calls of fault_health_cycle() */
    uint32_t bus_off_count;          /**< fault_health_inputs_t.bus_off_count at the last cycle */
    uint32_t error_passive_entries;  /**< likewise for error_passive_entries */
    uint32_t clear_requests;         /**< the receiver's count of clear requests carried out */
    uint32_t clears;                 /**< clears carried out */
    plat_reset_reason_t reset_cause; /**< the cause given to fault_init() */
    uint32_t failed;                 /**< bit n: the DTC of fault n has testFailed */
    uint32_t rose;                   /**< testFailed rose since the last summary */
    uint32_t fell;                   /**< testFailed fell since the last summary (a clear too) */
    uint32_t latched; /**< of `rose`, those that rose after the last clear: active in the next
                           summary even if they have passed again */
    bool rolled_back; /**< the `rolled_back` given to fault_init() */
} fault_monitor_t;

/**
 * Set up the monitor at the start of a run: every DTC as after a clear (lib/dtc), the four
 * reset faults judged once from the reset cause of this start, `cause` (plat_reset_cause()):
 * the fault of the cause FAILED, the other three PASSED; and UPDATE_ROLLBACK judged once from
 * `rolled_back`, whether the bootloader's hand-off reports a rollback, a rejected trial or a
 * fallback (update_rolled_back(), update.h): FAILED if so, PASSED otherwise, also for a start
 * without the bootloader. Every other fault waits for its monitor. Returns false if lib/dtc
 * refuses the table of fault.c, which only a wrong build can make it do. Before the scheduler
 * starts (the node: app_start()).
 */
bool fault_init(fault_monitor_t *monitor, plat_reset_reason_t cause, bool rolled_back);

/**
 * The verdict on one sample: `sample` as the sensor task took it, `unanswered` the groups of it
 * (BMS_SAMPLE_*) the front end did not answer, `pack` the pack state after bms_process() worked
 * the sample in. The rules, per fault:
 *   - the limits (0..5): only for the groups the sample carries, only from readings that are
 *     valid in `pack` (a reading that is flagged or implausible says nothing about the cell),
 *     and on the sample's readings, not the filtered values: the filter would add its lag to
 *     the debouncing. Failing if a valid reading lies beyond the limit; passing if every
 *     channel of the group is valid and every reading lies within the release value;
 *   - SENSOR_CHANNEL_INVALID: failing if a group the front end answered has a channel that is
 *     invalid in `pack` (flagged by the front end, or implausible); passing if every channel
 *     of `pack` is valid;
 *   - FRONT_END_NO_ANSWER: failing if `unanswered` holds a group the sample carries; passing
 *     otherwise;
 *   - SAMPLE_TIMEOUT: passing (the sample came).
 * `clear_requests` is left 0 for the poster to set. No state, any context (the node: the
 * processing task).
 */
void fault_judge_sample(const bms_sample_t *sample, uint8_t unanswered, const bms_pack_t *pack,
                        fault_verdict_t *verdict);

/** The verdict on a wait for a sample that timed out: SAMPLE_TIMEOUT failing, nothing else
 *  judged, `clear_requests` 0 for the poster to set. No state, any context (the node: the
 *  processing task). */
void fault_judge_timeout(fault_verdict_t *verdict);

/** Work one verdict into the DTCs. First, if the verdict carries a count of clear requests
 *  newer than the count carried out (fault_clear_if_requested()), clear: the verdict's
 *  evidence came after that request. Then, in the order of the faults: DTC_REPORT_PREFAILED for
 *  every failing fault and DTC_REPORT_PREPASSED for every passing one, or the matured
 *  DTC_REPORT_FAILED and DTC_REPORT_PASSED for a fault whose threshold is 1 (-1), which the
 *  first piece of evidence decides (fault.c, THE TABLE OF FAULTS). Bits beyond the processing
 *  task's faults are ignored; a fault in both masks counts as failing. The owner only, with the
 *  verdicts in the order they were posted. */
void fault_apply(fault_monitor_t *monitor, const fault_verdict_t *verdict);

/**
 * One cycle of the health task's monitors:
 *   - CAN_BUS_OFF failing if a bus-off happened since the last cycle (the count went up) or the
 *     controller is bus off now; passing otherwise;
 *   - CAN_ERROR_PASSIVE failing if the controller went error passive since the last cycle or is
 *     not error active now; passing otherwise;
 *   - SUPERVISION_EXPIRED failing while the global status is EXPIRED or STOPPED; passing while it
 *     is OK or FAILED; not judged while it is DEACTIVATED.
 * The owner only, once per cycle.
 */
void fault_health_cycle(fault_monitor_t *monitor, const fault_health_inputs_t *inputs);

/**
 * Carry out the clear requests of TESTER_Command: if `requests` (command_rx_t.clear_requests,
 * which only grows, modulo 2^32) is newer than the count carried out so far (ahead of it by 1
 * to 2^31 - 1), clear every DTC (dtc_clear_all(), ClearDiagnosticInformation for all groups)
 * once, however many requests came since, and count the clear; an older count, or the same,
 * clears nothing. Returns true if it cleared. The clear ends the testFailed of every DTC that
 * had it (a fall, for the log) and forgets the rises of the period before it (THE SUMMARY OF A
 * PERIOD). A fault still present comes back as soon as its monitor has debounced it again; the
 * reset faults, judged only at the start, stay cleared. The owner only.
 */
bool fault_clear_if_requested(fault_monitor_t *monitor, uint32_t requests);

/** The summary of the DTCs at the end of a period, for BMS_Fault, and the changes of testFailed
 *  in the period, for the log (THE SUMMARY OF A PERIOD); a new period begins. The owner only. */
void fault_summary(fault_monitor_t *monitor, fault_summary_t *summary, fault_changes_t *changes);

/** The name of a fault as in the DBC's value table ("CELL_OVERVOLTAGE"), or "unknown". Any
 *  context. */
const char *fault_name(uint32_t event);

/** The DTC number of a fault, or 0 for a number that is no fault. Any context. */
uint32_t fault_dtc_number(uint32_t event);

#endif /* FCN_FAULT_H */
