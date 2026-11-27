/* SPDX-License-Identifier: MIT */
/*
 * The E2E state machine, in the order of its header: initialisation, input errors, every
 * transition of Fig. 6.131 (PRS R24-11, transitToInvalidExtended = false) with its exact
 * moment, the timeout through NONEWDATA, the clearing of the window, a long random run against
 * a reference model, and Profile 11 and the state machine together.
 *
 * Most tests use s_config: a window of 6 and limits that differ from state to state, so that a
 * transition that looks at the limits of the wrong state happens at the wrong moment:
 *
 *     INIT     2 OKs needed, 1 error tolerated
 *     VALID    3 OKs needed, 1 error tolerated
 *     INVALID  4 OKs needed, no error tolerated
 *
 * Expected moments are worked out in the comment next to them. The window is written oldest
 * entry first, O for OK, E for ERROR, R for REPEATED, W for WRONGSEQUENCE, N for NONEWDATA;
 * entries still E2E_P_NOTAVAILABLE are left out.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "e2e/e2e_p11.h"
#include "e2e/e2e_sm.h"
#include "unity.h"

#define OK    E2E_P_OK
#define REP   E2E_P_REPEATED
#define WSEQ  E2E_P_WRONGSEQUENCE
#define ERR   E2E_P_ERROR
#define NONEW E2E_P_NONEWDATA

static const e2e_sm_config_t s_config = {.window_size = 6U,
                                         .min_ok_state_init = 2U,
                                         .max_error_state_init = 1U,
                                         .min_ok_state_valid = 3U,
                                         .max_error_state_valid = 1U,
                                         .min_ok_state_invalid = 4U,
                                         .max_error_state_invalid = 0U};

/* The state under test, and a copy of it for the tests that show that a call changed nothing
 * at all. */
static e2e_sm_check_state_t s_sm;
static e2e_sm_check_state_t s_snapshot;

/* xorshift32. The state is set before every test, so a test sees the same "random" numbers on
 * every run and whatever order the tests run in. */
static uint32_t s_random_state;

static uint32_t random_u32(void)
{
    s_random_state ^= s_random_state << 13U;
    s_random_state ^= s_random_state >> 17U;
    s_random_state ^= s_random_state << 5U;
    return s_random_state;
}

void setUp(void)
{
    s_random_state = 0x5EED0E2EU;
    /* Nonsense first: e2e_sm_check_init() must need nothing from the memory it is given. */
    (void)memset(&s_sm, 0xA5, sizeof(s_sm));
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------------------- */

static const char *at(const char *what, unsigned long value)
{
    static char text[64];

    (void)snprintf(text, sizeof(text), "%s %lu", what, value);
    return text;
}

static const char *state_name(e2e_sm_state_t state)
{
    const char *name;

    switch (state) {
    case E2E_SM_DEINIT:
        name = "DEINIT";
        break;
    case E2E_SM_NODATA:
        name = "NODATA";
        break;
    case E2E_SM_INIT:
        name = "INIT";
        break;
    case E2E_SM_VALID:
        name = "VALID";
        break;
    case E2E_SM_INVALID:
        name = "INVALID";
        break;
    default:
        name = "(not a state)";
        break;
    }

    return name;
}

#define ASSERT_STATE(expected, actual)                                                             \
    TEST_ASSERT_EQUAL_STRING(state_name(expected), state_name(actual))
#define ASSERT_STATE_MESSAGE(expected, actual, message)                                            \
    TEST_ASSERT_EQUAL_STRING_MESSAGE(state_name(expected), state_name(actual), message)

static void start_with(const e2e_sm_config_t *config)
{
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_sm_check_init(&s_sm, config));
}

static void start(void)
{
    start_with(&s_config);
}

/* One step with s_config; asserts E2E_E_OK and returns the state afterwards. */
static e2e_sm_state_t feed(e2e_p_check_status_t status)
{
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_sm_check(status, &s_config, &s_sm));
    return s_sm.sm_state;
}

/* `count` steps with the same verdict; asserts `during` after every step but the last and
 * returns the state after the last. */
static e2e_sm_state_t feed_n(e2e_p_check_status_t status, unsigned int count, e2e_sm_state_t during)
{
    e2e_sm_state_t state = s_sm.sm_state;

    for (unsigned int i = 0U; i < count; i++) {
        state = feed(status);

        if ((i + 1U) < count) {
            ASSERT_STATE_MESSAGE(during, state, at("step", i));
        }
    }

    return state;
}

/* From NODATA to VALID with a window full of OKs: one OK to leave NODATA (not counted), then
 * six that fill the window (VALID after the second of them). */
