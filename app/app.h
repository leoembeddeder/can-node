/* SPDX-License-Identifier: MIT */
/**
 * @file
 * Entry point of the application: creates the tasks and their queues.
 */
#ifndef FCN_APP_H
#define FCN_APP_H

#include <stdint.h>

#include "node_stats.h"

typedef struct {
    /** Stop by itself after this many seconds of tick time; 0 runs until shutdown is
     *  requested. Used by tests and measurement scripts. */
    uint32_t run_seconds;
    /** The healthy health periods in a row after which a trial confirms itself
     *  (update_init()); 0 for the product's window of UPDATE_CONFIRM_AFTER_S seconds. A test
     *  hook the product does not have (the host's --confirm-periods). */
    uint32_t confirm_periods;
} app_config_t;

/** Create all tasks and queues. Call once, before the scheduler starts. */
void app_start(const app_config_t *config);

/** Statistics captured by the health task just before it ended the scheduler. */
const node_stats_t *app_final_stats(void);

#endif /* FCN_APP_H */
