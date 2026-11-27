/* SPDX-License-Identifier: MIT */
/*
 * Real stack depth of each task on the host.
 *
 * uxTaskGetStackHighWaterMark() is of no use under the POSIX port. The kernel allocates the
 * stack buffer of uxStackDepth words as it would on a target, but the port only stores its
 * per-thread record (Thread_t) at the top of it and starts the task as a pthread created with
 * attributes from pthread_attr_init() alone: no stack is given, so the C library allocates one
 * of the process-wide default size (TASK_THREAD_STACK_BYTES, set by posix_stack_init()), and all
 * of the task's code runs there. The kernel's high-water mark scans the untouched buffer and
 * reports it as almost entirely free whatever the task does.
 *
 * So the measurement is done on the stack the task really uses: when a task starts it paints
 * the unused part of its pthread stack with a pattern, and the report scans from the low end
 * for the first byte that no longer holds the pattern. The depth is counted from the top of
 * the thread's stack, so every frame of the thread is in it: the task function's own frame
 * (its locals are allocated before it calls plat_task_enter()), and also what the C library
 * and the port put there before the task function ran (see plat_stack_info_t in platform.h).
 * The result is the depth on this host (x86-64 frames, the C library's own use, and the signal
 * frames that the port's "interrupts" push onto whichever task is running), not what the same
 * code needs on a Cortex-M.
 */
#include "posix_platform.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unwind.h>

#include "task.h"

/* The configured size of a task's stack is read from its TCB, which keeps the top of the
 * stack buffer only with this option (FreeRTOS.h defaults it to 0). It has to be set in
 * FreeRTOSConfig.h, because it changes the layout of the TCB for every file of the build. */
#if (configRECORD_STACK_HIGH_ADDRESS != 1)
#error "posix_stack.c needs configRECORD_STACK_HIGH_ADDRESS 1 in FreeRTOSConfig.h"
#endif

#define STACK_REGISTRY_MAX 16U
#define PAINT_BYTE         0xA5U

/* Stack given to every task thread. The port creates them without a stack attribute, so the
 * process-wide default is what sets their size. */
#define TASK_THREAD_STACK_BYTES ((size_t)512U * 1024U)

/* Bytes just below the frame of plat_task_enter() that are left unpainted: they hold the
 * frames of the painting code itself. Sanitizer builds have much larger frames. Together with
 * what lies above that frame, this sets the smallest depth the scan can report (see
 * plat_stack_info_t). */
#if defined(__SANITIZE_ADDRESS__)
#define PAINT_MARGIN_BYTES 4096U
#else
#define PAINT_MARGIN_BYTES 512U
#endif

/* The lowest pages of a pthread stack belong to the guard area on some C libraries. */
#define STACK_LOW_SKIP_BYTES 8192U

/*
 * One registered task. An entry is written by the task that registers, with interrupts
 * enabled, and read by whoever asks for a report, possibly a task that preempted the writer in
 * the middle. `ready` is therefore set last, when everything else is in place and the stack
 * is painted, and a reader looks at nothing else before it has seen `ready`.
 *
 * There is no way to unregister: the tasks of this node are never deleted. A task that could
 * be deleted would need one, because its stack must not be scanned after its thread is gone.
 */
typedef struct {
    char name[16];
    uint8_t *low;             /* lowest painted address */
    const uint8_t *paint_end; /* one past the highest painted address */
    const uint8_t *top;       /* one past the highest address of the thread's stack */
    size_t size;
    uint32_t start;       /* depth at which the caller of plat_task_enter() was entered */
    uint32_t configured;  /* bytes of the stack buffer the task was created with */
    uint32_t frozen_peak; /* peak depth recorded by posix_stack_freeze() */
    _Atomic bool ready;
} stack_entry_t;

static stack_entry_t s_entries[STACK_REGISTRY_MAX];
static _Atomic unsigned int s_count;
static _Atomic bool s_freeze_started; /* posix_stack_freeze() has been called */
static _Atomic bool s_frozen;         /* ... and every frozen_peak is in place */

