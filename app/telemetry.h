/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The frames the node sends, built from what it knows: every cyclic message of dbc/fcn.dbc
 * that has the node as its sender. The values are converted with app/signals.h, packed with
 * the generated codec (app/gen/can_db.h), and the four messages with an FcnE2eDataId are
 * protected end to end (lib/e2e, AUTOSAR E2E Profile 11A) as the last step.
 *
 *   message             built by                     protected   carries
 *   BMS_Fault           telemetry_fault()            yes         the DTCs' summary (fault.h)
 *   BMS_PackStatus      telemetry_pack_status()      yes         current, voltage, state, flags
 *   BMS_CellVoltages1   telemetry_cell_voltages1()   yes         cells 1..4
 *   BMS_CellVoltages2   telemetry_cell_voltages2()   yes         cells 5..8
 *   BMS_Temperatures    telemetry_temperatures()     yes         temperatures 1..4
 *   BMS_Heartbeat       telemetry_heartbeat()        no          period counter, tick count
 *   BMS_TaskStats       telemetry_task_stats()       no          one task's run-time figures
 *   BMS_NodeInfo        telemetry_node_info()        no          version, slot, reset, uptime
 *
 * A channel that is not valid in the pack state goes out as "signal not available"; the pack
 * current and the pack voltage, which have no SNA code, go out as 0 with their valid flag 0.
 * Bits that belong to no signal are sent as 0, as the codec writes them.
 *
 * END-TO-END PROTECTION. The sender keeps one Profile 11 counter per protected message
 * (telemetry_sender_t for the four of the measurement chain; the caller's
 * e2e_p11_protect_state_t for BMS_Fault, which another task sends). A builder of a protected
 * message writes the payload, then the counter and the CRC, and advances the counter: call it
 * once for every frame that is handed to the bus, and hand every frame it built to the bus, or
 * the receiver sees a counter jump.
 *
 * CONTEXT. No operating system, no locks. A telemetry_sender_t belongs to the one task that
 * builds the protected messages of the measurement chain, the counter of BMS_Fault to the one
 * that sends BMS_Fault, a telemetry_uptime_t to the one that sends BMS_NodeInfo; the builders
 * of the other messages keep no state and may be called from any task.
 */
#ifndef FCN_TELEMETRY_H
#define FCN_TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>

#include "bms/bms.h"
#include "can/can_frame.h"
#include "e2e/e2e_p11.h"
#include "fault.h"
#include "plat_retained.h"
#include "platform.h"
#include "update.h"

/** The protected messages, in the order of their counters in telemetry_sender_t. */
typedef enum {
    TELEMETRY_PACK_STATUS = 0,
    TELEMETRY_CELL_VOLTAGES1,
    TELEMETRY_CELL_VOLTAGES2,
    TELEMETRY_TEMPERATURES,
    TELEMETRY_PROTECTED_MESSAGES
} telemetry_protected_t;

/** The sender's end of the end-to-end protection: the counter each protected message carries
 *  next. Set up with telemetry_init(). */
typedef struct {
    e2e_p11_protect_state_t e2e[TELEMETRY_PROTECTED_MESSAGES];
} telemetry_sender_t;

/**
 * The uptime BMS_NodeInfo carries: whole seconds, counted by the task that sends the message
 * from its own periods. It is not taken from the kernel's tick count, whose width the port
 * chooses: the Cortex-M3 port allows at most 32 bits, and a 32-bit count of 1 ms ticks wraps
 * after 2^32 ms, 49.7 days. This count wraps after 2^32 s, 136 years, as the 32-bit
 * UptimeSeconds does. All zero before the first period.
 */
typedef struct {
    uint32_t seconds; /**< whole seconds counted */
    uint32_t periods; /**< periods counted towards the next second */
} telemetry_uptime_t;

/** One task's figures for BMS_TaskStats, in the units the node measures them in. */
typedef struct {
    uint32_t index;            /**< the task's place in the application's task table, 0..15 */
    uint32_t priority;         /**< its FreeRTOS priority, 0..15 */
    uint64_t run_time_us;      /**< its time in the Running state since the start */
    uint64_t total_us;         /**< the run-time counter since the start */
    uint32_t stack_peak_bytes; /**< deepest stack use seen (plat_stack_info_t.peak_bytes) */
    uint32_t overruns;         /**< periods in which it missed its deadline */
} telemetry_task_t;

/** Every counter at 0: the first frame of each protected message carries counter 0. */
void telemetry_init(telemetry_sender_t *sender);

