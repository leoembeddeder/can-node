/* SPDX-License-Identifier: MIT */
#include "can_io.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "can/can_bits.h"
#include "log/log.h"
#include "plat_can.h"
#include "platform.h"

/* The shortest classical frame on the bus: a base-format data frame without data and without
 * a single stuff bit is 44 bits (SOF 1, identifier 11, RTR 1, IDE 1, r0 1, DLC 4, CRC 15, CRC
 * delimiter 1, ACK 2, EOF 7), followed by the intermission. */
#define SHORTEST_FRAME_BITS (44U + CAN_BITS_INTERMISSION)

/* The receive task takes at most one batch per tick when frames keep coming (see rx_task), so
 * a batch per tick must cover the most frames a full bus can deliver in a tick. Otherwise the
 * receive FIFO overruns for as long as the load lasts. */
_Static_assert(((uint64_t)CAN_IO_RX_BATCH * SHORTEST_FRAME_BITS * (uint64_t)configTICK_RATE_HZ) >=
                   (uint64_t)PLAT_CAN_MAX_BITRATE,
               "CAN_IO_RX_BATCH is below the frame rate of a full bus at PLAT_CAN_MAX_BITRATE");

/* Notification bits of the two tasks. */
#define NOTIFY_RX_PENDING 0x01U /* receive task: the controller's receive FIFO has frames */
#define NOTIFY_TX_REQUEST 0x01U /* transmit task: a producer put something into the queue */
#define NOTIFY_TX_EVENT   0x02U /* transmit task: the controller has events */

/* Longest either task waits without being notified. Each loop iteration is also where the
 * task shows that it is alive, so no wait may be unbounded. */
#define RX_IDLE_WAIT_MS 50U
#define TX_IDLE_WAIT_MS 50U

#define TASK_STACK_WORDS 512U

/* Recovery policy of the transmit queue after bus off. */
#define TX_HOLDOFF_INITIAL_MS 100U
#define TX_HOLDOFF_MAX_MS     1600U
#define TX_STABLE_MS          5000U
#define TX_EVENT_TIMEOUT_MS   1000U

static const char TAG[] = "can";

typedef struct {
    bool used;
    bool extended;
    uint32_t id;
    can_io_rx_handler_t handler;
} rx_entry_t;

static StaticTask_t s_rx_tcb;
static StackType_t s_rx_stack[TASK_STACK_WORDS];
static StaticTask_t s_tx_tcb;
static StackType_t s_tx_stack[TASK_STACK_WORDS];
static TaskHandle_t s_rx_task;
static TaskHandle_t s_tx_task;

static rx_entry_t s_rx_table[CAN_IO_RX_HANDLERS];

/* The receive cycle (can_io_on_rx_cycle): set before the scheduler starts, read only by the
 * receive task afterwards. */
static can_io_cycle_handler_t s_rx_cycle_handler;
static TickType_t s_rx_cycle_ticks;

/* The transmit queue is shared by the producers and the transmit task. Every access is a
 * few dozen instructions on a small structure, so it is guarded by a critical section rather
 * than a mutex: shorter than the mutex calls would be, and usable from any priority. */
static can_txq_t s_queue;
static can_io_stats_t s_stats;

/* The alive indications (can_io_on_alive): set before the scheduler starts, read only by the
 * two tasks afterwards. */
static can_io_alive_handler_t s_rx_alive;
static can_io_alive_handler_t s_tx_alive;

/* The transmit-done handler (can_io_on_tx_done): set before the scheduler starts, read only by
 * the transmit task afterwards. */
static can_io_tx_done_handler_t s_tx_done_handler;
static uint32_t s_tx_done_id;

/* Interrupt context: turn the controller's causes into notifications. */
static void can_io_isr(void *context, uint32_t causes, BaseType_t *woken)
{
    (void)context;

    if ((causes & PLAT_CAN_IRQ_RX) != 0U) {
        (void)xTaskNotifyFromISR(s_rx_task, NOTIFY_RX_PENDING, eSetBits, woken);
    }

    if ((causes & PLAT_CAN_IRQ_EVENT) != 0U) {
        (void)xTaskNotifyFromISR(s_tx_task, NOTIFY_TX_EVENT, eSetBits, woken);
    }
}

/* ------------------------------------------------------------------------------- receive */

