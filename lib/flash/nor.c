/* SPDX-License-Identifier: MIT */
#include "flash/nor.h"

#include <stddef.h>

/*
 * What the model does is described in nor.h; the comments here say how. Between any two calls
 * on an initialised nor_t:
 *
 *   - cut_countdown is 0 (no cut armed) or the number of programs and erases still to pass
 *     before the cut, counting the one that is cut;
 *   - last_cut.op is NOR_OP_NONE until the first cut, and describes the latest one after it;
 *   - powered is false only after a cut and before nor_power_on() (or after a refused
 *     nor_init()).
 */

/* SplitMix64's increment and the constants of its mixing function (Steele, Lea and Flood,
 * "Fast splittable pseudorandom number generators", OOPSLA 2014). */
#define SPLITMIX_GAMMA 0x9E3779B97F4A7C15ULL
#define SPLITMIX_MUL1  0xBF58476D1CE4E5B9ULL
#define SPLITMIX_MUL2  0x94D049BB133111EBULL

/* The largest sector the model takes: its bits must fit a uint32_t count. */
#define MAX_SECTOR_SIZE 0x10000000UL

/* The timeline of an erase, in steps: every position, NOR_CUT_RESOLUTION steps each. */
#define ERASE_STEPS (NOR_ERASE_POSITIONS * NOR_CUT_RESOLUTION)

_Static_assert(NOR_ERASE_POSITIONS >= 1U, "an erase has at least one position");
_Static_assert((NOR_CUT_RESOLUTION & (NOR_CUT_RESOLUTION - 1U)) == 0U,
               "the resolution is a power of two");
/* rng_below() takes bounds up to UINT32_MAX, and the largest is ERASE_STEPS + 1. */
_Static_assert(ERASE_STEPS < 0x7FFFFFFFU, "the erase timeline fits a uint32_t bound");
/* (p * R + t) << 16 in torn_program() stays below 2^64 for p < 2^28 units of a page. */
_Static_assert(NOR_CUT_RESOLUTION <= 65536U, "the program timeline fits the duration's arithmetic");

typedef struct {
    uint64_t state;
} rng_t;

/* SplitMix64 started from `seed`: its output number k is mix(seed + k * gamma). */
static void rng_seed(rng_t *rng, uint32_t seed)
{
    rng->state = (uint64_t)seed;
}

static uint64_t rng_next(rng_t *rng)
{
    uint64_t z;

    rng->state += SPLITMIX_GAMMA;
    z = rng->state;
    z = (z ^ (z >> 30U)) * SPLITMIX_MUL1;
    z = (z ^ (z >> 27U)) * SPLITMIX_MUL2;
    return z ^ (z >> 31U);
}

/* A value in 0 .. bound - 1, bound >= 1: the high 32 bits of a draw scaled to the bound
 * (multiply and shift, which leaves a bias below bound / 2^32 and needs no division). */
static uint32_t rng_below(rng_t *rng, uint32_t bound)
{
    return (uint32_t)(((rng_next(rng) >> 32U) * (uint64_t)bound) >> 32U);
}

static bool power_of_two(uint32_t value)
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

bool nor_geometry_valid(const flash_geometry_t *geometry)
{
    bool valid = false;

    if (geometry != NULL) {
        /* Powers of two divide each other exactly when the smaller is not larger. */
        valid = power_of_two(geometry->unit_size) && power_of_two(geometry->page_size) &&
                power_of_two(geometry->sector_size) &&
                (geometry->unit_size <= geometry->page_size) &&
                (geometry->page_size <= geometry->sector_size) &&
                (geometry->sector_size <= MAX_SECTOR_SIZE) && (geometry->size != 0U) &&
                ((geometry->size % geometry->sector_size) == 0U);
    }

    return valid;
}

static uint32_t sector_count(const nor_t *nor)
{
    return nor->geometry.size / nor->geometry.sector_size;
}

