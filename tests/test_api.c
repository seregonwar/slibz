/* API-level tests that need library internals or interruption control.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 *   slz-test unit                          sanitizer, tree listing, journal and read argument unit tests
 *   slz-test list <archive>                one line per entry: data offset, usize, name
 *   slz-test read <archive> [cap=N] [max=N] [index]...  every entry (or the given ones; each option
 *                                          applies to the indices after it) through slz_read_mem
 *                                          (cap: N, default the declared size) then slz_read
 *                                          (max_total: N), cross-checked; one line per entry:
 *                                          index type off usize mem_rc n crc32 cb_rc total name
 *   slz-test resume <archive> <dest> <KiB> [checkpoint_KiB] destructive extraction, cancelled every <KiB> of
 *                                          output and resumed until done (simulated crashes);
 *                                          prints the peak on-disk size of the archive. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* lstat */
#endif
#include "internal.h"
#include <stdio.h>

static int fails;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                     \
            fails++;                                                                                         \
        }                                                                                                    \
    } while (0)

static void test_sanitize(void) {
    char o[64];
    CHECK(sanitize("a/b/c.txt", 9, o, sizeof o) == 9 && !strcmp(o, "a/b/c.txt"));
    CHECK(sanitize("./a//b/", 7, o, sizeof o) == 3 && !strcmp(o, "a/b"));
    CHECK(sanitize("a\\b", 3, o, sizeof o) == 3 && !strcmp(o, "a/b"));
    CHECK(sanitize("../x", 4, o, sizeof o) == SLZ_E_UNSAFE);
    CHECK(sanitize("a/../../x", 9, o, sizeof o) == SLZ_E_UNSAFE);
    CHECK(sanitize("/etc/passwd", 11, o, sizeof o) == SLZ_E_UNSAFE);
    CHECK(sanitize("\\\\srv\\x", 7, o, sizeof o) == SLZ_E_UNSAFE);
    CHECK(sanitize("a\0b", 3, o, sizeof o) == SLZ_E_UNSAFE);
    CHECK(sanitize("a\nb", 3, o, sizeof o) == SLZ_E_UNSAFE);
    CHECK(sanitize("..a/b..c", 8, o, sizeof o) == 8);
    CHECK(sanitize("./", 2, o, sizeof o) == 0);
    CHECK(sanitize("0123456789012345678901234567890123456789012345678901234567890123456789", 70, o,
                   sizeof o) == SLZ_E_LIMIT);
#ifdef _WIN32
    CHECK(sanitize("C:/x", 4, o, sizeof o) == SLZ_E_UNSAFE);
    CHECK(sanitize("a/CON", 5, o, sizeof o) == SLZ_E_UNSAFE);
    CHECK(sanitize("com1.txt", 8, o, sizeof o) == SLZ_E_UNSAFE);
    CHECK(sanitize("a/file.", 7, o, sizeof o) == SLZ_E_UNSAFE);
    CHECK(sanitize("x:ads", 5, o, sizeof o) == SLZ_E_UNSAFE);
    CHECK(sanitize("console.txt", 11, o, sizeof o) == 11);
#endif
}

/* tree listing over a hand-built archive object (no file needed) */
typedef struct {
    char out[512];
    size_t n;
} acc;
static int collect(void *u, const char *name, size_t len, int is_dir, long long idx) {
    acc *a = (acc *)u;
    a->n += (size_t)snprintf(a->out + a->n, sizeof a->out - a->n, "%.*s%s%s,", (int)len, name,
                             is_dir ? "/" : "", idx < 0 ? "*" : "");
    return 0;
}
static void test_tree(void) {
    static const char *names[] = {"b/", "a/x/1", "a/x/2", "a/y", "a/x.txt", "c", "a/z/", "a/x-1"};
    slz_entry e[8];
    slz_archive ar;
    acc r;
    size_t i;
    memset(&ar, 0, sizeof ar);
    memset(e, 0, sizeof e);
    for (i = 0; i < 8; i++) {
        e[i].name = names[i];
        e[i].nlen = (uint32_t)strlen(names[i]);
        e[i].type = names[i][e[i].nlen - 1] == '/' ? SLZ_DIR : SLZ_FILE;
    }
    ar.e = e;
    ar.n = 8;
    memset(&r, 0, sizeof r);
    CHECK(slz_dir(&ar, "", collect, &r) == 0);
    CHECK(!strcmp(r.out, "a/*,b/,c,"));
    memset(&r, 0, sizeof r);
    CHECK(slz_dir(&ar, "a", collect, &r) == 0);
    CHECK(!strcmp(r.out, "x-1,x.txt,x/*,y,z/,"));
    memset(&r, 0, sizeof r);
    CHECK(slz_dir(&ar, "a/x", collect, &r) == 0);
    CHECK(!strcmp(r.out, "1,2,"));
    memset(&r, 0, sizeof r);
    CHECK(slz_dir(&ar, "nope", collect, &r) == 0 && r.n == 0);
    free((void *)ar.sorted);
}

