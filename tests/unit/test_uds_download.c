/* SPDX-License-Identifier: MIT */
/*
 * lib/uds: the download (uds.h, THE DOWNLOAD): eraseMemory, RequestDownload, TransferData,
 * RequestTransferExit and checkProgrammingDependencies, the order of each service's checks,
 * the block counter, the end of a download by a refusal, a session change or S3server, and
 * lib/boot's status as an NRC.
 *
 * Twice: on the fake backend of uds_test_support.h, which answers whatever the test chooses;
 * and on lib/boot's real slot writer over lib/flash's NOR model with the host's layout and
 * lib/bootimg as the validator, so that the mapping of lib/boot's refusals is tested against
 * the code it maps, and a download through the server commits an image the bootloader's select
 * then boots as a trial. The real backend does one lib/boot step per poll, as a flash task that
 * runs one step per job would, and the fake clock advances by the time the test chooses.
 */
#include <string.h>

#include "boot/boot.h"
#include "bootimg/bootimg.h"
#include "flash/nor.h"
#include "image/image.h"
#include "sha256/sha256.h"
#include "uds/uds.h"
#include "uds_test_support.h"
#include "unity.h"

#define LEN(a) ((uint32_t)sizeof(a))

/* The host layout (docs/update.md): metadata 0x0..0x2000, slot A 0x2000, slot B 0x102000. */
#define HOST_SIZE   0x202000U
#define HOST_SECTOR 4096U
#define SLOT_A      0x2000U
#define SLOT_B      0x102000U
#define SLOT_SIZE   0x100000U

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

/* ------------------------------------------------------------------------ building requests */

static void put32(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)(value >> 24U);
    at[1] = (uint8_t)(value >> 16U);
    at[2] = (uint8_t)(value >> 8U);
    at[3] = (uint8_t)value;
}

/* 31 01 FF 00 44 <address> <size> */
static void erase_req(uint8_t req[13], uint32_t address, uint32_t size)
{
    req[0] = 0x31;
    req[1] = 0x01;
    req[2] = 0xFF;
    req[3] = 0x00;
    req[4] = 0x44;
    put32(&req[5], address);
    put32(&req[9], size);
}

/* 34 00 44 <address> <size> */
static void download_req(uint8_t req[11], uint32_t address, uint32_t size)
{
    req[0] = 0x34;
    req[1] = 0x00;
    req[2] = 0x44;
    put32(&req[3], address);
    put32(&req[7], size);
}

static const uint8_t k_erase_ok[] = {0x71, 0x01, 0xFF, 0x00, 0x00, 0x00};
static const uint8_t k_download_ok[] = {0x74, 0x20, 0x04, 0x02};
static const uint8_t k_exit[] = {0x37};
static const uint8_t k_exit_ok[] = {0x77};
static const uint8_t k_check[] = {0x31, 0x01, 0xFF, 0x01};
static const uint8_t k_check_ok[] = {0x71, 0x01, 0xFF, 0x01, 0x00, 0x00};

static void erase_expect(uint32_t address, uint32_t size, uint8_t nrc)
{
    uint8_t req[13];

    erase_req(req, address, size);
    if (nrc == UDS_NRC_NONE) {
        send_expect(req, LEN(req), k_erase_ok, LEN(k_erase_ok));
    } else {
        send_expect_nrc(req, LEN(req), nrc);
    }
}

static void download_expect(uint32_t address, uint32_t size, uint8_t nrc)
{
    uint8_t req[11];

    download_req(req, address, size);
    if (nrc == UDS_NRC_NONE) {
        send_expect(req, LEN(req), k_download_ok, LEN(k_download_ok));
    } else {
        send_expect_nrc(req, LEN(req), nrc);
    }
}

/* 36 <counter> <data>: send it; the length of the response. */
static uint32_t send_block(uint8_t counter, const uint8_t *data, uint32_t len)
{
    static uint8_t req[UDS_MAX_BLOCK_LENGTH + 64U];

    TEST_ASSERT_TRUE(len <= (sizeof(req) - 2U));
    req[0] = 0x36;
    req[1] = counter;
    if (len > 0U) {
        (void)memcpy(&req[2], data, len);
    }
    return send(req, len + 2U);
}

static void block_expect(uint8_t counter, const uint8_t *data, uint32_t len, uint8_t nrc)
{
    (void)send_block(counter, data, len);
    if (nrc == UDS_NRC_NONE) {
        const uint8_t ok[] = {0x76, counter};

        expect_rsp(ok, LEN(ok));
    } else {
        expect_nrc(0x36, nrc);
    }
}

/* A pattern of `len` bytes. */
static uint8_t s_pattern[FAKE_IMAGE_MAX];

static void make_pattern(uint32_t seed)
{
    for (uint32_t i = 0U; i < FAKE_IMAGE_MAX; i++) {
        s_pattern[i] = (uint8_t)((i * 7U) + (i >> 8U) + seed);
    }
}

/* Send `size` bytes of s_pattern in blocks of `block` bytes, counters from `first`. */
static uint8_t send_all(uint32_t size, uint32_t block, uint8_t first)
{
    uint8_t counter = first;

    for (uint32_t offset = 0U; offset < size; offset += block) {
        const uint32_t n = ((size - offset) < block) ? (size - offset) : block;

        block_expect(counter, &s_pattern[offset], n, UDS_NRC_NONE);
        counter++;
    }
    return (uint8_t)(counter - 1U);
}

/* Programming session, eraseMemory and RequestDownload for (address, size), on the fake. */
static void begin_download(uint32_t address, uint32_t size)
{
    enter_session(UDS_SESSION_PROGRAMMING);
    erase_expect(address, size, UDS_NRC_NONE);
    download_expect(address, size, UDS_NRC_NONE);
}

/* -------------------------------------------------------------------- on the fake backend */

/* The whole sequence: every response, the backend's arguments, the data at their offsets, the
 * states and the statistics, and the durations on the fake clock. */
static void test_the_whole_download_on_the_fake_backend(void)
{
    static const uint8_t erase_results[] = {0x31, 0x03, 0xFF, 0x00};
    static const uint8_t erase_results_ok[] = {0x71, 0x03, 0xFF, 0x00, 0x00, 0x00};
    static const uint8_t check_results[] = {0x31, 0x03, 0xFF, 0x01};
    static const uint8_t check_results_ok[] = {0x71, 0x03, 0xFF, 0x01, 0x00, 0x00};

    make_pattern(1U);
    enter_session(UDS_SESSION_PROGRAMMING);
    TEST_ASSERT_EQUAL(UDS_DL_IDLE, stats().download.state);
    erase_expect(SLOT_B, 3000U, UDS_NRC_NONE);
    TEST_ASSERT_EQUAL_HEX32(SLOT_B, g_fake.erase_address);
    TEST_ASSERT_EQUAL_UINT32(3000U, g_fake.erase_size);
    TEST_ASSERT_EQUAL(UDS_DL_ERASED, stats().download.state);

    const uint64_t t = g_now;

    download_expect(SLOT_B, 3000U, UDS_NRC_NONE);
    TEST_ASSERT_EQUAL(UDS_DL_TRANSFER, stats().download.state);
    g_now += 10U * MS;
    block_expect(1U, &s_pattern[0], 1024U, UDS_NRC_NONE);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.write_offset);
    TEST_ASSERT_EQUAL_UINT32(1024U, g_fake.write_len);
    g_now += 10U * MS;
    block_expect(2U, &s_pattern[1024], 1024U, UDS_NRC_NONE);
    TEST_ASSERT_EQUAL_UINT32(1024U, g_fake.write_offset);
    g_now += 10U * MS;
    block_expect(3U, &s_pattern[2048], 952U, UDS_NRC_NONE);
    TEST_ASSERT_EQUAL_UINT32(2048U, g_fake.write_offset);
    TEST_ASSERT_EQUAL_UINT32(952U, g_fake.write_len);
    g_now += 10U * MS;
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
    TEST_ASSERT_EQUAL(UDS_DL_EXITED, stats().download.state);
    send_expect(k_check, LEN(k_check), k_check_ok, LEN(k_check_ok));
    TEST_ASSERT_EQUAL(UDS_DL_CHECKED, stats().download.state);
    send_expect(erase_results, LEN(erase_results), erase_results_ok, LEN(erase_results_ok));
    send_expect(check_results, LEN(check_results), check_results_ok, LEN(check_results_ok));

    TEST_ASSERT_EQUAL_UINT32(3000U, g_fake.bytes_written);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_pattern, g_fake.image, 3000U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.transfer_exit.starts);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.check.starts);

    const uds_stats_t st = stats();

    TEST_ASSERT_EQUAL_HEX32(SLOT_B, st.download.address);
    TEST_ASSERT_EQUAL_UINT32(3000U, st.download.size);
    TEST_ASSERT_EQUAL_UINT32(3000U, st.download.received);
    TEST_ASSERT_EQUAL_UINT32(3U, st.download.blocks);
    TEST_ASSERT_EQUAL_UINT32(0U, st.download.repeated_blocks);
    TEST_ASSERT_EQUAL_UINT8(3U, st.download.counter);
    TEST_ASSERT_EQUAL_UINT32(1U, st.download.erases);
    TEST_ASSERT_EQUAL_UINT32(1U, st.download.started);
    TEST_ASSERT_EQUAL_UINT32(1U, st.download.exited);
    TEST_ASSERT_EQUAL_UINT32(1U, st.download.committed);
    TEST_ASSERT_EQUAL_UINT32(0U, st.download.aborted);
    TEST_ASSERT_EQUAL_UINT32(0U, st.download.failed);
    TEST_ASSERT_EQUAL_UINT64(0U, st.download.erase_us);
    TEST_ASSERT_EQUAL_UINT64(40U * MS, st.download.transfer_us);
    TEST_ASSERT_EQUAL_UINT64(g_now - t - (40U * MS), st.download.check_us);
    TEST_ASSERT_EQUAL_UINT32(0U, st.response_pending);
}

