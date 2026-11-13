/* slz - extraction engine.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * Normal mode: entries sorted by offset, N workers pull the next entry atomically, each with its
 *   own archive handle, inflater and cached directory fd.
 * Destructive mode: sequential; at checkpoints  outputs durable -> journal durable -> punch the
 *   consumed archive range. A crash/cancel at any point resumes from the last journal record
 *   (mid-entry for DEFLATE: block boundary + 32K window re-read from the output file).
 *   Peak disk use = max(archive, output) + one checkpoint interval. */
#include "internal.h"

#define CHUNK  (1u << 20)
#define PALIGN (1u << 20)
#define NPEND  64
#define JMAGIC 0x31657461745a4c53ull /* "SLZstat1" */

typedef struct {
    uint64_t magic, seq, fp, idx, doff, bitpos, ooff, punched, crc, sum;
} jrec;

typedef struct {
    slz_archive *a;
    slz_opts *o;
    const slz_entry **ord;
    uint64_t *lim;
    size_t n;
    volatile int64_t next, files, skipped, out, total;
    volatile int64_t err; /* first error, set once with os_cas, read atomically */
    int destr, resumed;
    unsigned ft;  /* decoder threads inside one 7z folder (solid LZMA2 splits into segments) */
    size_t *sched; /* parallel mode: work item to run k-th (NULL: in order) */
    int (*unit)(void *w, size_t k, const jrec *res); /* one work item: an entry, or a 7z folder */
    uint64_t punch_base;                             /* never punch below this (7z signature header) */
    os_fd arw, jf;
    jrec j;
    uint64_t lastck, every;
    os_fd pend[NPEND];
    int np;
    const char *dest;
} job;

typedef struct {
    job *j;
    os_fd in, out;
    os_dst dst;
    inf *z;
    uint8_t *buf;
    rng src; /* compressed bytes of the current entry */
    uint8_t *nbc, *nbu; /* .slz: compressed and decoded block buffers */
    void *zd;           /* .slz: zstd decoder */
    const slz_entry *e;
    size_t k;
    uint64_t wpos, doff;
    uint32_t crc;
} wk;

static int w_wr(void *u, const uint8_t *p, size_t n) {
    wk *w = (wk *)u;
    job *j = w->j;
    int64_t t;
    int r;
    if (j->a->fmt != SLZ_GZ && w->e && n > w->e->usize - w->wpos)
        return SLZ_E_CORRUPT; /* larger than declared */
    t = os_add(&j->out, (int64_t)n);
    if (j->o->max_total && (uint64_t)t > j->o->max_total) return SLZ_E_LIMIT;
    if (j->o->progress && j->o->progress(j->o->user, (uint64_t)t, (uint64_t)j->total)) return SLZ_E_CANCEL;
    if (j->a->fmt != SLZ_TAR && j->a->fmt != SLZ_TGZ && j->a->fmt != SLZ_SLZ)
        w->crc = slz_crc32(w->crc, p, n); /* tar: none; .slz: xxHash64 per block and file */
    if ((r = os_pwrite(w->out, p, n, w->wpos))) return r;
    w->wpos += n;
    return 0;
}

/* ---------- destructive journal ---------- */
static uint64_t jsum(const jrec *r) {
    const uint64_t *v = (const uint64_t *)r;
    uint64_t h = 0xcbf29ce484222325ull;
    int i;
    for (i = 0; i < 9; i++)
        h = (h ^ v[i]) * 0x100000001b3ull, h ^= h >> 29;
    return h;
}
static int jwrite(job *j) {
    int r;
    j->j.seq++;
    j->j.sum = jsum(&j->j);
    if ((r = os_pwrite(j->jf, &j->j, sizeof j->j, (j->j.seq & 1) * sizeof j->j))) return r;
    return os_sync(j->jf, 1);
}
static int sync_pend(job *j) {
    int i, r = 0;
    for (i = 0; i < j->np; i++) {
        if (!r) r = os_sync(j->pend[i], 0);
        os_close(j->pend[i]);
    }
    j->np = 0;
    return r;
}
static int ckpt(wk *w, uint64_t bitpos, int done) {
    job *j = w->j;
    uint64_t prev = j->j.punched, p = (bitpos / 8) & ~(uint64_t)(PALIGN - 1);
    int r;
    /* data and names of everything written so far must be durable before the journal moves on */
    if ((r = sync_pend(j)) || (w->out != OS_BAD && (r = os_sync(w->out, 0))) || (r = os_dst_sync(&w->dst)))
        return r;
    j->j.idx = w->k + (size_t)done;
    j->j.doff = w->doff;
    j->j.bitpos = done ? 0 : bitpos;
    j->j.ooff = done ? 0 : w->wpos;
    j->j.crc = done ? 0 : w->crc;
    if (p > prev) j->j.punched = p;
    if ((r = jwrite(j))) return r;
    j->lastck = bitpos / 8;
    return j->j.punched > prev ? os_punch(j->arw, prev, j->j.punched - prev) : 0;
}
static int w_ck(void *u, inf *s) { /* called by inflate at every block boundary */
    wk *w = (wk *)u;
    int r;
    if (inf_bitpos(s) / 8 - w->j->lastck < w->j->every) return 0;
    return (r = inf_flush(s)) ? r : ckpt(w, inf_bitpos(s), 0);
}

