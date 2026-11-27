/* SPDX-License-Identifier: MIT */
/*
 * fcn_boot_held: the bootloader (platform/posix/boot_main.c, the same source) linked with
 * -Wl,--wrap=fexecve, so that its exec of the image goes through __wrap_fexecve() below. That
 * keeps the bootloader's old memory map alive past the exec, every time, as a process that reads
 * /proc/<pid>/stat or /proc/<pid>/maps of the bootloader at the moment of its exec does now and
 * then (docs/bug-log.md, 010): it starts a process that shares the memory map (clone() with
 * CLONE_VM) and holds it until the image has ended. The old map's shared mappings of the flash
 * file and of the retained cell then exist while the image opens and locks both files, and with
 * them the open file descriptions that the bootloader locked. So the image can lock the files
 * only if the bootloader released its locks before the exec (posix_flash_unlock(),
 * posix_retained_unlock()). For tests/system/test_boot.py.
 *
 * THE HOLDER is a process of its own (no CLONE_THREAD, and no CLONE_FILES: its descriptor table
 * is a copy of the bootloader's) that shares the bootloader's memory map. It closes every
 * descriptor of its copy but the read end of a pipe, so that it refers to the two files only
 * through the memory map, and reads from the pipe until its write end is closed. The write end
 * is not close-on-exec: the image inherits it, and it is closed when the image ends (or when the
 * bootloader ends, if the exec failed). Then the holder ends, and the old memory map with it.
 * The holder runs on a stack of its own in the shared map but with the bootloader's thread
 * pointer, so the C library's and the sanitizers' per-thread state it would see is the
 * bootloader's, which goes on running until its exec: the holder makes raw system calls only,
 * and the sanitizers do not instrument it.
 *
 * If the holder cannot be started, there is no exec: fexecve() fails with the error, and the
 * bootloader ends with 42. A test that expects the image to run then fails, as it should,
 * instead of passing without the old memory map held.
 */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The names are fixed by the linker's --wrap convention. */
int __real_fexecve(int fd, char *const argv[], char *const envp[]);
int __wrap_fexecve(int fd, char *const argv[], char *const envp[]);

#define HOLDER_STACK_SIZE (64U * 1024U)

static _Alignas(16) unsigned char s_stack[HOLDER_STACK_SIZE];
static int s_read_end = -1; /* the pipe's ends, as the holder's copy of the table has them */
static int s_write_end = -1;

__attribute__((no_sanitize("address", "undefined"))) static int hold(void *unused)
{
    unsigned char byte = 0U;
    const unsigned int keep = (unsigned int)s_read_end;

    (void)unused;

    /* The write end first: if close_range() is missing (Linux before 5.9), the holder keeps its
     * copies of the other descriptors, which refer to the files as well, but still ends. */
    (void)syscall(SYS_close, s_write_end);

    if (keep > 0U) {
        (void)syscall(SYS_close_range, 0U, keep - 1U, 0U);
    }

    (void)syscall(SYS_close_range, keep + 1U, ~0U, 0U);

    /* 0 once every write end is closed. An error ends the hold early as well (only a signal
     * with a handler could interrupt the read, and none reaches the holder). */
    while (syscall(SYS_read, s_read_end, &byte, 1U) > 0) {
        /* nothing is written into the pipe */
    }

    return 0;
}

int __wrap_fexecve(int fd, char *const argv[], char *const envp[])
{
    int ends[2];

    if (pipe(ends) != 0) {
        const int saved = errno;

        (void)fprintf(stderr, "fcn_boot_held: pipe: %s\n", strerror(saved));
        errno = saved;
        return -1;
    }

    s_read_end = ends[0];
    s_write_end = ends[1];

    /* Only the write end goes to the image. */
    if ((fcntl(ends[0], F_SETFD, FD_CLOEXEC) != 0) ||
        (clone(hold, s_stack + sizeof(s_stack), CLONE_VM | SIGCHLD, NULL) < 0)) {
        const int saved = errno;

        (void)close(ends[0]);
        (void)close(ends[1]);
        (void)fprintf(stderr, "fcn_boot_held: the old memory map cannot be held: %s\n",
                      strerror(saved));
        errno = saved;
        return -1;
    }

    (void)close(ends[0]);
    return __real_fexecve(fd, argv, envp);
}
