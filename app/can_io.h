/* SPDX-License-Identifier: MIT */
/**
 * @file
 * CAN input and output of the application: the receive task, the transmit task and the
 * interrupt handler between them and the controller.
 *
 *                 can_io_publish()            +----------+   plat_can_tx()   +------------+
 *   producers ---------------------------->   | transmit |  --------------> | controller |
 *   (any task)    can_io_send_event()         |   task   |  <-------------- | (plat_can) |
 *                 -> transmit queue           +----------+   events, by     +------------+
 *                    (lib/can/can_txq)             ^         notification         |
 *                                                  |                             | interrupt
 *                                             +----------+                       v
 *   handlers  <-----------------------------  | receive  |  <--------------  can_io ISR
 *   (in the receive task's context)           |   task   |   plat_can_rx(), by notification
 *                                             +----------+
 *
 * Why two tasks. Reception is the most urgent thing the node does: a request has to be seen
 * and a flow-control frame answered within the transport protocol's timing, so the receive
 * task has the highest priority and does little per frame. Transmission is driven by what the
 * other tasks produce and by the controller's transmit-done events; it runs below the tasks
 * that produce the data, so a producer is never preempted by its own output.
 *
 * Why task notifications. Each task waits for "something for you" from an interrupt, with a
 * few distinct causes that may coincide. A notification value used as a set of bits does
 * exactly that without a kernel object per cause, and a bit set while the task is busy is not
 * lost: it is still there when the task next waits.
 */
#ifndef FCN_CAN_IO_H
#define FCN_CAN_IO_H

#include <stdbool.h>
#include <stdint.h>

#include "can/can_frame.h"
#include "can/can_txq.h"

/** Priorities of the two tasks (the application's priority table is in app_tasks.h). Phase 5
 *  moved both up by one, so that the diagnostic task fits between the transmit task and the
 *  health task (app_tasks.h). */
#define CAN_IO_RX_PRIORITY 7U
#define CAN_IO_TX_PRIORITY 4U

/** Most frames the receive task handles before it lets other work run for one tick. One batch
 *  per tick has to keep up with a full bus at PLAT_CAN_MAX_BITRATE (can_io.c checks this at
 *  compile time): 1 Mbit/s carries up to 21.3 of the shortest frames per millisecond, and
 *  the margin above that lets the task catch up after a late tick. */
#define CAN_IO_RX_BATCH 32U

/** Number of identifiers a handler can be registered for. */
#define CAN_IO_RX_HANDLERS 8U

/** Controller mailboxes the transmit task keeps loaded. */
#define CAN_IO_TX_MAILBOXES 3U

/**
 * Called in the receive task for every received frame whose identifier and format match.
 * `eof_ns` is when the frame ended on the bus. Keep it short: it runs at the highest task
 * priority.
 */
typedef void (*can_io_rx_handler_t)(const can_frame_t *frame, uint64_t eof_ns);

/**
 * Called in the receive task at the end of each period of the receive cycle (see
 * can_io_on_rx_cycle), after the frames that had arrived by then. Keep it short, like a frame
 * handler.
 */
typedef void (*can_io_cycle_handler_t)(void);

/**
 * Called by the receive task and by the transmit task, each with its own handler, once in every
 * pass of its loop: the task's alive indication for the watchdog manager (app.c). Every pass
 * begins with a wait of at most 50 ms of tick time, so a task that does not hang calls it at
 * least that often. Keep it short and non-blocking.
 */
typedef void (*can_io_alive_handler_t)(void);

/**
 * Called in the transmit task for every event frame with the identifier it was registered for
 * (can_io_on_tx_done) that the controller reports sent, in the order of the reports: the
 * transmit queue keeps event frames in a FIFO and one of them in a mailbox at a time
 * (can_txq.h), so that is the order they were queued in. `stamp_ns` is the time the frame was
 * queued with, as can_io_send_event_stamped() returned it. Event frames the queue discards
 * (expired, dropped) are never reported. Keep it short and non-blocking: it runs in the
 * transmit task's pass.
 */
typedef void (*can_io_tx_done_handler_t)(uint64_t stamp_ns);

