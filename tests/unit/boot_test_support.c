/* SPDX-License-Identifier: MIT */
#include "boot_test_support.h"

#include <stdio.h>
#include <string.h>

#include "boot_test_flash.h"
#include "crc/crc.h"
#include "unity.h"

uint8_t bt_mem[BT_MEM_MAX];
boot_config_t bt_config;
boot_t bt_boot;
boot_writer_t bt_writer;
bt_checker_control_t bt_checker;

const bt_shape_t bt_shape_host_small = {.sector_size = 4096U,
                                        .page_size = 256U,
                                        .unit_size = 8U,
                                        .meta_sectors = 2U,
                                        .slot_sectors = 3U,
                                        .spare_sectors = 0U};

static boot_test_flash_t s_flash;
static flash_dev_t s_traced;
static uint32_t s_device_size;

/* ------------------------------------------------------------------ the tracer */

typedef struct {
    bt_counts_t counts;
    bool cut_armed;
    bool cut_tear;
    uint32_t cut_op;
    uint32_t cut_position;
    uint32_t cut_seed;
    bool fired;
    uint32_t fail_left;
    flash_status_t fail_status;
    flash_status_t read_status;
    bt_op_t *log;
    uint32_t log_cap;
    uint32_t log_len;
} tracer_t;

static tracer_t s_tracer;

static flash_status_t traced_read(void *ctx, uint32_t addr, uint8_t *dst, uint32_t len)
{
    flash_status_t status = FLASH_E_POWER_LOST;

    (void)ctx;
    if (s_tracer.fired) {
        /* status stays FLASH_E_POWER_LOST */
    } else if (s_tracer.read_status != FLASH_OK) {
        status = s_tracer.read_status;
    } else {
        s_tracer.counts.reads++;
        status = s_flash.dev.read(s_flash.dev.ctx, addr, dst, len);
    }

    return status;
}

/* Before a program or erase: the log, the cut, a planted failure. True if the operation goes
 * on to the device; otherwise *status is its result. */
static bool before_write(bool erase, uint32_t addr, uint32_t len, flash_status_t *status)
{
    const uint32_t op = s_tracer.counts.ops;
    bool go = true;

    s_tracer.counts.ops++;
    if (s_tracer.fired) {
        *status = FLASH_E_POWER_LOST;
        go = false;
    } else {
        if ((s_tracer.log != NULL) && (s_tracer.log_len < s_tracer.log_cap)) {
            s_tracer.log[s_tracer.log_len].erase = erase;
            s_tracer.log[s_tracer.log_len].addr = addr;
            s_tracer.log[s_tracer.log_len].len = len;
            s_tracer.log_len++;
        }
        if (s_tracer.cut_armed && (op == s_tracer.cut_op)) {
            s_tracer.cut_armed = false;
            s_tracer.fired = true;
            if (s_tracer.cut_tear) {
                s_flash.arm_next(s_flash.model, s_tracer.cut_position, s_tracer.cut_seed);
            } else {
                *status = FLASH_E_POWER_LOST;
                go = false;
            }
        } else if (s_tracer.fail_left > 0U) {
            s_tracer.fail_left--;
            *status = s_tracer.fail_status;
            go = false;
        } else {
            /* an ordinary operation */
        }
    }

    return go;
}

static flash_status_t traced_program(void *ctx, uint32_t addr, const uint8_t *src, uint32_t len)
{
    flash_status_t status = FLASH_OK;

    (void)ctx;
    if (before_write(false, addr, len, &status)) {
        s_tracer.counts.programs++;
        status = s_flash.dev.program(s_flash.dev.ctx, addr, src, len);
        if (status == FLASH_OK) {
            s_tracer.counts.units += len / s_traced.geometry.unit_size;
        }
    }

    return status;
}

static flash_status_t traced_erase(void *ctx, uint32_t addr)
{
    flash_status_t status = FLASH_OK;

    (void)ctx;
    if (before_write(true, addr, s_traced.geometry.sector_size, &status)) {
        s_tracer.counts.erases++;
        status = s_flash.dev.erase(s_flash.dev.ctx, addr);
    }

    return status;
}

void bt_counts(bt_counts_t *counts)
{
    *counts = s_tracer.counts;
}

void bt_counts_reset(void)
{
    (void)memset(&s_tracer.counts, 0, sizeof(s_tracer.counts));
}

void bt_trace(bt_op_t *log, uint32_t cap)
{
    s_tracer.log = log;
    s_tracer.log_cap = cap;
    s_tracer.log_len = 0U;
}