static void dispatch(const plat_can_rx_t *received)
{
    const bool extended = (received->frame.flags & CAN_FLAG_EXT) != 0U;
    const bool remote = (received->frame.flags & CAN_FLAG_RTR) != 0U;
    bool handled = false;

    if (!remote) {
        for (uint32_t i = 0U; (i < CAN_IO_RX_HANDLERS) && !handled; i++) {
            const rx_entry_t *entry = &s_rx_table[i];

            if (entry->used && (entry->id == received->frame.id) && (entry->extended == extended)) {
                entry->handler(&received->frame, received->eof_ns);
                handled = true;
            }
        }
    }

    if (!handled) {
        s_stats.rx_unhandled++;
    }
}

/* Ticks the receive task may wait for a notification: the idle wait, or less if the period of
 * the receive cycle that began at `cycle_start` ends sooner (0 if it has ended). Differences
 * of tick counts, so that a tick count that wraps does no harm. */
static TickType_t rx_wait_ticks(TickType_t cycle_start)
{
    TickType_t wait = pdMS_TO_TICKS(RX_IDLE_WAIT_MS);

    if (s_rx_cycle_handler != NULL) {
        const TickType_t elapsed = xTaskGetTickCount() - cycle_start;
        const TickType_t left = (elapsed < s_rx_cycle_ticks) ? (s_rx_cycle_ticks - elapsed) : 0U;

        if (left < wait) {
            wait = left;
        }
    }

    return wait;
}

/* The end of a period of the receive cycle, if it has come: one call of the handler, however
 * many periods have ended since `cycle_start`. Returns the start of the period now running. */
static TickType_t run_rx_cycle(TickType_t cycle_start)
{
    TickType_t start = cycle_start;

    if (s_rx_cycle_handler != NULL) {
        const TickType_t elapsed = xTaskGetTickCount() - cycle_start;

        if (elapsed >= s_rx_cycle_ticks) {
            const TickType_t periods = elapsed / s_rx_cycle_ticks;

            s_rx_cycle_handler();
            s_stats.rx_cycles++;
            s_stats.rx_cycles_skipped += (uint32_t)(periods - 1U);
            start = cycle_start + (periods * s_rx_cycle_ticks);
        }
    }

    return start;
}

static void rx_task(void *arg)
{
    TickType_t cycle_start;

    (void)arg;
    plat_task_enter("can_rx");
    cycle_start = xTaskGetTickCount();

    for (;;) {
        uint32_t bits = 0U;
        uint32_t handled = 0U;
        plat_can_rx_t received;

        (void)xTaskNotifyWait(0U, UINT32_MAX, &bits, rx_wait_ticks(cycle_start));

        if (s_rx_alive != NULL) {
            s_rx_alive();
        }

        /* Drain the controller's FIFO, but only a bounded number of frames per activation:
         * this task outranks every other, and its execution time per activation has to have
         * a bound for the schedulability analysis to mean anything. */
        while ((handled < CAN_IO_RX_BATCH) && plat_can_rx(&received)) {
            s_stats.rx_frames++;
            dispatch(&received);
            handled++;
        }

        if (handled == CAN_IO_RX_BATCH) {
            /* More may be waiting: come back for it after lower-priority work had a chance
             * to run for one tick. */
            s_stats.rx_batches_cut++;
            vTaskDelay(1U);
            (void)xTaskNotify(s_rx_task, NOTIFY_RX_PENDING, eSetBits);
        }

        /* After the frames, so that a frame that arrived before the end of a period counts
         * in that period. */
        cycle_start = run_rx_cycle(cycle_start);
    }
}

/* ------------------------------------------------------------------------------ transmit */

static void carry_out(uint32_t requests)
{
    if ((requests & CAN_TXQ_REQUEST_ABORT_MAILBOXES) != 0U) {
        plat_can_abort_tx();
    }

    if ((requests & CAN_TXQ_REQUEST_RESTART) != 0U) {
        LOG_WRN(TAG, "bus off: requesting recovery");
        plat_can_request_restart();
    }
}

