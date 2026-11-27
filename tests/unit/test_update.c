/* SPDX-License-Identifier: MIT */
/*
 * app/update.c: what the hand-off means for the node, and the confirm of a trial (update.h).
 * Every test drives the monitor as the health task does, one period at a time, with 10 periods
 * a second (APP_HEALTH_PERIOD_MS 100), so that the window is UPDATE_CONFIRM_AFTER_S * 10
 * periods. The count of acknowledged frames goes on from period to period as the transmit
 * queue's does: run() adds a period's frames to it, run_acked() as many as a test says.
 */
#include <string.h>

#include "unity.h"
#include "update.h"

#define PER_SECOND 10U
#define WINDOW     (UPDATE_CONFIRM_AFTER_S * PER_SECOND)

/* Frames acknowledged in a period of run(): what the DBC's cycle times give a node with its
 * telemetry on (32.7 frames in 100 ms, rounded up); the rule asks for one. */
#define FRAMES_PER_PERIOD 33U

static update_monitor_t s_monitor;

/* The count of acknowledged frames as the last period read it. It never goes back, also not
 * when a test sets the monitor up again (update_init() starts its own copy from 0). */
static uint64_t s_tx_acked;

void setUp(void)
{
    (void)memset(&s_monitor, 0xA5, sizeof(s_monitor));
    s_tx_acked = 0U;
}

void tearDown(void)
{
}

static plat_boot_handoff_t handoff(plat_boot_mode_t mode, uint8_t attempt, plat_boot_event_t event)
{
    plat_boot_handoff_t h;

    (void)memset(&h, 0, sizeof(h));
    h.slot = 1U;
    h.mode = mode;
    h.attempt = attempt;
    h.image_version = 0x01020003U;
    h.event = event;
    h.event_slot = (event == PLAT_BOOT_EVENT_NONE) ? PLAT_HANDOFF_NO_SLOT : 0U;
    return h;
}

/* A period in good health, but for the count of acknowledged frames, which the run fills in. */
static update_health_t healthy(void)
{
    const update_health_t h = {.supervision = WDGM_GLOBAL_STATUS_OK,
                               .can_state = CAN_FC_ERROR_ACTIVE,
                               .bus_off_count = 0U,
                               .tx_acked = 0U};

    return h;
}

/* `count` periods of `health`, each with `acked` frames acknowledged since the period before
 * (0: none); returns how many of them asked for the confirm. */
static uint32_t run_acked(const update_health_t *health, uint64_t acked, uint32_t count)
{
    uint32_t requests = 0U;

    for (uint32_t i = 0U; i < count; i++) {
        update_health_t period = *health;

        s_tx_acked += acked;
        period.tx_acked = s_tx_acked;

        if (update_health_period(&s_monitor, &period)) {
            requests++;
        }
    }

    return requests;
}

/* `count` periods of `health` with FRAMES_PER_PERIOD frames acknowledged in each. */
static uint32_t run(const update_health_t *health, uint32_t count)
{
    return run_acked(health, FRAMES_PER_PERIOD, count);
}

/* The counts of the periods that failed a condition of good health: the supervision, the
 * controller's state, a bus-off, the acknowledgement. */
static void check_failed(uint32_t unsupervised, uint32_t not_active, uint32_t bus_off,
                         uint32_t unacked)
{
    TEST_ASSERT_EQUAL_UINT32(unsupervised, s_monitor.unsupervised_periods);
    TEST_ASSERT_EQUAL_UINT32(not_active, s_monitor.not_active_periods);
    TEST_ASSERT_EQUAL_UINT32(bus_off, s_monitor.bus_off_periods);
    TEST_ASSERT_EQUAL_UINT32(unacked, s_monitor.unacked_periods);
}

/* No hand-off, or the CONFIRMED image: nothing to confirm, ever, however healthy the node. */
static void test_without_a_trial_nothing_is_confirmed(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t confirmed = handoff(PLAT_BOOT_CONFIRMED, 0U, PLAT_BOOT_EVENT_NONE);

    TEST_ASSERT_TRUE(update_init(&s_monitor, NULL, PER_SECOND, 0U));
    TEST_ASSERT_FALSE(s_monitor.have_handoff);
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_NOT_NEEDED, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 10U * WINDOW));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_NOT_NEEDED, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(0U, update_healthy_seconds(&s_monitor, PER_SECOND));

    TEST_ASSERT_TRUE(update_init(&s_monitor, &confirmed, PER_SECOND, 0U));
    TEST_ASSERT_TRUE(s_monitor.have_handoff);
    TEST_ASSERT_EQUAL_HEX32(0x01020003U, s_monitor.handoff.image_version);
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 10U * WINDOW));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_NOT_NEEDED, s_monitor.state);

    TEST_ASSERT_FALSE(update_init(NULL, &confirmed, PER_SECOND, 0U));
}

