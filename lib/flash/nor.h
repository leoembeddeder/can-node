/* SPDX-License-Identifier: MIT */
/**
 * @file
 * A model of a NOR flash with power cuts: flash_dev_t (flash.h) over memory the caller gives,
 * with counters, the time each operation would take on the modelled part, and cuts of the
 * supply that tear an operation the way a NOR part can be torn.
 *
 * WHAT IT IS FOR. Fail-safe update code is only as good as the evidence that it survives a
 * loss of supply in every operation it makes. This model lets a test cut any program or erase
 * at any point of it and see what a real part could have left behind, the same way every time
 * for the same seed. The host platform drives it over a mapped file (plat_flash.h), so that the
 * node and the bootloader run on it as on a part; the unit tests of lib/boot run on it over a
 * buffer.
 *
 * THE ARRAY. The device's bytes are the caller's memory, geometry.size bytes, used in place:
 * the model keeps no copy, and what the memory holds when nor_init() is called is the content
 * of the device (the host maps its flash file there; a test fills a buffer with 0xFF for an
 * erased part). The operations follow every rule of flash.h, write-once included.
 *
 * COUNTERS (nor_get_stats). Every call is counted once, in the first of these that applies:
 * `unpowered` (answered FLASH_E_POWER_LOST because the supply was gone), `refused` (refused by
 * the range, alignment or write-once check), `reads`, `programs`, `erases`. A program or erase
 * that was cut is counted as a program or erase. `units_programmed` counts the units programs
 * started on: a whole program's units, or for a torn one the units before the cut and the
 * torn unit. The wear: every erase of a sector, a torn one included (its cells went through
 * part of an erase), adds one to that sector's count in the caller's erase_counts array, which
 * stops at UINT32_MAX.
 *
 * TIME. The model takes no time; it says how long each operation would take on the modelled
 * part, from the figures in nor_timing_t: a program takes page_program_ns, whatever its length
 * (a datasheet gives the time of a page program, tPP; a part programs a shorter page somewhat
 * faster, which is not modelled, so short programs are charged a whole page), an erase takes
 * sector_erase_ns, a read takes no time (on a serial part it costs the bus transfer, which is
 * left out), a refused or unpowered call takes no time. A torn operation takes the time up to
 * the moment of its cut (POWER CUTS). nor_last_duration_ns() gives the time of the latest call;
 * the platform waits that long, and busy_ns sums it.
 *
 * POWER CUTS. nor_arm_cut(k, position, seed) arms a cut of the k-th program or erase from now
 * (k = 1: the next one). Only calls that pass their checks count: a read changes nothing in the
 * array, so a cut during a read leaves what a cut at the start of the next program or erase
 * that changes nothing leaves (below), and a refused call does nothing at all. The operation
 * that is cut does this, and returns FLASH_E_POWER_LOST:
 *
 *  - A PROGRAM of n units has the positions 0 to n - 1, one per unit. A cut at position p
 *    leaves the units before p written (old AND src), unit p with a subset of its 1-to-0
 *    transitions done, and the units after p untouched.
 *  - An ERASE has the positions 0 to NOR_ERASE_POSITIONS - 1, sixteenths of its duration. A
 *    cut at any of them leaves a subset of the sector's 0 bits set to 1: none, some or all of
 *    them. A sector is erased as a whole, not byte by byte, so the bits that read 1 after a
 *    cut can lie anywhere in it; later positions leave more of them on average.
 *
 * Which subset, for a given seed: the cut moment t, and the moments at which the transitions of
 * the torn unit (or of the sector) would happen, are drawn from the seed. The transitions
 * happen at moments spread over a window [a, b] of the unit's or the erase's duration, whose
 * ends are drawn too (cells of one unit or sector change together, within a spread), and the
 * transitions before t are done. A cut before the window leaves none of them, a cut after it
 * all of them: both happen often (each in about a third of the seeds for a program; for an
 * erase, rarely all early in it and rarely none late in it), and in between any subset can
 * occur. A cut of a unit with all its transitions done is a program that completed but whose
 * caller lost the supply before it saw the result; a cut with none done leaves the unit
 * erased.
 *
 * The draws come from SplitMix64 (Steele, Lea and Flood, 2014) started from the seed, in a
 * fixed order: the position when it is to be drawn (NOR_CUT_SEEDED_POSITION), the cut moment,
 * the window, then one moment per transition in the order of the bits (byte by byte from the
 * lowest address, bit 0 first). So the same seed and the same operation on the same content
 * leave the same result on every run and every machine. No rand(), no clock.
 *
 * After a cut, and until nor_power_on(), the supply is gone: every call returns
 * FLASH_E_POWER_LOST and changes nothing. A cut fires once; nor_arm_cut() arms the next one.
 *
 * LIMITS OF THE MODEL. A real part can do things this model does not:
 *  - no weak or unstable bits: a torn bit is a stable 0 or 1 and reads the same every time,
 *    while a cell left between the two levels can read differently from one read to the next
 *    and change on its own later;
 *  - no retention errors and no read disturb: a bit never changes without a program or erase;
 *  - a program tears in the order of its units, lowest address first. A part programs a page in
 *    an order of its own, often many bits at once, so a cut can leave bits of any unit of the
 *    page done. A program of one unit (a commit or attempt unit) tears the same way in both;
 *    code that relies on the order of the units within one longer program would not be tested
 *    against what a part can do, and must not rely on it;
 *  - no erase or program suspend, and no pre-programming before an erase (some parts first
 *    program a sector to 0 and then erase it; cut early, they leave bits cleared that were 1);
 *  - no wear-out: a sector works however often it is erased (the counts say how often);
 *  - no error-correcting code: a unit reads exactly its data bits. A unit programmed with
 *    0xFF, or torn before any bit changed, reads erased, and the model accepts a program of it
 *    again. On a part with a code per unit its check bits may already be written, and that
 *    program fails or the unit reads with an error. Code above flash.h that programs a unit a
 *    cut may have reached because it reads erased (lib/boot does: the next record can go into
 *    a torn entry that reads erased, the next attempt into such a field) is not tested against
 *    that here: on such a part the driver has to report a unit it cannot read cleanly as not
 *    erased (lib/boot's LIMITS);
 *  - a read takes no time, and a program of part of a page as long as a whole page.
 *
 * CONTEXT. Pure C: no globals, no allocation, no clock, no OS call; the state is the caller's
 * nor_t. Calls on one nor_t must not run at the same time; the caller serialises them. The
 * functions never block.
 */
