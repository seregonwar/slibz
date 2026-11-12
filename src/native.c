/* slz - the native .slz format (docs/slz-format.md): reader and writer.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * Reader: the trailer, the compressed index and every record are validated against the rules of the
 * specification before anything is used; a block is decoded into exactly its declared size and
 * checked against its xxHash64. Writer: inputs are walked without following links, ordered by
 * extension, deduplicated (hash match confirmed byte for byte), packed into one stream cut into
 * blocks, compressed in parallel and written to "<path>.part", which is renamed only at the end. */
#include "internal.h"
#include <stdio.h>
#ifdef SLZ_ZSTD
#include <zstd.h>
#endif

#define HDR_SIZE      32u
#define TRAILER_SIZE  64u
#define BLOCK_REC     32u
#define ENTRY_REC     48u
#define INDEX_FIXED   24u
#define MAX_BLOCK     (256u << 20)
#define MAX_INDEX     (1ull << 30)
#define DEF_BLOCK     (8u << 20)
#define CODEC_STORE   0
#define CODEC_LZ4     1
#define CODEC_ZSTD    2
#define ENTRY_DEDUP   1

static const uint8_t MAGIC[4] = {'S', 'L', 'Z', 0x1a}, TMAGIC[4] = {'S', 'L', 'Z', 'I'};

int slz_has_zstd(void) {
#ifdef SLZ_ZSTD
    return 1;
#else
    return 0;
#endif
}

static void wr16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    wr16(p, v);
    wr16(p + 2, v >> 16);
}
static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

/* ======================================================================== reader */

/* decode `in` (csize bytes) with `codec` into exactly `usize` bytes at `out` */
static int unpack(int codec, const uint8_t *in, size_t csize, uint8_t *out, size_t usize, void **zctx) {
    switch (codec) {
        case CODEC_STORE:
            if (csize != usize) return SLZ_E_CORRUPT;
            if (in != out) memcpy(out, in, usize);
            return 0;
        case CODEC_LZ4:
            return lz4_decompress(in, csize, out, usize);
        case CODEC_ZSTD:
#ifdef SLZ_ZSTD
        {
            size_t r;
            if (!*zctx) {
                if (!(*zctx = ZSTD_createDCtx())) return SLZ_E_NOMEM;
                ZSTD_DCtx_setParameter((ZSTD_DCtx *)*zctx, ZSTD_d_windowLogMax, 28); /* bounded memory */
            }
            r = ZSTD_decompressDCtx((ZSTD_DCtx *)*zctx, out, usize, in, csize);
            return ZSTD_isError(r) || r != usize ? SLZ_E_CORRUPT : 0;
        }
#else
            (void)zctx;
            return SLZ_E_METHOD; /* built without zstd */
#endif
        default:
            return SLZ_E_METHOD;
    }
}

void nat_zfree(void *zctx) {
#ifdef SLZ_ZSTD
    ZSTD_freeDCtx((ZSTD_DCtx *)zctx);
#else
    (void)zctx;
#endif
}

int nat_decode(os_fd fd, const natblock *b, uint8_t *cbuf, uint8_t *out, void **zctx) {
    uint8_t *dst = b->codec == CODEC_STORE ? out : cbuf;
    int r;
    if (os_pread(fd, dst, b->csize, b->off) != (int64_t)b->csize) return SLZ_E_CORRUPT;
    if ((r = unpack(b->codec, dst, b->csize, out, b->usize, zctx))) return r;
    return xxh64(out, b->usize, 0) == b->hash ? 0 : SLZ_E_CORRUPT;
}

void nat_free(natidx *n) {
    if (!n) return;
    free(n->b);
    free(n->order);
    free(n->spans);
    free(n->ehash);
    free(n);
}

