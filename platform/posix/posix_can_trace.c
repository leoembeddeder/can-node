/* SPDX-License-Identifier: MIT */
#include "posix_can_trace.h"

#include <stdio.h>
#include <string.h>

_Static_assert((POSIX_CAN_TRACE_RECORDS & (POSIX_CAN_TRACE_RECORDS - 1U)) == 0U,
               "the ring's size is a power of two");

static posix_can_trace_record_t s_ring[POSIX_CAN_TRACE_RECORDS];
static uint32_t s_newest; /* the number the next record gets */

void posix_can_trace_reset(void)
{
    (void)memset(s_ring, 0, sizeof(s_ring));
    s_newest = 0U;
}

void posix_can_trace_add(const posix_can_trace_record_t *record)
{
    s_ring[s_newest % POSIX_CAN_TRACE_RECORDS] = *record;
    s_newest++;
}

uint32_t posix_can_trace_newest(void)
{
    return s_newest;
}

uint32_t posix_can_trace_oldest(void)
{
    return (s_newest > POSIX_CAN_TRACE_RECORDS) ? (s_newest - POSIX_CAN_TRACE_RECORDS) : 0U;
}

bool posix_can_trace_get(uint32_t seq, posix_can_trace_record_t *out)
{
    const bool kept = (seq >= posix_can_trace_oldest()) && (seq < s_newest);

    if (kept) {
        *out = s_ring[seq % POSIX_CAN_TRACE_RECORDS];
    }

    return kept;
}

const char *posix_can_trace_kind_name(uint8_t kind)
{
    static const char *const names[POSIX_CAN_TRACE_KINDS] = {
        "state", "counters", "restart", "first_tx", "fault", "inject_tx", "inject_rx"};

    return (kind < (uint8_t)POSIX_CAN_TRACE_KINDS) ? names[kind] : "?";
}

bool posix_can_trace_format(uint32_t from, char *reply, size_t reply_len)
{
    const uint32_t oldest = posix_can_trace_oldest();
    size_t used;
    int length;

    if (reply_len < POSIX_CAN_TRACE_REPLY_MIN) {
        (void)snprintf(reply, reply_len, "reply buffer too small");
        return false;
    }

    length = snprintf(reply, reply_len, "oldest=%u newest=%u", (unsigned int)oldest,
                      (unsigned int)s_newest);
    used = (length > 0) ? (size_t)length : 0U;

    for (uint32_t seq = (from > oldest) ? from : oldest; seq < s_newest; seq++) {
        const posix_can_trace_record_t *record = &s_ring[seq % POSIX_CAN_TRACE_RECORDS];
        char word[POSIX_CAN_TRACE_REPLY_MIN];

        length = snprintf(
            word, sizeof(word), " r%u=%s,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%llu",
            (unsigned int)seq, posix_can_trace_kind_name(record->kind),
            (unsigned long long)record->model_ns, (unsigned long long)record->pass_ns,
            (unsigned long long)record->start_ns, (unsigned long long)record->submit_ns,
            (unsigned long long)record->attempts_failed, (unsigned int)record->value,
            (unsigned int)record->state, (unsigned int)record->old_state, (unsigned int)record->tec,
            (unsigned int)record->rec, (unsigned long long)record->frames);

        /* Whole records only: one that does not fit is the first of the next request. */
        if ((length <= 0) || (((size_t)length + used) >= reply_len)) {
            break;
        }

        (void)memcpy(&reply[used], word, (size_t)length + 1U);
        used += (size_t)length;
    }

    return true;
}
