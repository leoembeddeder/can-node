/* SPDX-License-Identifier: MIT */
/*
 * fcn_boot, the bootloader of the host build: it decides which image to start, tells the image
 * what it decided, and starts it in its own process.
 *
 *   fcn_boot --flash-file F [--retained-file R] [--flash-cut N[:SEED]] [--log-file L]
 *            [--log-level LEVEL] [-- node options...]
 *
 * WHAT IT DOES, in the order a bootloader on a microcontroller does it:
 *   1. Reads the retained cell (posix_retained_open_boot()): the reset cause, which it leaves
 *      in the cell for the image, and a hand-off an earlier start left in it, whose words the
 *      open zeroes.
 *   2. Opens the flash (posix_flash.c; created erased if missing, as an empty part) and arms a
 *      cut of its supply if --flash-cut asks for one.
 *   3. Runs lib/boot's select with lib/image's validation behind it (lib/bootimg): it validates
 *      the slots it has to decide about, counts an attempt before it starts a trial, and writes
 *      a rollback, a rejection, a fallback or the adoption of the factory image when that is
 *      the state change to make (docs/update.md).
 *   4. Prepares the "jump": copies the payload of the slot it chose (the bytes after the
 *      256-byte image header) into a memory file (memfd_create), checks the SHA-256 of what it
 *      copied against the header, seals the file, and makes the image's command line: the node
 *      options given after "--", followed by --flash-file F and --retained-file R.
 *   5. Writes the HAND-OFF into the cell's reserved words (plat_retained.h): slot, trial or
 *      confirmed, attempt, image version, the state change it made. It does so only now, when
 *      nothing but the exec is left to fail, so that an image it does not start leaves no
 *      hand-off for a later start without the bootloader.
 *   6. Releases its locks of the flash file and the cell (posix_flash_unlock(),
 *      posix_retained_unlock()) and fexecve()s the memory file. The process keeps its id: the
 *      test harness that started fcn_boot sees one process, the node, and its exit status. The
 *      image opens and locks both files itself. The exec alone would not free them for it in
 *      time: a lock belongs to the open file description, which the old process image's
 *      mappings refer to as well, and those can outlast the exec (while another process holds
 *      a reference to the old memory map: posix_retained.c, THE LOCK BELONGS TO THE OPEN FILE
 *      DESCRIPTION). So the locks go first, by flock(LOCK_UN), and the descriptors (close-on-
 *      exec) and the cell's mapping stay for a failed exec (the flash's mapping is a private
 *      one from the unlock on, posix_flash_unlock()): then fcn_boot takes the cell's lock again
 *      (posix_retained_relock()) and zeroes the hand-off's words through the mapping
 *      (posix_retained_clear_handoff()) before the run ends with 42. If another process locked
 *      the cell in between, fcn_boot writes nothing into it and says that the hand-off could
 *      not be taken back (plat_retained.h, THE REMAINING WINDOW).
 *      Phase 7's bootloader on the Cortex-M3 sets VTOR and the main stack pointer from the
 *      slot's vector table and branches to its reset handler instead.
 *
 * EXIT STATUS (only when it does not jump): 0 after --help or --version; 1 for a usage or
 * start-up error; 12 when a power cut armed with --flash-cut fired (posix_flash.c ends the
 * run as a loss of supply: the cell is destroyed); FCN_BOOT_EXIT_NOTHING_BOOTABLE (40) when no
 * slot may be started; FCN_BOOT_EXIT_FLASH (41) when the flash could not be read (lib/boot
 * then starts nothing: a device it cannot read cannot be trusted); FCN_BOOT_EXIT_JUMP (42)
 * when the chosen image could not be started. After a jump the status is the image's.
 * None of these ends leaves a hand-off in the cell: 0 and 1 end before the cell is opened,
 * which leaves it as it was, or (a flash file that cannot be used) after the open, which
 * zeroed an earlier hand-off; 12, 40 and 41 end in or before select, before the hand-off is
 * written; 42 ends before it is written (the header read back, the copy, its digest, the
 * seal, the image's arguments) or after a failed exec, which zeroes its words again. Of its own
 * ends, only a kill between the write and the exec leaves one behind; and another program that
 * opens the cell between the unlock and the image's own open takes it as its own
 * (plat_retained.h, THE REMAINING WINDOW).
 * Every one of these ends releases the locks of the flash file and the cell itself, as the
 * jump does, so that the next start finds both free at once, whatever still refers to this
 * process's memory map after it has ended (posix_flash.c, THE LOCK AND THE END OF A RUN): 0 and
 * 1 before a file is open, or after the close of the one that was; 12 by the two unlocks of
 * posix_flash.c's end without a kernel; 40, 41 and 42 by posix_flash_close() and
 * posix_retained_close() at the end of main(). Only a kill leaves them to the kernel.
 *
 * THE LOG. Lines in lib/log's format, stamped with CLOCK_MONOTONIC like the node's, appended to
 * --log-file (default stderr): the start (reset cause, cell, the build id of build_id.h, which
 * --version prints as well, and the flash), the metadata as found, the decision, the state
 * change, the jump. Errors also go to stderr. The test harness gives the bootloader a log of
 * its own: the node truncates its --log-file when it starts.
 *
 * TIMING. Flash operations wait the modelled times with clock_nanosleep (posix_flash.c). A
 * select that boots the confirmed image reads only (no waits); a trial costs one program of
 * an attempt unit (0.4 ms), a rollback one record (two programs), and the first record of a
 * metadata sector one erase (45 ms) more. Validation reads the slots without waiting: a
 * 1 MiB slot is about 16,400 steps of lib/image.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "boot_host.h"
#include "build_id.h"
#include "posix_flash.h"
#include "posix_retained.h"

#ifndef FCN_VERSION
#define FCN_VERSION "unknown"
#endif

/* The payload goes from the flash into the memory file in pieces of this size. */
#define COPY_CHUNK 4096U

