/* Differential fuzzer: slz inflate vs zlib (reference). Build with -fsanitize=address,undefined.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 *   fuzz_inflate <iterations> [seed]
 * 1. random data of varied entropy, compressed by zlib with random level/strategy/window/memLevel,
 *    decoded by slz with random input chunk sizes (stresses refill + stored-block paths)
 *    -> output must be identical and the stream end exact.
 * 2. resume, as destructive extraction does it: block-boundary checkpoints (bit position, output
 *    so far) recorded through the ck hook; decoding restarted from a random one with inf_reset
 *    (byte offset, skip bits, last 32 KiB of output as history) must produce the rest exactly.
 * 3. the same streams mutated (bit flips, truncation, splices) -> no crash; whenever zlib accepts,
 *    slz must accept with identical output.
 * 4. slz_inflate (in memory, zero-copy) on every stream: exact-size input and output buffers, with
 *    output slack and trailing input, one byte short (-> SLZ_E_LIMIT); on mutated streams it must
 *    agree with the streaming decoder. */
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
/* separate generator for decoder-driven choices (read chunk sizes, checkpoint sampling): how
 * often the decoder reads or reaches a block boundary must not shift the main sequence, so runs
 * of different decoder versions with the same seed see the same data and mutations */
static uint32_t dr = 1;
static uint32_t drnd(void) {
    dr = dr * 1103515245u + 12345u;
    return dr >> 8;
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
        size_t c = 1 + drnd() % s->maxchunk;
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

/* block-boundary checkpoints of the last decode (ck hook) */
static uint64_t ckbit[64], ckout[64];
static unsigned nck, ckseen;
static int ck(void *u, inf *z) {
    unsigned k = ckseen++;
    (void)u;
    if (k >= 64) /* reservoir sample once the table is full */
        k = drnd() % ckseen;
    if (k < 64) {
        ckbit[k] = inf_bitpos(z);
        ckout[k] = z->obase + z->op;
        if (k >= nck) nck = k + 1;
    }
    return 0;
}

/* decode cn bytes of c starting at a checkpoint: input byte bit/8 after skipping bit%8 bits,
 * history = the hn output bytes before `out` */
static int slz_resume(inf *z, const uint8_t *c, size_t cn, uint64_t bit, const uint8_t *hist, size_t hn,
                      uint64_t out, uint8_t *o, size_t cap, size_t *on, size_t maxchunk) {
    io s;
    int r;
    s.in = c;
    s.n = cn;
    s.pos = (size_t)(bit / 8);
    s.maxchunk = maxchunk;
    s.out = o;
    s.on = 0;
    s.cap = cap;
    inf_reset(z, bit / 8, (unsigned)(bit % 8), hist, hn, out);
    z->rd = rd;
    z->wr = wr;
    z->ck = NULL;
    z->ru = z->wu = &s;
    r = inf_run(z);
    *on = s.on;
    return r;
}

static int slz_stream(inf *z, const uint8_t *c, size_t cn, uint8_t *out, size_t cap, size_t *on,
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
    z->ck = ck;
    nck = ckseen = 0;
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
    long it = argc > 1 ? atol(argv[1]) : 1000, i, lenient = 0, mut_ok = 0, resumed = 0, direct = 0;
    size_t MAXN = 3u << 20, CAP = 8u << 20;
    uint8_t *d = (uint8_t *)malloc(MAXN), *c = (uint8_t *)malloc(CAP), *o1 = (uint8_t *)malloc(CAP),
            *o2 = (uint8_t *)malloc(CAP), *o3 = (uint8_t *)malloc(CAP);
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
        r = slz_stream(z, c, cn, o1, CAP, &on1, mc, &end);
        if (r || on1 != n || memcmp(o1, d, n) || end != cn) {
            fprintf(stderr, "MISMATCH iter %ld: r=%d n=%zu got=%zu end=%llu cn=%zu chunk=%zu\n", i, r, n, on1,
                    (unsigned long long)end, cn, mc);
            return 1;
        }
        { /* in-memory decode: exact-size input and output buffers (sanitizers catch any access past
           * either), then with slack, then one byte short */
            uint8_t *ci = (uint8_t *)malloc(cn + !cn), *ob = (uint8_t *)malloc(n + !n);
            size_t used, got, slack = drnd() % 64;
            memcpy(ci, c, cn);
            r = slz_inflate(ci, cn, ob, n, &used, &got);
            if (r || got != n || used != cn || memcmp(ob, d, n)) {
                fprintf(stderr, "DIRECT MISMATCH iter %ld: r=%d n=%zu got=%zu used=%zu cn=%zu\n", i, r, n, got,
                        used, cn);
                return 1;
            }
            r = slz_inflate(c, cn + (drnd() % 2) * 16, o2, n + slack, &used, &got); /* + trailing bytes */
            if (r || got != n || used != cn || memcmp(o2, d, n)) {
                fprintf(stderr, "DIRECT SLACK MISMATCH iter %ld: r=%d n=%zu got=%zu used=%zu slack=%zu\n", i, r,
                        n, got, used, slack);
                return 1;
            }
            if (n && (r = slz_inflate(ci, cn, ob, n - 1, &used, &got)) != SLZ_E_LIMIT) {
                fprintf(stderr, "DIRECT LIMIT iter %ld: r=%d n=%zu\n", i, r, n);
                return 1;
            }
            free(ci);
            free(ob);
            direct++;
        }
        if (nck) { /* resume from a random block boundary */
            unsigned k = rnd() % nck;
            size_t out = (size_t)ckout[k], hn = out < 32768 ? out : 32768;
            if (out > n || ckbit[k] > (uint64_t)cn * 8) {
                fprintf(stderr, "BAD CHECKPOINT iter %ld: bit=%llu out=%zu\n", i, (unsigned long long)ckbit[k], out);
                return 1;
            }
            r = slz_resume(z, c, cn, ckbit[k], d + out - hn, hn, out, o1, CAP, &on1, mc);
            if (r || on1 != n - out || memcmp(o1, d + out, on1) || inf_bitpos(z) / 8 != cn) {
                fprintf(stderr, "RESUME MISMATCH iter %ld: r=%d bit=%llu out=%zu got=%zu want=%zu\n", i, r,
                        (unsigned long long)ckbit[k], out, on1, n - out);
                return 1;
            }
            resumed++;
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
            rl = slz_stream(z, mbuf, mn, o1, CAP, &on1, mc, &end);
            if (rz == 0 && (rl != 0 || on1 != on2 || memcmp(o1, x, on1))) {
                fprintf(stderr, "DIFF iter %ld mut %d: zlib ok (%zu bytes) slz r=%d (%zu bytes)\n", i, m, on2,
                        rl, on1);
                return 1;
            }
            { /* in-memory decode of the exact-size mutated input must agree with the streaming one
               * (the error may differ only when one of them ran out of output space first) */
                uint8_t *ci = (uint8_t *)malloc(mn + !mn);
                size_t used, got;
                int rd2;
                memcpy(ci, mbuf, mn);
                rd2 = slz_inflate(ci, mn, o3, CAP, &used, &got);
                if ((rd2 == 0) != (rl == 0) || (!rl && (got != on1 || used != end || memcmp(o3, o1, got))) ||
                    (rl && rd2 != rl && rl != SLZ_E_LIMIT && rd2 != SLZ_E_LIMIT)) {
                    fprintf(stderr, "DIRECT DIFF iter %ld mut %d: stream r=%d (%zu bytes) direct r=%d (%zu bytes)\n",
                            i, m, rl, on1, rd2, got);
                    return 1;
                }
                if (!rl && (rd2 = slz_inflate(ci, mn, o3, on1, &used, &got)) != 0) {
                    fprintf(stderr, "DIRECT EXACT iter %ld mut %d: r=%d (%zu bytes)\n", i, m, rd2, on1);
                    return 1;
                }
                free(ci);
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
    printf("fuzz_inflate: %ld streams ok (%ld resumed from a checkpoint, %ld also in memory), %ld mutated "
           "streams valid for both, %ld accepted only by slz\n",
           it, resumed, direct, mut_ok, lenient);
    free(d);
    free(c);
    free(o1);
    free(o2);
    free(o3);
    free(z);
    return 0;
}
