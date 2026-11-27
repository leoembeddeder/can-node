/* SPDX-License-Identifier: MIT */
/**
 * @file
 * ISO 15765-2:2016 (ISO-TP, DoCAN transport layer) for one connection on classical CAN: messages
 * of 1 to 4095 bytes cut into frames of 8 bytes and put together again, with flow control and
 * the protocol's timers. Pure logic with time passed in: no operating system, no clock of its
 * own, no allocation; frames leave through a callback, and the caller reports frames received
 * and frames confirmed sent.
 *
 * WHAT IT IS. One isotp_t is one connection between two nodes: frames it sends carry
 * config.tx_id, frames it accepts carry config.rx_id (on the node 0x7E8 and 0x7E0). It is a
 * sender and a receiver at the same time, and the two are independent, as in the standard: a
 * message can be received while one is being sent. Normal addressing (the whole first byte of
 * every frame is protocol control information, the N_PCI), physical addressing, frames of
 * exactly 8 bytes (CAN_DL 8). Rules are named below as the standard names them; clause numbers
 * are not given because they were not checked against the text of the 2016 edition.
 *
 * NOT IN THIS MODULE. CAN FD (CAN_DL above 8, the escape sequence of a single frame), extended
 * and mixed addressing (an address byte before the N_PCI), functional addressing (one request
 * to many receivers, single frames only), messages longer than 4095 bytes (a first frame with
 * the 32-bit escape length is always answered with OVFLW, see RECEPTION), and the transmission
 * of frames shorter than 8 bytes (every frame is padded, see FRAMES).
 *
 * FRAMES (the N_PCI, byte by byte; "d" is message data, "p" the padding byte ISOTP_PADDING,
 * 0xCC). Every frame this module sends has DLC 8: bytes after the last meaningful one are
 * padding.
 *
 *   type                byte 0          byte 1       bytes 2..5      bytes 6..7
 *   SingleFrame  SF     0x0L, L 1..7    d or p       d or p          d or p
 *   FirstFrame   FF     0x1H            LL           d d d d         d d
 *   FF, escape          0x10            0x00         FF_DL, 32 bits  d d
 *   ConsecutiveFrame CF 0x2N, N = SN    d or p       d or p          d or p
 *   FlowControl  FC     0x3S, S = FS    BS           STmin, then p   p p
 *
 *   L    SF_DL, the length of the message in a single frame: 1..7 (0 is the CAN FD escape and
 *        8..15 do not fit, so neither is a valid SF on classical CAN).
 *   HLL  FF_DL, 12 bits, the length of the whole message: 8..4095 (a shorter message is an SF).
 *        FF_DL 0 is the escape: the length follows as 32 bits, most significant byte first, for
 *        messages longer than 4095 bytes; the escape FF carries 2 data bytes, the normal FF 6.
 *   N    SN, the sequence number: the FF counts as 0, the first CF is 1, then 2, ..., 15, 0, 1.
 *   S    FS, the flow status: 0 ContinueToSend (CTS), 1 WAIT, 2 Overflow (OVFLW); 3..15 are
 *        reserved.
 *   BS   block size: CFs the sender may send before it waits for the next FC; 0: no further FC.
 *   STmin  separation time: the least time between two CFs (ISOTP_STMIN below).
 *   Types 0x4..0xF of the high nibble of byte 0 are reserved.
 *
 * A message of 1..7 bytes goes as one SF; one of 8..4095 bytes as an FF with its first 6 bytes
 * and then CFs of 7 bytes each, the last one with what is left. A message of 0 bytes has no
 * encoding on classical CAN (SF_DL 0 is reserved), so isotp_send() refuses length 0 and the
 * receiver ignores an SF with SF_DL 0.
 *
 * RECEIVED FRAMES ARE CHECKED before they are acted on, and a frame that fails a check is
 * ignored: it changes nothing but a counter of isotp_stats_t.ignored (isotp_ignored_t says
 * which; the call still acts on the timers of its time, see TIMERS):
 *   - not for this connection: another identifier or format, a remote frame, DLC above 8;
 *   - shorter than its N_PCI needs (the received DLC is the length): DLC 0; an SF whose DLC is
 *     below 1 + SF_DL; an FF whose DLC is below 8; a CF whose DLC is below 1 + the data it has
 *     to carry (7, or what is left of the message for the last CF); an FC whose DLC is below 3.
 *     Longer frames are accepted and the bytes after the N_PCI's data are not looked at;
 *   - a length the protocol does not allow: SF_DL 0 or 8..15; FF_DL 1..7; an escape FF whose
 *     32-bit FF_DL is 4095 or less (the escape is only for longer messages);
 *   - a reserved N_PCI type (0x4..0xF);
 *   - a CF that no reception expects now, an FC that no transmission waits for (see UNEXPECTED
 *     PDUS).
 *
 * TRANSMISSION (the sender). isotp_send() takes a message of 1..ISOTP_MAX_LENGTH bytes; the data
 * is not copied and must stay unchanged until the confirmation (callback `confirm`, the
 * standard's N_USData.confirm) has been given. One message at a time: a second call while one
 * is being sent is refused. The sender hands out the SF or the FF at once. After an FF it waits
 * for an FC (timer N_Bs), and then:
 *   CTS    takes BS and STmin from this FC and sends a block: BS CFs (all that are left when BS is
 *          0), each no earlier than STmin after the confirmation of the one before; after the
 *          last CF of a block it waits for the next FC (N_Bs). The values of every CTS apply to
 *          the block after it. The first CF after the FF goes out at once; the first CF of a later
 *          block no earlier than STmin after the confirmation of the previous CF (after the FC's
 *          arrival, if the FC came before that confirmation, see CONFIRMATIONS), so that STmin
 *          also holds across a block boundary.
 *   WAIT   restarts N_Bs and keeps waiting. More than config.tx_wft_max WAITs in a row end the
 *          transmission with ISOTP_RESULT_WFT_OVRN: the standard bounds WAITs only at the
 *          receiver (N_WFTmax), and a sender that trusts that bound waits for ever behind a
 *          receiver that never stops sending WAIT. The result N_WFT_OVRN is defined for the
 *          receiver; using it here is this module's extension.
 *   OVFLW  ends the transmission with ISOTP_RESULT_BUFFER_OVFLW, also at a later block boundary
 *          (the receiver says it cannot store the message; going on would be pointless).
 *   reserved FS  ends the transmission with ISOTP_RESULT_INVALID_FS.
 * The transmission ends with ISOTP_RESULT_OK when the SF, or the last CF, is confirmed.
 *
 * STMIN (the FC's byte 2, isotp_stmin_us()): 0x00..0x7F are 0..127 ms; 0xF1..0xF9 are 100..900
 * us; every other value is reserved, and a sender that receives one uses the longest STmin,
 * 127 ms, for that block (the standard's rule), counted in stats.stmin_reserved. This module's
 * own STmin (config.rx_stmin) has to be a valid value.
 *
 * RECEPTION (the receiver). The message is put together in the caller's buffer (isotp_init()),
 * of which at most ISOTP_MAX_LENGTH bytes are used.
 *   SF     delivered at once through `indication` (N_USData.indication) with ISOTP_RESULT_OK.
 *   FF     the start of a message of FF_DL bytes. One that does not fit the buffer, and every
 *          escape FF (FF_DL above 4095), is answered with an FC OVFLW and nothing is received
 *          (stats.rx_overflow). Otherwise `ff_indication` (N_USData_FF.indication) is given and
 *          the receiver answers with an FC CTS carrying config.rx_bs and config.rx_stmin, then
 *          takes CFs, each one checked for the expected SN; after every config.rx_bs CFs (never
 *          when rx_bs is 0, nor after the last CF) it sends the next FC CTS. A CF with a wrong
 *          SN ends the reception with ISOTP_RESULT_WRONG_SN. When the last CF is in, the
 *          message is delivered with ISOTP_RESULT_OK.
 * The buffer and WAIT: `indication` returns true to keep the buffer (zero copy: the data stays
 * where it is until isotp_rx_release()) or false to give it back at once. While the buffer is
 * kept, an SF cannot be stored and is dropped (stats.rx_sf_busy), and an FF that fits starts a
 * reception that waits for the buffer: its first FC goes out when the buffer is released, but
 * no later than config.n_br_us after the FF; if it is still kept then, the receiver sends an FC
 * WAIT, and again n_br_us after each WAIT's confirmation, at most config.rx_wft_max WAITs (the
 * standard's N_WFTmax; 0: no WAIT at all). When one more would be needed, the reception ends
 * with ISOTP_RESULT_WFT_OVRN and nothing more is sent. The 6 data bytes of the FF wait in the
 * isotp_t meanwhile. An FC WAIT or OVFLW carries BS 0 and STmin 0; the sender ignores both.
 *
 * TIMERS. Every time is a uint64_t count of microseconds on the caller's monotonic clock. A timer
 * set at time t with a value of v has run out at t + v exactly (not 1 us earlier): a call with
 * that time or a later one acts on it before it does anything else, so a frame that arrives at
 * the very moment a timer runs out is too late. The standard's values (its table of network
 * layer timing parameters) are ISOTP_TIMEOUT_US, 1000 ms, for the four timeouts:
 *   N_As   sender: from the hand-out of an SF, FF or CF to its confirmation. Runs out: the
 *          transmission ends with ISOTP_RESULT_TIMEOUT_A.            config.n_as_us
 *   N_Ar   receiver: from the hand-out of an FC CTS or WAIT to its confirmation. Runs out: the
 *          reception ends with ISOTP_RESULT_TIMEOUT_A.               config.n_ar_us
 *   N_Bs   sender: from the confirmation of the FF, or of the last CF of a block, or from the
 *          arrival of an FC WAIT, to the arrival of the next FC. Runs out:
 *          ISOTP_RESULT_TIMEOUT_BS.                                  config.n_bs_us
 *   N_Cr   receiver: from the confirmation of an FC CTS, or from the arrival of a CF, to the
 *          arrival of the next CF. Runs out: ISOTP_RESULT_TIMEOUT_CR.  config.n_cr_us
 * and two performance requirements, which are no timeouts:
 *   N_Br   receiver: the time until it hands out the next FC. Without WAIT the FC goes out in
 *          the call that received the FF or the last CF of a block. While the buffer is kept it
 *          is the receiver's wait described above, config.n_br_us. The standard asks for
 *          N_Br + N_Ar < 0.9 * N_Bs of the sender. Measured: stats.n_br_max_us, and FCs later
 *          than n_br_us are counted in stats.n_br_late.
 *   N_Cs   sender: the time from the confirmation of a CF (or the arrival of the FC) until it
 *          hands out the next CF, STmin included. The standard asks for N_Cs + N_As < 0.9 *
 *          N_Cr of the receiver. CFs go out in the first call at or after the time STmin allows,
 *          so N_Cs is as good as the caller's calls; measured: stats.n_cs_max_us, and CFs later
 *          than config.n_cs_us are counted in stats.n_cs_late (choose n_cs_us of at least
 *          127 ms, the longest STmin, so that STmin alone cannot make a CF late).
 * isotp_next_deadline() is the earliest moment a timer runs out or a CF becomes due.
 *
 * UNEXPECTED PDUS (the standard's table of the handling of unexpected arrival of N_PDUs, with
 * reception and transmission independent). A reception is in progress from a valid FF that fits
 * until its message is delivered or the reception ends; a transmission from isotp_send() until
 * the confirmation:
 *   SF, FF   a reception in progress ends with ISOTP_RESULT_UNEXP_PDU, then the frame is
 *            processed as the start of a new reception. A transmission in progress goes on.
 *   CF       processed (SN checked) by a reception that expects CFs now: after its FC CTS was
 *            handed out and until the end of the block; ignored otherwise (no reception, a
 *            reception whose FC CTS is not out yet, or one that waits for the buffer).
 *   FC       processed by a transmission that waits for an FC; ignored otherwise.
 *   reserved type  ignored.
 * Only a valid frame counts: an ignored frame (above) does not end a reception.
 *
 * CONFIRMATIONS. isotp_on_tx_done() reports that a frame this instance handed out has been sent
 * (the transmit-done event of the node). It carries no identity, so the caller must confirm the
 * frames in the order they were handed out (a transmit path that keeps the connection's frames
 * in a FIFO, as lib/can's transmit queue does with event frames, confirms them in that order),
 * and must never confirm a frame after its N_As or N_Ar (counted from the hand-out) has run out:
 * the module has then given up on that frame and dropped it, and a late confirmation would be
 * taken for the next one. A transmit path that discards a frame it could not send by then, as
 * lib/can's transmit queue does with its event timeout, keeps this rule. The order of a
 * confirmation and of a frame received from the peer is NOT required: an answer of the peer
 * proves that the frame it answers was sent. So an FC that arrives before the confirmation of
 * the FF, or of the last CF of a block, is processed as if that frame had been confirmed when the
 * FC arrived, and a CF that arrives before the confirmation of the receiver's FC CTS likewise;
 * the confirmation, when it comes, then changes nothing. At most ISOTP_PIPELINE_DEPTH frames are
 * handed out and not confirmed at a time; a frame that is due while that many are waits for the
 * next confirmation, or for the frames to be given up.
 *
 * WHAT THE CALLER CALLS, AND WHEN.
 *   isotp_init()          once, before anything else.
 *   isotp_on_frame()      for every frame received with config.rx_id (others are ignored).
 *   isotp_on_tx_done()    for every frame the `send_frame` callback took, when it has been sent.
 *   isotp_poll()          at isotp_next_deadline() or later; calling it more often is harmless.
 *   isotp_send()          to send a message; isotp_rx_release() to give back a kept buffer.
 * Every call takes the current time and first acts on the timers that have run out by then;
 * frames that are due go out in the same call. Right after a call with time t, the deadline is
 * later than t, so a caller that sleeps until the deadline (or until a frame or a confirmation
 * arrives) does not spin.
 *
 * CALLBACKS (isotp_callbacks_t) run inside the calls above, in the caller's context, and must
 * not call any function of this module for the same instance except the const queries
 * (isotp_stats, isotp_next_deadline, isotp_tx_busy, isotp_rx_busy, isotp_rx_held); such a call is
 * refused and counted in stats.reentry_refused. A callback that wants to send (a response to a
 * request, say) records that, and the caller sends after the call has returned.
 *
 * STATISTICS (isotp_stats_t). Counters of 32 bits that wrap. These hold at every moment between
 * two calls, and the unit tests check them after every step:
 *   frames given to isotp_on_frame() (not refused) = sum(frames_in) + sum(ignored)
 *   tx_requests = sum(tx_results) + (1 while a transmission is in progress)
 *   frames_in[FF] = ff_indications + rx_overflow
 *   frames_in[SF] - rx_sf_too_long - rx_sf_busy + ff_indications
 *       = sum(rx_results) + (1 while a reception is in progress)
 *   rx_overflow = frames_out[FC_OVFLW] + fc_dropped + (1 while an OVFLW is due)
 *   sum(frames_out) = confirmed + unconfirmed + outstanding
 *
 * CONTEXT. The caller owns the isotp_t and serialises every call on it (on the node, the
 * diagnostic task owns it). Under that condition any task may make them. Every call takes a
 * bounded time: a few frames' worth of work, at most ISOTP_PIPELINE_DEPTH entries looked at, at
 * most 7 bytes copied per frame.
 */
