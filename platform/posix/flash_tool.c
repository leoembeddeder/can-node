/* SPDX-License-Identifier: MIT */
/*
 * fcn_flash, the flash tool of the host build: it makes, updates and reads a flash file with
 * the same code the node and the bootloader run (lib/flash's model behind posix_flash.c,
 * lib/image, lib/boot, lib/bootimg).
 *
 *   fcn_flash factory --flash-file F [--no-wait] [--flash-cut N[:SEED]] IMAGE
 *   fcn_flash stage   --flash-file F [--no-wait] [--flash-cut N[:SEED]] [--address A]
 *                     [--chunk N] IMAGE
 *   fcn_flash info    --flash-file F
 *   fcn_flash --version   the version and the build id of the tree it was built from
 *                         (build_id.h), so that a measurement can check the tool it ran
 *
 * IMAGE is an image file as `python -m fcn_tools.image pack` writes it: the 256-byte header and
 * the payload.
 *
 * factory  A new flash file, as it leaves the factory: F must not exist. The file is created
 *          erased, IMAGE is programmed into slot A page by page (no erase: the slot is erased
 *          already), and lib/boot's select runs once, as the bootloader's first start would: it
 *          validates slot A and writes the ADOPT record, so that slot A holds the CONFIRMED
 *          image. IMAGE is validated in memory first (lib/image, as for slot A); one that does
 *          not validate is refused and no file is made.
 * stage    An update, exactly the calls phase 5's UDS download makes in the node, in the order
 *          it makes them: boot_slot_by_address() for the address the tester names (--address;
 *          default the start of the slot that is not CONFIRMED, from boot_describe()),
 *          boot_writer_begin() with the image size (RequestDownload), boot_writer_erase_step()
 *          until it is done (one sector per call), boot_writer_write() with IMAGE in pieces of
 *          --chunk bytes at their offsets (TransferData; default 1024), boot_writer_finish_step()
 *          until the image is validated and committed as the TRIAL (RequestTransferExit and the
 *          check routine). The next start of fcn_boot boots it as attempt 1.
 * info     The flash as JSON on stdout: the layout, the metadata as lib/boot describes them
 *          (records, the newest one, each slot's state, version and id, the attempts of a
 *          trial) and each slot as lib/image validates it (the header, the verdict, the id the
 *          validator gives, and whether it is the id the metadata record). Reads only.
 *
 * factory and stage print one JSON object on stdout as well: what they did and the flash's
 * counts (posix_flash.h). --no-wait leaves out the modelled times of programs and erases (a
 * tool that prepares a test need not wait 45 ms per sector); without it every operation waits
 * as on the node, by sleeping. --flash-cut cuts the supply in the N-th program or erase of the
 * run, which then ends as a loss of supply. The tool holds the flash file's lock while it works
 * and releases it itself at every end, the cut's included (posix_flash.c, THE LOCK AND THE END
 * OF A RUN): the next program can open the file as soon as this one's exit has been seen.
 *
 * EXIT STATUS: 0 done; 1 a usage or start-up error (the flash file cannot be opened, is in use,
 * is missing for stage or info, exists for factory; IMAGE cannot be read); 2 refused: lib/image
 * or lib/boot said no (the image does not validate or does not fit, the address names no slot
 * or the CONFIRMED one, a trial is pending), with the reason in the JSON and on stderr; 12 a
 * power cut fired (PLAT_RESET_POWER_LOSS).
 */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "boot_host.h"
#include "build_id.h"
#include "posix_flash.h"

#ifndef FCN_VERSION
#define FCN_VERSION "unknown"
#endif

#define EXIT_REFUSED  2
#define DEFAULT_CHUNK 1024U

typedef enum { COMMAND_FACTORY, COMMAND_STAGE, COMMAND_INFO } command_t;

typedef struct {
    command_t command;
    const char *flash_path;
    const char *image_path;
    bool no_wait;
    bool cut_given;
    posix_flash_cut_t cut;
    bool address_given;
    uint32_t address;
    uint32_t chunk;
} tool_options_t;

