/* SPDX-License-Identifier: MIT */
#include "can/can_ctrl.h"

#include <string.h>

#include "can/can_bits.h"

/*
 * HOW THE MODEL RUNS
 *
 * The model is a discrete-event simulation on the bus timeline. Every call that takes a time
 * first advances the model to that time (catch_up(); an input stops just before the events of
 * its own instant), then applies its own input, so an input always meets the model exactly as
 * it stood at that moment, whoever polled in between. That is what makes the result independent
 * of how often the caller polls: polling only at can_ctrl_next_wakeup() and at input times gives
 * the same actions, with the same times, as polling every microsecond. The one deliberate
 * exception is the late-caller rule.
 *
 * Advancing takes the earliest pending event, processes it at its own time (model_ns), and
 * repeats until the next event lies after `now`. The events, and at equal times the order in
 * which they are taken:
 *
 *   1  FRAME_END  the attempt in flight ends, or the oldest received frame on the timeline
 *                 reaches its end of frame. While an attempt is in flight it is always the
 *                 earlier of the two: a frame is placed on the timeline only after everything
 *                 placed before it, and an attempt starts only once the frames before it have
 *                 ended.
 *   2  RECOVERY   the 128th occurrence of 11 recessive bits.
 *   3  FAULT      a fault change takes effect.
 *   4  DOMINANT   the next counter step while the bus is held dominant.
 *   5  START      the bus is free and the winner of arbitration starts (next_start()): this
 *                 node's best mailbox (TX_START) or the oldest received frame waiting in the
 *                 wire queue (RX_START). Neither produces an action. A TX_START is a wake-up
 *                 time, because a start taken late by more than a frame length would trigger
 *                 the late-caller rule. An RX_START is not: nothing depends on when it is
 *                 processed, so can_ctrl_next_wakeup() looks through it to that frame's end.
 *
 * Frame ends come first so that a fault or a start at the very end of the intermission sees the
 * frame finished; a recovery occurrence that ends exactly at a start of frame or at the onset of
 * a dominant fault counts, because its 11 recessive bits are complete by then. A fault comes
 * before a start at the same instant, so that a fault taking effect when the bus becomes free
 * is in force at the start of the next frame.
 *
 * DECISIONS WHERE THE CONTRACT IS OPEN (each is also stated where it is implemented)
 *
 *  - Order of actions. The counters of an event are updated first, then its STATE_CHANGED (if
 *    any) is queued, then its frame action (TX_COMPLETE, RX_DELIVER, TX_ABORTED). Every action
 *    carries the counters as they are after the whole event. The contract states this order for
 *    TX_ABORTED; it is applied to all frame actions alike.
 *  - When a fault takes effect. A fault set at F takes effect at the end of the intermission of
 *    whatever is on the bus at F (F itself on an idle bus). This is "from the next frame", and
 *    for a dominant fault it is the "or after the bus would have become free" of the contract.
 *  - Received frames, waiting or placed. A frame handed in goes on the timeline at once only
 *    if no received frame waits before it, the bus carries frames (no short or dominant bus in
 *    force) and its start would lie before the time of the call (hand_in()). Every start of
 *    frame before that time has been decided by then (unless a full action queue holds the
 *    model up, see below), and none took the bus: the received frame was alone. A start at the
 *    very instant of the call has not been decided (an input comes before the events of its
 *    instant), so such a frame waits and arbitrates.
 *  - Arbitration against other nodes. At a start, this node's best mailbox competes if it
 *    could start then (transmission_start()), the oldest waiting received frame if it could
 *    start then (received_start()); if both can, the smaller arbitration key wins and equal
 *    keys go to the received frame. Only the oldest waiting received frame competes, so
 *    received frames keep their arrival order.
 *  - Faults and received frames. A frame handed in while the fault last set is a short or a
 *    dominant bus is discarded (rx_lost_fault). Received frames already queued that have not
 *    started when such a fault takes effect never reach the bus either: they are discarded then
 *    and counted the same way. When a short ends, nothing starts before the moment it ended;
 *    when a dominant fault ends, nothing starts for 11 bit times.
 *  - Injected transmit errors and bus faults. The error that strikes first wins: a short
 *    destroys the start-of-frame bit, before the middle of the frame where an injected error
 *    strikes, so under a short the injected error is not consumed; the missing acknowledgment
 *    comes after the middle, so under CAN_BUS_FAULT_NO_ACK the injected error is consumed.
 *  - Suspend transmission. "The transmitter of the last frame on the bus" is decided by the
 *    timeline: a received frame placed after this node's attempt, or a period of dominant bus,
 *    ends the suspend.
 *  - Bus held dominant. The steps follow the contract: 6 bits, then 7, then 7, then every 8
 *    bits. (Rule 6 of the specification counts the 14th dominant bit after an ACTIVE error flag,
 *    which would put the first run step 13 bits after the dominant bit of rule 2, not 7; the 7
 *    of the contract is the count for a passive flag. Kept as the contract says.) Steps are taken
 *    while the node is not bus off and its receive counter is below its maximum.
 *  - Received frames and bus off. A frame is judged at its end of frame: if the node is bus off
 *    then, it is not delivered and counts in rx_lost_fault. A frame handed in while the node is
 *    bus off is refused and counted at once, but kept in the wire queue (marked lost) as long
 *    as it has room, because it still passes on the bus: it occupies the bus, delays later
 *    frames, counts towards recovery and, once the node has recovered, arbitrates against it.
 *  - Recovery counting. See recovery_count_until().
 *  - Late callers. A transmission whose start lies more than its own frame length before `now`
 *    starts at `now`. With pacing off a frame lasts 0 ns, so any lateness moves the start to
 *    `now`, and is counted: the frame then completes at the poll, as the contract says. The
 *    arbitration against received frames is not run again for the moved start: this node's
 *    frame won the bus, the received frames that lost wait for it. (Among the mailboxes it is
 *    run again, and its winner has a key no larger than the frame that won.)
 *  - Inputs while the action queue is full. The model stops before an event for which the queue
 *    has no room for the two actions an event can produce. An input given while it is stopped
 *    is applied to the model as it stands; a mailbox submitted then still competes only from its
 *    submission time on, and a received frame handed in then that has to wait for the bus
 *    only from the time of the call. A caller that reads its actions never meets this case.
 *  - Event budget. One call processes at most EVENTS_PER_CALL events. With pacing off a node
 *    that is never acknowledged retries for ever at one instant; the budget ends the call, and
 *    can_ctrl_next_wakeup() then asks for an immediate poll. At any positive bit rate the budget
 *    is never reached: a late poll moves the next start of this node to `now` instead of
 *    replaying the past, and the received frames it replays are at most CAN_CTRL_WIRE_QUEUE,
 *    two events each.
 *  - can_ctrl_poll() with a NULL action returns true when at least one action is queued.
 *  - can_ctrl_tx_abort_all() removes every waiting frame even if `max_handles` is smaller than
 *    their number; it writes the handles of the first `max_handles` of them, in submission order.
 *  - can_ctrl_wire_rx() counts nothing for an invalid frame: the statistics have no counter for
 *    a malformed input. A fault value outside the enumeration is stored and acts as no fault.
 */

