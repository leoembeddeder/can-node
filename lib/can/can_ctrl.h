/* SPDX-License-Identifier: MIT */
/**
 * @file
 * Model of a CAN controller on a shared bus.
 *
 * WHY. The transports this project runs on, a Linux vcan interface or UDP datagrams on
 * localhost, deliver a frame instantly and never fail. A real 500 kbit/s bus carries one frame
 * at a time, a frame takes about a quarter of a millisecond, a disturbed bus destroys frames,
 * and a controller that keeps failing takes itself off the bus. This model sits between the
 * driver and the transport and adds those properties, so that throughput, bus load, latency
 * and bus-off behaviour seen by the software above are those of a bus, not of a socket.
 *
 * WHAT IT IS NOT. It works at frame granularity. It does not simulate bit timing or the
 * detection of errors, and it decides arbitration by comparing whole arbitration fields, not
 * bit by bit. It is told which fault is on the bus and derives what a controller would
 * observe; the counter and state rules themselves are in can_fc. The assumptions made for
 * each fault, and for the frames of other nodes, are spelled out below, because they are
 * modelling decisions, not part of the standard.
 *
 * TIME. The model has no clock. Calls that depend on time take `now_ns`, a monotonic time in
 * nanoseconds that never decreases from one call to the next. A unit test passes whatever
 * times it likes; the host glue passes CLOCK_MONOTONIC and sleeps until can_ctrl_next_wakeup().
 *
 * THE BUS TIMELINE. The bus is free from `bus_free_at`. Every frame, sent or received, takes
 * its exact stuffed length in bit times (can_bits_exact) and is followed by the 3-bit
 * intermission, so the modelled bus can never carry more than 100 % load.
 *
 *  - Transmit. A frame submitted with can_ctrl_tx_submit() waits in one of
 *    CAN_CTRL_TX_MAILBOXES mailboxes and competes for the bus from its submission on. Of the
 *    frames submitted by the moment the bus becomes free, the one with the smallest
 *    arbitration key (can_frame_arbitration_key) competes for this node; mailboxes with equal
 *    keys go in submission order. When it wins (see ARBITRATION) it occupies the bus from its
 *    start of frame to its end of frame and is reported by can_ctrl_poll() as
 *    CAN_CTRL_ACTION_TX_COMPLETE at its end of frame: only then does the glue write it to
 *    the transport, because that is when a real receiver would have it.
 *  - Receive. A frame read from the transport is handed in with can_ctrl_wire_rx(). The
 *    transport delivers a frame once another node has sent it, so the model learns of it
 *    after the fact, and takes it to come from a node that was ready to send it one frame
 *    length before its arrival. It goes into the wire queue (CAN_CTRL_WIRE_QUEUE frames) and
 *    is reported as CAN_CTRL_ACTION_RX_DELIVER at its end of frame:
 *      - If no other received frame waits and the bus has been free since before the call,
 *        the frame passed while the bus was free. This node cannot have competed for that bus
 *        time: a frame of its own that was allowed to start then would have started. The frame
 *        is put on the timeline at once: it ends at its arrival, or, if the bus became free
 *        less than one frame length before that, one frame length after the bus became free.
 *      - Otherwise its sender is taken to be still waiting for the bus (the bus is busy, or
 *        becomes free at the very instant of the call, or other received frames wait before
 *        it). The frame waits in the wire queue, behind the frames that arrived before it,
 *        and starts when it wins arbitration; it competes from the time of the call on, as a
 *        mailbox does from its submission.
 *    Received frames keep the order in which the transport delivered them. A sender that
 *    ignores the bit rate therefore cannot push more than the bus could carry into the node:
 *    its frames wait in the wire queue and are delivered at bus speed, and a frame handed in
 *    while the queue holds CAN_CTRL_WIRE_QUEUE frames (waiting or on the timeline) is
 *    dropped and counted as wire_overruns. The queue stands for the transmit buffers of the
 *    other nodes: under a load above what the bus carries, it fills, and from then on the
 *    excess of the other nodes' traffic is counted there.
 *  - Late callers. The model reports a frame no earlier than its end of frame, but the caller
 *    may poll late. If a transmission could have started more than one frame length before
 *    `now_ns`, its start is moved to `now_ns` and late_resyncs is counted, so that a stalled
 *    host never makes the model emit frames faster than the bit rate in real time. Only the
 *    start of this node's frame that won arbitration moves; received frames that lost to it
 *    wait for it, and received frames are never moved: they are reported at their own end of
 *    frame even when that lies before `now_ns`.
 *  - Pacing off. With bitrate 0 every bit time is zero: frames complete as soon as they are
 *    polled. This mode exists to measure the raw transport.
 *
 * ARBITRATION. Whenever the bus becomes free (at the end of an intermission, of a bus fault,
 * of a recovery or of this node's suspend), the frames waiting for it compete: this node's
 * best mailbox, if the node may transmit, and the oldest frame waiting in the wire queue. The
 * one with the smaller arbitration key starts, as on a real bus, where every node that has a
 * frame pending starts it at the same instant and the smaller arbitration field survives. A
 * frame that loses waits for the next time the bus is free. With equal keys the received frame
 * goes first. (Two nodes that send the same arbitration field break a rule of CAN: neither
 * loses arbitration and the frames collide later in the frame. The model has to pick one.)
 * So a backlog of lower-priority frames from other nodes, however long, delays a frame of this
 * node by at most the frame on the bus when it was submitted, while a waiting frame of another
 * node with a smaller key still goes first.
 *  - Only the oldest waiting received frame competes. A frame from the transport carries no
 *    sender, so the model cannot tell which waiting frames come from different nodes and would
 *    arbitrate among themselves; it keeps the order the transport delivered them in. A
 *    received frame with a small key that waits behind one with a large key therefore waits
 *    for it, even where this node's frame goes in between.
 *  - A received frame starts without competing when no frame of this node may start at that
 *    instant: none is pending, the node is bus off, or it is waiting out its suspend.
 *  - A short or a bus held dominant lets no received frame start; RECEPTION AND FAULTS says
 *    what happens to the frames waiting when such a fault takes effect.
 *
 * TRANSMISSION ATTEMPTS AND FAULTS. The outcome of an attempt is decided by the bus fault in
 * force at its start of frame. A destroyed attempt is retried automatically, as on a real
 * controller: the frame stays in its mailbox. An error frame is taken as 6 flag bits plus
 * 8 delimiter bits. Each case lists the bus time used before the intermission and the
 * can_fc events applied.
 *
 *  - No fault: the frame's length; CAN_FC_TX_SUCCESS.
 *  - An injected transmit error (can_ctrl_inject_tx_errors, one consumed per attempt): the
 *    attempt is destroyed halfway, (length / 2) + 14 bits; CAN_FC_TX_ERROR.
 *  - CAN_BUS_FAULT_SHORT, CAN_H shorted to CAN_L, so the bus reads recessive whatever is
 *    driven: bit error on the start-of-frame bit, 1 + 14 bits; CAN_FC_TX_ERROR, and while the
 *    node is error active also CAN_FC_TX_BIT_ERROR_IN_FLAG, because the dominant bits of its
 *    active error flag read back recessive too (applied once per flag).
 *  - CAN_BUS_FAULT_NO_ACK, no other node acknowledges: acknowledgment error in the ACK slot,
 *    (length - 8) + 14 bits; CAN_FC_TX_ACK_ERROR_ALONE.
 *  - CAN_BUS_FAULT_STUCK_DOMINANT, the bus is held dominant: the bus is never idle, so no
 *    attempt starts. The node observes the dominant level as a receiver: 6 bits after the
 *    fault begins (or after the bus would have become free) a stuff error, CAN_FC_RX_ERROR;
 *    the node then sends its error flag (6 bits) and sees a dominant first bit after it,
 *    7 bits after the error, CAN_FC_RX_DOMINANT_AFTER_FLAG. Rule 6 counts the dominant bits
 *    that follow the flag: the 14th after an active error flag, the 8th after a passive one,
 *    so CAN_FC_RX_DOMINANT_RUN comes 13 bits after the first bit after the flag if the node
 *    was error active when it detected the error, 7 if it was error passive, and then every
 *    8 bits, until the receive counter saturates. When the fault is removed the bus is free
 *    11 bit times later.
 *  A fault that begins or ends while a frame is on the wire takes effect from the next frame.
 *
 * SUSPEND TRANSMISSION. An error-passive node that was the transmitter of the last frame on
 * the bus waits 8 further bit times after the intermission before it may start again. A
 * received frame waiting for the bus starts at the end of the intermission without competing;
 * it is then the last frame on the bus, and the suspend is over.
 *
 * RECEPTION AND FAULTS. A frame handed to can_ctrl_wire_rx() while the node is bus off, or
 * while the bus is shorted or stuck dominant, cannot have reached this node: it is discarded
 * and counted in rx_lost_fault. Received frames in the wire queue that have not started when
 * a short or a dominant bus takes effect, those still waiting for the bus included, never
 * pass either: they are discarded then and counted the same way. A frame handed in after the
 * removal of such a fault was asked for, but before the removal takes effect, waits for the
 * bus to be released. An injected receive error (can_ctrl_inject_rx_errors, one per frame)
 * destroys the frame at its end: CAN_FC_RX_ERROR, nothing delivered. Otherwise
 * CAN_FC_RX_SUCCESS and the frame is delivered.
 *
 * ABORTING. can_ctrl_tx_abort_all() empties the mailboxes, as a driver does when what it
 * queued has gone stale: on bus off, or when a frame was not confirmed in time because the
 * bus is faulty or busy. A frame whose attempt is on the wire at that moment cannot be taken
 * back; it is marked instead. If that attempt succeeds, the frame is transmitted and
 * reported as usual. If it fails, the frame is not retried: it is removed and reported with
 * CAN_CTRL_ACTION_TX_ABORTED at the end of the failed attempt, after the error counting for
 * that attempt. (bxCAN's abort request and M_CAN's cancellation request behave this way.)
 * Without the mark, a node whose frames are never acknowledged would keep one frame in the
 * air for ever, and no abort could ever catch it between two attempts.
 *
 * BUS OFF AND RECOVERY. When the transmit counter reaches the bus-off limit the node stops
 * transmitting and receiving. Frames still in mailboxes stay there until the driver aborts
 * them (can_ctrl_tx_abort_all) or the node recovers. Recovery starts only on
 * can_ctrl_request_restart() and completes after 128 occurrences of 11 consecutive recessive
 * bits: on an idle bus, or one shorted to recessive, that is 128 x 11 bit times (2.816 ms at
 * 500 kbit/s); a frame passing on the bus contributes one occurrence, its last 11 bits; a
 * bus held dominant contributes none. Recovery is reported as a state change to error
 * active with both counters at 0.
 *
 * ACTIONS. can_ctrl_poll() returns one action per call, oldest first, until nothing more is
 * due at `now_ns`. Every change of the error state is reported as
 * CAN_CTRL_ACTION_STATE_CHANGED, in sequence with the frame actions around it.
 */
