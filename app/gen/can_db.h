/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The node's CAN messages: identifiers, timing, end-to-end protection, and a codec for the signals
 * of each message. docs/can-protocol.md describes the messages.
 *
 * WHAT IS HERE. For every message of the DBC:
 *     CAN_DB_<MSG>_ID             identifier, base format (11 bits)
 *     CAN_DB_<MSG>_DLC            number of data bytes
 *     CAN_DB_<MSG>_CYCLE_MS       cycle time in ms; 0 for a message sent on events
 *     CAN_DB_<MSG>_E2E_PROTECTED  1U if protected end to end (AUTOSAR E2E Profile 11A), else 0U
 *     CAN_DB_<MSG>_E2E_DATA_ID    the Data ID; defined for protected messages only, so that
 *                                 using it for any other message does not compile
 * and for every message with signals a struct can_db_<msg>_t of the raw signal values, with
 * can_db_<msg>_pack() and can_db_<msg>_unpack(). For every signal:
 *     CAN_DB_<MSG>_<SIG>_START, _LENGTH   first bit and number of bits; bit 0 is the least
 *                                         significant bit of byte 0, and bit i of the raw value
 *                                         is frame bit START + i (little-endian)
 *     _RAW_MIN, _RAW_MAX                  range of the raw value
 *     _FACTOR_NUM, _FACTOR_DEN            physical = raw * FACTOR_NUM / FACTOR_DEN
 *     _OFFSET_NUM, _OFFSET_DEN                       + OFFSET_NUM / OFFSET_DEN, exactly;
 *                                         int32_t constants, the fractions in lowest terms
 *     _<NAME>                             raw value of an entry of the signal's value table;
 *                                         _SNA ("signal not available") where the table has it
 * Raw values of unsigned signals are unsigned constants (4095U), those of signed signals constants
 * of the struct member's type (((int16_t)(-32768))).
 *
 * RAW VALUES. The structs hold the bits on the wire as an integer, in the smallest fixed-width type
 * that holds them; a signed signal is a two's complement number. Converting to and from physical
 * units is left to the code that produces or uses a value, which knows how to round and saturate
 * and when to send SNA. That code can check its assumptions about the DBC at compile time, one
 * relation per assertion, for example
 *     _Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP1_OFFSET_NUM == -40, "offset -40 degC");
 * so that a change of the DBC it does not follow breaks the build instead of the data.
 *
 * PACK AND UNPACK. pack() writes all DLC data bytes: every signal from its struct member, every bit
 * that belongs to no signal as 0. A member's bits above the signal's length are masked off, not
 * saturated: a value cannot reach another signal's bits, but 4096 in a 12-bit signal goes out as 0.
 * unpack() reads every signal into its member; a signed signal is sign-extended. Both refuse NULL
 * pointers and a buffer of the wrong size, and then return false without touching the output. The
 * E2E header signals (E2E_Crc, E2E_Counter) are packed like any other; the end-to-end protection
 * overwrites them after pack().
 *
 * CONTEXT. No state, no allocation, no operating system: every function may be called from any task
 * or interrupt, also concurrently, as long as no two calls write the same buffer.
 *
 * GENERATED FILE, DO NOT EDIT. The interface of the codec. Made by tools/gen_can_db.py from
 * dbc/fcn.dbc, SHA-256 ee5a3fa516d3029851805544da6004843add051153e5492607e90ef8e1881e18.
 *
 * After a change of the DBC, run scripts/codegen-check.sh --update. Without --update the script
 * fails while this file is out of date; CI and scripts/check.sh run it.
 */
#ifndef FCN_CAN_DB_H
#define FCN_CAN_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------------------------------
 * BMS_Fault: 0x010, 8 bytes, every 100 ms; from BMS to TESTER. Protected end to end (E2E Profile
 * 11A), Data ID 0x0110; the CRC of the Data ID bytes is 0xA9.
 * The node's diagnostic trouble codes, every 100 ms, sent by the task that owns them after the last
 * change it made in the period, so that every change goes out in the frame of the period that made
 * it: a fault that became active and passed again within the period is active in that frame.
 * Protected end to end. Fault n (the value table of FaultMostRecent, app/fault.h) is bit n of
 * FaultActive and FaultConfirmed.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_BMS_FAULT_ID            0x010U
#define CAN_DB_BMS_FAULT_DLC           8U
#define CAN_DB_BMS_FAULT_CYCLE_MS      100U
#define CAN_DB_BMS_FAULT_E2E_PROTECTED 1U
#define CAN_DB_BMS_FAULT_E2E_DATA_ID   0x0110U

/*
 * E2E_Crc: bits 0..7 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. E2E Profile 11
 * CRC over the Data ID and bytes 1 to 7.
 */
#define CAN_DB_BMS_FAULT_E2E_CRC_START      0U
#define CAN_DB_BMS_FAULT_E2E_CRC_LENGTH     8U
#define CAN_DB_BMS_FAULT_E2E_CRC_RAW_MIN    0U
#define CAN_DB_BMS_FAULT_E2E_CRC_RAW_MAX    255U
#define CAN_DB_BMS_FAULT_E2E_CRC_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_FAULT_E2E_CRC_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_FAULT_E2E_CRC_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_FAULT_E2E_CRC_OFFSET_DEN ((int32_t)1)

/*
 * E2E_Counter: bits 8..11 (4 bits, unsigned); raw 0..15; physical = raw, range 0..14. E2E Profile
 * 11 counter: 0 to 14, then 0 again; 15 is never sent.
 */
#define CAN_DB_BMS_FAULT_E2E_COUNTER_START      8U
#define CAN_DB_BMS_FAULT_E2E_COUNTER_LENGTH     4U
#define CAN_DB_BMS_FAULT_E2E_COUNTER_RAW_MIN    0U
#define CAN_DB_BMS_FAULT_E2E_COUNTER_RAW_MAX    15U
#define CAN_DB_BMS_FAULT_E2E_COUNTER_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_FAULT_E2E_COUNTER_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_FAULT_E2E_COUNTER_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_FAULT_E2E_COUNTER_OFFSET_DEN ((int32_t)1)

/*
 * FaultActive: bits 16..32 (17 bits, unsigned); raw 0..131071; physical = raw, range 0..131071. Bit
 * n: the DTC of fault n has testFailed (ISO 14229-1 status bit 0) at the end of the period, or its
 * testFailed rose during the period (after the clear, if the period had one): the fault is present,
 * or was present since the last frame. PackFaultActive is 1 while any bit is.
 */
#define CAN_DB_BMS_FAULT_FAULT_ACTIVE_START      16U
#define CAN_DB_BMS_FAULT_FAULT_ACTIVE_LENGTH     17U
#define CAN_DB_BMS_FAULT_FAULT_ACTIVE_RAW_MIN    0U
#define CAN_DB_BMS_FAULT_FAULT_ACTIVE_RAW_MAX    131071U
#define CAN_DB_BMS_FAULT_FAULT_ACTIVE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_FAULT_FAULT_ACTIVE_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_FAULT_FAULT_ACTIVE_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_FAULT_FAULT_ACTIVE_OFFSET_DEN ((int32_t)1)

/*
 * FaultConfirmed: bits 33..49 (17 bits, unsigned); raw 0..131071; physical = raw, range 0..131071.
 * Bit n: the DTC of fault n has confirmedDTC (status bit 3): the fault occurred since the node
 * started or since the last clear (CmdClearFaultsSeq).
 */
#define CAN_DB_BMS_FAULT_FAULT_CONFIRMED_START      33U
#define CAN_DB_BMS_FAULT_FAULT_CONFIRMED_LENGTH     17U
#define CAN_DB_BMS_FAULT_FAULT_CONFIRMED_RAW_MIN    0U
#define CAN_DB_BMS_FAULT_FAULT_CONFIRMED_RAW_MAX    131071U
#define CAN_DB_BMS_FAULT_FAULT_CONFIRMED_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_FAULT_FAULT_CONFIRMED_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_FAULT_FAULT_CONFIRMED_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_FAULT_FAULT_CONFIRMED_OFFSET_DEN ((int32_t)1)

