/* SPDX-License-Identifier: MIT */
/*
 * The fault-confinement rules of the Bosch CAN Specification 2.0 (Part B, section 8): first
 * one by one, then as the trajectories they produce, then for every state at once.
 *
 * "Rule n" is the specification's numbering, which can_fc.h repeats. The numbers a test
 * expects are worked out from the rules in the comment next to them, and the limits are
 * written as the literals of the specification (128, 256, 96 ...) rather than as the header's
 * macros, so that a changed macro shows up here.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "can/can_fc.h"
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------ helpers */

/* Rules 9 to 11 as the tests read them: the state that belongs to a pair of counters. */
static can_fc_state_t state_for(uint16_t tec, uint16_t rec)
{
    can_fc_state_t state = CAN_FC_ERROR_ACTIVE; /* rule 11: both at or below 127 */

    if ((tec >= 128U) || (rec >= 128U)) {
        state = CAN_FC_ERROR_PASSIVE; /* rule 9 */
    }

    if (tec >= 256U) {
        state = CAN_FC_BUS_OFF; /* rule 10 */
    }

    return state;
}

/* A node with the given counters, in the state that belongs to them, no restart requested.
 * The structure is public; building it directly lets a test start at any value without first
 * trusting the code under test to get there. */
static can_fc_t node_with(uint16_t tec, uint16_t rec)
{
    can_fc_t fc;

    can_fc_init(&fc);
    fc.tec = tec;
    fc.rec = rec;
    fc.state = state_for(tec, rec);

    return fc;
}

/* A bus-off node (`tec` 256..263) in one of the 129 situations it can be in: situation 0 is
 * "no restart requested", situation n above 0 is a requested restart with n - 1 of the 128
 * occurrences counted so far. */
#define BUS_OFF_SITUATIONS 129U

static can_fc_t bus_off_node(uint16_t tec, uint16_t rec, uint16_t situation)
{
    can_fc_t fc = node_with(tec, rec);

    if (situation > 0U) {
        fc.restart_requested = true;
        fc.recovery_occurrences = (uint16_t)(situation - 1U);
    }

    return fc;
}

/* Apply the same event `times` times; the state after the last one. */
static can_fc_state_t apply_n(can_fc_t *fc, can_fc_event_t event, uint32_t times)
{
    can_fc_state_t state = fc->state;

    for (uint32_t i = 0U; i < times; i++) {
        state = can_fc_apply(fc, event);
    }

    return state;
}

static bool same(const can_fc_t *a, const can_fc_t *b)
{
    return (a->tec == b->tec) && (a->rec == b->rec) && (a->state == b->state) &&
           (a->restart_requested == b->restart_requested) &&
           (a->recovery_occurrences == b->recovery_occurrences);
}

/* State and both counters in one line; a failure is reported at the line of the caller. */
#define ASSERT_NODE(state, tec, rec, fc) assert_node((state), (tec), (rec), (fc), __LINE__)

static void assert_node(can_fc_state_t state, uint16_t tec, uint16_t rec, const can_fc_t *fc,
                        int line)
{
    UNITY_TEST_ASSERT_EQUAL_INT(state, fc->state, line, "error state");
    UNITY_TEST_ASSERT_EQUAL_UINT16(tec, fc->tec, line, "transmit error count");
    UNITY_TEST_ASSERT_EQUAL_UINT16(rec, fc->rec, line, "receive error count");
}

/* Every field, the recovery bookkeeping included. */
#define ASSERT_SAME(expected, actual) assert_same((expected), (actual), __LINE__)

static void assert_same(const can_fc_t *expected, const can_fc_t *actual, int line)
{
    assert_node(expected->state, expected->tec, expected->rec, actual, line);
    UNITY_TEST_ASSERT(expected->restart_requested == actual->restart_requested, line,
                      "restart request");
    UNITY_TEST_ASSERT_EQUAL_UINT16(expected->recovery_occurrences, actual->recovery_occurrences,
                                   line, "recovery count");
}

/* The only counter the rules let an event change in a node that is not bus off. */
typedef enum { CHANGES_NOTHING = 0, CHANGES_TEC, CHANGES_REC } changes_t;

typedef struct {
    const char *name;
    changes_t changes;
} event_info_t;

/* The table of can_fc.h, one entry per event, and one for CAN_FC_EVENT_COUNT, which is not an
 * event and has to be ignored like any stray value. */
#define EVENT_VALUES ((unsigned int)CAN_FC_EVENT_COUNT + 1U)

static const event_info_t s_events[EVENT_VALUES] = {
    [CAN_FC_RX_ERROR] = {"RX_ERROR", CHANGES_REC},
    [CAN_FC_RX_DOMINANT_AFTER_FLAG] = {"RX_DOMINANT_AFTER_FLAG", CHANGES_REC},
    [CAN_FC_TX_ERROR] = {"TX_ERROR", CHANGES_TEC},
    [CAN_FC_TX_ACK_ERROR_ALONE] = {"TX_ACK_ERROR_ALONE", CHANGES_TEC},
    [CAN_FC_TX_STUFF_ERROR_IN_ARBITRATION] = {"TX_STUFF_ERROR_IN_ARBITRATION", CHANGES_NOTHING},
    [CAN_FC_TX_BIT_ERROR_IN_FLAG] = {"TX_BIT_ERROR_IN_FLAG", CHANGES_TEC},
    [CAN_FC_RX_BIT_ERROR_IN_FLAG] = {"RX_BIT_ERROR_IN_FLAG", CHANGES_REC},
    [CAN_FC_TX_DOMINANT_RUN] = {"TX_DOMINANT_RUN", CHANGES_TEC},
    [CAN_FC_RX_DOMINANT_RUN] = {"RX_DOMINANT_RUN", CHANGES_REC},
    [CAN_FC_TX_SUCCESS] = {"TX_SUCCESS", CHANGES_TEC},
    [CAN_FC_RX_SUCCESS] = {"RX_SUCCESS", CHANGES_REC},
    [CAN_FC_RECESSIVE_11] = {"RECESSIVE_11", CHANGES_NOTHING},
    [CAN_FC_EVENT_COUNT] = {"(not an event)", CHANGES_NOTHING},
};

/* The tests in this file know twelve events. A thirteenth in the header would get an empty
 * entry in the table above, be swept as an event that changes nothing, and pass without a rule
 * implemented for it. The build stops here instead, until the new event has its entry above
 * and a test of its rule below. */
