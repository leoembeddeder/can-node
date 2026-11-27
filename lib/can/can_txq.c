/* SPDX-License-Identifier: MIT */
#include "can/can_txq.h"

#include <stddef.h>
#include <string.h>

/*
 * What the queue does is described in can_txq.h. This file adds how, and the reasons for the
 * points at which the contract needed a decision; each is argued where it is implemented:
 *
 *   - when exactly a timeout is over                                    has_elapsed()
 *   - what a handle is made of                                          HANDLE_SERIALS
 *   - which of two frames with the same arbitration key goes first      event_goes_first()
 *   - only one event frame is in a mailbox at a time                    event_goes_first()
 *   - expired event frames do not keep a new one out                    can_txq_enqueue()
 *   - an event frame that comes back finds room in a full FIFO          can_txq_on_tx_aborted()
 *   - the hold-off is doubled when the node leaves bus off              can_txq_on_state()
 *   - a recall made unnecessary by a confirmation is withdrawn          recall_due()
 *   - one request covers every stale frame there is at the time         recall_due()
 *   - an event frame is called back once, and not while bus off         event_recall_armed()
 *   - a note made before a bus-off does not survive it                  can_txq_on_state()
 *   - the deadline of a pending recall of a cyclic value is 0           can_txq_next_deadline()
 *
 * What holds for a can_txq_t between any two calls, from can_txq_init() on:
 *
 *   - A frame that was accepted and whose fate is still open is in exactly one place: the
 *     waiting value of its cyclic slot, an entry of the event FIFO, or a mailbox record.
 *     stats.pending is the number of these frames. Every step that moves a frame or ends its
 *     life adjusts the counters in the same call, so
 *         produced = sent + superseded + dropped + expired + pending
 *     holds after every call, not only once the queue has drained. The counters of a slot
 *     (slot_stats) move in the same calls, so a slot's own identity holds after every call
 *     too: its pending is 1 for a waiting value plus 1 for a value in a mailbox.
 *   - At most config.mailboxes mailbox records are in use: at most one per cyclic slot, which
 *     slot.in_mailbox mirrors, and at most one for an event frame, which event_in_mailbox
 *     mirrors and event_record points to.
 *   - recall_pending is false while bus_off. event_recalled and event_record mean something
 *     only while event_in_mailbox.
 *   - The FIFO holds at most CAN_TXQ_EVENT_DEPTH frames that came in through
 *     can_txq_enqueue(), and one more only if that one came back from a mailbox.
 *   - restart_pending implies bus_off, holdoff_reset_pending implies the opposite, and
 *     stats.holdoff_ms lies between the configured initial and maximum values.
 */

#define NS_PER_MS 1000000ULL

/*
 * A handle is `serial * CAN_TXQ_MAX_MAILBOXES + index`: the index of the mailbox record that
 * notes the frame, and a serial number that counts hand-outs.
 *
 * The index is what guarantees that a handle is not reused while it is outstanding: two
 * outstanding frames are noted in two records, so their handles differ whatever the serial
 * does. The serial tells successive users of one record apart, so that a handle presented a
 * second time (a driver that reports a frame both done and aborted, say) is refused instead of
 * completing the frame that has the record now. The serial starts again after HANDLE_SERIALS
 * hand-outs, a bound chosen so that the largest handle still fits 32 bits: the product never
 * wraps, so `handle % CAN_TXQ_MAX_MAILBOXES` is the index for any number of mailboxes and not
 * only for a power of two. Only a handle kept across that many hand-outs, half a billion,
 * could be mistaken for a current one.
 */
#define HANDLE_SERIALS (UINT32_MAX / CAN_TXQ_MAX_MAILBOXES)

/*
 * True once `delay_ms` has passed since `since_ns`, the moment it is over included: a timeout
 * of 50 ms that started at t is over at t + 50 ms, not one nanosecond earlier and not one
 * later. The three timers of the queue (event timeout, hold-off, stable time) are all measured
 * this way, so that the time can_txq_next_deadline() names is the time at which
 * can_txq_poll() acts.
 *
 * It is written as a difference so that no sum can overflow, and it is false while `now_ns`
 * is before `since_ns`. That happens without any fault when two tasks read the clock and then
 * enter their critical sections in the other order; a frame must not expire, nor a restart be
 * requested early, because of it.
 */