/* What the attempt in flight ends in (tx_outcome). */
#define OUTCOME_SUCCESS  0U
#define OUTCOME_INJECTED 1U
#define OUTCOME_SHORT    2U
#define OUTCOME_NO_ACK   3U

/* A shorted bus is detected on the start-of-frame bit. */
#define SHORT_ERROR_AFTER_BITS 1U
/* The ACK delimiter and the 7 bits of end of frame follow the ACK slot. A missing
 * acknowledgment is detected in the ACK slot, so the error frame replaces these 8 bits; they are
 * also the first 8 of the 11 recessive bits a passing frame contributes to a recovery. */
#define BITS_AFTER_ACK_SLOT 8U

/* Rule 12: the recessive run that counts towards recovery from bus off. */
#define RECESSIVE_RUN_BITS 11U

/* Bus held dominant: a stuff error after 6 dominant bits (rule 1), then the 6-bit error flag and
 * the first bit after it, which is dominant (rule 2, 7 bits after the error). Rule 6 counts the
 * consecutive dominant bits following the flag, that first bit included: the 14th after an
 * active error flag, the 8th after a passive one, so the first step comes 13 or 7 bits after
 * the rule-2 bit, and every 8 bits after that. Whether the flag is active or passive is decided
 * by the node's state when it detected the error. */
#define STUCK_STUFF_ERROR_BITS  6U
#define STUCK_FLAG_BITS         7U
#define STUCK_FIRST_RUN_ACTIVE  13U
#define STUCK_FIRST_RUN_PASSIVE 7U
#define STUCK_RUN_BITS          8U
/* After the dominant level goes away the node waits for 11 recessive bits (bus integration). */
#define STUCK_RELEASE_BITS 11U

/* An event produces at most a state change and one frame action. */
#define ACTIONS_PER_EVENT 2U
/* See "Event budget" above. */
#define EVENTS_PER_CALL 1024U

/* start_ns of a received frame that is still waiting for the bus. Later than any time, so a
 * walk over the wire queue that stops at the first frame not started yet stops at it too. */
#define NOT_STARTED UINT64_MAX

typedef enum {
    EVENT_NONE = 0,
    EVENT_FRAME_END,
    EVENT_RECOVERY,
    EVENT_FAULT,
    EVENT_DOMINANT,
    EVENT_TX_START,
    EVENT_RX_START
} ctrl_event_t;

/* ------------------------------------------------------------------------- time helpers */

/* Every duration is converted as a whole, from the moment it is measured from, so that rounding
 * at bit rates that do not divide 1e9 never accumulates bit by bit. */
static uint64_t bits_ns(const can_ctrl_t *ctrl, uint32_t bits)
{
    return can_bits_to_ns(bits, ctrl->config.bitrate);
}

static uint64_t later_of(uint64_t a, uint64_t b)
{
    uint64_t result = a;

    if (b > a) {
        result = b;
    }

    return result;
}

static uint64_t minus_or_zero(uint64_t a, uint64_t b)
{
    uint64_t result = 0U;

    if (a > b) {
        result = a - b;
    }

    return result;
}

static uint32_t wire_index(const can_ctrl_t *ctrl, uint32_t position)
{
    return (ctrl->wire_head + position) % CAN_CTRL_WIRE_QUEUE;
}

/* ------------------------------------------------------------------------------ actions */

/* Room for the action is guaranteed: advance() takes an event only when ACTIONS_PER_EVENT
 * entries are free, and nothing else queues actions. */
static can_ctrl_action_t *new_action(can_ctrl_t *ctrl, can_ctrl_action_kind_t kind,
                                     uint64_t time_ns)
{
    const uint32_t index = (ctrl->action_head + ctrl->action_count) % CAN_CTRL_ACTION_QUEUE;
    can_ctrl_action_t *const action = &ctrl->actions[index];

    (void)memset(action, 0, sizeof(*action));
    action->kind = kind;
    action->time_ns = time_ns;
    action->state = ctrl->fc.state;
    action->old_state = ctrl->fc.state;
    action->tec = ctrl->fc.tec;
    action->rec = ctrl->fc.rec;
    ctrl->action_count++;

    return action;
}

static void report_state_change(can_ctrl_t *ctrl, can_fc_state_t before, uint64_t time_ns)
{
    if (ctrl->fc.state != before) {
        can_ctrl_action_t *const action = new_action(ctrl, CAN_CTRL_ACTION_STATE_CHANGED, time_ns);

        action->old_state = before;

        if (ctrl->fc.state == CAN_FC_BUS_OFF) {
            ctrl->stats.bus_off_events++;
        }
    }
}

/* ----------------------------------------------------------------------------- recovery */

/* Rule 12 is counted only after a restart was requested, and a bus held dominant has no
 * recessive bits to count. A shorted bus reads recessive and counts like an idle one. */
static bool recovery_counting(const can_ctrl_t *ctrl)
{
    return (ctrl->fc.state == CAN_FC_BUS_OFF) && ctrl->fc.restart_requested &&
           (ctrl->fault_effective != CAN_BUS_FAULT_STUCK_DOMINANT);
}

static uint32_t recovery_missing(const can_ctrl_t *ctrl)
{
    return CAN_FC_RECOVERY_OCCURRENCES - (uint32_t)ctrl->fc.recovery_occurrences;
}

