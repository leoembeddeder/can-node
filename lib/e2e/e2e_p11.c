/* SPDX-License-Identifier: MIT */
#include "e2e/e2e_p11.h"

#include <stdbool.h>

#include "crc/crc.h"

/*
 * What the profile does is described in e2e_p11.h. This file adds how, and where the
 * specification needed a reading:
 *
 *   - why the CRC starts at 0x00 and has no final XOR                    compute_crc()
 *   - "taking into wrap around 0xE"                                     counter_delta()
 *   - a received counter of 15                                          judge_message()
 *   - which input errors are checked                                    check_input_valid()
 *
 * The configuration and the counter in a state are checked on every call rather than once,
 * at initialisation: neither the configuration nor the state is owned by this module, and an
 * out-of-range data_len would make the CRC read past the end of a CAN frame.
 */

/* The counter values: 0..14 in use, 15 reserved (PRS_E2E_00504, PRS 6.12.1.1). */
#define COUNTER_MODULUS 15U
#define COUNTER_MASK    0x0FU /* the counter nibble, bits 0..3 of byte 1 */
#define PAYLOAD_NIBBLE  0xF0U /* bits 4..7 of byte 1: payload in variant 11A */

/* E2E_P11CheckInit sets Counter to 0xE (SWS_E2E_00556). */
#define CHECK_INIT_COUNTER 0x0EU

/* What the code below relies on. */
_Static_assert(COUNTER_MODULUS == (E2E_P11_COUNTER_MAX + 1U), "the counter runs 0..14");
_Static_assert(COUNTER_MASK == COUNTER_MODULUS,
               "the reserved value 15 is the one nibble value that is not a counter");
_Static_assert(CHECK_INIT_COUNTER == E2E_P11_COUNTER_MAX, "the receiver starts one before 0");
_Static_assert(E2E_P11_CRC_BYTE == 0U, "compute_crc() covers bytes 1..n-1 after byte 0");
_Static_assert(E2E_P11_COUNTER_BYTE == (E2E_P11_CRC_BYTE + 1U), "11A: counter in byte 1");
_Static_assert(E2E_P11_MIN_DATA_LEN == (E2E_P11_COUNTER_BYTE + 1U),
               "the shortest message holds the CRC and the counter byte");
_Static_assert(E2E_P11_MAX_DATA_LEN <= 8U, "a classical CAN frame has at most 8 data bytes");

/*
 * The register before the first byte, and the XOR applied after the last, of the CRC that
 * Fig. 6.97 computes. Its boxes call Crc_CalculateCRC8 (CRC-8-SAE J1850: initial value 0xFF,
 * final XOR 0xFF, SWS_Crc_00073) always with Crc_IsFirstCall = FALSE, the first time with
 * Crc_StartValue8 = 0xFF, and XOR the last result with 0xFF once more. A call with
 * Crc_IsFirstCall = FALSE starts from Crc_StartValue8 XOR 0xFF (SWS_Crc_00041), which undoes
 * the final XOR of the call before it. So the first call starts from 0xFF ^ 0xFF = 0x00, every
 * later call continues from the register the previous one left, and the final XOR of the last
 * call is undone by the profile's own: start 0x00, no final XOR. CRC_PROFILE11_START is the
 * only constant that is needed for that.
 */
#define CRC_PROFILE11_START 0x00U

static bool config_valid(const e2e_p11_config_t *config)
{
    return (config->data_len >= E2E_P11_MIN_DATA_LEN) &&
           (config->data_len <= E2E_P11_MAX_DATA_LEN) && (config->max_delta_counter >= 1U) &&
           (config->max_delta_counter <= E2E_P11_COUNTER_MAX);
}

static bool counter_valid(uint8_t counter)
{
    return counter <= E2E_P11_COUNTER_MAX;
}