static bool has_elapsed(uint64_t now_ns, uint64_t since_ns, uint32_t delay_ms)
{
    return (now_ns >= since_ns) && ((now_ns - since_ns) >= ((uint64_t)delay_ms * NS_PER_MS));
}

/*
 * The first moment at which has_elapsed() is true for the same arguments. A moment beyond
 * the range of the clock is never reached, which is what CAN_TXQ_NO_DEADLINE says.
 */
static uint64_t time_after(uint64_t since_ns, uint32_t delay_ms)
{
    const uint64_t delay_ns = (uint64_t)delay_ms * NS_PER_MS;
    uint64_t at_ns = CAN_TXQ_NO_DEADLINE;

    if (delay_ns <= (UINT64_MAX - since_ns)) {
        at_ns = since_ns + delay_ns;
    }

    return at_ns;
}

/* Entries of the ring that holds the event FIFO: one more than the FIFO admits, see
 * can_txq_on_tx_aborted(). Taken from the array itself so that the two cannot disagree. */
static uint32_t event_ring_size(const can_txq_t *queue)
{
    return (uint32_t)(sizeof(queue->events) / sizeof(queue->events[0]));
}

/* Index of the ring entry `offset` places behind the oldest waiting event frame. */
static uint32_t event_index(const can_txq_t *queue, uint32_t offset)
{
    return (queue->event_head + offset) % event_ring_size(queue);
}

/*
 * Discard the event frames that have waited for the timeout.
 *
 * Only the oldest frame is looked at, again and again until one is found that is still in
 * time. That is enough because the FIFO is in order of enqueue time: frames are stamped with
 * the caller's clock as they come in, and a frame that returns from a mailbox returns to the
 * head, where it came from. Should a caller's time ever step back, a frame further down may
 * be older than the head; it is then discarded when it reaches the head, which is still
 * before it could be handed out.
 */
static void expire_events(can_txq_t *queue, uint64_t now_ns)
{
    /* A timeout of 0 switches expiry off; it does not mean "expires at once". */
    if (queue->config.event_timeout_ms != 0U) {
        while ((queue->event_count > 0U) &&
               has_elapsed(now_ns, queue->events[queue->event_head].stamp_ns,
                           queue->config.event_timeout_ms)) {
            queue->event_head = event_index(queue, 1U);
            queue->event_count--;
            queue->stats.expired++;
            queue->stats.pending--;
        }
    }
}

/* The hold-off for the bus-off that follows one which was served with `holdoff_ms`. */
static uint32_t doubled_holdoff(const can_txq_config_t *config, uint32_t holdoff_ms)
{
    uint32_t next_ms = config->holdoff_max_ms;

    /* Compared with half the maximum, so that the doubling itself cannot overflow. An
     * initial hold-off of 0 stays 0: a configuration that starts there asks for immediate
     * recovery, and doubling has nothing to grow from. */
    if (holdoff_ms <= (config->holdoff_max_ms / 2U)) {
        next_ms = holdoff_ms * 2U;
    }

    return next_ms;
}

/* The hold-off returns to its initial value once the node has been back on the bus for the
 * stable time. */
static void reset_holdoff_when_stable(can_txq_t *queue, uint64_t now_ns)
{
    if (queue->holdoff_reset_pending &&
        has_elapsed(now_ns, queue->recovered_at_ns, queue->config.stable_ms)) {
        queue->holdoff_reset_pending = false;
        queue->stats.holdoff_ms = queue->config.holdoff_initial_ms;
    }
}

/* Index of a free mailbox record among those the configuration allows to be used, or
 * CAN_TXQ_MAX_MAILBOXES if the configured number of mailboxes is in use. */
static uint32_t free_record(const can_txq_t *queue)
{
    uint32_t index = CAN_TXQ_MAX_MAILBOXES;
    bool found = false;

    for (uint32_t i = 0U; (i < queue->config.mailboxes) && !found; i++) {
        if (!queue->mailboxes[i].used) {
            index = i;
            found = true;
        }
    }

    return index;
}

