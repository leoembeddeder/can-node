/* SPDX-License-Identifier: MIT */
/*
 * THE POWER-CUT CAMPAIGN of boot control (lib/boot): every sequence of operations of the update
 * life cycle, cut at every program and erase it makes and at every unit position within each,
 * with several seeds, on three geometries; after every cut the supply comes back, a fresh boot_t
 * is set up as after a reset, and the checks below run.
 *
 * HOW. A sequence is a setup (run once, without cuts, and kept as a snapshot of the flash) and a
 * list of steps (select, confirm, an update through the slot writer, an update whose image does
 * not validate). The steps are first run without a cut: the REFERENCE RUN. It numbers the
 * program and erase operations, notes which API call made each of them, and after every call
 * the abstract state the metadata reads as (bt_ref_t: per slot its state and image id, and the
 * attempts of a trial). Every call of the reference run is checked against the reference model
 * of boot_test_support.c, written from the state machine of boot.h.
 *
 * Then, for every operation i of the reference run, the steps are run again from the snapshot
 * with the supply cut:
 *
 *   - just before operation i (it does not happen at all), and
 *   - during operation i, torn by the flash model: for a program at every unit position, with
 *     the seeds TEAR_SEEDS; for an erase with the seeds ERASE_SEEDS,
 *
 * and once with no cut at all (the cut "after the last operation"). After each, with the call
 * that made operation i called k:
 *
 *   atomicity  the metadata reads as the state before call k or the state after it, never as
 *              anything else; the metadata decodes (a valid record exists unless both are the
 *              factory state). This holds the durability of every earlier call too.
 *   (a)        select boots something: every sequence starts with a validated image that is, or
 *              is about to be, CONFIRMED.
 *   (b)        the slot select boots validates, with the id the metadata names for it; and the
 *              choice is the one the reference model makes for the state read.
 *   (c)        a TRIAL that has attempts left and validates is booted next, as a trial.
 *   (d)        then select is called again and again, as after a trial that crashes each time:
 *              the trial is booted at most BOOT_MAX_ATTEMPTS times in all, counting the boots of
 *              the reference run before the cut, and is then rolled back.
 *   (e)        the CONFIRMED image read after the cut stays the CONFIRMED one and is booted
 *              after the rollback.
 *   (f)        the next update succeeds: an image through the slot writer into the other slot,
 *              booted as a trial, confirmed, booted as the CONFIRMED image.
 *
 * THE PARTS. The campaign is built three times, as three CTest tests that each stay well inside
 * the time CTest gives a unit test (60 s), also on a slower flash model or under coverage
 * instrumentation: BOOT_CUTS_PART 0 is the host geometry with an empty log
 * (test_boot_cuts_host), 1 the host geometry with the log filled first (test_boot_cuts_host_log),
 * 2 the two other geometries (test_boot_cuts_small).
 *
 * The flash model is the one of boot_test_flash.h (boot_test_nor.c by default). With it, seed s
 * does (s mod 9)/8 of the work of the torn operation: TEAR_SEEDS 1, 2, 4, 6, 8 leave 1/8, 1/4,
 * 1/2, 3/4 and all of the torn unit's transitions done (all of unit p and none of unit p + 1 is
 * also "none at p + 1"); the erase seeds 0..8 range from nothing to all of the erase. An erase
 * cut with seed s is made at position 15 s / 8 (0..15), which boot_test_nor.c ignores and a
 * model that tears an erase by the moment of the cut (lib/flash: sixteenths of its duration)
 * spreads over the erase. The cuts "just before" an operation are made by the tracer, whatever
 * the model, so a cut that does nothing of an operation, and one that completes it (= just
 * before the next), are part of the campaign on any model.
 *
 * THE LOG. A record whose sector is full erases the next metadata sector first (boot.h, WRITING
 * A RECORD). Each geometry runs some sequences again after `prefill` complete updates made
 * right after the adoption, chosen so that a record of every kind the steps write (BEGIN,
 * COMMIT, CONFIRM, ROLLBACK, REJECT, FALLBACK) is, in some run, the one that erases a metadata
 * sector; the campaign checks that every such run has a metadata erase among its operations.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime(), for the time the campaign takes */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "boot/boot.h"
#include "boot_test_flash.h"
#include "boot_test_support.h"
#include "unity.h"

#ifndef BOOT_CUTS_PART
#error "BOOT_CUTS_PART selects the part of the campaign: see tests/unit/CMakeLists.txt"
#endif

static const uint32_t TEAR_SEEDS[] = {1U, 2U, 4U, 6U, 8U};
#define TEAR_SEED_COUNT  ((uint32_t)(sizeof(TEAR_SEEDS) / sizeof(TEAR_SEEDS[0])))
#define ERASE_SEED_COUNT 9U