/*
 * FaultMostRecent: bits 50..54 (5 bits, unsigned); raw 0..31; physical = raw, range 0..16. Value
 * table: 0 CELL_OVERVOLTAGE, 1 CELL_UNDERVOLTAGE, 2 OVERTEMPERATURE, 3 UNDERTEMPERATURE, 4
 * OVERCURRENT_DISCHARGE, 5 OVERCURRENT_CHARGE, 6 SENSOR_CHANNEL_INVALID, 7 FRONT_END_NO_ANSWER, 8
 * SAMPLE_TIMEOUT, 9 CAN_BUS_OFF, 10 CAN_ERROR_PASSIVE, 11 SUPERVISION_EXPIRED, 12 RESET_WATCHDOG,
 * 13 RESET_ASSERT, 14 RESET_STACK_OVERFLOW, 15 RESET_MALLOC_FAILED, 16 UPDATE_ROLLBACK, 31 NONE.
 * The fault whose DTC went to testFailed most recently since the start or the last clear; NONE if
 * none did.
 */
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_START                  50U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_LENGTH                 5U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_RAW_MIN                0U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_RAW_MAX                31U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_FACTOR_NUM             ((int32_t)1)
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_FACTOR_DEN             ((int32_t)1)
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_OFFSET_NUM             ((int32_t)0)
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_OFFSET_DEN             ((int32_t)1)
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_CELL_OVERVOLTAGE       0U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_CELL_UNDERVOLTAGE      1U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_OVERTEMPERATURE        2U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_UNDERTEMPERATURE       3U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_OVERCURRENT_DISCHARGE  4U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_OVERCURRENT_CHARGE     5U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_SENSOR_CHANNEL_INVALID 6U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_FRONT_END_NO_ANSWER    7U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_SAMPLE_TIMEOUT         8U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_CAN_BUS_OFF            9U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_CAN_ERROR_PASSIVE      10U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_SUPERVISION_EXPIRED    11U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_RESET_WATCHDOG         12U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_RESET_ASSERT           13U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_RESET_STACK_OVERFLOW   14U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_RESET_MALLOC_FAILED    15U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_UPDATE_ROLLBACK        16U
#define CAN_DB_BMS_FAULT_FAULT_MOST_RECENT_NONE                   31U

/*
 * FaultWarning: bit 55 (1 bit, unsigned); raw 0..1; physical = raw, range 0..1. 1 while a DTC
 * requests the warning indicator (status bit 7): a limit of the pack was exceeded, or its
 * measurement failed, since the start or the last clear.
 */
#define CAN_DB_BMS_FAULT_FAULT_WARNING_START      55U
#define CAN_DB_BMS_FAULT_FAULT_WARNING_LENGTH     1U
#define CAN_DB_BMS_FAULT_FAULT_WARNING_RAW_MIN    0U
#define CAN_DB_BMS_FAULT_FAULT_WARNING_RAW_MAX    1U
#define CAN_DB_BMS_FAULT_FAULT_WARNING_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_FAULT_FAULT_WARNING_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_FAULT_FAULT_WARNING_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_FAULT_FAULT_WARNING_OFFSET_DEN ((int32_t)1)

/*
 * FaultClearCount: bits 56..63 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. Clears
 * of the DTCs carried out since the node started, modulo 256.
 */
#define CAN_DB_BMS_FAULT_FAULT_CLEAR_COUNT_START      56U
#define CAN_DB_BMS_FAULT_FAULT_CLEAR_COUNT_LENGTH     8U
#define CAN_DB_BMS_FAULT_FAULT_CLEAR_COUNT_RAW_MIN    0U
#define CAN_DB_BMS_FAULT_FAULT_CLEAR_COUNT_RAW_MAX    255U
#define CAN_DB_BMS_FAULT_FAULT_CLEAR_COUNT_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_FAULT_FAULT_CLEAR_COUNT_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_FAULT_FAULT_CLEAR_COUNT_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_FAULT_FAULT_CLEAR_COUNT_OFFSET_DEN ((int32_t)1)

/** The raw signal values of BMS_Fault, in the order of their start bits. */
typedef struct {
    uint8_t e2e_crc;           /**< E2E_Crc */
    uint8_t e2e_counter;       /**< E2E_Counter */
    uint32_t fault_active;     /**< FaultActive */
    uint32_t fault_confirmed;  /**< FaultConfirmed */
    uint8_t fault_most_recent; /**< FaultMostRecent */
    uint8_t fault_warning;     /**< FaultWarning */
    uint8_t fault_clear_count; /**< FaultClearCount */
} can_db_bms_fault_t;

/**
 * Write the CAN_DB_BMS_FAULT_DLC data bytes of BMS_Fault into data[0] to data[7]. Returns false,
 * writing nothing, if a pointer is NULL or `capacity`, the size of `data` in bytes, is less than
 * the DLC.
 */
bool can_db_bms_fault_pack(const can_db_bms_fault_t *msg, uint8_t *data, size_t capacity);

/**
 * Read the signals of BMS_Fault from `len` data bytes. Returns false, leaving *msg as it was, if a
 * pointer is NULL or `len` is not CAN_DB_BMS_FAULT_DLC.
 */
bool can_db_bms_fault_unpack(can_db_bms_fault_t *msg, const uint8_t *data, size_t len);

/* -------------------------------------------------------------------------------------------------
 * BMS_PackStatus: 0x020, 8 bytes, every 10 ms; from BMS to TESTER. Protected end to end (E2E
 * Profile 11A), Data ID 0x0120; the CRC of the Data ID bytes is 0x68.
 * Pack summary, every 10 ms. Protected end to end.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_BMS_PACK_STATUS_ID            0x020U
#define CAN_DB_BMS_PACK_STATUS_DLC           8U
#define CAN_DB_BMS_PACK_STATUS_CYCLE_MS      10U
#define CAN_DB_BMS_PACK_STATUS_E2E_PROTECTED 1U
#define CAN_DB_BMS_PACK_STATUS_E2E_DATA_ID   0x0120U

/*
 * E2E_Crc: bits 0..7 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. E2E Profile 11
 * CRC over the Data ID and bytes 1 to 7.
 */
#define CAN_DB_BMS_PACK_STATUS_E2E_CRC_START      0U
#define CAN_DB_BMS_PACK_STATUS_E2E_CRC_LENGTH     8U
#define CAN_DB_BMS_PACK_STATUS_E2E_CRC_RAW_MIN    0U
#define CAN_DB_BMS_PACK_STATUS_E2E_CRC_RAW_MAX    255U
#define CAN_DB_BMS_PACK_STATUS_E2E_CRC_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_E2E_CRC_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_E2E_CRC_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_PACK_STATUS_E2E_CRC_OFFSET_DEN ((int32_t)1)

/*
 * E2E_Counter: bits 8..11 (4 bits, unsigned); raw 0..15; physical = raw, range 0..14. E2E Profile
 * 11 counter: 0 to 14, then 0 again; 15 is never sent.
 */
#define CAN_DB_BMS_PACK_STATUS_E2E_COUNTER_START      8U
#define CAN_DB_BMS_PACK_STATUS_E2E_COUNTER_LENGTH     4U
#define CAN_DB_BMS_PACK_STATUS_E2E_COUNTER_RAW_MIN    0U
#define CAN_DB_BMS_PACK_STATUS_E2E_COUNTER_RAW_MAX    15U
#define CAN_DB_BMS_PACK_STATUS_E2E_COUNTER_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_E2E_COUNTER_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_E2E_COUNTER_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_PACK_STATUS_E2E_COUNTER_OFFSET_DEN ((int32_t)1)

/*
 * PackCurrent: bits 16..31 (16 bits, signed); raw -32768..32767; physical = raw * 0.01 A, range
 * -327.68..327.67 A. Filtered pack current, positive while the pack is discharging; meaningful only
 * while PackCurrentValid is 1.
 */
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_START      16U
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_LENGTH     16U
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_RAW_MIN    ((int16_t)(-32768))
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_RAW_MAX    ((int16_t)32767)
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_FACTOR_DEN ((int32_t)100)
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_OFFSET_DEN ((int32_t)1)

/*
 * PackVoltage: bits 32..47 (16 bits, unsigned); raw 0..65535; physical = raw * 0.001 V, range
 * 0..65.535 V. Sum of the filtered cell voltages; meaningful only while PackVoltageValid is 1.
 */
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_START      32U
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_LENGTH     16U
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_RAW_MIN    0U
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_RAW_MAX    65535U
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_FACTOR_DEN ((int32_t)1000)
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_OFFSET_DEN ((int32_t)1)

/*
 * PackState: bits 48..49 (2 bits, unsigned); raw 0..3; physical = raw, range 0..2. Value table: 0
 * STARTUP, 1 NORMAL, 2 FAULT. STARTUP until every channel has delivered a reading, FAULT while a
 * channel is invalid or a fault is active (PackFaultActive), NORMAL otherwise.
 */
#define CAN_DB_BMS_PACK_STATUS_PACK_STATE_START      48U
#define CAN_DB_BMS_PACK_STATUS_PACK_STATE_LENGTH     2U
#define CAN_DB_BMS_PACK_STATUS_PACK_STATE_RAW_MIN    0U
#define CAN_DB_BMS_PACK_STATUS_PACK_STATE_RAW_MAX    3U
#define CAN_DB_BMS_PACK_STATUS_PACK_STATE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_PACK_STATE_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_PACK_STATE_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_PACK_STATUS_PACK_STATE_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_PACK_STATE_STARTUP    0U
#define CAN_DB_BMS_PACK_STATUS_PACK_STATE_NORMAL     1U
#define CAN_DB_BMS_PACK_STATUS_PACK_STATE_FAULT      2U

/*
 * PackFaultActive: bit 50 (1 bit, unsigned); raw 0..1; physical = raw, range 0..1. 1 while a fault
 * is active: a bit of FaultActive is set in the last BMS_Fault, so a DTC had testFailed at the end
 * of the fault owner's last 100 ms period or its testFailed rose during it.
 */
