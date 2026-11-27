/* SPDX-License-Identifier: MIT */
#include "isotp/isotp.h"

#include <stddef.h>
#include <string.h>

/*
 * What the module does is described in isotp.h. This file adds how, and the reasons for the
 * points at which the contract needed a decision; each is argued where it is implemented:
 *
 *   - every call acts on the timers of its time first, and again after its work   service()
 *   - a frame of the peer proves the frame it answers                    tx_on_fc(), rx_on_cf()
 *   - which frame a confirmation belongs to                              isotp_on_tx_done()
 *   - a frame whose confirmation is overdue is given up                  give_up_handouts()
 *   - STmin across a block boundary                                      tx_on_fc()
 *   - a valid SF or FF ends what the receiver was doing                  rx_make_way()
 *   - the FF's data waits in the instance while the buffer is kept       rx_on_ff()
 *
 * What holds for an isotp_t between any two calls, from isotp_init() on:
 *
 *   - handouts[0..handout_count-1] are the frames handed out and neither confirmed nor given up,
 *     oldest first, and stats.outstanding is handout_count. No entry's deadline has passed at the
 *     time of the last call.
 *   - tx.state is SENT exactly when the sender waits for the confirmation of hand-out
 *     tx.awaited, which is then in handouts; likewise rx.state FC_SENT and rx.awaited. A proven
 *     frame (see CONFIRMATIONS in isotp.h) stays in handouts, but nobody awaits it any more.
 *   - tx.state DUE or rx.state FC_DUE only while the pipeline is full: a frame that is due is
 *     handed out in the call that made it due if there is room.
 *   - rx.held is set only by an indication with ISOTP_RESULT_OK, and cleared only by
 *     isotp_rx_release().
 */

/* The N_PCI types, the high nibble of byte 0. */
#define PCI_TYPE_SF 0x0U
#define PCI_TYPE_FF 0x1U
#define PCI_TYPE_CF 0x2U
#define PCI_TYPE_FC 0x3U

/* The flow status values of an FC, the low nibble of its byte 0. */
#define FS_CTS   0x0U
#define FS_WAIT  0x1U
#define FS_OVFLW 0x2U

/* FF_DLmin for classical CAN with normal addressing: a shorter message is a single frame. */
#define FF_DL_MIN 8U

/* The N_PCI of an FC is 3 bytes: FS, BS, STmin. */
#define FC_PCI_LENGTH 3U

/* The DLC of every frame sent, and the one an FF must have: the frame is full. */
#define FRAME_LENGTH CAN_MAX_DLC

/* The largest STmin given in milliseconds, and the range given in 100 microsecond steps. */
#define STMIN_MS_MAX     0x7FU
#define STMIN_100US_LOW  0xF1U
#define STMIN_100US_HIGH 0xF9U

_Static_assert(ISOTP_SF_DATA == (FRAME_LENGTH - 1U), "an SF has 1 byte of N_PCI");
_Static_assert(ISOTP_FF_DATA == (FRAME_LENGTH - 2U), "an FF has 2 bytes of N_PCI");
_Static_assert(ISOTP_CF_DATA == (FRAME_LENGTH - 1U), "a CF has 1 byte of N_PCI");
_Static_assert(ISOTP_MAX_LENGTH == 0xFFFU, "FF_DL has 12 bits");
_Static_assert(FF_DL_MIN == (ISOTP_SF_DATA + 1U), "the shortest FF message is one byte too long "
                                                  "for an SF");
_Static_assert(ISOTP_PIPELINE_DEPTH <= 255U, "handout_count is a uint8_t");
_Static_assert(ISOTP_STMIN_RESERVED_US == (STMIN_MS_MAX * 1000U), "a reserved STmin reads 0x7F");

static const char *const s_result_names[ISOTP_RESULTS] = {
    [ISOTP_RESULT_OK] = "OK",
    [ISOTP_RESULT_TIMEOUT_A] = "TIMEOUT_A",
    [ISOTP_RESULT_TIMEOUT_BS] = "TIMEOUT_BS",
    [ISOTP_RESULT_TIMEOUT_CR] = "TIMEOUT_CR",
    [ISOTP_RESULT_WRONG_SN] = "WRONG_SN",
    [ISOTP_RESULT_INVALID_FS] = "INVALID_FS",
    [ISOTP_RESULT_UNEXP_PDU] = "UNEXP_PDU",
    [ISOTP_RESULT_WFT_OVRN] = "WFT_OVRN",
    [ISOTP_RESULT_BUFFER_OVFLW] = "BUFFER_OVFLW",
    [ISOTP_RESULT_ERROR] = "ERROR",
};

static const char *const s_frame_kind_names[ISOTP_FRAME_KINDS] = {
    [ISOTP_FRAME_SF] = "SF",
    [ISOTP_FRAME_FF] = "FF",
    [ISOTP_FRAME_CF] = "CF",
    [ISOTP_FRAME_FC_CTS] = "FC_CTS",
    [ISOTP_FRAME_FC_WAIT] = "FC_WAIT",
    [ISOTP_FRAME_FC_OVFLW] = "FC_OVFLW",
    [ISOTP_FRAME_FC_RESERVED] = "FC_RESERVED",
};

