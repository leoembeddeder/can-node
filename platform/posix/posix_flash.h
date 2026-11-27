/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The flash of the host build (plat_flash.h): a file mapped into the process and driven by the
 * NOR model of lib/flash (nor.h), with the time of each operation waited out, power cuts, and
 * the simulation control commands "flash.*". docs/update.md describes it for the reader of a
 * result; posix_flash.c says how it is built.
 *
 * The module knows nothing about FreeRTOS, so that the bootloader (fcn_boot) and the tools,
 * which do not link the kernel, use it as the node does. The two things that differ between
 * them are functions the program hands in: how an operation waits (posix_flash_set_wait()) and
 * how a run ends when a cut fires (posix_flash_set_power_loss()).
 *
 * THE FILE. Exactly the size of the device in plat_flash_layout() (2105344 bytes), byte n of
 * the file being byte n of the device, named with `--flash-file PATH`. posix_flash_open()
 * creates it erased (every byte 0xFF) when it does not exist, and refuses a file of another
 * size: its content would not be a flash of this layout. It is mapped shared (MAP_SHARED), so
 * every program and erase is in the file as soon as it is done, whether or not the process
 * ends in order, and the next process (the bootloader after the node, the node after a reset)
 * finds it there. It is locked (flock) while it is open: two processes driving one flash would
 * each change it under the other. The lock is released by posix_flash_close(), and by
 * posix_flash_unlock() where a run ends without the close (before the bootloader's exec and
 * before every _exit()), never left to the end of the process: posix_flash.c, THE LOCK AND THE
 * END OF A RUN. Without the option there is no flash: plat_flash_device() returns NULL and
 * nothing else changes.
 *
 * TIMING. Each program and erase waits the time the modelled part takes before it returns
 * (the figures below, or the ones given to posix_flash_open()): a read takes none. With no wait
 * function the calling thread sleeps until then (clock_nanosleep, CLOCK_MONOTONIC, an absolute
 * deadline: never shorter); the node sets a function that blocks its task for one tick and is
 * called until the time has passed, as a driver polls the busy flag of a serial NOR once per
 * tick. A host that runs the process late makes an operation longer, never shorter.
 *
 * POWER CUTS. `--flash-cut N[:SEED]` (posix_flash_parse_cut(), posix_flash_arm_cut()) cuts the
 * N-th program or erase of the run, and the simulation command flash.cut the N-th from the
 * moment it is served, at a position the seed chooses (NOR_CUT_SEEDED_POSITION). The cut
 * operation is torn as nor.h describes, runs for the time up to its cut, and then the supply is
 * gone: a line "FATAL flash: power cut in operation ..." is written to stderr and the power-loss
 * function ends the run. Without one, the run ends as a loss of supply without a kernel: the
 * retained cell is destroyed (posix_retained_note_reset()), the locks of the cell and of the
 * flash file are released (posix_retained_unlock(), posix_flash_unlock()) and the process exits
 * with PLAT_RESET_POWER_LOSS (12) at once (_exit: nothing buffered in stdio is written). Every
 * FreeRTOS program on this platform has plat_reset(PLAT_RESET_POWER_LOSS) set by
 * posix_platform_init(), which ends with the same status and releases the same locks.
 *
 * A HANG. `--flash-hang N` (posix_flash_arm_hang()) makes the N-th program or erase of the run
 * hang: the model makes the operation, and then its busy flag never clears, as a part that has
 * failed, or a driver that waits for a status it never gets, would leave its caller. The call
 * writes the line "flash: operation N hangs: its busy flag never clears (--flash-hang)" to
 * stderr and then waits for ever, with the wait function (the node: one tick at a time, so that
 * the other tasks run and the node's supervision can find the task that is stuck, app.c) or in
 * sleeps of a millisecond. It is a fault of the simulated hardware for the tests of that
 * supervision, put in from the start so that it does not race the node. Without a flash it is
 * refused by the caller (node_main.c).
 *
 * SIMULATION CONTROL (posix_sim.h), served by whichever thread calls posix_sim_service() (in
 * the node, the CAN hardware thread):
 *
 *   flash.status
 *       ok flash=on size=<bytes> reads=<n> bytes_read=<n> programs=<n> units_programmed=<n>
 *          erases=<n> refused=<n> busy_ns=<ns> erases_meta=<n> erases_slot_a=<n>
 *          erases_slot_b=<n> max_sector_erases=<n> cut=none|armed|fired cut_in=<n>
 *          cut_seed=<n>
 *       or, without a flash, ok flash=off. Counts since posix_flash_open(): reads, programs
 *       and erases done (a torn one counted), refused calls, units programmed, the modelled
 *       time of every operation, the erases of each area of the layout and of the sector erased
 *       most often. cut=armed with cut_in, the programs and erases until the cut (1: the next),
 *       and cut_seed; cut_in=0 cut_seed=0 otherwise. A fired cut ends the run, so a node never
 *       answers cut=fired; only a program whose power-loss function returns does (a test).
 *   flash.cut N[:SEED]
 *       ok cut=armed cut_in=<N> cut_seed=<SEED> t_ns=<CLOCK_MONOTONIC when served>
 *       Cut the N-th program or erase that starts after the command is served (1: the next),
 *       N from 1 to 2147483647, SEED from 0 to 4294967295, 0 if left out. Replaces a cut armed
 *       before. The cut is a fault of the simulated hardware, the supply that fails, not a
 *       request to the application.
 *   flash.cut off
 *       ok cut=none t_ns=<ns>: no cut armed any more.
 * flash.cut without a flash answers "err no flash in this run (started without
 * --flash-file)"; bad arguments answer err with the usage.
 *
 * THREADS. The device's operations run in one thread at a time (on the node, the one task that
 * writes the flash); the simulation commands in another. The two share only atomics: the
 * command leaves its request in one atomic word that the next operation takes, and each
 * operation publishes its counters in atomics that the status reads. Nothing here takes a lock.
 * posix_flash_open(), the set functions and posix_flash_close() belong to the main thread,
 * before and after the others run. posix_flash_unlock() belongs to the context that ends the
 * run, whichever thread that is.
 */