#define CAN_DB_BMS_PACK_STATUS_PACK_FAULT_ACTIVE_START      50U
#define CAN_DB_BMS_PACK_STATUS_PACK_FAULT_ACTIVE_LENGTH     1U
#define CAN_DB_BMS_PACK_STATUS_PACK_FAULT_ACTIVE_RAW_MIN    0U
#define CAN_DB_BMS_PACK_STATUS_PACK_FAULT_ACTIVE_RAW_MAX    1U
#define CAN_DB_BMS_PACK_STATUS_PACK_FAULT_ACTIVE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_PACK_FAULT_ACTIVE_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_PACK_FAULT_ACTIVE_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_PACK_STATUS_PACK_FAULT_ACTIVE_OFFSET_DEN ((int32_t)1)

/*
 * PackCurrentValid: bit 51 (1 bit, unsigned); raw 0..1; physical = raw, range 0..1. 1 if
 * PackCurrent holds a valid, filtered measurement.
 */
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_VALID_START      51U
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_VALID_LENGTH     1U
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_VALID_RAW_MIN    0U
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_VALID_RAW_MAX    1U
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_VALID_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_VALID_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_VALID_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_VALID_OFFSET_DEN ((int32_t)1)

/*
 * PackVoltageValid: bit 52 (1 bit, unsigned); raw 0..1; physical = raw, range 0..1. 1 if every cell
 * voltage is valid, so that PackVoltage is their sum.
 */
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_VALID_START      52U
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_VALID_LENGTH     1U
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_VALID_RAW_MIN    0U
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_VALID_RAW_MAX    1U
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_VALID_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_VALID_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_VALID_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_VALID_OFFSET_DEN ((int32_t)1)

/** The raw signal values of BMS_PackStatus, in the order of their start bits. */
typedef struct {
    uint8_t e2e_crc;            /**< E2E_Crc */
    uint8_t e2e_counter;        /**< E2E_Counter */
    int16_t pack_current;       /**< PackCurrent */
    uint16_t pack_voltage;      /**< PackVoltage */
    uint8_t pack_state;         /**< PackState */
    uint8_t pack_fault_active;  /**< PackFaultActive */
    uint8_t pack_current_valid; /**< PackCurrentValid */
    uint8_t pack_voltage_valid; /**< PackVoltageValid */
} can_db_bms_pack_status_t;

/**
 * Write the CAN_DB_BMS_PACK_STATUS_DLC data bytes of BMS_PackStatus into data[0] to data[7].
 * Returns false, writing nothing, if a pointer is NULL or `capacity`, the size of `data` in bytes,
 * is less than the DLC.
 */
bool can_db_bms_pack_status_pack(const can_db_bms_pack_status_t *msg, uint8_t *data,
                                 size_t capacity);

/**
 * Read the signals of BMS_PackStatus from `len` data bytes. Returns false, leaving *msg as it was,
 * if a pointer is NULL or `len` is not CAN_DB_BMS_PACK_STATUS_DLC.
 */
bool can_db_bms_pack_status_unpack(can_db_bms_pack_status_t *msg, const uint8_t *data, size_t len);

/* -------------------------------------------------------------------------------------------------
 * BMS_Heartbeat: 0x030, 8 bytes, every 10 ms; from BMS to TESTER. Not protected end to end.
 * Heartbeat, every 10 ms: the status frame of phase 1, unchanged. The node keeps the latest value
 * in a slot of its transmit queue, so a value not yet sent is replaced by the next one, and a
 * missing counter value means it was replaced. Not protected end to end.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_BMS_HEARTBEAT_ID            0x030U
#define CAN_DB_BMS_HEARTBEAT_DLC           8U
#define CAN_DB_BMS_HEARTBEAT_CYCLE_MS      10U
#define CAN_DB_BMS_HEARTBEAT_E2E_PROTECTED 0U

/*
 * PeriodCounter: bits 0..31 (32 bits, unsigned); raw 0..4294967295; physical = raw, range
 * 0..4294967295. Number of 10 ms periods the node has run since it started; the first frame carries
 * 1. A period it skipped after an overrun is not counted, so the counter never jumps; TickCount
 * shows the time.
 */
#define CAN_DB_BMS_HEARTBEAT_PERIOD_COUNTER_START      0U
#define CAN_DB_BMS_HEARTBEAT_PERIOD_COUNTER_LENGTH     32U
#define CAN_DB_BMS_HEARTBEAT_PERIOD_COUNTER_RAW_MIN    0U
#define CAN_DB_BMS_HEARTBEAT_PERIOD_COUNTER_RAW_MAX    4294967295U
#define CAN_DB_BMS_HEARTBEAT_PERIOD_COUNTER_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_HEARTBEAT_PERIOD_COUNTER_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_HEARTBEAT_PERIOD_COUNTER_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_HEARTBEAT_PERIOD_COUNTER_OFFSET_DEN ((int32_t)1)

/*
 * TickCount: bits 32..63 (32 bits, unsigned); raw 0..4294967295; physical = raw tick, range
 * 0..4294967295 tick. The node's FreeRTOS tick count when it published the frame (1 tick = 1 ms).
 */
#define CAN_DB_BMS_HEARTBEAT_TICK_COUNT_START      32U
#define CAN_DB_BMS_HEARTBEAT_TICK_COUNT_LENGTH     32U
#define CAN_DB_BMS_HEARTBEAT_TICK_COUNT_RAW_MIN    0U
#define CAN_DB_BMS_HEARTBEAT_TICK_COUNT_RAW_MAX    4294967295U
#define CAN_DB_BMS_HEARTBEAT_TICK_COUNT_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_HEARTBEAT_TICK_COUNT_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_HEARTBEAT_TICK_COUNT_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_HEARTBEAT_TICK_COUNT_OFFSET_DEN ((int32_t)1)

/** The raw signal values of BMS_Heartbeat, in the order of their start bits. */
typedef struct {
    uint32_t period_counter; /**< PeriodCounter */
    uint32_t tick_count;     /**< TickCount */
} can_db_bms_heartbeat_t;

/**
 * Write the CAN_DB_BMS_HEARTBEAT_DLC data bytes of BMS_Heartbeat into data[0] to data[7]. Returns
 * false, writing nothing, if a pointer is NULL or `capacity`, the size of `data` in bytes, is less
 * than the DLC.
 */
bool can_db_bms_heartbeat_pack(const can_db_bms_heartbeat_t *msg, uint8_t *data, size_t capacity);

/**
 * Read the signals of BMS_Heartbeat from `len` data bytes. Returns false, leaving *msg as it was,
 * if a pointer is NULL or `len` is not CAN_DB_BMS_HEARTBEAT_DLC.
 */
bool can_db_bms_heartbeat_unpack(can_db_bms_heartbeat_t *msg, const uint8_t *data, size_t len);

/* -------------------------------------------------------------------------------------------------
 * BMS_CellVoltages1: 0x040, 8 bytes, every 20 ms; from BMS to TESTER. Protected end to end (E2E
 * Profile 11A), Data ID 0x0140; the CRC of the Data ID bytes is 0xF7.
 * Filtered voltages of cells 1 to 4, every 20 ms. Protected end to end.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_BMS_CELL_VOLTAGES1_ID            0x040U
#define CAN_DB_BMS_CELL_VOLTAGES1_DLC           8U
#define CAN_DB_BMS_CELL_VOLTAGES1_CYCLE_MS      20U
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_PROTECTED 1U
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_DATA_ID   0x0140U

/*
 * E2E_Crc: bits 0..7 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. E2E Profile 11
 * CRC over the Data ID and bytes 1 to 7.
 */
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_CRC_START      0U
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_CRC_LENGTH     8U
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_CRC_RAW_MIN    0U
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_CRC_RAW_MAX    255U
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_CRC_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_CRC_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_CRC_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_CRC_OFFSET_DEN ((int32_t)1)

/*
 * E2E_Counter: bits 8..11 (4 bits, unsigned); raw 0..15; physical = raw, range 0..14. E2E Profile
 * 11 counter: 0 to 14, then 0 again; 15 is never sent.
 */
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_COUNTER_START      8U
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_COUNTER_LENGTH     4U
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_COUNTER_RAW_MIN    0U
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_COUNTER_RAW_MAX    15U
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_COUNTER_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_COUNTER_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_COUNTER_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_CELL_VOLTAGES1_E2E_COUNTER_OFFSET_DEN ((int32_t)1)

/*
 * Cell1Voltage: bits 16..27 (12 bits, unsigned); raw 0..4095; physical = raw * 0.001 + 1 V, range
 * 1..5.094 V. Value table: 4095 SNA. Raw 4095 (SNA): no valid measurement. Voltages below 1.000 V
 * are sent as 1.000 V, above 5.094 V as 5.094 V.
 */
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_START      16U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_LENGTH     12U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_RAW_MIN    0U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_RAW_MAX    4095U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_FACTOR_DEN ((int32_t)1000)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_OFFSET_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_SNA        4095U

