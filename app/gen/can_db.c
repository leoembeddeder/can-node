/* SPDX-License-Identifier: MIT */
/*
 * The codec of can_db.h. Every pack and unpack function is a list of calls of the helpers below,
 * one per signal, with the signal's start bit and length as constants; the helpers do the bit work
 * for every signal alike.
 *
 * GENERATED FILE, DO NOT EDIT. The implementation of the codec. Made by tools/gen_can_db.py from
 * dbc/fcn.dbc, SHA-256 ee5a3fa516d3029851805544da6004843add051153e5492607e90ef8e1881e18.
 *
 * After a change of the DBC, run scripts/codegen-check.sh --update. Without --update the script
 * fails while this file is out of date; CI and scripts/check.sh run it.
 */
#include "can_db.h"

#include <string.h>

/* The number of bits from frame bit `pos` up to the end of its byte, at most `remaining`. */
static uint32_t chunk_bits(uint32_t pos, uint32_t remaining)
{
    uint32_t chunk = 8U - (pos % 8U);

    if (chunk > remaining) {
        chunk = remaining;
    }

    return chunk;
}

/*
 * Write the `length` low bits of `raw` into the frame from frame bit `start` on: bit i of
 * `raw` goes to bit (start + i) % 8 of data[(start + i) / 8]. Bits of `raw` above `length` are
 * not written. The bits are ORed in, so the frame must hold 0 there; pack() clears it first.
 * The generator has checked 1 <= length <= 32 and that the bits lie inside the DLC.
 */
static void put_bits(uint8_t *data, uint32_t start, uint32_t length, uint32_t raw)
{
    uint32_t done = 0U;

    while (done < length) {
        const uint32_t pos = start + done;
        const uint32_t chunk = chunk_bits(pos, length - done);
        const uint32_t part = (raw >> done) & (0xFFU >> (8U - chunk));

        data[pos / 8U] |= (uint8_t)(part << (pos % 8U));
        done += chunk;
    }
}

/* The `length`-bit value from frame bit `start` on, as put_bits() wrote it; 0 above `length`. */
static uint32_t get_bits(const uint8_t *data, uint32_t start, uint32_t length)
{
    uint32_t raw = 0U;
    uint32_t done = 0U;

    while (done < length) {
        const uint32_t pos = start + done;
        const uint32_t chunk = chunk_bits(pos, length - done);
        const uint32_t part = ((uint32_t)data[pos / 8U] >> (pos % 8U)) & (0xFFU >> (8U - chunk));

        raw |= part << done;
        done += chunk;
    }

    return raw;
}

/*
 * The value of the `length`-bit two's complement number in `raw`, whose bits above `length`
 * are 0. A negative number is computed as -magnitude - 1, with magnitude the complement of its
 * low length - 1 bits, so that no unsigned value outside the range of int32_t is ever
 * converted to it (C leaves the result of that conversion to the implementation).
 */
static int32_t to_signed(uint32_t raw, uint32_t length)
{
    const uint32_t sign = (uint32_t)1U << (length - 1U);
    int32_t value;

    if ((raw & sign) != 0U) {
        const uint32_t magnitude = (~raw) & (sign - 1U);

        value = (-(int32_t)magnitude) - 1;
    } else {
        value = (int32_t)raw;
    }

    return value;
}

bool can_db_bms_fault_pack(const can_db_bms_fault_t *msg, uint8_t *data, size_t capacity)
{
    bool packed = false;

    if ((msg != NULL) && (data != NULL) && (capacity >= CAN_DB_BMS_FAULT_DLC)) {
        (void)memset(data, 0, CAN_DB_BMS_FAULT_DLC);
        put_bits(data, 0U, 8U, msg->e2e_crc);
        put_bits(data, 8U, 4U, msg->e2e_counter);
        put_bits(data, 16U, 17U, msg->fault_active);
        put_bits(data, 33U, 17U, msg->fault_confirmed);
        put_bits(data, 50U, 5U, msg->fault_most_recent);
        put_bits(data, 55U, 1U, msg->fault_warning);
        put_bits(data, 56U, 8U, msg->fault_clear_count);
        packed = true;
    }

    return packed;
}