void posix_stack_init(void)
{
    pthread_attr_t attr;

    if (pthread_attr_init(&attr) == 0) {
        (void)pthread_attr_setstacksize(&attr, TASK_THREAD_STACK_BYTES);
        (void)pthread_setattr_default_np(&attr);
        (void)pthread_attr_destroy(&attr);
    }
}

/* These two functions read and write below the current stack frame by design, so they are
 * kept out of line and are not instrumented by the sanitizers. */
#define STACK_SCAN_FN __attribute__((noinline, no_sanitize("address"), no_sanitize("undefined")))

/* A plain loop on purpose: a library call would put its own frames into the range being
 * painted. */
STACK_SCAN_FN static void paint(uint8_t *from, const uint8_t *to)
{
    for (volatile uint8_t *p = from; p < to; p++) {
        *p = (uint8_t)PAINT_BYTE;
    }
}

STACK_SCAN_FN static const uint8_t *first_used(const uint8_t *from, const uint8_t *to)
{
    const uint8_t *p = from;

    while ((p < to) && (*p == (uint8_t)PAINT_BYTE)) {
        p++;
    }

    return p;
}

/* Size of the calling task's stack buffer as the kernel recorded it: from its lowest word to
 * its highest one, which the kernel rounds down to portBYTE_ALIGNMENT when it creates the
 * task. So this is the uxStackDepth words the task was created with, less up to
 * portBYTE_ALIGNMENT - sizeof(StackType_t) bytes; on this host StackType_t is 8 bytes wide
 * and so is the alignment, and it is exactly uxStackDepth * 8. */
static uint32_t configured_stack_bytes(void)
{
    TaskStatus_t status;

    vTaskGetInfo(NULL, &status, pdFALSE, eRunning);

    return (uint32_t)((size_t)((status.pxEndOfStack + 1) - status.pxStackBase) *
                      sizeof(StackType_t));
}

/*
 * Walk state of caller_frame(). The unwinder hands over one context per function on the call
 * chain, from the innermost outwards. A context's region start is the first address of that
 * function, and its CFA is the stack pointer that function had when it made its call, one
 * level further in. So the context of plat_task_enter() is followed by the context of its
 * caller, and the context after that has the caller's CFA: the stack pointer just before the
 * caller was called.
 */
typedef struct {
    unsigned int step; /* 0 looking for plat_task_enter(), 1 at the caller, 2 one beyond */
    uintptr_t caller_cfa;
} caller_walk_t;

static _Unwind_Reason_Code caller_frame(struct _Unwind_Context *context, void *arg)
{
    caller_walk_t *walk = arg;
    _Unwind_Reason_Code result = _URC_NO_REASON;

    if (walk->step == 0U) {
        if ((uintptr_t)_Unwind_GetRegionStart(context) == (uintptr_t)&plat_task_enter) {
            walk->step = 1U;
        }
    } else if (walk->step == 1U) {
        walk->step = 2U;
    } else {
        walk->caller_cfa = (uintptr_t)_Unwind_GetCFA(context);
        result = _URC_END_OF_STACK; /* anything but _URC_NO_REASON ends the walk */
    }

    return result;
}

/*
 * Depth of the stack, counted from `top`, at which the function that called plat_task_enter()
 * was entered: everything above that function's frame. A frame's CFA is the stack pointer just
 * before the call that created the frame, and the unwind tables (.eh_frame, which the compiler
 * emits for every function on x86-64) give it without relying on frame pointers, which an
 * optimised build omits. 0 when the walk does not find the frame.
 *
 * What lies there on this host is the same for every task the port creates: the C library's
 * thread descriptor and static TLS, and the frames of its thread start function and of the
 * port's prvWaitForStart(). For a task that calls plat_task_enter() from a helper rather than
 * from its task function (the idle task, from its hook), the frames between the task function
 * and the helper are part of it too.
 */
static uint32_t start_depth(const uint8_t *top)
{
    caller_walk_t walk = {0U, 0U};
    uint32_t depth = 0U;

    (void)_Unwind_Backtrace(caller_frame, &walk);

    if ((walk.caller_cfa != 0U) && (walk.caller_cfa <= (uintptr_t)top)) {
        depth = (uint32_t)((uintptr_t)top - walk.caller_cfa);
    }

    return depth;
}

