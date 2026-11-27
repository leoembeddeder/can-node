/* SPDX-License-Identifier: MIT */
/*
 * The application: one battery module measured, judged and published on CAN, commanded from the
 * bus, its faults kept as diagnostic trouble codes, its tasks supervised. The tasks, their
 * priorities and their periods are listed in app_tasks.h; this file is what they do and how
 * they talk to each other.
 *
 *   front end --> sensor --(sample queue)--> process --> protected telemetry --> can_io --> bus
 *                   |                           |   \                          ^
 *                   +--> heartbeat ------------ | --(verdict queue)--> health -+--> BMS_Fault,
 *                                               v                      |  ^       task stats,
 *                                    pack state (mutex) <--------------+  |       node info
 *                                                                         |
 *   bus --> can_rx --> TESTER_Command check (command.c) --> telemetry switch --> process
 *                                                      \--> clear requests --> health
 *
 *   sensor, process, can_rx, can_tx --(alive indications)--> watchdog manager (health)
 *                                                       health --(kick)--> hardware watchdog
 *
 * IPC
 *   - sensor -> process: a queue of APP_SAMPLE_QUEUE_LENGTH samples (app_sample_t, copied).
 *     The sensor task never waits for room: a sample the queue cannot take is counted and
 *     dropped, and the processing task sees the gap in the sequence numbers (lib/bms counts it
 *     as lost). The processing task waits for a sample with a timeout of
 *     APP_SAMPLE_TIMEOUT_MS: a timeout means the sensor task has stopped delivering
 *     (supervision), and is counted and judged as a fault. The processing task writes no log
 *     line (task_table.c): the health task logs the start and the end of such a stall from its
 *     counts (log_sample_wait()).
 *   - the pack state (lib/bms) is behind a mutex with priority inheritance: written by the
 *     processing task, read by the health task for its log line and the final statistics.
 *     Both hold it only to run lib/bms on one sample or to copy the state; frames are built
 *     from the copy, outside the lock.
 *   - process -> health: a queue of APP_FAULT_VERDICT_QUEUE_LENGTH verdicts on the samples
 *     (fault_verdict_t, fault.h), each tagged with the count of clear requests the processing
 *     task read when it handed the verdict over. The processing task never waits for room; a
 *     lost verdict is counted. The health task works them into the DTCs in their order once per
 *     period, and carries out a clear request before the first verdict that carries it
 *     (run_faults()).
 *   - the DTCs (fault_monitor_t) have one owner, the health task: it alone works the verdicts
 *     in, judges the CAN controller and the watchdog manager, carries out clears and sends
 *     BMS_Fault. After every period it publishes the summary (fault_summary_t) in a critical
 *     section; the processing task reads from it whether a fault is active, for BMS_PackStatus.
 *   - the telemetry switch and the count of clear requests are written by the receive task
 *     (TESTER_Command) and read by the processing and the health task, in short critical
 *     sections.
 *   - the alive indications of the supervised tasks go to the watchdog manager (lib/wdgm)
 *     lock-free: wdgm_checkpoint() is one atomic addition. Everything else of the manager
 *     belongs to the health task.
 *   - every frame goes out through can_io (a slot per cyclic message, s_messages).
 *
 * WHO SENDS WHAT
 *   - BMS_Heartbeat: the sensor task, first thing in each of its periods. It is the status
 *     frame of phase 1, and its two numbers describe exactly that task: the number of the
 *     period it runs (a count of its passes, so that the periods it skipped after an overrun,
 *     sensor_task(), leave no gap in it) and the tick it was released in. The sensor task is
 *     the highest-priority periodic task and is released by the tick alone, so the numbers keep
 *     the timing of phase 1's ticker. The frame reaches the bus only when can_tx loads it, and
 *     can_tx runs below the processing task: the frame goes out after the processing job of
 *     its period, and a heartbeat on the bus says that the sensor task and can_tx both run.
 *     A processing task that blocks shows on the bus by itself: its protected messages stop
 *     while the heartbeat goes on, and their receivers' E2E checks time out. A task that hangs
 *     or keeps the CPU is found by the watchdogs (SUPERVISION below).
 *   - BMS_PackStatus, BMS_CellVoltages1/2, BMS_Temperatures: the processing task, for every
 *     sample that carries the measurements of the message (10, 20, 100 ms), protected end to
 *     end (telemetry.c), and only while the telemetry switch is on.
 *   - BMS_Fault: the health task, in every period, after the period's last change of the DTCs,
 *     protected end to end, whatever the telemetry switch says. Nothing else changes the DTCs,
 *     so every change goes out in the frame of the period that made it: a fault whose
 *     testFailed rose in the period is active in its frame, even if it passed again before the
 *     frame (fault.h, THE SUMMARY OF A PERIOD).
 *   - BMS_TaskStats, BMS_NodeInfo, BMS_BootInfo: the health task.
 *   - BMS_Pong: the receive task, as in phase 1.
 *
 * THE UPDATE (update.h). The bootloader's hand-off (plat_retained.h) is read once, before the
 * scheduler starts, and is constant after: which slot runs, as a trial or as the confirmed
 * image, the image's version, and a rollback the bootloader made (the DTC UPDATE_ROLLBACK,
 * judged in fault_init() as the reset faults are). The health task judges every period
 * whether a trial runs in good health (update_health_period(): the watchdog manager OK, the CAN
 * controller error active, no bus-off, and a frame acknowledged since the period before); once
 * it has done so for UPDATE_CONFIRM_AFTER_S seconds without a break (a host test can lengthen
 * the window: app_config_t) it asks the flash task to confirm it, once. The tester's confirm
 * routine (UDS RoutineControl 0xF000) is the other way: whichever comes first writes the
 * CONFIRM record, in the flash task, and the other one then writes nothing (flash_job.h, THE
 * CONFIRM); the health task learns of a confirm by the routine from the flash task
 * (update_routine_confirmed()).
 *
 * THE FLASH TASK, the lowest-priority task, makes every write of the node to the flash: a flash
 * operation blocks its caller for the time the part takes (a page program 0.4 ms, a sector
 * erase 45 ms; 3 ms and 400 ms at most), which no task with a deadline or an alive indication
 * to give may spend waiting. It owns lib/boot's state (boot_t) and the slot writer: at start it
 * reads the metadata once (boot_describe(), for the log and the statistics); then it runs the
 * jobs of the diagnostic task, one at a time (flash_job.h: an erase, a block written, the exit,
 * the check, the confirm routine), and the confirm the health task asks for, and hands each
 * result back. It waits for them at most APP_FLASH_IDLE_WAIT_MS, so that it gives an alive
 * indication at least that often, and gives one after every call of lib/boot in a job (see
 * SUPERVISION). Without a flash (plat_flash_device() NULL) every job ends with BOOT_E_STATE.
 *
 * THE DIAGNOSTIC TASK owns the diagnostic connection (diag.h: lib/isotp in front of lib/uds) and
 * the UDS backend (diag_backend.h):
 *   - the receive task puts every UDS_Request frame (0x7E0) into a queue of
 *     APP_DIAG_RX_QUEUE_LENGTH frames (never waiting: a frame the queue refuses is lost and
 *     counted), and the transmit task every report of a UDS_Response frame (0x7E8) sent into a
 *     queue of APP_DIAG_TX_DONE_QUEUE_LENGTH (can_io_on_tx_done()); each notifies the task;
 *   - the task takes the reports first, then the frames, at most APP_DIAG_EVENTS_PER_TICK in a
 *     tick, and polls the connection after them and at its next deadline (the timers of lib/isotp
 *     and of lib/uds, and an accepted ECUReset's deadline), waiting at most
 *     APP_DIAG_IDLE_WAIT_MS;
 *   - its frames go out as event frames of the transmit queue (can_io_send_event_stamped());
 *   - the flash work of a service is a job for the flash task, posted into a mailbox of one job
 *     (lib/uds runs one operation at a time) and polled while lib/uds answers responsePending;
 *     the flash task notifies the task when the job is done;
 *   - the DTC services read the snapshot of the DTCs the health task publishes after every period
 *     (a copy of lib/dtc's state in a critical section, at most one health period and the health
 *     task's response time old), and ClearDiagnosticInformation adds one to a count of clear
 *     requests that the health task carries out as it carries out those of TESTER_Command
 *     (run_faults()), after which it notifies the task: the positive response follows the clear;
 *   - an accepted ECUReset is made by the task once the transmission of its response has ended,
 *     or 2 s after the task took the request if that comes first, a deadline of the connection
 *     that cuts an earlier response the tester's flow control still holds (diag.h, ECURESET):
 *     plat_reset(PLAT_RESET_REQUESTED).
 *
 * SUPERVISION. The sensor, processing, receive, transmit and diagnostic tasks each report an
 * alive indication once per pass of their loop, and the flash task once per pass and after every
 * call of lib/boot in a job (s_supervised). The health task runs one supervision
 * cycle of the watchdog manager per period and kicks the hardware watchdog after it while the
 * manager allows a kick (global status OK, FAILED or EXPIRED). An entity beyond its tolerance
 * makes the global status STOPPED at once (expired_tolerance 0): the health task records it (a
 * log line, the DTC SUPERVISION_EXPIRED, which goes out in BMS_Fault in the same period) and
 * kicks no more, and the hardware watchdog resets the node at most APP_WDG_TIMEOUT_MS after the
 * last kick. The health task itself is not an entity: it is the one that kicks, so the
 * hardware watchdog covers it. Neither is the idle task: it does nothing that could hang, and
 * a task above it that keeps the CPU starves it as it starves the health task, whose kicks
 * stop then. The threads of the host platform that stand for hardware (the CAN controller,
 * the tick, the watchdog) are not tasks.
 */
#include "app.h"

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "app_tasks.h"
#include "boot/boot.h"
#include "bootimg/bootimg.h"
#include "can_db.h"
#include "can_io.h"
#include "command.h"
#include "diag.h"
#include "diag_backend.h"
#include "fault.h"
#include "flash_job.h"
#include "log/log.h"
#include "plat_can.h"
#include "plat_flash.h"
#include "plat_retained.h"
#include "plat_sensor.h"
#include "plat_wdg.h"
#include "platform.h"
#include "telemetry.h"
#include "update.h"
#include "wdgm/wdgm.h"

#ifndef FCN_VERSION_MAJOR
#define FCN_VERSION_MAJOR 0
#endif
#ifndef FCN_VERSION_MINOR
#define FCN_VERSION_MINOR 0
#endif
#ifndef FCN_VERSION_PATCH
#define FCN_VERSION_PATCH 0
#endif

_Static_assert((FCN_VERSION_MAJOR >= 0) && (FCN_VERSION_MAJOR <= 255), "SwMajor: 8 bits");
_Static_assert((FCN_VERSION_MINOR >= 0) && (FCN_VERSION_MINOR <= 255), "SwMinor: 8 bits");
_Static_assert((FCN_VERSION_PATCH >= 0) && (FCN_VERSION_PATCH <= 255), "SwPatch: 8 bits");

/* Transmit-queue slots, one per cyclic message. The heartbeat keeps slot 0, the slot of the
 * status frame of phase 1. */
#define SLOT_HEARTBEAT      0U
#define SLOT_PACK_STATUS    1U
#define SLOT_CELL_VOLTAGES1 2U
#define SLOT_CELL_VOLTAGES2 3U
#define SLOT_TEMPERATURES   4U
#define SLOT_TASK_STATS     5U
#define SLOT_NODE_INFO      6U
#define SLOT_FAULT          7U
#define SLOT_BOOT_INFO      8U
#define SLOTS               9U

