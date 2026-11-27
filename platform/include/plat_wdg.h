/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The hardware watchdog: an independent watchdog, as the IWDG of an STM32 is.
 *
 * Once started it counts down from its timeout on a clock of its own; a kick starts the count
 * again; if the count runs out, the watchdog resets the device. It cannot be stopped once it
 * runs, by software or by a fault of the software: only a reset ends it, and the next image
 * starts without it (the application starts it again). The reset it causes is
 * PLAT_RESET_WATCHDOG, which the next start reads from plat_reset_cause() (plat_retained.h).
 *
 * WHO KICKS. Exactly one place in the application: the supervising task, after the watchdog
 * manager (lib/wdgm) says the supervised tasks are alive (wdgm_may_kick()). A kick from an
 * interrupt handler or a timer callback would keep the watchdog quiet while every task hangs,
 * so although plat_wdg_kick() works from any context, the application never kicks from one of
 * those.
 *
 * WHAT A KICK COSTS. On a target, one store to the key register (0xAAAA to IWDG_KR): a few
 * cycles, no wait, any context. On the host, one read of CLOCK_MONOTONIC through the vDSO and
 * two atomic operations (a store of the time stamp, an add to the kick count): no lock, no
 * system call in the usual case, nothing that can block, so a FreeRTOS task, a simulated
 * interrupt handler, a critical section and a thread outside the scheduler may all call it.
 * tests/platform/wdg_selftest.c (`kick-cost`) measures it on the host.
 *
 * THE HOST MODEL (platform/posix/posix_wdg.c). A thread outside the scheduler, created by
 * plat_wdg_start(), stands for the watchdog's own clock: every millisecond it adds the time
 * since its last pass, measured on CLOCK_MONOTONIC, to the time counted since the last kick,
 * and when the count reaches the timeout it calls plat_reset(PLAT_RESET_WATCHDOG). It therefore
 * never resets earlier than the timeout after the last kick. A gap of more than 50 ms between
 * two of its passes counts as 50 ms only: the host stopped running the process (a loaded
 * machine, a virtual machine that was descheduled), and the node's tasks could not kick in that
 * time either, just as a target's watchdog does not count while its chip is not clocked. Such
 * gaps delay a reset; they never cause one. Ending the scheduler in an orderly way (host only,
 * plat_end_scheduler()) switches the simulated board off and the watchdog with it.
 */
#ifndef FCN_PLAT_WDG_H
#define FCN_PLAT_WDG_H

#include <stdbool.h>
#include <stdint.h>

/** The shortest timeout plat_wdg_start() accepts, in ms. */
#define PLAT_WDG_TIMEOUT_MIN_MS 1U

/** The longest timeout plat_wdg_start() accepts, in ms: that of an STM32F1 IWDG with its 12-bit
 *  reload register at the maximum (4096 counts), the largest prescaler (256) and the nominal
 *  40 kHz of its LSI clock, 4096 x 256 / 40000 Hz = 26.2144 s, rounded down. The LSI varies
 *  widely from part to part (see the device's datasheet), so a target sets the timeout for its
 *  fastest LSI; the host model keeps time exactly. */
#define PLAT_WDG_TIMEOUT_MAX_MS 26214U

/**
 * Start the watchdog with a timeout of `timeout_ms` (PLAT_WDG_TIMEOUT_MIN_MS to
 * PLAT_WDG_TIMEOUT_MAX_MS). The start counts as the first kick: the first reset can come
 * `timeout_ms` after it.
 *
 * Returns false and changes nothing if the timeout is out of range or the watchdog already
 * runs (a second start does not change the timeout). Call it once, from a task or from main()
 * before the scheduler starts, not from an interrupt handler; it may take a lock of the C
 * library on the host (it creates the model's thread inside a critical section).
 */
bool plat_wdg_start(uint32_t timeout_ms);

/** Start the count again from the timeout. Before plat_wdg_start() it does nothing. Any
 *  context, constant time, never blocks; see WHAT A KICK COSTS and WHO KICKS. */
void plat_wdg_kick(void);

#endif /* FCN_PLAT_WDG_H */
