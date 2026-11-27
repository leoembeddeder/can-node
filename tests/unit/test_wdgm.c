/* SPDX-License-Identifier: MIT */
/*
 * The watchdog manager, in the order of its header: the state before and after wdgm_init(),
 * the checkpoint, the examination at the end of each reference cycle and its bounds, every
 * transition of the Local Supervision Status (SWS R22-11 Figure 2) and of the Global
 * Supervision Status (Figure 3) with its requirement number, what may be kicked in which
 * status, the first expired entity, the reaction to a damaged state, a long random run against
 * a reference model written from the SWS's requirements, and checkpoints reported from several
 * threads while the supervision runs (scripts/test-tsan.sh runs this test under
 * ThreadSanitizer as well, because it creates threads).
 *
 * Most tests use one entity, ENTITY_A, with 10 indications expected per reference cycle of one
 * supervision cycle, 2 fewer and 3 more still correct (8 to 13), and the failed tolerance and
 * the expired tolerance the test needs.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "wdgm/wdgm.h"

#define OK_L      WDGM_LOCAL_STATUS_OK
#define FAILED_L  WDGM_LOCAL_STATUS_FAILED
#define EXPIRED_L WDGM_LOCAL_STATUS_EXPIRED
#define DEACT_L   WDGM_LOCAL_STATUS_DEACTIVATED
#define OK_G      WDGM_GLOBAL_STATUS_OK
#define FAILED_G  WDGM_GLOBAL_STATUS_FAILED
#define EXPIRED_G WDGM_GLOBAL_STATUS_EXPIRED
#define STOPPED_G WDGM_GLOBAL_STATUS_STOPPED
#define DEACT_G   WDGM_GLOBAL_STATUS_DEACTIVATED
#define ENTITY_A  0U
#define ENTITY_B  1U
#define GOOD      10U /* a correct count for the default entity */
#define TOO_FEW   7U  /* one below the lower bound 8 */
#define TOO_MANY  14U /* one above the upper bound 13 */

static wdgm_state_t s_state;
static wdgm_entity_config_t s_entities[WDGM_MAX_ENTITIES];
static wdgm_config_t s_config;

/* xorshift32, seeded before every test: the same "random" numbers on every run and in any
 * order of the tests. */
static uint32_t s_random_state;

static uint32_t random_u32(void)
{
    s_random_state ^= s_random_state << 13U;
    s_random_state ^= s_random_state >> 17U;
    s_random_state ^= s_random_state << 5U;
    return s_random_state;
}

static uint32_t random_below(uint32_t bound)
{
    return random_u32() % bound;
}

static const char *at(const char *what, unsigned long value)
{
    static char text[64];

    (void)snprintf(text, sizeof(text), "%s %lu", what, value);
    return text;
}

/* Every entity like the default one: 10 expected, margins 2 and 3, one supervision cycle per
 * reference cycle, `failed_tolerance`; the manager with `expired_tolerance`. */
static void configure(uint8_t entities, uint8_t failed_tolerance, uint8_t expired_tolerance)
{
    for (uint32_t i = 0U; i < WDGM_MAX_ENTITIES; i++) {
        s_entities[i] = (wdgm_entity_config_t){.name = "entity",
                                               .expected_alive = 10U,
                                               .min_margin = 2U,
                                               .max_margin = 3U,
                                               .reference_cycle = 1U,
                                               .failed_tolerance = failed_tolerance};
    }

    s_config = (wdgm_config_t){
        .entities = s_entities, .entity_count = entities, .expired_tolerance = expired_tolerance};
}

/* Configure and initialise; the state starts out as nonsense, which wdgm_init() must not need. */
static void start(uint8_t entities, uint8_t failed_tolerance, uint8_t expired_tolerance)
{
    configure(entities, failed_tolerance, expired_tolerance);
    (void)memset(&s_state, 0xA5, sizeof(s_state));
    TEST_ASSERT_TRUE(wdgm_init(&s_state, &s_config));
}

static void report(uint8_t entity, uint32_t times)
{
    for (uint32_t i = 0U; i < times; i++) {
        TEST_ASSERT_TRUE(wdgm_checkpoint(&s_state, entity));
    }
}

/* One supervision cycle in which entity A reported `count` times and every other entity a
 * correct count; returns the global status. */
static wdgm_global_status_t cycle_a(uint32_t count)
{
    report(ENTITY_A, count);

    for (uint8_t i = 1U; i < s_config.entity_count; i++) {
        report(i, GOOD);
    }

    return wdgm_main_function(&s_state, &s_config);
}

/* One supervision cycle with the counts of entities A and B given, the others correct. */
static wdgm_global_status_t cycle_ab(uint32_t count_a, uint32_t count_b)
{
    report(ENTITY_A, count_a);
    report(ENTITY_B, count_b);

    for (uint8_t i = 2U; i < s_config.entity_count; i++) {
        report(i, GOOD);
    }

    return wdgm_main_function(&s_state, &s_config);
}

#define ASSERT_LOCAL(expected, id)                                                                 \
    TEST_ASSERT_EQUAL_INT((int)(expected), (int)wdgm_local_status(&s_state, (id)))
#define ASSERT_GLOBAL(expected)                                                                    \
    TEST_ASSERT_EQUAL_INT((int)(expected), (int)wdgm_global_status(&s_state))
#define ASSERT_FAILED_CYCLES(expected, id)                                                         \
    TEST_ASSERT_EQUAL_UINT8((expected), s_state.entity[(id)].failed_cycles)

void setUp(void)
{
    s_random_state = 0x5EED0D09U;
    configure(1U, 0U, 0U);
    (void)memset(&s_state, 0, sizeof(s_state));
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------------------------
 * Before and after initialisation
 * ---------------------------------------------------------------------------------------- */

/* SWS_WdgM_00387: before WdgM_Init the global status is DEACTIVATED, here the value of zeroed
 * memory. Nothing is supervised, nothing counted, and no kick is allowed. */
static void test_a_zeroed_state_is_deactivated_and_allows_no_kick(void)
{
    ASSERT_GLOBAL(DEACT_G);
    ASSERT_LOCAL(DEACT_L, ENTITY_A);
    TEST_ASSERT_FALSE(wdgm_may_kick(&s_state));
    TEST_ASSERT_FALSE(wdgm_checkpoint(&s_state, ENTITY_A));
    TEST_ASSERT_EQUAL_INT((int)DEACT_G, (int)wdgm_main_function(&s_state, &s_config));
    TEST_ASSERT_EQUAL_UINT32(0U, s_state.cycles); /* SWS_WdgM_00063: no supervision */
    TEST_ASSERT_EQUAL_UINT8(WDGM_NO_ENTITY, wdgm_first_expired(&s_state));
}

static void expect_refused(const wdgm_config_t *config, const char *what)
{
    (void)memset(&s_state, 0xA5, sizeof(s_state));
    TEST_ASSERT_FALSE_MESSAGE(wdgm_init(&s_state, config), what);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)DEACT_G, (int)wdgm_global_status(&s_state), what);
    TEST_ASSERT_FALSE_MESSAGE(wdgm_may_kick(&s_state), what);
    TEST_ASSERT_FALSE_MESSAGE(wdgm_checkpoint(&s_state, ENTITY_A), what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, s_state.entity_count, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(WDGM_NO_ENTITY, s_state.first_expired, what);

    for (uint32_t i = 0U; i < WDGM_MAX_ENTITIES; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)DEACT_L, (int)s_state.entity[i].status, what);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, atomic_load(&s_state.entity[i].alive), what);
    }
}

