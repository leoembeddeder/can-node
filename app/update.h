/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The node's side of an update in phase 4: what the bootloader's hand-off says about the image
 * that runs (plat_retained.h, THE HAND-OFF), and the confirm of a trial image, which decides
 * when the trial has proved itself and asks for the CONFIRM record (lib/boot, boot_confirm()).
 *
 * THE HAND-OFF. Read once at the start (plat_boot_handoff()). No hand-off means that the node
 * was started without the bootloader: ActiveSlot SNA in BMS_NodeInfo, BootMode NONE in
 * BMS_BootInfo, nothing to confirm. A hand-off names the slot, whether the image runs as the
 * CONFIRMED image or as a TRIAL (and which of its three attempts), its version, and the state
 * change the bootloader made before the start; a rollback among those is the DTC
 * UPDATE_ROLLBACK (fault.h, update_rolled_back()).
 *
 * THE CONFIRM OF A TRIAL. A trial confirms itself once it has run UPDATE_CONFIRM_AFTER_S
 * seconds in good health without a break. A health period is healthy when all four hold:
 *   - the watchdog manager's global status is OK;
 *   - the CAN controller is error active (neither error passive nor bus off) at the period;
 *   - no bus-off happened since the period before, although the controller may have recovered;
 *   - the controller has transmitted at least one frame with an acknowledgement since the
 *     period before (its count of acknowledged frames grew).
 * The CAN conditions come from the controller's own state and counters, never from the DTCs
 * CAN_ERROR_PASSIVE and CAN_BUS_OFF, whose status can stay set after the fault has gone. The
 * health task judges each period (update_health_period()); a period that is not healthy starts
 * the window again, counted in `window_restarts`, and every condition it failed is counted on
 * its own (`unsupervised_periods`, `not_active_periods`, `bus_off_periods`, `unacked_periods`).
 * When the window is full the monitor asks for the confirm once (it returns true), and the
 * flash task makes it (boot_confirm() of the slot the hand-off names) and reports how it went
 * (update_confirm_done()). A confirm that fails is not tried again in the run: the trial is then
 * rolled back after its attempts, which is safe, while writing again and again into a flash
 * that refused a write is not.
 *
 * WHY 5 SECONDS (UPDATE_CONFIRM_AFTER_S). Long enough for every check of the node to have
 * judged the new image at least once with a margin: the watchdog manager finds a task that
 * stops within 300 to 400 ms and the hardware watchdog resets the node 500 ms after its last
 * kick (app.c, SUPERVISION), so an image that hangs in its first seconds is reset well inside
 * the window, as is one that crashes; 50 supervision cycles and 5 BMS_NodeInfo frames have gone
 * by, and in each of the 50 periods frames of the node were acknowledged on the bus (how many:
 * docs/update.md, the confirm). A bus fault that other nodes signal with error flags, or that
 * the node sees as bit errors, adds 8 to the transmit error counter for every failed frame:
 * the controller is error passive after 16 in a row (128) and bus off after 32 (above 255),
 * and either restarts the window. A node that nobody acknowledges never goes bus off: alone on
 * the bus, cut off from it, or with a transmit path that leads nowhere, it sees an ACK error
 * with every frame, and under ISO 11898-1's exception an error-passive transmitter does not
 * count an ACK error that is all it sees (lib/can/can_fc.c, CAN_FC_TX_ACK_ERROR_ALONE). Its
 * controller stays error passive at 128, and none of its frames is acknowledged, for as long
 * as nobody is there to. That is why an error passive controller, and a period without a newly
 * acknowledged frame, restart the window as well: a trial confirms itself only if it has been
 * heard. It does not prove that the trial
 * hears: an image that cannot receive (a wrong acceptance filter, say) confirms itself all the
 * same. A tester's routine proves the receive path, and phase 5 adds one. Short enough for the
 * system tests, which run several updates. A product would wait longer (minutes, a drive cycle)
 * or leave the confirm to a tester's routine after its own checks of the new image; phase 5's
 * UDS server can add the routine.
 *
 * WHY EVERY PERIOD NEEDS AN ACKNOWLEDGED FRAME. The weakest form that still catches a node
 * nobody acknowledges would be "a frame acknowledged within the last N periods"; N = 1 is
 * enough because no period of a node that works sees none. Whatever the telemetry switch says,
 * the sensor task publishes BMS_Heartbeat every 10 ms and the health task BMS_Fault every
 * period (app.c, WHO SENDS WHAT); BMS_TaskStats, BMS_NodeInfo and BMS_BootInfo come on top, and
 * with telemetry on BMS_PackStatus every 10 ms, BMS_CellVoltages1/2 every 20 ms and
 * BMS_Temperatures every 100 ms. The health task's periods are released 100 ms apart and it
 * reads the count within its job, whose response time is under 45 ms (44.9 ms in the
 * response-time analysis of the task table since phase 5's diagnostic task, 19.9 ms before it,
 * docs/architecture.md), so two reads are at least 55 ms apart. In between, at least five
 * heartbeats and the BMS_Fault of the period before have been published, and each is
 * acknowledged a few milliseconds after its publication: the
 * transmit task's response time (under 1 ms) and the frames ahead of it, also under a flood of
 * other nodes' frames of lower priority, since the node's periodic frames win the arbitration
 * at the next frame boundary. No tester is needed: any other node on the bus acknowledges (on
 * the host, the controller model does unless simulation control takes the acknowledgement
 * away), and what the node receives plays no part. A period without a newly acknowledged frame
 * is therefore a fault, or a host that held the node up for longer than a period, which the
 * system tests treat as a timing tolerance. (The fewest a system test counted in one period:
 * 11 with the telemetry off, 30 with it on; docs/update.md.)
 *
 * THE CONFIRM ROUTINE (phase 5). A tester confirms the trial with UDS RoutineControl 0xF000
 * (app/diag_backend.h), in the extended session; a positive response to it also shows that the
 * trial receives, which the window above cannot. The routine and the window's confirm both run
 * in the flash task, one after the other, and the first that comes writes the CONFIRM record
 * (app/flash_job.h, THE CONFIRM): after the window's, the routine is refused with
 * conditionsNotCorrect (lib/boot finds no trial); after the routine's, the window's confirm,
 * should it come, writes nothing and changes nothing here, since update_routine_confirmed() has
 * made the state CONFIRMED already.
 *
 * CONTEXT. No operating system and no locks: an update_monitor_t belongs to the health task
 * (the node: app.c), which alone calls update_health_period(), update_confirm_done() and
 * update_routine_confirmed(); what it passes to them comes from the flash task through the
 * health task's own copy. update_init() runs before the scheduler starts.
 */
