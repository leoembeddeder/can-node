/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "log/log.h"
#include "unity.h"

static char s_line[LOG_LINE_MAX + 8U];
static size_t s_len;
static unsigned int s_calls;
static uint64_t s_now_ns;

static void capture_sink(const char *line, size_t len)
{
    s_calls++;
    s_len = len;
    (void)memset(s_line, 0, sizeof(s_line));

    if (len < sizeof(s_line)) {
        (void)memcpy(s_line, line, len);
    }
}

static uint64_t fake_time_ns(void)
{
    return s_now_ns;
}

void setUp(void)
{
    s_calls = 0U;
    s_len = 0U;
    s_now_ns = 0U;
    (void)memset(s_line, 0, sizeof(s_line));
    log_init(capture_sink, fake_time_ns, LOG_LEVEL_INFO);
}

void tearDown(void)
{
    log_init(NULL, NULL, LOG_LEVEL_INFO);
}

static void test_line_has_timestamp_level_tag_message_and_newline(void)
{
    s_now_ns = 12345678901ULL; /* 12.345678901 s */
    LOG_INF("can", "frame 0x%03X dlc %d", 0x123U, 8);

    TEST_ASSERT_EQUAL_UINT(1U, s_calls);
    TEST_ASSERT_EQUAL_STRING("[    12.345678] I can: frame 0x123 dlc 8\n", s_line);
    TEST_ASSERT_EQUAL_size_t(strlen(s_line), s_len);
}

static void test_each_level_has_its_letter(void)
{
    log_set_level(LOG_LEVEL_DEBUG);

    LOG_ERR("t", "x");
    TEST_ASSERT_NOT_NULL(strstr(s_line, "] E t: x\n"));
    LOG_WRN("t", "x");
    TEST_ASSERT_NOT_NULL(strstr(s_line, "] W t: x\n"));
    LOG_INF("t", "x");
    TEST_ASSERT_NOT_NULL(strstr(s_line, "] I t: x\n"));
    LOG_DBG("t", "x");
    TEST_ASSERT_NOT_NULL(strstr(s_line, "] D t: x\n"));
    TEST_ASSERT_EQUAL_UINT(4U, s_calls);
}

static void test_lines_above_the_level_are_discarded(void)
{
    log_set_level(LOG_LEVEL_WARN);
    TEST_ASSERT_EQUAL_INT(LOG_LEVEL_WARN, log_get_level());

    LOG_INF("t", "not shown");
    LOG_DBG("t", "not shown");
    TEST_ASSERT_EQUAL_UINT(0U, s_calls);

    LOG_WRN("t", "shown");
    LOG_ERR("t", "shown");
    TEST_ASSERT_EQUAL_UINT(2U, s_calls);
}

static void test_nothing_happens_without_a_sink(void)
{
    log_init(NULL, fake_time_ns, LOG_LEVEL_DEBUG);
    LOG_ERR("t", "nobody listens");
    TEST_ASSERT_EQUAL_UINT(0U, s_calls);
}

static void test_missing_time_source_and_tag_are_tolerated(void)
{
    log_init(capture_sink, NULL, LOG_LEVEL_INFO);
    LOG_INF(NULL, "hello");
    TEST_ASSERT_EQUAL_STRING("[     0.000000] I -: hello\n", s_line);
}

static void test_overlong_message_is_truncated_to_one_full_line(void)
{
    char big[400];

    (void)memset(big, 'x', sizeof(big) - 1U);
    big[sizeof(big) - 1U] = '\0';
    LOG_INF("t", "%s", big);

    TEST_ASSERT_EQUAL_UINT(1U, s_calls);
    TEST_ASSERT_EQUAL_size_t(LOG_LINE_MAX, s_len);
    TEST_ASSERT_EQUAL_CHAR('\n', s_line[LOG_LINE_MAX - 1U]);
    TEST_ASSERT_EQUAL_CHAR('x', s_line[LOG_LINE_MAX - 2U]);
}

static void test_overlong_tag_still_yields_one_terminated_line(void)
{
    char tag[300];

    (void)memset(tag, 'T', sizeof(tag) - 1U);
    tag[sizeof(tag) - 1U] = '\0';
    LOG_INF(tag, "message that no longer fits");

    TEST_ASSERT_EQUAL_UINT(1U, s_calls);
    TEST_ASSERT_EQUAL_size_t(LOG_LINE_MAX, s_len);
    TEST_ASSERT_EQUAL_CHAR('\n', s_line[LOG_LINE_MAX - 1U]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_line_has_timestamp_level_tag_message_and_newline);
    RUN_TEST(test_each_level_has_its_letter);
    RUN_TEST(test_lines_above_the_level_are_discarded);
    RUN_TEST(test_nothing_happens_without_a_sink);
    RUN_TEST(test_missing_time_source_and_tag_are_tolerated);
    RUN_TEST(test_overlong_message_is_truncated_to_one_full_line);
    RUN_TEST(test_overlong_tag_still_yields_one_terminated_line);
    return UNITY_END();
}
