/* SPDX-License-Identifier: MIT */
#include "command.h"

#include <stddef.h>
#include <string.h>

#include "can_db.h"

_Static_assert(COMMAND_CYCLE_MS == CAN_DB_TESTER_COMMAND_CYCLE_MS,
               "the supervision period is the cycle time of TESTER_Command");
_Static_assert(CAN_DB_TESTER_COMMAND_E2E_PROTECTED == 1U, "TESTER_Command is protected");
_Static_assert(CAN_DB_TESTER_COMMAND_DLC == E2E_P11_MAX_DATA_LEN, "TESTER_Command: 8 bytes");
_Static_assert(CAN_DB_TESTER_COMMAND_E2E_CRC_START == (E2E_P11_CRC_BYTE * 8U), "CRC: byte 0");
_Static_assert(CAN_DB_TESTER_COMMAND_E2E_COUNTER_START == (E2E_P11_COUNTER_BYTE * 8U),
               "counter: low nibble of byte 1");
_Static_assert(CAN_DB_TESTER_COMMAND_CMD_TELEMETRY_ON_LENGTH == 1U, "CmdTelemetryOn: 1 bit");
_Static_assert(CAN_DB_TESTER_COMMAND_CMD_CLEAR_FAULTS_SEQ_LENGTH == 8U,
               "CmdClearFaultsSeq: a uint8_t");
_Static_assert((COMMAND_MAX_DELTA_COUNTER >= 1U) && (COMMAND_MAX_DELTA_COUNTER <= 14U),
               "max_delta_counter: 1..14");
_Static_assert(COMMAND_SM_WINDOW <= E2E_SM_WINDOW_MAX, "the window fits the state block");

static const e2e_p11_config_t s_e2e_config = {.data_id = CAN_DB_TESTER_COMMAND_E2E_DATA_ID,
                                              .data_len = CAN_DB_TESTER_COMMAND_DLC,
                                              .max_delta_counter = COMMAND_MAX_DELTA_COUNTER};

static const e2e_sm_config_t s_sm_config = {.window_size = COMMAND_SM_WINDOW,
                                            .min_ok_state_init = COMMAND_SM_MIN_OK_INIT,
                                            .max_error_state_init = COMMAND_SM_MAX_ERROR_INIT,
                                            .min_ok_state_valid = COMMAND_SM_MIN_OK_VALID,
                                            .max_error_state_valid = COMMAND_SM_MAX_ERROR_VALID,
                                            .min_ok_state_invalid = COMMAND_SM_MIN_OK_INVALID,
                                            .max_error_state_invalid =
                                                COMMAND_SM_MAX_ERROR_INVALID};

static void count_status(command_status_counts_t *counts, e2e_p11_status_t status)
{
    switch (status) {
    case E2E_P11STATUS_OK:
        counts->ok++;
        break;
    case E2E_P11STATUS_OKSOMELOST:
        counts->ok_some_lost++;
        break;
    case E2E_P11STATUS_REPEATED:
        counts->repeated++;
        break;
    case E2E_P11STATUS_WRONGSEQUENCE:
        counts->wrong_sequence++;
        break;
    case E2E_P11STATUS_NONEWDATA:
        counts->no_new_data++;
        break;
    default:
        counts->error++;
        break;
    }
}

/* One check and the state machine's step after it; returns the state machine's input. */
static e2e_p_check_status_t check_and_step(command_rx_t *rx, const uint8_t *data, size_t length)
{
    const e2e_result_t result = e2e_p11_check(&s_e2e_config, &rx->check, data, length);
    const e2e_p_check_status_t verdict = e2e_p11_map_status_to_sm(result, rx->check.status);

    rx->checks++;

    if (result == E2E_E_OK) {
        count_status(&rx->status, rx->check.status);
    } else {
        rx->check_errors++;
    }

    if (e2e_sm_check(verdict, &s_sm_config, &rx->sm) != E2E_E_OK) {
        rx->sm_errors++;
    }

    /* A stream that is not VALID has lost its reference for clear requests: the next command
     * carried out may come from another tester, or from this one after a restart. */
    if (rx->sm.sm_state != E2E_SM_VALID) {
        rx->clear_reference_valid = false;
    }

    return verdict;
}

void command_init(command_rx_t *rx)
{
    (void)memset(rx, 0, sizeof(*rx));
    (void)e2e_p11_check_init(&rx->check);
    (void)e2e_sm_check_init(&rx->sm, &s_sm_config);
    rx->telemetry_on = true;
}

void command_on_frame(command_rx_t *rx, const can_frame_t *frame)
{
    const size_t length = (size_t)frame->dlc;
    const e2e_p_check_status_t verdict = check_and_step(rx, frame->data, length);
    can_db_tester_command_t command = {0};
    /* New, intact data while the state machine says the stream can be trusted. The length
     * was checked by the profile: a frame of another length gives E2E_P_ERROR. */
    bool act = (verdict == E2E_P_OK) && (rx->sm.sm_state == E2E_SM_VALID);

    rx->frames++;
    rx->frame_in_period = true;

    if (act) {
        act = can_db_tester_command_unpack(&command, frame->data, length);
    }

    if (act) {
        if (rx->clear_reference_valid && (command.cmd_clear_faults_seq != rx->clear_faults_seq)) {
            rx->clear_requests++;
        }

        rx->telemetry_on = (command.cmd_telemetry_on != 0U);
        rx->clear_faults_seq = command.cmd_clear_faults_seq;
        rx->have_command = true;
        rx->clear_reference_valid = true;
        rx->accepted++;
    } else {
        rx->ignored++;
    }
}

void command_on_cycle(command_rx_t *rx)
{
    rx->cycles++;

    if (!rx->frame_in_period) {
        (void)check_and_step(rx, NULL, 0U);
    }

    rx->frame_in_period = false;
}

bool command_telemetry_on(const command_rx_t *rx)
{
    return rx->telemetry_on;
}

uint32_t command_clear_requests(const command_rx_t *rx)
{
    return rx->clear_requests;
}
