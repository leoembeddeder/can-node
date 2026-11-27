/* SPDX-License-Identifier: MIT */
#include "can/can_bits.h"

#include <stddef.h>

#include "crc/crc.h"

/* Levels as the frame formats write them: a dominant bit is a 0, a recessive bit a 1. */
#define BIT_DOMINANT  0U
#define BIT_RECESSIVE 1U

/* Field widths (Bosch CAN Specification 2.0, Part B, section 3.2.1). */
#define BASE_ID_BITS      11U /* ID-28..ID-18: the whole identifier of a base frame */
#define ID_EXTENSION_BITS 18U /* ID-17..ID-0 */
#define DLC_BITS          4U
#define DATA_BYTE_BITS    8U
#define CRC_BITS          15U

/* Bits from start of frame to the end of the CRC sequence when there is no data field. This
 * is the part of a frame that is bit-stuffed:
 *   base      SOF 1, identifier 11, RTR 1, IDE 1, r0 1, DLC 4, CRC 15
 *   extended  SOF 1, identifier 11, SRR 1, IDE 1, identifier 18, RTR 1, r1 1, r0 1, DLC 4,
 *             CRC 15 */
#define STUFFED_BITS_BASE     34U
#define STUFFED_BITS_EXTENDED 54U

/* CRC delimiter 1, ACK slot 1, ACK delimiter 1, end of frame 7: fixed form, never stuffed. */
#define TAIL_BITS 10U

/* A stuff bit follows five consecutive bits of the same level (section 6, "Coding"). */
#define STUFF_RUN 5U

#define NS_PER_SECOND 1000000000ULL

/* A frame on its way onto the wire, reduced to what its length depends on. */
typedef struct {
    uint16_t crc;       /* CRC-15 register over the frame's own bits sent so far */
    uint16_t wire_bits; /* bits on the wire so far, stuff bits included */
    uint8_t stuff_bits; /* how many of them are stuff bits */
    uint8_t level;      /* level of the last bit on the wire */
    uint8_t run;        /* how many bits of that level end the sequence on the wire */
} bits_tx_t;

/* Put one bit on the wire and, if it completes a run of five, the stuff bit behind it. */
static void bits_tx_wire(bits_tx_t *tx, uint8_t bit)
{
    if (bit == tx->level) {
        tx->run++;
    } else {
        tx->level = bit;
        tx->run = 1U;
    }

    tx->wire_bits++;

    if (tx->run == STUFF_RUN) {
        /* The stuff bit has the opposite level and is a bit on the wire like any other. It
         * is therefore the first bit of the next run: four more bits of its level, not
         * five, call for the next stuff bit. */
        tx->level = (bit == BIT_DOMINANT) ? BIT_RECESSIVE : BIT_DOMINANT;
        tx->run = 1U;
        tx->wire_bits++;
        tx->stuff_bits++;
    }
}

/* Send the low `width` bits of `value`, most significant bit first, as (part of) one of the
 * fields that the CRC covers. */
static void bits_tx_field(bits_tx_t *tx, uint32_t value, uint32_t width)
{
    for (uint32_t left = width; left > 0U; left--) {
        const uint8_t bit = (uint8_t)((value >> (left - 1U)) & 1U);

        tx->crc = crc15_can_update_bit(tx->crc, bit);
        bits_tx_wire(tx, bit);
    }
}

/* Bits of the data field: none in a remote frame, whatever its DLC says. */
static uint32_t bits_data_field(bool remote, uint8_t dlc)
{
    uint32_t bytes = 0U;

    if (!remote) {
        bytes = (dlc > CAN_MAX_DLC) ? CAN_MAX_DLC : (uint32_t)dlc;
    }

    return bytes * DATA_BYTE_BITS;
}

/* Bits between start of frame and the end of the CRC sequence, before stuffing. */
static uint32_t bits_stuffed_span(bool extended, bool remote, uint8_t dlc)
{
    uint32_t without_data = STUFFED_BITS_BASE;

    if (extended) {
        without_data = STUFFED_BITS_EXTENDED;
    }

    return without_data + bits_data_field(remote, dlc);
}