size_t nat_block_of(const natidx *n, uint64_t pos) { /* block containing stream position pos */
    size_t lo = 0, hi = n->nb;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (n->b[mid].start <= pos)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

size_t nat_first(const slz_archive *a, uint64_t start) { /* first ordered file ending after start */
    const natidx *n = a->nat;
    size_t lo = 0, hi = n->norder;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const slz_entry *e = &a->e[n->order[mid]];
        if (e->off + e->usize <= start)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

typedef struct {
    uint64_t off;
    uint32_t i;
} offidx;
static int by_stream(const void *x, const void *y) { /* stream offset, then entry index (stable) */
    const offidx *p = (const offidx *)x, *q = (const offidx *)y;
    if (p->off != q->off) return p->off < q->off ? -1 : 1;
    return p->i < q->i ? -1 : p->i > q->i;
}

int open_slz(slz_archive *a, const uint8_t *head, size_t hn) {
    uint8_t t[TRAILER_SIZE], *idx = NULL, *cidx = NULL;
    uint64_t ioff, icsize, iusize, names_size, prev_end = HDR_SIZE, nb, ne;
    natidx *n;
    size_t i;
    int r = SLZ_E_CORRUPT;
    void *zctx = NULL;
    if (hn < HDR_SIZE || a->size < HDR_SIZE + TRAILER_SIZE) return SLZ_E_FORMAT;
    if ((uint32_t)xxh64(head, 28, 0) != rd32(head + 28) || rd16(head + 4) != 1) return SLZ_E_CORRUPT;
    if (rd32(head + 8)) return SLZ_E_NOTSUP; /* required features this version does not know */
    if (os_pread(a->fd, t, TRAILER_SIZE, a->size - TRAILER_SIZE) != TRAILER_SIZE || memcmp(t, TMAGIC, 4) ||
        xxh64(t, 56, 0) != rd64(t + 56) || rd32(t + 40) != rd32(head + 8))
        return SLZ_E_CORRUPT;
    ioff = rd64(t + 8);
    icsize = rd64(t + 16);
    iusize = rd64(t + 24);
    if (ioff < HDR_SIZE || ioff > a->size - TRAILER_SIZE || icsize != a->size - TRAILER_SIZE - ioff ||
        iusize < INDEX_FIXED || iusize > MAX_INDEX || icsize > MAX_INDEX)
        return SLZ_E_CORRUPT;
    if (!(n = (natidx *)calloc(1, sizeof *n)) || !(cidx = (uint8_t *)malloc((size_t)icsize + 1)) ||
        !(idx = (uint8_t *)malloc((size_t)iusize + 1))) {
        free(n);
        free(cidx);
        return SLZ_E_NOMEM;
    }
    a->nat = n;
    if (os_pread(a->fd, cidx, (size_t)icsize, ioff) != (int64_t)icsize) goto out;
    if ((r = unpack((int)rd32(t + 4), cidx, (size_t)icsize, idx, (size_t)iusize, &zctx))) goto out;
    r = SLZ_E_CORRUPT;
    if (xxh64(idx, (size_t)iusize, 0) != rd64(t + 32)) goto out;
    nb = rd32(idx);
    ne = rd32(idx + 4);
    names_size = rd64(idx + 8);
    if (names_size > iusize || nb > iusize / BLOCK_REC || ne > iusize / ENTRY_REC ||
        INDEX_FIXED + BLOCK_REC * nb + ENTRY_REC * ne + names_size != iusize)
        goto out;
    if (!(n->b = (natblock *)calloc((size_t)nb + 1, sizeof *n->b)) ||
        !(a->e = (slz_entry *)calloc((size_t)ne + 1, sizeof *a->e)) ||
        !(a->names = (char *)malloc((size_t)names_size + 1)) || !(n->order = (uint32_t *)malloc((ne + 1) * 4)) ||
        !(n->spans = (uint8_t *)calloc((size_t)ne + 1, 1)) || !(n->ehash = (uint64_t *)calloc((size_t)ne + 1, 8))) {
        r = SLZ_E_NOMEM;
        goto out;
    }
    for (i = 0; i < nb; i++) { /* blocks: increasing, non-overlapping, before the index */
        const uint8_t *p = idx + INDEX_FIXED + BLOCK_REC * i;
        natblock *b = &n->b[i];
        b->off = rd64(p);
        b->csize = rd32(p + 8);
        b->usize = rd32(p + 12);
        b->codec = p[16];
        b->hash = rd64(p + 24);
        if (b->off < prev_end || b->csize > ioff - b->off || b->usize < 1 || b->usize > MAX_BLOCK ||
            (b->codec == CODEC_STORE && b->csize != b->usize))
            goto out;
        prev_end = b->off + b->csize;
        b->start = n->total;
        n->total += b->usize;
        if (b->csize > n->max_c) n->max_c = b->csize;
        if (b->usize > n->max_u) n->max_u = b->usize;
    }
    n->nb = (size_t)nb;
    memcpy(a->names, idx + INDEX_FIXED + BLOCK_REC * nb + ENTRY_REC * ne, (size_t)names_size);
    a->names[names_size] = 0;
    for (i = 0; i < ne; i++) {
        const uint8_t *p = idx + INDEX_FIXED + BLOCK_REC * nb + ENTRY_REC * i;
        slz_entry *e = &a->e[i];
        uint64_t so = rd64(p), size = rd64(p + 8);
        uint32_t noff = rd32(p + 24), nlen = rd32(p + 28);
        uint8_t type = p[36];
        if (type > 1 || noff >= names_size || nlen >= names_size - noff || a->names[noff + nlen] ||
            memchr(a->names + noff, 0, nlen) || (type == 1 && size) || (type == 0 && (so > n->total || size > n->total - so)))
            goto out;
        e->name = (const char *)(uintptr_t)noff; /* offset; finish_index makes it a pointer */
        e->nlen = nlen;
        e->type = type ? SLZ_DIR : SLZ_FILE;
        e->usize = size;
        e->off = so;
        e->mtime = (int64_t)rd64(p + 16);
        e->mode = rd32(p + 32) & 07777;
        n->ehash[i] = rd64(p + 40);
        e->method = SLZ_M_STORE;
        if (type == 0 && size) {
            size_t first = nat_block_of(n, so), last = nat_block_of(n, so + size - 1);
            static const uint16_t m[3] = {SLZ_M_STORE, SLZ_M_LZ4, SLZ_M_ZSTD};
            e->method = n->b[first].codec < 3 ? m[n->b[first].codec] : 0xffff;
            n->spans[i] = first != last;
            n->order[n->norder++] = (uint32_t)i;
        }
    }
    a->n = (size_t)ne;
    /* file ranges must be identical (deduplicated) or disjoint */
    {
        offidx *v = (offidx *)malloc((n->norder + 1) * sizeof *v);
        if (!v) {
            r = SLZ_E_NOMEM;
            goto out;
        }
        for (i = 0; i < n->norder; i++) {
            v[i].off = a->e[n->order[i]].off;
            v[i].i = n->order[i];
        }
        qsort(v, n->norder, sizeof *v, by_stream);
        for (i = 0; i < n->norder; i++)
            n->order[i] = v[i].i;
        free(v);
    }
    for (i = 1; i < n->norder; i++) {
        const slz_entry *p = &a->e[n->order[i - 1]], *q = &a->e[n->order[i]];
        if (q->off < p->off + p->usize && !(q->off == p->off && q->usize == p->usize)) goto out;
    }
    a->cd_off = ioff;
    r = 0;
out:
    nat_zfree(zctx);
    free(cidx);
    free(idx);
    return r;
}

/* ======================================================================== writer */

typedef struct {
    char *path, *name;
    int dir, dup;
    uint64_t size, so, hash;
    int64_t mtime;
    unsigned mode;
    int hashed;
} witem;

typedef struct {
    witem *v;
    size_t n, cap;
} wlist;

static int collect(void *u, const char *path, const char *rel, int is_dir, uint64_t size, int64_t mtime, unsigned mode) {
    wlist *l = (wlist *)u;
    witem *it;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 256;
        witem *v = (witem *)realloc(l->v, cap * sizeof *v);
        if (!v) return SLZ_E_NOMEM;
        l->v = v;
        l->cap = cap;
    }
    it = &l->v[l->n];
    memset(it, 0, sizeof *it);
    if (!(it->path = (char *)malloc(strlen(path) + 1)) || !(it->name = (char *)malloc(strlen(rel) + 1))) {
        free(it->path);
        return SLZ_E_NOMEM;
    }
    strcpy(it->path, path);
    strcpy(it->name, rel);
    it->dir = is_dir;
    it->size = size;
    it->mtime = mtime;
    it->mode = mode;
    l->n++;
    return 0;
}

static const char *ext_of(const char *name) {
    const char *b = strrchr(name, '/'), *d;
    b = b ? b + 1 : name;
    d = strrchr(b, '.');
    return d && d != b ? d + 1 : "";
}
static int ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
}
/* directories first, then files grouped by extension (similar data ends up in the same blocks) */
static int by_kind(const void *x, const void *y) {
    const witem *p = (const witem *)x, *q = (const witem *)y;
    int c;
    if (p->dir != q->dir) return q->dir - p->dir;
    if (!p->dir && (c = ci_cmp(ext_of(p->name), ext_of(q->name)))) return c;
    return strcmp(p->name, q->name);
}
static int strcmp_ptr(const void *x, const void *y) {
    return strcmp(*(const char *const *)x, *(const char *const *)y);
}
static int by_size(const void *x, const void *y) {
    const witem *p = *(const witem *const *)x, *q = *(const witem *const *)y;
    return p->size != q->size ? (p->size < q->size ? -1 : 1) : (p < q ? -1 : p > q);
}

