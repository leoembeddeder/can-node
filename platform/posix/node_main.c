/* SPDX-License-Identifier: MIT */
/*
 * Host entry point of the node: parses the command line, brings up the POSIX platform layer
 * and the simulated hardware, starts the application and, after an orderly stop, writes the
 * run's statistics as JSON.
 *
 * Everything here runs on the main thread before the scheduler starts or after it has ended,
 * which is why stdio streams are used freely; tasks never write to one.
 */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app.h"
#include "app_tasks.h"
#include "build_id.h"
#include "can_backend.h"
#include "plat_can.h"
#include "posix_flash.h"
#include "posix_platform.h"
#include "posix_retained.h"
#include "posix_sensor.h"
#include "posix_sim.h"
#include "posix_trace.h"

#ifndef FCN_VERSION
#define FCN_VERSION "unknown"
#endif

/* Whether the build has a sanitizer that adds to every stack frame (AddressSanitizer's
 * redzones, ThreadSanitizer's calls into its runtime). The stack figures of such a build are
 * not those of the code, and the statistics say so ("stack_instrumented"). */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define STACK_INSTRUMENTED "true"
#else
#define STACK_INSTRUMENTED "false"
#endif

#define DEFAULT_CAN_SPEC "udp"
#define DEFAULT_BITRATE  500000U
#define MAX_BITRATE      PLAT_CAN_MAX_BITRATE

typedef struct {
    posix_config_t platform;
    app_config_t app;
    const char *stats_path;
    const char *retained_path; /* NULL: no retained cell, every start is a power-on */
    const char *flash_path;    /* NULL: no flash, plat_flash_device() is NULL */
    bool flash_cut_given;      /* --flash-cut: flash_cut is armed for this run */
    posix_flash_cut_t flash_cut;
    uint32_t flash_hang; /* --flash-hang: the program or erase that hangs; 0: none */
    const char *can_spec;
    const char *can_fault;  /* --can-fault: the bus fault in force from the start; NULL: none */
    const char *trace_path; /* --trace-file: the host-only trace (posix_trace.h); NULL: none */
    uint32_t trace_events;
    uint32_t bitrate;
    uint32_t sensor_seed;
    uint16_t sim_port;
} node_options_t;

static void print_usage(const char *program)
{
    (void)printf("usage: %s [options]\n"
                 "  --can SPEC             CAN transport: udp[:<base port>] (default, port %u) or\n"
                 "                         socketcan:<interface>, for example socketcan:vcan0\n"
                 "  --bitrate N            bit rate of the modelled bus in bit/s (default %u);\n"
                 "                         0 switches the bus model's pacing off\n"
                 "  --sim-port N           serve simulation control (fault injection) on UDP\n"
                 "                         port N of the loopback interface (default: off);\n"
                 "                         its can.trace reads a host-only trace of the CAN\n"
                 "                         controller's bus offs and recoveries, with the\n"
                 "                         model's and the host's times (docs/can-simulation.md)\n"
                 "  --can-fault FAULT      a fault of the bus in force from the start, as\n"
                 "                         simulation control's can.fault applies one: none\n"
                 "                         (default), short, dominant or noack\n"
                 "  --sensor-seed N        seed of the sensor model's noise, 0 to 4294967295\n"
                 "                         (default %u); the same seed gives the same readings\n"
                 "  --duration-s N         stop after N seconds of tick time (default and 0: run\n"
                 "                         until SIGINT or SIGTERM)\n"
                 "  --stats-json PATH      write run statistics to PATH on an orderly stop\n"
                 "  --retained-file PATH   the retained-RAM cell: the reset cause survives a\n"
                 "                         reset in PATH, which is created if missing; remove\n"
                 "                         it to simulate a power loss (default: no cell, every\n"
                 "                         start is a power-on)\n"
                 "  --flash-file PATH      the flash (2 MiB and 8 KiB): PATH is created erased if\n"
                 "                         missing, and one of another size is refused (default:\n"
                 "                         no flash)\n"
                 "  --flash-cut N[:SEED]   cut the supply in the N-th flash program or erase of\n"
                 "                         this run (1 to %u), torn as SEED (0 to 4294967295,\n"
                 "                         default 0) says; the run ends as a power loss (12)\n"
                 "  --flash-hang N         make the N-th flash program or erase of this run hang\n"
                 "                         (1 to %u): its busy flag never clears\n"
                 "  --confirm-periods N    a trial confirms itself after N healthy health periods\n"
                 "                         of %u ms in a row, 1 to 4294967295 (default %u, the\n"
                 "                         product's %u s): a test hook the product does not have\n"
                 "  --tick-mode MODE       locked (default) or stock\n"
                 "  --irq-mode MODE        signal (default) or poll\n"
                 "  --log-file PATH        write the log to PATH instead of stderr\n"
                 "  --log-level LEVEL      error, warn, info (default) or debug\n"
                 "  --trace-file PATH      record a host-only trace of the scheduler (task\n"
                 "                         switches, releases, xTaskDelayUntil), of the\n"
                 "                         node's CAN frames and of the sensor reads, on\n"
                 "                         CLOCK_MONOTONIC, and write it to PATH on an orderly\n"
                 "                         stop (platform/posix/posix_trace.h, for\n"
                 "                         bench/node_timing.py). It changes nothing the\n"
                 "                         application does; the statistics say what one\n"
                 "                         event cost (\"trace\")\n"
                 "  --trace-events N       room for N events, 1 to %u (default %u), 32\n"
                 "                         bytes each; events beyond it are counted, not\n"
                 "                         recorded\n"
                 "  --version              print the version and the build id (the commit, and a\n"
                 "                         hash of uncommitted changes) and exit; the build id\n"
                 "                         is in the statistics (\"build_id\") and the log too\n"
                 "  --help                 print this text and exit\n"
                 "exit status: 0 after an orderly stop; 1 on a usage or start-up error; %d when\n"
                 "the CAN transport went away while the node ran (the statistics are written);\n"
                 "%d when an orderly stop hung; 10 to 22 for a reset of the node\n"
                 "(platform/include/platform.h)\n",
                 program, (unsigned int)CAN_UDP_DEFAULT_PORT, (unsigned int)DEFAULT_BITRATE,
                 (unsigned int)POSIX_SENSOR_DEFAULT_SEED, (unsigned int)POSIX_FLASH_CUT_MAX,
                 (unsigned int)POSIX_FLASH_CUT_MAX, (unsigned int)APP_HEALTH_PERIOD_MS,
                 (unsigned int)(UPDATE_CONFIRM_AFTER_S * (1000U / APP_HEALTH_PERIOD_MS)),
                 (unsigned int)UPDATE_CONFIRM_AFTER_S, (unsigned int)POSIX_TRACE_MAX_EVENTS,
                 (unsigned int)POSIX_TRACE_DEFAULT_EVENTS, POSIX_EXIT_CAN_TRANSPORT_LOST,
                 POSIX_EXIT_SHUTDOWN_TIMEOUT);
}

static bool parse_log_level(const char *text, log_level_t *out)
{
    static const struct {
        const char *name;
        log_level_t level;
    } table[] = {{"error", LOG_LEVEL_ERROR},
                 {"warn", LOG_LEVEL_WARN},
                 {"info", LOG_LEVEL_INFO},
                 {"debug", LOG_LEVEL_DEBUG}};
    bool found = false;

    for (size_t i = 0U; i < (sizeof(table) / sizeof(table[0])); i++) {
        if (strcmp(text, table[i].name) == 0) {
            *out = table[i].level;
            found = true;
        }
    }

    return found;
}

/* A decimal number of at most `max`, with nothing before or after it. */
static bool parse_number(const char *text, unsigned long max, unsigned long *out)
{
    char *end = NULL;
    bool ok = false;

    if ((text[0] >= '0') && (text[0] <= '9')) {
        const unsigned long value = strtoul(text, &end, 10);

        if ((*end == '\0') && (value <= max)) {
            *out = value;
            ok = true;
        }
    }

    return ok;
}