static const char *const s_ignored_names[ISOTP_IGNORED_REASONS] = {
    [ISOTP_IGNORED_FOREIGN] = "FOREIGN",
    [ISOTP_IGNORED_SHORT] = "SHORT",
    [ISOTP_IGNORED_LENGTH] = "LENGTH",
    [ISOTP_IGNORED_RESERVED_TYPE] = "RESERVED_TYPE",
    [ISOTP_IGNORED_UNEXPECTED_CF] = "UNEXPECTED_CF",
    [ISOTP_IGNORED_UNEXPECTED_FC] = "UNEXPECTED_FC",
};

_Static_assert((sizeof(s_result_names) / sizeof(s_result_names[0])) == (size_t)ISOTP_RESULTS,
               "a name for every result");
_Static_assert((sizeof(s_frame_kind_names) / sizeof(s_frame_kind_names[0])) ==
                   (size_t)ISOTP_FRAME_KINDS,
               "a name for every frame kind");
_Static_assert((sizeof(s_ignored_names) / sizeof(s_ignored_names[0])) ==
                   (size_t)ISOTP_IGNORED_REASONS,
               "a name for every reason");

/* ------------------------------------------------------------------------------------ time */

/* The moment a timer of `delay_us` set at `now_us` runs out; a moment beyond the clock's range
 * is never reached, which is what ISOTP_NO_DEADLINE says. */
static uint64_t time_after(uint64_t now_us, uint32_t delay_us)
{
    uint64_t at_us = ISOTP_NO_DEADLINE;

    if ((uint64_t)delay_us < (ISOTP_NO_DEADLINE - now_us)) {
        at_us = now_us + (uint64_t)delay_us;
    }

    return at_us;
}

/* True once `deadline_us` has been reached: at that very microsecond, not one later. */
static bool has_run_out(uint64_t now_us, uint64_t deadline_us)
{
    return (deadline_us != ISOTP_NO_DEADLINE) && (now_us >= deadline_us);
}

/* Microseconds from `since_us` to `now_us`, saturating; 0 while `now_us` is before it (a caller
 * whose clock readings crossed). */
static uint32_t elapsed_us(uint64_t now_us, uint64_t since_us)
{
    uint32_t elapsed = 0U;

    if (now_us > since_us) {
        const uint64_t difference = now_us - since_us;
        elapsed = (difference > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)difference;
    }

    return elapsed;
}

static uint64_t earlier(uint64_t a_us, uint64_t b_us)
{
    return (a_us < b_us) ? a_us : b_us;
}

/* -------------------------------------------------------------------------------- callbacks */

/* Every callback runs with in_callback set, so that a call back into the instance is refused
 * (see CALLBACKS in isotp.h): the state is consistent when a callback runs, but the call that
 * runs it is not finished. */

static bool call_send_frame(isotp_t *tp, const can_frame_t *frame)
{
    bool taken;

    tp->in_callback = true;
    taken = tp->callbacks.send_frame(tp->callbacks.context, frame);
    tp->in_callback = false;

    return taken;
}

static void call_ff_indication(isotp_t *tp, uint16_t length)
{
    if (tp->callbacks.ff_indication != NULL) {
        tp->in_callback = true;
        tp->callbacks.ff_indication(tp->callbacks.context, length);
        tp->in_callback = false;
    }
}

static bool call_indication(isotp_t *tp, isotp_result_t result, const uint8_t *data,
                            uint16_t length)
{
    bool keep;

    tp->in_callback = true;
    keep = tp->callbacks.indication(tp->callbacks.context, result, data, length);
    tp->in_callback = false;

    return keep;
}

static void call_confirm(isotp_t *tp, isotp_result_t result)
{
    if (tp->callbacks.confirm != NULL) {
        tp->in_callback = true;
        tp->callbacks.confirm(tp->callbacks.context, result);
        tp->in_callback = false;
    }
}

/* True if `tp` may be worked on: initialised, and not called from one of its callbacks (that
 * call is counted and refused). */
static bool usable(isotp_t *tp)
{
    bool ok = false;

    if ((tp != NULL) && tp->ready) {
        if (tp->in_callback) {
            tp->stats.reentry_refused++;
        } else {
            ok = true;
        }
    }

    return ok;
}

/* ------------------------------------------------------------------------- the hand-outs */

static bool pipeline_has_room(const isotp_t *tp)
{
    return tp->handout_count < ISOTP_PIPELINE_DEPTH;
}

/* Note a frame that the send_frame callback has just taken; returns its serial. Its N_As (a
 * frame of the sender) or N_Ar (an FC of the receiver) runs from now. */
static uint32_t record_handout(isotp_t *tp, bool rx_side, uint64_t now_us)
{
    isotp_handout_t *entry = &tp->handouts[tp->handout_count];

    entry->serial = tp->serial;
    entry->rx_side = rx_side;
    entry->deadline_us = time_after(now_us, rx_side ? tp->config.n_ar_us : tp->config.n_as_us);
    tp->serial++;
    tp->handout_count++;
    tp->stats.outstanding++;

    return entry->serial;
}

static void remove_handout(isotp_t *tp, uint8_t index)
{
    for (uint8_t i = index; (i + 1U) < tp->handout_count; i++) {
        tp->handouts[i] = tp->handouts[i + 1U];
    }
    tp->handout_count--;
    tp->stats.outstanding--;
}

/* A new frame of this instance: the identifier, DLC 8, the N_PCI's first byte, padding after. */
static void frame_begin(const isotp_t *tp, can_frame_t *frame, uint8_t pci)
{
    frame->id = tp->config.tx_id;
    frame->flags = tp->config.extended_ids ? CAN_FLAG_EXT : 0U;
    frame->dlc = (uint8_t)FRAME_LENGTH;
    for (uint32_t i = 0U; i < FRAME_LENGTH; i++) {
        frame->data[i] = ISOTP_PADDING;
    }
    frame->data[0] = pci;
}

