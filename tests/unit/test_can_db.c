/* SPDX-License-Identifier: MIT */
/*
 * The CAN codec generated from dbc/fcn.dbc (app/gen/can_db.c).
 *
 * Two kinds of test:
 *
 *   - Frames worked out by hand from the DBC, one or more per message: the bytes are written
 *     down here, not computed, so they hold the codec to the layout of the DBC independently of
 *     the generator. The heartbeat is also held to the phase 1 status frame, which every bus
 *     test of phase 1 counts on.
 *   - Properties checked for every signal of every message, through the table of
 *     tests/tools/can_db_table.h: round trip, field isolation (a signal changes its own bits
 *     and no others), sign extension, masking, and that bits outside all signals stay 0. The
 *     table is generated with the codec; tests/python/test_gen_can_db.py checks it, and the
 *     codec, against cantools.
 *
 * Message identifiers, sizes and cycle times are written as the literals of the message set,
 * so that a change of the DBC shows up here.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "can_db.h"
#include "can_db_table.h"
#include "unity.h"

#define FILL 0xA5U /* what a buffer holds before a call that must not touch it */

static uint32_t s_random_state;

static uint32_t random_u32(void)
{
    s_random_state ^= s_random_state << 13U;
    s_random_state ^= s_random_state >> 17U;
    s_random_state ^= s_random_state << 5U;
    return s_random_state;
}

