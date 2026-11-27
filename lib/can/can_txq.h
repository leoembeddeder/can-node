/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The software side of transmission: which frame is loaded into a free controller mailbox
 * next, what happens to frames that cannot go out yet, and what to do when the controller
 * goes bus off.
 *
 * A CAN controller has a handful of transmit mailboxes. Everything else waits in software,
 * and how it waits decides what the bus sees after a busy spell or a bus-off: this module is
 * that waiting room. It is pure logic with time passed in, so the policy can be tested
 * without a controller; the transmit task feeds it events and carries out what it asks for.
 *
 * TWO KINDS OF FRAME
 *
 *  - Cyclic frames (can_txq_publish): periodic telemetry. Only the newest value of a message
 *    is worth sending, so each message has one slot and publishing again replaces a value
 *    that has not gone out yet. The replaced value is counted as superseded. A busy bus or a
 *    bus-off therefore never builds a backlog of stale telemetry: when the bus is back, the
 *    current value of every message goes out once.
 *  - Event frames (can_txq_enqueue): frames that must not be replaced or reordered, such as
 *    responses and diagnostic traffic. They wait in a FIFO of CAN_TXQ_EVENT_DEPTH frames;
 *    when it is full a new frame is refused and counted as dropped. An event frame that has
 *    waited for the configured timeout without being sent is discarded instead of being sent
 *    late, counted as expired (a transport-protocol frame is useless after its timeout): at
 *    once if it is still in the FIFO, after the controller has given it back if it is in a
 *    mailbox (see STALE FRAMES IN A MAILBOX). The timeout runs from the enqueue and is over at
 *    exactly that time plus the timeout.
 *
 * WHICH FRAME NEXT. can_txq_next() hands out the waiting frame with the smallest arbitration
 * key among: every cyclic slot with a value waiting, and the oldest event frame. Of frames
 * with equal keys the one in the lower slot goes first and the event frame last. A cyclic
 * message occupies at most one mailbox at a time, so two values of one message cannot
 * overtake each other: a value published while an earlier one is in a mailbox waits in the
 * slot until that one is done or aborted. For the same reason only one event frame is in a
 * mailbox at a time: a controller sends its mailboxes in priority order, not in the order
 * they were loaded, and two event frames in two mailboxes could swap places on the bus. The
 * queue never occupies more mailboxes than it was configured for: fewer mailboxes mean
 * fresher data and better priority order on the bus, more mean less idle bus between frames
 * when the transmit-done interrupt is slow.
 *
 * STALE FRAMES IN A MAILBOX. A frame that has been handed out stays in its mailbox until the
 * controller reports it sent or aborted, and the controller aborts nothing it is not asked to.
 * Bus off (below) is not the only fault that keeps frames there, and the others never lead to
 * it: on a bus where nobody acknowledges, the node goes error passive and the controller
 * retries the same frame for ever; on a bus held dominant it starts no attempt at all. When
 * such a fault ends, the bus would first see what was in the mailboxes when it began. So the
 * queue also asks for the mailboxes to be aborted (CAN_TXQ_REQUEST_ABORT_MAILBOXES, returned
 * by can_txq_poll() and counted in recall_requests) when a frame in a mailbox has gone stale
 * by the rules above:
 *
 *  - A cyclic message is published while its earlier value is still in a mailbox. That value
 *    has missed a whole period, and its successor replaces it, as it would in the slot. The
 *    next poll asks once; a later publish in the same situation asks again. If the earlier
 *    value is confirmed before the poll, nothing stale is left and the request is withdrawn.
 *  - An event frame in a mailbox reaches its timeout. The poll asks once for that frame, at
 *    exactly the moment it is over, and can_txq_next_deadline() names that moment.
 *
 * The abort is then carried out as for bus off, frame by frame through the abort reports: the
 * earlier value finds its successor waiting and is counted as superseded; the event frame that
 * reached its timeout goes back to the head of the FIFO and is discarded there as expired the
 * next time the FIFO is looked at (can_txq_next, can_txq_enqueue, can_txq_poll); every other
 * frame comes back unchanged and is handed out again in the order described above. A frame
 * whose attempt is on the wire when the abort arrives cannot be taken back: if the attempt
 * succeeds, it is reported sent and counts as sent (lib/can/can_ctrl.h, ABORTING). On a
 * healthy bus a frame is confirmed long before its message is published again, so none of
 * this happens there. While the controller is bus off none of it applies: the abort was
 * requested on the way in.
 *
 * BUS OFF. When the controller reports bus off (can_txq_on_state), the queue asks the caller
 * to abort the mailboxes, because frames queued in hardware would otherwise go out stale
 * after recovery. An aborted cyclic value goes back to its slot unless a newer one is already
 * waiting there (then it is superseded); an aborted event frame goes back to the head of the
 * FIFO, which keeps a place for it even if it has filled up in the meantime. Frames keep
 * being accepted while the node is off the bus, under the rules above: that is the transmit
 * buffering during bus-off. Recovery is requested automatically after a hold-off that starts
 * at the configured initial value and doubles with every further bus-off up to the configured
 * maximum; once the node has stayed out of bus-off for the configured stable time, the
 * hold-off returns to its initial value. The doubling is there because a fault that is still
 * present takes the node straight back to bus off, and an eager node would spend its time
 * hammering a broken bus.
 *
 * ACCOUNTING. At every moment
 *
 *     produced = sent + superseded + dropped + expired + pending
 *
 * where pending counts the frames waiting in slots, in the FIFO and in mailboxes. The same
 * holds for every cyclic slot on its own (can_txq_slot_stats), without the two terms that only
 * event frames have:
 *
 *     produced = sent + superseded + pending          (one slot)
 *
 * so that one message can be accounted for frame by frame while others share the queue. The
 * slots' superseded values add up to the queue's. The unit tests assert all of this after
 * every step of randomised scenarios.
 *
 * TIME. Every time is a uint64_t count of nanoseconds on the caller's clock, passed in; the
 * queue reads no clock of its own. A deadline at or beyond UINT64_MAX is never reached, which
 * is what CAN_TXQ_NO_DEADLINE says: it lies 584 years after the clock's zero.
 *
 * CONCURRENCY. None in here: the caller serialises all calls (the application does so with
 * a short critical section around each).
 */