static void test_init_refuses_every_invalid_configuration_and_leaves_the_state_deactivated(void)
{
    wdgm_config_t config;

    TEST_ASSERT_FALSE(wdgm_init(NULL, &s_config));
    expect_refused(NULL, "no configuration");

    config = s_config;
    config.entities = NULL;
    expect_refused(&config, "no entity table");

    config = s_config;
    config.entity_count = 0U;
    expect_refused(&config, "no entity");

    config = s_config;
    config.entity_count = WDGM_MAX_ENTITIES + 1U;
    expect_refused(&config, "more entities than the state holds");

    configure(3U, 0U, 0U);
    s_entities[2].expected_alive = 0U;
    s_entities[2].min_margin = 0U;
    expect_refused(&s_config, "no indication expected");

    configure(3U, 0U, 0U);
    s_entities[2].min_margin = s_entities[2].expected_alive;
    expect_refused(&s_config, "a lower bound of 0 would accept an entity that never runs");

    configure(3U, 0U, 0U);
    s_entities[2].reference_cycle = 0U;
    expect_refused(&s_config, "a reference cycle of no supervision cycle");
}

/* The limits themselves are valid: a lower bound of 1, the largest margins and tolerances, a
 * full table. */
static void test_init_accepts_the_limits_of_every_parameter(void)
{
    configure(WDGM_MAX_ENTITIES, UINT8_MAX, UINT8_MAX);
    s_entities[0].expected_alive = 1U;
    s_entities[0].min_margin = 0U;
    s_entities[1].expected_alive = UINT16_MAX;
    s_entities[1].min_margin = UINT16_MAX - 1U;
    s_entities[1].max_margin = UINT16_MAX;
    s_entities[2].reference_cycle = UINT16_MAX;
    s_entities[3].name = NULL;
    TEST_ASSERT_TRUE(wdgm_init(&s_state, &s_config));
    TEST_ASSERT_EQUAL_UINT8(WDGM_MAX_ENTITIES, s_state.entity_count);
}

/* SWS_WdgM_00268, SWS_WdgM_00285: every entity OK with its counters at 0, the global status OK
 * with the expired cycle counter at 0, whatever the memory held before. */
static void test_init_makes_every_entity_ok_and_the_global_status_ok(void)
{
    start(3U, 2U, 1U);
    ASSERT_GLOBAL(OK_G);
    TEST_ASSERT_TRUE(wdgm_may_kick(&s_state));
    TEST_ASSERT_EQUAL_UINT8(0U, s_state.expired_cycles);
    TEST_ASSERT_EQUAL_UINT8(WDGM_NO_ENTITY, wdgm_first_expired(&s_state));
    TEST_ASSERT_EQUAL_UINT32(0U, s_state.cycles);

    for (uint8_t i = 0U; i < 3U; i++) {
        ASSERT_LOCAL(OK_L, i);
        ASSERT_FAILED_CYCLES(0U, i);
        TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_state.entity[i].alive));
        TEST_ASSERT_EQUAL_UINT16(0U, s_state.entity[i].cycle);
        TEST_ASSERT_EQUAL_UINT32(0U, s_state.entity[i].examinations);
    }

    /* Entities beyond the configuration stay unsupervised. */
    for (uint8_t i = 3U; i < WDGM_MAX_ENTITIES; i++) {
        ASSERT_LOCAL(DEACT_L, i);
        TEST_ASSERT_EQUAL_INT((int)DEACT_L, (int)s_state.entity[i].status);
    }
}

/* A second wdgm_init() starts everything again, an EXPIRED entity and a STOPPED manager
 * included (as a reset of the node would). */
static void test_init_again_starts_from_the_beginning(void)
{
    start(2U, 0U, 0U);
    report(ENTITY_A, 5U);
    TEST_ASSERT_EQUAL_INT((int)STOPPED_G, (int)cycle_a(0U));
    report(ENTITY_B, 3U);
    TEST_ASSERT_TRUE(wdgm_init(&s_state, &s_config));
    ASSERT_GLOBAL(OK_G);
    ASSERT_LOCAL(OK_L, ENTITY_A);
    TEST_ASSERT_EQUAL_UINT8(WDGM_NO_ENTITY, wdgm_first_expired(&s_state));
    TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_state.entity[ENTITY_B].alive));
    TEST_ASSERT_EQUAL_UINT32(0U, s_state.entity[ENTITY_A].examinations);
}

/* ------------------------------------------------------------------------------------------
 * The checkpoint and the examination
 * ---------------------------------------------------------------------------------------- */

static void test_a_checkpoint_counts_only_for_an_entity_of_the_configuration(void)
{
    start(2U, 0U, 0U);
    TEST_ASSERT_FALSE(wdgm_checkpoint(NULL, ENTITY_A));
    TEST_ASSERT_FALSE(wdgm_checkpoint(&s_state, 2U));
    TEST_ASSERT_FALSE(wdgm_checkpoint(&s_state, WDGM_NO_ENTITY));
    TEST_ASSERT_TRUE(wdgm_checkpoint(&s_state, ENTITY_B));
    TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_state.entity[ENTITY_A].alive));
    TEST_ASSERT_EQUAL_UINT32(1U, atomic_load(&s_state.entity[ENTITY_B].alive));
    TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_state.entity[2].alive));

    /* A damaged entity count cannot send an increment beyond the state. */
    s_state.entity_count = 200U;
    TEST_ASSERT_FALSE(wdgm_checkpoint(&s_state, WDGM_MAX_ENTITIES));
    TEST_ASSERT_EQUAL_INT((int)DEACT_L, (int)wdgm_local_status(&s_state, WDGM_MAX_ENTITIES));
}

/* SWS_WdgM_00098: with a reference cycle of 3 supervision cycles, the counter is examined in
 * every third call only, and it then holds every indication since the last examination. A
 * window without any indication in cycles 1 and 2 changes nothing; the third cycle counts all
 * ten of the window, and the next window starts from 0. */