#ifndef FCN_ISOTP_H
#define FCN_ISOTP_H

#include <stdbool.h>
#include <stdint.h>

#include "can/can_frame.h"

/** The longest message: FF_DL has 12 bits (longer ones need the escape FF, not supported). */
#define ISOTP_MAX_LENGTH 4095U

/** Data bytes of a single frame, of a first frame and of a consecutive frame (classical CAN,
 *  normal addressing). */
#define ISOTP_SF_DATA 7U
#define ISOTP_FF_DATA 6U
#define ISOTP_CF_DATA 7U

/** The byte that fills every frame this module sends up to 8 bytes (lead decision 2). */
#define ISOTP_PADDING 0xCCU

/** The standard's value of the timeouts N_As, N_Ar, N_Bs and N_Cr: 1000 ms. */
#define ISOTP_TIMEOUT_US 1000000U

/** The STmin a sender uses instead of a reserved value: 0x7F, 127 ms. */
#define ISOTP_STMIN_RESERVED_US 127000U

/** Frames handed out and not yet confirmed, at most (see CONFIRMATIONS). */
#define ISOTP_PIPELINE_DEPTH 4U

/** Returned by isotp_next_deadline() when nothing is timed. */
#define ISOTP_NO_DEADLINE UINT64_MAX

/** The outcome of a transmission or a reception: the standard's N_Result. */
typedef enum {
    ISOTP_RESULT_OK = 0,       /**< N_OK: the message went through */
    ISOTP_RESULT_TIMEOUT_A,    /**< N_TIMEOUT_A: N_As (sender) or N_Ar (receiver) ran out */
    ISOTP_RESULT_TIMEOUT_BS,   /**< N_TIMEOUT_Bs: sender, no FC in time */
    ISOTP_RESULT_TIMEOUT_CR,   /**< N_TIMEOUT_Cr: receiver, no CF in time */
    ISOTP_RESULT_WRONG_SN,     /**< N_WRONG_SN: receiver, a CF with an unexpected SN */
    ISOTP_RESULT_INVALID_FS,   /**< N_INVALID_FS: sender, an FC with a reserved FS */
    ISOTP_RESULT_UNEXP_PDU,    /**< N_UNEXP_PDU: receiver, an SF or FF during a reception */
    ISOTP_RESULT_WFT_OVRN,     /**< N_WFT_OVRN: receiver, N_WFTmax WAITs were not enough;
                                    sender, more than tx_wft_max WAITs (an extension) */
    ISOTP_RESULT_BUFFER_OVFLW, /**< N_BUFFER_OVFLW: sender, the receiver answered OVFLW */
    ISOTP_RESULT_ERROR,        /**< N_ERROR: the send_frame callback refused a frame */
    ISOTP_RESULTS              /**< the number of results, not a result */
} isotp_result_t;

