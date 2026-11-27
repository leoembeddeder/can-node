/* SPDX-License-Identifier: MIT */
/**
 * @file
 * Snapshot of the node's statistics, taken by a task while the scheduler is still running:
 * the kernel's run-time statistics, the counters of the CAN tasks, and those of the
 * application (sensor, processing, telemetry, commands).
 */
#ifndef FCN_NODE_STATS_H
#define FCN_NODE_STATS_H

#include <stdbool.h>
#include <stdint.h>

#include "bms/bms.h"
#include "boot/boot.h"
#include "can_io.h"
#include "command.h"
#include "diag.h"
#include "diag_backend.h"
#include "fault.h"
#include "flash_job.h"
#include "isotp/isotp.h"
#include "plat_retained.h"
#include "uds/uds.h"
#include "update.h"
#include "wdgm/wdgm.h"

#define NODE_STATS_MAX_TASKS 12U
#define NODE_STATS_NAME_LEN  16U

/** The cyclic messages the node sends, one transmit-queue slot each. */
#define NODE_STATS_MESSAGES 9U

typedef struct {
    char name[NODE_STATS_NAME_LEN];
    uint32_t priority;
    uint64_t run_time_us; /**< time spent in the Running state */
    uint32_t overruns;    /**< periods in which the task missed its deadline; see app.c */
} node_task_stats_t;

/** What the sensor task did. */
typedef struct {
    uint32_t periods;         /**< periods run (passes), the heartbeat's period counter */
    uint32_t overruns;        /**< periods whose release had passed when the last ended */
    uint32_t skipped_periods; /**< periods skipped after an overrun, not run (app.c) */
    uint32_t samples_posted;  /**< samples the queue took */
    uint32_t queue_full;      /**< samples the queue refused because it was full */
    uint32_t current_reads;   /**< reads of each group, answered or not */
    uint32_t cell_reads;
    uint32_t temp_reads;
    uint32_t current_unanswered; /**< reads the front end did not answer */
    uint32_t cell_unanswered;
    uint32_t temp_unanswered;
} node_sensor_stats_t;

/** What the processing task did. */
typedef struct {
    uint32_t samples;         /**< samples taken from the queue and processed */
    uint32_t timeouts;        /**< waits for a sample that ran out (sensor supervision) */
    uint32_t recoveries;      /**< samples that came after a timeout: ends of a stall */
    uint32_t backlog;         /**< samples that found another one waiting behind them */
    uint32_t high_water;      /**< most samples the queue held at once, as the task found it */
    uint32_t verdicts_posted; /**< verdicts on faults the queue to the health task took */
    uint32_t verdicts_lost;   /**< verdicts it refused because it was full */
} node_process_stats_t;

/** One entity of the watchdog manager: its configuration and what the supervision saw. */
typedef struct {
    const char *name;
    wdgm_local_status_t status;
    uint32_t examinations;   /**< reference cycles examined */
    uint32_t incorrect;      /**< of those, with a count outside the bounds */
    uint32_t last_count;     /**< alive indications in the last examined reference cycle */
    uint32_t min_count;      /**< fewest in one examined reference cycle (0 before the first) */
    uint32_t max_count;      /**< most in one */
    uint16_t expected_alive; /**< the configuration, wdgm_entity_config_t */
    uint16_t min_margin;
    uint16_t max_margin;
    uint16_t reference_cycle;
    uint8_t failed_tolerance;
} node_supervised_stats_t;

/** The watchdog manager and the hardware watchdog, as the health task left them. */
typedef struct {
    wdgm_global_status_t status;
    uint32_t cycles;           /**< supervision cycles run */
    uint32_t kicks;            /**< plat_wdg_kick() calls after a supervision cycle */
    uint32_t kicks_withheld;   /**< supervision cycles after which lib/wdgm allowed no kick */
    uint32_t timeout_ms;       /**< the hardware watchdog's timeout */
    uint32_t entity_count;     /**< entries of `entities` used */
    uint8_t first_expired;     /**< wdgm_first_expired() */
    uint8_t expired_cycles;    /**< wdgm_state_t.expired_cycles */
    uint8_t expired_tolerance; /**< wdgm_config_t.expired_tolerance */
    bool watchdog_started;     /**< plat_wdg_start() succeeded */
    node_supervised_stats_t entities[WDGM_MAX_ENTITIES];
} node_supervision_stats_t;

/** One cyclic message: what the application did with it, and the transmit queue's counters
 *  of its slot. */
typedef struct {
    const char *name;   /**< the DBC's name */
    uint32_t id;        /**< identifier */
    uint32_t cycle_ms;  /**< cycle time of the DBC */
    uint8_t slot;       /**< transmit-queue slot */
    bool protected_e2e; /**< protected end to end; then data_id is its Data ID */
    uint16_t data_id;
    bool gated;          /**< switched off and on by CmdTelemetryOn */
    uint32_t published;  /**< frames handed to the transmit queue */
    uint32_t suppressed; /**< frames not built because telemetry was off */
    uint32_t failed;     /**< frames that could not be built or were refused (never) */
    can_txq_slot_stats_t queue;
} node_message_stats_t;