/*
 * Cell2Voltage: bits 28..39 (12 bits, unsigned); raw 0..4095; physical = raw * 0.001 + 1 V, range
 * 1..5.094 V. Value table: 4095 SNA. As Cell1Voltage.
 */
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_START      28U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_LENGTH     12U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_RAW_MIN    0U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_RAW_MAX    4095U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_FACTOR_DEN ((int32_t)1000)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_OFFSET_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_SNA        4095U

/*
 * Cell3Voltage: bits 40..51 (12 bits, unsigned); raw 0..4095; physical = raw * 0.001 + 1 V, range
 * 1..5.094 V. Value table: 4095 SNA. As Cell1Voltage.
 */
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_START      40U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_LENGTH     12U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_RAW_MIN    0U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_RAW_MAX    4095U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_FACTOR_DEN ((int32_t)1000)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_OFFSET_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_SNA        4095U

/*
 * Cell4Voltage: bits 52..63 (12 bits, unsigned); raw 0..4095; physical = raw * 0.001 + 1 V, range
 * 1..5.094 V. Value table: 4095 SNA. As Cell1Voltage.
 */
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_START      52U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_LENGTH     12U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_RAW_MIN    0U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_RAW_MAX    4095U
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_FACTOR_DEN ((int32_t)1000)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_OFFSET_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_SNA        4095U

/** The raw signal values of BMS_CellVoltages1, in the order of their start bits. */
typedef struct {
    uint8_t e2e_crc;        /**< E2E_Crc */
    uint8_t e2e_counter;    /**< E2E_Counter */
    uint16_t cell1_voltage; /**< Cell1Voltage */
    uint16_t cell2_voltage; /**< Cell2Voltage */
    uint16_t cell3_voltage; /**< Cell3Voltage */
    uint16_t cell4_voltage; /**< Cell4Voltage */
} can_db_bms_cell_voltages1_t;

/**
 * Write the CAN_DB_BMS_CELL_VOLTAGES1_DLC data bytes of BMS_CellVoltages1 into data[0] to data[7].
 * Returns false, writing nothing, if a pointer is NULL or `capacity`, the size of `data` in bytes,
 * is less than the DLC.
 */
bool can_db_bms_cell_voltages1_pack(const can_db_bms_cell_voltages1_t *msg, uint8_t *data,
                                    size_t capacity);

/**
 * Read the signals of BMS_CellVoltages1 from `len` data bytes. Returns false, leaving *msg as it
 * was, if a pointer is NULL or `len` is not CAN_DB_BMS_CELL_VOLTAGES1_DLC.
 */
bool can_db_bms_cell_voltages1_unpack(can_db_bms_cell_voltages1_t *msg, const uint8_t *data,
                                      size_t len);

/* -------------------------------------------------------------------------------------------------
 * BMS_CellVoltages2: 0x041, 8 bytes, every 20 ms; from BMS to TESTER. Protected end to end (E2E
 * Profile 11A), Data ID 0x0141; the CRC of the Data ID bytes is 0xBB.
 * Filtered voltages of cells 5 to 8, every 20 ms. Protected end to end.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_BMS_CELL_VOLTAGES2_ID            0x041U
#define CAN_DB_BMS_CELL_VOLTAGES2_DLC           8U
#define CAN_DB_BMS_CELL_VOLTAGES2_CYCLE_MS      20U
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_PROTECTED 1U
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_DATA_ID   0x0141U

/*
 * E2E_Crc: bits 0..7 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. E2E Profile 11
 * CRC over the Data ID and bytes 1 to 7.
 */
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_CRC_START      0U
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_CRC_LENGTH     8U
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_CRC_RAW_MIN    0U
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_CRC_RAW_MAX    255U
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_CRC_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_CRC_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_CRC_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_CRC_OFFSET_DEN ((int32_t)1)

/*
 * E2E_Counter: bits 8..11 (4 bits, unsigned); raw 0..15; physical = raw, range 0..14. E2E Profile
 * 11 counter: 0 to 14, then 0 again; 15 is never sent.
 */
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_COUNTER_START      8U
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_COUNTER_LENGTH     4U
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_COUNTER_RAW_MIN    0U
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_COUNTER_RAW_MAX    15U
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_COUNTER_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_COUNTER_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_COUNTER_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_CELL_VOLTAGES2_E2E_COUNTER_OFFSET_DEN ((int32_t)1)

/*
 * Cell5Voltage: bits 16..27 (12 bits, unsigned); raw 0..4095; physical = raw * 0.001 + 1 V, range
 * 1..5.094 V. Value table: 4095 SNA. As Cell1Voltage of BMS_CellVoltages1.
 */
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_START      16U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_LENGTH     12U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_RAW_MIN    0U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_RAW_MAX    4095U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_FACTOR_DEN ((int32_t)1000)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_OFFSET_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_SNA        4095U

/*
 * Cell6Voltage: bits 28..39 (12 bits, unsigned); raw 0..4095; physical = raw * 0.001 + 1 V, range
 * 1..5.094 V. Value table: 4095 SNA. As Cell1Voltage of BMS_CellVoltages1.
 */
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_START      28U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_LENGTH     12U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_RAW_MIN    0U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_RAW_MAX    4095U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_FACTOR_DEN ((int32_t)1000)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_OFFSET_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_SNA        4095U

/*
 * Cell7Voltage: bits 40..51 (12 bits, unsigned); raw 0..4095; physical = raw * 0.001 + 1 V, range
 * 1..5.094 V. Value table: 4095 SNA. As Cell1Voltage of BMS_CellVoltages1.
 */
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_START      40U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_LENGTH     12U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_RAW_MIN    0U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_RAW_MAX    4095U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_FACTOR_DEN ((int32_t)1000)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_OFFSET_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_SNA        4095U

/*
 * Cell8Voltage: bits 52..63 (12 bits, unsigned); raw 0..4095; physical = raw * 0.001 + 1 V, range
 * 1..5.094 V. Value table: 4095 SNA. As Cell1Voltage of BMS_CellVoltages1.
 */
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_START      52U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_LENGTH     12U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_RAW_MIN    0U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_RAW_MAX    4095U
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_FACTOR_DEN ((int32_t)1000)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_OFFSET_NUM ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_SNA        4095U

/** The raw signal values of BMS_CellVoltages2, in the order of their start bits. */
typedef struct {
    uint8_t e2e_crc;        /**< E2E_Crc */
    uint8_t e2e_counter;    /**< E2E_Counter */
    uint16_t cell5_voltage; /**< Cell5Voltage */
    uint16_t cell6_voltage; /**< Cell6Voltage */
    uint16_t cell7_voltage; /**< Cell7Voltage */
    uint16_t cell8_voltage; /**< Cell8Voltage */
} can_db_bms_cell_voltages2_t;

/**
 * Write the CAN_DB_BMS_CELL_VOLTAGES2_DLC data bytes of BMS_CellVoltages2 into data[0] to data[7].
 * Returns false, writing nothing, if a pointer is NULL or `capacity`, the size of `data` in bytes,
 * is less than the DLC.
 */
bool can_db_bms_cell_voltages2_pack(const can_db_bms_cell_voltages2_t *msg, uint8_t *data,
                                    size_t capacity);

/**
 * Read the signals of BMS_CellVoltages2 from `len` data bytes. Returns false, leaving *msg as it
 * was, if a pointer is NULL or `len` is not CAN_DB_BMS_CELL_VOLTAGES2_DLC.
 */
bool can_db_bms_cell_voltages2_unpack(can_db_bms_cell_voltages2_t *msg, const uint8_t *data,
                                      size_t len);

/* -------------------------------------------------------------------------------------------------
 * BMS_Temperatures: 0x050, 8 bytes, every 100 ms; from BMS to TESTER. Protected end to end (E2E
 * Profile 11A), Data ID 0x0150; the CRC of the Data ID bytes is 0x43.
 * Filtered temperatures, every 100 ms. Protected end to end.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_BMS_TEMPERATURES_ID            0x050U
#define CAN_DB_BMS_TEMPERATURES_DLC           8U
#define CAN_DB_BMS_TEMPERATURES_CYCLE_MS      100U
#define CAN_DB_BMS_TEMPERATURES_E2E_PROTECTED 1U
#define CAN_DB_BMS_TEMPERATURES_E2E_DATA_ID   0x0150U

/*
 * E2E_Crc: bits 0..7 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. E2E Profile 11
 * CRC over the Data ID and bytes 1 to 7.
 */
#define CAN_DB_BMS_TEMPERATURES_E2E_CRC_START      0U
#define CAN_DB_BMS_TEMPERATURES_E2E_CRC_LENGTH     8U
#define CAN_DB_BMS_TEMPERATURES_E2E_CRC_RAW_MIN    0U
#define CAN_DB_BMS_TEMPERATURES_E2E_CRC_RAW_MAX    255U
#define CAN_DB_BMS_TEMPERATURES_E2E_CRC_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_E2E_CRC_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_E2E_CRC_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_TEMPERATURES_E2E_CRC_OFFSET_DEN ((int32_t)1)

