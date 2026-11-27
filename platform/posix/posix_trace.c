/* SPDX-License-Identifier: MIT */
/*
 * The host-only trace (posix_trace.h): a buffer of records allocated and written once at open,
 * filled by the hooks while the node runs, written to a file after the run.
 *
 * Part of fcn_host_io, which knows nothing about FreeRTOS: the kernel's hooks call in here
 * from posix_hooks.c, the hardware models from their own files. Every function may be called
 * when no trace is open, and then returns at once.
 */
#include "posix_trace.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How many records open() times to measure the cost of one, and how many times it repeats
 * that to keep the fastest round: the cost of a record on the host as it runs, not of the
 * moments in which the host took the thread away. */
#define COST_RECORDS 4096U
#define COST_ROUNDS  8U

/* The task-name length of the kernel (configMAX_TASK_NAME_LEN), with room to spare. */
#define NAME_BYTES 16U

/* The file's header: 64 bytes, every field little-endian on the host. */
typedef struct {
    char magic[8]; /* "FCNTRACE" */
    uint32_t version;
    uint32_t record_bytes;
    uint64_t records;
    uint64_t dropped;
    uint64_t capacity;
    uint64_t cost_ps;
    uint32_t cost_runs;
    uint32_t reserved;
    uint64_t reserved2;
} file_header_t;

_Static_assert(sizeof(file_header_t) == 64U, "the trace file's header is 64 bytes");

static posix_trace_event_t *s_buffer;
static uint64_t s_capacity;
static _Atomic bool s_recording;
static _Atomic uint64_t s_next; /* the next free slot; beyond the capacity: events dropped */
static uint64_t s_cost_ps;
static uint32_t s_cost_runs;

/* Kernel context only (vTaskSwitchContext, which never runs twice at once): the task the
 * kernel is switching out, between its two hooks. */
static uint32_t s_out_task;
static bool s_out_blocked;
static bool s_out_valid;

/* The task names, taken at each task's first switch-in (kernel context), read after the run. */
static char s_names[POSIX_TRACE_MAX_TASKS][NAME_BYTES];
static _Atomic uint64_t s_named; /* bit n: s_names[n] is set */

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/* The slot for one more record, or NULL when the buffer is full (the event is counted). */
static posix_trace_event_t *take_slot(void)
{
    const uint64_t slot = atomic_fetch_add_explicit(&s_next, 1U, memory_order_relaxed);

    return (slot < s_capacity) ? &s_buffer[slot] : NULL;
}

static bool recording(void)
{
    return atomic_load_explicit(&s_recording, memory_order_relaxed);
}

static void record(uint8_t kind, uint8_t task, uint64_t value)
{
    posix_trace_event_t *event = take_slot();

    if (event != NULL) {
        event->time_ns = monotonic_ns();
        event->value = value;
        event->arg = 0U;
        event->kind = kind;
        event->task = task;
        event->other = 0U;
        event->flags = 0U;
        (void)memset(event->data, 0, sizeof(event->data));
    }
}

static uint8_t task_byte(uint32_t task)
{
    return (task <= UINT8_MAX) ? (uint8_t)task : (uint8_t)UINT8_MAX;
}

bool posix_trace_open(uint32_t capacity, char *error, size_t error_len)
{
    if ((capacity == 0U) || (capacity > POSIX_TRACE_MAX_EVENTS)) {
        (void)snprintf(error, error_len, "--trace-events %u: 1 to %u", (unsigned int)capacity,
                       (unsigned int)POSIX_TRACE_MAX_EVENTS);
        return false;
    }

    s_buffer = malloc((size_t)capacity * sizeof(posix_trace_event_t));

    if (s_buffer == NULL) {
        (void)snprintf(error, error_len, "cannot allocate a trace of %u events",
                       (unsigned)capacity);
        return false;
    }

    /* Every page written once now, so that no page fault falls into the run. */
    (void)memset(s_buffer, 0, (size_t)capacity * sizeof(posix_trace_event_t));
    s_capacity = capacity;

    /* The cost of a record: the fastest of COST_ROUNDS rounds of COST_RECORDS records. */
    const uint32_t runs = (capacity < COST_RECORDS) ? capacity : COST_RECORDS;
    uint64_t best_ns = UINT64_MAX;

    for (uint32_t round = 0U; round < COST_ROUNDS; round++) {
        atomic_store(&s_next, 0U);
        const uint64_t start_ns = monotonic_ns();

        for (uint32_t i = 0U; i < runs; i++) {
            record(0U, 0U, i);
        }

        const uint64_t took_ns = monotonic_ns() - start_ns;

        if (took_ns < best_ns) {
            best_ns = took_ns;
        }
    }

    s_cost_ps = (best_ns * 1000U) / runs;
    s_cost_runs = runs * COST_ROUNDS;
    (void)memset(s_buffer, 0, (size_t)runs * sizeof(posix_trace_event_t));
    atomic_store(&s_next, 0U);
    atomic_store(&s_named, 0U);
    s_out_valid = false;
    atomic_store(&s_recording, true);
    return true;
}

void posix_trace_stop(void)
{
    atomic_store(&s_recording, false);
}

void posix_trace_get_info(posix_trace_info_t *out)
{
    const uint64_t next = atomic_load(&s_next);

    (void)memset(out, 0, sizeof(*out));
    out->open = (s_buffer != NULL);
    out->capacity = s_capacity;
    out->recorded = (next < s_capacity) ? next : s_capacity;
    out->dropped = (next > s_capacity) ? (next - s_capacity) : 0U;
    out->cost_ps = s_cost_ps;
    out->cost_runs = s_cost_runs;
}

