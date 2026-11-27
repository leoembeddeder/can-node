/* SPDX-License-Identifier: MIT */
/*
 * lib/uds: the server's core (the general response behaviour and the order of its checks,
 * suppressPosRspMsgIndicationBit, a request while one is pending), the sessions and their
 * timing (S3server, the responsePending cadence), and every service but the download, which
 * test_uds_download.c tests. On the fake clock and the fake backend of uds_test_support.h:
 * every time is the clock's, so every test is deterministic.
 */
#include <string.h>

#include "dtc/dtc.h"
#include "uds/uds.h"
#include "uds_test_support.h"
#include "unity.h"

#define LEN(a) ((uint32_t)sizeof(a))

void setUp(void)
{
    harness_init();
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g_fake.calls_out_of_order,
                                     "the server ran two backend operations at once");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g_fake.args_changed,
                                     "a poll had other arguments than its start");
}

/* A request of one or two bytes, for the tests that go through many. */
static uint32_t send1(uint8_t b0)
{
    const uint8_t req[1] = {b0};

    return send(req, 1U);
}

static uint32_t send2(uint8_t b0, uint8_t b1)
{
    const uint8_t req[2] = {b0, b1};

    return send(req, 2U);
}

static const uint8_t k_tp[] = {0x3E, 0x00};
static const uint8_t k_tp_rsp[] = {0x7E, 0x00};
static const uint8_t k_reset[] = {0x11, 0x01};
static const uint8_t k_reset_rsp[] = {0x51, 0x01};

/* ------------------------------------------------------------------------------- arguments */

static void test_init_refuses_null_pointers_and_an_incomplete_backend(void)
{
    static uds_server_t s;
    const uds_backend_t full = fake_backend();
    uds_backend_t b;

    TEST_ASSERT_FALSE(uds_init(NULL, &full));
    TEST_ASSERT_FALSE(uds_init(&s, NULL));

    for (uint32_t which = 0U; which < 9U; which++) {
        b = full;
        switch (which) {
        case 0U:
            b.ecu_reset = NULL;
            break;
        case 1U:
            b.boot_info = NULL;
            break;
        case 2U:
            b.dtc_snapshot = NULL;
            break;
        case 3U:
            b.dtc_clear = NULL;
            break;
        case 4U:
            b.erase = NULL;
            break;
        case 5U:
            b.write = NULL;
            break;
        case 6U:
            b.transfer_exit = NULL;
            break;
        case 7U:
            b.check = NULL;
            break;
        default:
            b.confirm = NULL;
            break;
        }
        (void)memset(&s, 0, sizeof(s));
        TEST_ASSERT_FALSE(uds_init(&s, &b));
        /* Nothing was prepared: the server refuses every call. */
        uds_output_t out;
        TEST_ASSERT_FALSE(uds_request(&s, k_tp, LEN(k_tp), g_now, g_rsp, UDS_RESPONSE_MAX, &out));
    }

    /* The context may be NULL: it is the backend's. */
    b = full;
    b.ctx = NULL;
    TEST_ASSERT_TRUE(uds_init(&s, &b));
}

static void test_calls_with_bad_arguments_do_nothing(void)
{
    static uds_server_t zero;
    uds_output_t out = {.len = 99U, .reset = true};
    uds_stats_t st;

    TEST_ASSERT_FALSE(uds_request(&g_srv, k_tp, 2U, g_now, g_rsp, UDS_NEGATIVE_LEN - 1U, &out));
    TEST_ASSERT_EQUAL_UINT32(0U, out.len);
    TEST_ASSERT_FALSE(out.reset);
    TEST_ASSERT_FALSE(uds_request(&g_srv, k_tp, 2U, g_now, NULL, UDS_RESPONSE_MAX, &out));
    TEST_ASSERT_FALSE(uds_request(&g_srv, NULL, 2U, g_now, g_rsp, UDS_RESPONSE_MAX, &out));
    TEST_ASSERT_FALSE(uds_request(NULL, k_tp, 2U, g_now, g_rsp, UDS_RESPONSE_MAX, &out));
    TEST_ASSERT_FALSE(uds_request(&g_srv, k_tp, 2U, g_now, g_rsp, UDS_RESPONSE_MAX, NULL));
    TEST_ASSERT_FALSE(uds_poll(NULL, g_now, g_rsp, UDS_RESPONSE_MAX, &out));
    TEST_ASSERT_FALSE(uds_poll(&g_srv, g_now, NULL, UDS_RESPONSE_MAX, &out));
    TEST_ASSERT_FALSE(uds_poll(&g_srv, g_now, g_rsp, 2U, &out));
    TEST_ASSERT_FALSE(uds_poll(&g_srv, g_now, g_rsp, UDS_RESPONSE_MAX, NULL));

    /* None of them counted anything. */
    st = stats();
    for (uint32_t i = 0U; i < UDS_SERVICE_COUNT; i++) {
        TEST_ASSERT_EQUAL_UINT32(0U, st.requests[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, st.positive);

    /* A server uds_init() never prepared. */
    (void)memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_FALSE(uds_request(&zero, k_tp, 2U, g_now, g_rsp, UDS_RESPONSE_MAX, &out));
    TEST_ASSERT_FALSE(uds_poll(&zero, g_now, g_rsp, UDS_RESPONSE_MAX, &out));
    TEST_ASSERT_EQUAL_UINT64(UDS_NO_DEADLINE, uds_next_deadline(&zero));
    TEST_ASSERT_EQUAL_UINT64(UDS_NO_DEADLINE, uds_next_deadline(NULL));
    TEST_ASSERT_FALSE(uds_busy(&zero));
    TEST_ASSERT_FALSE(uds_busy(NULL));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_DEFAULT, uds_session(&zero));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_DEFAULT, uds_session(NULL));
    (void)memset(&st, 0xAA, sizeof(st));
    uds_get_stats(&zero, &st);
    TEST_ASSERT_EQUAL_UINT32(0U, st.positive);
    TEST_ASSERT_EQUAL_UINT32(0U, st.requests[0]);
    (void)memset(&st, 0xAA, sizeof(st));
    uds_get_stats(NULL, &st);
    TEST_ASSERT_EQUAL_UINT32(0U, st.response_pending);
    uds_get_stats(&g_srv, NULL);

    /* No bytes and no buffer is an empty request, which is ignored. */
    TEST_ASSERT_TRUE(uds_request(&g_srv, NULL, 0U, g_now, g_rsp, UDS_RESPONSE_MAX, &out));
    TEST_ASSERT_EQUAL_UINT32(0U, out.len);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().requests_empty);
}

/* A request of 0 bytes has no SID: nothing is sent, nothing counted but requests_empty, and it
 * does not restart S3server. */
static void test_an_empty_request_is_ignored(void)
{
    const uint8_t none[1] = {0x3E};

    enter_session(UDS_SESSION_EXTENDED);
    const uint64_t start = g_now;

    g_now = start + (4000U * MS);
    TEST_ASSERT_EQUAL_UINT32(0U, send(none, 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, stats().requests_empty);
    TEST_ASSERT_EQUAL_UINT32(0U, stats().requests[9]);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().positive); /* the session's response only */
    (void)poll_at(start + (5000U * MS));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_DEFAULT, uds_session(&g_srv));
}

/* --------------------------------------------- the general response behaviour, step by step */

/* Step 1: while a request is pending, every other one is busyRepeatRequest, before any other
 * check; the pending one goes on and ends with its own response. */
static void test_step1_a_request_while_one_is_pending_is_busy_repeat_request(void)
{
    g_fake.ecu_reset.pending = 1000U;
    TEST_ASSERT_EQUAL_UINT32(0U, send(k_reset, LEN(k_reset)));
    TEST_ASSERT_TRUE(uds_busy(&g_srv));

    (void)send1(0x99); /* not supported */
    expect_nrc(0x99, UDS_NRC_BUSY_REPEAT_REQUEST);
    (void)send1(0x34); /* not in the default session, and too short */
    expect_nrc(0x34, UDS_NRC_BUSY_REPEAT_REQUEST);
    (void)send1(0x10); /* too short for its sub-function */
    expect_nrc(0x10, UDS_NRC_BUSY_REPEAT_REQUEST);
    (void)send2(0x10, 0x05); /* a sub-function not supported */
    expect_nrc(0x10, UDS_NRC_BUSY_REPEAT_REQUEST);
    (void)send2(0x3E, 0x80); /* the SPRMIB does not suppress a negative response */
    expect_nrc(0x3E, UDS_NRC_BUSY_REPEAT_REQUEST);
    (void)send(k_reset, LEN(k_reset)); /* the same request again */
    expect_nrc(0x11, UDS_NRC_BUSY_REPEAT_REQUEST);

    /* The pending request was not touched: one start, and it ends with its response. */
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.ecu_reset.starts);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.ecu_reset.polls);
    g_fake.ecu_reset.left = 0U;
    TEST_ASSERT_EQUAL_UINT32(2U, poll_at(g_now + (1U * MS)));
    expect_rsp(k_reset_rsp, LEN(k_reset_rsp));
    TEST_ASSERT_TRUE(g_out.reset);
    TEST_ASSERT_FALSE(uds_busy(&g_srv));

    const uds_stats_t st = stats();

    TEST_ASSERT_EQUAL_UINT32(6U, nrc_count(UDS_NRC_BUSY_REPEAT_REQUEST));
    TEST_ASSERT_EQUAL_UINT32(1U, st.requests_unsupported);
    TEST_ASSERT_EQUAL_UINT32(2U, st.requests[0]); /* 0x10 */
    TEST_ASSERT_EQUAL_UINT32(2U, st.requests[1]); /* 0x11, both counted at their arrival */
    TEST_ASSERT_EQUAL_UINT32(1U, st.requests[6]); /* 0x34 */
    TEST_ASSERT_EQUAL_UINT32(1U, st.requests[9]); /* 0x3E */
    TEST_ASSERT_EQUAL_UINT32(1U, st.positive);

    /* Once it is done, the next request is processed. */
    send_expect(k_tp, LEN(k_tp), k_tp_rsp, LEN(k_tp_rsp));
}