/* Entry fully written: verify size and checksum, set mtime, then close it, or in destructive
 * mode keep it open until the next checkpoint makes it durable. */
static int finish(wk *w, const slz_entry *e, int check_crc) {
    job *j = w->j;
    int r;
    if ((j->a->fmt == SLZ_GZ ? (w->wpos & 0xffffffffu) : w->wpos) != e->usize) return SLZ_E_CORRUPT;
    if (check_crc && w->crc != e->crc) return SLZ_E_CORRUPT;
    if (!(j->o->flags & SLZ_NO_MTIME)) os_mtime(w->out, e->mtime);
    os_add(&j->files, 1);
    if (!j->destr) {
        os_close(w->out);
    } else {
        if (j->np == NPEND && (r = sync_pend(j))) return r;
        j->pend[j->np++] = w->out;
    }
    w->out = OS_BAD;
    return 0;
}

static int create_mode(const job *j) {
    return ((j->o->flags & SLZ_OVERWRITE) || j->resumed) ? OC_OVERWRITE : OC_EXCL;
}

/* ---------- one zip / tar / gzip entry ---------- */
static int one(void *wv, size_t k, const jrec *res) {
    wk *w = (wk *)wv;
    job *j = w->j;
    const slz_entry *e = j->ord[k];
    char nm[4096];
    int r, n;
    unsigned skip = 0;
    size_t hn = 0;
    uint64_t doff = e->off, lim = j->lim[k];
    w->e = e;
    w->k = k;
    w->out = OS_BAD;
    if (e->type == SLZ_LINK || e->type == SLZ_OTHER) {
        os_add(&j->skipped, 1);
        return 0;
    }
    if ((n = sanitize(e->name, e->nlen, nm, sizeof nm)) <= 0) return n;
    if (e->type == SLZ_DIR) return os_create(&w->dst, nm, OC_MKDIR, 0, NULL);
    if (e->enc) return SLZ_E_ENCRYPTED;
    if (e->method != SLZ_M_STORE && e->method != SLZ_M_DEFLATE && e->method != SLZ_M_LZMA)
        return SLZ_E_METHOD;
    if (e->method == SLZ_M_STORE && e->csize != e->usize) return SLZ_E_CORRUPT;
    if (res && !res->ooff && !res->bitpos) res = NULL; /* checkpoint at entry start: nothing to resume */
    if (res)
        doff = res->doff; /* local header may already be punched */
    else if (j->a->fmt == SLZ_ZIP) {
        uint8_t h[30];
        if (e->off > lim || lim - e->off < 30 || os_pread(w->in, h, 30, e->off) != 30 ||
            rd32(h) != 0x04034b50)
            return SLZ_E_CORRUPT;
        doff = e->off + 30 + rd16(h + 26) + rd16(h + 28);
    }
    if (doff > lim || e->csize > lim - doff) return SLZ_E_CORRUPT; /* overlapping / out of bounds data */
    r = os_create(&w->dst, nm, res ? OC_RESUME : create_mode(j), e->mode, &w->out);
    if (r) return r;
    w->doff = w->src.pos = doff;
    w->src.end = doff + e->csize;
    w->wpos = 0;
    w->crc = 0;
    if (res) { /* resume inside this entry */
        w->wpos = res->ooff;
        w->crc = (uint32_t)res->crc;
        if (e->method == 0)
            w->src.pos = doff + w->wpos;
        else {
            w->src.pos = res->bitpos / 8;
            skip = (unsigned)(res->bitpos & 7);
            hn = (size_t)MIN((uint64_t)WIN, w->wpos);
            if (os_pread(w->out, w->buf, hn, w->wpos - hn) != (int64_t)hn) return SLZ_E_STATE;
        }
        os_add(&j->out, (int64_t)w->wpos);
        /* drop whatever was written past the checkpoint */
        if ((r = os_trunc(w->out, w->wpos))) return r;
    }
    /* A new file is already empty (created with O_EXCL / CREATE_NEW) and is never truncated: on
     * ext4 a truncate to size 0 arms auto_da_alloc, which makes close() start writeback of the
     * whole file, so extraction would run at disk speed instead of page-cache speed. */
    if (e->method == 0) {
        while (w->src.pos < w->src.end) {
            size_t c = (size_t)MIN((uint64_t)CHUNK, w->src.end - w->src.pos);
            if (os_pread(w->in, w->buf, c, w->src.pos) != (int64_t)c) return SLZ_E_CORRUPT;
            if ((r = w_wr(w, w->buf, c))) return r;
            w->src.pos += c;
            if (j->destr && w->src.pos - j->lastck >= j->every && (r = ckpt(w, w->src.pos * 8, 0))) return r;
        }
    } else if (e->method == SLZ_M_LZMA) { /* zip LZMA: version(2) props size(2) props(5), then data */
        uint8_t h[9];
        lzd *z;
        if (e->csize < 9 || os_pread(w->in, h, 9, w->src.pos) != 9 || rd16(h + 2) != 5) return SLZ_E_CORRUPT;
        w->src.pos += 9;
        if (!(z = lzd_new(rd32(h + 5), e->usize))) return SLZ_E_NOMEM;
        lzd_io(z, rng_read, &w->src, w_wr, w);
        r = lzd_lzma(z, h + 4, e->usize, 1);
        lzd_free(z);
        if (r) return r;
    } else {
        if (!w->z && !(w->z = inf_new())) return SLZ_E_NOMEM;
        inf_reset(w->z, w->src.pos, skip, w->buf, hn, w->wpos);
        w->z->rd = rng_read;
        w->z->wr = w_wr;
        w->z->ck = j->destr ? w_ck : NULL;
        w->z->ru = &w->src;
        w->z->wu = w;
        if ((r = inf_run(w->z))) return r;
    }
    if ((r = finish(w, e, j->a->fmt != SLZ_TAR))) return r; /* tar has no checksum */
    return j->destr && w->src.end - j->lastck >= j->every ? ckpt(w, w->src.end * 8, 1) : 0;
}