typedef struct {
    uint32_t rx_frames;      /**< frames taken from the controller */
    uint32_t rx_unhandled;   /**< of those, frames no handler was registered for */
    uint32_t rx_batches_cut; /**< times the receive task stopped at CAN_IO_RX_BATCH */
    uint32_t tx_load_failed; /**< times the controller refused a frame the queue handed out */
    uint32_t rx_cycles;      /**< calls of the receive cycle handler */
    /** periods of the receive cycle that ended without a call of their own, because the task
     *  ran more than a period late (a host stall): one call stood for several periods */
    uint32_t rx_cycles_skipped;
    /** state events of the controller into error passive (the bus-off events are counted by
     *  the transmit queue, can_txq_stats_t.bus_off_count) */
    uint32_t error_passive_entries;
    /** reports handed to the transmit-done handler (can_io_on_tx_done) */
    uint32_t tx_done_reports;
    /** event frames can_io_send_event_stamped() queued behind another event frame, without
     *  waking the transmit task (see there) */
    uint32_t tx_quiet_enqueues;
} can_io_stats_t;

/** Create the two tasks and connect the interrupt. Call once, before the scheduler starts. */
void can_io_start(void);

/** Register `handler` for data frames with this identifier. Call before the scheduler
 *  starts. Returns false if the table is full. */
bool can_io_on_frame(uint32_t id, bool extended, can_io_rx_handler_t handler);

/**
 * Have `handler` called in the receive task at the end of every period of `period_ms` of tick
 * time, counted from the task's start: the receive cycle of a supervised cyclic message, whose
 * handler can then tell a period in which no frame arrived. The frames received up to the end
 * of a period are handled before the period's call. The receive task waits for frames no
 * longer than to the end of the current period, so a call is late only when the task is.
 *
 * A call is never repeated for periods the task missed: when it runs more than a period late,
 * one call stands for every period that ended since the previous call, and the others are
 * counted in rx_cycles_skipped. On a target the receive task, the most urgent one, does not
 * fall a period behind; on the host it does when the host stalls the node, and the frames that
 * were held up arrive at once afterwards.
 *
 * One cycle handler; `period_ms` below one tick counts as one tick. Call before the scheduler
 * starts.
 */
void can_io_on_rx_cycle(uint32_t period_ms, can_io_cycle_handler_t handler);

/** Have `rx_alive` called in every pass of the receive task's loop and `tx_alive` in every pass
 *  of the transmit task's (either may be NULL). Call before the scheduler starts. */
void can_io_on_alive(can_io_alive_handler_t rx_alive, can_io_alive_handler_t tx_alive);

/** Publish the current value of a cyclic message (see can_txq_publish). Any task. */
bool can_io_publish(uint8_t slot, const can_frame_t *frame, uint64_t stamp_ns);

/** Queue an event frame (see can_txq_enqueue). Any task. Returns false if it was refused. */
bool can_io_send_event(const can_frame_t *frame);

/**
 * Queue an event frame as can_io_send_event() does, and give the time it was queued with in
 * `*stamp_ns` (the clock of plat_time_ns()): the transmit-done handler gets it back
 * (can_io_on_tx_done), so that the caller can tell which of its frames a report is for. Returns
 * false if the frame was refused (`*stamp_ns` is then left alone).
 *
 * The transmit task is woken only if the queue holds no other event frame once this one is in
 * (can_txq_events_held() read in the critical section of the enqueue, after the frames that had
 * expired were discarded). Otherwise an event frame is ahead of this one,
 * in a mailbox or waiting for one, and the report of that frame or of the cyclic frames in the
 * mailboxes wakes the transmit task, which then loads this one: so a producer that sends a burst
 * of frames costs the transmit task one pass for the burst, not one per frame (task_table.c,
 * can_tx). Counted in tx_quiet_enqueues. Any task.
 */
bool can_io_send_event_stamped(const can_frame_t *frame, uint64_t *stamp_ns);

/**
 * Have `handler` called for every event frame with this identifier that the controller reports
 * sent (can_io_tx_done_handler_t). The transmit queue's report carries the identifier but not the
 * format, so the node must send no event frame of the other format with the same number (it
 * sends base-format frames only). One handler; a second call replaces it. Call before the
 * scheduler starts.
 */
void can_io_on_tx_done(uint32_t id, can_io_tx_done_handler_t handler);

/** Copies of the counters. Any task. */
void can_io_get_stats(can_io_stats_t *io, can_txq_stats_t *queue);

/** Copies of the counters of every cyclic slot of the transmit queue (can_txq_slot_stats).
 *  Any task. */
void can_io_get_slot_stats(can_txq_slot_stats_t slots[CAN_TXQ_CYCLIC_SLOTS]);

#endif /* FCN_CAN_IO_H */
