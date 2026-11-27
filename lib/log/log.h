/* SPDX-License-Identifier: MIT */
/**
 * @file
 * Minimal logging front end.
 *
 * A log call formats one line into a buffer on the caller's stack and hands it to a sink that
 * the platform installed. The front end never blocks and never allocates; what the sink does
 * with the line (a ring drained by another thread on the host, a UART on a target) is the
 * platform's business. Lines longer than LOG_LINE_MAX are truncated, never split.
 *
 * Line format:  "[ssssss.uuuuuu] L tag: message\n"   with L one of E, W, I, D.
 */
#ifndef FCN_LOG_H
#define FCN_LOG_H

#include <stddef.h>
#include <stdint.h>

/** Longest line handed to the sink, including the trailing newline, excluding any NUL. */
#define LOG_LINE_MAX 158U

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

/** Receives one complete line of `len` bytes; it is not NUL-terminated at `len`. */
typedef void (*log_sink_fn)(const char *line, size_t len);

/** Returns the time to stamp lines with, in nanoseconds. */
typedef uint64_t (*log_time_fn)(void);

#if defined(__GNUC__)
#define LOG_PRINTF_LIKE(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define LOG_PRINTF_LIKE(fmt_index, first_arg)
#endif

/**
 * Install the sink and the time source. Until this is called, and whenever `sink` is NULL,
 * log lines are discarded. `time_ns` may be NULL, in which case lines are stamped 0.
 */
void log_init(log_sink_fn sink, log_time_fn time_ns, log_level_t level);

/** Lines above this level are discarded before any formatting happens. */
void log_set_level(log_level_t level);

log_level_t log_get_level(void);

/** Format and emit one line. Use the LOG_* macros rather than calling this directly. */
void log_write(log_level_t level, const char *tag, const char *fmt, ...) LOG_PRINTF_LIKE(3, 4);

#define LOG_ERR(tag, ...) log_write(LOG_LEVEL_ERROR, (tag), __VA_ARGS__)
#define LOG_WRN(tag, ...) log_write(LOG_LEVEL_WARN, (tag), __VA_ARGS__)
#define LOG_INF(tag, ...) log_write(LOG_LEVEL_INFO, (tag), __VA_ARGS__)
#define LOG_DBG(tag, ...) log_write(LOG_LEVEL_DEBUG, (tag), __VA_ARGS__)

#endif /* FCN_LOG_H */