/* A trial that stays healthy asks for its confirm exactly once, in the period that completes
 * the window, and not one period earlier. */
static void test_a_healthy_trial_asks_once_when_the_window_is_full(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 1U, PLAT_BOOT_EVENT_NONE);

    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_WAITING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(WINDOW, s_monitor.periods_needed);

    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, WINDOW - 1U));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_WAITING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(UPDATE_CONFIRM_AFTER_S - 1U,
                             update_healthy_seconds(&s_monitor, PER_SECOND));

    TEST_ASSERT_EQUAL_UINT32(1U, run(&good, 1U));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(WINDOW, s_monitor.requested_period);
    TEST_ASSERT_EQUAL_UINT32(UPDATE_CONFIRM_AFTER_S,
                             update_healthy_seconds(&s_monitor, PER_SECOND));
    /* The first period counts the frames acknowledged since the start. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)WINDOW * FRAMES_PER_PERIOD, s_monitor.window_acked);
    TEST_ASSERT_EQUAL_UINT64(FRAMES_PER_PERIOD, s_monitor.fewest_acked);
    TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.window_restarts);
    check_failed(0U, 0U, 0U, 0U);

    /* Never again, whatever follows. */
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 10U * WINDOW));
    TEST_ASSERT_EQUAL_UINT32(WINDOW, s_monitor.periods);
}

/* One frame acknowledged in a period is enough (update.h, WHY EVERY PERIOD NEEDS AN
 * ACKNOWLEDGED FRAME): a node with its telemetry switched off sends fewer frames than one with
 * it on, and frames that wait behind other nodes' traffic are acknowledged later, but however
 * few a period has, the window goes on. The window's counts are those of its own periods. */
static void test_one_acknowledged_frame_a_period_keeps_the_window(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 1U, PLAT_BOOT_EVENT_NONE);
    update_health_t failed = good;

    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, WINDOW / 2U));
    TEST_ASSERT_EQUAL_UINT32(0U, run_acked(&good, 1U, (WINDOW / 2U) - 1U));
    TEST_ASSERT_EQUAL_UINT64(1U, s_monitor.fewest_acked);
    TEST_ASSERT_EQUAL_UINT32(1U, run_acked(&good, 11U, 1U));
    TEST_ASSERT_EQUAL_UINT32(WINDOW, s_monitor.requested_period);
    /* Half the window with FRAMES_PER_PERIOD each, the other half with one each but the last,
     * which had 11. */
    const uint64_t half = WINDOW / 2U;
    const uint64_t window_frames = (half * FRAMES_PER_PERIOD) + half + 10U;

    TEST_ASSERT_EQUAL_UINT64(window_frames, s_monitor.window_acked);
    TEST_ASSERT_EQUAL_UINT64(1U, s_monitor.fewest_acked);
    TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.window_restarts);
    check_failed(0U, 0U, 0U, 0U);

    /* After a restart the window's counts start again with it. */
    failed.supervision = WDGM_GLOBAL_STATUS_FAILED;
    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, run_acked(&good, 1U, 3U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&failed, 1U));
    TEST_ASSERT_EQUAL_UINT64(0U, s_monitor.window_acked);
    TEST_ASSERT_EQUAL_UINT64(0U, s_monitor.fewest_acked);
    TEST_ASSERT_EQUAL_UINT32(1U, run(&good, WINDOW));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)WINDOW * FRAMES_PER_PERIOD, s_monitor.window_acked);
    TEST_ASSERT_EQUAL_UINT64(FRAMES_PER_PERIOD, s_monitor.fewest_acked);
    TEST_ASSERT_EQUAL_UINT32(WINDOW + 4U, s_monitor.requested_period);
}

/* The last period of a window is `bad`, with `acked` frames acknowledged since the period
 * before, instead of a healthy one: it asks for nothing and starts the window again, counted
 * once, and the window that follows it is a whole new one. From the bad period on, the count
 * of bus-offs stays where it went. */
