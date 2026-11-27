/* SPDX-License-Identifier: MIT */
/*
 * app/flash_job.c: the flash task's jobs (flash_job.h) on lib/boot's slot writer over lib/flash's
 * NOR model, with the host's layout and lib/bootimg as the validator: what each job calls, its
 * result and its steps, the alive indication after every call of lib/boot, the refusals that
 * come from lib/boot itself, a run without a flash, and the confirm of a trial by the routine and
 * by the trial itself, the first one winning.
 */
#include <string.h>

#include "boot/boot.h"
#include "bootimg/bootimg.h"
#include "flash/nor.h"
#include "flash_job.h"
#include "image/image.h"
#include "sha256/sha256.h"
#include "unity.h"

#define HOST_SIZE   0x202000U
#define HOST_SECTOR 4096U
#define SLOT_A      0x2000U
#define SLOT_B      0x102000U
#define SLOT_SIZE   0x100000U

static uint8_t s_flash[HOST_SIZE];
static uint32_t s_erase_counts[HOST_SIZE / HOST_SECTOR];
static nor_t s_nor;
static flash_dev_t s_dev;
static bootimg_t s_validator;
static boot_t s_boot;
static flash_jobs_t s_jobs;
static uint8_t s_image[IMAGE_HEADER_SIZE + 20000U];
static uint32_t s_alive;

void setUp(void)
{
    s_alive = 0U;
}

void tearDown(void)
{
}

static void count_alive(void *ctx)
{
    TEST_ASSERT_EQUAL_PTR(&s_alive, ctx);
    s_alive++;
}

/* An image of `payload_size` bytes (a pattern of `seed`) and `version` into `mem`. */
static void put_image(uint8_t *mem, uint32_t payload_size, uint32_t version, uint8_t seed)
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
}

/* The host's flash with a factory image in slot A, adopted by the bootloader's first select. */
static void bench(void)
{
    const nor_config_t config = {.geometry = {.size = HOST_SIZE,
                                              .sector_size = HOST_SECTOR,
                                              .page_size = 256U,
                                              .unit_size = 8U},
                                 .timing = {.page_program_ns = 0U, .sector_erase_ns = 0U},
                                 .memory = s_flash,
                                 .erase_counts = s_erase_counts};
    boot_config_t boot;
    boot_selection_t sel;

    (void)memset(s_flash, 0xFF, sizeof(s_flash));
    TEST_ASSERT_TRUE(nor_init(&s_nor, &config));
    nor_device(&s_nor, &s_dev);
    put_image(&s_flash[SLOT_A], 3000U, 0x01000000U, 1U);
    (void)memset(&boot, 0, sizeof(boot));
    boot.dev = &s_dev;
    boot.meta_addr = 0U;
    boot.meta_sectors = 2U;
    boot.slot[0] = (boot_region_t){.addr = SLOT_A, .size = SLOT_SIZE};
    boot.slot[1] = (boot_region_t){.addr = SLOT_B, .size = SLOT_SIZE};
    TEST_ASSERT_TRUE(bootimg_init(&s_validator, IMAGE_TARGET_HOST, &boot.validator));
    TEST_ASSERT_EQUAL(BOOT_OK, boot_init(&s_boot, &boot));
    TEST_ASSERT_EQUAL(BOOT_OK, boot_select(&s_boot, &sel));
    TEST_ASSERT_EQUAL(BOOT_EVENT_ADOPT, sel.event);
    TEST_ASSERT_TRUE(flash_jobs_init(&s_jobs, &s_boot, NULL));
}

static flash_job_result_t run(flash_job_kind_t kind, uint32_t address, uint32_t size,
                              uint32_t offset, const uint8_t *data, uint32_t len)
{
    const flash_job_t job = {
        .kind = kind, .address = address, .size = size, .offset = offset, .data = data, .len = len};
    flash_job_result_t result = {.status = BOOT_E_FLASH, .steps = 12345U};

    s_alive = 0U;
    flash_job_run(&s_jobs, &job, count_alive, &s_alive, &result);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(result.steps, s_alive,
                                     "an alive indication after every call of lib/boot");
    return result;
}

static boot_description_t describe(void)
{
    boot_description_t d;

    TEST_ASSERT_EQUAL(BOOT_OK, boot_describe(&s_boot, &d));
    return d;
}

