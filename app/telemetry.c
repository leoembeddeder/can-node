/* SPDX-License-Identifier: MIT */
#include "telemetry.h"

#include <stddef.h>
#include <string.h>

#include "can_db.h"
#include "platform.h"
#include "signals.h"

/*
 * What this file assumes about the DBC beyond the conversions of signals.c.
 *
 * The protected messages carry the E2E header where lib/e2e writes it (Profile 11A: the CRC in
 * byte 0, the counter in the low nibble of byte 1) and are 8 bytes long, the length their E2E
 * configuration gives. The generator checks the header's place for every message with a Data
 * ID; these assertions check that the four messages built here are those messages.
 */
_Static_assert(CAN_DB_BMS_PACK_STATUS_E2E_PROTECTED == 1U, "BMS_PackStatus is protected");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_E2E_PROTECTED == 1U, "BMS_CellVoltages1 is protected");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_E2E_PROTECTED == 1U, "BMS_CellVoltages2 is protected");
_Static_assert(CAN_DB_BMS_TEMPERATURES_E2E_PROTECTED == 1U, "BMS_Temperatures is protected");
_Static_assert(CAN_DB_BMS_PACK_STATUS_DLC == E2E_P11_MAX_DATA_LEN, "BMS_PackStatus: 8 bytes");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_DLC == E2E_P11_MAX_DATA_LEN, "CellVoltages1: 8 bytes");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_DLC == E2E_P11_MAX_DATA_LEN, "CellVoltages2: 8 bytes");
_Static_assert(CAN_DB_BMS_TEMPERATURES_DLC == E2E_P11_MAX_DATA_LEN, "BMS_Temperatures: 8 bytes");
_Static_assert(CAN_DB_BMS_PACK_STATUS_E2E_CRC_START == (E2E_P11_CRC_BYTE * 8U), "CRC: byte 0");
_Static_assert(CAN_DB_BMS_PACK_STATUS_E2E_CRC_LENGTH == 8U, "CRC: 8 bits");
_Static_assert(CAN_DB_BMS_PACK_STATUS_E2E_COUNTER_START == (E2E_P11_COUNTER_BYTE * 8U),
               "counter: low nibble of byte 1");
_Static_assert(CAN_DB_BMS_PACK_STATUS_E2E_COUNTER_LENGTH == 4U, "counter: 4 bits");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_E2E_CRC_START == (E2E_P11_CRC_BYTE * 8U), "CRC: byte 0");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_E2E_COUNTER_START == (E2E_P11_COUNTER_BYTE * 8U),
               "counter: low nibble of byte 1");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_E2E_CRC_START == (E2E_P11_CRC_BYTE * 8U), "CRC: byte 0");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_E2E_COUNTER_START == (E2E_P11_COUNTER_BYTE * 8U),
               "counter: low nibble of byte 1");
_Static_assert(CAN_DB_BMS_TEMPERATURES_E2E_CRC_START == (E2E_P11_CRC_BYTE * 8U), "CRC: byte 0");
_Static_assert(CAN_DB_BMS_TEMPERATURES_E2E_COUNTER_START == (E2E_P11_COUNTER_BYTE * 8U),
               "counter: low nibble of byte 1");

/* The heartbeat is the status frame of phase 1, which every bus test of phase 1 accounts
 * against: two little-endian 32-bit numbers, the period counter first, and no protection. */
_Static_assert(CAN_DB_BMS_HEARTBEAT_E2E_PROTECTED == 0U, "BMS_Heartbeat is not protected");
_Static_assert(CAN_DB_BMS_HEARTBEAT_DLC == 8U, "BMS_Heartbeat: 8 bytes");
_Static_assert(CAN_DB_BMS_HEARTBEAT_PERIOD_COUNTER_START == 0U, "PeriodCounter: bytes 0..3");
_Static_assert(CAN_DB_BMS_HEARTBEAT_PERIOD_COUNTER_LENGTH == 32U, "PeriodCounter: bytes 0..3");
_Static_assert(CAN_DB_BMS_HEARTBEAT_TICK_COUNT_START == 32U, "TickCount: bytes 4..7");
_Static_assert(CAN_DB_BMS_HEARTBEAT_TICK_COUNT_LENGTH == 32U, "TickCount: bytes 4..7");

