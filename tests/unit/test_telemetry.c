/* SPDX-License-Identifier: MIT */
/*
 * The frames the node sends (app/telemetry.c), and the uptime BMS_NodeInfo carries.
 *
 * Each frame is checked twice: byte by byte against what is worked out by hand in the comment
 * next to it, and through the receiving end, the generated unpack() and an independent
 * Profile 11 receiver (e2e_p11_check). Identifiers, lengths and Data IDs are written as the
 * literals of the message set rather than as the DBC's macros, so that a changed DBC shows
 * up here.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "can_db.h"
#include "e2e/e2e_p11.h"
#include "telemetry.h"
#include "unity.h"

static telemetry_sender_t s_sender;
static bms_pack_t s_pack;

/* A pack in a known state: NORMAL, 45.123 A, every cell and temperature valid. */
static void healthy_pack(void)
{
    static const uint16_t cells[8] = {3702U, 3698U, 3705U, 3695U, 3700U, 3703U, 3697U, 3701U};
    static const int16_t temps[4] = {251, 248, 256, 245};

    bms_init(&s_pack);
    s_pack.state = BMS_STATE_NORMAL;
    s_pack.current_ma = 45123;
    s_pack.current_valid = true;
    s_pack.pack_mv = 29601U;
    s_pack.pack_valid = true;
    (void)memcpy(s_pack.cell_mv, cells, sizeof(cells));
    (void)memcpy(s_pack.temp_dc, temps, sizeof(temps));
    s_pack.cell_valid = 0xFFU;
    s_pack.temp_valid = 0x0FU;
}

void setUp(void)
{
    (void)memset(&s_sender, 0xA5, sizeof(s_sender));
    telemetry_init(&s_sender);
    healthy_pack();
}

void tearDown(void)
{
}

static void expect_header(const can_frame_t *frame, uint32_t id)
{
    TEST_ASSERT_EQUAL_HEX32(id, frame->id);
    TEST_ASSERT_EQUAL_UINT8(0U, frame->flags);
    TEST_ASSERT_EQUAL_UINT8(8U, frame->dlc);
}

/* The verdict of a fresh receiver of `data_id` on `frames` frames in a row. */
static void expect_e2e(const can_frame_t *frames, uint32_t count, uint16_t data_id,
                       e2e_p11_status_t first, e2e_p11_status_t rest)
{
    const e2e_p11_config_t config = {.data_id = data_id, .data_len = 8U, .max_delta_counter = 1U};
    e2e_p11_check_state_t receiver;

    TEST_ASSERT_EQUAL(E2E_E_OK, e2e_p11_check_init(&receiver));

    for (uint32_t i = 0U; i < count; i++) {
        TEST_ASSERT_EQUAL(E2E_E_OK, e2e_p11_check(&config, &receiver, frames[i].data, 8U));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE((i == 0U) ? first : rest, receiver.status, "E2E status");
    }
}

/* -------------------------------------------------------------------------------- heartbeat */

