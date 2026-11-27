/* SPDX-License-Identifier: MIT */
/*
 * Log sink for the host: lines go into a ring and a thread outside the scheduler writes them
 * out. A task therefore never writes to a stream or a file descriptor and never takes a
 * stream lock, which on the POSIX port could deadlock the scheduler (see posix_platform.h).
 * Formatting a line with vsnprintf() into a buffer on the task's stack is all a task does.
 *
 * The ring is single-producer, so producers are serialised with a spin flag. A context that
 * the scheduler can switch out (a task) takes the flag only inside a critical section, so it
 * can never be descheduled by FreeRTOS while holding it. External threads take it directly;
 * they are not subject to the FreeRTOS scheduler, only to the host's.
 */
#include "posix_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ring/spsc_ring.h"
#include "task.h"

#define LOG_RING_RECORDS    1024U
#define LOG_DRAIN_PERIOD_NS 5000000L

typedef struct {
    uint16_t len;
    char text[LOG_LINE_MAX];
} log_record_t;

static uint8_t s_storage[LOG_RING_RECORDS * sizeof(log_record_t)];
static spsc_ring_t s_ring;
static atomic_flag s_producer_busy = ATOMIC_FLAG_INIT;
static _Atomic uint32_t s_dropped;
static pthread_mutex_t s_consumer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_thread;
static _Atomic uint64_t s_shutdown_deadline_ns; /* 0: the shutdown guard is not armed */
static _Atomic bool s_thread_run;
static bool s_thread_started;
static int s_fd = STDERR_FILENO;

/* Set by posix_external_thread_setup(): this thread is not known to the scheduler. */
static _Thread_local bool t_external_thread;

void posix_external_thread_setup(const char *name)
{
    sigset_t all;

    (void)sigfillset(&all);
    (void)pthread_sigmask(SIG_SETMASK, &all, NULL);
    (void)pthread_setname_np(pthread_self(), name);
    t_external_thread = true;
}

bool posix_on_external_thread(void)
{
    return t_external_thread;
}

void posix_log_sink(const char *line, size_t len)
{
    log_record_t record;
    bool pushed;

    record.len = (uint16_t)((len < sizeof(record.text)) ? len : sizeof(record.text));
    (void)memcpy(record.text, line, record.len);

    if (!t_external_thread) {
        taskENTER_CRITICAL();
    }

    while (atomic_flag_test_and_set_explicit(&s_producer_busy, memory_order_acquire)) {
        /* Spin. The holder needs the flag for one ring push and is normally running on
         * another core. If the host has descheduled it, this wait lasts until it runs again;
         * a task spins here inside a critical section, so ticks are late by that much. */
    }

    pushed = spsc_ring_push(&s_ring, &record);
    atomic_flag_clear_explicit(&s_producer_busy, memory_order_release);

    if (!t_external_thread) {
        taskEXIT_CRITICAL();
    }

    if (!pushed) {
        (void)atomic_fetch_add(&s_dropped, 1U);
    }
}

static void write_all(const char *data, size_t len)
{
    size_t done = 0U;

    while (done < len) {
        const ssize_t n = write(s_fd, &data[done], len - done);

        if (n > 0) {
            done += (size_t)n;
        } else if ((n < 0) && (errno == EINTR)) {
            /* try again */
        } else {
            break; /* the log must never take the node down */
        }
    }
}

void posix_log_drain(void)
{
    log_record_t record;

    (void)pthread_mutex_lock(&s_consumer_mutex);

    while (spsc_ring_pop(&s_ring, &record)) {
        write_all(record.text, record.len);
    }

    (void)pthread_mutex_unlock(&s_consumer_mutex);
}

void posix_shutdown_guard_arm(uint32_t timeout_ms)
{
    atomic_store(&s_shutdown_deadline_ns,
                 posix_monotonic_ns() + ((uint64_t)timeout_ms * 1000000ULL));
}

/* The logger thread doubles as the guard of an orderly stop: it is outside the scheduler, so
 * it still runs if ending the scheduler hangs, and then ends the process with a status that
 * says so instead of leaving a stuck node behind (posix_end_process(): the locks of the
 * retained cell and the flash file are released first, as at a reset). */
static void check_shutdown_guard(void)
{
    const uint64_t deadline_ns = atomic_load(&s_shutdown_deadline_ns);

    if ((deadline_ns != 0U) && (posix_monotonic_ns() > deadline_ns)) {
        static const char message[] = "FATAL the scheduler did not stop in time\n";

        posix_log_drain();
        (void)!write(STDERR_FILENO, message, sizeof(message) - 1U);
        posix_end_process(POSIX_EXIT_SHUTDOWN_TIMEOUT);
    }
}

static void *logger_main(void *arg)
{
    const struct timespec period = {0, LOG_DRAIN_PERIOD_NS};

    (void)arg;
    posix_external_thread_setup("fcn-log");

    while (atomic_load(&s_thread_run)) {
        posix_log_drain();
        check_shutdown_guard();
        (void)nanosleep(&period, NULL);
    }

    return NULL;
}

void posix_log_start(const char *path)
{
    (void)spsc_ring_init(&s_ring, s_storage, (uint32_t)sizeof(log_record_t), LOG_RING_RECORDS);

    if (path != NULL) {
        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

        if (fd >= 0) {
            s_fd = fd;
        }
    }

    atomic_store(&s_thread_run, true);
    s_thread_started = (pthread_create(&s_thread, NULL, logger_main, NULL) == 0);
}

void posix_log_stop(void)
{
    atomic_store(&s_shutdown_deadline_ns, 0U); /* the scheduler has stopped: disarm the guard */

    if (s_thread_started) {
        atomic_store(&s_thread_run, false);
        (void)pthread_join(s_thread, NULL);
        s_thread_started = false;
    }

    posix_log_drain();
}

uint32_t posix_log_dropped(void)
{
    return atomic_load(&s_dropped);
}
