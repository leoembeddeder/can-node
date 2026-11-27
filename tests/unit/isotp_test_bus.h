/* SPDX-License-Identifier: MIT */
/*
 * Test support for lib/isotp: a fake clock, an endpoint that wraps one isotp_t and records what
 * its callbacks report, a bus that connects two endpoints (one frame on the wire at a time, a
 * time per frame, confirmations that may come late, frames that may be lost), and a wire checker
 * that judges every frame on that bus by the rules of ISO 15765-2 as docs/diagnostics.md states
 * them, written without looking at lib/isotp/isotp.c.
 *
 * Every test is deterministic: time is the fake clock, and nothing reads the host's.
 */
#ifndef FCN_ISOTP_TEST_BUS_H
#define FCN_ISOTP_TEST_BUS_H

#include <stdbool.h>
#include <stdint.h>

#include "isotp/isotp.h"

/* The identifiers of the node's connection: the node (the instance under test in the
 * single-instance tests) sends 0x7E8 and accepts 0x7E0. */
#define TB_NODE_TX 0x7E8U
#define TB_NODE_RX 0x7E0U

/* Frames an endpoint can have handed out and not yet taken by the bus or the test. */
#define TB_QUEUE 64U
/* Indications and confirmations remembered (the counts go on beyond it). */
#define TB_LOG 64U

/* The fake clock, in microseconds. */
extern uint64_t tb_now_us;

typedef struct {
    can_frame_t frame;
    uint64_t at_us; /* when send_frame took it */
} tb_frame_t;

typedef struct {
    isotp_result_t result;
    uint16_t length;
    uint64_t at_us;
} tb_event_t;

typedef struct {
    isotp_t tp;
    isotp_config_t config;
    uint16_t buffer_size;
    /* The receive buffer, with one guard byte after the part the instance may use. */
    uint8_t buffer[ISOTP_MAX_LENGTH + 1U];
    /* Frames send_frame took, oldest first, not yet taken by the bus or the test. */
    tb_frame_t out[TB_QUEUE];
    uint32_t out_head;
    uint32_t out_count;
    uint32_t handed_out; /* frames send_frame took, in all */
    uint32_t given;      /* frames given to isotp_on_frame() */
    bool refuse;         /* send_frame refuses every frame */
    bool keep;           /* indication keeps the buffer of every message */
    bool probe;          /* every callback tries to call back into the instance */
    uint32_t probes_refused;
    /* What the callbacks reported. */
    uint32_t ff_count;
    uint16_t ff_length;
    uint32_t ind_count;
    tb_event_t ind[TB_LOG];
    uint8_t message[ISOTP_MAX_LENGTH]; /* the last message delivered with OK, copied */
    uint16_t message_length;
    uint32_t messages;
    uint32_t conf_count;
    tb_event_t conf[TB_LOG];
} tb_end_t;

/** The node's configuration with the standard's timeouts: BS 0, STmin 0, no WAIT. */
isotp_config_t tb_config(uint32_t tx_id, uint32_t rx_id);

/** Start an endpoint (asserts that isotp_init() accepts it). Clears every record. */
void tb_start(tb_end_t *end, const isotp_config_t *config, uint16_t buffer_size);

/** Frames waiting in the endpoint's out queue. */
uint32_t tb_waiting(const tb_end_t *end);

/** Take the oldest waiting frame (asserts there is one). */
can_frame_t tb_take(tb_end_t *end);

/** Take the oldest waiting frame and assert its 8 bytes and DLC 8. */
void tb_take_expect(tb_end_t *end, const uint8_t expected[8]);

/** isotp_on_tx_done() at tb_now_us. */
bool tb_confirm(tb_end_t *end);

/** Take the oldest waiting frame and confirm it. */
can_frame_t tb_take_confirm(tb_end_t *end);

/** A frame of `dlc` bytes with the endpoint's config.rx_id given to isotp_on_frame() at
 *  tb_now_us; `bytes` holds `dlc` bytes. */
bool tb_inject(tb_end_t *end, uint8_t dlc, const uint8_t *bytes);

/** The same frame, padded to 8 bytes with 0xCC. */
bool tb_inject8(tb_end_t *end, uint8_t count, const uint8_t *bytes);

/** The last indication and confirmation (assert there is one). */
tb_event_t tb_last_ind(const tb_end_t *end);
tb_event_t tb_last_conf(const tb_end_t *end);

/** Assert the accounting identities of isotp.h (STATISTICS) and that the deadline is later
 *  than tb_now_us (TIMERS). */
void tb_check(const tb_end_t *end);

/* ------------------------------------------------------------------------------- the bus */

typedef enum { TB_DELIVER, TB_DROP, TB_DUPLICATE } tb_fate_t;

typedef struct {
    uint32_t frame_us;       /* time on the bus of one frame, at least 1 */
    uint32_t confirm_lag_us; /* the sender's confirmation comes this long after the frame */
    bool answer_first;       /* with lag 0: the receiver sees the frame before the sender's
                                confirmation comes (with a lag it always does) */
    /* What happens to a frame that would go on the bus; NULL: every frame is delivered. A
     * dropped frame is never confirmed nor delivered (lost in the transmit path). */
    tb_fate_t (*fate)(void *context, const tb_end_t *from, const can_frame_t *frame);
    void *fate_context;
    /* Called for every frame that went over the bus, with its start and end. */
    void (*monitor)(void *context, const tb_end_t *from, const can_frame_t *frame,
                    uint64_t start_us, uint64_t end_us);
    void *monitor_context;
    /* Called after every event of the bus, outside any callback (it may call the instances,
     * for example to release a kept buffer); NULL: nothing. */
    void (*tick)(void *context);
    void *tick_context;
} tb_link_t;

/** Run the two endpoints over one bus until nothing is on it, nothing waits and no timer is
 *  set (true), or until the next event would come after `limit_us` (false). */
bool tb_run(tb_end_t *a, tb_end_t *b, const tb_link_t *link, uint64_t limit_us);

/* ---------------------------------------------------------------------- the wire checker */

/* One direction of the connection as the checker sees it. */
typedef struct {
    bool receiving;  /* a segmented message is under way */
    bool cts_seen;   /* the CTS that allows the current block was seen */
    uint32_t length; /* FF_DL */
    uint32_t got;    /* bytes put together */
    uint8_t next_sn;
    uint8_t bs;           /* of the last CTS */
    uint32_t stmin_us;    /* of the last CTS */
    uint32_t in_block;    /* CFs since the last CTS */
    bool cf_before;       /* a CF of this message went before */
    uint64_t last_cf_end; /* end of the previous CF */
    uint8_t message[ISOTP_MAX_LENGTH];
} tb_direction_t;

typedef struct {
    uint32_t a_tx; /* identifier a sends; b sends the other one */
    uint32_t b_tx;
    tb_direction_t dir[2]; /* [0]: a to b, [1]: b to a */
    uint32_t frames;       /* frames checked */
    uint32_t messages[2];  /* messages completed per direction */
    uint32_t violations;   /* rules broken (the first one fails the test at once) */
    /* The message a direction is expected to carry next, set by the test. */
    const uint8_t *expected[2];
    uint32_t expected_length[2];
} tb_checker_t;

void tb_checker_init(tb_checker_t *checker, uint32_t a_tx, uint32_t b_tx);

/** A monitor for tb_link_t: checks one frame (fails the test at the first broken rule). */
void tb_checker_monitor(void *context, const tb_end_t *from, const can_frame_t *frame,
                        uint64_t start_us, uint64_t end_us);

#endif /* FCN_ISOTP_TEST_BUS_H */
