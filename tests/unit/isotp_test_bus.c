/* SPDX-License-Identifier: MIT */
#include "isotp_test_bus.h"

#include <stdio.h>
#include <string.h>

#include "unity.h"

/* The byte after the usable part of a receive buffer: the instance must never write it. */
#define GUARD_BYTE 0x5AU

uint64_t tb_now_us;

/* ------------------------------------------------------------------------ the callbacks */

static void probe_reentry(tb_end_t *end)
{
    const uint8_t byte = 0x11U;
    const can_frame_t frame = {.id = end->config.rx_id, .dlc = 1U, .data = {0x01U}};

    TEST_ASSERT_EQUAL(ISOTP_E_BUSY, isotp_send(&end->tp, &byte, 1U, tb_now_us));
    TEST_ASSERT_FALSE(isotp_on_frame(&end->tp, &frame, tb_now_us));
    TEST_ASSERT_FALSE(isotp_on_tx_done(&end->tp, tb_now_us));
    isotp_poll(&end->tp, tb_now_us);
    TEST_ASSERT_FALSE(isotp_rx_release(&end->tp, tb_now_us));
    end->probes_refused += 5U;
}

static bool cb_send_frame(void *context, const can_frame_t *frame)
{
    tb_end_t *end = context;
    bool taken = false;

    if (end->probe) {
        probe_reentry(end);
    }
    if (!end->refuse) {
        TEST_ASSERT_TRUE_MESSAGE(end->out_count < TB_QUEUE, "out queue of the test is full");
        tb_frame_t *slot = &end->out[(end->out_head + end->out_count) % TB_QUEUE];
        slot->frame = *frame;
        slot->at_us = tb_now_us;
        end->out_count++;
        end->handed_out++;
        taken = true;
    }

    return taken;
}

static void cb_ff_indication(void *context, uint32_t length)
{
    tb_end_t *end = context;

    if (end->probe) {
        probe_reentry(end);
    }
    end->ff_count++;
    end->ff_length = length;
}

static bool cb_indication(void *context, isotp_result_t result, const uint8_t *data,
                          uint32_t length)
{
    tb_end_t *end = context;

    if (end->probe) {
        probe_reentry(end);
    }
    end->ind[end->ind_count % TB_LOG] =
        (tb_event_t){.result = result, .length = length, .at_us = tb_now_us};
    end->ind_count++;
    if (result == ISOTP_RESULT_OK) {
        TEST_ASSERT_NOT_NULL(data);
        TEST_ASSERT_TRUE(length >= 1U);
        TEST_ASSERT_TRUE(length <= end->buffer_size);
        TEST_ASSERT_TRUE(data == end->buffer);
        if (length <= sizeof(end->message)) {
            (void)memcpy(end->message, data, length);
        }
        end->message_length = length;
        end->messages++;
    } else {
        TEST_ASSERT_NULL(data);
    }

    return end->keep;
}

static void cb_confirm(void *context, isotp_result_t result)
{
    tb_end_t *end = context;

    if (end->probe) {
        probe_reentry(end);
    }
    end->conf[end->conf_count % TB_LOG] =
        (tb_event_t){.result = result, .length = 0U, .at_us = tb_now_us};
    end->conf_count++;
}

/* --------------------------------------------------------------------------- endpoints */

isotp_config_t tb_config(uint32_t tx_id, uint32_t rx_id)
{
    return (isotp_config_t){.tx_id = tx_id,
                            .rx_id = rx_id,
                            .n_as_us = ISOTP_TIMEOUT_US,
                            .n_ar_us = ISOTP_TIMEOUT_US,
                            .n_bs_us = ISOTP_TIMEOUT_US,
                            .n_cr_us = ISOTP_TIMEOUT_US,
                            .n_br_us = 100000U,
                            .n_cs_us = 200000U,
                            .extended_ids = false,
                            .can_fd = false,
                            .can_fd_brs = false,
                            .tx_dl = 0U,
                            .rx_bs = 0U,
                            .rx_stmin = 0U,
                            .rx_wft_max = 0U,
                            .tx_wft_max = 0U};
}