/* Every member to what a refused nor_init() leaves: no memory, unpowered, nothing counted. */
static void clear_state(nor_t *nor)
{
    nor->geometry.size = 0U;
    nor->geometry.sector_size = 0U;
    nor->geometry.page_size = 0U;
    nor->geometry.unit_size = 0U;
    nor->timing.page_program_ns = 0U;
    nor->timing.sector_erase_ns = 0U;
    nor->memory = NULL;
    nor->erase_counts = NULL;
    nor->stats.reads = 0U;
    nor->stats.bytes_read = 0U;
    nor->stats.programs = 0U;
    nor->stats.units_programmed = 0U;
    nor->stats.erases = 0U;
    nor->stats.refused = 0U;
    nor->stats.unpowered = 0U;
    nor->stats.busy_ns = 0U;
    nor->stats.cuts = 0U;
    nor->last_cut.op = NOR_OP_NONE;
    nor->last_cut.operation = 0U;
    nor->last_cut.addr = 0U;
    nor->last_cut.len = 0U;
    nor->last_cut.position = 0U;
    nor->last_cut.positions = 0U;
    nor->last_cut.seed = 0U;
    nor->last_cut.transitions = 0U;
    nor->last_cut.done = 0U;
    nor->last_cut.duration_ns = 0U;
    nor->last_duration_ns = 0U;
    nor->cut_countdown = 0U;
    nor->cut_position = 0U;
    nor->cut_seed = 0U;
    nor->powered = false;
}

bool nor_init(nor_t *nor, const nor_config_t *config)
{
    bool ok = false;

    if (nor != NULL) {
        clear_state(nor);

        if ((config != NULL) && (config->memory != NULL) && (config->erase_counts != NULL) &&
            nor_geometry_valid(&config->geometry)) {
            nor->geometry = config->geometry;
            nor->timing = config->timing;
            nor->memory = config->memory;
            nor->erase_counts = config->erase_counts;

            for (uint32_t sector = 0U; sector < sector_count(nor); sector++) {
                nor->erase_counts[sector] = 0U;
            }

            nor->powered = true;
            ok = true;
        }
    }

    return ok;
}

/* The first check of every call: the supply. A call without it is counted as unpowered. */
static bool supplied(nor_t *nor)
{
    bool powered = false;

    if (nor != NULL) {
        nor->last_duration_ns = 0U;
        powered = nor->powered;

        if (!powered) {
            nor->stats.unpowered++;
        }
    }

    return powered;
}

/* True if the len bytes at addr lie inside the device and len is not 0. */
static bool in_range(const nor_t *nor, uint32_t addr, uint32_t len)
{
    return (len != 0U) && (addr < nor->geometry.size) && (len <= (nor->geometry.size - addr));
}

/* True if a program of len bytes at addr writes whole units inside one page; the range has
 * been checked, so addr + len - 1 does not wrap. */
static bool program_aligned(const nor_t *nor, uint32_t addr, uint32_t len)
{
    const uint32_t unit = nor->geometry.unit_size;
    const uint32_t page = nor->geometry.page_size;

    return ((addr % unit) == 0U) && ((len % unit) == 0U) &&
           ((addr / page) == ((addr + len - 1U) / page));
}

/* True if every byte of the range reads 0xFF; with whole units checked, every unit does. */
static bool erased(const nor_t *nor, uint32_t addr, uint32_t len)
{
    bool all_ff = true;

    for (uint32_t i = 0U; (i < len) && all_ff; i++) {
        all_ff = (nor->memory[addr + i] == 0xFFU);
    }

    return all_ff;
}

/* Count one program or erase that passed its checks against the armed cut; true if it is the
 * one to cut. */
static bool cut_now(nor_t *nor)
{
    bool cut = false;

    if (nor->cut_countdown == 1U) {
        nor->cut_countdown = 0U;
        cut = true;
    } else if (nor->cut_countdown > 1U) {
        nor->cut_countdown--;
    } else {
        /* no cut armed */
    }

    return cut;
}

