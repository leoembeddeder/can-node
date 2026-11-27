/* SPDX-License-Identifier: MIT */
/*
 * can_db_cli: the generated CAN codec (app/gen/can_db.c) behind a pipe.
 *
 * tests/python/test_gen_can_db.py compares the codec with cantools, which reads the DBC on its
 * own, on thousands of frames. It reaches the codec through this program and the table of
 * can_db_table.h.
 *
 *     can_db_cli --list
 *
 * prints the table, one line per message in the order of the table, and exits:
 *
 *     <id> <name> <dlc> <cycle_ms> <e2e_data_id> <signal count>[ <signal>]...
 *     <signal> = <name>:<start>:<length>:<u|s>:<raw_min>:<raw_max>:<field_min>:<field_max>
 *
 *     020 BMS_PackStatus 8 10 288 8 E2E_Crc:0:8:u:0:255:0:255 ...
 *
 * Without arguments it reads requests from standard input, one per line, and answers every
 * line with exactly one line on standard output, so that answer n always belongs to line n:
 *
 *     P <id> <raw value>...     pack: the message's DLC data bytes, two hexadecimal digits each
 *     U <id> <data>             unpack: the raw values, separated by one space
 *
 *     P 40 0 14 2700 0 4094 4095    000E8C0A00FEFFFF       (upper case, nothing in between)
 *     U 40 000E8C0A00FEFFFF         0 14 2700 0 4094 4095
 *
 *     id         identifier of a message with signals: 1 to 8 hexadecimal digits, no prefix
 *     raw value  decimal, with a leading '-' if negative, one for every signal of the message in
 *                the order of the table, each within the range of the C type of its struct
 *                member (field_min..field_max); bits above the signal's length are passed to
 *                pack() as they are
 *     data       0 to 8 bytes, two hexadecimal digits each, nothing in between; unpack() gets
 *                exactly these bytes and refuses any number but the DLC
 *
 * The answer is "invalid" for a line that is not of this form, an identifier without signals
 * in the table, a wrong number of values, a value outside its range, and whenever pack() or
 * unpack() returns false. The form is taken literally, as in can_bits_cli: no blanks at either
 * end, one space between fields, no "0x", no "+", no carriage return.
 *
 * The exit status is 0 when the input was read to its end, whatever the answers were, and 1 if
 * reading or writing failed or the command line is not one of the two above.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "can_db_table.h"

/* The longest request: "P", an identifier of 8 digits, 64 values of up to 12 characters. */
#define LINE_CAPACITY 1024U

#define ID_DIGITS_MAX    8U
#define VALUE_DIGITS_MAX 11U /* more than any 32-bit value has, few enough for int64_t */
#define DATA_BYTES_MAX   8U

typedef struct {
    char text[LINE_CAPACITY];
    size_t length; /* characters in the line; more than LINE_CAPACITY if it did not fit */
} line_t;

typedef struct {
    const char *text;
    size_t length;
    size_t pos;
} cursor_t;

/* Reads up to and including the next newline; false if the input ended before a line began.
 * A longer line is read to its end, so that it gets one answer like any other, but only its
 * length is kept. */
static bool read_line(FILE *in, line_t *line)
{
    int c = getc(in);
    const bool any = (c != EOF);

    line->length = 0U;

    while ((c != EOF) && (c != '\n')) {
        if (line->length < LINE_CAPACITY) {
            line->text[line->length] = (char)c;
        }

        line->length++;
        c = getc(in);
    }

    return any;
}

static bool take_char(cursor_t *cur, char expected)
{
    bool taken = false;

    if ((cur->pos < cur->length) && (cur->text[cur->pos] == expected)) {
        cur->pos++;
        taken = true;
    }

    return taken;
}

/* Value of a digit in base 10 or 16, or `base` itself if `c` is not a digit of that base. */
static uint32_t digit_value(char c, uint32_t base)
{
    uint32_t value = base;

    if ((c >= '0') && (c <= '9')) {
        value = (uint32_t)(c - '0');
    } else if ((c >= 'A') && (c <= 'F')) {
        value = (uint32_t)(c - 'A') + 10U;
    } else if ((c >= 'a') && (c <= 'f')) {
        value = (uint32_t)(c - 'a') + 10U;
    } else {
        /* not a digit */
    }

    return (value < base) ? value : base;
}

/* Takes all digits that follow and succeeds if there are between 1 and `max_digits` of them. */
static bool take_number(cursor_t *cur, uint32_t base, size_t max_digits, uint64_t *number)
{
    uint64_t value = 0U;
    size_t digits = 0U;
    bool more = true;

    while (more && (cur->pos < cur->length)) {
        const uint32_t digit = digit_value(cur->text[cur->pos], base);

        if (digit < base) {
            /* Cannot wrap: a run longer than max_digits is rejected below, and 11 decimal or
             * 8 hexadecimal digits fit 64 bits with room to spare; the count only grows. */
            if (digits < max_digits) {
                value = (value * base) + digit;
            }
            digits++;
            cur->pos++;
        } else {
            more = false;
        }
    }

    *number = value;
    return (digits >= 1U) && (digits <= max_digits);
}

static bool take_value(cursor_t *cur, int64_t *value)
{
    const bool negative = take_char(cur, '-');
    uint64_t magnitude = 0U;
    const bool ok = take_number(cur, 10U, VALUE_DIGITS_MAX, &magnitude);

    *value = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    return ok;
}