/** Kinds of frame, for the counters of frames in and out. */
typedef enum {
    ISOTP_FRAME_SF = 0,
    ISOTP_FRAME_FF, /**< escape FFs included */
    ISOTP_FRAME_CF,
    ISOTP_FRAME_FC_CTS,
    ISOTP_FRAME_FC_WAIT,
    ISOTP_FRAME_FC_OVFLW,
    ISOTP_FRAME_FC_RESERVED, /**< an FC with a reserved FS: received only, never sent */
    ISOTP_FRAME_KINDS        /**< the number of kinds, not a kind */
} isotp_frame_kind_t;

/** Why a received frame was ignored (see RECEIVED FRAMES ARE CHECKED). */
typedef enum {
    ISOTP_IGNORED_FOREIGN = 0,   /**< another identifier or format, a remote frame, DLC > 8 */
    ISOTP_IGNORED_SHORT,         /**< DLC below what the N_PCI needs */
    ISOTP_IGNORED_LENGTH,        /**< SF_DL 0 or 8..15, FF_DL 1..7, escape FF_DL <= 4095 */
    ISOTP_IGNORED_RESERVED_TYPE, /**< N_PCI type 0x4..0xF */
    ISOTP_IGNORED_UNEXPECTED_CF, /**< a CF no reception expects now */
    ISOTP_IGNORED_UNEXPECTED_FC, /**< an FC no transmission waits for */
    ISOTP_IGNORED_REASONS        /**< the number of reasons, not a reason */
} isotp_ignored_t;

