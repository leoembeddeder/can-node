/* SPDX-License-Identifier: MIT */
/* sigaction(), pthread_kill() and nanosleep() are POSIX, not C11. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "ring/spsc_ring.h"
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_init_rejects_invalid_arguments(void)
{
    spsc_ring_t ring;
    uint8_t storage[16];

    TEST_ASSERT_FALSE(spsc_ring_init(NULL, storage, 1U, 16U));
    TEST_ASSERT_FALSE(spsc_ring_init(&ring, NULL, 1U, 16U));
    TEST_ASSERT_FALSE(spsc_ring_init(&ring, storage, 0U, 16U));
    TEST_ASSERT_FALSE(spsc_ring_init(&ring, storage, 1U, 0U));
    TEST_ASSERT_FALSE(spsc_ring_init(&ring, storage, 1U, 1U));
    TEST_ASSERT_FALSE(spsc_ring_init(&ring, storage, 1U, 12U)); /* not a power of two */
    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, 1U, 16U));
    TEST_ASSERT_EQUAL_UINT32(16U, spsc_ring_capacity(&ring));
    TEST_ASSERT_EQUAL_UINT32(0U, spsc_ring_count(&ring));
}

static void test_elements_come_out_in_the_order_they_went_in(void)
{
    spsc_ring_t ring;
    uint32_t storage[8];
    uint32_t out = 0U;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(uint32_t), 8U));

    for (uint32_t i = 0U; i < 5U; i++) {
        const uint32_t value = 100U + i;

        TEST_ASSERT_TRUE(spsc_ring_push(&ring, &value));
    }

    TEST_ASSERT_EQUAL_UINT32(5U, spsc_ring_count(&ring));

    for (uint32_t i = 0U; i < 5U; i++) {
        TEST_ASSERT_TRUE(spsc_ring_pop(&ring, &out));
        TEST_ASSERT_EQUAL_UINT32(100U + i, out);
    }

    TEST_ASSERT_EQUAL_UINT32(0U, spsc_ring_count(&ring));
}

static void test_full_ring_rejects_push_and_keeps_its_contents(void)
{
    spsc_ring_t ring;
    uint8_t storage[4];
    uint8_t out = 0U;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, 1U, 4U));

    for (uint8_t i = 0U; i < 4U; i++) {
        TEST_ASSERT_TRUE(spsc_ring_push(&ring, &i));
    }

    const uint8_t extra = 0xEEU;

    TEST_ASSERT_FALSE(spsc_ring_push(&ring, &extra));
    TEST_ASSERT_EQUAL_UINT32(4U, spsc_ring_count(&ring));

    for (uint8_t i = 0U; i < 4U; i++) {
        TEST_ASSERT_TRUE(spsc_ring_pop(&ring, &out));
        TEST_ASSERT_EQUAL_UINT8(i, out);
    }
}

static void test_empty_ring_rejects_pop_and_leaves_the_output_alone(void)
{
    spsc_ring_t ring;
    uint16_t storage[2];
    uint16_t out = 0xBEEFU;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(uint16_t), 2U));
    TEST_ASSERT_FALSE(spsc_ring_pop(&ring, &out));
    TEST_ASSERT_EQUAL_HEX16(0xBEEFU, out);
}

static void test_slots_are_reused_across_many_wraps(void)
{
    spsc_ring_t ring;
    uint32_t storage[4];
    uint32_t out = 0U;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(uint32_t), 4U));

    /* Keep the ring partly filled so pushes and pops straddle the wrap point. */
    for (uint32_t i = 0U; i < 1000U; i++) {
        TEST_ASSERT_TRUE(spsc_ring_push(&ring, &i));

        if (i >= 2U) {
            TEST_ASSERT_TRUE(spsc_ring_pop(&ring, &out));
            TEST_ASSERT_EQUAL_UINT32(i - 2U, out);
        }
    }

    TEST_ASSERT_EQUAL_UINT32(2U, spsc_ring_count(&ring));
}