/* ---------- 7z: a folder is decoded once; its output is split across its files in order ---------- */
typedef struct {
    wk *w;
    const folder7 *f;
    size_t k;      /* next substream */
    uint64_t left; /* bytes still owed to the current file */
    uint32_t fcrc; /* CRC of the whole folder output */
    int discard;   /* current substream is not written (link, device) */
} split7;

/* The current file is complete: finish it, then open the next substream's file (empty ones are
 * finished right away). */
static int split_next(split7 *s) {
    wk *w = s->w;
    job *j = w->j;
    const z7 *z = j->a->z7;
    int r;
    if (w->e && !s->discard && (r = finish(w, w->e, z->entry_hascrc[w->e - j->a->e]))) return r;
    w->e = NULL;
    while (s->k < s->f->nsub) {
        const slz_entry *e = &j->a->e[z->sub_entry[s->f->first + s->k++]];
        char nm[4096];
        int n = 0;
        w->e = e;
        w->wpos = 0;
        w->crc = 0;
        s->left = e->usize;
        s->discard = 1;
        if (e->type == SLZ_LINK || e->type == SLZ_OTHER)
            os_add(&j->skipped, 1);
        else if ((n = sanitize(e->name, e->nlen, nm, sizeof nm)) < 0)
            return n;
        if (n > 0) {
            if ((r = os_create(&w->dst, nm, create_mode(j), e->mode, &w->out))) return r;
            s->discard = 0;
        }
        if (s->left) return 0;
        if (!s->discard && (r = finish(w, e, z->entry_hascrc[e - j->a->e]))) return r;
        w->e = NULL;
    }
    return 0;
}