/*
 * The CRC of a message of `length` bytes (PRS_E2E_00505, Fig. 6.97 with CRCOffset 0 and
 * DataIDMode BOTH): over the Data ID low byte, the Data ID high byte, and bytes 1 to
 * length - 1 of the message. Byte 0, which holds the CRC, is left out; byte 1 with the counter
 * is covered. `length` is at least 2.
 */
static uint8_t compute_crc(uint16_t data_id, const uint8_t *data, size_t length)
{
    const uint8_t id_bytes[2] = {(uint8_t)(data_id & 0xFFU), (uint8_t)(data_id >> 8U)};
    const uint8_t crc = crc8_j1850_update(CRC_PROFILE11_START, id_bytes, sizeof(id_bytes));

    return crc8_j1850_update(crc, &data[E2E_P11_CRC_BYTE + 1U], length - 1U);
}

/*
 * How many steps the sender has gone forward from `stored` to `received`, both 0..14: Fig.
 * 6.110's "ReceivedCounter - State->Counter (taking into wrap around 0xE)". The counter wraps
 * from 14 to 0, so the difference is taken modulo 15: plus 15 when it is negative, as Profile
 * 1 spells it out (PRS Fig. 6.60). Stored 14 and received 0 is one step; stored 0 and received
 * 14 is fourteen, a message from the past. The result is 0..14.
 */
static uint8_t counter_delta(uint8_t received, uint8_t stored)
{
    uint8_t delta;

    if (received >= stored) {
        delta = (uint8_t)(received - stored);
    } else {
        delta = (uint8_t)((received + COUNTER_MODULUS) - stored);
    }

    return delta;
}

/*
 * The step "Do Checks" (Fig. 6.110) for a message of the right length: the status, and the
 * update of the stored counter.
 *
 * Between the CRC and the counter comparison stands one test that the chart does not have: a
 * received counter of 15 is an ERROR, and the stored counter is kept, as after a CRC error.
 * e2e_p11.h explains why (RULE 3).
 */
static e2e_p11_status_t judge_message(const e2e_p11_config_t *config, e2e_p11_check_state_t *state,
                                      const uint8_t *data, size_t length)
{
    const uint8_t received_crc = data[E2E_P11_CRC_BYTE];
    const uint8_t received_counter = (uint8_t)(data[E2E_P11_COUNTER_BYTE] & COUNTER_MASK);
    e2e_p11_status_t status = E2E_P11STATUS_ERROR;

    if ((received_crc != compute_crc(config->data_id, data, length)) ||
        !counter_valid(received_counter)) {
        status = E2E_P11STATUS_ERROR;
    } else {
        const uint8_t delta = counter_delta(received_counter, state->counter);

        /* In the order of the chart, the step is compared with max_delta_counter first. As
         * max_delta_counter is at least 1, steps of 0 and 1 are always within it. */
        if (delta > config->max_delta_counter) {
            status = E2E_P11STATUS_WRONGSEQUENCE;
        } else if (delta == 0U) {
            status = E2E_P11STATUS_REPEATED;
        } else if (delta == 1U) {
            status = E2E_P11STATUS_OK;
        } else {
            status = E2E_P11STATUS_OKSOMELOST;
        }

        /* All four outcomes store the counter (Fig. 6.110, "State->Counter =
         * ReceivedCounter"): the receiver follows the sender wherever it jumped. */
        state->counter = received_counter;
    }

    return status;
}

e2e_result_t e2e_p11_protect_init(e2e_p11_protect_state_t *state)
{
    e2e_result_t result = E2E_E_OK;

    if (state == NULL) {
        result = E2E_E_INPUTERR_NULL;
    } else {
        state->counter = 0U;
    }

    return result;
}

