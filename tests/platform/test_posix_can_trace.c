/* SPDX-License-Identifier: MIT */
/*
 * Tests of the ring of the CAN controller's bus-off trace (platform/posix/posix_can_trace): the
 * numbering, what the ring keeps once it is full, and the text of "can.trace", in whole records
 * only. What posix_can.c records, and when, is tested against the real hardware thread in
 * test_posix_can.c.
 *
 * The last test measures what adding a record costs and prints it; it decides nothing from the
 * host's clock (docs/can-simulation.md quotes the figure with the command).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "posix_can_trace.h"
#include "unity.h"

/* A reply buffer as large as simulation control gives the handler (posix_sim.c). */
#define REPLY_BYTES 987U

void setUp(void)
{
    posix_can_trace_reset();
}

void tearDown(void)
{
}

static posix_can_trace_record_t record_of(uint8_t kind, uint64_t base)
{
    posix_can_trace_record_t record;

    (void)memset(&record, 0, sizeof(record));
    record.kind = kind;
    record.model_ns = base + 1U;
    record.pass_ns = base + 2U;
    record.start_ns = base + 3U;
    record.submit_ns = base + 4U;
    record.attempts_failed = base + 5U;
    record.frames = base + 6U;
    record.value = (uint32_t)(base % 1000U);
    record.state = 2U;
    record.old_state = 1U;
    record.tec = 256U;
    record.rec = 7U;
    return record;
}

/* The longest record there is: every number at its largest. */
static posix_can_trace_record_t longest_record(void)
{
    posix_can_trace_record_t record;

    (void)memset(&record, 0xFF, sizeof(record));
    record.kind = (uint8_t)POSIX_CAN_TRACE_INJECT_TX; /* the longest name with inject_rx */
    return record;
}

/* The number of records in a reply, and the number of the first and the last. Every record is
 * checked to be whole: eleven commas, twelve fields. */
static unsigned int count_records(const char *reply, unsigned long *first, unsigned long *last)
{
    unsigned int count = 0U;
    const char *at = strstr(reply, " r");

    while (at != NULL) {
        const char *end = strchr(&at[1], ' ');
        const size_t length = (end != NULL) ? (size_t)(end - at) : strlen(at);
        unsigned int commas = 0U;
        const unsigned long seq = strtoul(&at[2], NULL, 10);

        for (size_t i = 0U; i < length; i++) {
            commas += (at[i] == ',') ? 1U : 0U;
        }

        TEST_ASSERT_EQUAL_UINT_MESSAGE(11U, commas, "a record cut short");

        if (count == 0U) {
            *first = seq;
        }

        *last = seq;
        count++;
        at = (end != NULL) ? strstr(end, " r") : NULL;
    }

    return count;
}

