/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The node's end of TESTER_Command: decoding, the end-to-end check (AUTOSAR E2E Profile 11A
 * and the E2E state machine, lib/e2e), and the decision whether a command is carried out.
 *
 * TWO KINDS OF CALL, AS PROFILE 11 ASKS FOR THEM (e2e/e2e_p11.h, e2e_p11_check()):
 *   - command_on_frame() for every TESTER_Command frame that arrives, in the order they
 *     arrive;
 *   - command_on_cycle() once at the end of every supervision period of COMMAND_CYCLE_MS (the
 *     message's cycle time). If no frame arrived in the period, it runs the check with "no new
 *     data", which is how a tester that stopped sending becomes visible: NONEWDATA verdicts
 *     push the OKs out of the state machine's window until it leaves VALID.
 * Every check is followed by one step of the state machine, with the check's mapped verdict.
 *
 * WHAT IS CARRIED OUT. A frame's command is acted on if, after its check and the state
 * machine's step, the state machine is VALID and the frame itself was new and intact (Profile
 * 11 status OK or OKSOMELOST). A repeated frame, a frame out of sequence, a damaged one, and
 * every frame while the state machine is NODATA, INIT or INVALID are counted and ignored.
 * Acting on a command means:
 *   - CmdTelemetryOn becomes the node's telemetry switch (command_telemetry_on());
 *   - CmdClearFaultsSeq is stored, and if it differs from that of the command acted on before
 *     it while the stream stayed VALID, a request to clear the DTCs is counted (clear_requests;
 *     the owner of the DTCs carries it out, fault.h). The first command acted on after the
 *     stream started, or after it was lost (any step of the state machine that leaves it
 *     anything but VALID), only sets the reference and requests nothing: a tester that starts,
 *     another tester after one went away, or a tester that comes back after a timeout or a
 *     restart, with whatever value, clears nothing by doing so, as long as its stream was lost
 *     in between. A sender that comes back before that (silent for at most one period, or two
 *     if its counter happens to go on in sequence; COMMAND_SM_* below) cannot be told from the
 *     one before it, and a changed value from it is a request. The price: a tester that
 *     changed its value while the stream was lost has requested nothing; it sees
 *     FaultClearCount unchanged in BMS_Fault and changes the value again.
 * Before the first command that is acted on, the telemetry switch is on. Afterwards it keeps
 * the value of the last command acted on, also while the command stream is lost: a node whose
 * tester went away does not change its behaviour by itself.
 *
 * A frame whose length is not the DLC of TESTER_Command (8) is checked like any other; the
 * profile refuses it (E2E_E_INPUTERR_WRONG), and the state machine counts that as an error
 * (E2E_P11MapStatusToSM, SWS_E2E_00558). It is counted in check_errors.
 *
 * CONTEXT. No operating system, no locks: one command_rx_t belongs to one task, which makes
 * every call on it (in the node, the receive task; app/app.c).
 */
#ifndef FCN_COMMAND_H
#define FCN_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

#include "can/can_frame.h"
#include "e2e/e2e_p11.h"

/** The supervision period: the cycle time of TESTER_Command (checked against the DBC). */
#define COMMAND_CYCLE_MS 100U

/** The receiver's Profile 11 tolerance: up to one lost frame in a row still counts as new
 *  data (OKSOMELOST). */
#define COMMAND_MAX_DELTA_COUNTER 2U

/**
 * The state machine's parameters, counted in supervision periods or frames:
 *   - window of the last 4 verdicts;
 *   - INIT -> VALID with 2 OKs and at most 1 error in it: the 3rd good frame after a tester
 *     starts is the first that is acted on (the one that leaves NODATA is not counted);
 *   - VALID is left when the window holds fewer than 2 OKs or more than 1 error: in the 3rd
 *     period in a row without a frame (4 - 2 + 1), 200 ms after the first one, or with the
 *     2nd damaged frame within 4 verdicts;
 *   - INVALID -> VALID with 3 OKs and no error in the window.
 */
#define COMMAND_SM_WINDOW            4U
#define COMMAND_SM_MIN_OK_INIT       2U
#define COMMAND_SM_MAX_ERROR_INIT    1U
#define COMMAND_SM_MIN_OK_VALID      2U
#define COMMAND_SM_MAX_ERROR_VALID   1U
#define COMMAND_SM_MIN_OK_INVALID    3U
#define COMMAND_SM_MAX_ERROR_INVALID 0U

/** How often each Profile 11 status came out of a check that returned E2E_E_OK. */
typedef struct {
    uint32_t ok;
    uint32_t ok_some_lost;
    uint32_t repeated;
    uint32_t wrong_sequence;
    uint32_t error;
    uint32_t no_new_data;
} command_status_counts_t;

/** The receiver. Set up with command_init(); read the members for diagnosis, write none. */
typedef struct {
    e2e_p11_check_state_t check; /**< Profile 11 state of the message */
    e2e_sm_check_state_t sm;     /**< state machine; sm.sm_state is the verdict */
    command_status_counts_t status;
    uint32_t frames;       /**< frames passed to command_on_frame() */
    uint32_t cycles;       /**< calls of command_on_cycle() */
    uint32_t checks;       /**< checks made: every frame, and every cycle without one */
    uint32_t check_errors; /**< checks that did not return E2E_E_OK (a frame of wrong length) */
    uint32_t sm_errors;    /**< state machine steps that did not return E2E_E_OK (never) */
    uint32_t accepted;     /**< frames whose command was carried out */
    uint32_t ignored;      /**< frames whose command was not, see WHAT IS CARRIED OUT */
    /** commands acted on whose CmdClearFaultsSeq differed from the one before, in a stream that
     *  stayed VALID between the two (wraps) */
    uint32_t clear_requests;
    uint8_t clear_faults_seq;   /**< CmdClearFaultsSeq of the last command acted on */
    bool have_command;          /**< a command has been acted on since command_init() */
    bool clear_reference_valid; /**< clear_faults_seq is the reference for a clear request: a
                                     command has been acted on since the stream last became
                                     VALID */
    bool telemetry_on;          /**< the telemetry switch */
    bool frame_in_period;       /**< a frame has arrived since the last command_on_cycle() */
} command_rx_t;

/** No command seen: Profile 11 and the state machine initialised (NODATA), telemetry on,
 *  every counter 0. Needs nothing from the memory it is given. */
void command_init(command_rx_t *rx);

/** A TESTER_Command frame arrived: check it, step the state machine, and carry out its
 *  command if WHAT IS CARRIED OUT allows. `frame` is taken as a TESTER_Command; the caller
 *  routes frames by identifier. */
void command_on_frame(command_rx_t *rx, const can_frame_t *frame);

/** A supervision period has ended: if no frame arrived in it, check with no new data and step
 *  the state machine. Call once every COMMAND_CYCLE_MS. */
void command_on_cycle(command_rx_t *rx);

/** The telemetry switch: true before the first command acted on, then that command's
 *  CmdTelemetryOn. */
bool command_telemetry_on(const command_rx_t *rx);

/** The requests to clear the DTCs since command_init(): see WHAT IS CARRIED OUT. Only grows,
 *  modulo 2^32. */
uint32_t command_clear_requests(const command_rx_t *rx);

#endif /* FCN_COMMAND_H */