_Static_assert(SLOTS == NODE_STATS_MESSAGES, "one statistics entry per cyclic message");
_Static_assert(SLOTS <= CAN_TXQ_CYCLIC_SLOTS, "the transmit queue has a slot for each");

/* The periods of the tasks are the cycle times of the messages they send. */
_Static_assert(APP_SENSOR_PERIOD_MS == CAN_DB_BMS_HEARTBEAT_CYCLE_MS, "heartbeat: every period");
_Static_assert(APP_SENSOR_PERIOD_MS == CAN_DB_BMS_PACK_STATUS_CYCLE_MS, "PackStatus: every sample");
_Static_assert((APP_SENSOR_PERIOD_MS * APP_CELLS_EVERY) == CAN_DB_BMS_CELL_VOLTAGES1_CYCLE_MS,
               "CellVoltages1: every sample with cells");
_Static_assert((APP_SENSOR_PERIOD_MS * APP_CELLS_EVERY) == CAN_DB_BMS_CELL_VOLTAGES2_CYCLE_MS,
               "CellVoltages2: every sample with cells");
_Static_assert((APP_SENSOR_PERIOD_MS * APP_TEMPS_EVERY) == CAN_DB_BMS_TEMPERATURES_CYCLE_MS,
               "Temperatures: every sample with temperatures");
_Static_assert(APP_HEALTH_PERIOD_MS == CAN_DB_BMS_FAULT_CYCLE_MS, "BMS_Fault: every health period");
_Static_assert((APP_HEALTH_PERIOD_MS * APP_TASK_STATS_EVERY) == CAN_DB_BMS_TASK_STATS_CYCLE_MS,
               "TaskStats: every second health period");
_Static_assert((APP_HEALTH_PERIOD_MS * APP_NODE_INFO_EVERY) == CAN_DB_BMS_NODE_INFO_CYCLE_MS,
               "NodeInfo: every tenth health period");
_Static_assert((1000U % APP_HEALTH_PERIOD_MS) == 0U, "whole health periods per second");
/* The health task counts the uptime in whole seconds of its own periods (telemetry_uptime_t),
 * not from the tick count, and sends it once a second: BMS_NodeInfo's period has to be one. */
_Static_assert((APP_HEALTH_PERIOD_MS * APP_NODE_INFO_EVERY) == 1000U,
               "NodeInfo: once a second, with the seconds the health task counts");
/* BMS_TaskStats goes in the odd periods, BMS_NodeInfo in periods divisible by
 * APP_NODE_INFO_EVERY, which are even, and BMS_BootInfo in the periods APP_BOOT_INFO_PHASE past
 * those, even and not divisible: a period sends BMS_Fault and at most one of the three. */
_Static_assert(APP_TASK_STATS_EVERY == 2U, "TaskStats: the odd periods");
_Static_assert((APP_NODE_INFO_EVERY % APP_TASK_STATS_EVERY) == 0U, "NodeInfo: even periods only");
_Static_assert(((APP_BOOT_INFO_PHASE % APP_TASK_STATS_EVERY) == 0U) &&
                   (APP_BOOT_INFO_PHASE != 0U) && (APP_BOOT_INFO_PHASE < APP_NODE_INFO_EVERY),
               "BootInfo: even periods that are not NodeInfo's");
_Static_assert((APP_HEALTH_PERIOD_MS * APP_NODE_INFO_EVERY) == CAN_DB_BMS_BOOT_INFO_CYCLE_MS,
               "BootInfo: once a second");

/* The hardware watchdog: a timeout the IWDG can have, several health periods long. */
_Static_assert((APP_WDG_TIMEOUT_MS >= PLAT_WDG_TIMEOUT_MIN_MS) &&
                   (APP_WDG_TIMEOUT_MS <= PLAT_WDG_TIMEOUT_MAX_MS),
               "the hardware watchdog accepts the timeout");
_Static_assert(APP_WDG_TIMEOUT_MS >= (3U * APP_HEALTH_PERIOD_MS),
               "the kicks of a late health task do not reset the node");

/* The verdict queue holds more than a health period of verdicts: one per sample, or one per
 * timeout of the processing task's wait, which is longer than a sample period. */
_Static_assert(APP_FAULT_VERDICT_QUEUE_LENGTH >=
                   (2U * (APP_HEALTH_PERIOD_MS / APP_SENSOR_PERIOD_MS)),
               "the verdicts of two health periods fit the queue");
_Static_assert(APP_SAMPLE_TIMEOUT_MS > APP_SENSOR_PERIOD_MS, "a timeout is longer than a period");

/* The tasks BMS_TaskStats goes round, in this order (TaskIndex). */
#define TASK_STATS_TASKS 8U
static const char *const s_stats_tasks[TASK_STATS_TASKS] = {"can_rx", "sensor", "process", "can_tx",
                                                            "health", "flash",  "IDLE",    "diag"};
_Static_assert(TASK_STATS_TASKS <= (CAN_DB_BMS_TASK_STATS_TASK_INDEX_RAW_MAX + 1U),
               "TaskIndex holds every index");
_Static_assert((uint32_t)configMAX_PRIORITIES <= (CAN_DB_BMS_TASK_STATS_TASK_PRIORITY_RAW_MAX + 1U),
               "TaskPriority holds every priority");

/* The filters of lib/bms, counted in samples of each group: the current is sampled every
 * 10 ms (2^2 samples: about 40 ms), the cells every 20 ms (about 80 ms), the temperatures,
 * which change slowly anyway, every 100 ms (2^1 samples: about 200 ms). */
static const bms_config_t s_bms_config = {.cell_shift = 2U, .temp_shift = 1U, .current_shift = 2U};

/* The cyclic messages: what the statistics say about each. Indexed by slot. */
typedef struct {
    const char *name;
    uint32_t id;
    uint32_t cycle_ms;
    uint16_t data_id; /* meaningful if protected_e2e */
    bool protected_e2e;
    bool gated;
} message_info_t;

static const message_info_t s_messages[SLOTS] = {
    [SLOT_HEARTBEAT] = {"BMS_Heartbeat", CAN_DB_BMS_HEARTBEAT_ID, CAN_DB_BMS_HEARTBEAT_CYCLE_MS, 0U,
                        false, false},
    [SLOT_PACK_STATUS] = {"BMS_PackStatus", CAN_DB_BMS_PACK_STATUS_ID,
                          CAN_DB_BMS_PACK_STATUS_CYCLE_MS, CAN_DB_BMS_PACK_STATUS_E2E_DATA_ID, true,
                          true},
    [SLOT_CELL_VOLTAGES1] = {"BMS_CellVoltages1", CAN_DB_BMS_CELL_VOLTAGES1_ID,
                             CAN_DB_BMS_CELL_VOLTAGES1_CYCLE_MS,
                             CAN_DB_BMS_CELL_VOLTAGES1_E2E_DATA_ID, true, true},
    [SLOT_CELL_VOLTAGES2] = {"BMS_CellVoltages2", CAN_DB_BMS_CELL_VOLTAGES2_ID,
                             CAN_DB_BMS_CELL_VOLTAGES2_CYCLE_MS,
                             CAN_DB_BMS_CELL_VOLTAGES2_E2E_DATA_ID, true, true},
    [SLOT_TEMPERATURES] = {"BMS_Temperatures", CAN_DB_BMS_TEMPERATURES_ID,
                           CAN_DB_BMS_TEMPERATURES_CYCLE_MS, CAN_DB_BMS_TEMPERATURES_E2E_DATA_ID,
                           true, true},
    [SLOT_TASK_STATS] = {"BMS_TaskStats", CAN_DB_BMS_TASK_STATS_ID, CAN_DB_BMS_TASK_STATS_CYCLE_MS,
                         0U, false, false},
    [SLOT_NODE_INFO] = {"BMS_NodeInfo", CAN_DB_BMS_NODE_INFO_ID, CAN_DB_BMS_NODE_INFO_CYCLE_MS, 0U,
                        false, false},
    [SLOT_FAULT] = {"BMS_Fault", CAN_DB_BMS_FAULT_ID, CAN_DB_BMS_FAULT_CYCLE_MS,
                    CAN_DB_BMS_FAULT_E2E_DATA_ID, true, false},
    [SLOT_BOOT_INFO] = {"BMS_BootInfo", CAN_DB_BMS_BOOT_INFO_ID, CAN_DB_BMS_BOOT_INFO_CYCLE_MS, 0U,
                        false, false}};

/*
 * The supervised entities of the watchdog manager, one per task that must not stop, numbered as
 * their entries below. Every entity is examined once per supervision cycle (reference_cycle 1,
 * one health period, 100 ms) and its count is correct within the bounds
 * expected_alive - min_margin .. expected_alive + max_margin.
 *
 * The lower bound of every entity is 1: the supervision is there to find a task that has
 * stopped (it hangs, deadlocks, or waits for something that never comes), and one alive
 * indication per 100 ms is what a running task of this set always gives. Timing within a
 * period is the business of the overrun counters (BMS_TaskStats). The upper bounds:
 *   - sensor, process: one indication per pass, a pass per 10 ms (the process task's also on
 *     each timeout of its wait, every 30 ms, while the samples do not come): 10 expected, at
 *     most about twice the rate of its period, so that a task whose wait no longer waits (a
 *     loop that runs through xTaskDelayUntil or the queue at once) is found as well. A task that
 *     goes on after a stall stays below it. The sensor task does not make up the periods it
 *     missed: it skips them and keeps the grid of its period (sensor_task()). Between two
 *     examinations it makes a pass for each period of the grid that begins in between, and
 *     after a stall at most two more (the end of the pass that stalled, and at once the pass
 *     of the period the tick is in): 12 when the examinations are 100 ms apart, 17 when the
 *     health task's response time (under 45 ms since phase 5's diagnostic task, task_table.c)
 *     puts them 145 ms apart: at most 20. The processing task, after a stall of its own, finds
 *     at most the APP_SAMPLE_QUEUE_LENGTH (4) samples the queue held and then one per pass of
 *     the sensor task: at most 4 + 17 = 21 (20 before phase 5, when the examinations were at
 *     most 120 ms apart); its timeouts come only while no sample does;
 *   - can_rx, can_tx: a pass per wake-up, at least every 50 ms (can_io.h), at most one per
 *     frame on the bus (2200 per 100 ms at 1 Mbit/s, task_table.c): 2 expected and the largest
 *     upper bound a uint16_t margin gives, 65535; a lower bound only, in effect;
 *   - diag: the same, for a pass per event or deadline, at least every APP_DIAG_IDLE_WAIT_MS
 *     (50 ms);
 *   - flash: a pass per job or request, at least every APP_FLASH_IDLE_WAIT_MS (100 ms) between
 *     them, and an indication after every call of lib/boot in a job (flash_job.h), up to
 *     thousands in a cycle (the validation of an image is a step per 64 bytes): the same bounds
 *     as can_rx. But one call can block the task for longer than a supervision cycle: a record
 *     written into a metadata sector that has to be erased first takes a sector erase and three
 *     page programs, 409 ms at the part's maximum times (plat_flash.h), and the erase steps of
 *     a download take 400 ms each at the maximum, back to back. So the flash entity is examined
 *     every 6 cycles (reference_cycle 6): a window of 600 ms, which the health task's response
 *     time (under 45 ms, task_table.c) shortens to no less than 555 ms, always holds an
 *     indication of a flash task that works, also with every call at the part's maximum time and
 *     the task's own response time to it (under 70 ms) on top. failed_tolerance 1: a flash task that
 *     stops (a job that never returns, --flash-hang in the tests) makes the first window that
 *     begins after its last indication incorrect and the second EXPIRED, 1.2 to 1.8 s after
 *     that indication, and the hardware watchdog resets the node APP_WDG_TIMEOUT_MS after the
 *     last kick. A download stuck in the flash task therefore ends in a reset, never in a hang:
 *     the tester has had responsePending every 2 s until then, and gets no answer after it.
 * failed_tolerance 2: two failed cycles, net of good ones in between, are tolerated, so that one
 * pass of an event-driven task that slips past the end of a cycle is not taken for a hang, and
 * a task that stands still for up to two cycles and then goes on is not reset for it: the
 * cycle in which it goes on is correct again (the upper bounds above), and the good cycles
 * after it make the entity OK. Measured from the task's last indication before the stall to its
 * first after it, a stall of less than 300 ms leaves at most two examinations without an
 * indication, wherever it falls against the health task's period, and one of more than 400 ms
 * leaves three; the health task's response time moves an examination by up to its worst case,
 * and these figures by as much. A task that stops makes the first cycle that begins after its
 * last indication incorrect, and the third such cycle makes it EXPIRED (T + 1 = 3): 300 to
 * 400 ms after the last indication (lib/wdgm/wdgm.h: within T + 2 reference cycles). The kick
 * of the cycle before was the last, 200 to 300 ms after it, and the hardware watchdog resets
 * the node APP_WDG_TIMEOUT_MS after that kick: 700 to 800 ms after the last indication, plus
 * the health task's response times. A task that goes on after the third incorrect cycle is
 * reset all the same: STOPPED is final.
 */