/*
 * E2E_Counter: bits 8..11 (4 bits, unsigned); raw 0..15; physical = raw, range 0..14. E2E Profile
 * 11 counter: 0 to 14, then 0 again; 15 is never sent.
 */
#define CAN_DB_BMS_TEMPERATURES_E2E_COUNTER_START      8U
#define CAN_DB_BMS_TEMPERATURES_E2E_COUNTER_LENGTH     4U
#define CAN_DB_BMS_TEMPERATURES_E2E_COUNTER_RAW_MIN    0U
#define CAN_DB_BMS_TEMPERATURES_E2E_COUNTER_RAW_MAX    15U
#define CAN_DB_BMS_TEMPERATURES_E2E_COUNTER_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_E2E_COUNTER_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_E2E_COUNTER_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_TEMPERATURES_E2E_COUNTER_OFFSET_DEN ((int32_t)1)

/*
 * Temp1: bits 16..23 (8 bits, unsigned); raw 0..255; physical = raw * 1 - 40 degC, range -40..214
 * degC. Value table: 255 SNA. Raw 255 (SNA): no valid measurement. Temperatures below -40 degC are
 * sent as -40 degC, above 214 degC as 214 degC.
 */
#define CAN_DB_BMS_TEMPERATURES_TEMP1_START      16U
#define CAN_DB_BMS_TEMPERATURES_TEMP1_LENGTH     8U
#define CAN_DB_BMS_TEMPERATURES_TEMP1_RAW_MIN    0U
#define CAN_DB_BMS_TEMPERATURES_TEMP1_RAW_MAX    255U
#define CAN_DB_BMS_TEMPERATURES_TEMP1_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_TEMP1_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_TEMP1_OFFSET_NUM ((int32_t)(-40))
#define CAN_DB_BMS_TEMPERATURES_TEMP1_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_TEMP1_SNA        255U

/*
 * Temp2: bits 24..31 (8 bits, unsigned); raw 0..255; physical = raw * 1 - 40 degC, range -40..214
 * degC. Value table: 255 SNA. As Temp1.
 */
#define CAN_DB_BMS_TEMPERATURES_TEMP2_START      24U
#define CAN_DB_BMS_TEMPERATURES_TEMP2_LENGTH     8U
#define CAN_DB_BMS_TEMPERATURES_TEMP2_RAW_MIN    0U
#define CAN_DB_BMS_TEMPERATURES_TEMP2_RAW_MAX    255U
#define CAN_DB_BMS_TEMPERATURES_TEMP2_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_TEMP2_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_TEMP2_OFFSET_NUM ((int32_t)(-40))
#define CAN_DB_BMS_TEMPERATURES_TEMP2_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_TEMP2_SNA        255U

/*
 * Temp3: bits 32..39 (8 bits, unsigned); raw 0..255; physical = raw * 1 - 40 degC, range -40..214
 * degC. Value table: 255 SNA. As Temp1.
 */
#define CAN_DB_BMS_TEMPERATURES_TEMP3_START      32U
#define CAN_DB_BMS_TEMPERATURES_TEMP3_LENGTH     8U
#define CAN_DB_BMS_TEMPERATURES_TEMP3_RAW_MIN    0U
#define CAN_DB_BMS_TEMPERATURES_TEMP3_RAW_MAX    255U
#define CAN_DB_BMS_TEMPERATURES_TEMP3_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_TEMP3_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_TEMP3_OFFSET_NUM ((int32_t)(-40))
#define CAN_DB_BMS_TEMPERATURES_TEMP3_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_TEMP3_SNA        255U

/*
 * Temp4: bits 40..47 (8 bits, unsigned); raw 0..255; physical = raw * 1 - 40 degC, range -40..214
 * degC. Value table: 255 SNA. As Temp1.
 */
#define CAN_DB_BMS_TEMPERATURES_TEMP4_START      40U
#define CAN_DB_BMS_TEMPERATURES_TEMP4_LENGTH     8U
#define CAN_DB_BMS_TEMPERATURES_TEMP4_RAW_MIN    0U
#define CAN_DB_BMS_TEMPERATURES_TEMP4_RAW_MAX    255U
#define CAN_DB_BMS_TEMPERATURES_TEMP4_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_TEMP4_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_TEMP4_OFFSET_NUM ((int32_t)(-40))
#define CAN_DB_BMS_TEMPERATURES_TEMP4_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_TEMPERATURES_TEMP4_SNA        255U

/** The raw signal values of BMS_Temperatures, in the order of their start bits. */
typedef struct {
    uint8_t e2e_crc;     /**< E2E_Crc */
    uint8_t e2e_counter; /**< E2E_Counter */
    uint8_t temp1;       /**< Temp1 */
    uint8_t temp2;       /**< Temp2 */
    uint8_t temp3;       /**< Temp3 */
    uint8_t temp4;       /**< Temp4 */
} can_db_bms_temperatures_t;

/**
 * Write the CAN_DB_BMS_TEMPERATURES_DLC data bytes of BMS_Temperatures into data[0] to data[7].
 * Returns false, writing nothing, if a pointer is NULL or `capacity`, the size of `data` in bytes,
 * is less than the DLC.
 */
bool can_db_bms_temperatures_pack(const can_db_bms_temperatures_t *msg, uint8_t *data,
                                  size_t capacity);

/**
 * Read the signals of BMS_Temperatures from `len` data bytes. Returns false, leaving *msg as it
 * was, if a pointer is NULL or `len` is not CAN_DB_BMS_TEMPERATURES_DLC.
 */
bool can_db_bms_temperatures_unpack(can_db_bms_temperatures_t *msg, const uint8_t *data,
                                    size_t len);

/* -------------------------------------------------------------------------------------------------
 * TESTER_Command: 0x100, 8 bytes, every 100 ms; from TESTER to BMS. Protected end to end (E2E
 * Profile 11A), Data ID 0x0200; the CRC of the Data ID bytes is 0x3A.
 * Commands to the node, sent cyclically every 100 ms so that their loss is detectable. Protected
 * end to end.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_TESTER_COMMAND_ID            0x100U
#define CAN_DB_TESTER_COMMAND_DLC           8U
#define CAN_DB_TESTER_COMMAND_CYCLE_MS      100U
#define CAN_DB_TESTER_COMMAND_E2E_PROTECTED 1U
#define CAN_DB_TESTER_COMMAND_E2E_DATA_ID   0x0200U

/*
 * E2E_Crc: bits 0..7 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. E2E Profile 11
 * CRC over the Data ID and bytes 1 to 7.
 */
#define CAN_DB_TESTER_COMMAND_E2E_CRC_START      0U
#define CAN_DB_TESTER_COMMAND_E2E_CRC_LENGTH     8U
#define CAN_DB_TESTER_COMMAND_E2E_CRC_RAW_MIN    0U
#define CAN_DB_TESTER_COMMAND_E2E_CRC_RAW_MAX    255U
#define CAN_DB_TESTER_COMMAND_E2E_CRC_FACTOR_NUM ((int32_t)1)
#define CAN_DB_TESTER_COMMAND_E2E_CRC_FACTOR_DEN ((int32_t)1)
#define CAN_DB_TESTER_COMMAND_E2E_CRC_OFFSET_NUM ((int32_t)0)
#define CAN_DB_TESTER_COMMAND_E2E_CRC_OFFSET_DEN ((int32_t)1)

/*
 * E2E_Counter: bits 8..11 (4 bits, unsigned); raw 0..15; physical = raw, range 0..14. E2E Profile
 * 11 counter: 0 to 14, then 0 again; 15 is never sent.
 */
#define CAN_DB_TESTER_COMMAND_E2E_COUNTER_START      8U
#define CAN_DB_TESTER_COMMAND_E2E_COUNTER_LENGTH     4U
#define CAN_DB_TESTER_COMMAND_E2E_COUNTER_RAW_MIN    0U
#define CAN_DB_TESTER_COMMAND_E2E_COUNTER_RAW_MAX    15U
#define CAN_DB_TESTER_COMMAND_E2E_COUNTER_FACTOR_NUM ((int32_t)1)
#define CAN_DB_TESTER_COMMAND_E2E_COUNTER_FACTOR_DEN ((int32_t)1)
#define CAN_DB_TESTER_COMMAND_E2E_COUNTER_OFFSET_NUM ((int32_t)0)
#define CAN_DB_TESTER_COMMAND_E2E_COUNTER_OFFSET_DEN ((int32_t)1)

/*
 * CmdTelemetryOn: bit 16 (1 bit, unsigned); raw 0..1; physical = raw, range 0..1. 0 silences the
 * protected cyclic telemetry (BMS_PackStatus, BMS_CellVoltages1, BMS_CellVoltages2,
 * BMS_Temperatures); the heartbeat, the task statistics and the node information go on. The node
 * behaves as if 1 until a valid command has arrived.
 */