#ifndef FCN_UPDATE_H
#define FCN_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "boot/boot.h"
#include "can/can_fc.h"
#include "plat_flash.h"
#include "plat_retained.h"
#include "wdgm/wdgm.h"

/** How long a trial must run in good health before it confirms itself, in seconds. */
#define UPDATE_CONFIRM_AFTER_S 5U

/** Where the confirm stands: the values of BMS_BootInfo's ConfirmState. */
typedef enum {
    UPDATE_CONFIRM_NOT_NEEDED = 0, /**< no trial: no hand-off, or the CONFIRMED image runs */
    UPDATE_CONFIRM_WAITING = 1,    /**< a trial counting its healthy time */
    UPDATE_CONFIRM_CONFIRMING = 2, /**< the confirm was asked for; the flash task writes it */
    UPDATE_CONFIRM_CONFIRMED = 3,  /**< the trial confirmed itself: it is the active image */
    UPDATE_CONFIRM_FAILED = 4      /**< the confirm could not be written */
} update_confirm_state_t;

/** What the health task reads in each period for the confirm: the watchdog manager's status
 *  and the CAN controller's own state and counters. */
typedef struct {
    wdgm_global_status_t supervision; /**< the watchdog manager's global status now */
    can_fc_state_t can_state;         /**< the CAN controller's error state now */
    uint32_t bus_off_count;           /**< bus-off events since the start */
    /** frames the controller has transmitted with an acknowledgement since the start: its
     *  transmit-done events, which a frame gets only once it went out without an error,
     *  acknowledgement included (can_txq_stats_t.sent) */
    uint64_t tx_acked;
} update_health_t;

