/* SPDX-License-Identifier: MIT */
/*
 * app/diag.c and app/diag_backend.c: the node's diagnostic connection, lib/isotp in front of
 * lib/uds, with the node's backend over a fake node, on a fake clock and a fake transmit path
 * that records every frame and stamps it.
 *
 * What it shows: the node's ISO-TP parameters on the wire (padding, BS 16 and STmin 0 in every
 * flow control, after the first frame and every 16 consecutive frames); a request goes to lib/uds
 * and its response out; a response produced while another is in transmission waits for it, and
 * one that finds both buffers in use is dropped; the confirmations: passed for the oldest frame,
 * dropped at and after its N_As, records of frames never reported forgotten, reports for no frame
 * dropped; the ECUReset due after its response's transmission ended, or at once, or at its
 * deadline behind an earlier response the tester's flow control holds, and never before its
 * response for a tester that waits for each response; a frame the
 * transmit path refuses; and the backend: a flash job posted and answered when it is done, a job
 * the flash task does not take, the clear that waits for the health task, the boot information
 * and a missing DTC snapshot.
 */
#include <string.h>

#include "diag.h"
#include "diag_backend.h"
#include "unity.h"

#define MS      ((uint64_t)1000U)
#define T0_US   ((uint64_t)5000000U)
#define MAX_OUT 256U
#define LEN(a)  ((uint32_t)sizeof(a))

static diag_t s_diag;
static diag_backend_t s_backend;
static uint64_t s_now;

/* The fake transmit path: every frame it took, with its stamp. */
static can_frame_t s_out[MAX_OUT];
static uint64_t s_out_stamp[MAX_OUT];
static uint32_t s_out_count;
static uint64_t s_next_stamp;
static bool s_refuse;

/* The fake node of the backend. */
static flash_job_t s_job;
static uint32_t s_jobs_posted;
static bool s_post_refused;
static bool s_job_finished;
static flash_job_result_t s_job_result;
static uint32_t s_clear_posts;
static bool s_clear_made;
static const dtc_state_t *s_dtc;

static bool port_send(void *ctx, const can_frame_t *frame, uint64_t *stamp_ns)
{
    bool taken = false;

    (void)ctx;

    if (!s_refuse) {
        TEST_ASSERT_TRUE_MESSAGE(s_out_count < MAX_OUT, "too many frames");
        s_out[s_out_count] = *frame;
        s_next_stamp += 1000U;
        s_out_stamp[s_out_count] = s_next_stamp;
        *stamp_ns = s_next_stamp;
        s_out_count++;
        taken = true;
    }

    return taken;
}

static bool node_post_job(void *ctx, const flash_job_t *job)
{
    (void)ctx;

    if (!s_post_refused) {
        s_job = *job;
        s_jobs_posted++;
        s_job_finished = false;
    }

    return !s_post_refused;
}

static bool node_job_done(void *ctx, flash_job_result_t *result)
{
    (void)ctx;

    if (s_job_finished) {
        *result = s_job_result;
        s_job_finished = false;
        return true;
    }

    return false;
}

static void node_boot_info(void *ctx, uds_boot_info_t *info)
{
    (void)ctx;
    (void)memset(info, 0, sizeof(*info));
    info->handoff = true;
    info->slot = 1U;
    info->mode = 2U;
    info->attempt = 1U;
    info->event = 0U;
    info->event_slot = 0xFFU;
    info->confirm_state = 1U;
    info->image_version = 0x01020003U;
}

static const dtc_state_t *node_dtc_snapshot(void *ctx)
{
    (void)ctx;
    return s_dtc;
}

static uint32_t node_clear_post(void *ctx)
{
    (void)ctx;
    s_clear_posts++;
    return 41U + s_clear_posts;
}

static bool node_clear_done(void *ctx, uint32_t number)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_UINT32(41U + s_clear_posts, number);
    return s_clear_made;
}

static const diag_node_t NODE = {.ctx = NULL,
                                 .post_job = node_post_job,
                                 .job_done = node_job_done,
                                 .boot_info = node_boot_info,
                                 .dtc_snapshot = node_dtc_snapshot,
                                 .clear_post = node_clear_post,
                                 .clear_done = node_clear_done};

static const diag_port_t PORT = {.ctx = NULL, .send = port_send};

