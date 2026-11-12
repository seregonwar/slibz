/* API-level tests that need library internals or interruption control.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 *   slz-test unit                          sanitizer + tree listing unit tests
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

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "unit")) {
        test_sanitize();
        test_tree();
        test_journal();
        printf("%s\n", fails ? "unit: FAILED" : "unit: ok");
        return fails != 0;
    }
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "resume"))
        return resume(argv[2], argv[3], (uint64_t)atoll(argv[4]) << 10,
                      argc == 6 ? (uint64_t)atoll(argv[5]) << 10 : 0);
    fprintf(stderr, "usage: slz-test unit | resume <archive> <dest> <KiB> [checkpoint_KiB]\n");
    return 2;
}