_Static_assert((unsigned int)CAN_FC_EVENT_COUNT == 12U,
               "can_fc.h has a new event: add it to s_events and test its rule");

/* Fail with the whole step in the message: the node before, what was done to it, the node
 * after. The sweeps below go through millions of steps, and "expected 128 was 120" alone
 * would not say which one went wrong. */
static void fail_change(const char *what, const can_fc_t *before, const char *cause,
                        const can_fc_t *after)
{
    char message[256];

    (void)snprintf(message, sizeof(message),
                   "%s: TEC %u REC %u state %d restart %d count %u, then %s, gives "
                   "TEC %u REC %u state %d restart %d count %u",
                   what, (unsigned int)before->tec, (unsigned int)before->rec, (int)before->state,
                   (int)before->restart_requested, (unsigned int)before->recovery_occurrences,
                   cause, (unsigned int)after->tec, (unsigned int)after->rec, (int)after->state,
                   (int)after->restart_requested, (unsigned int)after->recovery_occurrences);
    TEST_FAIL_MESSAGE(message);
}

static void fail_step(const char *what, const can_fc_t *before, can_fc_event_t event,
                      const can_fc_t *after)
{
    const char *name = ((unsigned int)event < EVENT_VALUES) ? s_events[event].name : NULL;

    fail_change(what, before, (name != NULL) ? name : "an event that s_events does not list",
                after);
}

typedef struct {
    uint16_t tec;
    uint16_t rec;
} counters_t;

/* What one rule makes of the counters of a node that is not bus off. The functions below are
 * rules 1 to 8 as the tests read them, each on its own. */
typedef counters_t (*rule_fn)(counters_t c);

static counters_t unchanged(counters_t c)
{
    return c;
}

static counters_t tec_plus_8(counters_t c)
{
    c.tec = (uint16_t)(c.tec + 8U);
    return c;
}

/* Rule 3 as long as the node is error active (both counters at or below 127), exception 1
 * from then on. */
static counters_t tec_plus_8_while_error_active(counters_t c)
{
    if ((c.tec <= 127U) && (c.rec <= 127U)) {
        c.tec = (uint16_t)(c.tec + 8U);
    }

    return c;
}

static counters_t tec_minus_1_not_below_0(counters_t c)
{
    if (c.tec >= 1U) {
        c.tec--;
    }

    return c;
}

static counters_t rec_plus_1_up_to_255(counters_t c)
{
    if (c.rec <= 254U) {
        c.rec++;
    }

    return c;
}

static counters_t rec_plus_8_up_to_255(counters_t c)
{
    if (c.rec <= 247U) {
        c.rec = (uint16_t)(c.rec + 8U);
    } else {
        c.rec = 255U;
    }

    return c;
}

/* Rule 8. */
static counters_t rec_minus_1_or_127_from_above(counters_t c)
{
    if (c.rec >= 128U) {
        c.rec = 127U;
    } else if (c.rec >= 1U) {
        c.rec--;
    } else {
        /* 0 stays 0 */
    }

    return c;
}

/* Apply `event` to every node that is not bus off, TEC 0..255 x REC 0..255, and require the
 * counters `rule` gives, the state that belongs to them, and nothing else touched. */
static void expect_from_every_state(can_fc_event_t event, rule_fn rule)
{
    for (uint16_t tec = 0U; tec <= 255U; tec++) {
        for (uint16_t rec = 0U; rec <= 255U; rec++) {
            const counters_t start = {tec, rec};
            const counters_t want = rule(start);
            const can_fc_t before = node_with(tec, rec);
            const can_fc_t expected = node_with(want.tec, want.rec);
            can_fc_t after = before;
            const can_fc_state_t returned = can_fc_apply(&after, event);

            if (!same(&expected, &after) || (returned != expected.state)) {
                fail_step("not what the rule says", &before, event, &after);
            }
        }
    }
}

/* Reset, 37 receive errors, then 32 transmit errors: 32 x 8 = 256, bus off. The receive
 * count is there to show that bus off keeps it and that the recovery clears it. */
static void go_bus_off(can_fc_t *fc)
{
    can_fc_init(fc);
    (void)apply_n(fc, CAN_FC_RX_ERROR, 37U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(fc, CAN_FC_TX_ERROR, 32U));
    ASSERT_NODE(CAN_FC_BUS_OFF, 256U, 37U, fc);
}

/* ------------------------------------------------------------------ reset */

static void test_init_gives_an_error_active_node_with_both_counters_at_zero(void)
{
    can_fc_t fc;

    /* Every field starts out wrong, so that a field init forgets is seen. */
    fc.tec = 300U;
    fc.rec = 200U;
    fc.state = CAN_FC_BUS_OFF;
    fc.restart_requested = true;
    fc.recovery_occurrences = 99U;

    can_fc_init(&fc);

    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 0U, &fc);
    TEST_ASSERT_FALSE(fc.restart_requested);
    TEST_ASSERT_EQUAL_UINT16(0U, fc.recovery_occurrences);
    TEST_ASSERT_FALSE(can_fc_warning(&fc));
}

/* ------------------------------------------------------------------ rules 1 to 8, one by one */

static void test_rule_1_a_receive_error_adds_1_to_the_receive_counter(void)
{
    can_fc_t fc;

    can_fc_init(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 1U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 2U, &fc);

    expect_from_every_state(CAN_FC_RX_ERROR, rec_plus_1_up_to_255);
}

static void test_rule_2_a_dominant_bit_after_the_error_flag_adds_8_to_the_receive_counter(void)
{
    can_fc_t fc;

    can_fc_init(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_DOMINANT_AFTER_FLAG));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 8U, &fc);

    /* The receiver that signals an error before the other nodes gets rule 1 for the error and
     * rule 2 for the dominant bit after its flag: 1 + 8 = 9. */
    can_fc_init(&fc);
    (void)can_fc_apply(&fc, CAN_FC_RX_ERROR);
    (void)can_fc_apply(&fc, CAN_FC_RX_DOMINANT_AFTER_FLAG);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 9U, &fc);

    expect_from_every_state(CAN_FC_RX_DOMINANT_AFTER_FLAG, rec_plus_8_up_to_255);
}

static void test_rule_3_a_transmit_error_adds_8_to_the_transmit_counter(void)
{
    can_fc_t fc;

    can_fc_init(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 8U, 0U, &fc);

    /* Being error passive changes nothing about rule 3: 200 + 8 = 208. */
    fc = node_with(200U, 0U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 208U, 0U, &fc);

    expect_from_every_state(CAN_FC_TX_ERROR, tec_plus_8);
}