bool can_db_bms_fault_unpack(can_db_bms_fault_t *msg, const uint8_t *data, size_t len)
{
    bool unpacked = false;

    if ((msg != NULL) && (data != NULL) && (len == CAN_DB_BMS_FAULT_DLC)) {
        msg->e2e_crc = (uint8_t)get_bits(data, 0U, 8U);
        msg->e2e_counter = (uint8_t)get_bits(data, 8U, 4U);
        msg->fault_active = get_bits(data, 16U, 17U);
        msg->fault_confirmed = get_bits(data, 33U, 17U);
        msg->fault_most_recent = (uint8_t)get_bits(data, 50U, 5U);
        msg->fault_warning = (uint8_t)get_bits(data, 55U, 1U);
        msg->fault_clear_count = (uint8_t)get_bits(data, 56U, 8U);
        unpacked = true;
    }

    return unpacked;
}

bool can_db_bms_pack_status_pack(const can_db_bms_pack_status_t *msg, uint8_t *data,
                                 size_t capacity)
{
    bool packed = false;

    if ((msg != NULL) && (data != NULL) && (capacity >= CAN_DB_BMS_PACK_STATUS_DLC)) {
        (void)memset(data, 0, CAN_DB_BMS_PACK_STATUS_DLC);
        put_bits(data, 0U, 8U, msg->e2e_crc);
        put_bits(data, 8U, 4U, msg->e2e_counter);
        put_bits(data, 16U, 16U, (uint32_t)msg->pack_current);
        put_bits(data, 32U, 16U, msg->pack_voltage);
        put_bits(data, 48U, 2U, msg->pack_state);
        put_bits(data, 50U, 1U, msg->pack_fault_active);
        put_bits(data, 51U, 1U, msg->pack_current_valid);
        put_bits(data, 52U, 1U, msg->pack_voltage_valid);
        packed = true;
    }

    return packed;
}

bool can_db_bms_pack_status_unpack(can_db_bms_pack_status_t *msg, const uint8_t *data, size_t len)
{
    bool unpacked = false;

    if ((msg != NULL) && (data != NULL) && (len == CAN_DB_BMS_PACK_STATUS_DLC)) {
        msg->e2e_crc = (uint8_t)get_bits(data, 0U, 8U);
        msg->e2e_counter = (uint8_t)get_bits(data, 8U, 4U);
        msg->pack_current = (int16_t)to_signed(get_bits(data, 16U, 16U), 16U);
        msg->pack_voltage = (uint16_t)get_bits(data, 32U, 16U);
        msg->pack_state = (uint8_t)get_bits(data, 48U, 2U);
        msg->pack_fault_active = (uint8_t)get_bits(data, 50U, 1U);
        msg->pack_current_valid = (uint8_t)get_bits(data, 51U, 1U);
        msg->pack_voltage_valid = (uint8_t)get_bits(data, 52U, 1U);
        unpacked = true;
    }

    return unpacked;
}

bool can_db_bms_heartbeat_pack(const can_db_bms_heartbeat_t *msg, uint8_t *data, size_t capacity)
{
    bool packed = false;

    if ((msg != NULL) && (data != NULL) && (capacity >= CAN_DB_BMS_HEARTBEAT_DLC)) {
        (void)memset(data, 0, CAN_DB_BMS_HEARTBEAT_DLC);
        put_bits(data, 0U, 32U, msg->period_counter);
        put_bits(data, 32U, 32U, msg->tick_count);
        packed = true;
    }

    return packed;
}

bool can_db_bms_heartbeat_unpack(can_db_bms_heartbeat_t *msg, const uint8_t *data, size_t len)
{
    bool unpacked = false;

    if ((msg != NULL) && (data != NULL) && (len == CAN_DB_BMS_HEARTBEAT_DLC)) {
        msg->period_counter = get_bits(data, 0U, 32U);
        msg->tick_count = get_bits(data, 32U, 32U);
        unpacked = true;
    }

    return unpacked;
}