#define CAN_DB_TESTER_COMMAND_CMD_TELEMETRY_ON_START      16U
#define CAN_DB_TESTER_COMMAND_CMD_TELEMETRY_ON_LENGTH     1U
#define CAN_DB_TESTER_COMMAND_CMD_TELEMETRY_ON_RAW_MIN    0U
#define CAN_DB_TESTER_COMMAND_CMD_TELEMETRY_ON_RAW_MAX    1U
#define CAN_DB_TESTER_COMMAND_CMD_TELEMETRY_ON_FACTOR_NUM ((int32_t)1)
#define CAN_DB_TESTER_COMMAND_CMD_TELEMETRY_ON_FACTOR_DEN ((int32_t)1)
#define CAN_DB_TESTER_COMMAND_CMD_TELEMETRY_ON_OFFSET_NUM ((int32_t)0)
#define CAN_DB_TESTER_COMMAND_CMD_TELEMETRY_ON_OFFSET_DEN ((int32_t)1)

/*
 * CmdClearFaultsSeq: bits 24..31 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. The
 * node clears its DTCs whenever this value changes from one command it carries out to the next
 * while the command stream stays valid; the first command carried out after the stream started or
 * was lost only sets the reference.
 */
#define CAN_DB_TESTER_COMMAND_CMD_CLEAR_FAULTS_SEQ_START      24U
#define CAN_DB_TESTER_COMMAND_CMD_CLEAR_FAULTS_SEQ_LENGTH     8U
#define CAN_DB_TESTER_COMMAND_CMD_CLEAR_FAULTS_SEQ_RAW_MIN    0U
#define CAN_DB_TESTER_COMMAND_CMD_CLEAR_FAULTS_SEQ_RAW_MAX    255U
#define CAN_DB_TESTER_COMMAND_CMD_CLEAR_FAULTS_SEQ_FACTOR_NUM ((int32_t)1)
#define CAN_DB_TESTER_COMMAND_CMD_CLEAR_FAULTS_SEQ_FACTOR_DEN ((int32_t)1)
#define CAN_DB_TESTER_COMMAND_CMD_CLEAR_FAULTS_SEQ_OFFSET_NUM ((int32_t)0)
#define CAN_DB_TESTER_COMMAND_CMD_CLEAR_FAULTS_SEQ_OFFSET_DEN ((int32_t)1)

/** The raw signal values of TESTER_Command, in the order of their start bits. */
typedef struct {
    uint8_t e2e_crc;              /**< E2E_Crc */
    uint8_t e2e_counter;          /**< E2E_Counter */
    uint8_t cmd_telemetry_on;     /**< CmdTelemetryOn */
    uint8_t cmd_clear_faults_seq; /**< CmdClearFaultsSeq */
} can_db_tester_command_t;

/**
 * Write the CAN_DB_TESTER_COMMAND_DLC data bytes of TESTER_Command into data[0] to data[7]. Returns
 * false, writing nothing, if a pointer is NULL or `capacity`, the size of `data` in bytes, is less
 * than the DLC.
 */
bool can_db_tester_command_pack(const can_db_tester_command_t *msg, uint8_t *data, size_t capacity);

/**
 * Read the signals of TESTER_Command from `len` data bytes. Returns false, leaving *msg as it was,
 * if a pointer is NULL or `len` is not CAN_DB_TESTER_COMMAND_DLC.
 */
bool can_db_tester_command_unpack(can_db_tester_command_t *msg, const uint8_t *data, size_t len);

/* -------------------------------------------------------------------------------------------------
 * BMS_TaskStats: 0x600, 8 bytes, every 200 ms; from BMS to TESTER. Not protected end to end.
 * Run-time statistics of one task per frame; the node cycles through its tasks, one frame every 200
 * ms. Not protected end to end.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_BMS_TASK_STATS_ID            0x600U
#define CAN_DB_BMS_TASK_STATS_DLC           8U
#define CAN_DB_BMS_TASK_STATS_CYCLE_MS      200U
#define CAN_DB_BMS_TASK_STATS_E2E_PROTECTED 0U

/*
 * TaskIndex: bits 0..3 (4 bits, unsigned); raw 0..15; physical = raw, range 0..15. Which task this
 * frame describes: 0 to the number of tasks minus 1.
 */
#define CAN_DB_BMS_TASK_STATS_TASK_INDEX_START      0U
#define CAN_DB_BMS_TASK_STATS_TASK_INDEX_LENGTH     4U
#define CAN_DB_BMS_TASK_STATS_TASK_INDEX_RAW_MIN    0U
#define CAN_DB_BMS_TASK_STATS_TASK_INDEX_RAW_MAX    15U
#define CAN_DB_BMS_TASK_STATS_TASK_INDEX_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_TASK_STATS_TASK_INDEX_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_TASK_STATS_TASK_INDEX_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_TASK_STATS_TASK_INDEX_OFFSET_DEN ((int32_t)1)

/*
 * TaskPriority: bits 4..7 (4 bits, unsigned); raw 0..15; physical = raw, range 0..15. The task's
 * FreeRTOS priority.
 */
#define CAN_DB_BMS_TASK_STATS_TASK_PRIORITY_START      4U
#define CAN_DB_BMS_TASK_STATS_TASK_PRIORITY_LENGTH     4U
#define CAN_DB_BMS_TASK_STATS_TASK_PRIORITY_RAW_MIN    0U
#define CAN_DB_BMS_TASK_STATS_TASK_PRIORITY_RAW_MAX    15U
#define CAN_DB_BMS_TASK_STATS_TASK_PRIORITY_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_TASK_STATS_TASK_PRIORITY_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_TASK_STATS_TASK_PRIORITY_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_TASK_STATS_TASK_PRIORITY_OFFSET_DEN ((int32_t)1)

/*
 * TaskCpuShare: bits 8..23 (16 bits, unsigned); raw 0..65535; physical = raw * 0.01 %, range 0..100
 * %. Share of the CPU time the task used since the node started.
 */
#define CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_START      8U
#define CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_LENGTH     16U
#define CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_RAW_MIN    0U
#define CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_RAW_MAX    65535U
#define CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_FACTOR_DEN ((int32_t)100)
#define CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_OFFSET_DEN ((int32_t)1)

/*
 * TaskStackPeak: bits 24..39 (16 bits, unsigned); raw 0..65535; physical = raw byte, range 0..65535
 * byte. Deepest stack use seen, saturated at 65535.
 */
#define CAN_DB_BMS_TASK_STATS_TASK_STACK_PEAK_START      24U
#define CAN_DB_BMS_TASK_STATS_TASK_STACK_PEAK_LENGTH     16U
#define CAN_DB_BMS_TASK_STATS_TASK_STACK_PEAK_RAW_MIN    0U
#define CAN_DB_BMS_TASK_STATS_TASK_STACK_PEAK_RAW_MAX    65535U
#define CAN_DB_BMS_TASK_STATS_TASK_STACK_PEAK_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_TASK_STATS_TASK_STACK_PEAK_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_TASK_STATS_TASK_STACK_PEAK_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_TASK_STATS_TASK_STACK_PEAK_OFFSET_DEN ((int32_t)1)

/*
 * TaskOverruns: bits 40..47 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. Periods
 * in which the task missed its deadline, saturated at 255.
 */
#define CAN_DB_BMS_TASK_STATS_TASK_OVERRUNS_START      40U
#define CAN_DB_BMS_TASK_STATS_TASK_OVERRUNS_LENGTH     8U
#define CAN_DB_BMS_TASK_STATS_TASK_OVERRUNS_RAW_MIN    0U
#define CAN_DB_BMS_TASK_STATS_TASK_OVERRUNS_RAW_MAX    255U
#define CAN_DB_BMS_TASK_STATS_TASK_OVERRUNS_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_TASK_STATS_TASK_OVERRUNS_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_TASK_STATS_TASK_OVERRUNS_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_TASK_STATS_TASK_OVERRUNS_OFFSET_DEN ((int32_t)1)

/** The raw signal values of BMS_TaskStats, in the order of their start bits. */
typedef struct {
    uint8_t task_index;       /**< TaskIndex */
    uint8_t task_priority;    /**< TaskPriority */
    uint16_t task_cpu_share;  /**< TaskCpuShare */
    uint16_t task_stack_peak; /**< TaskStackPeak */
    uint8_t task_overruns;    /**< TaskOverruns */
} can_db_bms_task_stats_t;

/**
 * Write the CAN_DB_BMS_TASK_STATS_DLC data bytes of BMS_TaskStats into data[0] to data[7]. Returns
 * false, writing nothing, if a pointer is NULL or `capacity`, the size of `data` in bytes, is less
 * than the DLC.
 */
bool can_db_bms_task_stats_pack(const can_db_bms_task_stats_t *msg, uint8_t *data, size_t capacity);

/**
 * Read the signals of BMS_TaskStats from `len` data bytes. Returns false, leaving *msg as it was,
 * if a pointer is NULL or `len` is not CAN_DB_BMS_TASK_STATS_DLC.
 */
