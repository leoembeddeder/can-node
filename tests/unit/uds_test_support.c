/* SPDX-License-Identifier: MIT */
/*
 * The fake clock, the server and the fake backend of the UDS tests (uds_test_support.h).
 */
#include "uds_test_support.h"

#include <string.h>

#include "unity.h"

fake_backend_t g_fake;
uds_server_t g_srv;
uint64_t g_now;
uint8_t g_rsp[UDS_RESPONSE_MAX];
uint32_t g_rsp_cap;
uds_output_t g_out;

/* The operation running now, or NULL: the server must run one at a time. */
static fake_op_t *s_running;

/* One call of an operation: PENDING while it has PENDING answers left, then DONE. */
static uds_be_t step(fake_op_t *op, uds_call_t call)
{
    uds_be_t be = UDS_BE_DONE;

    if (call == UDS_CALL_START) {
        if (s_running != NULL) {
            g_fake.calls_out_of_order++;
        }
        op->starts++;
        op->running = true;
        op->left = op->pending;
        s_running = op;
    } else {
        if ((s_running != op) || !op->running) {
            g_fake.calls_out_of_order++;
        }
        op->polls++;
    }

    if (op->left > 0U) {
        op->left--;
        be = UDS_BE_PENDING;
    } else {
        op->running = false;
        op->done++;
        s_running = NULL;
    }

    return be;
}

static uds_be_t fake_ecu_reset(void *ctx, uds_call_t call, uint8_t reset_type, uint8_t *nrc)
{
    fake_backend_t *f = ctx;
    const uds_be_t be = step(&f->ecu_reset, call);

    if (call == UDS_CALL_START) {
        f->reset_type = reset_type;
    } else if (reset_type != f->reset_type) {
        f->args_changed++;
    }
    if (be == UDS_BE_DONE) {
        *nrc = f->ecu_reset.nrc;
    }
    return be;
}

static uds_be_t fake_boot_info(void *ctx, uds_call_t call, uds_boot_info_t *info, uint8_t *nrc)
{
    fake_backend_t *f = ctx;
    const uds_be_t be = step(&f->boot_info, call);

    if (be == UDS_BE_DONE) {
        *info = f->info;
        *nrc = f->boot_info.nrc;
    }
    return be;
}

static uds_be_t fake_dtc_snapshot(void *ctx, uds_call_t call, const dtc_state_t **state,
                                  uint8_t *nrc)
{
    fake_backend_t *f = ctx;
    const uds_be_t be = step(&f->dtc_snapshot, call);

    if (be == UDS_BE_DONE) {
        *state = f->dtc;
        *nrc = f->dtc_snapshot.nrc;
    }
    return be;
}

static uds_be_t fake_dtc_clear(void *ctx, uds_call_t call, uint32_t group, uint8_t *nrc)
{
    fake_backend_t *f = ctx;
    const uds_be_t be = step(&f->dtc_clear, call);

    if (call == UDS_CALL_START) {
        f->group = group;
    } else if (group != f->group) {
        f->args_changed++;
    }
    if (be == UDS_BE_DONE) {
        *nrc = f->dtc_clear.nrc;
        f->cleared = (*nrc == UDS_NRC_NONE);
    }
    return be;
}

static uds_be_t fake_erase(void *ctx, uds_call_t call, uint32_t address, uint32_t size,
                           boot_status_t *status)
{
    fake_backend_t *f = ctx;
    const uds_be_t be = step(&f->erase, call);

    if (call == UDS_CALL_START) {
        f->erase_address = address;
        f->erase_size = size;
    } else if ((address != f->erase_address) || (size != f->erase_size)) {
        f->args_changed++;
    }
    if (be == UDS_BE_DONE) {
        *status = f->erase.status;
    }
    return be;
}

static uds_be_t fake_write(void *ctx, uds_call_t call, uint32_t offset, const uint8_t *data,
                           uint32_t len, boot_status_t *status)
{
    fake_backend_t *f = ctx;
    const uds_be_t be = step(&f->write, call);

    if (call == UDS_CALL_START) {
        f->write_offset = offset;
        f->write_len = len;
    } else if ((offset != f->write_offset) || (len != f->write_len)) {
        f->args_changed++;
    }
    if (be == UDS_BE_DONE) {
        *status = f->write.status;
        if ((f->write.status == BOOT_OK) && (offset <= FAKE_IMAGE_MAX) &&
            (len <= (FAKE_IMAGE_MAX - offset))) {
            (void)memcpy(&f->image[offset], data, len);
            f->bytes_written += len;
        }
    }
    return be;
}

static uds_be_t fake_transfer_exit(void *ctx, uds_call_t call, boot_status_t *status)
{
    fake_backend_t *f = ctx;
    const uds_be_t be = step(&f->transfer_exit, call);

    if (be == UDS_BE_DONE) {
        *status = f->transfer_exit.status;
    }
    return be;
}

static uds_be_t fake_check(void *ctx, uds_call_t call, boot_status_t *status)
{
    fake_backend_t *f = ctx;
    const uds_be_t be = step(&f->check, call);

    if (be == UDS_BE_DONE) {
        *status = f->check.status;
    }
    return be;
}