/* Step 2: a SID that is not one of the services is serviceNotSupported, whatever follows it and
 * in every session. */
static void test_step2_an_unknown_sid_is_service_not_supported(void)
{
    static const uint8_t sids[] = {0x00, 0x27, 0x28, 0x29, 0x2E, 0x35, 0x3F, 0x50,
                                   0x62, 0x7F, 0x85, 0x86, 0xFF, 0x12, 0x23, 0x38};
    static const uint8_t sessions[] = {UDS_SESSION_DEFAULT, UDS_SESSION_EXTENDED,
                                       UDS_SESSION_PROGRAMMING};

    for (uint32_t k = 0U; k < LEN(sessions); k++) {
        enter_session(sessions[k]);
        for (uint32_t i = 0U; i < LEN(sids); i++) {
            const uint8_t longer[5] = {sids[i], 0x01, 0x02, 0x03, 0x04};

            (void)send1(sids[i]);
            expect_nrc(sids[i], UDS_NRC_SERVICE_NOT_SUPPORTED);
            (void)send(longer, LEN(longer));
            expect_nrc(sids[i], UDS_NRC_SERVICE_NOT_SUPPORTED);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(2U * LEN(sids) * LEN(sessions),
                             nrc_count(UDS_NRC_SERVICE_NOT_SUPPORTED));
    TEST_ASSERT_EQUAL_UINT32(2U * LEN(sids) * LEN(sessions), stats().requests_unsupported);
}

/* Step 3: a service outside its sessions is serviceNotSupportedInActiveSession, before its
 * length and its sub-function are looked at. */
static void test_step3_a_service_outside_its_sessions_comes_before_length_and_sub_function(void)
{
    static const uint8_t erase[] = {0x31, 0x01, 0xFF, 0x00, 0x44, 0x00, 0x10,
                                    0x20, 0x00, 0x00, 0x00, 0x10, 0x00};

    /* Default: neither RoutineControl nor the download services. */
    (void)send1(0x31);
    expect_nrc(0x31, UDS_NRC_SERVICE_NOT_IN_SESSION);
    (void)send2(0x31, 0x02);
    expect_nrc(0x31, UDS_NRC_SERVICE_NOT_IN_SESSION);
    send_expect_nrc(erase, LEN(erase), UDS_NRC_SERVICE_NOT_IN_SESSION);
    (void)send1(0x34);
    expect_nrc(0x34, UDS_NRC_SERVICE_NOT_IN_SESSION);
    (void)send2(0x36, 0x01);
    expect_nrc(0x36, UDS_NRC_SERVICE_NOT_IN_SESSION);
    (void)send1(0x37);
    expect_nrc(0x37, UDS_NRC_SERVICE_NOT_IN_SESSION);

    /* Extended: the download services are the programming session's. */
    enter_session(UDS_SESSION_EXTENDED);
    (void)send1(0x34);
    expect_nrc(0x34, UDS_NRC_SERVICE_NOT_IN_SESSION);
    (void)send2(0x36, 0x01);
    expect_nrc(0x36, UDS_NRC_SERVICE_NOT_IN_SESSION);
    (void)send1(0x37);
    expect_nrc(0x37, UDS_NRC_SERVICE_NOT_IN_SESSION);

    /* Programming: no DTC services. */
    enter_session(UDS_SESSION_PROGRAMMING);
    (void)send1(0x19);
    expect_nrc(0x19, UDS_NRC_SERVICE_NOT_IN_SESSION);
    (void)send2(0x19, 0x55);
    expect_nrc(0x19, UDS_NRC_SERVICE_NOT_IN_SESSION);
    (void)send1(0x14);
    expect_nrc(0x14, UDS_NRC_SERVICE_NOT_IN_SESSION);

    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.erase.starts);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.dtc_snapshot.starts);
    TEST_ASSERT_EQUAL_UINT32(12U, nrc_count(UDS_NRC_SERVICE_NOT_IN_SESSION));
}

/* Step 4: a service with a sub-function needs 2 bytes. */
static void test_step4_a_service_with_a_sub_function_needs_two_bytes(void)
{
    static const uint8_t sids[] = {0x10, 0x11, 0x19, 0x3E};

    for (uint32_t i = 0U; i < LEN(sids); i++) {
        (void)send1(sids[i]);
        expect_nrc(sids[i], UDS_NRC_INCORRECT_LENGTH);
    }
    enter_session(UDS_SESSION_EXTENDED);
    (void)send1(0x31);
    expect_nrc(0x31, UDS_NRC_INCORRECT_LENGTH);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.ecu_reset.starts);
}

/* Step 5: a sub-function the service does not have is subFunctionNotSupported, before its
 * session and the service's length checks. Bit 7 is the SPRMIB for 0x10, 0x11, 0x31 and 0x3E and
 * is removed first; for 0x19 it is part of the reportType. */
static void test_step5_an_unknown_sub_function_comes_before_its_session_and_the_length(void)
{
    static const uint8_t session_subs[] = {0x00, 0x04, 0x05, 0x40, 0x7F, 0x84, 0xFF};
    static const uint8_t reset_subs[] = {0x00, 0x02, 0x03, 0x04, 0x05, 0x60, 0x82, 0x83};
    static const uint8_t tester_subs[] = {0x01, 0x02, 0x7F, 0x81};
    static const uint8_t dtc_subs[] = {0x00, 0x03, 0x04, 0x06, 0x0B, 0x14, 0x81, 0x82, 0x8A};
    static const uint8_t routine_subs[] = {0x00, 0x02, 0x04, 0x7F, 0x82};

    for (uint32_t i = 0U; i < LEN(session_subs); i++) {
        const uint8_t longer[4] = {0x10, session_subs[i], 0x00, 0x00};

        (void)send2(0x10, session_subs[i]);
        expect_nrc(0x10, UDS_NRC_SUBFUNCTION_UNSUPPORTED);
        (void)send(longer, LEN(longer)); /* the wrong length is not what is reported */
        expect_nrc(0x10, UDS_NRC_SUBFUNCTION_UNSUPPORTED);
    }
    for (uint32_t i = 0U; i < LEN(reset_subs); i++) {
        (void)send2(0x11, reset_subs[i]);
        expect_nrc(0x11, UDS_NRC_SUBFUNCTION_UNSUPPORTED);
    }
    for (uint32_t i = 0U; i < LEN(tester_subs); i++) {
        (void)send2(0x3E, tester_subs[i]);
        expect_nrc(0x3E, UDS_NRC_SUBFUNCTION_UNSUPPORTED);
    }
    for (uint32_t i = 0U; i < LEN(dtc_subs); i++) {
        const uint8_t req[3] = {0x19, dtc_subs[i], 0xFF};

        (void)send(req, LEN(req));
        expect_nrc(0x19, UDS_NRC_SUBFUNCTION_UNSUPPORTED);
    }
    enter_session(UDS_SESSION_EXTENDED);
    for (uint32_t i = 0U; i < LEN(routine_subs); i++) {
        const uint8_t req[4] = {0x31, routine_subs[i], 0xF0, 0x00};

        (void)send(req, LEN(req));
        expect_nrc(0x31, UDS_NRC_SUBFUNCTION_UNSUPPORTED);
    }
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_EXTENDED, uds_session(&g_srv));
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.ecu_reset.starts);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.dtc_snapshot.starts);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.confirm.starts);
}

/* Step 6: a sub-function outside its sessions is subFunctionNotSupportedInActiveSession, before
 * the service's length check; the SPRMIB does not suppress it. */
static void test_step6_a_sub_function_outside_its_sessions_comes_before_the_length(void)
{
    static const uint8_t prog_longer[3] = {0x10, 0x02, 0x00};
    static const uint8_t ext_longer[3] = {0x10, 0x83, 0x00};

    (void)send2(0x10, 0x02);
    expect_nrc(0x10, UDS_NRC_SUBFUNCTION_NOT_IN_SESSION);
    send_expect_nrc(prog_longer, LEN(prog_longer), UDS_NRC_SUBFUNCTION_NOT_IN_SESSION);
    (void)send2(0x10, 0x82);
    expect_nrc(0x10, UDS_NRC_SUBFUNCTION_NOT_IN_SESSION);
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_DEFAULT, uds_session(&g_srv));

    enter_session(UDS_SESSION_PROGRAMMING);
    (void)send2(0x10, 0x03);
    expect_nrc(0x10, UDS_NRC_SUBFUNCTION_NOT_IN_SESSION);
    send_expect_nrc(ext_longer, LEN(ext_longer), UDS_NRC_SUBFUNCTION_NOT_IN_SESSION);
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_PROGRAMMING, uds_session(&g_srv));
    TEST_ASSERT_EQUAL_UINT32(5U, nrc_count(UDS_NRC_SUBFUNCTION_NOT_IN_SESSION));
}