/* ----------------------------------------------------------------------------- the sender */

static void tx_end(isotp_t *tp, isotp_result_t result)
{
    tp->tx.state = ISOTP_TX_IDLE;
    tp->tx.data = NULL;
    tp->stats.tx_results[result]++;
    call_confirm(tp, result);
}

/* The CF just handed out (or about to be confirmed) ends a block after which an FC comes. */
static bool tx_block_complete(const isotp_tx_t *tx)
{
    return (tx->bs != 0U) && (tx->block_count == tx->bs) && (tx->offset < tx->length);
}

static void tx_wait_fc(isotp_t *tp, uint64_t now_us)
{
    tp->tx.state = ISOTP_TX_WAIT_FC;
    tp->tx.timer_us = time_after(now_us, tp->config.n_bs_us);
}

/* The sender waits for an FC: N_Bs runs, or the frame that an FC answers (the FF, the last CF of
 * a block) is handed out and not confirmed yet. */
static bool tx_awaits_fc(const isotp_t *tp)
{
    bool awaits = tp->tx.state == ISOTP_TX_WAIT_FC;

    if (tp->tx.state == ISOTP_TX_SENT) {
        awaits = (tp->tx.frame == ISOTP_FRAME_FF) ||
                 ((tp->tx.frame == ISOTP_FRAME_CF) && tx_block_complete(&tp->tx));
    }

    return awaits;
}

/* The frame the sender handed out last was confirmed at `now_us`. */
static void tx_confirmed(isotp_t *tp, uint64_t now_us)
{
    switch (tp->tx.frame) {
    case ISOTP_FRAME_SF:
        tx_end(tp, ISOTP_RESULT_OK);
        break;
    case ISOTP_FRAME_FF:
        tx_wait_fc(tp, now_us);
        break;
    default: /* a CF */
        tp->tx.cf_done_us = now_us;
        tp->tx.cf_proven = false;
        if (tp->tx.offset >= tp->tx.length) {
            tx_end(tp, ISOTP_RESULT_OK);
        } else if (tx_block_complete(&tp->tx)) {
            tx_wait_fc(tp, now_us);
        } else {
            tp->tx.state = ISOTP_TX_STMIN;
            tp->tx.timer_us = time_after(now_us, tp->tx.stmin_us);
            tp->tx.since_us = now_us;
        }
        break;
    }
}

/*
 * An FC arrived. It is processed only by a sender that waits for one; that includes a sender
 * whose FF or last CF of a block is handed out and not confirmed yet: the FC answers that frame,
 * so the frame was sent, and the FC is processed as if the confirmation had come with it (see
 * CONFIRMATIONS in isotp.h). The frame's own confirmation, when it comes, finds nobody waiting
 * for it.
 */
static bool tx_on_fc(isotp_t *tp, const can_frame_t *frame, uint64_t now_us)
{
    bool processed = false;

    if (!tx_awaits_fc(tp)) {
        tp->stats.ignored[ISOTP_IGNORED_UNEXPECTED_FC]++;
    } else if (frame->dlc < FC_PCI_LENGTH) {
        tp->stats.ignored[ISOTP_IGNORED_SHORT]++;
    } else {
        const uint8_t status = (uint8_t)(frame->data[0] & 0x0FU);

        processed = true;
        if (tp->tx.state == ISOTP_TX_SENT) {
            tp->stats.proven++;
            tp->tx.cf_proven = tp->tx.frame == ISOTP_FRAME_CF;
        }
        tp->tx.state = ISOTP_TX_WAIT_FC;

        if (status == FS_CTS) {
            const uint8_t stmin = frame->data[2];
            uint64_t due_us = now_us;

            tp->stats.frames_in[ISOTP_FRAME_FC_CTS]++;
            if (!isotp_stmin_valid(stmin)) {
                tp->stats.stmin_reserved++;
            }
            tp->tx.bs = frame->data[1];
            tp->tx.stmin_us = isotp_stmin_us(stmin);
            tp->tx.block_count = 0U;
            tp->tx.wft_count = 0U;
            /* STmin holds between any two CFs, so also between the last CF of a block and the
             * first of the next, in case the receiver answered faster than STmin. It counts from
             * the previous CF's confirmation, or from now if that CF was proven by this FC and
             * so is known to have ended before now. After the FF there is no previous CF. */
            if (tp->tx.offset > ISOTP_FF_DATA) {
                const uint64_t reference_us = tp->tx.cf_proven ? now_us : tp->tx.cf_done_us;
                const uint64_t allowed_us = time_after(reference_us, tp->tx.stmin_us);

                if (allowed_us > due_us) {
                    due_us = allowed_us;
                }
            }
            tp->tx.state = ISOTP_TX_STMIN;
            tp->tx.timer_us = due_us;
            tp->tx.since_us = now_us;
        } else if (status == FS_WAIT) {
            tp->stats.frames_in[ISOTP_FRAME_FC_WAIT]++;
            if (tp->tx.wft_count >= tp->config.tx_wft_max) {
                tx_end(tp, ISOTP_RESULT_WFT_OVRN);
            } else {
                tp->tx.wft_count++;
                tx_wait_fc(tp, now_us);
            }
        } else if (status == FS_OVFLW) {
            tp->stats.frames_in[ISOTP_FRAME_FC_OVFLW]++;
            tx_end(tp, ISOTP_RESULT_BUFFER_OVFLW);
        } else {
            tp->stats.frames_in[ISOTP_FRAME_FC_RESERVED]++;
            tx_end(tp, ISOTP_RESULT_INVALID_FS);
        }
    }

    return processed;
}