/* The other two messages built here carry raw counts: 1 per bit, no offset. */
_Static_assert(CAN_DB_BMS_TASK_STATS_E2E_PROTECTED == 0U, "BMS_TaskStats is not protected");
_Static_assert(CAN_DB_BMS_NODE_INFO_E2E_PROTECTED == 0U, "BMS_NodeInfo is not protected");
_Static_assert(CAN_DB_BMS_TASK_STATS_TASK_STACK_PEAK_FACTOR_NUM == 1, "TaskStackPeak: bytes");
_Static_assert(CAN_DB_BMS_TASK_STATS_TASK_STACK_PEAK_FACTOR_DEN == 1, "TaskStackPeak: bytes");
_Static_assert(CAN_DB_BMS_TASK_STATS_TASK_STACK_PEAK_OFFSET_NUM == 0, "TaskStackPeak: bytes");
_Static_assert(CAN_DB_BMS_TASK_STATS_TASK_OVERRUNS_FACTOR_DEN == 1, "TaskOverruns: a count");
_Static_assert(CAN_DB_BMS_NODE_INFO_UPTIME_SECONDS_FACTOR_DEN == 1, "UptimeSeconds: seconds");
_Static_assert(CAN_DB_BMS_NODE_INFO_UPTIME_SECONDS_LENGTH == 32U, "UptimeSeconds: 32 bits");

/* ResetCause carries the numbers of plat_reset_reason_t, the cause of this start included. */
_Static_assert(CAN_DB_BMS_NODE_INFO_RESET_CAUSE_POWER_ON == (uint32_t)PLAT_RESET_POWER_ON,
               "ResetCause: POWER_ON");
_Static_assert(CAN_DB_BMS_NODE_INFO_RESET_CAUSE_REQUESTED == (uint32_t)PLAT_RESET_REQUESTED,
               "ResetCause: REQUESTED");
_Static_assert(CAN_DB_BMS_NODE_INFO_RESET_CAUSE_WATCHDOG == (uint32_t)PLAT_RESET_WATCHDOG,
               "ResetCause: WATCHDOG");
_Static_assert(CAN_DB_BMS_NODE_INFO_RESET_CAUSE_POWER_LOSS == (uint32_t)PLAT_RESET_POWER_LOSS,
               "ResetCause: POWER_LOSS");
_Static_assert(CAN_DB_BMS_NODE_INFO_RESET_CAUSE_IMAGE_FAULT == (uint32_t)PLAT_RESET_IMAGE_FAULT,
               "ResetCause: IMAGE_FAULT");
_Static_assert(CAN_DB_BMS_NODE_INFO_RESET_CAUSE_ASSERT == (uint32_t)PLAT_RESET_ASSERT,
               "ResetCause: ASSERT");
_Static_assert(CAN_DB_BMS_NODE_INFO_RESET_CAUSE_STACK_OVERFLOW ==
                   (uint32_t)PLAT_RESET_STACK_OVERFLOW,
               "ResetCause: STACK_OVERFLOW");
_Static_assert(CAN_DB_BMS_NODE_INFO_RESET_CAUSE_MALLOC_FAILED == (uint32_t)PLAT_RESET_MALLOC_FAILED,
               "ResetCause: MALLOC_FAILED");

/* The cell masks of bms_pack_t have one bit per cell, cell 1 in bit 0; two messages of four. */
_Static_assert(BMS_CELLS == 8U, "two cell messages of four cells");
_Static_assert(BMS_TEMPS == 4U, "one temperature message of four");

/* BMS_Fault: protected like the others, a bit of each mask per fault, and the value table of
 * FaultMostRecent numbered as fault_event_t, one assertion per fault. */