/** What isotp_send() answers. */
typedef enum {
    ISOTP_OK = 0, /**< the message was taken; its confirmation follows through `confirm` */
    ISOTP_E_ARG,  /**< a NULL pointer, length 0 or above ISOTP_MAX_LENGTH, an uninitialised
                        instance */
    ISOTP_E_BUSY  /**< a transmission is in progress, or the call came from a callback */
} isotp_status_t;

/**
 * The parameters of a connection. isotp_init() refuses a configuration with a value outside the
 * ranges given here.
 */
typedef struct {
    uint32_t tx_id;   /**< identifier of the frames sent: 0..0x7FF, or 0..0x1FFFFFFF if extended */
    uint32_t rx_id;   /**< identifier of the frames accepted; same range, not tx_id */
    uint32_t n_as_us; /**< N_As, 1..UINT32_MAX us; the standard: ISOTP_TIMEOUT_US */
    uint32_t n_ar_us; /**< N_Ar, 1..UINT32_MAX us; the standard: ISOTP_TIMEOUT_US */
    uint32_t n_bs_us; /**< N_Bs, 1..UINT32_MAX us; the standard: ISOTP_TIMEOUT_US */
    uint32_t n_cr_us; /**< N_Cr, 1..UINT32_MAX us; the standard: ISOTP_TIMEOUT_US */
    /** N_Br: the longest the receiver waits for a kept buffer before it sends an FC WAIT, and
     *  the time between WAITs (see RECEPTION), 0..UINT32_MAX us. 0: no wait, a WAIT (or the end
     *  of the reception) at once. The standard asks for N_Br + N_Ar < 0.9 * N_Bs. */
    uint32_t n_br_us;
    /** N_Cs performance target, any value: CFs handed out later are counted (see TIMERS). */
    uint32_t n_cs_us;
    bool extended_ids;  /**< both identifiers have 29 bits (CAN_FLAG_EXT) */
    uint8_t rx_bs;      /**< BS of this receiver's FC CTS, 0..255 (0: one block) */
    uint8_t rx_stmin;   /**< STmin of this receiver's FC CTS: 0x00..0x7F or 0xF1..0xF9 */
    uint8_t rx_wft_max; /**< N_WFTmax: FC WAITs this receiver sends in a row, 0..255 */
    uint8_t tx_wft_max; /**< FC WAITs this sender accepts in a row, 0..255 (see TRANSMISSION) */
} isotp_config_t;