static void test_rule_3_exception_2_a_stuff_error_in_arbitration_never_changes_anything(void)
{
    can_fc_t fc;

    can_fc_init(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE,
                          apply_n(&fc, CAN_FC_TX_STUFF_ERROR_IN_ARBITRATION, 1000U));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 0U, &fc);

    expect_from_every_state(CAN_FC_TX_STUFF_ERROR_IN_ARBITRATION, unchanged);

    /* Nor while bus off, with a recovery under way: 100 of the 128 occurrences counted. */
    go_bus_off(&fc);
    can_fc_request_restart(&fc);
    (void)apply_n(&fc, CAN_FC_RECESSIVE_11, 100U);

    const can_fc_t recovering = fc;

    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF,
                          apply_n(&fc, CAN_FC_TX_STUFF_ERROR_IN_ARBITRATION, 1000U));
    ASSERT_SAME(&recovering, &fc);
}

static void test_rule_4_a_transmitters_bit_error_in_its_flag_adds_8_to_the_transmit_counter(void)
{
    can_fc_t fc;

    can_fc_init(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_TX_BIT_ERROR_IN_FLAG));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 8U, 0U, &fc);

    /* A transmitter that cannot put its error flag on the bus either gets rule 3 for the
     * error and rule 4 for the flag: 8 + 8 = 16. */
    can_fc_init(&fc);
    (void)can_fc_apply(&fc, CAN_FC_TX_ERROR);
    (void)can_fc_apply(&fc, CAN_FC_TX_BIT_ERROR_IN_FLAG);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 16U, 0U, &fc);

    expect_from_every_state(CAN_FC_TX_BIT_ERROR_IN_FLAG, tec_plus_8);
}

static void test_rule_5_a_receivers_bit_error_in_its_flag_adds_8_to_the_receive_counter(void)
{
    can_fc_t fc;

    can_fc_init(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_BIT_ERROR_IN_FLAG));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 8U, &fc);

    expect_from_every_state(CAN_FC_RX_BIT_ERROR_IN_FLAG, rec_plus_8_up_to_255);
}

static void test_rule_6_a_dominant_run_adds_8_to_the_counter_of_the_role_the_node_has(void)
{
    can_fc_t fc;

    can_fc_init(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_TX_DOMINANT_RUN));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 8U, 0U, &fc);

    can_fc_init(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_DOMINANT_RUN));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 8U, &fc);

    expect_from_every_state(CAN_FC_TX_DOMINANT_RUN, tec_plus_8);
    expect_from_every_state(CAN_FC_RX_DOMINANT_RUN, rec_plus_8_up_to_255);
}

static void test_rule_7_a_transmitted_frame_takes_1_off_the_transmit_counter_but_not_below_0(void)
{
    can_fc_t fc;

    can_fc_init(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_TX_SUCCESS));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 0U, &fc); /* 0 stays 0, it does not wrap to 65535 */

    (void)can_fc_apply(&fc, CAN_FC_TX_ERROR); /* 8 */
    (void)apply_n(&fc, CAN_FC_TX_SUCCESS, 7U);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 1U, 0U, &fc); /* 8 - 7 */
    (void)can_fc_apply(&fc, CAN_FC_TX_SUCCESS);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 0U, &fc);
    (void)can_fc_apply(&fc, CAN_FC_TX_SUCCESS);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 0U, &fc);

    expect_from_every_state(CAN_FC_TX_SUCCESS, tec_minus_1_not_below_0);
}

static void test_rule_8_a_received_frame_takes_1_off_up_to_127_and_sets_127_from_above(void)
{
    can_fc_t fc;

    /* The rule allows any value from 119 to 127 here; the header chose, and the numbers
     * below are worked out for that choice. */
    TEST_ASSERT_EQUAL_UINT(127U, CAN_FC_REC_AFTER_PASSIVE);

    can_fc_init(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_SUCCESS));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 0U, &fc); /* 0 stays 0 */

    fc = node_with(0U, 1U);
    (void)can_fc_apply(&fc, CAN_FC_RX_SUCCESS);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 0U, &fc);

    fc = node_with(0U, 127U); /* the highest value that is counted down */
    (void)can_fc_apply(&fc, CAN_FC_RX_SUCCESS);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 126U, &fc);

    fc = node_with(0U, 128U); /* the lowest value "greater than 127" */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_SUCCESS));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 127U, &fc);

    fc = node_with(0U, 255U); /* from the maximum, one good frame is enough as well */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_SUCCESS));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 127U, &fc);

    expect_from_every_state(CAN_FC_RX_SUCCESS, rec_minus_1_or_127_from_above);
}

static void test_the_receive_counter_stops_at_its_maximum_and_never_wraps(void)
{
    static const can_fc_event_t plus_8[] = {CAN_FC_RX_DOMINANT_AFTER_FLAG,
                                            CAN_FC_RX_BIT_ERROR_IN_FLAG, CAN_FC_RX_DOMINANT_RUN};
    can_fc_t fc;

    /* The numbers below are worked out for a maximum of 255. */
    TEST_ASSERT_EQUAL_UINT(255U, CAN_FC_REC_MAX);

    fc = node_with(0U, 254U);
    (void)can_fc_apply(&fc, CAN_FC_RX_ERROR);
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 255U, &fc); /* 254 + 1: the last step that fits */
    (void)can_fc_apply(&fc, CAN_FC_RX_ERROR);
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 255U, &fc); /* 256 would be 0 in 8 bits */

    for (size_t i = 0U; i < (sizeof(plus_8) / sizeof(plus_8[0])); i++) {
        fc = node_with(0U, 247U);
        (void)can_fc_apply(&fc, plus_8[i]);
        ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 255U, &fc); /* 247 + 8: the last step that fits */

        fc = node_with(0U, 248U);
        (void)can_fc_apply(&fc, plus_8[i]);
        ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 255U, &fc); /* 256 would be 0 in 8 bits */

        fc = node_with(0U, 255U);
        (void)can_fc_apply(&fc, plus_8[i]);
        ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 255U, &fc); /* 263 would be 7 in 8 bits */
    }
}

/* ------------------------------------------------------------------ rules 9 to 11: the limits */

