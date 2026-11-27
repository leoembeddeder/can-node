/* SPDX-License-Identifier: MIT */
/*
 * The table of can_db_table.h for the codec generated from the same DBC: every message, every
 * signal, and the adapters between arrays of raw values and the structs.
 *
 * GENERATED FILE, DO NOT EDIT. Test support, not part of the node. Made by tools/gen_can_db.py from
 * dbc/fcn.dbc, SHA-256 ee5a3fa516d3029851805544da6004843add051153e5492607e90ef8e1881e18.
 *
 * After a change of the DBC, run scripts/codegen-check.sh --update. Without --update the script
 * fails while this file is out of date; CI and scripts/check.sh run it.
 */
#include "can_db_table.h"

#include "can_db.h"

static bool pack_bms_fault(const int64_t *raw, uint8_t *data, size_t capacity)
{
    can_db_bms_fault_t msg;

    msg.e2e_crc = (uint8_t)raw[0];
    msg.e2e_counter = (uint8_t)raw[1];
    msg.fault_active = (uint32_t)raw[2];
    msg.fault_confirmed = (uint32_t)raw[3];
    msg.fault_most_recent = (uint8_t)raw[4];
    msg.fault_warning = (uint8_t)raw[5];
    msg.fault_clear_count = (uint8_t)raw[6];
    return can_db_bms_fault_pack(&msg, data, capacity);
}

static bool unpack_bms_fault(const uint8_t *data, size_t len, int64_t *raw)
{
    can_db_bms_fault_t msg = {0};
    const bool unpacked = can_db_bms_fault_unpack(&msg, data, len);

    if (unpacked) {
        raw[0] = msg.e2e_crc;
        raw[1] = msg.e2e_counter;
        raw[2] = msg.fault_active;
        raw[3] = msg.fault_confirmed;
        raw[4] = msg.fault_most_recent;
        raw[5] = msg.fault_warning;
        raw[6] = msg.fault_clear_count;
    }

    return unpacked;
}

static const can_db_table_signal_t SIGNALS_BMS_FAULT[] = {
    {"E2E_Crc", 0U, 8U, false, 0, 255, 0, 255},                    /* uint8_t e2e_crc */
    {"E2E_Counter", 8U, 4U, false, 0, 15, 0, 255},                 /* uint8_t e2e_counter */
    {"FaultActive", 16U, 17U, false, 0, 131071, 0, 4294967295},    /* uint32_t fault_active */
    {"FaultConfirmed", 33U, 17U, false, 0, 131071, 0, 4294967295}, /* uint32_t fault_confirmed */
    {"FaultMostRecent", 50U, 5U, false, 0, 31, 0, 255},            /* uint8_t fault_most_recent */
    {"FaultWarning", 55U, 1U, false, 0, 1, 0, 255},                /* uint8_t fault_warning */
    {"FaultClearCount", 56U, 8U, false, 0, 255, 0, 255},           /* uint8_t fault_clear_count */
};

static bool pack_bms_pack_status(const int64_t *raw, uint8_t *data, size_t capacity)
{
    can_db_bms_pack_status_t msg;

    msg.e2e_crc = (uint8_t)raw[0];
    msg.e2e_counter = (uint8_t)raw[1];
    msg.pack_current = (int16_t)raw[2];
    msg.pack_voltage = (uint16_t)raw[3];
    msg.pack_state = (uint8_t)raw[4];
    msg.pack_fault_active = (uint8_t)raw[5];
    msg.pack_current_valid = (uint8_t)raw[6];
    msg.pack_voltage_valid = (uint8_t)raw[7];
    return can_db_bms_pack_status_pack(&msg, data, capacity);
}