static void reach_valid_with_a_full_window(void)
{
    start();
    ASSERT_STATE(E2E_SM_INIT, feed(OK));
    ASSERT_STATE(E2E_SM_INIT, feed(OK));
    ASSERT_STATE(E2E_SM_VALID, feed_n(OK, 5U, E2E_SM_VALID));
    TEST_ASSERT_EQUAL_UINT8(6U, s_sm.ok_count);
}

static void expect_window_cleared(void)
{
    TEST_ASSERT_EACH_EQUAL_INT(E2E_P_NOTAVAILABLE, s_sm.window, E2E_SM_WINDOW_MAX);
    TEST_ASSERT_EQUAL_UINT8(0U, s_sm.window_top_index);
    TEST_ASSERT_EQUAL_UINT8(0U, s_sm.ok_count);
    TEST_ASSERT_EQUAL_UINT8(0U, s_sm.error_count);
}

/* Configurations e2e_sm_check_init() and e2e_sm_check() refuse: window 0 and 17, and for each
 * of the three states a minimum of 0 OKs, a minimum above the window and a maximum of errors
 * equal to the window. Built from s_config so that each has exactly one fault. */
#define BAD_CONFIGS 11U

static void bad_configs(e2e_sm_config_t configs[BAD_CONFIGS])
{
    for (size_t i = 0U; i < BAD_CONFIGS; i++) {
        configs[i] = s_config;
    }

    configs[0].window_size = 0U;
    configs[1].window_size = 17U;
    configs[2].min_ok_state_init = 0U;
    configs[3].min_ok_state_init = 7U;
    configs[4].max_error_state_init = 6U;
    configs[5].min_ok_state_valid = 0U;
    configs[6].min_ok_state_valid = 7U;
    configs[7].max_error_state_valid = 6U;
    configs[8].min_ok_state_invalid = 0U;
    configs[9].min_ok_state_invalid = 7U;
    configs[10].max_error_state_invalid = 6U;
}

/* ------------------------------------------------------------------------------------------
 * Initialisation and input errors
 * ---------------------------------------------------------------------------------------- */

/* SWS_E2E_00370: every window entry NOTAVAILABLE, counts and top index 0, state NODATA. The
 * memory held 0xA5 before. */
static void test_check_init_empties_the_window_and_enters_nodata(void)
{
    start();
    ASSERT_STATE(E2E_SM_NODATA, s_sm.sm_state);
    expect_window_cleared();
}

static void test_check_init_refuses_null_and_bad_configurations(void)
{
    e2e_sm_config_t configs[BAD_CONFIGS];

    bad_configs(configs);
    s_snapshot = s_sm;

    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_sm_check_init(NULL, &s_config));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_sm_check_init(&s_sm, NULL));

    for (size_t i = 0U; i < BAD_CONFIGS; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(E2E_E_INPUTERR_WRONG, e2e_sm_check_init(&s_sm, &configs[i]),
                                       at("configuration", i));
    }

    TEST_ASSERT_EQUAL_MEMORY(&s_snapshot, &s_sm, sizeof(s_sm));
}

/* The boundaries are accepted: a window of 1 (1 OK, no error) and of 16 (16 OKs, 15 errors). */
static void test_check_init_accepts_the_boundaries(void)
{
    const e2e_sm_config_t smallest = {.window_size = 1U,
                                      .min_ok_state_init = 1U,
                                      .max_error_state_init = 0U,
                                      .min_ok_state_valid = 1U,
                                      .max_error_state_valid = 0U,
                                      .min_ok_state_invalid = 1U,
                                      .max_error_state_invalid = 0U};
    const e2e_sm_config_t largest = {.window_size = 16U,
                                     .min_ok_state_init = 16U,
                                     .max_error_state_init = 15U,
                                     .min_ok_state_valid = 16U,
                                     .max_error_state_valid = 15U,
                                     .min_ok_state_invalid = 16U,
                                     .max_error_state_invalid = 15U};

    start_with(&smallest);
    start_with(&largest);

    /* A window of 1: VALID on each OK after the one that leaves NODATA, INVALID on anything
     * else, and each verdict alone decides. */
    start_with(&smallest);
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_sm_check(OK, &smallest, &s_sm));
    ASSERT_STATE(E2E_SM_INIT, s_sm.sm_state);
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_sm_check(OK, &smallest, &s_sm));
    ASSERT_STATE(E2E_SM_VALID, s_sm.sm_state);
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_sm_check(NONEW, &smallest, &s_sm));
    ASSERT_STATE(E2E_SM_INVALID, s_sm.sm_state);
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_sm_check(OK, &smallest, &s_sm));
    ASSERT_STATE(E2E_SM_VALID, s_sm.sm_state);
}