static void test_rule_9_error_passive_starts_at_128_of_the_transmit_counter(void)
{
    can_fc_t fc = node_with(119U, 0U);

    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 127U, 0U, &fc); /* 119 + 8: one below the limit */

    fc = node_with(120U, 0U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 128U, 0U, &fc); /* 120 + 8: at the limit */

    fc = node_with(121U, 0U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 129U, 0U, &fc); /* 121 + 8: one above */
}

static void test_rule_9_error_passive_starts_at_128_of_the_receive_counter(void)
{
    can_fc_t fc = node_with(0U, 126U);

    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 127U, &fc); /* 126 + 1: one below the limit */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_RX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 128U, &fc); /* 127 + 1: at the limit */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_RX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 129U, &fc); /* 128 + 1: one above */

    /* The same limit reached in a step of 8. */
    fc = node_with(0U, 119U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_DOMINANT_AFTER_FLAG));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 127U, &fc); /* 119 + 8 */

    fc = node_with(0U, 120U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_RX_DOMINANT_AFTER_FLAG));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 128U, &fc); /* 120 + 8 */
}

static void test_a_bit_error_in_a_flag_still_counts_once_the_node_is_error_passive(void)
{
    /* Rule 9, last sentence: the error that makes a node error passive is still signalled
     * with an ACTIVE error flag, and an overload flag is dominant in every state. A bit error
     * in such a flag (rules 4 and 5) therefore reaches a node that is error passive already,
     * and the engine must not refuse it on the grounds that a passive flag cannot fail.
     *
     * Transmitter at 120: the error makes 128 (rule 3) and the node error passive; the bit
     * error in the active flag that still follows makes 136 (rule 4). */
    can_fc_t fc = node_with(120U, 0U);

    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 128U, 0U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_BIT_ERROR_IN_FLAG));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 136U, 0U, &fc);

    /* Receiver at 127: 128 for the error (rule 1), 136 for the bit error in its flag
     * (rule 5, which takes the place of rule 1 for that second error). */
    fc = node_with(0U, 127U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_RX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 128U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_RX_BIT_ERROR_IN_FLAG));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 136U, &fc);
}

static void test_rule_10_bus_off_starts_at_256_of_the_transmit_counter(void)
{
    can_fc_t fc = node_with(247U, 0U);

    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 255U, 0U, &fc); /* 247 + 8: one below the limit */

    fc = node_with(248U, 0U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_BUS_OFF, 256U, 0U, &fc); /* 248 + 8: at the limit */

    /* 255 + 8 = 263, the highest count there is. It is kept as it is: an 8-bit register
     * would hold 7 here and the node would look error active. */
    fc = node_with(255U, 0U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_BUS_OFF, 263U, 0U, &fc);

    /* Rule 10 outranks rule 9: with both counters above 127 the node is bus off, not error
     * passive. */
    fc = node_with(248U, 200U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_BUS_OFF, 256U, 200U, &fc);
}

static void test_rule_11_error_active_returns_only_when_both_counters_are_at_or_below_127(void)
{
    can_fc_t fc = node_with(128U, 128U);

    /* The receive counter first: 128 -> 127 by rule 8, but the transmit counter is still 128. */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_RX_SUCCESS));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 128U, 127U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_TX_SUCCESS));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 127U, 127U, &fc);

    /* The other way round: 128 -> 127 by rule 7, but the receive counter is still 128. */
    fc = node_with(128U, 128U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_SUCCESS));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 127U, 128U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_SUCCESS));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 127U, 127U, &fc);

    /* There is no hysteresis: one error later the node is error passive again. */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_RX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 127U, 128U, &fc);
}

/* ------------------------------------------------------------------ trajectories */

static void test_32_consecutive_transmit_errors_take_a_node_from_reset_to_bus_off(void)
{
    can_fc_t fc;

    can_fc_init(&fc);

    /* 8 per error: 15 make 120, the 16th 128 (rule 9), the 31st 248, the 32nd 256 (rule 10). */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, apply_n(&fc, CAN_FC_TX_ERROR, 15U));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 120U, 0U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 128U, 0U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, apply_n(&fc, CAN_FC_TX_ERROR, 15U));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 248U, 0U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_BUS_OFF, 256U, 0U, &fc);
}

static void test_alternating_error_and_success_needs_37_errors_to_reach_bus_off(void)
{
    can_fc_t fc;

    can_fc_init(&fc);

    /* One error and one success net 8 - 1 = 7: the counter is 7k after k pairs and 7k + 8
     * right after the error of pair k + 1.
     *   error passive: 7k + 8 >= 128 from k = 18 (126 + 8 = 134), so at the 19th error;
     *                  the 18th error (k = 17) reaches only 119 + 8 = 127.
     *   bus off:       7k + 8 >= 256 from k = 36 (252 + 8 = 260), so at the 37th error;
     *                  the 36th error (k = 35) reaches only 245 + 8 = 253.
     * 37 errors where 32 in a row would do, and the count at bus off is 260, not 256. */
    for (uint16_t pair = 1U; pair <= 36U; pair++) {
        const can_fc_state_t expected = (pair <= 18U) ? CAN_FC_ERROR_ACTIVE : CAN_FC_ERROR_PASSIVE;

        TEST_ASSERT_EQUAL_INT(expected, can_fc_apply(&fc, CAN_FC_TX_ERROR));
        TEST_ASSERT_EQUAL_UINT16((7U * (pair - 1U)) + 8U, fc.tec);
        TEST_ASSERT_EQUAL_INT(expected, can_fc_apply(&fc, CAN_FC_TX_SUCCESS));
        TEST_ASSERT_EQUAL_UINT16(7U * pair, fc.tec);
    }

    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 252U, 0U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_BUS_OFF, 260U, 0U, &fc);
}

static void test_one_destroyed_frame_in_nine_is_absorbed_without_leaving_error_active(void)
{
    can_fc_t fc;

    can_fc_init(&fc);

    /* 8 up for the error, 1 down for each of the 8 good frames after it: every round ends
     * where it began. This is the error rate the 8:1 weighting tolerates for ever. */
    for (uint32_t round = 0U; round < 1000U; round++) {
        TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_TX_ERROR));
        TEST_ASSERT_EQUAL_UINT16(8U, fc.tec);
        TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, apply_n(&fc, CAN_FC_TX_SUCCESS, 8U));
        TEST_ASSERT_EQUAL_UINT16(0U, fc.tec);
    }
}

