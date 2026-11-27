/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The node's diagnostic connection: lib/isotp (ISO 15765-2) on UDS_Request 0x7E0 and
 * UDS_Response 0x7E8 in front of lib/uds (ISO 14229-1), and what joins the two to each other and
 * to the CAN tasks. Pure C over lib/isotp and lib/uds: no operating system, time passed in as a
 * monotonic count of microseconds, frames out through the caller's port. The diagnostic task
 * owns a diag_t and makes every call on it (app.c, THE DIAGNOSTIC TASK).
 *
 * THE TRANSPORT'S PARAMETERS (lib/isotp's isotp_config_t), the node's choice:
 *
 *   identifiers      0x7E8 out, 0x7E0 in, 11 bits, normal addressing, physical only
 *   N_As N_Ar N_Bs N_Cr   DIAG_TIMEOUT_US, 1000 ms: the standard's values; N_As and N_Ar also
 *                    equal the transmit queue's event timeout (can_io.c), which discards an event
 *                    frame that has not gone out in that time
 *   BS               DIAG_BLOCK_SIZE, 16: the tester sends at most 16 consecutive frames before it
 *                    waits for the next flow control, which the node sends only once the
 *                    diagnostic task has taken the 16th out of its queue. So the queue between
 *                    the receive task and the diagnostic task (app.c, DIAG_RX_QUEUE_LENGTH) never
 *                    holds more than one block of a tester that keeps to the protocol, however
 *                    late the diagnostic task runs: no consecutive frame can be lost there by
 *                    timing. The receive task above it takes every frame off the controller
 *                    (task_table.c, can_rx), which the receive FIFO's analysis of phase 1 shows
 *   STmin            0x00: the node needs no gap between consecutive frames (above)
 *   receive buffer   DIAG_RX_BUFFER, UDS_REQUEST_MAX (1026 bytes), the longest request lib/uds
 *                    takes (a TransferData block); a longer message is answered with FC OVFLW
 *   N_WFTmax, N_Br   0: the node never keeps the receive buffer (A REQUEST), so it never waits
 *                    for it and never sends FC WAIT
 *   WAITs accepted   DIAG_TX_WFT_MAX, 4 in a row, from a tester that is not ready for a response
 *   N_Cs target      DIAG_N_CS_US, 150 ms: at least the longest STmin a tester can ask for
 *                    (127 ms), so that only a consecutive frame the node itself delays is late
 *
 * A REQUEST. The indication of a complete message goes straight to uds_request(), which keeps a
 * copy of the request it has to keep (lib/uds: up to UDS_REQUEST_MAX bytes), so the indication
 * gives the receive buffer back at once (returns false). lib/uds's answer, if it has one, waits
 * in a response buffer until the transport is free to send it.
 *
 * RESPONSES (DIAG_RESPONSES buffers). lib/isotp does not copy what it sends: a buffer stays
 * untouched until the transmission's confirmation. A response that lib/uds produces while
 * another is being sent (the final response after a responsePending that is still on its way, or
 * the answer to a request of a tester that did not wait for the response before) goes into the
 * second buffer and is sent when the first is confirmed. A response that finds both buffers in
 * use is counted in responses_dropped and not sent; a tester that keeps to the protocol never
 * makes that happen (one request at a time, at most one responsePending before the response).
 *
 * CONFIRMATIONS (lib/isotp, CONFIRMATIONS). The node's transmit-done report of a 0x7E8 frame
 * carries the time the frame was queued with (can_io_on_tx_done), and every frame handed out is
 * recorded with that time and the time of its hand-out (DIAG_HANDOUTS records). A report is
 * passed to lib/isotp only if it is for the oldest recorded frame (records of older frames are
 * those of frames the transmit queue discarded unsent: dropped, counted in confirms_forgotten)
 * and only while that frame's N_As has not run out. Otherwise lib/isotp has given the frame up
 * already, and a confirmation passed on would be taken for the next frame: it is dropped
 * (confirms_late), which can happen only to a frame whose attempt was on the wire when the
 * transmit queue recalled it at its event timeout, or when the diagnostic task took the report
 * later than N_As. A report for no recorded frame is dropped as well (confirms_unknown).
 *
 * ECURESET. lib/uds accepts it and hands back its response with uds_output_t.reset. The reset is
 * due (diag_reset_due()) at the first of these:
 *   - at once, if there is no response (SPRMIB) or it found no free buffer;
 *   - when the transmission of its response has ended, with N_OK or not;
 *   - DIAG_RESET_DEADLINE_US (2 x N_As, 2 s) after the call that accepted it, in the first call
 *     at or after that time (the deadline is one of diag_next_deadline()'s), whatever is still
 *     being sent then.
 * ISO 14229-1 asks for the positive response before the reset and sets no time for a hardReset.
 * The deadline is the node's: without it the reset would wait for the whole transmission ahead
 * of its response, whose length the tester's flow control sets (up to DIAG_TX_WFT_MAX WAITs per
 * block, BS, STmin), and a reset the tester asked for is not to be held off by the tester's own
 * flow control. For a tester that waits for each response the deadline never comes first: ahead
 * of the positive response (51 01) there is at most the last frame of the previous response,
 * handed out before the request (the tester has received it), so that transmission ends within
 * N_As of the request (TIMERS in isotp.h), and the 51 01, a single frame, ends within N_As of its
 * hand-out. A tester that did not wait for an earlier response (RESPONSES) may get the reset with
 * that response cut and without its 51 01, if the earlier one is still being sent at the
 * deadline, or with its 51 01 handed out and cut, if the earlier one ended shortly before the
 * deadline. No response is started once the reset is due; the caller makes it
 * (plat_reset(PLAT_RESET_REQUESTED)) and handles nothing more.
 *
 * STATISTICS (diag_stats_t, and lib/isotp's and lib/uds's own: diag_isotp_stats(),
 * diag_uds_stats()). Counters of 32 bits that wrap. These hold between two calls:
 *   confirms = confirms_passed + confirms_late + confirms_unknown
 *   responses = responses_sent + responses_failed + responses_dropped + (responses waiting)
 *
 * CONTEXT. The owner of the diag_t (the diagnostic task) makes every call; nothing locks.
 */