uint32_t bt_trace_len(void)
{
    return s_tracer.log_len;
}

void bt_cut_before(uint32_t op)
{
    s_tracer.cut_armed = true;
    s_tracer.cut_tear = false;
    s_tracer.cut_op = op;
    s_tracer.fired = false;
}

void bt_cut_tear(uint32_t op, uint32_t position, uint32_t seed)
{
    s_tracer.cut_armed = true;
    s_tracer.cut_tear = true;
    s_tracer.cut_op = op;
    s_tracer.cut_position = position;
    s_tracer.cut_seed = seed;
    s_tracer.fired = false;
}

bool bt_cut_fired(void)
{
    return s_tracer.fired;
}

void bt_power_on(void)
{
    s_flash.power_on(s_flash.model);
    s_tracer.cut_armed = false;
    s_tracer.fired = false;
    s_tracer.fail_left = 0U;
    s_tracer.read_status = FLASH_OK;
}

void bt_power_off(void)
{
    s_tracer.cut_armed = false;
    s_tracer.fired = true;
}

void bt_fail_next(flash_status_t status)
{
    s_tracer.fail_left++;
    s_tracer.fail_status = status;
}

void bt_fail_reads(flash_status_t status)
{
    s_tracer.read_status = status;
}

/* ------------------------------------------------------------------ test images */

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8U);
    p[2] = (uint8_t)(value >> 16U);
    p[3] = (uint8_t)(value >> 24U);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

/* A header that may start an image of at most `len` bytes. */
static bool header_ok(const uint8_t *h, uint32_t len)
{
    return (len >= BT_IMAGE_HEADER) && (get32(&h[0]) == BT_IMAGE_MAGIC) &&
           (get32(&h[28]) == crc32_compute(h, 28U)) && (get32(&h[8]) <= (len - BT_IMAGE_HEADER));
}

uint32_t bt_image_build(uint8_t *out, uint32_t cap, uint32_t version, uint32_t seed,
                        uint32_t payload)
{
    const uint32_t total = BT_IMAGE_HEADER + payload;
    uint32_t random = (seed * 2246822519U) ^ 0x5EED1234U;

    TEST_ASSERT_TRUE_MESSAGE(total <= cap, "test image larger than its buffer");
    for (uint32_t i = 0U; i < payload; i++) {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        out[BT_IMAGE_HEADER + i] = (uint8_t)(random >> 24U);
    }
    put32(&out[0], BT_IMAGE_MAGIC);
    put32(&out[4], version);
    put32(&out[8], payload);
    put32(&out[12], crc32_compute(&out[BT_IMAGE_HEADER], payload));
    put32(&out[16], seed);
    (void)memset(&out[20], 0x5A, 8U);
    put32(&out[28], crc32_compute(out, 28U));

    return total;
}

bool bt_image_check_mem(uint32_t addr, uint32_t len, uint8_t *id, uint32_t *version)
{
    const uint8_t *h = &bt_mem[addr];
    bool valid = header_ok(h, len);

    if (valid) {
        valid = (crc32_compute(&bt_mem[addr + BT_IMAGE_HEADER], get32(&h[8])) == get32(&h[12]));
    }
    if (valid && (id != NULL)) {
        (void)memcpy(id, h, BOOT_IMAGE_ID_SIZE);
    }
    if (valid && (version != NULL)) {
        *version = get32(&h[4]);
    }

    return valid;
}

/* The validator: a header step, then the payload in chunks. */
typedef struct {
    const flash_dev_t *dev;
    uint32_t addr;
    uint32_t len;
    uint32_t phase; /* 0 header, 1 payload, 2 done */
    uint32_t offset;
    uint32_t payload;
    uint32_t crc;
    uint8_t header[BT_IMAGE_HEADER];
} checker_t;

static checker_t s_check;

/* A value outside boot_check_t, read through a volatile so that no analyser takes it for a
 * constant. */
static volatile int s_bogus_check = 77;

static void checker_start(void *ctx, const flash_dev_t *dev, uint32_t addr, uint32_t len)
{
    checker_t *c = ctx;

    bt_checker.starts++;
    c->dev = dev;
    c->addr = addr;
    c->len = len;
    c->phase = 0U;
    c->offset = 0U;
    c->payload = 0U;
    c->crc = CRC32_INIT;
}