static void test_free_running_indices_survive_32_bit_overflow(void)
{
    spsc_ring_t ring;
    uint32_t storage[8];
    uint32_t out = 0U;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(uint32_t), 8U));

    /* Start three elements before the counters overflow. */
    atomic_store(&ring.head, UINT32_MAX - 2U);
    atomic_store(&ring.tail, UINT32_MAX - 2U);

    for (uint32_t i = 0U; i < 8U; i++) {
        TEST_ASSERT_TRUE(spsc_ring_push(&ring, &i));
    }

    const uint32_t extra = 99U;

    TEST_ASSERT_FALSE(spsc_ring_push(&ring, &extra));
    TEST_ASSERT_EQUAL_UINT32(8U, spsc_ring_count(&ring));

    for (uint32_t i = 0U; i < 8U; i++) {
        TEST_ASSERT_TRUE(spsc_ring_pop(&ring, &out));
        TEST_ASSERT_EQUAL_UINT32(i, out);
    }

    TEST_ASSERT_FALSE(spsc_ring_pop(&ring, &out));
}

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
} frame_like_t;

static void test_multi_byte_elements_are_copied_whole(void)
{
    spsc_ring_t ring;
    frame_like_t storage[4];
    frame_like_t in;
    frame_like_t out;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(frame_like_t), 4U));

    for (uint32_t n = 0U; n < 10U; n++) {
        (void)memset(&in, 0, sizeof(in));
        in.id = 0x100U + n;
        in.dlc = 8U;
        (void)memset(in.data, (int)(0x10U + n), sizeof(in.data));

        TEST_ASSERT_TRUE(spsc_ring_push(&ring, &in));
        TEST_ASSERT_TRUE(spsc_ring_pop(&ring, &out));
        TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof(in));
    }
}

/* ------------------------------------------------------------------ tests with threads
 *
 * The bugs these tests look for are races, so each one is caught only in runs where the threads
 * interleave in a particular way. What makes that likely is said with each test. Producers and
 * consumers yield the CPU whenever they cannot make progress instead of spinning: with fewer
 * free CPUs than threads, a spinning thread burns its whole time slice while the thread it waits
 * for is not running. (An earlier version of these tests spun: pinned to two CPUs, a run took
 * anything from 0.05 s to more than 30 s, and pinned to one, no run finished within 30 s.) */

#define STRESS_ITEMS    200000U
#define STRESS_CAPACITY 8U

static void *stress_producer(void *arg)
{
    spsc_ring_t *ring = arg;

    for (uint32_t value = 0U; value < STRESS_ITEMS;) {
        if (spsc_ring_push(ring, &value)) {
            value++;
        } else {
            (void)sched_yield();
        }
    }

    return NULL;
}

typedef struct {
    uint32_t received;       /* elements popped */
    uint32_t mismatches;     /* values that arrived out of order */
    uint32_t count_too_high; /* pops that failed although the consumer's count promised them */
} consumer_result_t;

/* The consumer of the first test, run on the test's own thread: STRESS_ITEMS elements, which
 * must be 0, 1, 2, ... It pops as many as its own count promises, and must be able to pop every
 * element it is told about. */
static consumer_result_t stress_consume(spsc_ring_t *ring)
{
    consumer_result_t result = {.received = 0U, .mismatches = 0U, .count_too_high = 0U};
    uint32_t out = 0U;

    while (result.received < STRESS_ITEMS) {
        uint32_t promised = spsc_ring_count(ring);

        if (promised == 0U) {
            (void)sched_yield();
        }

        while (promised > 0U) {
            if (!spsc_ring_pop(ring, &out)) {
                result.count_too_high++;
                break;
            }

            if (out != result.received) {
                result.mismatches++;
            }

            result.received++;
            promised--;
        }
    }

    return result;
}

/* One producer thread and one consumer thread, no lock: every value must arrive exactly once
 * and in order. A ring of 8 slots forces both the full and the empty path many times. There is
 * no third thread, so that on a host with two CPUs both run at the same time: an element
 * published before it is written, or a slot freed before it is read, shows up as a value out
 * of order only while the other side runs in parallel. */
static void test_two_threads_transfer_a_sequence_without_loss_or_reordering(void)
{
    spsc_ring_t ring;
    static uint32_t storage[STRESS_CAPACITY];
    pthread_t producer;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(uint32_t), STRESS_CAPACITY));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&producer, NULL, stress_producer, &ring));

    const consumer_result_t result = stress_consume(&ring);

    TEST_ASSERT_EQUAL_INT(0, pthread_join(producer, NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, result.mismatches);
    TEST_ASSERT_EQUAL_UINT32(0U, result.count_too_high);
    TEST_ASSERT_EQUAL_UINT32(0U, spsc_ring_count(&ring));
}

