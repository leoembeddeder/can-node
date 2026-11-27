/* SPDX-License-Identifier: MIT */
#include "diag.h"

#include <stddef.h>
#include <string.h>

#include "can_db.h"

/* A mark that diag_init() set up the instance. */
#define DIAG_READY 0x44494147U /* "DIAG" */

_Static_assert(DIAG_REQUEST_ID == CAN_DB_UDS_REQUEST_ID, "UDS_Request of the DBC");
_Static_assert(DIAG_RESPONSE_ID == CAN_DB_UDS_RESPONSE_ID, "UDS_Response of the DBC");
_Static_assert((DIAG_BLOCK_SIZE >= 1U) && (DIAG_BLOCK_SIZE <= 255U), "BS fits its byte, not 0");
_Static_assert(DIAG_RX_BUFFER <= ISOTP_MAX_LENGTH, "a buffer lib/isotp uses to its end");
_Static_assert(DIAG_RESPONSES == 2U, "a response in transmission and the one after it");
_Static_assert(DIAG_RESET_DEADLINE_US == (2U * DIAG_TIMEOUT_US), "the reset's deadline: 2 x N_As");
_Static_assert(DIAG_HANDOUTS >= ISOTP_PIPELINE_DEPTH, "a record for every frame in the pipeline");
_Static_assert(DIAG_N_CS_US >= ISOTP_STMIN_RESERVED_US, "the N_Cs target allows every STmin");

static bool ready(const diag_t *d)
{
    return (d != NULL) && (d->ready == DIAG_READY);
}

/* --------------------------------------------------------------------------- the records */

static void forget_oldest_handout(diag_t *d)
{
    d->handout_head = (d->handout_head + 1U) % DIAG_HANDOUTS;
    d->handout_count--;
}

/* A frame was handed out at d->call_us and queued with `stamp_ns`. A full record drops its
 * oldest entry, a frame given up long ago (DIAG_HANDOUTS is twice the pipeline). */
static void record_handout(diag_t *d, uint64_t stamp_ns)
{
    if (d->handout_count == DIAG_HANDOUTS) {
        forget_oldest_handout(d);
        d->stats.confirms_forgotten++;
    }

    diag_handout_t *entry = &d->handouts[(d->handout_head + d->handout_count) % DIAG_HANDOUTS];

    entry->stamp_ns = stamp_ns;
    entry->handout_us = d->call_us;
    d->handout_count++;
}

/* -------------------------------------------------------------------------- the responses */

/* The buffer lib/uds writes its next output into: the one after the queued ones, or none. */
static diag_response_t *free_response(diag_t *d)
{
    diag_response_t *slot = NULL;

    if (d->response_count < DIAG_RESPONSES) {
        slot = &d->responses[(d->response_head + d->response_count) % DIAG_RESPONSES];
    }

    return slot;
}

/* What lib/uds handed back with its output into `slot` (NULL: it wrote into the scratch). */
static void take_output(diag_t *d, diag_response_t *slot, const uds_output_t *out)
{
    bool reset_now = false;

    if (out->len != 0U) {
        d->stats.responses++;

        if (slot != NULL) {
            slot->len = out->len;
            slot->reset = out->reset;
            d->response_count++;
        } else {
            d->stats.responses_dropped++;
            reset_now = out->reset; /* nothing will be confirmed: no reason to wait */
        }
    } else {
        reset_now = out->reset; /* SPRMIB: the reset without a response */
    }

    if (out->reset) {
        d->stats.resets++;

        /* Due at the latest at this deadline, whatever is then still being sent (ECURESET); an
         * earlier accepted ECUReset keeps its own, which is earlier. */
        if (d->reset_deadline_us == UINT64_MAX) {
            d->reset_deadline_us = d->call_us + DIAG_RESET_DEADLINE_US;
        }
    }

    if (reset_now) {
        d->reset_due = true;
    }
}