static boot_check_t checker_step(void *ctx, boot_image_info_t *info)
{
    checker_t *c = ctx;
    uint8_t buf[256];
    boot_check_t result = BOOT_CHECK_BUSY;

    bt_checker.steps++;
    if (bt_checker.fail_reads ||
        ((bt_checker.fail_at_start != 0U) && (bt_checker.starts >= bt_checker.fail_at_start))) {
        result = BOOT_CHECK_ERROR;
    } else if (bt_checker.bogus) {
        result = (boot_check_t)s_bogus_check;
    } else if (bt_checker.stall) {
        result = BOOT_CHECK_BUSY;
    } else if (c->phase == 0U) {
        /* header_ok() refuses a length below the header's */
        if (c->dev->read(c->dev->ctx, c->addr, c->header, BT_IMAGE_HEADER) != FLASH_OK) {
            result = BOOT_CHECK_ERROR;
        } else if (!header_ok(c->header, c->len)) {
            result = BOOT_CHECK_INVALID;
        } else {
            c->payload = get32(&c->header[8]);
            c->phase = 1U;
        }
    } else if (c->phase == 1U) {
        const uint32_t left = c->payload - c->offset;
        const uint32_t n = (left < bt_checker.chunk) ? left : bt_checker.chunk;

        if ((n > 0U) && (c->dev->read(c->dev->ctx, c->addr + BT_IMAGE_HEADER + c->offset, buf, n) !=
                         FLASH_OK)) {
            result = BOOT_CHECK_ERROR;
        } else {
            c->crc = crc32_update(c->crc, buf, n);
            c->offset += n;
            if (c->offset == c->payload) {
                c->phase = 2U;
                if ((c->crc ^ CRC32_XOROUT) == get32(&c->header[12])) {
                    (void)memcpy(info->id, c->header, BOOT_IMAGE_ID_SIZE);
                    info->version = get32(&c->header[4]);
                    result = BOOT_CHECK_VALID;
                } else {
                    result = BOOT_CHECK_INVALID;
                }
            }
        }
    } else {
        result = BOOT_CHECK_INVALID; /* step after the end */
    }

    return result;
}

/* ------------------------------------------------------------------ the bench */

uint32_t bt_device_size(void)
{
    return s_device_size;
}

void bt_setup(const bt_shape_t *shape)
{
    const uint32_t sectors =
        shape->meta_sectors + (2U * shape->slot_sectors) + shape->spare_sectors;
    const uint32_t size = sectors * shape->sector_size;
    flash_geometry_t geometry;

    TEST_ASSERT_TRUE_MESSAGE(size <= BT_MEM_MAX, "bench layout larger than bt_mem");
    (void)memset(bt_mem, 0xFF, size);
    s_device_size = size;
    geometry.size = size;
    geometry.sector_size = shape->sector_size;
    geometry.page_size = shape->page_size;
    geometry.unit_size = shape->unit_size;
    boot_test_flash_init(&s_flash, bt_mem, &geometry);

    (void)memset(&s_tracer, 0, sizeof(s_tracer));
    s_traced.geometry = geometry;
    s_traced.ctx = NULL;
    s_traced.read = traced_read;
    s_traced.program = traced_program;
    s_traced.erase = traced_erase;

    (void)memset(&bt_checker, 0, sizeof(bt_checker));
    bt_checker.chunk = 64U;
    (void)memset(&s_check, 0, sizeof(s_check));

    (void)memset(&bt_config, 0, sizeof(bt_config));
    bt_config.dev = &s_traced;
    bt_config.meta_addr = 0U;
    bt_config.meta_sectors = shape->meta_sectors;
    bt_config.slot[0].addr = shape->meta_sectors * shape->sector_size;
    bt_config.slot[0].size = shape->slot_sectors * shape->sector_size;
    bt_config.slot[1].addr = bt_config.slot[0].addr + bt_config.slot[0].size;
    bt_config.slot[1].size = bt_config.slot[0].size;
    bt_config.validator.ctx = &s_check;
    bt_config.validator.start = checker_start;
    bt_config.validator.step = checker_step;

    (void)memset(&bt_writer, 0, sizeof(bt_writer));
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_init(&bt_boot, &bt_config));
}

void bt_reboot(void)
{
    (void)memset(&bt_boot, 0xA5, sizeof(bt_boot));
    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_init(&bt_boot, &bt_config));
}

/* ------------------------------------------------------------------ helpers */

/* Chunk sizes of bt_update(): around units and pages, one larger than a sector. */
static const uint32_t s_chunks[] = {256U, 1U, 7U, 300U, 33U, 512U, 1000U, 8U, 255U, 4097U};

