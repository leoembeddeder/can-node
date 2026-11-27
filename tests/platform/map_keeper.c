/* SPDX-License-Identifier: MIT */
/* The keeper of the platform tests; map_keeper.h says what it is for. */
#include "map_keeper.h"

#include <unistd.h>

/* Descriptors a keeper closes: more than a test process here ever has open. */
#define KEEPER_DESCRIPTORS 1024

bool map_keeper_prepare(map_keeper_t *keeper)
{
    bool ok = (pipe(keeper->let_go) == 0);

    if (ok && (pipe(keeper->gone) != 0)) {
        (void)close(keeper->let_go[0]);
        (void)close(keeper->let_go[1]);
        ok = false;
    }

    return ok;
}

pid_t map_keeper_fork(const map_keeper_t *keeper)
{
    const pid_t pid = fork();

    if (pid == 0) {
        char byte = 0;

        /* Only the mappings refer to the files from here on. The write end of let_go goes as
         * well, so that the test's is the last one; the write end of gone stays open until
         * this process ends. */
        for (int fd = 0; fd < KEEPER_DESCRIPTORS; fd++) {
            if ((fd != keeper->let_go[0]) && (fd != keeper->gone[1])) {
                (void)close(fd);
            }
        }

        while (read(keeper->let_go[0], &byte, 1U) > 0) {
            /* nothing is written into the pipe */
        }

        _exit(0);
    }

    return pid;
}

bool map_keeper_end(const map_keeper_t *keeper)
{
    char byte = 0;

    /* The last write end of let_go: every keeper's read ends. Then this process's own write
     * end of gone, so that the read below ends when the last keeper has. */
    (void)close(keeper->let_go[1]);
    (void)close(keeper->let_go[0]);
    (void)close(keeper->gone[1]);

    const bool gone = (read(keeper->gone[0], &byte, 1U) == 0);

    (void)close(keeper->gone[0]);
    return gone;
}
