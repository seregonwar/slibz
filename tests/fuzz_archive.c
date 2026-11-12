/* slz - in-process fuzzer for the whole pipeline (POSIX; build with ASan + UBSan).
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 *   fuzz_archive <workdir> <iterations> <seed> <archive>...
 *
 * Each iteration mutates one seed archive (bit flips, byte sets, truncation, splices of another
 * seed, 32-bit "interesting" values at random offsets), then runs slz_open, the full listing,
 * slz_dir on every directory level, and slz_extract into <workdir>/out. Any memory error aborts
 * (sanitizers); afterwards the destination must contain no symlink and nothing may appear
 * outside it. */
#define _DEFAULT_SOURCE
#include "slz.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t rs;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

static int walk_cb(void *u, const char *name, size_t len, int is_dir, long long idx) {
    (void)u;
    (void)name;
    (void)len;
    (void)is_dir;
    (void)idx;
    return 0;
}

/* remove a tree without following links; fail loudly if a symlink was created */
static int clean(const char *p, int top) {
    struct stat st;
    int bad = 0;
    if (lstat(p, &st)) return 0;
    if (S_ISLNK(st.st_mode)) {
        fprintf(stderr, "SYMLINK CREATED: %s\n", p);
        bad = 1;
    } else if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            char q[4096];
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(q, sizeof q, "%s/%s", p, e->d_name);
            bad |= clean(q, 0);
        }
        if (d) closedir(d);
        if (!top) rmdir(p);
        return bad;
    }
    unlink(p);
    return bad;
}

int main(int argc, char **argv) {
    static const uint32_t interesting[] = {0,       1,          0x7f,       0x80,       0xff, 0x7fff, 0xffff,
                                           0x10000, 0x7fffffff, 0xffffffff, 0x80000000, 46,   512,    4096};
    char arc[4096], out[4096], outside[4096];
    long it, iters;
    int nseeds = argc - 4, s;
    uint8_t **seed;
    size_t *seedn;
    if (argc < 5) {
        fprintf(stderr, "usage: fuzz_archive <workdir> <iterations> <seed> <archive>...\n");
        return 2;
    }
    iters = atol(argv[2]);
    rs = (uint64_t)atoll(argv[3]) * 0x9e3779b97f4a7c15ull + 1;
    seed = (uint8_t **)calloc((size_t)nseeds, sizeof *seed);
    seedn = (size_t *)calloc((size_t)nseeds, sizeof *seedn);
    for (s = 0; s < nseeds; s++) {
        FILE *f = fopen(argv[4 + s], "rb");
        long n;
        if (!f) return 2;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        seedn[s] = (size_t)n;
        seed[s] = (uint8_t *)malloc((size_t)n + 1);
        if (fread(seed[s], 1, (size_t)n, f) != (size_t)n) return 2;
        fclose(f);
    }
    snprintf(out, sizeof out, "%s/sub/out", argv[1]);
    snprintf(outside, sizeof outside, "%s/sub", argv[1]);
    mkdir(argv[1], 0755);
    mkdir(outside, 0755);
    for (it = 0; it < iters; it++) {
        int k = (int)(rnd() % (unsigned)nseeds), m, nm = 1 + (int)(rnd() % 8), err, ext;
        size_t n = seedn[k];
        uint8_t *b = (uint8_t *)malloc(n + 64);
        FILE *f;
        slz_archive *a;
        memcpy(b, seed[k], n);
        for (m = 0; m < nm && n; m++) {
            size_t pos =
                rnd() % 2 ? rnd() % n : n - 1 - rnd() % (n < 4096 ? n : 4096); /* headers live at the ends */
            switch (rnd() % 6) {
                case 0:
                    b[pos] ^= (uint8_t)(1u << (rnd() % 8));
                    break;
                case 1:
                    b[pos] = (uint8_t)rnd();
                    break;
                case 2:
                    n = pos;
                    break; /* truncate */
                case 3:
                    if (pos + 4 <= n) {
                        uint32_t v = interesting[rnd() % (sizeof interesting / sizeof *interesting)];
                        memcpy(b + pos, &v, 4);
                    }
                    break;
                case 4: { /* splice bytes from another seed */
                    int o = (int)(rnd() % (unsigned)nseeds);
                    size_t len = rnd() % 64, from = seedn[o] ? rnd() % seedn[o] : 0;
                    if (len > n - pos) len = n - pos;
                    if (len > seedn[o] - from) len = seedn[o] - from;
                    memcpy(b + pos, seed[o] + from, len);
                    break;
                }
                default:
                    b[pos] = 0;
            }
        }
        snprintf(arc, sizeof arc, "%s/case.bin", argv[1]);
        f = fopen(arc, "wb");
        fwrite(b, 1, n, f);
        fclose(f);
        free(b);
        a = slz_open(arc, &err);
        if (a) {
            size_t i, cnt = slz_count(a);
            slz_opts o;
            for (i = 0; i < cnt; i++) { /* listing + every directory level */
                const slz_entry *e = slz_get(a, i);
                char d[4096];
                size_t l = e->nlen < sizeof d - 1 ? e->nlen : sizeof d - 1;
                memcpy(d, e->name, l);
                d[l] = 0;
                while (l && d[l - 1] == '/')
                    d[--l] = 0;
                slz_dir(a, d, walk_cb, NULL);
                if (i > 2000) break;
            }
            slz_dir(a, "", walk_cb, NULL);
            memset(&o, 0, sizeof o);
            o.threads = 1 + rnd() % 4;
            o.max_total = 64u << 20; /* bombs are fine to refuse */
            ext = slz_extract(a, out, &o);
            (void)ext;
            slz_close(a);
        }
        if (clean(out, 0)) return 1;
        {
            DIR *d = opendir(outside);
            struct dirent *e;
            while (d && (e = readdir(d)))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                    fprintf(stderr, "WROTE OUTSIDE DEST: %s (iteration %ld)\n", e->d_name, it);
                    return 1;
                }
            if (d) closedir(d);
        }
    }
    printf("fuzz_archive: %ld iterations ok\n", iters);
    for (s = 0; s < nseeds; s++)
        free(seed[s]);
    free(seed);
    free(seedn);
    return 0;
}
