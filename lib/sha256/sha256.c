/* SPDX-License-Identifier: MIT */
#include "sha256/sha256.h"

#include <string.h>

/*
 * FIPS 180-4, section by section. The names follow the standard: a..h are the working
 * variables, W_t the message schedule, K_t the constants, H the hash value.
 *
 * Words are read from and written to bytes one byte at a time, most significant byte first
 * (the standard's big-endian convention, section 3.1 item 3), so the result does not depend on
 * the byte order of the machine or on the alignment of the caller's buffers.
 */

/* The block buffer of the context is indexed with the fill count: if a context was not
 * prepared by sha256_init() and its fill count is beyond a block, update and final refuse it
 * instead of writing past the buffer. */
#define FILL_MAX (SHA256_BLOCK_SIZE - 1U)

/* Bytes of a block that the message and its first padding byte may use before the 64-bit
 * length (section 5.1.1: l + 1 + k = 448 mod 512 bits; 448 bits are 56 bytes). */
#define LENGTH_OFFSET 56U

_Static_assert(SHA256_BLOCK_SIZE == 64U, "512-bit blocks, FIPS 180-4 section 1");
_Static_assert(SHA256_DIGEST_SIZE == 32U, "eight 32-bit words H0..H7");
_Static_assert((LENGTH_OFFSET + 8U) == SHA256_BLOCK_SIZE, "the 64-bit length ends the block");

/* Section 4.2.2: the first 32 bits of the fractional parts of the cube roots of the first 64
 * prime numbers. */
static const uint32_t s_k[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U,
    0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU,
    0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU,
    0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
    0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
    0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U,
    0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U,
    0xC67178F2U,
};

/* Section 5.3.3: the first 32 bits of the fractional parts of the square roots of the first
 * eight prime numbers. */
static const uint32_t s_h0[8] = {
    0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
    0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
};

/* Section 3.2: ROTR^n(x), for 0 < n < 32 (every caller passes a constant in that range, so
 * neither shift reaches the width of the word). */
static uint32_t rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32U - n));
}

/* Section 4.1.2, equations 4.2 to 4.7. */
static uint32_t ch(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ ((~x) & z);
}

