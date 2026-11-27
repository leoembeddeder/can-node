/* SPDX-License-Identifier: MIT */
/*
 * What the UDS server's tests share: a fake clock, one server, the caller's response buffer,
 * and a fake backend (uds_backend_t) whose every operation answers PENDING for a number of
 * calls the test chooses and then DONE with a result the test chooses, and records how it was
 * called. Every time is the fake clock's: nothing here reads the host's.
 */
#ifndef FCN_UDS_TEST_SUPPORT_H
#define FCN_UDS_TEST_SUPPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "uds/uds.h"

/* Microseconds per millisecond, for the fake clock. */
#define MS ((uint64_t)1000U)

/* The time the harness starts at: not 0, so that a time taken as 0 by mistake shows. */
#define T0_US ((uint64_t)1000000U)

/* One backend operation of the fake. */
typedef struct {
    uint32_t pending;     /* calls that answer PENDING before the one that answers DONE */
    uint8_t nrc;          /* the NRC of an operation that answers with one */
    boot_status_t status; /* the status of an operation that answers with lib/boot's */
    uint32_t starts;      /* UDS_CALL_START calls */
    uint32_t polls;       /* UDS_CALL_POLL calls */
    uint32_t done;        /* calls that answered DONE */
    uint32_t left;        /* PENDING answers left in the operation running now */
    bool running;         /* started and not DONE yet */
} fake_op_t;

/* The size of the image buffer the fake's writes land in. */
#define FAKE_IMAGE_MAX (64U * 1024U)

typedef struct {
    fake_op_t ecu_reset;
    fake_op_t boot_info;
    fake_op_t dtc_snapshot;
    fake_op_t dtc_clear;
    fake_op_t erase;
    fake_op_t write;
    fake_op_t transfer_exit;
    fake_op_t check;
    fake_op_t confirm;
    uds_boot_info_t info;   /* what boot_info answers */
    const dtc_state_t *dtc; /* what dtc_snapshot answers */
    uint8_t reset_type;     /* the arguments of the last start */
    uint32_t group;
    uint32_t erase_address;
    uint32_t erase_size;
    uint32_t write_offset;
    uint32_t write_len;
    uint32_t args_changed;         /* a POLL whose arguments differ from its START's */
    bool cleared;                  /* dtc_clear answered DONE without an NRC */
    uint8_t image[FAKE_IMAGE_MAX]; /* every write that answered DONE with BOOT_OK, at its offset */
    uint32_t bytes_written;        /* bytes of those writes */
    uint32_t calls_out_of_order;   /* a POLL with nothing running, or a START while one runs */
} fake_backend_t;

extern fake_backend_t g_fake;
extern uds_server_t g_srv;
extern uint64_t g_now; /* the fake clock, microseconds */
extern uint8_t g_rsp[UDS_RESPONSE_MAX];
extern uint32_t
    g_rsp_cap; /* the size given to the server: UDS_RESPONSE_MAX unless a test sets it */
extern uds_output_t g_out;

/* The fake backend's functions. */
uds_backend_t fake_backend(void);

/* A fresh fake (every operation DONE at once, BOOT_OK, no NRC), a fresh server on it, the
 * clock at T0_US. */
void harness_init(void);

/* A fresh server on `backend`; the fake and the clock are left as they are. */
void harness_init_with(const uds_backend_t *backend);

/* Deliver `len` bytes at the clock's time; the length of the response (g_out.len). */
uint32_t send(const uint8_t *req, uint32_t len);

/* uds_poll() at `now_us`, which becomes the clock's time; the length of what it gave. */
uint32_t poll_at(uint64_t now_us);

/* The last call gave exactly these bytes / this negative response / a responsePending for
 * `sid` / nothing. */
void expect_rsp(const uint8_t *bytes, uint32_t len);
void expect_nrc(uint8_t sid, uint8_t nrc);
void expect_pending(uint8_t sid);
void expect_none(void);

/* Send and expect, in one call. */
void send_expect(const uint8_t *req, uint32_t req_len, const uint8_t *rsp, uint32_t rsp_len);
void send_expect_nrc(const uint8_t *req, uint32_t req_len, uint8_t nrc);

/* Enter `session` by the transitions the server allows (programming through extended). */
void enter_session(uint8_t session);

/* The count of negative responses with `nrc` so far (uds_get_stats()). */
uint32_t nrc_count(uint8_t nrc);

/* The server's statistics now. */
uds_stats_t stats(void);

#endif /* FCN_UDS_TEST_SUPPORT_H */