#ifndef FCN_CAN_TXQ_H
#define FCN_CAN_TXQ_H

#include <stdbool.h>
#include <stdint.h>

#include "can/can_fc.h"
#include "can/can_frame.h"

#define CAN_TXQ_CYCLIC_SLOTS  16U
#define CAN_TXQ_EVENT_DEPTH   32U
#define CAN_TXQ_MAX_MAILBOXES 8U

/** Returned by can_txq_next_deadline() when no time-driven work is scheduled. */
#define CAN_TXQ_NO_DEADLINE UINT64_MAX

/** Requests to the caller, combined in a bit mask. */
#define CAN_TXQ_REQUEST_ABORT_MAILBOXES 0x1U /**< remove waiting frames from the controller */
#define CAN_TXQ_REQUEST_RESTART         0x2U /**< ask the controller to recover from bus off */

typedef struct {
    uint8_t mailboxes; /**< controller mailboxes to use, 1..CAN_TXQ_MAX_MAILBOXES */
    /** Wait before the first recovery request. 0: request recovery at once after every
     *  bus-off; the hold-off then never grows. */
    uint32_t holdoff_initial_ms;
    uint32_t holdoff_max_ms;   /**< upper bound of the doubling hold-off */
    uint32_t stable_ms;        /**< time out of bus-off after which the hold-off resets */
    uint32_t event_timeout_ms; /**< event frames that have waited this long expire; 0: never */
} can_txq_config_t;

typedef struct {
    uint64_t produced;   /**< frames handed to can_txq_publish() or can_txq_enqueue() */
    uint64_t sent;       /**< frames confirmed transmitted */
    uint64_t superseded; /**< cyclic values replaced by a newer value before they went out */
    uint64_t dropped;    /**< event frames refused because the FIFO was full */
    uint64_t expired;    /**< event frames discarded because they waited too long */
    uint32_t pending;    /**< frames waiting now: slots + FIFO + mailboxes */
    uint32_t bus_off_count;
    uint32_t restart_requests;
    uint32_t holdoff_ms; /**< hold-off that applies to the current or next bus-off */
    /** Aborts asked for because a frame in a mailbox had gone stale (see STALE FRAMES IN A
     *  MAILBOX), not those for bus off. 64 bits like the frame counters: on a bus too busy for
     *  a message's own period it can grow with every publish. */
    uint64_t recall_requests;
} can_txq_stats_t;

/** The counters of one cyclic slot: the share of can_txq_stats_t that belongs to its message.
 *  A cyclic value is never dropped and never expires, so produced = sent + superseded +
 *  pending holds for each slot at every moment. */
typedef struct {
    uint64_t produced;   /**< values can_txq_publish() accepted for the slot */
    uint64_t sent;       /**< of those, confirmed transmitted */
    uint64_t superseded; /**< of those, replaced by a newer value before they went out */
    /** of those, waiting now: 0, 1 or 2 (a value in a mailbox and its successor in the slot) */
    uint32_t pending;
} can_txq_slot_stats_t;