/* Step 7: then the service's own checks, here the length; the SPRMIB does not suppress them. */
static void test_step7_the_service_checks_follow_the_general_ones(void)
{
    static const uint8_t reqs[][3] = {{0x10, 0x01, 0x00}, {0x10, 0x81, 0x00}, {0x11, 0x01, 0x00},
                                      {0x11, 0x81, 0x00}, {0x3E, 0x00, 0x00}, {0x3E, 0x80, 0x00}};

    for (uint32_t i = 0U; i < (LEN(reqs) / LEN(reqs[0])); i++) {
        send_expect_nrc(reqs[i], 3U, UDS_NRC_INCORRECT_LENGTH);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.ecu_reset.starts);
    TEST_ASSERT_EQUAL_UINT32(0U, stats().suppressed);
}

/* Step 8: a positive response that does not fit the caller's buffer is responseTooLong; the
 * service was carried out (here the session changed). */
static void test_step8_a_response_longer_than_the_buffer_is_response_too_long(void)
{
    static const uint8_t ext[] = {0x10, 0x03};
    static const uint8_t ext_rsp[] = {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};

    g_rsp_cap = 5U;
    send_expect_nrc(ext, LEN(ext), UDS_NRC_RESPONSE_TOO_LONG);
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_EXTENDED, uds_session(&g_srv));
    g_rsp_cap = 6U;
    send_expect(ext, LEN(ext), ext_rsp, LEN(ext_rsp));
    g_rsp_cap = UDS_NEGATIVE_LEN;
    send_expect(k_tp, LEN(k_tp), k_tp_rsp, LEN(k_tp_rsp));
    TEST_ASSERT_EQUAL_UINT32(1U, nrc_count(UDS_NRC_RESPONSE_TOO_LONG));
}

/* ------------------------------------------------------------------------------- sessions */

/* Every transition of uds.h's table, with the response that reports the timing. */
static void test_session_transitions(void)
{
    static const struct {
        uint8_t from;
        uint8_t to;
        bool allowed;
    } cases[] = {
        {UDS_SESSION_DEFAULT, UDS_SESSION_DEFAULT, true},
        {UDS_SESSION_DEFAULT, UDS_SESSION_PROGRAMMING, false},
        {UDS_SESSION_DEFAULT, UDS_SESSION_EXTENDED, true},
        {UDS_SESSION_PROGRAMMING, UDS_SESSION_DEFAULT, true},
        {UDS_SESSION_PROGRAMMING, UDS_SESSION_PROGRAMMING, true},
        {UDS_SESSION_PROGRAMMING, UDS_SESSION_EXTENDED, false},
        {UDS_SESSION_EXTENDED, UDS_SESSION_DEFAULT, true},
        {UDS_SESSION_EXTENDED, UDS_SESSION_PROGRAMMING, true},
        {UDS_SESSION_EXTENDED, UDS_SESSION_EXTENDED, true},
    };

    for (uint32_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        harness_init();
        enter_session(cases[i].from);
        (void)send2(0x10, cases[i].to);
        if (cases[i].allowed) {
            const uint8_t rsp[] = {0x50, cases[i].to, 0x00, 0x32, 0x01, 0xF4};

            expect_rsp(rsp, LEN(rsp));
            TEST_ASSERT_EQUAL_UINT8(cases[i].to, uds_session(&g_srv));
        } else {
            expect_nrc(0x10, UDS_NRC_SUBFUNCTION_NOT_IN_SESSION);
            TEST_ASSERT_EQUAL_UINT8(cases[i].from, uds_session(&g_srv));
        }
    }
}

/* P2server_max 50 ms (0x0032, 1 ms) and P2*server_max 5000 ms (0x01F4, 10 ms). */
static void test_session_response_reports_p2_and_p2_star(void)
{
    static const uint8_t def[] = {0x10, 0x01};
    static const uint8_t def_rsp[] = {0x50, 0x01, 0x00, 0x32, 0x01, 0xF4};

    send_expect(def, LEN(def), def_rsp, LEN(def_rsp));
    TEST_ASSERT_EQUAL_UINT32(50U, UDS_P2_SERVER_MS);
    TEST_ASSERT_EQUAL_UINT32(5000U, UDS_P2_STAR_SERVER_MS);
}

/* A change is counted when the session differs; a request for the active one is none. */
static void test_session_changes_are_counted_when_the_session_differs(void)
{
    (void)send2(0x10, 0x01);
    TEST_ASSERT_EQUAL_UINT32(0U, stats().session_changes);
    (void)send2(0x10, 0x03);
    (void)send2(0x10, 0x03);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().session_changes);
    (void)send2(0x10, 0x02);
    (void)send2(0x10, 0x02);
    (void)send2(0x10, 0x01);
    TEST_ASSERT_EQUAL_UINT32(3U, stats().session_changes);
    TEST_ASSERT_EQUAL_UINT32(6U, stats().positive);
}

/* With the SPRMIB the session changes and nothing is sent. */
static void test_a_suppressed_session_control_changes_the_session_silently(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, send2(0x10, 0x83));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_EXTENDED, uds_session(&g_srv));
    TEST_ASSERT_EQUAL_UINT32(0U, send2(0x10, 0x82));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_PROGRAMMING, uds_session(&g_srv));
    TEST_ASSERT_EQUAL_UINT32(0U, send2(0x10, 0x81));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_DEFAULT, uds_session(&g_srv));
    TEST_ASSERT_FALSE(g_out.reset);
    TEST_ASSERT_EQUAL_UINT32(3U, stats().suppressed);
    TEST_ASSERT_EQUAL_UINT32(0U, stats().positive);
}

/* DID 0xF186 is the active session, in every session. */
static void test_active_session_did_in_every_session(void)
{
    static const uint8_t req[] = {0x22, 0xF1, 0x86};
    static const uint8_t sessions[] = {UDS_SESSION_DEFAULT, UDS_SESSION_EXTENDED,
                                       UDS_SESSION_PROGRAMMING};

    for (uint32_t i = 0U; i < LEN(sessions); i++) {
        const uint8_t rsp[] = {0x62, 0xF1, 0x86, sessions[i]};

        enter_session(sessions[i]);
        send_expect(req, LEN(req), rsp, LEN(rsp));
    }
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.boot_info.starts);
}

/* ------------------------------------------------------------------------------- S3server */

/* 5000 ms after the last response the session falls back, at exactly that time and not before;
 * the deadline names it. */
static void test_s3_returns_to_the_default_session_after_5000_ms(void)
{
    enter_session(UDS_SESSION_EXTENDED);
    const uint64_t start = g_now;

    TEST_ASSERT_EQUAL_UINT64(start + (5000U * MS), uds_next_deadline(&g_srv));
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(start + (5000U * MS) - 1U));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_EXTENDED, uds_session(&g_srv));
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(start + (5000U * MS)));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_DEFAULT, uds_session(&g_srv));
    TEST_ASSERT_EQUAL_UINT32(1U, stats().s3_timeouts);
    TEST_ASSERT_EQUAL_UINT32(2U, stats().session_changes);
    TEST_ASSERT_EQUAL_UINT64(UDS_NO_DEADLINE, uds_next_deadline(&g_srv));

    /* Nothing more happens in the default session. */
    (void)poll_at(start + (60000U * MS));
    TEST_ASSERT_EQUAL_UINT32(1U, stats().s3_timeouts);
}

/* Every request restarts S3server from its response, a refused one too. */
static void test_s3_runs_from_the_end_of_every_request(void)
{
    enter_session(UDS_SESSION_EXTENDED);
    const uint64_t start = g_now;

    g_now = start + (4000U * MS);
    send_expect(k_tp, LEN(k_tp), k_tp_rsp, LEN(k_tp_rsp));
    TEST_ASSERT_EQUAL_UINT64(start + (9000U * MS), uds_next_deadline(&g_srv));
    (void)poll_at(start + (8999U * MS));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_EXTENDED, uds_session(&g_srv));

    g_now = start + (8999U * MS);
    (void)send1(0x99);
    expect_nrc(0x99, UDS_NRC_SERVICE_NOT_SUPPORTED);
    (void)poll_at(start + (13998U * MS));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_EXTENDED, uds_session(&g_srv));
    (void)poll_at(start + (13999U * MS));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_DEFAULT, uds_session(&g_srv));
}

/* A request that comes after the timeout, with no poll in between, finds the default session. */
static void test_s3_is_checked_before_a_request(void)
{
    static const uint8_t confirm[] = {0x31, 0x01, 0xF0, 0x00};

    enter_session(UDS_SESSION_EXTENDED);
    g_now += 6000U * MS;
    send_expect_nrc(confirm, LEN(confirm), UDS_NRC_SERVICE_NOT_IN_SESSION);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().s3_timeouts);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.confirm.starts);
}