/* The next two tests call spsc_ring_count() from an observer, a third thread that owns neither
 * end of the ring. What they check depends on the two sides moving on between the observer's
 * load of tail and its load of head, a window of a few instructions that another CPU almost
 * never hits. The tests widen it: a fourth thread, the poker, sends the observer a signal every
 * few tens of microseconds, and when the signal arrives while the observer is inside
 * spsc_ring_count(), the handler sleeps. If that was between the two loads, the producer and
 * the consumer move on meanwhile.
 *
 * A signal lands in the window only if the observer is running when it arrives, so the
 * observers never yield. Both tests last as long as the poker, which stops after POKES_MAX
 * signals or POKES_MAX_NS, whichever comes first, so they take about the same time on any
 * host. How often the window is hit depends on the host: in every run when each of the four
 * threads has a CPU, in only some runs on one or two. A pass on a small host is weak evidence;
 * a failure anywhere is real, because neither test has a timing bound in its verdict. */
#define POKES_MAX     1200U
#define POKES_MAX_NS  200000000LL
#define POKE_EVERY_NS 20000L
#define POKE_PAUSE_NS 20000L
#define POKE_SIGNAL   SIGUSR1

/* Set and cleared by the observer thread only, around each call; it is 0 whenever no observer
 * runs, so no test has to reset it. */
static volatile sig_atomic_t s_observer_counting; /* the observer is inside spsc_ring_count() */

/* Runs on the observer's thread only, since the signal is sent to that thread. A signal that
 * does not arrive during spsc_ring_count() returns at once, so the observer sleeps only where
 * it helps. */
static void pause_observer(int signal_number)
{
    (void)signal_number;

    if (s_observer_counting != 0) {
        const int saved_errno = errno;
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = POKE_PAUSE_NS};

        /* nanosleep() is async-signal-safe in POSIX; errno is restored for the interrupted code. */
        (void)nanosleep(&pause, NULL);
        errno = saved_errno;
    }
}

/* spsc_ring_count() as the observers call it: marked, so that the handler knows. */
static uint32_t observer_count(const spsc_ring_t *ring)
{
    s_observer_counting = 1;
    const uint32_t count = spsc_ring_count(ring);
    s_observer_counting = 0;
    return count;
}

typedef struct {
    pthread_t observer; /* the thread to signal */
    _Atomic bool run;   /* cleared by the test to stop the poker early */
    _Atomic bool done;  /* set by the poker after its last signal */
} poker_t;

static int64_t monotonic_ns(void)
{
    struct timespec now;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return ((int64_t)now.tv_sec * 1000000000LL) + (int64_t)now.tv_nsec;
}

/* Sends up to POKES_MAX signals, fewer if that takes longer than POKES_MAX_NS (a host with
 * fewer CPUs than threads wakes the poker late) or if the test clears `run`. The time limit
 * decides how long the test runs and plays no part in its verdict. */
static void *poker_thread(void *arg)
{
    poker_t *poker = arg;
    const struct timespec interval = {.tv_sec = 0, .tv_nsec = POKE_EVERY_NS};
    const int64_t end_ns = monotonic_ns() + POKES_MAX_NS;

    for (uint32_t poke = 0U;
         atomic_load(&poker->run) && (poke < POKES_MAX) && (monotonic_ns() < end_ns); poke++) {
        (void)nanosleep(&interval, NULL);
        (void)pthread_kill(poker->observer, POKE_SIGNAL);
    }

    atomic_store(&poker->done, true);
    return NULL;
}

/* Install the handler and start the poker on an observer that is already running. */
static void poker_start(poker_t *poker, pthread_t observer, pthread_t *thread,
                        struct sigaction *previous)
{
    struct sigaction action;

    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = pause_observer;
    (void)sigemptyset(&action.sa_mask);
    TEST_ASSERT_EQUAL_INT(0, sigaction(POKE_SIGNAL, &action, previous));

    poker->observer = observer;
    atomic_init(&poker->run, true);
    atomic_init(&poker->done, false);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(thread, NULL, poker_thread, poker));
}

/* Stop the poker and wait for it. It must be gone before the observer ends: a thread that has
 * ended must not be signalled. */
static void poker_stop(poker_t *poker, pthread_t thread)
{
    atomic_store(&poker->run, false);
    TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
}

/* The producer of the two observer tests: pushes 0, 1, 2, ... until the poker has sent its last
 * signal, then says how many it pushed. It pushes at least one element, so that a producer that
 * starts late on a busy host still gives the test something to check. */
typedef struct {
    spsc_ring_t *ring;
    poker_t *poker;
    bool only_into_empty;    /* push only when the ring is empty */
    _Atomic bool done;       /* set after `pushed` has been stored */
    _Atomic uint32_t pushed; /* elements pushed in total */
} timed_producer_t;