static int hash_file(witem *it, uint8_t *buf, size_t bs) {
    os_fd f;
    uint64_t pos = 0;
    xxh64_t h;
    int r = os_open(it->path, 0, 0, &f);
    if (r) return r;
    xxh64_init(&h, 0);
    while (pos < it->size) {
        size_t k = (size_t)MIN((uint64_t)bs, it->size - pos);
        if (os_pread(f, buf, k, pos) != (int64_t)k) {
            os_close(f);
            return SLZ_E_IO; /* changed while reading */
        }
        xxh64_update(&h, buf, k);
        pos += k;
    }
    os_close(f);
    it->hash = xxh64_digest(&h);
    it->hashed = 1;
    return 0;
}

static int same_content(const witem *a, const witem *b, uint8_t *x, uint8_t *y, size_t bs) {
    os_fd f, g;
    uint64_t pos = 0;
    int same = 1;
    if (os_open(a->path, 0, 0, &f)) return 0;
    if (os_open(b->path, 0, 0, &g)) {
        os_close(f);
        return 0;
    }
    while (same && pos < a->size) {
        size_t k = (size_t)MIN((uint64_t)bs, a->size - pos);
        same = os_pread(f, x, k, pos) == (int64_t)k && os_pread(g, y, k, pos) == (int64_t)k && !memcmp(x, y, k);
        pos += k;
    }
    os_close(f);
    os_close(g);
    return same;
}