bool can_db_bms_task_stats_unpack(can_db_bms_task_stats_t *msg, const uint8_t *data, size_t len);

/* -------------------------------------------------------------------------------------------------
 * BMS_NodeInfo: 0x610, 8 bytes, every 1000 ms; from BMS to TESTER. Not protected end to end.
 * Identity and status of the node, once per second. Not protected end to end.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_BMS_NODE_INFO_ID            0x610U
#define CAN_DB_BMS_NODE_INFO_DLC           8U
#define CAN_DB_BMS_NODE_INFO_CYCLE_MS      1000U
#define CAN_DB_BMS_NODE_INFO_E2E_PROTECTED 0U

/*
 * SwMajor: bits 0..7 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. Software
 * version, major number.
 */
#define CAN_DB_BMS_NODE_INFO_SW_MAJOR_START      0U
#define CAN_DB_BMS_NODE_INFO_SW_MAJOR_LENGTH     8U
#define CAN_DB_BMS_NODE_INFO_SW_MAJOR_RAW_MIN    0U
#define CAN_DB_BMS_NODE_INFO_SW_MAJOR_RAW_MAX    255U
#define CAN_DB_BMS_NODE_INFO_SW_MAJOR_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_SW_MAJOR_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_SW_MAJOR_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_NODE_INFO_SW_MAJOR_OFFSET_DEN ((int32_t)1)

/*
 * SwMinor: bits 8..15 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. Software
 * version, minor number.
 */
#define CAN_DB_BMS_NODE_INFO_SW_MINOR_START      8U
#define CAN_DB_BMS_NODE_INFO_SW_MINOR_LENGTH     8U
#define CAN_DB_BMS_NODE_INFO_SW_MINOR_RAW_MIN    0U
#define CAN_DB_BMS_NODE_INFO_SW_MINOR_RAW_MAX    255U
#define CAN_DB_BMS_NODE_INFO_SW_MINOR_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_SW_MINOR_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_SW_MINOR_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_NODE_INFO_SW_MINOR_OFFSET_DEN ((int32_t)1)

/*
 * SwPatch: bits 16..23 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. Software
 * version, patch number.
 */
#define CAN_DB_BMS_NODE_INFO_SW_PATCH_START      16U
#define CAN_DB_BMS_NODE_INFO_SW_PATCH_LENGTH     8U
#define CAN_DB_BMS_NODE_INFO_SW_PATCH_RAW_MIN    0U
#define CAN_DB_BMS_NODE_INFO_SW_PATCH_RAW_MAX    255U
#define CAN_DB_BMS_NODE_INFO_SW_PATCH_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_SW_PATCH_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_SW_PATCH_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_NODE_INFO_SW_PATCH_OFFSET_DEN ((int32_t)1)

/*
 * ActiveSlot: bits 24..25 (2 bits, unsigned); raw 0..3; physical = raw, range 0..1. Value table: 0
 * A, 1 B, 3 SNA. The firmware slot the node runs from, as the bootloader's hand-off names it; SNA
 * for a node started without the bootloader. BMS_BootInfo says whether the image runs as a trial.
 */
#define CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_START      24U
#define CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_LENGTH     2U
#define CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_RAW_MIN    0U
#define CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_RAW_MAX    3U
#define CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_A          0U
#define CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_B          1U
#define CAN_DB_BMS_NODE_INFO_ACTIVE_SLOT_SNA        3U

/*
 * ResetCause: bits 26..31 (6 bits, unsigned); raw 0..63; physical = raw, range 0..62. Value table:
 * 0 POWER_ON, 10 REQUESTED, 11 WATCHDOG, 12 POWER_LOSS, 13 IMAGE_FAULT, 20 ASSERT, 21
 * STACK_OVERFLOW, 22 MALLOC_FAILED, 63 SNA. Why the node was last reset: the values of
 * plat_reset_reason_t; POWER_ON for a start without a reset before it; SNA while the node does not
 * know.
 */
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_START          26U
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_LENGTH         6U
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_RAW_MIN        0U
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_RAW_MAX        63U
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_FACTOR_NUM     ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_FACTOR_DEN     ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_OFFSET_NUM     ((int32_t)0)
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_OFFSET_DEN     ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_POWER_ON       0U
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_REQUESTED      10U
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_WATCHDOG       11U
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_POWER_LOSS     12U
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_IMAGE_FAULT    13U
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_ASSERT         20U
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_STACK_OVERFLOW 21U
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_MALLOC_FAILED  22U
#define CAN_DB_BMS_NODE_INFO_RESET_CAUSE_SNA            63U

/*
 * UptimeSeconds: bits 32..63 (32 bits, unsigned); raw 0..4294967295; physical = raw s, range
 * 0..4294967295 s. Seconds since the node started.
 */
#define CAN_DB_BMS_NODE_INFO_UPTIME_SECONDS_START      32U
#define CAN_DB_BMS_NODE_INFO_UPTIME_SECONDS_LENGTH     32U
#define CAN_DB_BMS_NODE_INFO_UPTIME_SECONDS_RAW_MIN    0U
#define CAN_DB_BMS_NODE_INFO_UPTIME_SECONDS_RAW_MAX    4294967295U
#define CAN_DB_BMS_NODE_INFO_UPTIME_SECONDS_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_UPTIME_SECONDS_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_NODE_INFO_UPTIME_SECONDS_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_NODE_INFO_UPTIME_SECONDS_OFFSET_DEN ((int32_t)1)

/** The raw signal values of BMS_NodeInfo, in the order of their start bits. */
typedef struct {
    uint8_t sw_major;        /**< SwMajor */
    uint8_t sw_minor;        /**< SwMinor */
    uint8_t sw_patch;        /**< SwPatch */
    uint8_t active_slot;     /**< ActiveSlot */
    uint8_t reset_cause;     /**< ResetCause */
    uint32_t uptime_seconds; /**< UptimeSeconds */
} can_db_bms_node_info_t;

/**
 * Write the CAN_DB_BMS_NODE_INFO_DLC data bytes of BMS_NodeInfo into data[0] to data[7]. Returns
 * false, writing nothing, if a pointer is NULL or `capacity`, the size of `data` in bytes, is less
 * than the DLC.
 */
bool can_db_bms_node_info_pack(const can_db_bms_node_info_t *msg, uint8_t *data, size_t capacity);

/**
 * Read the signals of BMS_NodeInfo from `len` data bytes. Returns false, leaving *msg as it was, if
 * a pointer is NULL or `len` is not CAN_DB_BMS_NODE_INFO_DLC.
 */
bool can_db_bms_node_info_unpack(can_db_bms_node_info_t *msg, const uint8_t *data, size_t len);

/* -------------------------------------------------------------------------------------------------
 * BMS_BootInfo: 0x611, 8 bytes, every 1000 ms; from BMS to TESTER. Not protected end to end.
 * The image the node runs and its update state, once per second: the companion of BMS_NodeInfo,
 * whose 64 bits are all in use. Taken from the bootloader's hand-off at start, except ConfirmState
 * and ConfirmSeconds, which follow the confirm of a trial. Not protected end to end.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_BMS_BOOT_INFO_ID            0x611U
#define CAN_DB_BMS_BOOT_INFO_DLC           8U
#define CAN_DB_BMS_BOOT_INFO_CYCLE_MS      1000U
#define CAN_DB_BMS_BOOT_INFO_E2E_PROTECTED 0U

/*
 * BootMode: bits 0..1 (2 bits, unsigned); raw 0..3; physical = raw, range 0..2. Value table: 0
 * NONE, 1 CONFIRMED, 2 TRIAL. How the bootloader started the image: as the CONFIRMED image, as a
 * TRIAL that confirms itself once it has run in good health, or NONE: the node was started without
 * the bootloader (no hand-off), and every other signal of this message but ConfirmState is 0.
 */
#define CAN_DB_BMS_BOOT_INFO_BOOT_MODE_START      0U
#define CAN_DB_BMS_BOOT_INFO_BOOT_MODE_LENGTH     2U
#define CAN_DB_BMS_BOOT_INFO_BOOT_MODE_RAW_MIN    0U
#define CAN_DB_BMS_BOOT_INFO_BOOT_MODE_RAW_MAX    3U
#define CAN_DB_BMS_BOOT_INFO_BOOT_MODE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_BOOT_MODE_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_BOOT_MODE_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_BOOT_INFO_BOOT_MODE_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_BOOT_MODE_NONE       0U
#define CAN_DB_BMS_BOOT_INFO_BOOT_MODE_CONFIRMED  1U
#define CAN_DB_BMS_BOOT_INFO_BOOT_MODE_TRIAL      2U

/*
 * BootAttempt: bits 2..3 (2 bits, unsigned); raw 0..3; physical = raw, range 0..3. For a TRIAL, the
 * attempt this start is: 1 to 3 (a trial is rolled back when its third start does not confirm it);
 * 0 otherwise.
 */