#ifndef FCN_CAN_CTRL_H
#define FCN_CAN_CTRL_H

#include <stdbool.h>
#include <stdint.h>

#include "can/can_fc.h"
#include "can/can_frame.h"

#define CAN_CTRL_TX_MAILBOXES 8U
#define CAN_CTRL_WIRE_QUEUE   64U
#define CAN_CTRL_ACTION_QUEUE 16U

/** Bits of an error frame as modelled: 6 flag bits and 8 delimiter bits. */
#define CAN_CTRL_ERROR_FRAME_BITS 14U
/** Extra bits an error-passive transmitter waits before transmitting again. */
#define CAN_CTRL_SUSPEND_BITS 8U
/** No wake-up is needed until something is submitted, received or changed. */
#define CAN_CTRL_NO_WAKEUP UINT64_MAX

typedef enum {
    CAN_BUS_FAULT_NONE = 0,
    CAN_BUS_FAULT_SHORT,          /**< CAN_H shorted to CAN_L: the bus always reads recessive */
    CAN_BUS_FAULT_STUCK_DOMINANT, /**< the bus is held dominant */
    CAN_BUS_FAULT_NO_ACK          /**< no other node acknowledges this node's frames */
} can_bus_fault_t;

typedef struct {
    uint32_t bitrate; /**< bit/s; 0 switches pacing off */
} can_ctrl_config_t;