#ifndef FCN_POSIX_FLASH_H
#define FCN_POSIX_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flash/nor.h"
#include "plat_flash.h"

/** Typical page program time tPP of the Winbond W25Q128JV, a 3 V serial NOR with 4 KiB sectors
 *  and 256-byte pages: 0.4 ms (3 ms at most). Datasheet "W25Q128JV-DTR, 3V 128M-bit serial
 *  flash memory", Revision C (March 2, 2018), AC Electrical Characteristics; the same datasheet
 *  gives at least 100,000 program and erase cycles per sector, which the model does not limit. */
#define POSIX_FLASH_PAGE_PROGRAM_NS 400000U

/** Typical 4 KiB sector erase time tSE of the same part and table: 45 ms (400 ms at most). */
#define POSIX_FLASH_SECTOR_ERASE_NS 45000000U

/** The largest N of a cut: the request to cut travels in one atomic word with its seed. */
#define POSIX_FLASH_CUT_MAX 2147483647U

/** A cut of `operation`-th program or erase (1 = the first, or the next) with `seed`. */
typedef struct {
    uint32_t operation; /**< 1 .. POSIX_FLASH_CUT_MAX */
    uint32_t seed;      /**< any value */
} posix_flash_cut_t;

/** How an operation waits: block the caller for a short while (the node: one tick). It is
 *  called again until the operation's time has passed. */
typedef void (*posix_flash_wait_fn)(void);

/** How the run ends when a cut fires. It should not return; if it does, the device stays
 *  without supply and every later call returns FLASH_E_POWER_LOST (what a test wants). */
typedef void (*posix_flash_power_loss_fn)(void);