static bool unpack_bms_pack_status(const uint8_t *data, size_t len, int64_t *raw)
{
    can_db_bms_pack_status_t msg = {0};
    const bool unpacked = can_db_bms_pack_status_unpack(&msg, data, len);

    if (unpacked) {
        raw[0] = msg.e2e_crc;
        raw[1] = msg.e2e_counter;
        raw[2] = msg.pack_current;
        raw[3] = msg.pack_voltage;
        raw[4] = msg.pack_state;
        raw[5] = msg.pack_fault_active;
        raw[6] = msg.pack_current_valid;
        raw[7] = msg.pack_voltage_valid;
    }

    return unpacked;
}

static const can_db_table_signal_t SIGNALS_BMS_PACK_STATUS[] = {
    {"E2E_Crc", 0U, 8U, false, 0, 255, 0, 255},                    /* uint8_t e2e_crc */
    {"E2E_Counter", 8U, 4U, false, 0, 15, 0, 255},                 /* uint8_t e2e_counter */
    {"PackCurrent", 16U, 16U, true, -32768, 32767, -32768, 32767}, /* int16_t pack_current */
    {"PackVoltage", 32U, 16U, false, 0, 65535, 0, 65535},          /* uint16_t pack_voltage */
    {"PackState", 48U, 2U, false, 0, 3, 0, 255},                   /* uint8_t pack_state */
    {"PackFaultActive", 50U, 1U, false, 0, 1, 0, 255},             /* uint8_t pack_fault_active */
    {"PackCurrentValid", 51U, 1U, false, 0, 1, 0, 255},            /* uint8_t pack_current_valid */
    {"PackVoltageValid", 52U, 1U, false, 0, 1, 0, 255},            /* uint8_t pack_voltage_valid */
};

static bool pack_bms_heartbeat(const int64_t *raw, uint8_t *data, size_t capacity)
{
    can_db_bms_heartbeat_t msg;

    msg.period_counter = (uint32_t)raw[0];
    msg.tick_count = (uint32_t)raw[1];
    return can_db_bms_heartbeat_pack(&msg, data, capacity);
}

static bool unpack_bms_heartbeat(const uint8_t *data, size_t len, int64_t *raw)
{
    can_db_bms_heartbeat_t msg = {0};
    const bool unpacked = can_db_bms_heartbeat_unpack(&msg, data, len);

    if (unpacked) {
        raw[0] = msg.period_counter;
        raw[1] = msg.tick_count;
    }

    return unpacked;
}

static const can_db_table_signal_t SIGNALS_BMS_HEARTBEAT[] = {
    {"PeriodCounter", 0U, 32U, false, 0, 4294967295, 0, 4294967295}, /* uint32_t period_counter */
    {"TickCount", 32U, 32U, false, 0, 4294967295, 0, 4294967295},    /* uint32_t tick_count */
};

static bool pack_bms_cell_voltages1(const int64_t *raw, uint8_t *data, size_t capacity)
{
    can_db_bms_cell_voltages1_t msg;

    msg.e2e_crc = (uint8_t)raw[0];
    msg.e2e_counter = (uint8_t)raw[1];
    msg.cell1_voltage = (uint16_t)raw[2];
    msg.cell2_voltage = (uint16_t)raw[3];
    msg.cell3_voltage = (uint16_t)raw[4];
    msg.cell4_voltage = (uint16_t)raw[5];
    return can_db_bms_cell_voltages1_pack(&msg, data, capacity);
}

static bool unpack_bms_cell_voltages1(const uint8_t *data, size_t len, int64_t *raw)
{
    can_db_bms_cell_voltages1_t msg = {0};
    const bool unpacked = can_db_bms_cell_voltages1_unpack(&msg, data, len);

    if (unpacked) {
        raw[0] = msg.e2e_crc;
        raw[1] = msg.e2e_counter;
        raw[2] = msg.cell1_voltage;
        raw[3] = msg.cell2_voltage;
        raw[4] = msg.cell3_voltage;
        raw[5] = msg.cell4_voltage;
    }

    return unpacked;
}