static void test_the_counter_is_examined_only_at_the_end_of_a_reference_cycle(void)
{
    start(1U, 0U, 0U);
    s_entities[0].reference_cycle = 3U;
    TEST_ASSERT_TRUE(wdgm_init(&s_state, &s_config));

    TEST_ASSERT_EQUAL_INT((int)OK_G, (int)cycle_a(0U)); /* cycle 1: not examined */
    TEST_ASSERT_EQUAL_UINT32(0U, s_state.entity[ENTITY_A].examinations);
    TEST_ASSERT_EQUAL_UINT16(1U, s_state.entity[ENTITY_A].cycle);
    TEST_ASSERT_EQUAL_INT((int)OK_G, (int)cycle_a(4U)); /* cycle 2: not examined */
    TEST_ASSERT_EQUAL_UINT32(4U, atomic_load(&s_state.entity[ENTITY_A].alive));
    TEST_ASSERT_EQUAL_INT((int)OK_G, (int)cycle_a(6U)); /* cycle 3: 0 + 4 + 6 = 10 */
    TEST_ASSERT_EQUAL_UINT32(1U, s_state.entity[ENTITY_A].examinations);
    TEST_ASSERT_EQUAL_UINT32(10U, s_state.entity[ENTITY_A].last_count);
    TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_state.entity[ENTITY_A].alive));
    TEST_ASSERT_EQUAL_UINT16(0U, s_state.entity[ENTITY_A].cycle);

    /* The next window: 7 in all is too few, and only its third cycle says so. */
    TEST_ASSERT_EQUAL_INT((int)OK_G, (int)cycle_a(7U));
    TEST_ASSERT_EQUAL_INT((int)OK_G, (int)cycle_a(0U));
    ASSERT_LOCAL(OK_L, ENTITY_A);
    TEST_ASSERT_EQUAL_INT((int)STOPPED_G, (int)cycle_a(0U));
    ASSERT_LOCAL(EXPIRED_L, ENTITY_A);
    TEST_ASSERT_EQUAL_UINT32(7U, s_state.entity[ENTITY_A].last_count);
    TEST_ASSERT_EQUAL_UINT32(1U, s_state.entity[ENTITY_A].incorrect);
    TEST_ASSERT_EQUAL_UINT32(6U, s_state.cycles);
}

/* SWS_WdgM_00074: correct within expected - min margin to expected + max margin, both ends
 * included: 8 and 13 are correct, 7 and 14 are not. Each count on a fresh manager without
 * tolerance, so that one incorrect examination shows as EXPIRED. */
static void test_the_bounds_of_a_correct_count_are_inclusive(void)
{
    static const struct {
        uint32_t count;
        wdgm_local_status_t status;
    } cases[] = {{0U, EXPIRED_L}, {TOO_FEW, EXPIRED_L},  {8U, OK_L},        {GOOD, OK_L},
                 {13U, OK_L},     {TOO_MANY, EXPIRED_L}, {1000U, EXPIRED_L}};

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        start(1U, 0U, 0U);
        (void)cycle_a(cases[i].count);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)cases[i].status,
                                      (int)wdgm_local_status(&s_state, ENTITY_A),
                                      at("count", cases[i].count));
        TEST_ASSERT_EQUAL_UINT32(cases[i].count, s_state.entity[ENTITY_A].last_count);
    }
}

/* Each entity is examined against its own parameters: B's window is 4 cycles long and its
 * bounds are 1 to 2, A's are those of the default. */
static void test_each_entity_is_examined_against_its_own_parameters(void)
{
    configure(2U, 0U, 0U);
    s_entities[1] = (wdgm_entity_config_t){.name = "b",
                                           .expected_alive = 2U,
                                           .min_margin = 1U,
                                           .max_margin = 0U,
                                           .reference_cycle = 4U,
                                           .failed_tolerance = 0U};
    TEST_ASSERT_TRUE(wdgm_init(&s_state, &s_config));

    for (uint32_t c = 0U; c < 8U; c++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)OK_G, (int)cycle_ab(GOOD, ((c % 4U) == 0U) ? 2U : 0U),
                                      at("cycle", c));
    }

    TEST_ASSERT_EQUAL_UINT32(2U, s_state.entity[ENTITY_B].examinations);
    TEST_ASSERT_EQUAL_UINT32(8U, s_state.entity[ENTITY_A].examinations);
    /* Three in one window is more than 2 + 0. */
    (void)cycle_ab(GOOD, 3U);
    (void)cycle_ab(GOOD, 0U);
    (void)cycle_ab(GOOD, 0U);
    ASSERT_LOCAL(OK_L, ENTITY_B);
    TEST_ASSERT_EQUAL_INT((int)STOPPED_G, (int)cycle_ab(GOOD, 0U));
    ASSERT_LOCAL(EXPIRED_L, ENTITY_B);
    ASSERT_LOCAL(OK_L, ENTITY_A);
}

/* ------------------------------------------------------------------------------------------
 * Local Supervision Status (Figure 2)
 * ---------------------------------------------------------------------------------------- */

/* SWS_WdgM_00201 */
static void test_an_entity_stays_ok_while_its_counts_are_correct(void)
{
    start(1U, 2U, 0U);

    for (uint32_t c = 0U; c < 100U; c++) {
        TEST_ASSERT_EQUAL_INT((int)OK_G, (int)cycle_a(8U + (c % 6U)));
        ASSERT_LOCAL(OK_L, ENTITY_A);
        ASSERT_FAILED_CYCLES(0U, ENTITY_A);
    }
}

/* SWS_WdgM_00202: no tolerance, so the first incorrect examination expires the entity. */
static void test_without_tolerance_one_incorrect_count_expires_the_entity(void)
{
    start(1U, 0U, 0U);
    (void)cycle_a(TOO_MANY);
    ASSERT_LOCAL(EXPIRED_L, ENTITY_A);
    ASSERT_FAILED_CYCLES(0U, ENTITY_A);
}

/* SWS_WdgM_00203, SWS_WdgM_00204, SWS_WdgM_00206: with a tolerance of 3, three incorrect
 * examinations are FAILED with the counter at 1, 2 and 3, and the fourth expires the entity. */
static void test_with_tolerance_failed_counts_up_and_expires_beyond_it(void)
{
    start(1U, 3U, 0U);
    (void)cycle_a(TOO_FEW);
    ASSERT_LOCAL(FAILED_L, ENTITY_A); /* SWS_WdgM_00203 */
    ASSERT_FAILED_CYCLES(1U, ENTITY_A);
    (void)cycle_a(0U);
    ASSERT_LOCAL(FAILED_L, ENTITY_A); /* SWS_WdgM_00204 */
    ASSERT_FAILED_CYCLES(2U, ENTITY_A);
    (void)cycle_a(TOO_MANY);
    ASSERT_LOCAL(FAILED_L, ENTITY_A); /* SWS_WdgM_00204 */
    ASSERT_FAILED_CYCLES(3U, ENTITY_A);
    (void)cycle_a(TOO_FEW);
    ASSERT_LOCAL(EXPIRED_L, ENTITY_A); /* SWS_WdgM_00206: F == T */
    ASSERT_FAILED_CYCLES(3U, ENTITY_A);
}

/* SWS_WdgM_00300, SWS_WdgM_00205: a correct examination in FAILED takes one off the counter;
 * at 1 it returns the entity to OK. */