static void test_an_empty_trace_has_no_records(void)
{
    char reply[REPLY_BYTES];
    posix_can_trace_record_t record;

    TEST_ASSERT_EQUAL_UINT32(0U, posix_can_trace_newest());
    TEST_ASSERT_EQUAL_UINT32(0U, posix_can_trace_oldest());
    TEST_ASSERT_FALSE(posix_can_trace_get(0U, &record));
    TEST_ASSERT_TRUE(posix_can_trace_format(0U, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("oldest=0 newest=0", reply);
}

static void test_records_come_back_with_their_numbers_and_every_field(void)
{
    char reply[REPLY_BYTES];
    posix_can_trace_record_t out;

    for (uint8_t kind = 0U; kind < (uint8_t)POSIX_CAN_TRACE_KINDS; kind++) {
        const posix_can_trace_record_t in = record_of(kind, 1000U * (uint64_t)kind);

        posix_can_trace_add(&in);
    }

    TEST_ASSERT_EQUAL_UINT32(POSIX_CAN_TRACE_KINDS, posix_can_trace_newest());
    TEST_ASSERT_EQUAL_UINT32(0U, posix_can_trace_oldest());
    TEST_ASSERT_TRUE(posix_can_trace_get(3U, &out));
    TEST_ASSERT_EQUAL_UINT8(POSIX_CAN_TRACE_FIRST_TX, out.kind);
    TEST_ASSERT_EQUAL_UINT64(3001U, out.model_ns);
    TEST_ASSERT_EQUAL_UINT64(3005U, out.attempts_failed);
    TEST_ASSERT_FALSE(posix_can_trace_get(POSIX_CAN_TRACE_KINDS, &out));

    TEST_ASSERT_TRUE(posix_can_trace_format(0U, reply, sizeof(reply)));
    TEST_ASSERT_NOT_NULL(strstr(reply, "oldest=0 newest=7 r0=state,1,2,3,4,5,0,2,1,256,7,6 "));
    TEST_ASSERT_NOT_NULL(strstr(reply, " r1=counters,1001,1002,1003,1004,1005,0,2,1,256,7,1006 "));
    TEST_ASSERT_NOT_NULL(strstr(reply, " r3=first_tx,3001,3002,3003,3004,3005,0,2,1,256,7,3006 "));
    TEST_ASSERT_NOT_NULL(strstr(reply, " r6=inject_rx,6001,6002,6003,6004,6005,0,2,1,256,7,6006"));
    TEST_ASSERT_EQUAL_STRING("restart", posix_can_trace_kind_name(POSIX_CAN_TRACE_RESTART));
    TEST_ASSERT_EQUAL_STRING("fault", posix_can_trace_kind_name(POSIX_CAN_TRACE_FAULT));
    TEST_ASSERT_EQUAL_STRING("inject_tx", posix_can_trace_kind_name(POSIX_CAN_TRACE_INJECT_TX));
    TEST_ASSERT_EQUAL_STRING("?", posix_can_trace_kind_name(POSIX_CAN_TRACE_KINDS));

    /* From a number on: the records before it are left out, the header stays. */
    TEST_ASSERT_TRUE(posix_can_trace_format(5U, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("oldest=0 newest=7 r5=inject_tx,5001,5002,5003,5004,5005,0,2,1,256,7,"
                             "5006 r6=inject_rx,6001,6002,6003,6004,6005,0,2,1,256,7,6006",
                             reply);
    TEST_ASSERT_TRUE(posix_can_trace_format(7U, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("oldest=0 newest=7", reply);
}

static void test_a_full_ring_keeps_the_newest_records(void)
{
    char reply[REPLY_BYTES];
    posix_can_trace_record_t out;
    unsigned long first = 0UL;
    unsigned long last = 0UL;

    for (uint32_t n = 0U; n < (POSIX_CAN_TRACE_RECORDS + 10U); n++) {
        const posix_can_trace_record_t in = record_of((uint8_t)POSIX_CAN_TRACE_COUNTERS, n);

        posix_can_trace_add(&in);
    }

    TEST_ASSERT_EQUAL_UINT32(POSIX_CAN_TRACE_RECORDS + 10U, posix_can_trace_newest());
    TEST_ASSERT_EQUAL_UINT32(10U, posix_can_trace_oldest());
    TEST_ASSERT_FALSE(posix_can_trace_get(9U, &out));
    TEST_ASSERT_TRUE(posix_can_trace_get(10U, &out));
    TEST_ASSERT_EQUAL_UINT64(11U, out.model_ns);
    TEST_ASSERT_TRUE(posix_can_trace_get(POSIX_CAN_TRACE_RECORDS + 9U, &out));
    TEST_ASSERT_EQUAL_UINT64(POSIX_CAN_TRACE_RECORDS + 10U, out.model_ns);

    /* Asked from before the oldest: the reply starts at the oldest, and says where that is. */
    TEST_ASSERT_TRUE(posix_can_trace_format(0U, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_INT(0, strncmp(reply, "oldest=10 newest=266 r10=counters,11,", 37U));
    TEST_ASSERT_TRUE(count_records(reply, &first, &last) > 0U);
    TEST_ASSERT_EQUAL_UINT32(10U, first);
}

static void test_a_reply_holds_whole_records_and_the_next_request_goes_on(void)
{
    char reply[POSIX_CAN_TRACE_REPLY_MIN + 100U];
    const posix_can_trace_record_t longest = longest_record();
    unsigned long first = 0UL;
    unsigned long last = 0UL;
    uint32_t from = 0U;
    uint32_t seen = 0U;

    for (uint32_t n = 0U; n < 20U; n++) {
        posix_can_trace_add(&longest);
    }

    /* The smallest buffer holds one record of the longest form; this one holds one, not two. */
    while (from < posix_can_trace_newest()) {
        TEST_ASSERT_TRUE(posix_can_trace_format(from, reply, sizeof(reply)));
        TEST_ASSERT_TRUE(strlen(reply) < sizeof(reply));
        TEST_ASSERT_EQUAL_UINT(1U, count_records(reply, &first, &last));
        TEST_ASSERT_EQUAL_UINT32(from, first);
        TEST_ASSERT_NOT_NULL(strstr(
            reply, ",18446744073709551615,4294967295,255,255,65535,65535,18446744073709551615"));
        from = (uint32_t)last + 1U;
        seen++;
    }

    TEST_ASSERT_EQUAL_UINT32(20U, seen);

    /* A buffer below the minimum is refused, with a message, and nothing overruns it. */
    TEST_ASSERT_FALSE(posix_can_trace_format(0U, reply, POSIX_CAN_TRACE_REPLY_MIN - 1U));
    TEST_ASSERT_EQUAL_STRING("reply buffer too small", reply);
}

static void test_the_cost_of_adding_a_record(void)
{
    enum { ADDS = 1000000 };
    const posix_can_trace_record_t record = record_of((uint8_t)POSIX_CAN_TRACE_COUNTERS, 42U);
    struct timespec before;
    struct timespec after;
    double ns;

    (void)clock_gettime(CLOCK_MONOTONIC, &before);

    for (int n = 0; n < ADDS; n++) {
        posix_can_trace_add(&record);
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &after);
    ns = ((double)(after.tv_sec - before.tv_sec) * 1e9) + (double)(after.tv_nsec - before.tv_nsec);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)ADDS, posix_can_trace_newest());
    /* Printed, not judged: a host that preempts the loop makes it longer, never wrong. */
    (void)printf("trace cost: %d records added in %.0f ns, %.2f ns per record\n", ADDS, ns,
                 ns / (double)ADDS);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_empty_trace_has_no_records);
    RUN_TEST(test_records_come_back_with_their_numbers_and_every_field);
    RUN_TEST(test_a_full_ring_keeps_the_newest_records);
    RUN_TEST(test_a_reply_holds_whole_records_and_the_next_request_goes_on);
    RUN_TEST(test_the_cost_of_adding_a_record);
    return UNITY_END();
}