/* Returns 0 to continue, 1 to exit successfully (--help, --version), 2 on a usage error. */
static int parse_options(int argc, char **argv, node_options_t *options)
{
    enum {
        OPT_DURATION = 1,
        OPT_STATS,
        OPT_TICK,
        OPT_IRQ,
        OPT_LOG_FILE,
        OPT_LOG_LEVEL,
        OPT_CAN,
        OPT_BITRATE,
        OPT_SIM_PORT,
        OPT_CAN_FAULT,
        OPT_SENSOR_SEED,
        OPT_RETAINED,
        OPT_FLASH,
        OPT_FLASH_CUT,
        OPT_FLASH_HANG,
        OPT_CONFIRM_PERIODS,
        OPT_TRACE_FILE,
        OPT_TRACE_EVENTS,
        OPT_VERSION,
        OPT_HELP
    };
    static const struct option long_options[] = {
        {"duration-s", required_argument, NULL, OPT_DURATION},
        {"stats-json", required_argument, NULL, OPT_STATS},
        {"tick-mode", required_argument, NULL, OPT_TICK},
        {"irq-mode", required_argument, NULL, OPT_IRQ},
        {"log-file", required_argument, NULL, OPT_LOG_FILE},
        {"log-level", required_argument, NULL, OPT_LOG_LEVEL},
        {"can", required_argument, NULL, OPT_CAN},
        {"bitrate", required_argument, NULL, OPT_BITRATE},
        {"sim-port", required_argument, NULL, OPT_SIM_PORT},
        {"can-fault", required_argument, NULL, OPT_CAN_FAULT},
        {"sensor-seed", required_argument, NULL, OPT_SENSOR_SEED},
        {"retained-file", required_argument, NULL, OPT_RETAINED},
        {"flash-file", required_argument, NULL, OPT_FLASH},
        {"flash-cut", required_argument, NULL, OPT_FLASH_CUT},
        {"flash-hang", required_argument, NULL, OPT_FLASH_HANG},
        {"confirm-periods", required_argument, NULL, OPT_CONFIRM_PERIODS},
        {"trace-file", required_argument, NULL, OPT_TRACE_FILE},
        {"trace-events", required_argument, NULL, OPT_TRACE_EVENTS},
        {"version", no_argument, NULL, OPT_VERSION},
        {"help", no_argument, NULL, OPT_HELP},
        {NULL, 0, NULL, 0}};
    int result = 0;
    int option;

    while ((result == 0) && ((option = getopt_long(argc, argv, "", long_options, NULL)) != -1)) {
        unsigned long number = 0UL;

        switch (option) {
        case OPT_DURATION:
            if (parse_number(optarg, 86400UL, &number)) {
                options->app.run_seconds = (uint32_t)number;
            } else {
                (void)fprintf(stderr, "invalid --duration-s: %s\n", optarg);
                result = 2;
            }
            break;
        case OPT_STATS:
            options->stats_path = optarg;
            break;
        case OPT_TICK:
            if (strcmp(optarg, "locked") == 0) {
                options->platform.tick_mode = POSIX_TICK_LOCKED;
            } else if (strcmp(optarg, "stock") == 0) {
                options->platform.tick_mode = POSIX_TICK_STOCK;
            } else {
                (void)fprintf(stderr, "invalid --tick-mode: %s\n", optarg);
                result = 2;
            }
            break;
        case OPT_IRQ:
            if (strcmp(optarg, "signal") == 0) {
                options->platform.irq_mode = POSIX_IRQ_SIGNAL;
            } else if (strcmp(optarg, "poll") == 0) {
                options->platform.irq_mode = POSIX_IRQ_POLL;
            } else {
                (void)fprintf(stderr, "invalid --irq-mode: %s\n", optarg);
                result = 2;
            }
            break;
        case OPT_LOG_FILE:
            options->platform.log_path = optarg;
            break;
        case OPT_LOG_LEVEL:
            if (!parse_log_level(optarg, &options->platform.log_level)) {
                (void)fprintf(stderr, "invalid --log-level: %s\n", optarg);
                result = 2;
            }
            break;
        case OPT_CAN:
            options->can_spec = optarg;
            break;
        case OPT_BITRATE:
            if (parse_number(optarg, MAX_BITRATE, &number)) {
                options->bitrate = (uint32_t)number;
            } else {
                (void)fprintf(stderr, "invalid --bitrate: %s (0 to %u)\n", optarg,
                              (unsigned int)MAX_BITRATE);
                result = 2;
            }
            break;
        case OPT_SIM_PORT:
            if (parse_number(optarg, 65535UL, &number)) {
                options->sim_port = (uint16_t)number;
            } else {
                (void)fprintf(stderr, "invalid --sim-port: %s\n", optarg);
                result = 2;
            }
            break;
        case OPT_CAN_FAULT:
            if (posix_can_set_start_fault(optarg)) {
                options->can_fault = optarg;
            } else {
                (void)fprintf(stderr, "invalid --can-fault: %s (none, short, dominant or noack)\n",
                              optarg);
                result = 2;
            }
            break;
        case OPT_SENSOR_SEED:
            if (parse_number(optarg, 4294967295UL, &number)) {
                options->sensor_seed = (uint32_t)number;
            } else {
                (void)fprintf(stderr, "invalid --sensor-seed: %s (0 to 4294967295)\n", optarg);
                result = 2;
            }
            break;
        case OPT_RETAINED:
            options->retained_path = optarg;
            break;
        case OPT_FLASH:
            options->flash_path = optarg;
            break;
        case OPT_FLASH_CUT: {
            char message[200];

            if (posix_flash_parse_cut(optarg, &options->flash_cut, message, sizeof(message))) {
                options->flash_cut_given = true;
            } else {
                (void)fprintf(stderr, "invalid --flash-cut: %s\n", message);
                result = 2;
            }
            break;
        }
        case OPT_FLASH_HANG:
            if (parse_number(optarg, POSIX_FLASH_CUT_MAX, &number) && (number != 0UL)) {
                options->flash_hang = (uint32_t)number;
            } else {
                (void)fprintf(stderr, "invalid --flash-hang: %s (1 to %u)\n", optarg,
                              (unsigned int)POSIX_FLASH_CUT_MAX);
                result = 2;
            }
            break;
        case OPT_CONFIRM_PERIODS:
            if (parse_number(optarg, 4294967295UL, &number) && (number != 0UL)) {
                options->app.confirm_periods = (uint32_t)number;
            } else {
                (void)fprintf(stderr, "invalid --confirm-periods: %s (1 to 4294967295)\n", optarg);
                result = 2;
            }
            break;
        case OPT_TRACE_FILE:
            options->trace_path = optarg;
            break;
        case OPT_TRACE_EVENTS:
            if (parse_number(optarg, POSIX_TRACE_MAX_EVENTS, &number) && (number != 0UL)) {
                options->trace_events = (uint32_t)number;
            } else {
                (void)fprintf(stderr, "invalid --trace-events: %s (1 to %u)\n", optarg,
                              (unsigned int)POSIX_TRACE_MAX_EVENTS);
                result = 2;
            }
            break;
        case OPT_VERSION:
            (void)printf("fcn_node %s (FreeRTOS %s) build %s\n", FCN_VERSION,
                         tskKERNEL_VERSION_NUMBER, fcn_build_id);
            result = 1;
            break;
        case OPT_HELP:
            print_usage(argv[0]);
            result = 1;
            break;
        default:
            print_usage(argv[0]);
            result = 2;
            break;
        }
    }

    if ((result == 0) && (optind < argc)) {
        (void)fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
        result = 2;
    }

    return result;
}

/* One "key": value member of a JSON object; `separator` is what follows it. */
static void put_u64(FILE *file, const char *key, uint64_t value, const char *separator)
{
    (void)fprintf(file, "\"%s\": %llu%s", key, (unsigned long long)value, separator);
}

static void put_i64(FILE *file, const char *key, int64_t value, const char *separator)
{
    (void)fprintf(file, "\"%s\": %lld%s", key, (long long)value, separator);
}

static void put_bool(FILE *file, const char *key, bool value, const char *separator)
{
    (void)fprintf(file, "\"%s\": %s%s", key, value ? "true" : "false", separator);
}

static const char *can_state_name(uint8_t state)
{
    static const char *const names[] = {"active", "passive", "busoff"};

    return (state <= PLAT_CAN_BUS_OFF) ? names[state] : "unknown";
}

/*
 * The "can" member. The layers that report here are sampled at two moments:
 *
 *  - by the health task, just before it ended the scheduler: "queue", and in "driver" the
 *    counters of the CAN tasks (rx_frames, rx_unhandled, rx_batches_cut, tx_load_failed);
 *  - after the hardware thread has stopped: "controller", "transport_counters", "pacing",
 *    and in "driver" the counters of the controller's FIFOs (rx_fifo_overruns,
 *    rx_fifo_pending, event_fifo_overruns, state_events_merged).
 *
 * In between, no task ran, but the hardware thread did. A frame transmitted then is counted
 * by the controller and not by the queue; a frame received then went into the receive FIFO
 * (rx_fifo_pending) or, with the FIFO full, was lost there (rx_fifo_overruns). So for the
 * receive path the counts agree exactly:
 *     controller.rx_frames == driver.rx_frames + driver.rx_fifo_overruns
 *                             + driver.rx_fifo_pending
 */