void setUp(void)
{
    uds_backend_t backend;

    (void)memset(s_out, 0, sizeof(s_out));
    s_out_count = 0U;
    s_next_stamp = 7000000000U;
    s_refuse = false;
    s_jobs_posted = 0U;
    s_post_refused = false;
    s_job_finished = false;
    s_clear_posts = 0U;
    s_clear_made = false;
    s_dtc = NULL;
    s_now = T0_US;
    (void)memset(&s_diag, 0xA5, sizeof(s_diag));
    TEST_ASSERT_TRUE(diag_backend_init(&s_backend, &NODE, &backend));
    TEST_ASSERT_TRUE(diag_init(&s_diag, &PORT, &backend));
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------- the tester side */

static void deliver(const uint8_t *bytes, uint8_t dlc)
{
    can_frame_t frame = {.id = DIAG_REQUEST_ID, .flags = 0U, .dlc = dlc, .data = {0U}};

    (void)memcpy(frame.data, bytes, dlc);
    diag_on_frame(&s_diag, &frame, s_now);
}

/* A request of up to 7 bytes in a single frame, padded as the tester pads. */
static void request_sf(const uint8_t *req, uint32_t len)
{
    uint8_t data[8];

    (void)memset(data, 0xCC, sizeof(data));
    data[0] = (uint8_t)len;
    (void)memcpy(&data[1], req, len);
    deliver(data, 8U);
}

/* The next frame the node sent is exactly `bytes` (8 of them), on 0x7E8 with DLC 8. */
static void expect_frame(uint32_t index, const uint8_t bytes[8])
{
    TEST_ASSERT_TRUE_MESSAGE(index < s_out_count, "the frame was not sent");
    TEST_ASSERT_EQUAL_HEX32(DIAG_RESPONSE_ID, s_out[index].id);
    TEST_ASSERT_EQUAL_UINT8(0U, s_out[index].flags);
    TEST_ASSERT_EQUAL_UINT8(8U, s_out[index].dlc);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bytes, s_out[index].data, 8U);
}

/* The transmit path reports frame `index` sent at the clock's time. */
static void report(uint32_t index)
{
    diag_on_tx_done(&s_diag, s_out_stamp[index], s_now);
}

/* A single-frame request, its single-frame response checked and confirmed. */
static void exchange(const uint8_t *req, uint32_t req_len, const uint8_t *rsp, uint32_t rsp_len)
{
    uint8_t expected[8];
    const uint32_t first = s_out_count;

    (void)memset(expected, 0xCC, sizeof(expected));
    expected[0] = (uint8_t)rsp_len;
    (void)memcpy(&expected[1], rsp, rsp_len);
    request_sf(req, req_len);
    TEST_ASSERT_EQUAL_UINT32(first + 1U, s_out_count);
    expect_frame(first, expected);
    report(first);
}

static void enter_programming(void)
{
    static const uint8_t extended[] = {0x10, 0x03};
    static const uint8_t extended_ok[] = {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};
    static const uint8_t programming[] = {0x10, 0x02};
    static const uint8_t programming_ok[] = {0x50, 0x02, 0x00, 0x32, 0x01, 0xF4};

    exchange(extended, LEN(extended), extended_ok, LEN(extended_ok));
    exchange(programming, LEN(programming), programming_ok, LEN(programming_ok));
}

/* An eraseMemory request (13 bytes: a first frame and a consecutive frame) for `address` and
 * `size`, with the node's flow control after the first frame checked and confirmed. */
static void request_erase(uint32_t address, uint32_t size)
{
    static const uint8_t cts[8] = {0x30, 16U, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    const uint8_t ff[8] = {0x10, 13U, 0x31, 0x01, 0xFF, 0x00, 0x44, (uint8_t)(address >> 24U)};
    const uint8_t cf[8] = {0x21,
                           (uint8_t)(address >> 16U),
                           (uint8_t)(address >> 8U),
                           (uint8_t)address,
                           (uint8_t)(size >> 24U),
                           (uint8_t)(size >> 16U),
                           (uint8_t)(size >> 8U),
                           (uint8_t)size};
    const uint32_t first = s_out_count;

    deliver(ff, 8U);
    expect_frame(first, cts);
    report(first);
    deliver(cf, 8U);
}

/* --------------------------------------------------------------------------------- the tests */

static void test_init_refuses_what_it_cannot_use(void)
{
    uds_backend_t backend;
    const diag_port_t no_send = {.ctx = NULL, .send = NULL};
    diag_node_t no_clear = NODE;

    TEST_ASSERT_TRUE(diag_backend_init(&s_backend, &NODE, &backend));
    TEST_ASSERT_FALSE(diag_init(NULL, &PORT, &backend));
    TEST_ASSERT_FALSE(diag_init(&s_diag, NULL, &backend));
    TEST_ASSERT_FALSE(diag_init(&s_diag, &no_send, &backend));
    TEST_ASSERT_FALSE(diag_init(&s_diag, &PORT, NULL));
    no_clear.clear_done = NULL;
    TEST_ASSERT_FALSE(diag_backend_init(&s_backend, &no_clear, &backend));
    TEST_ASSERT_FALSE(diag_backend_init(&s_backend, NULL, &backend));
    TEST_ASSERT_FALSE(diag_backend_init(NULL, &NODE, &backend));
    TEST_ASSERT_FALSE(diag_backend_init(&s_backend, &NODE, NULL));

    /* An instance that is not prepared does nothing and answers its defaults. */
    (void)memset(&s_diag, 0, sizeof(s_diag));
    request_sf((const uint8_t[]){0x3E, 0x00}, 2U);
    diag_poll(&s_diag, s_now);
    diag_on_tx_done(&s_diag, 1U, s_now);
    TEST_ASSERT_EQUAL_UINT32(0U, s_out_count);
    TEST_ASSERT_NULL(diag_stats(&s_diag));
    TEST_ASSERT_NULL(diag_isotp_stats(&s_diag));
    TEST_ASSERT_FALSE(diag_reset_due(&s_diag));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, diag_next_deadline(&s_diag));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_DEFAULT, diag_session(&s_diag));
}