void tb_start(tb_end_t *end, const isotp_config_t *config, uint32_t buffer_size)
{
    const isotp_callbacks_t callbacks = {.send_frame = cb_send_frame,
                                         .ff_indication = cb_ff_indication,
                                         .indication = cb_indication,
                                         .confirm = cb_confirm,
                                         .context = end};

    if ((buffer_size < 1U) || (buffer_size > TB_MAX_LENGTH)) {
        TEST_FAIL_MESSAGE("a test buffer has 1..TB_MAX_LENGTH bytes");
        return;
    }
    (void)memset(end, 0, sizeof(*end));
    /* Nonsense first: isotp_init() must need nothing from the memory it is given. */
    (void)memset(&end->tp, 0xA5, sizeof(end->tp));
    (void)memset(end->buffer, 0xA5, sizeof(end->buffer));
    end->buffer[buffer_size] = GUARD_BYTE;
    end->config = *config;
    end->buffer_size = buffer_size;
    TEST_ASSERT_TRUE(isotp_init(&end->tp, config, &callbacks, end->buffer, buffer_size));
}

uint32_t tb_waiting(const tb_end_t *end)
{
    return end->out_count;
}

can_frame_t tb_take(tb_end_t *end)
{
    TEST_ASSERT_TRUE_MESSAGE(end->out_count > 0U, "no frame was handed out");
    const can_frame_t frame = end->out[end->out_head].frame;
    end->out_head = (end->out_head + 1U) % TB_QUEUE;
    end->out_count--;
    return frame;
}

void tb_take_expect(tb_end_t *end, const uint8_t expected[8])
{
    tb_take_expect_len(end, 8U, expected);
}

void tb_take_expect_len(tb_end_t *end, uint8_t expected_dlc, const uint8_t *expected)
{
    const can_frame_t frame = tb_take(end);
    uint8_t expected_flags = end->config.extended_ids ? (uint8_t)CAN_FLAG_EXT : 0U;

    if (end->config.can_fd) {
        expected_flags |= (uint8_t)CAN_FLAG_FD;
        if (end->config.can_fd_brs) {
            expected_flags |= (uint8_t)CAN_FLAG_BRS;
        }
    }

    TEST_ASSERT_EQUAL_HEX32(end->config.tx_id, frame.id);
    TEST_ASSERT_EQUAL_HEX8(expected_flags, frame.flags);
    TEST_ASSERT_EQUAL_UINT8(expected_dlc, frame.dlc);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame.data, expected_dlc);
}

bool tb_confirm(tb_end_t *end)
{
    return isotp_on_tx_done(&end->tp, tb_now_us);
}

can_frame_t tb_take_confirm(tb_end_t *end)
{
    const can_frame_t frame = tb_take(end);

    TEST_ASSERT_TRUE_MESSAGE(tb_confirm(end), "the confirmation matched no frame");
    return frame;
}

bool tb_inject(tb_end_t *end, uint8_t dlc, const uint8_t *bytes)
{
    can_frame_t frame = {
        .id = end->config.rx_id, .flags = end->config.extended_ids ? CAN_FLAG_EXT : 0U, .dlc = dlc};

    TEST_ASSERT_TRUE(dlc <= 8U);
    (void)memset(frame.data, 0xEE, sizeof(frame.data)); /* beyond the DLC: never looked at */
    if (dlc > 0U) {
        (void)memcpy(frame.data, bytes, dlc);
    }
    end->given++;
    return isotp_on_frame(&end->tp, &frame, tb_now_us);
}

bool tb_inject8(tb_end_t *end, uint8_t count, const uint8_t *bytes)
{
    uint8_t data[8];

    TEST_ASSERT_TRUE(count <= 8U);
    (void)memset(data, 0xCC, sizeof(data));
    (void)memcpy(data, bytes, count);
    return tb_inject(end, 8U, data);
}

bool tb_inject_fd(tb_end_t *end, uint8_t dlc, const uint8_t *bytes)
{
    can_frame_t frame;
    uint8_t flags = end->config.extended_ids ? (uint8_t)CAN_FLAG_EXT : 0U;

    TEST_ASSERT_TRUE(dlc <= CAN_FD_MAX_DL);
    if (end->config.can_fd) {
        flags |= (uint8_t)CAN_FLAG_FD;
        if (end->config.can_fd_brs) {
            flags |= (uint8_t)CAN_FLAG_BRS;
        }
    }
    (void)memset(&frame, 0xEE, sizeof(frame));
    frame.id = end->config.rx_id;
    frame.flags = flags;
    frame.dlc = dlc;
    if (dlc > 0U) {
        (void)memcpy(frame.data, bytes, dlc);
    }
    end->given++;
    return isotp_on_frame(&end->tp, &frame, tb_now_us);
}

tb_event_t tb_last_ind(const tb_end_t *end)
{
    TEST_ASSERT_TRUE_MESSAGE(end->ind_count > 0U, "no indication");
    return end->ind[(end->ind_count - 1U) % TB_LOG];
}