static void test_s3_does_not_run_in_the_default_session(void)
{
    send_expect(k_tp, LEN(k_tp), k_tp_rsp, LEN(k_tp_rsp));
    TEST_ASSERT_EQUAL_UINT64(UDS_NO_DEADLINE, uds_next_deadline(&g_srv));
    (void)poll_at(g_now + (3600U * MS * 1000U));
    TEST_ASSERT_EQUAL_UINT32(0U, stats().s3_timeouts);
    TEST_ASSERT_EQUAL_UINT32(0U, stats().session_changes);
}

/* While a request is pending S3server does not run; it starts again at its final response. */
static void test_s3_does_not_run_while_a_request_is_pending(void)
{
    enter_session(UDS_SESSION_EXTENDED);
    const uint64_t start = g_now;

    g_fake.ecu_reset.pending = 1000U;
    TEST_ASSERT_EQUAL_UINT32(0U, send(k_reset, LEN(k_reset)));
    for (uint32_t t = 1U; t <= 12U; t++) {
        (void)poll_at(start + ((uint64_t)t * 1000U * MS));
    }
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_EXTENDED, uds_session(&g_srv));
    TEST_ASSERT_EQUAL_UINT32(0U, stats().s3_timeouts);
    g_fake.ecu_reset.left = 0U;
    (void)poll_at(start + (12500U * MS));
    expect_rsp(k_reset_rsp, LEN(k_reset_rsp));
    TEST_ASSERT_EQUAL_UINT64(start + (17500U * MS), uds_next_deadline(&g_srv));
    (void)poll_at(start + (17500U * MS));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_DEFAULT, uds_session(&g_srv));
}

/* TesterPresent with the SPRMIB keeps the session without a response. */
static void test_suppressed_tester_present_keeps_the_session(void)
{
    enter_session(UDS_SESSION_EXTENDED);
    const uint64_t start = g_now;

    for (uint32_t t = 1U; t <= 5U; t++) {
        g_now = start + ((uint64_t)t * 4000U * MS);
        TEST_ASSERT_EQUAL_UINT32(0U, send2(0x3E, 0x80));
        (void)poll_at(g_now + (3999U * MS));
        TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_EXTENDED, uds_session(&g_srv));
    }
    TEST_ASSERT_EQUAL_UINT32(5U, stats().suppressed);
    (void)poll_at(start + (25000U * MS));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_DEFAULT, uds_session(&g_srv));
}

/* ----------------------------------------------------------------------- responsePending */

/* The first at 40 ms after the request, then 2000 ms after each one, until the final response;
 * the deadline names each. */
static void test_response_pending_first_at_40_ms_then_every_2000_ms(void)
{
    const uint64_t t = g_now;

    g_fake.ecu_reset.pending = 1000000U;
    TEST_ASSERT_EQUAL_UINT32(0U, send(k_reset, LEN(k_reset)));
    TEST_ASSERT_EQUAL_UINT64(t + (40U * MS), uds_next_deadline(&g_srv));
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(t + (40U * MS) - 1U));
    (void)poll_at(t + (40U * MS));
    expect_pending(0x11);
    TEST_ASSERT_EQUAL_UINT64(t + (2040U * MS), uds_next_deadline(&g_srv));
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(t + (2040U * MS) - 1U));
    (void)poll_at(t + (2040U * MS));
    expect_pending(0x11);
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(t + (3000U * MS)));
    (void)poll_at(t + (4040U * MS));
    expect_pending(0x11);
    TEST_ASSERT_EQUAL_UINT64(t + (6040U * MS), uds_next_deadline(&g_srv));

    g_fake.ecu_reset.left = 0U;
    (void)poll_at(t + (5000U * MS));
    expect_rsp(k_reset_rsp, LEN(k_reset_rsp));
    TEST_ASSERT_TRUE(g_out.reset);

    const uds_stats_t st = stats();

    TEST_ASSERT_EQUAL_UINT32(3U, st.response_pending);
    TEST_ASSERT_EQUAL_UINT32(0U, nrc_count(UDS_NRC_RESPONSE_PENDING)); /* not among the NRCs */
    TEST_ASSERT_EQUAL_UINT64(5000U * MS, st.pending_max_us);
    TEST_ASSERT_EQUAL_UINT32(1U, st.positive);
    TEST_ASSERT_EQUAL_UINT32(1U, st.resets);
}

/* A backend that finishes before 40 ms causes no responsePending. */
static void test_no_response_pending_when_the_backend_answers_in_time(void)
{
    const uint64_t t = g_now;

    g_fake.ecu_reset.pending = 3U;
    TEST_ASSERT_EQUAL_UINT32(0U, send(k_reset, LEN(k_reset)));
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(t + (10U * MS)));
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(t + (20U * MS)));
    (void)poll_at(t + (40U * MS) - 1U);
    expect_rsp(k_reset_rsp, LEN(k_reset_rsp));
    TEST_ASSERT_EQUAL_UINT32(0U, stats().response_pending);
    TEST_ASSERT_EQUAL_UINT32(3U, g_fake.ecu_reset.polls);

    /* Finishing at the very poll at which one would be due: the final response wins. */
    g_fake.ecu_reset.pending = 1U;
    const uint64_t t2 = g_now;

    TEST_ASSERT_EQUAL_UINT32(0U, send(k_reset, LEN(k_reset)));
    (void)poll_at(t2 + (40U * MS));
    expect_rsp(k_reset_rsp, LEN(k_reset_rsp));
    TEST_ASSERT_EQUAL_UINT32(0U, stats().response_pending);
}

/* A late poll sends the responsePending late, and the next one counts from it. */
static void test_the_next_response_pending_counts_from_the_one_before(void)
{
    const uint64_t t = g_now;

    g_fake.ecu_reset.pending = 1000U;
    (void)send(k_reset, LEN(k_reset));
    (void)poll_at(t + (45U * MS));
    expect_pending(0x11);
    TEST_ASSERT_EQUAL_UINT64(t + (2045U * MS), uds_next_deadline(&g_srv));
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(t + (2045U * MS) - 1U));
    (void)poll_at(t + (2045U * MS));
    expect_pending(0x11);
}

/* A suppressed request answers nothing if it finishes in time, and its positive response once a
 * responsePending has gone out; the reset is reported either way. */
static void test_a_suppressed_request_gets_its_response_after_a_response_pending(void)
{
    const uint8_t reset_spr[] = {0x11, 0x81};
    uint64_t t = g_now;

    g_fake.ecu_reset.pending = 1U;
    TEST_ASSERT_EQUAL_UINT32(0U, send(reset_spr, LEN(reset_spr)));
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(t + (10U * MS)));
    TEST_ASSERT_TRUE(g_out.reset);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().suppressed);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().resets);

    t = g_now;
    g_fake.ecu_reset.pending = 1000U;
    TEST_ASSERT_EQUAL_UINT32(0U, send(reset_spr, LEN(reset_spr)));
    (void)poll_at(t + (40U * MS));
    expect_pending(0x11);
    g_fake.ecu_reset.left = 0U;
    (void)poll_at(t + (100U * MS));
    expect_rsp(k_reset_rsp, LEN(k_reset_rsp));
    TEST_ASSERT_TRUE(g_out.reset);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().suppressed);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().positive);
}

/* A request that arrives while one is pending changes nothing of the pending one's cadence. */
static void test_a_second_request_does_not_move_the_cadence(void)
{
    static const uint8_t did[] = {0x22, 0xF1, 0x86};
    const uint64_t t = g_now;

    g_fake.ecu_reset.pending = 1000U;
    (void)send(k_reset, LEN(k_reset));
    g_now = t + (20U * MS);
    send_expect_nrc(did, LEN(did), UDS_NRC_BUSY_REPEAT_REQUEST);
    TEST_ASSERT_EQUAL_UINT64(t + (40U * MS), uds_next_deadline(&g_srv));
    (void)poll_at(t + (40U * MS));
    expect_pending(0x11);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.boot_info.starts);
}

/* uds_poll() without a pending request and without S3server does nothing. */
static void test_a_poll_with_nothing_pending_sends_nothing(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(g_now + (100U * MS)));
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(g_now + (100000U * MS)));
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.ecu_reset.polls);
}

/* A time earlier than one the server has seen counts as that time. */
static void test_time_never_goes_back(void)
{
    const uint64_t t = g_now + (10000U * MS);

    g_now = t;
    g_fake.ecu_reset.pending = 1000U;
    (void)send(k_reset, LEN(k_reset));
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(T0_US)); /* counts as t: nothing due */
    TEST_ASSERT_EQUAL_UINT64(t + (40U * MS), uds_next_deadline(&g_srv));
    (void)poll_at(t + (40U * MS));
    expect_pending(0x11);
    g_fake.ecu_reset.left = 0U;
    (void)poll_at(T0_US); /* counts as t + 40 ms */
    expect_rsp(k_reset_rsp, LEN(k_reset_rsp));
    TEST_ASSERT_EQUAL_UINT64(40U * MS, stats().pending_max_us);
}

/* -------------------------------------------------------------------------------- ECUReset */