/* A state that was never initialised (zeroed static memory) is DEINIT and refused with
 * E2E_E_WRONGSTATE (Fig. 6.131), unchanged: it must not read as VALID. */
static void test_check_before_init_is_wrongstate(void)
{
    (void)memset(&s_sm, 0, sizeof(s_sm));
    s_snapshot = s_sm;

    ASSERT_STATE(E2E_SM_DEINIT, s_sm.sm_state);
    TEST_ASSERT_EQUAL_HEX8(E2E_E_WRONGSTATE, e2e_sm_check(OK, &s_config, &s_sm));
    TEST_ASSERT_EQUAL_MEMORY(&s_snapshot, &s_sm, sizeof(s_sm));
}

/* Refused with the state unchanged: NULL pointers, a configuration out of range, a verdict
 * that is not one (NOTAVAILABLE, or no value of the enumeration), a damaged state. */
static void test_check_refuses_wrong_input_and_changes_nothing(void)
{
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): outside it on purpose */
    const e2e_p_check_status_t not_a_verdict = (e2e_p_check_status_t)6;
    e2e_sm_config_t configs[BAD_CONFIGS];

    bad_configs(configs);
    start();
    (void)feed(OK);
    (void)feed(OK);
    s_snapshot = s_sm;

    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_sm_check(OK, NULL, &s_sm));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_NULL, e2e_sm_check(OK, &s_config, NULL));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_WRONG,
                           e2e_sm_check(E2E_P_NOTAVAILABLE, &s_config, &s_sm));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_WRONG, e2e_sm_check(not_a_verdict, &s_config, &s_sm));

    for (size_t i = 0U; i < BAD_CONFIGS; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(E2E_E_INPUTERR_WRONG, e2e_sm_check(OK, &configs[i], &s_sm),
                                       at("configuration", i));
    }

    TEST_ASSERT_EQUAL_MEMORY(&s_snapshot, &s_sm, sizeof(s_sm));

    /* A top index at or beyond the window would be a write outside it. */
    s_sm.window_top_index = 6U;
    s_snapshot = s_sm;
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_WRONG, e2e_sm_check(OK, &s_config, &s_sm));
    TEST_ASSERT_EQUAL_MEMORY(&s_snapshot, &s_sm, sizeof(s_sm));

    s_sm.window_top_index = 200U;
    s_snapshot = s_sm;
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_WRONG, e2e_sm_check(OK, &s_config, &s_sm));
    TEST_ASSERT_EQUAL_MEMORY(&s_snapshot, &s_sm, sizeof(s_sm));

    /* A state value that no call leaves behind. */
    s_sm.window_top_index = 0U;
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): outside it on purpose */
    s_sm.sm_state = (e2e_sm_state_t)7;
    s_snapshot = s_sm;
    TEST_ASSERT_EQUAL_HEX8(E2E_E_INPUTERR_WRONG, e2e_sm_check(OK, &s_config, &s_sm));
    TEST_ASSERT_EQUAL_MEMORY(&s_snapshot, &s_sm, sizeof(s_sm));
}

/* ------------------------------------------------------------------------------------------
 * The transitions
 * ---------------------------------------------------------------------------------------- */

/* NODATA stays NODATA on ERROR and NONEWDATA, however many, and does not count the window
 * (Fig. 6.133): both counts stay 0, although ERRORs fill it. */
static void test_nodata_ignores_errors_and_missing_data(void)
{
    start();

    for (unsigned int i = 0U; i < 40U; i++) {
        ASSERT_STATE_MESSAGE(E2E_SM_NODATA, feed(((i % 3U) == 0U) ? ERR : NONEW), at("step", i));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, s_sm.ok_count, at("step", i));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, s_sm.error_count, at("step", i));
    }
}

/* Each of OK, REPEATED and WRONGSEQUENCE leaves NODATA for INIT, and the window is cleared on
 * the way (E2E_SMClearStatus), the verdict that caused the transition included. */
static void test_nodata_leaves_for_init_on_any_message_without_error(void)
{
    static const e2e_p_check_status_t leaving[3] = {OK, REP, WSEQ};

    for (size_t i = 0U; i < 3U; i++) {
        start();
        (void)feed(ERR);
        (void)feed(NONEW);
        ASSERT_STATE_MESSAGE(E2E_SM_INIT, feed(leaving[i]), at("verdict", i));
        expect_window_cleared();
    }
}

/* INIT counts from the verdict after the one that left NODATA: with min_ok_state_init 2 the
 * data is VALID at the third OK from NODATA, not at the second. */