bool can_db_bms_cell_voltages1_pack(const can_db_bms_cell_voltages1_t *msg, uint8_t *data,
                                    size_t capacity)
{
    bool packed = false;

    if ((msg != NULL) && (data != NULL) && (capacity >= CAN_DB_BMS_CELL_VOLTAGES1_DLC)) {
        (void)memset(data, 0, CAN_DB_BMS_CELL_VOLTAGES1_DLC);
        put_bits(data, 0U, 8U, msg->e2e_crc);
        put_bits(data, 8U, 4U, msg->e2e_counter);
        put_bits(data, 16U, 12U, msg->cell1_voltage);
        put_bits(data, 28U, 12U, msg->cell2_voltage);
        put_bits(data, 40U, 12U, msg->cell3_voltage);
        put_bits(data, 52U, 12U, msg->cell4_voltage);
        packed = true;
    }

    return packed;
}

bool can_db_bms_cell_voltages1_unpack(can_db_bms_cell_voltages1_t *msg, const uint8_t *data,
                                      size_t len)
{
    bool unpacked = false;

    if ((msg != NULL) && (data != NULL) && (len == CAN_DB_BMS_CELL_VOLTAGES1_DLC)) {
        msg->e2e_crc = (uint8_t)get_bits(data, 0U, 8U);
        msg->e2e_counter = (uint8_t)get_bits(data, 8U, 4U);
        msg->cell1_voltage = (uint16_t)get_bits(data, 16U, 12U);
        msg->cell2_voltage = (uint16_t)get_bits(data, 28U, 12U);
        msg->cell3_voltage = (uint16_t)get_bits(data, 40U, 12U);
        msg->cell4_voltage = (uint16_t)get_bits(data, 52U, 12U);
        unpacked = true;
    }

    return unpacked;
}

bool can_db_bms_cell_voltages2_pack(const can_db_bms_cell_voltages2_t *msg, uint8_t *data,
                                    size_t capacity)
{
    bool packed = false;

    if ((msg != NULL) && (data != NULL) && (capacity >= CAN_DB_BMS_CELL_VOLTAGES2_DLC)) {
        (void)memset(data, 0, CAN_DB_BMS_CELL_VOLTAGES2_DLC);
        put_bits(data, 0U, 8U, msg->e2e_crc);
        put_bits(data, 8U, 4U, msg->e2e_counter);
        put_bits(data, 16U, 12U, msg->cell5_voltage);
        put_bits(data, 28U, 12U, msg->cell6_voltage);
        put_bits(data, 40U, 12U, msg->cell7_voltage);
        put_bits(data, 52U, 12U, msg->cell8_voltage);
        packed = true;
    }

    return packed;
}

bool can_db_bms_cell_voltages2_unpack(can_db_bms_cell_voltages2_t *msg, const uint8_t *data,
                                      size_t len)
{
    bool unpacked = false;

    if ((msg != NULL) && (data != NULL) && (len == CAN_DB_BMS_CELL_VOLTAGES2_DLC)) {
        msg->e2e_crc = (uint8_t)get_bits(data, 0U, 8U);
        msg->e2e_counter = (uint8_t)get_bits(data, 8U, 4U);
        msg->cell5_voltage = (uint16_t)get_bits(data, 16U, 12U);
        msg->cell6_voltage = (uint16_t)get_bits(data, 28U, 12U);
        msg->cell7_voltage = (uint16_t)get_bits(data, 40U, 12U);
        msg->cell8_voltage = (uint16_t)get_bits(data, 52U, 12U);
        unpacked = true;
    }

    return unpacked;
}