static void test_the_heartbeat_is_the_status_frame_of_phase_1_bit_for_bit(void)
{
    static const uint8_t first[8] = {0x01U, 0x00U, 0x00U, 0x00U, 0x0AU, 0x00U, 0x00U, 0x00U};
    static const uint8_t other[8] = {0x78U, 0x56U, 0x34U, 0x12U, 0xF0U, 0xDEU, 0xBCU, 0x9AU};
    can_frame_t frame;

    /* Period 1 at tick 10, as the first status frame of phase 1. */
    TEST_ASSERT_TRUE(telemetry_heartbeat(1U, 10U, &frame));
    expect_header(&frame, 0x030U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(first, frame.data, 8);

    /* Both numbers little-endian, the counter first. */
    TEST_ASSERT_TRUE(telemetry_heartbeat(0x12345678U, 0x9ABCDEF0U, &frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(other, frame.data, 8);
}

/* ------------------------------------------------------------------------------ PackStatus */

/*
 *   PackCurrent 45123 mA -> 4512 = 0x11A0, bytes 2..3: A0 11
 *   PackVoltage 29601 mV -> 0x73A1,        bytes 4..5: A1 73
 *   byte 6: PackState NORMAL (1) in bits 0..1, PackFaultActive 0 in bit 2, PackCurrentValid 1
 *   in bit 3, PackVoltageValid 1 in bit 4: 0b00011001 = 0x19; byte 7 unused: 0
 *   byte 1: counter 0 in the low nibble, the high nibble unused: 0
 */
static void test_the_pack_status_carries_the_pack_state_and_passes_the_e2e_check(void)
{
    static const uint8_t payload[6] = {0xA0U, 0x11U, 0xA1U, 0x73U, 0x19U, 0x00U};
    can_frame_t frame;
    can_db_bms_pack_status_t msg;

    TEST_ASSERT_TRUE(telemetry_pack_status(&s_sender, &s_pack, false, &frame));
    expect_header(&frame, 0x020U);
    TEST_ASSERT_EQUAL_HEX8(0x00U, frame.data[1]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, &frame.data[2], 6);

    TEST_ASSERT_TRUE(can_db_bms_pack_status_unpack(&msg, frame.data, 8U));
    TEST_ASSERT_EQUAL_INT16(4512, msg.pack_current);
    TEST_ASSERT_EQUAL_UINT16(29601U, msg.pack_voltage);
    TEST_ASSERT_EQUAL_UINT8(1U, msg.pack_state);
    TEST_ASSERT_EQUAL_UINT8(0U, msg.pack_fault_active);
    TEST_ASSERT_EQUAL_UINT8(1U, msg.pack_current_valid);
    TEST_ASSERT_EQUAL_UINT8(1U, msg.pack_voltage_valid);
    TEST_ASSERT_EQUAL_UINT8(0U, msg.e2e_counter);

    expect_e2e(&frame, 1U, 0x0120U, E2E_P11STATUS_OK, E2E_P11STATUS_OK);
}

static void test_an_invalid_current_or_pack_voltage_goes_out_as_0_with_its_flag_0(void)
{
    can_frame_t frame;
    can_db_bms_pack_status_t msg;

    s_pack.state = BMS_STATE_FAULT;
    s_pack.current_valid = false;
    s_pack.pack_valid = false; /* pack_mv is then the sum of the valid cells only */

    TEST_ASSERT_TRUE(telemetry_pack_status(&s_sender, &s_pack, false, &frame));
    TEST_ASSERT_TRUE(can_db_bms_pack_status_unpack(&msg, frame.data, 8U));
    TEST_ASSERT_EQUAL_INT16(0, msg.pack_current);
    TEST_ASSERT_EQUAL_UINT16(0U, msg.pack_voltage);
    TEST_ASSERT_EQUAL_UINT8(2U, msg.pack_state);
    TEST_ASSERT_EQUAL_UINT8(0U, msg.pack_current_valid);
    TEST_ASSERT_EQUAL_UINT8(0U, msg.pack_voltage_valid);
}

static void test_a_negative_current_is_sent_as_a_twos_complement_number(void)
{
    can_frame_t frame;
    can_db_bms_pack_status_t msg;

    s_pack.current_ma = -80005; /* -8000.5 -> -8001 = 0xE0BF */

    TEST_ASSERT_TRUE(telemetry_pack_status(&s_sender, &s_pack, false, &frame));
    TEST_ASSERT_EQUAL_HEX8(0xBFU, frame.data[2]);
    TEST_ASSERT_EQUAL_HEX8(0xE0U, frame.data[3]);
    TEST_ASSERT_TRUE(can_db_bms_pack_status_unpack(&msg, frame.data, 8U));
    TEST_ASSERT_EQUAL_INT16(-8001, msg.pack_current);
}

/* ------------------------------------------------------------------- cells and temperatures */

static void test_the_cells_go_out_in_order_four_per_frame_and_an_invalid_one_as_sna(void)
{
    can_frame_t first;
    can_frame_t second;
    can_db_bms_cell_voltages1_t one;
    can_db_bms_cell_voltages2_t two;

    s_pack.cell_valid = (uint8_t) ~((1U << 2U) | (1U << 7U)); /* cells 3 and 8 invalid */
    s_pack.cell_mv[0] = 999U;                                 /* below the signal: raw 0 */

    TEST_ASSERT_TRUE(telemetry_cell_voltages1(&s_sender, &s_pack, &first));
    TEST_ASSERT_TRUE(telemetry_cell_voltages2(&s_sender, &s_pack, &second));
    expect_header(&first, 0x040U);
    expect_header(&second, 0x041U);

    TEST_ASSERT_TRUE(can_db_bms_cell_voltages1_unpack(&one, first.data, 8U));
    TEST_ASSERT_EQUAL_UINT16(0U, one.cell1_voltage);
    TEST_ASSERT_EQUAL_UINT16(2698U, one.cell2_voltage);
    TEST_ASSERT_EQUAL_UINT16(4095U, one.cell3_voltage);
    TEST_ASSERT_EQUAL_UINT16(2695U, one.cell4_voltage);

    TEST_ASSERT_TRUE(can_db_bms_cell_voltages2_unpack(&two, second.data, 8U));
    TEST_ASSERT_EQUAL_UINT16(2700U, two.cell5_voltage);
    TEST_ASSERT_EQUAL_UINT16(2703U, two.cell6_voltage);
    TEST_ASSERT_EQUAL_UINT16(2697U, two.cell7_voltage);
    TEST_ASSERT_EQUAL_UINT16(4095U, two.cell8_voltage);

    expect_e2e(&first, 1U, 0x0140U, E2E_P11STATUS_OK, E2E_P11STATUS_OK);
    expect_e2e(&second, 1U, 0x0141U, E2E_P11STATUS_OK, E2E_P11STATUS_OK);
}

/*
 *   cells 1..4 of the healthy pack: 2702 = 0xA8E, 2698 = 0xA8A, 2705 = 0xA91, 2695 = 0xA87, at
 *   bits 16, 28, 40, 52:  byte 2 8E, byte 3 low nibble A | (A8A & 0xF) << 4 = A << 4 -> AA,
 *   byte 4 A8, byte 5 91, byte 6 low nibble A | (A87 & 0xF) << 4 = 7 << 4 -> 7A, byte 7 A8
 */
static void test_the_cells_are_packed_in_12_bit_fields_from_bit_16(void)
{
    static const uint8_t payload[6] = {0x8EU, 0xAAU, 0xA8U, 0x91U, 0x7AU, 0xA8U};
    can_frame_t frame;

    TEST_ASSERT_TRUE(telemetry_cell_voltages1(&s_sender, &s_pack, &frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, &frame.data[2], 6);
}

/*
 *   25.1 -> 25 -> 65 = 0x41, 24.8 -> 25 -> 0x41, 25.6 -> 26 -> 0x42, 24.5 -> 25 -> 0x41;
 *   sensor 2 invalid: 0xFF; bytes 6..7 unused: 0
 */
static void test_the_temperatures_go_out_in_whole_degrees_and_an_invalid_one_as_sna(void)
{
    static const uint8_t payload[6] = {0x41U, 0xFFU, 0x42U, 0x41U, 0x00U, 0x00U};
    can_frame_t frame;

    s_pack.temp_valid = 0x0DU; /* sensor 2 invalid */

    TEST_ASSERT_TRUE(telemetry_temperatures(&s_sender, &s_pack, &frame));
    expect_header(&frame, 0x050U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, &frame.data[2], 6);
    expect_e2e(&frame, 1U, 0x0150U, E2E_P11STATUS_OK, E2E_P11STATUS_OK);
}

/* ------------------------------------------------------------------ end-to-end protection */

static void test_each_protected_message_counts_on_its_own_from_0_to_14_and_round(void)
{
    can_frame_t status[20];
    can_frame_t cells[20];
    can_frame_t temps[20];

    /* Interleaved as the node sends them: the counters must not share a state. */
    for (uint32_t i = 0U; i < 20U; i++) {
        TEST_ASSERT_TRUE(telemetry_pack_status(&s_sender, &s_pack, false, &status[i]));
        TEST_ASSERT_TRUE(telemetry_cell_voltages1(&s_sender, &s_pack, &cells[i]));
        TEST_ASSERT_TRUE(telemetry_temperatures(&s_sender, &s_pack, &temps[i]));
        TEST_ASSERT_EQUAL_UINT8(i % 15U, status[i].data[1] & 0x0FU);
        TEST_ASSERT_EQUAL_UINT8(i % 15U, cells[i].data[1] & 0x0FU);
        TEST_ASSERT_EQUAL_UINT8(i % 15U, temps[i].data[1] & 0x0FU);
    }

    /* A receiver that starts with the first frame sees nothing but new data, across the wrap
     * from 14 to 0. */
    expect_e2e(status, 20U, 0x0120U, E2E_P11STATUS_OK, E2E_P11STATUS_OK);
    expect_e2e(cells, 20U, 0x0140U, E2E_P11STATUS_OK, E2E_P11STATUS_OK);
    expect_e2e(temps, 20U, 0x0150U, E2E_P11STATUS_OK, E2E_P11STATUS_OK);
}

static void test_a_protected_frame_fails_the_check_of_another_message(void)
{
    can_frame_t frame;
    const e2e_p11_config_t cells1 = {.data_id = 0x0140U, .data_len = 8U, .max_delta_counter = 1U};
    e2e_p11_check_state_t receiver;

    TEST_ASSERT_TRUE(telemetry_pack_status(&s_sender, &s_pack, false, &frame));
    TEST_ASSERT_EQUAL(E2E_E_OK, e2e_p11_check_init(&receiver));
    TEST_ASSERT_EQUAL(E2E_E_OK, e2e_p11_check(&cells1, &receiver, frame.data, 8U));
    TEST_ASSERT_EQUAL_HEX8(E2E_P11STATUS_ERROR, receiver.status);
}

static void test_init_starts_every_counter_at_0(void)
{
    can_frame_t frame;

    for (uint32_t i = 0U; i < 7U; i++) {
        TEST_ASSERT_TRUE(telemetry_cell_voltages2(&s_sender, &s_pack, &frame));
    }

    telemetry_init(&s_sender);
    TEST_ASSERT_TRUE(telemetry_cell_voltages2(&s_sender, &s_pack, &frame));
    TEST_ASSERT_EQUAL_UINT8(0U, frame.data[1] & 0x0FU);
}

/* ----------------------------------------------------------------- task stats, node info */

/*
 *   index 3 in bits 0..3, priority 5 in bits 4..7: byte 0 = 0x53
 *   CPU share 1549000 of 2000000 us = 77.45 % -> 7745 = 0x1E41, bytes 1..2: 41 1E
 *   stack peak 6968 = 0x1B38, bytes 3..4: 38 1B; overruns 2, byte 5; bytes 6..7 unused: 0
 */
static void test_the_task_stats_carry_one_task_in_the_units_of_the_dbc(void)
{
    static const uint8_t expected[8] = {0x53U, 0x41U, 0x1EU, 0x38U, 0x1BU, 0x02U, 0x00U, 0x00U};
    const telemetry_task_t task = {.index = 3U,
                                   .priority = 5U,
                                   .run_time_us = 1549000U,
                                   .total_us = 2000000U,
                                   .stack_peak_bytes = 6968U,
                                   .overruns = 2U};
    can_frame_t frame;

    TEST_ASSERT_TRUE(telemetry_task_stats(&task, &frame));
    expect_header(&frame, 0x600U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame.data, 8);
}

static void test_the_task_stats_saturate_what_their_signals_cannot_carry(void)
{
    const telemetry_task_t task = {.index = 20U,
                                   .priority = 16U,
                                   .run_time_us = 5U,
                                   .total_us = 4U,
                                   .stack_peak_bytes = 70000U,
                                   .overruns = 300U};
    can_frame_t frame;
    can_db_bms_task_stats_t msg;

    TEST_ASSERT_TRUE(telemetry_task_stats(&task, &frame));
    TEST_ASSERT_TRUE(can_db_bms_task_stats_unpack(&msg, frame.data, 8U));
    TEST_ASSERT_EQUAL_UINT8(15U, msg.task_index);
    TEST_ASSERT_EQUAL_UINT8(15U, msg.task_priority);
    TEST_ASSERT_EQUAL_UINT16(10000U, msg.task_cpu_share);
    TEST_ASSERT_EQUAL_UINT16(65535U, msg.task_stack_peak);
    TEST_ASSERT_EQUAL_UINT8(255U, msg.task_overruns);
}

/*
 *   version 0.1.0 in bytes 0..2; byte 3: ActiveSlot SNA (3) in bits 0..1, ResetCause in bits
 *   2..7: POWER_ON (0) gives 0x03, WATCHDOG (11 = 0b001011) 0x2F, a value that is no cause SNA
 *   (63) 0xFF; uptime 3600 s = 0x0E10, bytes 4..7: 10 0E 00 00
 */
static void test_the_node_info_carries_the_version_the_reset_cause_and_the_uptime(void)
{
    static const uint8_t power_on[8] = {0x00U, 0x01U, 0x00U, 0x03U, 0x10U, 0x0EU, 0x00U, 0x00U};
    can_frame_t frame;
    can_db_bms_node_info_t msg;

    TEST_ASSERT_TRUE(
        telemetry_node_info(0U, 1U, 0U, PLAT_HANDOFF_NO_SLOT, PLAT_RESET_POWER_ON, 3600U, &frame));
    expect_header(&frame, 0x610U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(power_on, frame.data, 8);

    TEST_ASSERT_TRUE(
        telemetry_node_info(0U, 1U, 0U, PLAT_HANDOFF_NO_SLOT, PLAT_RESET_WATCHDOG, 3600U, &frame));
    TEST_ASSERT_EQUAL_HEX8(0x2FU, frame.data[3]);

    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): no cause, on purpose */
    TEST_ASSERT_TRUE(telemetry_node_info(0U, 1U, 0U, PLAT_HANDOFF_NO_SLOT, (plat_reset_reason_t)99,
                                         3600U, &frame));
    TEST_ASSERT_EQUAL_HEX8(0xFFU, frame.data[3]);

    /* Every cause the platform knows goes out as its own value. */
    static const plat_reset_reason_t causes[] = {
        PLAT_RESET_POWER_ON,       PLAT_RESET_REQUESTED,    PLAT_RESET_WATCHDOG,
        PLAT_RESET_POWER_LOSS,     PLAT_RESET_IMAGE_FAULT,  PLAT_RESET_ASSERT,
        PLAT_RESET_STACK_OVERFLOW, PLAT_RESET_MALLOC_FAILED};

    for (size_t i = 0U; i < (sizeof(causes) / sizeof(causes[0])); i++) {
        TEST_ASSERT_TRUE(
            telemetry_node_info(1U, 2U, 3U, PLAT_HANDOFF_NO_SLOT, causes[i], 0U, &frame));
        TEST_ASSERT_TRUE(can_db_bms_node_info_unpack(&msg, frame.data, 8U));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)causes[i], msg.reset_cause);
        TEST_ASSERT_EQUAL_UINT8(3U, msg.active_slot);
    }
}

/* ------------------------------------------------------------------------------- BMS_Fault */

/*
 *   faults 0, 8 and 16 (UPDATE_ROLLBACK) active: 0x10101 in bits 16..32: bytes 2..3 01 01 and
 *   bit 0 of byte 4; confirmed also 15: 0x18101 in bits 33..49: bit 0 (1) in bit 1 of byte 4,
 *   which is 0x03 with FaultActive's bit 16, bits 7..14 (0x02) in byte 5, bits 15..16 (0b11) in
 *   bits 0..1 of byte 6; FaultMostRecent 16 (0b10000) in bits 2..6 of byte 6 and FaultWarning 1
 *   in bit 7: 0xC3; byte 7: FaultClearCount 3; byte 1: counter 0, the high nibble unused
 */
static void test_the_fault_frame_carries_the_summary_and_passes_the_e2e_check(void)
{
    static const uint8_t payload[6] = {0x01U, 0x01U, 0x03U, 0x02U, 0xC3U, 0x03U};
    const fault_summary_t summary = {.active = 0x10101U,
                                     .confirmed = 0x18101U,
                                     .most_recent = (uint8_t)FAULT_UPDATE_ROLLBACK,
                                     .clears = 3U,
                                     .warning = true};
    e2e_p11_protect_state_t counter;
    can_frame_t frames[16];
    can_db_bms_fault_t msg;

    TEST_ASSERT_EQUAL(E2E_E_OK, e2e_p11_protect_init(&counter));
    TEST_ASSERT_TRUE(telemetry_fault(&counter, &summary, &frames[0]));
    expect_header(&frames[0], 0x010U);
    TEST_ASSERT_EQUAL_HEX8(0x00U, frames[0].data[1]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, &frames[0].data[2], 6);

    TEST_ASSERT_TRUE(can_db_bms_fault_unpack(&msg, frames[0].data, 8U));
    TEST_ASSERT_EQUAL_HEX32(0x10101U, msg.fault_active);
    TEST_ASSERT_EQUAL_HEX32(0x18101U, msg.fault_confirmed);
    TEST_ASSERT_EQUAL_UINT8(16U, msg.fault_most_recent);
    TEST_ASSERT_EQUAL_UINT8(1U, msg.fault_warning);
    TEST_ASSERT_EQUAL_UINT8(3U, msg.fault_clear_count);

    /* The counter is the caller's: 0, 1, ..., 14, 0 across the wrap, every frame new data for
     * a receiver of Data ID 0x0110. */
    for (uint32_t i = 1U; i < 16U; i++) {
        TEST_ASSERT_TRUE(telemetry_fault(&counter, &summary, &frames[i]));
        TEST_ASSERT_EQUAL_UINT8(i % 15U, frames[i].data[1] & 0x0FU);
    }
    expect_e2e(frames, 16U, 0x0110U, E2E_P11STATUS_OK, E2E_P11STATUS_OK);
}

/* ActiveSlot is the hand-off's slot, 0 for A and 1 for B; any other value, the "no slot" of a
 * start without the bootloader among them, is SNA (3). Byte 3: ActiveSlot in bits 0..1. */
static void test_the_node_info_names_the_slot_of_the_hand_off(void)
{
    can_frame_t frame;
    can_db_bms_node_info_t msg;
    static const struct {
        uint8_t slot;
        uint8_t raw;
    } cases[] = {{0U, 0U}, {1U, 1U}, {PLAT_HANDOFF_NO_SLOT, 3U}, {2U, 3U}};

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        TEST_ASSERT_TRUE(
            telemetry_node_info(1U, 2U, 3U, cases[i].slot, PLAT_RESET_POWER_ON, 7U, &frame));
        TEST_ASSERT_TRUE(can_db_bms_node_info_unpack(&msg, frame.data, 8U));
        TEST_ASSERT_EQUAL_UINT8(cases[i].raw, msg.active_slot);
        TEST_ASSERT_EQUAL_UINT8(cases[i].raw, frame.data[3] & 0x03U);
        TEST_ASSERT_EQUAL_UINT8(1U, msg.sw_major);
        TEST_ASSERT_EQUAL_UINT32(7U, msg.uptime_seconds);
    }
}

/*
 * BMS_BootInfo of a trial in slot B, attempt 2, image 1.2.3, after a REJECT of slot A, waiting
 * with 4 healthy seconds:
 *   byte 0: BootMode TRIAL (2) in bits 0..1, BootAttempt 2 in bits 2..3, BootEvent REJECT (6) in
 *           bits 4..6: 0b0110_1010, 0x6A
 *   byte 1: ConfirmState WAITING (1) in bits 0..2, BootEventSlot A (0) in bits 3..4: 0x01
 *   byte 2: ImageVersionMajor 1; byte 3: ImageVersionMinor 2; bytes 4..5: ImageVersionPatch 3
 *   byte 6: ConfirmSeconds 4; byte 7: unused, 0
 */
static void test_the_boot_info_carries_the_hand_off_and_the_confirm(void)
{
    static const uint8_t expected[8] = {0x6AU, 0x01U, 0x01U, 0x02U, 0x03U, 0x00U, 0x04U, 0x00U};
    const plat_boot_handoff_t handoff = {.slot = 1U,
                                         .mode = PLAT_BOOT_TRIAL,
                                         .attempt = 2U,
                                         .image_version = 0x01020003U,
                                         .event = PLAT_BOOT_EVENT_REJECT,
                                         .event_slot = 0U,
                                         .select_write_error = 0U};
    can_frame_t frame;
    can_db_bms_boot_info_t msg;

    TEST_ASSERT_TRUE(telemetry_boot_info(&handoff, UPDATE_CONFIRM_WAITING, 4U, &frame));
    expect_header(&frame, 0x611U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame.data, 8);

    /* The patch takes all 16 bits, the seconds saturate at 255. */
    plat_boot_handoff_t wide = handoff;

    wide.image_version = 0xFFFEABCDU;
    wide.mode = PLAT_BOOT_CONFIRMED;
    wide.attempt = 0U;
    wide.event = PLAT_BOOT_EVENT_NONE;
    wide.event_slot = PLAT_HANDOFF_NO_SLOT;
    TEST_ASSERT_TRUE(telemetry_boot_info(&wide, UPDATE_CONFIRM_CONFIRMED, 1000U, &frame));
    TEST_ASSERT_TRUE(can_db_bms_boot_info_unpack(&msg, frame.data, 8U));
    TEST_ASSERT_EQUAL_UINT8(1U, msg.boot_mode);
    TEST_ASSERT_EQUAL_UINT8(0U, msg.boot_attempt);
    TEST_ASSERT_EQUAL_UINT8(0U, msg.boot_event);
    TEST_ASSERT_EQUAL_UINT8(3U, msg.boot_event_slot);
    TEST_ASSERT_EQUAL_UINT8(3U, msg.confirm_state);
    TEST_ASSERT_EQUAL_UINT8(0xFFU, msg.image_version_major);
    TEST_ASSERT_EQUAL_UINT8(0xFEU, msg.image_version_minor);
    TEST_ASSERT_EQUAL_HEX16(0xABCDU, msg.image_version_patch);
    TEST_ASSERT_EQUAL_UINT8(255U, msg.confirm_seconds);
}

/* Without a hand-off: BootMode NONE, every other signal 0 but ConfirmState and BootEventSlot
 * SNA. A hand-off or a state the DBC has no value for builds nothing. */
static void test_the_boot_info_without_a_hand_off_and_what_it_refuses(void)
{
    static const uint8_t none[8] = {0x00U, 0x18U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    const plat_boot_handoff_t good = {.slot = 0U,
                                      .mode = PLAT_BOOT_CONFIRMED,
                                      .attempt = 0U,
                                      .image_version = 1U,
                                      .event = PLAT_BOOT_EVENT_ROLLBACK,
                                      .event_slot = 1U,
                                      .select_write_error = 0U};
    plat_boot_handoff_t bad;
    can_frame_t frame;

    TEST_ASSERT_TRUE(telemetry_boot_info(NULL, UPDATE_CONFIRM_NOT_NEEDED, 0U, &frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(none, frame.data, 8);

    bad = good;
    bad.slot = 2U;
    TEST_ASSERT_FALSE(telemetry_boot_info(&bad, UPDATE_CONFIRM_NOT_NEEDED, 0U, &frame));
    bad = good;
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): no event, on purpose */
    bad.event = (plat_boot_event_t)3;
    TEST_ASSERT_FALSE(telemetry_boot_info(&bad, UPDATE_CONFIRM_NOT_NEEDED, 0U, &frame));
    bad = good;
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): no mode, on purpose */
    bad.mode = (plat_boot_mode_t)0;
    TEST_ASSERT_FALSE(telemetry_boot_info(&bad, UPDATE_CONFIRM_NOT_NEEDED, 0U, &frame));
    bad = good;
    bad.attempt = 4U;
    TEST_ASSERT_FALSE(telemetry_boot_info(&bad, UPDATE_CONFIRM_NOT_NEEDED, 0U, &frame));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): no state, on purpose */
    TEST_ASSERT_FALSE(telemetry_boot_info(&good, (update_confirm_state_t)5, 0U, &frame));
    TEST_ASSERT_TRUE(telemetry_boot_info(&good, UPDATE_CONFIRM_FAILED, 0U, &frame));
    TEST_ASSERT_EQUAL_HEX8(0x51U, frame.data[0]); /* CONFIRMED 1, attempt 0, ROLLBACK 5 */
    TEST_ASSERT_EQUAL_HEX8(0x0CU, frame.data[1]); /* FAILED 4, event slot B */
}

static void test_a_fault_frame_without_a_recent_fault_says_none_and_a_bad_one_is_refused(void)
{
    fault_summary_t summary = {.active = 0U,
                               .confirmed = 0U,
                               .most_recent = (uint8_t)FAULT_NONE,
                               .clears = 0U,
                               .warning = false};
    e2e_p11_protect_state_t counter;
    can_frame_t frame;

    TEST_ASSERT_EQUAL(E2E_E_OK, e2e_p11_protect_init(&counter));
    TEST_ASSERT_TRUE(telemetry_fault(&counter, &summary, &frame));
    TEST_ASSERT_EQUAL_HEX8(0x7CU, frame.data[6]); /* NONE = 31 in bits 2..6, no warning */
    TEST_ASSERT_EQUAL_HEX8(0x00U, frame.data[7]);

    /* A number that is neither a fault nor NONE builds nothing and leaves the counter alone. */
    summary.most_recent = (uint8_t)FAULT_EVENTS;
    TEST_ASSERT_FALSE(telemetry_fault(&counter, &summary, &frame));
    summary.most_recent = (uint8_t)FAULT_NONE;
    TEST_ASSERT_TRUE(telemetry_fault(&counter, &summary, &frame));
    TEST_ASSERT_EQUAL_UINT8(1U, frame.data[1] & 0x0FU);
}

/*
 *   PackFaultActive 1 in bit 2 of byte 6, and PackState FAULT (2) in bits 0..1 whatever the
 *   pack's own state: with both valid flags, 0b00011110 = 0x1E
 */
static void test_an_active_fault_makes_the_pack_state_fault_and_sets_its_flag(void)
{
    static const bms_state_t states[] = {BMS_STATE_STARTUP, BMS_STATE_NORMAL, BMS_STATE_FAULT};
    can_frame_t frame;
    can_db_bms_pack_status_t msg;

    for (size_t i = 0U; i < (sizeof(states) / sizeof(states[0])); i++) {
        s_pack.state = states[i];
        TEST_ASSERT_TRUE(telemetry_pack_status(&s_sender, &s_pack, true, &frame));
        TEST_ASSERT_EQUAL_HEX8(0x1EU, frame.data[6]);
        TEST_ASSERT_TRUE(can_db_bms_pack_status_unpack(&msg, frame.data, 8U));
        TEST_ASSERT_EQUAL_UINT8(2U, msg.pack_state);
        TEST_ASSERT_EQUAL_UINT8(1U, msg.pack_fault_active);

        /* Without an active fault the pack's own state, and the flag 0. */
        TEST_ASSERT_TRUE(telemetry_pack_status(&s_sender, &s_pack, false, &frame));
        TEST_ASSERT_TRUE(can_db_bms_pack_status_unpack(&msg, frame.data, 8U));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)states[i], msg.pack_state);
        TEST_ASSERT_EQUAL_UINT8(0U, msg.pack_fault_active);
    }
}