static void test_a_node_alone_on_the_bus_becomes_error_passive_but_never_bus_off(void)
{
    can_fc_t fc;

    can_fc_init(&fc);

    /* Nobody acknowledges. While the node is error active this is an ordinary transmit error
     * (rule 3): 15 of them make 120. */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, apply_n(&fc, CAN_FC_TX_ACK_ERROR_ALONE, 15U));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 120U, 0U, &fc);

    /* The 16th still finds the node error active and counts: 128, error passive. */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_ACK_ERROR_ALONE));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 128U, 0U, &fc);

    /* From here on exception 1 applies. With rule 3 instead, 16 more would mean bus off. */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_ACK_ERROR_ALONE));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 128U, 0U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, apply_n(&fc, CAN_FC_TX_ACK_ERROR_ALONE, 10000U));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 128U, 0U, &fc);

    /* A second node comes up and acknowledges: 128 - 1 = 127 (rule 7), error active again
     * (rule 11). */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_TX_SUCCESS));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 127U, 0U, &fc);

    /* Alone once more. The node is error active, so the next missing acknowledgment counts
     * again, 127 + 8 = 135, and the ones after it do not. */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_ACK_ERROR_ALONE));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 135U, 0U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, apply_n(&fc, CAN_FC_TX_ACK_ERROR_ALONE, 10000U));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 135U, 0U, &fc);
}

static void test_exception_1_goes_by_the_error_state_not_by_the_transmit_counter(void)
{
    /* Error passive through the receive counter alone. The transmit counter is far below
     * 128, and the exception applies all the same. */
    can_fc_t fc = node_with(40U, 128U);

    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_ACK_ERROR_ALONE));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 40U, 128U, &fc);

    /* One good reception makes the node error active (rule 8: 128 -> 127), and the same
     * error counts again: 40 + 8 = 48. */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_SUCCESS));
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_TX_ACK_ERROR_ALONE));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 48U, 127U, &fc);

    /* Any other transmit error of an error-passive node counts as always: exception 1 is
     * about the missing acknowledgment only. 128 + 8 = 136. */
    fc = node_with(128U, 0U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_TX_ERROR));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 136U, 0U, &fc);

    expect_from_every_state(CAN_FC_TX_ACK_ERROR_ALONE, tec_plus_8_while_error_active);
}

static void test_a_receiver_on_a_bus_held_dominant_becomes_error_passive_but_never_bus_off(void)
{
    can_fc_t fc;

    can_fc_init(&fc);

    /* What a receiver sees when the bus is held dominant: an error (rule 1), a dominant bit
     * after its flag (rule 2), then one dominant run after the other (rule 6):
     * 1 + 8 = 9, and 9 + 8k after k runs.
     *   error passive: 9 + 8k >= 128 from k = 15 (129); k = 14 gives 121.
     *   the maximum:   k = 30 gives 249; k = 31 would give 257 and is held at 255. */
    (void)can_fc_apply(&fc, CAN_FC_RX_ERROR);
    (void)can_fc_apply(&fc, CAN_FC_RX_DOMINANT_AFTER_FLAG);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 9U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, apply_n(&fc, CAN_FC_RX_DOMINANT_RUN, 14U));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 121U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_RX_DOMINANT_RUN));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 129U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, apply_n(&fc, CAN_FC_RX_DOMINANT_RUN, 15U));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 249U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, can_fc_apply(&fc, CAN_FC_RX_DOMINANT_RUN));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 255U, &fc);

    /* However long it lasts, the receive counter stays at its maximum, and bus off is a
     * matter of the transmit counter alone (rule 10). */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, apply_n(&fc, CAN_FC_RX_DOMINANT_RUN, 10000U));
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 0U, 255U, &fc);

    /* The bus is released and one frame is received: 127 by rule 8, error active. */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RX_SUCCESS));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 127U, &fc);
}

/* ------------------------------------------------------------------ bus off and rule 12 */

static void test_a_bus_off_node_ignores_every_event_except_the_recovery_count(void)
{
    can_fc_t fc;

    go_bus_off(&fc);

    const can_fc_t at_bus_off = fc;

    /* 300 of each: more successes than it would take to count 256 down to 0. */
    for (unsigned int e = 0U; e < EVENT_VALUES; e++) {
        const can_fc_event_t event = (can_fc_event_t)e;

        if (event != CAN_FC_RECESSIVE_11) {
            TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, event, 300U));
            ASSERT_SAME(&at_bus_off, &fc);
        }
    }

    /* Nor do they disturb a recovery that is under way, in either direction: with 100
     * occurrences counted, 28 more are needed afterwards, no fewer and no more. */
    can_fc_request_restart(&fc);
    (void)apply_n(&fc, CAN_FC_RECESSIVE_11, 100U);

    for (unsigned int e = 0U; e < EVENT_VALUES; e++) {
        const can_fc_event_t event = (can_fc_event_t)e;

        if (event != CAN_FC_RECESSIVE_11) {
            TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, event, 300U));
        }
    }

    ASSERT_NODE(CAN_FC_BUS_OFF, 256U, 37U, &fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, CAN_FC_RECESSIVE_11, 27U));
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RECESSIVE_11));
}

static void test_recessive_bits_do_not_count_before_a_restart_is_requested(void)
{
    can_fc_t fc;

    go_bus_off(&fc);

    const can_fc_t at_bus_off = fc;

    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, CAN_FC_RECESSIVE_11, 1000U));
    ASSERT_SAME(&at_bus_off, &fc);

    /* None of the 1000 was banked: the count starts with the request. */
    can_fc_request_restart(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, CAN_FC_RECESSIVE_11, 127U));
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RECESSIVE_11));
}

static void test_recovery_takes_128_occurrences_and_127_are_not_enough(void)
{
    can_fc_t fc;

    go_bus_off(&fc);
    can_fc_request_restart(&fc);
    TEST_ASSERT_TRUE(fc.restart_requested);

    for (uint16_t n = 1U; n <= 127U; n++) {
        TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, can_fc_apply(&fc, CAN_FC_RECESSIVE_11));
        TEST_ASSERT_EQUAL_UINT16(n, fc.recovery_occurrences);
    }

    /* After 127 the node is as it was when it went bus off. */
    ASSERT_NODE(CAN_FC_BUS_OFF, 256U, 37U, &fc);
    TEST_ASSERT_TRUE(fc.restart_requested);

    /* The 128th: error active with BOTH counters at 0 (the receive counter was 37), and
     * nothing left of the recovery. */
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RECESSIVE_11));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 0U, &fc);
    TEST_ASSERT_FALSE(fc.restart_requested);
    TEST_ASSERT_EQUAL_UINT16(0U, fc.recovery_occurrences);
}