/**
 * BMS_PackStatus from a copy of the pack state: PackCurrent and PackCurrentValid from
 * current_ma and current_valid, PackVoltage and PackVoltageValid from pack_mv and pack_valid
 * (pack_mv is the pack voltage only while pack_valid), PackFaultActive from `fault_active`
 * (a fault is active in the summary, fault_summary_t.active != 0), and PackState from state,
 * except that it is FAULT while `fault_active`. Then protected.
 *
 * Returns false, with `frame` unspecified and the counter not advanced, only if packing or
 * protection refused, which nothing but a wrong build can make them do.
 */
bool telemetry_pack_status(telemetry_sender_t *sender, const bms_pack_t *pack, bool fault_active,
                           can_frame_t *frame);

/**
 * BMS_Fault from a summary of the DTCs: FaultActive, FaultConfirmed, FaultMostRecent (the
 * fault's number, or NONE), FaultWarning and FaultClearCount as fault_summary_t gives them.
 * Then protected with `counter`, the Profile 11 state of BMS_Fault, which the one task that
 * sends BMS_Fault keeps (set up with e2e_p11_protect_init()). Returns false as
 * telemetry_pack_status(), and also, building nothing, if `summary->most_recent` is neither a
 * fault nor FAULT_NONE.
 */
bool telemetry_fault(e2e_p11_protect_state_t *counter, const fault_summary_t *summary,
                     can_frame_t *frame);

/** BMS_CellVoltages1: cells 1 to 4 of the pack state (bit n of cell_valid for cell n + 1), each
 *  SNA while not valid. Then protected. Returns false as telemetry_pack_status(). */
bool telemetry_cell_voltages1(telemetry_sender_t *sender, const bms_pack_t *pack,
                              can_frame_t *frame);

/** BMS_CellVoltages2: cells 5 to 8, as telemetry_cell_voltages1(). */
bool telemetry_cell_voltages2(telemetry_sender_t *sender, const bms_pack_t *pack,
                              can_frame_t *frame);

/** BMS_Temperatures: temperatures 1 to 4, each SNA while not valid. Then protected. Returns
 *  false as telemetry_pack_status(). */
bool telemetry_temperatures(telemetry_sender_t *sender, const bms_pack_t *pack, can_frame_t *frame);

/** BMS_Heartbeat, bit for bit the status frame of phase 1: bytes 0..3 the period counter,
 *  bytes 4..7 the tick count, both little-endian. Returns false only if packing refused. */
bool telemetry_heartbeat(uint32_t period_counter, uint32_t tick_count, can_frame_t *frame);

/** BMS_TaskStats for one task. The figures are converted with app/signals.h: CPU share in
 *  0.01 %, the stack peak, the overruns, the index and the priority saturated at their signals'
 *  largest values. Returns false only if packing refused. */
bool telemetry_task_stats(const telemetry_task_t *task, can_frame_t *frame);

/** BMS_NodeInfo: the software version, ActiveSlot from `active_slot` (0 A, 1 B, the slot the
 *  bootloader's hand-off names; any other value, PLAT_HANDOFF_NO_SLOT for a start without the
 *  bootloader, is SNA), ResetCause the value of `reset_cause` (plat_reset_cause(): POWER_ON or a
 *  reset reason; SNA for a value that is neither), the uptime in seconds. Returns false only
 *  if packing refused. */
bool telemetry_node_info(uint8_t sw_major, uint8_t sw_minor, uint8_t sw_patch, uint8_t active_slot,
                         plat_reset_reason_t reset_cause, uint32_t uptime_s, can_frame_t *frame);

/**
 * BMS_BootInfo: the bootloader's hand-off `handoff` (NULL: none, BootMode NONE and every signal
 * but ConfirmState 0, BootEventSlot SNA) as BootMode, BootAttempt, BootEvent, BootEventSlot and
 * ImageVersionMajor/Minor/Patch, and the confirm as ConfirmState (`confirm`) and ConfirmSeconds
 * (`confirm_seconds`, saturated at 255). Returns false, building nothing, for a hand-off whose
 * event, slot, mode or attempt the DBC has no value for, or for a confirm state outside the
 * enumeration; otherwise false only if packing refused. Any context.
 */
bool telemetry_boot_info(const plat_boot_handoff_t *handoff, update_confirm_state_t confirm,
                         uint32_t confirm_seconds, can_frame_t *frame);

/** One period of the counting task has ended; `periods_per_second` of them (at least 1) make a
 *  second. Returns true if this period completed a second, which uptime->seconds then
 *  includes. */
bool telemetry_uptime_period(telemetry_uptime_t *uptime, uint32_t periods_per_second);

#endif /* FCN_TELEMETRY_H */