/* bytes allocated on disk by a directory tree (test helper, portable) */
#ifdef _WIN32
#include <windows.h>
static uint64_t tree_alloc(const char *dir) {
    char pat[1024];
    WIN32_FIND_DATAA f;
    HANDLE h;
    uint64_t t = 0;
    snprintf(pat, sizeof pat, "%s/*", dir);
    if ((h = FindFirstFileA(pat, &f)) == INVALID_HANDLE_VALUE) return 0;
    do {
        char p[1024];
        if (!strcmp(f.cFileName, ".") || !strcmp(f.cFileName, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, f.cFileName);
        t += (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? tree_alloc(p) : os_allocated(p);
    } while (FindNextFileA(h, &f));
    FindClose(h);
    return t;
}
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
static uint64_t tree_alloc(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    uint64_t t = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        char p[1024];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (!lstat(p, &st)) t += S_ISDIR(st.st_mode) ? tree_alloc(p) : (uint64_t)st.st_blocks * 512;
    }
    closedir(d);
    return t;
}
#endif

typedef struct {
    uint64_t stop_at, peak;
    const char *path, *dest;
} cancel_ctx;
static int cancel_cb(void *u, uint64_t done, uint64_t total) {
    cancel_ctx *c = (cancel_ctx *)u;
    uint64_t footprint = os_allocated(c->path) + tree_alloc(c->dest);
    (void)total;
    if (footprint > c->peak) c->peak = footprint;
    return done >= c->stop_at;
}

static int resume(const char *archive, const char *dest, uint64_t step, uint64_t every) {
    cancel_ctx c;
    int runs = 0, r;
    uint64_t start = os_allocated(archive), out;
    memset(&c, 0, sizeof c);
    c.path = archive;
    c.dest = dest;
    do {
        slz_opts o;
        int err;
        slz_archive *a = slz_open(archive, &err);
        if (!a) {
            fprintf(stderr, "open: %s\n", slz_strerror(err));
            return 1;
        }
        memset(&o, 0, sizeof o);
        o.flags = SLZ_DESTRUCTIVE;
        o.checkpoint = every ? every : (step / 2 ? step / 2 : 1);
        o.progress = cancel_cb;
        o.user = &c;
        c.stop_at = step * (uint64_t)(runs + 1) + (uint64_t)runs * 12345; /* irregular cut points */
        r = slz_extract(a, dest, &o);
        slz_close(a);
        runs++;
        if (r && r != SLZ_E_CANCEL) {
            fprintf(stderr, "run %d: %s\n", runs, slz_strerror(r));
            return 1;
        }
    } while (r == SLZ_E_CANCEL && runs < 100000);
    out = tree_alloc(dest);
    /* footprint never exceeds max(archive, output) by more than ~2 checkpoints + punch alignment */
    printf("runs=%d archive=%llu output=%llu peak_footprint=%llu overhead=%lld archive_exists=%d\n", runs,
           (unsigned long long)start, (unsigned long long)out, (unsigned long long)c.peak,
           (long long)(c.peak - (start > out ? start : out)), os_exists(archive));
    return os_exists(archive) ? 1 : 0;
}

/* the destructive journal: exclusive (a second extraction of the same archive is refused) and
 * never opened through a symlink */
static void test_journal(void) {
#ifdef _WIN32
    const char *p = "slz-test-journal.slzstate";
#else
    const char *p = "/tmp/slz-test-journal.slzstate"; /* a POSIX filesystem (locks, symlinks) */
#endif
    os_fd a = OS_BAD, b = OS_BAD;
    os_unlink(p);
    CHECK(os_open_journal(p, &a) == 0);
    CHECK(os_open_journal(p, &b) == SLZ_E_STATE);
    if (a != OS_BAD) os_close(a);
    if (b != OS_BAD) os_close(b);
    os_unlink(p);
#ifndef _WIN32
    CHECK(symlink("/etc/passwd", p) == 0);
    CHECK(os_open_journal(p, &a) == SLZ_E_UNSAFE);
    unlink(p);
#endif
}

static int no_child(void *u, const char *name, size_t len, int is_dir, long long idx) {
    (void)u;
    (void)name;
    (void)len;
    (void)is_dir;
    (void)idx;
    return 0;
}

/* the fields the CLI listing leaves out */
static int list(const char *archive) {
    int err;
    size_t i, n;
    slz_archive *a = slz_open(archive, &err);
    if (!a) {
        fprintf(stderr, "open: %s\n", slz_strerror(err));
        return 1;
    }
    n = slz_count(a);
    err = n ? 0 : slz_dir(a, "", no_child, NULL); /* surfaces a failed tar.gz index */
    for (i = 0; i < n; i++) {
        const slz_entry *e = slz_get(a, i);
        printf("%llu %llu %s\n", (unsigned long long)e->off, (unsigned long long)e->usize, e->name);
    }
    if (err) fprintf(stderr, "list: %s\n", slz_strerror(err));
    slz_close(a);
    return err != 0;
}

/* ---------- slz_read / slz_read_mem ---------- */
typedef struct {
    const uint8_t *ref; /* what slz_read_mem stored: slz_read must deliver the same bytes */
    size_t refn;
    uint64_t got, done, total;
    uint32_t crc;
    int bad, stop, pstop, pcalls;
} rd_ctx;

static int rd_sink(void *u, const void *p, size_t n) {
    rd_ctx *c = (rd_ctx *)u;
    if (!n) { /* never called with nothing */
        c->bad = 1;
        return c->stop;
    }
    if (c->got < c->refn && memcmp(c->ref + (size_t)c->got, p, (size_t)MIN((uint64_t)n, c->refn - c->got)))
        c->bad = 1;
    c->crc = slz_crc32(c->crc, p, n);
    c->got += n;
    return c->stop;
}

static int rd_prog(void *u, uint64_t done, uint64_t total) {
    rd_ctx *c = (rd_ctx *)u;
    if (done < c->done) c->bad = 1;
    c->done = done;
    c->total = total;
    c->pcalls++;
    return c->pstop;
}

static uint32_t crc_of(const uint8_t *p, size_t n) {
    return n ? slz_crc32(0, p, n) : 0;
}

static void read_one(slz_archive *a, size_t i, int has_cap, uint64_t capv, uint64_t max) {
    static int cancelled; /* the cancel paths: once per run, on the first entry with data */
    const slz_entry *e = slz_get(a, i);
    uint64_t us = e ? e->usize : 0;
    size_t cap = (size_t)(has_cap ? capv : MIN(us, (uint64_t)256 << 20)), n = 12345, k;
    uint8_t *buf = cap ? (uint8_t *)malloc(cap + 64) : NULL;
    rd_ctx c;
    slz_opts o;
    int rm, rc;
    if (cap && !buf) {
        fprintf(stderr, "read %llu: out of memory\n", (unsigned long long)i);
        fails++;
        return;
    }
    if (buf) memset(buf + cap, 0xa5, 64);
    rm = slz_read_mem(a, i, buf, cap, &n);
    for (k = 0; buf && k < 64; k++)
        CHECK(buf[cap + k] == 0xa5); /* never past cap */
    CHECK(n <= cap);
    if (!rm) CHECK(e && (slz_format(a) == SLZ_GZ ? (uint64_t)n & 0xffffffffu : (uint64_t)n) == e->usize);
    if (rm == SLZ_E_LIMIT) CHECK(n == (us > cap ? 0 : cap)); /* refused up front, or cut at cap */
    if (rm == SLZ_E_ARG || rm == SLZ_E_STATE || rm == SLZ_E_NOTSUP || rm == SLZ_E_ENCRYPTED || rm == SLZ_E_METHOD)
        CHECK(n == 0);

    memset(&c, 0, sizeof c);
    c.ref = buf;
    c.refn = n;
    memset(&o, 0, sizeof o);
    o.max_total = max;
    o.progress = rd_prog;
    o.user = &c;
    rc = slz_read(a, i, rd_sink, &c, &o);
    CHECK(!c.bad);
    CHECK(rc || c.done == c.total);
    CHECK(!max || (c.got <= max && c.done <= max));
    if (!rm && !rc) CHECK(c.got == n && c.crc == crc_of(buf, n));
    if (!rc && !max && c.total > 1) { /* max_total is checked against the declared sizes up front */
        rd_ctx d;
        memset(&d, 0, sizeof d);
        o.user = &d;
        o.max_total = c.total - 1;
        CHECK(slz_read(a, i, rd_sink, &d, &o) == SLZ_E_LIMIT && d.got == 0 && d.pcalls == 0);
        if (c.total <= 1u << 20) {
            memset(&d, 0, sizeof d);
            o.max_total = c.total;
            CHECK(slz_read(a, i, rd_sink, &d, &o) == 0 && d.got == c.got && d.crc == c.crc);
        }
    }
    if (!rc && c.got && !cancelled) { /* cancelled by the sink, then by progress (before any data) */
        rd_ctx d;
        cancelled = 1;
        memset(&d, 0, sizeof d);
        d.stop = 1;
        CHECK(slz_read(a, i, rd_sink, &d, NULL) == SLZ_E_CANCEL && d.got > 0);
        memset(&d, 0, sizeof d);
        d.pstop = 1;
        o.user = &d;
        o.max_total = 0;
        CHECK(slz_read(a, i, rd_sink, &d, &o) == SLZ_E_CANCEL && d.pcalls == 1 && d.got == 0);
    }
    printf("%llu %d %llu %llu %d %llu %08lx %d %llu %s\n", (unsigned long long)i, e ? (int)e->type : -1,
           (unsigned long long)(e ? e->off : 0), (unsigned long long)us, rm, (unsigned long long)n,
           (unsigned long)crc_of(buf, n), rc, (unsigned long long)c.total, e ? e->name : "-");
    free(buf);
}

static int read_mode(int argc, char **argv) {
    int err, k, has_cap = 0, any = 0;
    uint64_t capv = 0, max = 0;
    slz_archive *a = slz_open(argv[2], &err);
    if (!a) {
        fprintf(stderr, "open: %s\n", slz_strerror(err));
        return 1;
    }
    for (k = 3; k < argc; k++) {
        if (!strncmp(argv[k], "cap=", 4)) {
            has_cap = 1;
            capv = (uint64_t)strtoull(argv[k] + 4, NULL, 10);
        } else if (!strncmp(argv[k], "max=", 4)) {
            max = (uint64_t)strtoull(argv[k] + 4, NULL, 10);
        } else {
            read_one(a, (size_t)strtoull(argv[k], NULL, 10), has_cap, capv, max);
            any = 1;
        }
    }
    if (!any) {
        size_t i, cnt = slz_count(a);
        for (i = 0; i < cnt; i++)
            read_one(a, i, has_cap, capv, max);
    }
    slz_close(a);
    return fails != 0;
}

static int nul_cb(void *u, const void *p, size_t n) {
    (void)u;
    (void)p;
    (void)n;
    return 0;
}

/* argument and state errors, all detected before any data is read (hand-built archive, no file) */
static void test_read_args(void) {
    slz_entry e[2];
    slz_archive ar;
    uint8_t b[4];
    size_t n = 7;
    memset(&ar, 0, sizeof ar);
    memset(e, 0, sizeof e);
    e[0].name = "d/";
    e[0].nlen = 2;
    e[0].type = SLZ_DIR;
    e[1].name = "f";
    e[1].nlen = 1;
    e[1].type = SLZ_FILE;
    e[1].usize = e[1].csize = 1;
    ar.e = e;
    ar.n = 2;
    ar.fmt = SLZ_ZIP;
    ar.fd = OS_BAD;
    CHECK(!strcmp(slz_strerror(SLZ_E_ARG), "invalid argument"));
    CHECK(!strcmp(slz_strerror(SLZ_E_ARG - 1), "unknown error"));
    CHECK(slz_read(NULL, 0, nul_cb, NULL, NULL) == SLZ_E_ARG);
    CHECK(slz_read(&ar, 1, NULL, NULL, NULL) == SLZ_E_ARG);
    CHECK(slz_read_mem(NULL, 1, b, sizeof b, &n) == SLZ_E_ARG && n == 0);
    n = 7;
    CHECK(slz_read_mem(&ar, 1, NULL, 1, &n) == SLZ_E_ARG && n == 0);
    CHECK(slz_read(&ar, 1, nul_cb, NULL, NULL) == SLZ_E_STATE); /* no archive file behind it */
    ar.fd = 0;                                                  /* never read: each call below fails first */
    n = 7;
    CHECK(slz_read_mem(&ar, 0, b, sizeof b, &n) == SLZ_E_NOTSUP && n == 0);
    CHECK(slz_read_mem(&ar, 0, NULL, 0, NULL) == SLZ_E_NOTSUP);
    CHECK(slz_read(&ar, 2, nul_cb, NULL, NULL) == SLZ_E_ARG);
    CHECK(slz_read_mem(&ar, (size_t)-1, b, sizeof b, &n) == SLZ_E_ARG);
    ar.partial = 1;
    CHECK(slz_read(&ar, 1, nul_cb, NULL, NULL) == SLZ_E_STATE);
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "unit")) {
        test_sanitize();
        test_tree();
        test_journal();
        test_read_args();
        printf("%s\n", fails ? "unit: FAILED" : "unit: ok");
        return fails != 0;
    }
    if (argc == 3 && !strcmp(argv[1], "list")) return list(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "read")) return read_mode(argc, argv);
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "resume"))
        return resume(argv[2], argv[3], (uint64_t)atoll(argv[4]) << 10,
                      argc == 6 ? (uint64_t)atoll(argv[5]) << 10 : 0);
    fprintf(stderr, "usage: slz-test unit | list <archive> | read <archive> [cap=N] [max=N] [index]... |"
                    " resume <archive> <dest> <KiB> [checkpoint_KiB]\n");
    return 2;
}