void setUp(void)
{
    s_random_state = 0xDBC0FFEEU;
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------------------- */

static bool frame_bit(const uint8_t *data, uint32_t bit)
{
    return (((uint32_t)data[bit / 8U] >> (bit % 8U)) & 1U) != 0U;
}

/* The low `length` bits of `value`, two's complement for a negative one: what a signal of that
 * length carries for it. */
static uint64_t low_bits(int64_t value, uint32_t length)
{
    return (uint64_t)value & ((UINT64_C(1) << length) - 1U);
}

/* Bit i of `pattern` is frame bit start + i, for i < length; every other bit of the frame is 0.
 * (DLC 8 at most and length 32 at most, so the shifts stay inside 64 bits.) */
static void expect_frame(const uint8_t *data, uint8_t dlc, uint32_t start, uint32_t length,
                         uint64_t pattern, const char *what)
{
    for (uint32_t bit = 0U; bit < (8U * dlc); bit++) {
        const bool inside = (bit >= start) && (bit < (start + length));
        const bool expected = inside && (((pattern >> (bit - start)) & 1U) != 0U);

        if (frame_bit(data, bit) != expected) {
            char message[160];

            (void)snprintf(message, sizeof(message), "%s: frame bit %u is %d, expected %d", what,
                           (unsigned int)bit, frame_bit(data, bit) ? 1 : 0, expected ? 1 : 0);
            TEST_FAIL_MESSAGE(message);
        }
    }
}

/* All bits of the frame are 0 except bits [start, start + length), which are 1. */
static void expect_only_bits(const uint8_t *data, uint8_t dlc, uint32_t start, uint32_t length,
                             const char *what)
{
    expect_frame(data, dlc, start, length, low_bits(-1, length), what);
}

/* A raw value whose low `length` bits are all 1, in the representation of the table: -1 for a
 * signed signal. */
static int64_t all_ones(const can_db_table_signal_t *signal)
{
    return signal->is_signed ? -1 : (int64_t)((UINT64_C(1) << signal->length) - 1U);
}

/* A random raw value of the signal, uniform over its raw range. */
static int64_t random_raw(const can_db_table_signal_t *signal)
{
    const uint64_t span = (uint64_t)(signal->raw_max - signal->raw_min) + 1U;
    const uint64_t draw = ((uint64_t)random_u32() << 32U) | random_u32();

    return signal->raw_min + (int64_t)(draw % span);
}

static const can_db_table_message_t *table_message(uint32_t id)
{
    const can_db_table_message_t *found = NULL;

    for (size_t i = 0U; i < can_db_table_count; i++) {
        if (can_db_table[i].id == id) {
            found = &can_db_table[i];
        }
    }

    TEST_ASSERT_NOT_NULL_MESSAGE(found, "message not in the table");
    return found;
}

/* ------------------------------------------------------------------------------------------
 * The message set
 * ---------------------------------------------------------------------------------------- */

static void test_the_message_set_is_the_one_of_the_design(void)
{
    /* identifier, DLC, cycle time in ms, Data ID or -1 */
    static const struct {
        uint32_t id;
        uint8_t dlc;
        uint16_t cycle_ms;
        int32_t data_id;
    } expected[] = {
        {0x010U, 8U, 100U, 0x0110}, /* BMS_Fault */
        {0x020U, 8U, 10U, 0x0120},  /* BMS_PackStatus */
        {0x030U, 8U, 10U, -1},      /* BMS_Heartbeat */
        {0x040U, 8U, 20U, 0x0140},  /* BMS_CellVoltages1 */
        {0x041U, 8U, 20U, 0x0141},  /* BMS_CellVoltages2 */
        {0x050U, 8U, 100U, 0x0150}, /* BMS_Temperatures */
        {0x100U, 8U, 100U, 0x0200}, /* TESTER_Command */
        {0x600U, 8U, 200U, -1},     /* BMS_TaskStats */
        {0x610U, 8U, 1000U, -1},    /* BMS_NodeInfo */
        {0x611U, 8U, 1000U, -1},    /* BMS_BootInfo */
        {0x6F0U, 8U, 0U, -1},       /* TESTER_Ping */
        {0x6F1U, 8U, 0U, -1},       /* BMS_Pong */
        {0x7E0U, 8U, 0U, -1},       /* UDS_Request */
        {0x7E8U, 8U, 0U, -1},       /* UDS_Response */
    };

    TEST_ASSERT_EQUAL_size_t(sizeof(expected) / sizeof(expected[0]), can_db_table_count);
    for (size_t i = 0U; i < can_db_table_count; i++) {
        TEST_ASSERT_EQUAL_HEX32(expected[i].id, can_db_table[i].id);
        TEST_ASSERT_EQUAL_UINT8(expected[i].dlc, can_db_table[i].dlc);
        TEST_ASSERT_EQUAL_UINT16(expected[i].cycle_ms, can_db_table[i].cycle_ms);
        TEST_ASSERT_EQUAL_INT32(expected[i].data_id, can_db_table[i].e2e_data_id);
    }

    /* The macros of the header say the same. */
    TEST_ASSERT_EQUAL_HEX32(0x020U, CAN_DB_BMS_PACK_STATUS_ID);
    TEST_ASSERT_EQUAL_UINT32(1U, CAN_DB_BMS_PACK_STATUS_E2E_PROTECTED);
    TEST_ASSERT_EQUAL_HEX32(0x0120U, CAN_DB_BMS_PACK_STATUS_E2E_DATA_ID);
    TEST_ASSERT_EQUAL_HEX32(0x030U, CAN_DB_BMS_HEARTBEAT_ID);
    TEST_ASSERT_EQUAL_UINT32(10U, CAN_DB_BMS_HEARTBEAT_CYCLE_MS);
    TEST_ASSERT_EQUAL_UINT32(0U, CAN_DB_BMS_HEARTBEAT_E2E_PROTECTED);
    TEST_ASSERT_EQUAL_HEX32(0x200U, CAN_DB_TESTER_COMMAND_E2E_DATA_ID);
    TEST_ASSERT_EQUAL_UINT32(0U, CAN_DB_TESTER_PING_CYCLE_MS);
}

/*
 * The scaling constants are what hand-written conversions will _Static_assert on, one relation
 * per assertion. (Two comparisons joined by && that expand to the same tokens, such as
 * FACTOR_NUM == 1 and OFFSET_DEN == 1, are reported by clang-tidy's misc-redundant-expression.)
 */
static void test_the_scaling_is_given_as_exact_fractions(void)
{
    /* Cells: 1 mV per bit, offset 1 V, so millivolts = raw + 1000. */
    _Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_FACTOR_NUM == 1, "cells: 1/1000 V");
    _Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_FACTOR_DEN == 1000, "cells: 1/1000 V");
    _Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_OFFSET_NUM == 1, "cells: offset 1 V");
    _Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_OFFSET_DEN == 1, "cells: offset 1 V");
    /* Temperatures: 1 degC per bit, offset -40 degC. */
    _Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP4_FACTOR_NUM == 1, "temperatures: 1 degC");
    _Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP4_FACTOR_DEN == 1, "temperatures: 1 degC");
    _Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP4_OFFSET_NUM == -40, "temperatures: -40 degC");
    _Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP4_OFFSET_DEN == 1, "temperatures: -40 degC");
    /* Current 10 mA, pack voltage 1 mV, CPU share 0.01 % per bit. */
    _Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_FACTOR_DEN == 100, "current: 1/100 A");
    _Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_FACTOR_DEN == 1000, "voltage: 1/1000 V");
    _Static_assert(CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_FACTOR_DEN == 100, "CPU share: 1/100 %");

    TEST_ASSERT_EQUAL_INT32(1, CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_FACTOR_NUM);
    TEST_ASSERT_EQUAL_INT32(0, CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_OFFSET_NUM);
    TEST_ASSERT_EQUAL_INT32(1, CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_FACTOR_NUM);

    /* SNA codes and value tables. */
    TEST_ASSERT_EQUAL_UINT32(4095U, CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_SNA);
    TEST_ASSERT_EQUAL_UINT32(4095U, CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_RAW_MAX);
    TEST_ASSERT_EQUAL_UINT32(255U, CAN_DB_BMS_TEMPERATURES_TEMP1_SNA);
    TEST_ASSERT_EQUAL_UINT32(3U, CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_SNA);
    TEST_ASSERT_EQUAL_UINT32(63U, CAN_DB_BMS_NODE_INFO_RESET_CAUSE_SNA);
    TEST_ASSERT_EQUAL_UINT32(11U, CAN_DB_BMS_NODE_INFO_RESET_CAUSE_WATCHDOG);
    TEST_ASSERT_EQUAL_UINT32(0U, CAN_DB_BMS_NODE_INFO_RESET_CAUSE_POWER_ON);
    TEST_ASSERT_EQUAL_UINT32(31U, CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_NONE);
    TEST_ASSERT_EQUAL_UINT32(15U, CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_RESET_MALLOC_FAILED);
    TEST_ASSERT_EQUAL_UINT32(2U, CAN_DB_BMS_PACK_STATUS_PACK_STATE_FAULT);
    TEST_ASSERT_EQUAL_INT32(-32768, CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_RAW_MIN);
    TEST_ASSERT_EQUAL_INT32(32767, CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_RAW_MAX);
}

