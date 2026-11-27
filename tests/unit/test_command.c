/* SPDX-License-Identifier: MIT */
/*
 * The node's end of TESTER_Command (app/command.c): which commands are carried out, and how a
 * tester that stops sending becomes visible.
 *
 * The tester is played by this file: it packs a command with the generated codec and protects
 * it with lib/e2e under the Data ID of the message set (0x0200, written as a literal). Every
 * step of the state machine is worked out in the comment before it, with the window of the
 * last four verdicts (the receiver's parameters: window 4; INIT -> VALID with 2 OKs and at
 * most 1 error; VALID kept with at least 2 OKs and at most 1 error; INVALID -> VALID with 3
 * OKs and no error).
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "can_db.h"
#include "command.h"
#include "e2e/e2e_p11.h"
#include "unity.h"

static command_rx_t s_rx;
static e2e_p11_protect_state_t s_tester;

static const e2e_p11_config_t TESTER = {
    .data_id = 0x0200U, .data_len = 8U, .max_delta_counter = 1U};

void setUp(void)
{
    (void)memset(&s_rx, 0xA5, sizeof(s_rx));
    command_init(&s_rx);
    TEST_ASSERT_EQUAL(E2E_E_OK, e2e_p11_protect_init(&s_tester));
}

void tearDown(void)
{
}

/* The next frame of the tester. */
static can_frame_t command(bool telemetry_on, uint8_t clear_faults_seq)
{
    const can_db_tester_command_t msg = {.e2e_crc = 0U,
                                         .e2e_counter = 0U,
                                         .cmd_telemetry_on = telemetry_on ? 1U : 0U,
                                         .cmd_clear_faults_seq = clear_faults_seq};
    can_frame_t frame = {.id = 0x100U, .flags = 0U, .dlc = 8U, .data = {0U}};

    TEST_ASSERT_TRUE(can_db_tester_command_pack(&msg, frame.data, sizeof(frame.data)));
    TEST_ASSERT_EQUAL(E2E_E_OK, e2e_p11_protect(&TESTER, &s_tester, frame.data, 8U));

    return frame;
}

/* One supervision period with this frame in it. */
static void period_with(const can_frame_t *frame)
{
    command_on_frame(&s_rx, frame);
    command_on_cycle(&s_rx);
}

/* `count` periods with a command each, as a tester sends them. */
static void periods_with_commands(uint32_t count, bool telemetry_on, uint8_t seq)
{
    for (uint32_t i = 0U; i < count; i++) {
        const can_frame_t frame = command(telemetry_on, seq);

        period_with(&frame);
    }
}

/* The node and a tester that has sent three good commands (counters 0, 1, 2): VALID with the
 * window [OK OK - -] (the first verdict left NODATA and was not counted), telemetry off,
 * faults sequence 7. */
static void valid_with_telemetry_off(void)
{
    periods_with_commands(3U, false, 7U);
    TEST_ASSERT_EQUAL(E2E_SM_VALID, s_rx.sm.sm_state);
    TEST_ASSERT_FALSE(command_telemetry_on(&s_rx));
}

/* ------------------------------------------------------------------------------- start */

static void test_before_any_command_telemetry_is_on_and_nothing_is_known(void)
{
    TEST_ASSERT_TRUE(command_telemetry_on(&s_rx));
    TEST_ASSERT_FALSE(s_rx.have_command);
    TEST_ASSERT_EQUAL(E2E_SM_NODATA, s_rx.sm.sm_state);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rx.frames);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rx.checks);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rx.status.ok + s_rx.status.no_new_data + s_rx.status.error);
    TEST_ASSERT_EQUAL_UINT32(0U, command_clear_requests(&s_rx));
}

static void test_without_a_tester_every_period_is_a_check_with_no_new_data(void)
{
    for (uint32_t i = 0U; i < 10U; i++) {
        command_on_cycle(&s_rx);
    }

    /* NONEWDATA keeps NODATA: a tester that never starts is not a fault of this variant. */
    TEST_ASSERT_EQUAL(E2E_SM_NODATA, s_rx.sm.sm_state);
    TEST_ASSERT_EQUAL_UINT32(10U, s_rx.cycles);
    TEST_ASSERT_EQUAL_UINT32(10U, s_rx.checks);
    TEST_ASSERT_EQUAL_UINT32(10U, s_rx.status.no_new_data);
    TEST_ASSERT_TRUE(command_telemetry_on(&s_rx));
}