static void print_usage(const char *program)
{
    (void)printf("usage: %s factory --flash-file F [--no-wait] [--flash-cut N[:SEED]] IMAGE\n"
                 "       %s stage   --flash-file F [--no-wait] [--flash-cut N[:SEED]]\n"
                 "                  [--address A] [--chunk N] IMAGE\n"
                 "       %s info    --flash-file F\n"
                 "       %s --version (the version and the build id) or --help\n"
                 "exit status: 0 done, 1 usage or start-up error, 2 refused by lib/image or\n"
                 "lib/boot, 12 a power cut fired\n",
                 program, program, program, program);
}

/* A number in C notation (decimal, or hexadecimal with 0x) of at most `max`. */
static bool parse_number(const char *text, unsigned long max, uint32_t *out)
{
    char *end = NULL;
    bool ok = false;

    if ((text[0] >= '0') && (text[0] <= '9')) {
        errno = 0;
        const unsigned long value = strtoul(text, &end, 0);

        if ((errno == 0) && (*end == '\0') && (value <= max)) {
            *out = (uint32_t)value;
            ok = true;
        }
    }

    return ok;
}

static int parse_options(int argc, char **argv, tool_options_t *options)
{
    enum { OPT_FLASH = 1, OPT_NO_WAIT, OPT_CUT, OPT_ADDRESS, OPT_CHUNK, OPT_HELP };
    static const struct option long_options[] = {{"flash-file", required_argument, NULL, OPT_FLASH},
                                                 {"no-wait", no_argument, NULL, OPT_NO_WAIT},
                                                 {"flash-cut", required_argument, NULL, OPT_CUT},
                                                 {"address", required_argument, NULL, OPT_ADDRESS},
                                                 {"chunk", required_argument, NULL, OPT_CHUNK},
                                                 {"help", no_argument, NULL, OPT_HELP},
                                                 {NULL, 0, NULL, 0}};
    int result = 0;
    int option;

    if ((argc < 2) || (strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return (argc < 2) ? 2 : 1;
    }

    if ((argc == 2) && (strcmp(argv[1], "--version") == 0)) {
        (void)printf("fcn_flash %s build %s\n", FCN_VERSION, fcn_build_id);
        return 1;
    }

    if (strcmp(argv[1], "factory") == 0) {
        options->command = COMMAND_FACTORY;
    } else if (strcmp(argv[1], "stage") == 0) {
        options->command = COMMAND_STAGE;
    } else if (strcmp(argv[1], "info") == 0) {
        options->command = COMMAND_INFO;
    } else {
        (void)fprintf(stderr, "fcn_flash: unknown command %s\n", argv[1]);
        return 2;
    }

    optind = 2;

    while ((result == 0) && ((option = getopt_long(argc, argv, "", long_options, NULL)) != -1)) {
        char message[200];

        switch (option) {
        case OPT_FLASH:
            options->flash_path = optarg;
            break;
        case OPT_NO_WAIT:
            options->no_wait = true;
            break;
        case OPT_CUT:
            if (posix_flash_parse_cut(optarg, &options->cut, message, sizeof(message))) {
                options->cut_given = true;
            } else {
                (void)fprintf(stderr, "fcn_flash: invalid --flash-cut: %s\n", message);
                result = 2;
            }
            break;
        case OPT_ADDRESS:
            options->address_given = parse_number(optarg, 0xFFFFFFFFUL, &options->address);
            if (!options->address_given) {
                (void)fprintf(stderr, "fcn_flash: invalid --address: %s\n", optarg);
                result = 2;
            }
            break;
        case OPT_CHUNK:
            if (!parse_number(optarg, 0x100000UL, &options->chunk) || (options->chunk == 0U)) {
                (void)fprintf(stderr, "fcn_flash: invalid --chunk: %s (1 to 1048576)\n", optarg);
                result = 2;
            }
            break;
        case OPT_HELP:
            print_usage(argv[0]);
            result = 1;
            break;
        default:
            print_usage(argv[0]);
            result = 2;
            break;
        }
    }

    if (result == 0) {
        const bool wants_image = (options->command != COMMAND_INFO);
        const int left = argc - optind;

        if (options->flash_path == NULL) {
            (void)fprintf(stderr, "fcn_flash: --flash-file is required\n");
            result = 2;
        } else if (wants_image && (left == 1)) {
            options->image_path = argv[optind];
        } else if (!wants_image && (left == 0)) {
            /* info takes no image */
        } else {
            (void)fprintf(stderr, "fcn_flash: %s\n",
                          wants_image ? "one IMAGE file is required" : "info takes no IMAGE");
            result = 2;
        }
    }

    return result;
}

/* ---------------------------------------------------------------------------- the image */

typedef struct {
    uint8_t *bytes; /* the slot: the file, then 0xFF */
    uint32_t size;  /* bytes of the file */
} image_file_t;

/* Read the image file into a buffer of a whole slot, erased beyond the file. */
static bool read_image(const char *path, image_file_t *out, char *error, size_t error_len)
{
    const uint32_t slot_size = plat_flash_layout()->slot[0].size;
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        (void)snprintf(error, error_len, "cannot read %s: %s", path, strerror(errno));
        return false;
    }

    out->bytes = malloc(slot_size + 1U);

    if (out->bytes == NULL) {
        (void)fclose(file);
        (void)snprintf(error, error_len, "out of memory");
        return false;
    }

    (void)memset(out->bytes, 0xFF, slot_size + 1U);
    const size_t got = fread(out->bytes, 1U, (size_t)slot_size + 1U, file);
    (void)fclose(file);

    if (got > slot_size) {
        (void)snprintf(error, error_len, "%s is larger than a slot (%lu bytes)", path,
                       (unsigned long)slot_size);
        free(out->bytes);
        return false;
    }

    out->bytes[slot_size] = 0xFFU;
    out->size = (uint32_t)got;
    return true;
}

