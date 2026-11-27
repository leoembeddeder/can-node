/* SPDX-License-Identifier: MIT */
/*
 * The CAN controller "hardware" of the host build.
 *
 * One thread outside the scheduler plays the peripheral. It owns the transport
 * (can_backend: SocketCAN or UDP) and runs the controller model (lib/can/can_ctrl) against
 * CLOCK_MONOTONIC: it sleeps until the model's next deadline or until a socket has input,
 * moves frames between the model and the transport, and raises the simulated CAN interrupt.
 *
 * The driver side (plat_can.h) is called by FreeRTOS tasks. The two sides meet only in
 * lock-free rings and atomics, because a thread outside the scheduler must not call the
 * kernel and a task must not block on a host lock:
 *
 *     driver task  --tx ring-->   hardware thread  --transport-->  bus
 *     driver task  <--rx ring--   hardware thread  <--transport--  bus
 *     driver task  <--event ring-- hardware thread     (TX done, TX aborted, state change)
 *     driver task  --command word + eventfd--> hardware thread   (abort, restart, wake up)
 *
 * Each ring has exactly one producer and one consumer. On the driver side that is why
 * plat_can_tx(), plat_can_rx() and plat_can_event() may each be called from one task only.
 *
 * What can be lost when the driver falls behind, and what cannot:
 *
 *  - Received frames can: the receive FIFO overruns, as on hardware, and the overrun is
 *    counted (rx_fifo_overruns).
 *  - The outcome of a transmit request (TX_DONE, TX_ABORTED) cannot. A driver that never
 *    heard what became of a frame would keep its mailbox for ever. plat_can_tx() therefore
 *    accepts a request only while fewer than PLAT_CAN_TX_MAILBOXES outcomes are still to be
 *    taken, and state events never use the last PLAT_CAN_TX_MAILBOXES entries of the event
 *    FIFO: an outcome always finds room. event_fifo_overruns counts the violations of this
 *    argument; it stays 0.
 *  - The latest error state cannot. A state change that finds the event FIFO filled up to
 *    that mark is not queued, but the state the controller is in is sent as soon as there is
 *    room (tell_state). A slow driver sees fewer changes, never a stale last one: a bus off
 *    it was never told about would never be recovered from.
 *
 * Abort. plat_can_abort_tx() affects the frames loaded before it, as a mailbox abort request
 * does on hardware, and none loaded after it, however soon. A request is on its way to a
 * mailbox for a while (in the tx ring), so the abort cannot simply empty the ring: every
 * request carries a submission serial, the abort records the serial it was made at, and the
 * hardware thread aborts the requests below that mark and loads the others (serve_driver).
 *
 * Timing. The model's times are exact; when the host carries them out is not. The thread
 * measures how late it is in three places and reports it (posix_can_get_stats, can.status),
 * without changing the model's times:
 *
 *  - pacing: a transmitted frame is written to the transport when the thread wakes up at the
 *    frame's modelled end of frame; the lag is the time from that end of frame to the write;
 *  - wake-up: a pass of the thread that serves a deadline it slept until is late by the time
 *    from the deadline to the start of the pass;
 *  - rx: a received frame reaches the receive FIFO late by the time from its modelled end of
 *    frame to the delivery.
 *
 * The pacing lag alone does not show a host stall during which a frame waited for the bus
 * (behind received frames, for example): the model moves that frame's start to the late
 * wake-up (late_resyncs in can_ctrl), its end of frame is then on time again, and the stall
 * becomes queueing time of the frame (start_ns - submit_ns). The wake-up lag shows it.
 *
 * Transport failures. The model decides what happens on the modelled bus; the transport only
 * carries the result to the other processes. A frame the transport refuses has still been
 * transmitted as far as the model and the driver are concerned (TX_DONE). The failure is
 * counted (the transport's tx_failed and rx_failed), logged (the first at once, then at most
 * once a second) and reported in can.status. A failure that means the interface is gone
 * (ENODEV or ENXIO: a deleted SocketCAN interface, from which the socket has been unbound)
 * also stops the node in an orderly way, since nothing will be sent or received again; it
 * then exits with POSIX_EXIT_CAN_TRANSPORT_LOST. An interface that is only down (ENETDOWN)
 * is a failure like any other: the socket stays bound and works again once the interface is
 * up, and so does the node. No bus fault is ever derived from a transport failure: the
 * bus faults belong to simulation control and the tests.
 *
 * Trace. The thread also records each step of a way into bus off and out of it again with the
 * model's time and the time of its own pass (posix_can_trace.h, read with can.trace), so that a
 * bench can tell the model's times from the host's. Nothing is recorded on a healthy bus.
 */
#include "posix_platform.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "can/can_ctrl.h"
#include "can_backend.h"
#include "plat_can.h"
#include "posix_can_trace.h"
#include "posix_sim.h"
#include "posix_trace.h"
#include "ring/spsc_ring.h"

#define CAN_IRQ_LINE 0U

#define TX_RING_ENTRIES    16U /* power of two, at least PLAT_CAN_TX_MAILBOXES */
#define RX_RING_ENTRIES    PLAT_CAN_RX_FIFO_DEPTH
#define EVENT_RING_ENTRIES PLAT_CAN_EVENT_FIFO_DEPTH

/* State events fill the event FIFO only up to here; the rest is kept for transmit outcomes. */
#define STATE_EVENT_LIMIT (EVENT_RING_ENTRIES - PLAT_CAN_TX_MAILBOXES)

/* Most frames taken from the transport in one pass of the hardware thread, so that a sender
 * flooding the transport cannot keep the thread reading: the model advances once per pass
 * whatever arrives. What is left stays in the socket for the next pass, which follows at once. */
#define TRANSPORT_READS_PER_PASS 256U

/* After the first transport failure is logged, the next report waits at least this long. */
#define TRANSPORT_REPORT_INTERVAL_NS 1000000000ULL