static void check_bad_period_restarts(const update_health_t *bad, uint64_t acked)
{
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 2U, PLAT_BOOT_EVENT_NONE);
    update_health_t after = healthy();

    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&after, WINDOW - 1U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, run_acked(bad, acked, 1U), "a bad period");
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_WAITING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.healthy_periods);
    TEST_ASSERT_EQUAL_UINT32(1U, s_monitor.window_restarts);

    after.bus_off_count = bad->bus_off_count;
    TEST_ASSERT_EQUAL_UINT32(0U, run(&after, WINDOW - 1U));
    TEST_ASSERT_EQUAL_UINT32(1U, run(&after, 1U));
    TEST_ASSERT_EQUAL_UINT32(2U * WINDOW, s_monitor.requested_period);
    TEST_ASSERT_EQUAL_UINT32(1U, s_monitor.window_restarts);
}

/* The watchdog manager FAILED, EXPIRED, STOPPED or DEACTIVATED: the window starts again. */
static void test_supervision_not_ok_starts_the_window_again(void)
{
    static const wdgm_global_status_t statuses[] = {
        WDGM_GLOBAL_STATUS_FAILED, WDGM_GLOBAL_STATUS_EXPIRED, WDGM_GLOBAL_STATUS_STOPPED,
        WDGM_GLOBAL_STATUS_DEACTIVATED};

    for (size_t i = 0U; i < (sizeof(statuses) / sizeof(statuses[0])); i++) {
        update_health_t bad = healthy();

        bad.supervision = statuses[i];
        check_bad_period_restarts(&bad, FRAMES_PER_PERIOD);
        check_failed(1U, 0U, 0U, 0U);
    }
}

/* Error passive restarts the window, also while frames are still acknowledged (a controller
 * that is error passive through its receive counter), and a trial whose controller stays error
 * passive never asks for its confirm. So does a controller that is bus off at the period with
 * no new bus-off since the period before (still off since the last one). */
static void test_error_passive_restarts_the_window_and_asks_for_no_confirm(void)
{
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 1U, PLAT_BOOT_EVENT_NONE);
    update_health_t passive = healthy();
    update_health_t off = healthy();

    passive.can_state = CAN_FC_ERROR_PASSIVE;
    check_bad_period_restarts(&passive, FRAMES_PER_PERIOD);
    check_failed(0U, 1U, 0U, 0U);

    off.can_state = CAN_FC_BUS_OFF;
    check_bad_period_restarts(&off, FRAMES_PER_PERIOD);
    check_failed(0U, 1U, 0U, 0U);

    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&passive, 10U * WINDOW));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_WAITING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.healthy_periods);
    TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.requested_period);
    check_failed(0U, 10U * WINDOW, 0U, 0U);
}

/* A bus-off between two periods (the count went up) restarts the window although the
 * controller is error active again by the period. */
static void test_a_bus_off_between_two_periods_starts_the_window_again(void)
{
    update_health_t recovered = healthy();

    recovered.bus_off_count = 1U;
    check_bad_period_restarts(&recovered, FRAMES_PER_PERIOD);
    check_failed(0U, 0U, 1U, 0U);
}

/* A period in which no frame was acknowledged since the period before restarts the window, the
 * controller error active and the supervision OK notwithstanding. */
static void test_a_period_without_an_acknowledged_frame_starts_the_window_again(void)
{
    const update_health_t good = healthy();

    check_bad_period_restarts(&good, 0U);
    check_failed(0U, 0U, 0U, 1U);
}

/* What a node that nobody acknowledges reports, period after period: error passive (at a
 * transmit error counter of 128), never bus off, no frame acknowledged (update.h, WHY 5
 * SECONDS). It never asks for its confirm, however long it runs; once it is acknowledged again
 * it confirms after a whole new window. */
static void test_a_trial_that_nobody_acknowledges_never_confirms_itself(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 1U, PLAT_BOOT_EVENT_NONE);
    update_health_t lost = good;

    lost.can_state = CAN_FC_ERROR_PASSIVE;
    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 10U));
    TEST_ASSERT_EQUAL_UINT32(0U, run_acked(&lost, 0U, 10U * WINDOW));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_WAITING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.healthy_periods);
    TEST_ASSERT_EQUAL_UINT32(1U, s_monitor.window_restarts);
    check_failed(0U, 10U * WINDOW, 0U, 10U * WINDOW);

    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, WINDOW - 1U));
    TEST_ASSERT_EQUAL_UINT32(1U, run(&good, 1U));
    TEST_ASSERT_EQUAL_UINT32(10U + (10U * WINDOW) + WINDOW, s_monitor.requested_period);
}

