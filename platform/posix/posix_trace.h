/* SPDX-License-Identifier: MIT */
/**
 * @file
 * A host-only trace of the scheduler and of the simulated hardware, for the measurements of
 * bench/node_timing.py (docs/measurements.md). The node records it only when it is started
 * with --trace-file; without that option nothing is allocated and every hook below returns
 * after one relaxed atomic load.
 *
 * WHAT IS RECORDED. Each event is one record of posix_trace_event_t, its time taken with
 * CLOCK_MONOTONIC (the clock of plat_time_ns()) in the context that makes the event:
 *
 *   kind          where                                  task      other / flags      value                arg              data
 *   SWITCH        vTaskSwitchContext() (the kernel's     the task  the task switched  the tick count       -                -
 *                 traceTASK_SWITCHED_OUT and _IN)        switched  out (0: none) /
 *                                                        in        bit 0: it blocked
 *   READY         prvAddTaskToReadyList() (the kernel's  the task  -                  the tick count       -                -
 *                 traceMOVED_TASK_TO_READY_STATE)
 *   DELAY_UNTIL   xTaskDelayUntil() when it blocks       the task  -                  the tick to wake at  -                -
 *                 (traceTASK_DELAY_UNTIL)
 *   TX            the CAN hardware thread, when the      -         - / the frame's    the model's end of   the identifier   the data
 *                 model completed a frame of the node    (dlc)     flags              frame (ns)
 *                 and it was written to the transport;
 *                 the time is that of the write
 *   SENSOR        plat_sensor_read_current(): the        -         -                  the sample's number  -                -
 *                 sensor task's read of the current,     (0, 1, 2, ... in the order of the reads)
 *                 one per sample
 *   TICK_BASE     the tick thread (posix_tick.c): the    -         -                  the first tick       the tick         -
 *                 time at which the tick of count 1 is   count it applies to          period (ns)
 *                 due, at the start and after every
 *                 drop of ticks; tick k is due at base + (k - 1) * period
 *
 * Task numbers are the kernel's uxTCBNumber (1, 2, ... in the order of creation); the names
 * are taken at each task's first switch-in (posix_trace_task_name()).
 *
 * THE COST. Recording an event takes a clock_gettime(), an atomic increment and a store of 32
 * bytes into memory written once at open, so that no page fault falls into the run. The open
 * measures the mean cost of one record on the host as it is then (posix_trace_info_t.cost_ps)
 * and the node reports it with the statistics. The trace changes nothing the application
 * does: the hooks only read the kernel's state and write the buffer.
 *
 * CONCURRENCY. The kernel's hooks run in the context that switches tasks (a task's thread or
 * the tick's signal handler, never two at once); TX in the CAN hardware thread; TICK_BASE in
 * the tick thread. A record takes its slot with one atomic increment, so any of them may record
 * at any time. posix_trace_write() reads the buffer after every one of them has stopped: main()
 * calls it after the scheduler ended and the hardware thread was joined.
 */
#ifndef FCN_POSIX_TRACE_H
#define FCN_POSIX_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can/can_frame.h"

/** Events the buffer holds when --trace-events is not given: 64 MiB of records. */
#define POSIX_TRACE_DEFAULT_EVENTS 2097152U
/** The most --trace-events accepts: 256 MiB of records. */
#define POSIX_TRACE_MAX_EVENTS 8388608U

/** The version of the file's layout (posix_trace_write()). */
#define POSIX_TRACE_FILE_VERSION 1U

typedef enum {
    POSIX_TRACE_SWITCH = 1,
    POSIX_TRACE_READY = 2,
    POSIX_TRACE_DELAY_UNTIL = 3,
    POSIX_TRACE_TX = 4,
    POSIX_TRACE_SENSOR = 5,
    POSIX_TRACE_TICK_BASE = 6
} posix_trace_kind_t;

/** SWITCH: the task switched out had blocked (it was in no ready list), rather than been
 *  preempted or yielded. */
#define POSIX_TRACE_FLAG_BLOCKED 0x01U

/** One record of the trace: 32 bytes, little-endian on the host, in the file as in memory. */
typedef struct {
    uint64_t time_ns; /**< CLOCK_MONOTONIC */
    uint64_t value;   /**< see the table in the file's text */
    uint32_t arg;
    uint8_t kind;  /**< posix_trace_kind_t */
    uint8_t task;  /**< SWITCH, READY, DELAY_UNTIL: the task's number; TX: the DLC */
    uint8_t other; /**< SWITCH: the task switched out, 0 for none */
    uint8_t flags; /**< SWITCH: POSIX_TRACE_FLAG_BLOCKED; TX: the frame's CAN_FLAG_* */
    uint8_t data[8];
} posix_trace_event_t;

_Static_assert(sizeof(posix_trace_event_t) == 32U, "a trace record is 32 bytes");

typedef struct {
    bool open;          /**< posix_trace_open() succeeded: the run is traced */
    uint64_t capacity;  /**< records the buffer holds */
    uint64_t recorded;  /**< records written */
    uint64_t dropped;   /**< events not recorded because the buffer was full */
    uint64_t cost_ps;   /**< mean cost of one record, measured at open, in picoseconds */
    uint32_t cost_runs; /**< records timed for cost_ps */
} posix_trace_info_t;

/** Allocate a buffer of `capacity` records (1 to POSIX_TRACE_MAX_EVENTS), write every page of
 *  it once, measure the cost of a record and start recording. Main thread, before the
 *  scheduler starts. False, with a message in `error`, if it cannot. */
bool posix_trace_open(uint32_t capacity, char *error, size_t error_len);

/** Stop recording. Any thread. */
void posix_trace_stop(void);

/** Write the trace to `path`: a header of 64 bytes (the magic "FCNTRACE", the version, the
 *  record size, the number of records, the events dropped, the capacity, cost_ps and
 *  cost_runs; see posix_trace.c) and the records in the order of their slots. Main thread,
 *  after every recording context has stopped. False, with a message, if it cannot. */
bool posix_trace_write(const char *path, char *error, size_t error_len);

/** Free the buffer. */
void posix_trace_close(void);

void posix_trace_get_info(posix_trace_info_t *out);

/** The name of task number `task` as the trace saw it, or NULL if it never ran. */
const char *posix_trace_task_name(uint32_t task);

/** Tasks numbers have this bound in the trace; higher ones are recorded without a name. */
#define POSIX_TRACE_MAX_TASKS 64U

/* The recording points of the host's hardware models. */
void posix_trace_tx(const can_frame_t *frame, uint64_t eof_ns);
void posix_trace_sensor(uint64_t sample);
void posix_trace_tick_base(uint64_t base_ns, uint64_t first_tick, uint32_t period_ns);

/* The kernel's hooks, which FreeRTOSConfig.h's trace macros call. That file declares them too,
 * for the kernel's sources, and says so with FCN_TRACE_HOOKS_DECLARED; a file that includes
 * both sees one declaration. */
#ifndef FCN_TRACE_HOOKS_DECLARED
void posix_trace_switched_out(uint32_t task, bool blocked);
void posix_trace_switched_in(uint32_t task, const char *name, uint64_t tick);
void posix_trace_ready(uint32_t task, uint64_t tick);
void posix_trace_delay_until(uint32_t task, uint64_t wake_tick);
#endif

#endif /* FCN_POSIX_TRACE_H */