/* TesterPresent in a padded single frame; its response in a single frame padded with 0xCC; the
 * counts before and after the confirmation. */
static void test_a_single_frame_request_gets_a_padded_single_frame_response(void)
{
    static const uint8_t expected[8] = {0x02, 0x7E, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    const diag_stats_t *stats = diag_stats(&s_diag);
    uds_stats_t uds;

    request_sf((const uint8_t[]){0x3E, 0x00}, 2U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_out_count);
    expect_frame(0U, expected);
    TEST_ASSERT_EQUAL_UINT32(1U, stats->frames_in);
    TEST_ASSERT_EQUAL_UINT32(1U, stats->requests);
    TEST_ASSERT_EQUAL_UINT32(1U, stats->responses);
    TEST_ASSERT_EQUAL_UINT32(1U, stats->frames_out);
    TEST_ASSERT_EQUAL_UINT32(0U, stats->responses_sent);
    report(0U);
    TEST_ASSERT_EQUAL_UINT32(1U, stats->responses_sent);
    TEST_ASSERT_EQUAL_UINT32(1U, stats->confirms_passed);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_isotp_stats(&s_diag)->tx_results[ISOTP_RESULT_OK]);
    diag_uds_stats(&s_diag, &uds);
    TEST_ASSERT_EQUAL_UINT32(1U, uds.positive);
    diag_uds_stats(NULL, &uds);
    TEST_ASSERT_EQUAL_UINT32(0U, uds.positive);
    diag_uds_stats(&s_diag, NULL);
}

/* A request of 200 bytes: the node's flow control after the first frame and after every 16
 * consecutive frames, BS 16 and STmin 0 padded with 0xCC, and none after the last one; the
 * answer of lib/uds (incorrectMessageLengthOrInvalidFormat) after the last frame. */
static void test_flow_control_after_the_first_frame_and_every_16_consecutive_frames(void)
{
    static const uint8_t cts[8] = {0x30, 0x10, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    static const uint8_t nrc[8] = {0x03, 0x7F, 0x22, 0x13, 0xCC, 0xCC, 0xCC, 0xCC};
    uint8_t frame[8] = {0x10, 200U, 0x22, 0xF1, 0x86, 0x00, 0x00, 0x00};
    uint32_t cfs = 0U;

    deliver(frame, 8U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_out_count);
    expect_frame(0U, cts);
    report(0U);

    /* 194 bytes after the first frame's 6: 28 consecutive frames. */
    for (uint32_t sent = 6U; sent < 200U; sent += 7U) {
        cfs++;
        (void)memset(frame, 0, sizeof(frame));
        frame[0] = (uint8_t)(0x20U | (cfs & 0x0FU));
        deliver(frame, 8U);

        if ((cfs % 16U) == 0U) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U + (cfs / 16U), s_out_count,
                                             "a flow control after every 16");
            expect_frame(s_out_count - 1U, cts);
            report(s_out_count - 1U);
        } else if ((sent + 7U) < 200U) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U + (cfs / 16U), s_out_count,
                                             "nothing between the flow controls");
        } else {
            /* the last consecutive frame */
        }
    }

    TEST_ASSERT_EQUAL_UINT32(28U, cfs);
    TEST_ASSERT_EQUAL_UINT32(3U, s_out_count);
    expect_frame(2U, nrc);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_isotp_stats(&s_diag)->rx_results[ISOTP_RESULT_OK]);
    TEST_ASSERT_EQUAL_UINT32(2U, diag_isotp_stats(&s_diag)->frames_out[ISOTP_FRAME_FC_CTS]);
    TEST_ASSERT_EQUAL_UINT32(0U, diag_isotp_stats(&s_diag)->frames_out[ISOTP_FRAME_FC_WAIT]);
}

/* A message longer than the receive buffer (1026 bytes) is answered with FC OVFLW. */
static void test_a_message_longer_than_the_buffer_gets_overflow(void)
{
    static const uint8_t ovflw[8] = {0x32, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    const uint8_t ff[8] = {0x14, 0x03, 0x36, 0x01, 0x00, 0x00, 0x00, 0x00}; /* FF_DL 1027 */

    deliver(ff, 8U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_out_count);
    expect_frame(0U, ovflw);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_isotp_stats(&s_diag)->rx_overflow);
    TEST_ASSERT_EQUAL_UINT32(0U, diag_stats(&s_diag)->requests);
}

/* An erase that the flash task takes long for: responsePending at 40 ms; the final response,
 * produced while the responsePending is still unconfirmed, waits in the second buffer and goes
 * out when the first is confirmed. The job carried the request's address and size. */