static void test_the_verdict_that_leaves_nodata_is_not_counted(void)
{
    start();
    ASSERT_STATE(E2E_SM_INIT, feed(OK));  /* window cleared */
    ASSERT_STATE(E2E_SM_INIT, feed(OK));  /* O: 1 OK */
    ASSERT_STATE(E2E_SM_VALID, feed(OK)); /* O O: 2 OKs */
}

/* INIT -> VALID needs min_ok_state_init (2) OKs and at most max_error_state_init (1) errors:
 * an error is tolerated on the way. */
static void test_init_becomes_valid_with_enough_oks_and_few_errors(void)
{
    start();
    (void)feed(WSEQ);                     /* -> INIT */
    ASSERT_STATE(E2E_SM_INIT, feed(ERR)); /* 0 OKs, 1 error */
    ASSERT_STATE(E2E_SM_INIT, feed(OK));  /* 1 OK, 1 error */
    ASSERT_STATE(E2E_SM_VALID, feed(OK)); /* 2 OKs, 1 error */
}

/* INIT -> INVALID on more than max_error_state_init (1) errors in the window. */
static void test_init_becomes_invalid_with_too_many_errors(void)
{
    start();
    (void)feed(OK);                          /* -> INIT */
    ASSERT_STATE(E2E_SM_INIT, feed(ERR));    /* 1 error */
    ASSERT_STATE(E2E_SM_INIT, feed(NONEW));  /* still 1 */
    ASSERT_STATE(E2E_SM_INVALID, feed(ERR)); /* 2 errors */
}

/* Counter faults and missing data never take INIT to INVALID: without the extended variant
 * (transitToInvalidExtended = false, PRS_E2E_00675) they only keep it from becoming VALID. */
static void test_init_waits_through_counter_faults_and_missing_data(void)
{
    static const e2e_p_check_status_t faults[3] = {REP, WSEQ, NONEW};

    start();
    (void)feed(OK);

    for (unsigned int i = 0U; i < 60U; i++) {
        ASSERT_STATE_MESSAGE(E2E_SM_INIT, feed(faults[i % 3U]), at("step", i));
    }

    /* One OK in the window, then another: VALID. */
    ASSERT_STATE(E2E_SM_INIT, feed(OK));
    ASSERT_STATE(E2E_SM_VALID, feed(OK));
}

/* VALID stays while the window holds min_ok_state_valid (3) OKs and at most
 * max_error_state_valid (1) error. */
static void test_valid_stays_within_its_limits(void)
{
    reach_valid_with_a_full_window();       /* O O O O O O */
    ASSERT_STATE(E2E_SM_VALID, feed(ERR));  /* O O O O O E: 5 OKs, 1 error */
    ASSERT_STATE(E2E_SM_VALID, feed(REP));  /* O O O O E R: 4, 1 */
    ASSERT_STATE(E2E_SM_VALID, feed(WSEQ)); /* O O O E R W: 3, 1 */
    ASSERT_STATE(E2E_SM_VALID, feed(OK));   /* O O E R W O: 3, 1 */
    ASSERT_STATE(E2E_SM_VALID, feed_n(OK, 4U, E2E_SM_VALID));
    TEST_ASSERT_EQUAL_UINT8(5U, s_sm.ok_count);    /* W O O O O O */
    TEST_ASSERT_EQUAL_UINT8(0U, s_sm.error_count); /* the error has left the window */
}

/* VALID -> INVALID on a second error in the window, even with enough OKs. */
static void test_valid_becomes_invalid_with_too_many_errors(void)
{
    reach_valid_with_a_full_window();
    ASSERT_STATE(E2E_SM_VALID, feed(ERR));
    ASSERT_STATE(E2E_SM_VALID, feed(OK));
    ASSERT_STATE(E2E_SM_INVALID, feed(ERR)); /* O O O E O E: 4 OKs, 2 errors */
}

/*
 * The timeout. From VALID with a window full of OKs, cycles without data (NONEWDATA) push the
 * OKs out one by one. With a window of 6 and min_ok_state_valid 3 the data stays VALID for
 * 6 - 3 = 3 such cycles and is INVALID in the 4th (PRS 6.20.4, item 2). Repetitions and
 * counter jumps starve the window in the same way.
 */