tb_event_t tb_last_conf(const tb_end_t *end)
{
    TEST_ASSERT_TRUE_MESSAGE(end->conf_count > 0U, "no confirmation");
    return end->conf[(end->conf_count - 1U) % TB_LOG];
}

static uint32_t sum(const uint32_t *values, uint32_t count)
{
    uint32_t total = 0U;

    for (uint32_t i = 0U; i < count; i++) {
        total += values[i];
    }
    return total;
}

void tb_check(const tb_end_t *end)
{
    const isotp_t *tp = &end->tp;
    const isotp_stats_t *s = isotp_stats(tp);
    const uint32_t ovflw_due =
        ((tp->rx.state == ISOTP_RX_FC_DUE) && (tp->rx.fc == ISOTP_FRAME_FC_OVFLW)) ? 1U : 0U;

    if (s == NULL) {
        TEST_FAIL_MESSAGE("no statistics");
        return;
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        end->given, sum(s->frames_in, ISOTP_FRAME_KINDS) + sum(s->ignored, ISOTP_IGNORED_REASONS),
        "frames given = processed + ignored");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        s->tx_requests, sum(s->tx_results, ISOTP_RESULTS) + (isotp_tx_busy(tp) ? 1U : 0U),
        "requests = confirmations + in progress");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(s->frames_in[ISOTP_FRAME_FF],
                                     s->ff_indications + s->rx_overflow,
                                     "FFs = FF indications + overflows");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        s->frames_in[ISOTP_FRAME_SF] - s->rx_sf_too_long - s->rx_sf_busy + s->ff_indications,
        sum(s->rx_results, ISOTP_RESULTS) + (isotp_rx_busy(tp) ? 1U : 0U),
        "receptions = indications + in progress");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        s->rx_overflow, s->frames_out[ISOTP_FRAME_FC_OVFLW] + s->fc_dropped + ovflw_due,
        "overflows = OVFLW sent + dropped + due");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sum(s->frames_out, ISOTP_FRAME_KINDS),
                                     s->confirmed + s->unconfirmed + s->outstanding,
                                     "frames out = confirmed + given up + outstanding");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(end->handed_out, sum(s->frames_out, ISOTP_FRAME_KINDS),
                                     "frames out = frames send_frame took");
    TEST_ASSERT_EQUAL_UINT32(0U, s->frames_out[ISOTP_FRAME_FC_RESERVED]);
    TEST_ASSERT_TRUE(s->outstanding <= ISOTP_PIPELINE_DEPTH);
    TEST_ASSERT_EQUAL_UINT32(end->ff_count, s->ff_indications);
    TEST_ASSERT_EQUAL_UINT32(end->ind_count, sum(s->rx_results, ISOTP_RESULTS));
    TEST_ASSERT_EQUAL_UINT32(end->conf_count, sum(s->tx_results, ISOTP_RESULTS));
    TEST_ASSERT_EQUAL_UINT32(end->messages, s->rx_results[ISOTP_RESULT_OK]);
    TEST_ASSERT_EQUAL_UINT32(end->probes_refused, s->reentry_refused);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(GUARD_BYTE, end->buffer[end->buffer_size],
                                   "the instance wrote past its buffer");
    TEST_ASSERT_TRUE_MESSAGE(isotp_next_deadline(tp) > tb_now_us,
                             "after a call the deadline lies in the future");
}

/* ------------------------------------------------------------------------------- the bus */

#define NEVER UINT64_MAX

typedef struct {
    uint64_t at_us[TB_QUEUE];
    uint32_t head;
    uint32_t count;
} confirm_queue_t;

static void push_confirm(confirm_queue_t *queue, uint64_t at_us)
{
    TEST_ASSERT_TRUE(queue->count < TB_QUEUE);
    queue->at_us[(queue->head + queue->count) % TB_QUEUE] = at_us;
    queue->count++;
}

static uint64_t next_confirm(const confirm_queue_t *queue)
{
    return (queue->count > 0U) ? queue->at_us[queue->head] : NEVER;
}

static void pop_confirm(confirm_queue_t *queue)
{
    queue->head = (queue->head + 1U) % TB_QUEUE;
    queue->count--;
}

static void deliver(tb_end_t *to, const can_frame_t *frame)
{
    to->given++;
    (void)isotp_on_frame(&to->tp, frame, tb_now_us);
}

