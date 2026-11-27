/* SPDX-License-Identifier: MIT */
/**
 * @file
 * How many bit times a classical CAN frame occupies on the bus.
 *
 * A frame's length on the wire is not fixed by its DLC: after five consecutive bits of the
 * same level the transmitter inserts a stuff bit of the opposite level, so the length depends
 * on the frame's content, including its CRC. This module builds the frame's bit sequence the
 * way a controller does and counts.
 *
 * Fields of a base-format data frame (extended in brackets):
 *
 *     SOF 1 | identifier 11 | RTR 1 | IDE 1 | r0 1 | DLC 4 | data 8n | CRC 15   <- stuffed
 *           [ identifier 11 | SRR 1 | IDE 1 | identifier 18 | RTR 1 | r1 1 | r0 1 | DLC 4 ... ]
 *     CRC delimiter 1 | ACK slot 1 | ACK delimiter 1 | EOF 7                     <- not stuffed
 *
 * Bit stuffing applies from the start of frame up to and including the CRC sequence, and a
 * stuff bit itself counts towards the next run of five. The CRC is computed over the same
 * span before stuffing. The fixed-form tail is never stuffed. After the frame the bus stays
 * recessive for the 3-bit intermission before the next frame may start.
 *
 * Unstuffed lengths, start of frame to end of frame: 44 + 8n bits (base), 64 + 8n (extended).
 */
#ifndef FCN_CAN_BITS_H
#define FCN_CAN_BITS_H

#include <stdbool.h>
#include <stdint.h>

#include "can/can_frame.h"

/** Bits of the interframe space that separate two frames (intermission). */
#define CAN_BITS_INTERMISSION 3U

typedef struct {
    uint16_t frame_bits; /**< start of frame to end of frame, stuff bits included */
    uint8_t stuff_bits;  /**< stuff bits inserted between start of frame and end of CRC */
    uint16_t crc15;      /**< the frame's CRC-15 sequence */
} can_bits_info_t;

/**
 * Exact length of `frame` on the wire. Returns false, leaving `out` untouched, if the frame
 * is not valid (see can_frame_is_valid).
 */
bool can_bits_exact(const can_frame_t *frame, can_bits_info_t *out);

/** Length without any stuff bit: 44 + 8n (base) or 64 + 8n (extended); n = 0 for a remote
 *  frame. `dlc` above 8 is treated as 8. */
uint16_t can_bits_unstuffed(bool extended, bool remote, uint8_t dlc);

/**
 * Upper bound of the length for any content: the unstuffed length plus the largest possible
 * number of stuff bits, floor((s - 1) / 4) for a stuffed span of s bits (s = 34 + 8n base,
 * 54 + 8n extended). This is the figure worst-case bus-load calculations use. No real frame
 * reaches it for every DLC, because the CRC cannot be chosen freely.
 */
uint16_t can_bits_worst_case(bool extended, bool remote, uint8_t dlc);

/** Duration of `bits` bit times at `bitrate` bit/s, in nanoseconds, rounded to nearest.
 *  `bitrate` must not be 0. */
uint64_t can_bits_to_ns(uint32_t bits, uint32_t bitrate);

#endif /* FCN_CAN_BITS_H */
