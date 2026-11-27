/* SPDX-License-Identifier: MIT */
/*
 * Simulated interrupts for the models of the peripherals.
 *
 * On a microcontroller a CAN controller raises an interrupt and the ISR hands the event to a
 * task. On the host the "controller" is a thread outside the scheduler, and it may not call
 * any FreeRTOS function. This file gives it an interrupt line instead.
 *
 * posix_irq_raise() latches the line in an atomic word and sends SIGUSR2 to the process.
 * The port keeps signals blocked in every thread except the one that runs the current task,
 * and blocks them there too during a critical section, so the signal is taken by the running
 * task's thread and only while "interrupts are enabled". The handler runs the attached ISRs,
 * which use the FromISR kernel API and request a context switch the usual way.
 *
 * One handler at a time. An interrupt does not preempt its own handler, and a task's stack is
 * sized on that assumption. The port's tick handler gets this for free: it counts the
 * critical nesting up and down itself and leaves the signal mask alone, so signals stay
 * blocked until the handler returns. Code outside the port cannot reach that counter. The
 * handler here has to use vPortEnterCritical() and vPortExitCritical(), a yield from an ISR
 * does the same, and the port's exit path unblocks every signal the moment the count is back
 * at zero, with the handler still on the stack. A signal pending at that instant is then
 * handled on top of the handler's frame; at a high interrupt rate handlers pile up until the
 * stack overflows (docs/bug-log.md, entry 002). pthread_sigmask() is therefore intercepted at
 * link time (-Wl,--wrap, like usleep in posix_tick.c) and an unblock requested while the
 * calling thread is inside the handler is dropped. The mask comes back when the handler
 * returns, exactly as for the tick.
 *
 * The tick hook polls the same latch. In POSIX_IRQ_POLL mode that is the only delivery path:
 * an interrupt is served at the next tick the host delivers, nominally within one tick
 * period. In POSIX_IRQ_SIGNAL mode it is a safety net, so that a line raised while delivery
 * was disabled is served at the next tick. How late that can be is the host's decision, not
 * a property of this code; results/platform_timing.json has the measured distribution.
 *
 * Because the safety net serves whatever the signal path misses, a broken signal path would
 * look like a slow one. The self-tests therefore switch the net off for one case
 * (posix_irq_test_set_tick_fallback, a test-only switch) and require that every signal was
 * served by the signal handler alone.
 */
#include "posix_platform.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include "task.h"

#define IRQ_SIGNAL SIGUSR2 /* SIGUSR1 is taken by the port, SIGALRM is its tick */

typedef struct {
    posix_isr_fn isr;
    void *context;
} irq_line_t;

static irq_line_t s_lines[POSIX_IRQ_LINES];
static posix_irq_mode_t s_mode = POSIX_IRQ_SIGNAL;
static _Atomic uint32_t s_pending;
static _Atomic bool s_enabled;
static _Atomic uint64_t s_raised;
static _Atomic uint64_t s_signals_sent;
static _Atomic uint64_t s_serviced_by_signal;
static _Atomic uint64_t s_serviced_by_tick;
static _Atomic uint32_t s_max_nesting;
/* Test-only (posix_irq_test_set_tick_fallback): false switches the tick hook's safety net off
 * in POSIX_IRQ_SIGNAL mode. The node never changes it. */
static _Atomic bool s_tick_fallback = true;

/* How many frames of irq_signal_handler() are on this thread's stack. It stays set while the
 * thread is parked inside the handler after a context switch. */
static _Thread_local uint32_t t_handler_depth;

/* The names are fixed by the linker's --wrap convention. */
int __real_pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
int __wrap_pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);

/* Every pthread_sigmask() call of the program, the port's included, arrives here. Inside the
 * interrupt handler an unblock is dropped (see the top of this file); everything else is
 * passed on unchanged. */
int __wrap_pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    if ((t_handler_depth != 0U) && (how == SIG_UNBLOCK)) {
        /* Nothing changes; report the current mask if the caller asked for it. */
        return (oldset != NULL) ? __real_pthread_sigmask(SIG_BLOCK, NULL, oldset) : 0;
    }

    return __real_pthread_sigmask(how, set, oldset);
}

/* Run the ISR of every pending line. Interrupt context. Returns true if any line was served. */
static bool dispatch(BaseType_t *woken)
{
    uint32_t pending = atomic_exchange(&s_pending, 0U);
    const bool any = (pending != 0U);

    for (unsigned int line = 0U; pending != 0U; line++) {
        if (((pending & 1U) != 0U) && (s_lines[line].isr != NULL)) {
            s_lines[line].isr(s_lines[line].context, woken);
        }

        pending >>= 1U;
    }

    return any;
}

