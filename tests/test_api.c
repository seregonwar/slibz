/* API-level tests that need library internals or interruption control.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 *   slz-test unit                          sanitizer, tree listing, journal and in-memory inflate unit tests
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

/* in-memory inflate (slz_inflate) against the streaming decoder, on streams from a tiny encoder:
 * fixed-Huffman blocks with greedy matches (distances cross block boundaries) and stored blocks */
typedef struct {
    uint8_t *b;
    size_t n;
    uint64_t acc;
    unsigned an;
} bitw;
static void put(bitw *w, unsigned v, unsigned n) { /* LSB first */
    w->acc |= (uint64_t)v << w->an;
    w->an += n;
    while (w->an >= 8) {
        w->b[w->n++] = (uint8_t)w->acc;
        w->acc >>= 8;
        w->an -= 8;
    }
}
static void put_code(bitw *w, unsigned code, unsigned n) { /* Huffman codes go MSB first */
    unsigned r = 0, i;
    for (i = 0; i < n; i++)
        r |= ((code >> i) & 1) << (n - 1 - i);
    put(w, r, n);
}
static void put_sym(bitw *w, unsigned s) { /* fixed literal/length code (RFC 1951 3.2.6) */
    if (s < 144)
        put_code(w, 0x30 + s, 8);
    else if (s < 256)
        put_code(w, 0x190 + s - 144, 9);
    else if (s < 280)
        put_code(w, s - 256, 7);
    else
        put_code(w, 0xc0 + s - 280, 8);
}
static void put_match(bitw *w, unsigned len, unsigned d) {
    static const unsigned short lb[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const unsigned char lx[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const unsigned short db[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                          33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                          1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    unsigned c = 28, k = 30;
    while (lb[c] > len)
        c--;
    put_sym(w, 257 + c);
    put(w, len - lb[c], lx[c]);
    while (db[--k] > d)
        ;
    put_code(w, k, 5);
    put(w, d - db[k], k < 4 ? 0 : k / 2 - 1);
}
static size_t mini_deflate(const uint8_t *in, size_t n, uint8_t *out) {
    static int32_t head[1 << 15];
    bitw w;
    size_t i, blk = 0;
    memset(&w, 0, sizeof w);
    w.b = out;
    for (i = 0; i < (1u << 15); i++)
        head[i] = -1;
    i = 0;
    do {
        size_t end = n - i > 65535 ? i + 65535 : n;
        unsigned last = end == n;
        if (blk++ % 3 == 2) { /* stored */
            put(&w, last, 1);
            put(&w, 0, 2);
            if (w.an) put(&w, 0, 8 - w.an);
            put(&w, (unsigned)(end - i), 16);
            put(&w, ~(unsigned)(end - i) & 0xffff, 16);
            memcpy(w.b + w.n, in + i, end - i);
            w.n += end - i;
            i = end;
            continue;
        }
        put(&w, last, 1);
        put(&w, 1, 2);
        while (i < end) {
            unsigned len = 0;
            if (end - i >= 3) {
                unsigned h = (unsigned)((in[i] | in[i + 1] << 8 | in[i + 2] << 16) * 2654435761u) >> 17;
                int32_t c = head[h];
                if (c >= 0 && i - (size_t)c <= 32768)
                    while (len < 258 && i + len < end && in[(size_t)c + len] == in[i + len])
                        len++;
                if (len >= 3) put_match(&w, len, (unsigned)(i - (size_t)c));
                head[h] = (int32_t)i;
            }
            if (len >= 3)
                i += len;
            else
                put_sym(&w, in[i++]);
        }
        put_sym(&w, 256);
    } while (i < n);
    if (w.an) put(&w, 0, 8 - w.an);
    return w.n;
}

typedef struct {
    const uint8_t *in;
    size_t n, pos;
    uint8_t *out;
    size_t on, cap;
} memio;
static int64_t mem_rd(void *u, uint8_t *b, size_t n) {
    memio *m = (memio *)u;
    size_t k = MIN(n, m->n - m->pos);
    memcpy(b, m->in + m->pos, k);
    m->pos += k;
    return (int64_t)k;
}
static int mem_wr(void *u, const uint8_t *p, size_t n) {
    memio *m = (memio *)u;
    if (n > m->cap - m->on) return SLZ_E_LIMIT;
    memcpy(m->out + m->on, p, n);
    m->on += n;
    return 0;
}
static int stream_inflate(const uint8_t *c, size_t cn, uint8_t *o, size_t cap, size_t *on) {
    memio m;
    int r;
    inf *s = inf_new();
    if (!s) return SLZ_E_NOMEM;
    memset(&m, 0, sizeof m);
    m.in = c;
    m.n = cn;
    m.out = o;
    m.cap = cap;
    inf_reset(s, 0, 0, NULL, 0, 0);
    s->rd = mem_rd;
    s->wr = mem_wr;
    s->ck = NULL;
    s->ru = s->wu = &m;
    r = inf_run(s);
    *on = m.on;
    free(s);
    return r;
}

static void test_inflate(void) {
    static const size_t sizes[] = {0, 1, 7, 300, 70000, (5u << 20) + 123}; /* the last > OBUF */
    uint32_t x = 12345;
    unsigned si, pat;
    size_t used, got;
    CHECK(slz_inflate("\x03\x00", 2, NULL, 0, &used, &got) == 0 && used == 2 && got == 0);
    CHECK(slz_inflate(NULL, 0, NULL, 0, &used, &got) == SLZ_E_CORRUPT);
    CHECK(slz_inflate("\x01\x01\x00\xfe\xff\x41", 6, NULL, 0, NULL, NULL) == SLZ_E_LIMIT);
    for (si = 0; si < sizeof sizes / sizeof *sizes; si++)
        for (pat = 0; pat < 4; pat++) {
            size_t n = sizes[si], cn, i;
            uint8_t *d = (uint8_t *)malloc(n + 1), *c = (uint8_t *)malloc(n + n / 4 + 1024),
                    *o = (uint8_t *)malloc(n + 1), *t;
            int r;
            if (!d || !c || !o) {
                CHECK(!"out of memory");
                free(d), free(c), free(o);
                return;
            }
            for (i = 0; i < n; i++) { /* zeros, 8-letter text, random, then all three in 4 KiB runs */
                unsigned m = pat == 3 ? (unsigned)(i >> 12) % 3 : pat;
                x = x * 1103515245u + 12345u;
                d[i] = m == 0 ? 0 : m == 1 ? (uint8_t)"abcdefgh"[x >> 29] : (uint8_t)(x >> 24);
            }
            cn = mini_deflate(d, n, c);
            /* exact-size buffers (sanitizers catch any access past either end) */
            t = (uint8_t *)malloc(cn);
            if (t) {
                memcpy(t, c, cn);
                r = slz_inflate(t, cn, o, n, &used, &got);
                CHECK(r == 0 && used == cn && got == n && !memcmp(o, d, n));
                if (n) CHECK(slz_inflate(t, cn, o, n - 1, &used, &got) == SLZ_E_LIMIT);
                CHECK(slz_inflate(t, cn - 1, o, n, &used, &got) != 0); /* truncated */
                free(t);
            }
            memset(c + cn, 0xa5, 64); /* trailing data after the stream is left alone */
            CHECK(slz_inflate(c, cn + 64, o, n + 1, &used, &got) == 0 && used == cn && got == n);
            memset(o, 0, n);
            CHECK(stream_inflate(c, cn, o, n, &got) == 0 && got == n && !memcmp(o, d, n));
            free(d);
            free(c);
            free(o);
        }
}

static void test_version(void) {
    char want[32];
    snprintf(want, sizeof want, "%d.%d.%d", SLZ_VERSION_MAJOR, SLZ_VERSION_MINOR, SLZ_VERSION_PATCH);
    CHECK(slz_version_number() == SLZ_VERSION_NUMBER);
    CHECK(SLZ_VERSION_NUMBER == SLZ_VERSION_MAJOR * 10000 + SLZ_VERSION_MINOR * 100 + SLZ_VERSION_PATCH);
    CHECK(!strcmp(slz_version_string(), SLZ_VERSION_STRING) && !strcmp(SLZ_VERSION_STRING, want));
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "unit")) {
        test_version();
        test_sanitize();
        test_tree();
        test_journal();
        test_inflate();
        printf("%s\n", fails ? "unit: FAILED" : "unit: ok");
        return fails != 0;
    }
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "resume"))
        return resume(argv[2], argv[3], (uint64_t)atoll(argv[4]) << 10,
                      argc == 6 ? (uint64_t)atoll(argv[5]) << 10 : 0);
    fprintf(stderr, "usage: slz-test unit | resume <archive> <dest> <KiB> [checkpoint_KiB]\n");
    return 2;
}