static void test_a_response_waits_for_the_transmission_ahead_of_it(void)
{
    static const uint8_t pending[8] = {0x03, 0x7F, 0x31, 0x78, 0xCC, 0xCC, 0xCC, 0xCC};
    static const uint8_t done[8] = {0x06, 0x71, 0x01, 0xFF, 0x00, 0x00, 0x00, 0xCC};
    uint32_t at;

    enter_programming();
    request_erase(0x00102000U, 0x00010000U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_jobs_posted);
    TEST_ASSERT_EQUAL(FLASH_JOB_ERASE, s_job.kind);
    TEST_ASSERT_EQUAL_HEX32(0x00102000U, s_job.address);
    TEST_ASSERT_EQUAL_HEX32(0x00010000U, s_job.size);
    at = s_out_count;
    TEST_ASSERT_EQUAL_UINT64(s_now + (40U * MS), diag_next_deadline(&s_diag));

    s_now += (40U * MS) - 1U;
    diag_poll(&s_diag, s_now);
    TEST_ASSERT_EQUAL_UINT32(at, s_out_count);
    s_now += 1U;
    diag_poll(&s_diag, s_now);
    TEST_ASSERT_EQUAL_UINT32(at + 1U, s_out_count);
    expect_frame(at, pending);

    /* The job is done while the responsePending is still being sent. */
    s_now += 5U * MS;
    s_job_result = (flash_job_result_t){.status = BOOT_OK, .steps = 17U};
    s_job_finished = true;
    diag_poll(&s_diag, s_now);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(at + 1U, s_out_count, "the final response waits");
    report(at);
    TEST_ASSERT_EQUAL_UINT32(at + 2U, s_out_count);
    expect_frame(at + 1U, done);
    report(at + 1U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_backend.stats.jobs[FLASH_JOB_ERASE]);
    TEST_ASSERT_EQUAL_UINT32(17U, s_backend.stats.last_steps[FLASH_JOB_ERASE]);
    TEST_ASSERT_EQUAL_UINT32(0U, s_backend.stats.jobs_failed[FLASH_JOB_ERASE]);
    TEST_ASSERT_EQUAL_UINT32(diag_stats(&s_diag)->responses, diag_stats(&s_diag)->responses_sent);
}

/* With a responsePending in transmission and the final response waiting behind it, the answer
 * to a request of a tester that did not wait finds no buffer: dropped and counted. */
static void test_a_response_that_finds_both_buffers_in_use_is_dropped(void)
{
    uint32_t at;

    enter_programming();
    request_erase(0x00102000U, 0x00001000U);
    s_now += 40U * MS;
    diag_poll(&s_diag, s_now); /* the responsePending, not confirmed */
    at = s_out_count;
    s_job_result = (flash_job_result_t){.status = BOOT_OK, .steps = 2U};
    s_job_finished = true;
    diag_poll(&s_diag, s_now); /* the final response, waiting */
    request_sf((const uint8_t[]){0x3E, 0x00}, 2U);
    TEST_ASSERT_EQUAL_UINT32(at, s_out_count);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->responses_dropped);
    report(at - 1U);
    report(at);
    TEST_ASSERT_EQUAL_UINT32(at + 1U, s_out_count); /* the final response, not TesterPresent's */
    TEST_ASSERT_EQUAL_HEX8(0x71, s_out[at].data[1]);
}

/* The confirmation of the response's frame: passed 1 us before its N_As runs out; at that
 * moment it is late, dropped, and lib/isotp ends the transmission with N_TIMEOUT_A. */
static void test_a_confirmation_counts_until_the_frame_s_n_as_runs_out(void)
{
    const uint64_t handed_out = s_now;

    request_sf((const uint8_t[]){0x3E, 0x00}, 2U);
    s_now = handed_out + DIAG_TIMEOUT_US - 1U;
    report(0U);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->confirms_passed);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->responses_sent);

    s_now += 1000U;
    request_sf((const uint8_t[]){0x3E, 0x00}, 2U);
    s_now += DIAG_TIMEOUT_US;
    report(1U);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->confirms_late);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->confirms_passed);
    diag_poll(&s_diag, s_now);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->responses_failed);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_isotp_stats(&s_diag)->tx_results[ISOTP_RESULT_TIMEOUT_A]);
    TEST_ASSERT_EQUAL_UINT32(0U, diag_isotp_stats(&s_diag)->confirm_unmatched);
}

/* A report newer than the oldest record means that frame was discarded unsent: its record is
 * forgotten. A report for no frame is dropped; lib/isotp sees neither. */
static void test_records_of_discarded_frames_are_forgotten_and_unknown_reports_dropped(void)
{
    request_sf((const uint8_t[]){0x3E, 0x00}, 2U);
    diag_on_tx_done(&s_diag, s_out_stamp[0] + 1U, s_now);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->confirms_forgotten);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->confirms_unknown);
    TEST_ASSERT_EQUAL_UINT32(0U, diag_stats(&s_diag)->confirms_passed);
    diag_on_tx_done(&s_diag, s_out_stamp[0], s_now); /* its record is gone */
    TEST_ASSERT_EQUAL_UINT32(2U, diag_stats(&s_diag)->confirms_unknown);
    TEST_ASSERT_EQUAL_UINT32(0U, diag_isotp_stats(&s_diag)->confirmed);
    TEST_ASSERT_EQUAL_UINT32(2U, diag_stats(&s_diag)->confirms);
}

/* More frames handed out than the records hold (lib/isotp gave them up unconfirmed): the oldest
 * record goes. */