static void timed_producer_init(timed_producer_t *producer, spsc_ring_t *ring, poker_t *poker,
                                bool only_into_empty)
{
    producer->ring = ring;
    producer->poker = poker;
    producer->only_into_empty = only_into_empty;
    atomic_init(&producer->done, false);
    atomic_init(&producer->pushed, 0U);
}

static void *timed_producer(void *arg)
{
    timed_producer_t *producer = arg;
    uint32_t value = 0U;

    while ((value == 0U) || !atomic_load(&producer->poker->done)) {
        /* The producer's own count is never below what is stored, so 0 means empty. */
        const bool may_push = !producer->only_into_empty || (spsc_ring_count(producer->ring) == 0U);

        if (may_push && spsc_ring_push(producer->ring, &value)) {
            value++;
        } else {
            (void)sched_yield();
        }
    }

    atomic_store(&producer->pushed, value);
    atomic_store(&producer->done, true);
    return NULL;
}

/* The consumer of the two observer tests, run on the test's own thread: everything the timed
 * producer pushed, in order. Unlike the first test's consumer it pops without asking for the
 * count first. The count's acquire load of head would order the element read after the
 * producer's write by itself, and ThreadSanitizer would not see a pop that lacks its own
 * acquire. It stops only when the producer is done and every element it pushed has arrived. */
static consumer_result_t consume_until_producer_done(timed_producer_t *producer)
{
    consumer_result_t result = {.received = 0U, .mismatches = 0U, .count_too_high = 0U};
    uint32_t out = 0U;

    for (;;) {
        if (spsc_ring_pop(producer->ring, &out)) {
            if (out != result.received) {
                result.mismatches++;
            }

            result.received++;
        } else if (atomic_load(&producer->done) &&
                   (result.received == atomic_load(&producer->pushed))) {
            break;
        } else {
            (void)sched_yield();
        }
    }

    return result;
}

typedef struct {
    spsc_ring_t *ring;
    _Atomic bool run;
    uint32_t calls;        /* written by the observer only */
    uint32_t out_of_range; /* written by the observer only */
} fill_observer_t;

static void *fill_observer(void *arg)
{
    fill_observer_t *observer = arg;
    const uint32_t capacity = spsc_ring_capacity(observer->ring);

    while (atomic_load(&observer->run)) {
        if (observer_count(observer->ring) > capacity) {
            observer->out_of_range++;
        }

        observer->calls++;
    }

    return NULL;
}

/* A transfer through a ring of 8 for as long as the poker runs, with an observer asking for the
 * fill level. The difference of a tail loaded before a pause and a head loaded after it is soon
 * above the capacity; the result must still not be. */
static void test_fill_level_seen_by_a_third_thread_stays_within_the_capacity(void)
{
    spsc_ring_t ring;
    static uint32_t storage[STRESS_CAPACITY];
    fill_observer_t observer = {.ring = &ring, .run = true, .calls = 0U, .out_of_range = 0U};
    timed_producer_t producer;
    poker_t poker;
    struct sigaction previous;
    pthread_t producing;
    pthread_t watcher;
    pthread_t poking;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(uint32_t), STRESS_CAPACITY));
    timed_producer_init(&producer, &ring, &poker, false);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&watcher, NULL, fill_observer, &observer));
    poker_start(&poker, watcher, &poking, &previous);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&producing, NULL, timed_producer, &producer));

    const consumer_result_t result = consume_until_producer_done(&producer);

    TEST_ASSERT_EQUAL_INT(0, pthread_join(producing, NULL));
    poker_stop(&poker, poking);
    atomic_store(&observer.run, false);
    TEST_ASSERT_EQUAL_INT(0, pthread_join(watcher, NULL));
    TEST_ASSERT_EQUAL_INT(0, sigaction(POKE_SIGNAL, &previous, NULL));

    TEST_ASSERT_EQUAL_UINT32(0U, result.mismatches);
    TEST_ASSERT_EQUAL_UINT32(0U, observer.out_of_range);
    TEST_ASSERT_TRUE(result.received > 0U);
    TEST_ASSERT_TRUE(observer.calls > 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, spsc_ring_count(&ring));
}

