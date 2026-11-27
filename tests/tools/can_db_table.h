/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The generated CAN codec (app/gen/can_db.h) as a table, for the tests.
 *
 * The codec has one struct and one pair of functions per message, which a test can only call
 * by name. To run the same check over every signal of every message, and to answer questions
 * about any message from Python (can_db_cli), the tests need the messages as data: this table
 * lists every message of the DBC with its signals, and for every message with signals two
 * adapters between an array of raw values, one per signal in the order of the table, and the
 * codec's struct.
 *
 * can_db_table.c is generated from the DBC together with the codec (tools/gen_can_db.py
 * --test-table), so it cannot fall behind it; tests/python/test_gen_can_db.py checks it
 * against cantools. It is not part of the node.
 */
#ifndef FCN_CAN_DB_TABLE_H
#define FCN_CAN_DB_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** The most signals a message can have: 64 signals of 1 bit in 8 bytes. */
#define CAN_DB_TABLE_MAX_SIGNALS 64U

typedef struct {
    const char *name; /**< as in the DBC */
    uint8_t start;    /**< first bit; bit 0 is the least significant bit of byte 0 */
    uint8_t length;   /**< bits, 1..32 */
    bool is_signed;   /**< two's complement */
    int64_t raw_min;  /**< range of the raw value */
    int64_t raw_max;
    int64_t field_min; /**< range of the C type of the struct member that holds it */
    int64_t field_max;
} can_db_table_signal_t;

/** Packs raw[0..signal_count-1] (each within field_min..field_max) with the message's pack(),
 *  whose result it returns. */
typedef bool (*can_db_table_pack_t)(const int64_t *raw, uint8_t *data, size_t capacity);

/** Unpacks with the message's unpack() and, if that succeeds, stores the members in
 *  raw[0..signal_count-1]. Returns what unpack() returned. */
typedef bool (*can_db_table_unpack_t)(const uint8_t *data, size_t len, int64_t *raw);

typedef struct {
    const char *name;
    uint32_t id;
    uint8_t dlc;
    uint16_t cycle_ms;                    /**< 0: sent on events */
    int32_t e2e_data_id;                  /**< -1: not protected end to end */
    size_t signal_count;                  /**< 0..CAN_DB_TABLE_MAX_SIGNALS */
    const can_db_table_signal_t *signals; /**< in the order of their start bits; NULL if none */
    can_db_table_pack_t pack;             /**< NULL for a message without signals */
    can_db_table_unpack_t unpack;         /**< NULL for a message without signals */
} can_db_table_message_t;

/** Every message of the DBC, in the order of their identifiers. */
extern const can_db_table_message_t can_db_table[];
extern const size_t can_db_table_count;

#endif /* FCN_CAN_DB_TABLE_H */