/* Start the record of the cut of the operation just counted, and seed its draws. The position
 * is resolved here: drawn first, from the seed, or the last one for a position past the end. */
static void begin_cut(nor_t *nor, nor_op_t op, uint32_t addr, uint32_t len, uint32_t positions,
                      rng_t *rng)
{
    uint32_t position = nor->cut_position;

    rng_seed(rng, nor->cut_seed);

    if (position == NOR_CUT_SEEDED_POSITION) {
        position = rng_below(rng, positions);
    } else if (position >= positions) {
        position = positions - 1U;
    } else {
        /* as armed */
    }

    nor->last_cut.op = op;
    nor->last_cut.operation = nor->stats.programs + nor->stats.erases;
    nor->last_cut.addr = addr;
    nor->last_cut.len = len;
    nor->last_cut.position = position;
    nor->last_cut.positions = positions;
    nor->last_cut.seed = nor->cut_seed;
    nor->last_cut.transitions = 0U;
    nor->last_cut.done = 0U;
}

/* The end of every cut: the supply is gone, and the call took `duration_ns`. */
static flash_status_t end_cut(nor_t *nor, uint32_t duration_ns)
{
    nor->last_cut.duration_ns = duration_ns;
    nor->stats.cuts++;
    nor->powered = false;
    nor->last_duration_ns = duration_ns;
    nor->stats.busy_ns += duration_ns;
    return FLASH_E_POWER_LOST;
}

/* The window of moments [*a, *b] over a timeline of `span` + 1 steps (0 .. span). */
static void draw_window(rng_t *rng, uint32_t span, uint32_t *a, uint32_t *b)
{
    const uint32_t first = rng_below(rng, span + 1U);
    const uint32_t second = rng_below(rng, span + 1U);

    *a = (first < second) ? first : second;
    *b = (first < second) ? second : first;
}

/* Whether one transition is done at a cut at moment t: its moment lies in [a, b]. */
static bool transition_done(rng_t *rng, uint32_t a, uint32_t b, uint32_t t)
{
    const uint32_t moment = a + rng_below(rng, (b - a) + 1U);

    return moment < t;
}

/* The program of `len` bytes at `addr` cut as nor.h says; the checks have passed. */
static flash_status_t torn_program(nor_t *nor, uint32_t addr, const uint8_t *src, uint32_t len)
{
    const uint32_t unit = nor->geometry.unit_size;
    const uint32_t units = len / unit;
    rng_t rng;
    uint32_t a = 0U;
    uint32_t b = 0U;

    begin_cut(nor, NOR_OP_PROGRAM, addr, len, units, &rng);

    const uint32_t position = nor->last_cut.position;
    const uint32_t start = position * unit;
    /* The cut moment within the torn unit, then the window of its transitions, both over the
     * unit's own timeline 0 .. NOR_CUT_RESOLUTION. */
    const uint32_t t = rng_below(&rng, NOR_CUT_RESOLUTION + 1U);

    draw_window(&rng, NOR_CUT_RESOLUTION, &a, &b);

    for (uint32_t i = 0U; i < start; i++) {
        nor->memory[addr + i] &= src[i];
    }

    for (uint32_t i = start; i < (start + unit); i++) {
        const uint32_t old = nor->memory[addr + i];
        const uint32_t clear = old & ~(uint32_t)src[i] & 0xFFU; /* the 1-to-0 transitions */
        uint32_t now = old;

        for (uint32_t bit = 0U; bit < 8U; bit++) {
            const uint32_t mask = (uint32_t)1U << bit;

            if ((clear & mask) != 0U) {
                nor->last_cut.transitions++;

                if (transition_done(&rng, a, b, t)) {
                    now &= ~mask;
                    nor->last_cut.done++;
                }
            }
        }

        nor->memory[addr + i] = (uint8_t)now;
    }

    nor->stats.units_programmed += (uint64_t)position + 1U;

    /* The time until the cut: (position + t / R) / units of a page program, computed as a
     * fraction of 2^16 so that the product stays within 64 bits. */
    const uint64_t elapsed = ((uint64_t)position * NOR_CUT_RESOLUTION) + t;
    const uint64_t fraction = (elapsed << 16U) / ((uint64_t)units * NOR_CUT_RESOLUTION);
    const uint64_t duration = ((uint64_t)nor->timing.page_program_ns * fraction) >> 16U;

    return end_cut(nor, (uint32_t)duration);
}

