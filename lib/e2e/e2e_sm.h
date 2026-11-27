/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The E2E state machine: from the verdicts of single check cycles to the decision whether the
 * data of a protected message may be used.
 *
 * Source: AUTOSAR "E2E Protocol Specification" (AUTOSAR_FO_PRS_E2EProtocol R24-11), section
 * 6.20, and the library functions E2E_SMCheck and E2E_SMCheckInit of the "Specification of
 * SW-C End-to-End Communication Protection Library" (AUTOSAR_CP_SWS_E2ELibrary R24-11),
 * section 8.3.16. Requirement numbers are quoted where a rule is implemented.
 *
 * WHY. One check of one cycle says little. A single lost frame is normal on a busy bus; a
 * single good frame after a long silence proves little. The profile's check therefore only
 * judges one cycle, and this state machine "builds up a state out of several results of
 * check() function within a reception window" (PRS 6.20).
 *
 * INPUT. The verdict of every check, as a profile-independent e2e_p_check_status_t (for
 * Profile 11: e2e_p11_map_status_to_sm()). OK counts as OK, ERROR
 * as an error; REPEATED, WRONGSEQUENCE and NONEWDATA count as neither (PRS 6.20.2, below
 * Fig. 6.131): they act only by keeping OKs out of the window.
 *
 * WINDOW. The last window_size verdicts are kept in a ring that starts out filled with
 * E2E_P_NOTAVAILABLE. After every new verdict outside NODATA the OKs and the errors in the ring
 * are counted (E2E_SMAddStatus, PRS_E2E_00466, Fig. 6.133); the transitions compare these two
 * counts with the limits of the current state. In NODATA the verdict goes into the ring but
 * nothing is counted, as Fig. 6.133 has it: no transition out of NODATA depends on the counts,
 * and both stay 0.
 *
 * STATES AND TRANSITIONS (PRS_E2E_00345 with transitToInvalidExtended = false, Fig. 6.131):
 *
 *   DEINIT   before e2e_sm_check_init(): e2e_sm_check() refuses with E2E_E_WRONGSTATE.
 *   NODATA   nothing has been received since the initialisation.
 *            -> INIT    on a verdict that is neither ERROR nor NONEWDATA. The window is
 *                       cleared (E2E_SMClearStatus, PRS_E2E_00467), so that verdict itself is
 *                       not counted in INIT.
 *   INIT     data arrives, but not yet enough of it to be trusted.
 *            -> VALID   errors <= max_error_state_init and OKs >= min_ok_state_init
 *            -> INVALID errors > max_error_state_init
 *   VALID    the only state in which the data may be used (PRS_E2E_00914).
 *            -> INVALID unless errors <= max_error_state_valid and OKs >= min_ok_state_valid.
 *                       The window is kept.
 *   INVALID  communication is not good enough; the data must not be used.
 *            -> VALID   errors <= max_error_state_invalid and OKs >= min_ok_state_invalid
 *            otherwise an ERROR clears the window, and recovery starts from nothing again.
 *
 * Only VALID means "use the data". NODATA, INIT and INVALID all mean "do not use it"; they
 * differ in what has been seen, which matters for diagnosis.
 *
 * TIMEOUT. A frame that does not come has no CRC to fail, so loss of a message shows only if
 * the caller runs a check cycle when the frame is due and passes "no new data"
 * (e2e_p11_check() with data NULL and length 0, giving NONEWDATA). From VALID with a window
 * full of OKs, the state machine then reaches INVALID in the (window_size -
 * min_ok_state_valid + 1)-th cycle without data, (window_size - min_ok_state_valid) cycle
 * times after the first one: the detection time of PRS 6.20.4, item 2, which says the same
 * of WRONGSEQUENCE. A sender that never starts leaves the receiver in NODATA: this variant
 * has no NODATA -> INVALID transition (PRS_E2E_00675). That is safe, NODATA being as unusable
 * as INVALID, but a "never received" diagnosis has to look at NODATA itself.
 *
 * WHAT IS LEFT OUT. R24-11 has three more parameters, fixed here:
 *   - one window size for all three states instead of WindowSizeInit, WindowSizeValid and
 *     WindowSizeInvalid. With equal sizes the step E2E_SMClearRemainingStatus (PRS_E2E_00607),
 *     which clears the part of the window a state change makes visible, never clears
 *     anything, and it is left out;
 *   - clearFromValidToInvalid = false: the window is kept on the way from VALID to INVALID;
 *   - transitToInvalidExtended = false and combinedNoDataInitCount = false: no NoDataInitCount
 *     and no transition from NODATA to INVALID (the R19-11 behaviour, PRS_E2E_00675).
 *
 * DIFFERENCES IN NUMBERING. e2e_p_check_status_t has the values of the library
 * (SWS_E2E_00347). e2e_sm_state_t does not: the library gives E2E_SM_VALID the value 0x00
 * (SWS_E2E_00344), so a state that was never initialised, in zeroed static memory, would read
 * as VALID. Here DEINIT is 0, and such a state is refused.
 *
 * CONTEXT. The functions keep no data of their own, allocate nothing and do not block. One
 * e2e_sm_check_state_t belongs to one received message; calls on one state have to be
 * serialised by the caller (the task that receives the message makes all of them). The
 * configuration is constant and may be shared.
 */
#ifndef FCN_E2E_SM_H
#define FCN_E2E_SM_H

#include <stdint.h>

#include "e2e/e2e.h"

/** Largest window the state block has room for. */
#define E2E_SM_WINDOW_MAX 16U

/** E2E_PCheckStatusType: the verdict of one check cycle, independent of the profile
 *  (SWS_E2E_00347, PRS_E2E_00915), with the values of the library. */