/*
 *   frame 1 (counter 0): OK; NODATA -> INIT, the window cleared      not carried out
 *   frame 2 (counter 1): OK; window [OK]: 1 OK, too few for VALID    not carried out
 *   frame 3 (counter 2): OK; window [OK OK]: INIT -> VALID           carried out
 */
static void test_the_third_good_command_of_a_new_tester_is_the_first_carried_out(void)
{
    periods_with_commands(1U, false, 1U);
    TEST_ASSERT_EQUAL(E2E_SM_INIT, s_rx.sm.sm_state);
    periods_with_commands(1U, false, 2U);
    TEST_ASSERT_EQUAL(E2E_SM_INIT, s_rx.sm.sm_state);
    TEST_ASSERT_TRUE(command_telemetry_on(&s_rx));
    TEST_ASSERT_FALSE(s_rx.have_command);
    TEST_ASSERT_EQUAL_UINT32(2U, s_rx.ignored);

    periods_with_commands(1U, false, 3U);
    TEST_ASSERT_EQUAL(E2E_SM_VALID, s_rx.sm.sm_state);
    TEST_ASSERT_FALSE(command_telemetry_on(&s_rx));
    TEST_ASSERT_TRUE(s_rx.have_command);
    TEST_ASSERT_EQUAL_UINT8(3U, s_rx.clear_faults_seq);
    TEST_ASSERT_EQUAL_UINT32(1U, s_rx.accepted);
    TEST_ASSERT_EQUAL_UINT32(2U, s_rx.ignored);
    TEST_ASSERT_EQUAL_UINT32(3U, s_rx.status.ok);
    /* A frame in every period: no check without data was made. */
    TEST_ASSERT_EQUAL_UINT32(3U, s_rx.checks);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rx.status.no_new_data);
}

static void test_every_good_command_in_valid_is_carried_out(void)
{
    valid_with_telemetry_off();

    periods_with_commands(1U, true, 8U);
    TEST_ASSERT_TRUE(command_telemetry_on(&s_rx));
    TEST_ASSERT_EQUAL_UINT8(8U, s_rx.clear_faults_seq);
    periods_with_commands(1U, false, 9U);
    TEST_ASSERT_FALSE(command_telemetry_on(&s_rx));
    TEST_ASSERT_EQUAL_UINT8(9U, s_rx.clear_faults_seq);
    TEST_ASSERT_EQUAL_UINT32(3U, s_rx.accepted);
}

/* ------------------------------------------------------------------------------ timeout */

/*
 *   VALID with the window [OK OK - -] after three periods with commands, then periods without:
 *   1st: [OK OK ND -], 2 OKs: VALID;  2nd: [OK OK ND ND], 2 OKs: VALID;
 *   3rd: the oldest OK is pushed out, [ND OK ND ND], 1 OK: below 2, VALID -> INVALID
 */
static void test_a_tester_that_stops_is_a_timeout_in_the_third_period_without_a_frame(void)
{
    valid_with_telemetry_off();

    command_on_cycle(&s_rx);
    TEST_ASSERT_EQUAL(E2E_SM_VALID, s_rx.sm.sm_state);
    command_on_cycle(&s_rx);
    TEST_ASSERT_EQUAL(E2E_SM_VALID, s_rx.sm.sm_state);
    command_on_cycle(&s_rx);
    TEST_ASSERT_EQUAL(E2E_SM_INVALID, s_rx.sm.sm_state);
    TEST_ASSERT_EQUAL_UINT32(3U, s_rx.status.no_new_data);

    /* The node keeps the last command it carried out. */
    TEST_ASSERT_FALSE(command_telemetry_on(&s_rx));
}

/*
 *   INVALID with [ND OK ND ND]; the tester comes back with the next counter (3), each frame
 *   replacing the oldest verdict: [ND OK ND ND] -> [ND OK' ND ND] 1 OK, [ND OK' OK ND] 2,
 *   [ND OK' OK OK] 3 OKs and no error: INVALID -> VALID. The first two are not carried out,
 *   the third is.
 */