flash_status_t nor_read(nor_t *nor, uint32_t addr, uint8_t *dst, uint32_t len)
{
    flash_status_t status = FLASH_E_POWER_LOST;

    if (supplied(nor)) {
        if ((dst == NULL) || !in_range(nor, addr, len)) {
            nor->stats.refused++;
            status = FLASH_E_RANGE;
        } else {
            for (uint32_t i = 0U; i < len; i++) {
                dst[i] = nor->memory[addr + i];
            }

            nor->stats.reads++;
            nor->stats.bytes_read += len;
            status = FLASH_OK;
        }
    }

    return status;
}

flash_status_t nor_program(nor_t *nor, uint32_t addr, const uint8_t *src, uint32_t len)
{
    flash_status_t status = FLASH_E_POWER_LOST;

    if (supplied(nor)) {
        if ((src == NULL) || !in_range(nor, addr, len)) {
            status = FLASH_E_RANGE;
        } else if (!program_aligned(nor, addr, len)) {
            status = FLASH_E_ALIGN;
        } else if (!erased(nor, addr, len)) {
            status = FLASH_E_NOT_ERASED;
        } else {
            status = FLASH_OK;
        }

        if (status != FLASH_OK) {
            nor->stats.refused++;
        } else {
            nor->stats.programs++;

            if (cut_now(nor)) {
                status = torn_program(nor, addr, src, len);
            } else {
                for (uint32_t i = 0U; i < len; i++) {
                    nor->memory[addr + i] &= src[i];
                }

                nor->stats.units_programmed += len / nor->geometry.unit_size;
                nor->last_duration_ns = nor->timing.page_program_ns;
                nor->stats.busy_ns += nor->timing.page_program_ns;
            }
        }
    }

    return status;
}

/* The erase of the sector at `addr` cut as nor.h says; the checks have passed. */
static flash_status_t torn_erase(nor_t *nor, uint32_t addr)
{
    rng_t rng;
    uint32_t a = 0U;
    uint32_t b = 0U;

    begin_cut(nor, NOR_OP_ERASE, addr, nor->geometry.sector_size, NOR_ERASE_POSITIONS, &rng);

    /* The cut moment within its position, then the window of the sector's transitions, both
     * over the erase's timeline 0 .. ERASE_STEPS. */
    const uint32_t t =
        (nor->last_cut.position * NOR_CUT_RESOLUTION) + rng_below(&rng, NOR_CUT_RESOLUTION);

    draw_window(&rng, ERASE_STEPS, &a, &b);

    for (uint32_t i = 0U; i < nor->geometry.sector_size; i++) {
        const uint32_t old = nor->memory[addr + i];
        uint32_t now = old;

        for (uint32_t bit = 0U; bit < 8U; bit++) {
            const uint32_t mask = (uint32_t)1U << bit;

            if ((old & mask) == 0U) {
                nor->last_cut.transitions++;

                if (transition_done(&rng, a, b, t)) {
                    now |= mask;
                    nor->last_cut.done++;
                }
            }
        }

        nor->memory[addr + i] = (uint8_t)now;
    }

    const uint64_t duration = ((uint64_t)nor->timing.sector_erase_ns * t) /
                              ((uint64_t)NOR_ERASE_POSITIONS * (uint64_t)NOR_CUT_RESOLUTION);

    return end_cut(nor, (uint32_t)duration);
}

