/* In-memory DEFLATE decode throughput: slz vs zlib vs libdeflate.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 *   bench_inflate <file>...   (each file is compressed with zlib level 6, then decoded N times)
 *
 * Built with -DSLZ_BENCH_ZLIBNG and bench_inflate_zng.c, it adds a zlib-ng column that decodes
 * the same zlib streams. */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <libdeflate.h>
#include <stdio.h>
#include <time.h>
#include <zlib.h>

#ifdef SLZ_BENCH_ZLIBNG
#define NDEC 4
const char *bench_zng_version(void);
int bench_zng_inflate(const uint8_t *c, size_t cn, uint8_t *o, size_t n);
#else
#define NDEC 3
#endif

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

typedef struct {
    const uint8_t *in;
    size_t n, pos;
    uint8_t *out;
    size_t on;
} io;
static int64_t rd(void *u, uint8_t *b, size_t n) {
    io *s = (io *)u;
    size_t k = MIN(n, s->n - s->pos);
    memcpy(b, s->in + s->pos, k);
    s->pos += k;
    return (int64_t)k;
}
static int wr(void *u, const uint8_t *p, size_t n) {
    io *s = (io *)u;
    memcpy(s->out + s->on, p, n);
    s->on += n;
    return 0;
}

int main(int argc, char **argv) {
    int a;
    inf *z = inf_new();
    struct libdeflate_decompressor *ld = libdeflate_alloc_decompressor();
    double tot[NDEC] = {0};
    size_t totn = 0;
#ifdef SLZ_BENCH_ZLIBNG
    printf("zlib %s, libdeflate %s, zlib-ng %s\n", zlibVersion(), LIBDEFLATE_VERSION_STRING, bench_zng_version());
    printf("%-14s %10s %10s %10s %10s %10s   (MB/s of output, best of 5)\n", "file", "size", "slz", "zlib",
           "libdeflate", "zlib-ng");
#else
    printf("zlib %s, libdeflate %s\n", zlibVersion(), LIBDEFLATE_VERSION_STRING);
    printf("%-14s %10s %10s %10s %10s   (MB/s of output, best of 5)\n", "file", "size", "slz", "zlib",
           "libdeflate");
#endif
    for (a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        size_t n, cn;
        uint8_t *d, *c, *o;
        z_stream zs;
        double best[NDEC];
        int it, k;
        for (k = 0; k < NDEC; k++) best[k] = 1e9;
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        n = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        d = malloc(n);
        c = malloc(n + n / 100 + 1024);
        o = malloc(n + 64);
        if (fread(d, 1, n, f) != n) return 1;
        fclose(f);
        memset(&zs, 0, sizeof zs);
        deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        zs.next_in = d;
        zs.avail_in = (uInt)n;
        zs.next_out = c;
        zs.avail_out = (uInt)(n + n / 100 + 1024);
        deflate(&zs, Z_FINISH);
        cn = zs.total_out;
        deflateEnd(&zs);
        for (it = 0; it < 5; it++) {
            double t0;
            io s;
            size_t got;
            /* slz (streaming API, as used by the extractor) */
            s.in = c;
            s.n = cn;
            s.pos = 0;
            s.out = o;
            s.on = 0;
            t0 = now();
            inf_reset(z, 0, 0, NULL, 0, 0);
            z->rd = rd;
            z->wr = wr;
            z->ck = NULL;
            z->ru = z->wu = &s;
            if (inf_run(z) || s.on != n || memcmp(o, d, n)) {
                fprintf(stderr, "slz mismatch %s\n", argv[a]);
                return 1;
            }
            t0 = now() - t0;
            if (t0 < best[0]) best[0] = t0;
            /* zlib */
            memset(&zs, 0, sizeof zs);
            t0 = now();
            inflateInit2(&zs, -15);
            zs.next_in = c;
            zs.avail_in = (uInt)cn;
            zs.next_out = o;
            zs.avail_out = (uInt)n;
            inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            t0 = now() - t0;
            if (t0 < best[1]) best[1] = t0;
            /* libdeflate (whole buffer) */
            t0 = now();
            libdeflate_deflate_decompress(ld, c, cn, o, n, &got);
            t0 = now() - t0;
            if (t0 < best[2]) best[2] = t0;
#ifdef SLZ_BENCH_ZLIBNG
            t0 = now();
            k = bench_zng_inflate(c, cn, o, n);
            t0 = now() - t0;
            if (k) {
                fprintf(stderr, "zlib-ng error %s\n", argv[a]);
                return 1;
            }
            if (t0 < best[3]) best[3] = t0;
#endif
        }
        {
            const char *b = strrchr(argv[a], '/');
            printf("%-14s %10zu", b ? b + 1 : argv[a], n);
        }
        for (k = 0; k < NDEC; k++) {
            printf(" %10.0f", (double)n / best[k] / 1e6);
            tot[k] += best[k];
        }
        printf("\n");
        totn += n;
        free(d);
        free(c);
        free(o);
    }
    printf("%-14s %10zu", "TOTAL", totn);
    for (a = 0; a < NDEC; a++) printf(" %10.0f", totn / tot[a] / 1e6);
    printf("\n");
    return 0;
}