static const can_db_table_signal_t SIGNALS_BMS_CELL_VOLTAGES1[] = {
    {"E2E_Crc", 0U, 8U, false, 0, 255, 0, 255},           /* uint8_t e2e_crc */
    {"E2E_Counter", 8U, 4U, false, 0, 15, 0, 255},        /* uint8_t e2e_counter */
    {"Cell1Voltage", 16U, 12U, false, 0, 4095, 0, 65535}, /* uint16_t cell1_voltage */
    {"Cell2Voltage", 28U, 12U, false, 0, 4095, 0, 65535}, /* uint16_t cell2_voltage */
    {"Cell3Voltage", 40U, 12U, false, 0, 4095, 0, 65535}, /* uint16_t cell3_voltage */
    {"Cell4Voltage", 52U, 12U, false, 0, 4095, 0, 65535}, /* uint16_t cell4_voltage */
};

static bool pack_bms_cell_voltages2(const int64_t *raw, uint8_t *data, size_t capacity)
{
    can_db_bms_cell_voltages2_t msg;

    msg.e2e_crc = (uint8_t)raw[0];
    msg.e2e_counter = (uint8_t)raw[1];
    msg.cell5_voltage = (uint16_t)raw[2];
    msg.cell6_voltage = (uint16_t)raw[3];
    msg.cell7_voltage = (uint16_t)raw[4];
    msg.cell8_voltage = (uint16_t)raw[5];
    return can_db_bms_cell_voltages2_pack(&msg, data, capacity);
}

static bool unpack_bms_cell_voltages2(const uint8_t *data, size_t len, int64_t *raw)
{
    can_db_bms_cell_voltages2_t msg = {0};
    const bool unpacked = can_db_bms_cell_voltages2_unpack(&msg, data, len);

    if (unpacked) {
        raw[0] = msg.e2e_crc;
        raw[1] = msg.e2e_counter;
        raw[2] = msg.cell5_voltage;
        raw[3] = msg.cell6_voltage;
        raw[4] = msg.cell7_voltage;
        raw[5] = msg.cell8_voltage;
    }

    return unpacked;
}

static const can_db_table_signal_t SIGNALS_BMS_CELL_VOLTAGES2[] = {
    {"E2E_Crc", 0U, 8U, false, 0, 255, 0, 255},           /* uint8_t e2e_crc */
    {"E2E_Counter", 8U, 4U, false, 0, 15, 0, 255},        /* uint8_t e2e_counter */
    {"Cell5Voltage", 16U, 12U, false, 0, 4095, 0, 65535}, /* uint16_t cell5_voltage */
    {"Cell6Voltage", 28U, 12U, false, 0, 4095, 0, 65535}, /* uint16_t cell6_voltage */
    {"Cell7Voltage", 40U, 12U, false, 0, 4095, 0, 65535}, /* uint16_t cell7_voltage */
    {"Cell8Voltage", 52U, 12U, false, 0, 4095, 0, 65535}, /* uint16_t cell8_voltage */
};

static bool pack_bms_temperatures(const int64_t *raw, uint8_t *data, size_t capacity)
{
    can_db_bms_temperatures_t msg;

    msg.e2e_crc = (uint8_t)raw[0];
    msg.e2e_counter = (uint8_t)raw[1];
    msg.temp1 = (uint8_t)raw[2];
    msg.temp2 = (uint8_t)raw[3];
    msg.temp3 = (uint8_t)raw[4];
    msg.temp4 = (uint8_t)raw[5];
    return can_db_bms_temperatures_pack(&msg, data, capacity);
}

static bool unpack_bms_temperatures(const uint8_t *data, size_t len, int64_t *raw)
{
    can_db_bms_temperatures_t msg = {0};
    const bool unpacked = can_db_bms_temperatures_unpack(&msg, data, len);

    if (unpacked) {
        raw[0] = msg.e2e_crc;
        raw[1] = msg.e2e_counter;
        raw[2] = msg.temp1;
        raw[3] = msg.temp2;
        raw[4] = msg.temp3;
        raw[5] = msg.temp4;
    }

    return unpacked;
}