#define COMMAND_ABORT_TX 0x1U
#define COMMAND_RESTART  0x2U

/* The driver interface numbers the error states like the fault-confinement module and has as
 * many mailboxes as the model: the code below passes both through without translation. */
_Static_assert((int)PLAT_CAN_ERROR_ACTIVE == (int)CAN_FC_ERROR_ACTIVE, "state numbering");
_Static_assert((int)PLAT_CAN_ERROR_PASSIVE == (int)CAN_FC_ERROR_PASSIVE, "state numbering");
_Static_assert((int)PLAT_CAN_BUS_OFF == (int)CAN_FC_BUS_OFF, "state numbering");
_Static_assert(PLAT_CAN_TX_MAILBOXES == CAN_CTRL_TX_MAILBOXES, "mailbox count");
_Static_assert(TX_RING_ENTRIES >= CAN_CTRL_TX_MAILBOXES, "transmit ring too small");
_Static_assert(EVENT_RING_ENTRIES >= (2U * PLAT_CAN_TX_MAILBOXES), "event ring too small");

/* Longest sleep of the hardware thread: bounds how long stopping it takes. */
#define MAX_SLEEP_NS 50000000ULL

typedef struct {
    can_frame_t frame;
    uint32_t handle;
    uint32_t serial; /* submission order: s_tx_serial when plat_can_tx() took the request */
} tx_request_t;

/* A lateness figure: written by the hardware thread only, read by any thread. */
typedef struct {
    _Atomic uint64_t max_ns;
    _Atomic uint64_t sum_ns;
    _Atomic uint64_t samples;
} lag_stats_t;

static const char TAG[] = "can-hw";

static can_backend_t s_backend;
static char s_spec[64];      /* the transport specification, for log messages */
static can_ctrl_t s_ctrl;    /* touched by the hardware thread only */
static uint64_t s_pass_ns;   /* hardware thread only: the time of the current pass of its loop */
static uint64_t s_due_ns;    /* hardware thread only: the deadline its last sleep was for */
static uint8_t s_told_state; /* hardware thread only: the error state the driver was last sent */
static uint32_t s_bitrate;
/* The bus fault in force from the start (the node's --can-fault, posix_can_set_start_fault()):
 * set before posix_can_start(), which puts it into the model. */
static can_bus_fault_t s_start_fault = CAN_BUS_FAULT_NONE;

/* Hardware thread only: the transmit request taken from the ring and not yet loaded into a
 * mailbox, if any. The ring can only be popped, and the loops in serve_driver() have to see a
 * request before they decide what to do with it, so it waits here until they have. */
static tx_request_t s_next_request;
static bool s_next_request_held;

/* Hardware thread only: transport failures since the last log line, and when the next line
 * may be written. */
static uint64_t s_failures_unreported;
static uint64_t s_next_report_ns;

/* Hardware thread only: the error counters of the last trace record, whether the next
 * transmitted frame is the first after a recovery from bus off, and the frames transmitted
 * since the start (posix_can_trace.h). */
static uint16_t s_trace_tec;
static uint16_t s_trace_rec;
static bool s_trace_after_recovery;
static uint64_t s_trace_frames;

static uint8_t s_tx_storage[TX_RING_ENTRIES * sizeof(tx_request_t)];
static uint8_t s_rx_storage[RX_RING_ENTRIES * sizeof(plat_can_rx_t)];
static uint8_t s_event_storage[EVENT_RING_ENTRIES * sizeof(plat_can_event_t)];
static spsc_ring_t s_tx_ring;
static spsc_ring_t s_rx_ring;
static spsc_ring_t s_event_ring;

static int s_wake_fd = -1;
static pthread_t s_thread;
static bool s_thread_started;
static _Atomic bool s_thread_run;
static _Atomic uint32_t s_commands;
static _Atomic uint32_t s_irq_causes;
static plat_can_isr_t s_isr;
static void *s_isr_context;

/* Transmit requests the driver made and has not yet taken the outcome of: the mailboxes in
 * use as the driver has to see them. Counted up by plat_can_tx(), down by plat_can_event(). */
static _Atomic uint32_t s_tx_outstanding;

/* The serial the next transmit request gets, and the serial at which the last abort was
 * made: a request with a lower serial was made before that abort. */
static _Atomic uint32_t s_tx_serial;
static _Atomic uint32_t s_abort_mark;

/* Published by the hardware thread for readers in other threads. */
static _Atomic uint16_t s_tec;
static _Atomic uint16_t s_rec;
static _Atomic uint8_t s_state;
static _Atomic uint32_t s_rx_fifo_overruns;
static _Atomic uint32_t s_event_fifo_overruns;
static _Atomic uint32_t s_state_events_merged;
static _Atomic int s_transport_error;
static lag_stats_t s_pacing;
static lag_stats_t s_wakeup;
static lag_stats_t s_rx_lag;
static uint64_t s_started_ns;
static uint64_t s_stopped_ns;

/* ------------------------------------------------------------------ hardware thread side */

static void lag_reset(lag_stats_t *stats)
{
    atomic_store(&stats->max_ns, 0U);
    atomic_store(&stats->sum_ns, 0U);
    atomic_store(&stats->samples, 0U);
}

static void lag_record(lag_stats_t *stats, uint64_t lag_ns)
{
    /* One writer: a load and a store are enough for the maximum. */
    if (lag_ns > atomic_load(&stats->max_ns)) {
        atomic_store(&stats->max_ns, lag_ns);
    }

    (void)atomic_fetch_add(&stats->sum_ns, lag_ns);
    (void)atomic_fetch_add(&stats->samples, 1U);
}

static uint64_t lag_mean(const lag_stats_t *stats)
{
    const uint64_t samples = atomic_load(&stats->samples);

    return (samples > 0U) ? (atomic_load(&stats->sum_ns) / samples) : 0U;
}