static void test_a_full_record_drops_its_oldest_entry(void)
{
    for (uint32_t i = 0U; i <= DIAG_HANDOUTS; i++) {
        request_sf((const uint8_t[]){0x3E, 0x00}, 2U);
        s_now += DIAG_TIMEOUT_US;
        diag_poll(&s_diag, s_now); /* N_As: given up */
    }

    TEST_ASSERT_EQUAL_UINT32(DIAG_HANDOUTS + 1U, s_out_count);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->confirms_forgotten);
    /* The newest: the 7 records before it are forgotten, and it is late. */
    report(DIAG_HANDOUTS);
    TEST_ASSERT_EQUAL_UINT32(DIAG_HANDOUTS, diag_stats(&s_diag)->confirms_forgotten);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->confirms_late);
}

/* ECUReset: due once the transmission of its response has ended, not before; nothing is sent
 * after it. */
static void test_an_ecureset_is_due_after_its_response(void)
{
    static const uint8_t ok[8] = {0x02, 0x51, 0x01, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

    request_sf((const uint8_t[]){0x11, 0x01}, 2U);
    expect_frame(0U, ok);
    TEST_ASSERT_FALSE(diag_reset_due(&s_diag));
    s_now += 3U * MS;
    report(0U);
    TEST_ASSERT_TRUE(diag_reset_due(&s_diag));
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->resets);
    TEST_ASSERT_EQUAL_UINT32(1U, s_backend.stats.resets);
    request_sf((const uint8_t[]){0x3E, 0x00}, 2U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_out_count, "nothing is sent once the reset is due");
}

/* ECUReset whose response does not get through: due when lib/isotp gives the response up, at
 * N_As. With SPRMIB there is no response, and the reset is due at once. */
static void test_an_ecureset_without_a_response_is_due_at_once_or_at_n_as(void)
{
    const uint64_t start = s_now;

    request_sf((const uint8_t[]){0x11, 0x01}, 2U);
    diag_poll(&s_diag, start + DIAG_TIMEOUT_US - 1U);
    TEST_ASSERT_FALSE(diag_reset_due(&s_diag));
    diag_poll(&s_diag, start + DIAG_TIMEOUT_US);
    TEST_ASSERT_TRUE(diag_reset_due(&s_diag));
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->responses_failed);

    setUp();
    request_sf((const uint8_t[]){0x11, 0x81}, 2U);
    TEST_ASSERT_TRUE(diag_reset_due(&s_diag));
    TEST_ASSERT_EQUAL_UINT32(0U, s_out_count);
}

/* A tester that waits for each response gets its 51 01 before the reset, on a bus that is slow
 * but carries every frame within N_As. Nothing ahead: the 51 01 confirmed 1 us before its N_As
 * runs out, the reset due then and not before. Behind the last frame of the previous response,
 * which the tester has received but whose confirmation comes 1 us before its N_As: the 51 01
 * goes out after it and is confirmed 1 us before its own N_As, nearly 2 x N_As after that frame's
 * hand-out, and the reset is due then, not before: its deadline, 2 x N_As after the request,
 * comes later. */
static void test_an_ecureset_of_a_tester_that_waits_is_due_only_after_its_response(void)
{
    static const uint8_t ok[8] = {0x02, 0x51, 0x01, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    uint64_t handed_out = s_now;

    request_sf((const uint8_t[]){0x11, 0x01}, 2U);
    expect_frame(0U, ok);
    TEST_ASSERT_EQUAL_UINT64(handed_out + DIAG_TIMEOUT_US, diag_next_deadline(&s_diag));
    s_now = handed_out + DIAG_TIMEOUT_US - 1U;
    diag_poll(&s_diag, s_now);
    TEST_ASSERT_FALSE(diag_reset_due(&s_diag));
    report(0U);
    TEST_ASSERT_TRUE(diag_reset_due(&s_diag));
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->responses_sent);

    setUp();
    handed_out = s_now;
    request_sf((const uint8_t[]){0x3E, 0x00}, 2U); /* 7E 00, which the tester receives */
    s_now = handed_out + MS;
    request_sf((const uint8_t[]){0x11, 0x01}, 2U); /* before the node has the confirmation */
    TEST_ASSERT_EQUAL_UINT32(1U, s_out_count);
    s_now = handed_out + DIAG_TIMEOUT_US - 1U;
    report(0U);
    TEST_ASSERT_EQUAL_UINT32(2U, s_out_count);
    expect_frame(1U, ok);
    s_now += DIAG_TIMEOUT_US - 1U;
    diag_poll(&s_diag, s_now);
    TEST_ASSERT_FALSE_MESSAGE(diag_reset_due(&s_diag), "the 51 01 is not confirmed yet");
    report(1U);
    TEST_ASSERT_TRUE(diag_reset_due(&s_diag));
    TEST_ASSERT_EQUAL_UINT32(2U, diag_stats(&s_diag)->responses_sent);
    TEST_ASSERT_EQUAL_UINT32(0U, diag_stats(&s_diag)->responses_failed);
}

/* ECUReset from a tester that did not wait for the response before, which it holds with its flow
 * control (FC WAIT, each within N_Bs): the 51 01 waits in the second buffer, and the reset is due
 * at its deadline, 2 x N_As after the request, not 1 us before, with the earlier response cut
 * after its first frame and the 51 01 never handed out. Once the earliest, the deadline is the
 * connection's next one. */