/* Bad periods before the first good one restart nothing (there was no window yet); each run of
 * good periods that a bad one ends counts once. */
static void test_restarts_count_the_windows_that_were_cut_short(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 3U, PLAT_BOOT_EVENT_NONE);
    update_health_t failed = good;

    failed.supervision = WDGM_GLOBAL_STATUS_FAILED;
    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&failed, 3U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.window_restarts);
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 7U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&failed, 2U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&failed, 1U));
    TEST_ASSERT_EQUAL_UINT32(2U, s_monitor.window_restarts);
    TEST_ASSERT_EQUAL_UINT32(14U, s_monitor.periods);
}

/* The flash task's answer: CONFIRMED for BOOT_OK, FAILED for anything else; an answer while no
 * confirm is asked for changes nothing. */
static void test_the_result_of_the_confirm_decides_its_state(void)
{
    static const boot_status_t failures[] = {BOOT_E_FLASH, BOOT_E_STATE, BOOT_E_ARG,
                                             BOOT_E_POWER_LOST};
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 1U, PLAT_BOOT_EVENT_NONE);

    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    update_confirm_done(&s_monitor, BOOT_OK); /* nothing asked yet */
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_WAITING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(1U, run(&good, WINDOW));
    update_confirm_done(&s_monitor, BOOT_OK);
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMED, s_monitor.state);
    TEST_ASSERT_EQUAL(BOOT_OK, s_monitor.confirm_status);
    update_confirm_done(&s_monitor, BOOT_E_FLASH); /* a late second answer */
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMED, s_monitor.state);

    for (size_t i = 0U; i < (sizeof(failures) / sizeof(failures[0])); i++) {
        TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
        TEST_ASSERT_EQUAL_UINT32(1U, run(&good, WINDOW));
        update_confirm_done(&s_monitor, failures[i]);
        TEST_ASSERT_EQUAL(UPDATE_CONFIRM_FAILED, s_monitor.state);
        TEST_ASSERT_EQUAL(failures[i], s_monitor.confirm_status);
        /* A failed confirm is not asked for again. */
        TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 2U * WINDOW));
    }
}

/* ROLLBACK, REJECT and FALLBACK are rollbacks; NONE and ADOPT are not; no hand-off is none. */
static void test_which_events_are_a_rollback(void)
{
    const plat_boot_event_t yes[] = {PLAT_BOOT_EVENT_ROLLBACK, PLAT_BOOT_EVENT_REJECT,
                                     PLAT_BOOT_EVENT_FALLBACK};
    const plat_boot_event_t no[] = {PLAT_BOOT_EVENT_NONE, PLAT_BOOT_EVENT_ADOPT};

    for (size_t i = 0U; i < 3U; i++) {
        const plat_boot_handoff_t h = handoff(PLAT_BOOT_CONFIRMED, 0U, yes[i]);

        TEST_ASSERT_TRUE(update_rolled_back(&h));
    }

    for (size_t i = 0U; i < 2U; i++) {
        const plat_boot_handoff_t h = handoff(PLAT_BOOT_CONFIRMED, 0U, no[i]);

        TEST_ASSERT_FALSE(update_rolled_back(&h));
    }

    TEST_ASSERT_FALSE(update_rolled_back(NULL));
}

static void test_the_names_are_those_of_the_dbc(void)
{
    TEST_ASSERT_EQUAL_STRING("NOT_NEEDED", update_confirm_state_name(UPDATE_CONFIRM_NOT_NEEDED));
    TEST_ASSERT_EQUAL_STRING("WAITING", update_confirm_state_name(UPDATE_CONFIRM_WAITING));
    TEST_ASSERT_EQUAL_STRING("CONFIRMING", update_confirm_state_name(UPDATE_CONFIRM_CONFIRMING));
    TEST_ASSERT_EQUAL_STRING("CONFIRMED", update_confirm_state_name(UPDATE_CONFIRM_CONFIRMED));
    TEST_ASSERT_EQUAL_STRING("FAILED", update_confirm_state_name(UPDATE_CONFIRM_FAILED));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): no state, on purpose */
    TEST_ASSERT_EQUAL_STRING("unknown", update_confirm_state_name((update_confirm_state_t)5));
    TEST_ASSERT_EQUAL_STRING("NONE", update_event_name(PLAT_BOOT_EVENT_NONE));
    TEST_ASSERT_EQUAL_STRING("ADOPT", update_event_name(PLAT_BOOT_EVENT_ADOPT));
    TEST_ASSERT_EQUAL_STRING("ROLLBACK", update_event_name(PLAT_BOOT_EVENT_ROLLBACK));
    TEST_ASSERT_EQUAL_STRING("REJECT", update_event_name(PLAT_BOOT_EVENT_REJECT));
    TEST_ASSERT_EQUAL_STRING("FALLBACK", update_event_name(PLAT_BOOT_EVENT_FALLBACK));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): no event, on purpose */
    TEST_ASSERT_EQUAL_STRING("unknown", update_event_name((plat_boot_event_t)2));
}

