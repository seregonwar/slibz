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
    const slz_entry *e;
    size_t k;
    uint64_t wpos, doff;
    uint32_t crc;
    wr_fn put; /* slz_read: the caller's sink instead of the output file */
    void *pu;
} wk;

/* n more bytes decoded: max_total, then progress */
static int account(job *j, uint64_t n) {
    int64_t t = os_add(&j->out, (int64_t)n);
    if (j->o->max_total && (uint64_t)t > j->o->max_total) return SLZ_E_LIMIT;
    if (j->o->progress && j->o->progress(j->o->user, (uint64_t)t, (uint64_t)j->total)) return SLZ_E_CANCEL;
    return 0;
}

static int w_wr(void *u, const uint8_t *p, size_t n) {
    wk *w = (wk *)u;
    job *j = w->j;
    int r;
    if (j->a->fmt != SLZ_GZ && w->e && n > w->e->usize - w->wpos)
        return SLZ_E_CORRUPT; /* larger than declared */
    if ((r = account(j, n))) return r;
    if (j->a->fmt != SLZ_TAR && j->a->fmt != SLZ_TGZ) w->crc = slz_crc32(w->crc, p, n); /* tar: no checksum */
    if ((r = w->put ? w->put(w->pu, p, n) : os_pwrite(w->out, p, n, w->wpos))) return r;
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

/* All of the entry's data went through w_wr: exact size (gzip: modulo 2^32, as its trailer keeps
 * it) and checksum. */
static int verify(const wk *w, const slz_entry *e, int check_crc) {
    if ((w->j->a->fmt == SLZ_GZ ? (w->wpos & 0xffffffffu) : w->wpos) != e->usize) return SLZ_E_CORRUPT;
    return check_crc && w->crc != e->crc ? SLZ_E_CORRUPT : 0;
}

/* Entry fully written: verify it, set mtime, then close it, or in destructive mode keep it open
 * until the next checkpoint makes it durable. */
static int finish(wk *w, const slz_entry *e, int check_crc) {
    job *j = w->j;
    int r;
    if ((r = verify(w, e, check_crc))) return r;
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

/* Checks shared with slz_read: can the entry be decoded, and where do its compressed bytes start
 * (*doff). lim = where the next entry starts, res = resume record (local header maybe punched). */
static int entry_data(const wk *w, const slz_entry *e, uint64_t lim, const jrec *res, uint64_t *doff) {
    *doff = res ? res->doff : e->off;
    if (e->enc) return SLZ_E_ENCRYPTED;
    if (e->method != SLZ_M_STORE && e->method != SLZ_M_DEFLATE && e->method != SLZ_M_LZMA)
        return SLZ_E_METHOD;
    if (e->method == SLZ_M_STORE && e->csize != e->usize) return SLZ_E_CORRUPT;
    if (!res && w->j->a->fmt == SLZ_ZIP) {
        uint8_t h[30];
        if (e->off > lim || lim - e->off < 30 || os_pread(w->in, h, 30, e->off) != 30 ||
            rd32(h) != 0x04034b50)
            return SLZ_E_CORRUPT;
        *doff = e->off + 30 + rd16(h + 26) + rd16(h + 28);
    }
    return *doff > lim || e->csize > lim - *doff ? SLZ_E_CORRUPT : 0; /* overlapping / out of bounds data */
}

/* Decode [w->src.pos, w->src.end) through w_wr. skip, hn: resume inside a DEFLATE stream (bits to
 * skip, bytes of history in w->buf). */
static int decode(wk *w, const slz_entry *e, unsigned skip, size_t hn) {
    job *j = w->j;
    int r;
    if (e->method == SLZ_M_STORE) {
        if (!w->buf && !(w->buf = (uint8_t *)malloc(CHUNK))) return SLZ_E_NOMEM;
        while (w->src.pos < w->src.end) {
            size_t c = (size_t)MIN((uint64_t)CHUNK, w->src.end - w->src.pos);
            if (os_pread(w->in, w->buf, c, w->src.pos) != (int64_t)c) return SLZ_E_CORRUPT;
            if ((r = w_wr(w, w->buf, c))) return r;
            w->src.pos += c;
            if (j->destr && w->src.pos - j->lastck >= j->every && (r = ckpt(w, w->src.pos * 8, 0))) return r;
        }
        return 0;
    }
    if (e->method == SLZ_M_LZMA) { /* zip LZMA: version(2) props size(2) props(5), then data */
        uint8_t h[9];
        lzd *z;
        if (e->csize < 9 || os_pread(w->in, h, 9, w->src.pos) != 9 || rd16(h + 2) != 5) return SLZ_E_CORRUPT;
        w->src.pos += 9;
        if (!(z = lzd_new(rd32(h + 5), e->usize))) return SLZ_E_NOMEM;
        lzd_io(z, rng_read, &w->src, w_wr, w);
        r = lzd_lzma(z, h + 4, e->usize, 1);
        lzd_free(z);
        return r;
    }
    if (!w->z && !(w->z = inf_new())) return SLZ_E_NOMEM;
    inf_reset(w->z, w->src.pos, skip, w->buf, hn, w->wpos);
    w->z->rd = rng_read;
    w->z->wr = w_wr;
    w->z->ck = j->destr ? w_ck : NULL;
    w->z->ru = &w->src;
    w->z->wu = w;
    return inf_run(w->z);
}

static int one(void *wv, size_t k, const jrec *res) {
    wk *w = (wk *)wv;
    job *j = w->j;
    const slz_entry *e = j->ord[k];
    char nm[4096];
    int r, n;
    unsigned skip = 0;
    size_t hn = 0;
    uint64_t doff;
    w->e = e;
    w->k = k;
    w->out = OS_BAD;
    if (e->type == SLZ_LINK || e->type == SLZ_OTHER) {
        os_add(&j->skipped, 1);
        return 0;
    }
    if ((n = sanitize(e->name, e->nlen, nm, sizeof nm)) <= 0) return n;
    if (e->type == SLZ_DIR) return os_create(&w->dst, nm, OC_MKDIR, 0, NULL);
    if (res && !res->ooff && !res->bitpos) res = NULL; /* checkpoint at entry start: nothing to resume */
    if ((r = entry_data(w, e, j->lim[k], res, &doff))) return r;
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
    }
    if ((r = os_trunc(w->out, w->wpos)) || (r = decode(w, e, skip, hn))) return r;
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
    if ((r = split_next(&s)) || (r = z7_decode(w->in, f, f->usize, split_wr, &s))) return r;
    if (s.left || w->e || s.k != f->nsub || (f->has_crc && s.fcrc != f->crc)) return SLZ_E_CORRUPT;
    return j->destr && w->src.end - j->lastck >= j->every ? ckpt(w, w->src.end * 8, 1) : 0;
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
}

static void worker(void *p) {
    job *j = (job *)p;
    wk w;
    int r = wk_init(&w, j);
    while (!r && !os_add(&j->err, 0)) {
        int64_t k = os_add(&j->next, 1) - 1;
        if (k >= (int64_t)j->n) break;
        if ((r = j->unit(&w, (size_t)k, NULL)) && w.out != OS_BAD) {
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
    if (j->a->fmt == SLZ_7Z && (r = prep7(j, w))) goto out;
    for (k = j->resumed ? (size_t)j->j.idx : 0; k < j->n && !r; k++)
        if ((r = j->unit(w, k, (j->resumed && k == j->j.idx) ? &j->j : NULL)) && w->out != OS_BAD) {
            os_close(w->out);
            w->out = OS_BAD;
        }
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
    j.n = a->fmt == SLZ_7Z ? a->z7->nf : a->n; /* work items: 7z folders, otherwise entries */
    j.unit = a->fmt == SLZ_7Z ? one7 : one;
    j.punch_base = a->fmt == SLZ_7Z ? PALIGN : 0; /* keep the 7z signature header */
    j.arw = j.jf = OS_BAD;
    j.destr = !!(opts->flags & SLZ_DESTRUCTIVE);
    j.every = opts->checkpoint ? opts->checkpoint : 64u << 20;
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
        if (a->fmt == SLZ_7Z) { /* directories and empty files first, then folders in parallel */
            wk w;
            if (!(r = wk_init(&w, &j))) r = prep7(&j, &w);
            wk_free(&w);
        }
        if (!r) {
            os_run(MIN(t, (unsigned)MIN(j.n ? j.n : 1, 64)), worker, &j);
            r = (int)j.err;
        }
    }
    opts->files = (uint64_t)j.files;
    opts->skipped = (uint64_t)j.skipped;
out:
    free(j.ord);
    free(j.lim);
    return r;
}

/* ---------- slz_read: one entry to a callback or a buffer, nothing written to disk ---------- */
#define RD_DONE (-1000) /* private: the entry is complete, stop decoding (never returned) */

/* Before anything is allocated: the declared size must fit cap, and max_total must cover every
 * byte that will be decoded (tar.gz and solid 7z: those before the entry too). */
static int rd_limits(wk *w, uint64_t cap, uint64_t need) {
    job *j = w->j;
    j->total = (int64_t)need;
    return w->e->usize > cap || (j->o->max_total && need > j->o->max_total) ? SLZ_E_LIMIT : 0;
}

/* zip, tar, gzip: the entry's own bytes, under slz_extract's overlap guard (in O(n), no sort) */
static int rd_one(wk *w, size_t i, uint64_t cap) {
    const slz_archive *a = w->j->a;
    const slz_entry *e = w->e;
    uint64_t lim = a->fmt == SLZ_ZIP ? a->cd_off : a->fmt == SLZ_GZ ? a->size - 8 : a->size, doff;
    size_t k;
    int r;
    for (k = 0; k < a->n; k++) {
        if (k == i || a->e[k].off < e->off) continue;
        if (a->e[k].off == e->off) return SLZ_E_CORRUPT; /* two entries share their data */
        lim = MIN(lim, a->e[k].off);
    }
    if ((r = entry_data(w, e, lim, NULL, &doff)) || (r = rd_limits(w, cap, e->usize))) return r;
    w->src.pos = doff;
    w->src.end = doff + e->csize;
    if ((r = decode(w, e, 0, 0))) return r;
    return verify(w, e, a->fmt != SLZ_TAR); /* tar has no checksum */
}

/* 7z: decode the entry's folder up to the end of the entry; in a solid folder the files before
 * it are decoded and dropped. */
typedef struct {
    wk *w;
    const folder7 *f;
    uint64_t skip; /* bytes of earlier files still to drop */
    uint32_t fcrc;
    int last; /* the entry ends the folder: it is decoded fully and its CRC checked too */
} rd7;

static int rd7_wr(void *u, const uint8_t *p, size_t n) {
    rd7 *s = (rd7 *)u;
    wk *w = s->w;
    size_t c = (size_t)MIN((uint64_t)n, s->skip);
    int r;
    if (s->last && s->f->has_crc) s->fcrc = slz_crc32(s->fcrc, p, n);
    if (c) {
        if ((r = account(w->j, c))) return r;
        s->skip -= c;
        p += c;
        n -= c;
    }
    if (n && (r = w_wr(w, p, (size_t)MIN((uint64_t)n, w->e->usize - w->wpos)))) return r;
    return !s->last && w->wpos == w->e->usize ? RD_DONE : 0;
}

static int rd_7z(wk *w, size_t i, uint64_t cap) {
    const slz_archive *a = w->j->a;
    const z7 *z = a->z7;
    const slz_entry *e = w->e;
    rd7 s;
    size_t k;
    int r;
    if (z->entry_folder[i] == UINT32_MAX) return e->usize ? SLZ_E_CORRUPT : 0; /* no data: empty file */
    if (z->entry_folder[i] >= z->nf) return SLZ_E_CORRUPT;
    memset(&s, 0, sizeof s);
    s.w = w;
    s.f = &z->f[z->entry_folder[i]];
    if ((r = z7_supported(s.f))) return r;
    for (k = 0; k < s.f->nsub && z->sub_entry[s.f->first + k] != i; k++)
        s.skip += a->e[z->sub_entry[s.f->first + k]].usize;
    if (k == s.f->nsub) return SLZ_E_CORRUPT;
    if (!e->usize) return verify(w, e, z->entry_hascrc[i]);
    if ((r = rd_limits(w, cap, s.skip + e->usize))) return r; /* substream sizes add up to f->usize */
    s.last = s.skip + e->usize == s.f->usize;
    r = z7_decode(w->in, s.f, s.skip + e->usize, rd7_wr, &s);
    if (r == RD_DONE) r = 0;
    if (!r) r = verify(w, e, z->entry_hascrc[i]);
    return !r && s.last && s.f->has_crc && s.fcrc != s.f->crc ? SLZ_E_CORRUPT : r;
}

/* tar.gz: inflate from the start up to the end of the entry, found again by its data offset */
typedef struct {
    wk *w;
    tarp t;
    uint64_t pos; /* tar bytes consumed */
    int on;       /* inside the entry's data */
} rdtgz;

static int rdtgz_wr(void *u, const uint8_t *p, size_t n) {
    rdtgz *g = (rdtgz *)u;
    wk *w = g->w;
    const slz_entry *e = w->e;
    size_t used = 0;
    while (used < n) {
        size_t k, dn = 0;
        const uint8_t *dp = NULL;
        int r, ev = tar_step(&g->t, p + used, n - used, &k, &dp, &dn);
        used += k;
        if (ev == T_ERR || ev == T_END) return SLZ_E_CORRUPT; /* the tar ends before the entry does */
        if (ev == T_DATA && g->on) {
            if ((r = w_wr(w, dp, dn))) return r;
            if (w->wpos == e->usize) return RD_DONE;
            continue;
        }
        if ((r = account(w->j, k))) return r; /* headers, padding, other entries: decoded, dropped */
        g->pos += k;
        if (ev == T_ENTRY && g->pos >= e->off) {
            if (g->pos != e->off || g->t.cur.type != SLZ_FILE || g->t.cur.usize != e->usize)
                return SLZ_E_CORRUPT; /* not the entry the index found there */
            g->on = 1;
        }
    }
    return 0;
}

static int rd_tgz(wk *w, uint64_t cap) {
    const slz_archive *a = w->j->a;
    const slz_entry *e = w->e;
    rdtgz *g;
    int r;
    if (!e->usize) return 0;
    if ((r = rd_limits(w, cap, e->off > UINT64_MAX - e->usize ? UINT64_MAX : e->off + e->usize))) return r;
    if (!(g = (rdtgz *)calloc(1, sizeof *g)) || !(w->z = inf_new())) {
        free(g);
        return SLZ_E_NOMEM;
    }
    g->w = w;
    w->src.pos = a->gz_off;
    w->src.end = a->size - 8;
    inf_reset(w->z, w->src.pos, 0, NULL, 0, 0);
    w->z->rd = rng_read;
    w->z->wr = rdtgz_wr;
    w->z->ck = NULL;
    w->z->ru = &w->src;
    w->z->wu = g;
    r = inf_run(w->z);
    tar_free(&g->t);
    free(g);
    return r == RD_DONE ? 0 : r ? r : SLZ_E_CORRUPT; /* the stream ended before the entry did */
}

static int rd_entry(slz_archive *a, size_t i, wr_fn put, void *pu, slz_opts *o, uint64_t cap) {
    slz_opts def;
    job j;
    wk w;
    int r;
    if (a->partial || a->fd == OS_BAD) return SLZ_E_STATE;
    if (i >= slz_count(a)) return a->lazy_err ? a->lazy_err : SLZ_E_ARG;
    if (a->e[i].type != SLZ_FILE) return SLZ_E_NOTSUP;
    if (!o) {
        memset(&def, 0, sizeof def);
        o = &def;
    }
    memset(&j, 0, sizeof j);
    j.a = a;
    j.o = o;
    memset(&w, 0, sizeof w);
    w.j = &j;
    w.in = w.src.fd = a->fd;
    w.out = OS_BAD;
    w.e = &a->e[i];
    w.put = put;
    w.pu = pu;
    r = a->fmt == SLZ_7Z ? rd_7z(&w, i, cap) : a->fmt == SLZ_TGZ ? rd_tgz(&w, cap) : rd_one(&w, i, cap);
    free(w.buf);
    free(w.z);
    return r;
}

typedef struct {
    slz_write_cb cb;
    void *user;
} rdcb;
static int cb_put(void *u, const uint8_t *p, size_t n) {
    const rdcb *c = (const rdcb *)u;
    return n && c->cb(c->user, p, n) ? SLZ_E_CANCEL : 0;
}
int slz_read(slz_archive *a, size_t i, slz_write_cb cb, void *user, slz_opts *opts) {
    rdcb c;
    if (!a || !cb) return SLZ_E_ARG;
    c.cb = cb;
    c.user = user;
    return rd_entry(a, i, cb_put, &c, opts, UINT64_MAX);
}

typedef struct {
    uint8_t *p;
    size_t n, cap;
} rdmem;
static int mem_put(void *u, const uint8_t *p, size_t n) {
    rdmem *m = (rdmem *)u;
    size_t c = MIN(n, m->cap - m->n);
    if (c) memcpy(m->p + m->n, p, c);
    m->n += c;
    return c < n ? SLZ_E_LIMIT : 0; /* never past cap, whatever the archive declared */
}
int slz_read_mem(slz_archive *a, size_t i, void *buf, size_t cap, size_t *n) {
    rdmem m;
    int r;
    if (n) *n = 0;
    if (!a || (!buf && cap)) return SLZ_E_ARG;
    m.p = (uint8_t *)buf;
    m.n = 0;
    m.cap = cap;
    r = rd_entry(a, i, mem_put, &m, NULL, cap);
    if (n) *n = m.n;
    return r;
}