/* RequestDownload's checks, one step at a time: each request passes the steps before and fails
 * the one it is for (uds_program.c, uds_svc_request_download()). */
static void test_request_download_checks_in_order(void)
{
    static const uint8_t short2[] = {0x34, 0x00};
    /* An addressAndLengthFormatIdentifier other than 0x44, with a length that would be wrong
     * for 0x44 as well: the identifier is reported. */
    static const uint8_t alfid_33[] = {0x34, 0x00, 0x33, 0x10, 0x20, 0x00, 0x00, 0x00, 0x10};
    static const uint8_t alfid_24[] = {0x34, 0x00, 0x24, 0x10, 0x20, 0x00, 0x00, 0x10};
    static const uint8_t length_10[] = {0x34, 0x00, 0x44, 0x00, 0x10, 0x20, 0x00, 0x00, 0x00, 0x10};
    static const uint8_t length_12[] = {0x34, 0x00, 0x44, 0x00, 0x10, 0x20,
                                        0x00, 0x00, 0x00, 0x10, 0x00, 0x00};
    /* A dataFormatIdentifier other than 0x00 (compression or encryption), with no erase made:
     * the identifier is reported before the missing erase. */
    static const uint8_t dfi_11[] = {0x34, 0x11, 0x44, 0x00, 0x10, 0x20,
                                     0x00, 0x00, 0x00, 0x10, 0x00};
    static const uint8_t dfi_01[] = {0x34, 0x01, 0x44, 0x00, 0x10, 0x20,
                                     0x00, 0x00, 0x00, 0x10, 0x00};

    enter_session(UDS_SESSION_PROGRAMMING);
    send_expect_nrc(k_exit, 1U, UDS_NRC_REQUEST_SEQUENCE_ERROR); /* nothing to exit */
    (void)send(short2, 1U);
    expect_nrc(0x34, UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(short2, LEN(short2), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(alfid_33, LEN(alfid_33), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(alfid_24, LEN(alfid_24), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(length_10, LEN(length_10), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(length_12, LEN(length_12), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(dfi_11, LEN(dfi_11), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(dfi_01, LEN(dfi_01), UDS_NRC_REQUEST_OUT_OF_RANGE);
    /* Well formed, but nothing erased. */
    download_expect(SLOT_B, 4096U, UDS_NRC_DOWNLOAD_NOT_ACCEPTED);

    /* Erased for (SLOT_B, 4096): another address or size is out of range. */
    erase_expect(SLOT_B, 4096U, UDS_NRC_NONE);
    download_expect(SLOT_A, 4096U, UDS_NRC_REQUEST_OUT_OF_RANGE);
    download_expect(SLOT_B, 4095U, UDS_NRC_REQUEST_OUT_OF_RANGE);
    download_expect(SLOT_B, 4097U, UDS_NRC_REQUEST_OUT_OF_RANGE);
    download_expect(SLOT_B + 1U, 4096U, UDS_NRC_REQUEST_OUT_OF_RANGE);
    TEST_ASSERT_EQUAL(UDS_DL_ERASED, stats().download.state);
    /* The DFI check comes before the state checks, with an erase made too. */
    send_expect_nrc(dfi_11, LEN(dfi_11), UDS_NRC_REQUEST_OUT_OF_RANGE);
    download_expect(SLOT_B, 4096U, UDS_NRC_NONE);

    /* In TRANSFER a second RequestDownload is conditionsNotCorrect, whatever it names. */
    download_expect(SLOT_B, 4096U, UDS_NRC_CONDITIONS_NOT_CORRECT);
    download_expect(SLOT_A, 1U, UDS_NRC_CONDITIONS_NOT_CORRECT);
    TEST_ASSERT_EQUAL(UDS_DL_TRANSFER, stats().download.state);

    /* After the exit and after the check, a new download needs a new erase. */
    make_pattern(2U);
    (void)send_all(4096U, 1024U, 1U);
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
    download_expect(SLOT_B, 4096U, UDS_NRC_DOWNLOAD_NOT_ACCEPTED);
    send_expect(k_check, LEN(k_check), k_check_ok, LEN(k_check_ok));
    download_expect(SLOT_B, 4096U, UDS_NRC_DOWNLOAD_NOT_ACCEPTED);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.started);
}

/* TransferData's checks, one step at a time (uds_program.c, transfer_checks()). */
static void test_transfer_data_checks_in_order(void)
{
    static const uint8_t data[4] = {1, 2, 3, 4};

    enter_session(UDS_SESSION_PROGRAMMING);
    /* 1: a request of 1 byte has no counter. */
    (void)send_block(0x01, data, 0U);
    expect_nrc(0x36, UDS_NRC_REQUEST_SEQUENCE_ERROR); /* 2 bytes: past step 1, no download */
    {
        const uint8_t one[1] = {0x36};

        send_expect_nrc(one, LEN(one), UDS_NRC_INCORRECT_LENGTH);
    }
    /* 2: no download: requestSequenceError, also with a block too long. */
    (void)send_block(0x01, s_pattern, UDS_BLOCK_DATA_MAX + 1U);
    expect_nrc(0x36, UDS_NRC_REQUEST_SEQUENCE_ERROR);
    erase_expect(SLOT_B, 1030U, UDS_NRC_NONE);
    (void)send_block(0x01, data, 4U);
    expect_nrc(0x36, UDS_NRC_REQUEST_SEQUENCE_ERROR); /* erased, not requested */
    download_expect(SLOT_B, 1030U, UDS_NRC_NONE);

    /* 3: longer than maxNumberOfBlockLength, before the counter is looked at. */
    (void)send_block(0x07, s_pattern, UDS_BLOCK_DATA_MAX + 1U);
    expect_nrc(0x36, UDS_NRC_REQUEST_OUT_OF_RANGE);
    /* 6: the counter: not the next one. */
    block_expect(0x00, data, 4U, UDS_NRC_WRONG_BLOCK_COUNTER);
    block_expect(0x02, data, 4U, UDS_NRC_WRONG_BLOCK_COUNTER);
    /* 7: no data. */
    block_expect(0x01, data, 0U, UDS_NRC_INCORRECT_LENGTH);
    /* A block of exactly maxNumberOfBlockLength is taken. */
    make_pattern(3U);
    block_expect(0x01, s_pattern, UDS_BLOCK_DATA_MAX, UDS_NRC_NONE);
    /* 4: the last counter again is a repeat: positive, not written; before the other checks. */
    block_expect(0x01, data, 0U, UDS_NRC_NONE);
    block_expect(0x01, s_pattern, UDS_BLOCK_DATA_MAX + 1U, UDS_NRC_REQUEST_OUT_OF_RANGE);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.write.starts);
    /* 6 again, after a block. */
    block_expect(0x03, data, 4U, UDS_NRC_WRONG_BLOCK_COUNTER);
    block_expect(0xFE, data, 4U, UDS_NRC_WRONG_BLOCK_COUNTER);
    /* 8: more data than the size leaves: transferDataSuspended, and the download ends. */
    block_expect(0x02, s_pattern, 7U, UDS_NRC_TRANSFER_SUSPENDED);
    TEST_ASSERT_EQUAL(UDS_DL_IDLE, stats().download.state);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.failed);
    block_expect(0x02, data, 4U, UDS_NRC_REQUEST_SEQUENCE_ERROR);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.write.starts);
}

/* 5: once every byte is taken, a new counter is requestSequenceError; the last one is still a
 * repeat. */
static void test_after_the_last_byte_only_a_repeat_is_taken(void)
{
    make_pattern(4U);
    begin_download(SLOT_B, 2000U);
    (void)send_all(2000U, 1000U, 1U);
    block_expect(0x03, s_pattern, 1U, UDS_NRC_REQUEST_SEQUENCE_ERROR);
    block_expect(0x05, s_pattern, 1U, UDS_NRC_REQUEST_SEQUENCE_ERROR);
    block_expect(0x02, &s_pattern[1000], 1000U, UDS_NRC_NONE);
    TEST_ASSERT_EQUAL_UINT32(2U, g_fake.write.starts);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.repeated_blocks);
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
}

/* The counter runs 1..0xFF, then 0x00, 0x01, ...; a repeat after the wrap is a repeat. */
static void test_the_block_counter_wraps_from_ff_to_00(void)
{
    const uint32_t blocks = 600U;
    const uint32_t size = blocks * 4U;

    make_pattern(5U);
    begin_download(SLOT_B, size);
    for (uint32_t i = 0U; i < blocks; i++) {
        const uint8_t counter = (uint8_t)(i + 1U);

        block_expect(counter, &s_pattern[(size_t)i * 4U], 4U, UDS_NRC_NONE);
        if (((i + 1U) % 256U) == 0U) {
            /* Block 256 (and 512) has the counter 0x00; sent again, it is a repeat. */
            block_expect(0x00, &s_pattern[(size_t)i * 4U], 4U, UDS_NRC_NONE);
        }
        if (i == 255U) {
            TEST_ASSERT_EQUAL_UINT8(0x00, stats().download.counter);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(size, g_fake.bytes_written);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_pattern, g_fake.image, size);
    TEST_ASSERT_EQUAL_UINT32(blocks, stats().download.blocks);
    TEST_ASSERT_EQUAL_UINT32(2U, stats().download.repeated_blocks);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)blocks, stats().download.counter);
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
}

/* A repeated block is answered positively and not written: the first, a middle one, the last. */
static void test_a_repeated_block_is_answered_without_writing(void)
{
    make_pattern(6U);
    begin_download(SLOT_B, 3072U);
    block_expect(1U, &s_pattern[0], 1024U, UDS_NRC_NONE);
    block_expect(1U, &s_pattern[0], 1024U, UDS_NRC_NONE);
    block_expect(2U, &s_pattern[1024], 1024U, UDS_NRC_NONE);
    block_expect(2U, &s_pattern[1024], 1024U, UDS_NRC_NONE);
    block_expect(2U, &s_pattern[1024], 1024U, UDS_NRC_NONE);
    block_expect(3U, &s_pattern[2048], 1024U, UDS_NRC_NONE);
    block_expect(3U, &s_pattern[2048], 1024U, UDS_NRC_NONE);
    TEST_ASSERT_EQUAL_UINT32(3U, g_fake.write.starts);
    TEST_ASSERT_EQUAL_UINT32(3072U, g_fake.bytes_written);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_pattern, g_fake.image, 3072U);
    TEST_ASSERT_EQUAL_UINT32(4U, stats().download.repeated_blocks);
    TEST_ASSERT_EQUAL_UINT32(3U, stats().download.blocks);
}