static void test_a_second_bus_off_needs_a_new_restart_request(void)
{
    can_fc_t fc;

    go_bus_off(&fc);
    can_fc_request_restart(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, apply_n(&fc, CAN_FC_RECESSIVE_11, 128U));

    /* Back on the bus, and off it again: 32 x 8 = 256. */
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, CAN_FC_TX_ERROR, 32U));
    ASSERT_NODE(CAN_FC_BUS_OFF, 256U, 0U, &fc);

    /* The first request was used up by the first recovery. */
    TEST_ASSERT_FALSE(fc.restart_requested);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, CAN_FC_RECESSIVE_11, 1000U));
    ASSERT_NODE(CAN_FC_BUS_OFF, 256U, 0U, &fc);

    can_fc_request_restart(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, CAN_FC_RECESSIVE_11, 127U));
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RECESSIVE_11));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 0U, &fc);
}

static void test_a_restart_request_does_nothing_unless_the_node_is_bus_off(void)
{
    can_fc_t fc;

    /* In every state on the bus, error active and error passive. */
    for (uint16_t tec = 0U; tec <= 255U; tec++) {
        for (uint16_t rec = 0U; rec <= 255U; rec++) {
            const can_fc_t before = node_with(tec, rec);

            fc = before;
            can_fc_request_restart(&fc);

            if (!same(&before, &fc)) {
                fail_change("the node is not bus off and must not change", &before,
                            "a restart request", &fc);
            }
        }
    }

    /* A request made too early is not remembered either: asked while error active and
     * again while error passive, the node still needs to be asked once it is bus off. */
    can_fc_init(&fc);
    can_fc_request_restart(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, apply_n(&fc, CAN_FC_TX_ERROR, 16U));
    can_fc_request_restart(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, CAN_FC_TX_ERROR, 16U));
    TEST_ASSERT_FALSE(fc.restart_requested);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, CAN_FC_RECESSIVE_11, 1000U));
    ASSERT_NODE(CAN_FC_BUS_OFF, 256U, 0U, &fc);
}

static void test_repeating_the_restart_request_does_not_start_the_count_again(void)
{
    can_fc_t fc;

    go_bus_off(&fc);
    can_fc_request_restart(&fc);
    (void)apply_n(&fc, CAN_FC_RECESSIVE_11, 100U);

    const can_fc_t counting = fc;

    /* A driver that asks on every pass of its loop must not postpone the recovery. */
    can_fc_request_restart(&fc);
    ASSERT_SAME(&counting, &fc);
    TEST_ASSERT_EQUAL_UINT16(100U, fc.recovery_occurrences);

    /* 100 + 27 = 127 leave the node bus off, the 128th since the FIRST request ends it. */
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, CAN_FC_RECESSIVE_11, 27U));
    can_fc_request_restart(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RECESSIVE_11));
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 0U, &fc);

    /* In every bus-off situation the request sets the request and leaves the rest alone, a
     * running count included. */
    for (uint16_t tec = 256U; tec <= 263U; tec++) {
        for (uint16_t rec = 0U; rec <= 255U; rec++) {
            for (uint16_t situation = 0U; situation < BUS_OFF_SITUATIONS; situation++) {
                const can_fc_t before = bus_off_node(tec, rec, situation);
                can_fc_t expected = before;

                expected.restart_requested = true;
                fc = before;
                can_fc_request_restart(&fc);

                if (!same(&expected, &fc)) {
                    fail_change("a request must set the request and nothing else", &before,
                                "a restart request", &fc);
                }
            }
        }
    }
}

static void test_recessive_bits_mean_nothing_to_a_node_that_is_not_bus_off(void)
{
    can_fc_t fc;

    /* An idle bus is an endless series of these. */
    expect_from_every_state(CAN_FC_RECESSIVE_11, unchanged);

    /* And nothing is banked for a later bus off: 1000 of them while error active, 1000
     * while error passive, and the recovery still takes 128 after the request. */
    can_fc_init(&fc);
    (void)apply_n(&fc, CAN_FC_RECESSIVE_11, 1000U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_PASSIVE, apply_n(&fc, CAN_FC_TX_ERROR, 16U));
    (void)apply_n(&fc, CAN_FC_RECESSIVE_11, 1000U);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, CAN_FC_TX_ERROR, 16U));
    can_fc_request_restart(&fc);
    TEST_ASSERT_EQUAL_INT(CAN_FC_BUS_OFF, apply_n(&fc, CAN_FC_RECESSIVE_11, 127U));
    TEST_ASSERT_EQUAL_INT(CAN_FC_ERROR_ACTIVE, can_fc_apply(&fc, CAN_FC_RECESSIVE_11));
}

static void test_a_value_that_is_not_an_event_changes_nothing(void)
{
    /* CAN_FC_EVENT_COUNT is the number of events, not one of them. For a bus-off node the
     * tests of bus off cover it. */
    expect_from_every_state(CAN_FC_EVENT_COUNT, unchanged);
}

/* ------------------------------------------------------------------ the warning */

static void test_the_warning_starts_at_96_of_either_counter(void)
{
    can_fc_t fc;

    /* Transmit counter: 11 errors make 88, the 12th 96, and one good frame takes it to 95. */
    can_fc_init(&fc);
    TEST_ASSERT_FALSE(can_fc_warning(&fc));
    (void)apply_n(&fc, CAN_FC_TX_ERROR, 11U);
    TEST_ASSERT_FALSE(can_fc_warning(&fc));
    (void)can_fc_apply(&fc, CAN_FC_TX_ERROR);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 96U, 0U, &fc);
    TEST_ASSERT_TRUE(can_fc_warning(&fc));
    (void)can_fc_apply(&fc, CAN_FC_TX_SUCCESS);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 95U, 0U, &fc);
    TEST_ASSERT_FALSE(can_fc_warning(&fc));

    /* Receive counter: 95 errors of 1, then the 96th, then one good frame. */
    can_fc_init(&fc);
    (void)apply_n(&fc, CAN_FC_RX_ERROR, 95U);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 95U, &fc);
    TEST_ASSERT_FALSE(can_fc_warning(&fc));
    (void)can_fc_apply(&fc, CAN_FC_RX_ERROR);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 96U, &fc);
    TEST_ASSERT_TRUE(can_fc_warning(&fc));
    (void)can_fc_apply(&fc, CAN_FC_RX_SUCCESS);
    ASSERT_NODE(CAN_FC_ERROR_ACTIVE, 0U, 95U, &fc);
    TEST_ASSERT_FALSE(can_fc_warning(&fc));

    /* Everywhere on the bus, error passive included: either counter at or above 96. */
    for (uint16_t tec = 0U; tec <= 255U; tec++) {
        for (uint16_t rec = 0U; rec <= 255U; rec++) {
            fc = node_with(tec, rec);

            if (can_fc_warning(&fc) != ((tec >= 96U) || (rec >= 96U))) {
                fail_change("wrong warning", &fc, "can_fc_warning()", &fc);
            }
        }
    }
}