bool can_db_bms_temperatures_pack(const can_db_bms_temperatures_t *msg, uint8_t *data,
                                  size_t capacity)
{
    bool packed = false;

    if ((msg != NULL) && (data != NULL) && (capacity >= CAN_DB_BMS_TEMPERATURES_DLC)) {
        (void)memset(data, 0, CAN_DB_BMS_TEMPERATURES_DLC);
        put_bits(data, 0U, 8U, msg->e2e_crc);
        put_bits(data, 8U, 4U, msg->e2e_counter);
        put_bits(data, 16U, 8U, msg->temp1);
        put_bits(data, 24U, 8U, msg->temp2);
        put_bits(data, 32U, 8U, msg->temp3);
        put_bits(data, 40U, 8U, msg->temp4);
        packed = true;
    }

    return packed;
}

bool can_db_bms_temperatures_unpack(can_db_bms_temperatures_t *msg, const uint8_t *data, size_t len)
{
    bool unpacked = false;

    if ((msg != NULL) && (data != NULL) && (len == CAN_DB_BMS_TEMPERATURES_DLC)) {
        msg->e2e_crc = (uint8_t)get_bits(data, 0U, 8U);
        msg->e2e_counter = (uint8_t)get_bits(data, 8U, 4U);
        msg->temp1 = (uint8_t)get_bits(data, 16U, 8U);
        msg->temp2 = (uint8_t)get_bits(data, 24U, 8U);
        msg->temp3 = (uint8_t)get_bits(data, 32U, 8U);
        msg->temp4 = (uint8_t)get_bits(data, 40U, 8U);
        unpacked = true;
    }

    return unpacked;
}

bool can_db_tester_command_pack(const can_db_tester_command_t *msg, uint8_t *data, size_t capacity)
{
    bool packed = false;

    if ((msg != NULL) && (data != NULL) && (capacity >= CAN_DB_TESTER_COMMAND_DLC)) {
        (void)memset(data, 0, CAN_DB_TESTER_COMMAND_DLC);
        put_bits(data, 0U, 8U, msg->e2e_crc);
        put_bits(data, 8U, 4U, msg->e2e_counter);
        put_bits(data, 16U, 1U, msg->cmd_telemetry_on);
        put_bits(data, 24U, 8U, msg->cmd_clear_faults_seq);
        packed = true;
    }

    return packed;
}

bool can_db_tester_command_unpack(can_db_tester_command_t *msg, const uint8_t *data, size_t len)
{
    bool unpacked = false;

    if ((msg != NULL) && (data != NULL) && (len == CAN_DB_TESTER_COMMAND_DLC)) {
        msg->e2e_crc = (uint8_t)get_bits(data, 0U, 8U);
        msg->e2e_counter = (uint8_t)get_bits(data, 8U, 4U);
        msg->cmd_telemetry_on = (uint8_t)get_bits(data, 16U, 1U);
        msg->cmd_clear_faults_seq = (uint8_t)get_bits(data, 24U, 8U);
        unpacked = true;
    }

    return unpacked;
}

bool can_db_bms_task_stats_pack(const can_db_bms_task_stats_t *msg, uint8_t *data, size_t capacity)
{
    bool packed = false;

    if ((msg != NULL) && (data != NULL) && (capacity >= CAN_DB_BMS_TASK_STATS_DLC)) {
        (void)memset(data, 0, CAN_DB_BMS_TASK_STATS_DLC);
        put_bits(data, 0U, 4U, msg->task_index);
        put_bits(data, 4U, 4U, msg->task_priority);
        put_bits(data, 8U, 16U, msg->task_cpu_share);
        put_bits(data, 24U, 16U, msg->task_stack_peak);
        put_bits(data, 40U, 8U, msg->task_overruns);
        packed = true;
    }

    return packed;
}

bool can_db_bms_task_stats_unpack(can_db_bms_task_stats_t *msg, const uint8_t *data, size_t len)
{
    bool unpacked = false;

    if ((msg != NULL) && (data != NULL) && (len == CAN_DB_BMS_TASK_STATS_DLC)) {
        msg->task_index = (uint8_t)get_bits(data, 0U, 4U);
        msg->task_priority = (uint8_t)get_bits(data, 4U, 4U);
        msg->task_cpu_share = (uint16_t)get_bits(data, 8U, 16U);
        msg->task_stack_peak = (uint16_t)get_bits(data, 24U, 16U);
        msg->task_overruns = (uint8_t)get_bits(data, 40U, 8U);
        unpacked = true;
    }

    return unpacked;
}