static void write_can_stats(FILE *file, const node_stats_t *stats)
{
    posix_can_report_t report;
    posix_can_stats_t host;

    posix_can_get_report(&report);
    posix_can_get_stats(&host);

    (void)fprintf(file, "  \"can\": {\n    \"transport\": \"%s\", \"bitrate\": %u, ",
                  report.transport, (unsigned int)report.bitrate);
    put_u64(file, "elapsed_ns", report.elapsed_ns, ",\n");
    (void)fprintf(file, "    \"state\": \"%s\", \"tec\": %u, \"rec\": %u,\n",
                  can_state_name(report.state), (unsigned int)report.tec, (unsigned int)report.rec);

    /* Why the transport ended the run, or null. The message is strerror()'s, which contains
     * no character that would need escaping in a JSON string. */
    if (report.transport_error != 0) {
        (void)fprintf(file, "    \"transport_error\": \"%s\",\n", strerror(report.transport_error));
    } else {
        (void)fprintf(file, "    \"transport_error\": null,\n");
    }

    (void)fprintf(file, "    \"controller\": {");
    put_u64(file, "tx_frames", report.tx_frames, ", ");
    put_u64(file, "tx_attempts_failed", report.tx_attempts_failed, ", ");
    put_u64(file, "tx_aborted", report.tx_aborted, ", ");
    put_u64(file, "rx_frames", report.rx_frames, ", ");
    put_u64(file, "rx_errors", report.rx_errors, ", ");
    put_u64(file, "rx_lost_fault", report.rx_lost_fault, ", ");
    put_u64(file, "wire_overruns", report.wire_overruns, ", ");
    put_u64(file, "bus_off_events", report.bus_off_events, ", ");
    put_u64(file, "late_resyncs", report.late_resyncs, ", ");
    put_u64(file, "busy_ns", report.busy_ns, "},\n");

    (void)fprintf(file, "    \"transport_counters\": {");
    put_u64(file, "tx_frames", report.transport_tx_frames, ", ");
    put_u64(file, "tx_failed", report.transport_tx_failed, ", ");
    put_u64(file, "rx_frames", report.transport_rx_frames, ", ");
    put_u64(file, "rx_malformed", report.transport_rx_malformed, ", ");
    put_u64(file, "rx_overflow", report.transport_rx_overflow, ", ");
    put_u64(file, "rx_failed", report.transport_rx_failed, "},\n");

    /* How late the host carried out the model's times; see posix_can_stats_t. */
    (void)fprintf(file, "    \"pacing\": {");
    put_u64(file, "frames", host.frames_paced, ", ");
    put_u64(file, "lag_max_ns", host.pacing_lag_max_ns, ", ");
    put_u64(file, "lag_mean_ns", host.pacing_lag_mean_ns, ", ");
    put_u64(file, "wakeups", host.wakeups, ", ");
    put_u64(file, "wakeup_lag_max_ns", host.wakeup_lag_max_ns, ", ");
    put_u64(file, "wakeup_lag_mean_ns", host.wakeup_lag_mean_ns, ", ");
    put_u64(file, "rx_frames", host.rx_delivered, ", ");
    put_u64(file, "rx_lag_max_ns", host.rx_lag_max_ns, ", ");
    put_u64(file, "rx_lag_mean_ns", host.rx_lag_mean_ns, "},\n");

    (void)fprintf(file, "    \"driver\": {");
    put_u64(file, "rx_frames", stats->can_io.rx_frames, ", ");
    put_u64(file, "rx_unhandled", stats->can_io.rx_unhandled, ", ");
    put_u64(file, "rx_batches_cut", stats->can_io.rx_batches_cut, ", ");
    put_u64(file, "tx_load_failed", stats->can_io.tx_load_failed, ", ");
    put_u64(file, "rx_cycles", stats->can_io.rx_cycles, ", ");
    put_u64(file, "rx_cycles_skipped", stats->can_io.rx_cycles_skipped, ", ");
    put_u64(file, "error_passive_entries", stats->can_io.error_passive_entries, ", ");
    put_u64(file, "tx_done_reports", stats->can_io.tx_done_reports, ", ");
    put_u64(file, "tx_quiet_enqueues", stats->can_io.tx_quiet_enqueues, ", ");
    put_u64(file, "rx_fifo_overruns", host.rx_fifo_overruns, ", ");
    put_u64(file, "event_fifo_overruns", host.event_fifo_overruns, ", ");
    put_u64(file, "state_events_merged", host.state_events_merged, ", ");
    put_u64(file, "rx_fifo_pending", host.rx_fifo_pending, "},\n");

    (void)fprintf(file, "    \"queue\": {");
    put_u64(file, "produced", stats->can_queue.produced, ", ");
    put_u64(file, "sent", stats->can_queue.sent, ", ");
    put_u64(file, "superseded", stats->can_queue.superseded, ", ");
    put_u64(file, "dropped", stats->can_queue.dropped, ", ");
    put_u64(file, "expired", stats->can_queue.expired, ", ");
    put_u64(file, "pending", stats->can_queue.pending, ", ");
    put_u64(file, "bus_off_count", stats->can_queue.bus_off_count, ", ");
    put_u64(file, "restart_requests", stats->can_queue.restart_requests, ", ");
    put_u64(file, "recall_requests", stats->can_queue.recall_requests, ", ");
    put_u64(file, "holdoff_ms", stats->can_queue.holdoff_ms, "}\n  },\n");
}

/*
 * "task_table": the task table of the node (app/task_table.c) in the input schema of the
 * schedulability check, so that `python -m fcn_tools.sched` can read the member as it is. The
 * strings of the table contain no character that would need escaping.
 */
static void write_task_table(FILE *file)
{
    size_t count = 0U;
    const app_task_budget_t *table = app_task_table(&count);

    (void)fprintf(file,
                  "  \"task_table\": {\n    \"schema\": \"fcn.task_table/1\",\n"
                  "    \"wcet_basis\": \"design\",\n    \"description\": \"%s\",\n"
                  "    \"tasks\": [",
                  app_task_table_description());

    for (size_t i = 0U; i < count; i++) {
        const app_task_budget_t *task = &table[i];

        (void)fprintf(file, "%s\n      {\"name\": \"%s\", \"priority\": %u, \"kind\": \"%s\", ",
                      (i == 0U) ? "" : ",", task->name, (unsigned int)task->priority,
                      task->periodic ? "periodic" : "sporadic");
        put_u64(file, task->periodic ? "period_us" : "min_interarrival_us", task->period_us, ", ");
        put_u64(file, "deadline_us", task->deadline_us, ", ");
        put_u64(file, "wcet_us", task->wcet_us, ", ");
        put_u64(file, "jitter_us", task->jitter_us, ", ");
        put_u64(file, "nonpreemptive_us", task->nonpreemptive_us, ", ");
        (void)fprintf(file, "\"critical_sections\": [");

        if (task->mutex != NULL) {
            (void)fprintf(file, "{\"resource\": \"%s\", \"length_us\": %u}", task->mutex,
                          (unsigned int)task->mutex_us);
        }

        (void)fprintf(file, "]}");
    }

    (void)fprintf(file, "\n    ]\n  },\n");
}

static const char *pack_state_name(bms_state_t state)
{
    static const char *const names[] = {"STARTUP", "NORMAL", "FAULT"};

    return ((uint32_t)state <= (uint32_t)BMS_STATE_FAULT) ? names[state] : "unknown";
}

static const char *sm_state_name(e2e_sm_state_t state)
{
    static const char *const names[] = {"DEINIT", "NODATA", "INIT", "VALID", "INVALID"};

    return ((uint32_t)state <= (uint32_t)E2E_SM_INVALID) ? names[state] : "unknown";
}

/* "sensor", "sample_queue", "process" and "pack": the measurement chain, as the health task
 * captured it when it ended the run. */
static void write_measurement_stats(FILE *file, const node_stats_t *stats)
{
    const node_sensor_stats_t *sensor = &stats->sensor;
    const node_process_stats_t *process = &stats->process;
    const bms_pack_t *pack = &stats->pack;

    (void)fprintf(file, "  \"sensor\": {");
    put_u64(file, "periods", sensor->periods, ", ");
    put_u64(file, "overruns", sensor->overruns, ", ");
    put_u64(file, "skipped_periods", sensor->skipped_periods, ", ");
    (void)fprintf(file, "\"reads\": {");
    put_u64(file, "current", sensor->current_reads, ", ");
    put_u64(file, "cells", sensor->cell_reads, ", ");
    put_u64(file, "temps", sensor->temp_reads, "}, ");
    (void)fprintf(file, "\"unanswered\": {");
    put_u64(file, "current", sensor->current_unanswered, ", ");
    put_u64(file, "cells", sensor->cell_unanswered, ", ");
    put_u64(file, "temps", sensor->temp_unanswered, "}},\n");

    (void)fprintf(file, "  \"sample_queue\": {");
    put_u64(file, "length", APP_SAMPLE_QUEUE_LENGTH, ", ");
    put_u64(file, "posted", sensor->samples_posted, ", ");
    put_u64(file, "full", sensor->queue_full, ", ");
    put_u64(file, "high_water", process->high_water, "},\n");

    (void)fprintf(file, "  \"process\": {");
    put_u64(file, "samples", process->samples, ", ");
    put_u64(file, "timeouts", process->timeouts, ", ");
    put_u64(file, "recoveries", process->recoveries, ", ");
    put_u64(file, "backlog", process->backlog, ", ");
    put_u64(file, "verdicts_posted", process->verdicts_posted, ", ");
    put_u64(file, "verdicts_lost", process->verdicts_lost, "},\n");

    (void)fprintf(file, "  \"pack\": {\"state\": \"%s\", ", pack_state_name(pack->state));
    put_u64(file, "updates", pack->updates, ", ");
    put_i64(file, "current_ma", pack->current_ma, ", ");
    put_bool(file, "current_valid", pack->current_valid, ", ");
    put_u64(file, "pack_mv", pack->pack_mv, ", ");
    put_bool(file, "pack_valid", pack->pack_valid, ",\n    ");
    (void)fprintf(file, "\"cell_mv\": [");
    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        (void)fprintf(file, "%s%u", (i == 0U) ? "" : ", ", (unsigned int)pack->cell_mv[i]);
    }
    (void)fprintf(file, "], ");
    put_u64(file, "cell_valid", pack->cell_valid, ", ");
    (void)fprintf(file, "\"temp_dc\": [");
    for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
        (void)fprintf(file, "%s%d", (i == 0U) ? "" : ", ", (int)pack->temp_dc[i]);
    }
    (void)fprintf(file, "], ");
    put_u64(file, "temp_valid", pack->temp_valid, ",\n    ");
    put_u64(file, "min_cell_mv", pack->min_cell_mv, ", ");
    put_u64(file, "min_cell", pack->min_cell, ", ");
    put_u64(file, "max_cell_mv", pack->max_cell_mv, ", ");
    put_u64(file, "max_cell", pack->max_cell, ",\n    ");
    put_u64(file, "flagged_readings", pack->flagged_readings, ", ");
    put_u64(file, "implausible_readings", pack->implausible_readings, ", ");
    put_u64(file, "lost_samples", pack->lost_samples, ", ");
    put_u64(file, "sequence_errors", pack->sequence_errors, "},\n");
}

