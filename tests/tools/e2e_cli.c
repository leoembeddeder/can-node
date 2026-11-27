/* SPDX-License-Identifier: MIT */
/*
 * e2e_cli: e2e_p11_protect() and e2e_p11_check() behind a pipe.
 *
 * tools/fcn_tools/e2e.py implements Profile 11 a second time, independently, and
 * tests/python/test_e2e.py compares the two on many messages. This program reads one request
 * per line from standard input and answers every line with exactly one line on standard
 * output, so that answer n always belongs to line n. It keeps no state between lines: every
 * request carries the state it is made from, and every answer the state it leaves, so that the
 * caller can compare both sides from the same state at every step of a sequence.
 *
 *     input                                                output
 *     I                                                    <ret> <counter> <ret> <counter> <status>
 *     P <id> <len> <counter> <data>                        <ret> <counter> <data>
 *     C <id> <len> <max> <counter> <status> <data>         <ret> <counter> <status>
 *
 *     I        e2e_p11_protect_init() and e2e_p11_check_init() on states full of 0xA5, and
 *              what they left: return code and counter of the sender, return code, counter
 *              and status of the receiver
 *     P        e2e_p11_protect() of the message <data> with Data ID <id>, data_len <len> and a
 *              sender whose counter is <counter> (max_delta_counter is 1, which protect does
 *              not use); the answer is the return code, the sender's counter afterwards and
 *              the message afterwards
 *     C        e2e_p11_check() of <data> with Data ID <id>, data_len <len>, max_delta_counter
 *              <max>, from a receiver whose counter is <counter> and whose status is
 *              <status>; the answer is the return code and the receiver's counter and status
 *              afterwards
 *
 *     id       the Data ID: 1 to 4 hexadecimal digits
 *     len      data_len: 1 to 3 decimal digits, 0..255
 *     max      max_delta_counter: 1 to 3 decimal digits, 0..255
 *     counter  a state's counter: 1 to 3 decimal digits, 0..255
 *     status   OK, NONEWDATA, ERROR, REPEATED, OKSOMELOST or WRONGSEQUENCE
 *     data     the message: two hexadecimal digits per byte, 1 to 16 bytes, nothing in
 *              between; or "-" for no message, which passes NULL and length 0
 *     ret      E_OK, E_INPUTERR_NULL or E_INPUTERR_WRONG
 *
 * The length passed to the functions is the number of bytes given, so a message whose length
 * differs from <len> is a request like any other. Ranges wider than the module accepts are
 * allowed on purpose: what is out of range for the module is its answer to give. Hexadecimal
 * digits are accepted in either case and written in upper case; the fields are separated by
 * one space each. "invalid" is the answer to a line that is not of this form, taken literally
 * as in tests/tools/can_bits_cli.c: no blanks at either end, no tabs, no carriage return.
 *
 * The exit status is 0 when the input was read to its end, whatever the answers were, and 1
 * if reading or writing failed.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "e2e/e2e_p11.h"

/* The longest line: "C FFFF 255 255 255 WRONGSEQUENCE " and 32 digits. */
#define LINE_CAPACITY 72U
#define DATA_CAPACITY 16U

/* One line of input without its newline. A longer line is read to its end, so that it gets
 * one answer like any other line, but only its length is kept. */
typedef struct {
    char text[LINE_CAPACITY];
    size_t length; /* characters in the line; more than LINE_CAPACITY if it did not fit */
} line_t;

/* Where the parser stands in a line. */
typedef struct {
    const char *text;
    size_t length;
    size_t pos;
} cursor_t;

/* A message as given: `present` false for "-". */
typedef struct {
    bool present;
    uint8_t bytes[DATA_CAPACITY];
    size_t length;
} message_t;

static const struct {
    const char *name;
    e2e_p11_status_t value;
} s_statuses[] = {
    {"OK", E2E_P11STATUS_OK},
    {"NONEWDATA", E2E_P11STATUS_NONEWDATA},
    {"ERROR", E2E_P11STATUS_ERROR},
    {"REPEATED", E2E_P11STATUS_REPEATED},
    {"OKSOMELOST", E2E_P11STATUS_OKSOMELOST},
    {"WRONGSEQUENCE", E2E_P11STATUS_WRONGSEQUENCE},
};