bool can_db_bms_node_info_pack(const can_db_bms_node_info_t *msg, uint8_t *data, size_t capacity)
{
    bool packed = false;

    if ((msg != NULL) && (data != NULL) && (capacity >= CAN_DB_BMS_NODE_INFO_DLC)) {
        (void)memset(data, 0, CAN_DB_BMS_NODE_INFO_DLC);
        put_bits(data, 0U, 8U, msg->sw_major);
        put_bits(data, 8U, 8U, msg->sw_minor);
        put_bits(data, 16U, 8U, msg->sw_patch);
        put_bits(data, 24U, 2U, msg->active_slot);
        put_bits(data, 26U, 6U, msg->reset_cause);
        put_bits(data, 32U, 32U, msg->uptime_seconds);
        packed = true;
    }

    return packed;
}

bool can_db_bms_node_info_unpack(can_db_bms_node_info_t *msg, const uint8_t *data, size_t len)
{
    bool unpacked = false;

    if ((msg != NULL) && (data != NULL) && (len == CAN_DB_BMS_NODE_INFO_DLC)) {
        msg->sw_major = (uint8_t)get_bits(data, 0U, 8U);
        msg->sw_minor = (uint8_t)get_bits(data, 8U, 8U);
        msg->sw_patch = (uint8_t)get_bits(data, 16U, 8U);
        msg->active_slot = (uint8_t)get_bits(data, 24U, 2U);
        msg->reset_cause = (uint8_t)get_bits(data, 26U, 6U);
        msg->uptime_seconds = get_bits(data, 32U, 32U);
        unpacked = true;
    }

    return unpacked;
}

bool can_db_bms_boot_info_pack(const can_db_bms_boot_info_t *msg, uint8_t *data, size_t capacity)
{
    bool packed = false;

    if ((msg != NULL) && (data != NULL) && (capacity >= CAN_DB_BMS_BOOT_INFO_DLC)) {
        (void)memset(data, 0, CAN_DB_BMS_BOOT_INFO_DLC);
        put_bits(data, 0U, 2U, msg->boot_mode);
        put_bits(data, 2U, 2U, msg->boot_attempt);
        put_bits(data, 4U, 3U, msg->boot_event);
        put_bits(data, 8U, 3U, msg->confirm_state);
        put_bits(data, 11U, 2U, msg->boot_event_slot);
        put_bits(data, 16U, 8U, msg->image_version_major);
        put_bits(data, 24U, 8U, msg->image_version_minor);
        put_bits(data, 32U, 16U, msg->image_version_patch);
        put_bits(data, 48U, 8U, msg->confirm_seconds);
        packed = true;
    }

    return packed;
}

bool can_db_bms_boot_info_unpack(can_db_bms_boot_info_t *msg, const uint8_t *data, size_t len)
{
    bool unpacked = false;

    if ((msg != NULL) && (data != NULL) && (len == CAN_DB_BMS_BOOT_INFO_DLC)) {
        msg->boot_mode = (uint8_t)get_bits(data, 0U, 2U);
        msg->boot_attempt = (uint8_t)get_bits(data, 2U, 2U);
        msg->boot_event = (uint8_t)get_bits(data, 4U, 3U);
        msg->confirm_state = (uint8_t)get_bits(data, 8U, 3U);
        msg->boot_event_slot = (uint8_t)get_bits(data, 11U, 2U);
        msg->image_version_major = (uint8_t)get_bits(data, 16U, 8U);
        msg->image_version_minor = (uint8_t)get_bits(data, 24U, 8U);
        msg->image_version_patch = (uint16_t)get_bits(data, 32U, 16U);
        msg->confirm_seconds = (uint8_t)get_bits(data, 48U, 8U);
        unpacked = true;
    }

    return unpacked;
}