static void test_ecu_reset_answers_then_reports_the_reset_in_every_session(void)
{
    static const uint8_t sessions[] = {UDS_SESSION_DEFAULT, UDS_SESSION_EXTENDED,
                                       UDS_SESSION_PROGRAMMING};

    for (uint32_t i = 0U; i < LEN(sessions); i++) {
        harness_init();
        enter_session(sessions[i]);
        send_expect(k_reset, LEN(k_reset), k_reset_rsp, LEN(k_reset_rsp));
        TEST_ASSERT_TRUE(g_out.reset);
        TEST_ASSERT_EQUAL_UINT8(UDS_RESET_HARD, g_fake.reset_type);
        TEST_ASSERT_EQUAL_UINT32(1U, stats().resets);
    }
}

static void test_ecu_reset_refused_by_the_backend_resets_nothing(void)
{
    g_fake.ecu_reset.nrc = UDS_NRC_CONDITIONS_NOT_CORRECT;
    send_expect_nrc(k_reset, LEN(k_reset), UDS_NRC_CONDITIONS_NOT_CORRECT);
    TEST_ASSERT_FALSE(g_out.reset);
    TEST_ASSERT_EQUAL_UINT32(0U, stats().resets);

    /* With the SPRMIB the refusal is sent all the same. */
    const uint8_t spr[] = {0x11, 0x81};

    send_expect_nrc(spr, LEN(spr), UDS_NRC_CONDITIONS_NOT_CORRECT);
}

/* With the SPRMIB the reset is reported with nothing to send before it. */
static void test_a_suppressed_ecu_reset_still_resets(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, send2(0x11, 0x81));
    TEST_ASSERT_TRUE(g_out.reset);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().resets);
}