/* Identical files are stored once. Only files sharing a size are hashed; every hash match is
 * confirmed byte for byte before the copy is dropped. */
static int dedup(wlist *l, size_t bs) {
    witem **v = (witem **)malloc((l->n + 1) * sizeof *v);
    uint8_t *x = (uint8_t *)malloc(bs), *y = (uint8_t *)malloc(bs);
    size_t i, j, k, m = 0;
    int r = 0;
    if (!v || !x || !y) {
        r = SLZ_E_NOMEM;
        goto out;
    }
    for (i = 0; i < l->n; i++)
        if (!l->v[i].dir && l->v[i].size) v[m++] = &l->v[i];
    qsort(v, m, sizeof *v, by_size);
    for (i = 0; i < m && !r; i = j) {
        for (j = i + 1; j < m && v[j]->size == v[i]->size; j++)
            ;
        if (j - i < 2) continue;
        for (k = i; k < j && !r; k++)
            r = hash_file(v[k], x, bs);
        for (k = i + 1; k < j && !r; k++) {
            size_t q;
            for (q = i; q < k; q++)
                if (!v[q]->dup && v[q]->hash == v[k]->hash && same_content(v[q], v[k], x, y, bs)) {
                    v[k]->dup = (int)(v[q] - l->v) + 1; /* index + 1 of the original */
                    break;
                }
        }
    }
out:
    free(v);
    free(x);
    free(y);
    return r;
}