/* Reads up to and including the next newline. Returns false if the input ended before a line
 * began. The last line counts with or without a newline behind it, and an empty line is a
 * line: it has to be answered too, or the answers would shift against the questions. */
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

/* Takes all digits that follow and succeeds if there are between 1 and `max_digits` of them
 * and the number is at most `max_value`. */
static bool take_number(cursor_t *cur, uint32_t base, size_t max_digits, uint32_t max_value,
                        uint32_t *number)
{
    uint32_t value = 0U;
    size_t digits = 0U;
    bool more = true;

    while (more && (cur->pos < cur->length)) {
        const uint32_t digit = digit_value(cur->text[cur->pos], base);

        if ((digit < base) && (digits < max_digits)) {
            value = (value * base) + digit;
            digits++;
            cur->pos++;
        } else {
            more = false;
        }
    }

    *number = value;
    /* A digit right behind the last one taken means too many digits. */
    return (digits >= 1U) && (value <= max_value) &&
           ((cur->pos >= cur->length) || (digit_value(cur->text[cur->pos], base) == base));
}

static bool take_byte_value(cursor_t *cur, uint8_t *value)
{
    uint32_t number = 0U;
    const bool ok = take_number(cur, 10U, 3U, 255U, &number);

    *value = (uint8_t)number;
    return ok;
}

/* The message: "-", or the rest of the line as bytes of two hexadecimal digits each. */
static bool take_message(cursor_t *cur, message_t *message)
{
    bool ok = true;

    message->present = !take_char(cur, '-');
    message->length = 0U;

    if (message->present) {
        while (ok && (cur->pos < cur->length)) {
            const uint32_t high = digit_value(cur->text[cur->pos], 16U);
            const uint32_t low =
                ((cur->pos + 1U) < cur->length) ? digit_value(cur->text[cur->pos + 1U], 16U) : 16U;

            if ((high < 16U) && (low < 16U) && (message->length < DATA_CAPACITY)) {
                message->bytes[message->length] = (uint8_t)((high << 4U) | low);
                message->length++;
                cur->pos += 2U;
            } else {
                ok = false;
            }
        }

        ok = ok && (message->length > 0U);
    }

    return ok && (cur->pos == cur->length);
}

static bool take_status(cursor_t *cur, e2e_p11_status_t *status)
{
    bool found = false;

    for (size_t i = 0U; (i < (sizeof(s_statuses) / sizeof(s_statuses[0]))) && !found; i++) {
        const size_t n = strlen(s_statuses[i].name);
        const size_t end = cur->pos + n;

        /* The whole name, followed by the end of the line or a space: "OK" must not match
         * the beginning of "OKSOMELOST". */
        if ((end <= cur->length) && (strncmp(&cur->text[cur->pos], s_statuses[i].name, n) == 0) &&
            ((end == cur->length) || (cur->text[end] == ' '))) {
            *status = s_statuses[i].value;
            cur->pos += n;
            found = true;
        }
    }

    return found;
}

/* "?" for a value that is no status: the caller's parser refuses the answer then. */
static const char *status_name(e2e_p11_status_t status)
{
    const char *name = "?";

    for (size_t i = 0U; i < (sizeof(s_statuses) / sizeof(s_statuses[0])); i++) {
        if (s_statuses[i].value == status) {
            name = s_statuses[i].name;
        }
    }

    return name;
}

/* "?" for a value that is no return code: the caller's parser refuses the answer then. */
static const char *result_name(e2e_result_t result)
{
    const char *name;

    switch (result) {
    case E2E_E_OK:
        name = "E_OK";
        break;
    case E2E_E_INPUTERR_NULL:
        name = "E_INPUTERR_NULL";
        break;
    case E2E_E_INPUTERR_WRONG:
        name = "E_INPUTERR_WRONG";
        break;
    case E2E_E_WRONGSTATE:
        name = "E_WRONGSTATE";
        break;
    default:
        name = "?";
        break;
    }

    return name;
}