bool posix_trace_write(const char *path, char *error, size_t error_len)
{
    posix_trace_info_t info;
    file_header_t header;
    FILE *file;
    bool ok;

    posix_trace_get_info(&info);
    (void)memset(&header, 0, sizeof(header));
    (void)memcpy(header.magic, "FCNTRACE", sizeof(header.magic));
    header.version = POSIX_TRACE_FILE_VERSION;
    header.record_bytes = (uint32_t)sizeof(posix_trace_event_t);
    header.records = info.recorded;
    header.dropped = info.dropped;
    header.capacity = info.capacity;
    header.cost_ps = info.cost_ps;
    header.cost_runs = info.cost_runs;

    file = fopen(path, "wb");

    if (file == NULL) {
        (void)snprintf(error, error_len, "cannot write --trace-file %s: %s", path, strerror(errno));
        return false;
    }

    ok = (fwrite(&header, sizeof(header), 1U, file) == 1U);

    if (ok && (info.recorded > 0U)) {
        ok = (fwrite(s_buffer, sizeof(posix_trace_event_t), (size_t)info.recorded, file) ==
              (size_t)info.recorded);
    }

    if (fclose(file) != 0) {
        ok = false;
    }

    if (!ok) {
        (void)snprintf(error, error_len, "cannot write --trace-file %s", path);
    }

    return ok;
}

void posix_trace_close(void)
{
    atomic_store(&s_recording, false);
    free(s_buffer);
    s_buffer = NULL;
    s_capacity = 0U;
}

const char *posix_trace_task_name(uint32_t task)
{
    const char *name = NULL;

    if ((task < POSIX_TRACE_MAX_TASKS) && ((atomic_load(&s_named) & (1ULL << task)) != 0U)) {
        name = s_names[task];
    }

    return name;
}

/* ---------------------------------------------------------------------- recording points */

void posix_trace_tx(const can_frame_t *frame, uint64_t eof_ns)
{
    if (recording()) {
        posix_trace_event_t *event = take_slot();

        if (event != NULL) {
            event->time_ns = monotonic_ns();
            event->value = eof_ns;
            event->arg = frame->id;
            event->kind = (uint8_t)POSIX_TRACE_TX;
            event->task = frame->dlc;
            event->other = 0U;
            event->flags = frame->flags;
            (void)memcpy(event->data, frame->data, sizeof(event->data));
        }
    }
}

void posix_trace_sensor(uint64_t sample)
{
    if (recording()) {
        record((uint8_t)POSIX_TRACE_SENSOR, 0U, sample);
    }
}

void posix_trace_tick_base(uint64_t base_ns, uint64_t first_tick, uint32_t period_ns)
{
    if (recording()) {
        posix_trace_event_t *event = take_slot();

        if (event != NULL) {
            /* The time of this record is the base itself: the moment tick `first_tick` is
             * due, minus (first_tick - 1) periods. */
            event->time_ns = base_ns;
            event->value = first_tick;
            event->arg = period_ns;
            event->kind = (uint8_t)POSIX_TRACE_TICK_BASE;
            event->task = 0U;
            event->other = 0U;
            event->flags = 0U;
            (void)memset(event->data, 0, sizeof(event->data));
        }
    }
}

void posix_trace_switched_out(uint32_t task, bool blocked)
{
    if (recording()) {
        s_out_task = task;
        s_out_blocked = blocked;
        s_out_valid = true;
    }
}

void posix_trace_switched_in(uint32_t task, const char *name, uint64_t tick)
{
    if (recording()) {
        const uint32_t out = s_out_valid ? s_out_task : 0U;
        const bool blocked = s_out_valid && s_out_blocked;

        s_out_valid = false;

        if ((task < POSIX_TRACE_MAX_TASKS) && ((atomic_load(&s_named) & (1ULL << task)) == 0U)) {
            /* Copied by hand: this may run in the tick's signal handler. */
            size_t i = 0U;

            while ((i < (NAME_BYTES - 1U)) && (name[i] != '\0')) {
                s_names[task][i] = name[i];
                i++;
            }

            s_names[task][i] = '\0';
            (void)atomic_fetch_or(&s_named, 1ULL << task);
        }

        /* vTaskSwitchContext() may keep the task that runs: that is no switch. */
        if (out != task) {
            posix_trace_event_t *event = take_slot();

            if (event != NULL) {
                event->time_ns = monotonic_ns();
                event->value = tick;
                event->arg = 0U;
                event->kind = (uint8_t)POSIX_TRACE_SWITCH;
                event->task = task_byte(task);
                event->other = task_byte(out);
                event->flags = blocked ? (uint8_t)POSIX_TRACE_FLAG_BLOCKED : 0U;
                (void)memset(event->data, 0, sizeof(event->data));
            }
        }
    }
}

void posix_trace_ready(uint32_t task, uint64_t tick)
{
    if (recording()) {
        record((uint8_t)POSIX_TRACE_READY, task_byte(task), tick);
    }
}

void posix_trace_delay_until(uint32_t task, uint64_t wake_tick)
{
    if (recording()) {
        record((uint8_t)POSIX_TRACE_DELAY_UNTIL, task_byte(task), wake_tick);
    }
}