/* Hand out the frame that is due, if there is room for it. */
static void tx_emit(isotp_t *tp, uint64_t now_us)
{
    if ((tp->tx.state == ISOTP_TX_DUE) && pipeline_has_room(tp)) {
        can_frame_t frame;
        uint16_t count;

        switch (tp->tx.frame) {
        case ISOTP_FRAME_SF:
            frame_begin(tp, &frame, (uint8_t)((PCI_TYPE_SF << 4U) | tp->tx.length));
            count = tp->tx.length;
            (void)memcpy(&frame.data[1], tp->tx.data, count);
            break;
        case ISOTP_FRAME_FF:
            frame_begin(tp, &frame, (uint8_t)((PCI_TYPE_FF << 4U) | (tp->tx.length >> 8U)));
            frame.data[1] = (uint8_t)(tp->tx.length & 0xFFU);
            count = (uint16_t)ISOTP_FF_DATA;
            (void)memcpy(&frame.data[2], tp->tx.data, count);
            break;
        default: /* a CF */
            frame_begin(tp, &frame, (uint8_t)((PCI_TYPE_CF << 4U) | tp->tx.sn));
            count = (uint16_t)(tp->tx.length - tp->tx.offset);
            if (count > ISOTP_CF_DATA) {
                count = (uint16_t)ISOTP_CF_DATA;
            }
            (void)memcpy(&frame.data[1], &tp->tx.data[tp->tx.offset], count);
            break;
        }

        if (!call_send_frame(tp, &frame)) {
            tp->stats.send_refused++;
            tx_end(tp, ISOTP_RESULT_ERROR);
        } else {
            tp->stats.frames_out[tp->tx.frame]++;
            if (tp->tx.frame == ISOTP_FRAME_CF) {
                const uint32_t n_cs_us = elapsed_us(now_us, tp->tx.since_us);

                if (n_cs_us > tp->stats.n_cs_max_us) {
                    tp->stats.n_cs_max_us = n_cs_us;
                }
                if (n_cs_us > tp->config.n_cs_us) {
                    tp->stats.n_cs_late++;
                }
                tp->tx.sn = (uint8_t)((tp->tx.sn + 1U) & 0x0FU);
                if (tp->tx.bs != 0U) { /* BS 0: one block, nothing to count */
                    tp->tx.block_count++;
                }
            }
            tp->tx.offset = (uint16_t)(tp->tx.offset + count);
            tp->tx.awaited = record_handout(tp, false, now_us);
            tp->tx.state = ISOTP_TX_SENT;
        }
    }
}

static void tx_timers(isotp_t *tp, uint64_t now_us)
{
    if ((tp->tx.state == ISOTP_TX_WAIT_FC) && has_run_out(now_us, tp->tx.timer_us)) {
        tx_end(tp, ISOTP_RESULT_TIMEOUT_BS);
    } else if ((tp->tx.state == ISOTP_TX_STMIN) && has_run_out(now_us, tp->tx.timer_us)) {
        tp->tx.state = ISOTP_TX_DUE;
        tp->tx.frame = ISOTP_FRAME_CF;
    } else {
        /* nothing has run out */
    }
}

/* --------------------------------------------------------------------------- the receiver */

static bool rx_in_progress(const isotp_t *tp)
{
    const isotp_rx_state_t state = tp->rx.state;

    return (state == ISOTP_RX_FC_SENT) || (state == ISOTP_RX_WAIT_BUFFER) ||
           (state == ISOTP_RX_CF) ||
           ((state == ISOTP_RX_FC_DUE) && (tp->rx.fc != ISOTP_FRAME_FC_OVFLW));
}

/* The reception in progress ends without its message. */
static void rx_end(isotp_t *tp, isotp_result_t result)
{
    tp->rx.state = ISOTP_RX_IDLE;
    tp->stats.rx_results[result]++;
    (void)call_indication(tp, result, NULL, tp->rx.length);
}

/* A message of `length` bytes is complete in the buffer. */
static void rx_deliver(isotp_t *tp, uint16_t length)
{
    tp->rx.state = ISOTP_RX_IDLE;
    tp->rx.length = length;
    tp->stats.rx_results[ISOTP_RESULT_OK]++;
    tp->rx.held = call_indication(tp, ISOTP_RESULT_OK, tp->buffer, length);
}

/*
 * A valid SF or FF arrived: whatever the receiver was doing ends. A reception in progress ends
 * with N_UNEXP_PDU (the table of unexpected N_PDUs); an OVFLW that is still due is not sent any
 * more, since the sender has moved on to a new message.
 */
static void rx_make_way(isotp_t *tp)
{
    if (rx_in_progress(tp)) {
        rx_end(tp, ISOTP_RESULT_UNEXP_PDU);
    } else if (tp->rx.state == ISOTP_RX_FC_DUE) {
        tp->rx.state = ISOTP_RX_IDLE;
        tp->stats.fc_dropped++;
    } else {
        /* the receiver was idle */
    }
}