static void test_the_warning_is_off_while_the_node_is_bus_off(void)
{
    can_fc_t fc;

    /* On at 248, the last count before bus off; off at 256, where the node reports bus off
     * instead. */
    can_fc_init(&fc);
    (void)apply_n(&fc, CAN_FC_TX_ERROR, 31U);
    ASSERT_NODE(CAN_FC_ERROR_PASSIVE, 248U, 0U, &fc);
    TEST_ASSERT_TRUE(can_fc_warning(&fc));
    (void)can_fc_apply(&fc, CAN_FC_TX_ERROR);
    ASSERT_NODE(CAN_FC_BUS_OFF, 256U, 0U, &fc);
    TEST_ASSERT_FALSE(can_fc_warning(&fc));

    /* Whatever the counters of the bus-off node are, a receive counter above 96 included. */
    for (uint16_t tec = 256U; tec <= 263U; tec++) {
        for (uint16_t rec = 0U; rec <= 255U; rec++) {
            fc = node_with(tec, rec);

            if (can_fc_warning(&fc)) {
                fail_change("warning from a bus-off node", &fc, "can_fc_warning()", &fc);
            }
        }
    }
}

/* ------------------------------------------------------------------ every state at once */

static void test_every_event_from_every_pair_of_counters_keeps_the_invariants(void)
{
    uint32_t steps = 0U;

    /* This test knows nothing of what the single rules do, only what none of them may
     * break. Every state a node on the bus can be in, times every event. */
    for (uint16_t tec = 0U; tec <= 255U; tec++) {
        for (uint16_t rec = 0U; rec <= 255U; rec++) {
            for (unsigned int e = 0U; e < EVENT_VALUES; e++) {
                const can_fc_event_t event = (can_fc_event_t)e;
                const can_fc_t before = node_with(tec, rec);
                can_fc_t after = before;
                const can_fc_state_t returned = can_fc_apply(&after, event);

                if (after.state != state_for(after.tec, after.rec)) {
                    fail_step("rules 9 to 11: the state does not belong to the counters", &before,
                              event, &after);
                }

                if (returned != after.state) {
                    fail_step("the returned state is not the state of the node", &before, event,
                              &after);
                }

                /* The largest step is 8 and a bus-off node counts nothing, so the transmit
                 * counter ends at 255 + 8 at most. */
                if ((after.tec > 263U) || (after.rec > 255U)) {
                    fail_step("a counter left its range", &before, event, &after);
                }

                if ((s_events[e].changes != CHANGES_TEC) && (after.tec != before.tec)) {
                    fail_step("the transmit counter changed on an event of another kind", &before,
                              event, &after);
                }

                if ((s_events[e].changes != CHANGES_REC) && (after.rec != before.rec)) {
                    fail_step("the receive counter changed on an event of another kind", &before,
                              event, &after);
                }

                if (after.restart_requested || (after.recovery_occurrences != 0U)) {
                    fail_step("recovery bookkeeping appeared in a node that was on the bus",
                              &before, event, &after);
                }

                steps++;
            }
        }
    }

    TEST_ASSERT_EQUAL_UINT32(256U * 256U * EVENT_VALUES, steps);
}

static void test_every_event_in_every_bus_off_situation_does_what_rule_12_says(void)
{
    uint32_t recoveries = 0U;

    for (uint16_t tec = 256U; tec <= 263U; tec++) {
        for (uint16_t rec = 0U; rec <= 255U; rec++) {
            for (uint16_t situation = 0U; situation < BUS_OFF_SITUATIONS; situation++) {
                const can_fc_t before = bus_off_node(tec, rec, situation);

                for (unsigned int e = 0U; e < EVENT_VALUES; e++) {
                    const can_fc_event_t event = (can_fc_event_t)e;
                    can_fc_t expected = before;
                    can_fc_t after = before;
                    const can_fc_state_t returned = can_fc_apply(&after, event);

                    /* Only a recessive occurrence after a request does anything at all: it
                     * is counted, and the 128th puts the node back as after a reset. */
                    if ((event == CAN_FC_RECESSIVE_11) && before.restart_requested) {
                        if (before.recovery_occurrences == 127U) {
                            expected = node_with(0U, 0U);
                            recoveries++;
                        } else {
                            expected.recovery_occurrences++;
                        }
                    }

                    if (!same(&expected, &after) || (returned != expected.state)) {
                        fail_step("not what rule 12 says of a bus-off node", &before, event,
                                  &after);
                    }
                }
            }
        }
    }

    /* One recovery for each pair of counters: 8 x 256. */
    TEST_ASSERT_EQUAL_UINT32(8U * 256U, recoveries);
}

/* xorshift32 (Marsaglia): small, and the same sequence on every machine. */
static uint32_t prng_next(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    *state = x;

    return x;
}

