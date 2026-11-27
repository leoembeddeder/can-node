/* SPDX-License-Identifier: MIT */
#include "e2e/e2e_sm.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * What the state machine does is described in e2e_sm.h. This file follows Fig. 6.131
 * (transitToInvalidExtended = false) box by box; the comments name the box. Where the figure
 * calls E2E_SMClearRemainingStatus, nothing is done: with one window size for all states it
 * never clears an entry (see e2e_sm.h, WHAT IS LEFT OUT).
 *
 * What holds for an initialised e2e_sm_check_state_t between any two calls:
 *
 *   - window_top_index < window_size of the configuration it is used with;
 *   - in NODATA, ok_count and error_count are 0: Fig. 6.133 does not count the window there;
 *   - in INIT, VALID and INVALID, ok_count and error_count are the numbers of E2E_P_OK and
 *     E2E_P_ERROR among the first window_size entries of the window;
 *   - sm_state is NODATA, INIT, VALID or INVALID.
 *
 * e2e_sm_check() verifies the first and the last before it writes anything, because the
 * state is caller memory: a damaged top index would be a write outside the window.
 */

_Static_assert(E2E_SM_WINDOW_MAX <= UINT8_MAX, "window index and counts are uint8_t");
_Static_assert(E2E_SM_WINDOW_MAX >= 1U, "a window holds at least one verdict");

/* The limits of one state: at least one OK is needed (none would let the data be VALID
 * without a single good message), and a maximum of errors that a full window cannot reach
 * would mean errors never count. */
static bool limits_valid(uint8_t min_ok, uint8_t max_error, uint8_t window_size)
{
    return (min_ok >= 1U) && (min_ok <= window_size) && (max_error < window_size);
}

static bool config_valid(const e2e_sm_config_t *config)
{
    const uint8_t size = config->window_size;

    return (size >= 1U) && (size <= E2E_SM_WINDOW_MAX) &&
           limits_valid(config->min_ok_state_init, config->max_error_state_init, size) &&
           limits_valid(config->min_ok_state_valid, config->max_error_state_valid, size) &&
           limits_valid(config->min_ok_state_invalid, config->max_error_state_invalid, size);
}

/* A verdict a check can give. E2E_P_NOTAVAILABLE is the initial value of the window, "not
 * returned by any E2E profile" (SWS_E2E_00347). */
static bool verdict_valid(e2e_p_check_status_t status)
{
    bool valid = false;

    switch (status) {
    case E2E_P_OK:
    case E2E_P_REPEATED:
    case E2E_P_WRONGSEQUENCE:
    case E2E_P_ERROR:
    case E2E_P_NONEWDATA:
        valid = true;
        break;
    case E2E_P_NOTAVAILABLE:
    default:
        valid = false;
        break;
    }

    return valid;
}

/* Whether the state can be stepped: E2E_E_OK, E2E_E_WRONGSTATE before the initialisation
 * (Fig. 6.131, DEINIT), E2E_E_INPUTERR_WRONG for a state that no call can have left. */
static e2e_result_t state_usable(const e2e_sm_check_state_t *state, const e2e_sm_config_t *config)
{
    e2e_result_t result = E2E_E_OK;

    switch (state->sm_state) {
    case E2E_SM_NODATA:
    case E2E_SM_INIT:
    case E2E_SM_VALID:
    case E2E_SM_INVALID:
        if (state->window_top_index >= config->window_size) {
            result = E2E_E_INPUTERR_WRONG;
        }
        break;
    case E2E_SM_DEINIT:
        result = E2E_E_WRONGSTATE;
        break;
    default:
        result = E2E_E_INPUTERR_WRONG;
        break;
    }

    return result;
}

/* E2E_SMClearStatus (PRS_E2E_00467, Fig. 6.135): every entry E2E_P_NOTAVAILABLE, counts 0,
 * top index 0. All E2E_SM_WINDOW_MAX entries, not only window_size of them, so that a state
 * never holds a stale verdict anywhere. */
static void clear_status(e2e_sm_check_state_t *state)
{
    for (uint32_t i = 0U; i < E2E_SM_WINDOW_MAX; i++) {
        state->window[i] = E2E_P_NOTAVAILABLE;
    }

    state->window_top_index = 0U;
    state->ok_count = 0U;
    state->error_count = 0U;
}

/*
 * E2E_SMAddStatus (PRS_E2E_00466, Fig. 6.133): write the verdict at the top index, move the
 * index on with wrap-around at window_size, and, except in NODATA, count E2E_P_OK and
 * E2E_P_ERROR over the window. In NODATA the figure goes from writing the verdict straight to
 * moving the index, with the note that the window needs no evaluation there: no transition out
 * of NODATA looks at the counts, so they keep the 0 that E2E_SMCheckInit left. The figure
 * counts over the last CurrentWindowSize entries; with one window size for all states that is
 * the whole window.
 */
