/* SPDX-License-Identifier: MIT */
/*
 * The held end, linked into fcn_flash_held, fcn_node_held, fcn_payload_crash_held and
 * fcn_boot_held (tests/tools/CMakeLists.txt): the real programs' own sources, linked with
 * -Wl,--wrap=_exit. It keeps the program's memory map alive after the program has ended, every
 * time, as a process that reads /proc/<pid>/stat or /proc/<pid>/maps of the program at the
 * moment of its end does now and then (docs/bug-log.md, 012): the end starts a process that
 * shares the memory map (clone() with CLONE_VM) and holds it until the test lets it go. The
 * map's shared mappings of the flash file and of the retained cell, if the program left any,
 * then still exist when the program's exit has been seen and the next process opens the files,
 * and with them the open file descriptions that the program locked. So the next process can
 * lock the files only if the program released its locks itself before it ended
 * (posix_flash_unlock() and posix_retained_unlock(), or the two closes). For
 * tests/system/test_held_ends.py.
 *
 * WHICH ENDS. An _exit() of the program's own code goes through __wrap__exit() below: a reset
 * (plat_reset()), the power cut without a kernel, the shutdown guard, a payload's end. exit()
 * and a return from main() run the handler that the constructor below registers with atexit()
 * before main(), so that it runs after every handler the program registers itself. An end by a
 * signal's default action runs neither; the program could not release anything there either.
 *
 * THE TEST'S SIDE. The environment variable FCN_HELD_EXIT names a FIFO that the test has open
 * for reading. The end opens it for writing, starts the holder, writes the holder's process id
 * as a decimal line and goes on to end the program. The test reads the line: a holder was
 * started. It refers to the holder by that id (pidfd_open(), which names that process and no
 * later one with the same id), sees there that the holder still runs once the next program has
 * had the files, and ends it with SIGKILL. The holder keeps the write end, so the FIFO is at
 * its end of file only when no holder is left. Without the variable no holder is started and
 * the program ends as the real one does. If the holder cannot be started, the end says so on
 * stderr and writes no line: a test that expects one then fails, as it should, instead of
 * passing without the memory map held.
 *
 * THE HOLDER is a process of its own (no CLONE_THREAD, and no CLONE_FILES: its descriptor table
 * is a copy of the program's) that shares the program's memory map. It blocks every signal,
 * closes every descriptor of its copy but the FIFO's write end, so that it refers to the two
 * files only through the memory map, and waits for as long as somebody has the FIFO open for
 * reading: until the test ends it, or until the test has ended without that (a holder whose
 * test died ends by itself). No time limits the hold, so how long the test takes over the
 * files decides nothing. It runs on a stack of its own in the shared map but with the thread
 * pointer of the thread that ended the program, whose per-thread state of the C library and
 * of the sanitizers it would otherwise share: it makes raw system calls only, and the
 * sanitizers do not instrument it.
 */
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/* The names are fixed by the linker's --wrap convention. */
_Noreturn void __real__exit(int status);
_Noreturn void __wrap__exit(int status);

#define HOLDER_STACK_SIZE (64U * 1024U)

static _Alignas(16) unsigned char s_stack[HOLDER_STACK_SIZE];
static int s_fifo = -1; /* the FIFO's write end, as the holder's copy of the table has it */
static atomic_flag s_ended = ATOMIC_FLAG_INIT; /* one end, one holder */

__attribute__((no_sanitize("address", "undefined"))) static int hold(void *unused)
{
    /* The kernel's signal set is 64 bits wide on every Linux. */
    const unsigned long long every_signal = ~0ULL;
    const unsigned int keep = (unsigned int)s_fifo;
    struct pollfd fifo = {.fd = s_fifo, .events = 0, .revents = 0};

    (void)unused;
    (void)syscall(SYS_rt_sigprocmask, SIG_BLOCK, &every_signal, NULL, sizeof(every_signal));

    /* If close_range() is missing (Linux before 5.9), the holder keeps its copies of the
     * program's descriptors, which refer to the files as well, and still holds and ends. */
    if (keep > 0U) {
        (void)syscall(SYS_close_range, 0U, keep - 1U, 0U);
    }

    (void)syscall(SYS_close_range, keep + 1U, ~0U, 0U);

    /* A poll that asks for no event, without a timeout: it returns only for POLLERR, which the
     * write end of a FIFO has once nobody has the FIFO open for reading. The test keeps its
     * read end open until it has ended the holder itself, with SIGKILL (every other signal is
     * blocked). If the poll fails, the holder ends at once, and the test, which looks for it
     * afterwards, fails. */
    (void)syscall(SYS_ppoll, &fifo, 1UL, NULL, NULL, 0UL);
    return 0;
}

static void say(const char *what)
{
    static const char prefix[] = "held end: ";

    (void)!write(STDERR_FILENO, prefix, sizeof(prefix) - 1U);
    (void)!write(STDERR_FILENO, what, strlen(what));
    (void)!write(STDERR_FILENO, "\n", 1U);
}

/* Start the holder, once, if the test asked for one. */
static void hold_the_memory_map(void)
{
    const char *const path = getenv("FCN_HELD_EXIT");

    if ((path == NULL) || atomic_flag_test_and_set(&s_ended)) {
        return;
    }

    /* Without waiting: a FIFO nobody reads fails with ENXIO. */
    const int fifo = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);

    if (fifo < 0) {
        say("FCN_HELD_EXIT cannot be opened for writing: no holder");
        return;
    }

    s_fifo = fifo;

    const pid_t holder = clone(hold, s_stack + sizeof(s_stack), CLONE_VM | SIGCHLD, NULL);

    if (holder < 0) {
        say("the memory map cannot be held (clone failed): no holder");
    } else {
        char line[32];
        const int len = snprintf(line, sizeof(line), "%ld\n", (long)holder);

        if (len > 0) {
            (void)!write(fifo, line, (size_t)len);
        }
    }

    (void)close(fifo);
}

void __wrap__exit(int status)
{
    hold_the_memory_map();
    __real__exit(status);
}

__attribute__((constructor)) static void hold_at_exit(void)
{
    (void)atexit(hold_the_memory_map);
}