static void test_after_a_timeout_three_good_commands_make_the_stream_valid_again(void)
{
    valid_with_telemetry_off();
    for (uint32_t i = 0U; i < 3U; i++) {
        command_on_cycle(&s_rx);
    }
    TEST_ASSERT_EQUAL(E2E_SM_INVALID, s_rx.sm.sm_state);

    periods_with_commands(2U, true, 7U);
    TEST_ASSERT_EQUAL(E2E_SM_INVALID, s_rx.sm.sm_state);
    TEST_ASSERT_FALSE(command_telemetry_on(&s_rx)); /* not carried out in INVALID */

    periods_with_commands(1U, true, 7U);
    TEST_ASSERT_EQUAL(E2E_SM_VALID, s_rx.sm.sm_state);
    TEST_ASSERT_TRUE(command_telemetry_on(&s_rx));
    TEST_ASSERT_EQUAL_UINT32(2U, s_rx.accepted); /* the 3rd of the first three, and this one */
    TEST_ASSERT_EQUAL_UINT32(4U, s_rx.ignored);
}

static void test_a_period_with_a_frame_makes_no_check_without_data(void)
{
    const can_frame_t frame = command(true, 0U);

    command_on_frame(&s_rx, &frame);
    command_on_cycle(&s_rx);
    command_on_cycle(&s_rx);

    /* One check for the frame, one for the second period, none for the first. */
    TEST_ASSERT_EQUAL_UINT32(2U, s_rx.checks);
    TEST_ASSERT_EQUAL_UINT32(1U, s_rx.status.ok);
    TEST_ASSERT_EQUAL_UINT32(1U, s_rx.status.no_new_data);
}

/* ------------------------------------------------------------------- frames not carried out */

/*
 *   VALID, then "telemetry on" (counter 3) and "off" (counter 4), both carried out. The last
 *   frame again, counter and all: REPEATED, not new data, not carried out a second time. The
 *   older "on" again, a replay: its counter is 14 steps from the stored 4, WRONGSEQUENCE, not
 *   carried out either, so the telemetry stays off.
 */
static void test_a_repeated_or_replayed_frame_is_not_carried_out(void)
{
    can_frame_t on;
    can_frame_t off;

    valid_with_telemetry_off();
    on = command(true, 9U);
    period_with(&on);
    off = command(false, 10U);
    period_with(&off);
    TEST_ASSERT_FALSE(command_telemetry_on(&s_rx));
    TEST_ASSERT_EQUAL_UINT32(3U, s_rx.accepted);

    period_with(&off);
    TEST_ASSERT_EQUAL_UINT32(1U, s_rx.status.repeated);
    TEST_ASSERT_EQUAL_UINT32(3U, s_rx.accepted);

    period_with(&on);
    TEST_ASSERT_EQUAL_UINT32(1U, s_rx.status.wrong_sequence);
    TEST_ASSERT_FALSE(command_telemetry_on(&s_rx));
    TEST_ASSERT_EQUAL_UINT8(10U, s_rx.clear_faults_seq);
    TEST_ASSERT_EQUAL_UINT32(3U, s_rx.accepted);
    TEST_ASSERT_EQUAL(E2E_SM_VALID, s_rx.sm.sm_state);
}

/*
 *   VALID with [OK OK - -]; one frame damaged on the way: ERROR, [OK OK ER -], 1 error: VALID.
 *   It is not carried out. The next frame's counter is two ahead of the last good one:
 *   OKSOMELOST (max_delta_counter 2), new data, carried out.
 */
static void test_a_damaged_frame_is_not_carried_out_and_the_next_one_is(void)
{
    can_frame_t frame;

    valid_with_telemetry_off();
    frame = command(true, 1U);
    frame.data[3] ^= 0x01U; /* CmdClearFaultsSeq 1 -> 0, the CRC no longer fits */
    period_with(&frame);
    TEST_ASSERT_EQUAL_UINT32(1U, s_rx.status.error);
    TEST_ASSERT_EQUAL(E2E_SM_VALID, s_rx.sm.sm_state);
    TEST_ASSERT_FALSE(command_telemetry_on(&s_rx));
    TEST_ASSERT_EQUAL_UINT8(7U, s_rx.clear_faults_seq);

    periods_with_commands(1U, true, 2U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_rx.status.ok_some_lost);
    TEST_ASSERT_TRUE(command_telemetry_on(&s_rx));
    TEST_ASSERT_EQUAL_UINT8(2U, s_rx.clear_faults_seq);
}