/* one block of the stream being compressed */
typedef struct {
    uint8_t *in, *out;
    size_t n, cn;
    int codec;
    uint64_t hash;
    void *cctx;
} wslot;

typedef struct {
    wslot *s;
    size_t ns;
    volatile int64_t next, err;
    int codec, level;
    uint32_t bs;
} wbatch;

/* likely incompressible: three 1 KiB samples with almost every byte value present (compressed or
 * encrypted data), checked before spending time on a doomed compression attempt */
static int incompressible(const uint8_t *p, size_t n) {
    size_t at[3], i;
    if (n < 4096) return 0;
    at[0] = 0;
    at[1] = n / 2 - 512;
    at[2] = n - 1024;
    for (i = 0; i < 3; i++) {
        uint8_t seen[256] = {0};
        unsigned uniq = 0, k;
        for (k = 0; k < 1024; k++)
            if (!seen[p[at[i] + k]]++) uniq++;
        if (uniq < 230) return 0;
    }
    return 1;
}

static void compress_slot(wbatch *b, wslot *s) {
    s->hash = xxh64(s->in, s->n, 0);
    s->codec = CODEC_STORE;
    s->cn = s->n;
    if (b->codec == CODEC_STORE || incompressible(s->in, s->n)) return;
    if (b->codec == CODEC_LZ4) {
        size_t c = lz4_compress(s->in, s->n, s->out);
        if (!c) {
            os_cas(&b->err, 0, SLZ_E_NOMEM);
            return;
        }
        if (c < s->n) {
            s->codec = CODEC_LZ4;
            s->cn = c;
        }
    }
#ifdef SLZ_ZSTD
    if (b->codec == CODEC_ZSTD) {
        size_t c;
        if (!s->cctx) {
            unsigned wlog = 10;
            if (!(s->cctx = ZSTD_createCCtx())) {
                os_cas(&b->err, 0, SLZ_E_NOMEM);
                return;
            }
            while ((1u << wlog) < b->bs && wlog < 27)
                wlog++;
            ZSTD_CCtx_setParameter((ZSTD_CCtx *)s->cctx, ZSTD_c_compressionLevel, b->level);
            ZSTD_CCtx_setParameter((ZSTD_CCtx *)s->cctx, ZSTD_c_checksumFlag, 0); /* the block hash covers it */
            ZSTD_CCtx_setParameter((ZSTD_CCtx *)s->cctx, ZSTD_c_dictIDFlag, 0);
            ZSTD_CCtx_setParameter((ZSTD_CCtx *)s->cctx, ZSTD_c_windowLog, (int)wlog);
            if (b->bs >= (128u << 10)) ZSTD_CCtx_setParameter((ZSTD_CCtx *)s->cctx, ZSTD_c_enableLongDistanceMatching, 1);
        }
        c = ZSTD_compress2((ZSTD_CCtx *)s->cctx, s->out, ZSTD_compressBound(s->n), s->in, s->n);
        if (!ZSTD_isError(c) && c < s->n) {
            s->codec = CODEC_ZSTD;
            s->cn = c;
        }
    }
#endif
}

static void compress_worker(void *arg) {
    wbatch *b = (wbatch *)arg;
    for (;;) {
        int64_t k = os_add(&b->next, 1) - 1;
        if (k >= (int64_t)b->ns) break;
        compress_slot(b, &b->s[k]);
    }
}