/** Hands a frame to the bus. Returns true if the transmit path took it (a confirmation will
 *  follow), false if not: the transmission or reception it belongs to then ends at once with
 *  ISOTP_RESULT_ERROR (an OVFLW is simply not sent). `frame` is valid during the call only. */
typedef bool (*isotp_send_frame_fn)(void *context, const can_frame_t *frame);

/** N_USData_FF.indication: an FF that fits started a reception of `length` bytes, 8..4095. */
typedef void (*isotp_ff_indication_fn)(void *context, uint16_t length);

/**
 * N_USData.indication: a reception ended. With ISOTP_RESULT_OK, `data` points to the message of
 * `length` bytes, 1..4095, in the receive buffer; return true to keep the buffer until
 * isotp_rx_release(), false to give it back when the callback returns (the data must not be used
 * after that). With any other result `data` is NULL, `length` is the FF_DL of the message that
 * was lost, and the return value is not used.
 */
typedef bool (*isotp_indication_fn)(void *context, isotp_result_t result, const uint8_t *data,
                                    uint16_t length);

/** N_USData.confirm: the transmission that isotp_send() started ended with `result`. The data
 *  given to isotp_send() is no longer used. */
typedef void (*isotp_confirm_fn)(void *context, isotp_result_t result);

/** The callbacks of an instance (see CALLBACKS). send_frame and indication are required. */
typedef struct {
    isotp_send_frame_fn send_frame;
    isotp_ff_indication_fn ff_indication; /**< may be NULL */
    isotp_indication_fn indication;
    isotp_confirm_fn confirm; /**< may be NULL */
    void *context;            /**< passed to every callback as it is */
} isotp_callbacks_t;