typedef enum {
    CAN_CTRL_ACTION_NONE = 0,
    CAN_CTRL_ACTION_TX_COMPLETE,   /**< write `frame` to the transport, confirm `handle` */
    CAN_CTRL_ACTION_RX_DELIVER,    /**< hand `frame` to the driver */
    CAN_CTRL_ACTION_STATE_CHANGED, /**< the error state went from `old_state` to `state` */
    CAN_CTRL_ACTION_TX_ABORTED     /**< the frame with `handle` was on the wire when the
                                        mailboxes were aborted, its attempt failed, and it
                                        has now been removed without being transmitted */
} can_ctrl_action_kind_t;

typedef struct {
    can_ctrl_action_kind_t kind;
    uint64_t time_ns;   /**< model time: end of frame, the moment of the state change, or the
                             end of the failed attempt (TX_ABORTED) */
    uint64_t start_ns;  /**< TX_COMPLETE, RX_DELIVER: start of frame on the timeline */
    uint64_t submit_ns; /**< TX_COMPLETE: when the frame was submitted */
    can_frame_t frame;  /**< TX_COMPLETE, RX_DELIVER, TX_ABORTED */
    uint32_t handle;    /**< TX_COMPLETE, TX_ABORTED: the handle given to can_ctrl_tx_submit() */
    can_fc_state_t state;
    can_fc_state_t old_state;
    uint16_t tec; /**< counters after the action */
    uint16_t rec;
} can_ctrl_action_t;