static uint64_t min64(uint64_t a, uint64_t b)
{
    return (a < b) ? a : b;
}

bool tb_run(tb_end_t *a, tb_end_t *b, const tb_link_t *link, uint64_t limit_us)
{
    tb_end_t *ends[2] = {a, b};
    confirm_queue_t confirms[2] = {{.count = 0U}, {.count = 0U}};
    bool in_flight = false;
    bool duplicate = false;
    can_frame_t frame = {.id = 0U};
    uint32_t from = 0U;
    uint64_t start_us = 0U;
    uint64_t end_us = 0U;
    uint64_t bus_free_us = tb_now_us;
    uint32_t steps = 0U;
    bool settled = false;
    bool over = false;

    TEST_ASSERT_TRUE(link->frame_us >= 1U);
    while (!settled && !over) {
        steps++;
        TEST_ASSERT_TRUE_MESSAGE(steps < 50000000U, "the bus does not settle");

        /* The next frame onto the bus: the one handed out first, a's on a tie. */
        if (!in_flight) {
            int pick = -1;
            for (int i = 0; i < 2; i++) {
                if ((ends[i]->out_count > 0U) &&
                    ((pick < 0) || (ends[i]->out[ends[i]->out_head].at_us <
                                    ends[pick]->out[ends[pick]->out_head].at_us))) {
                    pick = i;
                }
            }
            if (pick >= 0) {
                from = (uint32_t)pick;
                frame = tb_take(ends[from]);
                const tb_fate_t fate = (link->fate != NULL)
                                           ? link->fate(link->fate_context, ends[from], &frame)
                                           : TB_DELIVER;
                if (fate != TB_DROP) {
                    in_flight = true;
                    start_us = (tb_now_us > bus_free_us) ? tb_now_us : bus_free_us;
                    end_us = start_us + link->frame_us;
                    duplicate = fate == TB_DUPLICATE;
                }
                continue;
            }
        }

        const uint64_t t_frame = in_flight ? end_us : NEVER;
        const uint64_t t_conf = min64(next_confirm(&confirms[0]), next_confirm(&confirms[1]));
        const uint64_t t_timer = min64(isotp_next_deadline(&a->tp), isotp_next_deadline(&b->tp));
        const uint64_t t_next = min64(t_frame, min64(t_conf, t_timer));

        if (t_next == NEVER) {
            settled = true;
        } else if (t_next > limit_us) {
            over = true;
        } else {
            if (t_next > tb_now_us) {
                tb_now_us = t_next;
            }
            if (t_conf == t_next) {
                const uint32_t i = (next_confirm(&confirms[0]) == t_next) ? 0U : 1U;
                pop_confirm(&confirms[i]);
                (void)isotp_on_tx_done(&ends[i]->tp, tb_now_us);
            } else if (t_frame == t_next) {
                const uint32_t to = 1U - from;
                in_flight = false;
                bus_free_us = end_us;
                if (link->monitor != NULL) {
                    link->monitor(link->monitor_context, ends[from], &frame, start_us, end_us);
                }
                if ((link->confirm_lag_us == 0U) && !link->answer_first) {
                    (void)isotp_on_tx_done(&ends[from]->tp, tb_now_us);
                    deliver(ends[to], &frame);
                } else {
                    deliver(ends[to], &frame);
                    push_confirm(&confirms[from], tb_now_us + link->confirm_lag_us);
                }
                if (duplicate) {
                    deliver(ends[to], &frame);
                }
            } else {
                isotp_poll(&a->tp, tb_now_us);
                isotp_poll(&b->tp, tb_now_us);
            }
            if (link->tick != NULL) {
                link->tick(link->tick_context);
            }
        }
    }

    return settled;
}

/* ---------------------------------------------------------------------- the wire checker */

void tb_checker_init(tb_checker_t *checker, uint32_t a_tx, uint32_t b_tx)
{
    (void)memset(checker, 0, sizeof(*checker));
    checker->a_tx = a_tx;
    checker->b_tx = b_tx;
    checker->a_tx_dl = 8U;
    checker->b_tx_dl = 8U;
}

void tb_checker_init_fd(tb_checker_t *checker, uint32_t a_tx, uint32_t b_tx, uint8_t a_tx_dl,
                        uint8_t b_tx_dl, bool brs)
{
    tb_checker_init(checker, a_tx, b_tx);
    checker->can_fd = true;
    checker->can_fd_brs = brs;
    checker->a_tx_dl = a_tx_dl;
    checker->b_tx_dl = b_tx_dl;
}