/* The window counts periods: with 1 period a second, or 0 taken as 1, it is UPDATE_CONFIRM_AFTER_S
 * periods. */
static void test_the_window_follows_the_periods_per_second(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 1U, PLAT_BOOT_EVENT_NONE);

    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT32(UPDATE_CONFIRM_AFTER_S, s_monitor.periods_needed);
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, UPDATE_CONFIRM_AFTER_S - 1U));
    TEST_ASSERT_EQUAL_UINT32(UPDATE_CONFIRM_AFTER_S - 1U, update_healthy_seconds(&s_monitor, 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, run(&good, 1U));
}

/* A window given in periods (the host's --confirm-periods, app_config_t) replaces the
 * product's: the trial asks for its confirm in the period that completes it, not one earlier,
 * and its healthy seconds count up to the window's; 0 gives the product's window. */
static void test_a_window_given_in_periods_replaces_the_product_s(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 1U, PLAT_BOOT_EVENT_NONE);
    const uint32_t longer = (3U * WINDOW) + 7U;

    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, longer));
    TEST_ASSERT_EQUAL_UINT32(longer, s_monitor.periods_needed);
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, longer - 1U));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_WAITING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32((longer - 1U) / PER_SECOND,
                             update_healthy_seconds(&s_monitor, PER_SECOND));
    TEST_ASSERT_EQUAL_UINT32(1U, run(&good, 1U));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(longer, s_monitor.requested_period);

    /* Shorter than the product's as well: one period. */
    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 1U));
    TEST_ASSERT_EQUAL_UINT32(1U, run(&good, 1U));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMING, s_monitor.state);

    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    TEST_ASSERT_EQUAL_UINT32(WINDOW, s_monitor.periods_needed);
}

/* The tester's confirm routine confirms the trial from WAITING and CONFIRMING; a confirm of the
 * window that comes back later, and every other state, change nothing: the first one wins. */
static void test_the_confirm_routine_confirms_a_waiting_or_confirming_trial(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 1U, PLAT_BOOT_EVENT_NONE);
    const plat_boot_handoff_t confirmed = handoff(PLAT_BOOT_CONFIRMED, 0U, PLAT_BOOT_EVENT_NONE);

    /* WAITING: confirmed at once, and the window asks for nothing any more. */
    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 3U));
    update_routine_confirmed(&s_monitor);
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMED, s_monitor.state);
    TEST_ASSERT_EQUAL(BOOT_OK, s_monitor.confirm_status);
    TEST_ASSERT_TRUE(s_monitor.by_routine);
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 2U * WINDOW));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMED, s_monitor.state);

    /* CONFIRMING: the window asked, the routine came first; the window's late answer (the flash
     * task found the trial confirmed: BOOT_OK, or a failure) changes nothing. */
    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, run(&good, WINDOW));
    update_routine_confirmed(&s_monitor);
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMED, s_monitor.state);
    update_confirm_done(&s_monitor, BOOT_E_STATE);
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMED, s_monitor.state);
    TEST_ASSERT_EQUAL(BOOT_OK, s_monitor.confirm_status);
    TEST_ASSERT_TRUE(s_monitor.by_routine);

    /* Confirmed by its window first: the routine changes nothing (and lib/boot refuses it). */
    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, run(&good, WINDOW));
    update_confirm_done(&s_monitor, BOOT_OK);
    update_routine_confirmed(&s_monitor);
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMED, s_monitor.state);
    TEST_ASSERT_FALSE(s_monitor.by_routine);

    /* FAILED, and no trial at all: unchanged. */
    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND, 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, run(&good, WINDOW));
    update_confirm_done(&s_monitor, BOOT_E_FLASH);
    update_routine_confirmed(&s_monitor);
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_FAILED, s_monitor.state);
    TEST_ASSERT_FALSE(s_monitor.by_routine);
    TEST_ASSERT_TRUE(update_init(&s_monitor, &confirmed, PER_SECOND, 0U));
    update_routine_confirmed(&s_monitor);
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_NOT_NEEDED, s_monitor.state);
    TEST_ASSERT_TRUE(update_init(&s_monitor, NULL, PER_SECOND, 0U));
    update_routine_confirmed(&s_monitor);
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_NOT_NEEDED, s_monitor.state);
    TEST_ASSERT_FALSE(s_monitor.by_routine);
}

