/* SPDX-License-Identifier: MIT */
/**
 * @file
 * CAN fault confinement: the transmit and receive error counters and the three error states
 * of a node, as specified in section 8 of the Bosch CAN Specification 2.0 (Part B), whose
 * twelve rules ISO 11898-1 adopted.
 *
 * This module is only the rule engine. It does not detect errors; it is told what happened
 * on the bus, one event at a time, and keeps the counters and the state. What produces the
 * events (a real controller's bit-level logic, or here a model driven by injected faults) is
 * the business of can_ctrl. Keeping the rules apart lets every one of them be tested alone.
 *
 * The rules, with the event that applies each:
 *
 *   1  A receiver detects an error: REC + 1                           CAN_FC_RX_ERROR
 *      (except a bit error during its own active error or overload flag: that is rule 5)
 *   2  A receiver sees a dominant bit as the first bit after sending
 *      an error flag: REC + 8                                         CAN_FC_RX_DOMINANT_AFTER_FLAG
 *   3  A transmitter sends an error flag: TEC + 8                     CAN_FC_TX_ERROR
 *      Exception 1: the transmitter is error passive, the error is an
 *      acknowledgment error, and it sees no dominant bit while sending
 *      its passive error flag: TEC unchanged                          CAN_FC_TX_ACK_ERROR_ALONE
 *      Exception 2: a stuff error during arbitration on a stuff bit
 *      sent recessive but monitored dominant: TEC unchanged           CAN_FC_TX_STUFF_ERROR_IN_ARBITRATION
 *   4  A transmitter detects a bit error while sending an active
 *      error flag or an overload flag: TEC + 8                        CAN_FC_TX_BIT_ERROR_IN_FLAG
 *   5  A receiver detects the same: REC + 8                           CAN_FC_RX_BIT_ERROR_IN_FLAG
 *   6  After the 14th consecutive dominant bit following an active
 *      error or overload flag (the 8th following a passive error
 *      flag), and after every further 8 consecutive dominant bits:
 *      TEC + 8 for a transmitter, REC + 8 for a receiver              CAN_FC_TX_DOMINANT_RUN, CAN_FC_RX_DOMINANT_RUN
 *   7  A message is transmitted successfully: TEC - 1, not below 0    CAN_FC_TX_SUCCESS
 *   8  A message is received successfully: REC - 1 if it was 1..127;
 *      unchanged if 0; set to a value between 119 and 127 if it was
 *      above 127 (this implementation sets 127)                       CAN_FC_RX_SUCCESS
 *   9  The node is error passive when TEC >= 128 or REC >= 128
 *  10  The node is bus off when TEC >= 256
 *  11  An error-passive node is error active again when both counters
 *      are <= 127
 *  12  A bus-off node may become error active, with both counters at
 *      0, after 128 occurrences of 11 consecutive recessive bits      CAN_FC_RECESSIVE_11
 *
 * Rule 12 says "is permitted to": real controllers start that count either by themselves or
 * only when software asks (SJA1000 leaves reset mode on command, M_CAN when INIT is cleared,
 * bxCAN depending on ABOM). Here the count runs only after can_fc_request_restart().
 *
 * CAN_FC_TX_ACK_ERROR_ALONE is the acknowledgment error of a transmitter that sees no other
 * node at all. While the node is error active it is an ordinary transmitter error (rule 3);
 * once the node is error passive, exception 1 applies. This is why a node that is alone on
 * the bus climbs to TEC 128 and stays there: it becomes error passive but never bus off.
 */
#ifndef FCN_CAN_FC_H
#define FCN_CAN_FC_H

#include <stdbool.h>
#include <stdint.h>

#define CAN_FC_PASSIVE_LIMIT        128U /**< rule 9 */
#define CAN_FC_BUS_OFF_LIMIT        256U /**< rule 10 */
#define CAN_FC_WARNING_LIMIT        96U /**< "heavily disturbed bus", a note in the specification */
#define CAN_FC_REC_MAX              255U /**< the receive counter saturates here */
#define CAN_FC_REC_AFTER_PASSIVE    127U /**< rule 8: value taken after a success above 127 */
#define CAN_FC_RECOVERY_OCCURRENCES 128U /**< rule 12 */

typedef enum {
    CAN_FC_ERROR_ACTIVE = 0,
    CAN_FC_ERROR_PASSIVE = 1,
    CAN_FC_BUS_OFF = 2
} can_fc_state_t;

typedef enum {
    CAN_FC_RX_ERROR = 0,
    CAN_FC_RX_DOMINANT_AFTER_FLAG,
    CAN_FC_TX_ERROR,
    CAN_FC_TX_ACK_ERROR_ALONE,
    CAN_FC_TX_STUFF_ERROR_IN_ARBITRATION,
    CAN_FC_TX_BIT_ERROR_IN_FLAG,
    CAN_FC_RX_BIT_ERROR_IN_FLAG,
    CAN_FC_TX_DOMINANT_RUN,
    CAN_FC_RX_DOMINANT_RUN,
    CAN_FC_TX_SUCCESS,
    CAN_FC_RX_SUCCESS,
    CAN_FC_RECESSIVE_11,
    CAN_FC_EVENT_COUNT
} can_fc_event_t;

typedef struct {
    uint16_t tec; /**< transmit error count, 0..256 and above while bus off */
    uint16_t rec; /**< receive error count, 0..CAN_FC_REC_MAX */
    can_fc_state_t state;
    bool restart_requested;        /**< bus off and counting towards recovery */
    uint16_t recovery_occurrences; /**< occurrences of 11 recessive bits counted so far */
} can_fc_t;

/** Error active, both counters 0. */
void can_fc_init(can_fc_t *fc);

/**
 * Apply one event and return the state afterwards.
 *
 * While the node is bus off, every event except CAN_FC_RECESSIVE_11 is ignored: a bus-off
 * node takes no part in bus traffic. CAN_FC_RECESSIVE_11 is ignored unless the node is bus
 * off and a restart was requested.
 */
can_fc_state_t can_fc_apply(can_fc_t *fc, can_fc_event_t event);

/** Start counting towards recovery. Has no effect unless the node is bus off. */
void can_fc_request_restart(can_fc_t *fc);

/** True at or above the warning limit of either counter (and the node is not bus off). */
bool can_fc_warning(const can_fc_t *fc);

#endif /* FCN_CAN_FC_H */
