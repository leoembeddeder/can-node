/* SPDX-License-Identifier: MIT */
/**
 * @file
 * Host-only part of the platform layer: start-up, the models' way into the scheduler
 * (simulated interrupts) and the statistics the measurement scripts read.
 *
 * Threading rules of the FreeRTOS POSIX port that this layer is built around:
 *
 *  - Every FreeRTOS task is a pthread, but only one of them runs at a time. The port switches
 *    tasks from a signal handler (SIGALRM, the tick) or from a yield, and a critical section
 *    is "all signals blocked in the running task's thread".
 *  - A thread that the port did not create is invisible to the scheduler. It must block all
 *    signals (posix_external_thread_setup) and must never call a FreeRTOS API, not even the
 *    FromISR variants: nothing would stop the kernel's data from being modified under it.
 *  - Functions of the C library that take a lock (stdio streams, malloc) must not be called
 *    by a task where it can be switched out: a task switched out while it holds the lock
 *    deadlocks the next task that wants it. Tasks here never write to a stream and never
 *    allocate from the C library; formatting into a stack buffer with vsnprintf() takes no
 *    lock, and the one call that does allocate (in plat_task_enter()) is made inside a
 *    critical section.
 *
 * The models of the peripherals therefore live in external threads and talk to tasks through
 * lock-free rings plus a simulated interrupt (posix_irq_raise).
 */
#ifndef FCN_POSIX_PLATFORM_H
#define FCN_POSIX_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"

#include "log/log.h"
#include "platform.h"

/* ------------------------------------------------------------------------------ start-up */

typedef enum {
    POSIX_TICK_LOCKED = 0, /**< tick count locked to CLOCK_MONOTONIC (default) */
    POSIX_TICK_STOCK = 1   /**< the port's unmodified behaviour, kept for comparison */
} posix_tick_mode_t;

typedef enum {
    POSIX_IRQ_SIGNAL = 0, /**< deliver simulated interrupts with a signal (default) */
    POSIX_IRQ_POLL = 1    /**< only poll for them in the tick hook: served at the next tick */
} posix_irq_mode_t;

typedef struct {
    posix_tick_mode_t tick_mode;
    posix_irq_mode_t irq_mode;
    log_level_t log_level;
    const char *log_path; /**< NULL: log to stderr */
} posix_config_t;

/** Call once from main() before any FreeRTOS object is created. */
void posix_platform_init(const posix_config_t *config);

/** Start the scheduler. Returns after a task called plat_end_scheduler(). */
void posix_platform_run(void);

/** Stop the helper threads and flush the log. Call after posix_platform_run() returned. */
void posix_platform_shutdown(void);

/* --------------------------------------------------------- threads outside the scheduler */

/** First call in every thread that is not a FreeRTOS task: blocks all signals in it so the
 *  port's tick and the simulated interrupts are only ever handled by task threads. */
void posix_external_thread_setup(const char *name);

/** True in a thread that called posix_external_thread_setup(): one the scheduler does not
 *  know, which must not enter a FreeRTOS critical section. */
bool posix_on_external_thread(void);

/* -------------------------------------------------------------------- simulated interrupts */

#define POSIX_IRQ_LINES 8U

/** An interrupt service routine. Runs in the port's interrupt context: only FromISR kernel
 *  calls are allowed, and it sets *woken like a real ISR to request a context switch. */
typedef void (*posix_isr_fn)(void *context, BaseType_t *woken);

/** Connect an ISR to a line. Call before the scheduler starts. */
void posix_irq_attach(unsigned int line, posix_isr_fn isr, void *context);

/** Latch the line as pending and interrupt the running task. This is the one function an
 *  external thread may call to reach the scheduler. Safe to call from several threads. */
void posix_irq_raise(unsigned int line);

/** Service pending lines from the tick hook: the delivery path of POSIX_IRQ_POLL and the
 *  safety net of POSIX_IRQ_SIGNAL. */
void posix_irq_poll_from_tick(void);

/** Allow or stop delivery; the platform enables it around the scheduler's lifetime. */
void posix_irq_set_enabled(bool enabled);

/** Test-only: false switches off the tick hook's safety net in POSIX_IRQ_SIGNAL mode, so that
 *  a self-test can show that the signal path alone serves every interrupt. No effect in
 *  POSIX_IRQ_POLL mode, where the tick hook is the delivery path. The node never calls it.
 *  Call before the scheduler starts. */
void posix_irq_test_set_tick_fallback(bool enabled);