/* The buffer is free for the message whose FF is waiting: its data goes first. */
static void rx_take_buffer(isotp_t *tp)
{
    (void)memcpy(tp->buffer, tp->rx.stash, ISOTP_FF_DATA);
    tp->rx.offset = (uint16_t)ISOTP_FF_DATA;
    tp->rx.sn = 1U;
    tp->rx.block_count = 0U;
    tp->rx.fc = ISOTP_FRAME_FC_CTS;
    tp->rx.state = ISOTP_RX_FC_DUE;
}

static bool rx_on_sf(isotp_t *tp, const can_frame_t *frame)
{
    const uint8_t sf_dl = (uint8_t)(frame->data[0] & 0x0FU);
    bool processed = false;

    if ((sf_dl == 0U) || (sf_dl > ISOTP_SF_DATA)) {
        tp->stats.ignored[ISOTP_IGNORED_LENGTH]++;
    } else if (frame->dlc < (sf_dl + 1U)) {
        tp->stats.ignored[ISOTP_IGNORED_SHORT]++;
    } else {
        processed = true;
        tp->stats.frames_in[ISOTP_FRAME_SF]++;
        rx_make_way(tp);
        if (sf_dl > tp->buffer_size) {
            tp->stats.rx_sf_too_long++;
        } else if (tp->rx.held) {
            tp->stats.rx_sf_busy++;
        } else {
            (void)memcpy(tp->buffer, &frame->data[1], sf_dl);
            rx_deliver(tp, sf_dl);
        }
    }

    return processed;
}

/*
 * An FF arrived. Its data is kept in the instance (stash) until the buffer is free: at once
 * unless the upper layer keeps the buffer, in which case the reception waits for it (see
 * RECEPTION in isotp.h). One path for both cases, at the cost of copying 6 bytes twice.
 */
static bool rx_on_ff(isotp_t *tp, const can_frame_t *frame, uint64_t now_us)
{
    bool processed = false;

    if (frame->dlc < FRAME_LENGTH) {
        tp->stats.ignored[ISOTP_IGNORED_SHORT]++;
    } else {
        uint32_t ff_dl = ((uint32_t)(frame->data[0] & 0x0FU) << 8U) | (uint32_t)frame->data[1];
        const bool escape = ff_dl == 0U;

        if (escape) {
            ff_dl = ((uint32_t)frame->data[2] << 24U) | ((uint32_t)frame->data[3] << 16U) |
                    ((uint32_t)frame->data[4] << 8U) | (uint32_t)frame->data[5];
        }

        if ((escape && (ff_dl <= ISOTP_MAX_LENGTH)) || ((!escape) && (ff_dl < FF_DL_MIN))) {
            tp->stats.ignored[ISOTP_IGNORED_LENGTH]++;
        } else {
            processed = true;
            tp->stats.frames_in[ISOTP_FRAME_FF]++;
            rx_make_way(tp);
            tp->rx.since_us = now_us;
            if (ff_dl > tp->buffer_size) {
                /* Every escape FF lands here: its FF_DL is above ISOTP_MAX_LENGTH. */
                tp->stats.rx_overflow++;
                tp->rx.fc = ISOTP_FRAME_FC_OVFLW;
                tp->rx.state = ISOTP_RX_FC_DUE;
            } else {
                tp->rx.length = (uint16_t)ff_dl;
                tp->rx.wft_count = 0U;
                (void)memcpy(tp->rx.stash, &frame->data[2], ISOTP_FF_DATA);
                tp->stats.ff_indications++;
                if (tp->rx.held) {
                    tp->rx.state = ISOTP_RX_WAIT_BUFFER;
                    tp->rx.timer_us = time_after(now_us, tp->config.n_br_us);
                } else {
                    rx_take_buffer(tp);
                }
                call_ff_indication(tp, tp->rx.length);
            }
        }
    }

    return processed;
}

/*
 * A CF arrived. It is taken by a reception that expects CFs: one in RX_CF, and one whose FC CTS
 * is handed out and not confirmed yet, because the CF answers that FC and so proves it was sent
 * (see CONFIRMATIONS in isotp.h). The FC's own confirmation then finds nobody waiting for it.
 */
static bool rx_on_cf(isotp_t *tp, const can_frame_t *frame, uint64_t now_us)
{
    const bool expecting = (tp->rx.state == ISOTP_RX_CF) || ((tp->rx.state == ISOTP_RX_FC_SENT) &&
                                                             (tp->rx.fc == ISOTP_FRAME_FC_CTS));
    bool processed = false;

    if (!expecting) {
        tp->stats.ignored[ISOTP_IGNORED_UNEXPECTED_CF]++;
    } else {
        uint16_t count = (uint16_t)(tp->rx.length - tp->rx.offset);

        if (count > ISOTP_CF_DATA) {
            count = (uint16_t)ISOTP_CF_DATA;
        }

        if (frame->dlc < (count + 1U)) {
            tp->stats.ignored[ISOTP_IGNORED_SHORT]++;
        } else {
            processed = true;
            tp->stats.frames_in[ISOTP_FRAME_CF]++;
            if ((frame->data[0] & 0x0FU) != tp->rx.sn) {
                rx_end(tp, ISOTP_RESULT_WRONG_SN);
            } else {
                if (tp->rx.state == ISOTP_RX_FC_SENT) {
                    tp->stats.proven++;
                }
                tp->rx.state = ISOTP_RX_CF;
                (void)memcpy(&tp->buffer[tp->rx.offset], &frame->data[1], count);
                tp->rx.offset = (uint16_t)(tp->rx.offset + count);
                tp->rx.sn = (uint8_t)((tp->rx.sn + 1U) & 0x0FU);
                if (tp->config.rx_bs != 0U) { /* BS 0: one block, nothing to count */
                    tp->rx.block_count++;
                }
                if (tp->rx.offset >= tp->rx.length) {
                    rx_deliver(tp, tp->rx.length);
                } else if ((tp->config.rx_bs != 0U) && (tp->rx.block_count >= tp->config.rx_bs)) {
                    tp->rx.block_count = 0U;
                    tp->rx.fc = ISOTP_FRAME_FC_CTS;
                    tp->rx.state = ISOTP_RX_FC_DUE;
                    tp->rx.since_us = now_us;
                } else {
                    tp->rx.timer_us = time_after(now_us, tp->config.n_cr_us);
                }
            }
        }
    }

    return processed;
}