static void test_a_long_random_walk_stays_consistent_and_recovers_only_by_rule_12(void)
{
    uint32_t prng = 0xC0FFEE01U; /* fixed seed: the same walk on every run */
    can_fc_t fc;
    /* The test's own record of the recovery, kept without looking at the node's. */
    bool asked = false;
    uint16_t counted = 0U;
    uint32_t went_passive = 0U;
    uint32_t went_bus_off = 0U;
    uint32_t recovered = 0U;

    can_fc_init(&fc);

    for (uint32_t step = 0U; step < 400000U; step++) {
        const uint32_t r = prng_next(&prng);
        const uint32_t bias = (r >> 5U) % 4U;
        const bool was_bus_off = (fc.state == CAN_FC_BUS_OFF);
        can_fc_event_t event = (can_fc_event_t)((r >> 8U) % (uint32_t)CAN_FC_EVENT_COUNT);

        /* A uniform choice of events takes a node bus off within a few hundred steps and
         * leaves it there for thousands. Half of the steps are therefore a good frame while
         * the node is on the bus and a recessive occurrence while it is off, so that the
         * walk spends its time in all three states and recovers often. */
        if (was_bus_off) {
            if (bias >= 2U) {
                event = CAN_FC_RECESSIVE_11;
            }
        } else if (bias == 0U) {
            event = CAN_FC_TX_SUCCESS;
        } else if (bias == 1U) {
            event = CAN_FC_RX_SUCCESS;
        } else {
            /* keep the uniform choice */
        }

        /* One step in 32 asks for a restart first, whatever the state. */
        if ((r % 32U) == 0U) {
            can_fc_request_restart(&fc);

            if (was_bus_off) {
                asked = true;
            }
        }

        const can_fc_t before = fc;
        const can_fc_state_t returned = can_fc_apply(&fc, event);

        if (was_bus_off && asked && (event == CAN_FC_RECESSIVE_11)) {
            counted++;
        }

        if (was_bus_off && (counted == 128U)) {
            if ((fc.state != CAN_FC_ERROR_ACTIVE) || (fc.tec != 0U) || (fc.rec != 0U)) {
                fail_step("rule 12: 128 occurrences after the request did not reset the node",
                          &before, event, &fc);
            }

            asked = false;
            counted = 0U;
            recovered++;
        } else if (was_bus_off) {
            if ((fc.state != CAN_FC_BUS_OFF) || (fc.tec != before.tec) || (fc.rec != before.rec)) {
                fail_step("a bus-off node changed without a completed recovery", &before, event,
                          &fc);
            }
        } else if (fc.state == CAN_FC_BUS_OFF) {
            /* 127 + 8 is nowhere near 256: the way to bus off leads through error passive. */
            if (before.state != CAN_FC_ERROR_PASSIVE) {
                fail_step("bus off straight from error active", &before, event, &fc);
            }

            went_bus_off++;
        } else if ((before.state == CAN_FC_ERROR_ACTIVE) && (fc.state == CAN_FC_ERROR_PASSIVE)) {
            went_passive++;
        } else {
            /* stayed error active or error passive, or returned to error active */
        }

        if ((returned != fc.state) || (fc.state != state_for(fc.tec, fc.rec)) || (fc.tec > 263U) ||
            (fc.rec > 255U)) {
            fail_step("the node left the states the exhaustive tests cover", &before, event, &fc);
        }

        if ((fc.restart_requested != asked) || (fc.recovery_occurrences != counted)) {
            fail_step("the recovery bookkeeping differs from the test's own count", &before, event,
                      &fc);
        }
    }

    /* The walk is worth something only if it went everywhere, many times. */
    TEST_ASSERT_GREATER_THAN_UINT32(100U, went_passive);
    TEST_ASSERT_GREATER_THAN_UINT32(100U, went_bus_off);
    TEST_ASSERT_GREATER_THAN_UINT32(100U, recovered);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_gives_an_error_active_node_with_both_counters_at_zero);
    RUN_TEST(test_rule_1_a_receive_error_adds_1_to_the_receive_counter);
    RUN_TEST(test_rule_2_a_dominant_bit_after_the_error_flag_adds_8_to_the_receive_counter);
    RUN_TEST(test_rule_3_a_transmit_error_adds_8_to_the_transmit_counter);
    RUN_TEST(test_rule_3_exception_2_a_stuff_error_in_arbitration_never_changes_anything);
    RUN_TEST(test_rule_4_a_transmitters_bit_error_in_its_flag_adds_8_to_the_transmit_counter);
    RUN_TEST(test_rule_5_a_receivers_bit_error_in_its_flag_adds_8_to_the_receive_counter);
    RUN_TEST(test_rule_6_a_dominant_run_adds_8_to_the_counter_of_the_role_the_node_has);
    RUN_TEST(test_rule_7_a_transmitted_frame_takes_1_off_the_transmit_counter_but_not_below_0);
    RUN_TEST(test_rule_8_a_received_frame_takes_1_off_up_to_127_and_sets_127_from_above);
    RUN_TEST(test_the_receive_counter_stops_at_its_maximum_and_never_wraps);
    RUN_TEST(test_rule_9_error_passive_starts_at_128_of_the_transmit_counter);
    RUN_TEST(test_rule_9_error_passive_starts_at_128_of_the_receive_counter);
    RUN_TEST(test_a_bit_error_in_a_flag_still_counts_once_the_node_is_error_passive);
    RUN_TEST(test_rule_10_bus_off_starts_at_256_of_the_transmit_counter);
    RUN_TEST(test_rule_11_error_active_returns_only_when_both_counters_are_at_or_below_127);
    RUN_TEST(test_32_consecutive_transmit_errors_take_a_node_from_reset_to_bus_off);
    RUN_TEST(test_alternating_error_and_success_needs_37_errors_to_reach_bus_off);
    RUN_TEST(test_one_destroyed_frame_in_nine_is_absorbed_without_leaving_error_active);
    RUN_TEST(test_a_node_alone_on_the_bus_becomes_error_passive_but_never_bus_off);
    RUN_TEST(test_exception_1_goes_by_the_error_state_not_by_the_transmit_counter);
    RUN_TEST(test_a_receiver_on_a_bus_held_dominant_becomes_error_passive_but_never_bus_off);
    RUN_TEST(test_a_bus_off_node_ignores_every_event_except_the_recovery_count);
    RUN_TEST(test_recessive_bits_do_not_count_before_a_restart_is_requested);
    RUN_TEST(test_recovery_takes_128_occurrences_and_127_are_not_enough);
    RUN_TEST(test_a_second_bus_off_needs_a_new_restart_request);
    RUN_TEST(test_a_restart_request_does_nothing_unless_the_node_is_bus_off);
    RUN_TEST(test_repeating_the_restart_request_does_not_start_the_count_again);
    RUN_TEST(test_recessive_bits_mean_nothing_to_a_node_that_is_not_bus_off);
    RUN_TEST(test_a_value_that_is_not_an_event_changes_nothing);
    RUN_TEST(test_the_warning_starts_at_96_of_either_counter);
    RUN_TEST(test_the_warning_is_off_while_the_node_is_bus_off);
    RUN_TEST(test_every_event_from_every_pair_of_counters_keeps_the_invariants);
    RUN_TEST(test_every_event_in_every_bus_off_situation_does_what_rule_12_says);
    RUN_TEST(test_a_long_random_walk_stays_consistent_and_recovers_only_by_rule_12);
    return UNITY_END();
}
