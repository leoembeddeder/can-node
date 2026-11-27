/* SPDX-License-Identifier: MIT */
/*
 * Tests of the simulation control socket (platform/posix/posix_sim): the request and reply
 * format, the dispatch to registered handlers, and what happens to requests that are not
 * well formed. The test plays both sides: it sends datagrams from a client socket and then
 * calls posix_sim_service() itself, as the hardware thread does in the node.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "posix_sim.h"
#include "unity.h"

static int s_client = -1;
static uint16_t s_port;
static char s_error[160];
static char s_last_verb[64];
static char s_last_args[POSIX_SIM_MAX_DATAGRAM];
static unsigned int s_handler_calls;

void setUp(void)
{
    s_last_verb[0] = '\0';
    s_last_args[0] = '\0';
    s_handler_calls = 0U;
}

void tearDown(void)
{
}

/* Handler of the "demo." commands: echo | quiet | long | longfail; anything else fails. */
static bool demo_handler(const char *verb, const char *args, char *reply, size_t reply_len)
{
    bool ok = true;

    s_handler_calls++;
    (void)snprintf(s_last_verb, sizeof(s_last_verb), "%s", verb);
    (void)snprintf(s_last_args, sizeof(s_last_args), "%s", args);

    if (strcmp(verb, "echo") == 0) {
        (void)snprintf(reply, reply_len, "said=%s", args);
    } else if (strcmp(verb, "quiet") == 0) {
        reply[0] = '\0';
    } else if ((strcmp(verb, "long") == 0) || (strcmp(verb, "longfail") == 0)) {
        /* As much as fits: the handler is given the size and must stay inside it. */
        (void)memset(reply, 'x', reply_len - 1U);
        reply[reply_len - 1U] = '\0';
        ok = (strcmp(verb, "long") == 0);
    } else {
        (void)snprintf(reply, reply_len, "no such demo command '%s'", verb);
        ok = false;
    }

    return ok;
}