typedef enum {
    SUPERVISED_CAN_RX = 0,
    SUPERVISED_SENSOR,
    SUPERVISED_PROCESS,
    SUPERVISED_CAN_TX,
    SUPERVISED_DIAG,
    SUPERVISED_FLASH,
    SUPERVISED_ENTITIES
} supervised_t;

static const wdgm_entity_config_t s_supervised[SUPERVISED_ENTITIES] = {
    [SUPERVISED_CAN_RX] = {.name = "can_rx",
                           .expected_alive = 2U,
                           .min_margin = 1U,
                           .max_margin = 65533U,
                           .reference_cycle = 1U,
                           .failed_tolerance = 2U},
    [SUPERVISED_SENSOR] = {.name = "sensor",
                           .expected_alive = 10U,
                           .min_margin = 9U,
                           .max_margin = 10U,
                           .reference_cycle = 1U,
                           .failed_tolerance = 2U},
    [SUPERVISED_PROCESS] = {.name = "process",
                            .expected_alive = 10U,
                            .min_margin = 9U,
                            .max_margin = 11U,
                            .reference_cycle = 1U,
                            .failed_tolerance = 2U},
    [SUPERVISED_CAN_TX] = {.name = "can_tx",
                           .expected_alive = 2U,
                           .min_margin = 1U,
                           .max_margin = 65533U,
                           .reference_cycle = 1U,
                           .failed_tolerance = 2U},
    [SUPERVISED_DIAG] = {.name = "diag",
                         .expected_alive = 2U,
                         .min_margin = 1U,
                         .max_margin = 65533U,
                         .reference_cycle = 1U,
                         .failed_tolerance = 2U},
    [SUPERVISED_FLASH] = {.name = "flash",
                          .expected_alive = 2U,
                          .min_margin = 1U,
                          .max_margin = 65533U,
                          .reference_cycle = 6U,
                          .failed_tolerance = 1U}};

/* expired_tolerance 0: no supervision cycle with kicks after an entity expired. The time the
 * node has to record the failure before the reset is the hardware watchdog's timeout. */
static const wdgm_config_t s_supervision_config = {.entities = s_supervised,
                                                   .entity_count = (uint8_t)SUPERVISED_ENTITIES,
                                                   .expired_tolerance = 0U};

_Static_assert((uint32_t)SUPERVISED_ENTITIES <= WDGM_MAX_ENTITIES, "the manager has room");
_Static_assert((uint32_t)SUPERVISED_ENTITIES <= (uint32_t)FAULT_NONE, "entities fit a uint8_t");

/* A sample as the sensor task hands it over: lib/bms's sample, and which of its groups the
 * front end did not answer (they are in it with every channel flagged invalid), which only the
 * fault monitor needs to know. */
typedef struct {
    bms_sample_t bms;
    uint8_t unanswered; /* BMS_SAMPLE_* */
} app_sample_t;

/* What the application did with each message. Each message has one sender, the only task
 * that writes its entry; the health task reads all of them at the end. */
typedef struct {
    uint32_t published;
    uint32_t suppressed;
    uint32_t failed;
} message_counts_t;

static const char TAG[] = "app";

static StaticTask_t s_sensor_tcb;
static StackType_t s_sensor_stack[APP_TASK_STACK_WORDS];
static StaticTask_t s_process_tcb;
static StackType_t s_process_stack[APP_TASK_STACK_WORDS];
static StaticTask_t s_health_tcb;
static StackType_t s_health_stack[APP_TASK_STACK_WORDS];
static StaticTask_t s_flash_tcb;
static StackType_t s_flash_stack[APP_TASK_STACK_WORDS];
static TaskHandle_t s_flash_task;

/* The queues' storage as arrays of their items: the right size and alignment by construction.
 * The kernel takes it as bytes (a conversion MISRA rule 11.3 permits, to a character type). */
static StaticQueue_t s_sample_queue_state;
static app_sample_t s_sample_queue_storage[APP_SAMPLE_QUEUE_LENGTH];
static QueueHandle_t s_sample_queue;
static StaticQueue_t s_verdict_queue_state;
static fault_verdict_t s_verdict_queue_storage[APP_FAULT_VERDICT_QUEUE_LENGTH];
static QueueHandle_t s_verdict_queue;

static StaticSemaphore_t s_pack_mutex_state;
static SemaphoreHandle_t s_pack_mutex;

static app_config_t s_config;
static node_stats_t s_final_stats;

/* Written by the sensor task. */
static node_sensor_stats_t s_sensor;

/* Written by the processing task; s_pack only while it holds s_pack_mutex. */
static node_process_stats_t s_process;
static bms_pack_t s_pack;
static telemetry_sender_t s_sender;

/* Written by the health task: its overruns, the DTCs and the counter of BMS_Fault, the state
 * of the watchdog manager (but for the alive counters, see SUPERVISION), and what the
 * supervision saw. */
static uint32_t s_health_overruns;
static fault_monitor_t s_faults;
static e2e_p11_protect_state_t s_fault_counter;
static wdgm_state_t s_wdgm;
static node_supervision_stats_t s_supervision;
static uint32_t s_seen_examinations[SUPERVISED_ENTITIES];
static bool s_expiry_recorded;

/* The summary of the DTCs as the health task published it after its last period; read and
 * written in critical sections. */
static fault_summary_t s_fault_summary;

/* Owned by the receive task: every call on it is made there (on_command, on_command_cycle). */
static command_rx_t s_command;

/* The telemetry switch and the count of clear requests as the receive task last left them;
 * read and written in critical sections. */
static bool s_telemetry_on;
static uint32_t s_clear_requests;

static message_counts_t s_counts[SLOTS];

/* The bootloader's hand-off, read in app_start() and constant after it: read by any task. */
static bool s_have_handoff;
static plat_boot_handoff_t s_handoff;

/* The confirm of a trial: the health task's monitor (update.h). */
static update_monitor_t s_update;

/* Owned by the flash task: lib/boot's state and what the task did. The result of a confirm
 * goes to the health task in s_confirm_result, s_confirm_reported, read and written in
 * critical sections. */
static boot_t s_boot;
static bootimg_t s_validator;
static flash_jobs_t s_jobs;
static node_flash_task_stats_t s_flash_stats;
static boot_status_t s_confirm_result;
static bool s_confirm_reported;

/* What the flash task tells the others besides the confirm's result, read and written in
 * critical sections: the trial was confirmed (by either way), and by the routine (for the health
 * task, which then takes it into its monitor once: s_routine_reported). */
static bool s_trial_confirmed;
static bool s_routine_confirmed;
static bool s_routine_reported;

/* The notification bits of the flash task. */
#define FLASH_NOTIFY_CONFIRM 0x01U /* the health task asks for the confirm of the trial */
#define FLASH_NOTIFY_JOB     0x02U /* the diagnostic task posted a job */

/* The mailbox of the flash task's jobs (THE DIAGNOSTIC TASK): one job, its state and its result,
 * read and written in critical sections. The diagnostic task posts (IDLE -> POSTED) and takes the
 * result (DONE -> IDLE); the flash task runs it (POSTED -> RUNNING -> DONE). */
typedef enum { JOB_IDLE = 0, JOB_POSTED, JOB_RUNNING, JOB_DONE } job_state_t;

static job_state_t s_job_state;
static flash_job_t s_job;
static flash_job_result_t s_job_result;

/* The diagnostic task: the connection and the backend, owned by it. */
static StaticTask_t s_diag_tcb;
static StackType_t s_diag_stack[APP_TASK_STACK_WORDS];
static TaskHandle_t s_diag_task;
static diag_t s_diag;
static diag_backend_t s_diag_backend;
static dtc_state_t s_diag_dtc; /* its copy of the published snapshot */
static uint32_t s_diag_events;
static uint32_t s_diag_passes;
static uint32_t s_diag_budget_waits;

/* The notification bits of the diagnostic task. */
#define DIAG_NOTIFY_EVENT  0x01U /* a frame or a report in a queue */
#define DIAG_NOTIFY_JOB    0x02U /* the flash task finished the job */
#define DIAG_NOTIFY_HEALTH 0x04U /* the health task carried out a clear */

static StaticQueue_t s_diag_rx_queue_state;
static can_frame_t s_diag_rx_queue_storage[APP_DIAG_RX_QUEUE_LENGTH];
static QueueHandle_t s_diag_rx_queue;
static StaticQueue_t s_diag_tx_done_queue_state;
static uint64_t s_diag_tx_done_queue_storage[APP_DIAG_TX_DONE_QUEUE_LENGTH];
static QueueHandle_t s_diag_tx_done_queue;

/* Written by the receive task: UDS_Request frames lost, and the queue's high-water mark. */
static uint32_t s_diag_rx_lost;
static uint32_t s_diag_rx_high_water;
/* Written by the transmit task: reports lost. */
static uint32_t s_diag_tx_done_lost;

/* Published by the health task after every period, read by the diagnostic task, in critical
 * sections: the DTCs (a copy of lib/dtc's state), the count of clear requests it has carried out
 * (fault_monitor_t.clear_requests), and the confirm's state. */
static dtc_state_t s_dtc_published;
static uint32_t s_clears_carried_out;
static update_confirm_state_t s_confirm_state_published;

/* ClearDiagnosticInformation requests of the diagnostic task: added to those of TESTER_Command
 * (s_clear_requests), the sum is the count the health task carries out. Critical sections. */
static uint32_t s_uds_clear_requests;

/* The health task's: the count of TESTER_Command's clear requests at its last clear, for the log
 * line that names the source of a clear. */
static uint32_t s_command_clears_seen;

_Static_assert(APP_DIAG_RX_QUEUE_LENGTH == (DIAG_BLOCK_SIZE + 2U),
               "the queue holds a block of the node's BS and two frames more");
_Static_assert(APP_DIAG_TX_DONE_QUEUE_LENGTH >= DIAG_HANDOUTS,
               "a report for every frame the connection has recorded");

/* ------------------------------------------------------------------------------- helpers */

/* Hand a frame that was built (`built`) to its slot, and count what happened to it. */
static void publish(uint8_t slot, bool built, const can_frame_t *frame, uint64_t stamp_ns)
{
    if (built && can_io_publish(slot, frame, stamp_ns)) {
        s_counts[slot].published++;
    } else {
        s_counts[slot].failed++;
    }
}

