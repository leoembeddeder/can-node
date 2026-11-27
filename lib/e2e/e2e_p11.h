/* SPDX-License-Identifier: MIT */
/**
 * @file
 * End-to-end protection of cyclic CAN messages: AUTOSAR E2E Profile 11, variant 11A (Data ID
 * mode BOTH), for messages of 2 to 8 bytes.
 *
 * Sources: AUTOSAR "E2E Protocol Specification" (AUTOSAR_FO_PRS_E2EProtocol R24-11), section
 * 6.12, with the flow charts Fig. 6.93 to 6.110 (PRS); "Specification of SW-C End-to-End
 * Communication Protection Library" (AUTOSAR_CP_SWS_E2ELibrary R24-11), section 8.3.11 (LIB).
 * Requirement numbers are quoted where a rule is implemented. tests/unit/test_e2e_p11.c holds
 * the published test vectors; tools/fcn_tools/e2e.py is an independent implementation, and
 * tests/python/test_e2e.py compares the two.
 *
 * WHAT IT IS FOR. The CRC-15 of the CAN data link layer protects a frame on the wire. It
 * cannot see what happens above the wire: a frame lost or repeated by a buffer or a gateway, a
 * stale value sent again and again by a task that hangs, a frame sent under the wrong
 * identifier, a byte changed in a software buffer. End-to-end protection adds a check that the
 * producer of the data writes into the payload and its consumer verifies:
 *
 *   - a CRC over the message and over a Data ID that both ends know but nobody sends:
 *     corruption, and data arriving under the identity of another message;
 *   - a counter that the sender advances with every message: loss, repetition, reordering;
 *   - a check in every receive cycle, also when nothing arrived: timeout (see e2e/e2e_sm.h).
 *
 * LAYOUT (variant 11A, PRS_E2E_00540). Bit offsets count from the least significant bit of
 * byte 0, as for little-endian signals in a DBC:
 *
 *     byte 0             CRC                                    (CRCOffset 0)
 *     byte 1, bits 0..3  counter: 0, 1, ..., 14, 0, ...         (CounterOffset 8)
 *     byte 1, bits 4..7  payload, untouched by this module
 *     bytes 2..n-1       payload
 *
 * Fig. 6.91 of the PRS draws the bits of a byte the other way round; its formulas
 * (Fig. 6.96, 6.108) and the published vectors put the counter in the LOW nibble. Unused
 * payload bits are to be sent as 1 (PRS_E2E_00540, item 4: unusedBitPattern 0xFF); that is
 * the business of whoever fills the payload.
 *
 * CRC (PRS_E2E_00508, PRS_E2E_00505, PRS_E2E_00513 with Fig. 6.97). The profile calls the
 * library's CRC-8-SAE J1850 routine (polynomial 0x1D, MSB first) in a way whose net effect is
 * a start value of 0x00 and no final XOR: the catalogue's CRC-8/GSM-A, not CRC-8/SAE-J1850
 * (which starts at 0xFF and inverts the result). Bytes fed, in this order: Data ID low byte,
 * Data ID high byte, bytes 1 to n-1 of the message. The CRC byte itself is not covered; the
 * counter is.
 *
 * COUNTER (PRS_E2E_00504). The sender starts at 0 and counts up to 14, then starts at 0 again
 * (Fig. 6.99: "State->Counter %= 15"). 15 is reserved and never sent. The receiver compares
 * the received counter with the one of the last message whose CRC was right:
 * delta = received - stored, plus 15 if that is negative.
 *
 * CHECK (PRS_E2E_00516, Fig. 6.105 to 6.110). The first rule that applies decides:
 *
 *     1. nothing arrived                         E2E_P11STATUS_NONEWDATA
 *     2. CRC wrong                               E2E_P11STATUS_ERROR
 *     3. received counter 15                     E2E_P11STATUS_ERROR (this project's rule)
 *     4. delta > max_delta_counter               E2E_P11STATUS_WRONGSEQUENCE
 *     5. delta 0                                 E2E_P11STATUS_REPEATED
 *     6. delta 1                                 E2E_P11STATUS_OK
 *     7. delta 2 .. max_delta_counter            E2E_P11STATUS_OKSOMELOST
 *
 * In cases 4 to 7 the received counter is stored, so the receiver follows a sender that
 * jumped (a WRONGSEQUENCE is followed by OK if the sender goes on normally); in cases 1 to 3
 * the stored counter is kept. After e2e_p11_check_init() the stored counter is 14
 * (SWS_E2E_00556): only a first message with counter 0 gives OK; with max_delta_counter 1,
 * counter 14 gives REPEATED and 1 to 13 give WRONGSEQUENCE. The state machine absorbs that.
 *
 * RULE 3, A RECEIVED COUNTER OF 15. The PRS does not say what to do with it: Fig. 6.110 does
 * not test for it, and taken literally it gives OK after a stored 14 (delta 1), stores the
 * 15, and then judges the sender's next correct message, counter 0, as REPEATED. A message
 * with a correct CRC and counter 15 cannot come from a sender that follows the profile. It is
 * either a damaged message whose damage the CRC did not catch, or a sender that runs another
 * counter scheme, such as the 0..15 counter of Profiles 1 and 2. Neither should count as a
 * good message, nor move the stored counter. So it is ERROR with the state kept, like a CRC
 * error: "an error not related to counters" (E2E_P_ERROR, SWS_E2E_00347). Profile 1 refuses
 * such a counter too, there with the return code E2E_E_INPUTERR_WRONG (PRS Fig. 6.58); a
 * return code is not used here, because it means a mistake of the caller and leaves the
 * status of the previous cycle in place, while this is a fault of the received data.
 *
 * INPUT ERRORS (LIB SWS_E2E_00047). E2E_E_INPUTERR_NULL and E2E_E_INPUTERR_WRONG as the PRS
 * assigns them (Fig. 6.94, 6.106), and E2E_E_INPUTERR_WRONG in addition for a configuration
 * outside the ranges given at e2e_p11_config_t and for a state whose counter is above 14,
 * which no sequence of calls produces. On any error return nothing is written: not the data,
 * not the state, and so not the status of the previous check either.
 *
 * WHAT IT DOES NOT GIVE. A CRC is not authentication: whoever knows the Data ID can make a
 * valid message. The 8-bit CRC has a Hamming distance of 3 over a 64-bit message: every error
 * of 1 or 2 bits is detected, some errors of 3 bits are not (tests/python/test_e2e.py counts
 * them). Two Data IDs whose CRC over the two ID bytes is equal protect identically, so the
 * Data IDs of one bus must differ in that value (PRS_E2E_UC_00072). The counter repeats after
 * 15 messages: a loss of exactly 14 in a row (or 29, 44, ...) makes the next message look
 * like a repetition, and longer losses can look like shorter ones; that is why the receiver
 * also has to run its check in every cycle without data. Protection only detects; what to do
 * with data that cannot be trusted is the application's decision.
 *
 * CONTEXT. The functions keep no data of their own, allocate nothing and do not block. A
 * state belongs to one message at one end; calls on one state have to be serialised by the
 * caller (one task protects, or checks, a given message). The configuration is constant and
 * may be shared by sender and receiver.
 */