/* End of the n-th occurrence of 11 recessive bits in the run that starts at the mark. */
static uint64_t recovery_occurrence_ns(const can_ctrl_t *ctrl, uint32_t n)
{
    return ctrl->recovery_mark_ns + bits_ns(ctrl, n * RECESSIVE_RUN_BITS);
}

/*
 * How idle time and passing frames are combined into occurrences of 11 recessive bits.
 *
 * recovery_mark_ns is the start of the recessive run being counted. It is set to the time of
 * the restart request, and to the moment a dominant fault ends. The run lasts until the next
 * start of frame on the timeline and gives one occurrence per complete 11 bits: occurrence n
 * ends at mark + 11 n bit times, and one that ends exactly at the start of frame counts. The
 * bits left over before the start of frame are lost, because the start-of-frame bit is dominant.
 *
 * A frame ends with the ACK delimiter and 7 bits of end of frame, all recessive, followed by the
 * 3 recessive bits of intermission: its last 11 bits. So a frame starts a new run at its ACK
 * delimiter (the ACK slot before it is driven dominant by the receivers), and the first
 * occurrence of that run ends with the intermission. A frame passing on the bus therefore
 * contributes exactly one occurrence, its last 11 bits, and the idle time after it continues the
 * same run. (A frame whose ACK delimiter lies before the mark leaves the mark where it is.)
 *
 * This function counts the occurrences of the current run that end at or before `until_ns`,
 * at most as many as the recovery still needs; counting the last one completes the recovery.
 */
static void recovery_count_until(can_ctrl_t *ctrl, uint64_t until_ns)
{
    const uint32_t missing = recovery_missing(ctrl);
    uint32_t counted = 0U;

    while (counted < missing) {
        if (recovery_occurrence_ns(ctrl, counted + 1U) > until_ns) {
            break;
        }

        (void)can_fc_apply(&ctrl->fc, CAN_FC_RECESSIVE_11);
        counted++;
    }
}

static void complete_recovery(can_ctrl_t *ctrl, uint64_t time_ns)
{
    recovery_count_until(ctrl, time_ns);
    report_state_change(ctrl, CAN_FC_BUS_OFF, time_ns);

    /* The 128 occurrences were counted on a bus that was idle up to now. A frame handed in later
     * cannot be placed before this moment without contradicting them. */
    ctrl->bus_free_at_ns = later_of(ctrl->bus_free_at_ns, time_ns);
}

/* --------------------------------------------------------------------------- the timeline */

/* End of the intermission of whatever started before `at_ns`: the attempt in flight (its free
 * time is started_free_ns), a received frame in progress, or a frame that already ended. A
 * frame due to start at `at_ns` itself has not started: an input is applied before the events
 * of its own instant (catch_up), so a fault set then is in force at that frame's start, as it
 * is for a transmission due to start then. The walk stops at the first received frame that
 * has not started; the frames still waiting for the bus come after it and have NOT_STARTED. */
static uint64_t free_after_started(const can_ctrl_t *ctrl, uint64_t at_ns)
{
    uint64_t free_ns = later_of(at_ns, ctrl->started_free_ns);
    uint32_t position = 0U;

    while (position < ctrl->wire_count) {
        const can_ctrl_wire_slot_t *const slot = &ctrl->wire[wire_index(ctrl, position)];

        if (slot->start_ns >= at_ns) {
            break;
        }

        free_ns = later_of(
            free_ns, slot->start_ns + bits_ns(ctrl, (uint32_t)slot->bits + CAN_BITS_INTERMISSION));
        position++;
    }

    return free_ns;
}

/* A shorted bus and a bus held dominant carry no frame: no received frame starts while one of
 * them is in force. */
static bool bus_carries_frames(const can_ctrl_t *ctrl)
{
    return (ctrl->fault_effective != CAN_BUS_FAULT_SHORT) &&
           (ctrl->fault_effective != CAN_BUS_FAULT_STUCK_DOMINANT);
}

/* Index in `wire` of the oldest received frame that waits for the bus. Waiting frames follow
 * the placed ones. The caller makes sure that one waits. */
static uint32_t first_waiting(const can_ctrl_t *ctrl)
{
    return wire_index(ctrl, ctrl->wire_placed);
}

/* Put the oldest waiting received frame on the timeline, starting at `start_ns`. */
static void place_received(can_ctrl_t *ctrl, uint64_t start_ns)
{
    can_ctrl_wire_slot_t *const slot = &ctrl->wire[first_waiting(ctrl)];

    slot->start_ns = start_ns;
    slot->eof_ns = start_ns + bits_ns(ctrl, slot->bits);
    ctrl->wire_placed++;

    ctrl->bus_free_at_ns = start_ns + bits_ns(ctrl, (uint32_t)slot->bits + CAN_BITS_INTERMISSION);
    ctrl->last_frame_was_ours = false;
}

/*
 * A frame from the transport joins the wire queue; the caller made sure there is room. `now_ns`
 * is the time of the call, to which the model has been advanced: every start of frame before it
 * has been decided (unless a full action queue holds the model up, see "Inputs while the action
 * queue is full"). See "Receive" in can_ctrl.h.
 *
 * The frame's earliest start: it ended at its arrival, or one frame length after the bus became
 * free if that is later, and it never ends before the last event already processed, so that
 * what has been reported stays in time order even for an arrival time that lies before the
 * last `now`. If that start lies before `now_ns`, the bus was free then and nothing took it,
 * and no received frame waits that would go first: the frame is placed there. Otherwise it
 * waits and competes from `now_ns` on.
 */
static void hand_in(can_ctrl_t *ctrl, const can_frame_t *frame, uint16_t bits, uint64_t arrival_ns,
                    uint64_t now_ns, bool lost)
{
    const uint64_t frame_ns = bits_ns(ctrl, bits);
    const bool none_waiting = (ctrl->wire_placed == ctrl->wire_count);
    can_ctrl_wire_slot_t *const slot = &ctrl->wire[wire_index(ctrl, ctrl->wire_count)];
    uint64_t start_ns = later_of(minus_or_zero(arrival_ns, frame_ns), ctrl->bus_free_at_ns);

    start_ns = later_of(start_ns, minus_or_zero(ctrl->model_ns, frame_ns));

    slot->frame = *frame;
    slot->start_ns = NOT_STARTED;
    slot->eof_ns = NOT_STARTED;
    slot->ready_ns = now_ns;
    slot->bits = bits;
    slot->lost = lost;
    ctrl->wire_count++;

    if (none_waiting && bus_carries_frames(ctrl) && (start_ns < now_ns)) {
        place_received(ctrl, start_ns);
    }
}