/* The telemetry switch, as the receive task last left it, and the count of clear requests: those
 * of TESTER_Command (the receive task's) and those of ClearDiagnosticInformation (the diagnostic
 * task's), added up. Both only grow, so the sum does, and the health task carries out a clear for
 * every new sum as it did for the count of TESTER_Command alone (fault_clear_if_requested()). */
static void command_outputs(bool *telemetry_on, uint32_t *clear_requests)
{
    taskENTER_CRITICAL();
    *telemetry_on = s_telemetry_on;
    *clear_requests = s_clear_requests + s_uds_clear_requests;
    taskEXIT_CRITICAL();
}

/* The count of clear requests alone (see command_outputs()). */
static uint32_t clear_requests_counted(void)
{
    uint32_t requests;

    taskENTER_CRITICAL();
    requests = s_clear_requests + s_uds_clear_requests;
    taskEXIT_CRITICAL();

    return requests;
}

/* Whether a fault is active in the summary of the health task's last period: a DTC with
 * testFailed, or one whose testFailed rose in that period (fault.h). */
static bool fault_active(void)
{
    bool active;

    taskENTER_CRITICAL();
    active = (s_fault_summary.active != 0U);
    taskEXIT_CRITICAL();

    return active;
}

/* An alive indication of a supervised task. */
static void alive(supervised_t entity)
{
    (void)wdgm_checkpoint(&s_wdgm, (uint8_t)entity);
}

/* ---------------------------------------------------------------------------- the sensor */

/* Sample number `index` (0, 1, 2, ...): the current in every one, the cells in every
 * APP_CELLS_EVERY-th, the temperatures in every APP_TEMPS_EVERY-th, the first one with all
 * three. A group the front end did not answer goes in with every channel flagged invalid, so
 * that lib/bms makes it invalid rather than keeping its last values (lib/bms/bms.h), and is
 * noted in `unanswered` for the fault monitor. */
static void acquire(app_sample_t *out, uint32_t index)
{
    bms_sample_t *sample = &out->bms;

    (void)memset(out, 0, sizeof(*out));
    sample->stamp_ns = plat_time_ns();
    sample->sequence = index;
    sample->groups = (uint8_t)BMS_SAMPLE_CURRENT;
    s_sensor.current_reads++;

    if (!plat_sensor_read_current(&sample->current_ma, &sample->current_invalid)) {
        sample->current_invalid = true;
        out->unanswered = (uint8_t)(out->unanswered | BMS_SAMPLE_CURRENT);
        s_sensor.current_unanswered++;
    }

    if ((index % APP_CELLS_EVERY) == 0U) {
        sample->groups = (uint8_t)(sample->groups | BMS_SAMPLE_CELLS);
        s_sensor.cell_reads++;

        if (!plat_sensor_read_cells(sample->cell_mv, &sample->cell_invalid)) {
            sample->cell_invalid = (uint8_t)BMS_ALL_CELLS;
            out->unanswered = (uint8_t)(out->unanswered | BMS_SAMPLE_CELLS);
            s_sensor.cell_unanswered++;
        }
    }

    if ((index % APP_TEMPS_EVERY) == 0U) {
        sample->groups = (uint8_t)(sample->groups | BMS_SAMPLE_TEMPS);
        s_sensor.temp_reads++;

        if (!plat_sensor_read_temps(sample->temp_dc, &sample->temp_invalid)) {
            sample->temp_invalid = (uint8_t)BMS_ALL_TEMPS;
            out->unanswered = (uint8_t)(out->unanswered | BMS_SAMPLE_TEMPS);
            s_sensor.temp_unanswered++;
        }
    }
}

/*
 * The sensor task's periods lie on a grid of APP_SENSOR_PERIOD_MS from its start. After an
 * overrun, xTaskDelayUntil() has not waited and has moved `last_wake` on by one period, to the
 * release that has passed already; a task that went on from there would make up every period
 * it missed, back to back. It skips them instead: `last_wake` moves on by the whole periods
 * that have passed since, to the start of the period the tick is in, so that this pass is that
 * period's and the next one waits for the next period of the same grid. The periods skipped
 * are counted (skipped_periods); they have no heartbeat and no sample, and the period counter
 * and the sample index go on from the pass before, without a gap: they count passes.
 */
static void skip_missed_periods(TickType_t *last_wake)
{
    const TickType_t period = pdMS_TO_TICKS(APP_SENSOR_PERIOD_MS);
    const TickType_t behind = (TickType_t)(xTaskGetTickCount() - *last_wake);
    const TickType_t missed = behind / period;

    *last_wake = (TickType_t)(*last_wake + (TickType_t)(missed * period));
    s_sensor.skipped_periods += (uint32_t)missed;
}

static void sensor_task(void *arg)
{
    TickType_t last_wake;
    uint32_t periods = 0U;

    (void)arg;
    plat_task_enter("sensor");
    last_wake = xTaskGetTickCount();

    for (;;) {
        can_frame_t frame = {0};
        app_sample_t sample;

        /* pdFALSE: the release time had passed already, the last period overran. */
        if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_SENSOR_PERIOD_MS)) == pdFALSE) {
            s_sensor.overruns++;
            skip_missed_periods(&last_wake);
        }

        periods++;

        /* The heartbeat first, as phase 1's ticker sent its status frame: the tick it carries
         * is the one its period ended in, or a later one only when the task ran late. */
        const bool built = telemetry_heartbeat(periods, (uint32_t)xTaskGetTickCount(), &frame);

        publish(SLOT_HEARTBEAT, built, &frame, plat_time_ns());

        acquire(&sample, periods - 1U);

        /* No waiting: a processing task that is behind loses this sample, it does not hold up
         * the time base. */
        if (xQueueSend(s_sample_queue, &sample, 0U) == pdTRUE) {
            s_sensor.samples_posted++;
        } else {
            s_sensor.queue_full++;
        }

        s_sensor.periods = periods;

        /* After the reads: a front end that hangs keeps the task from getting here. */
        alive(SUPERVISED_SENSOR);
    }
}

/* ------------------------------------------------------------------------- the processing */

/* Build and publish one protected message, or count it as suppressed while the telemetry is
 * off. */
static void publish_protected(uint8_t slot, bool telemetry_on, bool faults, const bms_pack_t *pack,
                              uint64_t stamp_ns)
{
    can_frame_t frame = {0};

    if (!telemetry_on) {
        s_counts[slot].suppressed++;
    } else {
        bool built;

        switch (slot) {
        case SLOT_PACK_STATUS:
            built = telemetry_pack_status(&s_sender, pack, faults, &frame);
            break;
        case SLOT_CELL_VOLTAGES1:
            built = telemetry_cell_voltages1(&s_sender, pack, &frame);
            break;
        case SLOT_CELL_VOLTAGES2:
            built = telemetry_cell_voltages2(&s_sender, pack, &frame);
            break;
        default:
            built = telemetry_temperatures(&s_sender, pack, &frame);
            break;
        }

        publish(slot, built, &frame, stamp_ns);
    }
}

/* Hand a verdict to the health task, the owner of the DTCs; never wait for room. */
static void post_verdict(const fault_verdict_t *verdict)
{
    if (xQueueSend(s_verdict_queue, verdict, 0U) == pdTRUE) {
        s_process.verdicts_posted++;
    } else {
        s_process.verdicts_lost++;
    }
}

static void process_sample(const app_sample_t *sample)
{
    bms_pack_t pack;
    fault_verdict_t verdict;
    bool telemetry_on;
    uint32_t clear_requests;
    bool faults;

    (void)xSemaphoreTake(s_pack_mutex, portMAX_DELAY);
    bms_process(&s_pack, &s_bms_config, &sample->bms);
    pack = s_pack;
    (void)xSemaphoreGive(s_pack_mutex);

    /* The verdict on the faults, from the copy that this sample made, tagged with the clear
     * requests counted by now: the health task clears before it, for a request it has not
     * carried out yet, and so keeps this evidence. */
    command_outputs(&telemetry_on, &clear_requests);
    fault_judge_sample(&sample->bms, sample->unanswered, &pack, &verdict);
    verdict.clear_requests = clear_requests;
    post_verdict(&verdict);

    s_process.samples++;
    faults = fault_active();

    /* Each message goes out with the samples that carry its measurements, stamped with the
     * time of the sample, so that the transmit-done report gives the sample-to-bus latency. */
    publish_protected(SLOT_PACK_STATUS, telemetry_on, faults, &pack, sample->bms.stamp_ns);

    if ((sample->bms.groups & BMS_SAMPLE_CELLS) != 0U) {
        publish_protected(SLOT_CELL_VOLTAGES1, telemetry_on, faults, &pack, sample->bms.stamp_ns);
        publish_protected(SLOT_CELL_VOLTAGES2, telemetry_on, faults, &pack, sample->bms.stamp_ns);
    }

    if ((sample->bms.groups & BMS_SAMPLE_TEMPS) != 0U) {
        publish_protected(SLOT_TEMPERATURES, telemetry_on, faults, &pack, sample->bms.stamp_ns);
    }
}

static void process_task(void *arg)
{
    bool timed_out = false;

    (void)arg;
    plat_task_enter("process");

    for (;;) {
        app_sample_t sample;

        if (xQueueReceive(s_sample_queue, &sample, pdMS_TO_TICKS(APP_SAMPLE_TIMEOUT_MS)) ==
            pdTRUE) {
            const uint32_t behind = (uint32_t)uxQueueMessagesWaiting(s_sample_queue);

            /* The sample just taken and those behind it were in the queue at once. Any behind
             * it means this task did not finish before the next release: an overrun. */
            if ((behind + 1U) > s_process.high_water) {
                s_process.high_water = behind + 1U;
            }

            if (behind > 0U) {
                s_process.backlog++;
            }

            /* Counted, not logged: the health task writes the lines (log_sample_wait()). */
            if (timed_out) {
                s_process.recoveries++;
                timed_out = false;
            }

            process_sample(&sample);
        } else {
            fault_verdict_t verdict;

            s_process.timeouts++;
            fault_judge_timeout(&verdict);
            verdict.clear_requests = clear_requests_counted();
            post_verdict(&verdict);
            timed_out = true;
        }

        alive(SUPERVISED_PROCESS);
    }
}

/* -------------------------------------------------------------------- the command receiver */

/* Receive task context. The switch and the count of clear requests are copied out for the
 * other tasks after every call that can change them. */
static void share_command_outputs(void)
{
    const bool on = command_telemetry_on(&s_command);
    const uint32_t clears = command_clear_requests(&s_command);

    taskENTER_CRITICAL();
    s_telemetry_on = on;
    s_clear_requests = clears;
    taskEXIT_CRITICAL();
}

static void on_command(const can_frame_t *frame, uint64_t eof_ns)
{
    (void)eof_ns;
    command_on_frame(&s_command, frame);
    share_command_outputs();
}

static void on_command_cycle(void)
{
    command_on_cycle(&s_command);
    share_command_outputs();
}

/* Receive task context: answer a ping with the same payload. */
static void on_ping(const can_frame_t *frame, uint64_t eof_ns)
{
    can_frame_t pong = *frame;

    (void)eof_ns;
    pong.id = CAN_DB_BMS_PONG_ID;
    (void)can_io_send_event(&pong);
}

/* The alive indications of the two CAN tasks, from inside their loops (can_io.h). */
static void can_rx_alive(void)
{
    alive(SUPERVISED_CAN_RX);
}