#define MAX_CALLS    512U
#define MAX_OPS      2048U
#define MAX_PREFILLS 10U

/* ------------------------------------------------------------------ sequences */

typedef enum {
    Q_END = 0,
    Q_FACTORY,       /* setup: image `arg` straight into slot A, metadata blank */
    Q_FACTORY_B,     /* setup: image `arg` straight into slot B */
    Q_DAMAGE_ACTIVE, /* setup: one payload bit of the CONFIRMED slot (A in the factory state) */
    Q_DAMAGE_OTHER,  /* setup: one payload bit of the other slot */
    Q_PREFILL,       /* setup: `arg` complete updates of the other slot, alternating images */
    Q_SELECT,        /* boot_select() */
    Q_CONFIRM,       /* boot_confirm() of the slot the last select booted */
    Q_UPDATE,        /* the slot writer: image `arg` into the slot that is not CONFIRMED */
    Q_UPDATE_BAD     /* the same with one payload byte changed in transfer: finish fails */
} q_kind_t;

typedef struct {
    q_kind_t kind;
    uint32_t arg;
} q_step_t;

/* Images of the campaign, sized for each geometry (campaign_images()). */
enum { I_FACTORY, I_PREVIOUS, I_MULTI, I_U2, I_U3, I_NEXT, I_FILL_A, I_FILL_B, I_COUNT };

typedef struct {
    const char *name;
    q_step_t setup[10];
    q_step_t steps[12];
} q_sequence_t;

#define S(kind)                                                                                    \
    {                                                                                              \
        (kind), 0U                                                                                 \
    }
#define SA(kind, a)                                                                                \
    {                                                                                              \
        (kind), (a)                                                                                \
    }

/* The sequences, by index. */
enum {
    SQ_ADOPT,
    SQ_ADOPT_B,
    SQ_UPDATE,
    SQ_ROLLBACK,
    SQ_REJECT,
    SQ_REJECTED_SLOT,
    SQ_TWO_UPDATES,
    SQ_FALLBACK,
    SQ_INVALID_THEN_VALID,
    SQ_COUNT
};

static const q_sequence_t s_sequences[] = {
    [SQ_ADOPT] = {"factory: adopt", {SA(Q_FACTORY, I_FACTORY)}, {S(Q_SELECT), S(Q_SELECT)}},
    [SQ_ADOPT_B] = {"factory: A broken, adopt B",
                    {SA(Q_FACTORY, I_FACTORY), SA(Q_FACTORY_B, I_U2), S(Q_DAMAGE_ACTIVE)},
                    {S(Q_SELECT), S(Q_SELECT)}},
    [SQ_UPDATE] = {"update of two sectors, boot, confirm",
                   {SA(Q_FACTORY, I_FACTORY), S(Q_SELECT)},
                   {SA(Q_UPDATE, I_MULTI), S(Q_SELECT), S(Q_CONFIRM), S(Q_SELECT)}},
    [SQ_ROLLBACK] = {"attempts run out, rollback",
                     {SA(Q_FACTORY, I_FACTORY), S(Q_SELECT), SA(Q_UPDATE, I_U2)},
                     {S(Q_SELECT), S(Q_SELECT), S(Q_SELECT), S(Q_SELECT), S(Q_SELECT)}},
    [SQ_REJECT] = {"a trial that does not validate",
                   {SA(Q_FACTORY, I_FACTORY), S(Q_SELECT), SA(Q_UPDATE, I_U2), S(Q_SELECT),
                    S(Q_DAMAGE_OTHER)},
                   {S(Q_SELECT), S(Q_SELECT)}},
    [SQ_REJECTED_SLOT] = {"begin over a rejected slot",
                          {SA(Q_FACTORY, I_FACTORY), S(Q_SELECT), SA(Q_UPDATE, I_U2), S(Q_SELECT),
                           S(Q_SELECT), S(Q_SELECT), S(Q_SELECT)},
                          {SA(Q_UPDATE, I_U3), S(Q_SELECT), S(Q_CONFIRM), S(Q_SELECT)}},
    [SQ_TWO_UPDATES] = {"two updates in a row",
                        {SA(Q_FACTORY, I_FACTORY), S(Q_SELECT)},
                        {SA(Q_UPDATE, I_U2), S(Q_SELECT), S(Q_CONFIRM), S(Q_SELECT),
                         SA(Q_UPDATE, I_U3), S(Q_SELECT), S(Q_CONFIRM), S(Q_SELECT)}},
    [SQ_FALLBACK] = {"the active image breaks: fallback",
                     {SA(Q_FACTORY, I_FACTORY), S(Q_SELECT), SA(Q_UPDATE, I_U2), S(Q_SELECT),
                      S(Q_CONFIRM), S(Q_DAMAGE_ACTIVE)},
                     {S(Q_SELECT), S(Q_SELECT)}},
    [SQ_INVALID_THEN_VALID] = {"an update that does not validate, then one that does",
                               {SA(Q_FACTORY, I_FACTORY), S(Q_SELECT)},
                               {SA(Q_UPDATE_BAD, I_U2), SA(Q_UPDATE, I_U3), S(Q_SELECT),
                                S(Q_CONFIRM)}},
};

