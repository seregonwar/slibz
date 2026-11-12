/* slz command line tool.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 *   slz l <archive> [dir]         list entries, or the direct children of dir
 *   slz x <archive> <dest> [opts] extract: -o overwrite, -d destructive, -j N threads,
 *                                 -c MiB checkpoint interval, -n no mtime, -q quiet */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "slz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static double now(void) {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / (double)f.QuadPart;
}
#else
#include <time.h>
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
#endif

static int print_child(void *user, const char *name, size_t len, int is_dir, long long idx) {
    slz_archive *a = (slz_archive *)user;
    const slz_entry *e = idx >= 0 ? slz_get(a, (size_t)idx) : NULL;
    printf("%c %12llu  %.*s%s\n", is_dir ? 'd' : '-', e ? (unsigned long long)e->usize : 0ULL, (int)len, name,
           is_dir ? "/" : "");
    return 0;
}

static int usage(void) {
    fprintf(stderr, "usage: slz l <archive> [dir]\n"
                    "       slz x <archive> <dest> [-o] [-d] [-n] [-q] [-j threads] [-c checkpoint_MiB]\n"
                    "       slz a <archive.slz> <file|dir>... [-l zstd_level] [-b block_MiB] [-j threads]\n"
                    "             [--lz4 | --store] [--no-dedup] [-q]\n");
    return 2;
}

/* slz a: create a .slz archive */
static int create(int argc, char **argv) {
    slz_create_opts o;
    const char **in = (const char **)calloc((size_t)argc, sizeof *in);
    size_t n = 0;
    int i, err, quiet = 0;
    double t0 = now();
    memset(&o, 0, sizeof o);
    if (!in) return 1;
    for (i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-l") && i + 1 < argc)
            o.level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc)
            o.block_size = (uint32_t)atoi(argv[++i]) << 20;
        else if (!strcmp(argv[i], "-j") && i + 1 < argc)
            o.threads = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lz4"))
            o.codec = SLZ_CODEC_LZ4;
        else if (!strcmp(argv[i], "--store"))
            o.codec = SLZ_CODEC_STORE;
        else if (!strcmp(argv[i], "--no-dedup"))
            o.flags |= SLZ_NO_DEDUP;
        else if (!strcmp(argv[i], "-q"))
            quiet = 1;
        else if (argv[i][0] == '-' && argv[i][1]) {
            free(in);
            return usage();
        } else
            in[n++] = argv[i];
    }
    if (!n) {
        free(in);
        return usage();
    }
    err = slz_create(argv[2], in, n, &o);
    free(in);
    if (err) {
        fprintf(stderr, "slz: %s\n", slz_strerror(err));
        return 1;
    }
    if (!quiet)
        fprintf(stderr, "%llu files, %llu -> %llu bytes (%.1f%%), %.3f s\n", (unsigned long long)o.files,
                (unsigned long long)o.in_bytes, (unsigned long long)o.out_bytes,
                o.in_bytes ? 100.0 * (double)o.out_bytes / (double)o.in_bytes : 0.0, now() - t0);
    return 0;
}

int main(int argc, char **argv) {
    slz_archive *a;
    int err = 0, i, quiet = 0;
    double t0;
    if (argc >= 3 && !strcmp(argv[1], "a")) return create(argc, argv);
    if (argc < 3 || (strcmp(argv[1], "l") && strcmp(argv[1], "x"))) return usage();

    t0 = now();
    a = slz_open(argv[2], &err);
    if (!a) {
        fprintf(stderr, "slz: %s: %s\n", argv[2], slz_strerror(err));
        return 1;
    }

    if (argv[1][0] == 'l') {
        if (argc > 3) {
            err = slz_dir(a, argv[3], print_child, a);
        } else {
            size_t n = slz_count(a), k;
            if (!n) err = slz_dir(a, "", print_child, a); /* surfaces a failed tar.gz index */
            for (k = 0; k < n; k++) {
                const slz_entry *e = slz_get(a, k);
                printf("%c %12llu %12llu  %s\n", "-dlo"[e->type], (unsigned long long)e -> usize,
                       (unsigned long long)e -> csize, e -> name);
            }
        }
        fprintf(stderr, "%zu entries, listed in %.3f ms\n", slz_count(a), (now() - t0) * 1e3);
    } else {
        slz_opts o;
        memset(&o, 0, sizeof o);
        if (argc < 4) return usage();
        for (i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "-o"))
                o.flags |= SLZ_OVERWRITE;
            else if (!strcmp(argv[i], "-d"))
                o.flags |= SLZ_DESTRUCTIVE;
            else if (!strcmp(argv[i], "-n"))
                o.flags |= SLZ_NO_MTIME;
            else if (!strcmp(argv[i], "-q"))
                quiet = 1;
            else if (!strcmp(argv[i], "-j") && i + 1 < argc)
                o.threads = (unsigned)atoi(argv[++i]);
            else if (!strcmp(argv[i], "-c") && i + 1 < argc)
                o.checkpoint = (uint64_t)atoll(argv[++i]) << 20;
            else
                return usage();
        }
        err = slz_extract(a, argv[3], &o);
        if (!quiet)
            fprintf(stderr, "%llu files, %llu skipped, %.3f s\n", (unsigned long long)o.files,
                    (unsigned long long)o.skipped, now() - t0);
    }
    if (err) fprintf(stderr, "slz: %s\n", slz_strerror(err));
    slz_close(a);
    return err ? 1 : 0;
}