/* spsc_ring_count() loads tail before head. Both only grow and head is never behind tail, so a
 * head loaded after the tail is at least that tail and the difference does not wrap. The result
 * is then bounded by indices the observer can load itself: it is at most (head loaded after
 * the call) - (tail loaded before the call), the number stored when the call began plus what
 * was pushed during it. The bound holds whatever the other threads do and however long the
 * call takes. With the two loads the other way round, a consumer that catches up between them
 * makes the difference wrap, and the clamp turns that into "full": a ring that never holds more
 * than one element here reads as holding ORDER_CAPACITY.
 *
 * The producer pushes only into an empty ring, so two transfers during a pause are enough for
 * the reversed order to wrap. The wrapped value is clamped to the capacity, and that is above
 * the bound only if fewer elements than the capacity passed during the pause. Signals that
 * arrive while the handler sleeps chain several pauses: with a ring of 4096, a version of
 * spsc_ring_count() without the clamp read more than 4096 here, so more had passed between
 * its two loads. The capacity is therefore 65536.
 *
 * This test is about the order of the two loads in the program, which x86-64 keeps as written.
 * Whether they also need acquire ordering, so that a weakly ordered CPU keeps them in that
 * order, no test here can show: on x86-64 every load already has it, and ThreadSanitizer does
 * not see it either (see spsc_ring.h). */
#define ORDER_CAPACITY 65536U

typedef struct {
    spsc_ring_t *ring;
    _Atomic bool run;
    uint32_t calls;          /* written by the observer only */
    uint32_t above_bound;    /* written by the observer only */
    uint32_t above_capacity; /* written by the observer only */
} order_observer_t;

static void *order_observer(void *arg)
{
    order_observer_t *observer = arg;
    const uint32_t capacity = spsc_ring_capacity(observer->ring);

    while (atomic_load(&observer->run)) {
        const uint32_t tail_before =
            atomic_load_explicit(&observer->ring->tail, memory_order_acquire);
        const uint32_t count = observer_count(observer->ring);
        const uint32_t head_after =
            atomic_load_explicit(&observer->ring->head, memory_order_acquire);

        if (count > (head_after - tail_before)) {
            observer->above_bound++;
        }

        if (count > capacity) {
            observer->above_capacity++;
        }

        observer->calls++;
    }

    return NULL;
}

static void test_count_from_a_third_context_never_exceeds_what_passed_during_the_call(void)
{
    spsc_ring_t ring;
    static uint32_t storage[ORDER_CAPACITY];
    order_observer_t observer = {
        .ring = &ring, .run = true, .calls = 0U, .above_bound = 0U, .above_capacity = 0U};
    timed_producer_t producer;
    poker_t poker;
    struct sigaction previous;
    pthread_t producing;
    pthread_t watcher;
    pthread_t poking;

    TEST_ASSERT_TRUE(spsc_ring_init(&ring, storage, sizeof(uint32_t), ORDER_CAPACITY));
    timed_producer_init(&producer, &ring, &poker, true);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&watcher, NULL, order_observer, &observer));
    poker_start(&poker, watcher, &poking, &previous);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&producing, NULL, timed_producer, &producer));

    const consumer_result_t result = consume_until_producer_done(&producer);

    TEST_ASSERT_EQUAL_INT(0, pthread_join(producing, NULL));
    poker_stop(&poker, poking);
    atomic_store(&observer.run, false);
    TEST_ASSERT_EQUAL_INT(0, pthread_join(watcher, NULL));
    TEST_ASSERT_EQUAL_INT(0, sigaction(POKE_SIGNAL, &previous, NULL));

    TEST_ASSERT_EQUAL_UINT32(0U, result.mismatches);
    TEST_ASSERT_EQUAL_UINT32(0U, observer.above_bound);
    TEST_ASSERT_EQUAL_UINT32(0U, observer.above_capacity);
    TEST_ASSERT_TRUE(result.received > 0U);
    TEST_ASSERT_TRUE(observer.calls > 0U);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_invalid_arguments);
    RUN_TEST(test_elements_come_out_in_the_order_they_went_in);
    RUN_TEST(test_full_ring_rejects_push_and_keeps_its_contents);
    RUN_TEST(test_empty_ring_rejects_pop_and_leaves_the_output_alone);
    RUN_TEST(test_slots_are_reused_across_many_wraps);
    RUN_TEST(test_free_running_indices_survive_32_bit_overflow);
    RUN_TEST(test_multi_byte_elements_are_copied_whole);
    RUN_TEST(test_two_threads_transfer_a_sequence_without_loss_or_reordering);
    RUN_TEST(test_fill_level_seen_by_a_third_thread_stays_within_the_capacity);
    RUN_TEST(test_count_from_a_third_context_never_exceeds_what_passed_during_the_call);
    return UNITY_END();
}
