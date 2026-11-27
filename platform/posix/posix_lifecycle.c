/* SPDX-License-Identifier: MIT */
/* Start-up, orderly shutdown and reset of the node process. */
#include "posix_platform.h"

#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include "posix_flash.h"
#include "posix_retained.h"
#include "task.h"

#if defined(FCN_COVERAGE)
#include <gcov.h>
#endif

static volatile sig_atomic_t s_shutdown_requested;

static void shutdown_signal_handler(int signum)
{
    (void)signum;
    s_shutdown_requested = 1;
}

/* A cut of the simulated flash's supply (posix_flash.h) is a loss of supply of the board: the
 * run ends as every reset does, here with PLAT_RESET_POWER_LOSS, which destroys the cell. */
static void flash_power_lost(void)
{
    plat_reset(PLAT_RESET_POWER_LOSS);
}

void posix_platform_init(const posix_config_t *config)
{
    struct sigaction action;

    posix_flash_set_power_loss(flash_power_lost);
    posix_clock_init();
    posix_stack_init();
    posix_tick_configure(config->tick_mode);
    posix_irq_init(config->irq_mode);
    posix_log_start(config->log_path);
    log_init(posix_log_sink, plat_time_ns, config->log_level);

    /* SIGINT and SIGTERM only set a flag. The health task sees it and ends the scheduler, so
     * the process leaves through main() and coverage data and logs are written normally. */
    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = shutdown_signal_handler;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
}

void posix_platform_run(void)
{
    posix_irq_set_enabled(true);
    vTaskStartScheduler();
    posix_irq_set_enabled(false);
}

void posix_platform_shutdown(void)
{
    posix_wdg_shutdown();
    posix_log_stop();
}

bool plat_shutdown_requested(void)
{
    return (s_shutdown_requested != 0);
}

void plat_end_scheduler(void)
{
    posix_irq_set_enabled(false);
    posix_stack_freeze(); /* while every task's thread, the idle task's included, still exists */
    posix_wdg_stop();     /* the board is switched off; the shutdown guard takes over */
    posix_shutdown_guard_arm(POSIX_SHUTDOWN_TIMEOUT_MS);
    vTaskEndScheduler();
}

/* One context ends the process. Two that come to an end at once (the watchdog's thread and a
 * task's assertion, or the shutdown guard next to a reset): the first goes on, and its reason
 * or status is the one recorded and exited with; the other waits here for that exit. */
static void claim_the_end(void)
{
    static atomic_flag s_ending = ATOMIC_FLAG_INIT;

    if (atomic_flag_test_and_set(&s_ending)) {
        for (;;) {
            (void)pause();
        }
    }
}

/* The last step of an end that does not return from main(): the locks of the retained cell
 * and of the flash file, then the process. A lock left to the exit would go only with the
 * memory map, which another process can hold for a moment after the exit (posix_retained.c,
 * THE LOCK BELONGS TO THE OPEN FILE DESCRIPTION), and the next start, which the bench makes as
 * soon as it has seen this exit, would find the file "in use". The cell is written before
 * this (s_locked refuses a write after it), and a task that is still running when the hardware
 * watchdog's thread or the shutdown guard ends the run no longer reaches the flash file
 * (posix_flash_unlock()). */
static _Noreturn void release_and_exit(int status)
{
    posix_retained_unlock();
    posix_flash_unlock();
    _exit(status);
}

void posix_end_process(int status)
{
    claim_the_end();
    release_and_exit(status);
}

void plat_reset(plat_reset_reason_t reason)
{
    const plat_reset_reason_t recorded =
        posix_reset_reason_valid(reason) ? reason : PLAT_RESET_ASSERT;

    /* A reset is abrupt on purpose: like the reset line of a microcontroller it stops the
     * image wherever it is. Only what a test bench would still have afterwards is saved: the
     * retained cell, the log lines already produced and, in coverage builds, the counters.
     * Then the files are given up and the process ends (release_and_exit()).
     * The hardware watchdog's thread is not a task and enters no critical section. */
    if (!posix_on_external_thread()) {
        vPortEnterCritical();
    }

    claim_the_end();
    posix_retained_note_reset(recorded);
    posix_log_drain();
#if defined(FCN_COVERAGE)
    __gcov_dump();
#endif
    release_and_exit((int)recorded);
}