/* A read-only flash device over a buffer: lib/image validates an image file in memory. */
typedef struct {
    const uint8_t *bytes;
    uint32_t size;
} ram_dev_t;

static flash_status_t ram_read(void *ctx, uint32_t addr, uint8_t *dst, uint32_t len)
{
    const ram_dev_t *ram = ctx;
    flash_status_t status = FLASH_E_RANGE;

    if ((len != 0U) && (addr < ram->size) && (len <= (ram->size - addr))) {
        (void)memcpy(dst, &ram->bytes[addr], len);
        status = FLASH_OK;
    }

    return status;
}

static image_status_t validate_in_memory(const image_file_t *image, image_header_t *header)
{
    const uint32_t slot_size = plat_flash_layout()->slot[0].size;
    ram_dev_t ram = {.bytes = image->bytes, .size = slot_size};
    flash_dev_t dev;

    (void)memset(&dev, 0, sizeof(dev));
    dev.geometry = plat_flash_layout()->geometry;
    dev.geometry.size = slot_size;
    dev.ctx = &ram;
    dev.read = ram_read;

    return image_validate(&dev, 0U, slot_size, IMAGE_TARGET_HOST, header);
}

/* ------------------------------------------------------------------------------ the JSON */

static void put_hex(const char *key, const uint8_t *bytes, size_t len, const char *separator)
{
    (void)printf("\"%s\": \"", key);

    for (size_t i = 0U; i < len; i++) {
        (void)printf("%02x", (unsigned int)bytes[i]);
    }

    (void)printf("\"%s", separator);
}

static void put_version(const char *key, uint32_t version, const char *separator)
{
    char text[BOOT_HOST_VERSION_TEXT];

    (void)printf("\"%s\": \"%s\", \"%s_code\": %lu%s", key, boot_host_version_text(version, text),
                 key, (unsigned long)version, separator);
}

