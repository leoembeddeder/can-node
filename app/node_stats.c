/* SPDX-License-Identifier: MIT */
#include "node_stats.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

/* Static: TaskStatus_t is large and this runs on a task stack. Only the health task calls the
 * functions of this file. */
static TaskStatus_t s_status[NODE_STATS_MAX_TASKS];

static void copy_task(node_task_stats_t *task, const TaskStatus_t *status)
{
    (void)memset(task, 0, sizeof(*task));
    (void)strncpy(task->name, status->pcTaskName, NODE_STATS_NAME_LEN - 1U);
    task->priority = (uint32_t)status->uxCurrentPriority;
    task->run_time_us = (uint64_t)status->ulRunTimeCounter;
}

void node_stats_capture_tasks(node_stats_t *out)
{
    configRUN_TIME_COUNTER_TYPE total = 0U;
    const UBaseType_t count = uxTaskGetSystemState(s_status, NODE_STATS_MAX_TASKS, &total);

    out->total_run_time_us = (uint64_t)total;
    out->tick_count = (uint64_t)xTaskGetTickCount();
    out->task_count = (uint32_t)count;

    for (UBaseType_t i = 0U; i < count; i++) {
        copy_task(&out->tasks[i], &s_status[i]);
    }

    /* uxTaskGetSystemState() returns 0 when the array is too small for all tasks. */
    out->valid = (count > 0U);
}

void node_stats_capture_can(node_stats_t *out)
{
    can_io_get_stats(&out->can_io, &out->can_queue);
    can_io_get_slot_stats(out->can_slots);
}

bool node_stats_task(const char *name, node_task_stats_t *out, uint64_t *total_us)
{
    configRUN_TIME_COUNTER_TYPE total = 0U;
    const UBaseType_t count = uxTaskGetSystemState(s_status, NODE_STATS_MAX_TASKS, &total);
    bool found = false;

    for (UBaseType_t i = 0U; (i < count) && !found; i++) {
        if (strncmp(s_status[i].pcTaskName, name, NODE_STATS_NAME_LEN) == 0) {
            copy_task(out, &s_status[i]);
            /* The priority the task was created with, not one it inherits for a moment. */
            out->priority = (uint32_t)s_status[i].uxBasePriority;
            *total_us = (uint64_t)total;
            found = true;
        }
    }

    return found;
}