/* The record of the frame that was handed out with `handle`, or NULL if no outstanding frame
 * has this handle: it was never given out, or the frame is already done or aborted. */
static can_txq_mailbox_t *find_record(can_txq_t *queue, uint32_t handle)
{
    can_txq_mailbox_t *record = &queue->mailboxes[handle % CAN_TXQ_MAX_MAILBOXES];

    if (!record->used || (record->handle != handle)) {
        record = NULL;
    }

    return record;
}

/*
 * The cyclic slot whose waiting value ranks best in arbitration, with its key in `*best_key`,
 * or CAN_TXQ_CYCLIC_SLOTS if no slot has a value that may be loaded now.
 */
static uint32_t best_slot(const can_txq_t *queue, uint32_t *best_key)
{
    uint32_t best = CAN_TXQ_CYCLIC_SLOTS;

    for (uint32_t i = 0U; i < CAN_TXQ_CYCLIC_SLOTS; i++) {
        const can_txq_slot_t *slot = &queue->slots[i];

        /* A slot whose earlier value is still in a mailbox has to wait for it: with two
         * values of one message in two mailboxes, which of them goes out first would be up
         * to the controller. */
        if (slot->waiting && !slot->in_mailbox) {
            const uint32_t key = can_frame_arbitration_key(&slot->value.frame);

            /* Strictly smaller: of slots with equal keys the lower one keeps its place. */
            if ((best == CAN_TXQ_CYCLIC_SLOTS) || (key < *best_key)) {
                best = i;
                *best_key = key;
            }
        }
    }

    return best;
}

void can_txq_init(can_txq_t *queue, const can_txq_config_t *config)
{
    /* Everything empty, nothing in a mailbox, counters 0, not bus off: the queue does not
     * depend on what the memory held, so it may live on a stack as well as in .bss. */
    (void)memset(queue, 0, sizeof(*queue));
    queue->config = *config;

    if (queue->config.mailboxes < 1U) {
        queue->config.mailboxes = 1U;
    }

    if (queue->config.mailboxes > CAN_TXQ_MAX_MAILBOXES) {
        queue->config.mailboxes = (uint8_t)CAN_TXQ_MAX_MAILBOXES;
    }

    if (queue->config.holdoff_max_ms < queue->config.holdoff_initial_ms) {
        queue->config.holdoff_max_ms = queue->config.holdoff_initial_ms;
    }

    queue->stats.holdoff_ms = queue->config.holdoff_initial_ms;
}

bool can_txq_publish(can_txq_t *queue, uint8_t slot, const can_frame_t *frame, uint64_t stamp_ns)
{
    bool accepted = false;

    if ((slot < CAN_TXQ_CYCLIC_SLOTS) && (frame != NULL) && can_frame_is_valid(frame)) {
        can_txq_slot_t *entry = &queue->slots[slot];
        can_txq_slot_stats_t *counts = &queue->slot_stats[slot];

        queue->stats.produced++;
        counts->produced++;

        if (entry->waiting) {
            /* The value that was waiting will never go out: the new one takes its place. */
            queue->stats.superseded++;
            counts->superseded++;
        } else {
            entry->waiting = true;
            queue->stats.pending++;
            counts->pending++;
        }

        /* An earlier value that is already in a mailbox is not touched: it is noted in its
         * mailbox record, and this one waits here until that one is done or aborted. */
        entry->value.frame = *frame;
        entry->value.stamp_ns = stamp_ns;

        /* ... but it has missed a whole period and is stale: the mailboxes are to be called
         * back at the next poll, so that the newer value takes its place there too. Only noted
         * here, because a publish has no requests to return and the producers are not the
         * transmit task. Not while bus off: the abort was asked for on the way in, and a
         * note left from before then was dropped there (can_txq_on_state). */
        if (entry->in_mailbox && !queue->bus_off) {
            queue->recall_pending = true;
        }

        accepted = true;
    }

    return accepted;
}

