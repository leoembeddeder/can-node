/* SPDX-License-Identifier: MIT */
/**
 * @file
 * A keeper: a process that keeps another process's mappings of its files alive after that
 * process has let go of them or has ended. For the tests of a lock that must be free although
 * something still refers to the locked file's open file description.
 *
 * A flock lock belongs to the open file description, and a shared mapping of the file refers
 * to that description as the descriptor does. A process that ends while another process holds
 * its memory map for a moment (a reader of /proc/<pid>/maps or /proc/<pid>/stat does) leaves
 * its mappings, and with them the descriptions it locked, alive past its end, and a lock it did
 * not release itself is still held when its exit has been seen (docs/bug-log.md, 012). A
 * keeper is that state without chance, for as long as the test wants: it is forked by the
 * process once the files are mapped, so it has the mappings too; it closes every descriptor
 * but its two pipe ends, so that it refers to the files through the mappings only; and it
 * waits until the test lets it go.
 *
 * The test calls map_keeper_prepare() before it starts the processes whose mappings are to be
 * kept (they inherit the pipes), each of those calls map_keeper_fork() once its files are
 * mapped, and the test calls map_keeper_end() when it has looked at the files. A test that
 * ends without that call ends its keepers too: the pipe they wait on closes with it.
 */
#ifndef FCN_TEST_MAP_KEEPER_H
#define FCN_TEST_MAP_KEEPER_H

#include <stdbool.h>
#include <sys/types.h>

/** The pipes of a test's keepers. */
typedef struct {
    int let_go[2]; /**< the keepers wait for the end of this pipe */
    int gone[2];   /**< at its end once every keeper has ended */
} map_keeper_t;

/** Make the pipes. Returns false if the kernel gave none. The test, before it forks. */
bool map_keeper_prepare(map_keeper_t *keeper);

/**
 * Fork a keeper of the calling process's mappings as they are now. Returns the keeper's process
 * id, or -1 if the fork failed. The keeper never returns from this call. Call it with one
 * thread only, as any fork.
 */
pid_t map_keeper_fork(const map_keeper_t *keeper);

/** Let every keeper go and wait until all of them have ended. Returns false if that could not
 *  be seen. The test, once: the pipes are closed. */
bool map_keeper_end(const map_keeper_t *keeper);

#endif /* FCN_TEST_MAP_KEEPER_H */