static bool answer_init(const cursor_t *cur)
{
    e2e_p11_protect_state_t sender;
    e2e_p11_check_state_t receiver;
    bool ok = (cur->pos == cur->length);

    if (ok) {
        (void)memset(&sender, 0xA5, sizeof(sender));
        (void)memset(&receiver, 0xA5, sizeof(receiver));
        const e2e_result_t sent = e2e_p11_protect_init(&sender);
        const e2e_result_t checked = e2e_p11_check_init(&receiver);

        (void)printf("%s %u %s %u %s\n", result_name(sent), (unsigned int)sender.counter,
                     result_name(checked), (unsigned int)receiver.counter,
                     status_name(receiver.status));
    }

    return ok;
}

static bool answer_protect(cursor_t *cur)
{
    uint32_t id = 0U;
    e2e_p11_config_t config = {.data_id = 0U, .data_len = 0U, .max_delta_counter = 1U};
    e2e_p11_protect_state_t sender = {.counter = 0U};
    message_t message = {.present = false, .bytes = {0U}, .length = 0U};
    bool ok = take_char(cur, ' ');

    ok = ok && take_number(cur, 16U, 4U, 0xFFFFU, &id);
    ok = ok && take_char(cur, ' ') && take_byte_value(cur, &config.data_len);
    ok = ok && take_char(cur, ' ') && take_byte_value(cur, &sender.counter);
    ok = ok && take_char(cur, ' ') && take_message(cur, &message);

    if (ok) {
        uint8_t *data = message.present ? message.bytes : NULL;
        e2e_result_t result;

        config.data_id = (uint16_t)id;
        result = e2e_p11_protect(&config, &sender, data, message.length);
        (void)printf("%s %u ", result_name(result), (unsigned int)sender.counter);

        if (message.present) {
            for (size_t i = 0U; i < message.length; i++) {
                (void)printf("%02X", (unsigned int)message.bytes[i]);
            }
        } else {
            (void)printf("-");
        }

        (void)printf("\n");
    }

    return ok;
}

static bool answer_check(cursor_t *cur)
{
    uint32_t id = 0U;
    e2e_p11_config_t config = {.data_id = 0U, .data_len = 0U, .max_delta_counter = 0U};
    e2e_p11_check_state_t receiver = {.counter = 0U, .status = E2E_P11STATUS_ERROR};
    message_t message = {.present = false, .bytes = {0U}, .length = 0U};
    bool ok = take_char(cur, ' ');

    ok = ok && take_number(cur, 16U, 4U, 0xFFFFU, &id);
    ok = ok && take_char(cur, ' ') && take_byte_value(cur, &config.data_len);
    ok = ok && take_char(cur, ' ') && take_byte_value(cur, &config.max_delta_counter);
    ok = ok && take_char(cur, ' ') && take_byte_value(cur, &receiver.counter);
    ok = ok && take_char(cur, ' ') && take_status(cur, &receiver.status);
    ok = ok && take_char(cur, ' ') && take_message(cur, &message);

    if (ok) {
        const uint8_t *data = message.present ? message.bytes : NULL;
        e2e_result_t result;

        config.data_id = (uint16_t)id;
        result = e2e_p11_check(&config, &receiver, data, message.length);
        (void)printf("%s %u %s\n", result_name(result), (unsigned int)receiver.counter,
                     status_name(receiver.status));
    }

    return ok;
}

static bool answer(const line_t *line)
{
    cursor_t cur = {.text = line->text, .length = line->length, .pos = 0U};
    bool ok = (line->length <= LINE_CAPACITY);

    if (ok && take_char(&cur, 'I')) {
        ok = answer_init(&cur);
    } else if (ok && take_char(&cur, 'P')) {
        ok = answer_protect(&cur);
    } else if (ok && take_char(&cur, 'C')) {
        ok = answer_check(&cur);
    } else {
        ok = false;
    }

    return ok;
}

int main(void)
{
    line_t line;

    while (read_line(stdin, &line)) {
        if (!answer(&line)) {
            (void)puts("invalid");
        }

        /* Answer at once: a caller may write one line and wait for its answer before it
         * writes the next, and would wait forever for an answer that sits in a buffer. */
        (void)fflush(stdout);
    }

    return ((ferror(stdin) != 0) || (ferror(stdout) != 0)) ? EXIT_FAILURE : EXIT_SUCCESS;
}