static void irq_signal_handler(int signum)
{
    const int saved_errno = errno;

    (void)signum;
    t_handler_depth++;

    /* Stays 1 as long as nothing unblocks signals inside the handler; the self-tests and the
     * node's statistics check that it does. */
    if (t_handler_depth > atomic_load(&s_max_nesting)) {
        atomic_store(&s_max_nesting, t_handler_depth);
    }

    if (atomic_load(&s_enabled)) {
        BaseType_t woken = pdFALSE;

        /* The handler runs with every signal blocked (sa_mask). The critical section gives
         * the port's nesting count the value it has in the port's own tick handler, so that
         * kernel code which checks or nests critical sections sees an interrupt context. */
        vPortEnterCritical();

        if (dispatch(&woken)) {
            (void)atomic_fetch_add(&s_serviced_by_signal, 1U);
        }

        /* May switch to another task: this thread is then parked right here, inside the
         * handler, and continues when its task is scheduled again. */
        portYIELD_FROM_ISR(woken);
        vPortExitCritical();
    }

    t_handler_depth--;
    errno = saved_errno;
}

void posix_irq_init(posix_irq_mode_t mode)
{
    struct sigaction action;
    sigset_t block;

    s_mode = mode;
    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = irq_signal_handler;
    action.sa_flags = SA_RESTART;
    (void)sigfillset(&action.sa_mask);
    (void)sigaction(IRQ_SIGNAL, &action, NULL);

    /* Until the port has set up its own masks, keep the signal away from the calling (main)
     * thread and from every thread created from it. Task threads unblock it when they run. */
    (void)sigemptyset(&block);
    (void)sigaddset(&block, IRQ_SIGNAL);
    (void)pthread_sigmask(SIG_BLOCK, &block, NULL);
}

void posix_irq_attach(unsigned int line, posix_isr_fn isr, void *context)
{
    if (line < POSIX_IRQ_LINES) {
        s_lines[line].isr = isr;
        s_lines[line].context = context;
    } else {
        configASSERT(line < POSIX_IRQ_LINES);
    }
}

void posix_irq_set_enabled(bool enabled)
{
    atomic_store(&s_enabled, enabled);
}

void posix_irq_raise(unsigned int line)
{
    if (line >= POSIX_IRQ_LINES) {
        return; /* no such line; this function has no way to report it and must not assert */
    }

    const uint32_t bit = (uint32_t)1U << line;
    const uint32_t before = atomic_fetch_or(&s_pending, bit);

    (void)atomic_fetch_add(&s_raised, 1U);

    /* If something was already latched, the signal for it is still on its way and its handler
     * will serve this line as well. */
    if ((s_mode == POSIX_IRQ_SIGNAL) && (before == 0U) && atomic_load(&s_enabled)) {
        (void)atomic_fetch_add(&s_signals_sent, 1U);
        (void)kill(getpid(), IRQ_SIGNAL);
    }
}

void posix_irq_test_set_tick_fallback(bool enabled)
{
    atomic_store(&s_tick_fallback, enabled);
}

void posix_irq_poll_from_tick(void)
{
    /* In signal mode this is only the safety net, and a self-test may have switched it off.
     * In poll mode it is the delivery path and always runs. */
    const bool wanted = (s_mode == POSIX_IRQ_POLL) || atomic_load(&s_tick_fallback);

    if (wanted && atomic_load(&s_enabled) && (atomic_load(&s_pending) != 0U)) {
        BaseType_t woken = pdFALSE;

        /* Called from the tick interrupt: a task made ready here is switched to when the
         * tick handler returns, so `woken` needs no further action. */
        if (dispatch(&woken)) {
            (void)atomic_fetch_add(&s_serviced_by_tick, 1U);
        }
    }
}

void posix_irq_get_stats(posix_irq_stats_t *out)
{
    out->raised = atomic_load(&s_raised);
    out->signals_sent = atomic_load(&s_signals_sent);
    out->serviced_by_signal = atomic_load(&s_serviced_by_signal);
    out->serviced_by_tick = atomic_load(&s_serviced_by_tick);
    out->max_nesting = atomic_load(&s_max_nesting);
    out->pending = atomic_load(&s_pending);
}