static void put_flash_counts(void)
{
    posix_flash_info_t f;

    posix_flash_get_info(&f);
    (void)printf("\"flash\": {\"created\": %s, \"reads\": %llu, \"programs\": %llu, "
                 "\"units_programmed\": %llu, \"erases\": %llu, \"refused\": %llu, "
                 "\"busy_ns\": %llu, \"erases_meta\": %lu, \"erases_slot_a\": %lu, "
                 "\"erases_slot_b\": %lu, \"max_sector_erases\": %lu}",
                 f.created ? "true" : "false", (unsigned long long)f.reads,
                 (unsigned long long)f.programs, (unsigned long long)f.units_programmed,
                 (unsigned long long)f.erases, (unsigned long long)f.refused,
                 (unsigned long long)f.busy_ns, (unsigned long)f.erases_meta,
                 (unsigned long)f.erases_slot[0], (unsigned long)f.erases_slot[1],
                 (unsigned long)f.max_sector_erases);
}

static void put_header(const image_header_t *h, const char *separator)
{
    (void)printf("\"header\": {\"magic\": %lu, \"header_version\": %u, \"header_size\": %u, "
                 "\"target\": %lu, ",
                 (unsigned long)h->magic, (unsigned int)h->header_version,
                 (unsigned int)h->header_size, (unsigned long)h->target);
    /* The members of image_cli and fcn_tools.image (Header.as_json()). */
    char text[BOOT_HOST_VERSION_TEXT];

    (void)printf("\"image_version\": %lu, \"version\": \"%s\", ", (unsigned long)h->image_version,
                 boot_host_version_text(h->image_version, text));
    (void)printf("\"payload_size\": %lu, \"flags\": %lu, ", (unsigned long)h->payload_size,
                 (unsigned long)h->flags);
    put_hex("payload_sha256", h->payload_sha256, sizeof(h->payload_sha256), ", ");
    (void)printf("\"header_crc\": %lu}%s", (unsigned long)h->header_crc, separator);
}

/* A refusal: in the JSON (the caller has written the object's start) and on stderr. */
static int refuse(const char *command, const char *reason, const char *detail)
{
    (void)printf("{\"command\": \"%s\", \"status\": \"refused\", \"reason\": \"%s\", "
                 "\"detail\": \"%s\"}\n",
                 command, reason, detail);
    (void)fprintf(stderr, "fcn_flash %s: refused: %s (%s)\n", command, reason, detail);
    return EXIT_REFUSED;
}

/* ----------------------------------------------------------------------------- the flash */

static bool open_flash(const tool_options_t *options, bool must_exist, char *error,
                       size_t error_len)
{
    static const nor_timing_t no_wait = {.page_program_ns = 0U, .sector_erase_ns = 0U};
    struct stat info;
    const bool exists = (stat(options->flash_path, &info) == 0);

    if (must_exist && !exists) {
        (void)snprintf(error, error_len, "no flash file %s", options->flash_path);
        return false;
    }

    if (!must_exist && exists) {
        (void)snprintf(error, error_len,
                       "%s exists: factory makes a new flash file (remove it first)",
                       options->flash_path);
        return false;
    }

    if (!posix_flash_open(options->flash_path, options->no_wait ? &no_wait : NULL, error,
                          error_len)) {
        return false;
    }

    if (options->cut_given) {
        posix_flash_arm_cut(&options->cut);
    }

    return true;
}

static boot_status_t make_boot(boot_t *b, bootimg_t *v)
{
    boot_validator_t validator;
    boot_config_t config;

    (void)bootimg_init(v, IMAGE_TARGET_HOST, &validator);
    boot_host_config(&config, &validator);
    return boot_init(b, &config);
}

/* --------------------------------------------------------------------------- the commands */