/* The FC the receiver handed out last was confirmed at `now_us`. */
static void rx_confirmed(isotp_t *tp, uint64_t now_us)
{
    if (tp->rx.fc == ISOTP_FRAME_FC_CTS) {
        tp->rx.state = ISOTP_RX_CF;
        tp->rx.timer_us = time_after(now_us, tp->config.n_cr_us);
    } else { /* a WAIT: N_Br for the next FC runs from its confirmation */
        tp->rx.since_us = now_us;
        if (tp->rx.held) {
            tp->rx.state = ISOTP_RX_WAIT_BUFFER;
            tp->rx.timer_us = time_after(now_us, tp->config.n_br_us);
        } else {
            rx_take_buffer(tp);
        }
    }
}

static uint8_t flow_status(isotp_frame_kind_t fc)
{
    uint8_t status = FS_OVFLW;

    if (fc == ISOTP_FRAME_FC_CTS) {
        status = FS_CTS;
    } else if (fc == ISOTP_FRAME_FC_WAIT) {
        status = FS_WAIT;
    } else {
        /* OVFLW */
    }

    return status;
}

/* Hand out the FC that is due, if there is room for it. */
static void rx_emit(isotp_t *tp, uint64_t now_us)
{
    if ((tp->rx.state == ISOTP_RX_FC_DUE) && pipeline_has_room(tp)) {
        const bool cts = tp->rx.fc == ISOTP_FRAME_FC_CTS;
        can_frame_t frame;

        frame_begin(tp, &frame, (uint8_t)((PCI_TYPE_FC << 4U) | flow_status(tp->rx.fc)));
        frame.data[1] = cts ? tp->config.rx_bs : 0U;
        frame.data[2] = cts ? tp->config.rx_stmin : 0U;

        if (!call_send_frame(tp, &frame)) {
            tp->stats.send_refused++;
            if (tp->rx.fc == ISOTP_FRAME_FC_OVFLW) {
                tp->rx.state = ISOTP_RX_IDLE;
                tp->stats.fc_dropped++;
            } else {
                rx_end(tp, ISOTP_RESULT_ERROR);
            }
        } else {
            const uint32_t n_br_us = elapsed_us(now_us, tp->rx.since_us);
            const uint32_t serial = record_handout(tp, true, now_us);

            tp->stats.frames_out[tp->rx.fc]++;
            if (n_br_us > tp->stats.n_br_max_us) {
                tp->stats.n_br_max_us = n_br_us;
            }
            if (n_br_us > tp->config.n_br_us) {
                tp->stats.n_br_late++;
            }
            if (tp->rx.fc == ISOTP_FRAME_FC_OVFLW) {
                tp->rx.state = ISOTP_RX_IDLE;
            } else {
                if (tp->rx.fc == ISOTP_FRAME_FC_WAIT) {
                    tp->rx.wft_count++;
                }
                tp->rx.awaited = serial;
                tp->rx.state = ISOTP_RX_FC_SENT;
            }
        }
    }
}

static void rx_timers(isotp_t *tp, uint64_t now_us)
{
    if ((tp->rx.state == ISOTP_RX_CF) && has_run_out(now_us, tp->rx.timer_us)) {
        rx_end(tp, ISOTP_RESULT_TIMEOUT_CR);
    } else if ((tp->rx.state == ISOTP_RX_WAIT_BUFFER) && has_run_out(now_us, tp->rx.timer_us)) {
        if (tp->rx.wft_count < tp->config.rx_wft_max) {
            tp->rx.fc = ISOTP_FRAME_FC_WAIT;
            tp->rx.state = ISOTP_RX_FC_DUE;
        } else {
            rx_end(tp, ISOTP_RESULT_WFT_OVRN);
        }
    } else {
        /* nothing has run out */
    }
}

/* ---------------------------------------------------------------------- driving it all */

/*
 * Frames whose N_As or N_Ar has run out are given up: by the rule of CONFIRMATIONS in isotp.h
 * their confirmation will not come any more, so they leave the hand-outs wherever they are, and
 * the side that still waits for one ends with N_TIMEOUT_A. A frame that is not the oldest can
 * run out first when N_Ar is shorter than N_As.
 */