typedef struct {
    os_fd out;
    uint64_t pos;  /* next write offset in the archive */
    natblock *b;
    size_t nb, cap;
} wout;

static int flush_batch(wbatch *wb, wout *o, unsigned threads) {
    size_t i;
    if (!wb->ns) return 0;
    wb->next = 0;
    os_run(threads < wb->ns ? threads : (unsigned)wb->ns, compress_worker, wb);
    if (wb->err) return (int)wb->err;
    for (i = 0; i < wb->ns; i++) { /* in stream order */
        wslot *s = &wb->s[i];
        int r;
        if (o->nb == o->cap) {
            size_t cap = o->cap ? o->cap * 2 : 64;
            natblock *v = (natblock *)realloc(o->b, cap * sizeof *v);
            if (!v) return SLZ_E_NOMEM;
            o->b = v;
            o->cap = cap;
        }
        if ((r = os_pwrite(o->out, s->codec == CODEC_STORE ? s->in : s->out, s->cn, o->pos))) return r;
        o->b[o->nb].off = o->pos;
        o->b[o->nb].csize = (uint32_t)s->cn;
        o->b[o->nb].usize = (uint32_t)s->n;
        o->b[o->nb].codec = (uint8_t)s->codec;
        o->b[o->nb].hash = s->hash;
        o->nb++;
        o->pos += s->cn;
        s->n = 0;
    }
    wb->ns = 0;
    return 0;
}

/* index + trailer (format: docs/slz-format.md) */
static int write_index(wlist *l, wout *o, uint32_t bs, int codec, int level) {
    size_t names = 0, i, isz;
    uint8_t *idx, *cidx = NULL, t[TRAILER_SIZE];
    uint64_t noff = 0, icsize;
    int icodec = CODEC_STORE, r;
    for (i = 0; i < l->n; i++)
        names += strlen(l->v[i].name) + 1;
    isz = INDEX_FIXED + BLOCK_REC * o->nb + ENTRY_REC * l->n + names;
    if (o->nb > UINT32_MAX || l->n > UINT32_MAX || !(idx = (uint8_t *)calloc(isz, 1))) return SLZ_E_NOMEM;
    wr32(idx, (uint32_t)o->nb);
    wr32(idx + 4, (uint32_t)l->n);
    wr64(idx + 8, names);
    wr32(idx + 16, bs);
    for (i = 0; i < o->nb; i++) {
        uint8_t *p = idx + INDEX_FIXED + BLOCK_REC * i;
        wr64(p, o->b[i].off);
        wr32(p + 8, o->b[i].csize);
        wr32(p + 12, o->b[i].usize);
        p[16] = o->b[i].codec;
        wr64(p + 24, o->b[i].hash);
    }
    for (i = 0; i < l->n; i++) {
        const witem *it = &l->v[i];
        uint8_t *p = idx + INDEX_FIXED + BLOCK_REC * o->nb + ENTRY_REC * i;
        size_t k = strlen(it->name);
        wr64(p, it->dir ? 0 : it->so);
        wr64(p + 8, it->dir ? 0 : it->size);
        wr64(p + 16, (uint64_t)it->mtime);
        wr32(p + 24, (uint32_t)noff);
        wr32(p + 28, (uint32_t)k);
        wr32(p + 32, it->mode);
        p[36] = (uint8_t)it->dir;
        p[37] = it->dup ? ENTRY_DEDUP : 0;
        wr64(p + 40, it->dir ? 0 : it->hash);
        memcpy(idx + INDEX_FIXED + BLOCK_REC * o->nb + ENTRY_REC * l->n + noff, it->name, k + 1);
        noff += k + 1;
    }
    icsize = isz;
#ifdef SLZ_ZSTD
    if (codec != CODEC_STORE && (cidx = (uint8_t *)malloc(ZSTD_compressBound(isz)))) {
        size_t c = ZSTD_compress(cidx, ZSTD_compressBound(isz), idx, isz, level > 19 ? level : 19);
        if (!ZSTD_isError(c) && c < isz) {
            icodec = CODEC_ZSTD;
            icsize = c;
        }
    }
#else
    (void)level;
    if (codec != CODEC_STORE && (cidx = (uint8_t *)malloc(lz4_bound(isz)))) {
        size_t c = lz4_compress(idx, isz, cidx);
        if (c && c < isz) {
            icodec = CODEC_LZ4;
            icsize = c;
        }
    }
#endif
    r = os_pwrite(o->out, icodec == CODEC_STORE ? idx : cidx, (size_t)icsize, o->pos);
    memset(t, 0, sizeof t);
    memcpy(t, TMAGIC, 4);
    wr32(t + 4, (uint32_t)icodec);
    wr64(t + 8, o->pos);
    wr64(t + 16, icsize);
    wr64(t + 24, isz);
    wr64(t + 32, xxh64(idx, isz, 0));
    wr64(t + 56, xxh64(t, 56, 0));
    if (!r) r = os_pwrite(o->out, t, TRAILER_SIZE, o->pos + icsize);
    o->pos += icsize + TRAILER_SIZE;
    free(idx);
    free(cidx);
    return r;
}