/* How long ago `model_ns` was, now; 0 if it is still ahead. */
static uint64_t lateness(uint64_t model_ns)
{
    const uint64_t now_ns = posix_monotonic_ns();

    return (now_ns > model_ns) ? (now_ns - model_ns) : 0U;
}

/* True if serial `a` was taken before serial `b`. The difference is read as a signed number
 * so that the comparison holds across the wrap of the counter: requests that are in flight
 * at the same time are never more than a few serials apart. */
static bool serial_before(uint32_t a, uint32_t b)
{
    return ((a - b) & 0x80000000U) != 0U;
}

/* ENODEV: the interface was unregistered under the socket, which the kernel then unbinds;
 * ENXIO: a send on such an unbound socket. Not ENETDOWN: a bound socket survives its
 * interface going down and up. */
static bool transport_is_gone(int error)
{
    return (error == ENODEV) || (error == ENXIO);
}

/* A send() or recv() of the transport failed; s_backend.last_error says why. (strerror() of
 * the GNU C library returns a constant string for every errno it knows, so this thread may
 * call it.) */
static void transport_failed(const char *operation, uint64_t now_ns)
{
    const int error = s_backend.last_error;

    s_failures_unreported++;

    if (now_ns >= s_next_report_ns) {
        LOG_WRN(TAG, "transport %s: %s failed: %s (failures since the last report: %llu)", s_spec,
                operation, strerror(error), (unsigned long long)s_failures_unreported);
        s_failures_unreported = 0U;
        s_next_report_ns = now_ns + TRANSPORT_REPORT_INTERVAL_NS;
    }

    if (transport_is_gone(error) && (atomic_load(&s_transport_error) == 0)) {
        atomic_store(&s_transport_error, error);
        LOG_ERR(TAG, "transport %s is gone (%s): stopping the node", s_spec, strerror(error));
        /* The same orderly stop as a SIGTERM from outside: the health task ends the
         * scheduler and main() writes the statistics. Sent to the process, not raised: this
         * thread blocks every signal, so one sent to it alone would never be delivered. */
        (void)kill(getpid(), SIGTERM);
    }
}

static void raise_irq(uint32_t causes)
{
    (void)atomic_fetch_or(&s_irq_causes, causes);
    posix_irq_raise(CAN_IRQ_LINE);
}

/* Queue an event and interrupt the driver. Returns false if the event FIFO was full. */
static bool push_event(const plat_can_event_t *event)
{
    const bool pushed = spsc_ring_push(&s_event_ring, event);

    if (pushed) {
        raise_irq(PLAT_CAN_IRQ_EVENT);
    }

    return pushed;
}

/* Queue the outcome of a transmit request (TX_DONE or TX_ABORTED). There is always room for
 * it, by the argument at the top of this file; the counter is there to show that the argument
 * holds, and the tests check that it stays 0. */
static void push_outcome(const plat_can_event_t *event)
{
    if (!push_event(event)) {
        (void)atomic_fetch_add(&s_event_fifo_overruns, 1U);
    }
}

/* Tell the driver that a frame left its mailbox without being transmitted. */
static void report_aborted(uint32_t handle, uint64_t time_ns)
{
    plat_can_event_t event;

    (void)memset(&event, 0, sizeof(event));
    event.kind = PLAT_CAN_EVENT_TX_ABORTED;
    event.handle = handle;
    event.eof_ns = time_ns;
    push_outcome(&event);
}

/* Send the driver a state event unless `state` is what it was last sent. Returns true if the
 * driver has been sent `state` when the function returns, false if the event FIFO had no room
 * for a state event. The old state in the event is the one the driver was last sent, so the
 * events form an unbroken chain even when changes in between were never queued. */
static bool tell_state(uint8_t state, uint16_t tec, uint16_t rec, uint64_t time_ns)
{
    bool told = (state == s_told_state);

    if (!told && (spsc_ring_count(&s_event_ring) < STATE_EVENT_LIMIT)) {
        plat_can_event_t event;

        (void)memset(&event, 0, sizeof(event));
        event.kind = PLAT_CAN_EVENT_STATE;
        event.eof_ns = time_ns;
        event.state = state;
        event.old_state = s_told_state;
        event.tec = tec;
        event.rec = rec;
        told = push_event(&event);

        if (told) {
            s_told_state = state;
        }
    }

    return told;
}

static void publish_status(void)
{
    atomic_store(&s_state, (uint8_t)can_ctrl_state(&s_ctrl));
    atomic_store(&s_tec, can_ctrl_tec(&s_ctrl));
    atomic_store(&s_rec, can_ctrl_rec(&s_ctrl));
}

/* Add a trace record of a step made in this pass at `model_ns` (posix_can_trace.h), with the
 * model's state and counters as they are now. */
static void trace_step(posix_can_trace_kind_t kind, uint64_t model_ns, uint32_t value)
{
    posix_can_trace_record_t record;

    (void)memset(&record, 0, sizeof(record));
    record.kind = (uint8_t)kind;
    record.model_ns = model_ns;
    record.pass_ns = s_pass_ns;
    record.attempts_failed = can_ctrl_stats(&s_ctrl)->tx_attempts_failed;
    record.frames = s_trace_frames;
    record.value = value;
    record.state = (uint8_t)can_ctrl_state(&s_ctrl);
    record.old_state = record.state;
    record.tec = can_ctrl_tec(&s_ctrl);
    record.rec = can_ctrl_rec(&s_ctrl);
    posix_can_trace_add(&record);
    s_trace_tec = record.tec;
    s_trace_rec = record.rec;
}

/* Add a trace record of a model action carried out in this pass: a state change, or the first
 * frame transmitted after a recovery, with the time and the counters the action carries. */