bool can_txq_enqueue(can_txq_t *queue, const can_frame_t *frame, uint64_t now_ns)
{
    bool accepted = false;

    if ((frame != NULL) && can_frame_is_valid(frame)) {
        /* "Full" has to mean full of frames that can still be sent. Without this, a frame
         * would be refused because of dead ones that only wait for the next poll, and whether
         * it is dropped would depend on when the transmit task last ran. */
        expire_events(queue, now_ns);
        queue->stats.produced++;

        if (queue->event_count < CAN_TXQ_EVENT_DEPTH) {
            can_txq_item_t *entry = &queue->events[event_index(queue, queue->event_count)];

            entry->frame = *frame;
            entry->stamp_ns = now_ns;
            queue->event_count++;
            queue->stats.pending++;
            accepted = true;
        } else {
            queue->stats.dropped++;
        }
    }

    return accepted;
}

/*
 * True if the oldest event frame is what goes into the next free mailbox; false if the value
 * of `slot` goes, or nothing at all. `slot` and `slot_key` are what best_slot() found.
 *
 * Only one event frame is in a mailbox at a time, so the oldest waiting one is a candidate
 * only while none is out. Handing the second oldest to the next free mailbox would leave the
 * order of the two to the controller: it sends whichever wins arbitration, and for equal
 * identifiers whichever its mailbox order prefers (bxCAN, for one, sends the lower-numbered
 * mailbox first unless it is switched to request order). After an abort they would come back
 * in the order of the abort reports. Keeping the FIFO to one mailbox makes "not reordered"
 * hold whatever the controller does; the price is one transmit-done round trip between two
 * event frames, during which the other mailboxes still carry cyclic frames. It is the rule for
 * a cyclic message, applied to the FIFO for the same reason.
 *
 * If a slot has a candidate too, the event frame goes first only with the strictly smaller
 * key. Of equal keys the slot wins, and best_slot() has already preferred the lower slot:
 * lower slot first, event frame last. Equal keys need two sources with one identifier, which
 * a sound bus design does not have; the rule is there so that the outcome is defined, and it
 * favours the value that can still go stale.
 */
static bool event_goes_first(const can_txq_t *queue, uint32_t slot, uint32_t slot_key)
{
    bool event_first = (queue->event_count > 0U) && !queue->event_in_mailbox;

    if (event_first && (slot < CAN_TXQ_CYCLIC_SLOTS)) {
        const uint32_t event_key =
            can_frame_arbitration_key(&queue->events[queue->event_head].frame);

        event_first = event_key < slot_key;
    }

    return event_first;
}

/* Move the oldest event frame from the FIFO to the record with this index. The index is kept
 * for the recall at the frame's timeout, and that recall is armed afresh: it is once per
 * frame, and this is the only way an event frame gets into a mailbox. */
static void move_event_to_record(can_txq_t *queue, uint32_t index)
{
    can_txq_mailbox_t *record = &queue->mailboxes[index];

    record->cyclic = false;
    record->slot = 0U; /* none; a fixed value, so that can_txq_done_t is always fully defined */
    record->item = queue->events[queue->event_head];
    queue->event_head = event_index(queue, 1U);
    queue->event_count--;
    queue->event_in_mailbox = true;
    queue->event_record = (uint8_t)index;
    queue->event_recalled = false;
}

/* Move the waiting value of `slot` to `record`. The slot can take a newer value at once; that
 * one then waits there until this one is done or aborted. */
static void move_slot_value_to_record(can_txq_t *queue, can_txq_mailbox_t *record, uint32_t slot)
{
    can_txq_slot_t *entry = &queue->slots[slot];

    record->cyclic = true;
    record->slot = (uint8_t)slot;
    record->item = entry->value;
    entry->waiting = false;
    entry->in_mailbox = true;
}