int slz_create(const char *path, const char *const *inputs, size_t ninputs, slz_create_opts *opts) {
    slz_create_opts def;
    wlist l;
    wout o;
    wbatch wb;
    uint8_t hdr[HDR_SIZE];
    char *part = NULL;
    size_t i, bs, fill = 0;
    uint64_t total_in = 0, done_in = 0, stream = 0;
    unsigned threads;
    int r = 0, codec;
    if (!opts) {
        memset(&def, 0, sizeof def);
        opts = &def;
    }
    memset(&l, 0, sizeof l);
    memset(&o, 0, sizeof o);
    memset(&wb, 0, sizeof wb);
    o.out = OS_BAD;
    bs = opts->block_size ? opts->block_size : DEF_BLOCK;
    if (bs < (64u << 10) || bs > MAX_BLOCK) return SLZ_E_LIMIT;
    codec = opts->codec == SLZ_CODEC_STORE ? CODEC_STORE : opts->codec == SLZ_CODEC_LZ4 ? CODEC_LZ4 : CODEC_ZSTD;
#ifndef SLZ_ZSTD
    if (opts->codec == SLZ_CODEC_ZSTD) return SLZ_E_NOTSUP;
    if (codec == CODEC_ZSTD) codec = CODEC_LZ4;
#endif
    threads = opts->threads ? opts->threads : os_ncpu();
    if (threads > 64) threads = 64;
    /* 1. inputs, order, duplicate names, deduplication */
    for (i = 0; i < ninputs && !r; i++)
        r = os_walk(inputs[i], collect, &l);
    if (r) goto out;
    qsort(l.v, l.n, sizeof *l.v, by_kind);
    {
        const char **names = (const char **)malloc((l.n + 1) * sizeof *names);
        if (!names) {
            r = SLZ_E_NOMEM;
            goto out;
        }
        for (i = 0; i < l.n; i++)
            names[i] = l.v[i].name;
        qsort(names, l.n, sizeof *names, strcmp_ptr);
        for (i = 1; i < l.n && !r; i++)
            if (!strcmp(names[i - 1], names[i])) r = SLZ_E_EXISTS; /* two inputs map to one name */
        free(names);
        if (r) goto out;
    }
    if (!(opts->flags & SLZ_NO_DEDUP) && (r = dedup(&l, bs))) goto out;
    for (i = 0; i < l.n; i++)
        if (!l.v[i].dir && !l.v[i].dup) total_in += l.v[i].size;
    /* 2. output and batch buffers (one block per thread in flight) */
    if (!(part = (char *)malloc(strlen(path) + 6))) {
        r = SLZ_E_NOMEM;
        goto out;
    }
    strcpy(part, path);
    strcat(part, ".part");
    if ((r = os_open(part, 1, 1, &o.out)) || (r = os_trunc(o.out, 0))) goto out;
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, MAGIC, 4);
    wr16(hdr + 4, 1);
    for (i = 0; i < l.n; i++)
        if (l.v[i].dup) wr32(hdr + 12, 1); /* optional feature: deduplicated entries */
    wr32(hdr + 28, (uint32_t)xxh64(hdr, 28, 0));
    if ((r = os_pwrite(o.out, hdr, HDR_SIZE, 0))) goto out;
    o.pos = HDR_SIZE;
    wb.codec = codec;
    wb.level = opts->level ? opts->level : 3;
    wb.bs = (uint32_t)bs;
    if (!(wb.s = (wslot *)calloc(threads, sizeof *wb.s))) {
        r = SLZ_E_NOMEM;
        goto out;
    }
    for (i = 0; i < threads; i++) {
        size_t ob = lz4_bound(bs);
#ifdef SLZ_ZSTD
        if (ZSTD_compressBound(bs) > ob) ob = ZSTD_compressBound(bs);
#endif
        if (!(wb.s[i].in = (uint8_t *)malloc(bs)) || !(wb.s[i].out = (uint8_t *)malloc(ob))) {
            r = SLZ_E_NOMEM;
            goto out;
        }
    }
    /* 3. the stream: file contents back to back, cut into blocks */
    for (i = 0; i < l.n && !r; i++) {
        witem *it = &l.v[i];
        os_fd f;
        uint64_t pos = 0;
        xxh64_t h;
        if (it->dir || it->dup || !it->size) continue;
        if ((r = os_open(it->path, 0, 0, &f))) break;
        it->so = stream;
        xxh64_init(&h, 0);
        while (pos < it->size && !r) {
            wslot *s = &wb.s[wb.ns];
            size_t k = (size_t)MIN((uint64_t)(bs - fill), it->size - pos);
            if (os_pread(f, s->in + fill, k, pos) != (int64_t)k) {
                r = SLZ_E_IO; /* the file shrank while being archived */
                break;
            }
            if (!it->hashed) xxh64_update(&h, s->in + fill, k);
            fill += k;
            pos += k;
            stream += k;
            done_in += k;
            if (fill == bs) {
                s->n = fill;
                fill = 0;
                if (++wb.ns == threads) r = flush_batch(&wb, &o, threads);
            }
            if (!r && opts->progress && opts->progress(opts->user, done_in, total_in)) r = SLZ_E_CANCEL;
        }
        os_close(f);
        if (!it->hashed) {
            it->hash = xxh64_digest(&h);
            it->hashed = 1;
        }
        opts->files++;
    }
    if (!r && fill) {
        wb.s[wb.ns++].n = fill;
        fill = 0;
    }
    if (!r) r = flush_batch(&wb, &o, threads);
    if (r) goto out;
    for (i = 0; i < l.n; i++) { /* duplicates point at their original's range */
        witem *it = &l.v[i];
        if (it->dup) {
            it->so = l.v[it->dup - 1].so;
            it->hash = l.v[it->dup - 1].hash;
            opts->files++;
        }
    }
    /* 4. index, trailer, durability, then the final name */
    if ((r = write_index(&l, &o, (uint32_t)bs, codec, wb.level)) || (r = os_sync(o.out, 0))) goto out;
    os_close(o.out);
    o.out = OS_BAD;
    if ((r = os_rename(part, path))) goto out;
    opts->in_bytes = total_in;
    opts->out_bytes = o.pos;
out:
    if (o.out != OS_BAD) os_close(o.out);
    if (r && part) os_unlink(part);
    for (i = 0; wb.s && i < threads; i++) {
        free(wb.s[i].in);
        free(wb.s[i].out);
#ifdef SLZ_ZSTD
        ZSTD_freeCCtx((ZSTD_CCtx *)wb.s[i].cctx);
#endif
    }
    free(wb.s);
    for (i = 0; i < l.n; i++) {
        free(l.v[i].path);
        free(l.v[i].name);
    }
    free(l.v);
    free(o.b);
    free(part);
    return r;
}