typedef struct {
    uint64_t raised;             /**< calls to posix_irq_raise() */
    uint64_t signals_sent;       /**< of those, how many had to send a signal */
    uint64_t serviced_by_signal; /**< dispatch passes that found work, from the signal handler */
    uint64_t serviced_by_tick;   /**< dispatch passes that found work, from the tick hook */
    uint32_t max_nesting;        /**< deepest nesting of the signal handler: 1, or 0 if unused */
    uint32_t pending;            /**< lines latched now and not yet served, one bit per line */
} posix_irq_stats_t;

void posix_irq_get_stats(posix_irq_stats_t *out);

/* ------------------------------------------------------------------------------- the tick */

typedef struct {
    posix_tick_mode_t mode;
    bool tick_thread_seen;  /**< the wrapper recognised the port's tick thread */
    uint64_t signals_sent;  /**< tick signals the port's thread has raised */
    uint64_t catch_ups;     /**< extra signals sent to make up for a lost tick */
    uint64_t dropped_ticks; /**< ticks given up because the count had fallen too far behind */
    uint64_t give_backs;    /**< raises skipped because the kernel already held that tick: a
                                 made-up tick that was one too many (posix_tick_plan.h) */
} posix_tick_stats_t;

void posix_tick_get_stats(posix_tick_stats_t *out);

/** When tick `tick` (a tick count, 1 or more) is due on the locked tick's grid: the base the
 *  host-only trace records (posix_trace_tick_base()) plus (tick - 1) periods. 0 in stock mode,
 *  which keeps no grid, and before the port's tick thread first ran. Any thread; the base it
 *  reads moves only when ticks are given up. */
uint64_t posix_tick_due_ns(uint64_t tick);

/* ------------------------------------------------------------- the CAN controller hardware */

/**
 * Open the transport named by `backend_spec` ("udp:29500", "socketcan:vcan0") and start the
 * hardware thread that runs the controller model on it. `bitrate` 0 switches pacing off.
 * Call after posix_platform_init() and before the scheduler starts. On failure returns
 * false with a message for the user in `error`.
 */
bool posix_can_start(const char *backend_spec, uint32_t bitrate, char *error, size_t error_len);

/**
 * Put the bus fault `name` into the model from its start, as simulation control's
 * "can.fault <name>" puts it in from the moment it is served: none, short, dominant or noack
 * (docs/can-simulation.md). The node's --can-fault, for a test that must not race the node to
 * apply the fault. Returns false, changing nothing, for NULL or any other name. Call before
 * posix_can_start().
 */
bool posix_can_set_start_fault(const char *name);

/** Stop the hardware thread and close the transport. Call after the scheduler has ended. */
void posix_can_stop(void);

/** Exit status of a node whose CAN transport went away while it ran (the SocketCAN interface
 *  was deleted): the node stopped in an orderly way and wrote its statistics. An interface
 *  that only went down does not stop the node. */
#define POSIX_EXIT_CAN_TRANSPORT_LOST 74

/** The errno that ended the transport (ENODEV or ENXIO from a send or a receive),
 *  or 0 while it works. Once it is set, the hardware thread has asked for an orderly stop of
 *  the node. Any thread. */
int posix_can_transport_error(void);

/**
 * How late the host carried out the model's times, measured by the hardware thread. Each
 * figure is the time from a moment of the model's timeline to when the thread acted on it:
 *
 *  - pacing: a transmitted frame's end of frame -> its write to the transport;
 *  - wake-up: the deadline the thread slept until -> the start of the pass that served it.
 *    This covers every host stall while the model had something due, whatever is on the bus;
 *  - rx: a received frame's end of frame -> its delivery to the receive FIFO.
 *
 * A host stall while a frame waits for the bus does not show in the pacing lag: the model
 * moves that frame's start to the late wake-up (can_ctrl late_resyncs), and the stall becomes
 * part of the frame's start_ns - submit_ns. It does show in the wake-up lag.
 */