/* The transmission of responses[response_head] has ended. */
static void end_transmission(diag_t *d)
{
    const diag_response_t *done = &d->responses[d->response_head];

    if (d->tx_result == ISOTP_RESULT_OK) {
        d->stats.responses_sent++;
    } else {
        d->stats.responses_failed++;
    }

    if (done->reset) {
        d->reset_due = true;
    }

    d->response_head = (d->response_head + 1U) % DIAG_RESPONSES;
    d->response_count--;
    d->sending = false;
    d->tx_ended = false;
}

/* After every call of lib/isotp or lib/uds: make an accepted ECUReset due at its deadline, end a
 * transmission whose confirmation came, and start the next response while the transport is free.
 * A start that lib/isotp ends at once (the first frame refused: N_ERROR, through the confirmation
 * inside isotp_send()) ends here too, so the loop runs at most once per buffer. No response is
 * started once a reset is due. */
static void settle(diag_t *d)
{
    if (d->call_us >= d->reset_deadline_us) {
        d->reset_due = true;
    }

    for (uint32_t i = 0U; i <= DIAG_RESPONSES; i++) {
        if (d->sending && d->tx_ended) {
            end_transmission(d);
        }

        if (!d->sending && (d->response_count > 0U) && !d->reset_due) {
            const diag_response_t *next = &d->responses[d->response_head];

            if (isotp_send(&d->tp, next->data, (uint16_t)next->len, d->call_us) == ISOTP_OK) {
                d->sending = true;
            } else {
                /* Refused (a transmission lib/isotp still runs, which only another caller of
                 * isotp_send() could have started): counted, the next one is tried. */
                d->tx_result = ISOTP_RESULT_ERROR;
                d->sending = true;
                d->tx_ended = true;
            }
        }
    }
}

/* ------------------------------------------------------------------- lib/isotp's callbacks */

static bool on_send_frame(void *context, const can_frame_t *frame)
{
    diag_t *d = (diag_t *)context;
    uint64_t stamp_ns = 0U;
    const bool taken = d->port.send(d->port.ctx, frame, &stamp_ns);

    if (taken) {
        d->stats.frames_out++;
        record_handout(d, stamp_ns);
    } else {
        d->stats.frames_refused++;
    }

    return taken;
}

/* A reception ended. A complete request goes to lib/uds at once, which copies what it keeps,
 * so the buffer goes back to lib/isotp when this returns (false). */
static bool on_indication(void *context, isotp_result_t result, const uint8_t *data,
                          uint16_t length)
{
    diag_t *d = (diag_t *)context;

    if (result == ISOTP_RESULT_OK) {
        diag_response_t *slot = free_response(d);
        uds_output_t out = {.len = 0U, .reset = false};

        d->stats.requests++;
        (void)uds_request(&d->uds, data, length, d->call_us,
                          (slot != NULL) ? slot->data : d->scratch, UDS_RESPONSE_MAX, &out);
        take_output(d, slot, &out);
    }

    return false;
}

static void on_confirm(void *context, isotp_result_t result)
{
    diag_t *d = (diag_t *)context;

    d->tx_result = result;
    d->tx_ended = true;
}

/* ------------------------------------------------------------------------------ interface */

bool diag_init(diag_t *d, const diag_port_t *port, const uds_backend_t *backend)
{
    bool ok = (d != NULL) && (port != NULL) && (port->send != NULL) && (backend != NULL);

    if (ok) {
        const isotp_config_t config = {.tx_id = DIAG_RESPONSE_ID,
                                       .rx_id = DIAG_REQUEST_ID,
                                       .n_as_us = DIAG_TIMEOUT_US,
                                       .n_ar_us = DIAG_TIMEOUT_US,
                                       .n_bs_us = DIAG_TIMEOUT_US,
                                       .n_cr_us = DIAG_TIMEOUT_US,
                                       .n_br_us = 0U,
                                       .n_cs_us = DIAG_N_CS_US,
                                       .extended_ids = false,
                                       .rx_bs = (uint8_t)DIAG_BLOCK_SIZE,
                                       .rx_stmin = (uint8_t)DIAG_STMIN,
                                       .rx_wft_max = 0U,
                                       .tx_wft_max = (uint8_t)DIAG_TX_WFT_MAX};
        const isotp_callbacks_t callbacks = {.send_frame = on_send_frame,
                                             .ff_indication = NULL,
                                             .indication = on_indication,
                                             .confirm = on_confirm,
                                             .context = d};

        (void)memset(d, 0, sizeof(*d));
        d->port = *port;
        d->reset_deadline_us = UINT64_MAX;
        d->tx_result = ISOTP_RESULT_OK;
        ok = isotp_init(&d->tp, &config, &callbacks, d->rx_buffer, (uint16_t)DIAG_RX_BUFFER) &&
             uds_init(&d->uds, backend);

        if (ok) {
            d->ready = DIAG_READY;
        }
    }

    return ok;
}