/* A wrong counter is refused and the download goes on. */
static void test_a_wrong_counter_keeps_the_download(void)
{
    make_pattern(7U);
    begin_download(SLOT_B, 2048U);
    block_expect(0x00, &s_pattern[0], 1024U, UDS_NRC_WRONG_BLOCK_COUNTER);
    block_expect(0xFF, &s_pattern[0], 1024U, UDS_NRC_WRONG_BLOCK_COUNTER);
    block_expect(0x01, &s_pattern[0], 1024U, UDS_NRC_NONE);
    block_expect(0x03, &s_pattern[1024], 1024U, UDS_NRC_WRONG_BLOCK_COUNTER);
    TEST_ASSERT_EQUAL(UDS_DL_TRANSFER, stats().download.state);
    block_expect(0x02, &s_pattern[1024], 1024U, UDS_NRC_NONE);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_pattern, g_fake.image, 2048U);
    TEST_ASSERT_EQUAL_UINT32(3U, nrc_count(UDS_NRC_WRONG_BLOCK_COUNTER));
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
}

/* More bytes than the size: transferDataSuspended, the download ends, nothing is written. */
static void test_too_many_bytes_end_the_download(void)
{
    make_pattern(8U);
    begin_download(SLOT_B, 1000U);
    block_expect(1U, s_pattern, 1001U, UDS_NRC_TRANSFER_SUSPENDED);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.write.starts);
    TEST_ASSERT_EQUAL(UDS_DL_IDLE, stats().download.state);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.failed);
    block_expect(1U, s_pattern, 1000U, UDS_NRC_REQUEST_SEQUENCE_ERROR);
    send_expect_nrc(k_exit, LEN(k_exit), UDS_NRC_REQUEST_SEQUENCE_ERROR);
    download_expect(SLOT_B, 1000U, UDS_NRC_DOWNLOAD_NOT_ACCEPTED);

    /* After a block that was taken, the same. */
    erase_expect(SLOT_B, 1000U, UDS_NRC_NONE);
    download_expect(SLOT_B, 1000U, UDS_NRC_NONE);
    block_expect(1U, s_pattern, 600U, UDS_NRC_NONE);
    block_expect(2U, &s_pattern[600], 401U, UDS_NRC_TRANSFER_SUSPENDED);
    TEST_ASSERT_EQUAL_UINT32(2U, stats().download.failed);
    TEST_ASSERT_EQUAL_UINT32(600U, g_fake.bytes_written);
}

/* RequestTransferExit before the last byte is requestSequenceError and the download goes on. */
static void test_exit_before_the_last_byte_is_refused_and_the_download_goes_on(void)
{
    static const uint8_t exit_long[] = {0x37, 0x00};

    make_pattern(9U);
    begin_download(SLOT_B, 2049U);
    send_expect_nrc(k_exit, LEN(k_exit), UDS_NRC_REQUEST_SEQUENCE_ERROR);
    block_expect(1U, &s_pattern[0], 1024U, UDS_NRC_NONE);
    block_expect(2U, &s_pattern[1024], 1024U, UDS_NRC_NONE);
    send_expect_nrc(k_exit, LEN(k_exit), UDS_NRC_REQUEST_SEQUENCE_ERROR);
    TEST_ASSERT_EQUAL(UDS_DL_TRANSFER, stats().download.state);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.transfer_exit.starts);
    block_expect(3U, &s_pattern[2048], 1U, UDS_NRC_NONE);
    /* A transferRequestParameterRecord is not taken: the length is checked first. */
    send_expect_nrc(exit_long, LEN(exit_long), UDS_NRC_INCORRECT_LENGTH);
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
    send_expect_nrc(k_exit, LEN(k_exit), UDS_NRC_REQUEST_SEQUENCE_ERROR); /* EXITED */
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.transfer_exit.starts);
}

/* The check routine comes once, after the exit. */
static void test_the_check_comes_once_after_the_exit(void)
{
    static const uint8_t check_long[] = {0x31, 0x01, 0xFF, 0x01, 0x00};
    static const uint8_t results[] = {0x31, 0x03, 0xFF, 0x01};

    make_pattern(10U);
    enter_session(UDS_SESSION_PROGRAMMING);
    send_expect_nrc(k_check, LEN(k_check), UDS_NRC_REQUEST_SEQUENCE_ERROR); /* IDLE */
    send_expect_nrc(results, LEN(results), UDS_NRC_REQUEST_SEQUENCE_ERROR); /* never ran */
    erase_expect(SLOT_B, 100U, UDS_NRC_NONE);
    send_expect_nrc(k_check, LEN(k_check), UDS_NRC_REQUEST_SEQUENCE_ERROR); /* ERASED */
    download_expect(SLOT_B, 100U, UDS_NRC_NONE);
    send_expect_nrc(k_check, LEN(k_check), UDS_NRC_REQUEST_SEQUENCE_ERROR); /* TRANSFER */
    block_expect(1U, s_pattern, 100U, UDS_NRC_NONE);
    send_expect_nrc(k_check, LEN(k_check), UDS_NRC_REQUEST_SEQUENCE_ERROR); /* all bytes */
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
    send_expect_nrc(check_long, LEN(check_long), UDS_NRC_INCORRECT_LENGTH);
    send_expect(k_check, LEN(k_check), k_check_ok, LEN(k_check_ok));
    send_expect_nrc(k_check, LEN(k_check), UDS_NRC_REQUEST_SEQUENCE_ERROR); /* CHECKED */
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.check.starts);
}