static const can_db_table_signal_t SIGNALS_BMS_TEMPERATURES[] = {
    {"E2E_Crc", 0U, 8U, false, 0, 255, 0, 255},    /* uint8_t e2e_crc */
    {"E2E_Counter", 8U, 4U, false, 0, 15, 0, 255}, /* uint8_t e2e_counter */
    {"Temp1", 16U, 8U, false, 0, 255, 0, 255},     /* uint8_t temp1 */
    {"Temp2", 24U, 8U, false, 0, 255, 0, 255},     /* uint8_t temp2 */
    {"Temp3", 32U, 8U, false, 0, 255, 0, 255},     /* uint8_t temp3 */
    {"Temp4", 40U, 8U, false, 0, 255, 0, 255},     /* uint8_t temp4 */
};

static bool pack_tester_command(const int64_t *raw, uint8_t *data, size_t capacity)
{
    can_db_tester_command_t msg;

    msg.e2e_crc = (uint8_t)raw[0];
    msg.e2e_counter = (uint8_t)raw[1];
    msg.cmd_telemetry_on = (uint8_t)raw[2];
    msg.cmd_clear_faults_seq = (uint8_t)raw[3];
    return can_db_tester_command_pack(&msg, data, capacity);
}

static bool unpack_tester_command(const uint8_t *data, size_t len, int64_t *raw)
{
    can_db_tester_command_t msg = {0};
    const bool unpacked = can_db_tester_command_unpack(&msg, data, len);

    if (unpacked) {
        raw[0] = msg.e2e_crc;
        raw[1] = msg.e2e_counter;
        raw[2] = msg.cmd_telemetry_on;
        raw[3] = msg.cmd_clear_faults_seq;
    }

    return unpacked;
}

static const can_db_table_signal_t SIGNALS_TESTER_COMMAND[] = {
    {"E2E_Crc", 0U, 8U, false, 0, 255, 0, 255},            /* uint8_t e2e_crc */
    {"E2E_Counter", 8U, 4U, false, 0, 15, 0, 255},         /* uint8_t e2e_counter */
    {"CmdTelemetryOn", 16U, 1U, false, 0, 1, 0, 255},      /* uint8_t cmd_telemetry_on */
    {"CmdClearFaultsSeq", 24U, 8U, false, 0, 255, 0, 255}, /* uint8_t cmd_clear_faults_seq */
};

static bool pack_bms_task_stats(const int64_t *raw, uint8_t *data, size_t capacity)
{
    can_db_bms_task_stats_t msg;

    msg.task_index = (uint8_t)raw[0];
    msg.task_priority = (uint8_t)raw[1];
    msg.task_cpu_share = (uint16_t)raw[2];
    msg.task_stack_peak = (uint16_t)raw[3];
    msg.task_overruns = (uint8_t)raw[4];
    return can_db_bms_task_stats_pack(&msg, data, capacity);
}

static bool unpack_bms_task_stats(const uint8_t *data, size_t len, int64_t *raw)
{
    can_db_bms_task_stats_t msg = {0};
    const bool unpacked = can_db_bms_task_stats_unpack(&msg, data, len);

    if (unpacked) {
        raw[0] = msg.task_index;
        raw[1] = msg.task_priority;
        raw[2] = msg.task_cpu_share;
        raw[3] = msg.task_stack_peak;
        raw[4] = msg.task_overruns;
    }

    return unpacked;
}

static const can_db_table_signal_t SIGNALS_BMS_TASK_STATS[] = {
    {"TaskIndex", 0U, 4U, false, 0, 15, 0, 255},            /* uint8_t task_index */
    {"TaskPriority", 4U, 4U, false, 0, 15, 0, 255},         /* uint8_t task_priority */
    {"TaskCpuShare", 8U, 16U, false, 0, 65535, 0, 65535},   /* uint16_t task_cpu_share */
    {"TaskStackPeak", 24U, 16U, false, 0, 65535, 0, 65535}, /* uint16_t task_stack_peak */
    {"TaskOverruns", 40U, 8U, false, 0, 255, 0, 255},       /* uint8_t task_overruns */
};