static void can_tx_alive(void)
{
    alive(SUPERVISED_CAN_TX);
}

/* --------------------------------------------------------------------------- supervision */

/* The fewest and the most alive indications of each entity in one examined cycle. */
static void note_alive_counts(void)
{
    for (uint32_t i = 0U; i < (uint32_t)SUPERVISED_ENTITIES; i++) {
        const wdgm_entity_state_t *entity = &s_wdgm.entity[i];
        node_supervised_stats_t *seen = &s_supervision.entities[i];

        if (entity->examinations != s_seen_examinations[i]) {
            if ((s_seen_examinations[i] == 0U) || (entity->last_count < seen->min_count)) {
                seen->min_count = entity->last_count;
            }

            if (entity->last_count > seen->max_count) {
                seen->max_count = entity->last_count;
            }

            s_seen_examinations[i] = entity->examinations;
        }
    }
}

/* Health task context, first thing in every period: one supervision cycle, and the kick of the
 * hardware watchdog if the watchdog manager allows it. */
static void supervise(void)
{
    const wdgm_global_status_t status = wdgm_main_function(&s_wdgm, &s_supervision_config);

    note_alive_counts();

    if (wdgm_may_kick(&s_wdgm)) {
        plat_wdg_kick();
        s_supervision.kicks++;
    } else {
        s_supervision.kicks_withheld++;
    }

    if (((status == WDGM_GLOBAL_STATUS_EXPIRED) || (status == WDGM_GLOBAL_STATUS_STOPPED)) &&
        !s_expiry_recorded) {
        const uint8_t first = wdgm_first_expired(&s_wdgm);
        const bool known = first < (uint8_t)SUPERVISED_ENTITIES;

        LOG_ERR(TAG,
                "supervision: %s expired (%lu alive in its last cycle); no more kicks: reset "
                "within %u ms",
                known ? s_supervised[first].name : "the state",
                known ? (unsigned long)s_wdgm.entity[first].last_count : 0UL,
                (unsigned int)APP_WDG_TIMEOUT_MS);
        s_expiry_recorded = true;
    }
}

/* ------------------------------------------------------------------------------ faults */

/* What the health task last saw of the processing task's wait for samples. */
typedef struct {
    uint32_t timeouts;
    uint32_t recoveries;
    bool stalled; /* the start of a stall is logged, its end not yet */
} sample_wait_t;

static sample_wait_t s_sample_wait;

/*
 * Health task context, once per period: the lines about the processing task's wait for samples,
 * written from its counts, so that the processing job makes no log call (task_table.c). A stall
 * begins with the first timeout after a sample and ends with the first sample after a timeout
 * (s_process.recoveries). At most one line of each per period: two stalls that begin and end
 * within one period are one pair of lines, and a stall that begins in the period in which the
 * last one ended gets its first line in the next period.
 */
static void log_sample_wait(void)
{
    uint32_t timeouts;
    uint32_t recoveries;

    taskENTER_CRITICAL();
    timeouts = s_process.timeouts;
    recoveries = s_process.recoveries;
    taskEXIT_CRITICAL();

    const bool timed_out = timeouts != s_sample_wait.timeouts;
    const bool recovered = recoveries != s_sample_wait.recoveries;

    if ((timed_out || recovered) && !s_sample_wait.stalled) {
        LOG_WRN(TAG, "no sample from the sensor task for %u ms",
                (unsigned int)APP_SAMPLE_TIMEOUT_MS);
        s_sample_wait.stalled = true;
    }

    if (recovered) {
        LOG_INF(TAG, "samples again after %lu timeouts", (unsigned long)timeouts);
        s_sample_wait.stalled = false;
    }

    s_sample_wait.timeouts = timeouts;
    s_sample_wait.recoveries = recoveries;
}

/* A line for every change of a fault's testFailed in this period: a fault that became active,
 * and one that stopped being active (by a pass or by a clear). A fault that did both in the
 * period gets both lines, in the order its testFailed at the end implies (set: it stopped, then
 * became active again). */
static void log_fault_changes(const fault_changes_t *changes)
{
    for (uint32_t event = 0U; event < (uint32_t)FAULT_EVENTS; event++) {
        const uint32_t mask = (uint32_t)(1UL << event);
        const bool rose = (changes->rose & mask) != 0U;
        const bool fell = (changes->fell & mask) != 0U;
        const bool failed = (changes->failed & mask) != 0U;

        if (fell && (!rose || failed)) {
            LOG_INF(TAG, "fault %s no longer active", fault_name(event));
        }

        if (rose) {
            LOG_WRN(TAG, "fault %s active (DTC 0x%06lX)", fault_name(event),
                    (unsigned long)fault_dtc_number(event));
        }

        if (fell && rose && !failed) {
            LOG_INF(TAG, "fault %s no longer active", fault_name(event));
        }
    }
}

/*
 * Health task context, after the supervision cycle: work the processing task's verdicts into
 * the DTCs in their order, judge the CAN controller and the watchdog manager, carry out the
 * clear requests, publish the summary, and send BMS_Fault with it. Hands the confirm of a trial
 * what it read of the watchdog manager and the controller, with the transmit queue's count of
 * acknowledged frames (update.h).
 *
 * The clears in order with the evidence. A verdict carries the count of clear requests the
 * processing task had read when it posted it, and fault_apply() clears before the first one
 * that carries a request not carried out yet. A request that no verdict of this job carries
 * is carried out after the verdicts, before the health task's own monitors. Their inputs are
 * read first, then the count, so that every request counted before the inputs were read is
 * carried out before the inputs are worked in: a clear erases nothing the inputs show after
 * its request. They show what happened since the last period, though, and cannot tell what of
 * it came before the request: a bus-off or an entry into error passive between the last
 * period and the request survives the clear. The count is read before the queue is
 * emptied: every verdict posted later carries it or a newer one, since the processing task,
 * above this one, does not block between reading the count and posting the verdict. The job
 * takes up to as many verdicts as the queue holds, so a verdict it leaves for the next period
 * was posted after the count was read; the queue is FIFO and the count only grows, so the tags
 * never go back, and no verdict left for the next period comes from before a clear of this
 * one.
 */
static void run_faults(update_health_t *confirm)
{
    fault_verdict_t verdict;
    can_io_stats_t io;
    can_txq_stats_t queue;
    plat_can_status_t controller;
    fault_summary_t summary;
    fault_changes_t changes;
    can_frame_t frame;
    bool more = true;
    const uint32_t clears = s_faults.clears;

    can_io_get_stats(&io, &queue);
    plat_can_get_status(&controller);

    const fault_health_inputs_t inputs = {.bus_off_count = queue.bus_off_count,
                                          .error_passive_entries = io.error_passive_entries,
                                          .can_state = (can_fc_state_t)controller.state,
                                          .supervision = wdgm_global_status(&s_wdgm)};
    const uint32_t clear_requests = clear_requests_counted();

    /* At most a queue full in one period, so that the job has a bound (task_table.c): what the
     * processing task posts meanwhile waits for the next period, in its order. */
    for (uint32_t i = 0U; more && (i < APP_FAULT_VERDICT_QUEUE_LENGTH); i++) {
        more = (xQueueReceive(s_verdict_queue, &verdict, 0U) == pdTRUE);

        if (more) {
            fault_apply(&s_faults, &verdict);
        }
    }

    (void)fault_clear_if_requested(&s_faults, clear_requests);
    fault_health_cycle(&s_faults, &inputs);

    if (s_faults.clears != clears) {
        uint32_t command_clears;

        taskENTER_CRITICAL();
        command_clears = s_clear_requests;
        taskEXIT_CRITICAL();

        /* Which source asked: TESTER_Command if its count moved since the last clear (phase 3's
         * line), otherwise ClearDiagnosticInformation. */
        if (command_clears != s_command_clears_seen) {
            LOG_INF(TAG, "DTCs cleared: CmdClearFaultsSeq changed (clear %lu)",
                    (unsigned long)s_faults.clears);
        } else {
            LOG_INF(TAG, "DTCs cleared: ClearDiagnosticInformation (clear %lu)",
                    (unsigned long)s_faults.clears);
        }

        s_command_clears_seen = command_clears;
    }

    fault_summary(&s_faults, &summary, &changes);
    log_fault_changes(&changes);

    /* The summary, and for the diagnostic task the snapshot of the DTCs and the clears carried
     * out, all of the end of this period (THE DIAGNOSTIC TASK). */
    taskENTER_CRITICAL();
    s_fault_summary = summary;
    s_dtc_published = s_faults.dtc;
    s_clears_carried_out = s_faults.clear_requests;
    taskEXIT_CRITICAL();

    if (s_faults.clears != clears) {
        (void)xTaskNotify(s_diag_task, DIAG_NOTIFY_HEALTH, eSetBits);
    }

    const bool built = telemetry_fault(&s_fault_counter, &summary, &frame);

    publish(SLOT_FAULT, built, &frame, plat_time_ns());

    confirm->supervision = inputs.supervision;
    confirm->can_state = inputs.can_state;
    confirm->bus_off_count = inputs.bus_off_count;
    confirm->tx_acked = queue.sent;
}

/* ------------------------------------------------------------------------------ update */

/* The three parts of an image version (lib/image's encoding: major.minor.patch), for a log
 * line's "%u.%u.%u". */
static unsigned int version_major(uint32_t version)
{
    return (unsigned int)(version >> 24U);
}

static unsigned int version_minor(uint32_t version)
{
    return (unsigned int)((version >> 16U) & 0xFFU);
}

static unsigned int version_patch(uint32_t version)
{
    return (unsigned int)(version & 0xFFFFU);
}

static const char *slot_name(uint8_t slot)
{
    const char *name = "none";

    if (slot == 0U) {
        name = "A";
    } else if (slot == 1U) {
        name = "B";
    } else {
        /* no slot */
    }

    return name;
}

/* Health task context, after run_faults() with what it read for the confirm (`health`): take the
 * flash task's result of a confirm, judge the period for the confirm of a trial, and ask the
 * flash task for it when the trial has run long enough in good health. */
static void run_update(const update_health_t *health)
{
    boot_status_t result;
    bool reported;
    bool by_routine;

    taskENTER_CRITICAL();
    reported = s_confirm_reported;
    result = s_confirm_result;
    s_confirm_reported = false;
    by_routine = s_routine_confirmed && !s_routine_reported;
    s_routine_reported = s_routine_confirmed;
    taskEXIT_CRITICAL();

    /* The routine's confirm first: a confirm of the window that the flash task answered after
     * it found the trial confirmed then changes nothing (update.h). */
    if (by_routine) {
        update_routine_confirmed(&s_update);
        LOG_INF(TAG, "the trial in slot %s is confirmed by the tester's confirm routine",
                slot_name(s_handoff.slot));
    }

    if (reported) {
        update_confirm_done(&s_update, result);
    }

    if (update_health_period(&s_update, health)) {
        (void)xTaskNotify(s_flash_task, FLASH_NOTIFY_CONFIRM, eSetBits);
    }

    taskENTER_CRITICAL();
    s_confirm_state_published = s_update.state;
    taskEXIT_CRITICAL();
}

/* Flash task context: the metadata as the task finds them at start, into its statistics and
 * the log. Reads only. */