/* ----------------------------------------------------------------------------------- uptime */

/* Periods of 100 ms in 2^32 ms, rounded up: after 2^32 ms a 32-bit count of 1 ms ticks, the
 * widest the Cortex-M3 port allows, wraps to 0. */
#define PERIODS_IN_2_POW_32_MS ((((uint64_t)UINT32_MAX + 1U) + 99U) / 100U)

/*
 * The uptime is the number of whole seconds in the periods counted, ten periods of 100 ms to
 * the second: counted period by period from the start to 50 periods past the point where a
 * 32-bit millisecond tick wraps, it is right after every one of them, and exactly every tenth
 * period completes a second. An uptime derived from such a tick count would fall back to 0
 * there.
 */
static void test_the_uptime_counts_whole_seconds_past_the_wrap_of_a_32_bit_millisecond_tick(void)
{
    telemetry_uptime_t uptime = {.seconds = 0U, .periods = 0U};
    const uint64_t last = PERIODS_IN_2_POW_32_MS + 50U;
    uint64_t first_wrong = 0U; /* the first period after which the count was wrong; 0: none */

    for (uint64_t period = 1U; (period <= last) && (first_wrong == 0U); period++) {
        const bool second = telemetry_uptime_period(&uptime, 10U);

        if ((second != ((period % 10U) == 0U)) || ((uint64_t)uptime.seconds != (period / 10U))) {
            first_wrong = period;
        }
    }

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0U, first_wrong, "the first period counted wrong");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(last / 10U), uptime.seconds);
    TEST_ASSERT_GREATER_THAN_UINT32((uint32_t)((((uint64_t)UINT32_MAX + 1U) / 1000U)),
                                    uptime.seconds);
}