/* eraseMemory's checks, one step at a time (uds_program.c, start_checks()). */
static void test_erase_checks_in_order(void)
{
    static const uint8_t no_record[] = {0x31, 0x01, 0xFF, 0x00};
    static const uint8_t alfid_only_33[] = {0x31, 0x01, 0xFF, 0x00, 0x33};
    static const uint8_t alfid_33[] = {0x31, 0x01, 0xFF, 0x00, 0x33, 0x00, 0x10,
                                       0x20, 0x00, 0x00, 0x00, 0x10, 0x00};
    static const uint8_t short6[] = {0x31, 0x01, 0xFF, 0x00, 0x44, 0x00};
    static const uint8_t long14[] = {0x31, 0x01, 0xFF, 0x00, 0x44, 0x00, 0x10,
                                     0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00};
    static const uint8_t results[] = {0x31, 0x03, 0xFF, 0x00};

    enter_session(UDS_SESSION_PROGRAMMING);
    send_expect_nrc(results, LEN(results), UDS_NRC_REQUEST_SEQUENCE_ERROR);
    send_expect_nrc(no_record, LEN(no_record), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(alfid_only_33, LEN(alfid_only_33), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(alfid_33, LEN(alfid_33), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(short6, LEN(short6), UDS_NRC_INCORRECT_LENGTH);
    send_expect_nrc(long14, LEN(long14), UDS_NRC_INCORRECT_LENGTH);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.erase.starts);
    /* A routine refused by its checks has not run. */
    send_expect_nrc(results, LEN(results), UDS_NRC_REQUEST_SEQUENCE_ERROR);

    /* In TRANSFER an erase is conditionsNotCorrect and the download goes on. */
    erase_expect(SLOT_B, 10U, UDS_NRC_NONE);
    download_expect(SLOT_B, 10U, UDS_NRC_NONE);
    erase_expect(SLOT_B, 10U, UDS_NRC_CONDITIONS_NOT_CORRECT);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.erase.starts);
    TEST_ASSERT_EQUAL(UDS_DL_TRANSFER, stats().download.state);
    make_pattern(11U);
    block_expect(1U, s_pattern, 10U, UDS_NRC_NONE);
}

/* Every lib/boot refusal of the erase, as its NRC; nothing is erased for a download after it,
 * whatever was before, and requestRoutineResults reports it. */
static void test_erase_refusals_map_to_their_nrcs(void)
{
    static const struct {
        boot_status_t status;
        uint8_t nrc;
    } cases[] = {{BOOT_E_ARG, UDS_NRC_REQUEST_OUT_OF_RANGE},
                 {BOOT_E_SIZE, UDS_NRC_REQUEST_OUT_OF_RANGE},
                 {BOOT_E_STATE, UDS_NRC_CONDITIONS_NOT_CORRECT},
                 {BOOT_E_SEQUENCE, UDS_NRC_REQUEST_SEQUENCE_ERROR},
                 {BOOT_E_FLASH, UDS_NRC_PROGRAMMING_FAILURE},
                 {BOOT_E_POWER_LOST, UDS_NRC_PROGRAMMING_FAILURE},
                 {BOOT_E_INVALID, UDS_NRC_PROGRAMMING_FAILURE},
                 {BOOT_BUSY, UDS_NRC_PROGRAMMING_FAILURE}};
    static const uint8_t results[] = {0x31, 0x03, 0xFF, 0x00};

    enter_session(UDS_SESSION_PROGRAMMING);
    for (uint32_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        const uint8_t failed[] = {0x71, 0x03, 0xFF, 0x00, 0x01, cases[i].nrc};

        g_fake.erase.status = BOOT_OK;
        erase_expect(SLOT_B, 4096U, UDS_NRC_NONE);
        g_fake.erase.status = cases[i].status;
        erase_expect(SLOT_B, 4096U, cases[i].nrc);
        TEST_ASSERT_EQUAL(UDS_DL_IDLE, stats().download.state);
        send_expect(results, LEN(results), failed, LEN(failed));
        download_expect(SLOT_B, 4096U, UDS_NRC_DOWNLOAD_NOT_ACCEPTED);
    }
    TEST_ASSERT_EQUAL_UINT32(8U, stats().download.erases);
    TEST_ASSERT_EQUAL_UINT32(0U, stats().download.failed); /* no download had begun */
}

/* Every refusal of a write ends the download with its NRC. */
static void test_write_refusals_end_the_download(void)
{
    static const struct {
        boot_status_t status;
        uint8_t nrc;
    } cases[] = {{BOOT_E_SEQUENCE, UDS_NRC_REQUEST_SEQUENCE_ERROR},
                 {BOOT_E_SIZE, UDS_NRC_REQUEST_OUT_OF_RANGE},
                 {BOOT_E_STATE, UDS_NRC_CONDITIONS_NOT_CORRECT},
                 {BOOT_E_FLASH, UDS_NRC_PROGRAMMING_FAILURE},
                 {BOOT_E_POWER_LOST, UDS_NRC_PROGRAMMING_FAILURE},
                 {BOOT_BUSY, UDS_NRC_PROGRAMMING_FAILURE}};

    make_pattern(12U);
    enter_session(UDS_SESSION_PROGRAMMING);
    for (uint32_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        g_fake.write.status = BOOT_OK;
        erase_expect(SLOT_B, 2048U, UDS_NRC_NONE);
        download_expect(SLOT_B, 2048U, UDS_NRC_NONE);
        block_expect(1U, s_pattern, 1024U, UDS_NRC_NONE);
        g_fake.write.status = cases[i].status;
        block_expect(2U, &s_pattern[1024], 1024U, cases[i].nrc);
        TEST_ASSERT_EQUAL(UDS_DL_IDLE, stats().download.state);
        TEST_ASSERT_EQUAL_UINT32(i + 1U, stats().download.failed);
        /* No repeat of block 1 either: the download is over. */
        block_expect(1U, s_pattern, 1024U, UDS_NRC_REQUEST_SEQUENCE_ERROR);
        TEST_ASSERT_EQUAL_UINT32(1024U, stats().download.received);
    }
}

/* The exit's refusals end the download; BOOT_OK is a success as BOOT_BUSY is. */
static void test_exit_refusals_end_the_download(void)
{
    static const struct {
        boot_status_t status;
        uint8_t nrc;
    } cases[] = {{BOOT_E_FLASH, UDS_NRC_PROGRAMMING_FAILURE},
                 {BOOT_E_SEQUENCE, UDS_NRC_REQUEST_SEQUENCE_ERROR},
                 {BOOT_E_INVALID, UDS_NRC_PROGRAMMING_FAILURE},
                 {BOOT_E_STATE, UDS_NRC_CONDITIONS_NOT_CORRECT},
                 {BOOT_OK, UDS_NRC_NONE},
                 {BOOT_BUSY, UDS_NRC_NONE}};

    make_pattern(13U);
    enter_session(UDS_SESSION_PROGRAMMING);
    for (uint32_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        begin_download(SLOT_B, 512U);
        block_expect(1U, s_pattern, 512U, UDS_NRC_NONE);
        g_fake.transfer_exit.status = cases[i].status;
        if (cases[i].nrc == UDS_NRC_NONE) {
            send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
            TEST_ASSERT_EQUAL(UDS_DL_EXITED, stats().download.state);
        } else {
            send_expect_nrc(k_exit, LEN(k_exit), cases[i].nrc);
            TEST_ASSERT_EQUAL(UDS_DL_IDLE, stats().download.state);
            send_expect_nrc(k_check, LEN(k_check), UDS_NRC_REQUEST_SEQUENCE_ERROR);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(4U, stats().download.failed);
    TEST_ASSERT_EQUAL_UINT32(2U, stats().download.exited);
}

/* The check's refusals end the download; BOOT_BUSY at its end is a failure. */
static void test_check_refusals_end_the_download(void)
{
    static const struct {
        boot_status_t status;
        uint8_t nrc;
    } cases[] = {{BOOT_E_INVALID, UDS_NRC_PROGRAMMING_FAILURE},
                 {BOOT_E_FLASH, UDS_NRC_PROGRAMMING_FAILURE},
                 {BOOT_E_STATE, UDS_NRC_CONDITIONS_NOT_CORRECT},
                 {BOOT_E_SEQUENCE, UDS_NRC_REQUEST_SEQUENCE_ERROR},
                 {BOOT_BUSY, UDS_NRC_PROGRAMMING_FAILURE}};
    static const uint8_t results[] = {0x31, 0x03, 0xFF, 0x01};

    make_pattern(14U);
    for (uint32_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        const uint8_t failed[] = {0x71, 0x03, 0xFF, 0x01, 0x01, cases[i].nrc};

        begin_download(SLOT_B, 256U);
        block_expect(1U, s_pattern, 256U, UDS_NRC_NONE);
        send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
        g_fake.check.status = cases[i].status;
        send_expect_nrc(k_check, LEN(k_check), cases[i].nrc);
        TEST_ASSERT_EQUAL(UDS_DL_IDLE, stats().download.state);
        send_expect(results, LEN(results), failed, LEN(failed));
        send_expect_nrc(k_check, LEN(k_check), UDS_NRC_REQUEST_SEQUENCE_ERROR);
        TEST_ASSERT_EQUAL_UINT32(i + 1U, stats().download.failed);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, stats().download.committed);
}

/* S3server ends a download: default session, and in the programming session again nothing is
 * erased or requested. */
static void test_s3_ends_a_download(void)
{
    make_pattern(15U);
    begin_download(SLOT_B, 4096U);
    block_expect(1U, s_pattern, 1024U, UDS_NRC_NONE);
    const uint64_t t = g_now;

    (void)poll_at(t + (5000U * MS) - 1U);
    TEST_ASSERT_EQUAL(UDS_DL_TRANSFER, stats().download.state);
    (void)poll_at(t + (5000U * MS));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_DEFAULT, uds_session(&g_srv));
    TEST_ASSERT_EQUAL(UDS_DL_IDLE, stats().download.state);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.aborted);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().s3_timeouts);

    block_expect(2U, &s_pattern[1024], 1024U, UDS_NRC_SERVICE_NOT_IN_SESSION);
    enter_session(UDS_SESSION_PROGRAMMING);
    block_expect(2U, &s_pattern[1024], 1024U, UDS_NRC_REQUEST_SEQUENCE_ERROR);
    send_expect_nrc(k_exit, LEN(k_exit), UDS_NRC_REQUEST_SEQUENCE_ERROR);
    download_expect(SLOT_B, 4096U, UDS_NRC_DOWNLOAD_NOT_ACCEPTED);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.write.starts);

    /* An S3server timeout after a completed download is no abort. */
    erase_expect(SLOT_B, 256U, UDS_NRC_NONE);
    download_expect(SLOT_B, 256U, UDS_NRC_NONE);
    block_expect(1U, s_pattern, 256U, UDS_NRC_NONE);
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
    send_expect(k_check, LEN(k_check), k_check_ok, LEN(k_check_ok));
    (void)poll_at(g_now + (5000U * MS));
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.aborted);
    TEST_ASSERT_EQUAL_UINT32(2U, stats().s3_timeouts);
}