static void describe_metadata(void)
{
    boot_description_t d;

    s_flash_stats.describe_status = boot_describe(&s_boot, &d);

    if (s_flash_stats.describe_status == BOOT_OK) {
        s_flash_stats.described = true;
        s_flash_stats.have_record = d.have_record;
        s_flash_stats.seq = d.seq;
        s_flash_stats.active_slot = d.active_slot;
        s_flash_stats.trial_slot = d.trial_slot;
        s_flash_stats.attempts_used = d.attempts_used;
        s_flash_stats.records_valid = d.records_valid;
        LOG_INF(TAG,
                "metadata: record %lu, active slot %s, trial slot %s with %u of %u attempts used",
                (unsigned long)d.seq, slot_name(d.active_slot), slot_name(d.trial_slot),
                (unsigned int)d.attempts_used, (unsigned int)BOOT_MAX_ATTEMPTS);
    } else {
        LOG_WRN(TAG, "metadata: cannot be read (%s)",
                boot_status_name(s_flash_stats.describe_status));
    }
}

/* Flash task context: the confirm the health task asked for, unless the tester's routine has
 * made it (flash_jobs_self_confirm()). */
static boot_status_t confirm_trial(void)
{
    const uint64_t start_ns = plat_time_ns();
    /* The window in whole seconds: the product's, or the one a host test gave (app_config_t). */
    const uint32_t window_s = (s_config.confirm_periods != 0U)
                                  ? (s_config.confirm_periods / (1000U / APP_HEALTH_PERIOD_MS))
                                  : UPDATE_CONFIRM_AFTER_S;
    bool written = false;

    LOG_INF(TAG, "trial in slot %s ran %u s in good health: writing its confirm",
            slot_name(s_handoff.slot), (unsigned int)window_s);

    const boot_status_t status = flash_jobs_self_confirm(&s_jobs, &written);
    const uint64_t took_ns = plat_time_ns() - start_ns;

    taskENTER_CRITICAL();
    s_flash_stats.confirms += written ? 1U : 0U;
    s_flash_stats.confirm_status = status;
    s_flash_stats.confirm_ns = written ? took_ns : s_flash_stats.confirm_ns;
    s_flash_stats.self_skipped = s_jobs.self_skipped;
    s_trial_confirmed = s_jobs.confirmed;
    taskEXIT_CRITICAL();

    if ((status == BOOT_OK) && !written) {
        LOG_INF(TAG, "the trial is confirmed by the tester's confirm routine already: nothing "
                     "written");
    } else if (status == BOOT_OK) {
        LOG_INF(TAG, "confirmed: slot %s, image %u.%u.%u, is the active image (%lu us)",
                slot_name(s_handoff.slot), version_major(s_handoff.image_version),
                version_minor(s_handoff.image_version), version_patch(s_handoff.image_version),
                (unsigned long)(took_ns / 1000U));
    } else {
        LOG_ERR(TAG,
                "the confirm of slot %s failed: %s; it stays a trial, rolled back after its "
                "attempts unless a later start confirms it",
                slot_name(s_handoff.slot), boot_status_name(status));
    }

    return status;
}

/* Flash task context: an alive indication from inside a job (flash_job.h). */
static void flash_alive(void *ctx)
{
    (void)ctx;
    alive(SUPERVISED_FLASH);
}

/* Flash task context: the log line at the end of a job. Writes are many (a line each would be
 * a line per TransferData block): only their failures are logged. */
static void log_job(const flash_job_t *job, const flash_job_result_t *result, uint64_t took_ns)
{
    const bool ok = (result->status == BOOT_OK) ||
                    ((job->kind == FLASH_JOB_EXIT) && (result->status == BOOT_BUSY));

    if (!ok) {
        LOG_WRN(TAG, "flash job %s: %s after %lu steps (%lu us)", flash_job_kind_name(job->kind),
                boot_status_name(result->status), (unsigned long)result->steps,
                (unsigned long)(took_ns / 1000U));
    } else if (job->kind == FLASH_JOB_ERASE) {
        LOG_INF(TAG, "download: slot at 0x%06lX erased for %lu bytes, %lu steps (%lu us)",
                (unsigned long)job->address, (unsigned long)job->size, (unsigned long)result->steps,
                (unsigned long)(took_ns / 1000U));
    } else if (job->kind == FLASH_JOB_CHECK) {
        LOG_INF(TAG,
                "download: the image validated and is committed as the trial, %lu steps "
                "(%lu us)",
                (unsigned long)result->steps, (unsigned long)(took_ns / 1000U));
    } else if (job->kind == FLASH_JOB_CONFIRM) {
        LOG_INF(TAG,
                "confirmed by the tester's confirm routine: slot %s, image %u.%u.%u, is the "
                "active image (%lu us)",
                slot_name(s_handoff.slot), version_major(s_handoff.image_version),
                version_minor(s_handoff.image_version), version_patch(s_handoff.image_version),
                (unsigned long)(took_ns / 1000U));
    } else {
        /* a write or the exit: no line */
    }
}

/* Flash task context: the job the diagnostic task posted, if one is (THE DIAGNOSTIC TASK). */
static void run_posted_job(void)
{
    flash_job_t job = {
        .kind = FLASH_JOB_NONE, .address = 0U, .size = 0U, .offset = 0U, .data = NULL, .len = 0U};
    flash_job_result_t result = {.status = BOOT_E_ARG, .steps = 0U};
    bool posted;

    taskENTER_CRITICAL();
    posted = (s_job_state == JOB_POSTED);

    if (posted) {
        job = s_job;
        s_job_state = JOB_RUNNING;
    }

    taskEXIT_CRITICAL();

    if (posted) {
        const uint64_t start_ns = plat_time_ns();

        flash_job_run(&s_jobs, &job, flash_alive, NULL, &result);

        const uint64_t took_ns = plat_time_ns() - start_ns;
        const uint32_t kind = (uint32_t)job.kind;

        log_job(&job, &result, took_ns);

        taskENTER_CRITICAL();
        s_job_result = result;
        s_job_state = JOB_DONE;

        if (kind < (uint32_t)FLASH_JOB_KINDS) {
            s_flash_stats.jobs[kind]++;
            s_flash_stats.job_status[kind] = result.status;
            s_flash_stats.job_steps[kind] = result.steps;
            s_flash_stats.job_last_ns[kind] = took_ns;
            s_flash_stats.job_total_ns[kind] += took_ns;

            if (took_ns > s_flash_stats.job_max_ns[kind]) {
                s_flash_stats.job_max_ns[kind] = took_ns;
            }
        }

        s_flash_stats.confirmed_by_routine = s_jobs.by_routine;
        s_trial_confirmed = s_jobs.confirmed;
        s_routine_confirmed = s_jobs.by_routine;
        taskEXIT_CRITICAL();

        (void)xTaskNotify(s_diag_task, DIAG_NOTIFY_JOB, eSetBits);
    }
}

/*
 * The flash task: every write of the node to the flash (see THE FLASH TASK). Blocked between
 * jobs and requests, at most APP_FLASH_IDLE_WAIT_MS; each operation it makes blocks it for the
 * part's time (plat_flash.h), which only the idle task waits behind.
 */
static void flash_task(void *arg)
{
    const flash_dev_t *dev = plat_flash_device();
    const plat_flash_layout_t *layout = plat_flash_layout();
    boot_validator_t validator;
    boot_config_t config;

    (void)arg;
    plat_task_enter("flash");

    s_flash_stats.flash = (dev != NULL);
    s_flash_stats.boot_init_status = BOOT_E_ARG;

    /* lib/image's validation for the slot writer (bootimg.h), for the images of this target. */
    if (bootimg_init(&s_validator, layout->image_target, &validator) &&
        update_boot_config(layout, dev, &validator, &config)) {
        s_flash_stats.boot_init_status = boot_init(&s_boot, &config);
        s_flash_stats.boot_ready = (s_flash_stats.boot_init_status == BOOT_OK);
    }

    (void)flash_jobs_init(&s_jobs, s_flash_stats.boot_ready ? &s_boot : NULL,
                          s_have_handoff ? &s_handoff : NULL);

    /* With a hand-off the metadata say what the bootloader decided: read them once. */
    if (s_flash_stats.boot_ready && s_have_handoff) {
        describe_metadata();
    }

    for (;;) {
        uint32_t bits = 0U;

        if (xTaskNotifyWait(0U, UINT32_MAX, &bits, pdMS_TO_TICKS(APP_FLASH_IDLE_WAIT_MS)) !=
            pdTRUE) {
            s_flash_stats.idle_passes++;
        }

        alive(SUPERVISED_FLASH);

        /* A job is looked for on every pass, not only on its bit: the bit and the job are set in
         * two steps by the diagnostic task. */
        run_posted_job();

        if ((bits & FLASH_NOTIFY_CONFIRM) != 0U) {
            const boot_status_t status = confirm_trial();

            taskENTER_CRITICAL();
            s_confirm_result = status;
            s_confirm_reported = true;
            taskEXIT_CRITICAL();
        }
    }
}

/* -------------------------------------------------------------------- the diagnostic task */

/* The time the connection works with: microseconds of plat_time_ns(). */
static uint64_t now_us(void)
{
    return plat_time_ns() / 1000U;
}

/* Receive task context: a UDS_Request frame into the diagnostic task's queue, never waiting. */
static void on_uds_request(const can_frame_t *frame, uint64_t eof_ns)
{
    (void)eof_ns;

    if (xQueueSend(s_diag_rx_queue, frame, 0U) == pdTRUE) {
        const uint32_t held = (uint32_t)uxQueueMessagesWaiting(s_diag_rx_queue);

        if (held > s_diag_rx_high_water) {
            s_diag_rx_high_water = held;
        }

        (void)xTaskNotify(s_diag_task, DIAG_NOTIFY_EVENT, eSetBits);
    } else {
        s_diag_rx_lost++;
    }
}

/* Transmit task context: the report of a UDS_Response frame sent, never waiting. */
static void on_uds_response_sent(uint64_t stamp_ns)
{
    if (xQueueSend(s_diag_tx_done_queue, &stamp_ns, 0U) == pdTRUE) {
        (void)xTaskNotify(s_diag_task, DIAG_NOTIFY_EVENT, eSetBits);
    } else {
        s_diag_tx_done_lost++;
    }
}

/* The connection's port (diag_port_t): its frames are event frames of the transmit queue. */
static bool diag_send(void *ctx, const can_frame_t *frame, uint64_t *stamp_ns)
{
    (void)ctx;
    return can_io_send_event_stamped(frame, stamp_ns);
}

/* The backend's side of the node (diag_node_t), all in the diagnostic task's context. */

static bool node_post_job(void *ctx, const flash_job_t *job)
{
    bool taken;

    (void)ctx;
    taskENTER_CRITICAL();
    taken = (s_job_state == JOB_IDLE);

    if (taken) {
        s_job = *job;
        s_job_state = JOB_POSTED;
    }

    taskEXIT_CRITICAL();

    if (taken) {
        (void)xTaskNotify(s_flash_task, FLASH_NOTIFY_JOB, eSetBits);
    }

    return taken;
}

static bool node_job_done(void *ctx, flash_job_result_t *result)
{
    bool done;

    (void)ctx;
    taskENTER_CRITICAL();
    done = (s_job_state == JOB_DONE);

    if (done) {
        *result = s_job_result;
        s_job_state = JOB_IDLE;
    }

    taskEXIT_CRITICAL();

    return done;
}

static void node_boot_info(void *ctx, uds_boot_info_t *info)
{
    update_confirm_state_t state;
    bool confirmed;

    (void)ctx;
    taskENTER_CRITICAL();
    state = s_confirm_state_published;
    confirmed = s_trial_confirmed;
    taskEXIT_CRITICAL();

    (void)memset(info, 0, sizeof(*info));
    info->handoff = s_have_handoff;
    info->slot = s_handoff.slot;
    info->mode = (uint8_t)s_handoff.mode;
    info->attempt = s_handoff.attempt;
    info->event = (uint8_t)s_handoff.event;
    info->event_slot = s_handoff.event_slot;
    info->image_version = s_handoff.image_version;
    /* The flash task knows of a confirm before the health task's next period does. */
    info->confirm_state = (uint8_t)(confirmed ? UPDATE_CONFIRM_CONFIRMED : state);
}