typedef struct {
    uint64_t frames_paced;       /**< transmitted frames the pacing figures cover */
    uint64_t pacing_lag_max_ns;  /**< worst lateness of a write against the modelled end of frame */
    uint64_t pacing_lag_mean_ns; /**< mean of the same */
    uint64_t wakeups;            /**< passes that served a deadline the thread slept until */
    uint64_t wakeup_lag_max_ns;  /**< worst lateness of such a pass against its deadline */
    uint64_t wakeup_lag_mean_ns; /**< mean of the same */
    uint64_t rx_delivered;       /**< received frames the rx figures cover: all the model
                                       delivered, the ones lost to a FIFO overrun included */
    uint64_t rx_lag_max_ns;      /**< worst lateness of a delivery against its end of frame */
    uint64_t rx_lag_mean_ns;     /**< mean of the same */
    uint32_t rx_fifo_overruns;   /**< received frames lost because the driver did not drain */
    uint32_t rx_fifo_pending;    /**< received frames in the FIFO, not taken by the driver */
    uint32_t event_fifo_overruns; /**< transmit outcomes that found the event FIFO full: 0, by
                                       the argument at the top of posix_can.c */
    uint32_t state_events_merged; /**< state changes not queued when they happened because the
                                       driver was far behind; the latest state was sent later */
} posix_can_stats_t;

/** The figures as of now. Any thread; after posix_can_stop() they are final. */
void posix_can_get_stats(posix_can_stats_t *out);

/** Everything the hardware side counted, for the end-of-run report. Only valid after
 *  posix_can_stop(): the model is then no longer being changed by its thread. */
typedef struct {
    const char *transport; /**< "udp" or "socketcan" */
    uint32_t bitrate;
    uint64_t elapsed_ns; /**< how long the hardware thread ran */
    uint8_t state;       /**< final error state, PLAT_CAN_* numbering */
    uint16_t tec;
    uint16_t rec;
    uint64_t tx_frames;
    uint64_t tx_attempts_failed;
    uint64_t tx_aborted;
    uint64_t rx_frames;
    uint64_t rx_errors;
    uint64_t rx_lost_fault;
    uint64_t wire_overruns;
    uint64_t bus_off_events;
    uint64_t late_resyncs;
    uint64_t busy_ns;
    uint64_t transport_tx_frames;
    uint64_t transport_tx_failed;
    uint64_t transport_rx_frames;
    uint64_t transport_rx_failed;
    uint64_t transport_rx_malformed;
    uint64_t transport_rx_overflow;
    int transport_error; /**< see posix_can_transport_error() */
} posix_can_report_t;

void posix_can_get_report(posix_can_report_t *out);

/* ---------------------------------------------------------------- the hardware watchdog */

/** What the watchdog model (posix_wdg.c, plat_wdg.h) counted. */
typedef struct {
    bool started;                 /**< plat_wdg_start() succeeded */
    uint32_t timeout_ms;          /**< its timeout; 0 if not started */
    uint64_t kicks;               /**< plat_wdg_kick() calls since the start */
    uint64_t longest_unkicked_ns; /**< the longest time counted without a kick: the timeout
                                       minus this is how close the node came to a reset */
    uint64_t gaps;                /**< passes of the model's thread that came more than 50 ms
                                       after the previous one: host stalls, counted as 50 ms */
    uint64_t longest_gap_ns;      /**< the longest such gap */
} posix_wdg_stats_t;

/** The figures as of now. Any thread. */
void posix_wdg_get_stats(posix_wdg_stats_t *out);

/** Stop counting: the orderly end of a run (plat_end_scheduler()). Any thread. */
void posix_wdg_stop(void);

/** Stop counting and join the model's thread. Main thread, after the scheduler has ended. */
void posix_wdg_shutdown(void);

/* -------------------------------------------------------------------------------- logging */

/** Number of log lines discarded because the log ring was full. */
uint32_t posix_log_dropped(void);

/* Internal to platform/posix. */
void posix_clock_init(void);
uint64_t posix_monotonic_ns(void);
void posix_tick_configure(posix_tick_mode_t mode);
void posix_tick_taken(void);
void posix_irq_init(posix_irq_mode_t mode);
void posix_log_start(const char *path);
void posix_log_stop(void);
void posix_log_drain(void);
void posix_log_sink(const char *line, size_t len);
void posix_stack_init(void);
void posix_stack_freeze(void);
void posix_tick_check_active(void);
void posix_shutdown_guard_arm(uint32_t timeout_ms);
/* End the process at once with `status`, from any thread, where the end is no reset (the
 * shutdown guard): one context ends the process, so a caller that comes while a reset or
 * another call is ending it waits for that exit instead; then the locks of the retained cell
 * and of the flash file are released, as plat_reset() releases them, and the process exits. */
_Noreturn void posix_end_process(int status);

/* Exit status when ending the scheduler hung and the guard had to end the process. */
#define POSIX_EXIT_SHUTDOWN_TIMEOUT 70
/* How long an orderly stop may take before the guard steps in. */
#define POSIX_SHUTDOWN_TIMEOUT_MS 5000U

#endif /* FCN_POSIX_PLATFORM_H */
