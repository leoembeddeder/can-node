/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The retained-RAM cell of the host build (plat_retained.h): a file that the node maps and
 * that outlives the process, as RAM outlives a reset. What the cell holds and when it counts
 * as a power-on is described in plat_retained.h; this header is the host side of it.
 *
 * The module knows nothing about FreeRTOS, so that its tests run without a kernel
 * (tests/platform/test_posix_retained.c). posix_retained_open() runs on the main thread before
 * anything else of the node; posix_retained_note_reset() is called by plat_reset() from
 * whichever context resets, and posix_retained_unlock() by the context that ends the run
 * (plat_reset() after it has recorded the reset, the shutdown guard from the logger thread);
 * the rest by the main thread.
 */
#ifndef FCN_POSIX_RETAINED_H
#define FCN_POSIX_RETAINED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "plat_retained.h"
#include "platform.h"

/** What posix_retained_open() found, for the log and the statistics. */
typedef struct {
    bool open;                 /**< a cell is mapped */
    plat_reset_reason_t cause; /**< plat_reset_cause() */
    uint32_t resets;           /**< resets since power-on, as the cell held them at start */
    /** Why the start is what it is: "no cell" (no file was given), "missing" (the file did
     *  not exist), "size", "magic", "version", "crc" (a cell that is not valid), "reason" (a
     *  valid cell whose reason is not one plat_reset() records), "no reset" (a valid cell that
     *  recorded no reset), "reset" (a valid cell with the reason of a reset). */
    const char *verdict;
    /** The bootloader's hand-off (plat_retained.h, THE HAND-OFF): "none" (no cell, a cell that
     *  was not valid, or reserved[0] is not PLAT_HANDOFF_MAGIC), "invalid" (the magic, but a
     *  word that breaks the rules: no hand-off) or "taken" (`handoff` holds it). */
    const char *handoff_verdict;
    bool have_handoff;           /**< handoff_verdict is "taken" */
    plat_boot_handoff_t handoff; /**< valid if have_handoff */
} posix_retained_info_t;

/**
 * Open the cell in the file `path`, or with `path` NULL run without one (every start is a
 * power-on). Reads the cause of this start and prepares the cell for this run as
 * plat_retained.h says: a cell that is not valid, or that recorded no reset, is written afresh
 * (every word 0, reason PLAT_RESET_POWER_ON); a valid one keeps its reset count and reserved
 * words and gets the reason PLAT_RESET_POWER_ON in place of the one just read. A hand-off of the
 * bootloader in a valid cell (plat_retained.h, THE HAND-OFF) is read first, whatever the
 * reason, and its words are then zeroed: plat_boot_handoff() returns it for this run, and the
 * next start without the bootloader finds none. Reserved words that hold no hand-off (word 0 is
 * not the magic) are kept as before.
 *
 * The file is created if it does not exist and given the cell's size if it has another; it is
 * locked (flock) for as long as it is open, until posix_retained_close() or
 * posix_retained_unlock() releases the lock, so that two nodes cannot share one cell. Returns
 * false with a message for the user in
 * `error` if the file cannot be opened, locked, sized or mapped, or a cell is already open;
 * nothing is changed then. Main thread, once, before anything that could call plat_reset().
 */
bool posix_retained_open(const char *path, char *error, size_t error_len);

/**
 * The bootloader's posix_retained_open(): the same, except that the reason of a valid cell
 * stays recorded in it, for the image the bootloader starts, which reads it as its own reset
 * cause. posix_retained_get_info() reports what was found (cause, resets, verdict, and a
 * hand-off left in the cell, whose words are zeroed as posix_retained_open() zeroes them). A
 * cell that is not valid is written afresh, as after power-on. The rest as
 * posix_retained_open(). Main thread, once.
 */
bool posix_retained_open_boot(const char *path, char *error, size_t error_len);

/**
 * Write `handoff` into the reserved words of the open cell (plat_retained.h, THE HAND-OFF) and
 * seal the cell with its CRC. Returns false, changing nothing, without a cell, while its lock is
 * released (posix_retained_unlock()), for a NULL `handoff`, or for one that breaks the rules.
 * The bootloader, after posix_retained_open_boot(), directly before it starts the image.
 */
bool posix_retained_set_handoff(const plat_boot_handoff_t *handoff);

/**
 * Take the hand-off back out of the open cell: if reserved word 0 is PLAT_HANDOFF_MAGIC, zero
 * the four reserved words and seal the cell again with its CRC; the reset reason, the count
 * and the rest of the cell stay as they are. Does nothing without a cell, while its lock is
 * released, or without the magic in word 0. The bootloader, when the image it wrote the
 * hand-off for could not be started (a failed exec), after posix_retained_relock() and before
 * it closes the cell: a later start then finds no hand-off.
 */