static void handle_controller_events(void)
{
    plat_can_event_t event;

    while (plat_can_event(&event)) {
        uint32_t requests = 0U;
        can_txq_done_t done = {.cyclic = true, .slot = 0U, .id = 0U, .stamp_ns = 0U};
        bool report = false;

        taskENTER_CRITICAL();

        switch (event.kind) {
        case PLAT_CAN_EVENT_TX_DONE:
            /* An event frame of the registered identifier goes to its handler, after the
             * critical section; the queue's record says which frame the handle was. */
            report = can_txq_on_tx_done(&s_queue, event.handle, &done) && !done.cyclic &&
                     (s_tx_done_handler != NULL) && (done.id == s_tx_done_id);

            if (report) {
                s_stats.tx_done_reports++;
            }
            break;
        case PLAT_CAN_EVENT_TX_ABORTED:
            (void)can_txq_on_tx_aborted(&s_queue, event.handle);
            break;
        case PLAT_CAN_EVENT_STATE:
            requests = can_txq_on_state(&s_queue, (can_fc_state_t)event.state, plat_time_ns());

            if (event.state == PLAT_CAN_ERROR_PASSIVE) {
                s_stats.error_passive_entries++;
            }
            break;
        default:
            break;
        }

        taskEXIT_CRITICAL();

        if (report) {
            s_tx_done_handler(done.stamp_ns);
        }

        if (event.kind == PLAT_CAN_EVENT_STATE) {
            LOG_WRN(TAG, "controller state %u -> %u (TEC %u, REC %u)",
                    (unsigned int)event.old_state, (unsigned int)event.state,
                    (unsigned int)event.tec, (unsigned int)event.rec);
        }

        carry_out(requests);
    }
}

static void load_mailboxes(void)
{
    bool more = true;

    while (more) {
        can_frame_t frame;
        uint32_t handle = 0U;

        taskENTER_CRITICAL();
        more = can_txq_next(&s_queue, plat_time_ns(), &frame, &handle);
        taskEXIT_CRITICAL();

        if (more && !plat_can_tx(&frame, handle)) {
            /* The queue never hands out more frames than the controller has mailboxes, so
             * this means the two disagree. Put the frame back, say so, and try again on the
             * next activation rather than in a loop. */
            taskENTER_CRITICAL();
            (void)can_txq_on_tx_aborted(&s_queue, handle);
            taskEXIT_CRITICAL();
            s_stats.tx_load_failed++;
            more = false;
        }
    }
}

/* Ticks to wait: until the queue's next time-driven deadline, but never longer than the idle
 * wait and never zero (a deadline that has passed is handled in this iteration anyway). */
static TickType_t tx_wait_ticks(void)
{
    uint64_t deadline_ns;
    uint64_t wait_ms = TX_IDLE_WAIT_MS;

    taskENTER_CRITICAL();
    deadline_ns = can_txq_next_deadline(&s_queue);
    taskEXIT_CRITICAL();

    if (deadline_ns != CAN_TXQ_NO_DEADLINE) {
        const uint64_t now_ns = plat_time_ns();
        const uint64_t until_ms =
            (deadline_ns > now_ns) ? (((deadline_ns - now_ns) / 1000000ULL) + 1ULL) : 1ULL;

        if (until_ms < wait_ms) {
            wait_ms = until_ms;
        }
    }

    return pdMS_TO_TICKS((TickType_t)wait_ms);
}

static void tx_task(void *arg)
{
    (void)arg;
    plat_task_enter("can_tx");

    for (;;) {
        uint32_t bits = 0U;
        uint32_t requests;

        (void)xTaskNotifyWait(0U, UINT32_MAX, &bits, tx_wait_ticks());

        if (s_tx_alive != NULL) {
            s_tx_alive();
        }

        /* Whatever woke the task, look at everything: the three steps are cheap when there
         * is nothing to do, and a cause can arrive while another is being handled. */
        handle_controller_events();

        taskENTER_CRITICAL();
        requests = can_txq_poll(&s_queue, plat_time_ns());
        taskEXIT_CRITICAL();
        carry_out(requests);

        load_mailboxes();
    }
}

/* ------------------------------------------------------------------------------ interface */