#ifndef FCN_DIAG_H
#define FCN_DIAG_H

#include <stdbool.h>
#include <stdint.h>

#include "can/can_frame.h"
#include "isotp/isotp.h"
#include "uds/uds.h"

/** UDS_Request (tester to node) and UDS_Response (node to tester), dbc/fcn.dbc. */
#define DIAG_REQUEST_ID  0x7E0U
#define DIAG_RESPONSE_ID 0x7E8U

/** The node's ISO-TP parameters (see THE TRANSPORT'S PARAMETERS). */
#define DIAG_TIMEOUT_US ISOTP_TIMEOUT_US
#define DIAG_BLOCK_SIZE 16U
#define DIAG_STMIN      0x00U
#define DIAG_RX_BUFFER  UDS_REQUEST_MAX
#define DIAG_TX_WFT_MAX 4U
#define DIAG_N_CS_US    150000U

/** Response buffers (see RESPONSES). */
#define DIAG_RESPONSES 2U

/** An accepted ECUReset is due at the latest this long after the call that accepted it
 *  (ECURESET): 2 x N_As (DIAG_TIMEOUT_US), N_As for the last frame of a previous response, which
 *  may still be unconfirmed, then N_As for the ECUReset's response. */
#define DIAG_RESET_DEADLINE_US 2000000U

/** Frames handed out that are recorded for their confirmation: lib/isotp's pipeline, and as
 *  many again for frames it gave up whose record has not been dropped yet. */
#define DIAG_HANDOUTS (2U * ISOTP_PIPELINE_DEPTH)

/** What the core needs from the node. */
typedef struct {
    void *ctx; /**< passed to `send` as it is */
    /** Hand a frame of DIAG_RESPONSE_ID to the transmit path: true if it was taken, with
     *  `*stamp_ns` the time it was queued with, which diag_on_tx_done() gets back with its
     *  report (can_io_send_event_stamped()); false if it was refused. */
    bool (*send)(void *ctx, const can_frame_t *frame, uint64_t *stamp_ns);
} diag_port_t;

/** The core's counters (see STATISTICS). */
typedef struct {
    uint32_t frames_in;          /**< frames given to diag_on_frame() */
    uint32_t frames_out;         /**< frames the port took */
    uint32_t frames_refused;     /**< frames the port refused (the transmit queue's FIFO full) */
    uint32_t confirms;           /**< transmit-done reports given to diag_on_tx_done() */
    uint32_t confirms_passed;    /**< of those, passed to lib/isotp */
    uint32_t confirms_late;      /**< of those, for a frame whose N_As had run out: dropped */
    uint32_t confirms_unknown;   /**< of those, for no recorded frame: dropped */
    uint32_t confirms_forgotten; /**< records of frames that were never reported, dropped */
    uint32_t requests;           /**< messages received and given to lib/uds */
    uint32_t responses;          /**< responses lib/uds produced */
    uint32_t responses_sent;     /**< of those, confirmed with N_OK */
    uint32_t responses_failed;   /**< ended with another N_Result, or refused by lib/isotp */
    uint32_t responses_dropped;  /**< found both buffers in use: not sent */
    uint32_t resets;             /**< ECUResets accepted */
} diag_stats_t;