static bool pack_bms_node_info(const int64_t *raw, uint8_t *data, size_t capacity)
{
    can_db_bms_node_info_t msg;

    msg.sw_major = (uint8_t)raw[0];
    msg.sw_minor = (uint8_t)raw[1];
    msg.sw_patch = (uint8_t)raw[2];
    msg.active_slot = (uint8_t)raw[3];
    msg.reset_cause = (uint8_t)raw[4];
    msg.uptime_seconds = (uint32_t)raw[5];
    return can_db_bms_node_info_pack(&msg, data, capacity);
}

static bool unpack_bms_node_info(const uint8_t *data, size_t len, int64_t *raw)
{
    can_db_bms_node_info_t msg = {0};
    const bool unpacked = can_db_bms_node_info_unpack(&msg, data, len);

    if (unpacked) {
        raw[0] = msg.sw_major;
        raw[1] = msg.sw_minor;
        raw[2] = msg.sw_patch;
        raw[3] = msg.active_slot;
        raw[4] = msg.reset_cause;
        raw[5] = msg.uptime_seconds;
    }

    return unpacked;
}

static const can_db_table_signal_t SIGNALS_BMS_NODE_INFO[] = {
    {"SwMajor", 0U, 8U, false, 0, 255, 0, 255},                       /* uint8_t sw_major */
    {"SwMinor", 8U, 8U, false, 0, 255, 0, 255},                       /* uint8_t sw_minor */
    {"SwPatch", 16U, 8U, false, 0, 255, 0, 255},                      /* uint8_t sw_patch */
    {"ActiveSlot", 24U, 2U, false, 0, 3, 0, 255},                     /* uint8_t active_slot */
    {"ResetCause", 26U, 6U, false, 0, 63, 0, 255},                    /* uint8_t reset_cause */
    {"UptimeSeconds", 32U, 32U, false, 0, 4294967295, 0, 4294967295}, /* uint32_t uptime_seconds */
};

static bool pack_bms_boot_info(const int64_t *raw, uint8_t *data, size_t capacity)
{
    can_db_bms_boot_info_t msg;

    msg.boot_mode = (uint8_t)raw[0];
    msg.boot_attempt = (uint8_t)raw[1];
    msg.boot_event = (uint8_t)raw[2];
    msg.confirm_state = (uint8_t)raw[3];
    msg.boot_event_slot = (uint8_t)raw[4];
    msg.image_version_major = (uint8_t)raw[5];
    msg.image_version_minor = (uint8_t)raw[6];
    msg.image_version_patch = (uint16_t)raw[7];
    msg.confirm_seconds = (uint8_t)raw[8];
    return can_db_bms_boot_info_pack(&msg, data, capacity);
}

static bool unpack_bms_boot_info(const uint8_t *data, size_t len, int64_t *raw)
{
    can_db_bms_boot_info_t msg = {0};
    const bool unpacked = can_db_bms_boot_info_unpack(&msg, data, len);

    if (unpacked) {
        raw[0] = msg.boot_mode;
        raw[1] = msg.boot_attempt;
        raw[2] = msg.boot_event;
        raw[3] = msg.confirm_state;
        raw[4] = msg.boot_event_slot;
        raw[5] = msg.image_version_major;
        raw[6] = msg.image_version_minor;
        raw[7] = msg.image_version_patch;
        raw[8] = msg.confirm_seconds;
    }

    return unpacked;
}

