/* Differential fuzzer: slz LZMA / LZMA2 vs liblzma (reference). Build with ASan + UBSan.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 *   fuzz_lzma <iterations> [seed]
 * Random data of varied entropy is compressed by liblzma (random lc/lp/pb, dictionary, mode,
 * nice_len) and decoded by slz with random input chunk sizes: output must be identical.
 * Mutated LZMA2 streams must never crash, and whenever liblzma accepts one, slz must produce
 * the same output. */
#include "internal.h"
#include <lzma.h>
#include <stdio.h>

static uint64_t rs;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

typedef struct {
    const uint8_t *in;
    size_t n, pos, maxchunk;
    uint8_t *out;
    size_t on, cap;
} io;
static int64_t rd(void *u, uint8_t *b, size_t n) {
    io *s = (io *)u;
    size_t k = MIN(n, s->n - s->pos);
    if (s->maxchunk) {
        size_t c = 1 + rnd() % s->maxchunk;
        k = MIN(k, c);
    }
    memcpy(b, s->in + s->pos, k);
    s->pos += k;
    return (int64_t)k;
}
static int wr(void *u, const uint8_t *p, size_t n) {
    io *s = (io *)u;
    if (n > s->cap - s->on) return SLZ_E_LIMIT;
    memcpy(s->out + s->on, p, n);
    s->on += n;
    return 0;
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
                            : (uint8_t)(i > 500 ? d[i - 1 - rnd() % 500] : i);
    }
}

static int slz_dec(int lzma2, const uint8_t *props, const uint8_t *c, size_t cn, size_t n, uint8_t *out,
                   size_t cap, size_t *on, size_t maxchunk, uint32_t dict) {
    io s;
    int r;
    lzd *z = lzd_new(dict, n ? n : 1);
    if (!z) return SLZ_E_NOMEM;
    s.in = c;
    s.n = cn;
    s.pos = 0;
    s.maxchunk = maxchunk;
    s.out = out;
    s.on = 0;
    s.cap = cap;
    lzd_io(z, rd, &s, wr, &s);
    r = lzma2 ? lzd_lzma2(z, n) : lzd_lzma(z, props, n, 1);
    *on = s.on;
    lzd_free(z);
    return r;
}

int main(int argc, char **argv) {
    long it = argc > 1 ? atol(argv[1]) : 300, i, mut_ok = 0;
    size_t MAXN = 2u << 20, CAP = 4u << 20;
    uint8_t *d = (uint8_t *)malloc(MAXN), *c = (uint8_t *)malloc(CAP), *o1 = (uint8_t *)malloc(CAP),
            *o2 = (uint8_t *)malloc(CAP), *mb = (uint8_t *)malloc(CAP);
    rs = argc > 2 ? (uint64_t)atoll(argv[2]) : 0x2545f4914f6cdd1dull;
    for (i = 0; i < it; i++) {
        size_t n = rnd() % 4 == 0 ? rnd() % 100 : rnd() % MAXN, cn = 0, on1, mc;
        int lzma2 = (int)(rnd() % 2), r, m;
        lzma_options_lzma opt;
        lzma_filter f[2];
        uint8_t props[5];
        gen(d, n);
        lzma_lzma_preset(&opt, rnd() % 7);
        opt.dict_size = 4096u << (rnd() % 10);
        opt.lc = rnd() % 5;
        opt.lp = rnd() % (5 - opt.lc); /* liblzma: lc + lp <= 4 */
        opt.pb = rnd() % 5;
        opt.mode = rnd() % 2 ? LZMA_MODE_FAST : LZMA_MODE_NORMAL;
        opt.nice_len = 8 + rnd() % 265;
        f[0].id = lzma2 ? LZMA_FILTER_LZMA2 : LZMA_FILTER_LZMA1;
        f[0].options = &opt;
        f[1].id = LZMA_VLI_UNKNOWN;
        if (lzma_raw_buffer_encode(f, NULL, d, n, c, &cn, CAP) != LZMA_OK) {
            fprintf(stderr, "encode failed\n");
            return 2;
        }
        props[0] = (uint8_t)((opt.pb * 5 + opt.lp) * 9 + opt.lc);
        props[1] = (uint8_t)opt.dict_size;
        props[2] = (uint8_t)(opt.dict_size >> 8);
        props[3] = (uint8_t)(opt.dict_size >> 16);
        props[4] = (uint8_t)(opt.dict_size >> 24);
        mc = rnd() % 3 == 0 ? 0 : 1 + rnd() % (rnd() % 2 ? 8 : 100000);
        r = slz_dec(lzma2, props, c, cn, n, o1, CAP, &on1, mc, opt.dict_size);
        if (r || on1 != n || memcmp(o1, d, n)) {
            fprintf(stderr, "MISMATCH iter %ld lzma2=%d: r=%d n=%zu got=%zu lc=%u lp=%u pb=%u dict=%u\n", i,
                    lzma2, r, n, on1, opt.lc, opt.lp, opt.pb, opt.dict_size);
            return 1;
        }
        for (m = 0; m < 4; m++) { /* mutations (LZMA2: liblzma decides validity) */
            size_t mn = cn, k, flips = 1 + rnd() % 4, zin = 0, zout = 0;
            lzma_ret zr;
            int rl;
            memcpy(mb, c, cn);
            if (rnd() % 4 == 0 && cn) mn = rnd() % cn;
            for (k = 0; k < flips && mn; k++)
                mb[rnd() % mn] ^= (uint8_t)(1u << (rnd() % 8));
            rl = slz_dec(lzma2, props, mb, mn, n, o1, CAP, &on1, mc, opt.dict_size);
            if (!lzma2) continue; /* LZMA1 has no self-delimiting validity to compare */
            zr = lzma_raw_buffer_decode(f, NULL, mb, &zin, mn, o2, &zout, CAP);
            if (zr == LZMA_OK && zout == n && zin == mn) {
                mut_ok++;
                if (rl || on1 != zout || memcmp(o1, o2, on1)) {
                    fprintf(stderr, "DIFF iter %ld: liblzma ok (%zu) slz r=%d (%zu)\n", i, zout, rl, on1);
                    return 1;
                }
            }
        }
    }
    printf("fuzz_lzma: %ld streams ok, %ld mutated LZMA2 streams valid for both\n", it, mut_ok);
    free(d);
    free(c);
    free(o1);
    free(o2);
    free(mb);
    return 0;
}