static void send_request(const char *text)
{
    struct sockaddr_in to;

    (void)memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(s_port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT((int)strlen(text), (int)sendto(s_client, text, strlen(text), 0,
                                                         (const struct sockaddr *)&to, sizeof(to)));
}

/* The next reply as a string, or NULL if none arrives within 200 ms. */
static const char *receive_reply(void)
{
    static char reply[2U * POSIX_SIM_MAX_DATAGRAM];
    struct pollfd fd = {.fd = s_client, .events = POLLIN, .revents = 0};
    const char *result = NULL;

    if (poll(&fd, 1U, 200) > 0) {
        const ssize_t n = recv(s_client, reply, sizeof(reply) - 1U, 0);

        if (n >= 0) {
            reply[n] = '\0';
            result = reply;
        }
    }

    return result;
}

/* One request, served, and its reply. */
static const char *exchange(const char *request)
{
    send_request(request);
    posix_sim_service();
    return receive_reply();
}

/* One request, served, and its reply; "<no reply>" if none came, which no expected reply is. */
static const char *exchange_text(const char *request)
{
    const char *reply = exchange(request);

    return (reply != NULL) ? reply : "<no reply>";
}

/* --------------------------------------------------------------------------------- tests */

static void test_ping_is_built_in_and_returns_the_process_id(void)
{
    char expected[64];

    (void)snprintf(expected, sizeof(expected), "t1 ok pid=%ld", (long)getpid());
    TEST_ASSERT_EQUAL_STRING(expected, exchange("t1 ping"));
}

static void test_a_command_goes_to_the_handler_of_its_prefix_with_verb_and_arguments(void)
{
    TEST_ASSERT_EQUAL_STRING("abc ok said=hello world", exchange("abc demo.echo hello world"));
    TEST_ASSERT_EQUAL_STRING("echo", s_last_verb);
    TEST_ASSERT_EQUAL_STRING("hello world", s_last_args);
    TEST_ASSERT_EQUAL_UINT(1U, s_handler_calls);
}

static void test_spaces_between_the_parts_and_a_trailing_newline_are_ignored(void)
{
    /* What a person typing into netcat sends. */
    TEST_ASSERT_EQUAL_STRING("n1 ok said=a b", exchange("n1   demo.echo    a b\r\n"));
    TEST_ASSERT_EQUAL_STRING("a b", s_last_args);
}

static void test_a_reply_without_values_is_just_ok(void)
{
    TEST_ASSERT_EQUAL_STRING("q ok", exchange("q demo.quiet"));
    TEST_ASSERT_EQUAL_STRING("", s_last_args);
}

static void test_a_failing_handler_produces_an_error_reply_with_its_message(void)
{
    TEST_ASSERT_EQUAL_STRING("x9 err no such demo command 'nope'", exchange("x9 demo.nope 1 2"));
}

static void test_an_unknown_command_is_an_error_and_reaches_no_handler(void)
{
    TEST_ASSERT_EQUAL_STRING("u err unknown command 'other.thing'", exchange("u other.thing"));
    TEST_ASSERT_EQUAL_UINT(0U, s_handler_calls);
}

static void test_requests_that_are_not_token_and_command_are_refused(void)
{
    char long_token[80];

    TEST_ASSERT_EQUAL_STRING("? err malformed request", exchange("lonely"));
    TEST_ASSERT_EQUAL_STRING("? err malformed request", exchange(""));
    TEST_ASSERT_EQUAL_STRING("? err malformed request", exchange("\n"));

    /* A token is at most 31 characters, so that it always fits into the reply. */
    (void)memset(long_token, 'T', 40U);
    (void)snprintf(&long_token[40], sizeof(long_token) - 40U, " ping");
    TEST_ASSERT_EQUAL_STRING("? err malformed request", exchange(long_token));
    TEST_ASSERT_EQUAL_UINT(0U, s_handler_calls);
}

static void test_a_token_of_31_characters_comes_back_and_one_of_32_is_refused(void)
{
    char request[64];
    char expected[64];

    TEST_ASSERT_EQUAL_UINT(31U, POSIX_SIM_MAX_TOKEN);

    (void)memset(request, 'K', POSIX_SIM_MAX_TOKEN);
    (void)snprintf(&request[POSIX_SIM_MAX_TOKEN], sizeof(request) - POSIX_SIM_MAX_TOKEN,
                   " demo.echo z");
    (void)memset(expected, 'K', POSIX_SIM_MAX_TOKEN);
    (void)snprintf(&expected[POSIX_SIM_MAX_TOKEN], sizeof(expected) - POSIX_SIM_MAX_TOKEN,
                   " ok said=z");
    TEST_ASSERT_EQUAL_STRING(expected, exchange(request));
    TEST_ASSERT_EQUAL_UINT(1U, s_handler_calls);

    (void)memset(request, 'K', POSIX_SIM_MAX_TOKEN + 1U);
    (void)snprintf(&request[POSIX_SIM_MAX_TOKEN + 1U], sizeof(request) - POSIX_SIM_MAX_TOKEN - 1U,
                   " demo.echo z");
    TEST_ASSERT_EQUAL_STRING("? err malformed request", exchange(request));
    TEST_ASSERT_EQUAL_UINT(1U, s_handler_calls);
}

/* A handler's reply has POSIX_SIM_MAX_DATAGRAM less the room for the token, " err " and the
 * terminating zero; with the longest token, the longest reply of either kind still goes out
 * whole. (Were that room too small, the datagram would be cut to POSIX_SIM_MAX_DATAGRAM - 1
 * characters.) */
static void test_the_longest_token_with_the_longest_reply_fits_one_datagram(void)
{
    const size_t text = (size_t)POSIX_SIM_MAX_DATAGRAM - (POSIX_SIM_MAX_TOKEN + 5U + 1U) - 1U;
    char request[64];
    const char *reply;

    (void)memset(request, 'L', POSIX_SIM_MAX_TOKEN);
    (void)snprintf(&request[POSIX_SIM_MAX_TOKEN], sizeof(request) - POSIX_SIM_MAX_TOKEN,
                   " demo.long");
    reply = exchange_text(request);
    TEST_ASSERT_EQUAL_UINT(POSIX_SIM_MAX_TOKEN + 4U + text, strlen(reply));
    TEST_ASSERT_EQUAL_STRING_LEN(" ok xxx", &reply[POSIX_SIM_MAX_TOKEN], 7U);
    TEST_ASSERT_EQUAL_CHAR('x', reply[strlen(reply) - 1U]);

    (void)snprintf(&request[POSIX_SIM_MAX_TOKEN], sizeof(request) - POSIX_SIM_MAX_TOKEN,
                   " demo.longfail");
    reply = exchange_text(request);
    TEST_ASSERT_EQUAL_UINT(POSIX_SIM_MAX_TOKEN + 5U + text, strlen(reply));
    TEST_ASSERT_EQUAL_STRING_LEN(" err xxx", &reply[POSIX_SIM_MAX_TOKEN], 8U);
    TEST_ASSERT_EQUAL_CHAR('x', reply[strlen(reply) - 1U]);
}

static void test_a_reply_never_exceeds_one_datagram(void)
{
    const char *reply = exchange("big demo.long");

    TEST_ASSERT_TRUE((reply != NULL) && (strlen(reply) < POSIX_SIM_MAX_DATAGRAM));
    TEST_ASSERT_EQUAL_STRING_LEN("big ok xxxx", reply, 11U);
}

static void test_an_oversized_request_is_cut_and_still_answered(void)
{
    static char request[2U * POSIX_SIM_MAX_DATAGRAM];
    const char *reply;

    (void)memset(request, 'a', sizeof(request) - 1U);
    (void)memcpy(request, "o demo.echo ", 12U);
    request[sizeof(request) - 1U] = '\0';
    reply = exchange(request);

    TEST_ASSERT_TRUE((reply != NULL) && (strlen(reply) < POSIX_SIM_MAX_DATAGRAM));
    TEST_ASSERT_EQUAL_STRING_LEN("o ok said=aaaa", reply, 14U);
}

static void test_every_waiting_request_is_served_in_one_call_in_order(void)
{
    send_request("a demo.echo 1");
    send_request("b demo.echo 2");
    send_request("c ping");
    posix_sim_service();

    TEST_ASSERT_EQUAL_STRING("a ok said=1", receive_reply());
    TEST_ASSERT_EQUAL_STRING("b ok said=2", receive_reply());

    const char *third = receive_reply();

    TEST_ASSERT_EQUAL_STRING_LEN("c ok pid=", third, 9U);
    TEST_ASSERT_NULL(receive_reply());
    TEST_ASSERT_EQUAL_UINT(2U, s_handler_calls);
}

static void test_serving_with_nothing_waiting_does_nothing(void)
{
    posix_sim_service();
    TEST_ASSERT_NULL(receive_reply());
}

static bool other_handler(const char *verb, const char *args, char *reply, size_t reply_len)
{
    (void)verb;
    (void)args;
    (void)snprintf(reply, reply_len, "other=1");
    return true;
}

/* Runs last: it changes the handler table. */
static void test_registering_a_prefix_again_replaces_its_handler(void)
{
    posix_sim_register("demo.", other_handler);
    TEST_ASSERT_EQUAL_STRING("r ok other=1", exchange("r demo.echo x"));
    TEST_ASSERT_EQUAL_UINT(0U, s_handler_calls);

    /* A model that is started again registers again. That must not use up the table: after
     * many more registrations than it has entries there is still room for a new prefix. */
    for (unsigned int i = 0U; i < 20U; i++) {
        posix_sim_register("demo.", demo_handler);
    }

    posix_sim_register("late.", other_handler);
    TEST_ASSERT_EQUAL_STRING("s ok said=y", exchange("s demo.echo y"));
    TEST_ASSERT_EQUAL_STRING("t ok other=1", exchange("t late.anything"));
}

static void test_the_port_is_taken_while_the_socket_is_open(void)
{
    struct sockaddr_in address;
    const int other = socket(AF_INET, SOCK_DGRAM, 0);

    TEST_ASSERT_TRUE(other >= 0);
    (void)memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(s_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT(-1, bind(other, (const struct sockaddr *)&address, sizeof(address)));
    (void)close(other);
}

int main(void)
{
    struct sockaddr_in any;
    socklen_t len = sizeof(any);
    int probe;

    UNITY_BEGIN();

    /* Port 0 leaves simulation control switched off. */
    if (!posix_sim_open(0U, s_error, sizeof(s_error)) || posix_sim_enabled()) {
        (void)printf("port 0 did not leave simulation control off\n");
        return 1;
    }

    posix_sim_service(); /* must be harmless while it is off */

    /* Let the kernel pick a free port, then give it to the code under test. */
    probe = socket(AF_INET, SOCK_DGRAM, 0);
    (void)memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if ((probe < 0) || (bind(probe, (const struct sockaddr *)&any, sizeof(any)) != 0) ||
        (getsockname(probe, (struct sockaddr *)&any, &len) != 0)) {
        (void)printf("cannot find a free port\n");
        return 1;
    }

    s_port = ntohs(any.sin_port);
    (void)close(probe);

    if (!posix_sim_open(s_port, s_error, sizeof(s_error))) {
        (void)printf("%s\n", s_error);
        return 1;
    }

    posix_sim_register("demo.", demo_handler);
    s_client = socket(AF_INET, SOCK_DGRAM, 0);

    RUN_TEST(test_ping_is_built_in_and_returns_the_process_id);
    RUN_TEST(test_a_command_goes_to_the_handler_of_its_prefix_with_verb_and_arguments);
    RUN_TEST(test_spaces_between_the_parts_and_a_trailing_newline_are_ignored);
    RUN_TEST(test_a_reply_without_values_is_just_ok);
    RUN_TEST(test_a_failing_handler_produces_an_error_reply_with_its_message);
    RUN_TEST(test_an_unknown_command_is_an_error_and_reaches_no_handler);
    RUN_TEST(test_requests_that_are_not_token_and_command_are_refused);
    RUN_TEST(test_a_token_of_31_characters_comes_back_and_one_of_32_is_refused);
    RUN_TEST(test_the_longest_token_with_the_longest_reply_fits_one_datagram);
    RUN_TEST(test_a_reply_never_exceeds_one_datagram);
    RUN_TEST(test_an_oversized_request_is_cut_and_still_answered);
    RUN_TEST(test_every_waiting_request_is_served_in_one_call_in_order);
    RUN_TEST(test_serving_with_nothing_waiting_does_nothing);
    RUN_TEST(test_the_port_is_taken_while_the_socket_is_open);
    RUN_TEST(test_registering_a_prefix_again_replaces_its_handler);

    (void)close(s_client);
    posix_sim_close();

    if (posix_sim_enabled()) {
        (void)printf("the socket is still open after posix_sim_close()\n");
        return 1;
    }

    return UNITY_END();
}