#ifndef FCN_FLASH_NOR_H
#define FCN_FLASH_NOR_H

#include <stdbool.h>
#include <stdint.h>

#include "flash/flash.h"

/** The positions of an erase for a cut: sixteenths of its duration. */
#define NOR_ERASE_POSITIONS 16U

/** The steps in which the model divides one position's duration for the cut moment and the
 *  moments of the transitions. */
#define NOR_CUT_RESOLUTION 65536U

/** A position for nor_arm_cut(): the seed chooses it, uniformly among the positions of the
 *  operation that is cut. */
#define NOR_CUT_SEEDED_POSITION UINT32_MAX

/** The figures of the modelled part, in nanoseconds. */
typedef struct {
    uint32_t page_program_ns; /**< one program, of any length within a page (tPP) */
    uint32_t sector_erase_ns; /**< one sector erase (tSE) */
} nor_timing_t;

/** What nor_init() needs. The memory and the array stay the caller's and must outlive the
 *  model. */
typedef struct {
    flash_geometry_t geometry; /**< valid as nor_geometry_valid() says */
    nor_timing_t timing;
    uint8_t *memory;        /**< geometry.size bytes: the array, used in place */
    uint32_t *erase_counts; /**< geometry.size / geometry.sector_size counters, set to 0 by
                                 nor_init() */
} nor_config_t;

/** The kind of operation a cut tore. */
typedef enum {
    NOR_OP_NONE = 0, /**< no cut has fired */
    NOR_OP_PROGRAM,
    NOR_OP_ERASE
} nor_op_t;

/** What the latest cut did (nor_last_cut). */
typedef struct {
    nor_op_t op;          /**< NOR_OP_NONE: no cut has fired since nor_init() */
    uint64_t operation;   /**< its number among the programs and erases since nor_init(), from 1 */
    uint32_t addr;        /**< the call's address */
    uint32_t len;         /**< the call's length; for an erase the sector size */
    uint32_t position;    /**< the position cut, after NOR_CUT_SEEDED_POSITION was resolved and
                               a position past the end was taken as the last one */
    uint32_t positions;   /**< the operation's positions: its units, or NOR_ERASE_POSITIONS */
    uint32_t seed;        /**< the seed of the cut */
    uint32_t transitions; /**< the bits the torn unit (program) or the sector (erase) would
                               have changed had the operation completed */
    uint32_t done;        /**< of those, the bits the cut left changed */
    uint32_t duration_ns; /**< the time the operation ran until the cut */
} nor_cut_t;