static void test_an_ecureset_behind_a_held_response_is_due_at_its_deadline(void)
{
    static const uint8_t wait[8] = {0x31, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    const diag_stats_t *stats = diag_stats(&s_diag);
    uint64_t requested;

    request_sf((const uint8_t[]){0x22, 0xFD, 0x00}, 3U); /* 15 bytes: a first frame first */
    TEST_ASSERT_EQUAL_UINT32(1U, s_out_count);
    report(0U);
    s_now += MS;
    requested = s_now;
    request_sf((const uint8_t[]){0x11, 0x01}, 2U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_out_count);
    TEST_ASSERT_EQUAL_UINT32(1U, stats->resets);

    /* Two WAITs 900 ms apart; after the second, N_Bs runs to 2.8 s after the request. */
    for (uint64_t i = 1U; i <= 2U; i++) {
        s_now = requested + (i * 900U * MS);
        deliver(wait, 8U);
        TEST_ASSERT_FALSE(diag_reset_due(&s_diag));
    }

    TEST_ASSERT_EQUAL_UINT64(requested + DIAG_RESET_DEADLINE_US, diag_next_deadline(&s_diag));
    diag_poll(&s_diag, requested + DIAG_RESET_DEADLINE_US - 1U);
    TEST_ASSERT_FALSE(diag_reset_due(&s_diag));
    diag_poll(&s_diag, requested + DIAG_RESET_DEADLINE_US);
    TEST_ASSERT_TRUE(diag_reset_due(&s_diag));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_out_count, "the 51 01 was never handed out");
    TEST_ASSERT_EQUAL_UINT32(2U, diag_isotp_stats(&s_diag)->frames_in[ISOTP_FRAME_FC_WAIT]);
    TEST_ASSERT_EQUAL_UINT32(2U, stats->responses);
    TEST_ASSERT_EQUAL_UINT32(0U, stats->responses_sent + stats->responses_failed +
                                     stats->responses_dropped);
}

/* A frame the transmit path refuses ends the response with N_ERROR at once. */
static void test_a_frame_the_transmit_path_refuses_ends_the_response(void)
{
    s_refuse = true;
    request_sf((const uint8_t[]){0x3E, 0x00}, 2U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_out_count);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->frames_refused);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->responses_failed);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_isotp_stats(&s_diag)->tx_results[ISOTP_RESULT_ERROR]);
    s_refuse = false;
    request_sf((const uint8_t[]){0x3E, 0x00}, 2U); /* the next one goes */
    TEST_ASSERT_EQUAL_UINT32(1U, s_out_count);
}

/* A job the flash task does not take: generalProgrammingFailure at once, counted; a job that
 * ends with a refusal of lib/boot: its NRC (conditionsNotCorrect for BOOT_E_STATE). */
static void test_the_backend_answers_a_job_it_could_not_post_and_a_refused_one(void)
{
    static const uint8_t failure[8] = {0x03, 0x7F, 0x31, 0x72, 0xCC, 0xCC, 0xCC, 0xCC};
    static const uint8_t refused[8] = {0x03, 0x7F, 0x31, 0x22, 0xCC, 0xCC, 0xCC, 0xCC};
    uint32_t at;

    enter_programming();
    s_post_refused = true;
    request_erase(0x00102000U, 0x00001000U);
    at = s_out_count - 1U;
    expect_frame(at, failure);
    report(at);
    TEST_ASSERT_EQUAL_UINT32(1U, s_backend.stats.post_refused);
    TEST_ASSERT_EQUAL_UINT32(0U, s_jobs_posted);

    s_post_refused = false;
    request_erase(0x00102000U, 0x00001000U);
    s_job_result = (flash_job_result_t){.status = BOOT_E_STATE, .steps = 1U};
    s_job_finished = true;
    diag_poll(&s_diag, s_now);
    expect_frame(s_out_count - 1U, refused);
    TEST_ASSERT_EQUAL_UINT32(1U, s_backend.stats.jobs_failed[FLASH_JOB_ERASE]);
    TEST_ASSERT_EQUAL(BOOT_E_STATE, s_backend.stats.last_status[FLASH_JOB_ERASE]);
}

