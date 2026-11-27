/* SPDX-License-Identifier: MIT */
/*
 * image_cli: lib/image behind a command line, for the comparison with the independent Python
 * implementation (tools/fcn_tools/image.py, tests/python/test_image.py).
 *
 *     image_cli pack --version M.m.p --target T [--slot-size N] PAYLOAD OUT
 *     image_cli check [--slot-size N] [--target T] [FILE...]
 *
 * pack     writes the image of the payload file PAYLOAD to OUT: the header that
 *          image_header_init() and image_header_encode() make, with the SHA-256 that
 *          lib/sha256 computes, followed by the payload. A payload that is empty or does not
 *          fit into a slot of N bytes (default 1 MiB, the host layout's slot) is refused.
 * check    validates each FILE as the content of a slot of N bytes (default 1 MiB): a flash
 *          device whose memory holds the file from address 0 and 0xFF, the erased value, after
 *          its end; a file longer than the slot is an error. The validation runs in steps
 *          (image_validate_begin, image_validate_step) and expects target T (default host).
 *          Without FILE arguments the file names are read from standard input, one per line,
 *          so that one run can check thousands of files. For each file one line of JSON:
 *
 *            {"file": "...", "status": "ok", "steps": 6, "header": {...}}
 *
 *          status is image_status_name() of the result, steps the number of
 *          image_validate_step() calls, and header the header as read
 *          (image_validator_header()), or null if it was not read completely. The header's
 *          members are magic, header_version, header_size, target, image_version, version
 *          ("M.m.p"), payload_size, flags (numbers), payload_sha256 (64 hex digits) and
 *          header_crc (a number).
 *
 * T is "host", "mps2-an385" or a number (decimal, or hexadecimal with 0x); N a number.
 *
 * Exit status: 0 when every image was valid (pack: the image was written), 1 when check refused
 * at least one, 2 for a usage error, a file that could not be read or written, or a file longer
 * than the slot (whose line then says "status": "io_error" or "too_long").
 */
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image/image.h"
#include "sha256/sha256.h"

#define DEFAULT_SLOT_SIZE (1024UL * 1024UL)
#define MAX_SLOT_SIZE     (16UL * 1024UL * 1024UL)
#define NAME_CAPACITY     4096U

#define EXIT_REFUSED 1
#define EXIT_USAGE   2

/* The slot of `check`, and the payload of `pack`. */
static uint8_t s_memory[MAX_SLOT_SIZE];

/* check: s_memory[0 .. s_slot_size) is the slot; every byte from s_dirty on is 0xFF. */
static uint32_t s_slot_size;
static uint32_t s_dirty;

static flash_status_t memory_read(void *ctx, uint32_t addr, uint8_t *dst, uint32_t len)
{
    flash_status_t status = FLASH_OK;

    (void)ctx;

    if ((len == 0U) || (addr >= s_slot_size) || (len > (s_slot_size - addr))) {
        status = FLASH_E_RANGE;
    } else {
        (void)memcpy(dst, &s_memory[addr], len);
    }

    return status;
}

static int usage(void)
{
    (void)fprintf(stderr,
                  "usage: image_cli pack --version M.m.p --target T [--slot-size N] PAYLOAD OUT\n"
                  "       image_cli check [--slot-size N] [--target T] [FILE...]\n");
    return EXIT_USAGE;
}

/* A whole number in `text` (decimal, or hexadecimal with 0x) no larger than `max`. */
static bool parse_number(const char *text, unsigned long max, unsigned long *value)
{
    char *end = NULL;
    bool ok = (text[0] >= '0') && (text[0] <= '9');

    if (ok) {
        errno = 0;
        *value = strtoul(text, &end, 0);
        ok = (errno == 0) && (end != text) && (*end == '\0') && (*value <= max);
    }

    return ok;
}

static bool parse_target(const char *text, uint32_t *target)
{
    unsigned long value = 0UL;
    bool ok = true;

    if (strcmp(text, "host") == 0) {
        *target = IMAGE_TARGET_HOST;
    } else if (strcmp(text, "mps2-an385") == 0) {
        *target = IMAGE_TARGET_MPS2_AN385;
    } else if (parse_number(text, UINT32_MAX, &value)) {
        *target = (uint32_t)value;
    } else {
        ok = false;
    }

    return ok;
}