/* The whole download as jobs, an image of `payload` bytes in blocks of 1024: the erase, the
 * writes, the exit, the check. */
static void download(uint32_t payload)
{
    const uint32_t size = IMAGE_HEADER_SIZE + payload;
    const uint32_t sectors = (size + HOST_SECTOR - 1U) / HOST_SECTOR;
    flash_job_result_t r;

    put_image(s_image, payload, 0x01010000U, 7U);
    r = run(FLASH_JOB_ERASE, SLOT_B, size, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_OK, r.status);
    TEST_ASSERT_EQUAL_UINT32(1U + sectors, r.steps); /* the begin and one step per sector */
    TEST_ASSERT_EQUAL(BOOT_SLOT_WRITING, describe().slot[1].state);

    for (uint32_t offset = 0U; offset < size; offset += 1024U) {
        const uint32_t len = ((size - offset) < 1024U) ? (size - offset) : 1024U;

        r = run(FLASH_JOB_WRITE, 0U, 0U, offset, &s_image[offset], len);
        TEST_ASSERT_EQUAL(BOOT_OK, r.status);
        TEST_ASSERT_EQUAL_UINT32(1U, r.steps);
    }

    r = run(FLASH_JOB_EXIT, 0U, 0U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_BUSY, r.status); /* the first finish step: the validation starts */
    TEST_ASSERT_EQUAL_UINT32(1U, r.steps);
    r = run(FLASH_JOB_CHECK, 0U, 0U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_OK, r.status);
    /* lib/bootimg: 8 + ceil(payload / 64) validator steps; then the COMMIT record. */
    TEST_ASSERT_EQUAL_UINT32(8U + ((payload + 63U) / 64U) + 1U, r.steps);
}

/* The download as jobs commits a trial in slot B, which the bootloader then starts. */
static void test_the_jobs_of_a_download_commit_a_trial(void)
{
    boot_selection_t sel;

    bench();
    download(10000U);
    TEST_ASSERT_EQUAL(BOOT_SLOT_TRIAL, describe().slot[1].state);
    TEST_ASSERT_EQUAL(BOOT_OK, boot_select(&s_boot, &sel));
    TEST_ASSERT_EQUAL_UINT8(1U, sel.slot);
    TEST_ASSERT_TRUE(sel.trial);
    TEST_ASSERT_EQUAL_UINT8(1U, sel.attempt);
}

/* lib/boot's refusals come through as the job's result: no slot at the address, the running
 * slot, a size beyond the slot, a trial pending; a write out of order. */
static void test_lib_boot_refuses_through_the_jobs(void)
{
    flash_job_result_t r;
    const uint8_t block[16] = {0};

    bench();
    r = run(FLASH_JOB_ERASE, SLOT_B + HOST_SECTOR, 4096U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_E_ARG, r.status);
    TEST_ASSERT_EQUAL_UINT32(0U, r.steps);
    r = run(FLASH_JOB_ERASE, SLOT_A, 4096U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_E_STATE, r.status);
    TEST_ASSERT_EQUAL_UINT32(1U, r.steps);
    r = run(FLASH_JOB_ERASE, SLOT_B, SLOT_SIZE + 1U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_E_SIZE, r.status);
    /* Nothing begun (every begin above was refused): an unused writer (boot.h). */
    r = run(FLASH_JOB_WRITE, 0U, 0U, 0U, block, sizeof(block));
    TEST_ASSERT_EQUAL(BOOT_E_ARG, r.status);
    r = run(FLASH_JOB_CHECK, 0U, 0U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_E_ARG, r.status);
    TEST_ASSERT_EQUAL_UINT32(1U, r.steps);

    download(3000U); /* a trial is pending now */
    r = run(FLASH_JOB_ERASE, SLOT_B, 4096U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_E_STATE, r.status);
    TEST_ASSERT_EQUAL(BOOT_SLOT_TRIAL, describe().slot[1].state);
}

/* Without a flash every job ends with BOOT_E_STATE and calls nothing; a kind that is no kind
 * with BOOT_E_ARG; NULL arguments do nothing. */