/** The counters of an instance (see STATISTICS). Arrays are indexed by the enums above. */
typedef struct {
    uint32_t tx_requests;                    /**< messages isotp_send() took */
    uint32_t tx_results[ISOTP_RESULTS];      /**< confirmations given, by result */
    uint32_t ff_indications;                 /**< N_USData_FF.indication given */
    uint32_t rx_results[ISOTP_RESULTS];      /**< indications given, by result; [OK]: messages */
    uint32_t frames_out[ISOTP_FRAME_KINDS];  /**< frames the send_frame callback took */
    uint32_t frames_in[ISOTP_FRAME_KINDS];   /**< frames received and processed */
    uint32_t ignored[ISOTP_IGNORED_REASONS]; /**< frames received and ignored, by reason */
    uint32_t rx_overflow;    /**< FFs that did not fit (escape FFs included): answered OVFLW */
    uint32_t rx_sf_too_long; /**< SFs longer than the receive buffer: dropped */
    uint32_t rx_sf_busy;     /**< SFs that came while the buffer was kept: dropped */
    /** OVFLW answers not sent: a new SF or FF came while one was due, or send_frame refused it */
    uint32_t fc_dropped;
    uint32_t stmin_reserved;    /**< FC CTS with a reserved STmin: 127 ms used */
    uint32_t send_refused;      /**< frames the send_frame callback refused */
    uint32_t confirmed;         /**< confirmations that matched a frame handed out */
    uint32_t confirm_unmatched; /**< isotp_on_tx_done() with no frame handed out */
    uint32_t unconfirmed;       /**< frames given up: their N_As or N_Ar ran out first */
    /** frames taken as sent because the peer's answer came before their confirmation (see
     *  CONFIRMATIONS) */
    uint32_t proven;
    uint32_t outstanding;     /**< frames handed out and not yet confirmed or given up, now */
    uint32_t reentry_refused; /**< calls refused because they came from a callback */
    uint32_t n_br_late;       /**< FCs handed out more than n_br_us after what called for them */
    uint32_t n_cs_late;       /**< CFs handed out more than n_cs_us after their reference */
    uint32_t n_br_max_us;     /**< the longest N_Br measured, saturating */
    uint32_t n_cs_max_us;     /**< the longest N_Cs measured, saturating */
} isotp_stats_t;

/* Private to isotp.c; in the header only so that an instance can be allocated statically. */
typedef enum {
    ISOTP_TX_IDLE = 0,
    ISOTP_TX_DUE,     /* the frame `frame` is to be handed out (waits for room in the pipeline) */
    ISOTP_TX_SENT,    /* handed out; its confirmation (`awaited`) is awaited, N_As runs */
    ISOTP_TX_WAIT_FC, /* waiting for an FC; N_Bs runs out at `timer_us` */
    ISOTP_TX_STMIN    /* the next CF is due at `timer_us` */
} isotp_tx_state_t;

