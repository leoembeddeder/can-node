/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The retained-RAM cell: a few words that survive a reset but not a loss of supply, and the
 * reset cause of this start that the platform reads from them.
 *
 * A microcontroller keeps its RAM through a reset, so a small struct placed in a section that
 * the start-up code does not zero (".noinit") still holds after a reset what was written into
 * it before. After power-on it holds garbage. A magic number, a layout version and a CRC tell
 * the two apart: a cell that does not carry all three correctly is taken as a power-on and
 * written afresh. Together with the device's reset-cause flags (on an STM32 the RCC_CSR bits:
 * IWDGRSTF, SFTRSTF, PORRSTF, PINRSTF and others) it answers why the device started.
 *
 * WHAT IT HOLDS. The reason given to the last plat_reset(), the number of resets since
 * power-on, and four reserved words that carry the bootloader's HAND-OFF (below). The platform
 * reads it once, at start, before the application runs: plat_reset_cause() then returns the
 * reason, or PLAT_RESET_POWER_ON if the cell was not valid or recorded no reset. Right after
 * reading, the platform sets the recorded reason back to PLAT_RESET_POWER_ON, so that a run
 * that ends without plat_reset() (on the host: an orderly stop, or a killed process) leads to
 * a power-on at the next start, with every word reset to 0. plat_reset() writes its reason and
 * counts the reset; plat_reset(PLAT_RESET_POWER_LOSS) destroys the cell instead, as a loss of
 * supply would.
 *
 * THE HAND-OFF. The bootloader (on the host fcn_boot, platform/posix/boot_main.c) reads the
 * cell before the image, zeroing the words of a hand-off an earlier start left in it, decides
 * which slot to start (lib/boot, boot_select()) and, once nothing but the start itself is left
 * to fail, writes what it decided into the reserved words, sealed with the cell's CRC: the
 * slot, whether the image runs as a TRIAL or as the CONFIRMED image, which attempt of the
 * trial this is, the image's version from its header, and the state change the bootloader
 * made on the way (a rollback, a rejected trial, a fallback, the adoption of the factory
 * image). It leaves the recorded reset reason in the cell for the image. If the start fails
 * after all (on the host a failed exec), it zeroes the four words again, with the reason and
 * the rest of the cell kept, before it gives up. The image's platform reads the hand-off
 * together with the reason (plat_boot_handoff()) and then zeroes the four words, so that a
 * later start without the bootloader finds none: no hand-off means that no bootloader started
 * the image, or (on the host) that fcn_boot had no cell to write it into, and a hand-off means
 * that the bootloader started this image, but for THE REMAINING WINDOW below. The words:
 *
 *     word  bits    content
 *     0     0-31    PLAT_HANDOFF_MAGIC ("FCNH"): a hand-off is present; anything else: none
 *     1     0-7     slot started: 0 (A) or 1 (B)
 *           8-15    plat_boot_mode_t: 1 CONFIRMED, 2 TRIAL
 *           16-23   attempt: 1 to 3 for a TRIAL, 0 for the CONFIRMED image
 *           24-31   plat_boot_event_t: what the bootloader's select wrote on the way
 *     2     0-31    the image version of the slot's header (lib/image: major.minor.patch)
 *     3     0-7     the slot the event acted on (lib/boot: the slot adopted, the trial's for a
 *                   rollback or a rejection, the slot fallen back to), or 0xFF for none
 *           8-15    0, or the lib/boot status (boot_status_t) of a write of the select that
 *                   failed (the image then runs as the CONFIRMED one, lib/boot)
 *           16-31   0
 *
 * A hand-off whose words break any of these rules is no hand-off. The boot attempts are not
 * kept here: RAM does not survive a loss of supply, and a trial that cuts its own supply must
 * still run out of attempts. lib/boot counts them in the flash (one program unit per attempt,
 * written before the trial is started).
 *
 * THE REMAINING WINDOW (host only). A hand-off stays in the cell only when the run ends between
 * its write and the image's read of the cell: fcn_boot killed after it wrote the hand-off and
 * before the exec (or, after a failed exec, before it zeroed the words again), or an image
 * that ends before its platform has opened the cell. The node and the test payloads open it
 * as soon as they have read their command line, before any other check of it, so only a
 * command line the node cannot parse (or --help, --version) ends it before the open: the
 * cell's path is one of its options. A node started directly on that cell, without fcn_boot,
 * would take that hand-off as its own. The next start through fcn_boot zeroes it when it opens
 * the cell. On a target the bootloader runs at every reset, so there is no start without it.
 *
 * The lock of the cell is the other part of the window. fcn_boot holds it (and the flash's)
 * until directly before the exec and then releases it (posix_retained_unlock()), so that the
 * image can lock the cell whatever still refers to the old process image's open file
 * descriptions after the exec; the image locks it again when it opens it. From the release
 * until that open, another program started on the same cell at that moment can take it. The
 * image then fails to start ("in use by another node", status 1), as it would have if that
 * program had come first, and that program finds the hand-off: a node started directly takes
 * it as its own (as above), another fcn_boot zeroes it. If the exec failed, fcn_boot cannot
 * take the lock back (posix_retained_relock()), writes nothing into the cell and says that the
 * hand-off could not be taken back. Only two programs started on one cell at the same moment
 * get there; a target has one reset vector.
 *
 * ON A TARGET. A software reset (assert, stack overflow, a requested reset) writes its reason
 * into the cell, then triggers the reset (SCB AIRCR SYSRESETREQ). A watchdog reset writes
 * nothing, because no code runs before it: the start-up code finds IWDGRSTF set in RCC_CSR and
 * reports PLAT_RESET_WATCHDOG, and reads the cell for the software reasons; PORRSTF set means
 * power-on whatever the cell says. Then it clears the flags (RMVF).
 *
 * ON THE HOST (platform/posix/posix_retained.c). The cell is a file of exactly
 * sizeof(plat_retained_cell_t) bytes that the node maps shared (mmap MAP_SHARED), named with
 * `--retained-file PATH`. It outlives the process as RAM outlives a reset; a test bench removes
 * it to cut the power. A missing file, one of another size or one with a bad magic, version or
 * CRC is a power-on. The watchdog model calls plat_reset(PLAT_RESET_WATCHDOG) like any other
 * reset, so on the host the cell carries the watchdog's reason as well. Without the option
 * there is no cell and every start is a power-on.
 *
 * LAYOUT. Native byte order, little-endian on every machine the project builds for (x86-64,
 * Cortex-M); 36 bytes, no padding (checked below). The CRC is CRC-32/ISO-HDLC (lib/crc,
 * equal to zlib.crc32) over the 32 bytes before it, so a test in Python can check a cell with
 * nothing but the standard library.
 */
#ifndef FCN_PLAT_RETAINED_H
#define FCN_PLAT_RETAINED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"

/** "FCNR" in a little-endian word: the bytes 'F', 'C', 'N', 'R'. */
#define PLAT_RETAINED_MAGIC 0x524E4346U

/** The layout below; a cell of another version is taken as a power-on. */
#define PLAT_RETAINED_VERSION 1U

/** The reserved words: the bootloader's hand-off (THE HAND-OFF above); 0 after power-on. */
#define PLAT_RETAINED_RESERVED_WORDS 4U

/** "FCNH" in a little-endian word, the bytes 'F', 'C', 'N', 'H': reserved[0] of a hand-off. */
#define PLAT_HANDOFF_MAGIC 0x484E4346U

/** The largest attempt of a trial in a hand-off: lib/boot's BOOT_MAX_ATTEMPTS. */
#define PLAT_HANDOFF_MAX_ATTEMPT 3U

/** "No slot" in a hand-off's event slot. */
#define PLAT_HANDOFF_NO_SLOT 0xFFU

typedef struct {
    uint32_t magic;        /**< PLAT_RETAINED_MAGIC */
    uint16_t version;      /**< PLAT_RETAINED_VERSION */
    uint16_t size;         /**< sizeof(plat_retained_cell_t) */
    uint32_t reset_reason; /**< what the next start reports: a plat_reset_reason_t */
    uint32_t resets;       /**< plat_reset() calls since power-on */
    uint32_t reserved[PLAT_RETAINED_RESERVED_WORDS]; /**< the hand-off, see above */
    uint32_t crc; /**< CRC-32/ISO-HDLC of the bytes before this member */
} plat_retained_cell_t;

_Static_assert(sizeof(plat_retained_cell_t) == 36U, "the cell has no padding");
_Static_assert((size_t)offsetof(plat_retained_cell_t, reset_reason) == 8U, "layout version 1");
_Static_assert((size_t)offsetof(plat_retained_cell_t, reserved) == 16U, "layout version 1");
_Static_assert((size_t)offsetof(plat_retained_cell_t, crc) == 32U, "the CRC is the last word");

/** How the bootloader started the image: the values of word 1, bits 8-15. */
typedef enum {
    PLAT_BOOT_CONFIRMED = 1, /**< the CONFIRMED image: nothing to confirm */
    PLAT_BOOT_TRIAL = 2      /**< a TRIAL: it confirms itself once it has run in good health */
} plat_boot_mode_t;

/** What the bootloader's select wrote before it started the image: the values of word 1,
 *  bits 24-31, which are those of lib/boot's boot_event_t for the events a select writes. */
typedef enum {
    PLAT_BOOT_EVENT_NONE = 0,     /**< nothing */
    PLAT_BOOT_EVENT_ADOPT = 1,    /**< the factory image was adopted as the CONFIRMED one */
    PLAT_BOOT_EVENT_ROLLBACK = 5, /**< a trial used all its attempts and was rejected */
    PLAT_BOOT_EVENT_REJECT = 6,   /**< a trial did not validate and was rejected */
    PLAT_BOOT_EVENT_FALLBACK = 7  /**< the CONFIRMED image did not validate: the one before it
                                       is CONFIRMED again */
} plat_boot_event_t;

/** The hand-off, decoded. */
typedef struct {
    uint8_t slot;               /**< 0 (A) or 1 (B) */
    plat_boot_mode_t mode;      /**< TRIAL or CONFIRMED */
    uint8_t attempt;            /**< 1..PLAT_HANDOFF_MAX_ATTEMPT for a TRIAL, 0 otherwise */
    uint32_t image_version;     /**< from the image header: major << 24 | minor << 16 | patch */
    plat_boot_event_t event;    /**< the state change the bootloader made */
    uint8_t event_slot;         /**< the slot it acted on (word 3 above), PLAT_HANDOFF_NO_SLOT
                                     for none */
    uint8_t select_write_error; /**< 0, or the boot_status_t of a write of the select that
                                     failed */
} plat_boot_handoff_t;

/**
 * The reset cause of this start: the reason given to the plat_reset() that ended the previous
 * run, or PLAT_RESET_POWER_ON if there was none (no cell, a cell that was not valid, or a
 * previous run that did not end with a reset). Constant while the image runs. Any task, any
 * time after the platform has started.
 */
plat_reset_reason_t plat_reset_cause(void);

/**
 * The bootloader's hand-off to this start: true with `*out` filled if the cell held one when
 * the platform started, false (nothing written) if it held none (no cell, no hand-off in it,
 * or one that breaks the rules of THE HAND-OFF). True means that the bootloader started this
 * image, but for THE REMAINING WINDOW on the host; false, that no bootloader started it or
 * that it had no cell to write into (on the host, fcn_boot without --retained-file). Constant
 * while the image runs. Any task, any time after the platform has started; NULL `out` returns
 * false.
 */
bool plat_boot_handoff(plat_boot_handoff_t *out);

#endif /* FCN_PLAT_RETAINED_H */