/*   [OK OK ER -] and then a second error: [OK OK ER ER], 2 errors: VALID -> INVALID. */
static void test_two_damaged_frames_in_the_window_make_the_stream_invalid(void)
{
    valid_with_telemetry_off();

    for (uint32_t i = 0U; i < 2U; i++) {
        can_frame_t frame = command(true, 1U);

        frame.data[0] ^= 0x80U;
        period_with(&frame);
    }

    TEST_ASSERT_EQUAL(E2E_SM_INVALID, s_rx.sm.sm_state);
    TEST_ASSERT_EQUAL_UINT32(2U, s_rx.status.error);
    TEST_ASSERT_FALSE(command_telemetry_on(&s_rx));
}

static void test_a_frame_of_another_message_is_an_error(void)
{
    const e2e_p11_config_t other = {.data_id = 0x0120U, .data_len = 8U, .max_delta_counter = 1U};
    e2e_p11_protect_state_t sender;
    can_frame_t frame = {.id = 0x100U, .flags = 0U, .dlc = 8U, .data = {0U}};

    valid_with_telemetry_off();
    /* Telemetry on, under the Data ID of BMS_PackStatus. */
    frame.data[2] = 0x01U;
    TEST_ASSERT_EQUAL(E2E_E_OK, e2e_p11_protect_init(&sender));
    TEST_ASSERT_EQUAL(E2E_E_OK, e2e_p11_protect(&other, &sender, frame.data, 8U));
    period_with(&frame);

    TEST_ASSERT_EQUAL_UINT32(1U, s_rx.status.error);
    TEST_ASSERT_FALSE(command_telemetry_on(&s_rx));
}

static void test_a_frame_of_the_wrong_length_is_refused_and_counted_as_an_error(void)
{
    can_frame_t frame;

    valid_with_telemetry_off();
    frame = command(true, 1U);
    frame.dlc = 7U;
    period_with(&frame);

    TEST_ASSERT_EQUAL_UINT32(1U, s_rx.check_errors);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rx.status.error);  /* not a verdict: the call was refused */
    TEST_ASSERT_EQUAL_UINT8(1U, s_rx.sm.error_count); /* but the state machine saw an error */
    TEST_ASSERT_FALSE(command_telemetry_on(&s_rx));

    frame.dlc = 0U;
    period_with(&frame);
    TEST_ASSERT_EQUAL_UINT32(2U, s_rx.check_errors);
    TEST_ASSERT_EQUAL(E2E_SM_INVALID, s_rx.sm.sm_state);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rx.sm_errors);
}

static void test_commands_before_the_stream_is_valid_change_nothing(void)
{
    /* A tester's first two frames turn the telemetry off: not yet trusted. */
    periods_with_commands(2U, false, 5U);
    TEST_ASSERT_TRUE(command_telemetry_on(&s_rx));
    TEST_ASSERT_FALSE(s_rx.have_command);
    TEST_ASSERT_EQUAL_UINT8(0U, s_rx.clear_faults_seq);
}

static void test_the_counters_add_up(void)
{
    valid_with_telemetry_off();
    command_on_cycle(&s_rx);
    periods_with_commands(2U, true, 1U);

    /* Every frame is either carried out or not; every check has one verdict or an error. */
    TEST_ASSERT_EQUAL_UINT32(s_rx.frames, s_rx.accepted + s_rx.ignored);
    TEST_ASSERT_EQUAL_UINT32(s_rx.checks, s_rx.status.ok + s_rx.status.ok_some_lost +
                                              s_rx.status.repeated + s_rx.status.wrong_sequence +
                                              s_rx.status.error + s_rx.status.no_new_data +
                                              s_rx.check_errors);
    TEST_ASSERT_EQUAL_UINT32(5U, s_rx.frames);
    TEST_ASSERT_EQUAL_UINT32(6U, s_rx.cycles);
    TEST_ASSERT_EQUAL_UINT32(6U, s_rx.checks);
}

/* ---------------------------------------------------------------------- clear requests */