/*
 * Received frames that have not started at `at_ns`, when the bus is shorted or held dominant,
 * never pass: those still waiting for the bus, and any placed frame due to start then or
 * later. A placed frame that started before `at_ns` is on the wire and passes. (When the model
 * runs without being held up, no such frame is left by then: its end of frame comes before the
 * fault. One is left when the fault was set while a full action queue held the model up,
 * before the start of a received frame that had to wait.)
 */
static void discard_queued_frames(can_ctrl_t *ctrl, uint64_t at_ns)
{
    uint32_t kept = 0U;

    while ((kept < ctrl->wire_placed) && (ctrl->wire[wire_index(ctrl, kept)].start_ns < at_ns)) {
        kept++;
    }

    for (uint32_t position = kept; position < ctrl->wire_count; position++) {
        if (!ctrl->wire[wire_index(ctrl, position)].lost) {
            ctrl->stats.rx_lost_fault++; /* a lost frame was counted when it was handed in */
        }
    }

    ctrl->wire_count = kept;
    ctrl->wire_placed = kept;
}

/* ------------------------------------------------------------------------- transmission */

/* The winner of arbitration among the frames submitted by `at_ns`: smallest key, and among
 * equal keys the one submitted first (not the lower mailbox: mailboxes are reused). The caller
 * makes sure that at least one frame was submitted by then. */
static uint32_t arbitration_winner(const can_ctrl_t *ctrl, uint64_t at_ns)
{
    uint32_t winner = CAN_CTRL_TX_MAILBOXES;
    uint32_t winner_key = 0U;
    uint64_t winner_order = 0U;

    for (uint32_t i = 0U; i < CAN_CTRL_TX_MAILBOXES; i++) {
        const can_ctrl_mailbox_t *const mailbox = &ctrl->mailboxes[i];

        if (mailbox->used && (mailbox->submit_ns <= at_ns)) {
            const uint32_t key = can_frame_arbitration_key(&mailbox->frame);
            const bool first = (winner == CAN_CTRL_TX_MAILBOXES);

            if (first || (key < winner_key) ||
                ((key == winner_key) && (mailbox->order < winner_order))) {
                winner = i;
                winner_key = key;
                winner_order = mailbox->order;
            }
        }
    }

    return winner;
}

/* When the next transmission could start, if one is pending and the node may transmit. */
static bool transmission_start(const can_ctrl_t *ctrl, uint64_t *start_ns)
{
    bool pending = false;

    if (!ctrl->tx_in_flight && (ctrl->fc.state != CAN_FC_BUS_OFF) &&
        (ctrl->fault_effective != CAN_BUS_FAULT_STUCK_DOMINANT)) {
        uint64_t earliest_submit = UINT64_MAX;

        for (uint32_t i = 0U; i < CAN_CTRL_TX_MAILBOXES; i++) {
            if (ctrl->mailboxes[i].used && (ctrl->mailboxes[i].submit_ns < earliest_submit)) {
                pending = true;
                earliest_submit = ctrl->mailboxes[i].submit_ns;
            }
        }

        if (pending) {
            /* Suspend transmission: an error-passive node that sent the last frame on the bus
             * waits 8 more bit times; another node may start in that time, and the received
             * frame it sends is then the last one. */
            uint64_t free_ns = ctrl->bus_free_at_ns;

            if (ctrl->last_frame_was_ours && (ctrl->fc.state == CAN_FC_ERROR_PASSIVE)) {
                free_ns = ctrl->own_free_at_ns;
            }

            /* Never before the last event processed, so that model time cannot go back. Every
             * event that lets a waiting frame go (end of an attempt, recovery, end of a fault)
             * also moves the free time to its own time or later, so today this bound never
             * binds; it keeps the invariant from depending on that. */
            *start_ns = later_of(later_of(free_ns, ctrl->model_ns), earliest_submit);
        }
    }

    return pending;
}

/*
 * When the oldest waiting received frame could start, if one waits and the bus can carry it:
 * when the bus is free, and not before it was handed in.
 *
 * Three of the conditions do not bind today; each is stated so that the invariant it keeps
 * does not depend on the reasoning that shows it. The attempt in flight: the free time lies
 * after it. The last event processed, so that model time cannot go back: as in
 * transmission_start(). A short or a dominant bus in force: a frame can wait while one is in
 * force only if it was handed in after the removal was asked for (the others were refused or
 * discarded), and the removal takes effect once what was on the bus at that moment has passed;
 * the frame cannot start earlier, and at the same instant the fault is taken first.
 */
static bool received_start(const can_ctrl_t *ctrl, uint64_t *start_ns)
{
    const bool possible =
        (ctrl->wire_count > ctrl->wire_placed) && !ctrl->tx_in_flight && bus_carries_frames(ctrl);

    if (possible) {
        const can_ctrl_wire_slot_t *const slot = &ctrl->wire[first_waiting(ctrl)];

        *start_ns = later_of(later_of(ctrl->bus_free_at_ns, ctrl->model_ns), slot->ready_ns);
    }

    return possible;
}

/*
 * The next start of frame and who wins it: EVENT_TX_START for this node's best mailbox,
 * EVENT_RX_START for the oldest waiting received frame, EVENT_NONE if nothing can start. A
 * frame that can start earlier than the other goes alone; two that can start at the same
 * instant arbitrate: the smaller key wins, and of equal keys the received frame.
 */
