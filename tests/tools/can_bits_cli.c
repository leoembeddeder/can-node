/* SPDX-License-Identifier: MIT */
/*
 * can_bits_cli: can_bits_exact() behind a pipe.
 *
 * The frame length is computed a second time by an independent implementation in Python. The
 * two can only be compared on thousands of frames if Python can ask the C code. This program
 * reads one frame per line from standard input and answers every line with exactly one line
 * on standard output, so that answer n always belongs to line n:
 *
 *     input     <id> <S|E> <D|R> <dlc> <data>            222 S D 5 0011223344
 *     output    <frame_bits> <stuff_bits> <crc15>        87 3 66DA
 *               invalid
 *
 *     id      identifier: 1 to 8 hexadecimal digits, no prefix
 *     S|E     base ("standard") or extended format
 *     D|R     data frame or remote frame
 *     dlc     data length code: 1 or 2 decimal digits
 *     data    the data bytes: two hexadecimal digits each, nothing in between. A data frame
 *             has exactly `dlc` of them, a remote frame none: it transmits its DLC but no data
 *     crc15   four hexadecimal digits
 *
 * Hexadecimal digits are accepted in either case and written in upper case. The fields are
 * separated by one space each. A line without data may end after the DLC or after the space
 * behind it. "invalid" is the answer to a frame that can_bits_exact() refuses and to a line
 * that is not of this form, and the form is taken literally: no blanks at either end, no
 * tabs, no "0x", no carriage return, no lower-case s, e, d or r. A forgiving parser would
 * have the reference compared with what the parser guessed, not with the frame the reference
 * meant.
 *
 * The exit status is 0 when the input was read to its end, whatever the answers were, and 1
 * if reading or writing failed.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "can/can_bits.h"

/* The longest line of the form above: 8 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 16 characters. */
#define LINE_CAPACITY 32U

#define ID_DIGITS_MAX  8U
#define DLC_DIGITS_MAX 2U

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

/* Reads up to and including the next newline. Returns false if the input ended before a line
 * began. The last line counts with or without a newline behind it, and an empty line is a
 * line: it has to be answered too, or the answers would shift against the questions. */
static bool read_line(FILE *in, line_t *line)
{
    int c = getc(in);
    bool any = (c != EOF);

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

/* Takes one of two letters and sets `*flag` to false for the first, to true for the second. */
static bool take_either(cursor_t *cur, char if_false, char if_true, bool *flag)
{
    bool taken = true;

    if (take_char(cur, if_false)) {
        *flag = false;
    } else if (take_char(cur, if_true)) {
        *flag = true;
    } else {
        taken = false;
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

/* Takes all digits that follow and succeeds if there are between 1 and `max_digits` of them:
 * a number with too many digits is an error, not a shorter number with something behind. */
static bool take_number(cursor_t *cur, uint32_t base, size_t max_digits, uint32_t *number)
{
    uint32_t value = 0U;
    size_t digits = 0U;
    bool more = true;

    while (more && (cur->pos < cur->length)) {
        const uint32_t digit = digit_value(cur->text[cur->pos], base);

        if (digit < base) {
            /* Wraps for a run of more than eight hexadecimal digits, which is rejected below
             * on its length. */
            value = (value * base) + digit;
            digits++;
            cur->pos++;
        } else {
            more = false;
        }
    }

    *number = value;
    return (digits >= 1U) && (digits <= max_digits);
}

/* Takes the rest of the line as bytes of two hexadecimal digits each. Fails on anything else
 * and on more than `capacity` bytes. */
static bool take_bytes(cursor_t *cur, uint8_t *bytes, size_t capacity, size_t *count)
{
    size_t taken = 0U;
    bool ok = true;

    while (ok && (cur->pos < cur->length)) {
        const uint32_t high = digit_value(cur->text[cur->pos], 16U);
        const uint32_t low =
            ((cur->pos + 1U) < cur->length) ? digit_value(cur->text[cur->pos + 1U], 16U) : 16U;

        if ((high < 16U) && (low < 16U) && (taken < capacity)) {
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

static bool parse_frame(const line_t *line, can_frame_t *frame)
{
    cursor_t cur = {.text = line->text, .length = line->length, .pos = 0U};
    uint32_t id = 0U;
    uint32_t dlc = 0U;
    size_t data_bytes = 0U;
    bool extended = false;
    bool remote = false;
    bool ok = (line->length <= LINE_CAPACITY);

    ok = ok && take_number(&cur, 16U, ID_DIGITS_MAX, &id);
    ok = ok && take_char(&cur, ' ');
    ok = ok && take_either(&cur, 'S', 'E', &extended);
    ok = ok && take_char(&cur, ' ');
    ok = ok && take_either(&cur, 'D', 'R', &remote);
    ok = ok && take_char(&cur, ' ');
    ok = ok && take_number(&cur, 10U, DLC_DIGITS_MAX, &dlc);

    /* The data field, and with it the space in front of it, may be missing. take_bytes()
     * reads to the end of the line or fails, so nothing can follow the data. */
    if (ok && (cur.pos < cur.length)) {
        ok = take_char(&cur, ' ');
        ok = ok && take_bytes(&cur, frame->data, sizeof(frame->data), &data_bytes);
    }

    /* can_frame_t has no data length of its own. A line whose data does not match its DLC
     * would be padded with zeros or cut without a word; it is the line that is wrong. */
    ok = ok && (data_bytes == (remote ? 0U : (size_t)dlc));

    frame->id = id;
    frame->flags = (uint8_t)((extended ? CAN_FLAG_EXT : 0U) | (remote ? CAN_FLAG_RTR : 0U));
    frame->dlc = (uint8_t)dlc; /* at most 99: two decimal digits */

    return ok;
}

int main(void)
{
    line_t line;

    while (read_line(stdin, &line)) {
        can_frame_t frame = {.id = 0U, .flags = 0U, .dlc = 0U, .data = {0U}};
        can_bits_info_t info = {.frame_bits = 0U, .stuff_bits = 0U, .crc15 = 0U};

        if (parse_frame(&line, &frame) && can_bits_exact(&frame, &info)) {
            (void)printf("%u %u %04X\n", (unsigned int)info.frame_bits,
                         (unsigned int)info.stuff_bits, (unsigned int)info.crc15);
        } else {
            (void)puts("invalid");
        }

        /* Answer at once: a caller may write one line and wait for its answer before it
         * writes the next, and would wait forever for an answer that sits in a buffer. */
        (void)fflush(stdout);
    }

    return ((ferror(stdin) != 0) || (ferror(stdout) != 0)) ? EXIT_FAILURE : EXIT_SUCCESS;
}