static void test_a_failed_entity_heals_one_correct_cycle_at_a_time(void)
{
    start(1U, 3U, 0U);
    (void)cycle_a(0U);
    (void)cycle_a(0U);
    ASSERT_FAILED_CYCLES(2U, ENTITY_A);
    (void)cycle_a(GOOD);
    ASSERT_LOCAL(FAILED_L, ENTITY_A); /* SWS_WdgM_00300: 2 -> 1 */
    ASSERT_FAILED_CYCLES(1U, ENTITY_A);
    (void)cycle_a(GOOD);
    ASSERT_LOCAL(OK_L, ENTITY_A); /* SWS_WdgM_00205: 1 -> 0 */
    ASSERT_FAILED_CYCLES(0U, ENTITY_A);
}

/* The tolerance is net of the good cycles in between: with a tolerance of 2, alternating bad
 * and good never expires, two bad for every good one does. Bad, bad, good, bad, bad: the
 * counter goes 1, 2, 1, 2, and the fifth examination finds it at 2 == T. */
static void test_the_tolerance_counts_failed_cycles_net_of_good_ones(void)
{
    start(1U, 2U, 0U);

    for (uint32_t c = 0U; c < 50U; c++) {
        (void)cycle_a(((c % 2U) == 0U) ? 0U : GOOD);
        ASSERT_LOCAL(((c % 2U) == 0U) ? FAILED_L : OK_L, ENTITY_A);
    }

    start(1U, 2U, 0U);
    (void)cycle_a(0U);
    (void)cycle_a(0U);
    (void)cycle_a(GOOD);
    (void)cycle_a(0U);
    ASSERT_LOCAL(FAILED_L, ENTITY_A);
    ASSERT_FAILED_CYCLES(2U, ENTITY_A);
    (void)cycle_a(0U);
    ASSERT_LOCAL(EXPIRED_L, ENTITY_A);
}

/* EXPIRED cannot be left: correct counts for ever after change nothing, and the entity is
 * still examined (its counter keeps being taken). */
static void test_an_expired_entity_stays_expired(void)
{
    start(1U, 0U, 5U);
    (void)cycle_a(0U);
    ASSERT_LOCAL(EXPIRED_L, ENTITY_A);

    for (uint32_t c = 0U; c < 20U; c++) {
        (void)cycle_a(GOOD);
        ASSERT_LOCAL(EXPIRED_L, ENTITY_A);
    }

    TEST_ASSERT_EQUAL_UINT32(21U, s_state.entity[ENTITY_A].examinations);
    TEST_ASSERT_EQUAL_UINT32(GOOD, s_state.entity[ENTITY_A].last_count);
}

/* ------------------------------------------------------------------------------------------
 * Global Supervision Status (Figure 3) and the kick
 * ---------------------------------------------------------------------------------------- */

/* SWS_WdgM_00078, SWS_WdgM_00076, SWS_WdgM_00217, SWS_WdgM_00218 with two entities. */
static void test_the_global_status_follows_failed_entities_back_to_ok(void)
{
    start(2U, 3U, 0U);
    TEST_ASSERT_EQUAL_INT((int)OK_G, (int)cycle_ab(GOOD, GOOD));        /* 00078 */
    TEST_ASSERT_EQUAL_INT((int)FAILED_G, (int)cycle_ab(0U, GOOD));      /* 00076 */
    TEST_ASSERT_EQUAL_INT((int)FAILED_G, (int)cycle_ab(GOOD, TOO_FEW)); /* 00217: B FAILED */
    ASSERT_LOCAL(OK_L, ENTITY_A);
    TEST_ASSERT_EQUAL_INT((int)FAILED_G, (int)cycle_ab(GOOD, TOO_MANY)); /* 00217 */
    TEST_ASSERT_EQUAL_INT((int)FAILED_G, (int)cycle_ab(GOOD, GOOD));
    TEST_ASSERT_EQUAL_INT((int)OK_G, (int)cycle_ab(GOOD, GOOD)); /* 00218 */
    TEST_ASSERT_TRUE(wdgm_may_kick(&s_state));
}

/* SWS_WdgM_00215, SWS_WdgM_00219, SWS_WdgM_00220, SWS_WdgM_00221: with an expired tolerance of
 * 3, the cycle in which the entity expires and the next two are EXPIRED with the counter at 1,
 * 2 and 3, and the kick is allowed in exactly those three; then STOPPED for good. */
static void test_expired_keeps_kicking_for_the_expired_tolerance_then_stops(void)
{
    start(2U, 0U, 3U);
    TEST_ASSERT_EQUAL_INT((int)EXPIRED_G, (int)cycle_ab(0U, GOOD)); /* 00215 */
    TEST_ASSERT_EQUAL_UINT8(1U, s_state.expired_cycles);
    TEST_ASSERT_TRUE(wdgm_may_kick(&s_state));
    TEST_ASSERT_EQUAL_INT((int)EXPIRED_G, (int)cycle_ab(GOOD, GOOD)); /* 00219 */
    TEST_ASSERT_EQUAL_UINT8(2U, s_state.expired_cycles);
    TEST_ASSERT_TRUE(wdgm_may_kick(&s_state));
    TEST_ASSERT_EQUAL_INT((int)EXPIRED_G, (int)cycle_ab(GOOD, 0U)); /* 00219, B expires too */
    TEST_ASSERT_EQUAL_UINT8(3U, s_state.expired_cycles);
    TEST_ASSERT_TRUE(wdgm_may_kick(&s_state));
    TEST_ASSERT_EQUAL_INT((int)STOPPED_G, (int)cycle_ab(GOOD, GOOD)); /* 00220: E == X */
    TEST_ASSERT_FALSE(wdgm_may_kick(&s_state));

    for (uint32_t c = 0U; c < 10U; c++) {
        TEST_ASSERT_EQUAL_INT((int)STOPPED_G, (int)cycle_ab(GOOD, GOOD)); /* 00221 */
        TEST_ASSERT_FALSE(wdgm_may_kick(&s_state));
    }
}

/* SWS_WdgM_00216: without an expired tolerance the kick stops in the cycle the entity
 * expires. */
static void test_without_expired_tolerance_the_kick_stops_at_once(void)
{
    start(2U, 0U, 0U);
    TEST_ASSERT_EQUAL_INT((int)OK_G, (int)cycle_ab(GOOD, GOOD));
    TEST_ASSERT_EQUAL_INT((int)STOPPED_G, (int)cycle_ab(GOOD, 0U));
    TEST_ASSERT_FALSE(wdgm_may_kick(&s_state));
    TEST_ASSERT_EQUAL_UINT8(0U, s_state.expired_cycles);
}

/* SWS_WdgM_00077 and SWS_WdgM_00117: from FAILED (one entity failed), another entity's expiry
 * makes the global status EXPIRED or STOPPED, by the expired tolerance. */