static void give_up_handouts(isotp_t *tp, uint64_t now_us)
{
    uint8_t i = 0U;

    while (i < tp->handout_count) {
        if (has_run_out(now_us, tp->handouts[i].deadline_us)) {
            const isotp_handout_t gone = tp->handouts[i];

            remove_handout(tp, i);
            tp->stats.unconfirmed++;
            if ((!gone.rx_side) && (tp->tx.state == ISOTP_TX_SENT) &&
                (tp->tx.awaited == gone.serial)) {
                tx_end(tp, ISOTP_RESULT_TIMEOUT_A);
            } else if (gone.rx_side && (tp->rx.state == ISOTP_RX_FC_SENT) &&
                       (tp->rx.awaited == gone.serial)) {
                rx_end(tp, ISOTP_RESULT_TIMEOUT_A);
            } else {
                /* a proven frame, or one of a transmission or reception that has ended */
            }
        } else {
            i++;
        }
    }
}

/* Act on everything that has run out by `now_us`. */
static void expire(isotp_t *tp, uint64_t now_us)
{
    give_up_handouts(tp, now_us);
    tx_timers(tp, now_us);
    rx_timers(tp, now_us);
}

/*
 * The end of every call: the timers once more, because the call's own work can set one that has
 * run out at once (an STmin of 0 after a confirmation, an N_Br of 0), then the frames that are
 * due. The receiver's FC goes first when both are due and only one fits: it is what lets a
 * sender go on, and the sender's N_Bs is running.
 */
static void service(isotp_t *tp, uint64_t now_us)
{
    expire(tp, now_us);
    rx_emit(tp, now_us);
    tx_emit(tp, now_us);
}

static bool on_valid_frame(isotp_t *tp, const can_frame_t *frame, uint64_t now_us)
{
    bool processed = false;

    switch ((uint8_t)(frame->data[0] >> 4U)) {
    case PCI_TYPE_SF:
        processed = rx_on_sf(tp, frame);
        break;
    case PCI_TYPE_FF:
        processed = rx_on_ff(tp, frame, now_us);
        break;
    case PCI_TYPE_CF:
        processed = rx_on_cf(tp, frame, now_us);
        break;
    case PCI_TYPE_FC:
        processed = tx_on_fc(tp, frame, now_us);
        break;
    default:
        tp->stats.ignored[ISOTP_IGNORED_RESERVED_TYPE]++;
        break;
    }

    return processed;
}

/* ----------------------------------------------------------------------------- interface */

static bool id_fits(uint32_t id, bool extended)
{
    return id <= (extended ? CAN_ID_EXT_MAX : CAN_ID_STD_MAX);
}

bool isotp_init(isotp_t *tp, const isotp_config_t *config, const isotp_callbacks_t *callbacks,
                uint8_t *rx_buffer, uint16_t rx_size)
{
    bool valid = (tp != NULL) && (config != NULL) && (callbacks != NULL) && (rx_buffer != NULL) &&
                 (rx_size > 0U);

    if (valid) {
        valid = (callbacks->send_frame != NULL) && (callbacks->indication != NULL) &&
                id_fits(config->tx_id, config->extended_ids) &&
                id_fits(config->rx_id, config->extended_ids) && (config->tx_id != config->rx_id) &&
                (config->n_as_us > 0U) && (config->n_ar_us > 0U) && (config->n_bs_us > 0U) &&
                (config->n_cr_us > 0U) && isotp_stmin_valid(config->rx_stmin);
    }

    if (valid) {
        (void)memset(tp, 0, sizeof(*tp));
        tp->config = *config;
        tp->callbacks = *callbacks;
        tp->buffer = rx_buffer;
        tp->buffer_size = (rx_size > ISOTP_MAX_LENGTH) ? (uint16_t)ISOTP_MAX_LENGTH : rx_size;
        tp->tx.state = ISOTP_TX_IDLE;
        tp->rx.state = ISOTP_RX_IDLE;
        tp->ready = true;
    }

    return valid;
}

isotp_status_t isotp_send(isotp_t *tp, const uint8_t *data, uint16_t length, uint64_t now_us)
{
    isotp_status_t status = ISOTP_E_ARG;

    if ((tp == NULL) || !tp->ready) {
        /* refused: ISOTP_E_ARG */
    } else if (tp->in_callback) {
        tp->stats.reentry_refused++;
        status = ISOTP_E_BUSY;
    } else {
        expire(tp, now_us);
        if ((data == NULL) || (length == 0U) || (length > ISOTP_MAX_LENGTH)) {
            status = ISOTP_E_ARG;
        } else if (tp->tx.state != ISOTP_TX_IDLE) {
            status = ISOTP_E_BUSY;
        } else {
            tp->tx.data = data;
            tp->tx.length = length;
            tp->tx.offset = 0U;
            tp->tx.sn = 1U;
            tp->tx.bs = 0U;
            tp->tx.block_count = 0U;
            tp->tx.wft_count = 0U;
            tp->tx.cf_proven = false;
            tp->tx.since_us = now_us;
            tp->tx.frame = (length <= ISOTP_SF_DATA) ? ISOTP_FRAME_SF : ISOTP_FRAME_FF;
            tp->tx.state = ISOTP_TX_DUE;
            tp->stats.tx_requests++;
            status = ISOTP_OK;
        }
        service(tp, now_us);
    }

    return status;
}

