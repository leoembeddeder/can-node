/* SPDX-License-Identifier: MIT */
#include "can/can_fc.h"

/*
 * What holds for a can_fc_t between any two calls, from can_fc_init() on and as long as only
 * this module writes to it:
 *
 *   - state is what rules 9 to 11 give for the two counters (state_from_counters);
 *   - rec is at most CAN_FC_REC_MAX; tec is at most 255 while the node is on the bus and
 *     256..263 while it is bus off: the step that crosses the limit is 8, starts from 255 at
 *     the latest, and nothing is added after it. This is why the transmit counter needs no
 *     overflow handling;
 *   - restart_requested is false unless the node is bus off; recovery_occurrences is 0 unless
 *     a restart was requested, and always below CAN_FC_RECOVERY_OCCURRENCES.
 *
 * tests/unit/test_can_fc.c applies every event to every such value and checks that the result
 * is such a value again.
 */

/*
 * Rules 9 to 11. The state is a function of the two counters alone: rule 11 is the exact
 * complement of rule 9, so there is no hysteresis between error active and error passive and
 * no earlier state to remember.
 */
static can_fc_state_t state_from_counters(uint16_t tec, uint16_t rec)
{
    can_fc_state_t state;

    if (tec >= CAN_FC_BUS_OFF_LIMIT) {
        state = CAN_FC_BUS_OFF; /* rule 10; asked first because it outranks rule 9 */
    } else if ((tec >= CAN_FC_PASSIVE_LIMIT) || (rec >= CAN_FC_PASSIVE_LIMIT)) {
        state = CAN_FC_ERROR_PASSIVE; /* rule 9 */
    } else {
        state = CAN_FC_ERROR_ACTIVE; /* rule 11 */
    }

    return state;
}

/*
 * The receive counter plus `step`, held at CAN_FC_REC_MAX.
 *
 * The specification sets no upper bound for this counter, and above 127 no rule asks more of
 * it than that it is above 127 (rules 8 and 9). It is stopped at the largest value of an
 * 8-bit register. Left to wrap, it would turn the receiver that sees the most errors on the
 * bus back into an error-active node.
 */
static uint16_t rec_plus(uint16_t rec, uint16_t step)
{
    uint16_t sum = (uint16_t)(rec + step);

    if (sum > CAN_FC_REC_MAX) {
        sum = CAN_FC_REC_MAX;
    }

    return sum;
}

void can_fc_init(can_fc_t *fc)
{
    fc->tec = 0U;
    fc->rec = 0U;
    fc->state = CAN_FC_ERROR_ACTIVE;
    fc->restart_requested = false;
    fc->recovery_occurrences = 0U;
}

/*
 * More than one rule may apply during one frame (the specification says so above rule 1). The
 * caller then applies one event per rule, in the order in which things happened on the bus: a
 * receiver that was the first to signal an error gets CAN_FC_RX_ERROR and then
 * CAN_FC_RX_DOMINANT_AFTER_FLAG, 9 in all.
 */