/* ------------------------------------------------------------------------------------------
 * Frames worked out by hand
 * ---------------------------------------------------------------------------------------- */

static void test_the_heartbeat_is_the_phase_1_status_frame(void)
{
    /* app.c of phase 1: put_u32_le(&data[0], periods); put_u32_le(&data[4], ticks). */
    const can_db_bms_heartbeat_t beat = {.period_counter = 0x04030201U, .tick_count = 0x08070605U};
    const uint8_t expected[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t data[8];
    can_db_bms_heartbeat_t back = {0};

    TEST_ASSERT_TRUE(can_db_bms_heartbeat_pack(&beat, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, data, 8);
    TEST_ASSERT_TRUE(can_db_bms_heartbeat_unpack(&back, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX32(0x04030201U, back.period_counter);
    TEST_ASSERT_EQUAL_HEX32(0x08070605U, back.tick_count);

    /* The first frame of a run, and the largest values. */
    const can_db_bms_heartbeat_t first = {.period_counter = 1U, .tick_count = 0xFFFFFFFFU};
    const uint8_t first_expected[8] = {0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};

    TEST_ASSERT_TRUE(can_db_bms_heartbeat_pack(&first, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(first_expected, data, 8);
}

static void test_pack_status_by_hand(void)
{
    /*
     * byte 0      E2E_Crc 0xAB
     * byte 1      E2E_Counter 3 in bits 0..3; bits 4..7 belong to no signal: 0
     * bytes 2..3  PackCurrent -12.34 A = raw -1234 = 0xFB2E, low byte first
     * bytes 4..5  PackVoltage 29.611 V = raw 29611 = 0x73AB
     * byte 6      PackState NORMAL (1) in bits 0..1, PackFaultActive 0 in bit 2,
     *             PackCurrentValid 1 in bit 3, PackVoltageValid 1 in bit 4: 0x19
     */
    const can_db_bms_pack_status_t status = {.e2e_crc = 0xABU,
                                             .e2e_counter = 3U,
                                             .pack_current = -1234,
                                             .pack_voltage = 29611U,
                                             .pack_state = CAN_DB_BMS_PACK_STATUS_PACK_STATE_NORMAL,
                                             .pack_fault_active = 0U,
                                             .pack_current_valid = 1U,
                                             .pack_voltage_valid = 1U};
    const uint8_t expected[8] = {0xAB, 0x03, 0x2E, 0xFB, 0xAB, 0x73, 0x19, 0x00};
    uint8_t data[8];
    can_db_bms_pack_status_t back;

    TEST_ASSERT_TRUE(can_db_bms_pack_status_pack(&status, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, data, 8);

    (void)memset(&back, FILL, sizeof(back));
    TEST_ASSERT_TRUE(can_db_bms_pack_status_unpack(&back, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8(0xABU, back.e2e_crc);
    TEST_ASSERT_EQUAL_UINT8(3U, back.e2e_counter);
    TEST_ASSERT_EQUAL_INT16(-1234, back.pack_current);
    TEST_ASSERT_EQUAL_UINT16(29611U, back.pack_voltage);
    TEST_ASSERT_EQUAL_UINT8(1U, back.pack_state);
    TEST_ASSERT_EQUAL_UINT8(0U, back.pack_fault_active);
    TEST_ASSERT_EQUAL_UINT8(1U, back.pack_current_valid);
    TEST_ASSERT_EQUAL_UINT8(1U, back.pack_voltage_valid);
}

static void test_cell_voltages_by_hand(void)
{
    /*
     * Four 12-bit signals from bit 16 on, so that every other one starts in the middle of a
     * byte:
     *   Cell1 3.700 V = raw 2700 = 0xA8C   bits 16..27: byte 2 = 0x8C, byte 3 bits 0..3 = 0xA
     *   Cell2 1.000 V = raw 0              bits 28..39: byte 3 bits 4..7 = 0, byte 4 = 0
     *   Cell3 5.094 V = raw 4094 = 0xFFE   bits 40..51: byte 5 = 0xFE, byte 6 bits 0..3 = 0xF
     *   Cell4 SNA     = raw 4095 = 0xFFF   bits 52..63: byte 6 bits 4..7 = 0xF, byte 7 = 0xFF
     */
    const can_db_bms_cell_voltages1_t cells = {.e2e_crc = 0U,
                                               .e2e_counter = 14U,
                                               .cell1_voltage = 2700U,
                                               .cell2_voltage = 0U,
                                               .cell3_voltage = 4094U,
                                               .cell4_voltage =
                                                   CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_SNA};
    const uint8_t expected[8] = {0x00, 0x0E, 0x8C, 0x0A, 0x00, 0xFE, 0xFF, 0xFF};
    uint8_t data[8];
    can_db_bms_cell_voltages1_t back = {0};

    TEST_ASSERT_TRUE(can_db_bms_cell_voltages1_pack(&cells, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, data, 8);
    TEST_ASSERT_TRUE(can_db_bms_cell_voltages1_unpack(&back, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT16(2700U, back.cell1_voltage);
    TEST_ASSERT_EQUAL_UINT16(0U, back.cell2_voltage);
    TEST_ASSERT_EQUAL_UINT16(4094U, back.cell3_voltage);
    TEST_ASSERT_EQUAL_UINT16(4095U, back.cell4_voltage);
    TEST_ASSERT_EQUAL_UINT8(14U, back.e2e_counter);

    /* The second message has the same layout for cells 5 to 8. */
    const can_db_bms_cell_voltages2_t more = {.e2e_crc = 0x5AU,
                                              .e2e_counter = 1U,
                                              .cell5_voltage = 0x123U,
                                              .cell6_voltage = 0x456U,
                                              .cell7_voltage = 0x789U,
                                              .cell8_voltage = 0xABCU};
    const uint8_t more_expected[8] = {0x5A, 0x01, 0x23, 0x61, 0x45, 0x89, 0xC7, 0xAB};

    TEST_ASSERT_TRUE(can_db_bms_cell_voltages2_pack(&more, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(more_expected, data, 8);
}

static void test_temperatures_command_task_stats_and_node_info_by_hand(void)
{
    uint8_t data[8];

    /* Temp1 25 degC = raw 65, Temp2 -40 degC = raw 0, Temp3 214 degC = raw 254, Temp4 SNA. */
    const can_db_bms_temperatures_t temps = {.e2e_crc = 0U,
                                             .e2e_counter = 0U,
                                             .temp1 = 65U,
                                             .temp2 = 0U,
                                             .temp3 = 254U,
                                             .temp4 = CAN_DB_BMS_TEMPERATURES_TEMP4_SNA};
    const uint8_t temps_expected[8] = {0x00, 0x00, 0x41, 0x00, 0xFE, 0xFF, 0x00, 0x00};

    TEST_ASSERT_TRUE(can_db_bms_temperatures_pack(&temps, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(temps_expected, data, 8);

    /* CmdTelemetryOn in bit 16, CmdClearFaultsSeq in byte 3. */
    const can_db_tester_command_t command = {
        .e2e_crc = 0x11U, .e2e_counter = 9U, .cmd_telemetry_on = 1U, .cmd_clear_faults_seq = 0x5AU};
    const uint8_t command_expected[8] = {0x11, 0x09, 0x01, 0x5A, 0x00, 0x00, 0x00, 0x00};
    can_db_tester_command_t command_back = {0};

    TEST_ASSERT_TRUE(can_db_tester_command_pack(&command, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(command_expected, data, 8);
    TEST_ASSERT_TRUE(can_db_tester_command_unpack(&command_back, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT8(1U, command_back.cmd_telemetry_on);
    TEST_ASSERT_EQUAL_HEX8(0x5AU, command_back.cmd_clear_faults_seq);

    /* Index 5 and priority 3 share byte 0 (index in the low nibble); CPU share 12.34 % = 1234
     * = 0x04D2; stack peak 0x0ABC bytes; 7 overruns. */
    const can_db_bms_task_stats_t stats = {.task_index = 5U,
                                           .task_priority = 3U,
                                           .task_cpu_share = 1234U,
                                           .task_stack_peak = 0x0ABCU,
                                           .task_overruns = 7U};
    const uint8_t stats_expected[8] = {0x35, 0xD2, 0x04, 0xBC, 0x0A, 0x07, 0x00, 0x00};

    TEST_ASSERT_TRUE(can_db_bms_task_stats_pack(&stats, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(stats_expected, data, 8);

    /* Version 1.2.3; ActiveSlot SNA (3) in bits 24..25 and ResetCause WATCHDOG (11) in bits
     * 26..31 share byte 3: 3 | (11 << 2) = 0x2F; uptime 0x12345678 s. */
    const can_db_bms_node_info_t info = {.sw_major = 1U,
                                         .sw_minor = 2U,
                                         .sw_patch = 3U,
                                         .active_slot = CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_SNA,
                                         .reset_cause = CAN_DB_BMS_NODE_INFO_RESET_CAUSE_WATCHDOG,
                                         .uptime_seconds = 0x12345678U};
    const uint8_t info_expected[8] = {0x01, 0x02, 0x03, 0x2F, 0x78, 0x56, 0x34, 0x12};
    can_db_bms_node_info_t info_back = {0};

    TEST_ASSERT_TRUE(can_db_bms_node_info_pack(&info, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(info_expected, data, 8);
    TEST_ASSERT_TRUE(can_db_bms_node_info_unpack(&info_back, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT8(3U, info_back.active_slot);
    TEST_ASSERT_EQUAL_UINT8(11U, info_back.reset_cause);
    TEST_ASSERT_EQUAL_HEX32(0x12345678U, info_back.uptime_seconds);
}

/*
 * BMS_Fault:
 *   byte 0      E2E_Crc 0x3C
 *   byte 1      E2E_Counter 14 in bits 0..3; bits 4..7 belong to no signal: 0x0E
 *   bits 16..32 FaultActive 0x11234 (17 bits), low byte first: bytes 2..3 34 12, its bit 16 in
 *               bit 0 of byte 4
 *   bits 33..49 FaultConfirmed 0x1FEDC (0b1_1111_1110_1101_1100): its bits 0..6 (0b1011100) in
 *               bits 1..7 of byte 4, which is 0xB9 with FaultActive's bit; bits 7..14 (0xFD) in
 *               byte 5; bits 15..16 (0b11) in bits 0..1 of byte 6
 *   bits 50..54 FaultMostRecent 21 (0b10101) in bits 2..6 of byte 6, FaultWarning 1 in bit 7:
 *               byte 6 is 0b1101_0111, 0xD7
 *   byte 7      FaultClearCount 0xA5
 */
static void test_fault_by_hand(void)
{
    const can_db_bms_fault_t fault = {.e2e_crc = 0x3CU,
                                      .e2e_counter = 14U,
                                      .fault_active = 0x11234U,
                                      .fault_confirmed = 0x1FEDCU,
                                      .fault_most_recent = 21U,
                                      .fault_warning = 1U,
                                      .fault_clear_count = 0xA5U};
    const uint8_t expected[8] = {0x3C, 0x0E, 0x34, 0x12, 0xB9, 0xFD, 0xD7, 0xA5};
    uint8_t data[8];
    can_db_bms_fault_t back;

    TEST_ASSERT_TRUE(can_db_bms_fault_pack(&fault, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, data, 8);

    (void)memset(&back, FILL, sizeof(back));
    TEST_ASSERT_TRUE(can_db_bms_fault_unpack(&back, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8(0x3CU, back.e2e_crc);
    TEST_ASSERT_EQUAL_UINT8(14U, back.e2e_counter);
    TEST_ASSERT_EQUAL_HEX32(0x11234U, back.fault_active);
    TEST_ASSERT_EQUAL_HEX32(0x1FEDCU, back.fault_confirmed);
    TEST_ASSERT_EQUAL_UINT8(21U, back.fault_most_recent);
    TEST_ASSERT_EQUAL_UINT8(1U, back.fault_warning);
    TEST_ASSERT_EQUAL_HEX8(0xA5U, back.fault_clear_count);

    /* NONE, no warning, nothing active: byte 6 holds 31 alone, in bits 2..6. */
    const can_db_bms_fault_t none = {.e2e_crc = 0U,
                                     .e2e_counter = 0U,
                                     .fault_active = 0U,
                                     .fault_confirmed = 0U,
                                     .fault_most_recent = CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_NONE,
                                     .fault_warning = 0U,
                                     .fault_clear_count = 0U};
    const uint8_t none_expected[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x00};

    TEST_ASSERT_TRUE(can_db_bms_fault_pack(&none, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(none_expected, data, 8);
}

/* ------------------------------------------------------------------------------------------
 * Sign extension and masking, by hand
 * ---------------------------------------------------------------------------------------- */

static int16_t pack_current_of(uint8_t low, uint8_t high)
{
    const uint8_t data[8] = {0x00, 0x00, low, high, 0x00, 0x00, 0x00, 0x00};
    can_db_bms_pack_status_t status = {0};

    TEST_ASSERT_TRUE(can_db_bms_pack_status_unpack(&status, data, sizeof(data)));
    return status.pack_current;
}

static void test_a_signed_signal_is_sign_extended(void)
{
    TEST_ASSERT_EQUAL_INT16(-1, pack_current_of(0xFF, 0xFF));
    TEST_ASSERT_EQUAL_INT16(-32768, pack_current_of(0x00, 0x80));
    TEST_ASSERT_EQUAL_INT16(32767, pack_current_of(0xFF, 0x7F));
    TEST_ASSERT_EQUAL_INT16(-1234, pack_current_of(0x2E, 0xFB));
    TEST_ASSERT_EQUAL_INT16(1, pack_current_of(0x01, 0x00));
    TEST_ASSERT_EQUAL_INT16(0, pack_current_of(0x00, 0x00));

    /* And packed in two's complement, without touching the neighbours. */
    const can_db_bms_pack_status_t status = {.pack_current = -32768};
    const uint8_t expected[8] = {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00};
    uint8_t data[8];

    TEST_ASSERT_TRUE(can_db_bms_pack_status_pack(&status, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, data, 8);
}

static void test_a_value_wider_than_its_signal_is_masked(void)
{
    /* 0xF123 in a 12-bit signal: 0x123 goes out, Cell2 next to it stays 0. A 2-bit PackState of
     * 0xFF sets its own two bits only, an E2E_Counter of 0xFF the low nibble of byte 1 only. */
    const can_db_bms_cell_voltages1_t cells = {.cell1_voltage = 0xF123U};
    const uint8_t cells_expected[8] = {0x00, 0x00, 0x23, 0x01, 0x00, 0x00, 0x00, 0x00};
    const can_db_bms_pack_status_t status = {.e2e_counter = 0xFFU, .pack_state = 0xFFU};
    const uint8_t status_expected[8] = {0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00};
    uint8_t data[8];

    TEST_ASSERT_TRUE(can_db_bms_cell_voltages1_pack(&cells, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cells_expected, data, 8);
    TEST_ASSERT_TRUE(can_db_bms_pack_status_pack(&status, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(status_expected, data, 8);

    /* Masked, not saturated: 4096 in 12 bits is 0. */
    const can_db_bms_cell_voltages1_t over = {.cell2_voltage = 4096U};
    const uint8_t zero[8] = {0};

    TEST_ASSERT_TRUE(can_db_bms_cell_voltages1_pack(&over, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, data, 8);
}

/* ------------------------------------------------------------------------------------------
 * What pack() and unpack() refuse
 * ---------------------------------------------------------------------------------------- */

static void test_pack_refuses_null_pointers_and_a_short_buffer_and_writes_nothing(void)
{
    const can_db_bms_pack_status_t status = {.e2e_crc = 1U, .pack_voltage = 2U};
    uint8_t data[9];
    uint8_t untouched[9];

    (void)memset(untouched, FILL, sizeof(untouched));

    (void)memset(data, FILL, sizeof(data));
    TEST_ASSERT_FALSE(can_db_bms_pack_status_pack(&status, data, 7U));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(untouched, data, sizeof(data));
    TEST_ASSERT_FALSE(can_db_bms_pack_status_pack(NULL, data, 8U));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(untouched, data, sizeof(data));
    TEST_ASSERT_FALSE(can_db_bms_pack_status_pack(&status, NULL, 8U));

    /* A larger buffer is fine; only the DLC bytes are written. */
    TEST_ASSERT_TRUE(can_db_bms_pack_status_pack(&status, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8(0x01U, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02U, data[4]);
    TEST_ASSERT_EQUAL_HEX8(FILL, data[8]);
}

static void test_unpack_refuses_a_wrong_length_and_null_pointers_and_changes_nothing(void)
{
    const uint8_t data[9] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    can_db_bms_node_info_t info;
    can_db_bms_node_info_t untouched;

    (void)memset(&untouched, FILL, sizeof(untouched));
    (void)memset(&info, FILL, sizeof(info));

    TEST_ASSERT_FALSE(can_db_bms_node_info_unpack(&info, data, 7U));
    TEST_ASSERT_FALSE(can_db_bms_node_info_unpack(&info, data, 9U));
    TEST_ASSERT_FALSE(can_db_bms_node_info_unpack(&info, data, 0U));
    TEST_ASSERT_FALSE(can_db_bms_node_info_unpack(&info, NULL, 8U));
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &info, sizeof(info));
    TEST_ASSERT_FALSE(can_db_bms_node_info_unpack(NULL, data, 8U));

    TEST_ASSERT_TRUE(can_db_bms_node_info_unpack(&info, data, 8U));
    TEST_ASSERT_EQUAL_HEX8(0x11U, info.sw_major);
}

/* ------------------------------------------------------------------------------------------
 * Every signal of every message, through the table
 * ---------------------------------------------------------------------------------------- */

static void pack_one(const can_db_table_message_t *message, size_t index, int64_t value,
                     uint8_t *data)
{
    int64_t raw[CAN_DB_TABLE_MAX_SIGNALS] = {0};

    raw[index] = value;
    (void)memset(data, FILL, 8U);
    TEST_ASSERT_TRUE(message->pack(raw, data, 8U));
}

static void test_every_signal_alone_at_its_maximum_changes_only_its_own_bits(void)
{
    size_t checked = 0U;

    for (size_t m = 0U; m < can_db_table_count; m++) {
        const can_db_table_message_t *message = &can_db_table[m];

        for (size_t s = 0U; s < message->signal_count; s++) {
            const can_db_table_signal_t *signal = &message->signals[s];
            uint8_t data[8];

            /* Every bit of the signal: all ones (-1 for a signed one). */
            pack_one(message, s, all_ones(signal), data);
            expect_only_bits(data, message->dlc, signal->start, signal->length, signal->name);

            /* The largest raw value: for a signed signal the sign bit stays 0. */
            pack_one(message, s, signal->raw_max, data);
            expect_frame(data, message->dlc, signal->start, signal->length,
                         low_bits(signal->raw_max, signal->length), signal->name);

            /* The largest and the smallest value of the struct member, beyond the signal's
             * bits wherever the member is wider: masked to the signal's own bits. */
            pack_one(message, s, signal->field_max, data);
            expect_frame(data, message->dlc, signal->start, signal->length,
                         low_bits(signal->field_max, signal->length), signal->name);
            pack_one(message, s, signal->field_min, data);
            expect_frame(data, message->dlc, signal->start, signal->length,
                         low_bits(signal->field_min, signal->length), signal->name);
            checked++;
        }
    }

    TEST_ASSERT_EQUAL_size_t(59U, checked); /* the signals of the DBC */
}

static void test_every_signal_cleared_alone_clears_only_its_own_bits(void)
{
    for (size_t m = 0U; m < can_db_table_count; m++) {
        const can_db_table_message_t *message = &can_db_table[m];
        int64_t raw[CAN_DB_TABLE_MAX_SIGNALS] = {0};
        uint8_t full[8];

        if (message->signal_count == 0U) {
            continue;
        }

        for (size_t s = 0U; s < message->signal_count; s++) {
            raw[s] = all_ones(&message->signals[s]);
        }
        TEST_ASSERT_TRUE(message->pack(raw, full, sizeof(full)));

        for (size_t s = 0U; s < message->signal_count; s++) {
            const can_db_table_signal_t *signal = &message->signals[s];
            const int64_t keep = raw[s];
            uint8_t data[8];

            raw[s] = 0;
            TEST_ASSERT_TRUE(message->pack(raw, data, sizeof(data)));
            raw[s] = keep;

            /* The difference between the two frames is exactly the signal's bits. */
            for (size_t b = 0U; b < 8U; b++) {
                data[b] = (uint8_t)(data[b] ^ full[b]);
            }
            expect_only_bits(data, message->dlc, signal->start, signal->length, signal->name);
        }
    }
}

static void test_bits_that_belong_to_no_signal_are_sent_as_0_and_ignored(void)
{
    for (size_t m = 0U; m < can_db_table_count; m++) {
        const can_db_table_message_t *message = &can_db_table[m];
        int64_t raw[CAN_DB_TABLE_MAX_SIGNALS] = {0};
        bool used[64] = {false};
        uint8_t data[8];
        const uint8_t ones[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

        if (message->signal_count == 0U) {
            continue;
        }

        for (size_t s = 0U; s < message->signal_count; s++) {
            const can_db_table_signal_t *signal = &message->signals[s];

            raw[s] = signal->is_signed ? -1 : signal->field_max;
            for (uint32_t bit = signal->start; bit < (signal->start + signal->length); bit++) {
                used[bit] = true;
            }
        }
        TEST_ASSERT_TRUE(message->pack(raw, data, sizeof(data)));
        for (uint32_t bit = 0U; bit < (8U * message->dlc); bit++) {
            TEST_ASSERT_EQUAL_MESSAGE(used[bit], frame_bit(data, bit), message->name);
        }

        /* Unpacking a frame of all ones gives every signal all ones: the unused bits are not
         * read into any of them. */
        TEST_ASSERT_TRUE(message->unpack(ones, message->dlc, raw));
        for (size_t s = 0U; s < message->signal_count; s++) {
            TEST_ASSERT_EQUAL_INT64_MESSAGE(all_ones(&message->signals[s]), raw[s],
                                            message->signals[s].name);
        }
    }
}

static void test_every_signed_signal_is_sign_extended_from_its_own_top_bit(void)
{
    size_t signed_signals = 0U;

    for (size_t m = 0U; m < can_db_table_count; m++) {
        const can_db_table_message_t *message = &can_db_table[m];

        for (size_t s = 0U; s < message->signal_count; s++) {
            const can_db_table_signal_t *signal = &message->signals[s];
            int64_t raw[CAN_DB_TABLE_MAX_SIGNALS] = {0};
            uint8_t data[8] = {0};
            const uint32_t top = signal->start + signal->length - 1U;

            if (!signal->is_signed) {
                continue;
            }
            signed_signals++;

            /* Only the top bit: the most negative value. */
            data[top / 8U] = (uint8_t)(1U << (top % 8U));
            TEST_ASSERT_TRUE(message->unpack(data, message->dlc, raw));
            TEST_ASSERT_EQUAL_INT64(signal->raw_min, raw[s]);

            /* Every bit of the frame but the top bit: the largest value, whatever the
             * neighbours hold. */
            (void)memset(data, 0xFF, sizeof(data));
            data[top / 8U] = (uint8_t)((uint32_t)data[top / 8U] & ~(1U << (top % 8U)));
            TEST_ASSERT_TRUE(message->unpack(data, message->dlc, raw));
            TEST_ASSERT_EQUAL_INT64(signal->raw_max, raw[s]);
        }
    }

    TEST_ASSERT_EQUAL_size_t(1U, signed_signals); /* PackCurrent */
}

static void test_random_raw_values_come_back_unchanged(void)
{
    enum { ROUNDS = 2000 };

    for (size_t m = 0U; m < can_db_table_count; m++) {
        const can_db_table_message_t *message = &can_db_table[m];

        for (int round = 0; (round < ROUNDS) && (message->signal_count > 0U); round++) {
            int64_t raw[CAN_DB_TABLE_MAX_SIGNALS] = {0};
            int64_t back[CAN_DB_TABLE_MAX_SIGNALS] = {0};
            uint8_t data[8];

            for (size_t s = 0U; s < message->signal_count; s++) {
                raw[s] = random_raw(&message->signals[s]);
            }
            TEST_ASSERT_TRUE(message->pack(raw, data, sizeof(data)));
            TEST_ASSERT_TRUE(message->unpack(data, message->dlc, back));
            TEST_ASSERT_EQUAL_INT64_ARRAY(raw, back, message->signal_count);
        }
    }
}

static void test_random_frames_come_back_unchanged_in_the_bits_of_the_signals(void)
{
    enum { ROUNDS = 2000 };

    for (size_t m = 0U; m < can_db_table_count; m++) {
        const can_db_table_message_t *message = &can_db_table[m];

        for (int round = 0; (round < ROUNDS) && (message->signal_count > 0U); round++) {
            int64_t raw[CAN_DB_TABLE_MAX_SIGNALS] = {0};
            uint8_t data[8];
            uint8_t again[8];

            for (size_t b = 0U; b < 8U; b++) {
                data[b] = (uint8_t)random_u32();
            }
            TEST_ASSERT_TRUE(message->unpack(data, message->dlc, raw));
            TEST_ASSERT_TRUE(message->pack(raw, again, sizeof(again)));

            for (uint32_t bit = 0U; bit < (8U * message->dlc); bit++) {
                bool used = false;

                for (size_t s = 0U; s < message->signal_count; s++) {
                    const can_db_table_signal_t *signal = &message->signals[s];

                    used = used || ((bit >= signal->start) &&
                                    (bit < (uint32_t)(signal->start + signal->length)));
                }
                TEST_ASSERT_EQUAL_MESSAGE(used && frame_bit(data, bit), frame_bit(again, bit),
                                          message->name);
            }
        }
    }
}

static void test_the_table_reaches_every_message_by_its_identifier(void)
{
    TEST_ASSERT_EQUAL_size_t(7U, table_message(0x010U)->signal_count);
    TEST_ASSERT_EQUAL_size_t(8U, table_message(0x020U)->signal_count);
    TEST_ASSERT_EQUAL_size_t(2U, table_message(0x030U)->signal_count);
    TEST_ASSERT_EQUAL_size_t(6U, table_message(0x040U)->signal_count);
    TEST_ASSERT_EQUAL_size_t(6U, table_message(0x041U)->signal_count);
    TEST_ASSERT_EQUAL_size_t(6U, table_message(0x050U)->signal_count);
    TEST_ASSERT_EQUAL_size_t(4U, table_message(0x100U)->signal_count);
    TEST_ASSERT_EQUAL_size_t(5U, table_message(0x600U)->signal_count);
    TEST_ASSERT_EQUAL_size_t(6U, table_message(0x610U)->signal_count);
    TEST_ASSERT_EQUAL_size_t(0U, table_message(0x6F0U)->signal_count);
    TEST_ASSERT_NULL(table_message(0x7E8U)->pack);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_message_set_is_the_one_of_the_design);
    RUN_TEST(test_the_scaling_is_given_as_exact_fractions);
    RUN_TEST(test_the_heartbeat_is_the_phase_1_status_frame);
    RUN_TEST(test_pack_status_by_hand);
    RUN_TEST(test_cell_voltages_by_hand);
    RUN_TEST(test_temperatures_command_task_stats_and_node_info_by_hand);
    RUN_TEST(test_fault_by_hand);
    RUN_TEST(test_a_signed_signal_is_sign_extended);
    RUN_TEST(test_a_value_wider_than_its_signal_is_masked);
    RUN_TEST(test_pack_refuses_null_pointers_and_a_short_buffer_and_writes_nothing);
    RUN_TEST(test_unpack_refuses_a_wrong_length_and_null_pointers_and_changes_nothing);
    RUN_TEST(test_every_signal_alone_at_its_maximum_changes_only_its_own_bits);
    RUN_TEST(test_every_signal_cleared_alone_clears_only_its_own_bits);
    RUN_TEST(test_bits_that_belong_to_no_signal_are_sent_as_0_and_ignored);
    RUN_TEST(test_every_signed_signal_is_sign_extended_from_its_own_top_bit);
    RUN_TEST(test_random_raw_values_come_back_unchanged);
    RUN_TEST(test_random_frames_come_back_unchanged_in_the_bits_of_the_signals);
    RUN_TEST(test_the_table_reaches_every_message_by_its_identifier);
    return UNITY_END();
}