/** The counters; see COUNTERS. */
typedef struct {
    uint64_t reads;            /**< reads done */
    uint64_t bytes_read;       /**< bytes they copied */
    uint64_t programs;         /**< programs done, torn ones included */
    uint64_t units_programmed; /**< units programs started on */
    uint64_t erases;           /**< erases done, torn ones included */
    uint64_t refused;          /**< calls refused with RANGE, ALIGN or NOT_ERASED */
    uint64_t unpowered;        /**< calls answered POWER_LOST because the supply was gone */
    uint64_t busy_ns;          /**< the modelled time of every call */
    uint32_t cuts;             /**< cuts that fired */
} nor_stats_t;

/** The state of one modelled device: caller memory, set up by nor_init(). The members are the
 *  model's; read them through the functions below. */
typedef struct {
    flash_geometry_t geometry;
    nor_timing_t timing;
    uint8_t *memory;
    uint32_t *erase_counts;
    nor_stats_t stats;
    nor_cut_t last_cut;
    uint32_t last_duration_ns;
    uint32_t cut_countdown; /**< programs and erases until the armed cut, 1 = the next; 0: none */
    uint32_t cut_position;
    uint32_t cut_seed;
    bool powered;
} nor_t;

/**
 * True for a geometry flash.h allows: every size a power of two (size a multiple of
 * sector_size, not 0), unit_size dividing page_size, page_size dividing sector_size. Any
 * context.
 */
bool nor_geometry_valid(const flash_geometry_t *geometry);

/**
 * Set up `nor` over the configuration: powered, no cut armed, every counter 0, every sector's
 * erase count 0; the memory is left as it is. Returns false, and leaves the model unusable
 * (every operation then answers FLASH_E_POWER_LOST), for a NULL argument, a NULL memory or
 * erase_counts, or a geometry nor_geometry_valid() refuses.
 */
bool nor_init(nor_t *nor, const nor_config_t *config);

/** Fill `dev` with the device that `nor` models: its geometry, `nor` as the context, and the
 *  three operations below. */
void nor_device(nor_t *nor, flash_dev_t *dev);

/** flash.h's read on the model. A NULL `nor` answers FLASH_E_POWER_LOST (no device, no
 *  supply; nothing is counted), a NULL `dst` FLASH_E_RANGE (counted as refused). */
flash_status_t nor_read(nor_t *nor, uint32_t addr, uint8_t *dst, uint32_t len);

/** flash.h's program on the model, cut if a cut is armed for it. A NULL `nor` answers
 *  FLASH_E_POWER_LOST, a NULL `src` FLASH_E_RANGE (counted as refused). */
flash_status_t nor_program(nor_t *nor, uint32_t addr, const uint8_t *src, uint32_t len);

/** flash.h's erase on the model, cut if a cut is armed for it. A NULL `nor` answers
 *  FLASH_E_POWER_LOST. */
flash_status_t nor_erase(nor_t *nor, uint32_t addr);

/**
 * Arm a cut of the `operation`-th program or erase that passes its checks from now on (1: the
 * next), at `position` within it (see POWER CUTS; NOR_CUT_SEEDED_POSITION: drawn from the seed;
 * a position past the operation's last is its last), with `seed`. Replaces a cut armed before
 * and not yet fired. `operation` 0 disarms, as nor_disarm_cut(). While the supply is gone
 * nothing is counted, so the count resumes after nor_power_on().
 */
void nor_arm_cut(nor_t *nor, uint32_t operation, uint32_t position, uint32_t seed);

/** Disarm the armed cut, if any. */
void nor_disarm_cut(nor_t *nor);

/** The programs and erases until the armed cut, 1 if the next one is cut; 0 if no cut is
 *  armed. */
uint32_t nor_cut_countdown(const nor_t *nor);

/** Copy what the latest cut did into `out`; returns false if no cut has fired since
 *  nor_init() (then `out` has op NOR_OP_NONE). */
bool nor_last_cut(const nor_t *nor, nor_cut_t *out);

/** False after a cut and until nor_power_on(). */
bool nor_powered(const nor_t *nor);

/** The supply is back: operations work again. The array keeps what the cut left. */
void nor_power_on(nor_t *nor);

/** Copy the counters into `out`. */
void nor_get_stats(const nor_t *nor, nor_stats_t *out);

/** The erases of sector `sector` (its address / sector_size) since nor_init(), or 0 for a
 *  sector that does not exist. */
uint32_t nor_sector_erases(const nor_t *nor, uint32_t sector);

/** The modelled time of the latest call, in nanoseconds (see TIME); 0 before the first. */
uint32_t nor_last_duration_ns(const nor_t *nor);

#endif /* FCN_FLASH_NOR_H */