static void add_status(e2e_sm_check_state_t *state, const e2e_sm_config_t *config,
                       e2e_p_check_status_t status)
{
    state->window[state->window_top_index] = status;

    if (state->window_top_index >= (config->window_size - 1U)) {
        state->window_top_index = 0U;
    } else {
        state->window_top_index++;
    }

    if (state->sm_state != E2E_SM_NODATA) {
        uint8_t ok_count = 0U;
        uint8_t error_count = 0U;

        for (uint32_t i = 0U; i < config->window_size; i++) {
            if (state->window[i] == E2E_P_OK) {
                ok_count++;
            } else if (state->window[i] == E2E_P_ERROR) {
                error_count++;
            } else {
                /* REPEATED, WRONGSEQUENCE, NONEWDATA, NOTAVAILABLE: neither */
            }
        }

        state->ok_count = ok_count;
        state->error_count = error_count;
    }
}

/* The condition every transition into VALID, and staying there, depends on. */
static bool within_limits(const e2e_sm_check_state_t *state, uint8_t min_ok, uint8_t max_error)
{
    return (state->error_count <= max_error) && (state->ok_count >= min_ok);
}

/* One step of Fig. 6.131 from a state that is NODATA, INIT, VALID or INVALID. */
static void step(e2e_p_check_status_t status, const e2e_sm_config_t *config,
                 e2e_sm_check_state_t *state)
{
    add_status(state, config, status);

    if (state->sm_state == E2E_SM_NODATA) {
        /* [ProfileStatus != E2E_P_ERROR && ProfileStatus != E2E_P_NONEWDATA]
         * / E2E_SMClearStatus -> INIT. The verdict just added is cleared with the rest: INIT
         * counts from the next one on. */
        if ((status != E2E_P_ERROR) && (status != E2E_P_NONEWDATA)) {
            clear_status(state);
            state->sm_state = E2E_SM_INIT;
        }
    } else if (state->sm_state == E2E_SM_INIT) {
        /* [ErrorCount <= MaxErrorStateInit && OkCount >= MinOkStateInit] -> VALID;
         * else [ErrorCount > MaxErrorStateInit] -> INVALID (the window is kept:
         * clearFromValidToInvalid is false); else stay. */
        if (within_limits(state, config->min_ok_state_init, config->max_error_state_init)) {
            state->sm_state = E2E_SM_VALID;
        } else if (state->error_count > config->max_error_state_init) {
            state->sm_state = E2E_SM_INVALID;
        } else {
            /* not enough OKs yet, and not too many errors: stay in INIT */
        }
    } else if (state->sm_state == E2E_SM_VALID) {
        /* [ErrorCount <= MaxErrorStateValid && OkCount >= MinOkStateValid] stay; else
         * -> INVALID with the window kept (clearFromValidToInvalid is false). */
        if (!within_limits(state, config->min_ok_state_valid, config->max_error_state_valid)) {
            state->sm_state = E2E_SM_INVALID;
        }
    } else {
        /* INVALID: [ErrorCount <= MaxErrorStateInvalid && OkCount >= MinOkStateInvalid]
         * -> VALID; else [ProfileStatus == E2E_P_ERROR] / E2E_SMClearStatus, stay; else stay.
         * Clearing on every error means that recovery needs min_ok_state_invalid OKs after
         * the last error, whatever the window held before it. */
        if (within_limits(state, config->min_ok_state_invalid, config->max_error_state_invalid)) {
            state->sm_state = E2E_SM_VALID;
        } else if (status == E2E_P_ERROR) {
            clear_status(state);
        } else {
            /* not enough OKs yet: stay in INVALID */
        }
    }
}

e2e_result_t e2e_sm_check_init(e2e_sm_check_state_t *state, const e2e_sm_config_t *config)
{
    e2e_result_t result = E2E_E_OK;

    if ((state == NULL) || (config == NULL)) {
        result = E2E_E_INPUTERR_NULL;
    } else if (!config_valid(config)) {
        result = E2E_E_INPUTERR_WRONG;
    } else {
        /* SWS_E2E_00370, Fig. 6.134 */
        clear_status(state);
        state->sm_state = E2E_SM_NODATA;
    }

    return result;
}

e2e_result_t e2e_sm_check(e2e_p_check_status_t profile_status, const e2e_sm_config_t *config,
                          e2e_sm_check_state_t *state)
{
    e2e_result_t result = E2E_E_OK;

    /* SWS_E2E_00371: the pointers first. Then, before anything is written, the arguments and
     * the state; a never initialised state is E2E_E_WRONGSTATE (Fig. 6.131). */
    if ((state == NULL) || (config == NULL)) {
        result = E2E_E_INPUTERR_NULL;
    } else if (!config_valid(config) || !verdict_valid(profile_status)) {
        result = E2E_E_INPUTERR_WRONG;
    } else {
        result = state_usable(state, config);

        if (result == E2E_E_OK) {
            step(profile_status, config, state);
        }
    }

    return result;
}
