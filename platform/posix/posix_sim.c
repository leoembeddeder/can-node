/* SPDX-License-Identifier: MIT */
/* Simulation control socket; see posix_sim.h for what it is and the protocol. */
#include "posix_sim.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SIM_MAX_HANDLERS 8U

/* What the reply datagram needs besides the handler's text: the longest token, " err " (the
 * longer of " ok " and " err ") and the terminating zero. */
#define SIM_REPLY_OVERHEAD (POSIX_SIM_MAX_TOKEN + 5U + 1U)

typedef struct {
    const char *prefix;
    posix_sim_handler_t handler;
} sim_entry_t;

static int s_fd = -1;
static sim_entry_t s_entries[SIM_MAX_HANDLERS];
static size_t s_entry_count;

bool posix_sim_open(uint16_t port, char *error, size_t error_len)
{
    struct sockaddr_in address;
    int fd;

    if (port == 0U) {
        return true;
    }

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        (void)snprintf(error, error_len, "sim: socket: %s", strerror(errno));
        return false;
    }

    /* Loopback only: this socket can break the node on purpose and must not be reachable
     * from another machine. */
    (void)memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        (void)snprintf(error, error_len, "sim: cannot bind 127.0.0.1:%u: %s", (unsigned int)port,
                       strerror(errno));
        (void)close(fd);
        return false;
    }

    s_fd = fd;
    return true;
}

bool posix_sim_enabled(void)
{
    return (s_fd >= 0);
}

int posix_sim_fd(void)
{
    return s_fd;
}

void posix_sim_register(const char *prefix, posix_sim_handler_t handler)
{
    size_t index = s_entry_count;

    /* A prefix has one handler: registering it again replaces the handler. */
    for (size_t i = 0U; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].prefix, prefix) == 0) {
            index = i;
        }
    }

    if (index < SIM_MAX_HANDLERS) {
        s_entries[index].prefix = prefix;
        s_entries[index].handler = handler;

        if (index == s_entry_count) {
            s_entry_count++;
        }
    }
}

/* Split "<word> <rest>" in place: terminates the word and returns the rest (never NULL). */
static char *split_word(char *text)
{
    char *space = strchr(text, ' ');

    if (space == NULL) {
        return &text[strlen(text)];
    }

    *space = '\0';
    space++;

    while (*space == ' ') {
        space++;
    }

    return space;
}

static bool dispatch(const char *command, const char *args, char *reply, size_t reply_len)
{
    if (strcmp(command, "ping") == 0) {
        (void)snprintf(reply, reply_len, "pid=%ld", (long)getpid());
        return true;
    }

    for (size_t i = 0U; i < s_entry_count; i++) {
        const size_t prefix_len = strlen(s_entries[i].prefix);

        if (strncmp(command, s_entries[i].prefix, prefix_len) == 0) {
            return s_entries[i].handler(&command[prefix_len], args, reply, reply_len);
        }
    }

    (void)snprintf(reply, reply_len, "unknown command '%s'", command);
    return false;
}

void posix_sim_service(void)
{
    if (s_fd < 0) {
        return;
    }

    for (;;) {
        char request[POSIX_SIM_MAX_DATAGRAM];
        char reply[POSIX_SIM_MAX_DATAGRAM - SIM_REPLY_OVERHEAD];
        char datagram[POSIX_SIM_MAX_DATAGRAM];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const ssize_t received = recvfrom(s_fd, request, sizeof(request) - 1U, MSG_DONTWAIT,
                                          (struct sockaddr *)&from, &from_len);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            break; /* nothing waiting (or a socket error: try again on the next wake-up) */
        }

        /* Terminate the text, dropping the newline that a human typing with netcat sends. */
        size_t length = (size_t)received;

        while ((length > 0U) &&
               ((request[length - 1U] == '\n') || (request[length - 1U] == '\r'))) {
            length--;
        }

        request[length] = '\0';

        char *token = request;
        char *command = split_word(token);
        const char *args = split_word(command);
        int len;

        reply[0] = '\0';

        if ((token[0] == '\0') || (command[0] == '\0') || (strlen(token) > POSIX_SIM_MAX_TOKEN)) {
            len = snprintf(datagram, sizeof(datagram), "? err malformed request");
        } else if (dispatch(command, args, reply, sizeof(reply))) {
            len = snprintf(datagram, sizeof(datagram), "%s ok%s%s", token,
                           (reply[0] != '\0') ? " " : "", reply);
        } else {
            len = snprintf(datagram, sizeof(datagram), "%s err %s", token, reply);
        }

        if (len > 0) {
            const size_t count =
                ((size_t)len < sizeof(datagram)) ? (size_t)len : (sizeof(datagram) - 1U);

            (void)sendto(s_fd, datagram, count, MSG_DONTWAIT, (const struct sockaddr *)&from,
                         from_len);
        }
    }
}

void posix_sim_close(void)
{
    if (s_fd >= 0) {
        (void)close(s_fd);
        s_fd = -1;
    }
}