static void trace_action(posix_can_trace_kind_t kind, const can_ctrl_action_t *action)
{
    posix_can_trace_record_t record;

    (void)memset(&record, 0, sizeof(record));
    record.kind = (uint8_t)kind;
    record.model_ns = action->time_ns;
    record.pass_ns = s_pass_ns;
    record.attempts_failed = can_ctrl_stats(&s_ctrl)->tx_attempts_failed;
    record.frames = s_trace_frames;
    record.state = (uint8_t)action->state;
    record.old_state = (uint8_t)action->old_state;
    record.tec = action->tec;
    record.rec = action->rec;

    if (kind == POSIX_CAN_TRACE_FIRST_TX) {
        record.start_ns = action->start_ns;
        record.submit_ns = action->submit_ns;
        record.value = action->frame.id;
    }

    posix_can_trace_add(&record);
    s_trace_tec = record.tec;
    s_trace_rec = record.rec;
}

/* Take every action the model has due and carry it out. */
static void run_model(uint64_t now_ns)
{
    can_ctrl_action_t action;

    while (can_ctrl_poll(&s_ctrl, now_ns, &action)) {
        plat_can_event_t event;

        (void)memset(&event, 0, sizeof(event));

        switch (action.kind) {
        case CAN_CTRL_ACTION_TX_COMPLETE:
            /* The frame has reached its end of frame on the modelled bus: now, and only
             * now, the other participants get it. If the transport refuses it, the frame
             * has still been transmitted on the modelled bus (see the top of this file). */
            if (!s_backend.send(&s_backend, &action.frame)) {
                transport_failed("send", now_ns);
            }

            /* The host-only trace (posix_trace.h; nothing unless the node runs with
             * --trace-file): the frame, its modelled end of frame and the time of the write. */
            posix_trace_tx(&action.frame, action.time_ns);
            lag_record(&s_pacing, lateness(action.time_ns));
            s_trace_frames++;

            if (s_trace_after_recovery) {
                trace_action(POSIX_CAN_TRACE_FIRST_TX, &action);
                s_trace_after_recovery = false;
            }

            event.kind = PLAT_CAN_EVENT_TX_DONE;
            event.handle = action.handle;
            event.submit_ns = action.submit_ns;
            event.start_ns = action.start_ns;
            event.eof_ns = action.time_ns;
            push_outcome(&event);
            break;

        case CAN_CTRL_ACTION_RX_DELIVER: {
            plat_can_rx_t received;

            received.frame = action.frame;
            received.eof_ns = action.time_ns;
            lag_record(&s_rx_lag, lateness(action.time_ns));

            if (spsc_ring_push(&s_rx_ring, &received)) {
                raise_irq(PLAT_CAN_IRQ_RX);
            } else {
                /* The driver did not drain its FIFO: a receive overrun, as on hardware. */
                (void)atomic_fetch_add(&s_rx_fifo_overruns, 1U);
            }
            break;
        }

        case CAN_CTRL_ACTION_STATE_CHANGED:
            trace_action(POSIX_CAN_TRACE_STATE, &action);

            if (action.old_state == CAN_FC_BUS_OFF) {
                s_trace_after_recovery = true;
            }

            /* With the counters and the time of the change itself, if the event can be
             * queued now. If not, the end of this function catches up. */
            if (!tell_state((uint8_t)action.state, action.tec, action.rec, action.time_ns)) {
                (void)atomic_fetch_add(&s_state_events_merged, 1U);
            }
            break;

        case CAN_CTRL_ACTION_TX_ABORTED:
            /* The frame that was on the wire when the driver aborted, removed now that its
             * attempt has failed. */
            report_aborted(action.handle, action.time_ns);
            break;

        case CAN_CTRL_ACTION_NONE:
        default:
            break;
        }
    }

    /* A state change that found no room earlier, in this pass or in one before: send the
     * state the controller is in now. This runs at least once per MAX_SLEEP_NS. */
    (void)tell_state((uint8_t)can_ctrl_state(&s_ctrl), can_ctrl_tec(&s_ctrl), can_ctrl_rec(&s_ctrl),
                     now_ns);
    publish_status();

    /* The counters' way between the state changes, as far as the passes resolve it. */
    if ((can_ctrl_tec(&s_ctrl) != s_trace_tec) || (can_ctrl_rec(&s_ctrl) != s_trace_rec)) {
        trace_step(POSIX_CAN_TRACE_COUNTERS, now_ns, 0U);
    }
}

/* The oldest transmit request not yet loaded, or NULL if there is none. It stays the oldest
 * until take_request() is called. */
static const tx_request_t *peek_request(void)
{
    if (!s_next_request_held) {
        s_next_request_held = spsc_ring_pop(&s_tx_ring, &s_next_request);
    }

    return s_next_request_held ? &s_next_request : NULL;
}

/* Done with the request peek_request() returned. */
static void take_request(void)
{
    s_next_request_held = false;
}

/* The commands the driver has left since the last call.
 *
 * The wake-up is reset first and the commands are looked at afterwards. A request made after
 * this read writes the eventfd again, so the poll at the end of the pass returns at once:
 * nothing can arrive unseen between looking and sleeping. Taking the commands first would
 * leave such a gap: a command set after the exchange and before the read would have its
 * wake-up consumed here and wait for the thread's next pass. The value read is not needed. */
static uint32_t take_commands(void)
{
    uint64_t wake_count;

    (void)!read(s_wake_fd, &wake_count, sizeof(wake_count));
    return atomic_exchange(&s_commands, 0U);
}