static void test_an_entity_that_expires_while_another_has_failed(void)
{
    start(2U, 2U, 1U);
    s_entities[1].failed_tolerance = 0U;
    TEST_ASSERT_TRUE(wdgm_init(&s_state, &s_config));
    TEST_ASSERT_EQUAL_INT((int)FAILED_G, (int)cycle_ab(0U, GOOD));
    /* A fails again (F = 2) and B, without tolerance, expires. */
    TEST_ASSERT_EQUAL_INT((int)EXPIRED_G, (int)cycle_ab(0U, 0U)); /* 00077 */
    TEST_ASSERT_EQUAL_UINT8(1U, s_state.expired_cycles);
    ASSERT_LOCAL(FAILED_L, ENTITY_A);
    ASSERT_LOCAL(EXPIRED_L, ENTITY_B);
    TEST_ASSERT_EQUAL_INT((int)STOPPED_G, (int)cycle_ab(GOOD, GOOD)); /* X = 1: 00220 */

    start(2U, 2U, 0U);
    s_entities[1].failed_tolerance = 0U;
    TEST_ASSERT_TRUE(wdgm_init(&s_state, &s_config));
    TEST_ASSERT_EQUAL_INT((int)FAILED_G, (int)cycle_ab(0U, GOOD));
    TEST_ASSERT_EQUAL_INT((int)STOPPED_G, (int)cycle_ab(GOOD, 0U)); /* 00117 */
}

/* The global status is stepped in every supervision cycle, also in the cycles in which no
 * entity is examined (SWS_WdgM_00214): EXPIRED reaches STOPPED on time although the entity's
 * reference cycle is 10 supervision cycles long. */
static void test_the_expired_tolerance_counts_supervision_cycles_not_reference_cycles(void)
{
    start(1U, 0U, 2U);
    s_entities[0].reference_cycle = 10U;
    TEST_ASSERT_TRUE(wdgm_init(&s_state, &s_config));

    for (uint32_t c = 0U; c < 9U; c++) {
        TEST_ASSERT_EQUAL_INT((int)OK_G, (int)cycle_a(0U));
    }

    TEST_ASSERT_EQUAL_INT((int)EXPIRED_G, (int)cycle_a(0U)); /* the 10th: examined */
    TEST_ASSERT_EQUAL_INT((int)EXPIRED_G, (int)cycle_a(0U));
    TEST_ASSERT_EQUAL_INT((int)STOPPED_G, (int)cycle_a(0U));
}

/* SWS_WdgM_00223, SWS_WdgM_00122: kick in OK, FAILED and EXPIRED, not in STOPPED, nor before
 * initialisation; and for a NULL state nothing at all. */
static void test_the_kick_is_allowed_in_ok_failed_and_expired_only(void)
{
    static const struct {
        wdgm_global_status_t status;
        bool may_kick;
    } cases[] = {
        {DEACT_G, false},
        {OK_G, true},
        {FAILED_G, true},
        {EXPIRED_G, true},
        {STOPPED_G, false},
        /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): damaged on purpose */
        {(wdgm_global_status_t)99, false}};

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        s_state.status = cases[i].status;
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].may_kick, wdgm_may_kick(&s_state),
                                  at("status", (unsigned long)cases[i].status));
    }

    TEST_ASSERT_FALSE(wdgm_may_kick(NULL));
    TEST_ASSERT_EQUAL_INT((int)DEACT_G, (int)wdgm_global_status(NULL));
    TEST_ASSERT_EQUAL_INT((int)DEACT_L, (int)wdgm_local_status(NULL, ENTITY_A));
    TEST_ASSERT_EQUAL_UINT8(WDGM_NO_ENTITY, wdgm_first_expired(NULL));
    TEST_ASSERT_EQUAL_INT((int)DEACT_G, (int)wdgm_main_function(NULL, &s_config));
}

/* SWS_WdgM_00346: the first entity to expire is remembered; of several in one cycle the
 * lowest identifier; a later expiry changes nothing. */
static void test_the_first_expired_entity_is_remembered(void)
{
    start(4U, 0U, 10U);
    (void)cycle_a(GOOD);
    TEST_ASSERT_EQUAL_UINT8(WDGM_NO_ENTITY, wdgm_first_expired(&s_state));

    report(0U, GOOD);
    report(1U, GOOD);
    report(2U, 0U);
    report(3U, 0U);
    (void)wdgm_main_function(&s_state, &s_config);
    TEST_ASSERT_EQUAL_UINT8(2U, wdgm_first_expired(&s_state));

    report(0U, 0U);
    report(1U, GOOD);
    (void)wdgm_main_function(&s_state, &s_config);
    ASSERT_LOCAL(EXPIRED_L, 0U);
    TEST_ASSERT_EQUAL_UINT8(2U, wdgm_first_expired(&s_state));
}

/* ------------------------------------------------------------------------------------------
 * A damaged state or a wrong configuration
 * ---------------------------------------------------------------------------------------- */

typedef void (*damage_fn)(void);

static void damage_global_status(void)
{
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): damaged on purpose */
    s_state.status = (wdgm_global_status_t)99;
}
static void damage_expired_without_count(void)
{
    s_state.status = EXPIRED_G;
    s_state.expired_cycles = 0U;
}
static void damage_expired_count(void)
{
    s_state.expired_cycles = 3U; /* the expired tolerance is 2 */
}
static void damage_first_expired(void)
{
    s_state.first_expired = 2U; /* two entities */
}
static void damage_local_status(void)
{
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): damaged on purpose */
    s_state.entity[1].status = (wdgm_local_status_t)99;
}
static void damage_local_deactivated(void)
{
    s_state.entity[1].status = DEACT_L;
}
static void damage_cycle(void)
{
    s_state.entity[0].cycle = 1U; /* the reference cycle is 1 */
}
static void damage_failed_count(void)
{
    s_state.entity[1].status = FAILED_L;
    s_state.entity[1].failed_cycles = 3U; /* the failed tolerance is 2 */
}
static void damage_ok_with_failed_count(void)
{
    s_state.entity[0].failed_cycles = 1U;
}
static void damage_failed_without_count(void)
{
    s_state.entity[0].status = FAILED_L;
    s_state.entity[0].failed_cycles = 0U;
}
static void damage_entity_count(void)
{
    s_state.entity_count = 1U;
}
static void damage_config_size(void)
{
    s_config.entity_count = 3U;
}
static void damage_config_value(void)
{
    s_entities[1].min_margin = s_entities[1].expected_alive;
}

/* Each damage makes wdgm_main_function() STOPPED without examining anything: the counters stay
 * where they were, and no kick is allowed. A NULL configuration too. */