/* Takes the rest of the line as bytes of two hexadecimal digits each; fails on anything else
 * and on more than DATA_BYTES_MAX bytes. */
static bool take_bytes(cursor_t *cur, uint8_t *bytes, size_t *count)
{
    size_t taken = 0U;
    bool ok = true;

    while (ok && (cur->pos < cur->length)) {
        const uint32_t high = digit_value(cur->text[cur->pos], 16U);
        const uint32_t low =
            ((cur->pos + 1U) < cur->length) ? digit_value(cur->text[cur->pos + 1U], 16U) : 16U;

        if ((high < 16U) && (low < 16U) && (taken < DATA_BYTES_MAX)) {
            bytes[taken] = (uint8_t)((high << 4U) | low);
            taken++;
            cur->pos += 2U;
        } else {
            ok = false;
        }
    }

    *count = taken;
    return ok;
}

/* The message of the table with this identifier, if it has signals; NULL otherwise. */
static const can_db_table_message_t *find_message(uint64_t id)
{
    const can_db_table_message_t *found = NULL;

    for (size_t i = 0U; (i < can_db_table_count) && (found == NULL); i++) {
        if ((can_db_table[i].id == id) && (can_db_table[i].signal_count > 0U)) {
            found = &can_db_table[i];
        }
    }

    return found;
}

static void print_hex(const uint8_t *bytes, size_t count)
{
    for (size_t i = 0U; i < count; i++) {
        (void)printf("%02X", (unsigned int)bytes[i]);
    }
    (void)putchar('\n');
}

/* "P <id> <raw value>...": pack. */
static void answer_pack(cursor_t *cur, const can_db_table_message_t *message)
{
    int64_t raw[CAN_DB_TABLE_MAX_SIGNALS] = {0};
    uint8_t data[DATA_BYTES_MAX] = {0};
    bool ok = true;

    for (size_t i = 0U; ok && (i < message->signal_count); i++) {
        const can_db_table_signal_t *signal = &message->signals[i];

        ok = take_char(cur, ' ') && take_value(cur, &raw[i]) && (raw[i] >= signal->field_min) &&
             (raw[i] <= signal->field_max);
    }

    ok = ok && (cur->pos == cur->length) && message->pack(raw, data, sizeof(data));

    if (ok) {
        print_hex(data, message->dlc);
    } else {
        (void)puts("invalid");
    }
}

/* "U <id> <data>": unpack. */
static void answer_unpack(cursor_t *cur, const can_db_table_message_t *message)
{
    int64_t raw[CAN_DB_TABLE_MAX_SIGNALS] = {0};
    uint8_t data[DATA_BYTES_MAX] = {0};
    size_t count = 0U;
    bool ok = true;

    /* The data, and with it the space in front of it, may be missing: zero bytes. */
    if (cur->pos < cur->length) {
        ok = take_char(cur, ' ') && take_bytes(cur, data, &count);
    }

    ok = ok && message->unpack(data, count, raw);

    if (ok) {
        for (size_t i = 0U; i < message->signal_count; i++) {
            (void)printf((i == 0U) ? "%lld" : " %lld", (long long)raw[i]);
        }
        (void)putchar('\n');
    } else {
        (void)puts("invalid");
    }
}

static void answer(const line_t *line)
{
    cursor_t cur = {.text = line->text, .length = line->length, .pos = 0U};
    const bool pack = take_char(&cur, 'P');
    const bool unpack = !pack && take_char(&cur, 'U');
    uint64_t id = 0U;
    const can_db_table_message_t *message = NULL;

    if ((line->length <= LINE_CAPACITY) && (pack || unpack) && take_char(&cur, ' ') &&
        take_number(&cur, 16U, ID_DIGITS_MAX, &id)) {
        message = find_message(id);
    }

    if (message == NULL) {
        (void)puts("invalid");
    } else if (pack) {
        answer_pack(&cur, message);
    } else {
        answer_unpack(&cur, message);
    }
}

static void list_table(void)
{
    for (size_t i = 0U; i < can_db_table_count; i++) {
        const can_db_table_message_t *message = &can_db_table[i];

        (void)printf("%03X %s %u %u %ld %zu", (unsigned int)message->id, message->name,
                     (unsigned int)message->dlc, (unsigned int)message->cycle_ms,
                     (long)message->e2e_data_id, message->signal_count);

        for (size_t s = 0U; s < message->signal_count; s++) {
            const can_db_table_signal_t *signal = &message->signals[s];

            (void)printf(" %s:%u:%u:%c:%lld:%lld:%lld:%lld", signal->name,
                         (unsigned int)signal->start, (unsigned int)signal->length,
                         signal->is_signed ? 's' : 'u', (long long)signal->raw_min,
                         (long long)signal->raw_max, (long long)signal->field_min,
                         (long long)signal->field_max);
        }
        (void)putchar('\n');
    }
}

int main(int argc, char **argv)
{
    int status = EXIT_SUCCESS;

    if ((argc == 2) && (strcmp(argv[1], "--list") == 0)) {
        list_table();
    } else if (argc == 1) {
        line_t line;

        while (read_line(stdin, &line)) {
            answer(&line);
            /* Answer at once: a caller may wait for the answer before it writes the next line. */
            (void)fflush(stdout);
        }
    } else {
        (void)fprintf(stderr, "usage: %s [--list]\n", argv[0]);
        status = EXIT_FAILURE;
    }

    if ((ferror(stdin) != 0) || (ferror(stdout) != 0)) {
        status = EXIT_FAILURE;
    }

    return status;
}