/* ClearDiagnosticInformation: asked of the health task once, positive only once it is made. */
static void test_the_clear_waits_until_the_health_task_has_made_it(void)
{
    static const uint8_t ok[8] = {0x01, 0x54, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

    request_sf((const uint8_t[]){0x14, 0xFF, 0xFF, 0xFF}, 4U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_clear_posts);
    TEST_ASSERT_EQUAL_UINT32(0U, s_out_count);
    s_now += 10U * MS;
    diag_poll(&s_diag, s_now);
    TEST_ASSERT_EQUAL_UINT32(0U, s_out_count);
    s_clear_made = true;
    diag_poll(&s_diag, s_now);
    TEST_ASSERT_EQUAL_UINT32(1U, s_out_count);
    expect_frame(0U, ok);
    TEST_ASSERT_EQUAL_UINT32(1U, s_clear_posts);
    TEST_ASSERT_EQUAL_UINT32(1U, s_backend.stats.clears);
    TEST_ASSERT_EQUAL_UINT32(1U, s_backend.stats.clears_done);
}

/* DID 0xFD00 carries what the node's boot_info gives (a response of 15 bytes, in a first frame
 * and two consecutive frames after the tester's flow control); no DTC snapshot is
 * generalReject. */
static void test_the_boot_information_and_a_missing_snapshot(void)
{
    static const uint8_t ff[8] = {0x10, 15U, 0x62, 0xFD, 0x00, 0x01, 0x01, 0x01};
    static const uint8_t cf1[8] = {0x21, 0x02, 0x01, 0x01, 0x00, 0xFF, 0x01, 0x02};
    static const uint8_t cf2[8] = {0x22, 0x00, 0x03, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    static const uint8_t reject[8] = {0x03, 0x7F, 0x19, 0x10, 0xCC, 0xCC, 0xCC, 0xCC};
    static const uint8_t fc[8] = {0x30, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

    request_sf((const uint8_t[]){0x22, 0xFD, 0x00}, 3U);
    expect_frame(0U, ff);
    report(0U);
    deliver(fc, 8U);
    /* lib/isotp hands out a consecutive frame no earlier than STmin (0) after the confirmation
     * of the one before: one at a time. */
    TEST_ASSERT_EQUAL_UINT32(2U, s_out_count);
    expect_frame(1U, cf1);
    report(1U);
    TEST_ASSERT_EQUAL_UINT32(3U, s_out_count);
    expect_frame(2U, cf2);
    report(2U);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->responses_sent);

    request_sf((const uint8_t[]){0x19, 0x0A}, 2U);
    expect_frame(3U, reject);
    TEST_ASSERT_EQUAL_UINT32(1U, s_backend.stats.snapshots);
}

/* The flow control frame of the tester can come before the confirmation of the node's first
 * frame (the receive task above the transmit task): lib/isotp takes it as the proof. */
static void test_a_flow_control_before_the_confirmation_is_taken(void)
{
    static const uint8_t fc[8] = {0x30, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

    request_sf((const uint8_t[]){0x22, 0xFD, 0x00}, 3U);
    deliver(fc, 8U); /* before the first frame's report */
    TEST_ASSERT_EQUAL_UINT32(2U, s_out_count);
    report(0U); /* late, but within N_As: passed, and changes nothing */
    TEST_ASSERT_EQUAL_UINT32(2U, s_out_count);
    report(1U);
    TEST_ASSERT_EQUAL_UINT32(3U, s_out_count);
    report(2U);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_stats(&s_diag)->responses_sent);
    TEST_ASSERT_EQUAL_UINT32(1U, diag_isotp_stats(&s_diag)->proven);
}

/* A request of up to 111 bytes: a single frame, or a first frame and consecutive frames after the
 * node's flow control (confirmed). */
static void request(const uint8_t *req, uint32_t len)
{
    if (len <= 7U) {
        request_sf(req, len);
    } else {
        uint8_t frame[8] = {0x10, (uint8_t)len, 0U, 0U, 0U, 0U, 0U, 0U};
        uint32_t sn = 1U;

        (void)memcpy(&frame[2], req, 6U);
        deliver(frame, 8U);
        report(s_out_count - 1U); /* the flow control */

        for (uint32_t at = 6U; at < len; at += 7U) {
            const uint32_t n = ((len - at) < 7U) ? (len - at) : 7U;

            (void)memset(frame, 0xCC, sizeof(frame));
            frame[0] = (uint8_t)(0x20U | (sn & 0x0FU));
            (void)memcpy(&frame[1], &req[at], n);
            deliver(frame, 8U);
            sn++;
        }
    }
}

/* The node's last frame is a single frame with this response; it is confirmed. */
static void expect_response(const uint8_t *rsp, uint32_t len)
{
    uint8_t expected[8];

    (void)memset(expected, 0xCC, sizeof(expected));
    expected[0] = (uint8_t)len;
    (void)memcpy(&expected[1], rsp, len);
    expect_frame(s_out_count - 1U, expected);
    report(s_out_count - 1U);
}

/* The flash task finishes the job posted, of `kind`, with `status`; the poll that follows. */
static void finish_job(flash_job_kind_t kind, boot_status_t status)
{
    TEST_ASSERT_EQUAL(kind, s_job.kind);
    s_job_result = (flash_job_result_t){.status = status, .steps = 3U};
    s_job_finished = true;
    diag_poll(&s_diag, s_now);
}

/* Every job of a download and the confirm routine, through lib/uds and the backend: each posted
 * with the request's arguments and answered when the flash task is done; the exit's BOOT_BUSY is
 * its success. */
static void test_the_backend_runs_every_job_of_a_download(void)
{
    static const uint8_t erase[] = {0x31, 0x01, 0xFF, 0x00, 0x44, 0x00, 0x10,
                                    0x20, 0x00, 0x00, 0x00, 0x00, 0x04};
    static const uint8_t download[] = {0x34, 0x00, 0x44, 0x00, 0x10, 0x20,
                                       0x00, 0x00, 0x00, 0x00, 0x04};
    static const uint8_t block[] = {0x36, 0x01, 0xDE, 0xAD, 0xBE, 0xEF};
    static const uint8_t check[] = {0x31, 0x01, 0xFF, 0x01};
    static const uint8_t confirm[] = {0x31, 0x01, 0xF0, 0x00};

    enter_programming();
    request(erase, LEN(erase));
    finish_job(FLASH_JOB_ERASE, BOOT_OK);
    expect_response((const uint8_t[]){0x71, 0x01, 0xFF, 0x00, 0x00, 0x00}, 6U);
    request(download, LEN(download));
    expect_response((const uint8_t[]){0x74, 0x20, 0x04, 0x02}, 4U);
    request(block, LEN(block));
    TEST_ASSERT_EQUAL_UINT32(0U, s_job.offset);
    TEST_ASSERT_EQUAL_UINT32(4U, s_job.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&block[2], s_job.data, 4U);
    finish_job(FLASH_JOB_WRITE, BOOT_OK);
    expect_response((const uint8_t[]){0x76, 0x01}, 2U);
    request((const uint8_t[]){0x37}, 1U);
    finish_job(FLASH_JOB_EXIT, BOOT_BUSY);
    expect_response((const uint8_t[]){0x77}, 1U);
    request(check, LEN(check));
    finish_job(FLASH_JOB_CHECK, BOOT_OK);
    expect_response((const uint8_t[]){0x71, 0x01, 0xFF, 0x01, 0x00, 0x00}, 6U);

    exchange((const uint8_t[]){0x10, 0x01}, 2U,
             (const uint8_t[]){0x50, 0x01, 0x00, 0x32, 0x01, 0xF4}, 6U);
    exchange((const uint8_t[]){0x10, 0x03}, 2U,
             (const uint8_t[]){0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}, 6U);
    request(confirm, LEN(confirm));
    finish_job(FLASH_JOB_CONFIRM, BOOT_OK);
    expect_response((const uint8_t[]){0x71, 0x01, 0xF0, 0x00, 0x00, 0x00}, 6U);

    for (uint32_t kind = (uint32_t)FLASH_JOB_ERASE; kind < (uint32_t)FLASH_JOB_KINDS; kind++) {
        TEST_ASSERT_EQUAL_UINT32(1U, s_backend.stats.jobs[kind]);
        TEST_ASSERT_EQUAL_UINT32(0U, s_backend.stats.jobs_failed[kind]);
        TEST_ASSERT_EQUAL_UINT32(3U, s_backend.stats.last_steps[kind]);
    }

    TEST_ASSERT_EQUAL(BOOT_BUSY, s_backend.stats.last_status[FLASH_JOB_EXIT]);
    TEST_ASSERT_EQUAL_UINT32(5U, s_jobs_posted);
}

/* The next deadline is the earlier of lib/isotp's and lib/uds's. */
static void test_the_next_deadline_is_the_earlier_of_the_two(void)
{
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, diag_next_deadline(&s_diag));
    request_sf((const uint8_t[]){0x3E, 0x00}, 2U);
    TEST_ASSERT_EQUAL_UINT64(s_now + DIAG_TIMEOUT_US, diag_next_deadline(&s_diag)); /* N_As */
    report(0U);
    request_sf((const uint8_t[]){0x10, 0x03}, 2U);
    report(1U);
    /* S3server, 5 s, in the extended session. */
    TEST_ASSERT_EQUAL_UINT64(s_now + (5000U * MS), diag_next_deadline(&s_diag));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_EXTENDED, diag_session(&s_diag));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_refuses_what_it_cannot_use);
    RUN_TEST(test_a_single_frame_request_gets_a_padded_single_frame_response);
    RUN_TEST(test_flow_control_after_the_first_frame_and_every_16_consecutive_frames);
    RUN_TEST(test_a_message_longer_than_the_buffer_gets_overflow);
    RUN_TEST(test_a_response_waits_for_the_transmission_ahead_of_it);
    RUN_TEST(test_a_response_that_finds_both_buffers_in_use_is_dropped);
    RUN_TEST(test_a_confirmation_counts_until_the_frame_s_n_as_runs_out);
    RUN_TEST(test_records_of_discarded_frames_are_forgotten_and_unknown_reports_dropped);
    RUN_TEST(test_a_full_record_drops_its_oldest_entry);
    RUN_TEST(test_an_ecureset_is_due_after_its_response);
    RUN_TEST(test_an_ecureset_without_a_response_is_due_at_once_or_at_n_as);
    RUN_TEST(test_an_ecureset_of_a_tester_that_waits_is_due_only_after_its_response);
    RUN_TEST(test_an_ecureset_behind_a_held_response_is_due_at_its_deadline);
    RUN_TEST(test_a_frame_the_transmit_path_refuses_ends_the_response);
    RUN_TEST(test_the_backend_answers_a_job_it_could_not_post_and_a_refused_one);
    RUN_TEST(test_the_clear_waits_until_the_health_task_has_made_it);
    RUN_TEST(test_the_boot_information_and_a_missing_snapshot);
    RUN_TEST(test_a_flow_control_before_the_confirmation_is_taken);
    RUN_TEST(test_the_backend_runs_every_job_of_a_download);
    RUN_TEST(test_the_next_deadline_is_the_earlier_of_the_two);
    return UNITY_END();
}