static void dummy_start(void *ctx, const flash_dev_t *dev, uint32_t addr, uint32_t len)
{
    (void)ctx;
    (void)dev;
    (void)addr;
    (void)len;
}

static boot_check_t dummy_step(void *ctx, boot_image_info_t *info)
{
    (void)ctx;
    (void)info;
    return BOOT_CHECK_INVALID;
}

/* lib/boot's configuration is the layout's areas, the device given and the validator given, or
 * none. */
static void test_the_boot_configuration_is_the_platform_layout(void)
{
    const plat_flash_layout_t layout = {
        .geometry = {.size = 0x202000U, .sector_size = 4096U, .page_size = 256U, .unit_size = 8U},
        .meta = {.offset = 0U, .size = 0x2000U},
        .slot = {{.offset = 0x2000U, .size = 0x100000U}, {.offset = 0x102000U, .size = 0x100000U}}};
    const flash_dev_t dev = {.geometry = layout.geometry};
    static int s_marker;
    const boot_validator_t validator = {.ctx = &s_marker, .start = dummy_start, .step = dummy_step};
    boot_config_t config;

    (void)memset(&config, 0x5A, sizeof(config));
    TEST_ASSERT_TRUE(update_boot_config(&layout, &dev, &validator, &config));
    TEST_ASSERT_EQUAL_PTR(&s_marker, config.validator.ctx);
    TEST_ASSERT_TRUE(config.validator.start == dummy_start);
    TEST_ASSERT_TRUE(config.validator.step == dummy_step);

    (void)memset(&config, 0x5A, sizeof(config));
    TEST_ASSERT_TRUE(update_boot_config(&layout, &dev, NULL, &config));
    TEST_ASSERT_EQUAL_PTR(&dev, config.dev);
    TEST_ASSERT_EQUAL_UINT32(0U, config.meta_addr);
    TEST_ASSERT_EQUAL_UINT32(2U, config.meta_sectors);
    TEST_ASSERT_EQUAL_HEX32(0x2000U, config.slot[0].addr);
    TEST_ASSERT_EQUAL_HEX32(0x100000U, config.slot[0].size);
    TEST_ASSERT_EQUAL_HEX32(0x102000U, config.slot[1].addr);
    TEST_ASSERT_EQUAL_HEX32(0x100000U, config.slot[1].size);
    TEST_ASSERT_NULL(config.validator.start);
    TEST_ASSERT_NULL(config.validator.step);
    TEST_ASSERT_NULL(config.validator.ctx);

    TEST_ASSERT_FALSE(update_boot_config(&layout, NULL, &validator, &config));
    TEST_ASSERT_FALSE(update_boot_config(NULL, &dev, &validator, &config));
    TEST_ASSERT_FALSE(update_boot_config(&layout, &dev, &validator, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_without_a_trial_nothing_is_confirmed);
    RUN_TEST(test_a_healthy_trial_asks_once_when_the_window_is_full);
    RUN_TEST(test_one_acknowledged_frame_a_period_keeps_the_window);
    RUN_TEST(test_supervision_not_ok_starts_the_window_again);
    RUN_TEST(test_error_passive_restarts_the_window_and_asks_for_no_confirm);
    RUN_TEST(test_a_bus_off_between_two_periods_starts_the_window_again);
    RUN_TEST(test_a_period_without_an_acknowledged_frame_starts_the_window_again);
    RUN_TEST(test_a_trial_that_nobody_acknowledges_never_confirms_itself);
    RUN_TEST(test_restarts_count_the_windows_that_were_cut_short);
    RUN_TEST(test_the_result_of_the_confirm_decides_its_state);
    RUN_TEST(test_the_confirm_routine_confirms_a_waiting_or_confirming_trial);
    RUN_TEST(test_which_events_are_a_rollback);
    RUN_TEST(test_the_names_are_those_of_the_dbc);
    RUN_TEST(test_the_window_follows_the_periods_per_second);
    RUN_TEST(test_a_window_given_in_periods_replaces_the_product_s);
    RUN_TEST(test_the_boot_configuration_is_the_platform_layout);
    return UNITY_END();
}