static const can_db_table_signal_t SIGNALS_BMS_BOOT_INFO[] = {
    {"BootMode", 0U, 2U, false, 0, 3, 0, 255},                  /* uint8_t boot_mode */
    {"BootAttempt", 2U, 2U, false, 0, 3, 0, 255},               /* uint8_t boot_attempt */
    {"BootEvent", 4U, 3U, false, 0, 7, 0, 255},                 /* uint8_t boot_event */
    {"ConfirmState", 8U, 3U, false, 0, 7, 0, 255},              /* uint8_t confirm_state */
    {"BootEventSlot", 11U, 2U, false, 0, 3, 0, 255},            /* uint8_t boot_event_slot */
    {"ImageVersionMajor", 16U, 8U, false, 0, 255, 0, 255},      /* uint8_t image_version_major */
    {"ImageVersionMinor", 24U, 8U, false, 0, 255, 0, 255},      /* uint8_t image_version_minor */
    {"ImageVersionPatch", 32U, 16U, false, 0, 65535, 0, 65535}, /* uint16_t image_version_patch */
    {"ConfirmSeconds", 48U, 8U, false, 0, 255, 0, 255},         /* uint8_t confirm_seconds */
};

const can_db_table_message_t can_db_table[] = {
    {
        .name = "BMS_Fault",
        .id = CAN_DB_BMS_FAULT_ID,
        .dlc = CAN_DB_BMS_FAULT_DLC,
        .cycle_ms = CAN_DB_BMS_FAULT_CYCLE_MS,
        .e2e_data_id = 272,
        .signal_count = 7U,
        .signals = SIGNALS_BMS_FAULT,
        .pack = pack_bms_fault,
        .unpack = unpack_bms_fault,
    },
    {
        .name = "BMS_PackStatus",
        .id = CAN_DB_BMS_PACK_STATUS_ID,
        .dlc = CAN_DB_BMS_PACK_STATUS_DLC,
        .cycle_ms = CAN_DB_BMS_PACK_STATUS_CYCLE_MS,
        .e2e_data_id = 288,
        .signal_count = 8U,
        .signals = SIGNALS_BMS_PACK_STATUS,
        .pack = pack_bms_pack_status,
        .unpack = unpack_bms_pack_status,
    },
    {
        .name = "BMS_Heartbeat",
        .id = CAN_DB_BMS_HEARTBEAT_ID,
        .dlc = CAN_DB_BMS_HEARTBEAT_DLC,
        .cycle_ms = CAN_DB_BMS_HEARTBEAT_CYCLE_MS,
        .e2e_data_id = -1,
        .signal_count = 2U,
        .signals = SIGNALS_BMS_HEARTBEAT,
        .pack = pack_bms_heartbeat,
        .unpack = unpack_bms_heartbeat,
    },
    {
        .name = "BMS_CellVoltages1",
        .id = CAN_DB_BMS_CELL_VOLTAGES1_ID,
        .dlc = CAN_DB_BMS_CELL_VOLTAGES1_DLC,
        .cycle_ms = CAN_DB_BMS_CELL_VOLTAGES1_CYCLE_MS,
        .e2e_data_id = 320,
        .signal_count = 6U,
        .signals = SIGNALS_BMS_CELL_VOLTAGES1,
        .pack = pack_bms_cell_voltages1,
        .unpack = unpack_bms_cell_voltages1,
    },
    {
        .name = "BMS_CellVoltages2",
        .id = CAN_DB_BMS_CELL_VOLTAGES2_ID,
        .dlc = CAN_DB_BMS_CELL_VOLTAGES2_DLC,
        .cycle_ms = CAN_DB_BMS_CELL_VOLTAGES2_CYCLE_MS,
        .e2e_data_id = 321,
        .signal_count = 6U,
        .signals = SIGNALS_BMS_CELL_VOLTAGES2,
        .pack = pack_bms_cell_voltages2,
        .unpack = unpack_bms_cell_voltages2,
    },
    {
        .name = "BMS_Temperatures",
        .id = CAN_DB_BMS_TEMPERATURES_ID,
        .dlc = CAN_DB_BMS_TEMPERATURES_DLC,
        .cycle_ms = CAN_DB_BMS_TEMPERATURES_CYCLE_MS,
        .e2e_data_id = 336,
        .signal_count = 6U,
        .signals = SIGNALS_BMS_TEMPERATURES,
        .pack = pack_bms_temperatures,
        .unpack = unpack_bms_temperatures,
    },
    {
        .name = "TESTER_Command",
        .id = CAN_DB_TESTER_COMMAND_ID,
        .dlc = CAN_DB_TESTER_COMMAND_DLC,
        .cycle_ms = CAN_DB_TESTER_COMMAND_CYCLE_MS,
        .e2e_data_id = 512,
        .signal_count = 4U,
        .signals = SIGNALS_TESTER_COMMAND,
        .pack = pack_tester_command,
        .unpack = unpack_tester_command,
    },
    {
        .name = "BMS_TaskStats",
        .id = CAN_DB_BMS_TASK_STATS_ID,
        .dlc = CAN_DB_BMS_TASK_STATS_DLC,
        .cycle_ms = CAN_DB_BMS_TASK_STATS_CYCLE_MS,
        .e2e_data_id = -1,
        .signal_count = 5U,
        .signals = SIGNALS_BMS_TASK_STATS,
        .pack = pack_bms_task_stats,
        .unpack = unpack_bms_task_stats,
    },
    {
        .name = "BMS_NodeInfo",
        .id = CAN_DB_BMS_NODE_INFO_ID,
        .dlc = CAN_DB_BMS_NODE_INFO_DLC,
        .cycle_ms = CAN_DB_BMS_NODE_INFO_CYCLE_MS,
        .e2e_data_id = -1,
        .signal_count = 6U,
        .signals = SIGNALS_BMS_NODE_INFO,
        .pack = pack_bms_node_info,
        .unpack = unpack_bms_node_info,
    },
    {
        .name = "BMS_BootInfo",
        .id = CAN_DB_BMS_BOOT_INFO_ID,
        .dlc = CAN_DB_BMS_BOOT_INFO_DLC,
        .cycle_ms = CAN_DB_BMS_BOOT_INFO_CYCLE_MS,
        .e2e_data_id = -1,
        .signal_count = 9U,
        .signals = SIGNALS_BMS_BOOT_INFO,
        .pack = pack_bms_boot_info,
        .unpack = unpack_bms_boot_info,
    },
    {
        .name = "TESTER_Ping",
        .id = CAN_DB_TESTER_PING_ID,
        .dlc = CAN_DB_TESTER_PING_DLC,
        .cycle_ms = CAN_DB_TESTER_PING_CYCLE_MS,
        .e2e_data_id = -1,
        .signal_count = 0U,
        .signals = NULL,
        .pack = NULL,
        .unpack = NULL,
    },
    {
        .name = "BMS_Pong",
        .id = CAN_DB_BMS_PONG_ID,
        .dlc = CAN_DB_BMS_PONG_DLC,
        .cycle_ms = CAN_DB_BMS_PONG_CYCLE_MS,
        .e2e_data_id = -1,
        .signal_count = 0U,
        .signals = NULL,
        .pack = NULL,
        .unpack = NULL,
    },
    {
        .name = "UDS_Request",
        .id = CAN_DB_UDS_REQUEST_ID,
        .dlc = CAN_DB_UDS_REQUEST_DLC,
        .cycle_ms = CAN_DB_UDS_REQUEST_CYCLE_MS,
        .e2e_data_id = -1,
        .signal_count = 0U,
        .signals = NULL,
        .pack = NULL,
        .unpack = NULL,
    },
    {
        .name = "UDS_Response",
        .id = CAN_DB_UDS_RESPONSE_ID,
        .dlc = CAN_DB_UDS_RESPONSE_DLC,
        .cycle_ms = CAN_DB_UDS_RESPONSE_CYCLE_MS,
        .e2e_data_id = -1,
        .signal_count = 0U,
        .signals = NULL,
        .pack = NULL,
        .unpack = NULL,
    },
};

const size_t can_db_table_count = sizeof(can_db_table) / sizeof(can_db_table[0]);