_Static_assert(CAN_DB_BMS_FAULT_E2E_PROTECTED == 1U, "BMS_Fault is protected");
_Static_assert(CAN_DB_BMS_FAULT_DLC == E2E_P11_MAX_DATA_LEN, "BMS_Fault: 8 bytes");
_Static_assert(CAN_DB_BMS_FAULT_E2E_CRC_START == (E2E_P11_CRC_BYTE * 8U), "CRC: byte 0");
_Static_assert(CAN_DB_BMS_FAULT_E2E_COUNTER_START == (E2E_P11_COUNTER_BYTE * 8U),
               "counter: low nibble of byte 1");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_ACTIVE_LENGTH == (uint32_t)FAULT_EVENTS,
               "FaultActive: a bit per fault");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_CONFIRMED_LENGTH == (uint32_t)FAULT_EVENTS,
               "FaultConfirmed: a bit per fault");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_CLEAR_COUNT_LENGTH == 8U, "FaultClearCount: modulo 256");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_NONE == FAULT_NONE, "FaultMostRecent: NONE");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_CELL_OVERVOLTAGE ==
                   (uint32_t)FAULT_CELL_OVERVOLTAGE,
               "FaultMostRecent: CELL_OVERVOLTAGE");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_CELL_UNDERVOLTAGE ==
                   (uint32_t)FAULT_CELL_UNDERVOLTAGE,
               "FaultMostRecent: CELL_UNDERVOLTAGE");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_OVERTEMPERATURE ==
                   (uint32_t)FAULT_OVERTEMPERATURE,
               "FaultMostRecent: OVERTEMPERATURE");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_UNDERTEMPERATURE ==
                   (uint32_t)FAULT_UNDERTEMPERATURE,
               "FaultMostRecent: UNDERTEMPERATURE");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_OVERCURRENT_DISCHARGE ==
                   (uint32_t)FAULT_OVERCURRENT_DISCHARGE,
               "FaultMostRecent: OVERCURRENT_DISCHARGE");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_OVERCURRENT_CHARGE ==
                   (uint32_t)FAULT_OVERCURRENT_CHARGE,
               "FaultMostRecent: OVERCURRENT_CHARGE");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_SENSOR_CHANNEL_INVALID ==
                   (uint32_t)FAULT_SENSOR_CHANNEL_INVALID,
               "FaultMostRecent: SENSOR_CHANNEL_INVALID");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_FRONT_END_NO_ANSWER ==
                   (uint32_t)FAULT_FRONT_END_NO_ANSWER,
               "FaultMostRecent: FRONT_END_NO_ANSWER");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_SAMPLE_TIMEOUT == (uint32_t)FAULT_SAMPLE_TIMEOUT,
               "FaultMostRecent: SAMPLE_TIMEOUT");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_CAN_BUS_OFF == (uint32_t)FAULT_CAN_BUS_OFF,
               "FaultMostRecent: CAN_BUS_OFF");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_CAN_ERROR_PASSIVE ==
                   (uint32_t)FAULT_CAN_ERROR_PASSIVE,
               "FaultMostRecent: CAN_ERROR_PASSIVE");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_SUPERVISION_EXPIRED ==
                   (uint32_t)FAULT_SUPERVISION_EXPIRED,
               "FaultMostRecent: SUPERVISION_EXPIRED");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_RESET_WATCHDOG == (uint32_t)FAULT_RESET_WATCHDOG,
               "FaultMostRecent: RESET_WATCHDOG");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_RESET_ASSERT == (uint32_t)FAULT_RESET_ASSERT,
               "FaultMostRecent: RESET_ASSERT");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_RESET_STACK_OVERFLOW ==
                   (uint32_t)FAULT_RESET_STACK_OVERFLOW,
               "FaultMostRecent: RESET_STACK_OVERFLOW");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_RESET_MALLOC_FAILED ==
                   (uint32_t)FAULT_RESET_MALLOC_FAILED,
               "FaultMostRecent: RESET_MALLOC_FAILED");
_Static_assert(CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_UPDATE_ROLLBACK ==
                   (uint32_t)FAULT_UPDATE_ROLLBACK,
               "FaultMostRecent: UPDATE_ROLLBACK");
_Static_assert((uint32_t)FAULT_EVENTS == 17U, "every fault has its entry above");

/* BMS_NodeInfo's ActiveSlot and BMS_BootInfo carry the slots, the modes, the events of the
 * hand-off (plat_retained.h) and the confirm states (update.h) by their own numbers. */
_Static_assert(CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_A == 0U, "ActiveSlot: A is slot 0");
_Static_assert(CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_B == 1U, "ActiveSlot: B is slot 1");
_Static_assert(CAN_DB_BMS_BOOT_INFO_E2E_PROTECTED == 0U, "BMS_BootInfo is not protected");
_Static_assert(CAN_DB_BMS_BOOT_INFO_BOOT_MODE_CONFIRMED == (uint32_t)PLAT_BOOT_CONFIRMED,
               "BootMode: CONFIRMED");