/* Every accepted DiagnosticSessionControl begins its session afresh and ends a download. */
static void test_a_session_control_ends_a_download(void)
{
    static const uint8_t prog[] = {0x10, 0x02};
    static const uint8_t prog_rsp[] = {0x50, 0x02, 0x00, 0x32, 0x01, 0xF4};
    static const uint8_t def[] = {0x10, 0x01};
    static const uint8_t def_rsp[] = {0x50, 0x01, 0x00, 0x32, 0x01, 0xF4};

    make_pattern(16U);
    begin_download(SLOT_B, 2048U);
    block_expect(1U, s_pattern, 1024U, UDS_NRC_NONE);

    /* Entering the programming session again ends it as leaving it would. */
    send_expect(prog, LEN(prog), prog_rsp, LEN(prog_rsp));
    TEST_ASSERT_EQUAL(UDS_DL_IDLE, stats().download.state);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.aborted);
    block_expect(2U, &s_pattern[1024], 1024U, UDS_NRC_REQUEST_SEQUENCE_ERROR);
    download_expect(SLOT_B, 2048U, UDS_NRC_DOWNLOAD_NOT_ACCEPTED);

    /* From ERASED: no download had begun, nothing is counted, nothing is erased any more. */
    erase_expect(SLOT_B, 2048U, UDS_NRC_NONE);
    send_expect(prog, LEN(prog), prog_rsp, LEN(prog_rsp));
    download_expect(SLOT_B, 2048U, UDS_NRC_DOWNLOAD_NOT_ACCEPTED);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.aborted);

    /* From EXITED, to the default session. */
    erase_expect(SLOT_B, 2048U, UDS_NRC_NONE);
    download_expect(SLOT_B, 2048U, UDS_NRC_NONE);
    (void)send_all(2048U, 1024U, 1U);
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
    send_expect(def, LEN(def), def_rsp, LEN(def_rsp));
    TEST_ASSERT_EQUAL_UINT32(2U, stats().download.aborted);
    enter_session(UDS_SESSION_PROGRAMMING);
    send_expect_nrc(k_check, LEN(k_check), UDS_NRC_REQUEST_SEQUENCE_ERROR);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.check.starts);
}

/* An erase after the exit begins the download again; the old one counts as aborted. */
static void test_an_erase_after_the_exit_begins_again(void)
{
    make_pattern(17U);
    begin_download(SLOT_B, 512U);
    block_expect(1U, s_pattern, 512U, UDS_NRC_NONE);
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
    erase_expect(SLOT_B, 1024U, UDS_NRC_NONE);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.aborted);
    TEST_ASSERT_EQUAL(UDS_DL_ERASED, stats().download.state);
    TEST_ASSERT_EQUAL_UINT32(1024U, stats().download.size);
    download_expect(SLOT_B, 1024U, UDS_NRC_NONE);
    TEST_ASSERT_EQUAL_UINT32(0U, stats().download.received);
    (void)send_all(1024U, 1024U, 1U); /* the counter starts at 1 again */
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
    send_expect(k_check, LEN(k_check), k_check_ok, LEN(k_check_ok));
    TEST_ASSERT_EQUAL_UINT32(2U, stats().download.started);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.committed);
}

/* A write that is pending works on the server's copy of the block: the caller's buffer is free
 * as soon as the request is delivered, and a request meanwhile is busyRepeatRequest. */
static void test_a_pending_write_keeps_its_data(void)
{
    static uint8_t req[2U + 1024U];
    const uint8_t tp_busy[] = {0x7F, 0x3E, 0x21};

    make_pattern(18U);
    begin_download(SLOT_B, 1024U);
    req[0] = 0x36;
    req[1] = 0x01;
    (void)memcpy(&req[2], s_pattern, 1024U);
    g_fake.write.pending = 3U;
    const uint64_t t = g_now;

    TEST_ASSERT_EQUAL_UINT32(0U, send(req, LEN(req)));
    (void)memset(req, 0, sizeof(req));
    req[0] = 0x3E;
    req[1] = 0x00;
    (void)send(req, 2U);
    expect_rsp(tp_busy, LEN(tp_busy));
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(t + (10U * MS)));
    TEST_ASSERT_EQUAL_UINT32(0U, poll_at(t + (20U * MS)));
    (void)poll_at(t + (30U * MS));
    {
        const uint8_t ok[] = {0x76, 0x01};

        expect_rsp(ok, LEN(ok));
    }
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_pattern, g_fake.image, 1024U);
    TEST_ASSERT_EQUAL_UINT32(1024U, stats().download.received);
}

/* A pending exit (the flush of the last page): responsePending, then 77; the transfer's
 * duration runs to the final response. */
static void test_a_pending_exit(void)
{
    make_pattern(20U);
    begin_download(SLOT_B, 300U);
    const uint64_t t = g_now;

    block_expect(1U, s_pattern, 300U, UDS_NRC_NONE);
    g_fake.transfer_exit.pending = 1000U;
    TEST_ASSERT_EQUAL_UINT32(0U, send(k_exit, LEN(k_exit)));
    TEST_ASSERT_EQUAL(UDS_DL_TRANSFER, stats().download.state);
    (void)poll_at(t + (40U * MS));
    expect_pending(0x37);
    g_fake.transfer_exit.left = 0U;
    (void)poll_at(t + (70U * MS));
    expect_rsp(k_exit_ok, LEN(k_exit_ok));
    TEST_ASSERT_EQUAL(UDS_DL_EXITED, stats().download.state);
    TEST_ASSERT_EQUAL_UINT64(70U * MS, stats().download.transfer_us);
}

/* A long erase: responsePending at 40 ms and every 2000 ms after, then the response. */
static void test_response_pending_during_a_long_erase(void)
{
    uint8_t req[13];

    enter_session(UDS_SESSION_PROGRAMMING);
    const uint64_t t = g_now;

    erase_req(req, SLOT_B, SLOT_SIZE);
    g_fake.erase.pending = 1000000U;
    TEST_ASSERT_EQUAL_UINT32(0U, send(req, LEN(req)));
    for (uint32_t ms = 1U; ms <= 5000U; ms++) {
        (void)poll_at(t + ((uint64_t)ms * MS));
        if ((ms == 40U) || (ms == 2040U) || (ms == 4040U)) {
            expect_pending(0x31);
        } else {
            expect_none();
        }
    }
    g_fake.erase.left = 0U;
    (void)poll_at(t + (5001U * MS));
    expect_rsp(k_erase_ok, LEN(k_erase_ok));
    TEST_ASSERT_EQUAL_UINT32(3U, stats().response_pending);
    TEST_ASSERT_EQUAL_UINT64(5001U * MS, stats().download.erase_us);
    TEST_ASSERT_EQUAL_UINT32(5001U, g_fake.erase.polls);
}

/* A request longer than the server keeps is answered by its service's length check; a block
 * longer than maxNumberOfBlockLength is requestOutOfRange. */
static void test_an_overlong_request_is_answered_without_being_kept(void)
{
    static uint8_t big[3000];

    (void)memset(big, 0x44, sizeof(big));
    big[0] = 0x10;
    big[1] = 0x01;
    send_expect_nrc(big, LEN(big), UDS_NRC_INCORRECT_LENGTH);
    big[0] = 0x22;
    big[1] = 0xF1;
    big[2] = 0x86;
    send_expect_nrc(big, LEN(big), UDS_NRC_INCORRECT_LENGTH);
    enter_session(UDS_SESSION_PROGRAMMING);
    big[0] = 0x31;
    big[1] = 0x01;
    big[2] = 0xFF;
    big[3] = 0x00;
    send_expect_nrc(big, LEN(big), UDS_NRC_INCORRECT_LENGTH);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.erase.starts);

    make_pattern(19U);
    begin_download(SLOT_B, 4096U);
    big[0] = 0x36;
    big[1] = 0x01;
    send_expect_nrc(big, LEN(big), UDS_NRC_REQUEST_OUT_OF_RANGE);
    send_expect_nrc(big, UDS_MAX_BLOCK_LENGTH + 1U, UDS_NRC_REQUEST_OUT_OF_RANGE);
    {
        const uint8_t ok[] = {0x76, 0x01};

        (void)send(big, UDS_REQUEST_MAX);
        expect_rsp(ok, LEN(ok));
    }
    TEST_ASSERT_EQUAL_UINT32(UDS_BLOCK_DATA_MAX, g_fake.bytes_written);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&big[2], g_fake.image, UDS_BLOCK_DATA_MAX);
}

