/* SPDX-License-Identifier: MIT */
#include "boot/boot.h"

#include <stddef.h>
#include <string.h>

#include "boot/boot_internal.h"
#include "crc/crc.h"

/*
 * What the module does is described in boot.h; this file adds how. The slot writer is in
 * boot_writer.c and uses the functions of boot_internal.h.
 *
 * What holds for a boot_t between any two calls, from a successful boot_init() on:
 *
 *   - `ready` is READY_MARK and the configuration is one that boot_init() accepted, so every
 *     address computed below (entry_addr(), attempt_addr(), the slot regions) lies inside the
 *     device and every buffer length (body_size <= BODY_MAX, field_size <= FIELD_MAX,
 *     entry_size <= ENTRY_MAX) fits the stack buffers that hold them;
 *   - if have_record is set, `record` passed decode_record(), so exactly one of its slots is
 *     CONFIRMED and every state and event is a value of its enumeration.
 *
 * The scan fields describe the flash as it was at the last scan (records_valid only after the
 * scan of boot_describe(), which counts every valid record; other scans leave it 0). Every public
 * operation scans first, and boot_write_record() scans again after it wrote, so a decision is
 * never made on what an earlier call saw. Only the attempt write of select does not rescan:
 * nothing after it in the same call reads the scan fields.
 */

/* Set in boot_t.ready by boot_init(). */
#define READY_MARK 0x424F4F54U

/* The record (boot.h, THE RECORD): offsets of its fields. */
#define REC_MAGIC      0U
#define REC_VERSION    4U
#define REC_EVENT      5U
#define REC_EVENT_SLOT 6U
#define REC_RESERVED   7U
#define REC_SEQ        8U
#define REC_SLOTS      12U
#define REC_SLOT_SIZE  40U
#define REC_CRC        92U

/* Offsets within one slot of the record. */
#define SLOT_STATE    0U
#define SLOT_RESERVED 1U /* three bytes */
#define SLOT_VERSION  4U
#define SLOT_ID       8U

/* The range of sequence numbers. 0 and 0xFFFFFFFF are never written: they are what an
 * all-zero and an erased word read. */
#define SEQ_FIRST 1U
#define SEQ_LAST  0xFFFFFFFEU

/* An erased byte, and the version of a slot without an image. */
#define ERASED     0xFFU
#define NO_VERSION 0xFFFFFFFFU

/* "None" in a position. */
#define NONE 0xFFFFFFFFU

/* The largest record body (BOOT_RECORD_SIZE rounded up to BOOT_UNIT_MAX) and field. */
#define BODY_MAX  128U
#define FIELD_MAX BOOT_UNIT_MAX

/* The largest entry: record body, commit field and attempt fields, for the largest unit. */
#define ENTRY_MAX (BODY_MAX + ((1U + BOOT_MAX_ATTEMPTS) * FIELD_MAX))

_Static_assert(BOOT_SLOTS == 2U, "the other slot of slot s is 1 - s");
_Static_assert((REC_SLOTS + (BOOT_SLOTS * REC_SLOT_SIZE)) == REC_CRC, "slots end at the CRC");
_Static_assert((REC_CRC + 4U) == BOOT_RECORD_SIZE, "the CRC is the record's last word");
_Static_assert((SLOT_ID + BOOT_IMAGE_ID_SIZE) == REC_SLOT_SIZE, "the id ends the slot");
_Static_assert((BOOT_RECORD_SIZE % 32U) == 0U,
               "for units up to 32 bytes the record needs no padding and the fields after it "
               "stay aligned to their size");
_Static_assert((BODY_MAX % BOOT_UNIT_MAX) == 0U, "BODY_MAX holds a record padded to any unit");
_Static_assert(BODY_MAX >= BOOT_RECORD_SIZE, "BODY_MAX holds a record");
_Static_assert(BOOT_FIELD_MIN >= 8U, "the commit field holds two 32-bit words");
_Static_assert((BOOT_UNIT_MAX & (BOOT_UNIT_MAX - 1U)) == 0U, "BOOT_UNIT_MAX is a power of two");
_Static_assert(BOOT_MAX_ATTEMPTS < 0xFFU, "attempt counts are uint8_t");
_Static_assert(BOOT_META_SECTORS_MIN >= 2U,
               "a record goes to another sector than the newest one when its sector is full");
_Static_assert((BOOT_MAX_ATTEMPTS * FIELD_MAX) <= ENTRY_MAX, "the attempt fields fit the buffer");

/* ------------------------------------------------------------------ small helpers */

