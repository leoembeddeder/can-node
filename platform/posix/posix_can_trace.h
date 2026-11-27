/* SPDX-License-Identifier: MIT */
/**
 * @file
 * A host-only trace of the simulated CAN controller's way into bus off and out of it again,
 * read through simulation control ("can.trace", docs/can-simulation.md). It exists for the
 * bus-off bench (bench/bus_off.py), which has to tell apart what the controller model and the
 * standard fix from what the host adds.
 *
 * WHY. The model's times are exact and the host's are not (posix_can.c, Timing). A recovery
 * from bus off takes 128 occurrences of 11 recessive bits from the moment the model is asked
 * for it: a figure in bit times, fixed by ISO 11898-1. When the model is asked, and when the
 * hardware thread carries out what the model decided, is the host's business. can.status and
 * the statistics say what the state and the counters are, not when they changed; this trace
 * records both times of every step: the model's and that of the thread's pass.
 *
 * WHAT IS RECORDED. posix_can.c adds a record, in the hardware thread only, for:
 *
 *   state      a change of the error state (the model's STATE_CHANGED action)
 *   counters   a pass of the hardware thread that ended with error counters other than those
 *              of the last record: the transmit error counter's way up, step by step as far
 *              as the passes of the thread resolve it
 *   restart    the driver's request to recover from bus off, served
 *   first_tx   the first frame transmitted after a recovery from bus off
 *   fault      a bus fault set or removed through simulation control (can.fault)
 *   inject_tx, inject_rx  errors injected through simulation control (can.inject)
 *
 * On a healthy bus nothing is recorded: the error counters stay at 0 and the state never
 * changes. The application never sees the trace and nothing in the node depends on it.
 *
 * COST. Adding a record copies one posix_can_trace_record_t (64 bytes) into a static ring and
 * counts; it reads no clock (a record carries the time the pass already took), makes no system
 * call and takes no lock. tests/platform/test_posix_can_trace.c measures it and prints the
 * figure. The only cost on a healthy bus is the comparison of the two counters at the end of
 * each pass.
 *
 * NUMBERING. Records are numbered from 0 since posix_can_trace_reset() (posix_can_start()); the
 * ring keeps the last POSIX_CAN_TRACE_RECORDS of them. A 32-bit number does not wrap in any run
 * of the node: at most a few dozen records come with each bus off.
 *
 * THREADS. None: one thread adds and reads (the hardware thread; can.trace is served in it).
 */
#ifndef FCN_POSIX_CAN_TRACE_H
#define FCN_POSIX_CAN_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Records the ring keeps: a power of two, more than a run of bus offs under a persisting
 *  short leaves between two reads of the bus-off bench. */
#define POSIX_CAN_TRACE_RECORDS 256U

typedef enum {
    POSIX_CAN_TRACE_STATE = 0,
    POSIX_CAN_TRACE_COUNTERS,
    POSIX_CAN_TRACE_RESTART,
    POSIX_CAN_TRACE_FIRST_TX,
    POSIX_CAN_TRACE_FAULT,
    POSIX_CAN_TRACE_INJECT_TX,
    POSIX_CAN_TRACE_INJECT_RX,
    POSIX_CAN_TRACE_KINDS
} posix_can_trace_kind_t;

/**
 * One record. Every time is CLOCK_MONOTONIC in nanoseconds, the clock the model runs on.
 *
 *  - model_ns: state, the model's time of the change; first_tx, the frame's end of frame; every
 *    other kind, pass_ns (a restart request starts the count of rule 12 then, a fault or an
 *    injection is applied to the model then, and counters are those of the model advanced to
 *    then).
 *  - pass_ns: the start of the hardware thread's pass that recorded it: the time it carried the
 *    step out. The model was advanced to that time in the pass.
 *  - start_ns, submit_ns: first_tx only, the frame's start of frame and when it entered its
 *    mailbox; 0 otherwise.
 *  - attempts_failed: the model's count of destroyed transmission attempts at the end of the
 *    model's advance in that pass (for a state change, possibly with later attempts of the same
 *    pass in it; after bus off there are none).
 *  - frames: the node's frames the thread has carried out as transmitted since the start (each
 *    written to the transport at its end of frame), at the record; first_tx, this frame
 *    included: its number among the node's frames, from 1, so that an observer that heard every
 *    frame from the start knows which of them it is without comparing clocks.
 *  - value: first_tx, the frame's identifier; fault, the can_bus_fault_t set; inject_*, the
 *    count; 0 otherwise.
 *  - state, old_state: the error state after and before (equal except for a state change), in
 *    the numbering of can_fc_state_t; tec, rec: the error counters after the step.
 */
typedef struct {
    uint64_t model_ns;
    uint64_t pass_ns;
    uint64_t start_ns;
    uint64_t submit_ns;
    uint64_t attempts_failed;
    uint64_t frames;
    uint32_t value;
    uint16_t tec;
    uint16_t rec;
    uint8_t kind; /* posix_can_trace_kind_t */
    uint8_t state;
    uint8_t old_state;
} posix_can_trace_record_t;

/** Forget every record; the next one is number 0. */
void posix_can_trace_reset(void);

/** Add a copy of `record` as the next number. */
void posix_can_trace_add(const posix_can_trace_record_t *record);

/** The number the next record will get: the count of records added since the reset. */
uint32_t posix_can_trace_newest(void);

/** The number of the oldest record the ring still keeps (equal to posix_can_trace_newest()
 *  while it keeps none). */
uint32_t posix_can_trace_oldest(void);

/** Record number `seq` into `out`; false, changing nothing, if it is not kept (overwritten, or
 *  not added yet). */
bool posix_can_trace_get(uint32_t seq, posix_can_trace_record_t *out);

/** The name of a kind in the text of can.trace: "state", "counters", ...; "?" for none. */
const char *posix_can_trace_kind_name(uint8_t kind);

/**
 * The reply of "can.trace [<from>]": "oldest=<o> newest=<n>" and then, from record max(from, o)
 * on, as many whole records as fit into `reply_len`, each one word
 *
 *     r<seq>=<kind>,<model_ns>,<pass_ns>,<start_ns>,<submit_ns>,<attempts_failed>,<value>,
 *            <state>,<old_state>,<tec>,<rec>,<frames>
 *
 * (without the line break). A client asks again from the number after the last record it got
 * until it has every record up to newest; records below oldest are lost. Returns false only if
 * `reply_len` cannot hold the header and one record of the longest form
 * (POSIX_CAN_TRACE_REPLY_MIN); the reply then holds a message.
 */
bool posix_can_trace_format(uint32_t from, char *reply, size_t reply_len);

/** The shortest reply buffer posix_can_trace_format() fills: the header and one record with
 *  every number at its longest. */
#define POSIX_CAN_TRACE_REPLY_MIN 240U

#endif /* FCN_POSIX_CAN_TRACE_H */