static int command_factory(const tool_options_t *options, const image_file_t *image)
{
    static boot_t s_boot;
    static bootimg_t s_validator;
    const plat_flash_layout_t *layout = plat_flash_layout();
    const uint32_t page = layout->geometry.page_size;
    const uint32_t unit = layout->geometry.unit_size;
    image_header_t header;
    boot_selection_t sel;
    char error[512];
    uint8_t id[BOOT_IMAGE_ID_SIZE];

    (void)memset(&sel, 0, sizeof(sel));
    const image_status_t valid = validate_in_memory(image, &header);

    if (valid != IMAGE_OK) {
        return refuse("factory", "the image does not validate", image_status_name(valid));
    }

    if (!open_flash(options, false, error, sizeof(error))) {
        (void)fprintf(stderr, "fcn_flash: %s\n", error);
        return EXIT_FAILURE;
    }

    const flash_dev_t *dev = plat_flash_device();
    const uint32_t length = IMAGE_PAYLOAD_OFFSET + header.payload_size;
    /* Whole units: the bytes after the image in its last unit are 0xFF, as erased. */
    const uint32_t padded = ((length + unit - 1U) / unit) * unit;

    for (uint32_t at = 0U; at < padded; at += page) {
        const uint32_t len = ((padded - at) < page) ? (padded - at) : page;

        if (dev->program(dev->ctx, layout->slot[0].offset + at, &image->bytes[at], len) !=
            FLASH_OK) {
            posix_flash_close();
            (void)fprintf(stderr, "fcn_flash: a program of slot A failed at offset %lu\n",
                          (unsigned long)at);
            return EXIT_FAILURE;
        }
    }

    /* The bootloader's first start: the factory state adopts slot A. */
    boot_status_t status = make_boot(&s_boot, &s_validator);

    if (status == BOOT_OK) {
        status = boot_select(&s_boot, &sel);
    }

    if ((status != BOOT_OK) || (sel.event != BOOT_EVENT_ADOPT) || (sel.slot != 0U)) {
        posix_flash_close();
        return refuse("factory", "the bootloader's select did not adopt slot A",
                      boot_status_name(status));
    }

    (void)bootimg_image_id(&header, id);
    (void)printf("{\"command\": \"factory\", \"status\": \"ok\", \"slot\": \"A\", ");
    (void)printf("\"image_size\": %lu, ", (unsigned long)length);
    put_version("image_version", header.image_version, ", ");
    put_hex("id", id, sizeof(id), ", ");
    (void)printf("\"event\": \"%s\", ", boot_event_name(sel.event));
    put_flash_counts();
    (void)printf("}\n");
    posix_flash_close();
    return EXIT_SUCCESS;
}