/** What can_txq_on_tx_done() tells the caller about the frame that went out. */
typedef struct {
    bool cyclic;       /**< true: a cyclic slot; false: an event frame */
    uint8_t slot;      /**< cyclic: the slot; event frame: 0 */
    uint32_t id;       /**< identifier of the frame */
    uint64_t stamp_ns; /**< the time given when the frame was published or enqueued */
} can_txq_done_t;

/* Private to can_txq.c; in the header only so that a queue can be allocated statically.
 *
 * A frame and the time that travels with it. A frame the queue holds is in exactly one place:
 * waiting in its cyclic slot, waiting in the event FIFO, or noted in a mailbox record. */
typedef struct {
    can_frame_t frame;
    uint64_t stamp_ns; /* cyclic: as given to can_txq_publish(); event: time of the enqueue */
} can_txq_item_t;

typedef struct {
    can_txq_item_t value; /* the value waiting to be loaded; meaningful while `waiting` */
    bool waiting;         /* a value is waiting to be loaded */
    bool in_mailbox;      /* an earlier value is in a controller mailbox */
} can_txq_slot_t;

/* One frame that was handed out and has been neither confirmed nor aborted yet. */
typedef struct {
    bool used;
    bool cyclic;         /* true: a value of `slot`; false: an event frame */
    uint8_t slot;        /* cyclic: the slot the value belongs to; event frame: 0 */
    uint32_t handle;     /* as given to the caller */
    can_txq_item_t item; /* reported when the frame is done, put back when it is aborted */
} can_txq_mailbox_t;

/* The members are in an order that leaves no gap between them beyond one byte after
 * `event_record`: the 32-bit members and the flags stand together instead of each next to what
 * it describes, and the three small members of the recall fill the gap that the 20 bytes of
 * `config` leave before the 8-byte alignment of the slots. A node on two buses would have an
 * array of these, and the padding check of the project's clang-tidy configuration multiplies
 * every wasted byte by the length of the array. */
typedef struct {
    can_txq_config_t config; /* after clamping */
    bool recall_pending;     /* a publish found its predecessor in a mailbox; see can_txq_poll */
    bool event_recalled;     /* the event frame in a mailbox has been called back already */
    uint8_t event_record;    /* index of the record of the event frame, while event_in_mailbox */
    can_txq_slot_t slots[CAN_TXQ_CYCLIC_SLOTS];
    /* The event FIFO, a ring. One entry more than the FIFO admits: the place of the event
     * frame that may come back from a mailbox (see can_txq_on_tx_aborted). */
    can_txq_item_t events[CAN_TXQ_EVENT_DEPTH + 1U];
    uint32_t event_head;  /* index of the oldest waiting event frame */
    uint32_t event_count; /* event frames waiting */
    can_txq_mailbox_t mailboxes[CAN_TXQ_MAX_MAILBOXES];
    uint32_t handle_serial;     /* counts hand-outs; part of every handle */
    bool event_in_mailbox;      /* an event frame is in a controller mailbox */
    bool bus_off;               /* the controller's last reported state was bus off */
    bool restart_pending;       /* bus off, and recovery has not been requested yet */
    bool holdoff_reset_pending; /* back on the bus with a raised hold-off */
    uint64_t bus_off_since_ns;  /* start of the hold-off while restart_pending */
    uint64_t recovered_at_ns;   /* start of the stable time while holdoff_reset_pending */
    can_txq_stats_t stats;
    can_txq_slot_stats_t slot_stats[CAN_TXQ_CYCLIC_SLOTS];
} can_txq_t;

/**
 * Prepare an empty queue for an error-active controller. Out-of-range configuration values
 * are clamped: mailboxes to 1..CAN_TXQ_MAX_MAILBOXES, holdoff_max_ms to at least
 * holdoff_initial_ms.
 */
void can_txq_init(can_txq_t *queue, const can_txq_config_t *config);

/**
 * Publish the current value of the cyclic message that owns `slot`. Replaces a value of the
 * same slot that is still waiting. `stamp_ns` travels with the frame and comes back in
 * can_txq_done_t (the application passes the time of the sensor sample, which makes the
 * sample-to-bus latency measurable). If an earlier value of the message is still in a mailbox
 * and the controller is not bus off, the next can_txq_poll() asks for the mailboxes to be
 * aborted, unless by then the earlier value has been confirmed or aborted and no other value
 * waits behind its predecessor, or the controller has gone bus off (see STALE FRAMES IN A
 * MAILBOX). Returns false only for an invalid slot or frame; such a call is not counted as
 * produced and changes nothing.
 */
bool can_txq_publish(can_txq_t *queue, uint8_t slot, const can_frame_t *frame, uint64_t stamp_ns);