bool isotp_on_frame(isotp_t *tp, const can_frame_t *frame, uint64_t now_us)
{
    bool processed = false;

    if (usable(tp) && (frame != NULL)) {
        /* A data frame of this connection carries exactly the format flag of its identifier:
         * a remote frame, the other format or an unknown flag is someone else's frame. */
        const uint8_t flags = tp->config.extended_ids ? CAN_FLAG_EXT : 0U;

        expire(tp, now_us);
        if ((frame->id != tp->config.rx_id) || (frame->flags != flags) ||
            (frame->dlc > CAN_MAX_DLC)) {
            tp->stats.ignored[ISOTP_IGNORED_FOREIGN]++;
        } else if (frame->dlc == 0U) {
            tp->stats.ignored[ISOTP_IGNORED_SHORT]++;
        } else {
            processed = on_valid_frame(tp, frame, now_us);
        }
        service(tp, now_us);
    }

    return processed;
}

/*
 * The confirmation is the oldest hand-out's (see CONFIRMATIONS in isotp.h). Before it is
 * matched, the hand-outs whose time has run out are given up: their confirmations will not come,
 * so the oldest of the others is the one confirmed. Nothing is handed out before the match,
 * since a new frame must not take a confirmation that was meant for an older one.
 */
bool isotp_on_tx_done(isotp_t *tp, uint64_t now_us)
{
    bool matched = false;

    if (usable(tp)) {
        expire(tp, now_us);
        if (tp->handout_count == 0U) {
            tp->stats.confirm_unmatched++;
        } else {
            const isotp_handout_t done = tp->handouts[0];

            matched = true;
            remove_handout(tp, 0U);
            tp->stats.confirmed++;
            if ((!done.rx_side) && (tp->tx.state == ISOTP_TX_SENT) &&
                (tp->tx.awaited == done.serial)) {
                tx_confirmed(tp, now_us);
            } else if (done.rx_side && (tp->rx.state == ISOTP_RX_FC_SENT) &&
                       (tp->rx.awaited == done.serial)) {
                rx_confirmed(tp, now_us);
            } else {
                /* a proven frame, or one of a transmission or reception that has ended */
            }
        }
        service(tp, now_us);
    }

    return matched;
}

void isotp_poll(isotp_t *tp, uint64_t now_us)
{
    if (usable(tp)) {
        service(tp, now_us);
    }
}

bool isotp_rx_release(isotp_t *tp, uint64_t now_us)
{
    bool released = false;

    if (usable(tp)) {
        expire(tp, now_us);
        if (tp->rx.held) {
            released = true;
            tp->rx.held = false;
            /* A reception that waits for the buffer gets its FC CTS now; a WAIT that is due but
             * not out yet is not needed any more. A WAIT that is out gets the CTS after its
             * confirmation (rx_confirmed). */
            if ((tp->rx.state == ISOTP_RX_WAIT_BUFFER) ||
                ((tp->rx.state == ISOTP_RX_FC_DUE) && (tp->rx.fc == ISOTP_FRAME_FC_WAIT))) {
                rx_take_buffer(tp);
            }
        }
        service(tp, now_us);
    }

    return released;
}

uint64_t isotp_next_deadline(const isotp_t *tp)
{
    uint64_t at_us = ISOTP_NO_DEADLINE;

    if ((tp != NULL) && tp->ready) {
        for (uint8_t i = 0U; i < tp->handout_count; i++) {
            at_us = earlier(at_us, tp->handouts[i].deadline_us);
        }
        if ((tp->tx.state == ISOTP_TX_WAIT_FC) || (tp->tx.state == ISOTP_TX_STMIN)) {
            at_us = earlier(at_us, tp->tx.timer_us);
        }
        if ((tp->rx.state == ISOTP_RX_CF) || (tp->rx.state == ISOTP_RX_WAIT_BUFFER)) {
            at_us = earlier(at_us, tp->rx.timer_us);
        }
    }

    return at_us;
}

bool isotp_tx_busy(const isotp_t *tp)
{
    return (tp != NULL) && tp->ready && (tp->tx.state != ISOTP_TX_IDLE);
}

bool isotp_rx_busy(const isotp_t *tp)
{
    return (tp != NULL) && tp->ready && rx_in_progress(tp);
}

bool isotp_rx_held(const isotp_t *tp)
{
    return (tp != NULL) && tp->ready && tp->rx.held;
}

const isotp_stats_t *isotp_stats(const isotp_t *tp)
{
    return (tp != NULL) ? &tp->stats : NULL;
}

uint32_t isotp_stmin_us(uint8_t stmin)
{
    uint32_t us = ISOTP_STMIN_RESERVED_US;

    if (stmin <= STMIN_MS_MAX) {
        us = (uint32_t)stmin * 1000U;
    } else if ((stmin >= STMIN_100US_LOW) && (stmin <= STMIN_100US_HIGH)) {
        us = ((uint32_t)stmin - 0xF0U) * 100U;
    } else {
        /* reserved: the longest STmin (the standard's rule for a sender) */
    }

    return us;
}

bool isotp_stmin_valid(uint8_t stmin)
{
    return (stmin <= STMIN_MS_MAX) || ((stmin >= STMIN_100US_LOW) && (stmin <= STMIN_100US_HIGH));
}

const char *isotp_result_name(isotp_result_t result)
{
    return ((uint32_t)result < (uint32_t)ISOTP_RESULTS) ? s_result_names[result] : "?";
}

const char *isotp_frame_kind_name(isotp_frame_kind_t kind)
{
    return ((uint32_t)kind < (uint32_t)ISOTP_FRAME_KINDS) ? s_frame_kind_names[kind] : "?";
}

const char *isotp_ignored_name(isotp_ignored_t reason)
{
    return ((uint32_t)reason < (uint32_t)ISOTP_IGNORED_REASONS) ? s_ignored_names[reason] : "?";
}