static void test_a_damaged_state_or_configuration_stops_the_kick(void)
{
    static const struct {
        damage_fn damage;
        const char *what;
    } cases[] = {{damage_global_status, "global status out of range"},
                 {damage_expired_without_count, "EXPIRED with the counter at 0"},
                 {damage_expired_count, "expired counter beyond the tolerance"},
                 {damage_first_expired, "first expired entity out of range"},
                 {damage_local_status, "local status out of range"},
                 {damage_local_deactivated, "a configured entity DEACTIVATED"},
                 {damage_cycle, "cycle beyond the reference cycle"},
                 {damage_failed_count, "failed counter beyond the tolerance"},
                 {damage_ok_with_failed_count, "OK with failed cycles"},
                 {damage_failed_without_count, "FAILED without failed cycles"},
                 {damage_entity_count, "entity count of the state changed"},
                 {damage_config_size, "configuration of another size"},
                 {damage_config_value, "configuration not valid"}};

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        start(2U, 2U, 2U);
        report(ENTITY_A, GOOD);
        cases[i].damage();
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)STOPPED_G, (int)wdgm_main_function(&s_state, &s_config),
                                      cases[i].what);
        TEST_ASSERT_FALSE_MESSAGE(wdgm_may_kick(&s_state), cases[i].what);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(GOOD, atomic_load(&s_state.entity[ENTITY_A].alive),
                                         cases[i].what);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_state.entity[ENTITY_A].examinations, cases[i].what);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_state.cycles, cases[i].what);
    }

    start(2U, 2U, 2U);
    TEST_ASSERT_EQUAL_INT((int)STOPPED_G, (int)wdgm_main_function(&s_state, NULL));
    TEST_ASSERT_FALSE(wdgm_may_kick(&s_state));

    /* Without damage the same state goes on: the cases above failed on their damage alone. */
    start(2U, 2U, 2U);
    TEST_ASSERT_EQUAL_INT((int)OK_G, (int)cycle_ab(GOOD, GOOD));
}

/* ------------------------------------------------------------------------------------------
 * Against a reference model
 * ---------------------------------------------------------------------------------------- */

/*
 * The model is written from the requirement texts of SWS R22-11 and kept apart from the code
 * under test in its form: the counts are kept as a list of indications per supervision cycle
 * and summed over the window when an examination is due; and each status step is a table of
 * the requirements, every row a condition written as the requirement states it. Exactly one
 * row of a table must apply in every step, which also checks that the requirements, as written
 * here, leave no case open and do not contradict each other.
 */
#define MODEL_CYCLES 300U

typedef struct {
    wdgm_local_status_t status;
    uint32_t failed;                 /* counter for Failed Supervision Reference Cycles */
    uint32_t reported[MODEL_CYCLES]; /* indications reported in each supervision cycle */
    uint32_t last_count;
} model_entity_t;

typedef struct {
    model_entity_t entity[WDGM_MAX_ENTITIES];
    wdgm_global_status_t status;
    uint32_t expired; /* Expired Cycle Counter */
    uint32_t cycles;  /* supervision cycles run */
    uint8_t first_expired;
} model_t;

static model_t s_model;

typedef struct {
    wdgm_local_status_t from;
    int alive_correct; /* 1 correct, 0 incorrect */
    int (*holds)(uint32_t failed, uint32_t tolerance);
    wdgm_local_status_t to;
    int failed_change; /* +1 increment, -1 decrement, 0 keep */
    const char *requirement;
} local_rule_t;

static int always(uint32_t failed, uint32_t tolerance)
{
    (void)failed;
    (void)tolerance;
    return 1;
}
static int tolerance_zero(uint32_t failed, uint32_t tolerance)
{
    (void)failed;
    return tolerance == 0U;
}
static int tolerance_above_zero(uint32_t failed, uint32_t tolerance)
{
    (void)failed;
    return tolerance > 0U;
}
static int failed_less_than_tolerance(uint32_t failed, uint32_t tolerance)
{
    return failed < tolerance;
}
static int failed_equals_tolerance(uint32_t failed, uint32_t tolerance)
{
    return failed == tolerance;
}
static int failed_above_one(uint32_t failed, uint32_t tolerance)
{
    (void)tolerance;
    return failed > 1U;
}
static int failed_equals_one(uint32_t failed, uint32_t tolerance)
{
    (void)tolerance;
    return failed == 1U;
}

/* SWS 7.1.2. Deadline and Logical Supervision are not used: "all their results are correct". */
static const local_rule_t LOCAL_RULES[] = {
    {OK_L, 1, always, OK_L, 0, "00201"},
    {OK_L, 0, tolerance_zero, EXPIRED_L, 0, "00202"},
    {OK_L, 0, tolerance_above_zero, FAILED_L, +1, "00203"},
    {FAILED_L, 0, failed_less_than_tolerance, FAILED_L, +1, "00204"},
    {FAILED_L, 1, failed_above_one, FAILED_L, -1, "00300"},
    {FAILED_L, 1, failed_equals_one, OK_L, -1, "00205"},
    {FAILED_L, 0, failed_equals_tolerance, EXPIRED_L, 0, "00206"},
    {EXPIRED_L, 1, always, EXPIRED_L, 0, "WdgM_LocalStatusType: cannot be left"},
    {EXPIRED_L, 0, always, EXPIRED_L, 0, "WdgM_LocalStatusType: cannot be left"},
};

/* The message of a failed "exactly one rule" check: how many applied, and the last one. */
static const char *rules_message(const char *table, unsigned int matches, const char *last)
{
    static char text[96];

    (void)snprintf(text, sizeof(text), "%u %s rules apply (the last: SWS_WdgM_%s)", matches, table,
                   (last != NULL) ? last : "none");
    return text;
}

static void model_local_step(model_entity_t *entity, bool correct, uint32_t tolerance)
{
    const local_rule_t *match = NULL;
    unsigned int matches = 0U;

    for (size_t r = 0U; r < (sizeof(LOCAL_RULES) / sizeof(LOCAL_RULES[0])); r++) {
        const local_rule_t *rule = &LOCAL_RULES[r];

        if ((rule->from == entity->status) && (rule->alive_correct == (correct ? 1 : 0)) &&
            (rule->holds(entity->failed, tolerance) != 0)) {
            match = rule;
            matches++;
        }
    }

    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        1U, matches, rules_message("local", matches, (match != NULL) ? match->requirement : NULL));
    entity->status = match->to;

    if (match->failed_change > 0) {
        entity->failed++;
    } else if (match->failed_change < 0) {
        entity->failed--;
    } else {
        /* kept */
    }
}

typedef struct {
    wdgm_global_status_t from;
    int (*holds)(int any_failed, int any_expired, uint32_t expired, uint32_t tolerance);
    wdgm_global_status_t to;
    int expired_change; /* +1 increment, 0 keep */
    const char *requirement;
} global_rule_t;

static int all_ok(int any_failed, int any_expired, uint32_t expired, uint32_t tolerance)
{
    (void)expired;
    (void)tolerance;
    return !any_failed && !any_expired;
}
static int failed_none_expired(int any_failed, int any_expired, uint32_t expired,
                               uint32_t tolerance)
{
    (void)expired;
    (void)tolerance;
    return any_failed && !any_expired;
}
static int expired_with_tolerance(int any_failed, int any_expired, uint32_t expired,
                                  uint32_t tolerance)
{
    (void)any_failed;
    (void)expired;
    return any_expired && (tolerance > 0U);
}
static int expired_without_tolerance(int any_failed, int any_expired, uint32_t expired,
                                     uint32_t tolerance)
{
    (void)any_failed;
    (void)expired;
    return any_expired && (tolerance == 0U);
}
static int expired_counter_below(int any_failed, int any_expired, uint32_t expired,
                                 uint32_t tolerance)
{
    (void)any_failed;
    return any_expired && (expired < tolerance);
}
static int expired_counter_reached(int any_failed, int any_expired, uint32_t expired,
                                   uint32_t tolerance)
{
    (void)any_failed;
    return any_expired && (expired == tolerance);
}
static int anything(int any_failed, int any_expired, uint32_t expired, uint32_t tolerance)
{
    (void)any_failed;
    (void)any_expired;
    (void)expired;
    (void)tolerance;
    return 1;
}