static ctrl_event_t next_start(const can_ctrl_t *ctrl, uint64_t *start_ns)
{
    uint64_t tx_ns = 0U;
    uint64_t rx_ns = 0U;
    const bool tx = transmission_start(ctrl, &tx_ns);
    const bool rx = received_start(ctrl, &rx_ns);
    ctrl_event_t event = EVENT_NONE;

    if (tx && rx && (tx_ns == rx_ns)) {
        const uint32_t ours =
            can_frame_arbitration_key(&ctrl->mailboxes[arbitration_winner(ctrl, tx_ns)].frame);
        const uint32_t theirs = can_frame_arbitration_key(&ctrl->wire[first_waiting(ctrl)].frame);

        event = (ours < theirs) ? EVENT_TX_START : EVENT_RX_START;
        *start_ns = tx_ns;
    } else if (tx && (!rx || (tx_ns < rx_ns))) {
        event = EVENT_TX_START;
        *start_ns = tx_ns;
    } else if (rx) {
        event = EVENT_RX_START;
        *start_ns = rx_ns;
    } else {
        /* nothing waits, or nothing may start */
    }

    return event;
}

/* The outcome of an attempt is fixed by the fault in force at its start of frame. Bus time of
 * each case before the intermission, as listed in the contract. */
static void start_transmission(can_ctrl_t *ctrl, uint64_t could_start_ns, uint64_t now_ns)
{
    uint64_t start_ns = could_start_ns;
    uint32_t winner = arbitration_winner(ctrl, start_ns);
    const uint64_t frame_ns = bits_ns(ctrl, ctrl->mailboxes[winner].bits);

    /* Late callers: started more than one frame length ago means the caller was not there to
     * see it. Starting it now keeps the frames that follow at the bit rate in real time. */
    if (minus_or_zero(now_ns, start_ns) > frame_ns) {
        start_ns = now_ns;
        ctrl->stats.late_resyncs++;
        winner = arbitration_winner(ctrl, start_ns);
    }

    const uint32_t length = ctrl->mailboxes[winner].bits;
    uint32_t bits = length;

    if (ctrl->fault_effective == CAN_BUS_FAULT_SHORT) {
        ctrl->tx_outcome = OUTCOME_SHORT;
        bits = SHORT_ERROR_AFTER_BITS + CAN_CTRL_ERROR_FRAME_BITS;
    } else if (ctrl->inject_tx_errors > 0U) {
        ctrl->inject_tx_errors--;
        ctrl->tx_outcome = OUTCOME_INJECTED;
        bits = (length / 2U) + CAN_CTRL_ERROR_FRAME_BITS;
    } else if (ctrl->fault_effective == CAN_BUS_FAULT_NO_ACK) {
        ctrl->tx_outcome = OUTCOME_NO_ACK;
        bits = (length - BITS_AFTER_ACK_SLOT) + CAN_CTRL_ERROR_FRAME_BITS;
    } else {
        ctrl->tx_outcome = OUTCOME_SUCCESS;
    }

    ctrl->model_ns = start_ns;
    ctrl->tx_in_flight = true;
    ctrl->tx_mailbox = (uint8_t)winner;
    ctrl->tx_bits = (uint16_t)bits;
    ctrl->tx_start_ns = start_ns;
    ctrl->tx_end_ns = start_ns + bits_ns(ctrl, bits);
    ctrl->bus_free_at_ns = start_ns + bits_ns(ctrl, bits + CAN_BITS_INTERMISSION);
    ctrl->own_free_at_ns =
        start_ns + bits_ns(ctrl, bits + CAN_BITS_INTERMISSION + CAN_CTRL_SUSPEND_BITS);
    ctrl->started_free_ns = ctrl->bus_free_at_ns;
    ctrl->last_frame_was_ours = true;
}

/* The can_fc events of a destroyed attempt. `before` is the state in which the error was
 * signalled: rule 9 ends by saying that the error which makes a node error passive is still
 * signalled with an active flag, so the bit error in that flag (rule 4) is decided by the state
 * before CAN_FC_TX_ERROR is applied, not after. */
static void count_attempt_error(can_ctrl_t *ctrl, can_fc_state_t before)
{
    if (ctrl->tx_outcome == OUTCOME_NO_ACK) {
        (void)can_fc_apply(&ctrl->fc, CAN_FC_TX_ACK_ERROR_ALONE);
    } else {
        (void)can_fc_apply(&ctrl->fc, CAN_FC_TX_ERROR);

        /* On a shorted bus the dominant bits of an active error flag read back recessive. */
        if ((ctrl->tx_outcome == OUTCOME_SHORT) && (before == CAN_FC_ERROR_ACTIVE)) {
            (void)can_fc_apply(&ctrl->fc, CAN_FC_TX_BIT_ERROR_IN_FLAG);
        }
    }
}

static void end_transmission(can_ctrl_t *ctrl)
{
    can_ctrl_mailbox_t *const mailbox = &ctrl->mailboxes[ctrl->tx_mailbox];
    const can_fc_state_t before = ctrl->fc.state;
    const uint64_t end_ns = ctrl->tx_end_ns;

    ctrl->tx_in_flight = false;
    ctrl->stats.busy_ns += bits_ns(ctrl, ctrl->tx_bits);

    if (ctrl->tx_outcome == OUTCOME_SUCCESS) {
        (void)can_fc_apply(&ctrl->fc, CAN_FC_TX_SUCCESS);
        ctrl->stats.tx_frames++;
        report_state_change(ctrl, before, end_ns);

        can_ctrl_action_t *const action = new_action(ctrl, CAN_CTRL_ACTION_TX_COMPLETE, end_ns);

        action->start_ns = ctrl->tx_start_ns;
        action->submit_ns = mailbox->submit_ns;
        action->frame = mailbox->frame;
        action->handle = mailbox->handle;
        mailbox->used = false;
    } else {
        /* The frame stays in its mailbox and competes again, unless it was aborted while on the
         * wire: then it goes now, after the error counting of this attempt. */
        count_attempt_error(ctrl, before);
        ctrl->stats.tx_attempts_failed++;
        report_state_change(ctrl, before, end_ns);

        if (ctrl->tx_abort_marked) {
            can_ctrl_action_t *const action = new_action(ctrl, CAN_CTRL_ACTION_TX_ABORTED, end_ns);

            action->frame = mailbox->frame;
            action->handle = mailbox->handle;
            mailbox->used = false;
            ctrl->stats.tx_aborted++;
        }
    }

    ctrl->tx_abort_marked = false;
}

/* ---------------------------------------------------------------------------- reception */