/* "M.m.p" with M and m 0..255 and p 0..65535, decimal, nothing else. */
static bool parse_version(const char *text, uint32_t *version)
{
    unsigned long parts[3] = {0UL, 0UL, 0UL};
    static const unsigned long max[3] = {255UL, 255UL, 65535UL};
    const char *cursor = text;
    bool ok = true;

    for (size_t i = 0U; ok && (i < 3U); i++) {
        char *end = NULL;

        ok = (cursor[0] >= '0') && (cursor[0] <= '9');

        if (ok) {
            errno = 0;
            parts[i] = strtoul(cursor, &end, 10);
            ok = (errno == 0) && (parts[i] <= max[i]) && (*end == ((i < 2U) ? '.' : '\0'));
            cursor = end + 1;
        }
    }

    if (ok) {
        *version = image_version_make((uint8_t)parts[0], (uint8_t)parts[1], (uint16_t)parts[2]);
    }

    return ok;
}

/* Read a whole file into `buffer` of `capacity` bytes. Returns false if it cannot be read;
 * *too_long tells whether it was longer than the buffer. */
static bool read_file(const char *path, uint8_t *buffer, size_t capacity, size_t *length,
                      bool *too_long)
{
    FILE *file = fopen(path, "rb");
    bool ok = (file != NULL);

    *length = 0U;
    *too_long = false;

    if (ok) {
        *length = fread(buffer, 1U, capacity, file);
        ok = (ferror(file) == 0);

        if (ok && (*length == capacity)) {
            *too_long = (fgetc(file) != EOF);
        }

        (void)fclose(file);
    }

    return ok;
}

static void print_json_string(const char *text)
{
    (void)putchar('"');

    for (size_t i = 0U; text[i] != '\0'; i++) {
        const unsigned char c = (unsigned char)text[i];

        if ((c == '"') || (c == '\\')) {
            (void)printf("\\%c", c);
        } else if (c < 0x20U) {
            (void)printf("\\u%04x", c);
        } else {
            (void)putchar(c);
        }
    }

    (void)putchar('"');
}

static void print_header(const image_header_t *header)
{
    (void)printf("{\"magic\": %lu, \"header_version\": %u, \"header_size\": %u, "
                 "\"target\": %lu, \"image_version\": %lu, \"version\": \"%u.%u.%u\", "
                 "\"payload_size\": %lu, \"flags\": %lu, \"payload_sha256\": \"",
                 (unsigned long)header->magic, (unsigned int)header->header_version,
                 (unsigned int)header->header_size, (unsigned long)header->target,
                 (unsigned long)header->image_version,
                 (unsigned int)image_version_major(header->image_version),
                 (unsigned int)image_version_minor(header->image_version),
                 (unsigned int)image_version_patch(header->image_version),
                 (unsigned long)header->payload_size, (unsigned long)header->flags);

    for (size_t i = 0U; i < SHA256_DIGEST_SIZE; i++) {
        (void)printf("%02x", (unsigned int)header->payload_sha256[i]);
    }

    (void)printf("\", \"header_crc\": %lu}", (unsigned long)header->header_crc);
}

/* Validate one file as the content of the slot and print its line. Returns the exit status
 * this file asks for: 0, EXIT_REFUSED or EXIT_USAGE. */
static int check_file(const char *path, uint32_t target)
{
    static const flash_dev_t dev = {
        .geometry = {.size = 0U, .sector_size = 4096U, .page_size = 256U, .unit_size = 8U},
        .ctx = NULL,
        .read = memory_read,
        .program = NULL,
        .erase = NULL,
    };
    flash_dev_t slot = dev;
    image_validator_t validator;
    image_header_t header;
    size_t length = 0U;
    bool too_long = false;
    int exit_status = 0;
    const bool read = read_file(path, s_memory, s_slot_size, &length, &too_long);

    /* Whatever the last file left after this one's end is erased again. */
    if (s_dirty > length) {
        (void)memset(&s_memory[length], 0xFF, s_dirty - length);
    }

    s_dirty = (uint32_t)length;
    (void)printf("{\"file\": ");
    print_json_string(path);

    if (!read || too_long) {
        (void)printf(", \"status\": \"%s\", \"steps\": 0, \"header\": null}\n",
                     read ? "too_long" : "io_error");
        exit_status = EXIT_USAGE;
    } else {
        uint32_t steps = 0U;
        image_status_t status;

        slot.geometry.size = s_slot_size;
        status = image_validate_begin(&validator, &slot, 0U, s_slot_size, target);

        while (status == IMAGE_PENDING) {
            status = image_validate_step(&validator);
            steps++;
        }

        (void)printf(", \"status\": \"%s\", \"steps\": %lu, \"header\": ",
                     image_status_name(status), (unsigned long)steps);

        if (image_validator_header(&validator, &header)) {
            print_header(&header);
        } else {
            (void)printf("null");
        }

        (void)printf("}\n");
        exit_status = (status == IMAGE_OK) ? 0 : EXIT_REFUSED;
    }

    return exit_status;
}

static int worse(int a, int b)
{
    return (a > b) ? a : b;
}