/* "telemetry": the switch, and per cyclic message what the application did with it and what
 * its slot of the transmit queue did (the queue's counters were captured with "can.queue"). */
static void write_telemetry_stats(FILE *file, const node_stats_t *stats)
{
    (void)fprintf(file, "  \"telemetry\": {");
    put_bool(file, "enabled", stats->telemetry_on, ", ");
    (void)fprintf(file, "\"messages\": {");

    for (uint32_t i = 0U; i < NODE_STATS_MESSAGES; i++) {
        const node_message_stats_t *message = &stats->messages[i];

        (void)fprintf(file, "%s\n    \"%s\": {", (i == 0U) ? "" : ",",
                      (message->name != NULL) ? message->name : "unknown");
        put_u64(file, "id", message->id, ", ");
        put_u64(file, "slot", message->slot, ", ");
        put_u64(file, "cycle_ms", message->cycle_ms, ", ");

        if (message->protected_e2e) {
            put_u64(file, "e2e_data_id", message->data_id, ", ");
        } else {
            (void)fprintf(file, "\"e2e_data_id\": null, ");
        }

        put_bool(file, "gated", message->gated, ", ");
        put_u64(file, "published", message->published, ", ");
        put_u64(file, "suppressed", message->suppressed, ", ");
        put_u64(file, "failed", message->failed, ",\n      ");
        (void)fprintf(file, "\"queue\": {");
        put_u64(file, "produced", message->queue.produced, ", ");
        put_u64(file, "sent", message->queue.sent, ", ");
        put_u64(file, "superseded", message->queue.superseded, ", ");
        put_u64(file, "pending", message->queue.pending, "}}");
    }

    (void)fprintf(file, "\n  }},\n");
}

/*
 * "reset" and "hw_watchdog": what the platform knows about why this run started (the
 * retained cell, plat_retained.h) and what the hardware watchdog model counted (plat_wdg.h).
 * The verdict is one of the fixed words of posix_retained_info_t, which need no escaping.
 */
static void write_platform_reset_stats(FILE *file)
{
    posix_retained_info_t retained;
    posix_wdg_stats_t wdg;

    posix_retained_get_info(&retained);
    posix_wdg_get_stats(&wdg);

    (void)fprintf(file, "  \"reset\": {\"cause\": \"%s\", ",
                  posix_reset_reason_name(retained.cause));
    put_u64(file, "cause_code", (uint64_t)retained.cause, ", ");
    put_bool(file, "retained_cell", retained.open, ", ");
    (void)fprintf(file, "\"verdict\": \"%s\", ", retained.verdict);
    put_u64(file, "resets_since_power_on", retained.resets, "},\n");

    (void)fprintf(file, "  \"hw_watchdog\": {");
    put_bool(file, "started", wdg.started, ", ");
    put_u64(file, "timeout_ms", wdg.timeout_ms, ", ");
    put_u64(file, "kicks", wdg.kicks, ", ");
    put_u64(file, "longest_unkicked_ns", wdg.longest_unkicked_ns, ", ");
    put_u64(file, "gaps", wdg.gaps, ", ");
    put_u64(file, "longest_gap_ns", wdg.longest_gap_ns, "},\n");
}

/* "flash": what the simulated flash counted (posix_flash.h; the same figures as the simulation
 * command flash.status). "file" is false for a run without --flash-file; every count is 0
 * then. A cut that fired ends the run, so a run that writes statistics shows "none" or
 * "armed". */
static void write_flash_stats(FILE *file, const node_options_t *options)
{
    posix_flash_info_t flash;

    posix_flash_get_info(&flash);
    (void)fprintf(file, "  \"flash\": {");
    put_bool(file, "file", (options->flash_path != NULL), ", ");
    put_bool(file, "created", flash.created, ", ");
    put_u64(file, "reads", flash.reads, ", ");
    put_u64(file, "bytes_read", flash.bytes_read, ", ");
    put_u64(file, "programs", flash.programs, ", ");
    put_u64(file, "units_programmed", flash.units_programmed, ", ");
    put_u64(file, "erases", flash.erases, ", ");
    put_u64(file, "refused", flash.refused, ",\n    ");
    put_u64(file, "busy_ns", flash.busy_ns, ", ");
    put_u64(file, "erases_meta", flash.erases_meta, ", ");
    put_u64(file, "erases_slot_a", flash.erases_slot[0], ", ");
    put_u64(file, "erases_slot_b", flash.erases_slot[1], ", ");
    put_u64(file, "max_sector_erases", flash.max_sector_erases, ", ");

    const char *cut = "none";

    if (flash.cut_fired) {
        cut = "fired";
    } else if (flash.cut_in != 0U) {
        cut = "armed";
    } else {
        /* none armed */
    }

    (void)fprintf(file, "\"cut\": \"%s\", ", cut);
    put_u64(file, "cut_in", flash.cut_in, ", ");
    put_u64(file, "cut_seed", flash.cut_seed, "},\n");
}

/* A slot index as a JSON value: "A", "B", or null for any other. */
static const char *slot_name(uint8_t slot)
{
    static const char *const names[] = {"\"A\"", "\"B\""};

    return (slot < 2U) ? names[slot] : "null";
}

/* An image version (lib/image: major.minor.patch) as "1.2.3" and its number. */
static void put_version(FILE *file, const char *key, uint32_t version, const char *separator)
{
    (void)fprintf(file, "\"%s\": \"%u.%u.%u\", \"%s_code\": %lu%s", key,
                  (unsigned int)(version >> 24U), (unsigned int)((version >> 16U) & 0xFFU),
                  (unsigned int)(version & 0xFFFFU), key, (unsigned long)version, separator);
}

/*
 * "handoff" and "confirm": what the bootloader handed over (plat_retained.h, THE HAND-OFF; the
 * verdict is posix_retained_info_t's) and the confirm of a trial (app/update.h) with the flash
 * task that makes it (app.c, THE UPDATE). Without a hand-off "present" is false and the slot,
 * mode and version are null. The names are fixed words, which need no escaping.
 */
static void write_update_stats(FILE *file, const node_stats_t *stats)
{
    const node_update_stats_t *update = &stats->update;
    const plat_boot_handoff_t *h = &update->handoff;
    const update_monitor_t *m = &update->monitor;
    const node_flash_task_stats_t *f = &update->flash_task;
    posix_retained_info_t retained;

    posix_retained_get_info(&retained);
    (void)fprintf(file, "  \"handoff\": {\"verdict\": \"%s\", ", retained.handoff_verdict);
    put_bool(file, "present", update->have_handoff, ", ");

    if (update->have_handoff) {
        (void)fprintf(file, "\"slot\": %s, \"mode\": \"%s\", ", slot_name(h->slot),
                      (h->mode == PLAT_BOOT_TRIAL) ? "trial" : "confirmed");
        put_u64(file, "attempt", h->attempt, ", ");
        put_version(file, "image_version", h->image_version, ", ");
        (void)fprintf(file, "\"event\": \"%s\", \"event_slot\": %s, ", update_event_name(h->event),
                      slot_name(h->event_slot));
        put_u64(file, "select_write_error", h->select_write_error, ", ");
    } else {
        (void)fprintf(file, "\"slot\": null, \"mode\": null, \"attempt\": 0, "
                            "\"image_version\": null, \"image_version_code\": null, "
                            "\"event\": \"NONE\", \"event_slot\": null, "
                            "\"select_write_error\": 0, ");
    }

    put_bool(file, "rolled_back", update_rolled_back(update->have_handoff ? h : NULL), "},\n");

    (void)fprintf(file, "  \"confirm\": {\"state\": \"%s\", ", update_confirm_state_name(m->state));
    put_u64(file, "confirm_after_s", UPDATE_CONFIRM_AFTER_S, ", ");
    put_u64(file, "periods_needed", m->periods_needed, ", ");
    put_u64(file, "periods", m->periods, ", ");
    put_u64(file, "healthy_periods", m->healthy_periods, ", ");
    put_u64(file, "window_restarts", m->window_restarts, ", ");
    put_u64(file, "requested_period", m->requested_period, ", ");
    put_u64(file, "unsupervised_periods", m->unsupervised_periods, ", ");
    put_u64(file, "not_active_periods", m->not_active_periods, ", ");
    put_u64(file, "bus_off_periods", m->bus_off_periods, ", ");
    put_u64(file, "unacked_periods", m->unacked_periods, ", ");
    put_u64(file, "window_acked", m->window_acked, ", ");
    put_u64(file, "fewest_acked", m->fewest_acked, ", ");
    (void)fprintf(file, "\"status\": \"%s\",\n    \"flash_task\": {",
                  boot_status_name(m->confirm_status));
    put_bool(file, "flash", f->flash, ", ");
    (void)fprintf(file, "\"boot_init\": \"%s\", ", boot_status_name(f->boot_init_status));
    put_bool(file, "described", f->described, ", ");

    if (f->described) {
        put_bool(file, "have_record", f->have_record, ", ");
        put_u64(file, "seq", f->seq, ", ");
        (void)fprintf(file, "\"active_slot\": %s, \"trial_slot\": %s, ", slot_name(f->active_slot),
                      slot_name(f->trial_slot));
        put_u64(file, "attempts_used", f->attempts_used, ", ");
        put_u64(file, "records_valid", f->records_valid, ", ");
    }

    put_u64(file, "confirms", f->confirms, ", ");
    (void)fprintf(file, "\"confirm_status\": \"%s\", ", boot_status_name(f->confirm_status));
    put_u64(file, "confirm_ns", f->confirm_ns, ", ");
    put_bool(file, "confirmed_by_routine", f->confirmed_by_routine, ", ");
    put_u64(file, "self_skipped", f->self_skipped, ", ");
    put_u64(file, "idle_passes", f->idle_passes, ",\n      \"jobs\": {");

    /* Phase 5: the flash jobs of the diagnostic task, by kind (app/flash_job.h). */
    for (uint32_t kind = 1U; kind < (uint32_t)FLASH_JOB_KINDS; kind++) {
        (void)fprintf(file, "%s\"%s\": {", (kind == 1U) ? "" : ", ",
                      flash_job_kind_name((flash_job_kind_t)kind));
        put_u64(file, "run", f->jobs[kind], ", ");
        (void)fprintf(file, "\"last_status\": \"%s\", ", boot_status_name(f->job_status[kind]));
        put_u64(file, "last_steps", f->job_steps[kind], ", ");
        put_u64(file, "last_ns", f->job_last_ns[kind], ", ");
        put_u64(file, "max_ns", f->job_max_ns[kind], ", ");
        /* Phase 6: their sum, for the mean time of a job (bench/isotp_throughput.py). */
        put_u64(file, "total_ns", f->job_total_ns[kind], "}");
    }

    (void)fprintf(file, "}}},\n");
}