/** What the flash counted, for the statistics and the log; see flash.status. */
typedef struct {
    bool open;    /**< a flash file is open */
    bool created; /**< posix_flash_open() created it erased */
    uint64_t reads;
    uint64_t bytes_read;
    uint64_t programs;
    uint64_t units_programmed;
    uint64_t erases;
    uint64_t refused;
    uint64_t busy_ns;
    uint32_t erases_meta;
    uint32_t erases_slot[PLAT_FLASH_SLOTS];
    uint32_t max_sector_erases;
    uint32_t cut_in;   /**< programs and erases until the armed cut, 1: the next; 0: none */
    uint32_t cut_seed; /**< its seed, 0 if none */
    bool cut_fired;    /**< a cut fired (only seen when the power-loss function returned) */
} posix_flash_info_t;

/**
 * Parse "N" or "N:SEED", the syntax of `--flash-cut` and of flash.cut, into `out`: decimal
 * numbers with nothing before, between or after them, N from 1 to POSIX_FLASH_CUT_MAX, SEED
 * from 0 to 4294967295 (0 when left out). On a mistake returns false with a message for the
 * user in `error` and leaves `out` alone. Any thread.
 */
bool posix_flash_parse_cut(const char *text, posix_flash_cut_t *out, char *error, size_t error_len);

/**
 * Open the flash in the file `path`, or with `path` NULL run without one (plat_flash_device()
 * returns NULL). Creates the file erased if it does not exist; refuses one of another size,
 * one that is not a regular file, and one another process has open. `timing` NULL takes the
 * W25Q128JV figures above; a tool that wants no waits passes zeros. Registers the "flash."
 * commands with simulation control, also without a file. Returns false with a message for the
 * user in `error` (nothing is open then). Main thread, before any operation and before the
 * thread that serves simulation control starts.
 */
bool posix_flash_open(const char *path, const nor_timing_t *timing, char *error, size_t error_len);

/** Arm `cut` (counted from the next operation; from the run's first if called right after
 *  posix_flash_open()). Does nothing without a flash. Any thread. */
void posix_flash_arm_cut(const posix_flash_cut_t *cut);

/** Make the `operation`-th program or erase of the run, counted from the next one (1 ..
 *  POSIX_FLASH_CUT_MAX), hang (see A HANG); 0 arms nothing. Does nothing without a flash. Main
 *  thread, right after posix_flash_open() and before any operation. */
void posix_flash_arm_hang(uint32_t operation);

/** Set how operations wait; NULL, the default, sleeps until the end of the operation. Kept
 *  across open and close. Main thread, before the first operation. */
void posix_flash_set_wait(posix_flash_wait_fn wait);

/** Set how the run ends when a cut fires; NULL, the default, ends it as described in POWER
 *  CUTS. Kept across open and close. Main thread, before the first operation. */
void posix_flash_set_power_loss(posix_flash_power_loss_fn end);

/** Unmap the flash, release its lock (flock LOCK_UN, whatever else still refers to the open
 *  file description) and close the file; the flash stays in it. The counters stay readable.
 *  Main thread, after the last operation. */
void posix_flash_close(void);

/**
 * Give the open flash file up at once, keeping it open: no store of this process reaches the
 * file any more, and its lock is released (flock LOCK_UN). The lock belongs to the open file
 * description (flock(2)), which the mapping refers to as the descriptor does: without this
 * call it lasts as long as any reference to the description, also one that outlives the
 * process or its image (while another process holds a reference to the memory map,
 * posix_retained.c). The mapping is replaced, at its address and in one step, by a private
 * copy-on-write mapping of the file: the device's reads still come from the file, and a
 * program or erase made after the call, by a thread that is still running when another ends
 * the run, changes this process's copy only. From the call on another process can open and
 * lock the file. If the mapping cannot be replaced, the lock is kept (it then goes with the
 * memory map). Does nothing without a flash; a second call changes nothing in the file.
 * For every end of a run that does not get to posix_flash_close(): the bootloader directly
 * before the exec of the image, which opens and locks the file itself, and directly before
 * _exit() the power cut without a kernel, plat_reset() and the shutdown guard
 * (posix_lifecycle.c). The context that ends the run, any thread: two system calls (mmap,
 * flock), and no lock of this process taken.
 */
void posix_flash_unlock(void);

/** What the flash counted so far. Any thread. */
void posix_flash_get_info(posix_flash_info_t *out);

#endif /* FCN_POSIX_FLASH_H */