boot_status_t bt_update(boot_t *b, uint8_t slot, const uint8_t *image, uint32_t len,
                        uint32_t chunking)
{
    boot_status_t status = boot_writer_begin(&bt_writer, b, slot, len);
    uint32_t offset = 0U;
    uint32_t k = chunking;

    while (status == BOOT_OK) {
        status = boot_writer_erase_step(&bt_writer);
        if (status == BOOT_OK) {
            break;
        }
        if (status == BOOT_BUSY) {
            status = BOOT_OK;
        }
    }
    while ((status == BOOT_OK) && (offset < len)) {
        uint32_t n = s_chunks[k % (uint32_t)(sizeof(s_chunks) / sizeof(s_chunks[0]))];

        k++;
        if (n > (len - offset)) {
            n = len - offset;
        }
        status = boot_writer_write(&bt_writer, offset, &image[offset], n);
        offset += n;
    }
    while (status == BOOT_OK) {
        status = boot_writer_finish_step(&bt_writer);
        if (status == BOOT_OK) {
            break;
        }
        if (status == BOOT_BUSY) {
            status = BOOT_OK;
        }
    }

    return status;
}

void bt_raw_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    TEST_ASSERT_TRUE(len <= (s_device_size - addr));
    (void)memcpy(&bt_mem[addr], data, len);
}

void bt_flip_bit(uint32_t addr, uint32_t bit)
{
    bt_mem[addr] = (uint8_t)(bt_mem[addr] ^ (uint8_t)(1U << bit));
}

uint32_t bt_slot_addr(uint8_t slot)
{
    return bt_config.slot[slot].addr;
}

uint32_t bt_slot_size(uint8_t slot)
{
    return bt_config.slot[slot].size;
}

uint32_t bt_entry_addr(uint32_t sector, uint32_t index)
{
    boot_description_t d;

    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_describe(&bt_boot, &d));
    return bt_config.meta_addr + (sector * bt_config.dev->geometry.sector_size) +
           (index * d.entry_stride);
}

/* ------------------------------------------------------------------ the reference model */

void bt_truth(bt_truth_t *truth)
{
    for (uint8_t s = 0U; s < BOOT_SLOTS; s++) {
        (void)memset(truth->id[s], 0xFF, BOOT_IMAGE_ID_SIZE);
        truth->valid[s] = bt_image_check_mem(bt_slot_addr(s), bt_slot_size(s), truth->id[s], NULL);
    }
}

static void ref_clear(bt_ref_t *r, uint8_t slot, boot_slot_state_t state)
{
    r->state[slot] = state;
    (void)memset(r->id[slot], 0xFF, BOOT_IMAGE_ID_SIZE);
}

void bt_ref_factory(bt_ref_t *r)
{
    r->have = false;
    ref_clear(r, 0U, BOOT_SLOT_EMPTY);
    ref_clear(r, 1U, BOOT_SLOT_EMPTY);
    r->attempts = 0U;
}

uint8_t bt_ref_confirmed(const bt_ref_t *r)
{
    return (r->state[1] == BOOT_SLOT_CONFIRMED) ? 1U : 0U;
}

/* The slot holds the image the model names for it. */
static bool ref_matches(const bt_ref_t *r, const bt_truth_t *truth, uint8_t slot)
{
    return truth->valid[slot] && (memcmp(truth->id[slot], r->id[slot], BOOT_IMAGE_ID_SIZE) == 0);
}

static void ref_choose(bt_ref_choice_t *choice, uint8_t slot, uint8_t attempt)
{
    choice->status = BOOT_OK;
    choice->slot = slot;
    choice->trial = (attempt != 0U);
    choice->attempt = attempt;
}