/* One object of counters indexed by an enumeration, keyed by the names `name` gives. */
static void put_named_counts(FILE *file, const char *key, const uint32_t *counts, uint32_t n,
                             const char *(*name)(uint32_t), const char *separator)
{
    (void)fprintf(file, "\"%s\": {", key);

    for (uint32_t i = 0U; i < n; i++) {
        (void)fprintf(file, "%s\"%s\": %lu", (i == 0U) ? "" : ", ", name(i),
                      (unsigned long)counts[i]);
    }

    (void)fprintf(file, "}%s", separator);
}

static const char *result_name_of(uint32_t i)
{
    return isotp_result_name((isotp_result_t)i);
}

static const char *frame_kind_name_of(uint32_t i)
{
    return isotp_frame_kind_name((isotp_frame_kind_t)i);
}

static const char *ignored_name_of(uint32_t i)
{
    return isotp_ignored_name((isotp_ignored_t)i);
}

static const char *service_name_of(uint32_t i)
{
    return uds_service_name(uds_service_sid(i));
}

static const char *nrc_name_of(uint32_t i)
{
    return uds_nrc_name(uds_nrc_code(i));
}

/*
 * "diag" (phase 5): the diagnostic connection (app/diag.h) with its parameters, lib/isotp's and
 * lib/uds's counters, the UDS backend's (app/diag_backend.h), and the diagnostic task's and its
 * queues' (app/app.c, THE DIAGNOSTIC TASK). Every name is a fixed word, which needs no escaping.
 */
static void write_diag_stats(FILE *file, const node_stats_t *stats)
{
    const node_diag_stats_t *d = &stats->diag;
    const diag_stats_t *c = &d->core;
    const isotp_stats_t *t = &d->isotp;
    const uds_stats_t *u = &d->uds;
    const uds_download_stats_t *dl = &u->download;
    const diag_backend_stats_t *b = &d->backend;

    (void)fprintf(file, "  \"diag\": {\"session\": \"%s\",\n    \"task\": {",
                  uds_session_name(d->session));
    put_u64(file, "events", d->events, ", ");
    put_u64(file, "passes", d->passes, ", ");
    put_u64(file, "budget_waits", d->budget_waits, ", ");
    put_u64(file, "events_per_tick", APP_DIAG_EVENTS_PER_TICK, ", ");
    put_u64(file, "rx_queue_length", APP_DIAG_RX_QUEUE_LENGTH, ", ");
    put_u64(file, "rx_lost", d->rx_lost, ", ");
    put_u64(file, "rx_high_water", d->rx_high_water, ", ");
    put_u64(file, "tx_done_queue_length", APP_DIAG_TX_DONE_QUEUE_LENGTH, ", ");
    put_u64(file, "tx_done_lost", d->tx_done_lost, "},\n    \"connection\": {");
    put_u64(file, "frames_in", c->frames_in, ", ");
    put_u64(file, "frames_out", c->frames_out, ", ");
    put_u64(file, "frames_refused", c->frames_refused, ", ");
    put_u64(file, "confirms", c->confirms, ", ");
    put_u64(file, "confirms_passed", c->confirms_passed, ", ");
    put_u64(file, "confirms_late", c->confirms_late, ", ");
    put_u64(file, "confirms_unknown", c->confirms_unknown, ", ");
    put_u64(file, "confirms_forgotten", c->confirms_forgotten, ", ");
    put_u64(file, "requests", c->requests, ", ");
    put_u64(file, "responses", c->responses, ", ");
    put_u64(file, "responses_sent", c->responses_sent, ", ");
    put_u64(file, "responses_failed", c->responses_failed, ", ");
    put_u64(file, "responses_dropped", c->responses_dropped, ", ");
    put_u64(file, "resets", c->resets, "},\n    \"isotp\": {");
    (void)fprintf(file, "\"parameters\": {");
    put_u64(file, "tx_id", DIAG_RESPONSE_ID, ", ");
    put_u64(file, "rx_id", DIAG_REQUEST_ID, ", ");
    put_u64(file, "bs", DIAG_BLOCK_SIZE, ", ");
    put_u64(file, "stmin", DIAG_STMIN, ", ");
    put_u64(file, "rx_buffer", DIAG_RX_BUFFER, ", ");
    put_u64(file, "timeout_us", DIAG_TIMEOUT_US, ", ");
    put_u64(file, "n_br_us", 0U, ", ");
    put_u64(file, "n_cs_us", DIAG_N_CS_US, ", ");
    put_u64(file, "rx_wft_max", 0U, ", ");
    put_u64(file, "tx_wft_max", DIAG_TX_WFT_MAX, ", ");
    put_u64(file, "pipeline", ISOTP_PIPELINE_DEPTH, "},\n      ");
    put_u64(file, "tx_requests", t->tx_requests, ", ");
    put_named_counts(file, "tx_results", t->tx_results, ISOTP_RESULTS, result_name_of, ", ");
    put_u64(file, "ff_indications", t->ff_indications, ",\n      ");
    put_named_counts(file, "rx_results", t->rx_results, ISOTP_RESULTS, result_name_of, ",\n      ");
    put_named_counts(file, "frames_out", t->frames_out, ISOTP_FRAME_KINDS, frame_kind_name_of,
                     ",\n      ");
    put_named_counts(file, "frames_in", t->frames_in, ISOTP_FRAME_KINDS, frame_kind_name_of,
                     ",\n      ");
    put_named_counts(file, "ignored", t->ignored, ISOTP_IGNORED_REASONS, ignored_name_of,
                     ",\n      ");
    put_u64(file, "rx_overflow", t->rx_overflow, ", ");
    put_u64(file, "rx_sf_too_long", t->rx_sf_too_long, ", ");
    put_u64(file, "rx_sf_busy", t->rx_sf_busy, ", ");
    put_u64(file, "fc_dropped", t->fc_dropped, ", ");
    put_u64(file, "stmin_reserved", t->stmin_reserved, ", ");
    put_u64(file, "send_refused", t->send_refused, ",\n      ");
    put_u64(file, "confirmed", t->confirmed, ", ");
    put_u64(file, "confirm_unmatched", t->confirm_unmatched, ", ");
    put_u64(file, "unconfirmed", t->unconfirmed, ", ");
    put_u64(file, "proven", t->proven, ", ");
    put_u64(file, "outstanding", t->outstanding, ", ");
    put_u64(file, "reentry_refused", t->reentry_refused, ",\n      ");
    put_u64(file, "n_br_late", t->n_br_late, ", ");
    put_u64(file, "n_cs_late", t->n_cs_late, ", ");
    put_u64(file, "n_br_max_us", t->n_br_max_us, ", ");
    put_u64(file, "n_cs_max_us", t->n_cs_max_us, "},\n    \"uds\": {");
    put_named_counts(file, "requests", u->requests, UDS_SERVICE_COUNT, service_name_of, ", ");
    put_u64(file, "requests_unsupported", u->requests_unsupported, ", ");
    put_u64(file, "requests_empty", u->requests_empty, ",\n      ");
    put_named_counts(file, "nrc", u->nrc, UDS_NRC_KINDS, nrc_name_of, ",\n      ");
    put_u64(file, "response_pending", u->response_pending, ", ");
    put_u64(file, "positive", u->positive, ", ");
    put_u64(file, "suppressed", u->suppressed, ", ");
    put_u64(file, "session_changes", u->session_changes, ", ");
    put_u64(file, "s3_timeouts", u->s3_timeouts, ", ");
    put_u64(file, "resets", u->resets, ", ");
    put_u64(file, "pending_max_us", u->pending_max_us, ",\n      \"download\": {");
    (void)fprintf(file, "\"state\": \"%s\", ", uds_dl_state_name(dl->state));
    put_u64(file, "address", dl->address, ", ");
    put_u64(file, "size", dl->size, ", ");
    put_u64(file, "received", dl->received, ", ");
    put_u64(file, "blocks", dl->blocks, ", ");
    put_u64(file, "repeated_blocks", dl->repeated_blocks, ", ");
    put_u64(file, "counter", dl->counter, ", ");
    put_u64(file, "erases", dl->erases, ", ");
    put_u64(file, "started", dl->started, ", ");
    put_u64(file, "exited", dl->exited, ", ");
    put_u64(file, "committed", dl->committed, ", ");
    put_u64(file, "aborted", dl->aborted, ", ");
    put_u64(file, "failed", dl->failed, ", ");
    put_u64(file, "erase_us", dl->erase_us, ", ");
    put_u64(file, "transfer_us", dl->transfer_us, ", ");
    put_u64(file, "check_us", dl->check_us, "}},\n    \"backend\": {");
    put_u64(file, "post_refused", b->post_refused, ", ");
    put_u64(file, "snapshots", b->snapshots, ", ");
    put_u64(file, "clears", b->clears, ", ");
    put_u64(file, "clears_done", b->clears_done, ", ");
    put_u64(file, "resets", b->resets, ",\n      \"jobs\": {");

    for (uint32_t kind = 1U; kind < (uint32_t)FLASH_JOB_KINDS; kind++) {
        (void)fprintf(file, "%s\"%s\": {", (kind == 1U) ? "" : ", ",
                      flash_job_kind_name((flash_job_kind_t)kind));
        put_u64(file, "posted", b->jobs[kind], ", ");
        put_u64(file, "failed", b->jobs_failed[kind], ", ");
        put_u64(file, "last_steps", b->last_steps[kind], ", ");
        (void)fprintf(file, "\"last_status\": \"%s\"}", boot_status_name(b->last_status[kind]));
    }

    (void)fprintf(file, "}}},\n");
}