#ifndef FCN_E2E_P11_H
#define FCN_E2E_P11_H

#include <stddef.h>
#include <stdint.h>

#include "e2e/e2e.h"
#include "e2e/e2e_sm.h"

#define E2E_P11_CRC_BYTE     0U  /**< index of the CRC byte (CRCOffset 0) */
#define E2E_P11_COUNTER_BYTE 1U  /**< index of the byte whose low nibble is the counter */
#define E2E_P11_COUNTER_MAX  14U /**< the counter runs 0..14 (PRS_E2E_00504) */
#define E2E_P11_MIN_DATA_LEN 2U  /**< CRC byte and counter byte */
#define E2E_P11_MAX_DATA_LEN 8U  /**< classical CAN; the profile itself allows 32 bytes */

/**
 * E2E_P11ConfigType (PRS_E2E_00893) for variant 11A. Constant data, the same at both ends of
 * one message. Valid values, checked on every call (anything else gives E2E_E_INPUTERR_WRONG):
 */
typedef struct {
    uint16_t data_id; /**< DataID, any 16-bit value; covered by the CRC, never sent */
    uint8_t data_len; /**< length of the whole message in BYTES, 2..8 (the AUTOSAR DataLength
                           is in bits) */
    /** MaxDeltaCounter, receiver only: the largest counter step that still counts as OK or
     *  OKSOMELOST, 1..14. 1 tolerates no lost message; n tolerates up to n - 1 lost messages
     *  in a row. 0 would make OK impossible, and anything above 14 means the same as 14. */
    uint8_t max_delta_counter;
} e2e_p11_config_t;

/** E2E_P11CheckStatusType, with the values of the library (SWS_E2E_00564). */
typedef enum {
    E2E_P11STATUS_OK = 0x00,           /**< new message, counter one ahead of the last */
    E2E_P11STATUS_NONEWDATA = 0x01,    /**< nothing arrived since the previous check */
    E2E_P11STATUS_ERROR = 0x07,        /**< CRC wrong, or the reserved counter 15 */
    E2E_P11STATUS_REPEATED = 0x08,     /**< CRC right, the counter of the last message */
    E2E_P11STATUS_OKSOMELOST = 0x20,   /**< CRC right, 1 .. max_delta_counter - 1 lost */
    E2E_P11STATUS_WRONGSEQUENCE = 0x40 /**< CRC right, counter step above max_delta_counter */
} e2e_p11_status_t;