/* SWS 7.1.3 */
static const global_rule_t GLOBAL_RULES[] = {
    {OK_G, all_ok, OK_G, 0, "00078"},
    {OK_G, failed_none_expired, FAILED_G, 0, "00076"},
    {OK_G, expired_with_tolerance, EXPIRED_G, +1, "00215"},
    {OK_G, expired_without_tolerance, STOPPED_G, 0, "00216"},
    {FAILED_G, failed_none_expired, FAILED_G, 0, "00217"},
    {FAILED_G, all_ok, OK_G, 0, "00218"},
    {FAILED_G, expired_with_tolerance, EXPIRED_G, +1, "00077"},
    {FAILED_G, expired_without_tolerance, STOPPED_G, 0, "00117"},
    {EXPIRED_G, expired_counter_below, EXPIRED_G, +1, "00219"},
    {EXPIRED_G, expired_counter_reached, STOPPED_G, 0, "00220"},
    {STOPPED_G, anything, STOPPED_G, 0, "00221"},
};

static void model_global_step(model_t *model, uint8_t entities, uint32_t tolerance)
{
    int any_failed = 0;
    int any_expired = 0;
    const global_rule_t *match = NULL;
    unsigned int matches = 0U;

    for (uint8_t i = 0U; i < entities; i++) {
        any_failed = any_failed || (model->entity[i].status == FAILED_L);
        any_expired = any_expired || (model->entity[i].status == EXPIRED_L);
    }

    for (size_t r = 0U; r < (sizeof(GLOBAL_RULES) / sizeof(GLOBAL_RULES[0])); r++) {
        const global_rule_t *rule = &GLOBAL_RULES[r];

        if ((rule->from == model->status) &&
            (rule->holds(any_failed, any_expired, model->expired, tolerance) != 0)) {
            match = rule;
            matches++;
        }
    }

    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        1U, matches, rules_message("global", matches, (match != NULL) ? match->requirement : NULL));
    model->status = match->to;

    if (match->expired_change > 0) {
        model->expired++;
    }
}

/* One supervision cycle of the model: cycle number `n` (0-based) since the initialisation. */
static void model_cycle(model_t *model, const wdgm_config_t *config, uint32_t n)
{
    for (uint8_t i = 0U; i < config->entity_count; i++) {
        const wdgm_entity_config_t *c = &config->entities[i];
        model_entity_t *e = &model->entity[i];

        /* SWS_WdgM_00098: the examination at the end of every reference cycle, over the
         * indications of its reference_cycle supervision cycles. */
        if (((n + 1U) % c->reference_cycle) == 0U) {
            uint32_t count = 0U;

            for (uint32_t back = 0U; back < c->reference_cycle; back++) {
                count += e->reported[n - back];
            }

            /* SWS_WdgM_00074 */
            const bool correct = (count >= ((uint32_t)c->expected_alive - c->min_margin)) &&
                                 (count <= ((uint32_t)c->expected_alive + c->max_margin));
            const wdgm_local_status_t before = e->status;

            e->last_count = count;
            model_local_step(e, correct, c->failed_tolerance);

            if ((e->status == EXPIRED_L) && (before != EXPIRED_L) &&
                (model->first_expired == WDGM_NO_ENTITY)) {
                model->first_expired = i;
            }
        }
    }

    model_global_step(model, config->entity_count, config->expired_tolerance);
    model->cycles++;
}

/* How many indications an entity reports in one supervision cycle: mostly the expected rate,
 * spread over its reference cycle; now and then a cycle with none (a task that is held up) or
 * too many (a task that loops). */
static uint32_t random_report(const wdgm_entity_config_t *c)
{
    const uint32_t per_cycle =
        ((uint32_t)c->expected_alive + c->reference_cycle - 1U) / c->reference_cycle;
    const uint32_t pick = random_below(20U);
    uint32_t count;

    if (pick < 2U) {
        count = 0U;
    } else if (pick < 3U) {
        count = per_cycle * 2U + random_below(3U);
    } else {
        count = per_cycle + random_below(3U) - ((per_cycle > 0U) ? 1U : 0U);
    }

    return count;
}

/* 300 random configurations, 300 supervision cycles each: after every cycle the global status,
 * the expired cycle counter, the first expired entity and every entity's status, failed counter
 * and examined count equal the model's. The statuses visited are counted, so that the test
 * cannot pass by staying in OK. */
static void test_random_runs_match_the_reference_model(void)
{
    unsigned long local_visits[4] = {0U};
    unsigned long global_visits[5] = {0U};

    for (unsigned int run = 0U; run < 300U; run++) {
        const uint8_t entities = (uint8_t)(1U + random_below(WDGM_MAX_ENTITIES));

        for (uint8_t i = 0U; i < entities; i++) {
            wdgm_entity_config_t *c = &s_entities[i];

            c->name = NULL;
            c->expected_alive = (uint16_t)(1U + random_below(12U));
            c->min_margin = (uint16_t)random_below(c->expected_alive);
            c->max_margin = (uint16_t)random_below(4U);
            c->reference_cycle = (uint16_t)(1U + random_below(4U));
            c->failed_tolerance = (uint8_t)random_below(5U);
        }

        s_config = (wdgm_config_t){.entities = s_entities,
                                   .entity_count = entities,
                                   .expired_tolerance = (uint8_t)random_below(5U)};
        (void)memset(&s_state, 0x5A, sizeof(s_state));
        TEST_ASSERT_TRUE(wdgm_init(&s_state, &s_config));
        (void)memset(&s_model, 0, sizeof(s_model));
        s_model.status = OK_G;
        s_model.first_expired = WDGM_NO_ENTITY;

        for (uint8_t i = 0U; i < entities; i++) {
            s_model.entity[i].status = OK_L;
        }

        for (uint32_t n = 0U; n < MODEL_CYCLES; n++) {
            for (uint8_t i = 0U; i < entities; i++) {
                const uint32_t count = random_report(&s_entities[i]);

                s_model.entity[i].reported[n] = count;
                report(i, count);
            }

            const wdgm_global_status_t returned = wdgm_main_function(&s_state, &s_config);
            const unsigned long where = (run * 1000UL) + n;

            model_cycle(&s_model, &s_config, n);
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)s_model.status, (int)returned,
                                          at("run * 1000 + cycle", where));
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)s_model.status, (int)s_state.status,
                                          at("run * 1000 + cycle", where));
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(s_model.expired, s_state.expired_cycles,
                                             at("run * 1000 + cycle", where));
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(s_model.first_expired, wdgm_first_expired(&s_state),
                                            at("run * 1000 + cycle", where));
            TEST_ASSERT_EQUAL_MESSAGE((s_model.status != STOPPED_G), wdgm_may_kick(&s_state),
                                      at("run * 1000 + cycle", where));
            TEST_ASSERT_EQUAL_UINT32(s_model.cycles, s_state.cycles);
            global_visits[s_state.status]++;

            for (uint8_t i = 0U; i < entities; i++) {
                TEST_ASSERT_EQUAL_INT_MESSAGE((int)s_model.entity[i].status,
                                              (int)wdgm_local_status(&s_state, i),
                                              at("run * 1000 + cycle", where));
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(s_model.entity[i].failed,
                                                 s_state.entity[i].failed_cycles,
                                                 at("run * 1000 + cycle", where));
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(s_model.entity[i].last_count,
                                                 s_state.entity[i].last_count,
                                                 at("run * 1000 + cycle", where));
                local_visits[s_state.entity[i].status]++;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_UINT32(10000U, local_visits[OK_L]);
    TEST_ASSERT_GREATER_THAN_UINT32(10000U, local_visits[FAILED_L]);
    TEST_ASSERT_GREATER_THAN_UINT32(10000U, local_visits[EXPIRED_L]);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, global_visits[OK_G]);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, global_visits[FAILED_G]);
    TEST_ASSERT_GREATER_THAN_UINT32(100U, global_visits[EXPIRED_G]);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, global_visits[STOPPED_G]);
}