/* "command": the receiver of TESTER_Command (app/command.h). */
static void write_command_stats(FILE *file, const node_stats_t *stats)
{
    const command_rx_t *command = &stats->command;

    (void)fprintf(file, "  \"command\": {");
    put_u64(file, "frames", command->frames, ", ");
    put_u64(file, "cycles", command->cycles, ", ");
    put_u64(file, "checks", command->checks, ", ");
    put_u64(file, "check_errors", command->check_errors, ", ");
    put_u64(file, "sm_errors", command->sm_errors, ",\n    ");
    (void)fprintf(file, "\"status\": {");
    put_u64(file, "OK", command->status.ok, ", ");
    put_u64(file, "OKSOMELOST", command->status.ok_some_lost, ", ");
    put_u64(file, "REPEATED", command->status.repeated, ", ");
    put_u64(file, "WRONGSEQUENCE", command->status.wrong_sequence, ", ");
    put_u64(file, "ERROR", command->status.error, ", ");
    put_u64(file, "NONEWDATA", command->status.no_new_data, "},\n    ");
    (void)fprintf(file, "\"sm_state\": \"%s\", ", sm_state_name(command->sm.sm_state));
    put_u64(file, "sm_ok_count", command->sm.ok_count, ", ");
    put_u64(file, "sm_error_count", command->sm.error_count, ", ");
    put_u64(file, "accepted", command->accepted, ", ");
    put_u64(file, "ignored", command->ignored, ", ");
    put_bool(file, "telemetry_on", command->telemetry_on, ", ");
    put_u64(file, "clear_requests", command->clear_requests, ", ");

    if (command->have_command) {
        put_u64(file, "clear_faults_seq", command->clear_faults_seq, "},\n");
    } else {
        (void)fprintf(file, "\"clear_faults_seq\": null},\n");
    }
}

/* A DTC number in the display form of SAE J2012 and ISO 14229-1, as app/fault.h lists them:
 * the system letter and four characters of the two high bytes, the failure type after a dash
 * ("P1A01-00"). `out` holds at least 9 characters and the terminating zero. */
static void dtc_code(uint32_t dtc, char out[10])
{
    static const char letters[] = "PCBU";
    const unsigned int code = (unsigned int)((dtc >> 8) & 0xFFFFU);

    (void)snprintf(out, 10, "%c%u%03X-%02X", letters[(code >> 14) & 3U], (code >> 12) & 3U,
                   code & 0xFFFU, (unsigned int)(dtc & 0xFFU));
}

/* "faults": the DTCs as their owner, the health task, left them (app/fault.h), and the summary
 * of them the last BMS_Fault carried. */
static void write_fault_stats(FILE *file, const node_stats_t *stats)
{
    const fault_monitor_t *faults = &stats->faults;
    const fault_summary_t *summary = &stats->fault_summary;

    (void)fprintf(file, "  \"faults\": {");
    put_bool(file, "pack_fault_active", summary->active != 0U, ", ");
    put_u64(file, "active", summary->active, ", ");
    put_u64(file, "confirmed", summary->confirmed, ", ");

    if (summary->most_recent < (uint8_t)FAULT_EVENTS) {
        (void)fprintf(file, "\"most_recent\": \"%s\", ", fault_name(summary->most_recent));
    } else {
        (void)fprintf(file, "\"most_recent\": null, ");
    }

    put_bool(file, "warning", summary->warning, ", ");
    put_u64(file, "clears", faults->clears, ", ");
    put_u64(file, "clear_requests_seen", faults->clear_requests, ",\n    ");
    put_u64(file, "verdicts_applied", faults->verdicts, ", ");
    put_u64(file, "health_cycles", faults->health_cycles, ", ");
    put_u64(file, "status_changes", dtc_status_change_count(&faults->dtc), ", ");
    (void)fprintf(file, "\"reset_cause\": \"%s\",\n    \"dtcs\": [",
                  posix_reset_reason_name(faults->reset_cause));

    for (uint32_t event = 0U; event < (uint32_t)FAULT_EVENTS; event++) {
        dtc_entry_t entry;
        char code[10];

        (void)memset(&entry, 0, sizeof(entry));
        (void)dtc_get(&faults->dtc, (uint16_t)event, &entry);
        dtc_code(fault_dtc_number(event), code);
        (void)fprintf(file, "%s\n      {", (event == 0U) ? "" : ",");
        put_u64(file, "event", event, ", ");
        (void)fprintf(file, "\"name\": \"%s\", ", fault_name(event));
        put_u64(file, "dtc", fault_dtc_number(event), ", ");
        (void)fprintf(file, "\"code\": \"%s\", ", code);
        put_u64(file, "status", entry.status, ", ");
        put_i64(file, "counter", entry.counter, ", ");
        put_i64(file, "fdc", entry.fault_detection_counter, ", ");
        put_u64(file, "failed_cycles", entry.failed_cycles, "}");
    }

    (void)fprintf(file, "\n    ]},\n");
}

static const char *local_status_name(wdgm_local_status_t status)
{
    static const char *const names[] = {"DEACTIVATED", "OK", "FAILED", "EXPIRED"};

    return ((uint32_t)status <= (uint32_t)WDGM_LOCAL_STATUS_EXPIRED) ? names[status] : "unknown";
}

static const char *global_status_name(wdgm_global_status_t status)
{
    static const char *const names[] = {"DEACTIVATED", "OK", "FAILED", "EXPIRED", "STOPPED"};

    return ((uint32_t)status <= (uint32_t)WDGM_GLOBAL_STATUS_STOPPED) ? names[status] : "unknown";
}

/* "watchdog_manager": the supervision of the tasks (lib/wdgm) and the kicks of the hardware
 * watchdog, as the health task left them; "hw_watchdog" has what the watchdog itself counted. */
static void write_supervision_stats(FILE *file, const node_stats_t *stats)
{
    const node_supervision_stats_t *supervision = &stats->supervision;

    (void)fprintf(file, "  \"watchdog_manager\": {\"global_status\": \"%s\", ",
                  global_status_name(supervision->status));
    put_u64(file, "cycles", supervision->cycles, ", ");

    if (supervision->first_expired < supervision->entity_count) {
        (void)fprintf(file, "\"first_expired\": \"%s\", ",
                      supervision->entities[supervision->first_expired].name);
    } else {
        (void)fprintf(file, "\"first_expired\": null, ");
    }

    put_u64(file, "expired_cycles", supervision->expired_cycles, ", ");
    put_u64(file, "expired_tolerance", supervision->expired_tolerance, ",\n    ");
    put_u64(file, "kicks", supervision->kicks, ", ");
    put_u64(file, "kicks_withheld", supervision->kicks_withheld, ", ");
    put_bool(file, "hw_started", supervision->watchdog_started, ", ");
    put_u64(file, "hw_timeout_ms", supervision->timeout_ms, ",\n    \"entities\": [");

    for (uint32_t i = 0U; (i < supervision->entity_count) && (i < WDGM_MAX_ENTITIES); i++) {
        const node_supervised_stats_t *entity = &supervision->entities[i];

        (void)fprintf(file, "%s\n      {\"name\": \"%s\", \"status\": \"%s\", ",
                      (i == 0U) ? "" : ",", (entity->name != NULL) ? entity->name : "unknown",
                      local_status_name(entity->status));
        put_u64(file, "examinations", entity->examinations, ", ");
        put_u64(file, "incorrect", entity->incorrect, ", ");
        put_u64(file, "last_count", entity->last_count, ", ");
        put_u64(file, "min_count", entity->min_count, ", ");
        put_u64(file, "max_count", entity->max_count, ",\n       ");
        put_u64(file, "expected_alive", entity->expected_alive, ", ");
        put_u64(file, "min_margin", entity->min_margin, ", ");
        put_u64(file, "max_margin", entity->max_margin, ", ");
        put_u64(file, "reference_cycle", entity->reference_cycle, ", ");
        put_u64(file, "failed_tolerance", entity->failed_tolerance, "}");
    }

    (void)fprintf(file, "\n    ]}\n");
}