static int split_wr(void *u, const uint8_t *p, size_t n) {
    split7 *s = (split7 *)u;
    int r;
    if (s->f->has_crc) s->fcrc = slz_crc32(s->fcrc, p, n);
    while (n) {
        size_t c;
        if (!s->left) return SLZ_E_CORRUPT; /* more output than the folder's files */
        c = (size_t)MIN((uint64_t)n, s->left);
        if (s->discard) {
            os_add(&s->w->j->out, (int64_t)c);
            s->w->wpos += c;
        } else if ((r = w_wr(s->w, p, c))) {
            return r;
        }
        s->left -= c;
        p += c;
        n -= c;
        if (!s->left && (r = split_next(s))) return r;
    }
    return 0;
}

static int one7(void *wv, size_t k, const jrec *res) {
    wk *w = (wk *)wv;
    job *j = w->j;
    const folder7 *f = &j->a->z7->f[k];
    split7 s;
    int r;
    (void)res; /* a folder restarts from its beginning on resume */
    memset(&s, 0, sizeof s);
    s.w = w;
    s.f = f;
    w->k = k;
    w->e = NULL;
    w->out = OS_BAD;
    w->doff = w->src.pos = f->pack_off;
    w->src.end = f->pack_off + f->pack_size;
    if (!f->nsub) return 0;
    if ((r = z7_supported(f))) return r; /* before creating any file */
    if ((r = split_next(&s)) || (r = z7_decode(w->in, f, split_wr, &s, j->ft))) return r;
    if (s.left || w->e || s.k != f->nsub || (f->has_crc && s.fcrc != f->crc)) return SLZ_E_CORRUPT;
    return j->destr && w->src.end - j->lastck >= j->every ? ckpt(w, w->src.end * 8, 1) : 0;
}

/* ---------- .slz: a block is decoded once; every file range it covers is written ---------- */

/* a partly written file (spanning several blocks): closed now, or in destructive mode kept until the
 * next checkpoint makes it durable */
static int park(wk *w) {
    job *j = w->j;
    int r;
    if (!j->destr) {
        os_close(w->out);
    } else {
        if (j->np == NPEND && (r = sync_pend(j))) return r;
        j->pend[j->np++] = w->out;
    }
    w->out = OS_BAD;
    return 0;
}

static int one_slz(void *wv, size_t k, const jrec *res) {
    wk *w = (wk *)wv;
    job *j = w->j;
    const natidx *n = j->a->nat;
    const natblock *b = &n->b[k];
    uint64_t bend = b->start + b->usize;
    size_t i;
    int r;
    (void)res; /* a block restarts from its beginning on resume */
    w->k = k;
    w->e = NULL;
    w->out = OS_BAD;
    w->doff = w->src.pos = b->off;
    w->src.end = b->off + b->csize;
    if (!w->nbu && (!(w->nbc = (uint8_t *)malloc(n->max_c + 1)) || !(w->nbu = (uint8_t *)malloc(n->max_u + 1))))
        return SLZ_E_NOMEM;
    if ((r = nat_decode(w->in, b, w->nbc, w->nbu, &w->zd))) return r;
    for (i = nat_first(j->a, b->start); i < n->norder; i++) {
        uint32_t ei = n->order[i];
        const slz_entry *e = &j->a->e[ei];
        uint64_t s0, s1;
        char nm[4096];
        int len;
        if (e->off >= bend) break;
        s0 = e->off > b->start ? e->off : b->start;
        s1 = e->off + e->usize < bend ? e->off + e->usize : bend;
        if ((len = sanitize(e->name, e->nlen, nm, sizeof nm)) < 0) return len;
        if (!len) continue;
        w->e = e;
        if (!n->spans[ei]) { /* the whole file is in this block: verify, create, write, finish */
            if (xxh64(w->nbu + (e->off - b->start), (size_t)e->usize, 0) != n->ehash[ei]) return SLZ_E_CORRUPT;
            if ((r = os_create(&w->dst, nm, create_mode(j), e->mode, &w->out))) return r;
            w->wpos = 0;
            if ((r = w_wr(w, w->nbu + (s0 - b->start), (size_t)(s1 - s0))) || (r = finish(w, e, 0))) return r;
        } else { /* one slice of a file pre-created by prep_slz; other blocks write the rest */
            if ((r = os_create(&w->dst, nm, OC_RESUME, e->mode, &w->out))) return r;
            w->wpos = s0 - e->off;
            if ((r = w_wr(w, w->nbu + (s0 - b->start), (size_t)(s1 - s0))) || (r = park(w))) return r;
        }
        w->e = NULL;
    }
    return j->destr && w->src.end - j->lastck >= j->every ? ckpt(w, w->src.end * 8, 1) : 0;
}