static int command_stage(const tool_options_t *options, const image_file_t *image)
{
    static boot_t s_boot;
    static bootimg_t s_validator;
    static boot_writer_t s_writer;
    const plat_flash_layout_t *layout = plat_flash_layout();
    const uint32_t chunk = (options->chunk != 0U) ? options->chunk : DEFAULT_CHUNK;
    boot_description_t d;
    char error[512];
    uint8_t slot = BOOT_NO_SLOT;
    uint32_t erase_steps = 0U;
    uint32_t writes = 0U;
    uint32_t finish_steps = 0U;

    if (!open_flash(options, true, error, sizeof(error))) {
        (void)fprintf(stderr, "fcn_flash: %s\n", error);
        return EXIT_FAILURE;
    }

    boot_status_t status = make_boot(&s_boot, &s_validator);

    if (status == BOOT_OK) {
        status = boot_describe(&s_boot, &d);
    }

    if (status != BOOT_OK) {
        posix_flash_close();
        return refuse("stage", "lib/boot cannot read the metadata", boot_status_name(status));
    }

    /* The tester names the slot by its address; by default the one that is not CONFIRMED. */
    uint32_t address = options->address;

    if (!options->address_given) {
        const uint32_t target = (d.active_slot == 1U) ? 0U : 1U;

        address = layout->slot[target].offset;
    }

    status = boot_slot_by_address(&s_boot, address, &slot);

    if (status == BOOT_OK) {
        status = boot_writer_begin(&s_writer, &s_boot, slot, image->size);
    }

    while (status == BOOT_OK) {
        status = boot_writer_erase_step(&s_writer);
        erase_steps++;

        if (status == BOOT_BUSY) {
            status = BOOT_OK;
        } else {
            break;
        }
    }

    for (uint32_t at = 0U; (status == BOOT_OK) && (at < image->size); at += chunk) {
        const uint32_t len = ((image->size - at) < chunk) ? (image->size - at) : chunk;

        status = boot_writer_write(&s_writer, at, &image->bytes[at], len);
        writes++;
    }

    status = (status == BOOT_OK) ? BOOT_BUSY : status;

    while (status == BOOT_BUSY) {
        status = boot_writer_finish_step(&s_writer);
        finish_steps++;
    }

    if (status != BOOT_OK) {
        boot_writer_progress_t progress;
        char detail[128];

        boot_writer_progress(&s_writer, &progress);
        (void)snprintf(detail, sizeof(detail), "%s at address 0x%06lx, writer %s",
                       boot_status_name(status), (unsigned long)address,
                       boot_writer_phase_name(progress.phase));
        posix_flash_close();
        return refuse("stage", "lib/boot's slot writer refused or failed", detail);
    }

    (void)printf("{\"command\": \"stage\", \"status\": \"ok\", \"slot\": \"%s\", "
                 "\"address\": %lu, \"image_size\": %lu, \"chunk\": %lu, \"erase_steps\": %lu, "
                 "\"writes\": %lu, \"finish_steps\": %lu, ",
                 boot_host_slot_name(slot), (unsigned long)address, (unsigned long)image->size,
                 (unsigned long)chunk, (unsigned long)erase_steps, (unsigned long)writes,
                 (unsigned long)finish_steps);
    put_flash_counts();
    (void)printf("}\n");
    posix_flash_close();
    return EXIT_SUCCESS;
}

static void put_slot(boot_t *b, const boot_description_t *d, uint8_t s)
{
    const plat_flash_layout_t *layout = plat_flash_layout();
    const boot_slot_info_t *info = &d->slot[s];
    image_header_t header;
    const image_status_t verdict = image_validate(plat_flash_device(), layout->slot[s].offset,
                                                  layout->slot[s].size, IMAGE_TARGET_HOST, &header);
    const bool recorded = (info->state != BOOT_SLOT_EMPTY) && (info->state != BOOT_SLOT_WRITING);

    (void)b;
    (void)printf("    {\"name\": \"%s\", \"offset\": %lu, \"size\": %lu, \"state\": \"%s\", ",
                 boot_host_slot_name(s), (unsigned long)info->addr, (unsigned long)info->size,
                 boot_slot_state_name(info->state));

    if (recorded) {
        put_version("recorded_version", info->version, ", ");
        put_hex("recorded_id", info->id, sizeof(info->id), ", ");
    } else {
        (void)printf("\"recorded_version\": null, \"recorded_version_code\": null, "
                     "\"recorded_id\": null, ");
    }

    (void)printf("\"validation\": \"%s\", ", image_status_name(verdict));

    if (verdict == IMAGE_OK) {
        uint8_t id[BOOT_IMAGE_ID_SIZE];

        (void)bootimg_image_id(&header, id);
        put_header(&header, ", ");
        put_hex("id", id, sizeof(id), ", ");
        (void)printf("\"id_matches_record\": %s}",
                     (recorded && (memcmp(id, info->id, sizeof(id)) == 0)) ? "true" : "false");
    } else {
        (void)printf("\"header\": null, \"id\": null, \"id_matches_record\": false}");
    }
}