bool can_txq_next(can_txq_t *queue, uint64_t now_ns, can_frame_t *frame, uint32_t *handle)
{
    bool handed_out = false;

    /* A caller that cannot take the frame or its handle gets neither; the frame stays. */
    if ((frame != NULL) && (handle != NULL)) {
        /* First, so that a frame which is too old is never among the candidates, and before
         * the checks below, so that expiry does not wait for a free mailbox or for the bus. */
        expire_events(queue, now_ns);

        const uint32_t index = free_record(queue);

        if (!queue->bus_off && (index < CAN_TXQ_MAX_MAILBOXES)) {
            uint32_t slot_key = 0U;
            const uint32_t slot = best_slot(queue, &slot_key);
            const bool take_event = event_goes_first(queue, slot, slot_key);

            if (take_event || (slot < CAN_TXQ_CYCLIC_SLOTS)) {
                can_txq_mailbox_t *record = &queue->mailboxes[index];

                if (take_event) {
                    move_event_to_record(queue, index);
                } else {
                    move_slot_value_to_record(queue, record, slot);
                }

                /* The frame only changes place: stats.pending stays as it is. */
                record->used = true;
                record->handle = (queue->handle_serial * CAN_TXQ_MAX_MAILBOXES) + index;
                queue->handle_serial = (queue->handle_serial + 1U) % HANDLE_SERIALS;

                *frame = record->item.frame;
                *handle = record->handle;
                handed_out = true;
            }
        }
    }

    return handed_out;
}

bool can_txq_on_tx_done(can_txq_t *queue, uint32_t handle, can_txq_done_t *info)
{
    can_txq_mailbox_t *record = find_record(queue, handle);
    bool known = false;

    if (record != NULL) {
        if (info != NULL) {
            /* From the record, not from the slot: the slot may hold a newer value by now. */
            info->cyclic = record->cyclic;
            info->slot = record->slot;
            info->id = record->item.frame.id;
            info->stamp_ns = record->item.stamp_ns;
        }

        if (record->cyclic) {
            queue->slots[record->slot].in_mailbox = false;
            queue->slot_stats[record->slot].sent++;
            queue->slot_stats[record->slot].pending--;
        } else {
            queue->event_in_mailbox = false;
        }

        record->used = false;
        queue->stats.sent++;
        queue->stats.pending--;
        known = true;
    }

    return known;
}

bool can_txq_on_tx_aborted(can_txq_t *queue, uint32_t handle)
{
    can_txq_mailbox_t *record = find_record(queue, handle);
    bool known = false;

    if (record != NULL) {
        if (record->cyclic) {
            can_txq_slot_t *entry = &queue->slots[record->slot];

            if (entry->waiting) {
                /* A newer value was published while this one was in the mailbox. Only the
                 * newest is worth sending, so this one ends here. */
                queue->stats.superseded++;
                queue->stats.pending--;
                queue->slot_stats[record->slot].superseded++;
                queue->slot_stats[record->slot].pending--;
            } else {
                entry->value = record->item;
                entry->waiting = true;
            }

            entry->in_mailbox = false;
        } else {
            /*
             * Back to the head of the FIFO, in front of everything that was enqueued after
             * it; as it is the only event frame that can be out, that restores the order. It
             * keeps the time of its enqueue, so the timeout does not start again.
             *
             * The FIFO may have filled up while the frame was away. It returns all the same:
             * the ring has one entry more than can_txq_enqueue() ever fills, kept for this
             * frame, so the step back from the head always lands on a free entry. The other
             * ways out would each lose a frame that was accepted, this one or the youngest,
             * and tear a hole into a sequence the sender believes to be queued. The FIFO then
             * holds one frame more than its depth and refuses new ones until it is below the
             * depth again. No counter changes: the frame was pending and still is.
             */
            queue->event_head = event_index(queue, event_ring_size(queue) - 1U);
            queue->events[queue->event_head] = record->item;
            queue->event_count++;
            queue->event_in_mailbox = false;
        }

        record->used = false;
        known = true;
    }

    return known;
}

