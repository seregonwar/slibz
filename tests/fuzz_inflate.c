/* Differential fuzzer: slz inflate vs zlib (reference). Build with -fsanitize=address,undefined.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 *   fuzz_inflate <iterations> [seed]
 * 1. random data of varied entropy, compressed by zlib with random level/strategy/window/memLevel,
 *    decoded by slz with random input chunk sizes (stresses refill + stored-block paths)
 *    -> output must be identical and the stream end exact.
 * 2. the same streams mutated (bit flips, truncation, splices) -> no crash; whenever zlib accepts,
 *    slz must accept with identical output. */
#include "internal.h"
#include <stdio.h>
#include <zlib.h>

static uint64_t rs;
static size_t last_read;
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
    } /* MIN evaluates twice */
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
        size_t run = 1 + rnd() % 2000, k;
        unsigned m = mode == 4 ? rnd() % 4 : mode;
        for (k = 0; k < run && i < n; k++, i++)
            d[i] = m == 0   ? (uint8_t)rnd()
                   : m == 1 ? (uint8_t)"abcdefgh"[rnd() % 8]
                   : m == 2 ? 0
                            : (uint8_t)(i > 300 ? d[i - 1 - rnd() % 300] : i);
    }
}

static int slz_inflate(inf *z, const uint8_t *c, size_t cn, uint8_t *out, size_t cap, size_t *on,
                       size_t maxchunk, uint64_t *end) {
    io s;
    int r;
    s.in = c;
    s.n = cn;
    s.pos = 0;
    s.maxchunk = maxchunk;
    s.out = out;
    s.on = 0;
    s.cap = cap;
    inf_reset(z, 0, 0, NULL, 0, 0);
    z->rd = rd;
    z->wr = wr;
    z->ck = NULL;
    z->ru = z->wu = &s;
    r = inf_run(z);
    *on = s.on;
    *end = inf_bitpos(z) / 8;
    last_read = s.pos;
    if (s.pos > cn) {
        fprintf(stderr, "reader overrun %zu > %zu\n", s.pos, cn);
        abort();
    }
    return r;
}

static const char *zmsg;
static int zlib_inflate(const uint8_t *c, size_t cn, uint8_t *out, size_t cap, size_t *on) {
    z_stream zs;
    int r;
    memset(&zs, 0, sizeof zs);
    inflateInit2(&zs, -15);
    zs.next_in = (Bytef *)c;
    zs.avail_in = (uInt)cn;
    zs.next_out = out;
    zs.avail_out = (uInt)cap;
    r = inflate(&zs, Z_FINISH);
    *on = zs.total_out;
    zmsg = zs.msg ? zs.msg : r == Z_BUF_ERROR ? "truncated (Z_BUF_ERROR)" : "?";
    inflateEnd(&zs);
    return r == Z_STREAM_END ? 0 : -1;
}

int main(int argc, char **argv) {
    long it = argc > 1 ? atol(argv[1]) : 1000, i, lenient = 0, mut_ok = 0;
    size_t MAXN = 3u << 20, CAP = 8u << 20;
    uint8_t *d = (uint8_t *)malloc(MAXN), *c = (uint8_t *)malloc(CAP), *o1 = (uint8_t *)malloc(CAP),
            *o2 = (uint8_t *)malloc(CAP);
    inf *z = inf_new();
    rs = argc > 2 ? (uint64_t)atoll(argv[2]) : 0x9e3779b97f4a7c15ull;
    for (i = 0; i < it; i++) {
        size_t n = rnd() % 4 == 0 ? rnd() % 64 : rnd() % (rnd() % 2 ? 70000 : MAXN), cn, on1, on2, mc;
        uint64_t end;
        z_stream zs;
        int r, m;
        gen(d, n);
        memset(&zs, 0, sizeof zs);
        deflateInit2(&zs, (int)(rnd() % 10), Z_DEFLATED, -(int)(9 + rnd() % 7), (int)(1 + rnd() % 9),
                     (int)(rnd() % 5) /* default, filtered, huffman-only, rle, fixed */);
        zs.next_in = d;
        zs.avail_in = (uInt)n;
        zs.next_out = c;
        zs.avail_out = (uInt)CAP;
        if (rnd() % 3 == 0) { /* full/sync flushes create stored + empty blocks mid stream */
            size_t half = n / 2;
            zs.avail_in = (uInt)half;
            deflate(&zs, rnd() % 2 ? Z_FULL_FLUSH : Z_SYNC_FLUSH);
            zs.avail_in = (uInt)(n - half);
        }
        deflate(&zs, Z_FINISH);
        cn = zs.total_out;
        deflateEnd(&zs);
        mc = rnd() % 3 == 0 ? 0 : 1 + rnd() % (rnd() % 2 ? 16 : 70000);
        r = slz_inflate(z, c, cn, o1, CAP, &on1, mc, &end);
        if (r || on1 != n || memcmp(o1, d, n) || end != cn) {
            fprintf(stderr, "MISMATCH iter %ld: r=%d n=%zu got=%zu end=%llu cn=%zu chunk=%zu\n", i, r, n, on1,
                    (unsigned long long)end, cn, mc);
            return 1;
        }
        for (m = 0; m < 4; m++) { /* mutations */
            size_t mn = cn, k, flips = 1 + rnd() % 4;
            uint8_t *x = o2; /* reuse as mutation buffer; outputs go to d-sized buffers below */
            static uint8_t mbuf[8u << 20];
            int rz, rl;
            memcpy(mbuf, c, cn);
            if (rnd() % 4 == 0 && cn) mn = rnd() % cn;
            for (k = 0; k < flips && mn; k++)
                mbuf[rnd() % mn] ^= (uint8_t)(1u << (rnd() % 8));
            rz = zlib_inflate(mbuf, mn, x, CAP, &on2);
            rl = slz_inflate(z, mbuf, mn, o1, CAP, &on1, mc, &end);
            if (rz == 0 && (rl != 0 || on1 != on2 || memcmp(o1, x, on1))) {
                fprintf(stderr, "DIFF iter %ld mut %d: zlib ok (%zu bytes) slz r=%d (%zu bytes)\n", i, m, on2,
                        rl, on1);
                return 1;
            }
            if (rz && !rl && lenient++ < 3 && getenv("FUZZ_VERBOSE"))
                fprintf(stderr,
                        "lenient: zlib says '%s' (slz output %zu, zlib %zu) input=%zu slz_end=%llu pad=%u "
                        "bn=%u read=%zu ibase=%llu\n",
                        zmsg, on1, on2, mn, (unsigned long long)end, z->pad, z->bn, last_read,
                        (unsigned long long)z->ibase);
            if (!rz) mut_ok++;
        }
    }
    printf("fuzz_inflate: %ld streams ok, %ld mutated streams valid for both, %ld accepted only by slz\n", it,
           mut_ok, lenient);
    free(d);
    free(c);
    free(o1);
    free(o2);
    free(z);
    return 0;
}