void can_io_start(void)
{
    const can_txq_config_t config = {.mailboxes = (uint8_t)CAN_IO_TX_MAILBOXES,
                                     .holdoff_initial_ms = TX_HOLDOFF_INITIAL_MS,
                                     .holdoff_max_ms = TX_HOLDOFF_MAX_MS,
                                     .stable_ms = TX_STABLE_MS,
                                     .event_timeout_ms = TX_EVENT_TIMEOUT_MS};

    can_txq_init(&s_queue, &config);
    s_rx_task = xTaskCreateStatic(rx_task, "can_rx", TASK_STACK_WORDS, NULL, CAN_IO_RX_PRIORITY,
                                  s_rx_stack, &s_rx_tcb);
    s_tx_task = xTaskCreateStatic(tx_task, "can_tx", TASK_STACK_WORDS, NULL, CAN_IO_TX_PRIORITY,
                                  s_tx_stack, &s_tx_tcb);
    configASSERT((s_rx_task != NULL) && (s_tx_task != NULL));
    plat_can_set_isr(can_io_isr, NULL);
}

bool can_io_on_frame(uint32_t id, bool extended, can_io_rx_handler_t handler)
{
    bool registered = false;

    for (uint32_t i = 0U; (i < CAN_IO_RX_HANDLERS) && !registered; i++) {
        if (!s_rx_table[i].used) {
            s_rx_table[i].used = true;
            s_rx_table[i].extended = extended;
            s_rx_table[i].id = id;
            s_rx_table[i].handler = handler;
            registered = true;
        }
    }

    return registered;
}

void can_io_on_rx_cycle(uint32_t period_ms, can_io_cycle_handler_t handler)
{
    const TickType_t ticks = pdMS_TO_TICKS(period_ms);

    s_rx_cycle_ticks = (ticks > 0U) ? ticks : 1U;
    s_rx_cycle_handler = handler;
}

void can_io_on_alive(can_io_alive_handler_t rx_alive, can_io_alive_handler_t tx_alive)
{
    s_rx_alive = rx_alive;
    s_tx_alive = tx_alive;
}

bool can_io_publish(uint8_t slot, const can_frame_t *frame, uint64_t stamp_ns)
{
    bool accepted;

    taskENTER_CRITICAL();
    accepted = can_txq_publish(&s_queue, slot, frame, stamp_ns);
    taskEXIT_CRITICAL();

    if (accepted) {
        (void)xTaskNotify(s_tx_task, NOTIFY_TX_REQUEST, eSetBits);
    }

    return accepted;
}

bool can_io_send_event(const can_frame_t *frame)
{
    bool accepted;

    taskENTER_CRITICAL();
    accepted = can_txq_enqueue(&s_queue, frame, plat_time_ns());
    taskEXIT_CRITICAL();

    if (accepted) {
        (void)xTaskNotify(s_tx_task, NOTIFY_TX_REQUEST, eSetBits);
    }

    return accepted;
}

bool can_io_send_event_stamped(const can_frame_t *frame, uint64_t *stamp_ns)
{
    bool accepted;
    bool wake;
    uint64_t now_ns;

    taskENTER_CRITICAL();
    now_ns = plat_time_ns();
    accepted = can_txq_enqueue(&s_queue, frame, now_ns);
    /* Counted after the enqueue, in its critical section: the enqueue has discarded the event
     * frames that had expired, so any other frame still held is one that will be reported or
     * loaded (see the header), and none can leave before this one is in. */
    wake = accepted && (can_txq_events_held(&s_queue) == 1U);

    if (accepted && !wake) {
        s_stats.tx_quiet_enqueues++;
    }

    taskEXIT_CRITICAL();

    if (accepted) {
        *stamp_ns = now_ns;

        if (wake) {
            (void)xTaskNotify(s_tx_task, NOTIFY_TX_REQUEST, eSetBits);
        }
    }

    return accepted;
}

void can_io_on_tx_done(uint32_t id, can_io_tx_done_handler_t handler)
{
    s_tx_done_id = id;
    s_tx_done_handler = handler;
}

void can_io_get_stats(can_io_stats_t *io, can_txq_stats_t *queue)
{
    taskENTER_CRITICAL();

    if (io != NULL) {
        *io = s_stats;
    }

    if (queue != NULL) {
        *queue = *can_txq_stats(&s_queue);
    }

    taskEXIT_CRITICAL();
}

void can_io_get_slot_stats(can_txq_slot_stats_t slots[CAN_TXQ_CYCLIC_SLOTS])
{
    taskENTER_CRITICAL();

    for (uint8_t slot = 0U; slot < CAN_TXQ_CYCLIC_SLOTS; slot++) {
        slots[slot] = *can_txq_slot_stats(&s_queue, slot);
    }

    taskEXIT_CRITICAL();
}