/** What the flash task did (app.c, THE UPDATE). */
typedef struct {
    bool flash;                     /**< the run has a flash (plat_flash_device() not NULL) */
    bool boot_ready;                /**< boot_init() accepted the platform's layout */
    boot_status_t boot_init_status; /**< what boot_init() returned (BOOT_E_ARG: no flash) */
    bool described;                 /**< the metadata were read at start */
    boot_status_t describe_status;  /**< what boot_describe() returned */
    bool have_record;               /**< of the description: a record was found */
    uint32_t seq;                   /**< the newest record's sequence number */
    uint8_t active_slot;            /**< the CONFIRMED slot, or BOOT_NO_SLOT */
    uint8_t trial_slot;             /**< the TRIAL slot, or BOOT_NO_SLOT */
    uint8_t attempts_used;          /**< of the trial */
    uint32_t records_valid;         /**< valid records in the log */
    uint32_t confirms;              /**< confirms made (boot_confirm() calls) */
    boot_status_t confirm_status;   /**< what the last one returned */
    uint64_t confirm_ns;            /**< how long it took, waits for the flash included */
    /* Phase 5: the jobs of the diagnostic task (flash_job.h), by kind (flash_job_kind_t). */
    uint32_t jobs[FLASH_JOB_KINDS];            /**< jobs run */
    boot_status_t job_status[FLASH_JOB_KINDS]; /**< the result of the last one of the kind */
    uint32_t job_steps[FLASH_JOB_KINDS];       /**< its lib/boot calls */
    uint64_t job_last_ns[FLASH_JOB_KINDS];  /**< how long it took, waits for the flash included */
    uint64_t job_max_ns[FLASH_JOB_KINDS];   /**< the longest of the kind */
    uint64_t job_total_ns[FLASH_JOB_KINDS]; /**< all of the kind together: a mean for phase 6 */
    bool confirmed_by_routine;              /**< the trial was confirmed by the routine */
    uint32_t self_skipped; /**< self-confirms that found the trial confirmed by the routine */
    uint32_t idle_passes;  /**< waits that ended without a job or a request */
} node_flash_task_stats_t;

/** The diagnostic connection (app.c, THE DIAGNOSTIC TASK): the core's, lib/isotp's, lib/uds's and
 *  the backend's counters, and those of the task and its queues. */
typedef struct {
    diag_stats_t core;
    isotp_stats_t isotp;
    uds_stats_t uds;
    diag_backend_stats_t backend;
    uint8_t session;        /**< the UDS session at the end */
    uint32_t rx_lost;       /**< UDS_Request frames the queue refused (full): lost in the node */
    uint32_t rx_high_water; /**< most frames the queue held, as the receive task found it */
    uint32_t tx_done_lost;  /**< reports of UDS_Response frames the queue refused */
    uint32_t events;        /**< frames and reports the task took */
    uint32_t passes;        /**< passes of its loop */
    uint32_t budget_waits;  /**< ticks it waited with events left (APP_DIAG_EVENTS_PER_TICK) */
} node_diag_stats_t;

/** The update: the hand-off, the confirm monitor of the health task, the flash task. */
typedef struct {
    bool have_handoff;
    plat_boot_handoff_t handoff;
    update_monitor_t monitor;
    node_flash_task_stats_t flash_task;
} node_update_stats_t;

typedef struct {
    bool valid;
    uint64_t total_run_time_us; /**< run-time counter when the snapshot was taken */
    uint64_t tick_count;
    uint32_t task_count;
    node_task_stats_t tasks[NODE_STATS_MAX_TASKS];
    can_io_stats_t can_io;
    can_txq_stats_t can_queue;
    can_txq_slot_stats_t can_slots[CAN_TXQ_CYCLIC_SLOTS];

    /* The application, filled by app.c. */
    node_sensor_stats_t sensor;
    node_process_stats_t process;
    bms_pack_t pack;   /**< the pack state as the processing task left it */
    bool telemetry_on; /**< the telemetry switch at the end */
    node_message_stats_t messages[NODE_STATS_MESSAGES];
    command_rx_t command;          /**< the receiver of TESTER_Command */
    fault_monitor_t faults;        /**< the DTCs, copied by their owner, the health task */
    fault_summary_t fault_summary; /**< what the last BMS_Fault reported */
    node_supervision_stats_t supervision;
    node_update_stats_t update; /**< the hand-off, the confirm, the flash task */
    node_diag_stats_t diag;     /**< the diagnostic connection (phase 5) */
} node_stats_t;

/** Fill the task table of `out` from uxTaskGetSystemState() (overruns 0). Call from a task;
 *  only one task may call the functions of this file. */
void node_stats_capture_tasks(node_stats_t *out);

/** Fill the counters of the CAN tasks and of the transmit queue, its slots included. Call
 *  from a task. */
void node_stats_capture_can(node_stats_t *out);

/** One task's entry as node_stats_capture_tasks() would give it, and the run-time counter in
 *  `*total_us`. False, with nothing written, if no task has this name. Call from a task; it
 *  reads the whole task list with the scheduler suspended. */
bool node_stats_task(const char *name, node_task_stats_t *out, uint64_t *total_us);

#endif /* FCN_NODE_STATS_H */