typedef struct {
    uint64_t tx_frames;          /**< frames transmitted successfully */
    uint64_t tx_attempts_failed; /**< transmission attempts destroyed by an error */
    uint64_t tx_aborted;         /**< frames removed by can_ctrl_tx_abort_all(), at once or
                                      after their attempt in flight failed */
    uint64_t rx_frames;          /**< frames delivered */
    uint64_t rx_errors;          /**< received frames destroyed by an injected error */
    uint64_t rx_lost_fault;      /**< frames discarded: node bus off or bus faulty */
    uint64_t wire_overruns;      /**< frames discarded: they arrived faster than the bit rate
                                      allows for longer than the wire queue covers */
    uint64_t bus_off_events;     /**< times the node went bus off */
    uint64_t late_resyncs;       /**< times a late caller made the timeline skip ahead */
    uint64_t busy_ns;            /**< time the bus carried frames or error frames, without
                                      intermissions; bus load = busy_ns / elapsed time */
} can_ctrl_stats_t;

/* The structures below are part of the header only so that a controller can be allocated
 * statically. Their fields are private to can_ctrl.c, which explains them. */
typedef struct {
    uint64_t submit_ns;
    uint64_t order; /* submission order, for equal arbitration keys; 64 bits never wrap */
    can_frame_t frame;
    uint32_t handle;
    uint16_t bits; /* stuffed length, from can_bits_exact() at submission */
    bool used;
} can_ctrl_mailbox_t;

typedef struct {
    uint64_t start_ns; /* on the timeline: start of frame; still waiting: UINT64_MAX */
    uint64_t eof_ns;   /* on the timeline: end of frame */
    uint64_t ready_ns; /* when it was handed in: it competes for the bus from then on */
    can_frame_t frame;
    uint16_t bits;
    bool lost; /* passes while the node is bus off: occupies the bus, never delivered */
} can_ctrl_wire_slot_t;

typedef struct {
    can_ctrl_config_t config;
    can_fc_t fc;
    can_bus_fault_t fault;           /* as last set, what can_ctrl_fault() returns */
    can_bus_fault_t fault_effective; /* in force on the timeline */
    uint64_t fault_since_ns;         /* when `fault` takes effect, while the two differ */

    uint64_t last_now_ns;     /* latest time any call was given */
    uint64_t model_ns;        /* time of the last event processed */
    uint64_t bus_free_at_ns;  /* end of the intermission after everything placed on the bus */
    uint64_t own_free_at_ns;  /* the same plus the suspend, after an attempt of this node */
    uint64_t started_free_ns; /* end of the intermission after what has started */
    bool last_frame_was_ours;

    can_ctrl_mailbox_t mailboxes[CAN_CTRL_TX_MAILBOXES];
    uint64_t next_order;

    uint64_t tx_start_ns;
    uint64_t tx_end_ns;
    uint16_t tx_bits; /* bus time of the attempt in flight, its error frame included */
    uint8_t tx_mailbox;
    uint8_t tx_outcome; /* what the attempt in flight ends in, decided at its start */
    bool tx_in_flight;
    bool tx_abort_marked;

    can_ctrl_wire_slot_t wire[CAN_CTRL_WIRE_QUEUE]; /* received frames, in arrival order */
    uint32_t wire_head;
    uint32_t wire_count;  /* frames in the queue */
    uint32_t wire_placed; /* the first wire_placed of them are on the timeline, the rest wait */

    uint32_t inject_tx_errors;
    uint32_t inject_rx_errors;

    uint64_t dominant_base_ns;        /* stuck-dominant fault: when it took effect */
    uint32_t dominant_bits;           /* bit time after the base of the next counter step */
    uint32_t dominant_first_run_bits; /* first bit after the flag to the first rule-6 step */

    uint64_t recovery_mark_ns; /* start of the recessive run being counted towards recovery */

    can_ctrl_action_t actions[CAN_CTRL_ACTION_QUEUE];
    uint32_t action_head;
    uint32_t action_count;

    can_ctrl_stats_t stats;
} can_ctrl_t;