/* .slz before the blocks: directories, empty files, and every file spanning several blocks
 * (created once so that the blocks' workers only open and write their slice) */
static int prep_slz(job *j, wk *w) {
    const natidx *n = j->a->nat;
    size_t i;
    int r = 0;
    for (i = 0; i < j->a->n && !r; i++) {
        const slz_entry *e = &j->a->e[i];
        char nm[4096];
        int len = sanitize(e->name, e->nlen, nm, sizeof nm);
        if (len <= 0) {
            r = len;
            continue;
        }
        w->e = e;
        w->wpos = 0;
        if (e->type == SLZ_DIR) {
            r = os_create(&w->dst, nm, OC_MKDIR, 0, NULL);
        } else if (!e->usize) {
            if (!(r = os_create(&w->dst, nm, create_mode(j), e->mode, &w->out))) r = finish(w, e, 0);
        } else if (n->spans[i]) { /* on resume the data written so far must survive */
            if (!(r = os_create(&w->dst, nm, j->resumed ? OC_RESUME : create_mode(j), e->mode, &w->out))) r = park(w);
        }
    }
    w->e = NULL;
    return r;
}

/* .slz after the blocks: mtime of the files that spanned blocks */
static int done_slz(job *j, wk *w) {
    const natidx *n = j->a->nat;
    size_t i;
    int r = 0;
    for (i = 0; i < j->a->n && !r; i++) {
        const slz_entry *e = &j->a->e[i];
        char nm[4096];
        if (e->type != SLZ_FILE || !e->usize || !n->spans[i] || sanitize(e->name, e->nlen, nm, sizeof nm) <= 0) continue;
        if (!(r = os_create(&w->dst, nm, OC_RESUME, e->mode, &w->out))) {
            if (!(j->o->flags & SLZ_NO_MTIME)) os_mtime(w->out, e->mtime);
            os_add(&j->files, 1);
            r = park(w);
        }
    }
    return r;
}

/* 7z entries without data: directories and empty files */
static int prep7(job *j, wk *w) {
    size_t i;
    int r = 0;
    for (i = 0; i < j->a->n && !r; i++) {
        const slz_entry *e = &j->a->e[i];
        char nm[4096];
        int n;
        if (j->a->z7->entry_folder[i] != UINT32_MAX) continue;
        if (e->type == SLZ_LINK || e->type == SLZ_OTHER) {
            os_add(&j->skipped, 1);
            continue;
        }
        if ((n = sanitize(e->name, e->nlen, nm, sizeof nm)) <= 0) {
            r = n;
            continue;
        }
        w->e = e;
        if (e->type == SLZ_DIR) {
            r = os_create(&w->dst, nm, OC_MKDIR, 0, NULL);
        } else if (!(r = os_create(&w->dst, nm, create_mode(j), e->mode, &w->out))) {
            w->wpos = 0;
            r = finish(w, e, 0);
        }
    }
    w->e = NULL;
    return r;
}

static int wk_init(wk *w, job *j) {
    int r;
    memset(w, 0, sizeof *w);
    w->j = j;
    w->in = w->out = OS_BAD;
    if (!(w->buf = (uint8_t *)malloc(CHUNK))) return SLZ_E_NOMEM;
    if ((r = os_open(j->a->path, 0, 0, &w->in))) return r;
    w->src.fd = w->in;
    return os_dst_open(&w->dst, j->dest);
}
static void wk_free(wk *w) {
    if (w->out != OS_BAD) os_close(w->out);
    if (w->in != OS_BAD) os_close(w->in);
    os_dst_close(&w->dst);
    free(w->buf);
    free(w->z);
    free(w->nbc);
    free(w->nbu);
    nat_zfree(w->zd);
}

/* Parallel order: work items of 1 MiB or more first, largest first, so that a big one never starts
 * last while the other threads run out of work (longest-processing-time scheduling); the small
 * ones follow in on-disk order, which keeps reads sequential and consecutive files in the same
 * directory. NULL: keep the on-disk order. */