_Static_assert(CAN_DB_BMS_BOOT_INFO_BOOT_MODE_TRIAL == (uint32_t)PLAT_BOOT_TRIAL,
               "BootMode: TRIAL");
_Static_assert(CAN_DB_BMS_BOOT_INFO_BOOT_ATTEMPT_RAW_MAX == PLAT_HANDOFF_MAX_ATTEMPT,
               "BootAttempt: 0 to 3");
_Static_assert(CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_NONE == (uint32_t)PLAT_BOOT_EVENT_NONE,
               "BootEvent: NONE");
_Static_assert(CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_ADOPT == (uint32_t)PLAT_BOOT_EVENT_ADOPT,
               "BootEvent: ADOPT");
_Static_assert(CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_ROLLBACK == (uint32_t)PLAT_BOOT_EVENT_ROLLBACK,
               "BootEvent: ROLLBACK");
_Static_assert(CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_REJECT == (uint32_t)PLAT_BOOT_EVENT_REJECT,
               "BootEvent: REJECT");
_Static_assert(CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_FALLBACK == (uint32_t)PLAT_BOOT_EVENT_FALLBACK,
               "BootEvent: FALLBACK");
_Static_assert(CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_A == 0U, "BootEventSlot: A is slot 0");
_Static_assert(CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_B == 1U, "BootEventSlot: B is slot 1");
_Static_assert(CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_NOT_NEEDED == (uint32_t)UPDATE_CONFIRM_NOT_NEEDED,
               "ConfirmState: NOT_NEEDED");
_Static_assert(CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_WAITING == (uint32_t)UPDATE_CONFIRM_WAITING,
               "ConfirmState: WAITING");
_Static_assert(CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_CONFIRMING == (uint32_t)UPDATE_CONFIRM_CONFIRMING,
               "ConfirmState: CONFIRMING");
_Static_assert(CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_CONFIRMED == (uint32_t)UPDATE_CONFIRM_CONFIRMED,
               "ConfirmState: CONFIRMED");
_Static_assert(CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_FAILED == (uint32_t)UPDATE_CONFIRM_FAILED,
               "ConfirmState: FAILED");
_Static_assert(CAN_DB_BMS_BOOT_INFO_CONFIRM_SECONDS_RAW_MAX == 255U, "ConfirmSeconds: 8 bits");
_Static_assert(CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_PATCH_LENGTH == 16U,
               "ImageVersionPatch: the 16 bits of lib/image's patch");

/* The E2E configuration of BMS_Fault, as s_e2e_config below for the other four. */
static const e2e_p11_config_t s_fault_e2e_config = {.data_id = CAN_DB_BMS_FAULT_E2E_DATA_ID,
                                                    .data_len = CAN_DB_BMS_FAULT_DLC,
                                                    .max_delta_counter = 1U};

/*
 * The E2E configuration of each protected message, in the order of telemetry_protected_t. The
 * Data IDs are the DBC's (FcnE2eDataId). max_delta_counter is the receiver's tolerance and is
 * not used by e2e_p11_protect(), which still checks that it lies in 1..14: 1 here, the value
 * for a receiver that tolerates no lost frame.
 */
static const e2e_p11_config_t s_e2e_config[TELEMETRY_PROTECTED_MESSAGES] = {
    {.data_id = CAN_DB_BMS_PACK_STATUS_E2E_DATA_ID,
     .data_len = CAN_DB_BMS_PACK_STATUS_DLC,
     .max_delta_counter = 1U},
    {.data_id = CAN_DB_BMS_CELL_VOLTAGES1_E2E_DATA_ID,
     .data_len = CAN_DB_BMS_CELL_VOLTAGES1_DLC,
     .max_delta_counter = 1U},
    {.data_id = CAN_DB_BMS_CELL_VOLTAGES2_E2E_DATA_ID,
     .data_len = CAN_DB_BMS_CELL_VOLTAGES2_DLC,
     .max_delta_counter = 1U},
    {.data_id = CAN_DB_BMS_TEMPERATURES_E2E_DATA_ID,
     .data_len = CAN_DB_BMS_TEMPERATURES_DLC,
     .max_delta_counter = 1U}};

/* An empty base-format data frame with this identifier and length. */
static void start_frame(can_frame_t *frame, uint32_t id, uint8_t dlc)
{
    (void)memset(frame, 0, sizeof(*frame));
    frame->id = id;
    frame->dlc = dlc;
}