static uint32_t checker_stmin_us(uint8_t value)
{
    uint32_t us = 127000U;

    if (value <= 0x7FU) {
        us = value * 1000U;
    } else if ((value >= 0xF1U) && (value <= 0xF9U)) {
        us = (value - 0xF0U) * 100U;
    }
    return us;
}

static void violation(tb_checker_t *checker, const char *rule, const can_frame_t *frame)
{
    char text[200];

    checker->violations++;
    (void)snprintf(text, sizeof(text),
                   "wire rule broken: %s (frame %u: id 0x%03X dlc %u %02X %02X %02X %02X %02X "
                   "%02X %02X %02X)",
                   rule, (unsigned int)checker->frames, (unsigned int)frame->id,
                   (unsigned int)frame->dlc, frame->data[0], frame->data[1], frame->data[2],
                   frame->data[3], frame->data[4], frame->data[5], frame->data[6], frame->data[7]);
    TEST_FAIL_MESSAGE(text);
}

static void check_padding(tb_checker_t *checker, const can_frame_t *frame, uint32_t first,
                          uint32_t total)
{
    for (uint32_t i = first; i < total; i++) {
        if (frame->data[i] != 0xCCU) {
            violation(checker, "padding is 0xCC", frame);
        }
    }
}

static void complete(tb_checker_t *checker, uint32_t d, const can_frame_t *frame)
{
    tb_direction_t *dir = &checker->dir[d];

    if (checker->expected[d] != NULL) {
        if ((dir->length != checker->expected_length[d]) ||
            (memcmp(dir->message, checker->expected[d], dir->length) != 0)) {
            violation(checker, "the message on the wire is the one sent", frame);
        }
    }
    dir->receiving = false;
    checker->messages[d]++;
}

static uint8_t wire_pad_len(uint8_t raw)
{
    return (raw <= 8U) ? 8U : can_fd_pad_len(raw);
}