/*
 * A request to clear the DTCs is a CmdClearFaultsSeq that differs from that of the command
 * carried out before it: the first command carried out (7) is the reference, a repeated value
 * is none, every change is one, back to an earlier value included.
 */
static void test_a_changed_clear_faults_seq_between_two_commands_carried_out_is_one_request(void)
{
    valid_with_telemetry_off();
    TEST_ASSERT_EQUAL_UINT32(0U, command_clear_requests(&s_rx));

    periods_with_commands(2U, false, 7U);
    TEST_ASSERT_EQUAL_UINT32(0U, command_clear_requests(&s_rx));
    periods_with_commands(1U, false, 8U);
    TEST_ASSERT_EQUAL_UINT32(1U, command_clear_requests(&s_rx));
    periods_with_commands(3U, true, 8U);
    TEST_ASSERT_EQUAL_UINT32(1U, command_clear_requests(&s_rx));
    periods_with_commands(1U, true, 9U);
    periods_with_commands(1U, true, 7U);
    TEST_ASSERT_EQUAL_UINT32(3U, command_clear_requests(&s_rx));
    TEST_ASSERT_EQUAL_UINT32(3U, s_rx.clear_requests);
    TEST_ASSERT_EQUAL_UINT8(7U, s_rx.clear_faults_seq);
}

/* Before the stream is valid nothing is carried out, so nothing is requested either, and the
 * first command carried out, whatever its value, is the reference. */
static void test_the_first_command_carried_out_requests_no_clear(void)
{
    periods_with_commands(1U, true, 5U);
    periods_with_commands(1U, true, 6U);
    TEST_ASSERT_FALSE(s_rx.have_command);
    periods_with_commands(1U, true, 9U);
    TEST_ASSERT_TRUE(s_rx.have_command);
    TEST_ASSERT_EQUAL_UINT8(9U, s_rx.clear_faults_seq);
    TEST_ASSERT_EQUAL_UINT32(0U, command_clear_requests(&s_rx));
}

/*
 * A frame that is not carried out requests nothing, whatever it carries: a damaged one, a
 * repeated one, and those of a stream that is INVALID after a timeout. The command carried out
 * after a damaged frame, in a stream that stayed VALID, is compared with the last one carried
 * out, not with the frame in between; the first one carried out after the timeout only sets
 * the reference again (7), and the change after it (8) is the request.
 */
static void test_frames_not_carried_out_request_no_clear(void)
{
    can_frame_t frame;

    valid_with_telemetry_off();
    frame = command(true, 99U);
    frame.data[0] ^= 0x01U; /* damaged */
    period_with(&frame);
    TEST_ASSERT_EQUAL_UINT32(0U, command_clear_requests(&s_rx));

    for (uint32_t i = 0U; i < 3U; i++) {
        command_on_cycle(&s_rx);
    }
    TEST_ASSERT_EQUAL(E2E_SM_INVALID, s_rx.sm.sm_state);

    periods_with_commands(2U, true, 42U); /* INVALID: not carried out */
    TEST_ASSERT_EQUAL_UINT32(0U, command_clear_requests(&s_rx));
    TEST_ASSERT_EQUAL_UINT8(7U, s_rx.clear_faults_seq);

    periods_with_commands(1U, true, 7U); /* VALID again, the value of the last one carried out */
    TEST_ASSERT_EQUAL(E2E_SM_VALID, s_rx.sm.sm_state);
    TEST_ASSERT_EQUAL_UINT32(0U, command_clear_requests(&s_rx));

    frame = command(true, 8U);
    period_with(&frame);
    period_with(&frame); /* the same frame again: REPEATED, not a second request */
    TEST_ASSERT_EQUAL_UINT32(1U, s_rx.status.repeated);
    TEST_ASSERT_EQUAL_UINT32(1U, command_clear_requests(&s_rx));
}

/* Tester A (faults sequence 7) goes away: three periods without a frame take the stream from
 * VALID to INVALID. */
static void lose_the_stream_of_tester_a(void)
{
    valid_with_telemetry_off();

    for (uint32_t i = 0U; i < 3U; i++) {
        command_on_cycle(&s_rx);
    }

    TEST_ASSERT_EQUAL(E2E_SM_INVALID, s_rx.sm.sm_state);
}