/* -------------------------------------------- on lib/boot's slot writer over lib/flash's model */

static uint8_t s_flash[HOST_SIZE];
static uint32_t s_erase_counts[HOST_SIZE / HOST_SECTOR];
static nor_t s_nor;
static flash_dev_t s_dev;
static bootimg_t s_validator;
static boot_t s_boot;
static uint8_t s_image[IMAGE_HEADER_SIZE + 12000U];

/* The backend over lib/boot: one lib/boot step per call of an erase or a check, as the node's
 * flash task would make one step per job. */
typedef struct {
    boot_t *boot;
    boot_writer_t writer;
    uint8_t trial_slot; /* the slot the running trial was started from; BOOT_NO_SLOT: none */
    uint32_t erase_steps;
    uint32_t finish_steps;
    uint32_t writes;
} real_t;

static real_t s_real;

static uds_be_t real_ecu_reset(void *ctx, uds_call_t call, uint8_t reset_type, uint8_t *nrc)
{
    (void)ctx;
    (void)call;
    (void)reset_type;
    *nrc = UDS_NRC_NONE;
    return UDS_BE_DONE;
}

static uds_be_t real_boot_info(void *ctx, uds_call_t call, uds_boot_info_t *info, uint8_t *nrc)
{
    (void)ctx;
    (void)call;
    (void)memset(info, 0, sizeof(*info));
    *nrc = UDS_NRC_NONE;
    return UDS_BE_DONE;
}

static uds_be_t real_dtc_snapshot(void *ctx, uds_call_t call, const dtc_state_t **state,
                                  uint8_t *nrc)
{
    (void)ctx;
    (void)call;
    *state = NULL;
    *nrc = UDS_NRC_CONDITIONS_NOT_CORRECT;
    return UDS_BE_DONE;
}

static uds_be_t real_dtc_clear(void *ctx, uds_call_t call, uint32_t group, uint8_t *nrc)
{
    (void)ctx;
    (void)call;
    (void)group;
    *nrc = UDS_NRC_CONDITIONS_NOT_CORRECT;
    return UDS_BE_DONE;
}

/* START: boot_slot_by_address() and boot_writer_begin(); each POLL: one erase step. */
static uds_be_t real_erase(void *ctx, uds_call_t call, uint32_t address, uint32_t size,
                           boot_status_t *status)
{
    real_t *r = ctx;
    uds_be_t be = UDS_BE_PENDING;

    if (call == UDS_CALL_START) {
        uint8_t slot = BOOT_NO_SLOT;
        boot_status_t st = boot_slot_by_address(r->boot, address, &slot);

        if (st == BOOT_OK) {
            st = boot_writer_begin(&r->writer, r->boot, slot, size);
        }
        if (st != BOOT_OK) {
            *status = st;
            be = UDS_BE_DONE;
        }
    } else {
        const boot_status_t st = boot_writer_erase_step(&r->writer);

        r->erase_steps++;
        if (st != BOOT_BUSY) {
            *status = st;
            be = UDS_BE_DONE;
        }
    }
    return be;
}

static uds_be_t real_write(void *ctx, uds_call_t call, uint32_t offset, const uint8_t *data,
                           uint32_t len, boot_status_t *status)
{
    real_t *r = ctx;

    (void)call;
    r->writes++;
    *status = boot_writer_write(&r->writer, offset, data, len);
    return UDS_BE_DONE;
}

static uds_be_t real_transfer_exit(void *ctx, uds_call_t call, boot_status_t *status)
{
    real_t *r = ctx;

    (void)call;
    r->finish_steps++;
    *status = boot_writer_finish_step(&r->writer);
    return UDS_BE_DONE;
}

/* START: nothing yet; each POLL: one finish step, until it is not BOOT_BUSY. */
static uds_be_t real_check(void *ctx, uds_call_t call, boot_status_t *status)
{
    real_t *r = ctx;
    uds_be_t be = UDS_BE_PENDING;

    if (call == UDS_CALL_POLL) {
        const boot_status_t st = boot_writer_finish_step(&r->writer);

        r->finish_steps++;
        if (st != BOOT_BUSY) {
            *status = st;
            be = UDS_BE_DONE;
        }
    }
    return be;
}

/* The confirm of the running trial, as the node's flash task makes it; no trial: BOOT_E_STATE. */
static uds_be_t real_confirm(void *ctx, uds_call_t call, boot_status_t *status)
{
    real_t *r = ctx;

    (void)call;
    *status = (r->trial_slot == BOOT_NO_SLOT) ? BOOT_E_STATE : boot_confirm(r->boot, r->trial_slot);
    return UDS_BE_DONE;
}

static uds_backend_t real_backend(void)
{
    uds_backend_t b;

    (void)memset(&b, 0, sizeof(b));
    b.ctx = &s_real;
    b.ecu_reset = real_ecu_reset;
    b.boot_info = real_boot_info;
    b.dtc_snapshot = real_dtc_snapshot;
    b.dtc_clear = real_dtc_clear;
    b.erase = real_erase;
    b.write = real_write;
    b.transfer_exit = real_transfer_exit;
    b.check = real_check;
    b.confirm = real_confirm;
    return b;
}

/* An image of `payload_size` bytes (a pattern of `seed`) and `version` into `mem`. */
static image_header_t put_image(uint8_t *mem, uint32_t payload_size, uint32_t version, uint8_t seed)
{
    image_header_t header;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t *payload = &mem[IMAGE_PAYLOAD_OFFSET];

    for (uint32_t i = 0U; i < payload_size; i++) {
        payload[i] = (uint8_t)((i * 31U) + seed);
    }
    TEST_ASSERT_TRUE(sha256_compute(payload, payload_size, digest));
    TEST_ASSERT_TRUE(image_header_init(&header, IMAGE_TARGET_HOST, version, payload_size, digest));
    TEST_ASSERT_TRUE(image_header_encode(&header, mem));
    return header;
}

/* The host's flash, erased, with a factory image in slot A; with `adopt`, the bootloader's
 * first select has adopted it. A server on the real backend in the programming session. */
static void real_bench(bool adopt)
{
    const nor_config_t config = {.geometry = {.size = HOST_SIZE,
                                              .sector_size = HOST_SECTOR,
                                              .page_size = 256U,
                                              .unit_size = 8U},
                                 .timing = {.page_program_ns = 0U, .sector_erase_ns = 0U},
                                 .memory = s_flash,
                                 .erase_counts = s_erase_counts};
    boot_config_t boot;

    (void)memset(s_flash, 0xFF, sizeof(s_flash));
    TEST_ASSERT_TRUE(nor_init(&s_nor, &config));
    nor_device(&s_nor, &s_dev);
    (void)put_image(&s_flash[SLOT_A], 3000U, 0x01000000U, 1U);

    (void)memset(&boot, 0, sizeof(boot));
    boot.dev = &s_dev;
    boot.meta_addr = 0U;
    boot.meta_sectors = 2U;
    boot.slot[0] = (boot_region_t){.addr = SLOT_A, .size = SLOT_SIZE};
    boot.slot[1] = (boot_region_t){.addr = SLOT_B, .size = SLOT_SIZE};
    TEST_ASSERT_TRUE(bootimg_init(&s_validator, IMAGE_TARGET_HOST, &boot.validator));
    TEST_ASSERT_EQUAL(BOOT_OK, boot_init(&s_boot, &boot));
    if (adopt) {
        boot_selection_t sel;

        TEST_ASSERT_EQUAL(BOOT_OK, boot_select(&s_boot, &sel));
        TEST_ASSERT_EQUAL(BOOT_EVENT_ADOPT, sel.event);
    }

    (void)memset(&s_real, 0, sizeof(s_real));
    s_real.boot = &s_boot;
    s_real.trial_slot = BOOT_NO_SLOT;

    const uds_backend_t backend = real_backend();

    harness_init_with(&backend);
    enter_session(UDS_SESSION_PROGRAMMING);
}

/* Poll every `every_us` until the pending request has its final response, which is then in
 * g_rsp; the number of responsePending on the way. */