typedef struct {
    uint64_t size;
    size_t k;
} sitem;
static int by_size_desc(const void *x, const void *y) {
    const sitem *a = (const sitem *)x, *b = (const sitem *)y;
    return a->size != b->size ? (a->size < b->size) - (a->size > b->size) : (a->k > b->k) - (a->k < b->k);
}
static size_t *schedule(const job *j, unsigned threads) {
    sitem *big;
    size_t *s, k, nb = 0, n = 0;
    if (threads < 2 || j->n < 2 || j->a->fmt == SLZ_SLZ) return NULL; /* .slz blocks are equal-sized */
    s = (size_t *)malloc(j->n * sizeof *s);
    big = (sitem *)malloc(j->n * sizeof *big);
    if (!s || !big) {
        free(s);
        free(big);
        return NULL;
    }
    for (k = 0; k < j->n; k++) {
        uint64_t size = j->a->fmt == SLZ_7Z ? j->a->z7->f[k].usize : j->ord[k]->usize;
        if (size >= (1u << 20)) {
            big[nb].size = size;
            big[nb++].k = k;
        }
    }
    qsort(big, nb, sizeof *big, by_size_desc);
    for (k = 0; k < nb; k++)
        s[n++] = big[k].k;
    for (k = 0; k < j->n; k++) {
        uint64_t size = j->a->fmt == SLZ_7Z ? j->a->z7->f[k].usize : j->ord[k]->usize;
        if (size < (1u << 20)) s[n++] = k;
    }
    free(big);
    return s;
}

static void worker(void *p) {
    job *j = (job *)p;
    wk w;
    int r = wk_init(&w, j);
    while (!r && !os_add(&j->err, 0)) {
        int64_t k = os_add(&j->next, 1) - 1;
        if (k >= (int64_t)j->n) break;
        if ((r = j->unit(&w, j->sched ? j->sched[k] : (size_t)k, NULL)) && w.out != OS_BAD) {
            os_close(w.out);
            w.out = OS_BAD;
        }
    }
    if (r) os_cas(&j->err, 0, r); /* keep the first error */
    wk_free(&w);
}

/* ---------- tar.gz: single streaming pass ---------- */
typedef struct {
    wk *w;
    tarp t;
    int end;
    uint32_t crc;
    uint64_t n;
    const slz_entry *e;
} tgz;
static int tgz_wr(void *u, const uint8_t *p, size_t n) {
    tgz *g = (tgz *)u;
    wk *w = g->w;
    size_t used = 0;
    g->crc = slz_crc32(g->crc, p, n);
    g->n += n;
    while (used < n && !g->end) {
        size_t k;
        const uint8_t *dp;
        size_t dn;
        char nm[4096];
        int r = 0, ev;
        ev = tar_step(&g->t, p + used, n - used, &k, &dp, &dn);
        used += k;
        if (ev == T_ERR) return SLZ_E_CORRUPT;
        if (ev == T_END) g->end = 1;
        if (ev == T_ENTRY) {
            const slz_entry *e = &g->t.cur;
            int sn;
            if (w->out != OS_BAD) {
                if (!(w->j->o->flags & SLZ_NO_MTIME)) os_mtime(w->out, g->e->mtime);
                os_close(w->out);
                w->out = OS_BAD;
            }
            w->e = NULL;
            w->wpos = 0;
            g->e = e;
            if (e->type == SLZ_LINK || e->type == SLZ_OTHER) {
                os_add(&w->j->skipped, 1);
                continue;
            }
            if ((sn = sanitize(e->name, e->nlen, nm, sizeof nm)) < 0) return sn;
            if (!sn) continue;
            if (e->type == SLZ_DIR)
                r = os_create(&w->dst, nm, OC_MKDIR, 0, NULL);
            else if (!(r = os_create(&w->dst, nm, (w->j->o->flags & SLZ_OVERWRITE) ? OC_OVERWRITE : OC_EXCL,
                                     e->mode, &w->out)))
                os_add(&w->j->files, 1);
        }
        if (ev == T_DATA && w->out != OS_BAD) r = w_wr(w, dp, dn);
        if (r) return r;
    }
    return 0;
}
static int run_tgz(job *j, wk *w) {
    tgz *g = (tgz *)calloc(1, sizeof *g);
    int r;
    uint8_t tr[8];
    if (!g || (!w->z && !(w->z = inf_new()))) {
        free(g);
        return SLZ_E_NOMEM;
    }
    g->w = w;
    w->src.pos = j->a->gz_off;
    w->src.end = j->a->size - 8;
    inf_reset(w->z, w->src.pos, 0, NULL, 0, 0);
    w->z->rd = rng_read;
    w->z->wr = tgz_wr;
    w->z->ck = NULL;
    w->z->ru = &w->src;
    w->z->wu = g;
    r = inf_run(w->z);
    if (w->out != OS_BAD) {
        if (!r && !(j->o->flags & SLZ_NO_MTIME)) os_mtime(w->out, g->e->mtime);
        os_close(w->out);
        w->out = OS_BAD;
    }
    if (!r && (g->t.st != 4 && (g->t.st != 0 || g->t.hn))) r = SLZ_E_CORRUPT;
    if (!r &&
        (os_pread(w->in, tr, 8, j->a->size - 8) != 8 || rd32(tr) != g->crc || rd32(tr + 4) != (uint32_t)g->n))
        r = SLZ_E_CORRUPT;
    tar_free(&g->t);
    free(g);
    return r;
}

