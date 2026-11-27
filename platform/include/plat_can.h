/* SPDX-License-Identifier: MIT */
/**
 * @file
 * What the CAN driver needs from the controller it drives.
 *
 * On the host the controller is the model in lib/can, run in real time by a thread outside
 * the scheduler on top of a SocketCAN or UDP transport (platform/posix/posix_can.c). On a
 * target the same functions would wrap a peripheral's mailboxes, receive FIFO, status
 * register and interrupt. The driver above (app/can_io.c) is the same in both cases.
 *
 * The shape is that of real CAN hardware: a few transmit mailboxes that the driver loads one
 * frame at a time, a receive FIFO it drains, a status it can read, and one interrupt that
 * says "something happened" with flags for what.
 *
 * A transmit mailbox is in use from plat_can_tx() until the driver has taken the event that
 * says what became of the frame (TX_DONE or TX_ABORTED), like a request-completed flag that
 * the driver has to clear before the mailbox can be loaded again. Every accepted frame gets
 * exactly one such event, and the event is never lost, however far the driver falls behind.
 * Received frames can be lost when the driver does not drain the receive FIFO (counted in the
 * status), and of several state changes the driver did not take in time only the last is
 * guaranteed to be reported.
 */
#ifndef FCN_PLAT_CAN_H
#define FCN_PLAT_CAN_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"

#include "can/can_frame.h"

/** Transmit mailboxes of the controller. plat_can_tx() refuses a frame when all are in use. */
#define PLAT_CAN_TX_MAILBOXES 8U

/** Highest bit rate the controller can be configured for: classical CAN ends at 1 Mbit/s. The
 *  driver sizes its receive path for the frame rate of a full bus at this rate. */
#define PLAT_CAN_MAX_BITRATE 1000000U

/** Received frames the controller holds for the driver before it overruns. (A peripheral's
 *  own FIFO is a few frames deep; on a target this would be that plus the ISR's buffer.) */
#define PLAT_CAN_RX_FIFO_DEPTH 512U

/** Events the controller holds for the driver. The last PLAT_CAN_TX_MAILBOXES entries are
 *  only ever used for TX_DONE and TX_ABORTED, so that those always find room. */
#define PLAT_CAN_EVENT_FIFO_DEPTH 64U

/** Error state of the controller, in the numbering of can_fc_state_t. */
#define PLAT_CAN_ERROR_ACTIVE  0U
#define PLAT_CAN_ERROR_PASSIVE 1U
#define PLAT_CAN_BUS_OFF       2U

/** Interrupt causes passed to the ISR. */
#define PLAT_CAN_IRQ_RX    0x01U /**< the receive FIFO is not empty */
#define PLAT_CAN_IRQ_EVENT 0x02U /**< the event FIFO is not empty */

/** A received frame. */
typedef struct {
    can_frame_t frame;
    uint64_t eof_ns; /**< when the frame ended on the bus (plat_time_ns time base) */
} plat_can_rx_t;

typedef enum {
    PLAT_CAN_EVENT_TX_DONE = 1,    /**< a frame was transmitted; its mailbox is free again */
    PLAT_CAN_EVENT_TX_ABORTED = 2, /**< a frame was removed by plat_can_abort_tx(); mailbox free */
    PLAT_CAN_EVENT_STATE = 3       /**< the error state changed */
} plat_can_event_kind_t;

typedef struct {
    plat_can_event_kind_t kind;
    uint32_t handle;    /**< TX_DONE, TX_ABORTED: the handle given to plat_can_tx() */
    uint64_t submit_ns; /**< TX_DONE: the controller got the frame */
    uint64_t start_ns;  /**< TX_DONE: start of frame on the bus */
    uint64_t eof_ns;    /**< TX_DONE: end of frame on the bus; TX_ABORTED: when the frame left
                             its mailbox; STATE: time of the change */
    uint8_t state;      /**< STATE: new error state (PLAT_CAN_*) */
    uint8_t old_state;  /**< STATE: the state of the previous STATE event (error active before
                             the first), so the events form an unbroken chain */
    uint16_t tec;       /**< STATE: transmit error counter after the change */
    uint16_t rec;       /**< STATE: receive error counter after the change */
} plat_can_event_t;

typedef struct {
    uint8_t state; /**< PLAT_CAN_* */
    uint16_t tec;
    uint16_t rec;
    uint32_t rx_fifo_overruns; /**< received frames lost because the driver did not drain */
} plat_can_status_t;

/**
 * The driver's interrupt service routine. Runs in interrupt context: FromISR kernel calls
 * only. `causes` is a combination of PLAT_CAN_IRQ_*.
 */
typedef void (*plat_can_isr_t)(void *context, uint32_t causes, BaseType_t *woken);

/** Install the ISR. Call once, before the scheduler starts. */
void plat_can_set_isr(plat_can_isr_t isr, void *context);

/**
 * Load one frame into a transmit mailbox. `handle` identifies it in the TX_DONE or
 * TX_ABORTED event. Returns false, with no event to follow, if all PLAT_CAN_TX_MAILBOXES
 * mailboxes are in use. A frame that is accepted but not a valid CAN frame is reported with
 * TX_ABORTED. Call from one task only.
 */
bool plat_can_tx(const can_frame_t *frame, uint32_t handle);

/** Take the oldest received frame. Returns false if the receive FIFO is empty. Call from one
 *  task only. */
bool plat_can_rx(plat_can_rx_t *out);

/** Take the oldest event. Returns false if there is none. Taking a TX_DONE or TX_ABORTED
 *  event frees the frame's mailbox. Call from one task only. */
bool plat_can_event(plat_can_event_t *out);

/** Abort all transmit mailboxes. Every frame loaded by plat_can_tx() before this call leaves
 *  its mailbox and is reported with exactly one event: TX_ABORTED, or TX_DONE for a frame
 *  whose transmission was already under way and succeeded. (If that transmission fails, the
 *  frame is not retried: TX_ABORTED.) A frame loaded after this call returned is not
 *  affected, however soon after it comes. */
void plat_can_abort_tx(void);

/** Ask a bus-off controller to start its recovery sequence. */
void plat_can_request_restart(void);

/** Current error state and counters. May be called from any task. */
void plat_can_get_status(plat_can_status_t *out);

#endif /* FCN_PLAT_CAN_H */