/** E2E_P11ProtectStateType (SWS_E2E_00567): the counter the next message will carry. */
typedef struct {
    uint8_t counter; /**< 0..14 */
} e2e_p11_protect_state_t;

/** E2E_P11CheckStateType (SWS_E2E_00563). */
typedef struct {
    uint8_t counter;         /**< counter of the last message whose CRC was right, 0..14 */
    e2e_p11_status_t status; /**< result of the last check that returned E2E_E_OK */
} e2e_p11_check_state_t;

/**
 * E2E_P11ProtectInit (SWS_E2E_00576, SWS_E2E_00555): counter 0, so the first message carries
 * 0. Returns E2E_E_INPUTERR_NULL if `state` is NULL, otherwise E2E_E_OK.
 */
e2e_result_t e2e_p11_protect_init(e2e_p11_protect_state_t *state);

/**
 * E2E_P11Protect (SWS_E2E_00575; PRS_E2E_00509 to PRS_E2E_00515, Fig. 6.93 to 6.99): make a
 * message ready to be sent. `data` holds the whole message, `length` bytes, with the payload
 * already in place. Writes the counter into the low nibble of byte 1 (the high nibble keeps
 * what the caller put there), then the CRC over the result into byte 0, then advances the
 * counter (14 is followed by 0). Nothing else in `data` is written.
 *
 * Call it once for every message that is handed to the bus, from the task that produces the
 * message, after the payload is complete: a message protected and then changed is sent with a
 * wrong CRC, and one protected twice skips a counter value.
 *
 * Returns E2E_E_INPUTERR_NULL if a pointer is NULL; E2E_E_INPUTERR_WRONG if `length` is not
 * config->data_len (Fig. 6.94), the configuration is out of range, or state->counter is above
 * 14; nothing is written in either case. Otherwise E2E_E_OK.
 */
e2e_result_t e2e_p11_protect(const e2e_p11_config_t *config, e2e_p11_protect_state_t *state,
                             uint8_t *data, size_t length);

/**
 * E2E_P11CheckInit (SWS_E2E_00573, SWS_E2E_00556): counter 14 and status ERROR, so that the
 * first message of a sender that has just started (counter 0) is one step ahead. Returns
 * E2E_E_INPUTERR_NULL if `state` is NULL, otherwise E2E_E_OK.
 */
e2e_result_t e2e_p11_check_init(e2e_p11_check_state_t *state);

/**
 * E2E_P11Check (SWS_E2E_00572; PRS_E2E_00516 to PRS_E2E_00521, Fig. 6.105 to 6.110): judge
 * one receive cycle. `data` is the message that arrived since the previous call and `length`
 * its length in bytes; pass data NULL and length 0 if none arrived. The verdict goes to
 * state->status, by the rules in the file comment.
 *
 * Call it from the task that receives the message: once for every message that arrives, in
 * the order they arrive, and once with data NULL and length 0 for every period of the
 * message in which none arrived. Without the second kind of call a sender that stops is
 * never noticed. Feed every result to the state machine (e2e_p11_map_status_to_sm(),
 * e2e_sm_check()).
 *
 * Returns E2E_E_OK whenever it has set state->status. Otherwise, with nothing written:
 *   - E2E_E_INPUTERR_NULL    if `config` or `state` is NULL (Fig. 6.106);
 *   - E2E_E_INPUTERR_WRONG   if only one of "data is NULL" and "length is 0" holds, or the
 *                            length is not config->data_len (Fig. 6.106), or the
 *                            configuration is out of range, or state->counter is above 14.
 */
e2e_result_t e2e_p11_check(const e2e_p11_config_t *config, e2e_p11_check_state_t *state,
                           const uint8_t *data, size_t length);

/**
 * E2E_P11MapStatusToSM (SWS_E2E_00574, SWS_E2E_00557, SWS_E2E_00608, SWS_E2E_00558;
 * PRS_E2E_00920): the input of the state machine for one check. E2E_P_ERROR whenever
 * `check_return` is not E2E_E_OK, whatever `status` holds then; otherwise OK and OKSOMELOST
 * give E2E_P_OK, and ERROR, REPEATED, NONEWDATA and WRONGSEQUENCE their E2E_P_ namesakes. A
 * `status` that is none of the six values gives E2E_P_ERROR. Any context.
 */
e2e_p_check_status_t e2e_p11_map_status_to_sm(e2e_result_t check_return, e2e_p11_status_t status);

#endif /* FCN_E2E_P11_H */