/* The count wraps with its 32-bit signal, after 2^32 s; a second of one period, or of 0, is
 * every period. */
static void test_the_uptime_wraps_after_2_pow_32_seconds_as_its_signal(void)
{
    telemetry_uptime_t uptime = {.seconds = UINT32_MAX, .periods = 9U};

    TEST_ASSERT_TRUE(telemetry_uptime_period(&uptime, 10U));
    TEST_ASSERT_EQUAL_UINT32(0U, uptime.seconds);
    TEST_ASSERT_EQUAL_UINT32(0U, uptime.periods);

    TEST_ASSERT_TRUE(telemetry_uptime_period(&uptime, 1U));
    TEST_ASSERT_TRUE(telemetry_uptime_period(&uptime, 0U));
    TEST_ASSERT_EQUAL_UINT32(2U, uptime.seconds);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_heartbeat_is_the_status_frame_of_phase_1_bit_for_bit);
    RUN_TEST(test_the_pack_status_carries_the_pack_state_and_passes_the_e2e_check);
    RUN_TEST(test_an_invalid_current_or_pack_voltage_goes_out_as_0_with_its_flag_0);
    RUN_TEST(test_a_negative_current_is_sent_as_a_twos_complement_number);
    RUN_TEST(test_the_cells_go_out_in_order_four_per_frame_and_an_invalid_one_as_sna);
    RUN_TEST(test_the_cells_are_packed_in_12_bit_fields_from_bit_16);
    RUN_TEST(test_the_temperatures_go_out_in_whole_degrees_and_an_invalid_one_as_sna);
    RUN_TEST(test_each_protected_message_counts_on_its_own_from_0_to_14_and_round);
    RUN_TEST(test_a_protected_frame_fails_the_check_of_another_message);
    RUN_TEST(test_init_starts_every_counter_at_0);
    RUN_TEST(test_the_task_stats_carry_one_task_in_the_units_of_the_dbc);
    RUN_TEST(test_the_task_stats_saturate_what_their_signals_cannot_carry);
    RUN_TEST(test_the_node_info_carries_the_version_the_reset_cause_and_the_uptime);
    RUN_TEST(test_the_node_info_names_the_slot_of_the_hand_off);
    RUN_TEST(test_the_boot_info_carries_the_hand_off_and_the_confirm);
    RUN_TEST(test_the_boot_info_without_a_hand_off_and_what_it_refuses);
    RUN_TEST(test_the_fault_frame_carries_the_summary_and_passes_the_e2e_check);
    RUN_TEST(test_a_fault_frame_without_a_recent_fault_says_none_and_a_bad_one_is_refused);
    RUN_TEST(test_an_active_fault_makes_the_pack_state_fault_and_sets_its_flag);
    RUN_TEST(test_the_uptime_counts_whole_seconds_past_the_wrap_of_a_32_bit_millisecond_tick);
    RUN_TEST(test_the_uptime_wraps_after_2_pow_32_seconds_as_its_signal);
    return UNITY_END();
}