static const dtc_state_t *node_dtc_snapshot(void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL();
    s_diag_dtc = s_dtc_published;
    taskEXIT_CRITICAL();

    return &s_diag_dtc;
}

static uint32_t node_clear_post(void *ctx)
{
    uint32_t number;

    (void)ctx;
    taskENTER_CRITICAL();
    s_uds_clear_requests++;
    number = s_clear_requests + s_uds_clear_requests;
    taskEXIT_CRITICAL();

    return number;
}

static bool node_clear_done(void *ctx, uint32_t number)
{
    uint32_t carried_out;

    (void)ctx;
    taskENTER_CRITICAL();
    carried_out = s_clears_carried_out;
    taskEXIT_CRITICAL();

    /* Counts that only grow, modulo 2^32: carried out up to `number` or beyond it. */
    return (carried_out - number) < 0x80000000U;
}

/* Ticks the task may wait for an event: until the connection's next deadline, rounded up to a
 * whole tick, but at least one and at most APP_DIAG_IDLE_WAIT_MS (a deadline that has passed is
 * handled in the pass anyway). */
static TickType_t diag_wait_ticks(void)
{
    const uint64_t deadline_us = diag_next_deadline(&s_diag);
    const uint64_t now = now_us();
    uint64_t wait_ms = APP_DIAG_IDLE_WAIT_MS;

    if (deadline_us != UINT64_MAX) {
        const uint64_t until_ms = (deadline_us > now) ? (((deadline_us - now) + 999U) / 1000U) : 1U;

        if (until_ms < wait_ms) {
            wait_ms = (until_ms == 0U) ? 1U : until_ms;
        }
    }

    return pdMS_TO_TICKS((TickType_t)wait_ms);
}

/* An ECUReset that is due: after the log line, the reset (diag.h, ECURESET). */
static void reset_if_due(void)
{
    if (diag_reset_due(&s_diag)) {
        LOG_WRN(TAG, "ECUReset: resetting (requested by the tester)");
        plat_reset(PLAT_RESET_REQUESTED);
    }
}

/* The events of one pass: the reports first, then the frames, while the tick's budget of
 * APP_DIAG_EVENTS_PER_TICK lasts. Returns true if events are left when the budget is used up. */
static bool take_events(TickType_t *budget_tick, uint32_t *taken)
{
    bool left = false;
    bool more = true;

    while (more) {
        const TickType_t tick = xTaskGetTickCount();
        uint64_t stamp_ns = 0U;
        can_frame_t frame;

        if (tick != *budget_tick) {
            *budget_tick = tick;
            *taken = 0U;
        }

        if (*taken >= APP_DIAG_EVENTS_PER_TICK) {
            left = (uxQueueMessagesWaiting(s_diag_tx_done_queue) != 0U) ||
                   (uxQueueMessagesWaiting(s_diag_rx_queue) != 0U);
            more = false;
        } else if (xQueueReceive(s_diag_tx_done_queue, &stamp_ns, 0U) == pdTRUE) {
            diag_on_tx_done(&s_diag, stamp_ns, now_us());
            (*taken)++;
            s_diag_events++;
        } else if (xQueueReceive(s_diag_rx_queue, &frame, 0U) == pdTRUE) {
            diag_on_frame(&s_diag, &frame, now_us());
            (*taken)++;
            s_diag_events++;
        } else {
            more = false;
        }

        reset_if_due();
    }

    return left;
}

/*
 * The diagnostic task (THE DIAGNOSTIC TASK): waits for an event or the connection's deadline,
 * takes the events, polls the connection. With events left over at the end of the tick's budget
 * it waits for the next tick instead, without a notification, and goes on.
 */
static void diag_task(void *arg)
{
    TickType_t budget_tick;
    uint32_t taken = 0U;
    bool left = false;

    (void)arg;
    plat_task_enter("diag");
    budget_tick = xTaskGetTickCount();

    for (;;) {
        uint32_t bits = 0U;

        if (left) {
            s_diag_budget_waits++;
            vTaskDelay(1U);
        } else {
            (void)xTaskNotifyWait(0U, UINT32_MAX, &bits, diag_wait_ticks());
        }

        alive(SUPERVISED_DIAG);
        s_diag_passes++;
        left = take_events(&budget_tick, &taken);
        diag_poll(&s_diag, now_us());
        reset_if_due();
    }
}

/* Before the scheduler: the connection, its backend, its queues and the task. */
static bool diag_start(void)
{
    static const diag_node_t node = {.ctx = NULL,
                                     .post_job = node_post_job,
                                     .job_done = node_job_done,
                                     .boot_info = node_boot_info,
                                     .dtc_snapshot = node_dtc_snapshot,
                                     .clear_post = node_clear_post,
                                     .clear_done = node_clear_done};
    static const diag_port_t port = {.ctx = NULL, .send = diag_send};
    uds_backend_t backend;
    bool ok;

    s_dtc_published = s_faults.dtc;
    s_confirm_state_published = s_update.state;
    ok = diag_backend_init(&s_diag_backend, &node, &backend) && diag_init(&s_diag, &port, &backend);

    s_diag_rx_queue =
        xQueueCreateStatic(APP_DIAG_RX_QUEUE_LENGTH, sizeof(can_frame_t),
                           (uint8_t *)s_diag_rx_queue_storage, &s_diag_rx_queue_state);
    s_diag_tx_done_queue =
        xQueueCreateStatic(APP_DIAG_TX_DONE_QUEUE_LENGTH, sizeof(uint64_t),
                           (uint8_t *)s_diag_tx_done_queue_storage, &s_diag_tx_done_queue_state);
    s_diag_task = xTaskCreateStatic(diag_task, "diag", APP_TASK_STACK_WORDS, NULL,
                                    APP_DIAG_PRIORITY, s_diag_stack, &s_diag_tcb);

    const bool registered = can_io_on_frame(CAN_DB_UDS_REQUEST_ID, false, on_uds_request);

    can_io_on_tx_done(CAN_DB_UDS_RESPONSE_ID, on_uds_response_sent);

    return ok && registered && (s_diag_rx_queue != NULL) && (s_diag_tx_done_queue != NULL) &&
           (s_diag_task != NULL);
}

/* ------------------------------------------------------------------------------ health */

static uint32_t overruns_of(const char *name)
{
    uint32_t overruns = 0U;

    taskENTER_CRITICAL();

    if (strcmp(name, "sensor") == 0) {
        /* The periods it missed: those that overran, and those it skipped after them. */
        overruns = s_sensor.overruns + s_sensor.skipped_periods;
    } else if (strcmp(name, "process") == 0) {
        overruns = s_process.backlog;
    } else if (strcmp(name, "health") == 0) {
        overruns = s_health_overruns;
    } else {
        /* The other tasks have no period of their own to overrun. */
    }

    taskEXIT_CRITICAL();

    return overruns;
}

/* The deepest stack use the platform has seen for this task, or 0 if it does not know it. */
static uint32_t stack_peak_of(const char *name)
{
    static plat_stack_info_t s_stacks[NODE_STATS_MAX_TASKS];
    const size_t count = plat_stack_report(s_stacks, NODE_STATS_MAX_TASKS);
    uint32_t peak = 0U;

    for (size_t i = 0U; i < count; i++) {
        if (strncmp(s_stacks[i].name, name, sizeof(s_stacks[i].name)) == 0) {
            peak = s_stacks[i].peak_bytes;
        }
    }

    return peak;
}

static void send_task_stats(uint32_t index)
{
    const char *const name = s_stats_tasks[index];
    node_task_stats_t task;
    uint64_t total_us = 0U;
    can_frame_t frame = {0};
    bool built = false;

    if (node_stats_task(name, &task, &total_us)) {
        const telemetry_task_t figures = {.index = index,
                                          .priority = task.priority,
                                          .run_time_us = task.run_time_us,
                                          .total_us = total_us,
                                          .stack_peak_bytes = stack_peak_of(name),
                                          .overruns = overruns_of(name)};

        built = telemetry_task_stats(&figures, &frame);
    }

    publish(SLOT_TASK_STATS, built, &frame, plat_time_ns());
}

/* `uptime_s`: the seconds the health task has counted (telemetry_uptime_t), which do not
 * depend on the width of the kernel's tick count. */
static void send_node_info(uint32_t uptime_s)
{
    can_frame_t frame;
    const uint8_t active_slot = s_have_handoff ? s_handoff.slot : (uint8_t)PLAT_HANDOFF_NO_SLOT;
    const bool built = telemetry_node_info((uint8_t)FCN_VERSION_MAJOR, (uint8_t)FCN_VERSION_MINOR,
                                           (uint8_t)FCN_VERSION_PATCH, active_slot,
                                           plat_reset_cause(), uptime_s, &frame);

    publish(SLOT_NODE_INFO, built, &frame, plat_time_ns());
}

/* Health task context: the hand-off and the confirm as BMS_BootInfo. */
static void send_boot_info(void)
{
    can_frame_t frame;
    const uint32_t per_second = 1000U / APP_HEALTH_PERIOD_MS;
    const bool built = telemetry_boot_info(s_have_handoff ? &s_handoff : NULL, s_update.state,
                                           update_healthy_seconds(&s_update, per_second), &frame);

    publish(SLOT_BOOT_INFO, built, &frame, plat_time_ns());
}

static const char *state_name(bms_state_t state)
{
    const char *name = "FAULT";

    if (state == BMS_STATE_STARTUP) {
        name = "STARTUP";
    } else if (state == BMS_STATE_NORMAL) {
        name = "NORMAL";
    } else {
        /* FAULT */
    }

    return name;
}

static void log_alive(uint32_t seconds)
{
    bms_state_t state;
    uint32_t pack_mv;
    int32_t current_ma;
    uint32_t periods;
    bool telemetry_on;
    fault_summary_t faults;

    (void)xSemaphoreTake(s_pack_mutex, portMAX_DELAY);
    state = s_pack.state;
    pack_mv = s_pack.pack_mv;
    current_ma = s_pack.current_ma;
    (void)xSemaphoreGive(s_pack_mutex);

    taskENTER_CRITICAL();
    periods = s_sensor.periods;
    telemetry_on = s_telemetry_on;
    faults = s_fault_summary;
    taskEXIT_CRITICAL();

    LOG_INF(TAG,
            "alive: %lu s, sensor periods %lu, pack %s %lu mV %ld mA, telemetry %s, faults "
            "active 0x%05lX confirmed 0x%05lX",
            (unsigned long)seconds, (unsigned long)periods, state_name(state),
            (unsigned long)pack_mv, (long)current_ma, telemetry_on ? "on" : "off",
            (unsigned long)faults.active, (unsigned long)faults.confirmed);
}