/* The rows of the state machine in boot.h, in its order. */
void bt_ref_select(bt_ref_t *r, const bt_truth_t *truth, bt_ref_choice_t *choice)
{
    choice->status = BOOT_E_NOTHING_BOOTABLE;
    choice->slot = BOOT_NO_SLOT;
    choice->trial = false;
    choice->attempt = 0U;
    choice->event = BOOT_EVENT_NONE;
    choice->event_slot = BOOT_NO_SLOT;

    if (!r->have) {
        for (uint8_t s = 0U; s < BOOT_SLOTS; s++) {
            if ((choice->status != BOOT_OK) && truth->valid[s]) {
                r->have = true;
                r->state[s] = BOOT_SLOT_CONFIRMED;
                (void)memcpy(r->id[s], truth->id[s], BOOT_IMAGE_ID_SIZE);
                ref_clear(r, (uint8_t)(1U - s), BOOT_SLOT_EMPTY);
                choice->event = BOOT_EVENT_ADOPT;
                choice->event_slot = s;
                ref_choose(choice, s, 0U);
            }
        }
        return;
    }

    const uint8_t c = bt_ref_confirmed(r);
    const uint8_t o = (uint8_t)(1U - c);

    if (r->state[o] == BOOT_SLOT_TRIAL) {
        if (r->attempts >= BOOT_MAX_ATTEMPTS) {
            r->state[o] = BOOT_SLOT_REJECTED;
            r->attempts = 0U;
            choice->event = BOOT_EVENT_ROLLBACK;
            choice->event_slot = o;
        } else if (ref_matches(r, truth, o)) {
            r->attempts++;
            ref_choose(choice, o, r->attempts);
            return;
        } else {
            r->state[o] = BOOT_SLOT_REJECTED;
            r->attempts = 0U;
            choice->event = BOOT_EVENT_REJECT;
            choice->event_slot = o;
        }
    }

    if (ref_matches(r, truth, c)) {
        ref_choose(choice, c, 0U);
    } else if ((r->state[o] == BOOT_SLOT_PREVIOUS) && ref_matches(r, truth, o)) {
        r->state[o] = BOOT_SLOT_CONFIRMED;
        r->state[c] = BOOT_SLOT_REJECTED;
        choice->event = BOOT_EVENT_FALLBACK;
        choice->event_slot = o;
        ref_choose(choice, o, 0U);
    } else {
        /* nothing bootable */
    }
}

boot_status_t bt_ref_begin(bt_ref_t *r, uint8_t slot, uint32_t size, uint32_t slot_size)
{
    boot_status_t status = BOOT_OK;

    if (!r->have || (r->state[0] == BOOT_SLOT_TRIAL) || (r->state[1] == BOOT_SLOT_TRIAL) ||
        (r->state[slot] == BOOT_SLOT_CONFIRMED)) {
        status = BOOT_E_STATE;
    } else if ((size == 0U) || (size > slot_size)) {
        status = BOOT_E_SIZE;
    } else {
        ref_clear(r, slot, BOOT_SLOT_WRITING);
    }

    return status;
}

void bt_ref_commit(bt_ref_t *r, uint8_t slot, const uint8_t *id)
{
    r->state[slot] = BOOT_SLOT_TRIAL;
    (void)memcpy(r->id[slot], id, BOOT_IMAGE_ID_SIZE);
    r->attempts = 0U;
}

boot_status_t bt_ref_confirm(bt_ref_t *r, uint8_t slot)
{
    boot_status_t status = BOOT_E_STATE;

    if (r->have && (r->state[slot] == BOOT_SLOT_TRIAL) && (r->attempts > 0U)) {
        r->state[1U - slot] = BOOT_SLOT_PREVIOUS;
        r->state[slot] = BOOT_SLOT_CONFIRMED;
        r->attempts = 0U;
        status = BOOT_OK;
    }

    return status;
}

void bt_ref_of(const boot_description_t *d, bt_ref_t *r)
{
    r->have = d->have_record;
    for (uint8_t s = 0U; s < BOOT_SLOTS; s++) {
        r->state[s] = d->slot[s].state;
        (void)memcpy(r->id[s], d->slot[s].id, BOOT_IMAGE_ID_SIZE);
    }
    r->attempts = (d->trial_slot != BOOT_NO_SLOT) ? d->attempts_used : 0U;
}

void bt_ref_now(bt_ref_t *r)
{
    boot_description_t d;

    TEST_ASSERT_EQUAL_INT(BOOT_OK, boot_describe(&bt_boot, &d));
    bt_ref_of(&d, r);
}

bool bt_ref_equal(const bt_ref_t *a, const bt_ref_t *b)
{
    bool equal = (a->have == b->have) && (a->attempts == b->attempts);

    for (uint8_t s = 0U; s < BOOT_SLOTS; s++) {
        equal = equal && (a->state[s] == b->state[s]) &&
                (memcmp(a->id[s], b->id[s], BOOT_IMAGE_ID_SIZE) == 0);
    }

    return equal;
}

const char *bt_ref_text(const bt_ref_t *r, char *buf, size_t len)
{
    if (!r->have) {
        (void)snprintf(buf, len, "factory");
    } else {
        /* The id of a test image is its header; bytes 16..17 are the low bytes of its seed. */
        (void)snprintf(buf, len, "A:%s(%02x%02x) B:%s(%02x%02x) k=%u",
                       boot_slot_state_name(r->state[0]), r->id[0][16], r->id[0][17],
                       boot_slot_state_name(r->state[1]), r->id[1][16], r->id[1][17],
                       (unsigned)r->attempts);
    }

    return buf;
}