static uds_be_t fake_confirm(void *ctx, uds_call_t call, boot_status_t *status)
{
    fake_backend_t *f = ctx;
    const uds_be_t be = step(&f->confirm, call);

    if (be == UDS_BE_DONE) {
        *status = f->confirm.status;
    }
    return be;
}

uds_backend_t fake_backend(void)
{
    uds_backend_t b;

    (void)memset(&b, 0, sizeof(b));
    b.ctx = &g_fake;
    b.ecu_reset = fake_ecu_reset;
    b.boot_info = fake_boot_info;
    b.dtc_snapshot = fake_dtc_snapshot;
    b.dtc_clear = fake_dtc_clear;
    b.erase = fake_erase;
    b.write = fake_write;
    b.transfer_exit = fake_transfer_exit;
    b.check = fake_check;
    b.confirm = fake_confirm;
    return b;
}

void harness_init_with(const uds_backend_t *backend)
{
    (void)memset(&g_srv, 0x5A, sizeof(g_srv)); /* uds_init() must not rely on zeroed memory */
    TEST_ASSERT_TRUE(uds_init(&g_srv, backend));
    (void)memset(g_rsp, 0, sizeof(g_rsp));
    (void)memset(&g_out, 0, sizeof(g_out));
    g_rsp_cap = UDS_RESPONSE_MAX;
}

void harness_init(void)
{
    (void)memset(&g_fake, 0, sizeof(g_fake));
    g_fake.ecu_reset.status = BOOT_OK;
    g_fake.boot_info.status = BOOT_OK;
    g_fake.dtc_snapshot.status = BOOT_OK;
    g_fake.dtc_clear.status = BOOT_OK;
    g_fake.erase.status = BOOT_OK;
    g_fake.write.status = BOOT_OK;
    g_fake.transfer_exit.status = BOOT_BUSY; /* what the first finish step answers */
    g_fake.check.status = BOOT_OK;
    g_fake.confirm.status = BOOT_OK;
    s_running = NULL;
    g_now = T0_US;

    const uds_backend_t backend = fake_backend();

    harness_init_with(&backend);
}

uint32_t send(const uint8_t *req, uint32_t len)
{
    (void)memset(g_rsp, 0xEE, sizeof(g_rsp));
    TEST_ASSERT_TRUE(uds_request(&g_srv, req, len, g_now, g_rsp, g_rsp_cap, &g_out));
    return g_out.len;
}

uint32_t poll_at(uint64_t now_us)
{
    g_now = now_us;
    (void)memset(g_rsp, 0xEE, sizeof(g_rsp));
    TEST_ASSERT_TRUE(uds_poll(&g_srv, g_now, g_rsp, g_rsp_cap, &g_out));
    return g_out.len;
}

void expect_rsp(const uint8_t *bytes, uint32_t len)
{
    TEST_ASSERT_EQUAL_UINT32(len, g_out.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bytes, g_rsp, len);
}

void expect_nrc(uint8_t sid, uint8_t nrc)
{
    const uint8_t expect[3] = {UDS_SID_NEGATIVE, sid, nrc};

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3U, g_out.len, "a negative response");
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, g_rsp, 3U);
    TEST_ASSERT_FALSE(g_out.reset);
}

void expect_pending(uint8_t sid)
{
    expect_nrc(sid, UDS_NRC_RESPONSE_PENDING);
}

void expect_none(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g_out.len, "nothing to send");
}

void send_expect(const uint8_t *req, uint32_t req_len, const uint8_t *rsp, uint32_t rsp_len)
{
    (void)send(req, req_len);
    expect_rsp(rsp, rsp_len);
}

void send_expect_nrc(const uint8_t *req, uint32_t req_len, uint8_t nrc)
{
    (void)send(req, req_len);
    expect_nrc(req[0], nrc);
}

void enter_session(uint8_t session)
{
    static const uint8_t to_default[] = {0x10, 0x01};
    static const uint8_t to_extended[] = {0x10, 0x03};
    static const uint8_t to_programming[] = {0x10, 0x02};

    if (session == UDS_SESSION_DEFAULT) {
        (void)send(to_default, sizeof(to_default));
    } else {
        if (uds_session(&g_srv) == UDS_SESSION_PROGRAMMING) {
            (void)send(to_default, sizeof(to_default));
        }
        (void)send(to_extended, sizeof(to_extended));
        if (session == UDS_SESSION_PROGRAMMING) {
            (void)send(to_programming, sizeof(to_programming));
        }
    }
    TEST_ASSERT_EQUAL_UINT8(session, uds_session(&g_srv));
}

uds_stats_t stats(void)
{
    uds_stats_t st;

    uds_get_stats(&g_srv, &st);
    return st;
}

uint32_t nrc_count(uint8_t nrc)
{
    const uds_stats_t st = stats();
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < UDS_NRC_KINDS; i++) {
        if (uds_nrc_code(i) == nrc) {
            count = st.nrc[i];
        }
    }
    return count;
}