static void serve_driver(uint32_t commands, uint64_t now_ns)
{
    if ((commands & COMMAND_ABORT_TX) != 0U) {
        /* Read after the command: plat_can_abort_tx() stores the mark before it sets the
         * command, so this is the mark of the abort being served or of a later one. A later
         * one has set the command again, and nothing is loaded until it is served (below). */
        const uint32_t mark = atomic_load(&s_abort_mark);
        uint32_t handles[CAN_CTRL_TX_MAILBOXES];
        const uint32_t removed = can_ctrl_tx_abort_all(&s_ctrl, handles, CAN_CTRL_TX_MAILBOXES);
        const tx_request_t *request;

        /* Every frame in the mailboxes was loaded before the abort (see the loading loop). */
        for (uint32_t i = 0U; i < removed; i++) {
            report_aborted(handles[i], now_ns);
        }

        /* A request still on its way to a mailbox that was made before the abort is aborted
         * with the rest. Without this it would be loaded after the abort, sit in its mailbox
         * through a bus-off and go out stale after recovery. A request made after the abort
         * stays: the ring is in submission order, so the first such request ends the drain,
         * and the loading loop takes it and the ones behind it. */
        for (request = peek_request(); (request != NULL) && serial_before(request->serial, mark);
             request = peek_request()) {
            report_aborted(request->handle, now_ns);
            take_request();
        }
    }

    if ((commands & COMMAND_RESTART) != 0U) {
        /* The count of rule 12 starts now (the model's request time is this pass's). */
        can_ctrl_request_restart(&s_ctrl, now_ns);
        trace_step(POSIX_CAN_TRACE_RESTART, now_ns, 0U);
    }

    /* Move transmit requests into the controller's mailboxes. The driver never queues more
     * than there are mailboxes, so the loop cannot leave a request stranded for long.
     *
     * Nothing is loaded while an abort is pending: the next pass serves it first. So when an
     * abort is served, the mailboxes hold only frames that were made before it. The request
     * is taken from the ring first and the commands are looked at afterwards, in that order.
     * plat_can_abort_tx() sets its command before the driver pushes any later request, so a
     * request taken here that was made after an abort finds that abort's command set, unless
     * the abort has been served already (at the top of this pass or earlier). It is then held
     * until the abort is served, and the drain keeps it, since its serial is not before the
     * mark. Looking at the commands before taking the request would leave a gap: an abort
     * and a new request could both come between the look and the take, and the new frame
     * would be loaded here and then taken back by the abort. */
    while (can_ctrl_tx_pending(&s_ctrl) < CAN_CTRL_TX_MAILBOXES) {
        const tx_request_t *request = peek_request();

        if ((request == NULL) || ((atomic_load(&s_commands) & COMMAND_ABORT_TX) != 0U)) {
            break;
        }

        if (!can_ctrl_tx_submit(&s_ctrl, &request->frame, request->handle, now_ns)) {
            /* An invalid frame: give the mailbox back to the driver. */
            report_aborted(request->handle, now_ns);
        }

        take_request();
    }
}

static void read_transport(uint64_t now_ns)
{
    can_frame_t frame;
    uint32_t reads = 0U;
    int result = 0;

    while (reads < TRANSPORT_READS_PER_PASS) {
        result = s_backend.recv(&s_backend, &frame);

        if (result != 1) {
            break;
        }

        (void)can_ctrl_wire_rx(&s_ctrl, &frame, now_ns);
        reads++;
    }

    if (result < 0) {
        transport_failed("receive", now_ns);
    }
}

static void *hardware_main(void *arg)
{
    (void)arg;
    posix_external_thread_setup("fcn-can");
    (void)prctl(PR_SET_TIMERSLACK, 1UL); /* wake-ups as close to the deadline as the host gives */

    while (atomic_load(&s_thread_run)) {
        struct pollfd fds[3];
        nfds_t count = 2U;

        /* The driver's commands first, then the time: a command is applied at a time no
         * earlier than the moment it was made. (Taken the other way round, a restart made just
         * after the time was read would start its recovery before it was requested.) */
        const uint32_t commands = take_commands();

        /* One time for everything done in this pass, simulation control commands included:
         * the model requires that the times it is given never go backwards. */
        s_pass_ns = posix_monotonic_ns();

        /* A pass that starts at or after the deadline the thread slept until serves that
         * deadline, whatever ended the sleep. One that starts before it was woken early, by
         * input or by the MAX_SLEEP_NS limit, and is not late for anything. */
        if ((s_due_ns != CAN_CTRL_NO_WAKEUP) && (s_pass_ns >= s_due_ns)) {
            lag_record(&s_wakeup, s_pass_ns - s_due_ns);
        }

        posix_sim_service();
        serve_driver(commands, s_pass_ns);
        read_transport(s_pass_ns);
        run_model(s_pass_ns);

        const uint64_t wake_ns = can_ctrl_next_wakeup(&s_ctrl);
        const uint64_t now_ns = posix_monotonic_ns();
        uint64_t sleep_ns = MAX_SLEEP_NS;

        s_due_ns = wake_ns;

        if (wake_ns != CAN_CTRL_NO_WAKEUP) {
            sleep_ns = (wake_ns > now_ns) ? (wake_ns - now_ns) : 0U;

            if (sleep_ns > MAX_SLEEP_NS) {
                sleep_ns = MAX_SLEEP_NS;
            }
        }

        const struct timespec timeout = {(time_t)(sleep_ns / 1000000000ULL),
                                         (long)(sleep_ns % 1000000000ULL)};

        fds[0].fd = s_backend.fd;
        fds[0].events = POLLIN;
        fds[1].fd = s_wake_fd;
        fds[1].events = POLLIN;

        if (posix_sim_enabled()) {
            fds[2].fd = posix_sim_fd();
            fds[2].events = POLLIN;
            count = 3U;
        }

        (void)ppoll(fds, count, &timeout, NULL);
    }

    return NULL;
}

/* -------------------------------------------------------------------- simulation control */

static const char *state_name(can_fc_state_t state)
{
    static const char *const names[] = {"active", "passive", "busoff"};

    return ((unsigned int)state < 3U) ? names[state] : "?";
}