static uint32_t maj(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t big_sigma0(uint32_t x)
{
    return rotr(x, 2U) ^ rotr(x, 13U) ^ rotr(x, 22U);
}

static uint32_t big_sigma1(uint32_t x)
{
    return rotr(x, 6U) ^ rotr(x, 11U) ^ rotr(x, 25U);
}

static uint32_t small_sigma0(uint32_t x)
{
    return rotr(x, 7U) ^ rotr(x, 18U) ^ (x >> 3U);
}

static uint32_t small_sigma1(uint32_t x)
{
    return rotr(x, 17U) ^ rotr(x, 19U) ^ (x >> 10U);
}

/* The word at bytes[at..at + 3], most significant byte first. */
static uint32_t load_be32(const uint8_t *bytes, uint32_t at)
{
    return ((uint32_t)bytes[at] << 24U) | ((uint32_t)bytes[at + 1U] << 16U) |
           ((uint32_t)bytes[at + 2U] << 8U) | (uint32_t)bytes[at + 3U];
}

/* `word` into bytes[at..at + 3], most significant byte first. */
static void store_be32(uint8_t *bytes, uint32_t at, uint32_t word)
{
    bytes[at] = (uint8_t)(word >> 24U);
    bytes[at + 1U] = (uint8_t)(word >> 16U);
    bytes[at + 2U] = (uint8_t)(word >> 8U);
    bytes[at + 3U] = (uint8_t)word;
}

/* Section 6.2.2, steps 1 to 4, for one 64-byte block.
 *
 * Step 1 defines W_0..W_63, but W_t for t >= 16 needs only W_{t-2}, W_{t-7}, W_{t-15} and
 * W_{t-16}, all among the 16 words before it. So the schedule is kept as a window of 16 words,
 * W_t in w[t mod 16]: when W_t is computed, w[t mod 16] still holds W_{t-16}, the last word it
 * needs from there, and W_t takes its place. This is the same computation in 64 bytes of stack
 * instead of 256. */
static void compress(uint32_t hash[8], const uint8_t *block)
{
    uint32_t w[16];
    uint32_t a = hash[0];
    uint32_t b = hash[1];
    uint32_t c = hash[2];
    uint32_t d = hash[3];
    uint32_t e = hash[4];
    uint32_t f = hash[5];
    uint32_t g = hash[6];
    uint32_t h = hash[7];

    for (uint32_t t = 0U; t < 64U; t++) {
        uint32_t wt;

        if (t < 16U) {
            wt = load_be32(block, 4U * t);
        } else {
            wt = small_sigma1(w[(t - 2U) & 15U]) + w[(t - 7U) & 15U] +
                 small_sigma0(w[(t - 15U) & 15U]) + w[t & 15U];
        }

        w[t & 15U] = wt;

        const uint32_t t1 = h + big_sigma1(e) + ch(e, f, g) + s_k[t] + wt;
        const uint32_t t2 = big_sigma0(a) + maj(a, b, c);

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
}

bool sha256_init(sha256_ctx_t *ctx)
{
    const bool ok = (ctx != NULL);

    if (ok) {
        for (uint32_t i = 0U; i < 8U; i++) {
            ctx->state[i] = s_h0[i];
        }

        ctx->bytes = 0U;
        (void)memset(ctx->block, 0, sizeof(ctx->block));
        ctx->fill = 0U;
    }

    return ok;
}

bool sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    const bool ok = (ctx != NULL) && ((data != NULL) || (len == 0U)) && (ctx->fill <= FILL_MAX);

    if (ok) {
        size_t done = 0U;

        ctx->bytes += (uint64_t)len;

        /* Bytes go into the block buffer until it is full, and a full buffer is compressed at
         * once, so between two calls the buffer is never full. */
        while (done < len) {
            size_t take = (size_t)SHA256_BLOCK_SIZE - (size_t)ctx->fill;

            if (take > (len - done)) {
                take = len - done;
            }

            (void)memcpy(&ctx->block[ctx->fill], &data[done], take);
            ctx->fill += (uint32_t)take;
            done += take;

            if (ctx->fill == SHA256_BLOCK_SIZE) {
                compress(ctx->state, ctx->block);
                ctx->fill = 0U;
            }
        }
    }

    return ok;
}

bool sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
    const bool ok = (ctx != NULL) && (digest != NULL) && (ctx->fill <= FILL_MAX);

    if (ok) {
        /* Section 5.1.1: l, the length of the message in bits, as a 64-bit number. */
        uint64_t bits = ctx->bytes << 3U;

        /* The "1" bit and the first seven of the k "0" bits. There is always room for it: the
         * buffer is never full between two calls. */
        ctx->block[ctx->fill] = 0x80U;
        ctx->fill++;

        /* No room left for the length in this block (more than 55 message bytes in it): the
         * zeros fill it up, and the length goes into a block of its own. */
        if (ctx->fill > LENGTH_OFFSET) {
            (void)memset(&ctx->block[ctx->fill], 0, (size_t)SHA256_BLOCK_SIZE - ctx->fill);
            compress(ctx->state, ctx->block);
            ctx->fill = 0U;
        }

        (void)memset(&ctx->block[ctx->fill], 0, (size_t)LENGTH_OFFSET - ctx->fill);

        /* l, most significant byte first: the last byte of the block is its lowest. */
        for (uint32_t i = SHA256_BLOCK_SIZE; i > LENGTH_OFFSET; i--) {
            ctx->block[i - 1U] = (uint8_t)bits;
            bits >>= 8U;
        }

        compress(ctx->state, ctx->block);

        /* Section 6.2.2: the digest is H0 || H1 || ... || H7. */
        for (uint32_t i = 0U; i < 8U; i++) {
            store_be32(digest, 4U * i, ctx->state[i]);
        }

        (void)sha256_init(ctx);
    }

    return ok;
}

bool sha256_compute(const uint8_t *data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE])
{
    sha256_ctx_t ctx;
    const bool ok = (digest != NULL) && ((data != NULL) || (len == 0U));

    if (ok) {
        /* With these arguments none of the three can refuse. */
        (void)sha256_init(&ctx);
        (void)sha256_update(&ctx, data, len);
        (void)sha256_final(&ctx, digest);
    }

    return ok;
}