e2e_result_t e2e_p11_protect(const e2e_p11_config_t *config, e2e_p11_protect_state_t *state,
                             uint8_t *data, size_t length)
{
    e2e_result_t result = E2E_E_OK;

    /* Fig. 6.94: the pointers, then the length. The configuration and the counter of the
     * state are this module's additions; with either out of range there is no right message
     * to make. */
    if ((config == NULL) || (state == NULL) || (data == NULL)) {
        result = E2E_E_INPUTERR_NULL;
    } else if (!config_valid(config) || !counter_valid(state->counter) ||
               (length != (size_t)config->data_len)) {
        result = E2E_E_INPUTERR_WRONG;
    } else {
        /* Fig. 6.96, then 6.97 and 6.98, then 6.99. The counter goes in first because the
         * CRC covers it. */
        data[E2E_P11_COUNTER_BYTE] =
            (uint8_t)((data[E2E_P11_COUNTER_BYTE] & PAYLOAD_NIBBLE) | state->counter);
        data[E2E_P11_CRC_BYTE] = compute_crc(config->data_id, data, length);
        state->counter = (uint8_t)((state->counter + 1U) % COUNTER_MODULUS);
    }

    return result;
}

e2e_result_t e2e_p11_check_init(e2e_p11_check_state_t *state)
{
    e2e_result_t result = E2E_E_OK;

    if (state == NULL) {
        result = E2E_E_INPUTERR_NULL;
    } else {
        state->counter = CHECK_INIT_COUNTER;
        state->status = E2E_P11STATUS_ERROR;
    }

    return result;
}

/*
 * The step "Verify inputs of the check function" (Fig. 6.106) after the pointer test, with this
 * module's tests of the configuration and of the stored counter, which apply also to a call
 * without data, so that a bad configuration is reported on every call.
 *
 * The data is either "no new data", data NULL together with length 0, or a message of exactly
 * config->data_len bytes. Anything else is wrong: data NULL with a length the caller believes
 * to be there, or a buffer with a length of 0 or another length.
 */
static bool check_input_valid(const e2e_p11_config_t *config, const e2e_p11_check_state_t *state,
                              const uint8_t *data, size_t length)
{
    const bool no_new_data = (data == NULL) && (length == 0U);
    const bool message = (data != NULL) && (length == (size_t)config->data_len);

    return config_valid(config) && counter_valid(state->counter) && (no_new_data || message);
}

e2e_result_t e2e_p11_check(const e2e_p11_config_t *config, e2e_p11_check_state_t *state,
                           const uint8_t *data, size_t length)
{
    e2e_result_t result = E2E_E_OK;

    if ((config == NULL) || (state == NULL)) {
        result = E2E_E_INPUTERR_NULL;
    } else if (!check_input_valid(config, state, data, length)) {
        result = E2E_E_INPUTERR_WRONG;
    } else if (data == NULL) {
        /* NewDataAvailable FALSE (Fig. 6.110, first decision) */
        state->status = E2E_P11STATUS_NONEWDATA;
    } else {
        state->status = judge_message(config, state, data, length);
    }

    return result;
}

e2e_p_check_status_t e2e_p11_map_status_to_sm(e2e_result_t check_return, e2e_p11_status_t status)
{
    e2e_p_check_status_t mapped = E2E_P_ERROR;

    /* SWS_E2E_00558: a check that did not return E2E_E_OK is an error whatever its status
     * says, the status being the one of an earlier cycle then. */
    if (check_return == E2E_E_OK) {
        switch (status) {
        case E2E_P11STATUS_OK:
        case E2E_P11STATUS_OKSOMELOST:
            mapped = E2E_P_OK;
            break;
        case E2E_P11STATUS_REPEATED:
            mapped = E2E_P_REPEATED;
            break;
        case E2E_P11STATUS_NONEWDATA:
            mapped = E2E_P_NONEWDATA;
            break;
        case E2E_P11STATUS_WRONGSEQUENCE:
            mapped = E2E_P_WRONGSEQUENCE;
            break;
        case E2E_P11STATUS_ERROR:
        default:
            /* ERROR, and any value that is not a status at all (a damaged state). */
            mapped = E2E_P_ERROR;
            break;
        }
    }

    return mapped;
}