bool can_bits_exact(const can_frame_t *frame, can_bits_info_t *out)
{
    bool ok = false;

    /* The header does not say what a NULL `out` means. There is nowhere to put the result,
     * so the call fails in the same way as for an invalid frame. */
    if ((out != NULL) && can_frame_is_valid(frame)) {
        const bool extended = (frame->flags & CAN_FLAG_EXT) != 0U;
        const bool remote = (frame->flags & CAN_FLAG_RTR) != 0U;

        /* The bus is recessive before a frame and start of frame is dominant, so the first
         * bit always opens a new run: idle time never counts towards a stuff bit. */
        bits_tx_t tx = {
            .crc = CRC15_CAN_INIT,
            .wire_bits = 0U,
            .stuff_bits = 0U,
            .level = BIT_RECESSIVE,
            .run = 0U,
        };

        /* RTR is the bit that tells the two kinds of frame apart: dominant in a data frame. */
        uint32_t rtr = BIT_DOMINANT;

        if (remote) {
            rtr = BIT_RECESSIVE;
        }

        bits_tx_field(&tx, BIT_DOMINANT, 1U); /* start of frame */

        if (extended) {
            bits_tx_field(&tx, frame->id >> ID_EXTENSION_BITS, BASE_ID_BITS);
            bits_tx_field(&tx, BIT_RECESSIVE, 1U); /* SRR, in the place of a base frame's RTR */
            bits_tx_field(&tx, BIT_RECESSIVE, 1U); /* IDE */
            bits_tx_field(&tx, frame->id, ID_EXTENSION_BITS);
            bits_tx_field(&tx, rtr, 1U);
            bits_tx_field(&tx, BIT_DOMINANT, 1U); /* r1 */
            bits_tx_field(&tx, BIT_DOMINANT, 1U); /* r0 */
        } else {
            bits_tx_field(&tx, frame->id, BASE_ID_BITS);
            bits_tx_field(&tx, rtr, 1U);
            bits_tx_field(&tx, BIT_DOMINANT, 1U); /* IDE */
            bits_tx_field(&tx, BIT_DOMINANT, 1U); /* r0 */
        }

        bits_tx_field(&tx, frame->dlc, DLC_BITS);

        /* A remote frame carries the DLC of the data frame it asks for, but no data field. */
        if (!remote) {
            for (uint8_t i = 0U; i < frame->dlc; i++) {
                bits_tx_field(&tx, frame->data[i], DATA_BYTE_BITS);
            }
        }

        /* The CRC sequence is stuffed like the fields before it, but it is not part of its
         * own input, so it bypasses the register.
         *
         * Its last bit can complete a run of five. The stuff bit is sent then as well, between
         * the CRC sequence and the CRC delimiter: the rule acts on five equal bits "in the bit
         * stream to be transmitted", and all five lie in a stuffed field. This is also how the
         * frame-length calculation of can-utils (canframelen.c) and the CAN decoder of sigrok
         * read the rule. */
        const uint16_t crc = tx.crc;

        for (uint32_t left = CRC_BITS; left > 0U; left--) {
            bits_tx_wire(&tx, (uint8_t)(((uint32_t)crc >> (left - 1U)) & 1U));
        }

        out->frame_bits = (uint16_t)(tx.wire_bits + TAIL_BITS);
        out->stuff_bits = tx.stuff_bits;
        out->crc15 = crc;
        ok = true;
    }

    return ok;
}

uint16_t can_bits_unstuffed(bool extended, bool remote, uint8_t dlc)
{
    return (uint16_t)(bits_stuffed_span(extended, remote, dlc) + TAIL_BITS);
}

uint16_t can_bits_worst_case(bool extended, bool remote, uint8_t dlc)
{
    /* The header states the clamp of `dlc` to 8 for can_bits_unstuffed only; the bound is
     * built on the same span, so the same clamp applies here.
     *
     * The first stuff bit needs five equal bits. Every further one needs only four more,
     * because the stuff bit itself opens the next run. A span of s bits can therefore take
     * a stuff bit after bit 5, 9, 13, ..., which is floor((s - 1) / 4) of them. */
    const uint32_t span = bits_stuffed_span(extended, remote, dlc);
    const uint32_t stuff_max = (span - 1U) / 4U;

    return (uint16_t)(span + stuff_max + TAIL_BITS);
}

uint64_t can_bits_to_ns(uint32_t bits, uint32_t bitrate)
{
    uint64_t ns = 0U;

    /* The header forbids a bit rate of 0. It is answered with 0 instead of a division by
     * zero, which is undefined behaviour and a fault on a microcontroller that traps it;
     * 0 is also what "pacing off" means to the controller model. */
    if (bitrate != 0U) {
        /* bits * 1e9 is below 2^32 * 2^30, so adding half the divisor cannot overflow 64
         * bits. Adding it before the division rounds to nearest; an exact half rounds up. */
        ns = (((uint64_t)bits * NS_PER_SECOND) + ((uint64_t)bitrate / 2U)) / (uint64_t)bitrate;
    }

    return ns;
}