static void test_without_a_flash_every_job_is_refused(void)
{
    static const flash_job_kind_t kinds[] = {FLASH_JOB_ERASE, FLASH_JOB_WRITE, FLASH_JOB_EXIT,
                                             FLASH_JOB_CHECK, FLASH_JOB_CONFIRM};
    const plat_boot_handoff_t trial = {.slot = 1U,
                                       .mode = PLAT_BOOT_TRIAL,
                                       .attempt = 1U,
                                       .image_version = 0U,
                                       .event = PLAT_BOOT_EVENT_NONE,
                                       .event_slot = PLAT_HANDOFF_NO_SLOT,
                                       .select_write_error = 0U};
    flash_job_result_t r;
    bool written = true;

    TEST_ASSERT_TRUE(flash_jobs_init(&s_jobs, NULL, &trial));

    for (size_t i = 0U; i < (sizeof(kinds) / sizeof(kinds[0])); i++) {
        r = run(kinds[i], SLOT_B, 4096U, 0U, NULL, 0U);
        TEST_ASSERT_EQUAL(BOOT_E_STATE, r.status);
        TEST_ASSERT_EQUAL_UINT32(0U, r.steps);
    }

    r = run(FLASH_JOB_NONE, 0U, 0U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_E_ARG, r.status);
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): no kind, on purpose */
    r = run((flash_job_kind_t)FLASH_JOB_KINDS, 0U, 0U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_E_ARG, r.status);
    TEST_ASSERT_EQUAL(BOOT_E_ARG, flash_jobs_self_confirm(&s_jobs, &written));
    TEST_ASSERT_FALSE(written);
    TEST_ASSERT_EQUAL(BOOT_E_ARG, flash_jobs_self_confirm(NULL, &written));
    TEST_ASSERT_EQUAL(BOOT_E_ARG, flash_jobs_self_confirm(&s_jobs, NULL));
    TEST_ASSERT_FALSE(flash_jobs_init(NULL, NULL, NULL));

    r.status = BOOT_OK;
    flash_job_run(&s_jobs, NULL, count_alive, &s_alive, &r);
    TEST_ASSERT_EQUAL(BOOT_OK, r.status); /* untouched */
    flash_job_run(&s_jobs, NULL, NULL, NULL, NULL);
}

/* A trial in slot B started as attempt 1, its hand-off, and the jobs set up for it. */
static void start_trial(void)
{
    boot_selection_t sel;
    plat_boot_handoff_t handoff;

    bench();
    download(5000U);
    TEST_ASSERT_EQUAL(BOOT_OK, boot_select(&s_boot, &sel));
    handoff = (plat_boot_handoff_t){.slot = sel.slot,
                                    .mode = PLAT_BOOT_TRIAL,
                                    .attempt = sel.attempt,
                                    .image_version = sel.version,
                                    .event = PLAT_BOOT_EVENT_NONE,
                                    .event_slot = PLAT_HANDOFF_NO_SLOT,
                                    .select_write_error = 0U};
    TEST_ASSERT_TRUE(flash_jobs_init(&s_jobs, &s_boot, &handoff));
}

/* The routine first: it confirms the trial; the trial's own confirm after it writes nothing and
 * answers BOOT_OK; a second routine is refused by lib/boot. */
static void test_the_routine_first_then_the_self_confirm_writes_nothing(void)
{
    flash_job_result_t r;
    bool written = true;
    uint32_t seq;

    start_trial();
    r = run(FLASH_JOB_CONFIRM, 0U, 0U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_OK, r.status);
    TEST_ASSERT_EQUAL_UINT32(1U, r.steps);
    TEST_ASSERT_TRUE(s_jobs.confirmed);
    TEST_ASSERT_TRUE(s_jobs.by_routine);
    TEST_ASSERT_EQUAL_UINT8(1U, describe().active_slot);
    seq = describe().seq;

    TEST_ASSERT_EQUAL(BOOT_OK, flash_jobs_self_confirm(&s_jobs, &written));
    TEST_ASSERT_FALSE(written);
    TEST_ASSERT_EQUAL_UINT32(1U, s_jobs.self_skipped);
    TEST_ASSERT_EQUAL_UINT32(seq, describe().seq); /* no record written */

    r = run(FLASH_JOB_CONFIRM, 0U, 0U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_E_STATE, r.status);
    TEST_ASSERT_EQUAL_UINT32(seq, describe().seq);
}

/* The trial's own confirm first: the routine after it is refused by lib/boot (BOOT_E_STATE, the
 * slot is no longer TRIAL), and nothing more is written. */