/**
 * Queue an event frame at time `now_ns`. Returns false if the frame is invalid (not
 * counted) or the FIFO is full (counted as produced and dropped). Frames that have expired
 * by `now_ns` are discarded first, so that they do not keep a new frame out.
 */
bool can_txq_enqueue(can_txq_t *queue, const can_frame_t *frame, uint64_t now_ns);

/**
 * The next frame to load into a controller mailbox, or false if nothing is waiting, the
 * configured number of mailboxes is in use, the controller is bus off, or the only frames
 * waiting are event frames and one event frame is already in a mailbox. For the same reason a
 * value of a cyclic message is not handed out while its earlier value is in a mailbox (see
 * WHICH FRAME NEXT): when that is all that waits, the result is false as well. `handle`
 * identifies the frame in can_txq_on_tx_done() and can_txq_on_tx_aborted(); no two frames
 * that are outstanding at the same time have the same handle. Event frames that have expired
 * by `now_ns` are discarded here and never returned. If `frame` or `handle` is NULL, the call
 * returns false and changes nothing at all, expiry included.
 */
bool can_txq_next(can_txq_t *queue, uint64_t now_ns, can_frame_t *frame, uint32_t *handle);

/** The controller confirmed the frame with this handle. `info` may be NULL. Returns false,
 *  changing nothing, if the handle is not one that is outstanding. */
bool can_txq_on_tx_done(can_txq_t *queue, uint32_t handle, can_txq_done_t *info);

/** The controller removed the frame with this handle from its mailbox without sending it.
 *  The frame goes back to its slot or to the head of the FIFO as described above. Returns
 *  false, changing nothing, if the handle is not one that is outstanding. */
bool can_txq_on_tx_aborted(can_txq_t *queue, uint32_t handle);

/**
 * The controller's error state changed. Returns the requests the caller has to carry out
 * now (CAN_TXQ_REQUEST_ABORT_MAILBOXES when the controller just went bus off). The queue
 * only tells bus off from not bus off: error active and error passive are the same to it,
 * and a report of the state it already knows changes nothing.
 */
uint32_t can_txq_on_state(can_txq_t *queue, can_fc_state_t state, uint64_t now_ns);

/**
 * Time-driven work: returns CAN_TXQ_REQUEST_RESTART once when the bus-off hold-off has
 * elapsed, CAN_TXQ_REQUEST_ABORT_MAILBOXES when a frame in a mailbox has gone stale (see
 * STALE FRAMES IN A MAILBOX), resets the hold-off after the stable time, and discards event
 * frames that have expired. The two requests never come together: one needs the controller to
 * be bus off, the other needs it not to be. Call it whenever can_txq_next_deadline() has
 * passed; calling it more often is harmless.
 */
uint32_t can_txq_poll(can_txq_t *queue, uint64_t now_ns);

/**
 * When can_txq_poll() next has something to do: the earliest of the restart request, the
 * hold-off reset, the expiry of the oldest waiting event frame and the recall of an event
 * frame in a mailbox at its timeout; 0, which has always passed, while the recall of a cyclic
 * value is due; CAN_TXQ_NO_DEADLINE if none of them is scheduled (see TIME). Right after
 * can_txq_poll() was called with a time, the deadline is later than that time, so a caller
 * that polls and then sleeps until the deadline does not spin. A publish or an enqueue can
 * make the deadline earlier, and so can the reports of the controller (an event frame that
 * comes back, a recovery from bus off), so such a caller reads the deadline again after each
 * of its own calls and has to be woken by the calls of others, as the transmit task of the
 * application is.
 */
uint64_t can_txq_next_deadline(const can_txq_t *queue);

const can_txq_stats_t *can_txq_stats(const can_txq_t *queue);

/**
 * The event frames the queue holds now: those waiting in the FIFO and the one in a mailbox, if
 * one is. While it is not 0 the transmit path has an event frame to report or to load, so a
 * caller that serves it (the transmit task) looks at the FIFO again when that frame's report
 * comes, or when the cyclic frames that keep the mailboxes busy are reported: a producer that
 * reads it in the same critical section as its can_txq_enqueue() and finds it above 0 knows
 * that its frame will be loaded without a wake-up of its own (app/can_io.c,
 * can_io_send_event_stamped()). Any context, serialised with the other calls.
 */
uint32_t can_txq_events_held(const can_txq_t *queue);

/** The counters of cyclic slot `slot` (see ACCOUNTING); NULL if `slot` is not below
 *  CAN_TXQ_CYCLIC_SLOTS. */
const can_txq_slot_stats_t *can_txq_slot_stats(const can_txq_t *queue, uint8_t slot);

#endif /* FCN_CAN_TXQ_H */