/* The watchdog manager as the statistics show it. Health task context. */
static void capture_supervision(node_supervision_stats_t *out)
{
    *out = s_supervision;
    out->status = wdgm_global_status(&s_wdgm);
    out->cycles = s_wdgm.cycles;
    out->first_expired = wdgm_first_expired(&s_wdgm);
    out->expired_cycles = s_wdgm.expired_cycles;
    out->expired_tolerance = s_supervision_config.expired_tolerance;
    out->timeout_ms = APP_WDG_TIMEOUT_MS;
    out->entity_count = (uint32_t)SUPERVISED_ENTITIES;

    for (uint32_t i = 0U; i < (uint32_t)SUPERVISED_ENTITIES; i++) {
        node_supervised_stats_t *entity = &out->entities[i];

        entity->name = s_supervised[i].name;
        entity->status = wdgm_local_status(&s_wdgm, (uint8_t)i);
        entity->examinations = s_wdgm.entity[i].examinations;
        entity->incorrect = s_wdgm.entity[i].incorrect;
        entity->last_count = s_wdgm.entity[i].last_count;
        entity->expected_alive = s_supervised[i].expected_alive;
        entity->min_margin = s_supervised[i].min_margin;
        entity->max_margin = s_supervised[i].max_margin;
        entity->reference_cycle = s_supervised[i].reference_cycle;
        entity->failed_tolerance = s_supervised[i].failed_tolerance;
    }
}

/* The application's counters into the final statistics. Runs in the health task after it has
 * raised itself above every other task, so that none of them is in the middle of an update. */
static void capture_app(node_stats_t *out)
{
    for (uint32_t i = 0U; i < out->task_count; i++) {
        out->tasks[i].overruns = overruns_of(out->tasks[i].name);
    }

    (void)xSemaphoreTake(s_pack_mutex, portMAX_DELAY);
    out->pack = s_pack;
    (void)xSemaphoreGive(s_pack_mutex);

    /* The health task's own state: no lock. The flash task's statistics: it writes them in
     * critical sections, or, for its own counters, it is below this task. The diagnostic task's:
     * it is above this task, so it is blocked now between two of its calls, not in one. */
    out->faults = s_faults;
    capture_supervision(&out->supervision);
    out->update.have_handoff = s_have_handoff;
    out->update.handoff = s_handoff;
    out->update.monitor = s_update;

    out->diag.core = *diag_stats(&s_diag);
    out->diag.isotp = *diag_isotp_stats(&s_diag);
    diag_uds_stats(&s_diag, &out->diag.uds);
    out->diag.backend = s_diag_backend.stats;
    out->diag.session = diag_session(&s_diag);
    out->diag.events = s_diag_events;
    out->diag.passes = s_diag_passes;
    out->diag.budget_waits = s_diag_budget_waits;

    taskENTER_CRITICAL();
    out->update.flash_task = s_flash_stats;
    out->diag.rx_lost = s_diag_rx_lost;
    out->diag.rx_high_water = s_diag_rx_high_water;
    out->diag.tx_done_lost = s_diag_tx_done_lost;
    out->sensor = s_sensor;
    out->process = s_process;
    out->command = s_command;
    out->telemetry_on = s_telemetry_on;
    out->fault_summary = s_fault_summary;

    for (uint8_t slot = 0U; slot < SLOTS; slot++) {
        node_message_stats_t *message = &out->messages[slot];

        message->name = s_messages[slot].name;
        message->id = s_messages[slot].id;
        message->cycle_ms = s_messages[slot].cycle_ms;
        message->slot = slot;
        message->protected_e2e = s_messages[slot].protected_e2e;
        message->data_id = s_messages[slot].data_id;
        message->gated = s_messages[slot].gated;
        message->published = s_counts[slot].published;
        message->suppressed = s_counts[slot].suppressed;
        message->failed = s_counts[slot].failed;
        message->queue = out->can_slots[slot];
    }

    taskEXIT_CRITICAL();
}

/* The priority stop_node() takes: the highest, which no task has (FreeRTOSConfig.h). */
#define STOP_PRIORITY ((uint32_t)configMAX_PRIORITIES - 1U)
_Static_assert((CAN_IO_RX_PRIORITY < STOP_PRIORITY) && (CAN_IO_TX_PRIORITY < STOP_PRIORITY) &&
                   (APP_SENSOR_PRIORITY < STOP_PRIORITY) &&
                   (APP_PROCESS_PRIORITY < STOP_PRIORITY) && (APP_DIAG_PRIORITY < STOP_PRIORITY) &&
                   (APP_HEALTH_PRIORITY < STOP_PRIORITY) && (APP_FLASH_PRIORITY < STOP_PRIORITY),
               "the stop priority is above every task's");

/*
 * End the run: take the final statistics and end the scheduler. Only the host build ever gets
 * here; firmware on a microcontroller does not stop.
 *
 * The task table is read first, while this task still has its own priority. Then the task
 * raises itself above every other (STOP_PRIORITY), so that nothing runs between reading the
 * counters and the end of the scheduler: what the report says a task did is then all it did,
 * and the counters of the layers below can be checked against it frame for frame. "Above" has
 * to be strictly above: at the priority of another task, time slicing runs that task at the
 * next tick (a receive task that takes frames after its counter was read leaves them out of
 * every count).
 */
static void stop_node(void)
{
    node_stats_capture_tasks(&s_final_stats);
    vTaskPrioritySet(NULL, (UBaseType_t)configMAX_PRIORITIES - 1U); /* STOP_PRIORITY */
    node_stats_capture_can(&s_final_stats);
    capture_app(&s_final_stats);
    LOG_INF(TAG, "stopping after %lu ticks", (unsigned long)xTaskGetTickCount());
    plat_end_scheduler();
}

static void health_task(void *arg)
{
    TickType_t last_wake;
    uint32_t cycles = 0U;
    uint32_t next_task = 0U;
    telemetry_uptime_t uptime = {.seconds = 0U, .periods = 0U};

    (void)arg;
    plat_task_enter("health");
    last_wake = xTaskGetTickCount();

    for (;;) {
        if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_HEALTH_PERIOD_MS)) == pdFALSE) {
            taskENTER_CRITICAL();
            s_health_overruns++;
            taskEXIT_CRITICAL();
        }

        cycles++;

        /* First the supervision and the kick, at the same point of every period. */
        update_health_t confirm;

        supervise();
        log_sample_wait();
        run_faults(&confirm);
        run_update(&confirm);

        if ((cycles % APP_TASK_STATS_EVERY) == 1U) {
            send_task_stats(next_task);
            next_task = (next_task + 1U) % TASK_STATS_TASKS;
        }

        /* Every APP_NODE_INFO_EVERY-th period ends a second (asserted at the top). */
        if (telemetry_uptime_period(&uptime, APP_NODE_INFO_EVERY)) {
            send_node_info(uptime.seconds);
            log_alive(uptime.seconds);
        }

        if ((cycles % APP_NODE_INFO_EVERY) == APP_BOOT_INFO_PHASE) {
            send_boot_info();
        }

        const bool time_is_up = (s_config.run_seconds != 0U) &&
                                (cycles >= (s_config.run_seconds * (1000U / APP_HEALTH_PERIOD_MS)));

        if (time_is_up || plat_shutdown_requested()) {
            stop_node();
        }
    }
}

/* ------------------------------------------------------------------------------ start-up */

/* Before the scheduler: the hand-off, for the log. */
static void log_handoff(void)
{
    const uint32_t version = s_handoff.image_version;

    if (!s_have_handoff) {
        LOG_INF(TAG, "started without the bootloader (no hand-off): nothing to confirm");
    } else {
        if (s_handoff.mode == PLAT_BOOT_TRIAL) {
            LOG_INF(TAG,
                    "started by the bootloader: slot %s as a trial, attempt %u of %u, image "
                    "%u.%u.%u",
                    slot_name(s_handoff.slot), (unsigned int)s_handoff.attempt,
                    (unsigned int)PLAT_HANDOFF_MAX_ATTEMPT, version_major(version),
                    version_minor(version), version_patch(version));
        } else {
            LOG_INF(TAG, "started by the bootloader: slot %s, the confirmed image, image %u.%u.%u",
                    slot_name(s_handoff.slot), version_major(version), version_minor(version),
                    version_patch(version));
        }

        if (update_rolled_back(&s_handoff)) {
            LOG_WRN(TAG, "the bootloader rolled back before this start (%s of slot %s)",
                    update_event_name(s_handoff.event), slot_name(s_handoff.event_slot));
        }
    }
}

void app_start(const app_config_t *config)
{
    const plat_reset_reason_t cause = plat_reset_cause();
    fault_changes_t start_changes;
    bool ok;

    s_config = *config;
    s_have_handoff = plat_boot_handoff(&s_handoff);
    log_handoff();

    bms_init(&s_pack);
    telemetry_init(&s_sender);
    command_init(&s_command);
    s_telemetry_on = command_telemetry_on(&s_command);
    s_clear_requests = command_clear_requests(&s_command);

    /* The DTCs, with the reset faults judged from this start's cause, and their summary, so
     * that the first BMS_PackStatus already says whether one is active. A reset fault is
     * active from the start, not a change: no line for it (the reset cause has one, below). */
    ok = fault_init(&s_faults, cause, update_rolled_back(s_have_handoff ? &s_handoff : NULL));
    ok = ok && update_init(&s_update, s_have_handoff ? &s_handoff : NULL,
                           1000U / APP_HEALTH_PERIOD_MS, s_config.confirm_periods);
    fault_summary(&s_faults, &s_fault_summary, &start_changes);
    ok = ok && (e2e_p11_protect_init(&s_fault_counter) == E2E_E_OK);

    if (cause != PLAT_RESET_POWER_ON) {
        LOG_WRN(TAG, "the last reset was not a power-on: reset cause %u", (unsigned int)cause);
    }

    /* The watchdog manager before any task can report to it, then the hardware watchdog: from
     * here on, a hang before the health task's first kick resets the node too. */
    ok = ok && wdgm_init(&s_wdgm, &s_supervision_config);
    s_supervision.watchdog_started = plat_wdg_start(APP_WDG_TIMEOUT_MS);
    ok = ok && s_supervision.watchdog_started;

    s_sample_queue = xQueueCreateStatic(APP_SAMPLE_QUEUE_LENGTH, sizeof(app_sample_t),
                                        (uint8_t *)s_sample_queue_storage, &s_sample_queue_state);
    s_verdict_queue =
        xQueueCreateStatic(APP_FAULT_VERDICT_QUEUE_LENGTH, sizeof(fault_verdict_t),
                           (uint8_t *)s_verdict_queue_storage, &s_verdict_queue_state);
    s_pack_mutex = xSemaphoreCreateMutexStatic(&s_pack_mutex_state);
    configASSERT(ok && (s_sample_queue != NULL) && (s_verdict_queue != NULL) &&
                 (s_pack_mutex != NULL));

    can_io_start();
    (void)can_io_on_frame(CAN_DB_TESTER_PING_ID, false, on_ping);
    (void)can_io_on_frame(CAN_DB_TESTER_COMMAND_ID, false, on_command);
    can_io_on_rx_cycle(COMMAND_CYCLE_MS, on_command_cycle);
    can_io_on_alive(can_rx_alive, can_tx_alive);

    const bool diag_ok = diag_start();

    configASSERT(diag_ok);

    (void)xTaskCreateStatic(sensor_task, "sensor", APP_TASK_STACK_WORDS, NULL, APP_SENSOR_PRIORITY,
                            s_sensor_stack, &s_sensor_tcb);
    (void)xTaskCreateStatic(process_task, "process", APP_TASK_STACK_WORDS, NULL,
                            APP_PROCESS_PRIORITY, s_process_stack, &s_process_tcb);
    (void)xTaskCreateStatic(health_task, "health", APP_TASK_STACK_WORDS, NULL, APP_HEALTH_PRIORITY,
                            s_health_stack, &s_health_tcb);
    s_flash_task = xTaskCreateStatic(flash_task, "flash", APP_TASK_STACK_WORDS, NULL,
                                     APP_FLASH_PRIORITY, s_flash_stack, &s_flash_tcb);
    configASSERT(s_flash_task != NULL);
}

const node_stats_t *app_final_stats(void)
{
    return &s_final_stats;
}