/* The options fcn_boot adds to the node options. */
#define ADDED_OPTIONS 4

static const char TAG[] = "boot";

/* The two options the bootloader adds to the image's command line (argv is not const). */
static char s_flash_option[] = "--flash-file";
static char s_retained_option[] = "--retained-file";

typedef struct {
    char *flash_path;    /* from argv, as the image's argv takes it */
    char *retained_path; /* NULL: no cell, and so no hand-off */
    char *log_path;      /* NULL: stderr */
    log_level_t log_level;
    bool cut_given;
    posix_flash_cut_t cut;
    int node_argc; /* the options after "--" */
    char **node_argv;
} boot_options_t;

static void print_usage(const char *program)
{
    (void)printf(
        "usage: %s --flash-file PATH [options] [-- node options...]\n"
        "  --flash-file PATH      the flash (created erased if missing); required\n"
        "  --retained-file PATH   the retained-RAM cell: reset cause in, hand-off out\n"
        "                         (default: none, the image gets no hand-off)\n"
        "  --flash-cut N[:SEED]   cut the supply in the N-th flash program or erase of the\n"
        "                         bootloader (1 to %u), torn as SEED says (default 0)\n"
        "  --log-file PATH        append the log to PATH (default: stderr)\n"
        "  --log-level LEVEL      error, warn, info (default) or debug\n"
        "  --version              print the version and the build id (the commit, and a\n"
        "                         hash of uncommitted changes) and exit; also in the log\n"
        "  --help                 print this text and exit\n"
        "The options after -- go to the image, followed by --flash-file and --retained-file.\n"
        "exit status (when no image is started): 0 --help or --version; 1 usage or start-up\n"
        "error; 12 a power cut fired; %d nothing bootable; %d flash error; %d the image could\n"
        "not be started. After the jump the status is the image's.\n",
        program, (unsigned int)POSIX_FLASH_CUT_MAX, FCN_BOOT_EXIT_NOTHING_BOOTABLE,
        FCN_BOOT_EXIT_FLASH, FCN_BOOT_EXIT_JUMP);
}

