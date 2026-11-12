/* slz - in-memory LZMA2 decode throughput: slz vs liblzma.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 *   bench_lzma <file>...   (each file is compressed with liblzma LZMA2 preset 6, then decoded) */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <lzma.h>
#include <stdio.h>
#include <time.h>

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
    double tot[2] = {0, 0};
    size_t totn = 0;
    int a;
    printf("liblzma %s\n", lzma_version_string());
    printf("%-14s %10s %10s %10s   (MB/s of output, best of 3)\n", "file", "size", "slz", "liblzma");
    for (a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        size_t n, cn = 0;
        uint8_t *d, *c, *o;
        lzma_options_lzma opt;
        lzma_filter flt[2];
        double best[2] = {1e9, 1e9};
        int it;
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        n = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        d = malloc(n);
        c = malloc(n + n / 10 + 4096);
        o = malloc(n + 64);
        if (fread(d, 1, n, f) != n) return 1;
        fclose(f);
        lzma_lzma_preset(&opt, 6);
        flt[0].id = LZMA_FILTER_LZMA2;
        flt[0].options = &opt;
        flt[1].id = LZMA_VLI_UNKNOWN;
        if (lzma_raw_buffer_encode(flt, NULL, d, n, c, &cn, n + n / 10 + 4096) != LZMA_OK) return 1;
        for (it = 0; it < 3; it++) {
            double t0 = now();
            io s;
            size_t ip = 0, op = 0;
            lzd *z = lzd_new(opt.dict_size, n);
            s.in = c;
            s.n = cn;
            s.pos = 0;
            s.out = o;
            s.on = 0;
            lzd_io(z, rd, &s, wr, &s);
            if (lzd_lzma2(z, n) || s.on != n || memcmp(o, d, n)) {
                fprintf(stderr, "slz mismatch %s\n", argv[a]);
                return 1;
            }
            lzd_free(z);
            t0 = now() - t0;
            if (t0 < best[0]) best[0] = t0;
            t0 = now();
            lzma_raw_buffer_decode(flt, NULL, c, &ip, cn, o, &op, n);
            t0 = now() - t0;
            if (t0 < best[1]) best[1] = t0;
        }
        {
            const char *b = strrchr(argv[a], '/');
            printf("%-14s %10zu %10.0f %10.0f\n", b ? b + 1 : argv[a], n, n / best[0] / 1e6, n / best[1] / 1e6);
        }
        tot[0] += best[0];
        tot[1] += best[1];
        totn += n;
        free(d);
        free(c);
        free(o);
    }
    printf("%-14s %10zu %10.0f %10.0f\n", "TOTAL", totn, totn / tot[0] / 1e6, totn / tot[1] / 1e6);
    return 0;
}