/* ---------- destructive driver ---------- */
static int destructive(job *j, wk *w) {
    size_t L = strlen(j->a->path), k;
    char *jp = (char *)malloc(L + 10);
    int r;
    jrec s[2];
    int64_t got;
    if (!jp) return SLZ_E_NOMEM;
    memcpy(jp, j->a->path, L);
    memcpy(jp + L, ".slzstate", 10);
    w->dst.track = 1; /* record directories that get new entries (made durable at checkpoints) */
    if ((r = os_open(j->a->path, 1, 0, &j->arw)) || (r = os_open_journal(jp, &j->jf))) goto out;
    got = os_pread(j->jf, s, sizeof s, 0);
    for (k = 0; got == (int64_t)sizeof s && k < 2; k++)
        if (s[k].magic == JMAGIC && s[k].sum == jsum(&s[k]) && s[k].fp == j->a->fp &&
            (!j->resumed || s[k].seq > j->j.seq)) {
            j->j = s[k];
            j->resumed = 1;
        }
    if (!j->resumed) {
        static const uint8_t z[8192] = {0};
        /* no valid journal record => nothing was punched yet: (re)start from scratch */
        /* probe hole-punch support on the same filesystem before touching anything */
        if ((r = os_pwrite(j->jf, z, sizeof z, 0)) || (r = os_punch(j->jf, 4096, 4096)) ||
            (r = os_trunc(j->jf, 0))) {
            os_close(j->jf);
            j->jf = OS_BAD;
            os_unlink(jp); /* unsupported: leave no trace */
            goto out;
        }
        memset(&j->j, 0, sizeof j->j);
        j->j.magic = JMAGIC;
        j->j.fp = j->a->fp;
        j->j.punched = j->punch_base;
        if ((r = snap_write(j->a, j->jf, SNAP_AT)) || (r = jwrite(j))) goto out; /* jwrite syncs both */
        if ((r = os_sync_parent(jp))) goto out; /* the journal's own name must survive a crash */
    } else if (j->j.punched > j->punch_base &&
               (r = os_punch(j->arw, j->punch_base, j->j.punched - j->punch_base))) {
        goto out; /* re-punch: the crash may have happened between journal write and punch */
    }
    j->lastck = j->j.punched;
    if ((j->a->fmt == SLZ_7Z && (r = prep7(j, w))) || (j->a->fmt == SLZ_SLZ && (r = prep_slz(j, w)))) goto out;
    for (k = j->resumed ? (size_t)j->j.idx : 0; k < j->n && !r; k++)
        if ((r = j->unit(w, k, (j->resumed && k == j->j.idx) ? &j->j : NULL)) && w->out != OS_BAD) {
            os_close(w->out);
            w->out = OS_BAD;
        }
    if (!r && j->a->fmt == SLZ_SLZ) r = done_slz(j, w);
    if (!r && !(r = sync_pend(j))) {
        j->j.idx = j->n;
        j->j.bitpos = j->j.ooff = 0;
        if (!(r = jwrite(j))) {
            os_close(j->arw);
            j->arw = OS_BAD;
            os_close(w->in);
            w->in = OS_BAD;
            os_close(j->a->fd);
            j->a->fd = OS_BAD;
            if (!(r = os_unlink(j->a->path))) {
                os_close(j->jf);
                j->jf = OS_BAD;
                r = os_unlink(jp);
            }
        }
    }
out:
    sync_pend(j);
    if (j->arw != OS_BAD) os_close(j->arw);
    if (j->jf != OS_BAD) os_close(j->jf);
    free(jp);
    return r;
}