static bool parse_log_level(const char *text, log_level_t *out)
{
    static const struct {
        const char *name;
        log_level_t level;
    } table[] = {{"error", LOG_LEVEL_ERROR},
                 {"warn", LOG_LEVEL_WARN},
                 {"info", LOG_LEVEL_INFO},
                 {"debug", LOG_LEVEL_DEBUG}};
    bool found = false;

    for (size_t i = 0U; i < (sizeof(table) / sizeof(table[0])); i++) {
        if (strcmp(text, table[i].name) == 0) {
            *out = table[i].level;
            found = true;
        }
    }

    return found;
}

/* An option the bootloader passes to the image itself: the image may not be given it twice. */
static bool option_of_the_bootloader(const char *arg)
{
    static const char *const owned[] = {"--flash-file", "--retained-file"};
    bool owned_here = false;

    for (size_t i = 0U; i < (sizeof(owned) / sizeof(owned[0])); i++) {
        const size_t len = strlen(owned[i]);

        if ((strncmp(arg, owned[i], len) == 0) && ((arg[len] == '\0') || (arg[len] == '='))) {
            owned_here = true;
        }
    }

    return owned_here;
}

/* Returns 0 to go on, 1 to exit successfully (--help, --version), 2 on a usage error. */
static int parse_options(int argc, char **argv, boot_options_t *options)
{
    enum {
        OPT_FLASH = 1,
        OPT_RETAINED,
        OPT_CUT,
        OPT_LOG_FILE,
        OPT_LOG_LEVEL,
        OPT_VERSION,
        OPT_HELP
    };
    static const struct option long_options[] = {
        {"flash-file", required_argument, NULL, OPT_FLASH},
        {"retained-file", required_argument, NULL, OPT_RETAINED},
        {"flash-cut", required_argument, NULL, OPT_CUT},
        {"log-file", required_argument, NULL, OPT_LOG_FILE},
        {"log-level", required_argument, NULL, OPT_LOG_LEVEL},
        {"version", no_argument, NULL, OPT_VERSION},
        {"help", no_argument, NULL, OPT_HELP},
        {NULL, 0, NULL, 0}};
    int result = 0;
    int option;

    /* "+": stop at the first argument that is no option, so that "--" ends ours. */
    while ((result == 0) && ((option = getopt_long(argc, argv, "+", long_options, NULL)) != -1)) {
        char message[200];

        switch (option) {
        case OPT_FLASH:
            options->flash_path = optarg;
            break;
        case OPT_RETAINED:
            options->retained_path = optarg;
            break;
        case OPT_CUT:
            if (posix_flash_parse_cut(optarg, &options->cut, message, sizeof(message))) {
                options->cut_given = true;
            } else {
                (void)fprintf(stderr, "fcn_boot: invalid --flash-cut: %s\n", message);
                result = 2;
            }
            break;
        case OPT_LOG_FILE:
            options->log_path = optarg;
            break;
        case OPT_LOG_LEVEL:
            if (!parse_log_level(optarg, &options->log_level)) {
                (void)fprintf(stderr, "fcn_boot: invalid --log-level: %s\n", optarg);
                result = 2;
            }
            break;
        case OPT_VERSION:
            (void)printf("fcn_boot %s build %s\n", FCN_VERSION, fcn_build_id);
            result = 1;
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
        /* getopt_long stops at "--" and steps over it; anything else left is not ours. */
        if ((optind > 0) && (optind <= argc) && (strcmp(argv[optind - 1], "--") == 0)) {
            options->node_argc = argc - optind;
            options->node_argv = &argv[optind];
        } else if (optind < argc) {
            (void)fprintf(stderr, "fcn_boot: unexpected argument %s (node options go after --)\n",
                          argv[optind]);
            result = 2;
        } else {
            /* no node options */
        }
    }

    for (int i = 0; (result == 0) && (i < options->node_argc); i++) {
        if (option_of_the_bootloader(options->node_argv[i])) {
            (void)fprintf(stderr,
                          "fcn_boot: %s is the bootloader's to give to the image; give it to "
                          "fcn_boot before --\n",
                          options->node_argv[i]);
            result = 2;
        }
    }

    if ((result == 0) && (options->flash_path == NULL)) {
        (void)fprintf(stderr, "fcn_boot: --flash-file is required\n");
        result = 2;
    }

    return result;
}

/* An error the run ends with: in the log, and on stderr where the harness looks for it. */
static int fail(int status, const char *message)
{
    LOG_ERR(TAG, "%s", message);
    (void)fprintf(stderr, "fcn_boot: %s\n", message);
    return status;
}

/* ---------------------------------------------------------------------------- the metadata */

/* The recorded version of a slot, or "-" for a state that names no image (EMPTY, WRITING). */
static const char *recorded_version(const boot_slot_info_t *slot, char *text)
{
    const bool image = (slot->state != BOOT_SLOT_EMPTY) && (slot->state != BOOT_SLOT_WRITING);

    return image ? boot_host_version_text(slot->version, text) : "-";
}

static void log_metadata(boot_t *b)
{
    boot_description_t d;
    char va[BOOT_HOST_VERSION_TEXT];
    char vb[BOOT_HOST_VERSION_TEXT];

    if (boot_describe(b, &d) != BOOT_OK) {
        LOG_WRN(TAG, "metadata: cannot be read");
    } else if (!d.have_record) {
        LOG_INF(TAG, "metadata: factory state (no valid record in %lu entries used)",
                (unsigned long)d.entries_used);
    } else {
        LOG_INF(TAG,
                "metadata: record %lu (%s of slot %s) of %lu valid, slot A %s %s, slot B %s %s, "
                "trial attempts used %u of %u",
                (unsigned long)d.seq, boot_event_name(d.event), boot_host_slot_name(d.event_slot),
                (unsigned long)d.records_valid, boot_slot_state_name(d.slot[0].state),
                recorded_version(&d.slot[0], va), boot_slot_state_name(d.slot[1].state),
                recorded_version(&d.slot[1], vb), (unsigned int)d.attempts_used,
                (unsigned int)BOOT_MAX_ATTEMPTS);
    }
}

/* Why a slot is not bootable, for the line about "nothing bootable": lib/image's verdict on
 * it, from a validation of its own. */
static const char *slot_verdict(const flash_dev_t *dev, uint8_t slot)
{
    const plat_flash_layout_t *layout = plat_flash_layout();
    image_header_t header;

    return image_status_name(image_validate(dev, layout->slot[slot].offset, layout->slot[slot].size,
                                            IMAGE_TARGET_HOST, &header));
}

static void log_selection(const boot_selection_t *sel)
{
    char version[BOOT_HOST_VERSION_TEXT];

    switch (sel->event) {
    case BOOT_EVENT_ADOPT:
        LOG_INF(TAG, "select: adopted the image in slot %s as the confirmed one (factory state)",
                boot_host_slot_name(sel->event_slot));
        break;
    case BOOT_EVENT_ROLLBACK:
        LOG_WRN(TAG,
                "select: rolled back: the trial in slot %s used its %u attempts without "
                "confirming itself and is rejected",
                boot_host_slot_name(sel->event_slot), (unsigned int)BOOT_MAX_ATTEMPTS);
        break;
    case BOOT_EVENT_REJECT:
        LOG_WRN(TAG, "select: rolled back: the trial in slot %s does not validate and is rejected",
                boot_host_slot_name(sel->event_slot));
        break;
    case BOOT_EVENT_FALLBACK:
        /* The event's slot is the one fallen back to (boot.h): the image that does not validate
         * is in the other one. */
        LOG_WRN(TAG,
                "select: fell back: the confirmed image in slot %s does not validate; the one "
                "before it, in slot %s, is confirmed again",
                boot_host_slot_name((sel->event_slot == 0U) ? 1U : 0U),
                boot_host_slot_name(sel->event_slot));
        break;
    default:
        break;
    }

    if (sel->write_status != BOOT_OK) {
        LOG_WRN(TAG, "select: its write failed (%s): the confirmed image is started",
                boot_status_name(sel->write_status));
    }

    if (sel->trial) {
        LOG_INF(TAG, "select: slot %s as a trial, attempt %u of %u, image %s",
                boot_host_slot_name(sel->slot), (unsigned int)sel->attempt,
                (unsigned int)BOOT_MAX_ATTEMPTS, boot_host_version_text(sel->version, version));
    } else {
        LOG_INF(TAG, "select: slot %s, the confirmed image, image %s",
                boot_host_slot_name(sel->slot), boot_host_version_text(sel->version, version));
    }
}

/* -------------------------------------------------------------------------------- the jump */

static bool write_all(int fd, const uint8_t *data, size_t len)
{
    size_t done = 0U;

    while (done < len) {
        const ssize_t n = write(fd, &data[done], len - done);

        if (n > 0) {
            done += (size_t)n;
        } else if ((n < 0) && (errno == EINTR)) {
            /* try again */
        } else {
            return false;
        }
    }

    return true;
}

/*
 * Copy the payload of the slot `sel` chose into a sealed memory file and return its
 * descriptor, or -1 with `error` set. The header is decoded again and the copy hashed: the
 * process starts exactly the bytes whose digest the header names, whatever happened to the
 * flash between the validation and the copy.
 */
static int load_payload(const boot_selection_t *sel, char *error, size_t error_len,
                        uint32_t *payload_size)
{
    const flash_dev_t *dev = plat_flash_device();
    uint8_t bytes[IMAGE_HEADER_SIZE];
    image_header_t header;
    sha256_ctx_t sha;
    uint8_t digest[SHA256_DIGEST_SIZE];
    static uint8_t chunk[COPY_CHUNK];
    char name[32];

    if ((dev == NULL) || (dev->read(dev->ctx, sel->addr, bytes, IMAGE_HEADER_SIZE) != FLASH_OK) ||
        (image_header_decode(bytes, &header) != IMAGE_OK) ||
        (header.payload_size > (sel->size - IMAGE_PAYLOAD_OFFSET)) ||
        (header.image_version != sel->version)) {
        (void)snprintf(error, error_len, "the header of slot %s does not read back as validated",
                       boot_host_slot_name(sel->slot));
        return -1;
    }

    (void)snprintf(name, sizeof(name), "fcn-image-%s", boot_host_slot_name(sel->slot));
    const int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);

    if (fd < 0) {
        (void)snprintf(error, error_len, "memfd_create: %s", strerror(errno));
        return -1;
    }

    (void)sha256_init(&sha);

    for (uint32_t done = 0U; done < header.payload_size;) {
        const uint32_t rest = header.payload_size - done;
        const uint32_t len = (rest < COPY_CHUNK) ? rest : COPY_CHUNK;

        if (dev->read(dev->ctx, sel->addr + IMAGE_PAYLOAD_OFFSET + done, chunk, len) != FLASH_OK) {
            (void)snprintf(error, error_len, "a read of the payload of slot %s failed",
                           boot_host_slot_name(sel->slot));
            (void)close(fd);
            return -1;
        }

        (void)sha256_update(&sha, chunk, len);

        if (!write_all(fd, chunk, len)) {
            (void)snprintf(error, error_len, "write to the memory file: %s", strerror(errno));
            (void)close(fd);
            return -1;
        }

        done += len;
    }

    (void)sha256_final(&sha, digest);

    if (memcmp(digest, header.payload_sha256, sizeof(digest)) != 0) {
        (void)snprintf(error, error_len, "the payload of slot %s changed after its validation",
                       boot_host_slot_name(sel->slot));
        (void)close(fd);
        return -1;
    }

    /* Nothing may change the program from here on. */
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
        (void)snprintf(error, error_len, "sealing the memory file: %s", strerror(errno));
        (void)close(fd);
        return -1;
    }

    *payload_size = header.payload_size;
    return fd;
}