/* The last step of every protected message: counter and CRC into bytes 1 and 0. */
static bool protect(telemetry_sender_t *sender, telemetry_protected_t message, can_frame_t *frame)
{
    return e2e_p11_protect(&s_e2e_config[message], &sender->e2e[message], frame->data,
                           (size_t)frame->dlc) == E2E_E_OK;
}

static bool bit_set(uint8_t mask, uint32_t bit)
{
    return (((uint32_t)mask >> bit) & 1U) != 0U;
}

static uint16_t cell_raw(const bms_pack_t *pack, uint32_t cell)
{
    return signals_cell_voltage(pack->cell_mv[cell], bit_set(pack->cell_valid, cell));
}

static uint8_t temperature_raw(const bms_pack_t *pack, uint32_t sensor)
{
    return signals_temperature(pack->temp_dc[sensor], bit_set(pack->temp_valid, sensor));
}

void telemetry_init(telemetry_sender_t *sender)
{
    for (uint32_t i = 0U; i < (uint32_t)TELEMETRY_PROTECTED_MESSAGES; i++) {
        (void)e2e_p11_protect_init(&sender->e2e[i]);
    }
}

bool telemetry_pack_status(telemetry_sender_t *sender, const bms_pack_t *pack, bool fault_active,
                           can_frame_t *frame)
{
    const can_db_bms_pack_status_t msg = {
        .e2e_crc = 0U,
        .e2e_counter = 0U,
        .pack_current = signals_pack_current(pack->current_valid ? pack->current_ma : 0),
        .pack_voltage = signals_pack_voltage(pack->pack_valid ? pack->pack_mv : 0U),
        .pack_state = signals_pack_state(fault_active ? BMS_STATE_FAULT : pack->state),
        .pack_fault_active = fault_active ? 1U : 0U,
        .pack_current_valid = pack->current_valid ? 1U : 0U,
        .pack_voltage_valid = pack->pack_valid ? 1U : 0U};
    bool built;

    start_frame(frame, CAN_DB_BMS_PACK_STATUS_ID, CAN_DB_BMS_PACK_STATUS_DLC);
    built = can_db_bms_pack_status_pack(&msg, frame->data, sizeof(frame->data));

    if (built) {
        built = protect(sender, TELEMETRY_PACK_STATUS, frame);
    }

    return built;
}

bool telemetry_cell_voltages1(telemetry_sender_t *sender, const bms_pack_t *pack,
                              can_frame_t *frame)
{
    const can_db_bms_cell_voltages1_t msg = {.e2e_crc = 0U,
                                             .e2e_counter = 0U,
                                             .cell1_voltage = cell_raw(pack, 0U),
                                             .cell2_voltage = cell_raw(pack, 1U),
                                             .cell3_voltage = cell_raw(pack, 2U),
                                             .cell4_voltage = cell_raw(pack, 3U)};
    bool built;

    start_frame(frame, CAN_DB_BMS_CELL_VOLTAGES1_ID, CAN_DB_BMS_CELL_VOLTAGES1_DLC);
    built = can_db_bms_cell_voltages1_pack(&msg, frame->data, sizeof(frame->data));

    if (built) {
        built = protect(sender, TELEMETRY_CELL_VOLTAGES1, frame);
    }

    return built;
}

bool telemetry_cell_voltages2(telemetry_sender_t *sender, const bms_pack_t *pack,
                              can_frame_t *frame)
{
    const can_db_bms_cell_voltages2_t msg = {.e2e_crc = 0U,
                                             .e2e_counter = 0U,
                                             .cell5_voltage = cell_raw(pack, 4U),
                                             .cell6_voltage = cell_raw(pack, 5U),
                                             .cell7_voltage = cell_raw(pack, 6U),
                                             .cell8_voltage = cell_raw(pack, 7U)};
    bool built;

    start_frame(frame, CAN_DB_BMS_CELL_VOLTAGES2_ID, CAN_DB_BMS_CELL_VOLTAGES2_DLC);
    built = can_db_bms_cell_voltages2_pack(&msg, frame->data, sizeof(frame->data));

    if (built) {
        built = protect(sender, TELEMETRY_CELL_VOLTAGES2, frame);
    }

    return built;
}