static void end_reception(can_ctrl_t *ctrl)
{
    const can_ctrl_wire_slot_t slot = ctrl->wire[ctrl->wire_head];
    const can_fc_state_t before = ctrl->fc.state;

    ctrl->wire_head = wire_index(ctrl, 1U);
    ctrl->wire_count--;
    ctrl->wire_placed--;
    ctrl->stats.busy_ns += bits_ns(ctrl, slot.bits);
    ctrl->started_free_ns =
        later_of(ctrl->started_free_ns,
                 slot.start_ns + bits_ns(ctrl, (uint32_t)slot.bits + CAN_BITS_INTERMISSION));

    if (recovery_counting(ctrl)) {
        /* The run counted so far ends at this frame's start of frame; the next one starts at
         * its ACK delimiter (see recovery_count_until). */
        const uint64_t ack_delimiter_ns =
            slot.start_ns + bits_ns(ctrl, (uint32_t)slot.bits - BITS_AFTER_ACK_SLOT);

        recovery_count_until(ctrl, slot.start_ns);
        ctrl->recovery_mark_ns = later_of(ctrl->recovery_mark_ns, ack_delimiter_ns);
    }

    if (!slot.lost) {
        if (ctrl->fc.state == CAN_FC_BUS_OFF) {
            ctrl->stats.rx_lost_fault++;
        } else if (ctrl->inject_rx_errors > 0U) {
            ctrl->inject_rx_errors--;
            (void)can_fc_apply(&ctrl->fc, CAN_FC_RX_ERROR);
            ctrl->stats.rx_errors++;
            report_state_change(ctrl, before, slot.eof_ns);
        } else {
            (void)can_fc_apply(&ctrl->fc, CAN_FC_RX_SUCCESS);
            ctrl->stats.rx_frames++;
            report_state_change(ctrl, before, slot.eof_ns);

            can_ctrl_action_t *const action =
                new_action(ctrl, CAN_CTRL_ACTION_RX_DELIVER, slot.eof_ns);

            action->start_ns = slot.start_ns;
            action->frame = slot.frame;
        }
    }
}

/* ------------------------------------------------------------------------------- faults */

static bool dominant_step_pending(const can_ctrl_t *ctrl)
{
    return (ctrl->fault_effective == CAN_BUS_FAULT_STUCK_DOMINANT) &&
           (ctrl->fc.state != CAN_FC_BUS_OFF) && (ctrl->fc.rec < CAN_FC_REC_MAX);
}

static void dominant_step(can_ctrl_t *ctrl, uint64_t time_ns)
{
    const can_fc_state_t before = ctrl->fc.state;

    if (ctrl->dominant_bits == STUCK_STUFF_ERROR_BITS) {
        /* The flag this error makes the node send is active or passive by its state now. */
        ctrl->dominant_first_run_bits = STUCK_FIRST_RUN_PASSIVE;

        if (before == CAN_FC_ERROR_ACTIVE) {
            ctrl->dominant_first_run_bits = STUCK_FIRST_RUN_ACTIVE;
        }

        (void)can_fc_apply(&ctrl->fc, CAN_FC_RX_ERROR); /* rule 1 */
        ctrl->dominant_bits += STUCK_FLAG_BITS;
    } else if (ctrl->dominant_bits == (STUCK_STUFF_ERROR_BITS + STUCK_FLAG_BITS)) {
        (void)can_fc_apply(&ctrl->fc, CAN_FC_RX_DOMINANT_AFTER_FLAG); /* rule 2 */
        ctrl->dominant_bits += ctrl->dominant_first_run_bits;
    } else {
        (void)can_fc_apply(&ctrl->fc, CAN_FC_RX_DOMINANT_RUN); /* rule 6 */
        ctrl->dominant_bits += STUCK_RUN_BITS;
    }

    report_state_change(ctrl, before, time_ns);
}

static void fault_takes_effect(can_ctrl_t *ctrl)
{
    const uint64_t at_ns = ctrl->fault_since_ns;
    const can_bus_fault_t ending = ctrl->fault_effective;
    const can_bus_fault_t beginning = ctrl->fault;

    if (ending == CAN_BUS_FAULT_STUCK_DOMINANT) {
        /* Recessive from now on: the next run towards recovery starts here, and the bus is
         * idle once 11 recessive bits have been seen. */
        ctrl->bus_free_at_ns =
            later_of(ctrl->bus_free_at_ns, at_ns + bits_ns(ctrl, STUCK_RELEASE_BITS));
        ctrl->recovery_mark_ns = at_ns;
    } else if (ending == CAN_BUS_FAULT_SHORT) {
        /* Nothing passed while the bus was shorted. */
        ctrl->bus_free_at_ns = later_of(ctrl->bus_free_at_ns, at_ns);
    } else {
        /* No fault or no acknowledgment: the bus itself was usable. */
    }

    if (beginning == CAN_BUS_FAULT_STUCK_DOMINANT) {
        /* The recessive run being counted ends here (fault_effective is still the old one). */
        if (recovery_counting(ctrl)) {
            recovery_count_until(ctrl, at_ns);
        }

        ctrl->dominant_base_ns = at_ns;
        ctrl->dominant_bits = STUCK_STUFF_ERROR_BITS;
        ctrl->last_frame_was_ours = false;
        discard_queued_frames(ctrl, at_ns);
    } else if (beginning == CAN_BUS_FAULT_SHORT) {
        discard_queued_frames(ctrl, at_ns);
    } else {
        /* Received frames already queued pass as planned. */
    }

    ctrl->fault_effective = beginning;
}

/* -------------------------------------------------------------------------- the event loop */

static void consider(ctrl_event_t *event, uint64_t *when_ns, ctrl_event_t candidate, uint64_t at_ns)
{
    /* Strictly earlier: candidates are offered in the order that settles ties. */
    if ((*event == EVENT_NONE) || (at_ns < *when_ns)) {
        *event = candidate;
        *when_ns = at_ns;
    }
}