/*
 * "trace" (phase 6): the host-only trace of --trace-file (posix_trace.h), or false. With a
 * trace: its capacity, the events recorded and dropped, the size of a record, what one record
 * cost when the trace was opened (event_cost_ns, the fastest of the rounds timed, cost_records
 * records in all), and the number and name of every task the trace saw run, by which a reader
 * of the trace file names its tasks. Task names are the kernel's, which need no escaping.
 */
static void write_trace_stats(FILE *file)
{
    posix_trace_info_t info;

    posix_trace_get_info(&info);

    if (!info.open) {
        (void)fprintf(file, "  \"trace\": false,\n");
        return;
    }

    (void)fprintf(file, "  \"trace\": {");
    put_u64(file, "capacity", info.capacity, ", ");
    put_u64(file, "events", info.recorded, ", ");
    put_u64(file, "dropped", info.dropped, ", ");
    put_u64(file, "record_bytes", sizeof(posix_trace_event_t), ", ");
    put_u64(file, "file_version", POSIX_TRACE_FILE_VERSION, ", ");
    (void)fprintf(file, "\"event_cost_ns\": %llu.%03llu, ",
                  (unsigned long long)(info.cost_ps / 1000U),
                  (unsigned long long)(info.cost_ps % 1000U));
    put_u64(file, "cost_records", info.cost_runs, ",\n    \"tasks\": [");

    bool first = true;

    for (uint32_t task = 0U; task < POSIX_TRACE_MAX_TASKS; task++) {
        const char *name = posix_trace_task_name(task);

        if (name != NULL) {
            (void)fprintf(file, "%s{\"number\": %u, \"name\": \"%s\"}", first ? "" : ", ",
                          (unsigned int)task, name);
            first = false;
        }
    }

    (void)fprintf(file, "]},\n");
}

static bool write_stats(const char *path, const node_options_t *options, uint64_t elapsed_ns)
{
    const node_stats_t *stats = app_final_stats();
    plat_stack_info_t stacks[NODE_STATS_MAX_TASKS];
    const size_t stack_count = plat_stack_report(stacks, NODE_STATS_MAX_TASKS);
    posix_tick_stats_t tick;
    posix_irq_stats_t irq;
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        return false;
    }

    posix_tick_get_stats(&tick);
    posix_irq_get_stats(&irq);

    /* Within one schema number, members are only ever added. */
    (void)fprintf(file, "{\n  \"schema\": \"fcn.node_stats/1\",\n");
    (void)fprintf(file, "  \"version\": \"%s\",\n", FCN_VERSION);
    /* The tree the binary was built from (build_id.h), for a measurement to check it against
     * the tree its result names. Its characters need no escaping. */
    (void)fprintf(file, "  \"build_id\": \"%s\",\n", fcn_build_id);
    (void)fprintf(file, "  \"kernel\": \"%s\",\n", tskKERNEL_VERSION_NUMBER);
    (void)fprintf(file, "  \"stack_instrumented\": %s,\n", STACK_INSTRUMENTED);
    (void)fprintf(file, "  \"elapsed_ns\": %llu,\n", (unsigned long long)elapsed_ns);
    (void)fprintf(file,
                  "  \"tick\": {\"mode\": \"%s\", \"rate_hz\": %u, \"count\": %llu, "
                  "\"thread_seen\": %s, \"signals_sent\": %llu, \"catch_ups\": %llu, "
                  "\"dropped\": %llu, \"give_backs\": %llu},\n",
                  /* The mode the tick code ran in, not the option: a test can then tell
                   * whether the option took effect. */
                  (tick.mode == POSIX_TICK_LOCKED) ? "locked" : "stock",
                  (unsigned int)configTICK_RATE_HZ, (unsigned long long)stats->tick_count,
                  tick.tick_thread_seen ? "true" : "false", (unsigned long long)tick.signals_sent,
                  (unsigned long long)tick.catch_ups, (unsigned long long)tick.dropped_ticks,
                  (unsigned long long)tick.give_backs);
    (void)fprintf(file,
                  "  \"irq\": {\"mode\": \"%s\", \"raised\": %llu, \"signals_sent\": %llu, "
                  "\"serviced_by_signal\": %llu, \"serviced_by_tick\": %llu, "
                  "\"max_nesting\": %u},\n",
                  (options->platform.irq_mode == POSIX_IRQ_SIGNAL) ? "signal" : "poll",
                  (unsigned long long)irq.raised, (unsigned long long)irq.signals_sent,
                  (unsigned long long)irq.serviced_by_signal,
                  (unsigned long long)irq.serviced_by_tick, (unsigned int)irq.max_nesting);
    (void)fprintf(file, "  \"log_lines_dropped\": %u,\n", (unsigned int)posix_log_dropped());
    write_platform_reset_stats(file);
    write_flash_stats(file, options);
    write_update_stats(file, stats);
    write_diag_stats(file, stats);
    write_can_stats(file, stats);
    (void)fprintf(file, "  \"run_time_total_us\": %llu,\n",
                  (unsigned long long)stats->total_run_time_us);
    (void)fprintf(file, "  \"tasks\": [");

    for (uint32_t i = 0U; i < stats->task_count; i++) {
        const node_task_stats_t *task = &stats->tasks[i];
        /* -1: the task never registered with plat_task_enter(), so nothing is known. */
        long long stack_peak = -1;
        long long stack_start = -1;
        long long stack_size = -1;

        for (size_t k = 0U; k < stack_count; k++) {
            if (strcmp(stacks[k].name, task->name) == 0) {
                stack_peak = (long long)stacks[k].peak_bytes;
                stack_start = (long long)stacks[k].start_bytes;
                stack_size = (long long)stacks[k].configured_bytes;
            }
        }

        /* See plat_stack_info_t: stack_peak_bytes is the depth used of the host stack,
         * stack_start_bytes the part of it in use before the task's code ran, and
         * stack_size_bytes the stack the task was created with, the size meant for the
         * target. */
        (void)fprintf(file,
                      "%s\n    {\"name\": \"%s\", \"priority\": %u, \"run_time_us\": %llu, "
                      "\"stack_peak_bytes\": %lld, \"stack_start_bytes\": %lld, "
                      "\"stack_size_bytes\": %lld, \"overruns\": %u}",
                      (i == 0U) ? "" : ",", task->name, (unsigned int)task->priority,
                      (unsigned long long)task->run_time_us, stack_peak, stack_start, stack_size,
                      (unsigned int)task->overruns);
    }

    (void)fprintf(file, "\n  ],\n");
    write_trace_stats(file);
    write_task_table(file);
    write_measurement_stats(file, stats);
    write_telemetry_stats(file, stats);
    write_command_stats(file, stats);
    write_fault_stats(file, stats);
    write_supervision_stats(file, stats);
    (void)fprintf(file, "}\n");
    return (fclose(file) == 0);
}

/* True if the node will be able to create or overwrite the file at `path` (NULL: no file).
 * Checked before the run, so that a mistyped path is a start-up error and not a log that
 * quietly goes elsewhere or statistics that are lost after an hour's run. The file itself is
 * not created here: a node that dies before it writes its statistics leaves none. */
static bool output_path_usable(const char *path, const char *option, char *error, size_t error_len)
{
    struct stat info;
    bool usable = true;

    if (path == NULL) {
        return true;
    }

    if (stat(path, &info) == 0) {
        if (S_ISDIR(info.st_mode)) {
            errno = EISDIR;
            usable = false;
        } else {
            usable = (access(path, W_OK) == 0);
        }
    } else if (errno == ENOENT) {
        /* The file does not exist yet: its directory has to, and it has to be writable. */
        char directory[4096];
        const char *slash = strrchr(path, '/');
        const size_t length = (slash == NULL) ? 0U : (size_t)(slash - path);

        if (slash == NULL) {
            usable = (access(".", W_OK | X_OK) == 0);
        } else if (length == 0U) {
            usable = (access("/", W_OK | X_OK) == 0);
        } else if (length >= sizeof(directory)) {
            errno = ENAMETOOLONG;
            usable = false;
        } else {
            (void)memcpy(directory, path, length);
            directory[length] = '\0';
            usable = (access(directory, W_OK | X_OK) == 0);
        }
    } else {
        usable = false;
    }

    if (!usable) {
        (void)snprintf(error, error_len, "cannot write %s %s: %s", option, path, strerror(errno));
    }

    return usable;
}

/* What can be wrong with a configuration whose options each parsed: checked before anything
 * is started but the retained cell, which main() opens first and checks --retained-file for. */
static bool check_configuration(const node_options_t *options, char *error, size_t error_len)
{
    can_backend_spec_t spec;

    if (!can_backend_parse_spec(options->can_spec, &spec, error, error_len)) {
        return false;
    }

    /* The simulation control socket must not be one of the bus's ports. It would take a slot
     * of the bus, receive every frame sent on it, and answer each with an error datagram. */
    if ((spec.kind == CAN_BACKEND_UDP) && (options->sim_port != 0U) &&
        (options->sim_port >= spec.udp_base_port) &&
        ((uint32_t)options->sim_port < ((uint32_t)spec.udp_base_port + CAN_UDP_SLOTS))) {
        (void)snprintf(
            error, error_len, "--sim-port %u is one of the ports of the UDP bus %s (%u to %u)",
            (unsigned int)options->sim_port, options->can_spec, (unsigned int)spec.udp_base_port,
            (unsigned int)spec.udp_base_port + CAN_UDP_SLOTS - 1U);
        return false;
    }

    if (options->flash_cut_given && (options->flash_path == NULL)) {
        (void)snprintf(error, error_len, "--flash-cut needs a flash (--flash-file)");
        return false;
    }

    if ((options->flash_hang != 0U) && (options->flash_path == NULL)) {
        (void)snprintf(error, error_len, "--flash-hang needs a flash (--flash-file)");
        return false;
    }

    if ((options->trace_events != 0U) && (options->trace_path == NULL)) {
        (void)snprintf(error, error_len, "--trace-events needs a trace (--trace-file)");
        return false;
    }

    return output_path_usable(options->platform.log_path, "--log-file", error, error_len) &&
           output_path_usable(options->stats_path, "--stats-json", error, error_len) &&
           output_path_usable(options->trace_path, "--trace-file", error, error_len);
}