void tb_checker_monitor(void *context, const tb_end_t *from, const can_frame_t *frame,
                        uint64_t start_us, uint64_t end_us)
{
    tb_checker_t *checker = context;
    const uint32_t d = (frame->id == checker->a_tx) ? 0U : 1U;
    const uint32_t r = 1U - d;
    const uint8_t dir_tx_dl = (d == 0U) ? checker->a_tx_dl : checker->b_tx_dl;
    tb_direction_t *dir = &checker->dir[d];
    tb_direction_t *rev = &checker->dir[r];
    const uint8_t type = frame->data[0] >> 4U;
    const uint8_t low = frame->data[0] & 0x0FU;
    const uint8_t expected_flags =
        (uint8_t)((checker->can_fd ? CAN_FLAG_FD : 0U) |
                  ((checker->can_fd && checker->can_fd_brs) ? CAN_FLAG_BRS : 0U));

    (void)from;
    checker->frames++;
    if ((frame->id != checker->a_tx) && (frame->id != checker->b_tx)) {
        violation(checker, "the identifier is one of the connection's", frame);
    }
    if (!checker->can_fd) {
        if ((frame->dlc != 8U) || (frame->flags != 0U)) {
            violation(checker, "every frame is a data frame of 8 bytes", frame);
        }
    } else if (!can_frame_is_valid(frame) || (frame->flags != expected_flags)) {
        violation(checker, "every frame carries valid CAN FD flags and discrete length", frame);
    }

    switch (type) {
    case 0x0U: { /* SF */
        uint32_t sf_dl;
        uint32_t off;

        if (frame->dlc <= 8U) {
            if (frame->dlc != 8U) {
                violation(checker, "classical SF is padded to 8 bytes", frame);
            }
            if ((low == 0U) || (low > 7U)) {
                violation(checker, "SF_DL is 1..7 when CAN_DL <= 8", frame);
            }
            sf_dl = low;
            off = 1U;
        } else {
            sf_dl = frame->data[1];
            off = 2U;
            if ((low != 0U) || (sf_dl < 8U) || (sf_dl > (uint32_t)(dir_tx_dl - 2U)) ||
                (wire_pad_len((uint8_t)(sf_dl + 2U)) != frame->dlc)) {
                violation(checker, "CAN FD SF must satisfy Table 13", frame);
            }
        }
        if (dir->receiving) {
            violation(checker, "no SF while a segmented message is under way", frame);
        }
        check_padding(checker, frame, off + sf_dl, frame->dlc);
        dir->length = sf_dl;
        (void)memcpy(dir->message, &frame->data[off], sf_dl);
        complete(checker, d, frame);
        break;
    }
    case 0x1U: { /* FF */
        uint32_t ff_dl = ((uint32_t)low << 8U) | frame->data[1];
        uint32_t pci_len = 2U;
        const uint32_t ff_dl_min = (frame->dlc == 8U) ? 8U : ((uint32_t)frame->dlc - 1U);

        if (frame->dlc != dir_tx_dl) {
            violation(checker, "FF uses the sender's full TX_DL", frame);
        }
        if (ff_dl == 0U) {
            ff_dl = ((uint32_t)frame->data[2] << 24U) | ((uint32_t)frame->data[3] << 16U) |
                    ((uint32_t)frame->data[4] << 8U) | (uint32_t)frame->data[5];
            pci_len = 6U;
            if (ff_dl <= 4095U) {
                violation(checker, "escape FF_DL must exceed 4095", frame);
            }
        } else if ((ff_dl < ff_dl_min) || (ff_dl > 4095U)) {
            violation(checker, "FF_DL satisfies Table 14 (FF_DLmin..4095)", frame);
        }
        if (dir->receiving) {
            violation(checker, "no FF while a segmented message is under way", frame);
        }
        dir->receiving = true;
        dir->cts_seen = false;
        dir->rx_dl = frame->dlc;
        dir->length = ff_dl;
        dir->got = (uint32_t)frame->dlc - pci_len;
        dir->next_sn = 1U;
        dir->in_block = 0U;
        dir->cf_before = false;
        if (dir->got <= sizeof(dir->message)) {
            (void)memcpy(dir->message, &frame->data[pci_len], dir->got);
        }
        break;
    }
    case 0x2U: { /* CF */
        if (!dir->receiving || !dir->cts_seen) {
            violation(checker, "a CF only after an FC CTS allowed it", frame);
        }
        if (low != dir->next_sn) {
            violation(checker, "SN counts 1, 2, .., 15, 0, ..", frame);
        }
        if ((dir->bs != 0U) && (dir->in_block >= dir->bs)) {
            violation(checker, "at most BS CFs per block", frame);
        }
        if (dir->cf_before && ((start_us - dir->last_cf_end) < dir->stmin_us)) {
            violation(checker, "STmin between two CFs", frame);
        }
        const uint32_t cf_max = (uint32_t)dir->rx_dl - 1U;
        const uint32_t rem = dir->length - dir->got;
        const bool is_last = rem <= cf_max;
        const uint32_t n = is_last ? rem : cf_max;
        const uint8_t expected_dlc = is_last ? wire_pad_len((uint8_t)(n + 1U)) : dir->rx_dl;

        if (frame->dlc != expected_dlc) {
            violation(checker, "CF length matches RX_DL or minimal padded last CF", frame);
        }
        check_padding(checker, frame, 1U + n, frame->dlc);
        if ((dir->got + n) <= sizeof(dir->message)) {
            (void)memcpy(&dir->message[dir->got], &frame->data[1], n);
        }
        dir->got += n;
        dir->next_sn = (uint8_t)((dir->next_sn + 1U) & 0x0FU);
        dir->in_block++;
        dir->cf_before = true;
        dir->last_cf_end = end_us;
        if (dir->got == dir->length) {
            complete(checker, d, frame);
        } else if ((dir->bs != 0U) && (dir->in_block == dir->bs)) {
            dir->cts_seen = false;
        }
        break;
    }
    case 0x3U: /* FC, about the message the sender of the FC receives */
        if (frame->dlc != 8U) {
            violation(checker, "FC is always 8 bytes", frame);
        }
        check_padding(checker, frame, 3U, frame->dlc);
        if (!rev->receiving || rev->cts_seen) {
            violation(checker, "an FC only where a sender waits for one", frame);
        }
        if (low == 0U) {
            rev->cts_seen = true;
            rev->bs = frame->data[1];
            rev->stmin_us = checker_stmin_us(frame->data[2]);
            rev->in_block = 0U;
        } else if ((low == 1U) || (low == 2U)) {
            if ((frame->data[1] != 0U) || (frame->data[2] != 0U)) {
                violation(checker, "WAIT and OVFLW carry BS 0 and STmin 0", frame);
            }
            if (low == 2U) {
                rev->receiving = false; /* the sender gives the message up */
            }
        } else {
            violation(checker, "FS is 0, 1 or 2", frame);
        }
        break;
    default:
        violation(checker, "the N_PCI type is SF, FF, CF or FC", frame);
        break;
    }
}