#define SEQUENCES ((uint32_t)(sizeof(s_sequences) / sizeof(s_sequences[0])))
_Static_assert((sizeof(s_sequences) / sizeof(s_sequences[0])) == (size_t)SQ_COUNT,
               "one sequence per index");

/* ------------------------------------------------------------------ the bench of a geometry */

/* A sequence run again after `count` complete updates (0: the end of the list). */
typedef struct {
    uint32_t sequence;
    uint32_t count;
} prefill_t;

typedef struct {
    const char *name;
    bt_shape_t shape;
    uint32_t small_payload; /* bytes of the payload of the small images */
    uint32_t multi_payload; /* of I_MULTI: more than a sector */
    prefill_t prefills[MAX_PREFILLS];
} geometry_t;

static const geometry_t s_geometries[] = {
    {"host: 4096/256/8, 2 metadata sectors of 32 entries",
     {.sector_size = 4096U,
      .page_size = 256U,
      .unit_size = 8U,
      .meta_sectors = 2U,
      .slot_sectors = 3U,
      .spare_sectors = 0U},
     600U,
     4096U + 900U,
     /* 32 entries per sector: records 33, 65, 97 erase. After p updates the adoption and the
      * prefill are 1 + 3 p records; the setup of a sequence adds its own. */
     {{SQ_UPDATE, 10U},             /* COMMIT is record 33 */
      {SQ_TWO_UPDATES, 10U},        /* COMMIT 33 */
      {SQ_INVALID_THEN_VALID, 10U}, /* COMMIT 33 (the second begin writes no record) */
      {SQ_UPDATE, 21U},             /* BEGIN 65: the ring wraps to sector 0 */
      {SQ_TWO_UPDATES, 21U},        /* BEGIN 65 */
      {SQ_REJECTED_SLOT, 20U},      /* BEGIN 65, over the slot the ROLLBACK of the setup rejected */
      {SQ_UPDATE, 31U},             /* CONFIRM 97 */
      {SQ_ROLLBACK, 31U},           /* ROLLBACK 97 */
      {SQ_REJECT, 31U},             /* REJECT 97 */
      {SQ_FALLBACK, 20U}}},         /* FALLBACK 65 */
    {"1024/256/8, 2 metadata sectors of 8 entries",
     {.sector_size = 1024U,
      .page_size = 256U,
      .unit_size = 8U,
      .meta_sectors = 2U,
      .slot_sectors = 4U,
      .spare_sectors = 0U},
     400U,
     1024U + 600U,
     /* 8 entries per sector: records 9, 17, 25 erase */
     {{SQ_UPDATE, 2U},      /* COMMIT 9 */
      {SQ_TWO_UPDATES, 2U}, /* COMMIT 9 */
      {SQ_UPDATE, 5U},      /* BEGIN 17: the ring wraps */
      {SQ_UPDATE, 7U},      /* CONFIRM 25 */
      {SQ_ROLLBACK, 7U},    /* ROLLBACK 25 */
      {SQ_FALLBACK, 4U}}},  /* FALLBACK 17 */
    {"512/64/16, 3 metadata sectors of 2 entries, records over two pages",
     {.sector_size = 512U,
      .page_size = 64U,
      .unit_size = 16U,
      .meta_sectors = 3U,
      .slot_sectors = 6U,
      .spare_sectors = 0U},
     200U,
     512U + 300U,
     /* 2 entries per sector, 3 sectors: every odd record from 3 on erases */
     {{SQ_UPDATE, 1U}, {SQ_TWO_UPDATES, 1U}, {SQ_ROLLBACK, 1U}}},
};

#define GEOMETRIES ((uint32_t)(sizeof(s_geometries) / sizeof(s_geometries[0])))

static uint8_t s_image[I_COUNT][8192];
static uint32_t s_len[I_COUNT];
static uint8_t s_bad[8192];
static uint8_t s_snapshot[65536];
static uint32_t s_snapshot_len;
static char s_msg[512];
static char s_text_a[96];
static char s_text_b[96];
static char s_text_c[96];

static void campaign_images(const geometry_t *g)
{
    for (uint32_t i = 0U; i < I_COUNT; i++) {
        const uint32_t payload = (i == I_MULTI) ? g->multi_payload : (g->small_payload + (i * 8U));

        s_len[i] = bt_image_build(s_image[i], sizeof(s_image[i]), 0x0100U + i, 4000U + i, payload);
    }
}