/* Start the image: only returns, with the status to exit with, if it could not. */
static int jump(const boot_selection_t *sel, const boot_options_t *options)
{
    char error[256];
    char message[512];
    uint32_t payload_size = 0U;
    const int fd = load_payload(sel, error, sizeof(error), &payload_size);

    if (fd < 0) {
        return fail(FCN_BOOT_EXIT_JUMP, error);
    }

    /* argv: a name, the node options, the bootloader's two, the terminating NULL. */
    const size_t count = 1U + (size_t)options->node_argc + (size_t)ADDED_OPTIONS + 1U;
    char **argv = (char **)calloc(count, sizeof(char *));

    if (argv == NULL) {
        (void)close(fd);
        return fail(FCN_BOOT_EXIT_JUMP, "out of memory for the image's arguments");
    }

    size_t n = 0U;
    char program[16];

    (void)snprintf(program, sizeof(program), "fcn-image-%s", boot_host_slot_name(sel->slot));
    argv[n++] = program;

    for (int i = 0; i < options->node_argc; i++) {
        argv[n++] = options->node_argv[i];
    }

    argv[n++] = s_flash_option;
    argv[n++] = options->flash_path;

    if (options->retained_path != NULL) {
        argv[n++] = s_retained_option;
        argv[n++] = options->retained_path;
    }

    argv[n] = NULL;

    LOG_INF(TAG,
            "jump: slot %s at 0x%06lx, payload %lu bytes, SHA-256 of the copy checked, %d node "
            "options",
            boot_host_slot_name(sel->slot), (unsigned long)sel->addr, (unsigned long)payload_size,
            options->node_argc);

    /* The hand-off last, when only the exec is left to fail (step 5 at the top). */
    plat_boot_handoff_t handoff;

    boot_host_handoff(sel, &handoff);

    const bool handed_over = posix_retained_set_handoff(&handoff);

    if (!handed_over) {
        LOG_WRN(TAG, "no retained cell: the image gets no hand-off and runs as if started "
                     "without a bootloader");
    }

    /* The locks go now, whatever still refers to the files' open descriptions after the exec
     * (step 6 at the top): the image locks both files itself. The descriptors, close-on-exec,
     * and the cell's mapping stay for a failed exec. */
    posix_flash_unlock();
    posix_retained_unlock();
    (void)fexecve(fd, argv, environ);

    /* Only reached if the exec failed. The image did not start, so its hand-off is taken back
     * through the cell's mapping once the cell's lock is ours again; a cell another process
     * locked in the meantime is that process's, and nothing is written into it. main() closes
     * the cell before the exit. */
    (void)snprintf(error, sizeof(error), "fexecve of slot %s: %s", boot_host_slot_name(sel->slot),
                   strerror(errno));

    if (posix_retained_relock()) {
        posix_retained_clear_handoff();
    } else if (handed_over) {
        (void)snprintf(message, sizeof(message),
                       "the hand-off could not be taken back: --retained-file %s is in use by "
                       "another process",
                       options->retained_path);
        LOG_ERR(TAG, "%s", message);
        (void)fprintf(stderr, "fcn_boot: %s\n", message);
    } else {
        /* no hand-off was written (no cell): nothing to take back */
    }

    free((void *)argv);
    (void)close(fd);
    return fail(FCN_BOOT_EXIT_JUMP, error);
}