static void test_timeout_through_cycles_without_data(void)
{
    static const e2e_p_check_status_t starving[3] = {NONEW, REP, WSEQ};

    for (size_t i = 0U; i < 3U; i++) {
        reach_valid_with_a_full_window();
        ASSERT_STATE_MESSAGE(E2E_SM_VALID, feed_n(starving[i], 3U, E2E_SM_VALID), at("verdict", i));
        TEST_ASSERT_EQUAL_UINT8(3U, s_sm.ok_count);
        ASSERT_STATE_MESSAGE(E2E_SM_INVALID, feed(starving[i]), at("verdict", i));
        TEST_ASSERT_EQUAL_UINT8(2U, s_sm.ok_count);
    }
}

/* VALID -> INVALID keeps the window (clearFromValidToInvalid = false). Two errors take VALID to
 * INVALID with O O O O E E; INVALID tolerates no error, so the data is VALID again only once
 * both errors have left the window, at the 6th OK. A window cleared on the way would give
 * VALID at the 4th. */
static void test_valid_to_invalid_keeps_the_window(void)
{
    reach_valid_with_a_full_window();
    ASSERT_STATE(E2E_SM_VALID, feed(ERR));
    ASSERT_STATE(E2E_SM_INVALID, feed(ERR)); /* O O O O E E */
    TEST_ASSERT_EQUAL_UINT8(4U, s_sm.ok_count);
    ASSERT_STATE(E2E_SM_INVALID, feed_n(OK, 5U, E2E_SM_INVALID)); /* E O O O O O */
    ASSERT_STATE(E2E_SM_VALID, feed(OK));                         /* O O O O O O */
}

/* INVALID -> VALID needs min_ok_state_invalid (4) OKs and at most max_error_state_invalid (0)
 * errors, and every ERROR in INVALID clears the window: recovery needs 4 OKs after the last
 * error, not the 6 that the error would take to leave the window. */
static void test_an_error_in_invalid_clears_the_window(void)
{
    start();
    (void)feed(OK);                          /* -> INIT */
    (void)feed(ERR);                         /* E */
    ASSERT_STATE(E2E_SM_INVALID, feed(ERR)); /* E E: 2 errors */
    ASSERT_STATE(E2E_SM_INVALID, feed(OK));  /* E E O */
    ASSERT_STATE(E2E_SM_INVALID, feed(ERR)); /* an error in INVALID: cleared */
    expect_window_cleared();
    ASSERT_STATE(E2E_SM_INVALID, feed_n(OK, 3U, E2E_SM_INVALID));
    ASSERT_STATE(E2E_SM_VALID, feed(OK)); /* the 4th OK after the error */
}

/* INIT -> INVALID keeps the window too, and without a new error INVALID waits for the errors
 * to leave it: E E, then OKs; after 4 OKs there are 4 but still 2 errors, after 5 one error,
 * after 6 none. */
static void test_invalid_recovers_once_old_errors_have_left_the_window(void)
{
    start();
    (void)feed(OK);
    (void)feed(ERR);
    ASSERT_STATE(E2E_SM_INVALID, feed(ERR)); /* E E */
    ASSERT_STATE(E2E_SM_INVALID, feed_n(OK, 5U, E2E_SM_INVALID));
    TEST_ASSERT_EQUAL_UINT8(1U, s_sm.error_count); /* E O O O O O */
    ASSERT_STATE(E2E_SM_VALID, feed(OK));
    TEST_ASSERT_EQUAL_UINT8(0U, s_sm.error_count);
}

/*
 * In INVALID the condition for VALID is tested before the clearing on an error (Fig. 6.131).
 * The order shows only when an ERROR arrives in INVALID while the window is within the limits
 * of INVALID. That needs a state just entered from VALID, with fewer OKs needed in INVALID than
 * in VALID. Window 6; INIT 4 OKs / 1 error; VALID 5 / 1; INVALID 3 / 1:
 *
 *     INIT     R E O O O O      4 OKs, 1 error            -> VALID
 *     VALID    E O O O O N      4 OKs: fewer than 5        -> INVALID
 *     INVALID  O O O O N E      4 OKs, 1 error: within     -> VALID, the window kept
 *
 * Clearing first would have left INVALID with an empty window.
 */
static void test_invalid_tests_for_valid_before_clearing_on_an_error(void)
{
    static const e2e_p_check_status_t verdicts[9] = {OK, REP, ERR, OK, OK, OK, OK, NONEW, ERR};
    static const e2e_sm_state_t states[9] = {E2E_SM_INIT,  E2E_SM_INIT,    E2E_SM_INIT,
                                             E2E_SM_INIT,  E2E_SM_INIT,    E2E_SM_INIT,
                                             E2E_SM_VALID, E2E_SM_INVALID, E2E_SM_VALID};
    const e2e_sm_config_t config = {.window_size = 6U,
                                    .min_ok_state_init = 4U,
                                    .max_error_state_init = 1U,
                                    .min_ok_state_valid = 5U,
                                    .max_error_state_valid = 1U,
                                    .min_ok_state_invalid = 3U,
                                    .max_error_state_invalid = 1U};

    start_with(&config);

    for (size_t i = 0U; i < 9U; i++) {
        TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_sm_check(verdicts[i], &config, &s_sm));
        ASSERT_STATE_MESSAGE(states[i], s_sm.sm_state, at("step", i));
    }

    TEST_ASSERT_EQUAL_UINT8(4U, s_sm.ok_count);
    TEST_ASSERT_EQUAL_UINT8(1U, s_sm.error_count);
}