/* ------------------------------------------------------------------ running the steps */

/* What one run of the steps does and sees. */
typedef struct {
    bool reference; /* the reference run: record everything, check against the model */
    bool stopped;   /* the supply was cut */
    uint8_t last_booted;
    uint32_t calls;
    /* reference run only */
    bt_ref_t model;                       /* the reference model */
    uint32_t call_end[MAX_CALLS];         /* operations made up to the end of call k */
    bt_ref_t state[MAX_CALLS + 1U];       /* state[k] before call k, state[k + 1] after it */
    uint32_t boots[MAX_CALLS + 1U];       /* trial boots of the TRIAL of state[k] before it */
    uint8_t trial_id[BOOT_IMAGE_ID_SIZE]; /* the TRIAL whose boots are being counted */
    uint32_t trial_boots;
} run_t;

static run_t s_run;

/* The TRIAL slot of a state, or BOOT_NO_SLOT. */
static uint8_t trial_of(const bt_ref_t *r)
{
    uint8_t slot = BOOT_NO_SLOT;

    for (uint8_t s = 0U; s < BOOT_SLOTS; s++) {
        if (r->state[s] == BOOT_SLOT_TRIAL) {
            slot = s;
        }
    }
    return slot;
}

/* After every API call: stop at a cut; in the reference run note the state and check it. */
static void after_call(run_t *run, boot_status_t status, bool trial_boot)
{
    if ((status == BOOT_E_POWER_LOST) || bt_cut_fired()) {
        run->stopped = true;
    }
    if (run->reference) {
        bt_ref_t now;
        bt_counts_t counts;
        uint8_t t;
        char where[96];

        TEST_ASSERT_FALSE_MESSAGE(run->stopped, "the reference run lost power");
        TEST_ASSERT_TRUE_MESSAGE(run->calls < MAX_CALLS, "more calls than MAX_CALLS");
        bt_counts(&counts);
        bt_ref_now(&now);
        (void)snprintf(where, sizeof(where), "reference run, call %lu", (unsigned long)run->calls);
        if (!bt_ref_equal(&now, &run->model)) {
            (void)snprintf(s_msg, sizeof(s_msg), "%s: model %s, flash %s", where,
                           bt_ref_text(&run->model, s_text_a, sizeof(s_text_a)),
                           bt_ref_text(&now, s_text_b, sizeof(s_text_b)));
            TEST_FAIL_MESSAGE(s_msg);
        }
        /* Boots of the trial, per trial: a new id (or a new COMMIT) starts the count again. */
        if (trial_boot) {
            run->trial_boots++;
        }
        t = trial_of(&now);
        if ((t != BOOT_NO_SLOT) && (memcmp(now.id[t], run->trial_id, BOOT_IMAGE_ID_SIZE) != 0)) {
            (void)memcpy(run->trial_id, now.id[t], BOOT_IMAGE_ID_SIZE);
            run->trial_boots = 0U;
        }
        run->call_end[run->calls] = counts.ops;
        run->state[run->calls + 1U] = now;
        run->boots[run->calls + 1U] = run->trial_boots;
    }
    run->calls++;
}

static void step_select(run_t *run)
{
    boot_selection_t sel;
    bt_ref_choice_t choice;
    bt_truth_t truth;
    boot_status_t status;

    (void)memset(&choice, 0, sizeof(choice));
    if (run->reference) {
        bt_truth(&truth);
        bt_ref_select(&run->model, &truth, &choice);
    }
    status = boot_select(&bt_boot, &sel);
    if (run->reference) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(choice.status, status, "reference select");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(choice.slot, sel.slot, "reference select");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(choice.attempt, sel.attempt, "reference select");
        TEST_ASSERT_EQUAL_INT_MESSAGE(choice.event, sel.event, "reference select");
    }
    if (status == BOOT_OK) {
        run->last_booted = sel.slot;
    }
    after_call(run, status, (status == BOOT_OK) && sel.trial);
}

static void step_confirm(run_t *run)
{
    boot_status_t status;

    if (run->reference) {
        TEST_ASSERT_EQUAL_INT(BOOT_OK, bt_ref_confirm(&run->model, run->last_booted));
    }
    status = boot_confirm(&bt_boot, run->last_booted);
    if (run->reference) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(BOOT_OK, status, "reference confirm");
    }
    after_call(run, status, false);
}

/* Chunk sizes of the writes: around units and pages. */
static const uint32_t s_chunks[] = {256U, 1U, 7U, 300U, 33U, 512U, 1000U, 8U, 255U};