typedef enum {
    ISOTP_RX_IDLE = 0,
    ISOTP_RX_FC_DUE,      /* the FC `fc` is to be handed out (waits for room in the pipeline) */
    ISOTP_RX_FC_SENT,     /* the FC CTS or WAIT is out; its confirmation is awaited, N_Ar runs */
    ISOTP_RX_WAIT_BUFFER, /* the buffer is kept; the next FC is due at `timer_us` */
    ISOTP_RX_CF           /* taking CFs; N_Cr runs out at `timer_us` */
} isotp_rx_state_t;

/* A frame handed out and not confirmed: the oldest is handouts[0]. */
typedef struct {
    uint64_t deadline_us; /* its N_As or N_Ar runs out: it is given up */
    uint32_t serial;      /* which hand-out it was */
    bool rx_side;         /* an FC of the receiver; otherwise a frame of the sender */
} isotp_handout_t;

typedef struct {
    const uint8_t *data; /* the message, as given to isotp_send() */
    uint64_t timer_us;   /* WAIT_FC: N_Bs runs out; STMIN: the next CF is due */
    uint64_t since_us;   /* the reference of N_Cs for the next CF */
    uint64_t cf_done_us; /* confirmation of the previous CF, for STmin across a block */
    uint32_t awaited;    /* SENT: the serial of the frame handed out */
    uint32_t stmin_us;   /* STmin of the current block */
    uint16_t length;     /* of the message */
    uint16_t offset;     /* bytes handed out so far */
    isotp_tx_state_t state;
    isotp_frame_kind_t frame; /* DUE, SENT: SF, FF or CF */
    uint8_t sn;               /* SN of the next CF */
    uint8_t bs;               /* BS of the current block */
    uint8_t block_count;      /* CFs handed out in the current block */
    uint8_t wft_count;        /* FC WAITs in a row */
    bool cf_proven;           /* the previous CF was proven by an FC, not confirmed */
} isotp_tx_t;

typedef struct {
    uint64_t timer_us; /* RX_CF: N_Cr runs out; WAIT_BUFFER: the next FC is due */
    uint64_t since_us; /* the reference of N_Br for the next FC */
    uint32_t awaited;  /* FC_SENT: the serial of the FC handed out */
    uint16_t length;   /* FF_DL of the message being received */
    uint16_t offset;   /* bytes stored */
    isotp_rx_state_t state;
    isotp_frame_kind_t fc;        /* FC_DUE, FC_SENT: FC_CTS, FC_WAIT or FC_OVFLW */
    uint8_t sn;                   /* SN expected next */
    uint8_t block_count;          /* CFs taken in the current block */
    uint8_t wft_count;            /* FC WAITs handed out for this message */
    bool held;                    /* the upper layer keeps the buffer */
    uint8_t stash[ISOTP_FF_DATA]; /* the FF's data while the buffer is kept */
} isotp_rx_t;

typedef struct {
    isotp_config_t config;
    isotp_callbacks_t callbacks;
    uint8_t *buffer;
    isotp_tx_t tx;
    isotp_rx_t rx;
    isotp_handout_t handouts[ISOTP_PIPELINE_DEPTH];
    isotp_stats_t stats;
    uint32_t serial;      /* serial of the next hand-out */
    uint16_t buffer_size; /* usable bytes of the buffer, 1..ISOTP_MAX_LENGTH */
    uint8_t handout_count;
    bool ready;       /* initialised */
    bool in_callback; /* a callback is running */
} isotp_t;

/**
 * Prepare `tp` for a connection: copies `config` and `callbacks`, and keeps `rx_buffer`, of which
 * the first min(rx_size, ISOTP_MAX_LENGTH) bytes receive messages. Nothing is in progress, the
 * buffer is free and every counter is 0. The memory of `tp` may hold anything before. Call once,
 * before any other call on `tp`, in the context that will own it (on the node: before the
 * scheduler starts, or in the diagnostic task before its loop).
 *
 * Returns false, with `tp` unchanged, if a pointer or a required callback is NULL, rx_size is 0,
 * or a configuration value is outside its range (see isotp_config_t); otherwise true.
 */
bool isotp_init(isotp_t *tp, const isotp_config_t *config, const isotp_callbacks_t *callbacks,
                uint8_t *rx_buffer, uint16_t rx_size);