/* "sensor.hang on" (posix_sensor.h): the sensor task's read waits as a driver waits for an
 * interrupt that never comes, blocked one tick at a time, so that the tasks below it run. */
static void sensor_wait_one_tick(void)
{
    vTaskDelay(1U);
}

/* A flash operation (posix_flash.h) waits as a driver of a serial NOR waits for the busy flag
 * of its part: it looks once per tick and blocks its task in between, so that the tasks below
 * it run. Called again until the operation's modelled time has passed. Before the scheduler
 * runs there is no task to block: the main thread sleeps for a millisecond instead. */
static void flash_wait_one_tick(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        vTaskDelay(1U);
    } else {
        const struct timespec nap = {0, 1000000L};

        (void)nanosleep(&nap, NULL);
    }
}

int main(int argc, char **argv)
{
    node_options_t options;
    char error[1024] = {0}; /* room for a message that quotes a long path */
    int status;

    (void)memset(&options, 0, sizeof(options));
    options.platform.tick_mode = POSIX_TICK_LOCKED;
    options.platform.irq_mode = POSIX_IRQ_SIGNAL;
    options.platform.log_level = LOG_LEVEL_INFO;
    options.can_spec = DEFAULT_CAN_SPEC;
    options.bitrate = DEFAULT_BITRATE;
    options.sensor_seed = POSIX_SENSOR_DEFAULT_SEED;

    status = parse_options(argc, argv, &options);

    if (status != 0) {
        return (status == 1) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /* The retained cell is read first, as a target's start-up code reads its no-init RAM
     * before anything else runs, so that even a reset during start-up is recorded. First also
     * before the checks below: a node that ends at one of them has still taken the
     * bootloader's hand-off out of the cell, where a later start without the bootloader would
     * find it (plat_retained.h, THE REMAINING WINDOW). Only a command line that does not parse
     * (which may not even name the cell) ends the node before the open. */
    if (!output_path_usable(options.retained_path, "--retained-file", error, sizeof(error)) ||
        !posix_retained_open(options.retained_path, error, sizeof(error))) {
        (void)fprintf(stderr, "fcn_node: %s\n", error);
        return EXIT_FAILURE;
    }

    /* A test harness passes its process id here so that no node is left behind when the
     * harness dies: the kernel is asked to kill the node when its parent exits. The request
     * only covers a parent that is still alive when it is made, hence the check after it: if
     * the harness died before this point, the node already has another parent and ends here. */
    const char *const harness = getenv("FCN_DIE_WITH_PARENT");

    if (harness != NULL) {
        (void)prctl(PR_SET_PDEATHSIG, SIGKILL);

        if ((long)getppid() != strtol(harness, NULL, 10)) {
            posix_retained_close();
            return EXIT_FAILURE;
        }
    }

    if (!check_configuration(&options, error, sizeof(error))) {
        (void)fprintf(stderr, "fcn_node: %s\n", error);
        posix_retained_close();
        return EXIT_FAILURE;
    }

    /* The flash is part of the board as well: a file of the wrong size is a start-up error,
     * before anything runs. Without --flash-file the run has no flash and nothing changes. */
    if (!posix_flash_open(options.flash_path, NULL, error, sizeof(error))) {
        (void)fprintf(stderr, "fcn_node: %s\n", error);
        posix_retained_close();
        return EXIT_FAILURE;
    }

    posix_flash_set_wait(flash_wait_one_tick);

    if (options.flash_cut_given) {
        posix_flash_arm_cut(&options.flash_cut);
    }

    posix_flash_arm_hang(options.flash_hang);

    /* The host-only trace before anything that it records starts: the sensor model, the CAN
     * hardware thread, the tasks and the tick. */
    if ((options.trace_path != NULL) &&
        !posix_trace_open((options.trace_events != 0U) ? options.trace_events
                                                       : POSIX_TRACE_DEFAULT_EVENTS,
                          error, sizeof(error))) {
        (void)fprintf(stderr, "fcn_node: %s\n", error);
        posix_flash_close();
        posix_retained_close();
        return EXIT_FAILURE;
    }

    posix_platform_init(&options.platform);
    LOG_INF("main", "fcn_node %s starting (FreeRTOS %s)", FCN_VERSION, tskKERNEL_VERSION_NUMBER);
    LOG_INF("main", "build %s", fcn_build_id);

    if (options.trace_path != NULL) {
        posix_trace_info_t trace;

        posix_trace_get_info(&trace);
        LOG_INF("main", "host-only trace of up to %lu events, %lu ps per event (--trace-file)",
                (unsigned long)trace.capacity, (unsigned long)trace.cost_ps);
    }

    {
        posix_retained_info_t retained;

        posix_retained_get_info(&retained);
        LOG_INF("main", "reset cause %s (retained cell: %s, %lu resets since power-on)",
                posix_reset_reason_name(retained.cause), retained.verdict,
                (unsigned long)retained.resets);
    }

    if (options.flash_path != NULL) {
        posix_flash_info_t flash;

        /* The path last: a long one is cut off at the end of the line, not the rest. */
        posix_flash_get_info(&flash);

        if (options.flash_cut_given) {
            LOG_INF("main", "flash %s, power cut in program or erase %lu, seed %lu: %s",
                    flash.created ? "created erased" : "opened",
                    (unsigned long)options.flash_cut.operation,
                    (unsigned long)options.flash_cut.seed, options.flash_path);
        } else {
            LOG_INF("main", "flash %s, no power cut: %s",
                    flash.created ? "created erased" : "opened", options.flash_path);
        }

        if (options.flash_hang != 0U) {
            LOG_INF("main", "flash program or erase %lu of this run hangs (--flash-hang)",
                    (unsigned long)options.flash_hang);
        }
    }

    /* The simulated hardware comes up before the application, as a board is powered before
     * its firmware runs. Without a bus there is no node: failing here is a start-up error. The
     * sensor model starts before the CAN hardware thread, which serves its simulation control
     * commands and may only start once every model has registered them. */
    bool hardware_up = posix_sim_open(options.sim_port, error, sizeof(error));

    if (hardware_up) {
        posix_sensor_start(options.sensor_seed);
        posix_sensor_set_wait(sensor_wait_one_tick);
        hardware_up = posix_can_start(options.can_spec, options.bitrate, error, sizeof(error));
    }

    if (!hardware_up) {
        (void)fprintf(stderr, "fcn_node: %s\n", error);
        posix_sim_close();
        posix_platform_shutdown();
        posix_flash_close();
        posix_retained_close();
        posix_trace_close();
        return EXIT_FAILURE;
    }

    LOG_INF("main", "CAN on %s at %lu bit/s, simulation control %s, sensor seed %lu",
            options.can_spec, (unsigned long)options.bitrate,
            (options.sim_port != 0U) ? "on" : "off", (unsigned long)options.sensor_seed);

    if ((options.can_fault != NULL) && (strcmp(options.can_fault, "none") != 0)) {
        LOG_INF("main", "bus fault %s from the start (--can-fault)", options.can_fault);
    }

    if (options.app.confirm_periods != 0U) {
        LOG_INF("main",
                "a trial confirms itself after %lu healthy health periods (--confirm-periods)",
                (unsigned long)options.app.confirm_periods);
    }
    app_start(&options.app);

    const uint64_t start_ns = plat_time_ns();

    posix_platform_run();

    const uint64_t elapsed_ns = plat_time_ns() - start_ns;

    posix_can_stop();
    /* Every context that records has ended: the tasks, the hardware thread; the tick thread
     * ends with the platform below and records nothing after this. */
    posix_trace_stop();
    posix_sim_close();
    LOG_INF("main", "scheduler stopped");
    posix_platform_shutdown();
    posix_flash_close();
    posix_retained_close();

    bool trace_written = true;

    if (options.trace_path != NULL) {
        trace_written = posix_trace_write(options.trace_path, error, sizeof(error));

        if (!trace_written) {
            (void)fprintf(stderr, "fcn_node: %s\n", error);
        }
    }

    const int transport_error = posix_can_transport_error();
    const bool stats_written =
        (options.stats_path == NULL) || write_stats(options.stats_path, &options, elapsed_ns);

    posix_trace_close();

    if (!stats_written) {
        (void)fprintf(stderr, "cannot write %s\n", options.stats_path);
    }

    /* The run was stopped because the bus went away (posix_can.c): that is what the exit
     * status says, whether or not the statistics could be written. */
    if (transport_error != 0) {
        (void)fprintf(stderr, "fcn_node: the CAN transport %s went away: %s\n", options.can_spec,
                      strerror(transport_error));
        return POSIX_EXIT_CAN_TRANSPORT_LOST;
    }

    return (stats_written && trace_written) ? EXIT_SUCCESS : EXIT_FAILURE;
}