static void step_update(run_t *run, uint32_t image, bool bad)
{
    boot_description_t d;
    const uint8_t *data = s_image[image];
    const uint32_t len = s_len[image];
    uint8_t target;
    boot_status_t status;
    uint32_t offset = 0U;
    uint32_t k = image;

    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_describe(&bt_boot, &d));
    target = (d.active_slot == 0U) ? 1U : 0U;
    if (bad) {
        (void)memcpy(s_bad, data, len);
        s_bad[BT_IMAGE_HEADER + 3U] ^= 0x01U;
        data = s_bad;
    }

    if (run->reference) {
        TEST_ASSERT_EQUAL_INT(BOOT_OK,
                              bt_ref_begin(&run->model, target, len, bt_slot_size(target)));
    }
    status = boot_writer_begin(&bt_writer, &bt_boot, target, len);
    if (run->reference) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(BOOT_OK, status, "reference begin");
    }
    after_call(run, status, false);

    status = BOOT_BUSY;
    while (!run->stopped && (status == BOOT_BUSY)) {
        status = boot_writer_erase_step(&bt_writer);
        if (run->reference) {
            TEST_ASSERT_TRUE_MESSAGE((status == BOOT_OK) || (status == BOOT_BUSY),
                                     "reference erase step");
        }
        after_call(run, status, false);
    }

    while (!run->stopped && (offset < len)) {
        uint32_t n = s_chunks[k % (uint32_t)(sizeof(s_chunks) / sizeof(s_chunks[0]))];

        k++;
        if (n > (len - offset)) {
            n = len - offset;
        }
        status = boot_writer_write(&bt_writer, offset, &data[offset], n);
        if (run->reference) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(BOOT_OK, status, "reference write");
        }
        offset += n;
        after_call(run, status, false);
    }

    status = BOOT_BUSY;
    while (!run->stopped && (status == BOOT_BUSY)) {
        status = boot_writer_finish_step(&bt_writer);
        if (run->reference && (status == BOOT_OK)) {
            bt_ref_commit(&run->model, target, s_image[image]);
        }
        if (run->reference) {
            TEST_ASSERT_TRUE_MESSAGE((status == BOOT_BUSY) ||
                                         (status == (bad ? BOOT_E_INVALID : BOOT_OK)),
                                     "reference finish step");
        }
        after_call(run, status, false);
    }
}

static void run_steps(run_t *run, const q_step_t *steps)
{
    for (uint32_t i = 0U; (i < 12U) && !run->stopped && (steps[i].kind != Q_END); i++) {
        switch (steps[i].kind) {
        case Q_SELECT:
            step_select(run);
            break;
        case Q_CONFIRM:
            step_confirm(run);
            break;
        case Q_UPDATE:
            step_update(run, steps[i].arg, false);
            break;
        case Q_UPDATE_BAD:
            step_update(run, steps[i].arg, true);
            break;
        default:
            TEST_FAIL_MESSAGE("a setup step among the steps");
            break;
        }
    }
}

/* The setup of a sequence, without cuts, on a fresh bench; then the snapshot. The run's model
 * and trial count start from it. */
static void run_setup(const geometry_t *g, const q_sequence_t *seq, uint32_t prefill, run_t *run)
{
    (void)memset(run, 0, sizeof(*run));
    run->reference = true;
    bt_setup(&g->shape);
    bt_checker.chunk = 256U;
    bt_ref_factory(&run->model);

    for (uint32_t i = 0U; (i < 10U) && (seq->setup[i].kind != Q_END); i++) {
        const q_step_t *st = &seq->setup[i];

        switch (st->kind) {
        case Q_FACTORY:
        case Q_FACTORY_B:
            bt_raw_write(bt_slot_addr((uint8_t)((st->kind == Q_FACTORY) ? 0U : 1U)),
                         s_image[st->arg], s_len[st->arg]);
            break;
        case Q_DAMAGE_ACTIVE:
        case Q_DAMAGE_OTHER: {
            const uint8_t c = run->model.have ? bt_ref_confirmed(&run->model) : 0U;
            const uint8_t slot = (st->kind == Q_DAMAGE_ACTIVE) ? c : (uint8_t)(1U - c);

            bt_flip_bit(bt_slot_addr(slot) + BT_IMAGE_HEADER + 17U, 4U);
            break;
        }
        default: {
            const q_step_t one[2] = {*st, {Q_END, 0U}};

            run_steps(run, one);
            if ((i == 1U) && (prefill > 0U)) {
                /* right after the adoption: `prefill` complete updates */
                for (uint32_t p = 0U; p < prefill; p++) {
                    const q_step_t cycle[4] = {{Q_UPDATE, (p % 2U == 0U) ? I_FILL_A : I_FILL_B},
                                               {Q_SELECT, 0U},
                                               {Q_CONFIRM, 0U},
                                               {Q_END, 0U}};

                    run_steps(run, cycle);
                }
            }
            break;
        }
        }
    }

    s_snapshot_len = bt_device_size();
    TEST_ASSERT_TRUE(s_snapshot_len <= sizeof(s_snapshot));
    (void)memcpy(s_snapshot, bt_mem, s_snapshot_len);
    /* The reference run proper starts here. */
    bt_ref_now(&run->model);
    run->calls = 0U;
    run->state[0] = run->model;
    run->boots[0] = run->trial_boots;
    bt_counts_reset();
}