/**
 * N_USData.request: send the `length` bytes at `data` (see TRANSMISSION) at time `now_us`. The
 * SF or FF is handed out before the call returns, unless ISOTP_PIPELINE_DEPTH frames are
 * outstanding. Returns ISOTP_OK if the message was taken (its result comes through `confirm`,
 * possibly before this call returns, for example ISOTP_RESULT_ERROR when send_frame refused the
 * first frame), ISOTP_E_ARG or ISOTP_E_BUSY otherwise (nothing sent, nothing changed beyond the
 * timers of `now_us`, see TIMERS). The owner of `tp` only, never from a callback (see CONTEXT).
 */
isotp_status_t isotp_send(isotp_t *tp, const uint8_t *data, uint16_t length, uint64_t now_us);

/**
 * A frame arrived at time `now_us`: process it (see RECEPTION, TRANSMISSION, UNEXPECTED PDUS).
 * Returns true if the frame was processed, false if it was ignored for a reason of
 * isotp_ignored_t, and also (without counting it as a frame) if `tp` or `frame` is NULL, `tp`
 * is not initialised, or the call came from a callback. The owner of `tp` only (see CONTEXT).
 */
bool isotp_on_frame(isotp_t *tp, const can_frame_t *frame, uint64_t now_us);

/**
 * The oldest frame handed out and not yet confirmed has been sent; `now_us` is the time of the
 * confirmation (see CONFIRMATIONS for the order the caller must keep). Returns false, counted in
 * stats.confirm_unmatched, if no frame is outstanding once the timers of `now_us` have been acted
 * on; false without counting if `tp` is NULL, not initialised, or the call came from a callback.
 * The owner of `tp` only (see CONTEXT): a transmit-done event of another context is passed to
 * the owner first, in the order the events came.
 */
bool isotp_on_tx_done(isotp_t *tp, uint64_t now_us);

/** Act on the timers that have run out by `now_us` and hand out frames that are due. Does
 *  nothing if `tp` is NULL, not initialised, or the call came from a callback. The owner of `tp`
 *  only (see CONTEXT). */
void isotp_poll(isotp_t *tp, uint64_t now_us);

/**
 * Give back the receive buffer that `indication` kept. A reception that waits for it gets its FC
 * CTS in this call (see RECEPTION). Returns false if the buffer was not kept (or `tp` is NULL,
 * not initialised, or the call came from a callback), true otherwise. The owner of `tp` only,
 * after the indication has returned (see CONTEXT).
 */
bool isotp_rx_release(isotp_t *tp, uint64_t now_us);

/** The earliest time at which isotp_poll() has something to do (see TIMERS), or
 *  ISOTP_NO_DEADLINE. ISOTP_NO_DEADLINE also if `tp` is NULL or not initialised. The owner of
 *  `tp`, its callbacks included. */
uint64_t isotp_next_deadline(const isotp_t *tp);

/** True while a transmission is in progress (isotp_send() would answer ISOTP_E_BUSY). The owner
 *  of `tp`, its callbacks included. */
bool isotp_tx_busy(const isotp_t *tp);

/** True while a reception is in progress (see UNEXPECTED PDUS). The owner of `tp`, its callbacks
 *  included. */
bool isotp_rx_busy(const isotp_t *tp);

/** True while `indication` keeps the receive buffer. The owner of `tp`, its callbacks included. */
bool isotp_rx_held(const isotp_t *tp);

/** The counters, live: valid until the next call on `tp`. NULL if `tp` is NULL. The owner of
 *  `tp`, its callbacks included; another context copies them under the owner's serialisation. */
const isotp_stats_t *isotp_stats(const isotp_t *tp);

/** The separation time an STmin byte asks for, in microseconds, as a sender reads it:
 *  ISOTP_STMIN_RESERVED_US for a reserved value. Any context. */
uint32_t isotp_stmin_us(uint8_t stmin);

/** True if `stmin` is not a reserved value (0x00..0x7F, 0xF1..0xF9). Any context. */
bool isotp_stmin_valid(uint8_t stmin);

/** Names for logs and statistics: "OK", "TIMEOUT_A", ...; "SF", "FF", ..., "FC_RESERVED";
 *  "FOREIGN", "SHORT", ...; "?" for a value outside the enum. Any context. */
const char *isotp_result_name(isotp_result_t result);
const char *isotp_frame_kind_name(isotp_frame_kind_t kind);
const char *isotp_ignored_name(isotp_ignored_t reason);

#endif /* FCN_ISOTP_H */