bool telemetry_temperatures(telemetry_sender_t *sender, const bms_pack_t *pack, can_frame_t *frame)
{
    const can_db_bms_temperatures_t msg = {.e2e_crc = 0U,
                                           .e2e_counter = 0U,
                                           .temp1 = temperature_raw(pack, 0U),
                                           .temp2 = temperature_raw(pack, 1U),
                                           .temp3 = temperature_raw(pack, 2U),
                                           .temp4 = temperature_raw(pack, 3U)};
    bool built;

    start_frame(frame, CAN_DB_BMS_TEMPERATURES_ID, CAN_DB_BMS_TEMPERATURES_DLC);
    built = can_db_bms_temperatures_pack(&msg, frame->data, sizeof(frame->data));

    if (built) {
        built = protect(sender, TELEMETRY_TEMPERATURES, frame);
    }

    return built;
}

bool telemetry_heartbeat(uint32_t period_counter, uint32_t tick_count, can_frame_t *frame)
{
    const can_db_bms_heartbeat_t msg = {.period_counter = period_counter, .tick_count = tick_count};

    start_frame(frame, CAN_DB_BMS_HEARTBEAT_ID, CAN_DB_BMS_HEARTBEAT_DLC);

    return can_db_bms_heartbeat_pack(&msg, frame->data, sizeof(frame->data));
}

bool telemetry_task_stats(const telemetry_task_t *task, can_frame_t *frame)
{
    const can_db_bms_task_stats_t msg = {
        .task_index =
            (uint8_t)signals_saturate(task->index, CAN_DB_BMS_TASK_STATS_TASK_INDEX_RAW_MAX),
        .task_priority =
            (uint8_t)signals_saturate(task->priority, CAN_DB_BMS_TASK_STATS_TASK_PRIORITY_RAW_MAX),
        .task_cpu_share = signals_cpu_share(task->run_time_us, task->total_us),
        .task_stack_peak = (uint16_t)signals_saturate(
            task->stack_peak_bytes, CAN_DB_BMS_TASK_STATS_TASK_STACK_PEAK_RAW_MAX),
        .task_overruns =
            (uint8_t)signals_saturate(task->overruns, CAN_DB_BMS_TASK_STATS_TASK_OVERRUNS_RAW_MAX)};

    start_frame(frame, CAN_DB_BMS_TASK_STATS_ID, CAN_DB_BMS_TASK_STATS_DLC);

    return can_db_bms_task_stats_pack(&msg, frame->data, sizeof(frame->data));
}

bool telemetry_fault(e2e_p11_protect_state_t *counter, const fault_summary_t *summary,
                     can_frame_t *frame)
{
    const can_db_bms_fault_t msg = {.e2e_crc = 0U,
                                    .e2e_counter = 0U,
                                    .fault_active = summary->active,
                                    .fault_confirmed = summary->confirmed,
                                    .fault_most_recent = summary->most_recent,
                                    .fault_warning = summary->warning ? 1U : 0U,
                                    .fault_clear_count = summary->clears};
    bool built = (summary->most_recent < (uint8_t)FAULT_EVENTS) ||
                 (summary->most_recent == (uint8_t)FAULT_NONE);

    start_frame(frame, CAN_DB_BMS_FAULT_ID, CAN_DB_BMS_FAULT_DLC);

    if (built) {
        built = can_db_bms_fault_pack(&msg, frame->data, sizeof(frame->data));
    }

    if (built) {
        built = e2e_p11_protect(&s_fault_e2e_config, counter, frame->data, (size_t)frame->dlc) ==
                E2E_E_OK;
    }

    return built;
}

/* ResetCause: the value of a cause the value table names, SNA for any other. */
static uint8_t reset_cause_raw(plat_reset_reason_t cause)
{
    uint8_t raw = (uint8_t)CAN_DB_BMS_NODE_INFO_RESET_CAUSE_SNA;

    switch (cause) {
    case PLAT_RESET_POWER_ON:
    case PLAT_RESET_REQUESTED:
    case PLAT_RESET_WATCHDOG:
    case PLAT_RESET_POWER_LOSS:
    case PLAT_RESET_IMAGE_FAULT:
    case PLAT_RESET_ASSERT:
    case PLAT_RESET_STACK_OVERFLOW:
    case PLAT_RESET_MALLOC_FAILED:
        raw = (uint8_t)cause;
        break;
    default:
        /* not a cause: SNA */
        break;
    }

    return raw;
}