/* ------------------------------------------------------------------ one cut */

typedef struct {
    uint32_t cases;
    uint32_t after_state; /* cases that read as the state after the cut call */
    uint32_t rollbacks;   /* crash loops that ended in a rollback */
} tally_t;

static void fail_case(const char *seq, uint32_t op, uint32_t position, uint32_t seed, bool tear,
                      const char *what)
{
    (void)snprintf(s_msg, sizeof(s_msg), "%s; cut %s op %lu pos %lu seed %lu: %s", seq,
                   tear ? "in" : "before", (unsigned long)op, (unsigned long)position,
                   (unsigned long)seed, what);
    TEST_FAIL_MESSAGE(s_msg);
}

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fail_case(seq->name, op, position, seed, tear, (what));                                \
        }                                                                                          \
    } while (0)

static void run_case(const q_sequence_t *seq, const run_t *ref, uint32_t ops, uint32_t op,
                     bool tear, uint32_t position, uint32_t seed, tally_t *tally)
{
    static run_t cut;
    bt_ref_t s;
    bt_ref_t model;
    bt_truth_t truth;
    bt_ref_choice_t choice;
    boot_selection_t sel;
    boot_description_t d;
    uint32_t k = ref->calls; /* the call that made operation `op` */
    uint8_t trial;
    uint32_t boots;
    uint8_t confirmed_id[BOOT_IMAGE_ID_SIZE];

    /* The run with the cut. */
    (void)memcpy(bt_mem, s_snapshot, s_snapshot_len);
    bt_power_on();
    bt_counts_reset();
    if (op < ops) {
        if (tear) {
            bt_cut_tear(op, position, seed);
        } else {
            bt_cut_before(op);
        }
    }
    bt_reboot();
    (void)memset(&cut, 0, sizeof(cut));
    cut.last_booted = ref->state[0].have ? bt_ref_confirmed(&ref->state[0]) : 0U;
    run_steps(&cut, seq->steps);
    CHECK((op == ops) == !bt_cut_fired(), "the cut did not happen where it was armed");
    for (uint32_t c = 0U; c < ref->calls; c++) {
        if (ref->call_end[c] > op) {
            k = c;
            break;
        }
    }

    /* The supply comes back; the bootloader starts from nothing. */
    bt_power_on();
    bt_reboot();
    bt_ref_now(&s);
    if (!bt_ref_equal(&s, &ref->state[k]) &&
        ((k == ref->calls) || !bt_ref_equal(&s, &ref->state[k + 1U]))) {
        (void)snprintf(
            s_msg, sizeof(s_msg),
            "call %lu: the flash reads %s, neither the state before (%s) nor after "
            "(%s)",
            (unsigned long)k, bt_ref_text(&s, s_text_a, sizeof(s_text_a)),
            bt_ref_text(&ref->state[k], s_text_b, sizeof(s_text_b)),
            (k < ref->calls) ? bt_ref_text(&ref->state[k + 1U], s_text_c, sizeof(s_text_c)) : "-");
        fail_case(seq->name, op, position, seed, tear, s_msg);
    }
    tally->after_state += ((k < ref->calls) && !bt_ref_equal(&s, &ref->state[k])) ? 1U : 0U;
    CHECK(s.have || !ref->state[k].have, "the metadata no longer decodes");

    /* (a), (b): the first select, as the reference model makes it. */
    bt_truth(&truth);
    model = s;
    bt_ref_select(&model, &truth, &choice);
    trial = trial_of(&s);
    CHECK(boot_select(&bt_boot, &sel) == choice.status, "select's result is not the model's");
    CHECK((sel.slot == choice.slot) && (sel.attempt == choice.attempt) &&
              (sel.event == choice.event),
          "select did not choose what the model chooses");
    if (sel.slot >= BOOT_SLOTS) {
        fail_case(seq->name, op, position, seed, tear, "(a) nothing bootable");
    } else {
        CHECK(truth.valid[sel.slot] &&
                  (memcmp(truth.id[sel.slot], model.id[sel.slot], BOOT_IMAGE_ID_SIZE) == 0),
              "(b) the slot booted does not hold the image the metadata names");
    }

    /* (c): a trial with attempts left that validates is booted next. */
    if ((trial != BOOT_NO_SLOT) && (s.attempts < BOOT_MAX_ATTEMPTS) && truth.valid[trial] &&
        (memcmp(truth.id[trial], s.id[trial], BOOT_IMAGE_ID_SIZE) == 0)) {
        CHECK(sel.trial && (sel.slot == trial) && (sel.attempt == (uint8_t)(s.attempts + 1U)),
              "(c) the committed trial was not booted next");
    }

    /* (d): boots of the trial before the cut (none if it is the one the cut call committed),
     * then select again and again as after a trial that crashes each time. */
    boots = 0U;
    if (trial != BOOT_NO_SLOT) {
        const uint8_t before = trial_of(&ref->state[k]);

        if ((before != BOOT_NO_SLOT) &&
            (memcmp(ref->state[k].id[before], s.id[trial], BOOT_IMAGE_ID_SIZE) == 0)) {
            boots = ref->boots[k];
        }
        CHECK(boots <= s.attempts, "(d) more boots than attempts counted");
    }
    (void)memcpy(confirmed_id, s.have ? s.id[bt_ref_confirmed(&s)] : truth.id[sel.slot],
                 BOOT_IMAGE_ID_SIZE);
    for (uint32_t round = 0U; (round < (BOOT_MAX_ATTEMPTS + 2U)) && sel.trial; round++) {
        boots++;
        CHECK(boots <= BOOT_MAX_ATTEMPTS, "(d) a trial booted more than BOOT_MAX_ATTEMPTS times");
        bt_ref_select(&model, &truth, &choice);
        CHECK(boot_select(&bt_boot, &sel) == BOOT_OK, "select failed in the crash loop");
        CHECK((sel.slot == choice.slot) && (sel.attempt == choice.attempt) &&
                  (sel.event == choice.event),
              "select in the crash loop did not choose what the model chooses");
        tally->rollbacks += (sel.event == BOOT_EVENT_ROLLBACK) ? 1U : 0U;
    }
    CHECK(!sel.trial, "(d) the crash loop did not end in a rollback");

    /* (e): the CONFIRMED image read after the cut is still the CONFIRMED one, and booted. */
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_describe(&bt_boot, &d));
    if (s.have && (choice.event != BOOT_EVENT_FALLBACK)) {
        CHECK((d.active_slot != BOOT_NO_SLOT) &&
                  (memcmp(d.slot[d.active_slot].id, confirmed_id, BOOT_IMAGE_ID_SIZE) == 0) &&
                  (sel.slot == d.active_slot) && !sel.trial,
              "(e) the confirmed image did not stay active");
    }

    /* (f): the next update succeeds. */
    {
        const uint8_t target = (d.active_slot == 0U) ? 1U : 0U;
        boot_status_t status = bt_update(&bt_boot, target, s_image[I_NEXT], s_len[I_NEXT], 1U);

        CHECK(status == BOOT_OK, "(f) the next update failed");
        CHECK((boot_select(&bt_boot, &sel) == BOOT_OK) && sel.trial && (sel.slot == target) &&
                  (sel.attempt == 1U),
              "(f) the next update did not boot as a trial");
        CHECK(boot_confirm(&bt_boot, target) == BOOT_OK, "(f) the next update did not confirm");
        CHECK((boot_select(&bt_boot, &sel) == BOOT_OK) && !sel.trial && (sel.slot == target),
              "(f) the confirmed next update did not boot");
    }
    tally->cases++;
}