static ctrl_event_t next_event(const can_ctrl_t *ctrl, uint64_t *when_ns)
{
    ctrl_event_t event = EVENT_NONE;
    uint64_t when = 0U;
    uint64_t start_ns = 0U;
    ctrl_event_t start = EVENT_NONE;

    if (ctrl->tx_in_flight) {
        consider(&event, &when, EVENT_FRAME_END, ctrl->tx_end_ns);
    } else if (ctrl->wire_placed > 0U) {
        consider(&event, &when, EVENT_FRAME_END, ctrl->wire[ctrl->wire_head].eof_ns);
    } else {
        /* the bus carries nothing */
    }

    if (recovery_counting(ctrl)) {
        const uint64_t done_ns = recovery_occurrence_ns(ctrl, recovery_missing(ctrl));

        /* Only if the run is not cut short by the received frame on the timeline, which has
         * started; if it is, that frame's end counts the run and starts the next one. A frame
         * that has yet to start needs no test here: if it starts before done_ns, its start is
         * the earlier event, and once it has started it is the frame on the timeline. */
        if ((ctrl->wire_placed == 0U) || (done_ns <= ctrl->wire[ctrl->wire_head].start_ns)) {
            consider(&event, &when, EVENT_RECOVERY, done_ns);
        }
    }

    if (ctrl->fault != ctrl->fault_effective) {
        consider(&event, &when, EVENT_FAULT, ctrl->fault_since_ns);
    }

    if (dominant_step_pending(ctrl)) {
        consider(&event, &when, EVENT_DOMINANT,
                 ctrl->dominant_base_ns + bits_ns(ctrl, ctrl->dominant_bits));
    }

    start = next_start(ctrl, &start_ns);

    if (start != EVENT_NONE) {
        consider(&event, &when, start, start_ns);
    }

    *when_ns = when;
    return event;
}

/* Process the events due at or before `now_ns`, or only those before it (see catch_up). */
static void advance(can_ctrl_t *ctrl, uint64_t now_ns, bool including_now)
{
    uint32_t budget = EVENTS_PER_CALL;
    bool running = true;

    while (running) {
        uint64_t when = 0U;
        const ctrl_event_t event = next_event(ctrl, &when);
        const uint32_t room = CAN_CTRL_ACTION_QUEUE - ctrl->action_count;
        const bool due = (when < now_ns) || (including_now && (when == now_ns));

        if ((event == EVENT_NONE) || !due || (room < ACTIONS_PER_EVENT) || (budget == 0U)) {
            running = false;
        } else {
            budget--;
            ctrl->model_ns = when;

            if (event == EVENT_FRAME_END) {
                if (ctrl->tx_in_flight) {
                    end_transmission(ctrl);
                } else {
                    end_reception(ctrl);
                }
            } else if (event == EVENT_RECOVERY) {
                complete_recovery(ctrl, when);
            } else if (event == EVENT_FAULT) {
                fault_takes_effect(ctrl);
            } else if (event == EVENT_DOMINANT) {
                dominant_step(ctrl, when);
            } else if (event == EVENT_TX_START) {
                start_transmission(ctrl, when, now_ns);
            } else {
                place_received(ctrl, when); /* EVENT_RX_START */
            }
        }
    }
}

/*
 * Every call that carries a time goes through here first. A time earlier than one already seen
 * is taken as the time already seen: the model's time never goes back.
 *
 * A poll processes everything due up to and including its time. An input processes only what
 * lies strictly before its time, and is applied before the events of that very instant: frames
 * submitted at the same instant then arbitrate together ("among those already submitted at that
 * moment"), a fault set at the instant a frame could start is in force at its start of frame,
 * and a frame handed in at the instant a transmission could start goes first if it had the bus
 * before that instant (hand_in) and arbitrates against the transmission otherwise.
 */
static uint64_t catch_up(can_ctrl_t *ctrl, uint64_t now_ns, bool including_now)
{
    const uint64_t now = later_of(now_ns, ctrl->last_now_ns);

    ctrl->last_now_ns = now;
    advance(ctrl, now, including_now);

    return now;
}

/* ------------------------------------------------------------------------ public functions */

void can_ctrl_init(can_ctrl_t *ctrl, const can_ctrl_config_t *config, uint64_t now_ns)
{
    (void)memset(ctrl, 0, sizeof(*ctrl));
    ctrl->config = *config;
    can_fc_init(&ctrl->fc);
    ctrl->fault = CAN_BUS_FAULT_NONE;
    ctrl->fault_effective = CAN_BUS_FAULT_NONE;
    ctrl->fault_since_ns = now_ns;
    ctrl->last_now_ns = now_ns;
    ctrl->model_ns = now_ns;
    ctrl->bus_free_at_ns = now_ns;
    ctrl->own_free_at_ns = now_ns;
    ctrl->started_free_ns = now_ns;
    ctrl->recovery_mark_ns = now_ns;
}

bool can_ctrl_tx_submit(can_ctrl_t *ctrl, const can_frame_t *frame, uint32_t handle,
                        uint64_t now_ns)
{
    can_bits_info_t info = {0U, 0U, 0U};
    bool accepted = false;
    const uint64_t now = catch_up(ctrl, now_ns, false);

    if (can_bits_exact(frame, &info)) {
        uint32_t free_box = 0U;

        while ((free_box < CAN_CTRL_TX_MAILBOXES) && ctrl->mailboxes[free_box].used) {
            free_box++;
        }

        if (free_box < CAN_CTRL_TX_MAILBOXES) {
            can_ctrl_mailbox_t *const mailbox = &ctrl->mailboxes[free_box];

            mailbox->used = true;
            mailbox->frame = *frame;
            mailbox->handle = handle;
            mailbox->bits = info.frame_bits;
            mailbox->submit_ns = now;
            mailbox->order = ctrl->next_order;
            ctrl->next_order++;
            accepted = true;
        }
    }

    return accepted;
}

uint32_t can_ctrl_tx_pending(const can_ctrl_t *ctrl)
{
    uint32_t pending = 0U;

    for (uint32_t i = 0U; i < CAN_CTRL_TX_MAILBOXES; i++) {
        if (ctrl->mailboxes[i].used) {
            pending++;
        }
    }

    return pending;
}

/* The waiting mailbox submitted first, or CAN_CTRL_TX_MAILBOXES if none waits. */
static uint32_t oldest_waiting(const can_ctrl_t *ctrl)
{
    uint32_t oldest = CAN_CTRL_TX_MAILBOXES;

    for (uint32_t i = 0U; i < CAN_CTRL_TX_MAILBOXES; i++) {
        const can_ctrl_mailbox_t *const mailbox = &ctrl->mailboxes[i];
        const bool on_wire = ctrl->tx_in_flight && (i == (uint32_t)ctrl->tx_mailbox);

        if (mailbox->used && !on_wire) {
            if ((oldest == CAN_CTRL_TX_MAILBOXES) ||
                (mailbox->order < ctrl->mailboxes[oldest].order)) {
                oldest = i;
            }
        }
    }

    return oldest;
}