static uint32_t finish_pending(uint64_t every_us, uint32_t max_polls)
{
    uint32_t pendings = 0U;

    for (uint32_t i = 0U; uds_busy(&g_srv) && (i < max_polls); i++) {
        (void)poll_at(g_now + every_us);
        if ((g_out.len == 3U) && (g_rsp[0] == 0x7F) && (g_rsp[2] == UDS_NRC_RESPONSE_PENDING)) {
            pendings++;
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(uds_busy(&g_srv), "the request did not finish");
    return pendings;
}

/* eraseMemory through the real backend, one erase step every 45 ms (the modelled part's typical
 * sector erase); its final response is in g_rsp. */
static uint32_t real_erase_request(uint32_t address, uint32_t size)
{
    uint8_t req[13];
    uint32_t pendings = 0U;

    erase_req(req, address, size);
    if (send(req, LEN(req)) == 0U) {
        pendings = finish_pending(45U * MS, 300U);
    }
    return pendings;
}

/* The rest of a download after the erase: RequestDownload, the blocks of 1024 bytes,
 * RequestTransferExit and the check, one finish step per millisecond. */
static void real_transfer_and_check(uint32_t size)
{
    uint8_t counter = 1U;

    download_expect(SLOT_B, size, UDS_NRC_NONE);
    for (uint32_t offset = 0U; offset < size; offset += UDS_BLOCK_DATA_MAX) {
        const uint32_t n =
            ((size - offset) < UDS_BLOCK_DATA_MAX) ? (size - offset) : UDS_BLOCK_DATA_MAX;

        block_expect(counter, &s_image[offset], n, UDS_NRC_NONE);
        counter++;
    }
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));
    TEST_ASSERT_EQUAL_UINT32(0U, send(k_check, LEN(k_check)));
    (void)finish_pending(1U * MS, 1000U);
}

/* A whole download through the server commits the image as the trial, which the bootloader's
 * select boots as attempt 1, and the confirm routine confirms it. */
static void test_real_download_commits_a_trial_that_the_bootloader_boots(void)
{
    const uint32_t payload = 10000U;
    const uint32_t size = IMAGE_HEADER_SIZE + payload;
    const image_header_t header = put_image(s_image, payload, 0x01010000U, 9U);
    static const uint8_t confirm[] = {0x31, 0x01, 0xF0, 0x00};
    static const uint8_t confirm_ok[] = {0x71, 0x01, 0xF0, 0x00, 0x00, 0x00};
    boot_description_t d;
    boot_selection_t sel;
    uint8_t id[BOOT_IMAGE_ID_SIZE];

    real_bench(true);

    /* The erase: three sectors, one per poll 45 ms apart: one responsePending, at 45 ms. */
    TEST_ASSERT_EQUAL_UINT32(1U, real_erase_request(SLOT_B, size));
    expect_rsp(k_erase_ok, LEN(k_erase_ok));
    TEST_ASSERT_EQUAL_UINT32(3U, s_real.erase_steps);
    TEST_ASSERT_EQUAL_UINT64(135U * MS, stats().download.erase_us);
    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    TEST_ASSERT_EQUAL(BOOT_SLOT_WRITING, d.slot[1].state);

    /* The transfer, with an early exit, repeated blocks, and the slot writer's own refusal of
     * a write at an offset it did not expect never reached: each repeat is not written. */
    download_expect(SLOT_B, size, UDS_NRC_NONE);
    block_expect(1U, &s_image[0], 1024U, UDS_NRC_NONE);
    block_expect(1U, &s_image[0], 1024U, UDS_NRC_NONE);
    uint8_t counter = 2U;
    uint32_t offset = 1024U;

    for (; (offset + 1024U) < size; offset += 1024U) {
        block_expect(counter, &s_image[offset], 1024U, UDS_NRC_NONE);
        counter++;
    }
    send_expect_nrc(k_exit, LEN(k_exit), UDS_NRC_REQUEST_SEQUENCE_ERROR);
    block_expect(counter, &s_image[offset], size - offset, UDS_NRC_NONE);
    block_expect(counter, &s_image[offset], size - offset, UDS_NRC_NONE);
    TEST_ASSERT_EQUAL_UINT32(11U, s_real.writes);
    TEST_ASSERT_EQUAL_UINT8(11U, counter);
    send_expect(k_exit, LEN(k_exit), k_exit_ok, LEN(k_exit_ok));

    /* The check: the validation, 8 + ceil(10000 / 64) = 165 validator steps (lib/bootimg),
     * then the COMMIT record: 166 finish steps, one per poll 1 ms apart, one responsePending. */
    const uint64_t t = g_now;

    TEST_ASSERT_EQUAL_UINT32(0U, send(k_check, LEN(k_check)));
    TEST_ASSERT_EQUAL_UINT32(1U, finish_pending(1U * MS, 1000U));
    expect_rsp(k_check_ok, LEN(k_check_ok));
    TEST_ASSERT_EQUAL_UINT32(1U + 166U, s_real.finish_steps);
    TEST_ASSERT_EQUAL_UINT64(166U * MS, g_now - t);
    TEST_ASSERT_EQUAL_UINT64(166U * MS, stats().download.check_us);

    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    TEST_ASSERT_EQUAL(BOOT_SLOT_CONFIRMED, d.slot[0].state);
    TEST_ASSERT_EQUAL(BOOT_SLOT_TRIAL, d.slot[1].state);
    TEST_ASSERT_EQUAL_UINT8(0U, d.attempts_used);
    TEST_ASSERT_TRUE(bootimg_image_id(&header, id));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(id, d.slot[1].id, sizeof(id));
    TEST_ASSERT_EQUAL_HEX32(0x01010000U, d.slot[1].version);

    /* The bootloader boots it as attempt 1 ... */
    TEST_ASSERT_EQUAL(BOOT_OK, boot_select(&s_boot, &sel));
    TEST_ASSERT_TRUE(sel.trial);
    TEST_ASSERT_EQUAL_UINT8(1U, sel.slot);
    TEST_ASSERT_EQUAL_UINT8(1U, sel.attempt);

    /* ... and, as that trial, the tester's routine confirms it, once. */
    s_real.trial_slot = 1U;
    enter_session(UDS_SESSION_DEFAULT);
    enter_session(UDS_SESSION_EXTENDED);
    send_expect(confirm, LEN(confirm), confirm_ok, LEN(confirm_ok));
    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    TEST_ASSERT_EQUAL(BOOT_SLOT_PREVIOUS, d.slot[0].state);
    TEST_ASSERT_EQUAL(BOOT_SLOT_CONFIRMED, d.slot[1].state);
    TEST_ASSERT_EQUAL(BOOT_EVENT_CONFIRM, d.event);
    send_expect_nrc(confirm, LEN(confirm), UDS_NRC_CONDITIONS_NOT_CORRECT);
}

/* lib/boot's refusals of the erase, each through the code that makes it. */
static void test_real_erase_refusals_map_to_their_nrcs(void)
{
    static const struct {
        uint32_t address;
        uint32_t size;
        uint8_t nrc;
    } cases[] = {
        {SLOT_B + HOST_SECTOR, 4096U, UDS_NRC_REQUEST_OUT_OF_RANGE}, /* no slot starts there */
        {0U, 4096U, UDS_NRC_REQUEST_OUT_OF_RANGE},                   /* the metadata */
        {HOST_SIZE, 4096U, UDS_NRC_REQUEST_OUT_OF_RANGE},            /* beyond the device */
        {SLOT_A, 4096U, UDS_NRC_CONDITIONS_NOT_CORRECT},             /* the running slot */
        {SLOT_B, 0U, UDS_NRC_REQUEST_OUT_OF_RANGE},                  /* a size of 0 */
        {SLOT_B, SLOT_SIZE + 1U, UDS_NRC_REQUEST_OUT_OF_RANGE},      /* beyond the slot */
    };
    static const uint8_t results[] = {0x31, 0x03, 0xFF, 0x00};
    boot_description_t d;

    real_bench(true);
    for (uint32_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        const uint8_t failed[] = {0x71, 0x03, 0xFF, 0x00, 0x01, cases[i].nrc};

        TEST_ASSERT_EQUAL_UINT32(0U, real_erase_request(cases[i].address, cases[i].size));
        expect_nrc(0x31, cases[i].nrc);
        send_expect(results, LEN(results), failed, LEN(failed));
        download_expect(cases[i].address, cases[i].size, UDS_NRC_DOWNLOAD_NOT_ACCEPTED);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, s_real.erase_steps);
    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    TEST_ASSERT_EQUAL(BOOT_SLOT_EMPTY, d.slot[1].state);

    /* The whole slot is a size the writer takes: 256 erase steps, 45 ms apart. */
    TEST_ASSERT_EQUAL_UINT32(6U, real_erase_request(SLOT_B, SLOT_SIZE));
    expect_rsp(k_erase_ok, LEN(k_erase_ok));
    TEST_ASSERT_EQUAL_UINT32(256U, s_real.erase_steps);
}

/* While a trial is pending no download can begin: the fallback would be overwritten. */
static void test_real_a_download_is_refused_while_a_trial_is_pending(void)
{
    const uint32_t size = IMAGE_HEADER_SIZE + 2000U;
    boot_description_t d;

    real_bench(true);
    (void)put_image(s_image, 2000U, 0x01020000U, 3U);
    (void)real_erase_request(SLOT_B, size);
    expect_rsp(k_erase_ok, LEN(k_erase_ok));
    real_transfer_and_check(size);
    expect_rsp(k_check_ok, LEN(k_check_ok));

    TEST_ASSERT_EQUAL_UINT32(0U, real_erase_request(SLOT_B, size));
    expect_nrc(0x31, UDS_NRC_CONDITIONS_NOT_CORRECT);
    TEST_ASSERT_EQUAL_UINT32(0U, real_erase_request(SLOT_A, size));
    expect_nrc(0x31, UDS_NRC_CONDITIONS_NOT_CORRECT);
    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    TEST_ASSERT_EQUAL(BOOT_SLOT_TRIAL, d.slot[1].state);
}

