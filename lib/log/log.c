/* SPDX-License-Identifier: MIT */
#include "log/log.h"

#include <stdarg.h>
#include <stdio.h>

static log_sink_fn s_sink = NULL;
static log_time_fn s_time_ns = NULL;
static log_level_t s_level = LOG_LEVEL_INFO;

void log_init(log_sink_fn sink, log_time_fn time_ns, log_level_t level)
{
    s_sink = sink;
    s_time_ns = time_ns;
    s_level = level;
}

void log_set_level(log_level_t level)
{
    s_level = level;
}

log_level_t log_get_level(void)
{
    return s_level;
}

static char level_letter(log_level_t level)
{
    char letter;

    switch (level) {
    case LOG_LEVEL_ERROR:
        letter = 'E';
        break;
    case LOG_LEVEL_WARN:
        letter = 'W';
        break;
    case LOG_LEVEL_INFO:
        letter = 'I';
        break;
    case LOG_LEVEL_DEBUG:
        letter = 'D';
        break;
    default:
        letter = '?';
        break;
    }

    return letter;
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...)
{
    if ((s_sink != NULL) && ((int)level <= (int)s_level)) {
        char line[LOG_LINE_MAX + 1U];
        const size_t limit = LOG_LINE_MAX - 1U; /* room for the newline */
        const uint64_t now_ns = (s_time_ns != NULL) ? s_time_ns() : 0U;
        const unsigned long seconds = (unsigned long)(now_ns / 1000000000ULL);
        const unsigned long micros = (unsigned long)((now_ns % 1000000000ULL) / 1000ULL);
        size_t len = 0U;

        int written = snprintf(line, limit + 1U, "[%6lu.%06lu] %c %s: ", seconds, micros,
                               level_letter(level), (tag != NULL) ? tag : "-");

        if (written > 0) {
            len = ((size_t)written < limit) ? (size_t)written : limit;
        }

        if (len < limit) {
            va_list args;

            va_start(args, fmt);
            written = vsnprintf(&line[len], (limit - len) + 1U, fmt, args);
            va_end(args);

            if (written > 0) {
                const size_t room = limit - len;

                len += ((size_t)written < room) ? (size_t)written : room;
            }
        }

        line[len] = '\n';
        len++;
        s_sink(line, len);
    }
}