/* ------------------------------------------------------------------------------------------
 * Against a reference model
 * ---------------------------------------------------------------------------------------- */

/*
 * The state machine as the header's table says, with the window kept another way: every
 * verdict since the last clearing in one long array, and the counts taken over its last
 * window_size entries (missing ones are NOTAVAILABLE), in every state but NODATA, where they
 * stay 0 (Fig. 6.133). No ring and no top index, so a fault in the ring arithmetic of the code
 * under test shows as a difference.
 */
#define REFERENCE_HISTORY 4096U

typedef struct {
    e2e_p_check_status_t history[REFERENCE_HISTORY];
    size_t length; /* verdicts since the last clearing */
    e2e_sm_state_t state;
    uint8_t ok_count;
    uint8_t error_count;
} reference_t;

static reference_t s_reference;

static void reference_clear(reference_t *ref)
{
    ref->length = 0U;
    ref->ok_count = 0U;
    ref->error_count = 0U;
}

static void reference_step(reference_t *ref, const e2e_sm_config_t *config,
                           e2e_p_check_status_t verdict)
{
    TEST_ASSERT_TRUE(ref->length < REFERENCE_HISTORY);
    ref->history[ref->length] = verdict;
    ref->length++;

    if (ref->state != E2E_SM_NODATA) {
        ref->ok_count = 0U;
        ref->error_count = 0U;

        for (size_t back = 0U; (back < config->window_size) && (back < ref->length); back++) {
            const e2e_p_check_status_t v = ref->history[ref->length - 1U - back];

            ref->ok_count = (uint8_t)(ref->ok_count + ((v == OK) ? 1U : 0U));
            ref->error_count = (uint8_t)(ref->error_count + ((v == ERR) ? 1U : 0U));
        }
    }

    switch (ref->state) {
    case E2E_SM_NODATA:
        if ((verdict != ERR) && (verdict != NONEW)) {
            reference_clear(ref);
            ref->state = E2E_SM_INIT;
        }
        break;
    case E2E_SM_INIT:
        if ((ref->error_count <= config->max_error_state_init) &&
            (ref->ok_count >= config->min_ok_state_init)) {
            ref->state = E2E_SM_VALID;
        } else if (ref->error_count > config->max_error_state_init) {
            ref->state = E2E_SM_INVALID;
        } else {
            /* stay */
        }
        break;
    case E2E_SM_VALID:
        if ((ref->error_count > config->max_error_state_valid) ||
            (ref->ok_count < config->min_ok_state_valid)) {
            ref->state = E2E_SM_INVALID;
        }
        break;
    default: /* INVALID */
        if ((ref->error_count <= config->max_error_state_invalid) &&
            (ref->ok_count >= config->min_ok_state_invalid)) {
            ref->state = E2E_SM_VALID;
        } else if (verdict == ERR) {
            reference_clear(ref);
        } else {
            /* stay */
        }
        break;
    }
}

static uint8_t random_below(uint32_t bound)
{
    return (uint8_t)(random_u32() % bound);
}

/* Mostly OKs, so that every state is reached; a run of one kind of verdict now and then, so
 * that windows fill with it. */
static e2e_p_check_status_t random_verdict(void)
{
    static const e2e_p_check_status_t others[4] = {REP, WSEQ, ERR, NONEW};

    return (random_below(10U) < 6U) ? OK : others[random_below(4U)];
}

/* 200 random configurations, 1000 random verdicts each: the state and both counts after every
 * step equal those of the reference. Also counts which states were visited, so that the test
 * cannot pass by never leaving NODATA. */