static const char *fault_name(can_bus_fault_t fault)
{
    static const char *const names[] = {"none", "short", "dominant", "noack"};

    return ((unsigned int)fault < 4U) ? names[fault] : "?";
}

/* The bus fault named `name` in can.fault and in the node's --can-fault, into `out`; false for
 * a name that is none of them. */
static bool fault_from_name(const char *name, can_bus_fault_t *out)
{
    static const struct {
        const char *name;
        can_bus_fault_t fault;
    } table[] = {{"none", CAN_BUS_FAULT_NONE},
                 {"short", CAN_BUS_FAULT_SHORT},
                 {"dominant", CAN_BUS_FAULT_STUCK_DOMINANT},
                 {"noack", CAN_BUS_FAULT_NO_ACK}};
    bool found = false;

    for (size_t i = 0U; i < (sizeof(table) / sizeof(table[0])); i++) {
        if (strcmp(name, table[i].name) == 0) {
            *out = table[i].fault;
            found = true;
        }
    }

    return found;
}

bool posix_can_set_start_fault(const char *name)
{
    can_bus_fault_t fault = CAN_BUS_FAULT_NONE;
    const bool known = (name != NULL) && fault_from_name(name, &fault);

    if (known) {
        s_start_fault = fault;
    }

    return known;
}

/* "can.trace [<from>]": the trace records from number `from` on (default 0) that fit into the
 * reply (posix_can_trace_format()). */
static bool sim_can_trace(const char *args, char *reply, size_t reply_len)
{
    unsigned long from = 0UL;
    char *end = NULL;

    if (args[0] != '\0') {
        if ((args[0] < '0') || (args[0] > '9')) {
            (void)snprintf(reply, reply_len, "usage: can.trace [<from>]");
            return false;
        }

        from = strtoul(args, &end, 10);

        if ((*end != '\0') || (from > (unsigned long)UINT32_MAX)) {
            (void)snprintf(reply, reply_len, "usage: can.trace [<from>]");
            return false;
        }
    }

    return posix_can_trace_format((uint32_t)from, reply, reply_len);
}

/* Commands under "can.": status | fault <none|short|dominant|noack> | inject <tx|rx> <count> |
 * trace [<from>]. Runs in the hardware thread, within a pass of its loop. */
static bool sim_can_handler(const char *verb, const char *args, char *reply, size_t reply_len)
{
    const uint64_t now_ns = s_pass_ns;

    if (strcmp(verb, "status") == 0) {
        const can_ctrl_stats_t *stats = can_ctrl_stats(&s_ctrl);
        posix_tick_stats_t tick;
        int length;

        can_backend_refresh_counters(&s_backend);
        /* The ticks the tick code has given up so far (posix_tick.c), in the same reply as the
         * receive overruns, so that a test can count both from the same moment of the run. */
        posix_tick_get_stats(&tick);

        /* New keys go at the end, so that a reader of the old ones is not disturbed. */
        length = snprintf(
            reply, reply_len,
            "state=%s tec=%u rec=%u fault=%s tx=%llu tx_failed=%llu tx_aborted=%llu rx=%llu "
            "rx_errors=%llu rx_lost_fault=%llu wire_overruns=%llu bus_off=%llu late_resyncs=%llu "
            "busy_ns=%llu elapsed_ns=%llu rx_fifo_overruns=%u transport_tx=%llu transport_rx=%llu "
            "transport_malformed=%llu transport_overflow=%llu pacing_lag_max_ns=%llu "
            "pacing_lag_mean_ns=%llu transport_tx_failed=%llu transport_rx_failed=%llu "
            "wakeup_lag_max_ns=%llu wakeup_lag_mean_ns=%llu rx_lag_max_ns=%llu "
            "rx_lag_mean_ns=%llu ticks_dropped=%llu",
            state_name(can_ctrl_state(&s_ctrl)), (unsigned int)can_ctrl_tec(&s_ctrl),
            (unsigned int)can_ctrl_rec(&s_ctrl), fault_name(can_ctrl_fault(&s_ctrl)),
            (unsigned long long)stats->tx_frames, (unsigned long long)stats->tx_attempts_failed,
            (unsigned long long)stats->tx_aborted, (unsigned long long)stats->rx_frames,
            (unsigned long long)stats->rx_errors, (unsigned long long)stats->rx_lost_fault,
            (unsigned long long)stats->wire_overruns, (unsigned long long)stats->bus_off_events,
            (unsigned long long)stats->late_resyncs, (unsigned long long)stats->busy_ns,
            (unsigned long long)(now_ns - s_started_ns),
            (unsigned int)atomic_load(&s_rx_fifo_overruns), (unsigned long long)s_backend.tx_frames,
            (unsigned long long)s_backend.rx_frames, (unsigned long long)s_backend.rx_malformed,
            (unsigned long long)s_backend.rx_overflow,
            (unsigned long long)atomic_load(&s_pacing.max_ns),
            (unsigned long long)lag_mean(&s_pacing), (unsigned long long)s_backend.tx_failed,
            (unsigned long long)s_backend.rx_failed,
            (unsigned long long)atomic_load(&s_wakeup.max_ns),
            (unsigned long long)lag_mean(&s_wakeup),
            (unsigned long long)atomic_load(&s_rx_lag.max_ns),
            (unsigned long long)lag_mean(&s_rx_lag), (unsigned long long)tick.dropped_ticks);

        /* A reply cut short would lose the keys at its end without anyone noticing. */
        if ((length < 0) || ((size_t)length >= reply_len)) {
            (void)snprintf(reply, reply_len, "the status does not fit into a reply");
            return false;
        }

        return true;
    }

    if (strcmp(verb, "fault") == 0) {
        can_bus_fault_t fault = CAN_BUS_FAULT_NONE;

        if (fault_from_name(args, &fault)) {
            can_ctrl_set_fault(&s_ctrl, fault, now_ns);
            trace_step(POSIX_CAN_TRACE_FAULT, now_ns, (uint32_t)fault);
            (void)snprintf(reply, reply_len, "fault=%s t_ns=%llu", fault_name(fault),
                           (unsigned long long)now_ns);
            return true;
        }

        (void)snprintf(reply, reply_len, "unknown fault '%s' (none, short, dominant, noack)", args);
        return false;
    }

    if (strcmp(verb, "inject") == 0) {
        char what[8] = {0};
        unsigned int count = 0U;

        /* NOLINTNEXTLINE(cert-err34-c): a count that does not convert fails the match */
        if ((sscanf(args, "%7s %u", what, &count) == 2) && (count > 0U)) {
            if (strcmp(what, "tx") == 0) {
                can_ctrl_inject_tx_errors(&s_ctrl, count);
                trace_step(POSIX_CAN_TRACE_INJECT_TX, now_ns, (uint32_t)count);
                (void)snprintf(reply, reply_len, "tx_errors=%u", count);
                return true;
            }

            if (strcmp(what, "rx") == 0) {
                can_ctrl_inject_rx_errors(&s_ctrl, count);
                trace_step(POSIX_CAN_TRACE_INJECT_RX, now_ns, (uint32_t)count);
                (void)snprintf(reply, reply_len, "rx_errors=%u", count);
                return true;
            }
        }

        (void)snprintf(reply, reply_len, "usage: can.inject <tx|rx> <count>");
        return false;
    }

    if (strcmp(verb, "trace") == 0) {
        return sim_can_trace(args, reply, reply_len);
    }

    (void)snprintf(reply, reply_len, "unknown command 'can.%s'", verb);
    return false;
}