#undef CHECK

/* ------------------------------------------------------------------ the campaign */

typedef struct {
    uint32_t cases;
    uint32_t ops;
    uint32_t sequences;
    uint32_t after_state;
    uint32_t rollbacks;
} totals_t;

static double seconds_since(const struct timespec *start)
{
    struct timespec now;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + ((double)(now.tv_nsec - start->tv_nsec) / 1e9);
}

static void campaign(const geometry_t *g, const q_sequence_t *seq, uint32_t prefill,
                     totals_t *totals)
{
    static bt_op_t log[MAX_OPS];
    const uint32_t meta_end = g->shape.meta_sectors * g->shape.sector_size;
    uint32_t meta_erases = 0U;
    tally_t tally;
    uint32_t ops;
    struct timespec start;

    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    (void)memset(&tally, 0, sizeof(tally));
    campaign_images(g);

    /* The reference run. */
    run_setup(g, seq, prefill, &s_run);
    bt_reboot();
    bt_trace(log, MAX_OPS);
    s_run.last_booted = s_run.state[0].have ? bt_ref_confirmed(&s_run.state[0]) : 0U;
    run_steps(&s_run, seq->steps);
    ops = bt_trace_len();
    bt_trace(NULL, 0U);
    TEST_ASSERT_TRUE_MESSAGE(ops < MAX_OPS, "more operations than MAX_OPS");
    TEST_ASSERT_EQUAL_UINT32(ops, s_run.call_end[s_run.calls - 1U]);
    for (uint32_t i = 0U; i < ops; i++) {
        meta_erases += (log[i].erase && (log[i].addr < meta_end)) ? 1U : 0U;
    }
    if (prefill > 0U) {
        TEST_ASSERT_TRUE_MESSAGE(meta_erases > 0U,
                                 "the prefill does not make a record of the steps erase a sector");
    }

    /* Every operation: just before it, and torn at every position with every seed. */
    for (uint32_t op = 0U; op <= ops; op++) {
        run_case(seq, &s_run, ops, op, false, 0U, 0U, &tally);
        if (op == ops) {
            break;
        }
        if (log[op].erase) {
            for (uint32_t seed = 0U; seed < ERASE_SEED_COUNT; seed++) {
                run_case(seq, &s_run, ops, op, true, (seed * 15U) / 8U, seed, &tally);
            }
        } else {
            const uint32_t units = log[op].len / g->shape.unit_size;

            for (uint32_t position = 0U; position < units; position++) {
                for (uint32_t s = 0U; s < TEAR_SEED_COUNT; s++) {
                    run_case(seq, &s_run, ops, op, true, position, TEAR_SEEDS[s], &tally);
                }
            }
        }
    }

    (void)printf("  %-52s prefill %2lu: %4lu operations (%lu metadata erases), %6lu cuts (%5lu "
                 "after the cut call, %4lu crash loops rolled back) %6.2f s\n",
                 seq->name, (unsigned long)prefill, (unsigned long)ops, (unsigned long)meta_erases,
                 (unsigned long)tally.cases, (unsigned long)tally.after_state,
                 (unsigned long)tally.rollbacks, seconds_since(&start));
    totals->cases += tally.cases;
    totals->ops += ops;
    totals->sequences++;
    totals->after_state += tally.after_state;
    totals->rollbacks += tally.rollbacks;
}