static void test_random_runs_match_the_reference_model(void)
{
    unsigned long visits[5] = {0U};

    for (unsigned int run = 0U; run < 200U; run++) {
        e2e_sm_config_t config;
        e2e_p_check_status_t repeat = OK;
        unsigned int repeat_left = 0U;

        config.window_size = (uint8_t)(1U + random_below(E2E_SM_WINDOW_MAX));
        config.min_ok_state_init = (uint8_t)(1U + random_below(config.window_size));
        config.max_error_state_init = random_below(config.window_size);
        config.min_ok_state_valid = (uint8_t)(1U + random_below(config.window_size));
        config.max_error_state_valid = random_below(config.window_size);
        config.min_ok_state_invalid = (uint8_t)(1U + random_below(config.window_size));
        config.max_error_state_invalid = random_below(config.window_size);

        start_with(&config);
        reference_clear(&s_reference);
        s_reference.state = E2E_SM_NODATA;

        for (unsigned int n = 0U; n < 1000U; n++) {
            e2e_p_check_status_t verdict = random_verdict();

            if (repeat_left > 0U) {
                verdict = repeat;
                repeat_left--;
            } else if (random_below(50U) == 0U) {
                repeat = verdict;
                repeat_left = random_below(20U);
            } else {
                /* a single random verdict */
            }

            TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_sm_check(verdict, &config, &s_sm));
            reference_step(&s_reference, &config, verdict);

            ASSERT_STATE_MESSAGE(s_reference.state, s_sm.sm_state,
                                 at("run * 10000 + step", (run * 10000UL) + n));
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(s_reference.ok_count, s_sm.ok_count,
                                            at("run * 10000 + step", (run * 10000UL) + n));
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(s_reference.error_count, s_sm.error_count,
                                            at("run * 10000 + step", (run * 10000UL) + n));
            visits[s_sm.sm_state]++;
        }
    }

    TEST_ASSERT_GREATER_THAN_UINT32(0U, visits[E2E_SM_NODATA]);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, visits[E2E_SM_INIT]);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, visits[E2E_SM_VALID]);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, visits[E2E_SM_INVALID]);
}

/* ------------------------------------------------------------------------------------------
 * Profile 11 and the state machine together
 * ---------------------------------------------------------------------------------------- */

/* What happens to the message of one cycle between the sender and the receiver. */
typedef enum {
    SILENT,  /* the sender sends nothing (not started yet) */
    DELIVER, /* the sender protects a message, the receiver gets it intact */
    DAMAGE,  /* ... the receiver gets it with one bit flipped */
    LOSE     /* ... the message is lost: the sender's counter moves on, the receiver sees nothing */
} channel_t;

/*
 * A sender and a receiver of one 8-byte message, cycle by cycle: every cycle the receiver
 * checks once (with no data if nothing arrived), maps the status and steps the state machine.
 * Profile 11 with max_delta_counter 1; the state machine with a window of 6 and the limits
 * INIT 3 OKs / 1 error, VALID 3 OKs / 1 error, INVALID 5 OKs / no error. Message n carries
 * counter n % 15. Window after each cycle, oldest first:
 *
 *   cycle  message       P11 status      window                     state
 *    0- 2  -             NONEWDATA       (NODATA ignores it)        NODATA
 *    3     0             OK              cleared on leaving NODATA  INIT
 *    4- 6  1, 2, 3       OK              O / O O / O O O            INIT, INIT, VALID
 *    7     4, damaged    ERROR           O O O E                    VALID (3 OKs, 1 error)
 *    8     5             WRONGSEQUENCE   O O O E W                  VALID: the counter jumped
 *                                                                   from 3 to 5
 *    9-14  6..11         OK              ... O O O O O O            VALID
 *   15-18  12..15 lost   NONEWDATA       O O O O O N ... O O N N N N  VALID x3, INVALID: the
 *                                                                   timeout, 4 = 6 - 3 + 1
 *   19     16            WRONGSEQUENCE   O N N N N W                INVALID: 11 -> 1 is 5 steps
 *   20-24  17..21        OK              N N N N W O ... W O O O O O  INVALID x4, VALID at the
 *                                                                   5th OK
 *
 * With max_delta_counter 1, one damaged message costs two verdicts: its own ERROR, and
 * WRONGSEQUENCE for the next one, whose counter is two steps after the last one received.
 */