void plat_task_enter(const char *name)
{
    const unsigned int index = atomic_fetch_add(&s_count, 1U);

    if (index < STACK_REGISTRY_MAX) {
        stack_entry_t *entry = &s_entries[index];
        pthread_attr_t attr;
        void *stack_addr = NULL;
        size_t size = 0U;
        uint32_t start = 0U;
        const uint8_t *const here = __builtin_frame_address(0);

        /* pthread_getattr_np() allocates, and the unwinder may take the C library's lock on
         * the list of loaded objects. A task that is switched out while it holds such a lock
         * blocks the host thread of the next task that wants it, and the scheduler with it,
         * so the calls are made where no task switch can happen. */
        taskENTER_CRITICAL();

        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            (void)pthread_attr_getstack(&attr, &stack_addr, &size);
            (void)pthread_attr_destroy(&attr);
        }

        if (stack_addr != NULL) {
            start = start_depth((const uint8_t *)stack_addr + size);
        }

        taskEXIT_CRITICAL();

        uint8_t *const base = stack_addr; /* lowest address of the thread's stack */

        (void)strncpy(entry->name, name, sizeof(entry->name) - 1U);
        entry->name[sizeof(entry->name) - 1U] = '\0';
        entry->size = size;
        entry->start = start;
        entry->configured = configured_stack_bytes();

        if ((base != NULL) && (size > (STACK_LOW_SKIP_BYTES + PAINT_MARGIN_BYTES)) &&
            (here > (base + STACK_LOW_SKIP_BYTES + PAINT_MARGIN_BYTES))) {
            entry->low = base + STACK_LOW_SKIP_BYTES;
            entry->paint_end = here - PAINT_MARGIN_BYTES;
            /* The depth is counted from the top of the stack, not from `here`: the frame of
             * the task function, which called this one, lies above `here` and has to be in the
             * figure. */
            entry->top = base + size;
            paint(entry->low, entry->paint_end);

            /* Published only now: a report taken while the stack was half painted would
             * have read the unpainted part as used. */
            atomic_store(&entry->ready, true);
        }
    }
}

static unsigned int registered_count(void)
{
    const unsigned int count = atomic_load(&s_count);

    return (count > STACK_REGISTRY_MAX) ? STACK_REGISTRY_MAX : count;
}

static uint32_t scan_peak(const stack_entry_t *entry)
{
    return (uint32_t)(entry->top - first_used(entry->low, entry->paint_end));
}

/* Record every task's peak now and report those values from here on.
 *
 * A stack can only be scanned while its thread exists. Ending the scheduler makes the kernel
 * delete the idle task, the port then cancels and joins its thread, and the C library is free
 * to unmap that stack or hand its pages back to the system: a scan made afterwards would read
 * zeroes (reported as a completely used stack) or fault. So the scan is done here, by the
 * platform, just before the scheduler is told to end. */
void posix_stack_freeze(void)
{
    if (!atomic_exchange(&s_freeze_started, true)) {
        const unsigned int count = registered_count();

        for (unsigned int i = 0U; i < count; i++) {
            if (atomic_load(&s_entries[i].ready)) {
                s_entries[i].frozen_peak = scan_peak(&s_entries[i]);
            }
        }

        /* Only now do reports switch to the recorded values: a report taken while the loop
         * above was still scanning must not find zeroes where peaks are yet to be stored. */
        atomic_store(&s_frozen, true);
    }
}

size_t plat_stack_report(plat_stack_info_t *out, size_t max_entries)
{
    const unsigned int count = registered_count();
    const bool frozen = atomic_load(&s_frozen);
    size_t written = 0U;

    for (unsigned int i = 0U; (i < count) && (written < max_entries); i++) {
        const stack_entry_t *entry = &s_entries[i];

        if (atomic_load(&entry->ready)) {
            (void)memcpy(out[written].name, entry->name, sizeof(out[written].name));
            out[written].peak_bytes = frozen ? entry->frozen_peak : scan_peak(entry);
            out[written].size_bytes = (uint32_t)entry->size;
            out[written].start_bytes = entry->start;
            out[written].configured_bytes = entry->configured;
            written++;
        }
    }

    return written;
}