#define CAN_DB_BMS_BOOT_INFO_BOOT_ATTEMPT_START      2U
#define CAN_DB_BMS_BOOT_INFO_BOOT_ATTEMPT_LENGTH     2U
#define CAN_DB_BMS_BOOT_INFO_BOOT_ATTEMPT_RAW_MIN    0U
#define CAN_DB_BMS_BOOT_INFO_BOOT_ATTEMPT_RAW_MAX    3U
#define CAN_DB_BMS_BOOT_INFO_BOOT_ATTEMPT_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_BOOT_ATTEMPT_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_BOOT_ATTEMPT_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_BOOT_INFO_BOOT_ATTEMPT_OFFSET_DEN ((int32_t)1)

/*
 * BootEvent: bits 4..6 (3 bits, unsigned); raw 0..7; physical = raw, range 0..7. Value table: 0
 * NONE, 1 ADOPT, 5 ROLLBACK, 6 REJECT, 7 FALLBACK. The state change the bootloader made before this
 * start: ADOPT (the factory image became the confirmed one), ROLLBACK (a trial used its 3
 * attempts), REJECT (a trial did not validate), FALLBACK (the confirmed image did not validate; the
 * one before it runs again), or NONE. ROLLBACK, REJECT and FALLBACK set the DTC UPDATE_ROLLBACK
 * (BMS_Fault).
 */
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_START      4U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_LENGTH     3U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_RAW_MIN    0U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_RAW_MAX    7U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_NONE       0U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_ADOPT      1U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_ROLLBACK   5U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_REJECT     6U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_FALLBACK   7U

/*
 * ConfirmState: bits 8..10 (3 bits, unsigned); raw 0..7; physical = raw, range 0..4. Value table: 0
 * NOT_NEEDED, 1 WAITING, 2 CONFIRMING, 3 CONFIRMED, 4 FAILED. NOT_NEEDED: the image is not a trial.
 * WAITING: a trial counting its time in good health (ConfirmSeconds). CONFIRMING: the confirm is
 * being written. CONFIRMED: this trial confirmed itself and is the active image. FAILED: the
 * confirm could not be written; the trial will be rolled back after its attempts.
 */
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_START      8U
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_LENGTH     3U
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_RAW_MIN    0U
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_RAW_MAX    7U
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_NOT_NEEDED 0U
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_WAITING    1U
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_CONFIRMING 2U
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_CONFIRMED  3U
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_STATE_FAILED     4U

/*
 * BootEventSlot: bits 11..12 (2 bits, unsigned); raw 0..3; physical = raw, range 0..1. Value table:
 * 0 A, 1 B, 3 SNA. The slot BootEvent acted on: the adopted slot (ADOPT), the rejected trial's
 * (ROLLBACK, REJECT), or the slot fallen back to, whose image is the confirmed one again
 * (FALLBACK); SNA for none.
 */
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_START      11U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_LENGTH     2U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_RAW_MIN    0U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_RAW_MAX    3U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_OFFSET_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_A          0U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_B          1U
#define CAN_DB_BMS_BOOT_INFO_BOOT_EVENT_SLOT_SNA        3U

/*
 * ImageVersionMajor: bits 16..23 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. The
 * version of the running image as its header gives it (major.minor.patch); 0 without the
 * bootloader.
 */
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MAJOR_START      16U
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MAJOR_LENGTH     8U
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MAJOR_RAW_MIN    0U
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MAJOR_RAW_MAX    255U
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MAJOR_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MAJOR_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MAJOR_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MAJOR_OFFSET_DEN ((int32_t)1)

/*
 * ImageVersionMinor: bits 24..31 (8 bits, unsigned); raw 0..255; physical = raw, range 0..255. As
 * ImageVersionMajor.
 */
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MINOR_START      24U
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MINOR_LENGTH     8U
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MINOR_RAW_MIN    0U
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MINOR_RAW_MAX    255U
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MINOR_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MINOR_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MINOR_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_MINOR_OFFSET_DEN ((int32_t)1)

/*
 * ImageVersionPatch: bits 32..47 (16 bits, unsigned); raw 0..65535; physical = raw, range 0..65535.
 * As ImageVersionMajor.
 */
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_PATCH_START      32U
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_PATCH_LENGTH     16U
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_PATCH_RAW_MIN    0U
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_PATCH_RAW_MAX    65535U
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_PATCH_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_PATCH_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_PATCH_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_BOOT_INFO_IMAGE_VERSION_PATCH_OFFSET_DEN ((int32_t)1)

/*
 * ConfirmSeconds: bits 48..55 (8 bits, unsigned); raw 0..255; physical = raw s, range 0..255 s. For
 * a trial: the seconds of good health counted towards its confirm (the watchdog manager OK, the
 * controller error active and not bus off since the period before, and a frame of the node
 * acknowledged since then, in every 100 ms period without a break), saturated at 255; the confirm
 * is made at 5.
 */
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_SECONDS_START      48U
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_SECONDS_LENGTH     8U
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_SECONDS_RAW_MIN    0U
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_SECONDS_RAW_MAX    255U
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_SECONDS_FACTOR_NUM ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_SECONDS_FACTOR_DEN ((int32_t)1)
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_SECONDS_OFFSET_NUM ((int32_t)0)
#define CAN_DB_BMS_BOOT_INFO_CONFIRM_SECONDS_OFFSET_DEN ((int32_t)1)

/** The raw signal values of BMS_BootInfo, in the order of their start bits. */
typedef struct {
    uint8_t boot_mode;            /**< BootMode */
    uint8_t boot_attempt;         /**< BootAttempt */
    uint8_t boot_event;           /**< BootEvent */
    uint8_t confirm_state;        /**< ConfirmState */
    uint8_t boot_event_slot;      /**< BootEventSlot */
    uint8_t image_version_major;  /**< ImageVersionMajor */
    uint8_t image_version_minor;  /**< ImageVersionMinor */
    uint16_t image_version_patch; /**< ImageVersionPatch */
    uint8_t confirm_seconds;      /**< ConfirmSeconds */
} can_db_bms_boot_info_t;

/**
 * Write the CAN_DB_BMS_BOOT_INFO_DLC data bytes of BMS_BootInfo into data[0] to data[7]. Returns
 * false, writing nothing, if a pointer is NULL or `capacity`, the size of `data` in bytes, is less
 * than the DLC.
 */
bool can_db_bms_boot_info_pack(const can_db_bms_boot_info_t *msg, uint8_t *data, size_t capacity);

/**
 * Read the signals of BMS_BootInfo from `len` data bytes. Returns false, leaving *msg as it was, if
 * a pointer is NULL or `len` is not CAN_DB_BMS_BOOT_INFO_DLC.
 */
bool can_db_bms_boot_info_unpack(can_db_bms_boot_info_t *msg, const uint8_t *data, size_t len);

/* -------------------------------------------------------------------------------------------------
 * TESTER_Ping: 0x6F0, 8 bytes, sent on events; from TESTER to -. Not protected end to end.
 * Echo request: any payload of 0 to 8 bytes. No signals; used for link tests and latency
 * measurements.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_TESTER_PING_ID            0x6F0U
#define CAN_DB_TESTER_PING_DLC           8U
#define CAN_DB_TESTER_PING_CYCLE_MS      0U
#define CAN_DB_TESTER_PING_E2E_PROTECTED 0U

/* No signals: the payload is passed on as raw bytes. */

/* -------------------------------------------------------------------------------------------------
 * BMS_Pong: 0x6F1, 8 bytes, sent on events; from BMS to -. Not protected end to end.
 * Echo reply: the payload of the request, 0 to 8 bytes.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_BMS_PONG_ID            0x6F1U
#define CAN_DB_BMS_PONG_DLC           8U
#define CAN_DB_BMS_PONG_CYCLE_MS      0U
#define CAN_DB_BMS_PONG_E2E_PROTECTED 0U

/* No signals: the payload is passed on as raw bytes. */

/* -------------------------------------------------------------------------------------------------
 * UDS_Request: 0x7E0, 8 bytes, sent on events; from TESTER to -. Not protected end to end.
 * Diagnostic requests, ISO 15765-2 frames (phase 5). No signals.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_UDS_REQUEST_ID            0x7E0U
#define CAN_DB_UDS_REQUEST_DLC           8U
#define CAN_DB_UDS_REQUEST_CYCLE_MS      0U
#define CAN_DB_UDS_REQUEST_E2E_PROTECTED 0U

/* No signals: the payload is passed on as raw bytes. */

/* -------------------------------------------------------------------------------------------------
 * UDS_Response: 0x7E8, 8 bytes, sent on events; from BMS to -. Not protected end to end.
 * Diagnostic responses, ISO 15765-2 frames (phase 5). No signals.
 * ---------------------------------------------------------------------------------------------- */
#define CAN_DB_UDS_RESPONSE_ID            0x7E8U
#define CAN_DB_UDS_RESPONSE_DLC           8U
#define CAN_DB_UDS_RESPONSE_CYCLE_MS      0U
#define CAN_DB_UDS_RESPONSE_E2E_PROTECTED 0U

/* No signals: the payload is passed on as raw bytes. */

#endif /* FCN_CAN_DB_H */