static void test_profile_11_through_the_state_machine(void)
{
    static const struct {
        channel_t channel;
        e2e_p11_status_t status;
        e2e_sm_state_t state;
    } cycles[] = {
        {SILENT, E2E_P11STATUS_NONEWDATA, E2E_SM_NODATA},       /* 0 */
        {SILENT, E2E_P11STATUS_NONEWDATA, E2E_SM_NODATA},       /* 1 */
        {SILENT, E2E_P11STATUS_NONEWDATA, E2E_SM_NODATA},       /* 2 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_INIT},               /* 3 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_INIT},               /* 4 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_INIT},               /* 5 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_VALID},              /* 6 */
        {DAMAGE, E2E_P11STATUS_ERROR, E2E_SM_VALID},            /* 7 */
        {DELIVER, E2E_P11STATUS_WRONGSEQUENCE, E2E_SM_VALID},   /* 8 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_VALID},              /* 9 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_VALID},              /* 10 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_VALID},              /* 11 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_VALID},              /* 12 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_VALID},              /* 13 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_VALID},              /* 14 */
        {LOSE, E2E_P11STATUS_NONEWDATA, E2E_SM_VALID},          /* 15 */
        {LOSE, E2E_P11STATUS_NONEWDATA, E2E_SM_VALID},          /* 16 */
        {LOSE, E2E_P11STATUS_NONEWDATA, E2E_SM_VALID},          /* 17 */
        {LOSE, E2E_P11STATUS_NONEWDATA, E2E_SM_INVALID},        /* 18 */
        {DELIVER, E2E_P11STATUS_WRONGSEQUENCE, E2E_SM_INVALID}, /* 19 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_INVALID},            /* 20 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_INVALID},            /* 21 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_INVALID},            /* 22 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_INVALID},            /* 23 */
        {DELIVER, E2E_P11STATUS_OK, E2E_SM_VALID},              /* 24 */
    };
    const e2e_p11_config_t p11 = {.data_id = 0x0123U, .data_len = 8U, .max_delta_counter = 1U};
    const e2e_sm_config_t sm = {.window_size = 6U,
                                .min_ok_state_init = 3U,
                                .max_error_state_init = 1U,
                                .min_ok_state_valid = 3U,
                                .max_error_state_valid = 1U,
                                .min_ok_state_invalid = 5U,
                                .max_error_state_invalid = 0U};
    e2e_p11_protect_state_t sender;
    e2e_p11_check_state_t receiver;

    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect_init(&sender));
    TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_check_init(&receiver));
    start_with(&sm);

    for (size_t c = 0U; c < (sizeof(cycles) / sizeof(cycles[0])); c++) {
        uint8_t data[8] = {0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U, 0x70U, 0x80U};
        const uint8_t *received = NULL;
        e2e_result_t checked;

        if (cycles[c].channel != SILENT) {
            TEST_ASSERT_EQUAL_HEX8(E2E_E_OK, e2e_p11_protect(&p11, &sender, data, 8U));
        }

        if (cycles[c].channel == DELIVER) {
            received = data;
        } else if (cycles[c].channel == DAMAGE) {
            data[6] ^= 0x04U;
            received = data;
        } else {
            /* SILENT or LOSE: nothing arrives */
        }

        checked = e2e_p11_check(&p11, &receiver, received, (received != NULL) ? 8U : 0U);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(E2E_E_OK, checked, at("cycle", c));
        TEST_ASSERT_EQUAL_INT_MESSAGE(cycles[c].status, receiver.status, at("cycle", c));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(
            E2E_E_OK, e2e_sm_check(e2e_p11_map_status_to_sm(checked, receiver.status), &sm, &s_sm),
            at("cycle", c));
        ASSERT_STATE_MESSAGE(cycles[c].state, s_sm.sm_state, at("cycle", c));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_init_empties_the_window_and_enters_nodata);
    RUN_TEST(test_check_init_refuses_null_and_bad_configurations);
    RUN_TEST(test_check_init_accepts_the_boundaries);
    RUN_TEST(test_check_before_init_is_wrongstate);
    RUN_TEST(test_check_refuses_wrong_input_and_changes_nothing);
    RUN_TEST(test_nodata_ignores_errors_and_missing_data);
    RUN_TEST(test_nodata_leaves_for_init_on_any_message_without_error);
    RUN_TEST(test_the_verdict_that_leaves_nodata_is_not_counted);
    RUN_TEST(test_init_becomes_valid_with_enough_oks_and_few_errors);
    RUN_TEST(test_init_becomes_invalid_with_too_many_errors);
    RUN_TEST(test_init_waits_through_counter_faults_and_missing_data);
    RUN_TEST(test_valid_stays_within_its_limits);
    RUN_TEST(test_valid_becomes_invalid_with_too_many_errors);
    RUN_TEST(test_timeout_through_cycles_without_data);
    RUN_TEST(test_valid_to_invalid_keeps_the_window);
    RUN_TEST(test_an_error_in_invalid_clears_the_window);
    RUN_TEST(test_invalid_recovers_once_old_errors_have_left_the_window);
    RUN_TEST(test_invalid_tests_for_valid_before_clearing_on_an_error);
    RUN_TEST(test_random_runs_match_the_reference_model);
    RUN_TEST(test_profile_11_through_the_state_machine);
    return UNITY_END();
}