flash_status_t nor_erase(nor_t *nor, uint32_t addr)
{
    flash_status_t status = FLASH_E_POWER_LOST;

    if (supplied(nor)) {
        if (addr >= nor->geometry.size) {
            nor->stats.refused++;
            status = FLASH_E_RANGE;
        } else if ((addr % nor->geometry.sector_size) != 0U) {
            nor->stats.refused++;
            status = FLASH_E_ALIGN;
        } else {
            uint32_t *const count = &nor->erase_counts[addr / nor->geometry.sector_size];

            nor->stats.erases++;

            if (*count < UINT32_MAX) {
                (*count)++;
            }

            if (cut_now(nor)) {
                status = torn_erase(nor, addr);
            } else {
                for (uint32_t i = 0U; i < nor->geometry.sector_size; i++) {
                    nor->memory[addr + i] = 0xFFU;
                }

                nor->last_duration_ns = nor->timing.sector_erase_ns;
                nor->stats.busy_ns += nor->timing.sector_erase_ns;
                status = FLASH_OK;
            }
        }
    }

    return status;
}

/* The adapters of flash_dev_t: the context is the nor_t that nor_device() put there. */
static flash_status_t dev_read(void *ctx, uint32_t addr, uint8_t *dst, uint32_t len)
{
    return nor_read((nor_t *)ctx, addr, dst, len);
}

static flash_status_t dev_program(void *ctx, uint32_t addr, const uint8_t *src, uint32_t len)
{
    return nor_program((nor_t *)ctx, addr, src, len);
}

static flash_status_t dev_erase(void *ctx, uint32_t addr)
{
    return nor_erase((nor_t *)ctx, addr);
}

void nor_device(nor_t *nor, flash_dev_t *dev)
{
    if ((nor != NULL) && (dev != NULL)) {
        dev->geometry = nor->geometry;
        dev->ctx = nor;
        dev->read = dev_read;
        dev->program = dev_program;
        dev->erase = dev_erase;
    }
}

void nor_arm_cut(nor_t *nor, uint32_t operation, uint32_t position, uint32_t seed)
{
    if (nor != NULL) {
        nor->cut_countdown = operation;
        nor->cut_position = position;
        nor->cut_seed = seed;
    }
}

void nor_disarm_cut(nor_t *nor)
{
    nor_arm_cut(nor, 0U, 0U, 0U);
}

uint32_t nor_cut_countdown(const nor_t *nor)
{
    return (nor != NULL) ? nor->cut_countdown : 0U;
}

bool nor_last_cut(const nor_t *nor, nor_cut_t *out)
{
    bool fired = false;

    if ((nor != NULL) && (out != NULL)) {
        *out = nor->last_cut;
        fired = (nor->last_cut.op != NOR_OP_NONE);
    }

    return fired;
}

bool nor_powered(const nor_t *nor)
{
    return (nor != NULL) && nor->powered;
}

void nor_power_on(nor_t *nor)
{
    /* A model that nor_init() refused has no memory and stays without supply. */
    if ((nor != NULL) && (nor->memory != NULL)) {
        nor->powered = true;
    }
}

void nor_get_stats(const nor_t *nor, nor_stats_t *out)
{
    if ((nor != NULL) && (out != NULL)) {
        *out = nor->stats;
    }
}

uint32_t nor_sector_erases(const nor_t *nor, uint32_t sector)
{
    uint32_t count = 0U;

    if ((nor != NULL) && (nor->erase_counts != NULL) && (sector < sector_count(nor))) {
        count = nor->erase_counts[sector];
    }

    return count;
}

uint32_t nor_last_duration_ns(const nor_t *nor)
{
    return (nor != NULL) ? nor->last_duration_ns : 0U;
}