uint32_t can_ctrl_tx_abort_all(can_ctrl_t *ctrl, uint32_t *handles, uint32_t max_handles)
{
    uint32_t removed = 0U;
    uint32_t oldest = oldest_waiting(ctrl);

    while (oldest < CAN_CTRL_TX_MAILBOXES) {
        if ((handles != NULL) && (removed < max_handles)) {
            handles[removed] = ctrl->mailboxes[oldest].handle;
        }

        ctrl->mailboxes[oldest].used = false;
        removed++;
        oldest = oldest_waiting(ctrl);
    }

    if (ctrl->tx_in_flight) {
        ctrl->tx_abort_marked = true;
    }

    ctrl->stats.tx_aborted += removed;
    return removed;
}

bool can_ctrl_wire_rx(can_ctrl_t *ctrl, const can_frame_t *frame, uint64_t arrival_ns)
{
    can_bits_info_t info = {0U, 0U, 0U};
    bool accepted = false;
    const uint64_t now = catch_up(ctrl, arrival_ns, false);

    if (can_bits_exact(frame, &info)) {
        if ((ctrl->fault == CAN_BUS_FAULT_SHORT) || (ctrl->fault == CAN_BUS_FAULT_STUCK_DOMINANT)) {
            ctrl->stats.rx_lost_fault++;
        } else if (ctrl->fc.state == CAN_FC_BUS_OFF) {
            ctrl->stats.rx_lost_fault++;

            /* Refused, but it still passes on the bus. Beyond the queue it is not modelled. */
            if (ctrl->wire_count < CAN_CTRL_WIRE_QUEUE) {
                hand_in(ctrl, frame, info.frame_bits, arrival_ns, now, true);
            }
        } else if (ctrl->wire_count >= CAN_CTRL_WIRE_QUEUE) {
            ctrl->stats.wire_overruns++;
        } else {
            hand_in(ctrl, frame, info.frame_bits, arrival_ns, now, false);
            accepted = true;
        }
    }

    return accepted;
}

bool can_ctrl_poll(can_ctrl_t *ctrl, uint64_t now_ns, can_ctrl_action_t *action)
{
    (void)catch_up(ctrl, now_ns, true);

    const bool due = (ctrl->action_count > 0U);

    if (due && (action != NULL)) {
        *action = ctrl->actions[ctrl->action_head];
        ctrl->action_head = (ctrl->action_head + 1U) % CAN_CTRL_ACTION_QUEUE;
        ctrl->action_count--;
    }

    return due;
}

uint64_t can_ctrl_next_wakeup(const can_ctrl_t *ctrl)
{
    uint64_t wake_ns = CAN_CTRL_NO_WAKEUP;
    uint64_t when = 0U;

    if (ctrl->action_count > 0U) {
        wake_ns = ctrl->last_now_ns; /* something is due already */
    } else {
        const ctrl_event_t event = next_event(ctrl, &when);

        if (event == EVENT_RX_START) {
            /* The start of a received frame reports nothing, and no late-caller rule depends
             * on it: the next thing to do is its end of frame. Nothing else comes in between.
             * A start of this node's frame waits for the intermission after it; a recovery
             * waits for its end (next_event); a bus held dominant, the only source of counter
             * steps, lets no received frame start; and a fault change takes effect once what
             * was on the bus when it was asked for has passed, so no later than this start,
             * and at the same instant the fault is taken first. */
            when += bits_ns(ctrl, ctrl->wire[first_waiting(ctrl)].bits);
        }

        if (event != EVENT_NONE) {
            /* Not earlier than the last `now`: an event still due then (the event budget was
             * used up) is due at once. */
            wake_ns = later_of(when, ctrl->last_now_ns);
        }
    }

    return wake_ns;
}

void can_ctrl_set_fault(can_ctrl_t *ctrl, can_bus_fault_t fault, uint64_t now_ns)
{
    const uint64_t now = catch_up(ctrl, now_ns, false);

    if (fault != ctrl->fault) {
        ctrl->fault = fault;
        ctrl->fault_since_ns = free_after_started(ctrl, now);
    }
}

can_bus_fault_t can_ctrl_fault(const can_ctrl_t *ctrl)
{
    return ctrl->fault;
}

void can_ctrl_inject_tx_errors(can_ctrl_t *ctrl, uint32_t count)
{
    if (count > (UINT32_MAX - ctrl->inject_tx_errors)) {
        ctrl->inject_tx_errors = UINT32_MAX;
    } else {
        ctrl->inject_tx_errors += count;
    }
}

void can_ctrl_inject_rx_errors(can_ctrl_t *ctrl, uint32_t count)
{
    if (count > (UINT32_MAX - ctrl->inject_rx_errors)) {
        ctrl->inject_rx_errors = UINT32_MAX;
    } else {
        ctrl->inject_rx_errors += count;
    }
}

void can_ctrl_request_restart(can_ctrl_t *ctrl, uint64_t now_ns)
{
    const uint64_t now = catch_up(ctrl, now_ns, false);

    /* A repeated request changes nothing (can_fc decides the same): the count is not started
     * again. While the bus is held dominant the mark is set again when the fault ends. */
    if ((ctrl->fc.state == CAN_FC_BUS_OFF) && !ctrl->fc.restart_requested) {
        can_fc_request_restart(&ctrl->fc);
        ctrl->recovery_mark_ns = now;
    }
}

can_fc_state_t can_ctrl_state(const can_ctrl_t *ctrl)
{
    return ctrl->fc.state;
}

uint16_t can_ctrl_tec(const can_ctrl_t *ctrl)
{
    return ctrl->fc.tec;
}

uint16_t can_ctrl_rec(const can_ctrl_t *ctrl)
{
    return ctrl->fc.rec;
}

const can_ctrl_stats_t *can_ctrl_stats(const can_ctrl_t *ctrl)
{
    return &ctrl->stats;
}