static int command_info(const tool_options_t *options)
{
    static boot_t s_boot;
    static bootimg_t s_validator;
    const plat_flash_layout_t *layout = plat_flash_layout();
    boot_description_t d;
    char error[512];

    if (!open_flash(options, true, error, sizeof(error))) {
        (void)fprintf(stderr, "fcn_flash: %s\n", error);
        return EXIT_FAILURE;
    }

    boot_status_t status = make_boot(&s_boot, &s_validator);

    if (status == BOOT_OK) {
        status = boot_describe(&s_boot, &d);
    }

    if (status != BOOT_OK) {
        posix_flash_close();
        return refuse("info", "lib/boot cannot read the metadata", boot_status_name(status));
    }

    (void)printf("{\n  \"schema\": \"fcn.flash_info/1\",\n");
    (void)printf("  \"layout\": {\"size\": %lu, \"sector_size\": %lu, \"page_size\": %lu, "
                 "\"unit_size\": %lu, \"meta_offset\": %lu, \"meta_size\": %lu},\n",
                 (unsigned long)layout->geometry.size, (unsigned long)layout->geometry.sector_size,
                 (unsigned long)layout->geometry.page_size,
                 (unsigned long)layout->geometry.unit_size, (unsigned long)layout->meta.offset,
                 (unsigned long)layout->meta.size);
    (void)printf("  \"metadata\": {\"have_record\": %s, ", d.have_record ? "true" : "false");

    if (d.have_record) {
        (void)printf("\"seq\": %lu, \"event\": \"%s\", \"event_slot\": \"%s\", "
                     "\"record_sector\": %lu, \"record_index\": %lu, ",
                     (unsigned long)d.seq, boot_event_name(d.event),
                     boot_host_slot_name(d.event_slot), (unsigned long)d.record_sector,
                     (unsigned long)d.record_index);
    } else {
        (void)printf("\"seq\": null, \"event\": null, \"event_slot\": null, "
                     "\"record_sector\": null, \"record_index\": null, ");
    }

    (void)printf("\"active_slot\": \"%s\", \"trial_slot\": \"%s\", \"attempts_used\": %u, "
                 "\"attempts_left\": %u,\n    \"records_valid\": %lu, \"entries_used\": %lu, "
                 "\"entries_total\": %lu, \"entries_per_sector\": %lu, \"entry_stride\": %lu, "
                 "\"field_size\": %lu, \"record_body\": %lu, \"next_write_erases\": %s, "
                 "\"next_sector\": %lu, \"next_index\": %lu},\n",
                 boot_host_slot_name(d.active_slot), boot_host_slot_name(d.trial_slot),
                 (unsigned int)d.attempts_used, (unsigned int)d.attempts_left,
                 (unsigned long)d.records_valid, (unsigned long)d.entries_used,
                 (unsigned long)d.entries_total, (unsigned long)d.entries_per_sector,
                 (unsigned long)d.entry_stride, (unsigned long)d.field_size,
                 (unsigned long)d.record_body, d.next_write_erases ? "true" : "false",
                 (unsigned long)d.next_sector, (unsigned long)d.next_index);
    (void)printf("  \"slots\": [\n");

    for (uint8_t s = 0U; s < BOOT_SLOTS; s++) {
        put_slot(&s_boot, &d, s);
        (void)printf("%s\n", (s + 1U < BOOT_SLOTS) ? "," : "");
    }

    (void)printf("  ],\n  ");
    put_flash_counts();
    (void)printf("\n}\n");
    posix_flash_close();
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    tool_options_t options;
    image_file_t image = {.bytes = NULL, .size = 0U};
    char error[512];
    int status;

    (void)memset(&options, 0, sizeof(options));
    status = parse_options(argc, argv, &options);

    if (status != 0) {
        return (status == 1) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if ((options.image_path != NULL) &&
        !read_image(options.image_path, &image, error, sizeof(error))) {
        (void)fprintf(stderr, "fcn_flash: %s\n", error);
        return EXIT_FAILURE;
    }

    switch (options.command) {
    case COMMAND_FACTORY:
        status = command_factory(&options, &image);
        break;
    case COMMAND_STAGE:
        status = command_stage(&options, &image);
        break;
    default:
        status = command_info(&options);
        break;
    }

    free(image.bytes);
    return status;
}