/* In the factory state (no record yet) lib/boot refuses to begin. */
static void test_real_the_factory_state_refuses_the_erase(void)
{
    real_bench(false);
    TEST_ASSERT_EQUAL_UINT32(0U, real_erase_request(SLOT_B, 4096U));
    expect_nrc(0x31, UDS_NRC_CONDITIONS_NOT_CORRECT);
}

/* An image damaged in transit fails the check: generalProgrammingFailure, the slot stays
 * WRITING, the bootloader starts the confirmed image. */
static void test_real_a_damaged_image_fails_the_check(void)
{
    static const uint8_t results[] = {0x31, 0x03, 0xFF, 0x01};
    static const uint8_t failed[] = {0x71, 0x03, 0xFF, 0x01, 0x01, 0x72};
    const uint32_t size = IMAGE_HEADER_SIZE + 5000U;
    boot_description_t d;
    boot_selection_t sel;

    real_bench(true);
    (void)put_image(s_image, 5000U, 0x01020000U, 4U);
    s_image[3000] ^= 0x01U;
    (void)real_erase_request(SLOT_B, size);
    real_transfer_and_check(size);
    expect_nrc(0x31, UDS_NRC_PROGRAMMING_FAILURE);
    TEST_ASSERT_EQUAL(UDS_DL_IDLE, stats().download.state);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.failed);
    send_expect(results, LEN(results), failed, LEN(failed));
    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    TEST_ASSERT_EQUAL(BOOT_SLOT_WRITING, d.slot[1].state);
    TEST_ASSERT_EQUAL(BOOT_OK, boot_select(&s_boot, &sel));
    TEST_ASSERT_FALSE(sel.trial);
    TEST_ASSERT_EQUAL_UINT8(0U, sel.slot);

    /* The next download, of the intact image, commits. */
    s_image[3000] ^= 0x01U;
    (void)real_erase_request(SLOT_B, size);
    expect_rsp(k_erase_ok, LEN(k_erase_ok));
    real_transfer_and_check(size);
    expect_rsp(k_check_ok, LEN(k_check_ok));
}

/* A loss of supply in a write: generalProgrammingFailure and the end of the download; with the
 * supply back, the next download commits. */
static void test_real_a_power_cut_in_a_write_ends_the_download(void)
{
    const uint32_t size = IMAGE_HEADER_SIZE + 5000U;
    boot_description_t d;

    real_bench(true);
    (void)put_image(s_image, 5000U, 0x01020000U, 5U);
    (void)real_erase_request(SLOT_B, size);
    download_expect(SLOT_B, size, UDS_NRC_NONE);
    block_expect(1U, &s_image[0], 1024U, UDS_NRC_NONE);
    nor_arm_cut(&s_nor, 1U, NOR_CUT_SEEDED_POSITION, 7U);
    block_expect(2U, &s_image[1024], 1024U, UDS_NRC_PROGRAMMING_FAILURE);
    TEST_ASSERT_EQUAL(UDS_DL_IDLE, stats().download.state);
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.failed);
    block_expect(3U, &s_image[2048], 1024U, UDS_NRC_REQUEST_SEQUENCE_ERROR);

    nor_power_on(&s_nor);
    (void)real_erase_request(SLOT_B, size);
    expect_rsp(k_erase_ok, LEN(k_erase_ok));
    real_transfer_and_check(size);
    expect_rsp(k_check_ok, LEN(k_check_ok));
    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    TEST_ASSERT_EQUAL(BOOT_SLOT_TRIAL, d.slot[1].state);
}

/* S3server in the middle of a transfer: the slot stays WRITING, the bootloader starts the
 * confirmed image, and the next download commits. */
static void test_real_s3_leaves_the_slot_writing(void)
{
    const uint32_t size = IMAGE_HEADER_SIZE + 5000U;
    boot_description_t d;
    boot_selection_t sel;

    real_bench(true);
    (void)put_image(s_image, 5000U, 0x01020000U, 6U);
    (void)real_erase_request(SLOT_B, size);
    download_expect(SLOT_B, size, UDS_NRC_NONE);
    block_expect(1U, &s_image[0], 1024U, UDS_NRC_NONE);
    block_expect(2U, &s_image[1024], 1024U, UDS_NRC_NONE);
    (void)poll_at(g_now + (5000U * MS));
    TEST_ASSERT_EQUAL_UINT8(UDS_SESSION_DEFAULT, uds_session(&g_srv));
    TEST_ASSERT_EQUAL_UINT32(1U, stats().download.aborted);
    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    TEST_ASSERT_EQUAL(BOOT_SLOT_WRITING, d.slot[1].state);
    TEST_ASSERT_EQUAL(BOOT_OK, boot_select(&s_boot, &sel));
    TEST_ASSERT_FALSE(sel.trial);
    TEST_ASSERT_EQUAL_UINT8(0U, sel.slot);

    enter_session(UDS_SESSION_PROGRAMMING);
    (void)real_erase_request(SLOT_B, size);
    expect_rsp(k_erase_ok, LEN(k_erase_ok));
    real_transfer_and_check(size);
    expect_rsp(k_check_ok, LEN(k_check_ok));
    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    TEST_ASSERT_EQUAL(BOOT_SLOT_TRIAL, d.slot[1].state);
}

/* The confirm without a running trial, and of a trial that has never run, is refused by
 * lib/boot (BOOT_E_STATE): conditionsNotCorrect. */
static void test_real_a_confirm_without_a_running_trial_is_refused(void)
{
    static const uint8_t confirm[] = {0x31, 0x01, 0xF0, 0x00};
    const uint32_t size = IMAGE_HEADER_SIZE + 1000U;
    boot_description_t d;

    real_bench(true);
    (void)put_image(s_image, 1000U, 0x01020000U, 7U);
    (void)real_erase_request(SLOT_B, size);
    real_transfer_and_check(size);
    expect_rsp(k_check_ok, LEN(k_check_ok));

    enter_session(UDS_SESSION_DEFAULT);
    enter_session(UDS_SESSION_EXTENDED);
    send_expect_nrc(confirm, LEN(confirm), UDS_NRC_CONDITIONS_NOT_CORRECT); /* no trial runs */
    s_real.trial_slot = 1U;
    send_expect_nrc(confirm, LEN(confirm), UDS_NRC_CONDITIONS_NOT_CORRECT); /* 0 attempts */
    s_real.trial_slot = 0U;
    send_expect_nrc(confirm, LEN(confirm), UDS_NRC_CONDITIONS_NOT_CORRECT); /* not TRIAL */
    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    TEST_ASSERT_EQUAL(BOOT_SLOT_TRIAL, d.slot[1].state);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_whole_download_on_the_fake_backend);
    RUN_TEST(test_request_download_checks_in_order);
    RUN_TEST(test_transfer_data_checks_in_order);
    RUN_TEST(test_after_the_last_byte_only_a_repeat_is_taken);
    RUN_TEST(test_the_block_counter_wraps_from_ff_to_00);
    RUN_TEST(test_a_repeated_block_is_answered_without_writing);
    RUN_TEST(test_a_wrong_counter_keeps_the_download);
    RUN_TEST(test_too_many_bytes_end_the_download);
    RUN_TEST(test_exit_before_the_last_byte_is_refused_and_the_download_goes_on);
    RUN_TEST(test_the_check_comes_once_after_the_exit);
    RUN_TEST(test_erase_checks_in_order);
    RUN_TEST(test_erase_refusals_map_to_their_nrcs);
    RUN_TEST(test_write_refusals_end_the_download);
    RUN_TEST(test_exit_refusals_end_the_download);
    RUN_TEST(test_check_refusals_end_the_download);
    RUN_TEST(test_s3_ends_a_download);
    RUN_TEST(test_a_session_control_ends_a_download);
    RUN_TEST(test_an_erase_after_the_exit_begins_again);
    RUN_TEST(test_a_pending_write_keeps_its_data);
    RUN_TEST(test_a_pending_exit);
    RUN_TEST(test_response_pending_during_a_long_erase);
    RUN_TEST(test_an_overlong_request_is_answered_without_being_kept);
    RUN_TEST(test_real_download_commits_a_trial_that_the_bootloader_boots);
    RUN_TEST(test_real_erase_refusals_map_to_their_nrcs);
    RUN_TEST(test_real_a_download_is_refused_while_a_trial_is_pending);
    RUN_TEST(test_real_the_factory_state_refuses_the_erase);
    RUN_TEST(test_real_a_damaged_image_fails_the_check);
    RUN_TEST(test_real_a_power_cut_in_a_write_ends_the_download);
    RUN_TEST(test_real_s3_leaves_the_slot_writing);
    RUN_TEST(test_real_a_confirm_without_a_running_trial_is_refused);
    return UNITY_END();
}