can_fc_state_t can_fc_apply(can_fc_t *fc, can_fc_event_t event)
{
    if (fc->state == CAN_FC_BUS_OFF) {
        /* A bus-off node takes no part in bus traffic, so none of rules 1 to 8 can apply to
         * it and its counters stay as they were when it left the bus. Rule 12 only permits
         * the way back; the count runs once the application has asked for it. */
        if ((event == CAN_FC_RECESSIVE_11) && fc->restart_requested) {
            fc->recovery_occurrences++;

            if (fc->recovery_occurrences >= CAN_FC_RECOVERY_OCCURRENCES) {
                /* Rule 12: error active with both counters at 0. The request is used up
                 * with it, so the next bus off needs a new one. */
                can_fc_init(fc);
            }
        }
    } else {
        switch (event) {
        case CAN_FC_RX_ERROR: /* rule 1 */
            fc->rec = rec_plus(fc->rec, 1U);
            break;

        case CAN_FC_RX_DOMINANT_AFTER_FLAG: /* rule 2 */
            fc->rec = rec_plus(fc->rec, 8U);
            break;

        case CAN_FC_TX_ERROR: /* rule 3 */
            fc->tec = (uint16_t)(fc->tec + 8U);
            break;

        case CAN_FC_TX_ACK_ERROR_ALONE:
            /* Rule 3 for an error-active node, exception 1 for an error-passive one. What
             * decides is the state in which the error is signalled, which is the state before
             * this event and not the transmit counter: a node that is error passive through
             * its receive counter is excepted too. Starting from 0, the 16th of these errors
             * finds the node still error active at 120, is counted, and is the last one that
             * is: the counter stays at 128. */
            if (fc->state == CAN_FC_ERROR_ACTIVE) {
                fc->tec = (uint16_t)(fc->tec + 8U);
            }
            break;

        case CAN_FC_TX_STUFF_ERROR_IN_ARBITRATION: /* rule 3, exception 2 */
            break;

        case CAN_FC_TX_BIT_ERROR_IN_FLAG: /* rule 4 */
            fc->tec = (uint16_t)(fc->tec + 8U);
            break;

        case CAN_FC_RX_BIT_ERROR_IN_FLAG: /* rule 5 */
            fc->rec = rec_plus(fc->rec, 8U);
            break;

        case CAN_FC_TX_DOMINANT_RUN: /* rule 6, transmitter */
            fc->tec = (uint16_t)(fc->tec + 8U);
            break;

        case CAN_FC_RX_DOMINANT_RUN: /* rule 6, receiver */
            fc->rec = rec_plus(fc->rec, 8U);
            break;

        case CAN_FC_TX_SUCCESS: /* rule 7 */
            if (fc->tec > 0U) {
                fc->tec--;
            }
            break;

        case CAN_FC_RX_SUCCESS: /* rule 8 */
            if (fc->rec >= CAN_FC_PASSIVE_LIMIT) {
                /* Not counted down from wherever it had run to, but put just below the
                 * limit: one good frame ends the error-passive state of a receiver. */
                fc->rec = CAN_FC_REC_AFTER_PASSIVE;
            } else if (fc->rec > 0U) {
                fc->rec--;
            } else {
                /* 0 stays 0 */
            }
            break;

        case CAN_FC_RECESSIVE_11: /* rule 12 is about a bus-off node only */
        case CAN_FC_EVENT_COUNT:  /* the number of events, not one of them */
        default:                  /* a stray value */
            break;
        }

        /* Rules 9 to 11. The new state governs the NEXT error. Rule 9 ends by saying that the
         * error which makes a node error passive is itself still signalled with an ACTIVE
         * error flag. That changes no counter here, but a caller that models the flag has to
         * take its kind from the state it saw BEFORE applying the error, not from the state
         * returned for it: on the kind of flag depend whether rule 4 or 5 can follow (a bit
         * error in an active flag only) and where the dominant run of rule 6 starts to count
         * (after the 14th bit for an active flag, the 8th for a passive one). It is also why
         * the cases of rules 4 and 5 above do not ask for the state: an error-passive node
         * still sends that one active flag, and overload flags, which are dominant in every
         * state. */
        fc->state = state_from_counters(fc->tec, fc->rec);
    }

    return fc->state;
}

void can_fc_request_restart(can_fc_t *fc)
{
    /* Asking again while the count is running changes nothing; in particular it does not
     * start the count again. The header leaves that case open. A driver that repeats the
     * request on every pass of its loop until the node is back would otherwise postpone the
     * recovery for as long as it keeps asking. */
    if (fc->state == CAN_FC_BUS_OFF) {
        fc->restart_requested = true;
    }
}

bool can_fc_warning(const can_fc_t *fc)
{
    /* A bus-off node reports bus off, not a warning: its transmit counter, frozen at 256 or
     * more, no longer says anything about the bus. */
    return (fc->state != CAN_FC_BUS_OFF) &&
           ((fc->tec >= CAN_FC_WARNING_LIMIT) || (fc->rec >= CAN_FC_WARNING_LIMIT));
}