/*
 * Tester B starts after A went away, with a counter of its own from 0 and a value of its own, 0
 * (a pack master after a reset long enough to lose the stream is the same case). Its first
 * frame is out of sequence for the receiver, which still has A's counter (WRONGSEQUENCE,
 * neither an OK nor an error for the state machine), and the OKs after it make the stream
 * VALID again; the first command of B carried out then is compared with nothing: A's value,
 * from before the stream was lost, is no reference for it, and B clears nothing by starting.
 * B's own change afterwards is a request.
 */
static void test_a_tester_that_starts_after_another_went_away_requests_no_clear(void)
{
    uint32_t frames = 0U;

    lose_the_stream_of_tester_a();
    TEST_ASSERT_EQUAL(E2E_E_OK, e2e_p11_protect_init(&s_tester));

    while ((s_rx.sm.sm_state != E2E_SM_VALID) && (frames < 10U)) {
        periods_with_commands(1U, true, 0U);
        frames++;
    }

    TEST_ASSERT_EQUAL(E2E_SM_VALID, s_rx.sm.sm_state);
    TEST_ASSERT_EQUAL_UINT32(1U, s_rx.status.wrong_sequence);
    TEST_ASSERT_EQUAL_UINT8(0U, s_rx.clear_faults_seq);
    periods_with_commands(2U, true, 0U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, command_clear_requests(&s_rx),
                                     "a tester that starts requested a clear");

    periods_with_commands(1U, true, 1U);
    TEST_ASSERT_EQUAL_UINT32(1U, command_clear_requests(&s_rx));
}

/*
 * Tester A comes back after the timeout with its next counter but a value it changed while the
 * stream was lost (7 -> 8): the first command carried out after the timeout only sets the
 * reference, so nothing is requested. A sees FaultClearCount unchanged and changes the value
 * again (8 -> 9): that one is a request.
 */
static void test_a_tester_back_with_a_changed_value_requests_no_clear(void)
{
    lose_the_stream_of_tester_a();

    periods_with_commands(3U, true, 8U);
    TEST_ASSERT_EQUAL(E2E_SM_VALID, s_rx.sm.sm_state);
    TEST_ASSERT_EQUAL_UINT8(8U, s_rx.clear_faults_seq);
    TEST_ASSERT_EQUAL_UINT32(0U, command_clear_requests(&s_rx));

    periods_with_commands(1U, true, 9U);
    TEST_ASSERT_EQUAL_UINT32(1U, command_clear_requests(&s_rx));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_before_any_command_telemetry_is_on_and_nothing_is_known);
    RUN_TEST(test_without_a_tester_every_period_is_a_check_with_no_new_data);
    RUN_TEST(test_the_third_good_command_of_a_new_tester_is_the_first_carried_out);
    RUN_TEST(test_every_good_command_in_valid_is_carried_out);
    RUN_TEST(test_a_tester_that_stops_is_a_timeout_in_the_third_period_without_a_frame);
    RUN_TEST(test_after_a_timeout_three_good_commands_make_the_stream_valid_again);
    RUN_TEST(test_a_period_with_a_frame_makes_no_check_without_data);
    RUN_TEST(test_a_repeated_or_replayed_frame_is_not_carried_out);
    RUN_TEST(test_a_damaged_frame_is_not_carried_out_and_the_next_one_is);
    RUN_TEST(test_two_damaged_frames_in_the_window_make_the_stream_invalid);
    RUN_TEST(test_a_frame_of_another_message_is_an_error);
    RUN_TEST(test_a_frame_of_the_wrong_length_is_refused_and_counted_as_an_error);
    RUN_TEST(test_commands_before_the_stream_is_valid_change_nothing);
    RUN_TEST(test_the_counters_add_up);
    RUN_TEST(test_a_changed_clear_faults_seq_between_two_commands_carried_out_is_one_request);
    RUN_TEST(test_the_first_command_carried_out_requests_no_clear);
    RUN_TEST(test_frames_not_carried_out_request_no_clear);
    RUN_TEST(test_a_tester_that_starts_after_another_went_away_requests_no_clear);
    RUN_TEST(test_a_tester_back_with_a_changed_value_requests_no_clear);
    return UNITY_END();
}