/** Reset the model: error active, counters 0, no fault, bus free at `now_ns`. A bitrate of 0
 *  switches pacing off. */
void can_ctrl_init(can_ctrl_t *ctrl, const can_ctrl_config_t *config, uint64_t now_ns);

/**
 * Put a frame into a free transmit mailbox. `handle` comes back in the TX_COMPLETE action.
 * Returns false if the frame is invalid or all mailboxes are in use. Frames may be submitted
 * while the node is bus off; they wait.
 */
bool can_ctrl_tx_submit(can_ctrl_t *ctrl, const can_frame_t *frame, uint32_t handle,
                        uint64_t now_ns);

/** Number of mailboxes in use, the frame on the wire included. */
uint32_t can_ctrl_tx_pending(const can_ctrl_t *ctrl);

/**
 * Empty all mailboxes. Returns the number of frames removed now and, if `handles` is not
 * NULL, writes their handles to it (at most `max_handles`; CAN_CTRL_TX_MAILBOXES is always
 * enough). The frame whose attempt is on the wire at this moment is not among them: it is
 * marked, and either completes (CAN_CTRL_ACTION_TX_COMPLETE) or, if the attempt fails, is
 * removed then and reported with CAN_CTRL_ACTION_TX_ABORTED; see ABORTING above. Both ways
 * of removal count in stats.tx_aborted. A frame submitted after the call is not affected.
 */
uint32_t can_ctrl_tx_abort_all(can_ctrl_t *ctrl, uint32_t *handles, uint32_t max_handles);

/**
 * A frame arrived from the transport at `arrival_ns` (not later than the next `now_ns`).
 * Returns false if it was discarded: invalid, node bus off, bus faulty, or wire queue full;
 * the matching statistic is counted.
 */
bool can_ctrl_wire_rx(can_ctrl_t *ctrl, const can_frame_t *frame, uint64_t arrival_ns);

/**
 * Advance the model to `now_ns` and take the next due action. Returns false when nothing
 * more is due; call it in a loop. `action` may be NULL to advance without taking actions
 * (they stay queued; if the action queue is full the model stops advancing until it is read).
 */
bool can_ctrl_poll(can_ctrl_t *ctrl, uint64_t now_ns, can_ctrl_action_t *action);

/**
 * Earliest model time at which can_ctrl_poll() will have something new to do if no frame is
 * submitted or received and no fault changes in the meantime; CAN_CTRL_NO_WAKEUP if there is
 * no such time. Never earlier than the last `now_ns` unless an action is already due.
 */
uint64_t can_ctrl_next_wakeup(const can_ctrl_t *ctrl);

/** Apply or remove (CAN_BUS_FAULT_NONE) a bus fault from `now_ns` on. */
void can_ctrl_set_fault(can_ctrl_t *ctrl, can_bus_fault_t fault, uint64_t now_ns);

can_bus_fault_t can_ctrl_fault(const can_ctrl_t *ctrl);

/** Destroy the next `count` transmission attempts with a bit error (adds to what is left). */
void can_ctrl_inject_tx_errors(can_ctrl_t *ctrl, uint32_t count);

/** Destroy the next `count` received frames with an error (adds to what is left). */
void can_ctrl_inject_rx_errors(can_ctrl_t *ctrl, uint32_t count);

/** Ask a bus-off node to recover. Ignored in any other state. */
void can_ctrl_request_restart(can_ctrl_t *ctrl, uint64_t now_ns);

can_fc_state_t can_ctrl_state(const can_ctrl_t *ctrl);
uint16_t can_ctrl_tec(const can_ctrl_t *ctrl);
uint16_t can_ctrl_rec(const can_ctrl_t *ctrl);
const can_ctrl_stats_t *can_ctrl_stats(const can_ctrl_t *ctrl);

#endif /* FCN_CAN_CTRL_H */