static int check(int argc, char **argv)
{
    unsigned long slot_size = DEFAULT_SLOT_SIZE;
    uint32_t target = IMAGE_TARGET_HOST;
    int exit_status = 0;
    int i = 0;
    bool ok = true;

    while (ok && (i < argc) && (strncmp(argv[i], "--", 2U) == 0)) {
        if ((strcmp(argv[i], "--slot-size") == 0) && ((i + 1) < argc)) {
            ok = parse_number(argv[i + 1], MAX_SLOT_SIZE, &slot_size) &&
                 (slot_size > IMAGE_HEADER_SIZE);
        } else if ((strcmp(argv[i], "--target") == 0) && ((i + 1) < argc)) {
            ok = parse_target(argv[i + 1], &target);
        } else {
            ok = false;
        }

        i += 2;
    }

    if (!ok) {
        exit_status = usage();
    } else {
        s_slot_size = (uint32_t)slot_size;
        s_dirty = 0U; /* main() erased all of s_memory */

        if (i < argc) {
            for (; i < argc; i++) {
                exit_status = worse(exit_status, check_file(argv[i], target));
            }
        } else {
            char name[NAME_CAPACITY];

            while (fgets(name, (int)sizeof(name), stdin) != NULL) {
                name[strcspn(name, "\n")] = '\0';
                exit_status = worse(exit_status, check_file(name, target));
            }
        }
    }

    return exit_status;
}

static bool write_image(const char *path, const uint8_t header[IMAGE_HEADER_SIZE],
                        const uint8_t *payload, size_t length)
{
    FILE *file = fopen(path, "wb");
    bool ok = (file != NULL);

    if (ok) {
        ok = (fwrite(header, 1U, IMAGE_HEADER_SIZE, file) == IMAGE_HEADER_SIZE) &&
             (fwrite(payload, 1U, length, file) == length);
        ok = (fclose(file) == 0) && ok;
    }

    return ok;
}

static int pack(int argc, char **argv)
{
    unsigned long slot_size = DEFAULT_SLOT_SIZE;
    uint32_t target = 0U;
    uint32_t version = 0U;
    bool have_target = false;
    bool have_version = false;
    int i = 0;
    bool ok = true;

    while (ok && (i < argc) && (strncmp(argv[i], "--", 2U) == 0)) {
        if ((strcmp(argv[i], "--slot-size") == 0) && ((i + 1) < argc)) {
            ok = parse_number(argv[i + 1], MAX_SLOT_SIZE, &slot_size) &&
                 (slot_size > IMAGE_HEADER_SIZE);
        } else if ((strcmp(argv[i], "--target") == 0) && ((i + 1) < argc)) {
            ok = parse_target(argv[i + 1], &target);
            have_target = true;
        } else if ((strcmp(argv[i], "--version") == 0) && ((i + 1) < argc)) {
            ok = parse_version(argv[i + 1], &version);
            have_version = true;
        } else {
            ok = false;
        }

        i += 2;
    }

    int exit_status = 0;

    if (!ok || !have_target || !have_version || ((argc - i) != 2)) {
        exit_status = usage();
    } else {
        const size_t capacity = (size_t)slot_size - IMAGE_HEADER_SIZE;
        size_t length = 0U;
        bool too_long = false;
        uint8_t digest[SHA256_DIGEST_SIZE];
        uint8_t bytes[IMAGE_HEADER_SIZE];
        image_header_t header;

        if (!read_file(argv[i], s_memory, capacity, &length, &too_long)) {
            (void)fprintf(stderr, "image_cli: cannot read %s\n", argv[i]);
            exit_status = EXIT_USAGE;
        } else if (too_long || (length == 0U)) {
            (void)fprintf(stderr, "image_cli: the payload must be 1 to %lu bytes\n",
                          (unsigned long)capacity);
            exit_status = EXIT_USAGE;
        } else {
            (void)sha256_compute(s_memory, length, digest);
            (void)image_header_init(&header, target, version, (uint32_t)length, digest);
            (void)image_header_encode(&header, bytes);

            if (!write_image(argv[i + 1], bytes, s_memory, length)) {
                (void)fprintf(stderr, "image_cli: cannot write %s\n", argv[i + 1]);
                exit_status = EXIT_USAGE;
            }
        }
    }

    return exit_status;
}

int main(int argc, char **argv)
{
    int exit_status = EXIT_USAGE;

    (void)memset(s_memory, 0xFF, sizeof(s_memory));

    if ((argc >= 2) && (strcmp(argv[1], "pack") == 0)) {
        exit_status = pack(argc - 2, &argv[2]);
    } else if ((argc >= 2) && (strcmp(argv[1], "check") == 0)) {
        exit_status = check(argc - 2, &argv[2]);
    } else {
        exit_status = usage();
    }

    return exit_status;
}
