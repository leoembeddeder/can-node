/* SPDX-License-Identifier: MIT */
/**
 * @file
 * What the application expects from whatever it runs on.
 *
 * platform/posix implements this for the Linux host under the FreeRTOS POSIX port. A target
 * build provides its own implementation; nothing in app/ or lib/ may include a host header.
 */
#ifndef FCN_PLATFORM_H
#define FCN_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Why the system is being reset. On the host the value is the exit status of the node
 * process, which is how the supervising test harness learns what happened, and plat_reset()
 * also writes it into the retained-RAM cell (plat_retained.h), from which the next start
 * learns it; on a target it would be stored in a reset-cause register or a no-init RAM cell.
 *
 * PLAT_RESET_POWER_ON is not a reason for a reset but the cause of a start that follows none:
 * the supply came up, or the retained cell held nothing valid. plat_reset_cause() returns it;
 * plat_reset() never takes it.
 */
typedef enum {
    PLAT_RESET_POWER_ON = 0,     /**< no reset: a start from power-on (plat_reset_cause() only) */
    PLAT_RESET_REQUESTED = 10,   /**< commanded: UDS ECUReset or a command frame */
    PLAT_RESET_WATCHDOG = 11,    /**< a task stopped checking in */
    PLAT_RESET_POWER_LOSS = 12,  /**< simulated loss of supply (host only) */
    PLAT_RESET_IMAGE_FAULT = 13, /**< the running image crashed (simulated on the host) */
    PLAT_RESET_ASSERT = 20,      /**< configASSERT() failed */
    PLAT_RESET_STACK_OVERFLOW = 21,
    PLAT_RESET_MALLOC_FAILED = 22
} plat_reset_reason_t;

/** Monotonic time in nanoseconds. On the host this is CLOCK_MONOTONIC, so timestamps taken
 *  inside the node can be compared with timestamps taken by another process. */
uint64_t plat_time_ns(void);

/** Free-running microsecond counter since platform initialisation; the time base of the
 *  FreeRTOS run-time statistics. */
uint64_t plat_run_time_counter_us(void);

/** Called by configASSERT(). Reports the location and resets with PLAT_RESET_ASSERT. */
_Noreturn void plat_assert_failed(const char *file, int line);

/**
 * Reset the system immediately. Nothing of the running image executes after this call.
 *
 * `reason` is recorded where the next start finds it (plat_reset_cause()). A value that is not
 * one of the reset reasons above, PLAT_RESET_POWER_ON included, is a programming error and
 * resets as PLAT_RESET_ASSERT. PLAT_RESET_POWER_LOSS models a loss of supply: the retained
 * cell is lost with it, so the next start is a power-on.
 *
 * Any context: a task, an interrupt handler, a critical section, and on the host also a thread
 * outside the scheduler (the hardware watchdog's). If two contexts call it at once, the first
 * one's reason is the one recorded and exited with; the other never returns either.
 */
_Noreturn void plat_reset(plat_reset_reason_t reason);

/** True once an orderly stop was requested from outside (SIGINT or SIGTERM on the host).
 *  A target has no such thing and returns false. */
bool plat_shutdown_requested(void);

/** Stop the scheduler so that the platform's start function returns. Call from a task. */
void plat_end_scheduler(void);

/**
 * Every task calls this first. It registers the task for stack measurement; on the host it
 * also paints the unused part of the task's real stack so the peak depth can be read later.
 */
void plat_task_enter(const char *name);

/** Peak stack depth of one registered task. */
typedef struct {
    char name[16];
    /** Deepest use seen since plat_task_enter(), counted from the top of the stack the task
     *  runs on, so the frame of the task function itself is included.
     *
     *  On the host that is the task's pthread stack, and the figure also holds start_bytes,
     *  what lies above the task function. The measurement has a floor: the depth of the
     *  stack at plat_task_enter() plus 512 bytes (4096 in a sanitizer build) left unpainted
     *  below it. Less use is reported as the floor itself, so a peak at the floor means "at
     *  most that much". The floor is a constant of the host and the build, a little different
     *  per task because the frames of the task functions differ: about 5 KiB in the release
     *  and debug builds and 9 KiB in the asan build of the development laptop (glibc 2.39). */
    uint32_t peak_bytes;
    /** Part of peak_bytes that was in use before the task's own code ran: the depth at which
     *  the function that called plat_task_enter() was entered. On the host it is the C
     *  library's thread descriptor and static TLS and the frames of the thread's and the
     *  port's start functions, the same for every task the port starts (4432 bytes in the
     *  release build of the development laptop); peak_bytes - start_bytes is what the task's
     *  code used. For a task that registers from a helper (the idle task, from its hook) it
     *  also holds the frames between the task function and that helper. 0 if it could not be
     *  found out. */
    uint32_t start_bytes;
    /** Size of the stack the task really runs on: on the host its pthread stack, which is
     *  not the stack it was created with. */
    uint32_t size_bytes;
    /** Size of the stack the task was created with (its uxStackDepth words), the size meant
     *  for the target. On the host the task's code never runs on it. */
    uint32_t configured_bytes;
} plat_stack_info_t;

/** Fill `out` with one entry per registered task; returns the number of entries written.
 *  After plat_end_scheduler() it reports the depths recorded at that moment. */
size_t plat_stack_report(plat_stack_info_t *out, size_t max_entries);

#endif /* FCN_PLATFORM_H */