/* Run the sequences on geometry `geometry`: with an empty log (`empty_log`), and again with the
 * log filled first (`filled_log`). */
static void campaign_on(uint32_t geometry, bool empty_log, bool filled_log)
{
    const geometry_t *g = &s_geometries[geometry];
    totals_t totals;
    struct timespec start;

    (void)memset(&totals, 0, sizeof(totals));
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    (void)printf("power-cut campaign on %s, flash model %s\n", g->name, boot_test_flash_name());
    for (uint32_t i = 0U; empty_log && (i < SEQUENCES); i++) {
        campaign(g, &s_sequences[i], 0U, &totals);
    }
    /* Sequences again, with the log filled to where one of their records crosses a sector. */
    for (uint32_t p = 0U; filled_log && (p < MAX_PREFILLS) && (g->prefills[p].count > 0U); p++) {
        campaign(g, &s_sequences[g->prefills[p].sequence], g->prefills[p].count, &totals);
    }
    (void)printf("  total: %lu sequences, %lu operations, %lu cuts, %lu read as the state after "
                 "the cut call, in %.2f s\n",
                 (unsigned long)totals.sequences, (unsigned long)totals.ops,
                 (unsigned long)totals.cases, (unsigned long)totals.after_state,
                 seconds_since(&start));
    /* Both sides of the atomicity check were reached, and the crash loops ran. */
    TEST_ASSERT_GREATER_THAN_UINT32(10U, totals.after_state);
    TEST_ASSERT_GREATER_THAN_UINT32(50U, totals.rollbacks);
}

void setUp(void)
{
}

void tearDown(void)
{
}

#if BOOT_CUTS_PART == 0
static void test_power_cuts_on_the_host_geometry(void)
{
    campaign_on(0U, true, false);
}
#elif BOOT_CUTS_PART == 1
static void test_power_cuts_on_the_host_geometry_with_the_log_filled_first(void)
{
    campaign_on(0U, false, true);
}
#else
static void test_power_cuts_with_eight_entries_per_metadata_sector(void)
{
    campaign_on(1U, true, true);
}

static void test_power_cuts_with_records_over_two_pages_and_three_metadata_sectors(void)
{
    campaign_on(2U, true, true);
}
#endif

int main(void)
{
    UNITY_BEGIN();
#if BOOT_CUTS_PART == 0
    RUN_TEST(test_power_cuts_on_the_host_geometry);
#elif BOOT_CUTS_PART == 1
    RUN_TEST(test_power_cuts_on_the_host_geometry_with_the_log_filled_first);
#else
    RUN_TEST(test_power_cuts_with_eight_entries_per_metadata_sector);
    RUN_TEST(test_power_cuts_with_records_over_two_pages_and_three_metadata_sectors);
#endif
    return UNITY_END();
}