/** The monitor. Set up with update_init(); read the members for diagnosis, write none. */
typedef struct {
    bool have_handoff;            /**< the bootloader started the node */
    plat_boot_handoff_t handoff;  /**< valid if have_handoff */
    update_confirm_state_t state; /**< where the confirm stands */
    uint32_t periods_needed;      /**< healthy periods that make the window */
    uint32_t healthy_periods;     /**< healthy periods in a row so far (stops at periods_needed) */
    uint32_t window_restarts;     /**< periods that were not healthy after a healthy one */
    uint32_t periods;             /**< health periods judged */
    uint32_t requested_period;    /**< the period the confirm was asked for in; 0 before */
    uint32_t bus_off_count;       /**< the count at the last period */
    boot_status_t confirm_status; /**< what boot_confirm() returned; BOOT_OK before */
    /* The periods judged that failed a condition of good health, one count per condition (a
     * period can fail several; THE CONFIRM OF A TRIAL). */
    uint32_t unsupervised_periods; /**< the watchdog manager's global status not OK */
    uint32_t not_active_periods;   /**< the controller not error active: passive or bus off */
    uint32_t bus_off_periods;      /**< a bus-off since the period before */
    uint32_t unacked_periods;      /**< no frame acknowledged since the period before */
    uint64_t tx_acked;             /**< the count of acknowledged frames at the last period */
    uint64_t window_acked; /**< frames acknowledged in the healthy periods of the window so far */
    /** the fewest frames acknowledged in one healthy period of the window so far; 0 before its
     *  first */
    uint64_t fewest_acked;
    /** the trial was confirmed by the tester's confirm routine (update_routine_confirmed()),
     *  not by its own window */
    bool by_routine;
} update_monitor_t;

/**
 * Set up `m` for this start: `handoff` is the bootloader's hand-off, or NULL for a start without
 * one. `periods_per_second` health periods make a second (at least 1). `window_periods` healthy
 * periods in a row make the window; 0 gives the product's, UPDATE_CONFIRM_AFTER_S seconds of
 * periods. Only the host build passes another, from its command line (--confirm-periods,
 * app_config_t): a test hook the product does not have, for a test that leaves the confirm to
 * the tester's routine and must not race the window. A TRIAL hand-off starts the window
 * (UPDATE_CONFIRM_WAITING); anything else needs no confirm. Returns false, setting up nothing,
 * for a NULL `m`. Before the scheduler starts.
 */
bool update_init(update_monitor_t *m, const plat_boot_handoff_t *handoff,
                 uint32_t periods_per_second, uint32_t window_periods);

/**
 * One health period of the confirm: judge `health` (see THE CONFIRM OF A TRIAL). Returns true
 * exactly once, in the period that completes the window: then the confirm is to be asked for,
 * and the state is UPDATE_CONFIRM_CONFIRMING. False otherwise, and in every state but WAITING,
 * whatever `health` says. The owner, once per health period.
 */
bool update_health_period(update_monitor_t *m, const update_health_t *health);

/** The flash task's result of the confirm asked for: CONFIRMED for BOOT_OK, FAILED for any
 *  other status. Ignored unless the state is CONFIRMING. The owner. */
void update_confirm_done(update_monitor_t *m, boot_status_t status);

/**
 * The tester's confirm routine (UDS RoutineControl 0xF000, app/diag_backend.h) confirmed the
 * trial: CONFIRMED with BOOT_OK, `by_routine` set, from WAITING or CONFIRMING; the window stops
 * counting, and a confirm it asked for that comes back later changes nothing (THE CONFIRM OF A
 * TRIAL: the first one wins). Ignored in every other state. The owner.
 */
void update_routine_confirmed(update_monitor_t *m);

/** The whole seconds of good health counted towards the confirm (0 without a trial; all of the
 *  window's, UPDATE_CONFIRM_AFTER_S with the product's window, once the confirm was asked for).
 *  Any context, a copy of `m`. */
uint32_t update_healthy_seconds(const update_monitor_t *m, uint32_t periods_per_second);

/** Whether the hand-off reports a rollback (the bootloader's ROLLBACK, REJECT or FALLBACK):
 *  the evidence of UPDATE_ROLLBACK (fault_init()). False for a NULL `handoff`. Any context. */
bool update_rolled_back(const plat_boot_handoff_t *handoff);

/** The name of a confirm state as in the DBC ("WAITING"), or "unknown". Any context. */
const char *update_confirm_state_name(update_confirm_state_t state);

/** The name of a hand-off's event as in the DBC ("ROLLBACK"), or "unknown". Any context. */
const char *update_event_name(plat_boot_event_t event);

/**
 * lib/boot's configuration for the node: `dev` (plat_flash_device()), the metadata area and the
 * slots of `layout` (plat_flash_layout()), and `validator` (copied), which the slot writer of a
 * download needs (boot.h; the node of phase 5 passes lib/bootimg's), or none for a NULL
 * `validator` (a node that only confirms). Fills `out` completely. Returns false, filling
 * nothing, for a NULL `layout`, `dev` or `out`. Any context.
 */
bool update_boot_config(const plat_flash_layout_t *layout, const flash_dev_t *dev,
                        const boot_validator_t *validator, boot_config_t *out);

#endif /* FCN_UPDATE_H */