/* A slot of the hand-off as the DBC's slot signals carry it: 0 and 1 as they are, SNA else. */
static uint8_t slot_raw(uint8_t slot, uint8_t sna)
{
    return (slot <= 1U) ? slot : sna;
}

bool telemetry_node_info(uint8_t sw_major, uint8_t sw_minor, uint8_t sw_patch, uint8_t active_slot,
                         plat_reset_reason_t reset_cause, uint32_t uptime_s, can_frame_t *frame)
{
    const can_db_bms_node_info_t msg = {
        .sw_major = sw_major,
        .sw_minor = sw_minor,
        .sw_patch = sw_patch,
        .active_slot = slot_raw(active_slot, (uint8_t)CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_SNA),
        .reset_cause = reset_cause_raw(reset_cause),
        .uptime_seconds = uptime_s};

    start_frame(frame, CAN_DB_BMS_NODE_INFO_ID, CAN_DB_BMS_NODE_INFO_DLC);

    return can_db_bms_node_info_pack(&msg, frame->data, sizeof(frame->data));
}

/* The hand-off's event, mode and attempt are values BMS_BootInfo has. */
static bool handoff_encodable(const plat_boot_handoff_t *h)
{
    const bool event = (h->event == PLAT_BOOT_EVENT_NONE) || (h->event == PLAT_BOOT_EVENT_ADOPT) ||
                       (h->event == PLAT_BOOT_EVENT_ROLLBACK) ||
                       (h->event == PLAT_BOOT_EVENT_REJECT) ||
                       (h->event == PLAT_BOOT_EVENT_FALLBACK);
    const bool mode = (h->mode == PLAT_BOOT_CONFIRMED) || (h->mode == PLAT_BOOT_TRIAL);

    return event && mode && (h->slot <= 1U) && (h->attempt <= PLAT_HANDOFF_MAX_ATTEMPT);
}

bool telemetry_boot_info(const plat_boot_handoff_t *handoff, update_confirm_state_t confirm,
                         uint32_t confirm_seconds, can_frame_t *frame)
{
    can_db_bms_boot_info_t msg;
    bool built = (uint32_t)confirm <= (uint32_t)UPDATE_CONFIRM_FAILED;

    (void)memset(&msg, 0, sizeof(msg));
    msg.boot_mode = (uint8_t)CAN_DB_BMS_BOOT_INFO_BOOT_MODE_NONE;
    msg.boot_event_slot = (uint8_t)CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_SNA;
    msg.confirm_state = (uint8_t)confirm;
    msg.confirm_seconds = (uint8_t)((confirm_seconds > CAN_DB_BMS_BOOT_INFO_CONFIRM_SECONDS_RAW_MAX)
                                        ? CAN_DB_BMS_BOOT_INFO_CONFIRM_SECONDS_RAW_MAX
                                        : confirm_seconds);

    if (built && (handoff != NULL)) {
        built = handoff_encodable(handoff);
        msg.boot_mode = (uint8_t)handoff->mode;
        msg.boot_attempt = handoff->attempt;
        msg.boot_event = (uint8_t)handoff->event;
        msg.boot_event_slot =
            slot_raw(handoff->event_slot, (uint8_t)CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_SNA);
        msg.image_version_major = (uint8_t)(handoff->image_version >> 24U);
        msg.image_version_minor = (uint8_t)((handoff->image_version >> 16U) & 0xFFU);
        msg.image_version_patch = (uint16_t)(handoff->image_version & 0xFFFFU);
    }

    start_frame(frame, CAN_DB_BMS_BOOT_INFO_ID, CAN_DB_BMS_BOOT_INFO_DLC);

    if (built) {
        built = can_db_bms_boot_info_pack(&msg, frame->data, sizeof(frame->data));
    }

    return built;
}

bool telemetry_uptime_period(telemetry_uptime_t *uptime, uint32_t periods_per_second)
{
    bool second = false;

    uptime->periods++;

    /* At least: a periods_per_second of 0 counts as 1. */
    if (uptime->periods >= periods_per_second) {
        uptime->periods = 0U;
        uptime->seconds++; /* after 2^32 s back to 0, as UptimeSeconds */
        second = true;
    }

    return second;
}