static void put_u32(uint8_t *buf, uint32_t at, uint32_t value)
{
    buf[at] = (uint8_t)(value & 0xFFU);
    buf[at + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
    buf[at + 2U] = (uint8_t)((value >> 16U) & 0xFFU);
    buf[at + 3U] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint32_t get_u32(const uint8_t *buf, uint32_t at)
{
    return (uint32_t)buf[at] | ((uint32_t)buf[at + 1U] << 8U) | ((uint32_t)buf[at + 2U] << 16U) |
           ((uint32_t)buf[at + 3U] << 24U);
}

/* Every one of `len` (>= 1) bytes reads 0xFF: the first does, and each byte equals the one after
 * it, which one memcmp() of the buffer against itself shifted by one byte tells. The scan
 * calls this for every entry of the log, most of which are erased. */
static bool all_erased(const uint8_t *buf, uint32_t len)
{
    return (buf[0] == ERASED) && (memcmp(buf, &buf[1], (size_t)len - 1U) == 0);
}

/* Stops at the first byte that differs. */
static bool bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t len)
{
    bool equal = true;

    for (uint32_t i = 0U; equal && (i < len); i++) {
        equal = (a[i] == b[i]);
    }

    return equal;
}

/* The number of 1 bits of each value of a nibble. */
static const uint8_t s_nibble_ones[16] = {0U, 1U, 1U, 2U, 1U, 2U, 2U, 3U,
                                          1U, 2U, 2U, 3U, 2U, 3U, 3U, 4U};

/* The number of 0 bits in `len` bytes: the Berger check of the commit field. Two table
 * lookups per byte, so that a scan of the whole log stays short on a small processor. */
static uint32_t zero_bits(const uint8_t *buf, uint32_t len)
{
    uint32_t ones = 0U;

    for (uint32_t i = 0U; i < len; i++) {
        ones += (uint32_t)s_nibble_ones[buf[i] & 0x0FU] + (uint32_t)s_nibble_ones[buf[i] >> 4U];
    }

    return (len * 8U) - ones;
}

static bool is_pow2(uint32_t value)
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

static uint32_t round_up(uint32_t value, uint32_t to)
{
    return ((value + to - 1U) / to) * to;
}

static uint8_t other_slot(uint8_t slot)
{
    return (uint8_t)(1U - (uint32_t)slot);
}

/* F of boot.h: max(BOOT_FIELD_MIN, unit). */
static uint32_t field_size_for(uint32_t unit)
{
    return (unit > BOOT_FIELD_MIN) ? unit : BOOT_FIELD_MIN;
}

/* ------------------------------------------------------------------ the flash */

boot_status_t boot_from_flash(flash_status_t status)
{
    boot_status_t result = BOOT_E_FLASH;

    switch (status) {
    case FLASH_OK:
        result = BOOT_OK;
        break;
    case FLASH_E_POWER_LOST:
        result = BOOT_E_POWER_LOST;
        break;
    case FLASH_E_RANGE:
    case FLASH_E_ALIGN:
    case FLASH_E_NOT_ERASED:
    default:
        result = BOOT_E_FLASH;
        break;
    }

    return result;
}

static boot_status_t read_flash(const boot_t *b, uint32_t addr, uint8_t *dst, uint32_t len)
{
    const flash_dev_t *dev = b->config.dev;

    return boot_from_flash(dev->read(dev->ctx, addr, dst, len));
}

boot_status_t boot_program(const boot_t *b, uint32_t addr, const uint8_t *src, uint32_t len)
{
    const flash_dev_t *dev = b->config.dev;
    const uint32_t page = dev->geometry.page_size;
    boot_status_t status = BOOT_OK;
    uint32_t done = 0U;

    while ((status == BOOT_OK) && (done < len)) {
        const uint32_t at = addr + done;
        const uint32_t room = page - (at % page);
        const uint32_t n = ((len - done) < room) ? (len - done) : room;

        status = boot_from_flash(dev->program(dev->ctx, at, &src[done], n));
        done += n;
    }

    return status;
}

/* Where entry `index` of metadata sector `sector` starts. */
static uint32_t entry_addr(const boot_t *b, uint32_t sector, uint32_t index)
{
    return b->config.meta_addr + (sector * b->config.dev->geometry.sector_size) +
           (index * b->stride);
}

/* Where attempt field `k` of the entry at `entry` starts. */
static uint32_t attempt_addr(const boot_t *b, uint32_t entry, uint32_t k)
{
    return entry + b->body_size + ((1U + k) * b->field_size);
}

/* ------------------------------------------------------------------ the record */

static bool state_from_byte(uint8_t byte, boot_slot_state_t *state)
{
    bool known = true;

    switch (byte) {
    case (uint8_t)BOOT_SLOT_EMPTY:
        *state = BOOT_SLOT_EMPTY;
        break;
    case (uint8_t)BOOT_SLOT_WRITING:
        *state = BOOT_SLOT_WRITING;
        break;
    case (uint8_t)BOOT_SLOT_TRIAL:
        *state = BOOT_SLOT_TRIAL;
        break;
    case (uint8_t)BOOT_SLOT_CONFIRMED:
        *state = BOOT_SLOT_CONFIRMED;
        break;
    case (uint8_t)BOOT_SLOT_PREVIOUS:
        *state = BOOT_SLOT_PREVIOUS;
        break;
    case (uint8_t)BOOT_SLOT_REJECTED:
        *state = BOOT_SLOT_REJECTED;
        break;
    default:
        known = false;
        break;
    }

    return known;
}

/* The events a record can hold: every one but NONE. */
static bool event_from_byte(uint8_t byte, boot_event_t *event)
{
    bool known = true;

    switch (byte) {
    case (uint8_t)BOOT_EVENT_ADOPT:
        *event = BOOT_EVENT_ADOPT;
        break;
    case (uint8_t)BOOT_EVENT_BEGIN:
        *event = BOOT_EVENT_BEGIN;
        break;
    case (uint8_t)BOOT_EVENT_COMMIT:
        *event = BOOT_EVENT_COMMIT;
        break;
    case (uint8_t)BOOT_EVENT_CONFIRM:
        *event = BOOT_EVENT_CONFIRM;
        break;
    case (uint8_t)BOOT_EVENT_ROLLBACK:
        *event = BOOT_EVENT_ROLLBACK;
        break;
    case (uint8_t)BOOT_EVENT_REJECT:
        *event = BOOT_EVENT_REJECT;
        break;
    case (uint8_t)BOOT_EVENT_FALLBACK:
        *event = BOOT_EVENT_FALLBACK;
        break;
    default:
        known = false;
        break;
    }

    return known;
}

/* The record bytes of `record` with sequence number `seq`, padded with 0xFF to body_size. */
static void encode_record(const boot_t *b, const boot_record_t *record, uint32_t seq, uint8_t *body)
{
    (void)memset(body, (int)ERASED, b->body_size);
    put_u32(body, REC_MAGIC, BOOT_RECORD_MAGIC);
    body[REC_VERSION] = (uint8_t)BOOT_RECORD_VERSION;
    body[REC_EVENT] = (uint8_t)record->event;
    body[REC_EVENT_SLOT] = record->event_slot;
    put_u32(body, REC_SEQ, seq);

    for (uint32_t s = 0U; s < BOOT_SLOTS; s++) {
        const uint32_t at = REC_SLOTS + (s * REC_SLOT_SIZE);

        body[at + SLOT_STATE] = (uint8_t)record->slot[s].state;
        put_u32(body, at + SLOT_VERSION, record->slot[s].version);
        (void)memcpy(&body[at + SLOT_ID], record->slot[s].id, BOOT_IMAGE_ID_SIZE);
    }

    put_u32(body, REC_CRC, crc32_compute(body, (size_t)REC_CRC));
}

/* The commit field that belongs to the record bytes `body` (boot.h, THE COMMIT FIELD). */
static void commit_value(const boot_t *b, const uint8_t *body, uint8_t *field)
{
    const uint32_t zeros = zero_bits(body, b->body_size);

    (void)memset(field, (int)ERASED, b->field_size);
    put_u32(field, 0U, zeros);
    put_u32(field, 4U, ~zeros);
}

/* One slot of a record: a known state, reserved bytes 0xFF, and no image for EMPTY and
 * WRITING. */
static bool decode_slot(const uint8_t *body, uint32_t at, boot_slot_record_t *slot)
{
    bool valid = state_from_byte(body[at + SLOT_STATE], &slot->state) &&
                 (body[at + SLOT_RESERVED] == ERASED) &&
                 (body[at + SLOT_RESERVED + 1U] == ERASED) &&
                 (body[at + SLOT_RESERVED + 2U] == ERASED);

    if (valid) {
        slot->version = get_u32(body, at + SLOT_VERSION);
        (void)memcpy(slot->id, &body[at + SLOT_ID], BOOT_IMAGE_ID_SIZE);
        if ((slot->state == BOOT_SLOT_EMPTY) || (slot->state == BOOT_SLOT_WRITING)) {
            valid = (slot->version == NO_VERSION) && all_erased(slot->id, BOOT_IMAGE_ID_SIZE);
        }
    }

    return valid;
}

/* The record in `body`, if it is well formed and its CRC matches (the commit field is checked
 * by the caller). `record` is undefined when the result is false. */
static bool decode_record(const uint8_t *body, boot_record_t *record)
{
    uint32_t confirmed = 0U;
    uint32_t trials = 0U;
    bool valid = (get_u32(body, REC_MAGIC) == BOOT_RECORD_MAGIC) &&
                 (body[REC_VERSION] == BOOT_RECORD_VERSION) && (body[REC_RESERVED] == ERASED) &&
                 (body[REC_EVENT_SLOT] < BOOT_SLOTS) &&
                 (get_u32(body, REC_CRC) == crc32_compute(body, (size_t)REC_CRC));

    if (valid) {
        record->seq = get_u32(body, REC_SEQ);
        record->event_slot = body[REC_EVENT_SLOT];
        valid = (record->seq >= SEQ_FIRST) && (record->seq <= SEQ_LAST) &&
                event_from_byte(body[REC_EVENT], &record->event);
    }

    for (uint32_t s = 0U; valid && (s < BOOT_SLOTS); s++) {
        valid = decode_slot(body, REC_SLOTS + (s * REC_SLOT_SIZE), &record->slot[s]);
        if (valid) {
            confirmed += (record->slot[s].state == BOOT_SLOT_CONFIRMED) ? 1U : 0U;
            trials += (record->slot[s].state == BOOT_SLOT_TRIAL) ? 1U : 0U;
        }
    }

    return valid && (confirmed == 1U) && (trials <= 1U);
}

void boot_record_clear_slot(boot_record_t *record, uint8_t slot, boot_slot_state_t state)
{
    record->slot[slot].state = state;
    record->slot[slot].version = NO_VERSION;
    (void)memset(record->slot[slot].id, (int)ERASED, BOOT_IMAGE_ID_SIZE);
}

/* The CONFIRMED slot of a record that passed decode_record(). */
static uint8_t confirmed_slot(const boot_record_t *record)
{
    return (record->slot[1].state == BOOT_SLOT_CONFIRMED) ? 1U : 0U;
}

/* ------------------------------------------------------------------ the log */

/* Whether `entry` (an entry as read, record body first) holds a valid record (boot.h, WHICH
 * RECORD COUNTS): its commit field is the one its record bytes give, and the record decodes.
 * `record` is set if so. */
static bool entry_record(const boot_t *b, const uint8_t *entry, boot_record_t *record)
{
    uint8_t expect[FIELD_MAX];
    bool valid = false;

    commit_value(b, entry, expect);
    if (bytes_equal(&entry[b->body_size], expect, b->field_size)) {
        valid = decode_record(entry, record);
    }

    return valid;
}

/* The attempt fields of the newest record: how many do not read erased, and the first one that
 * does read erased. `buf` holds at least BOOT_MAX_ATTEMPTS fields. */
static boot_status_t read_attempts(boot_t *b, uint8_t *buf)
{
    const uint32_t at = attempt_addr(b, entry_addr(b, b->record_sector, b->record_index), 0U);
    const boot_status_t status = read_flash(b, at, buf, BOOT_MAX_ATTEMPTS * b->field_size);
    uint32_t offset = 0U; /* of field k in buf */

    b->attempts_used = 0U;
    b->attempt_next = (uint8_t)BOOT_MAX_ATTEMPTS;
    for (uint32_t k = 0U; (status == BOOT_OK) && (k < BOOT_MAX_ATTEMPTS); k++) {
        if (!all_erased(&buf[offset], b->field_size)) {
            b->attempts_used++;
        } else if (b->attempt_next == (uint8_t)BOOT_MAX_ATTEMPTS) {
            b->attempt_next = (uint8_t)k;
        } else {
            /* a later erased field: the first one is already noted */
        }
        offset += b->field_size;
    }

    return status;
}

/* Where the next record goes (boot.h, WRITING A RECORD), from the last used entry of each
 * metadata sector (NONE for a sector with none). */
static void choose_append(boot_t *b, const uint32_t *last_used)
{
    const uint32_t current = b->have_record ? b->record_sector : 0U;

    if (last_used[current] == NONE) {
        b->append_sector = current;
        b->append_index = 0U;
        b->append_erases = false;
    } else if ((last_used[current] + 1U) < b->entries_per_sector) {
        b->append_sector = current;
        b->append_index = last_used[current] + 1U;
        b->append_erases = false;
    } else {
        b->append_sector = (current + 1U) % b->config.meta_sectors;
        b->append_index = 0U;
        b->append_erases = true;
    }
}

/* If `entry`, entry `index` of metadata sector `sector`, holds a valid record, make it the newest
 * record unless one with a sequence number as high or higher was found before. Returns whether
 * it is valid. */
static bool consider(boot_t *b, const uint8_t *entry, uint32_t sector, uint32_t index)
{
    boot_record_t record;
    const bool valid = entry_record(b, entry, &record);

    if (valid && (!b->have_record || (record.seq > b->record.seq))) {
        b->have_record = true;
        b->record = record;
        b->record_sector = sector;
        b->record_index = index;
    }

    return valid;
}

/*
 * Read the log into b's scan fields (boot.h, WHICH RECORD COUNTS).
 *
 * The USED PART of a metadata sector is its entries from entry 0 up to, not including, the first
 * entry that reads completely erased (or to the end of the sector). Records go into a sector in
 * the order of its entries, each into the first erased entry, so in the sector of the newest
 * record nothing used lies beyond the end of the used part; any other sector holds only older
 * records and is erased before a record goes into it (WRITING A RECORD). The scan reads each used
 * part forwards, one read per entry, and stops at its end.
 *
 * The newest record of a sector is the last valid record of its used part, since records are
 * written into a sector in increasing order of their sequence numbers: the scan reads the used
 * part backwards from its last entry and decodes until it finds a valid record, usually at once.
 * The newest record of the log is, of those, the one with the highest sequence number, and of
 * two with the same number (which no sequence of operations writes) the one met first.
 * `count_all` (boot_describe()) decodes every entry of the used parts instead, to count the valid
 * records; the newest record it finds is the same.
 */
static boot_status_t scan(boot_t *b, bool count_all)
{
    uint8_t entry[ENTRY_MAX];
    uint32_t last_used[BOOT_META_SECTORS_MAX];
    boot_status_t status = BOOT_OK;

    for (uint32_t s = 0U; s < BOOT_META_SECTORS_MAX; s++) {
        last_used[s] = NONE;
    }

    b->have_record = false;
    b->record_sector = 0U;
    b->record_index = 0U;
    b->attempts_used = 0U;
    b->attempt_next = 0U;
    b->records_valid = 0U;
    b->entries_used = 0U;

    for (uint32_t s = 0U; (status == BOOT_OK) && (s < b->config.meta_sectors); s++) {
        uint32_t used = 0U;
        bool end = false;
        bool found = false;

        while ((status == BOOT_OK) && !end && (used < b->entries_per_sector)) {
            status = read_flash(b, entry_addr(b, s, used), entry, b->entry_size);
            if (status != BOOT_OK) {
                /* the read failed: the scan ends with it */
            } else if (all_erased(entry, b->entry_size)) {
                end = true;
            } else {
                if (count_all) {
                    if (consider(b, entry, s, used)) {
                        b->records_valid++;
                    }
                }
                used++;
            }
        }
        last_used[s] = (used > 0U) ? (used - 1U) : NONE;
        b->entries_used += used;

        for (uint32_t n = used; (status == BOOT_OK) && !count_all && !found && (n > 0U); n--) {
            status = read_flash(b, entry_addr(b, s, n - 1U), entry, b->entry_size);
            if (status == BOOT_OK) {
                found = consider(b, entry, s, n - 1U);
            }
        }
    }

    if ((status == BOOT_OK) && b->have_record) {
        status = read_attempts(b, entry);
    }
    if (status == BOOT_OK) {
        choose_append(b, last_used);
    } else {
        b->have_record = false;
    }

    return status;
}

boot_status_t boot_scan(boot_t *b)
{
    return scan(b, false);
}

boot_status_t boot_write_record(boot_t *b, const boot_record_t *record)
{
    uint8_t body[BODY_MAX];
    uint8_t commit[FIELD_MAX];
    const uint32_t sector = b->append_sector;
    const uint32_t index = b->append_index;
    const uint32_t addr = entry_addr(b, sector, index);
    uint32_t seq = SEQ_FIRST;
    boot_status_t status = BOOT_OK;

    if (b->have_record) {
        if (b->record.seq >= SEQ_LAST) {
            status = BOOT_E_STATE; /* 2^32 - 2 records: beyond any part's endurance */
        } else {
            seq = b->record.seq + 1U;
        }
    }

    if (status == BOOT_OK) {
        encode_record(b, record, seq, body);
        commit_value(b, body, commit);
        if (b->append_erases) {
            const flash_dev_t *dev = b->config.dev;

            status = boot_from_flash(dev->erase(dev->ctx, entry_addr(b, sector, 0U)));
        }
    }

    /* The record, then in a separate operation the commit field: until the commit field holds
     * all its bits the entry does not count. */
    if (status == BOOT_OK) {
        status = boot_program(b, addr, body, b->body_size);
    }
    if (status == BOOT_OK) {
        status = boot_program(b, addr + b->body_size, commit, b->field_size);
    }

    if (status == BOOT_OK) {
        status = boot_scan(b);
        if ((status == BOOT_OK) && ((!b->have_record) || (b->record.seq != seq) ||
                                    (b->record_sector != sector) || (b->record_index != index))) {
            status = BOOT_E_FLASH;
        }
    }

    return status;
}

/* ------------------------------------------------------------------ init */

bool boot_ready(const boot_t *b)
{
    return (b != NULL) && (b->ready == READY_MARK);
}

bool boot_has_validator(const boot_t *b)
{
    return (b->config.validator.start != NULL) && (b->config.validator.step != NULL);
}

static bool geometry_usable(const flash_geometry_t *g)
{
    return is_pow2(g->sector_size) && is_pow2(g->page_size) && is_pow2(g->unit_size) &&
           (g->unit_size <= g->page_size) && (g->page_size <= g->sector_size) && (g->size != 0U) &&
           ((g->size % g->sector_size) == 0U) && (g->unit_size <= BOOT_UNIT_MAX) &&
           (g->page_size <= BOOT_PAGE_MAX) && (g->page_size >= field_size_for(g->unit_size));
}

/* A region of whole sectors inside a device of `device` bytes. */
static bool region_usable(const boot_region_t *r, uint32_t sector, uint32_t device)
{
    return ((r->addr % sector) == 0U) && ((r->size % sector) == 0U) && (r->size >= sector) &&
           (r->addr < device) && (r->size <= (device - r->addr));
}

static bool regions_overlap(const boot_region_t *a, const boot_region_t *b)
{
    return (a->addr < (b->addr + b->size)) && (b->addr < (a->addr + a->size));
}

/* The entry layout of boot.h, THE METADATA LOG, for a usable geometry. */
static void derive_sizes(boot_t *b, const flash_geometry_t *g)
{
    const uint32_t entry = round_up(BOOT_RECORD_SIZE, g->unit_size) +
                           ((1U + BOOT_MAX_ATTEMPTS) * field_size_for(g->unit_size));
    uint32_t stride = g->unit_size;

    if (entry <= g->page_size) {
        while (stride < entry) {
            stride *= 2U; /* ends at the page size at the latest: a power of two >= entry */
        }
    } else {
        stride = round_up(entry, g->page_size);
    }

    b->field_size = field_size_for(g->unit_size);
    b->body_size = round_up(BOOT_RECORD_SIZE, g->unit_size);
    b->entry_size = entry;
    b->stride = stride;
    b->entries_per_sector = g->sector_size / stride;
}

static bool layout_usable(const boot_config_t *config)
{
    const flash_geometry_t *g = &config->dev->geometry;
    bool usable = geometry_usable(g);
    boot_region_t meta = {.addr = 0U, .size = 0U};

    if (usable) {
        usable = (config->meta_sectors >= BOOT_META_SECTORS_MIN) &&
                 (config->meta_sectors <= BOOT_META_SECTORS_MAX);
    }
    if (usable) {
        meta.addr = config->meta_addr;
        /* Wraps only if the device has fewer than meta_sectors sectors, which the first
         * condition below refuses before the size is used. */
        meta.size = config->meta_sectors * g->sector_size;
        usable = ((g->size / g->sector_size) >= config->meta_sectors) &&
                 region_usable(&meta, g->sector_size, g->size) &&
                 region_usable(&config->slot[0], g->sector_size, g->size) &&
                 region_usable(&config->slot[1], g->sector_size, g->size);
    }
    if (usable) {
        usable = !regions_overlap(&meta, &config->slot[0]) &&
                 !regions_overlap(&meta, &config->slot[1]) &&
                 !regions_overlap(&config->slot[0], &config->slot[1]);
    }

    return usable;
}

boot_status_t boot_init(boot_t *b, const boot_config_t *config)
{
    boot_status_t status = BOOT_OK;

    if (b == NULL) {
        status = BOOT_E_ARG;
    } else {
        b->ready = 0U;
        if ((config == NULL) || (config->dev == NULL) || (config->dev->read == NULL) ||
            (config->dev->program == NULL) || (config->dev->erase == NULL) ||
            ((config->validator.start == NULL) != (config->validator.step == NULL))) {
            status = BOOT_E_ARG;
        } else if (!layout_usable(config)) {
            status = BOOT_E_LAYOUT;
        } else {
            (void)memset(b, 0, sizeof(*b));
            b->config = *config;
            derive_sizes(b, &config->dev->geometry);
            if (b->entries_per_sector == 0U) {
                status = BOOT_E_LAYOUT;
            } else {
                b->ready = READY_MARK;
            }
        }
    }

    return status;
}

/* ------------------------------------------------------------------ select */

/* Run the validator over slot `slot` in one call. With `expect_id`, a valid image with another
 * id counts as INVALID: it is not the image the record names. A value outside the enumeration
 * counts as ERROR. */
static boot_check_t check_slot(const boot_t *b, uint8_t slot, const uint8_t *expect_id,
                               boot_image_info_t *info)
{
    const boot_validator_t *v = &b->config.validator;
    const boot_region_t *region = &b->config.slot[slot];
    boot_check_t result = BOOT_CHECK_BUSY;
    uint32_t busy = 0U;

    v->start(v->ctx, b->config.dev, region->addr, region->size);
    while (result == BOOT_CHECK_BUSY) {
        result = v->step(v->ctx, info);
        if (result == BOOT_CHECK_BUSY) {
            busy++;
            if (busy > region->size) {
                result = BOOT_CHECK_INVALID; /* no progress: see THE VALIDATOR */
            }
        }
    }

    switch (result) {
    case BOOT_CHECK_VALID:
        if ((expect_id != NULL) && !bytes_equal(info->id, expect_id, BOOT_IMAGE_ID_SIZE)) {
            result = BOOT_CHECK_INVALID;
        }
        break;
    case BOOT_CHECK_INVALID:
    case BOOT_CHECK_ERROR:
        break;
    case BOOT_CHECK_BUSY:
    default:
        result = BOOT_CHECK_ERROR;
        break;
    }

    return result;
}

static void selection_clear(boot_selection_t *sel)
{
    sel->slot = BOOT_NO_SLOT;
    sel->trial = false;
    sel->attempt = 0U;
    sel->version = 0U;
    sel->addr = 0U;
    sel->size = 0U;
    sel->event = BOOT_EVENT_NONE;
    sel->event_slot = BOOT_NO_SLOT;
    sel->write_status = BOOT_OK;
}

static void selection_boot(const boot_t *b, boot_selection_t *sel, uint8_t slot, uint8_t attempt,
                           uint32_t version)
{
    sel->slot = slot;
    sel->trial = (attempt != 0U);
    sel->attempt = attempt;
    sel->version = version;
    sel->addr = b->config.slot[slot].addr;
    sel->size = b->config.slot[slot].size;
}

/* Note the record a select wrote (or tried to). */
static void selection_event(boot_selection_t *sel, boot_event_t event, uint8_t slot,
                            boot_status_t written)
{
    sel->event = event;
    sel->event_slot = slot;
    if (sel->write_status == BOOT_OK) {
        sel->write_status = written;
    }
}

/* The factory state: adopt the first slot that validates as CONFIRMED. */
static boot_status_t select_factory(boot_t *b, boot_selection_t *sel)
{
    boot_status_t status = BOOT_E_NOTHING_BOOTABLE;
    bool decided = false;

    for (uint8_t s = 0U; (!decided) && (s < BOOT_SLOTS); s++) {
        boot_image_info_t info;
        const boot_check_t check = check_slot(b, s, NULL, &info);

        if (check == BOOT_CHECK_ERROR) {
            status = BOOT_E_FLASH;
            decided = true;
        } else if (check == BOOT_CHECK_VALID) {
            boot_record_t record;
            boot_status_t written;

            record.seq = 0U;
            record.event = BOOT_EVENT_ADOPT;
            record.event_slot = s;
            record.slot[s].state = BOOT_SLOT_CONFIRMED;
            record.slot[s].version = info.version;
            (void)memcpy(record.slot[s].id, info.id, BOOT_IMAGE_ID_SIZE);
            boot_record_clear_slot(&record, other_slot(s), BOOT_SLOT_EMPTY);

            written = boot_write_record(b, &record);
            if (written == BOOT_E_POWER_LOST) {
                status = written;
            } else {
                /* Booted even if the record could not be written: it is the only image. */
                selection_event(sel, BOOT_EVENT_ADOPT, s, written);
                selection_boot(b, sel, s, 0U, info.version);
                status = BOOT_OK;
            }
            decided = true;
        } else {
            /* INVALID: try the next slot */
        }
    }

    return status;
}

/* Program the next attempt field of the newest record. */
static boot_status_t write_attempt(const boot_t *b)
{
    uint8_t field[FIELD_MAX];
    boot_status_t status = BOOT_E_FLASH;

    if (b->attempt_next < (uint8_t)BOOT_MAX_ATTEMPTS) {
        (void)memset(field, 0, b->field_size);
        status = boot_program(
            b, attempt_addr(b, entry_addr(b, b->record_sector, b->record_index), b->attempt_next),
            field, b->field_size);
    }

    return status;
}

/* `record` has a TRIAL in slot `o`: boot it with one more attempt, or reject it. Sets *booted
 * if it is booted. */
static boot_status_t select_trial(boot_t *b, const boot_record_t *record, uint8_t o,
                                  boot_selection_t *sel, bool *booted)
{
    boot_status_t status = BOOT_OK;
    boot_event_t reject = BOOT_EVENT_NONE;

    if (b->attempts_used >= (uint8_t)BOOT_MAX_ATTEMPTS) {
        reject = BOOT_EVENT_ROLLBACK;
    } else {
        boot_image_info_t info;
        const uint8_t attempt = (uint8_t)(b->attempts_used + 1U);
        const boot_check_t check = check_slot(b, o, record->slot[o].id, &info);

        if (check == BOOT_CHECK_ERROR) {
            status = BOOT_E_FLASH;
        } else if (check == BOOT_CHECK_VALID) {
            /* The attempt is counted before the boot, never after it. */
            const boot_status_t written = write_attempt(b);

            if (written == BOOT_OK) {
                selection_boot(b, sel, o, attempt, info.version);
                *booted = true;
            } else if (written == BOOT_E_POWER_LOST) {
                status = written;
            } else {
                /* An attempt that cannot be counted is not made: the trial is rolled back. */
                sel->write_status = written;
                reject = BOOT_EVENT_ROLLBACK;
            }
        } else {
            reject = BOOT_EVENT_REJECT;
        }
    }

    if ((status == BOOT_OK) && (reject != BOOT_EVENT_NONE)) {
        boot_record_t next = *record;
        boot_status_t written;

        next.slot[o].state = BOOT_SLOT_REJECTED;
        next.event = reject;
        next.event_slot = o;
        written = boot_write_record(b, &next);
        if (written == BOOT_E_POWER_LOST) {
            status = written;
        } else {
            selection_event(sel, reject, o, written);
        }
    }

    return status;
}

/* The CONFIRMED slot `c` of `record` does not validate, and the other one is PREVIOUS. */
static boot_status_t select_fallback(boot_t *b, const boot_record_t *record, uint8_t c,
                                     boot_selection_t *sel)
{
    const uint8_t o = other_slot(c);
    boot_image_info_t info;
    boot_status_t status = BOOT_E_NOTHING_BOOTABLE;
    const boot_check_t check = check_slot(b, o, record->slot[o].id, &info);

    if (check == BOOT_CHECK_ERROR) {
        status = BOOT_E_FLASH;
    } else if (check == BOOT_CHECK_VALID) {
        boot_record_t next = *record;
        boot_status_t written;

        next.slot[o].state = BOOT_SLOT_CONFIRMED;
        next.slot[c].state = BOOT_SLOT_REJECTED;
        next.event = BOOT_EVENT_FALLBACK;
        next.event_slot = o;
        written = boot_write_record(b, &next);
        if (written == BOOT_E_POWER_LOST) {
            status = written;
        } else {
            selection_event(sel, BOOT_EVENT_FALLBACK, o, written);
            selection_boot(b, sel, o, 0U, info.version);
            status = BOOT_OK;
        }
    } else {
        /* neither image can be booted */
    }

    return status;
}

/* Boot the CONFIRMED slot `c` of `record`, or the PREVIOUS one if `c` does not validate. */
static boot_status_t select_confirmed(boot_t *b, const boot_record_t *record, uint8_t c,
                                      boot_selection_t *sel)
{
    boot_image_info_t info;
    boot_status_t status = BOOT_E_NOTHING_BOOTABLE;
    const boot_check_t check = check_slot(b, c, record->slot[c].id, &info);

    if (check == BOOT_CHECK_ERROR) {
        status = BOOT_E_FLASH;
    } else if (check == BOOT_CHECK_VALID) {
        selection_boot(b, sel, c, 0U, info.version);
        status = BOOT_OK;
    } else if (record->slot[other_slot(c)].state == BOOT_SLOT_PREVIOUS) {
        status = select_fallback(b, record, c, sel);
    } else {
        /* EMPTY, WRITING, REJECTED, or a TRIAL just rejected: nothing else may be booted */
    }

    return status;
}

boot_status_t boot_select(boot_t *b, boot_selection_t *sel)
{
    boot_status_t status = BOOT_E_ARG;

    if (sel != NULL) {
        selection_clear(sel);
    }

    if (boot_ready(b) && (sel != NULL) && boot_has_validator(b)) {
        status = boot_scan(b);
        if ((status == BOOT_OK) && !b->have_record) {
            status = select_factory(b, sel);
        } else if (status == BOOT_OK) {
            /* A copy: the record writes below replace b->record. */
            const boot_record_t record = b->record;
            const uint8_t c = confirmed_slot(&record);
            bool booted = false;

            if (record.slot[other_slot(c)].state == BOOT_SLOT_TRIAL) {
                status = select_trial(b, &record, other_slot(c), sel, &booted);
            }
            if ((status == BOOT_OK) && !booted) {
                status = select_confirmed(b, &record, c, sel);
            }
        } else {
            /* the metadata could not be read */
        }
    }

    return status;
}

/* ------------------------------------------------------------------ confirm */

boot_status_t boot_confirm(boot_t *b, uint8_t slot)
{
    boot_status_t status = BOOT_E_ARG;

    if (boot_ready(b) && (slot < BOOT_SLOTS)) {
        status = boot_scan(b);
        if (status == BOOT_OK) {
            if (!b->have_record || (b->record.slot[slot].state != BOOT_SLOT_TRIAL) ||
                (b->attempts_used == 0U)) {
                status = BOOT_E_STATE;
            } else {
                boot_record_t next = b->record;

                next.slot[other_slot(slot)].state = BOOT_SLOT_PREVIOUS;
                next.slot[slot].state = BOOT_SLOT_CONFIRMED;
                next.event = BOOT_EVENT_CONFIRM;
                next.event_slot = slot;
                status = boot_write_record(b, &next);
            }
        }
    }

    return status;
}

/* ------------------------------------------------------------------ describe */

boot_status_t boot_describe(boot_t *b, boot_description_t *d)
{
    boot_status_t status = BOOT_E_ARG;

    if ((d != NULL) && boot_ready(b)) {
        status = scan(b, true);

        (void)memset(d, 0, sizeof(*d));
        d->event = BOOT_EVENT_NONE;
        d->event_slot = BOOT_NO_SLOT;
        d->active_slot = BOOT_NO_SLOT;
        d->trial_slot = BOOT_NO_SLOT;
        d->entries_per_sector = b->entries_per_sector;
        d->entries_total = b->entries_per_sector * b->config.meta_sectors;
        d->entry_stride = b->stride;
        d->field_size = b->field_size;
        d->record_body = b->body_size;
        for (uint32_t s = 0U; s < BOOT_SLOTS; s++) {
            d->slot[s].state = BOOT_SLOT_EMPTY;
            d->slot[s].version = NO_VERSION;
            (void)memset(d->slot[s].id, (int)ERASED, BOOT_IMAGE_ID_SIZE);
            d->slot[s].addr = b->config.slot[s].addr;
            d->slot[s].size = b->config.slot[s].size;
        }

        if (status == BOOT_OK) {
            d->records_valid = b->records_valid;
            d->entries_used = b->entries_used;
            d->next_write_erases = b->append_erases;
            d->next_sector = b->append_sector;
            d->next_index = b->append_index;
        }

        if ((status == BOOT_OK) && b->have_record) {
            d->have_record = true;
            d->seq = b->record.seq;
            d->event = b->record.event;
            d->event_slot = b->record.event_slot;
            d->record_sector = b->record_sector;
            d->record_index = b->record_index;
            for (uint8_t s = 0U; s < BOOT_SLOTS; s++) {
                d->slot[s].state = b->record.slot[s].state;
                d->slot[s].version = b->record.slot[s].version;
                (void)memcpy(d->slot[s].id, b->record.slot[s].id, BOOT_IMAGE_ID_SIZE);
                if (b->record.slot[s].state == BOOT_SLOT_CONFIRMED) {
                    d->active_slot = s;
                }
                if (b->record.slot[s].state == BOOT_SLOT_TRIAL) {
                    d->trial_slot = s;
                    d->attempts_used = b->attempts_used;
                    d->attempts_left = (uint8_t)(BOOT_MAX_ATTEMPTS - (uint32_t)b->attempts_used);
                }
            }
        }
    }

    return status;
}

/* ------------------------------------------------------------------ the rest */

boot_status_t boot_slot_by_address(const boot_t *b, uint32_t address, uint8_t *slot)
{
    boot_status_t status = BOOT_E_ARG;

    if (boot_ready(b) && (slot != NULL)) {
        for (uint8_t s = 0U; s < BOOT_SLOTS; s++) {
            if (b->config.slot[s].addr == address) {
                *slot = s;
                status = BOOT_OK;
            }
        }
    }

    return status;
}

/* The names of boot_status_t, boot_slot_state_t and boot_event_t, in the order of their values
 * (each enumeration starts at 0 and has no gaps). */
static const char *const s_status_names[] = {
    "ok",   "busy",    "argument",         "layout", "state",     "sequence",
    "size", "invalid", "nothing_bootable", "flash",  "power_lost"};
static const char *const s_state_names[] = {"empty",     "writing",  "trial",
                                            "confirmed", "previous", "rejected"};
static const char *const s_event_names[] = {"none",    "adopt",    "begin",  "commit",
                                            "confirm", "rollback", "reject", "fallback"};

_Static_assert((sizeof(s_status_names) / sizeof(s_status_names[0])) ==
                   ((uint32_t)BOOT_E_POWER_LOST + 1U),
               "one name per boot_status_t");
_Static_assert((sizeof(s_state_names) / sizeof(s_state_names[0])) ==
                   ((uint32_t)BOOT_SLOT_REJECTED + 1U),
               "one name per boot_slot_state_t");
_Static_assert((sizeof(s_event_names) / sizeof(s_event_names[0])) ==
                   ((uint32_t)BOOT_EVENT_FALLBACK + 1U),
               "one name per boot_event_t");

/* names[value] if value < count, else "?". */
static const char *name_of(const char *const *names, uint32_t count, uint32_t value)
{
    return (value < count) ? names[value] : "?";
}

const char *boot_status_name(boot_status_t status)
{
    return name_of(s_status_names, (uint32_t)(sizeof(s_status_names) / sizeof(s_status_names[0])),
                   (uint32_t)status);
}

const char *boot_slot_state_name(boot_slot_state_t state)
{
    return name_of(s_state_names, (uint32_t)(sizeof(s_state_names) / sizeof(s_state_names[0])),
                   (uint32_t)state);
}

const char *boot_event_name(boot_event_t event)
{
    return name_of(s_event_names, (uint32_t)(sizeof(s_event_names) / sizeof(s_event_names[0])),
                   (uint32_t)event);
}