/* ---------------------------------------------------------------------------- driver side */

/* The simulated CAN interrupt: runs on the task thread, in the port's interrupt context. */
static void can_irq(void *context, BaseType_t *woken)
{
    const uint32_t causes = atomic_exchange(&s_irq_causes, 0U);

    (void)context;

    if ((causes != 0U) && (s_isr != NULL)) {
        s_isr(s_isr_context, causes, woken);
    }
}

void plat_can_set_isr(plat_can_isr_t isr, void *context)
{
    s_isr = isr;
    s_isr_context = context;
}

/* Wake the hardware thread out of its poll. A write to an eventfd neither blocks nor takes a
 * lock in the C library, so a task may do it; it is repeated if a signal (the tick, a
 * simulated interrupt) cut it short, because a wake-up that is lost would leave a frame
 * waiting until the thread's next periodic pass. */
static void wake_hardware(void)
{
    const uint64_t one = 1U;
    ssize_t written;

    do {
        written = write(s_wake_fd, &one, sizeof(one));
    } while ((written < 0) && (errno == EINTR));
}

bool plat_can_tx(const can_frame_t *frame, uint32_t handle)
{
    bool accepted = false;

    /* This function is the only one that counts up and has a single caller, so the check and
     * the count need not be one atomic step. The count goes up before the request can be
     * seen by the hardware thread: its outcome may be taken, and counted down, by another
     * task before this function has returned. */
    if (atomic_load(&s_tx_outstanding) < PLAT_CAN_TX_MAILBOXES) {
        tx_request_t request;

        request.frame = *frame;
        request.handle = handle;
        request.serial = atomic_fetch_add(&s_tx_serial, 1U);
        (void)atomic_fetch_add(&s_tx_outstanding, 1U);

        if (spsc_ring_push(&s_tx_ring, &request)) {
            wake_hardware();
            accepted = true;
        } else {
            /* Cannot happen: the ring holds more requests than may be outstanding. */
            (void)atomic_fetch_sub(&s_tx_outstanding, 1U);
        }
    }

    return accepted;
}

bool plat_can_rx(plat_can_rx_t *out)
{
    return spsc_ring_pop(&s_rx_ring, out);
}

bool plat_can_event(plat_can_event_t *out)
{
    const bool taken = spsc_ring_pop(&s_event_ring, out);

    if (taken && (out->kind != PLAT_CAN_EVENT_STATE)) {
        /* The driver now knows what became of the frame: its mailbox is free again. */
        (void)atomic_fetch_sub(&s_tx_outstanding, 1U);
    }

    return taken;
}

static void send_command(uint32_t command)
{
    (void)atomic_fetch_or(&s_commands, command);
    wake_hardware();
}

void plat_can_abort_tx(void)
{
    /* The mark first, then the command: a hardware thread that sees the command sees this
     * mark or a later one. Requests with a serial below the mark were made before this call.
     * (The driver calls this and plat_can_tx() from one task, which orders them. For calls
     * from two tasks at the same time, which came first is not defined anyway.) */
    atomic_store(&s_abort_mark, atomic_load(&s_tx_serial));
    send_command(COMMAND_ABORT_TX);
}

void plat_can_request_restart(void)
{
    send_command(COMMAND_RESTART);
}

void plat_can_get_status(plat_can_status_t *out)
{
    out->state = atomic_load(&s_state);
    out->tec = atomic_load(&s_tec);
    out->rec = atomic_load(&s_rec);
    out->rx_fifo_overruns = atomic_load(&s_rx_fifo_overruns);
}

/* ------------------------------------------------------------------------------ lifecycle */