uint32_t can_txq_on_state(can_txq_t *queue, can_fc_state_t state, uint64_t now_ns)
{
    /* The queue only tells bus off from not bus off. Error passive is a node that still
     * transmits, so for the queue it is the same as error active. */
    const bool bus_off = (state == CAN_FC_BUS_OFF);
    uint32_t requests = 0U;

    if (bus_off && !queue->bus_off) {
        /* Whether the node was back for the stable time is decided here as well as in
         * can_txq_poll(), so that the hold-off of this bus-off does not depend on whether the
         * caller happened to poll in between. From now on the stable time no longer runs. */
        reset_holdoff_when_stable(queue, now_ns);
        queue->holdoff_reset_pending = false;

        queue->bus_off = true;
        queue->restart_pending = true;
        queue->bus_off_since_ns = now_ns;
        queue->stats.bus_off_count++;

        /* Asked for on every transition into bus off, whether or not a mailbox is in use:
         * aborting nothing is harmless, and the caller needs no second rule. The frames stay
         * noted in their records until the caller reports each abort. */
        requests = CAN_TXQ_REQUEST_ABORT_MAILBOXES;

        /* This abort also takes the stale value that a pending recall was for: the poll must
         * not ask a second time. The note is dropped rather than kept for after the recovery,
         * when the frame it was about has long come back. */
        queue->recall_pending = false;
    } else if (!bus_off && queue->bus_off) {
        queue->bus_off = false;

        /* A controller may come back before the hold-off is over, on its own or because
         * something else restarted it. There is then nothing left to request. */
        queue->restart_pending = false;

        /* The hold-off is raised now rather than at the next bus-off, so that
         * stats.holdoff_ms is "the hold-off of the current or next bus-off" at all times, as
         * the header says. If the node then stays on the bus for the stable time, the raised
         * value is never used. */
        queue->stats.holdoff_ms = doubled_holdoff(&queue->config, queue->stats.holdoff_ms);
        queue->holdoff_reset_pending =
            (queue->stats.holdoff_ms != queue->config.holdoff_initial_ms);
        queue->recovered_at_ns = now_ns;
    } else {
        /* A report of the state the queue already knows, or a change between error active
         * and error passive: nothing to do. */
    }

    return requests;
}

/* True if a cyclic message has a newer value waiting behind the one in its mailbox: the
 * situation in which can_txq_publish() notes a recall, still there. */
static bool stale_value_in_mailbox(const can_txq_t *queue)
{
    bool stale = false;

    for (uint32_t i = 0U; (i < CAN_TXQ_CYCLIC_SLOTS) && !stale; i++) {
        stale = queue->slots[i].waiting && queue->slots[i].in_mailbox;
    }

    return stale;
}

/*
 * True while the event frame in a mailbox is to be called back once its timeout is over: there
 * is one, it has not been called back yet, expiry is switched on, and the controller is not bus
 * off.
 *
 * Once per frame. Having asked, the queue waits for the controller's answer, an abort report or
 * a confirmation if the attempt on the wire succeeds; asking again would not bring either
 * sooner, and would keep can_txq_next_deadline() in the past. Not while bus off: the abort was
 * asked for on the way in. A frame that is still in its mailbox after the recovery, because the
 * caller did not carry that abort out, is called back then, its time being over.
 */
static bool event_recall_armed(const can_txq_t *queue)
{
    return queue->event_in_mailbox && !queue->event_recalled && !queue->bus_off &&
           (queue->config.event_timeout_ms != 0U);
}

/* The enqueue time of the event frame in a mailbox, from which its timeout runs. Only while
 * event_in_mailbox. */
static uint64_t event_out_since(const can_txq_t *queue)
{
    return queue->mailboxes[queue->event_record].item.stamp_ns;
}

/*
 * Whether this poll asks for the mailboxes to be aborted because a frame in one of them has
 * gone stale, with the bookkeeping for having asked.
 *
 * A note left by can_txq_publish() is checked against the slots before it is acted on, and
 * withdrawn if no value waits behind an earlier one any more: the earlier value was confirmed
 * after the publish, and its successor goes, or has gone, to a mailbox the ordinary way.
 * Acting on the note all the same would be harmless, as the abort would find nothing of that
 * message to remove, but it would not be free: every other frame would be taken out of the
 * controller and loaded again, which on a busy bus gives up their places in the controller for
 * a moment, and recall_requests would count the outcome of a race instead of stale frames. The
 * check costs a scan of the 16 slots, and only when a note is pending.
 *
 * One request covers everything that is stale at the time, since the abort empties every
 * mailbox anyway. The note is consumed either way, so a request is made at most once per
 * publish that found its predecessor in a mailbox.
 */