/* Private to diag.c; in the header so that a diag_t can be allocated statically. */
typedef struct {
    uint8_t data[UDS_RESPONSE_MAX];
    uint32_t len;
    bool reset; /* an ECUReset's response: the reset is due when its transmission ends */
} diag_response_t;

typedef struct {
    uint64_t stamp_ns;   /* the time the transmit path queued it with */
    uint64_t handout_us; /* the time of the lib/isotp call that handed it out */
} diag_handout_t;

typedef struct {
    diag_port_t port;
    isotp_t tp;
    uds_server_t uds;
    uint8_t rx_buffer[DIAG_RX_BUFFER];
    diag_response_t responses[DIAG_RESPONSES]; /* a FIFO of `response_count` from `response_head` */
    uint8_t scratch[UDS_RESPONSE_MAX];         /* lib/uds's output when no buffer is free */
    diag_handout_t handouts[DIAG_HANDOUTS];    /* a FIFO of `handout_count` from `handout_head` */
    uint64_t call_us;                          /* the time of the call in progress */
    uint64_t reset_deadline_us; /* an accepted ECUReset is due then at the latest; or UINT64_MAX */
    uint32_t response_head;
    uint32_t response_count;
    uint32_t handout_head;
    uint32_t handout_count;
    uint32_t ready;           /* set by diag_init() */
    isotp_result_t tx_result; /* of the confirmation that came in the call in progress */
    bool sending;             /* responses[response_head] is being sent */
    bool tx_ended;            /* the confirmation of that transmission came in this call */
    bool reset_due;           /* an accepted ECUReset is to be made */
    diag_stats_t stats;
} diag_t;

/**
 * Prepare `d`: lib/isotp with the node's parameters (THE TRANSPORT'S PARAMETERS) and lib/uds with
 * `backend` (copied; see uds_init()), every counter 0. `port` is copied; its `send` must be set.
 * The memory of `d` may hold anything before. Returns false, with `d` unusable, for a NULL
 * pointer or a `port` without `send`, or if lib/isotp or lib/uds refuses. The owner, before any
 * other call.
 */
bool diag_init(diag_t *d, const diag_port_t *port, const uds_backend_t *backend);

/**
 * A frame with DIAG_REQUEST_ID arrived; `now_us` is the time of the call (on the node: when the
 * diagnostic task takes the frame). Frames of another identifier are ignored by lib/isotp and
 * counted there. A complete request goes to lib/uds, and its response is sent as soon as the
 * transport is free. Does nothing for an unprepared `d` or a NULL frame. The owner.
 */
void diag_on_frame(diag_t *d, const can_frame_t *frame, uint64_t now_us);

/**
 * The transmit path reports a frame of DIAG_RESPONSE_ID sent: `stamp_ns` is the time it was
 * queued with (diag_port_t.send), `now_us` the time of the call. Passed to lib/isotp or dropped
 * as CONFIRMATIONS says. Does nothing for an unprepared `d`. The owner, with the reports in the
 * order the transmit path made them.
 */
void diag_on_tx_done(diag_t *d, uint64_t stamp_ns, uint64_t now_us);

/**
 * Time has passed, or a backend operation of lib/uds may have finished (on the node: the flash
 * task reports a job done, the health task a clear): lib/isotp's timers and lib/uds's pending
 * request at `now_us`. Call it no later than
 * diag_next_deadline() and whenever a backend operation may have finished; more often is
 * harmless. Does nothing for an unprepared `d`. The owner.
 */
void diag_poll(diag_t *d, uint64_t now_us);

/** The earliest moment diag_poll() has time-driven work: the earliest of lib/isotp's and
 *  lib/uds's deadlines and that of an accepted ECUReset not yet due (ECURESET); UINT64_MAX if
 *  none has one (or for an unprepared `d`). The owner. */
uint64_t diag_next_deadline(const diag_t *d);

/** Whether an accepted ECUReset is now to be made (ECURESET); at its deadline it becomes due in
 *  the first call at or after it. False for an unprepared `d`. The owner, after each call. */
bool diag_reset_due(const diag_t *d);

/** The core's counters; NULL for an unprepared `d`. The owner. */
const diag_stats_t *diag_stats(const diag_t *d);

/** lib/isotp's counters; NULL for an unprepared `d`. The owner. */
const isotp_stats_t *diag_isotp_stats(const diag_t *d);

/** lib/uds's counters (uds_get_stats()); zeros for an unprepared `d`. The owner. */
void diag_uds_stats(const diag_t *d, uds_stats_t *out);

/** The active UDS session (uds_session()). The owner. */
uint8_t diag_session(const diag_t *d);

#endif /* FCN_DIAG_H */
