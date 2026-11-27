/* SPDX-License-Identifier: MIT */
/**
 * @file
 * A classical CAN frame as the rest of the project passes it around, and the two properties
 * of a frame that do not depend on any controller: whether it is well formed, and how it
 * ranks in arbitration.
 */
#ifndef FCN_CAN_FRAME_H
#define FCN_CAN_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#define CAN_MAX_DLC    8U
#define CAN_ID_STD_MAX 0x7FFU
#define CAN_ID_EXT_MAX 0x1FFFFFFFU

#define CAN_FLAG_EXT 0x01U /**< extended (29-bit) identifier */
#define CAN_FLAG_RTR 0x02U /**< remote transmission request: carries a DLC but no data */

typedef struct {
    uint32_t id;     /**< identifier, right-aligned: 11 bits, or 29 bits with CAN_FLAG_EXT */
    uint8_t flags;   /**< CAN_FLAG_* */
    uint8_t dlc;     /**< data length code, 0..8 */
    uint8_t data[8]; /**< data bytes 0..dlc-1; the rest is ignored */
} can_frame_t;

/** True if the identifier fits its format, the DLC is at most 8 and no unknown flag is set. */
bool can_frame_is_valid(const can_frame_t *frame);

/**
 * Rank of a frame in bitwise arbitration: of two frames that start at the same time, the one
 * with the SMALLER key wins the bus.
 *
 * The key is the arbitration field as it appears on the wire, where a dominant bit (0) beats
 * a recessive bit (1): the 11 base identifier bits, then RTR (base format) or SRR (extended,
 * always recessive), then IDE, then for extended frames the 18 identifier extension bits and
 * RTR. A base-format frame therefore beats an extended frame with the same base identifier,
 * and a data frame beats a remote frame with the same identifier.
 */
uint32_t can_frame_arbitration_key(const can_frame_t *frame);

#endif /* FCN_CAN_FRAME_H */