static bool recall_due(can_txq_t *queue, uint64_t now_ns)
{
    const bool stale_value = queue->recall_pending && stale_value_in_mailbox(queue);
    const bool stale_event =
        event_recall_armed(queue) &&
        has_elapsed(now_ns, event_out_since(queue), queue->config.event_timeout_ms);

    queue->recall_pending = false;

    if (stale_event) {
        queue->event_recalled = true;
    }

    return stale_value || stale_event;
}

uint32_t can_txq_poll(can_txq_t *queue, uint64_t now_ns)
{
    uint32_t requests = 0U;

    expire_events(queue, now_ns);
    reset_holdoff_when_stable(queue, now_ns);

    if (queue->restart_pending &&
        has_elapsed(now_ns, queue->bus_off_since_ns, queue->stats.holdoff_ms)) {
        /* Once per bus-off. The controller counts its 128 idle periods from here and reports
         * the new state when it is back; asking again would not make that faster. */
        queue->restart_pending = false;
        queue->stats.restart_requests++;
        requests = CAN_TXQ_REQUEST_RESTART;
    }

    /* Not in an else: whether a recall is due has to be decided, and the note consumed, on
     * every poll. A restart is requested only while bus off, when no recall is ever due. */
    if (recall_due(queue, now_ns)) {
        queue->stats.recall_requests++;
        requests |= CAN_TXQ_REQUEST_ABORT_MAILBOXES;
    }

    return requests;
}

uint64_t can_txq_next_deadline(const can_txq_t *queue)
{
    uint64_t deadline_ns = CAN_TXQ_NO_DEADLINE;

    /* The things can_txq_poll() does. The first two exclude each other: one needs the node to
     * be bus off, the other needs it not to be. */
    if (queue->restart_pending) {
        deadline_ns = time_after(queue->bus_off_since_ns, queue->stats.holdoff_ms);
    } else if (queue->holdoff_reset_pending) {
        deadline_ns = time_after(queue->recovered_at_ns, queue->config.stable_ms);
    } else {
        /* no bus-off timer is running */
    }

    /* The oldest event frame in the FIFO expires ... */
    if ((queue->config.event_timeout_ms != 0U) && (queue->event_count > 0U)) {
        const uint64_t expiry_ns =
            time_after(queue->events[queue->event_head].stamp_ns, queue->config.event_timeout_ms);

        if (expiry_ns < deadline_ns) {
            deadline_ns = expiry_ns;
        }
    }

    /* ... and the one in a mailbox, which cannot expire there, is called back at the same
     * moment. */
    if (event_recall_armed(queue)) {
        const uint64_t recall_ns =
            time_after(event_out_since(queue), queue->config.event_timeout_ms);

        if (recall_ns < deadline_ns) {
            deadline_ns = recall_ns;
        }
    }

    /*
     * A noted recall of a cyclic value is due at once. The publish that noted it had no time
     * to give, and none is needed: the earliest time there is has passed for every clock, so a
     * caller that compares the deadline with its clock polls now, and one that sleeps until it
     * waits for the shortest time it can. Checked as can_txq_poll() will check it, so that a
     * note that will be withdrawn does not make the deadline lie.
     */
    if (queue->recall_pending && stale_value_in_mailbox(queue)) {
        deadline_ns = 0U;
    }

    return deadline_ns;
}

const can_txq_stats_t *can_txq_stats(const can_txq_t *queue)
{
    return &queue->stats;
}

uint32_t can_txq_events_held(const can_txq_t *queue)
{
    return queue->event_count + (queue->event_in_mailbox ? 1U : 0U);
}

const can_txq_slot_stats_t *can_txq_slot_stats(const can_txq_t *queue, uint8_t slot)
{
    const can_txq_slot_stats_t *counts = NULL;

    if (slot < CAN_TXQ_CYCLIC_SLOTS) {
        counts = &queue->slot_stats[slot];
    }

    return counts;
}