/* ---------------------------------------------------------------------------------- main */

/* Select and jump (the jump writes the hand-off). The flash and the cell are open. */
static int boot(const boot_options_t *options)
{
    static boot_t s_boot;
    static bootimg_t s_validator;
    boot_validator_t validator;
    boot_config_t config;
    boot_selection_t sel;
    char message[256];

    (void)bootimg_init(&s_validator, IMAGE_TARGET_HOST, &validator);
    boot_host_config(&config, &validator);

    if (boot_init(&s_boot, &config) != BOOT_OK) {
        return fail(FCN_BOOT_EXIT_FLASH, "lib/boot refuses the platform's flash layout");
    }

    log_metadata(&s_boot);

    const boot_status_t status = boot_select(&s_boot, &sel);

    if (status == BOOT_E_NOTHING_BOOTABLE) {
        log_selection(&sel);
        (void)snprintf(message, sizeof(message),
                       "nothing bootable: slot A %s, slot B %s, and the metadata allow neither",
                       slot_verdict(config.dev, 0U), slot_verdict(config.dev, 1U));
        return fail(FCN_BOOT_EXIT_NOTHING_BOOTABLE, message);
    }

    if (status != BOOT_OK) {
        (void)snprintf(message, sizeof(message), "select failed: %s (last validation: %s)",
                       boot_status_name(status), image_status_name(bootimg_status(&s_validator)));
        return fail(FCN_BOOT_EXIT_FLASH, message);
    }

    log_selection(&sel);
    return jump(&sel, options);
}