/* An NRC from the backend that the server does not name is generalReject. */
static void test_a_backend_nrc_the_server_does_not_name_is_general_reject(void)
{
    static const uint8_t unknown[] = {UDS_NRC_RESPONSE_PENDING, 0x33, 0x99, 0x01, 0xFF};
    static const uint8_t named[] = {UDS_NRC_CONDITIONS_NOT_CORRECT, UDS_NRC_BUSY_REPEAT_REQUEST,
                                    UDS_NRC_REQUEST_OUT_OF_RANGE, UDS_NRC_GENERAL_REJECT};

    for (uint32_t i = 0U; i < LEN(unknown); i++) {
        g_fake.ecu_reset.nrc = unknown[i];
        send_expect_nrc(k_reset, LEN(k_reset), UDS_NRC_GENERAL_REJECT);
    }
    for (uint32_t i = 0U; i < LEN(named); i++) {
        g_fake.ecu_reset.nrc = named[i];
        send_expect_nrc(k_reset, LEN(k_reset), named[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, stats().response_pending);
}

/* --------------------------------------------------------------------------- TesterPresent */

static void test_tester_present_in_every_session(void)
{
    static const uint8_t sessions[] = {UDS_SESSION_DEFAULT, UDS_SESSION_EXTENDED,
                                       UDS_SESSION_PROGRAMMING};

    for (uint32_t i = 0U; i < LEN(sessions); i++) {
        enter_session(sessions[i]);
        send_expect(k_tp, LEN(k_tp), k_tp_rsp, LEN(k_tp_rsp));
    }
}

/* -------------------------------------------------------------------- ReadDataByIdentifier */

static void test_read_did_length_and_identifier_checks(void)
{
    static const uint8_t short1[] = {0x22};
    static const uint8_t short2[] = {0x22, 0xF1};
    static const uint8_t two_dids[] = {0x22, 0xF1, 0x86, 0xF1, 0x89};
    static const uint8_t odd[] = {0x22, 0xF1, 0x86, 0x00};
    static const uint16_t unknown[] = {0x0000, 0xF180, 0xF187, 0xF18A, 0xF190,
                                       0xFD01, 0xFCFF, 0xFF00, 0xFFFF, 0x0186};

    send_expect_nrc(short1, LEN(short1), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(short2, LEN(short2), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(two_dids, LEN(two_dids), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(odd, LEN(odd), UDS_NRC_INCORRECT_LENGTH);
    for (uint32_t i = 0U; i < (sizeof(unknown) / sizeof(unknown[0])); i++) {
        const uint8_t req[3] = {0x22, (uint8_t)(unknown[i] >> 8U), (uint8_t)unknown[i]};

        send_expect_nrc(req, LEN(req), UDS_NRC_REQUEST_OUT_OF_RANGE);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.boot_info.starts);
}

static void read_version(uint32_t version, const char *text)
{
    static const uint8_t req[] = {0x22, 0xF1, 0x89};
    uint8_t rsp[32] = {0x62, 0xF1, 0x89};
    const uint32_t n = (uint32_t)strlen(text);

    for (uint32_t i = 0U; i < n; i++) {
        rsp[3U + i] = (uint8_t)text[i];
    }
    g_fake.info.handoff = true;
    g_fake.info.image_version = version;
    send_expect(req, LEN(req), rsp, 3U + n);
}

/* DID 0xF189: the image version from the hand-off as ASCII major.minor.patch. */
static void test_read_did_software_version(void)
{
    read_version(0x01020003U, "1.2.3");
    read_version(0x00000000U, "0.0.0");
    read_version(0xFFFFFFFFU, "255.255.65535");
    read_version(0x0A000064U, "10.0.100");
    read_version(0x01010000U, "1.1.0");
    read_version(0x00630009U, "0.99.9");
    TEST_ASSERT_EQUAL_UINT32(6U, g_fake.boot_info.starts);
}

/* Without a hand-off there is no image header and no version to give. */
static void test_read_did_software_version_without_a_hand_off_is_refused(void)
{
    static const uint8_t req[] = {0x22, 0xF1, 0x89};

    g_fake.info.handoff = false;
    g_fake.info.image_version = 0x01020003U;
    send_expect_nrc(req, LEN(req), UDS_NRC_CONDITIONS_NOT_CORRECT);
}

/* DID 0xFD00: the documented layout, and the none-values without a hand-off. */
static void test_read_did_boot_info_layout(void)
{
    static const uint8_t req[] = {0x22, 0xFD, 0x00};
    static const uint8_t trial[] = {0x62, 0xFD, 0x00, 0x01, 0x01, 0x01, 0x02, 0x02,
                                    0x01, 0x00, 0xFF, 0x01, 0x01, 0x00, 0x00};
    static const uint8_t rolled_back[] = {0x62, 0xFD, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00,
                                          0x00, 0x05, 0x01, 0x01, 0x00, 0x00, 0x00};
    static const uint8_t none[] = {0x62, 0xFD, 0x00, 0x01, 0x00, 0xFF, 0x00, 0x00,
                                   0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00};

    TEST_ASSERT_EQUAL_UINT32(LEN(trial), 3U + UDS_BOOT_INFO_SIZE);
    g_fake.info = (uds_boot_info_t){.handoff = true,
                                    .slot = 1U,
                                    .mode = 2U,
                                    .attempt = 2U,
                                    .event = 0U,
                                    .event_slot = UDS_BOOT_NO_SLOT,
                                    .confirm_state = 1U,
                                    .image_version = 0x01010000U};
    send_expect(req, LEN(req), trial, LEN(trial));

    g_fake.info = (uds_boot_info_t){.handoff = true,
                                    .slot = 0U,
                                    .mode = 1U,
                                    .attempt = 0U,
                                    .event = 5U,
                                    .event_slot = 1U,
                                    .confirm_state = 0U,
                                    .image_version = 0x01000000U};
    send_expect(req, LEN(req), rolled_back, LEN(rolled_back));

    /* Without a hand-off, whatever the other fields hold. */
    g_fake.info = (uds_boot_info_t){.handoff = false,
                                    .slot = 7U,
                                    .mode = 9U,
                                    .attempt = 3U,
                                    .event = 6U,
                                    .event_slot = 0U,
                                    .confirm_state = 0U,
                                    .image_version = 0x01020003U};
    send_expect(req, LEN(req), none, LEN(none));
}

/* The boot information through a backend that is pending, and one that refuses. */
static void test_read_did_pending_and_refused_by_the_backend(void)
{
    static const uint8_t req[] = {0x22, 0xFD, 0x00};
    const uint64_t t = g_now;

    g_fake.info.handoff = true;
    g_fake.boot_info.pending = 1000U;
    TEST_ASSERT_EQUAL_UINT32(0U, send(req, LEN(req)));
    (void)poll_at(t + (40U * MS));
    expect_pending(0x22);
    g_fake.boot_info.left = 0U;
    TEST_ASSERT_EQUAL_UINT32(3U + UDS_BOOT_INFO_SIZE, poll_at(t + (50U * MS)));
    TEST_ASSERT_EQUAL_HEX8(0x62, g_rsp[0]);

    g_fake.boot_info.pending = 0U;
    g_fake.boot_info.nrc = UDS_NRC_CONDITIONS_NOT_CORRECT;
    send_expect_nrc(req, LEN(req), UDS_NRC_CONDITIONS_NOT_CORRECT);
}

/* ---------------------------------------------------------------------- ReadDTCInformation */

/* Four DTCs: event 0 failed and confirmed (0x2F), event 1 never tested (0x50), event 2 failed
 * and passed (0x2E), event 3 passed (0x00: it matches no mask, and is still supported). */
static const dtc_event_config_t k_events[4] = {
    {.dtc = 0x1B0600U,
     .failed_threshold = 1,
     .passed_threshold = -1,
     .step_up = 1U,
     .step_down = 1U,
     .confirmation_threshold = 1U},
    {.dtc = 0xC07388U,
     .failed_threshold = 1,
     .passed_threshold = -1,
     .step_up = 1U,
     .step_down = 1U,
     .confirmation_threshold = 1U},
    {.dtc = 0x9A0001U,
     .failed_threshold = 1,
     .passed_threshold = -1,
     .step_up = 1U,
     .step_down = 1U,
     .confirmation_threshold = 1U},
    {.dtc = 0x9A0002U,
     .failed_threshold = 1,
     .passed_threshold = -1,
     .step_up = 1U,
     .step_down = 1U,
     .confirmation_threshold = 1U},
};
static dtc_state_t s_dtc;

static void four_dtcs(void)
{
    dtc_entry_t entry;

    TEST_ASSERT_TRUE(dtc_init(&s_dtc, k_events, 4U));
    TEST_ASSERT_TRUE(dtc_report(&s_dtc, 0U, DTC_REPORT_FAILED));
    TEST_ASSERT_TRUE(dtc_report(&s_dtc, 2U, DTC_REPORT_FAILED));
    TEST_ASSERT_TRUE(dtc_report(&s_dtc, 2U, DTC_REPORT_PASSED));
    TEST_ASSERT_TRUE(dtc_report(&s_dtc, 3U, DTC_REPORT_PASSED));
    TEST_ASSERT_TRUE(dtc_get(&s_dtc, 3U, &entry));
    TEST_ASSERT_EQUAL_HEX8(0x00, entry.status);
    g_fake.dtc = &s_dtc;
}

static void test_read_dtc_count_by_status_mask(void)
{
    static const struct {
        uint8_t mask;
        uint8_t count;
    } cases[] = {{0x01, 1}, {0x08, 2}, {0x40, 1}, {0x50, 1}, {0xFF, 3}, {0x00, 0}, {0x80, 0}};

    four_dtcs();
    for (uint32_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        const uint8_t req[] = {0x19, 0x01, cases[i].mask};
        const uint8_t rsp[] = {0x59, 0x01, 0xFF, 0x01, 0x00, cases[i].count};

        send_expect(req, LEN(req), rsp, LEN(rsp));
    }
}

static void test_read_dtc_by_status_mask(void)
{
    static const uint8_t confirmed[] = {0x19, 0x02, 0x08};
    static const uint8_t confirmed_rsp[] = {0x59, 0x02, 0xFF, 0x1B, 0x06, 0x00,
                                            0x2F, 0x9A, 0x00, 0x01, 0x2E};
    static const uint8_t untested[] = {0x19, 0x02, 0x40};
    static const uint8_t untested_rsp[] = {0x59, 0x02, 0xFF, 0xC0, 0x73, 0x88, 0x50};
    static const uint8_t nothing[] = {0x19, 0x02, 0x00};
    static const uint8_t nothing_rsp[] = {0x59, 0x02, 0xFF};
    static const uint8_t any[] = {0x19, 0x02, 0xFF};
    static const uint8_t any_rsp[] = {0x59, 0x02, 0xFF, 0x1B, 0x06, 0x00, 0x2F, 0xC0,
                                      0x73, 0x88, 0x50, 0x9A, 0x00, 0x01, 0x2E};

    four_dtcs();
    send_expect(confirmed, LEN(confirmed), confirmed_rsp, LEN(confirmed_rsp));
    send_expect(untested, LEN(untested), untested_rsp, LEN(untested_rsp));
    send_expect(nothing, LEN(nothing), nothing_rsp, LEN(nothing_rsp));
    send_expect(any, LEN(any), any_rsp, LEN(any_rsp)); /* not the DTC whose status is 0x00 */
}

static void test_read_dtc_supported(void)
{
    static const uint8_t req[] = {0x19, 0x0A};
    static const uint8_t rsp[] = {0x59, 0x0A, 0xFF, 0x1B, 0x06, 0x00, 0x2F, 0xC0, 0x73, 0x88,
                                  0x50, 0x9A, 0x00, 0x01, 0x2E, 0x9A, 0x00, 0x02, 0x00};

    four_dtcs();
    send_expect(req, LEN(req), rsp, LEN(rsp));
    /* In the extended session too. */
    enter_session(UDS_SESSION_EXTENDED);
    send_expect(req, LEN(req), rsp, LEN(rsp));
}

static void test_read_dtc_length_checks(void)
{
    static const uint8_t count_short[] = {0x19, 0x01};
    static const uint8_t count_long[] = {0x19, 0x01, 0xFF, 0x00};
    static const uint8_t list_short[] = {0x19, 0x02};
    static const uint8_t supported_long[] = {0x19, 0x0A, 0xFF};

    four_dtcs();
    send_expect_nrc(count_short, LEN(count_short), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(count_long, LEN(count_long), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(list_short, LEN(list_short), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(supported_long, LEN(supported_long), UDS_NRC_INCORRECT_LENGTH);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.dtc_snapshot.starts);
}

/* With every DTC a table can have, the list is UDS_RESPONSE_MAX bytes; one byte less of buffer
 * is responseTooLong. */
static void test_read_dtc_with_every_dtc_fills_the_longest_response(void)
{
    static dtc_event_config_t events[DTC_EVENTS_MAX];
    static const uint8_t req[] = {0x19, 0x0A};

    for (uint32_t i = 0U; i < DTC_EVENTS_MAX; i++) {
        events[i] = k_events[0];
        events[i].dtc = 0x010000U + i;
    }
    TEST_ASSERT_TRUE(dtc_init(&s_dtc, events, (uint16_t)DTC_EVENTS_MAX));
    TEST_ASSERT_TRUE(dtc_report(&s_dtc, (uint16_t)(DTC_EVENTS_MAX - 1U), DTC_REPORT_FAILED));
    g_fake.dtc = &s_dtc;

    TEST_ASSERT_EQUAL_UINT32(UDS_RESPONSE_MAX, send(req, LEN(req)));
    TEST_ASSERT_EQUAL_HEX8(0x01, g_rsp[3]);
    TEST_ASSERT_EQUAL_HEX8(0x50, g_rsp[6]);
    TEST_ASSERT_EQUAL_HEX8(0x1F, g_rsp[UDS_RESPONSE_MAX - 2U]);
    TEST_ASSERT_EQUAL_HEX8(0x2F, g_rsp[UDS_RESPONSE_MAX - 1U]);

    g_rsp_cap = UDS_RESPONSE_MAX - 1U;
    send_expect_nrc(req, LEN(req), UDS_NRC_RESPONSE_TOO_LONG);
}

static void test_read_dtc_through_a_pending_or_refusing_backend(void)
{
    static const uint8_t req[] = {0x19, 0x01, 0xFF};
    static const uint8_t rsp[] = {0x59, 0x01, 0xFF, 0x01, 0x00, 0x03};
    const uint64_t t = g_now;

    four_dtcs();
    g_fake.dtc_snapshot.pending = 1000U;
    TEST_ASSERT_EQUAL_UINT32(0U, send(req, LEN(req)));
    (void)poll_at(t + (40U * MS));
    expect_pending(0x19);
    g_fake.dtc_snapshot.left = 0U;
    (void)poll_at(t + (60U * MS));
    expect_rsp(rsp, LEN(rsp));

    g_fake.dtc_snapshot.pending = 0U;
    g_fake.dtc_snapshot.nrc = UDS_NRC_CONDITIONS_NOT_CORRECT;
    send_expect_nrc(req, LEN(req), UDS_NRC_CONDITIONS_NOT_CORRECT);

    /* A backend that gives no snapshot and no NRC breaks its interface: generalReject. */
    g_fake.dtc_snapshot.nrc = UDS_NRC_NONE;
    g_fake.dtc = NULL;
    send_expect_nrc(req, LEN(req), UDS_NRC_GENERAL_REJECT);
}

/* -------------------------------------------------------------- ClearDiagnosticInformation */

/* The positive response comes from the call at which the backend reports the clear made. */
static void test_clear_answers_only_once_the_clear_is_made(void)
{
    static const uint8_t req[] = {0x14, 0xFF, 0xFF, 0xFF};
    static const uint8_t rsp[] = {0x54};
    uint64_t t = g_now;

    g_fake.dtc_clear.pending = 2U;
    TEST_ASSERT_EQUAL_UINT32(0U, send(req, LEN(req)));
    TEST_ASSERT_FALSE(g_fake.cleared);
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(t + (10U * MS)));
    TEST_ASSERT_FALSE(g_fake.cleared);
    (void)poll_at(t + (20U * MS));
    TEST_ASSERT_TRUE(g_fake.cleared);
    expect_rsp(rsp, LEN(rsp));
    TEST_ASSERT_EQUAL_HEX32(UDS_DTC_GROUP_ALL, g_fake.group);

    /* A long clear: responsePending, then the response. */
    t = g_now;
    g_fake.cleared = false;
    g_fake.dtc_clear.pending = 1000U;
    (void)send(req, LEN(req));
    (void)poll_at(t + (40U * MS));
    expect_pending(0x14);
    TEST_ASSERT_FALSE(g_fake.cleared);
    g_fake.dtc_clear.left = 0U;
    (void)poll_at(t + (900U * MS));
    expect_rsp(rsp, LEN(rsp));
    TEST_ASSERT_TRUE(g_fake.cleared);

    /* In the extended session too. */
    enter_session(UDS_SESSION_EXTENDED);
    g_fake.dtc_clear.pending = 0U;
    send_expect(req, LEN(req), rsp, LEN(rsp));
}

static void test_clear_checks_in_order(void)
{
    static const uint8_t short1[] = {0x14};
    static const uint8_t short3[] = {0x14, 0xFF, 0xFF};
    static const uint8_t long6[] = {0x14, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
    static const uint8_t group_fe[] = {0x14, 0xFF, 0xFF, 0xFE};
    static const uint8_t group_0[] = {0x14, 0x00, 0x00, 0x00};
    static const uint8_t group_mid[] = {0x14, 0xFF, 0x00, 0xFF};
    static const uint8_t memory_selection[] = {0x14, 0xFF, 0xFF, 0xFF, 0x00};
    static const uint8_t both[] = {0x14, 0x12, 0x34, 0x56, 0x07};

    send_expect_nrc(short1, LEN(short1), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(short3, LEN(short3), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(long6, LEN(long6), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(group_fe, LEN(group_fe), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(group_0, LEN(group_0), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(group_mid, LEN(group_mid), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(memory_selection, LEN(memory_selection), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(both, LEN(both), UDS_NRC_REQUEST_OUT_OF_RANGE);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.dtc_clear.starts);
}

static void test_clear_refused_by_the_backend(void)
{
    static const uint8_t req[] = {0x14, 0xFF, 0xFF, 0xFF};

    g_fake.dtc_clear.nrc = UDS_NRC_CONDITIONS_NOT_CORRECT;
    send_expect_nrc(req, LEN(req), UDS_NRC_CONDITIONS_NOT_CORRECT);
    TEST_ASSERT_FALSE(g_fake.cleared);
}

/* ------------------------------------------------ RoutineControl: the checks and the confirm */

static void test_routine_control_checks(void)
{
    static const uint8_t short2[] = {0x31, 0x01};
    static const uint8_t short3[] = {0x31, 0x01, 0xF0};
    static const uint8_t unknown[] = {0x31, 0x01, 0x12, 0x34};
    static const uint8_t unknown_ff02[] = {0x31, 0x01, 0xFF, 0x02};
    static const uint8_t unknown_f001[] = {0x31, 0x03, 0xF0, 0x01};
    static const uint8_t erase[] = {0x31, 0x01, 0xFF, 0x00, 0x44, 0x00, 0x10,
                                    0x20, 0x00, 0x00, 0x00, 0x10, 0x00};
    static const uint8_t check[] = {0x31, 0x01, 0xFF, 0x01};
    static const uint8_t confirm[] = {0x31, 0x01, 0xF0, 0x00};

    enter_session(UDS_SESSION_EXTENDED);
    send_expect_nrc(short2, LEN(short2), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(short3, LEN(short3), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(unknown, LEN(unknown), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(unknown_ff02, LEN(unknown_ff02), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(unknown_f001, LEN(unknown_f001), UDS_NRC_REQUEST_OUT_OF_RANGE);
    /* The programming routines are not in the extended session ... */
    send_expect_nrc(erase, LEN(erase), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(check, LEN(check), UDS_NRC_REQUEST_OUT_OF_RANGE);
    /* ... nor the confirm in the programming session. */
    enter_session(UDS_SESSION_PROGRAMMING);
    send_expect_nrc(confirm, LEN(confirm), UDS_NRC_REQUEST_OUT_OF_RANGE);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.erase.starts);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.check.starts);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.confirm.starts);
}

/* The confirm routine: positive with its status record; lib/boot's refusals mapped. */
static void test_confirm_routine(void)
{
    static const uint8_t req[] = {0x31, 0x01, 0xF0, 0x00};
    static const uint8_t rsp[] = {0x71, 0x01, 0xF0, 0x00, 0x00, 0x00};
    static const uint8_t longer[] = {0x31, 0x01, 0xF0, 0x00, 0x00};
    static const struct {
        boot_status_t status;
        uint8_t nrc;
    } refusals[] = {{BOOT_E_STATE, UDS_NRC_CONDITIONS_NOT_CORRECT},
                    {BOOT_E_FLASH, UDS_NRC_PROGRAMMING_FAILURE},
                    {BOOT_E_POWER_LOST, UDS_NRC_PROGRAMMING_FAILURE},
                    {BOOT_E_ARG, UDS_NRC_REQUEST_OUT_OF_RANGE},
                    {BOOT_BUSY, UDS_NRC_PROGRAMMING_FAILURE}};

    enter_session(UDS_SESSION_EXTENDED);
    send_expect(req, LEN(req), rsp, LEN(rsp));
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.confirm.starts);
    send_expect_nrc(longer, LEN(longer), UDS_NRC_INCORRECT_LENGTH);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.confirm.starts);

    for (uint32_t i = 0U; i < (sizeof(refusals) / sizeof(refusals[0])); i++) {
        g_fake.confirm.status = refusals[i].status;
        send_expect_nrc(req, LEN(req), refusals[i].nrc);
    }

    /* Pending: responsePending, then the response. */
    const uint64_t t = g_now;

    g_fake.confirm.status = BOOT_OK;
    g_fake.confirm.pending = 1000U;
    TEST_ASSERT_EQUAL_UINT32(0U, send(req, LEN(req)));
    (void)poll_at(t + (40U * MS));
    expect_pending(0x31);
    g_fake.confirm.left = 0U;
    (void)poll_at(t + (45U * MS));
    expect_rsp(rsp, LEN(rsp));
}

/* requestRoutineResults reports the last start that ran in this session. */
static void test_routine_results(void)
{
    static const uint8_t start[] = {0x31, 0x01, 0xF0, 0x00};
    static const uint8_t start_long[] = {0x31, 0x01, 0xF0, 0x00, 0x01};
    static const uint8_t results[] = {0x31, 0x03, 0xF0, 0x00};
    static const uint8_t results_long[] = {0x31, 0x03, 0xF0, 0x00, 0x00};
    static const uint8_t results_spr[] = {0x31, 0x83, 0xF0, 0x00};
    static const uint8_t ok[] = {0x71, 0x03, 0xF0, 0x00, 0x00, 0x00};
    static const uint8_t failed[] = {0x71, 0x03, 0xF0, 0x00, 0x01, 0x22};

    enter_session(UDS_SESSION_EXTENDED);
    send_expect_nrc(results, LEN(results), UDS_NRC_REQUEST_SEQUENCE_ERROR);
    /* A start refused by its own checks did not run. */
    send_expect_nrc(start_long, LEN(start_long), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(results, LEN(results), UDS_NRC_REQUEST_SEQUENCE_ERROR);

    (void)send(start, LEN(start));
    send_expect(results, LEN(results), ok, LEN(ok));
    send_expect_nrc(results_long, LEN(results_long), UDS_NRC_INCORRECT_LENGTH);

    g_fake.confirm.status = BOOT_E_STATE;
    send_expect_nrc(start, LEN(start), UDS_NRC_CONDITIONS_NOT_CORRECT);
    send_expect(results, LEN(results), failed, LEN(failed));
    TEST_ASSERT_EQUAL_UINT32(0U, send(results_spr, LEN(results_spr)));

    /* A session that begins forgets it. */
    (void)send2(0x10, 0x03);
    send_expect_nrc(results, LEN(results), UDS_NRC_REQUEST_SEQUENCE_ERROR);
    TEST_ASSERT_EQUAL_UINT32(2U, g_fake.confirm.starts);
}

/* A routine started with the SPRMIB runs and answers nothing. */
static void test_a_suppressed_routine_runs(void)
{
    static const uint8_t req[] = {0x31, 0x81, 0xF0, 0x00};

    enter_session(UDS_SESSION_EXTENDED);
    TEST_ASSERT_EQUAL_UINT32(0U, send(req, LEN(req)));
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.confirm.done);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().suppressed);
}

/* ---------------------------------------------------------------- statistics and the tables */

static void test_statistics_count_requests_by_service_and_nrcs_by_code(void)
{
    static const uint8_t sids[UDS_SERVICE_COUNT] = {0x10, 0x11, 0x14, 0x19, 0x22,
                                                    0x31, 0x34, 0x36, 0x37, 0x3E};

    for (uint32_t i = 0U; i < UDS_SERVICE_COUNT; i++) {
        TEST_ASSERT_EQUAL_HEX8(sids[i], uds_service_sid(i));
        for (uint32_t n = 0U; n <= i; n++) {
            (void)send1(sids[i]);
        }
    }
    TEST_ASSERT_EQUAL_UINT8(0U, uds_service_sid(UDS_SERVICE_COUNT));

    const uds_stats_t st = stats();

    for (uint32_t i = 0U; i < UDS_SERVICE_COUNT; i++) {
        TEST_ASSERT_EQUAL_UINT32(i + 1U, st.requests[i]);
    }
    /* In the default session: 0x10, 0x11, 0x19, 0x3E too short (1 + 2 + 4 + 10), 0x14 and
     * 0x22 too short (3 + 5), 0x31, 0x34, 0x36, 0x37 not in the session (6 + 7 + 8 + 9). */
    TEST_ASSERT_EQUAL_UINT32(25U, nrc_count(UDS_NRC_INCORRECT_LENGTH));
    TEST_ASSERT_EQUAL_UINT32(30U, nrc_count(UDS_NRC_SERVICE_NOT_IN_SESSION));
    TEST_ASSERT_EQUAL_UINT32(0U, st.positive);
    TEST_ASSERT_EQUAL_UINT32(0U, st.requests_unsupported);
}

static void test_names_and_tables(void)
{
    for (uint32_t i = 0U; i < UDS_SERVICE_COUNT; i++) {
        TEST_ASSERT_TRUE(strcmp(uds_service_name(uds_service_sid(i)), "?") != 0);
        if (i > 0U) {
            TEST_ASSERT_TRUE(uds_service_sid(i) > uds_service_sid(i - 1U));
        }
    }
    for (uint32_t i = 0U; i < UDS_NRC_KINDS; i++) {
        TEST_ASSERT_TRUE(strcmp(uds_nrc_name(uds_nrc_code(i)), "?") != 0);
        TEST_ASSERT_NOT_EQUAL(UDS_NRC_RESPONSE_PENDING, uds_nrc_code(i));
        if (i > 0U) {
            TEST_ASSERT_TRUE(uds_nrc_code(i) > uds_nrc_code(i - 1U));
        }
    }
    TEST_ASSERT_EQUAL_UINT8(0U, uds_nrc_code(UDS_NRC_KINDS));
    TEST_ASSERT_EQUAL_STRING("DiagnosticSessionControl", uds_service_name(0x10));
    TEST_ASSERT_EQUAL_STRING("TesterPresent", uds_service_name(0x3E));
    TEST_ASSERT_EQUAL_STRING("?", uds_service_name(0x27));
    TEST_ASSERT_EQUAL_STRING("conditionsNotCorrect", uds_nrc_name(0x22));
    TEST_ASSERT_EQUAL_STRING("requestCorrectlyReceivedResponsePending", uds_nrc_name(0x78));
    TEST_ASSERT_EQUAL_STRING("?", uds_nrc_name(0x33));
    TEST_ASSERT_EQUAL_STRING("?", uds_nrc_name(0x00));
    TEST_ASSERT_EQUAL_STRING("defaultSession", uds_session_name(UDS_SESSION_DEFAULT));
    TEST_ASSERT_EQUAL_STRING("programmingSession", uds_session_name(UDS_SESSION_PROGRAMMING));
    TEST_ASSERT_EQUAL_STRING("extendedDiagnosticSession", uds_session_name(UDS_SESSION_EXTENDED));
    TEST_ASSERT_EQUAL_STRING("?", uds_session_name(0x04));
    TEST_ASSERT_EQUAL_STRING("idle", uds_dl_state_name(UDS_DL_IDLE));
    TEST_ASSERT_EQUAL_STRING("erased", uds_dl_state_name(UDS_DL_ERASED));
    TEST_ASSERT_EQUAL_STRING("transfer", uds_dl_state_name(UDS_DL_TRANSFER));
    TEST_ASSERT_EQUAL_STRING("exited", uds_dl_state_name(UDS_DL_EXITED));
    TEST_ASSERT_EQUAL_STRING("checked", uds_dl_state_name(UDS_DL_CHECKED));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): outside it on purpose */
    TEST_ASSERT_EQUAL_STRING("?", uds_dl_state_name((uds_dl_state_t)99));
}

/* lib/boot's status as an NRC (uds.h, LIB/BOOT'S STATUS AS AN NRC). */
static void test_boot_status_mapping(void)
{
    static const struct {
        boot_status_t status;
        uint8_t nrc;
    } cases[] = {
        {BOOT_OK, UDS_NRC_NONE},
        {BOOT_BUSY, UDS_NRC_NONE},
        {BOOT_E_ARG, UDS_NRC_REQUEST_OUT_OF_RANGE},
        {BOOT_E_SIZE, UDS_NRC_REQUEST_OUT_OF_RANGE},
        {BOOT_E_STATE, UDS_NRC_CONDITIONS_NOT_CORRECT},
        {BOOT_E_SEQUENCE, UDS_NRC_REQUEST_SEQUENCE_ERROR},
        {BOOT_E_INVALID, UDS_NRC_PROGRAMMING_FAILURE},
        {BOOT_E_FLASH, UDS_NRC_PROGRAMMING_FAILURE},
        {BOOT_E_POWER_LOST, UDS_NRC_PROGRAMMING_FAILURE},
        {BOOT_E_LAYOUT, UDS_NRC_PROGRAMMING_FAILURE},
        {BOOT_E_NOTHING_BOOTABLE, UDS_NRC_PROGRAMMING_FAILURE},
        /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): outside it on purpose */
        {(boot_status_t)99, UDS_NRC_PROGRAMMING_FAILURE},
    };

    for (uint32_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        TEST_ASSERT_EQUAL_HEX8(cases[i].nrc, uds_nrc_for_boot(cases[i].status));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_refuses_null_pointers_and_an_incomplete_backend);
    RUN_TEST(test_calls_with_bad_arguments_do_nothing);
    RUN_TEST(test_an_empty_request_is_ignored);
    RUN_TEST(test_step1_a_request_while_one_is_pending_is_busy_repeat_request);
    RUN_TEST(test_step2_an_unknown_sid_is_service_not_supported);
    RUN_TEST(test_step3_a_service_outside_its_sessions_comes_before_length_and_sub_function);
    RUN_TEST(test_step4_a_service_with_a_sub_function_needs_two_bytes);
    RUN_TEST(test_step5_an_unknown_sub_function_comes_before_its_session_and_the_length);
    RUN_TEST(test_step6_a_sub_function_outside_its_sessions_comes_before_the_length);
    RUN_TEST(test_step7_the_service_checks_follow_the_general_ones);
    RUN_TEST(test_step8_a_response_longer_than_the_buffer_is_response_too_long);
    RUN_TEST(test_session_transitions);
    RUN_TEST(test_session_response_reports_p2_and_p2_star);
    RUN_TEST(test_session_changes_are_counted_when_the_session_differs);
    RUN_TEST(test_a_suppressed_session_control_changes_the_session_silently);
    RUN_TEST(test_active_session_did_in_every_session);
    RUN_TEST(test_s3_returns_to_the_default_session_after_5000_ms);
    RUN_TEST(test_s3_runs_from_the_end_of_every_request);
    RUN_TEST(test_s3_is_checked_before_a_request);
    RUN_TEST(test_s3_does_not_run_in_the_default_session);
    RUN_TEST(test_s3_does_not_run_while_a_request_is_pending);
    RUN_TEST(test_suppressed_tester_present_keeps_the_session);
    RUN_TEST(test_response_pending_first_at_40_ms_then_every_2000_ms);
    RUN_TEST(test_no_response_pending_when_the_backend_answers_in_time);
    RUN_TEST(test_the_next_response_pending_counts_from_the_one_before);
    RUN_TEST(test_a_suppressed_request_gets_its_response_after_a_response_pending);
    RUN_TEST(test_a_second_request_does_not_move_the_cadence);
    RUN_TEST(test_a_poll_with_nothing_pending_sends_nothing);
    RUN_TEST(test_time_never_goes_back);
    RUN_TEST(test_ecu_reset_answers_then_reports_the_reset_in_every_session);
    RUN_TEST(test_ecu_reset_refused_by_the_backend_resets_nothing);
    RUN_TEST(test_a_suppressed_ecu_reset_still_resets);
    RUN_TEST(test_a_backend_nrc_the_server_does_not_name_is_general_reject);
    RUN_TEST(test_tester_present_in_every_session);
    RUN_TEST(test_read_did_length_and_identifier_checks);
    RUN_TEST(test_read_did_software_version);
    RUN_TEST(test_read_did_software_version_without_a_hand_off_is_refused);
    RUN_TEST(test_read_did_boot_info_layout);
    RUN_TEST(test_read_did_pending_and_refused_by_the_backend);
    RUN_TEST(test_read_dtc_count_by_status_mask);
    RUN_TEST(test_read_dtc_by_status_mask);
    RUN_TEST(test_read_dtc_supported);
    RUN_TEST(test_read_dtc_length_checks);
    RUN_TEST(test_read_dtc_with_every_dtc_fills_the_longest_response);
    RUN_TEST(test_read_dtc_through_a_pending_or_refusing_backend);
    RUN_TEST(test_clear_answers_only_once_the_clear_is_made);
    RUN_TEST(test_clear_checks_in_order);
    RUN_TEST(test_clear_refused_by_the_backend);
    RUN_TEST(test_routine_control_checks);
    RUN_TEST(test_confirm_routine);
    RUN_TEST(test_routine_results);
    RUN_TEST(test_a_suppressed_routine_runs);
    RUN_TEST(test_statistics_count_requests_by_service_and_nrcs_by_code);
    RUN_TEST(test_names_and_tables);
    RUN_TEST(test_boot_status_mapping);
    return UNITY_END();
}