static int by_off(const void *x, const void *y) {
    uint64_t a = (*(const slz_entry *const *)x)->off, b = (*(const slz_entry *const *)y)->off;
    return (a > b) - (a < b);
}

int slz_extract(slz_archive *a, const char *dest, slz_opts *opts) {
    job j;
    slz_opts def;
    size_t i;
    int r = 0;
    if (!opts) {
        memset(&def, 0, sizeof def);
        opts = &def;
    }
    memset(&j, 0, sizeof j);
    j.a = a;
    j.o = opts;
    j.dest = dest;
    /* work items: 7z folders, .slz blocks, otherwise entries */
    j.n = a->fmt == SLZ_7Z ? a->z7->nf : a->fmt == SLZ_SLZ ? a->nat->nb : a->n;
    j.unit = a->fmt == SLZ_7Z ? one7 : a->fmt == SLZ_SLZ ? one_slz : one;
    j.punch_base = a->fmt == SLZ_7Z || a->fmt == SLZ_SLZ ? PALIGN : 0; /* keep the header for re-opening */
    j.arw = j.jf = OS_BAD;
    j.destr = !!(opts->flags & SLZ_DESTRUCTIVE);
    j.every = opts->checkpoint ? opts->checkpoint : 64u << 20;
    { /* threads left over when there are fewer work items than threads go inside 7z folders */
        unsigned t = opts->threads ? opts->threads : os_ncpu();
        unsigned units = j.destr ? 1 : (unsigned)MIN(j.n ? j.n : 1, 64);
        j.ft = t > units ? t / units : 1;
    }
    if (a->partial && !j.destr) return SLZ_E_STATE;
    if (a->fd == OS_BAD) return SLZ_E_STATE;
    if (j.destr && a->fmt == SLZ_TGZ) return SLZ_E_NOTSUP;
    if (!(j.ord = (const slz_entry **)malloc(a->n * sizeof *j.ord + 1)) ||
        !(j.lim = (uint64_t *)malloc(a->n * sizeof *j.lim + 1))) {
        r = SLZ_E_NOMEM;
        goto out;
    }
    for (i = 0; i < a->n; i++) {
        j.ord[i] = &a->e[i];
        j.total += (int64_t)a->e[i].usize;
    }
    /* Overlap guard (zip, tar, gzip): in offset order, entry i owns [off_i, off_{i+1}) and nothing
     * more - the last one ends at the central directory / gzip trailer / end of file. one()
     * refuses any local header or compressed data that crosses its limit, so no two entries can
     * share bytes (overlap bombs) and destructive mode never punches data still needed. */
    qsort(j.ord, a->n, sizeof *j.ord, by_off);
    for (i = 0; i < a->n; i++)
        j.lim[i] = i + 1 < a->n        ? j.ord[i + 1]->off
                   : a->fmt == SLZ_ZIP ? a->cd_off
                   : a->fmt == SLZ_GZ  ? a->size - 8
                                       : a->size;
    if (j.destr || a->fmt == SLZ_TGZ) {
        wk w;
        if (!(r = wk_init(&w, &j))) r = j.destr ? destructive(&j, &w) : run_tgz(&j, &w);
        wk_free(&w);
    } else {
        unsigned t = opts->threads ? opts->threads : os_ncpu();
        if (a->fmt == SLZ_7Z || a->fmt == SLZ_SLZ) { /* directories, empty files first; units in parallel */
            wk w;
            if (!(r = wk_init(&w, &j))) r = a->fmt == SLZ_7Z ? prep7(&j, &w) : prep_slz(&j, &w);
            wk_free(&w);
        }
        if (!r) {
            unsigned nt = MIN(t, (unsigned)MIN(j.n ? j.n : 1, 64));
            j.sched = schedule(&j, nt);
            os_run(nt, worker, &j);
            r = (int)j.err;
        }
        if (!r && a->fmt == SLZ_SLZ) {
            wk w;
            if (!(r = wk_init(&w, &j))) r = done_slz(&j, &w);
            wk_free(&w);
        }
    }
    opts->files = (uint64_t)j.files;
    opts->skipped = (uint64_t)j.skipped;
out:
    free(j.ord);
    free(j.lim);
    free(j.sched);
    return r;
}