int main(int argc, char **argv)
{
    boot_options_t options;
    char error[1024] = {0};

    (void)memset(&options, 0, sizeof(options));
    options.log_level = LOG_LEVEL_INFO;

    const int parsed = parse_options(argc, argv, &options);

    if (parsed != 0) {
        return (parsed == 1) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /* As the node does (node_main.c): die with the test harness that started us. The setting
     * survives the exec, so the image inherits it. */
    const char *const harness = getenv("FCN_DIE_WITH_PARENT");

    if (harness != NULL) {
        (void)prctl(PR_SET_PDEATHSIG, SIGKILL);

        if ((long)getppid() != strtol(harness, NULL, 10)) {
            return EXIT_FAILURE;
        }
    }

    if (!boot_host_log_open(options.log_path, options.log_level, error, sizeof(error))) {
        (void)fprintf(stderr, "fcn_boot: %s\n", error);
        return EXIT_FAILURE;
    }

    /* The cell first, as a target's start-up code reads its no-init RAM before anything. */
    if (!posix_retained_open_boot(options.retained_path, error, sizeof(error))) {
        (void)fprintf(stderr, "fcn_boot: %s\n", error);
        return EXIT_FAILURE;
    }

    posix_retained_info_t retained;

    posix_retained_get_info(&retained);
    LOG_INF(TAG,
            "fcn_boot %s starting: reset cause %s (retained cell: %s, %lu resets since power-on)",
            FCN_VERSION, posix_reset_reason_name(retained.cause), retained.verdict,
            (unsigned long)retained.resets);
    LOG_INF(TAG, "build %s", fcn_build_id);

    /* The bootloader waits the modelled times by sleeping (no wait function), and a cut ends it
     * as a loss of supply without a kernel (no power-loss function): posix_flash.h. */
    if (!posix_flash_open(options.flash_path, NULL, error, sizeof(error))) {
        posix_retained_close();
        (void)fprintf(stderr, "fcn_boot: %s\n", error);
        return EXIT_FAILURE;
    }

    if (options.cut_given) {
        posix_flash_arm_cut(&options.cut);
    }

    {
        posix_flash_info_t flash;

        posix_flash_get_info(&flash);

        if (options.cut_given) {
            LOG_INF(TAG, "flash %s, power cut in program or erase %lu, seed %lu: %s",
                    flash.created ? "created erased" : "opened",
                    (unsigned long)options.cut.operation, (unsigned long)options.cut.seed,
                    options.flash_path);
        } else {
            LOG_INF(TAG, "flash %s, no power cut: %s", flash.created ? "created erased" : "opened",
                    options.flash_path);
        }
    }

    const int status = boot(&options);

    /* Only reached without a jump. */
    posix_flash_close();
    posix_retained_close();
    return status;
}
