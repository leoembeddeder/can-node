/* SPDX-License-Identifier: MIT */
/**
 * @file
 * SHA-256 as specified in FIPS 180-4 ("Secure Hash Standard", NIST, August 2015): the padding
 * of section 5.1.1, the initial hash value of section 5.3.3, the functions and constants of
 * sections 4.1.2 and 4.2.2 and the hash computation of section 6.2.2.
 *
 * STREAMING. A message is hashed in three steps: sha256_init(), any number of sha256_update()
 * calls with pieces of any length (0 included), and sha256_final(), which pads the message and
 * writes the 32-byte digest. The digest depends only on the concatenation of the pieces, not on
 * how the message was cut. sha256_compute() does all three for a message in one buffer.
 *
 * THE DIGEST is the 32 bytes of FIPS 180-4 section 6.2.2 step 4, H0 to H7, each word most
 * significant byte first: the bytes that hashlib.sha256().digest() returns and whose hex form
 * the standard's examples print.
 *
 * LENGTH. FIPS 180-4 hashes messages shorter than 2^64 bits. The context counts bytes in a
 * uint64_t, so any message the standard allows is counted exactly (2^61 bytes and more are
 * beyond the standard; their length would be taken modulo 2^64 bits).
 *
 * MEMORY. The context is the caller's memory. Beyond it, a call needs the stack for a message
 * schedule of 16 words (the last 16 W_t, not all 64: see sha256.c) and the eight working
 * variables. No dynamic allocation; the only static data are the constant tables.
 *
 * CONTEXT. The functions keep no state of their own, so any number of contexts may be used at
 * once from any tasks; one context must not be used by two tasks at the same time. Any task or
 * interrupt handler; a call takes time in proportion to its length (one compression per 64
 * bytes; sha256_final() at most two).
 */
#ifndef FCN_SHA256_H
#define FCN_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Bytes of one message block: the unit that the compression function consumes. */
#define SHA256_BLOCK_SIZE 64U

/** Bytes of a digest. */
#define SHA256_DIGEST_SIZE 32U

/** The state of one message being hashed. The members are private to sha256.c; the type is in
 *  the header only so that a context can be allocated statically or on the stack. */
typedef struct {
    uint32_t state[8];                /* H0..H7 after the blocks compressed so far */
    uint64_t bytes;                   /* message bytes given to sha256_update() so far */
    uint8_t block[SHA256_BLOCK_SIZE]; /* bytes of the next block received so far */
    uint32_t fill;                    /* how many of them: 0..63 between two calls */
} sha256_ctx_t;

/**
 * Begin a message: the initial hash value of FIPS 180-4 section 5.3.3, no bytes yet. The memory
 * of `ctx` may hold anything before. Returns false if `ctx` is NULL, true otherwise.
 */
bool sha256_init(sha256_ctx_t *ctx);

/**
 * Append `len` bytes at `data` to the message. `data` may be NULL when `len` is 0. Every
 * complete 64-byte block is compressed at once; the rest is kept in the context.
 *
 * Returns false, changing nothing, if `ctx` is NULL, `data` is NULL with `len` above 0, or the
 * context is not one that sha256_init() prepared (its fill count is beyond a block). Otherwise
 * true.
 */
bool sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);

/**
 * End the message: pad it (FIPS 180-4 section 5.1.1), compress the last one or two blocks and
 * write the 32-byte digest to `digest`. The context is then initialised again, as by
 * sha256_init(), and holds nothing of the message: the next update starts a new one.
 *
 * Returns false, changing nothing, if a pointer is NULL or the context is not one that
 * sha256_init() prepared. Otherwise true.
 */
bool sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

/**
 * The digest of the `len` bytes at `data` in one call: sha256_init(), sha256_update() and
 * sha256_final() on a context on the stack. `data` may be NULL when `len` is 0. Returns false,
 * leaving `digest` alone, if `digest` is NULL or `data` is NULL with `len` above 0.
 */
bool sha256_compute(const uint8_t *data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE]);

#endif /* FCN_SHA256_H */