void diag_on_frame(diag_t *d, const can_frame_t *frame, uint64_t now_us)
{
    if (ready(d) && (frame != NULL)) {
        d->call_us = now_us;
        d->stats.frames_in++;
        (void)isotp_on_frame(&d->tp, frame, now_us);
        settle(d);
    }
}

void diag_on_tx_done(diag_t *d, uint64_t stamp_ns, uint64_t now_us)
{
    if (ready(d)) {
        d->call_us = now_us;
        d->stats.confirms++;

        /* Records older than the report are of frames the transmit path discarded unsent. */
        while ((d->handout_count > 0U) && (d->handouts[d->handout_head].stamp_ns < stamp_ns)) {
            forget_oldest_handout(d);
            d->stats.confirms_forgotten++;
        }

        if ((d->handout_count > 0U) && (d->handouts[d->handout_head].stamp_ns == stamp_ns)) {
            const uint64_t handout_us = d->handouts[d->handout_head].handout_us;

            forget_oldest_handout(d);

            /* lib/isotp gives a frame up at exactly handout + N_As (N_Ar for a flow control, the
             * same value here): from then on the report is not its. */
            if (now_us < (handout_us + DIAG_TIMEOUT_US)) {
                (void)isotp_on_tx_done(&d->tp, now_us);
                d->stats.confirms_passed++;
            } else {
                d->stats.confirms_late++;
            }
        } else {
            d->stats.confirms_unknown++;
        }

        settle(d);
    }
}

void diag_poll(diag_t *d, uint64_t now_us)
{
    if (ready(d)) {
        diag_response_t *slot;
        uds_output_t out = {.len = 0U, .reset = false};

        d->call_us = now_us;
        isotp_poll(&d->tp, now_us);
        settle(d);

        slot = free_response(d);
        (void)uds_poll(&d->uds, now_us, (slot != NULL) ? slot->data : d->scratch, UDS_RESPONSE_MAX,
                       &out);
        take_output(d, slot, &out);
        settle(d);
    }
}

uint64_t diag_next_deadline(const diag_t *d)
{
    uint64_t deadline = UINT64_MAX;

    if (ready(d)) {
        const uint64_t transport = isotp_next_deadline(&d->tp);
        const uint64_t server = uds_next_deadline(&d->uds);
        const uint64_t reset = d->reset_due ? UINT64_MAX : d->reset_deadline_us;

        deadline = (transport < server) ? transport : server;
        deadline = (reset < deadline) ? reset : deadline;
    }

    return deadline;
}

bool diag_reset_due(const diag_t *d)
{
    return ready(d) && d->reset_due;
}

const diag_stats_t *diag_stats(const diag_t *d)
{
    return ready(d) ? &d->stats : NULL;
}

const isotp_stats_t *diag_isotp_stats(const diag_t *d)
{
    return ready(d) ? isotp_stats(&d->tp) : NULL;
}

void diag_uds_stats(const diag_t *d, uds_stats_t *out)
{
    if (out != NULL) {
        if (ready(d)) {
            uds_get_stats(&d->uds, out);
        } else {
            (void)memset(out, 0, sizeof(*out));
        }
    }
}

uint8_t diag_session(const diag_t *d)
{
    return ready(d) ? uds_session(&d->uds) : (uint8_t)UDS_SESSION_DEFAULT;
}