typedef enum {
    E2E_P_OK = 0x00,            /**< new data, all checks passed (Profile 11: OK, OKSOMELOST) */
    E2E_P_REPEATED = 0x01,      /**< new data with the counter of the last one */
    E2E_P_WRONGSEQUENCE = 0x02, /**< new data whose counter jumped further than allowed */
    E2E_P_ERROR = 0x03,         /**< corrupted data, or the check function did not return OK */
    E2E_P_NOTAVAILABLE = 0x04,  /**< the initial value of the window; never a verdict */
    E2E_P_NONEWDATA = 0x05      /**< nothing arrived in this cycle */
} e2e_p_check_status_t;

/** E2E_SMStateType (PRS_E2E_00914). Numbered differently from SWS_E2E_00344, see the file
 *  comment: 0 is DEINIT. */
typedef enum {
    E2E_SM_DEINIT = 0, /**< before e2e_sm_check_init(); data cannot be used */
    E2E_SM_NODATA,     /**< no data since the initialisation; data cannot be used */
    E2E_SM_INIT,       /**< data received, not yet enough to trust it; data cannot be used */
    E2E_SM_VALID,      /**< communication within its limits: the data can be used */
    E2E_SM_INVALID     /**< communication not within its limits; data cannot be used */
} e2e_sm_state_t;

/**
 * E2E_SMConfigType (PRS_E2E_00912) with one window size for all states. Constant data.
 *
 * Valid values, checked on every call (anything else gives E2E_E_INPUTERR_WRONG):
 * window_size 1..E2E_SM_WINDOW_MAX; every min_ok_* 1..window_size; every max_error_*
 * 0..window_size - 1. A minimum of 0 OKs would let the data become, or stay, VALID without a
 * single good frame, so that a lost sender would never be noticed; a maximum of window_size
 * errors or more could never be exceeded.
 */
typedef struct {
    uint8_t window_size;             /**< verdicts looked at (WindowSizeValid/Init/Invalid) */
    uint8_t min_ok_state_init;       /**< OKs needed for INIT -> VALID */
    uint8_t max_error_state_init;    /**< errors tolerated in INIT; more: -> INVALID */
    uint8_t min_ok_state_valid;      /**< OKs needed to stay VALID */
    uint8_t max_error_state_valid;   /**< errors tolerated in VALID */
    uint8_t min_ok_state_invalid;    /**< OKs needed for INVALID -> VALID */
    uint8_t max_error_state_invalid; /**< errors tolerated for INVALID -> VALID */
} e2e_sm_config_t;

/**
 * E2E_SMCheckStateType (PRS_E2E_00913) without NoDataInitCount. Statically allocated by the
 * caller, one per received message; written only by the two functions below. ok_count and
 * error_count may be read for diagnosis: in NODATA both are 0 (Fig. 6.133 counts nothing
 * there); in the other states they are the counts of the window after the last call, as the
 * transitions saw them.
 */
typedef struct {
    e2e_p_check_status_t window[E2E_SM_WINDOW_MAX]; /**< ProfileStatusWindow; the first
                                                         window_size entries are used */
    uint8_t window_top_index; /**< WindowTopIndex: where the next verdict goes */
    uint8_t ok_count;         /**< OKCount: E2E_P_OK in the window */
    uint8_t error_count;      /**< ErrorCount: E2E_P_ERROR in the window */
    e2e_sm_state_t sm_state;  /**< SMState */
} e2e_sm_check_state_t;

/**
 * E2E_SMCheckInit (SWS_E2E_00353, SWS_E2E_00370; PRS_E2E_00375, Fig. 6.134): window all
 * E2E_P_NOTAVAILABLE, counts 0, top index 0, state NODATA. Call it before the first
 * e2e_sm_check() and whenever supervision starts again, for example when the sender is known
 * to have restarted.
 *
 * Returns E2E_E_INPUTERR_NULL if a pointer is NULL and E2E_E_INPUTERR_WRONG for a
 * configuration outside the ranges given at e2e_sm_config_t (the library checks only the
 * pointers); the state is then unchanged. Otherwise E2E_E_OK.
 */
e2e_result_t e2e_sm_check_init(e2e_sm_check_state_t *state, const e2e_sm_config_t *config);

/**
 * E2E_SMCheck (SWS_E2E_00340, SWS_E2E_00371; PRS_E2E_00354, PRS_E2E_00345, Fig. 6.131 and
 * 6.133): add the verdict of one check cycle to the window and make the transition it calls
 * for. The new state is state->sm_state; only E2E_SM_VALID means the data may be used.
 *
 * Call it once after every check of the profile, with that check's mapped verdict, including
 * the checks of periods in which nothing arrived (E2E_P_NONEWDATA): those are what make a
 * timeout visible (see TIMEOUT above).
 *
 * Returns, with the state unchanged:
 *   - E2E_E_INPUTERR_NULL    if a pointer is NULL;
 *   - E2E_E_INPUTERR_WRONG   if the configuration is out of range, `profile_status` is
 *                            E2E_P_NOTAVAILABLE or no e2e_p_check_status_t value at all, or
 *                            the state is damaged (an unknown sm_state, a top index outside
 *                            the window);
 *   - E2E_E_WRONGSTATE       if the state is DEINIT, that is, was never initialised.
 * Otherwise E2E_E_OK.
 */
e2e_result_t e2e_sm_check(e2e_p_check_status_t profile_status, const e2e_sm_config_t *config,
                          e2e_sm_check_state_t *state);

#endif /* FCN_E2E_SM_H */