/* ------------------------------------------------------------------------------------------
 * Checkpoints from several threads
 * ---------------------------------------------------------------------------------------- */

#define REPORTERS          4U
#define REPORTS_PER_THREAD 200000U

/* Reporters 0 and 1 share entity 0, reporters 2 and 3 have entities 1 and 2 of their own: the
 * counter must take several writers at once as well as the examination. */
static uint8_t s_reporter_entity[REPORTERS] = {0U, 0U, 1U, 2U};

static atomic_uint s_reporters_done;

/* cppcheck-suppress constParameterCallback ; the signature is pthread_create's */
static void *reporter(void *arg)
{
    const uint8_t entity = *(const uint8_t *)arg;
    void *result = NULL;

    for (uint32_t i = 0U; (i < REPORTS_PER_THREAD) && (result == NULL); i++) {
        if (!wdgm_checkpoint(&s_state, entity)) {
            result = &s_reporters_done; /* any non-NULL value: a refused checkpoint */
        }
    }

    (void)atomic_fetch_add(&s_reporters_done, 1U);
    return result;
}

/* While four threads report 200,000 indications each, this thread runs supervision cycles as
 * fast as it can, and adds up what each examination counted. Every indication is counted by
 * exactly one examination: the sums equal what was reported, none lost and none twice. (A
 * plain load and store in place of the atomic exchange loses indications that come between
 * the two; ThreadSanitizer, scripts/test-tsan.sh, reports any non-atomic access to the
 * counter.) */
static void test_checkpoints_from_several_threads_are_each_counted_once(void)
{
    pthread_t threads[REPORTERS];
    uint64_t examined[3] = {0U};

    configure(3U, UINT8_MAX, UINT8_MAX);

    for (uint32_t i = 0U; i < 3U; i++) {
        s_entities[i].expected_alive = 1U;
        s_entities[i].min_margin = 0U;
        s_entities[i].max_margin = UINT16_MAX;
    }

    TEST_ASSERT_TRUE(wdgm_init(&s_state, &s_config));
    atomic_store(&s_reporters_done, 0U);

    for (uint32_t i = 0U; i < REPORTERS; i++) {
        TEST_ASSERT_EQUAL_INT(0,
                              pthread_create(&threads[i], NULL, reporter, &s_reporter_entity[i]));
    }

    while (atomic_load(&s_reporters_done) < REPORTERS) {
        (void)wdgm_main_function(&s_state, &s_config);

        for (uint32_t e = 0U; e < 3U; e++) {
            examined[e] += s_state.entity[e].last_count;
        }
    }

    for (uint32_t i = 0U; i < REPORTERS; i++) {
        void *result = &s_reporters_done;

        TEST_ASSERT_EQUAL_INT(0, pthread_join(threads[i], &result));
        TEST_ASSERT_NULL(result);
    }

    /* One more cycle takes what came after the last examination. */
    (void)wdgm_main_function(&s_state, &s_config);

    for (uint32_t e = 0U; e < 3U; e++) {
        examined[e] += s_state.entity[e].last_count;
    }

    TEST_ASSERT_EQUAL_UINT64(2ULL * REPORTS_PER_THREAD, examined[0]);
    TEST_ASSERT_EQUAL_UINT64(REPORTS_PER_THREAD, examined[1]);
    TEST_ASSERT_EQUAL_UINT64(REPORTS_PER_THREAD, examined[2]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_zeroed_state_is_deactivated_and_allows_no_kick);
    RUN_TEST(test_init_refuses_every_invalid_configuration_and_leaves_the_state_deactivated);
    RUN_TEST(test_init_accepts_the_limits_of_every_parameter);
    RUN_TEST(test_init_makes_every_entity_ok_and_the_global_status_ok);
    RUN_TEST(test_init_again_starts_from_the_beginning);
    RUN_TEST(test_a_checkpoint_counts_only_for_an_entity_of_the_configuration);
    RUN_TEST(test_the_counter_is_examined_only_at_the_end_of_a_reference_cycle);
    RUN_TEST(test_the_bounds_of_a_correct_count_are_inclusive);
    RUN_TEST(test_each_entity_is_examined_against_its_own_parameters);
    RUN_TEST(test_an_entity_stays_ok_while_its_counts_are_correct);
    RUN_TEST(test_without_tolerance_one_incorrect_count_expires_the_entity);
    RUN_TEST(test_with_tolerance_failed_counts_up_and_expires_beyond_it);
    RUN_TEST(test_a_failed_entity_heals_one_correct_cycle_at_a_time);
    RUN_TEST(test_the_tolerance_counts_failed_cycles_net_of_good_ones);
    RUN_TEST(test_an_expired_entity_stays_expired);
    RUN_TEST(test_the_global_status_follows_failed_entities_back_to_ok);
    RUN_TEST(test_expired_keeps_kicking_for_the_expired_tolerance_then_stops);
    RUN_TEST(test_without_expired_tolerance_the_kick_stops_at_once);
    RUN_TEST(test_an_entity_that_expires_while_another_has_failed);
    RUN_TEST(test_the_expired_tolerance_counts_supervision_cycles_not_reference_cycles);
    RUN_TEST(test_the_kick_is_allowed_in_ok_failed_and_expired_only);
    RUN_TEST(test_the_first_expired_entity_is_remembered);
    RUN_TEST(test_a_damaged_state_or_configuration_stops_the_kick);
    RUN_TEST(test_random_runs_match_the_reference_model);
    RUN_TEST(test_checkpoints_from_several_threads_are_each_counted_once);
    return UNITY_END();
}