void posix_retained_clear_handoff(void);

/**
 * Release the lock of the open cell at once (flock LOCK_UN), keeping the file open and mapped.
 * The lock belongs to the open file description (flock(2)), which the mapping refers to as the
 * descriptor does: without this call it lasts as long as any reference to the description,
 * also one that outlives the process image (posix_retained.c, THE LOCK BELONGS TO THE OPEN
 * FILE DESCRIPTION). From the call on, another process can open the cell, and nothing here
 * writes into it (posix_retained_set_handoff(), posix_retained_clear_handoff() and
 * posix_retained_note_reset() do nothing) until posix_retained_relock() has taken the lock
 * again. Does nothing without a cell or if the lock is released already. For every end of a
 * run that does not get to posix_retained_close(): the bootloader, after the hand-off is
 * written and directly before the exec of the image, which opens and locks the cell itself
 * (plat_retained.h, THE REMAINING WINDOW); and, after posix_retained_note_reset() and directly
 * before _exit(), plat_reset(), the shutdown guard, the power cut without a kernel
 * (posix_flash.c) and the test payloads. The context that ends the run, any thread; the caller
 * makes sure that only one context does (plat_reset() and the shutdown guard share one claim,
 * posix_lifecycle.c).
 */
void posix_retained_unlock(void);

/**
 * Take the lock of the open cell again after posix_retained_unlock(), without waiting
 * (flock LOCK_EX | LOCK_NB). Returns true if this process holds the lock (taken now, or never
 * released), false without a cell or if another process has locked the file in the meantime:
 * the cell is that process's then, and nothing here writes into it. True says only that nobody
 * else holds the cell now, not that nobody opened it after the unlock. The bootloader, after a
 * failed exec, before it takes the hand-off back. Main thread.
 */
bool posix_retained_relock(void);

/** The four reserved words of `handoff` (THE HAND-OFF). Pure; any thread. */
void posix_handoff_encode(const plat_boot_handoff_t *handoff,
                          uint32_t words[PLAT_RETAINED_RESERVED_WORDS]);

/**
 * Decode the four reserved words `words` of a valid cell: "taken" with `*out` filled if they
 * hold a hand-off that keeps every rule of THE HAND-OFF, "invalid" (nothing written) if word 0
 * is the magic but a rule is broken, "none" (nothing written) if word 0 is not the magic. Pure;
 * any thread.
 */
const char *posix_handoff_decode(const uint32_t words[PLAT_RETAINED_RESERVED_WORDS],
                                 plat_boot_handoff_t *out);

/**
 * Record a reset in the cell (plat_reset() calls it): the reason and one more reset, with the
 * CRC over them; for PLAT_RESET_POWER_LOSS the cell is destroyed instead (every byte 0), so the
 * next start is a power-on. Does nothing without a cell or while its lock is released
 * (posix_retained_unlock()). Only plain loads and stores of memory: it takes no lock and makes
 * no system call, so any context may call it, a signal handler included. The caller makes sure
 * that only one call happens (plat_reset() does).
 */
void posix_retained_note_reset(plat_reset_reason_t reason);

/** Unmap the cell, release its lock (flock LOCK_UN, whatever else still refers to the open file
 *  description) and close the file; the cell stays in it. Main thread, after the run. */
void posix_retained_close(void);

/** What the start found; valid after posix_retained_open(), also after posix_retained_close().
 *  Main thread. */
void posix_retained_get_info(posix_retained_info_t *out);

/** True for the reasons plat_reset() records in the cell as they are: every reason of
 *  plat_reset_reason_t except PLAT_RESET_POWER_ON and PLAT_RESET_POWER_LOSS. */
bool posix_retained_reason_recorded(uint32_t reason);

/** True for every reason plat_reset() accepts: the recorded ones and PLAT_RESET_POWER_LOSS. */
bool posix_reset_reason_valid(plat_reset_reason_t reason);

/** The CRC a valid cell carries: CRC-32/ISO-HDLC over every byte before `crc`. */
uint32_t posix_retained_crc(const plat_retained_cell_t *cell);

/** The name of a reason as in the DBC's value table ("WATCHDOG"), "POWER_ON", or "unknown". */
const char *posix_reset_reason_name(plat_reset_reason_t reason);

#endif /* FCN_POSIX_RETAINED_H */