bool posix_can_start(const char *backend_spec, uint32_t bitrate, char *error, size_t error_len)
{
    const can_ctrl_config_t config = {.bitrate = bitrate};

    if (!can_backend_open(&s_backend, backend_spec, error, error_len)) {
        return false;
    }

    s_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    if (s_wake_fd < 0) {
        (void)snprintf(error, error_len, "can: eventfd: %s", strerror(errno));
        s_backend.close(&s_backend);
        return false;
    }

    (void)spsc_ring_init(&s_tx_ring, s_tx_storage, (uint32_t)sizeof(tx_request_t), TX_RING_ENTRIES);
    (void)spsc_ring_init(&s_rx_ring, s_rx_storage, (uint32_t)sizeof(plat_can_rx_t),
                         RX_RING_ENTRIES);
    (void)spsc_ring_init(&s_event_ring, s_event_storage, (uint32_t)sizeof(plat_can_event_t),
                         EVENT_RING_ENTRIES);

    (void)snprintf(s_spec, sizeof(s_spec), "%s", backend_spec);
    s_bitrate = bitrate;
    s_started_ns = posix_monotonic_ns();
    s_pass_ns = s_started_ns;
    s_due_ns = CAN_CTRL_NO_WAKEUP;
    can_ctrl_init(&s_ctrl, &config, s_started_ns);
    posix_can_trace_reset();
    s_trace_tec = 0U;
    s_trace_rec = 0U;
    s_trace_after_recovery = false;
    s_trace_frames = 0U;

    if (s_start_fault != CAN_BUS_FAULT_NONE) {
        can_ctrl_set_fault(&s_ctrl, s_start_fault, s_started_ns);
        trace_step(POSIX_CAN_TRACE_FAULT, s_started_ns, (uint32_t)s_start_fault);
    }

    s_told_state = (uint8_t)can_ctrl_state(&s_ctrl);
    s_stopped_ns = 0U;
    s_next_request_held = false;
    s_failures_unreported = 0U;
    s_next_report_ns = 0U;
    atomic_store(&s_tx_outstanding, 0U);
    atomic_store(&s_tx_serial, 0U);
    atomic_store(&s_abort_mark, 0U);
    atomic_store(&s_commands, 0U);
    atomic_store(&s_irq_causes, 0U);
    atomic_store(&s_rx_fifo_overruns, 0U);
    atomic_store(&s_event_fifo_overruns, 0U);
    atomic_store(&s_state_events_merged, 0U);
    atomic_store(&s_transport_error, 0);
    lag_reset(&s_pacing);
    lag_reset(&s_wakeup);
    lag_reset(&s_rx_lag);
    publish_status();
    posix_irq_attach(CAN_IRQ_LINE, can_irq, NULL);
    posix_sim_register("can.", sim_can_handler);

    atomic_store(&s_thread_run, true);

    if (pthread_create(&s_thread, NULL, hardware_main, NULL) != 0) {
        (void)snprintf(error, error_len, "can: cannot start the hardware thread");
        s_backend.close(&s_backend);
        (void)close(s_wake_fd);
        s_wake_fd = -1;
        return false;
    }

    s_thread_started = true;
    return true;
}

void posix_can_stop(void)
{
    if (s_thread_started) {
        atomic_store(&s_thread_run, false);
        wake_hardware();
        (void)pthread_join(s_thread, NULL);
        s_thread_started = false;
        s_stopped_ns = posix_monotonic_ns();
        s_backend.close(&s_backend);
        (void)close(s_wake_fd);
        s_wake_fd = -1;
    }
}

int posix_can_transport_error(void)
{
    return atomic_load(&s_transport_error);
}

void posix_can_get_report(posix_can_report_t *out)
{
    const can_ctrl_stats_t *stats = can_ctrl_stats(&s_ctrl);

    (void)memset(out, 0, sizeof(*out));
    out->transport = (s_backend.kind != NULL) ? s_backend.kind : "none";
    out->bitrate = s_bitrate;
    out->elapsed_ns = (s_stopped_ns > s_started_ns) ? (s_stopped_ns - s_started_ns) : 0U;
    out->state = (uint8_t)can_ctrl_state(&s_ctrl);
    out->tec = can_ctrl_tec(&s_ctrl);
    out->rec = can_ctrl_rec(&s_ctrl);
    out->tx_frames = stats->tx_frames;
    out->tx_attempts_failed = stats->tx_attempts_failed;
    out->tx_aborted = stats->tx_aborted;
    out->rx_frames = stats->rx_frames;
    out->rx_errors = stats->rx_errors;
    out->rx_lost_fault = stats->rx_lost_fault;
    out->wire_overruns = stats->wire_overruns;
    out->bus_off_events = stats->bus_off_events;
    out->late_resyncs = stats->late_resyncs;
    out->busy_ns = stats->busy_ns;
    out->transport_tx_frames = s_backend.tx_frames;
    out->transport_tx_failed = s_backend.tx_failed;
    out->transport_rx_frames = s_backend.rx_frames;
    out->transport_rx_failed = s_backend.rx_failed;
    out->transport_rx_malformed = s_backend.rx_malformed;
    out->transport_rx_overflow = s_backend.rx_overflow;
    out->transport_error = atomic_load(&s_transport_error);
}

void posix_can_get_stats(posix_can_stats_t *out)
{
    (void)memset(out, 0, sizeof(*out));
    out->frames_paced = atomic_load(&s_pacing.samples);
    out->pacing_lag_max_ns = atomic_load(&s_pacing.max_ns);
    out->pacing_lag_mean_ns = lag_mean(&s_pacing);
    out->wakeups = atomic_load(&s_wakeup.samples);
    out->wakeup_lag_max_ns = atomic_load(&s_wakeup.max_ns);
    out->wakeup_lag_mean_ns = lag_mean(&s_wakeup);
    out->rx_delivered = atomic_load(&s_rx_lag.samples);
    out->rx_lag_max_ns = atomic_load(&s_rx_lag.max_ns);
    out->rx_lag_mean_ns = lag_mean(&s_rx_lag);
    out->rx_fifo_overruns = atomic_load(&s_rx_fifo_overruns);
    out->rx_fifo_pending = spsc_ring_count(&s_rx_ring);
    out->event_fifo_overruns = atomic_load(&s_event_fifo_overruns);
    out->state_events_merged = atomic_load(&s_state_events_merged);
}