static void test_the_self_confirm_first_then_the_routine_is_refused(void)
{
    flash_job_result_t r;
    bool written = false;
    uint32_t seq;

    start_trial();
    TEST_ASSERT_EQUAL(BOOT_OK, flash_jobs_self_confirm(&s_jobs, &written));
    TEST_ASSERT_TRUE(written);
    TEST_ASSERT_TRUE(s_jobs.confirmed);
    TEST_ASSERT_FALSE(s_jobs.by_routine);
    seq = describe().seq;
    TEST_ASSERT_EQUAL_UINT8(1U, describe().active_slot);

    r = run(FLASH_JOB_CONFIRM, 0U, 0U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_E_STATE, r.status);
    TEST_ASSERT_FALSE(s_jobs.by_routine);
    TEST_ASSERT_EQUAL_UINT32(seq, describe().seq);
}

/* A node that runs the confirmed image has no trial: the routine is refused without a call of
 * lib/boot, and so is the self-confirm. */
static void test_without_a_trial_the_confirm_is_refused(void)
{
    const plat_boot_handoff_t confirmed = {.slot = 0U,
                                           .mode = PLAT_BOOT_CONFIRMED,
                                           .attempt = 0U,
                                           .image_version = 0U,
                                           .event = PLAT_BOOT_EVENT_NONE,
                                           .event_slot = PLAT_HANDOFF_NO_SLOT,
                                           .select_write_error = 0U};
    flash_job_result_t r;
    bool written = true;

    bench();
    TEST_ASSERT_TRUE(flash_jobs_init(&s_jobs, &s_boot, &confirmed));
    r = run(FLASH_JOB_CONFIRM, 0U, 0U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_E_STATE, r.status);
    TEST_ASSERT_EQUAL_UINT32(0U, r.steps);
    TEST_ASSERT_EQUAL(BOOT_E_STATE, flash_jobs_self_confirm(&s_jobs, &written));
    TEST_ASSERT_FALSE(written);
    TEST_ASSERT_TRUE(flash_jobs_init(&s_jobs, &s_boot, NULL)); /* no hand-off at all */
    r = run(FLASH_JOB_CONFIRM, 0U, 0U, 0U, NULL, 0U);
    TEST_ASSERT_EQUAL(BOOT_E_STATE, r.status);
}

/* A job without an alive function still runs. */
static void test_a_job_runs_without_an_alive_function(void)
{
    const flash_job_t job = {.kind = FLASH_JOB_ERASE,
                             .address = SLOT_B,
                             .size = 8192U,
                             .offset = 0U,
                             .data = NULL,
                             .len = 0U};
    flash_job_result_t r;

    bench();
    flash_job_run(&s_jobs, &job, NULL, NULL, &r);
    TEST_ASSERT_EQUAL(BOOT_OK, r.status);
    TEST_ASSERT_EQUAL_UINT32(3U, r.steps);
}

static void test_the_kinds_have_names(void)
{
    TEST_ASSERT_EQUAL_STRING("none", flash_job_kind_name(FLASH_JOB_NONE));
    TEST_ASSERT_EQUAL_STRING("erase", flash_job_kind_name(FLASH_JOB_ERASE));
    TEST_ASSERT_EQUAL_STRING("write", flash_job_kind_name(FLASH_JOB_WRITE));
    TEST_ASSERT_EQUAL_STRING("exit", flash_job_kind_name(FLASH_JOB_EXIT));
    TEST_ASSERT_EQUAL_STRING("check", flash_job_kind_name(FLASH_JOB_CHECK));
    TEST_ASSERT_EQUAL_STRING("confirm", flash_job_kind_name(FLASH_JOB_CONFIRM));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): no kind, on purpose */
    TEST_ASSERT_EQUAL_STRING("?", flash_job_kind_name((flash_job_kind_t)FLASH_JOB_KINDS));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_jobs_of_a_download_commit_a_trial);
    RUN_TEST(test_lib_boot_refuses_through_the_jobs);
    RUN_TEST(test_without_a_flash_every_job_is_refused);
    RUN_TEST(test_the_routine_first_then_the_self_confirm_writes_nothing);
    RUN_TEST(test_the_self_confirm_first_then_the_routine_is_refused);
    RUN_TEST(test_without_a_trial_the_confirm_is_refused);
    RUN_TEST(test_a_job_runs_without_an_alive_function);
    RUN_TEST(test_the_kinds_have_names);
    return UNITY_END();
}
