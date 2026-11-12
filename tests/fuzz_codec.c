/* slz - differential fuzzer for the .slz codecs against the reference libraries.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 *   fuzz_codec <iterations> [seed]       (link with -llz4 -lxxhash; ASan + UBSan recommended)
 *
 * - slz LZ4 encoder -> liblz4 decoder, and liblz4 encoder -> slz LZ4 decoder: identical data.
 * - mutated LZ4 blocks: the slz decoder never crashes; whenever liblz4 decodes exactly n bytes,
 *   slz must accept and produce the same bytes.
 * - xxHash64 one-shot and streaming (random chunking) equal libxxhash XXH64. */
#include "internal.h"
#include <lz4.h>
#include <stdio.h>
#include <xxhash.h>

static uint64_t rs;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

static void gen(uint8_t *d, size_t n) {
    size_t i = 0;
    unsigned mode = rnd() % 5;
    while (i < n) {
        size_t run = 1 + rnd() % 3000, k;
        unsigned m = mode == 4 ? rnd() % 4 : mode;
        for (k = 0; k < run && i < n; k++, i++)
            d[i] = m == 0   ? (uint8_t)rnd()
                   : m == 1 ? (uint8_t)"abcdefgh"[rnd() % 8]
                   : m == 2 ? 0
                            : (uint8_t)(i > 300 ? d[i - 1 - rnd() % 300] : i);
    }
}

/* 1 if the block contains a match with offset 0: invalid per the LZ4 specification, rejected by
 * slz, but accepted by liblz4's decoder (its fast path does not check it) */
static int has_zero_offset(const uint8_t *p, size_t n) {
    const uint8_t *e = p + n;
    while (p < e) {
        unsigned tok = *p++;
        size_t lit = tok >> 4, b;
        if (lit == 15)
            do {
                if (p >= e) return 0;
                b = *p++;
                lit += b;
            } while (b == 255);
        if (lit >= (size_t)(e - p)) return 0;
        p += lit;
        if (e - p < 2) return 0;
        if (!(p[0] | p[1])) return 1;
        p += 2;
        if ((tok & 15) == 15)
            do {
                if (p >= e) return 0;
                b = *p++;
            } while (b == 255);
    }
    return 0;
}

int main(int argc, char **argv) {
    long it = argc > 1 ? atol(argv[1]) : 1000, i, accepted = 0;
    size_t MAXN = 3u << 20;
    uint8_t *d = (uint8_t *)malloc(MAXN), *c = (uint8_t *)malloc(lz4_bound(MAXN)), *o = (uint8_t *)malloc(MAXN + 1),
            *o2 = (uint8_t *)malloc(MAXN + 1);
    rs = argc > 2 ? (uint64_t)atoll(argv[2]) * 0x9e3779b97f4a7c15ull + 1 : 88172645463325252ull;
    for (i = 0; i < it; i++) {
        size_t n = rnd() % 4 == 0 ? rnd() % 64 : rnd() % (rnd() % 2 ? 70000 : MAXN), cn, k;
        int m;
        xxh64_t st;
        uint64_t seed = rnd() % 3 ? 0 : rnd();
        gen(d, n);
        /* xxHash64 */
        if (xxh64(d, n, seed) != XXH64(d, n, seed)) {
            fprintf(stderr, "XXH64 MISMATCH iter %ld n=%zu\n", i, n);
            return 1;
        }
        xxh64_init(&st, seed);
        for (k = 0; k < n;) {
            size_t step = 1 + rnd() % (rnd() % 2 ? 40 : 70000);
            if (step > n - k) step = n - k;
            xxh64_update(&st, d + k, step);
            k += step;
        }
        if (xxh64_digest(&st) != XXH64(d, n, seed)) {
            fprintf(stderr, "XXH64 STREAMING MISMATCH iter %ld n=%zu\n", i, n);
            return 1;
        }
        /* slz encoder -> liblz4 decoder */
        cn = lz4_compress(d, n, c);
        if (cn > lz4_bound(n) || LZ4_decompress_safe((const char *)c, (char *)o, (int)cn, (int)n) != (int)n ||
            memcmp(o, d, n)) {
            fprintf(stderr, "ENCODER MISMATCH iter %ld n=%zu cn=%zu\n", i, n, cn);
            return 1;
        }
        if (lz4_decompress(c, cn, o, n) || memcmp(o, d, n)) {
            fprintf(stderr, "ROUND TRIP MISMATCH iter %ld n=%zu\n", i, n);
            return 1;
        }
        /* liblz4 encoder -> slz decoder */
        cn = (size_t)LZ4_compress_default((const char *)d, (char *)c, (int)n, (int)lz4_bound(n));
        if (lz4_decompress(c, cn, o, n) || memcmp(o, d, n)) {
            fprintf(stderr, "DECODER MISMATCH iter %ld n=%zu\n", i, n);
            return 1;
        }
        /* mutations */
        for (m = 0; m < 4 && cn; m++) {
            size_t mn = cn, f;
            int ref, mine;
            uint8_t *mb = (uint8_t *)malloc(cn);
            memcpy(mb, c, cn);
            if (rnd() % 4 == 0) mn = rnd() % cn;
            for (f = 0; f < 1 + rnd() % 4 && mn; f++)
                mb[rnd() % mn] ^= (uint8_t)(1u << (rnd() % 8));
            mine = lz4_decompress(mb, mn, o, n);
            ref = LZ4_decompress_safe((const char *)mb, (char *)o2, (int)mn, (int)n);
            if (ref == (int)n && !has_zero_offset(mb, mn)) {
                accepted++;
                if (mine || memcmp(o, o2, n)) {
                    FILE *df = fopen("/tmp/lz4diff.bin", "wb");
                    if (df) {
                        fwrite(mb, 1, mn, df);
                        fclose(df);
                    }
                    fprintf(stderr, "DIFF iter %ld: liblz4 decodes %d bytes, slz r=%d (n=%zu, input %zu of %zu)\n", i,
                            ref, mine, n, mn, cn);
                    return 1;
                }
            }
            free(mb);
        }
    }
    printf("fuzz_codec: %ld iterations ok (LZ4 both directions, xxHash64), %ld mutated blocks valid for both\n", it,
           accepted);
    free(d);
    free(c);
    free(o);
    free(o2);
    return 0;
}
