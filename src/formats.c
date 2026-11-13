/* slz - archive parsing: ZIP/ZIP64 central directory (one read, names compacted in place),
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * tar (ustar/GNU/pax, push parser shared by tar and tar.gz), gzip; tree listing; path sanitizer. */
#include "internal.h"
#include <stdio.h>

#define MAX_META (1u << 20)

#ifdef _WIN32
static int reserved(const char *s, size_t k) { /* CON, NUL, COM1.txt, ... */
    char u[8];
    size_t b = 0;
    while (b < k && b < 7 && s[b] != '.') {
        u[b] = (char)(s[b] >= 'a' && s[b] <= 'z' ? s[b] - 32 : s[b]);
        b++;
    }
    if (b < k && s[b] != '.') return 0;
    u[b] = 0;
    if (b == 3) return !strcmp(u, "CON") || !strcmp(u, "PRN") || !strcmp(u, "AUX") || !strcmp(u, "NUL");
    if (b == 4) return (!memcmp(u, "COM", 3) || !memcmp(u, "LPT", 3)) && u[3] >= '0' && u[3] <= '9';
    return !strcmp(u, "CONIN$") || !strcmp(u, "CONOUT$");
}
#endif

/* ---------- path sanitizer: the single gate between archive names and the filesystem ---------- */
int sanitize(const char *in, size_t n, char *out, size_t cap) {
    size_t i = 0, o = 0;
    if (n && (in[0] == '/' || in[0] == '\\')) return SLZ_E_UNSAFE;
    while (i < n) {
        size_t s = i, k;
        while (i < n && in[i] != '/' && in[i] != '\\') {
            unsigned char c = (unsigned char)in[i++];
            if (c < 0x20 || c == 0x7f) return SLZ_E_UNSAFE;
#ifdef _WIN32
            if (strchr(":*?\"<>|", c)) return SLZ_E_UNSAFE;
#endif
        }
        k = i - s;
        i++;
        if (!k || (k == 1 && in[s] == '.')) continue;
        if (k == 2 && in[s] == '.' && in[s + 1] == '.') return SLZ_E_UNSAFE;
#ifdef _WIN32
        if (in[s + k - 1] == '.' || in[s + k - 1] == ' ' || reserved(in + s, k)) return SLZ_E_UNSAFE;
#endif
        if (o + k + 2 > cap) return SLZ_E_LIMIT;
        if (o) out[o++] = '/';
        memcpy(out + o, in + s, k);
        o += k;
    }
    out[o] = 0;
    return (int)o;
}

/* ---------- tar push parser ---------- */
static uint64_t num(const uint8_t *p, size_t n, int *bad) {
    uint64_t v = 0;
    size_t i = 0;
    if (p[0] & 0x80) { /* GNU base-256 */
        if (p[0] != 0x80) {
            *bad = 1;
            return 0;
        }
        for (i = 1; i < n; i++) {
            if (v >> 55) {
                *bad = 1;
                return 0;
            }
            v = v << 8 | p[i];
        }
        return v;
    }
    while (i < n && (p[i] == ' ' || p[i] == 0))
        i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (v >> 60) {
            *bad = 1;
            return 0;
        }
        v = v << 3 | (uint64_t)(p[i] - '0');
    }
    return v;
}

static int set_lname(tarp *t, const char *s, size_t n) {
    free(t->lname);
    if (!(t->lname = (char *)malloc(n + 1))) return SLZ_E_NOMEM;
    memcpy(t->lname, s, n);
    t->lname[n] = 0;
    return 0;
}

static int pax(tarp *t) {
    size_t i = 0;
    while (i < t->mn) {
        size_t len = 0, j = i, k;
        while (j < t->mn && t->meta[j] >= '0' && t->meta[j] <= '9' && len < MAX_META)
            len = len * 10 + (size_t)(t->meta[j++] - '0');
        if (j >= t->mn || t->meta[j] != ' ' || len <= j - i || len > t->mn - i ||
            t->meta[i + len - 1] != '\n')
            return SLZ_E_CORRUPT;
        for (k = ++j; k < i + len - 1 && t->meta[k] != '='; k++)
            ;
        if (k >= i + len - 1) return SLZ_E_CORRUPT; /* "key" without '=': value length would underflow */
        if (k - j == 4 && !memcmp(t->meta + j, "path", 4)) {
            int r = set_lname(t, t->meta + k + 1, i + len - 2 - k);
            if (r) return r;
        }
        if (k - j == 4 && !memcmp(t->meta + j, "size", 4)) {
            uint64_t v = 0;
            size_t q;
            for (q = k + 1; q < i + len - 1; q++) {
                if (t->meta[q] < '0' || t->meta[q] > '9' || v >> 59) return SLZ_E_CORRUPT;
                v = v * 10 + (uint64_t)(t->meta[q] - '0');
            }
            t->psize = v;
            t->haspsize = 1;
        }
        i += len;
    }
    return 0;
}

static int header(tarp *t) {
    const uint8_t *h = t->h;
    unsigned sum = 0, i;
    int bad = 0, sum_s = 0;
    uint64_t size, chk;
    char ty = (char)h[156];
    for (i = 0; i < 512; i++) {
        unsigned c = (i >= 148 && i < 156) ? ' ' : h[i];
        sum += c;
        sum_s += (i >= 148 && i < 156) ? ' ' : (signed char)h[i];
    }
    if (sum == 8 * ' ') { /* all-zero block */
        for (i = 0; i < 512 && !h[i]; i++)
            ;
        if (i == 512) return T_END;
    }
    chk = num(h + 148, 8, &bad);
    size = num(h + 124, 12, &bad);
    if (bad || (chk != sum && (int64_t)chk != sum_s)) return T_ERR;
    if (t->haspsize && ty != 'x' && ty != 'g') size = t->psize;
    t->left = size;
    t->pad = (512 - (size & 511)) & 511;
    if (ty == 'L' || ty == 'x' || ty == 'g' || ty == 'K') {
        if (size > MAX_META) return T_ERR;
        free(t->meta);
        if (!(t->meta = (char *)malloc((size_t)size + 1))) return T_ERR;
        t->mn = 0;
        t->mtype = ty;
        t->st = 1;
        return T_MORE;
    }
    memset(&t->cur, 0, sizeof t->cur);
    if (t->lname && strlen(t->lname) >= sizeof t->name) return T_ERR;
    if (t->lname) {
        snprintf(t->name, sizeof t->name, "%s", t->lname);
        free(t->lname);
        t->lname = NULL;
    } else if (!memcmp(h + 257, "ustar", 5) && h[345])
        snprintf(t->name, sizeof t->name, "%.155s/%.100s", (const char *)h + 345, (const char *)h);
    else
        snprintf(t->name, sizeof t->name, "%.100s", (const char *)h);
    t->cur.name = t->name;
    t->cur.nlen = (uint32_t)strlen(t->name);
    t->cur.type = (ty == '0' || ty == 0 || ty == '7') ? SLZ_FILE
                  : ty == '5'                         ? SLZ_DIR
                  : (ty == '1' || ty == '2')          ? SLZ_LINK
                                                      : SLZ_OTHER;
    if (t->cur.type == SLZ_FILE && t->cur.nlen && t->name[t->cur.nlen - 1] == '/') t->cur.type = SLZ_DIR;
    t->cur.mode = (uint32_t)num(h + 100, 8, &bad) & 07777;
    t->cur.mtime = (int64_t)num(h + 136, 12, &bad);
    t->cur.usize = t->cur.csize = size;
    t->haspsize = 0;
    t->st = 2;
    return T_ENTRY;
}

/* st: 0 header, 1 meta data, 2 entry data, 3 padding, 4 end */
int tar_step(tarp *t, const uint8_t *p, size_t n, size_t *used, const uint8_t **dp, size_t *dn) {
    size_t u = 0;
    int r = T_MORE;
    while (u < n && r == T_MORE) {
        size_t k;
        switch (t->st) {
            case 0:
                k = MIN(n - u, 512 - t->hn);
                memcpy(t->h + t->hn, p + u, k);
                t->hn += k;
                u += k;
                if (t->hn == 512) {
                    t->hn = 0;
                    r = header(t);
                    if (r == T_END) t->st = 4;
                }
                break;
            case 1:
                k = (size_t)MIN((uint64_t)(n - u), t->left);
                memcpy(t->meta + t->mn, p + u, k);
                t->mn += k;
                u += k;
                t->left -= k;
                if (!t->left) {
                    t->meta[t->mn] = 0;
                    t->st = 3;
                    if (t->mtype == 'L') {
                        if (set_lname(t, t->meta, strlen(t->meta))) r = T_ERR;
                    } else if (t->mtype == 'x' && pax(t))
                        r = T_ERR;
                }
                break;
            case 2:
                if (!t->left) {
                    t->st = 3;
                    break;
                }
                k = (size_t)MIN((uint64_t)(n - u), t->left);
                *dp = p + u;
                *dn = k;
                u += k;
                t->left -= k;
                r = T_DATA;
                break;
            case 3:
                k = (size_t)MIN((uint64_t)(n - u), t->pad);
                u += k;
                t->pad -= k;
                if (!t->pad) t->st = 0;
                break;
            default:
                u = n;
                r = T_END;
        }
    }
    if (t->st == 2 && !t->left && r == T_MORE) t->st = 3;
    *used = u;
    return r;
}

/* first block is a valid tar header (regular or pax/GNU meta header) */
static int is_tar(const uint8_t *h) {
    tarp *t = (tarp *)calloc(1, sizeof *t);
    size_t k, dn;
    const uint8_t *dp;
    int ev, ok;
    if (!t) return 0;
    ev = tar_step(t, h, 512, &k, &dp, &dn);
    ok = ev == T_ENTRY || (ev == T_MORE && t->st == 1);
    tar_free(t);
    free(t);
    return ok;
}

void tar_free(tarp *t) {
    free(t->meta);
    free(t->lname);
    t->meta = t->lname = NULL;
}

/* ---------- gzip ---------- */
static int gz_header(const uint8_t *p, size_t n, size_t *hlen, const char **fname) {
    size_t i = 10;
    *fname = NULL;
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b) return SLZ_E_FORMAT;
    if (p[2] != 8 || (p[3] & 0xe0)) return SLZ_E_METHOD;
    if (p[3] & 4) {
        if (i + 2 > n) return SLZ_E_CORRUPT;
        i += 2 + rd16(p + i);
    }
    if (p[3] & 8) {
        *fname = (const char *)p + i;
        while (i < n && p[i])
            i++;
        i++;
    }
    if (p[3] & 16) {
        while (i < n && p[i])
            i++;
        i++;
    }
    if (p[3] & 2) i += 2;
    if (i > n) return SLZ_E_CORRUPT;
    *hlen = i;
    return 0;
}

/* ---------- archive open ---------- */
static int grow(slz_archive *a, size_t *cap) {
    if (a->n < *cap) return 0;
    *cap = *cap ? *cap * 2 : 256;
    {
        slz_entry *e = (slz_entry *)realloc(a->e, *cap * sizeof *e);
        if (!e) return SLZ_E_NOMEM;
        a->e = e;
    }
    return 0;
}

static int add_name(slz_archive *a, size_t *nn, size_t *ncap, const char *s, size_t len) {
    if (*nn + len + 1 > *ncap) {
        char *p;
        size_t c = (*ncap + len + 1) * 2;
        if (!(p = (char *)realloc(a->names, c))) return SLZ_E_NOMEM;
        a->names = p;
        *ncap = c;
    }
    memcpy(a->names + *nn, s, len);
    a->names[*nn + len] = 0;
    a->e[a->n].name = (const char *)(uintptr_t)*nn; /* offset, fixed up in slz_open */
    a->e[a->n].nlen = (uint32_t)len;
    *nn += len + 1;
    return 0;
}

typedef struct {
    slz_archive *a;
    tarp t;
    size_t cap, nn, ncap;
} tgzl;

static int tar_list_chunk(tgzl *L, const uint8_t *p, size_t n, uint64_t base, int skipdata, uint64_t *skip) {
    size_t u = 0;
    while (u < n) {
        size_t k;
        const uint8_t *dp;
        size_t dn;
        int ev = tar_step(&L->t, p + u, n - u, &k, &dp, &dn);
        u += k;
        if (ev == T_ERR) return SLZ_E_CORRUPT;
        if (ev == T_END) return 1;
        if (ev == T_ENTRY) {
            int r;
            if ((r = grow(L->a, &L->cap))) return r;
            L->a->e[L->a->n] = L->t.cur;
            L->a->e[L->a->n].off = base + u;
            if ((r = add_name(L->a, &L->nn, &L->ncap, L->t.cur.name, L->t.cur.nlen))) return r;
            L->a->n++;
            if (skipdata) {
                *skip = u + L->t.left + L->t.pad;
                L->t.left = L->t.pad = 0;
                L->t.st = 0;
                return 2;
            }
        }
    }
    return 0;
}

static int list_tar(slz_archive *a) {
    tgzl L;
    uint64_t pos = 0;
    static const size_t W = 1u << 16;
    uint8_t *buf = (uint8_t *)malloc(W);
    int r = 0;
    memset(&L, 0, sizeof L);
    L.a = a;
    if (!buf) return SLZ_E_NOMEM;
    while (pos < a->size) {
        uint64_t skip = 0;
        int64_t got = os_pread(a->fd, buf, W, pos);
        if (got <= 0) {
            r = got < 0 ? (int)got : SLZ_E_CORRUPT;
            break;
        }
        r = tar_list_chunk(&L, buf, (size_t)got, pos, 1, &skip);
        if (r < 0 || r == 1) break;
        if (r == 2) {
            if (skip > a->size - pos) {
                r = SLZ_E_CORRUPT;
                break;
            }
            pos += skip;
        } else
            pos += (uint64_t)got;
        r = 0;
    }
    if (r == 0 && (L.t.st != 0 || L.t.hn)) r = SLZ_E_CORRUPT; /* truncated mid-member */
    free(buf);
    tar_free(&L.t);
    return r < 0 ? r : 0;
}

typedef struct {
    tgzl L;
    size_t got;
    uint8_t peek[512];
    int mode;
} tgzs; /* mode 0 peek, 1 list */
int64_t rng_read(void *u, uint8_t *b, size_t n) {
    rng *r = (rng *)u;
    int64_t k = os_pread(r->fd, b, (size_t)MIN((uint64_t)n, r->end - r->pos), r->pos);
    if (k > 0) r->pos += (uint64_t)k;
    return k;
}
static int gz_wr(void *u, const uint8_t *p, size_t n) {
    tgzs *s = (tgzs *)u;
    uint64_t sk;
    int r;
    if (s->mode == 0) {
        size_t k = MIN(n, 512 - s->got);
        memcpy(s->peek + s->got, p, k);
        s->got += k;
        return s->got == 512 ? SLZ_E_CANCEL : 0;
    }
    r = tar_list_chunk(&s->L, p, n, 0, 0, &sk);
    return r < 0 ? r : r == 1 ? SLZ_E_CANCEL : 0;
}

/* One pass of the gzip member through inflate: mode 0 peeks the first 512 bytes, mode 1 feeds
 * the tar lister. */
static int gz_pass(slz_archive *a, tgzs *s, int mode) {
    rng in;
    inf *z = inf_new();
    int r;
    if (!z) return SLZ_E_NOMEM;
    in.fd = a->fd;
    in.pos = a->gz_off;
    in.end = a->size - 8;
    s->mode = mode;
    inf_reset(z, a->gz_off, 0, NULL, 0, 0);
    z->rd = rng_read;
    z->wr = gz_wr;
    z->ck = NULL;
    z->ru = &in;
    z->wu = s;
    r = inf_run(z);
    free(z);
    return r == SLZ_E_CANCEL ? 0 : r; /* cancel = stopped early on purpose */
}

static void finish_index(slz_archive *a);

/* tar.gz has no index: listing needs a full decompression pass, so it runs on first use
 * (slz_count / slz_get / slz_dir) and extraction, which streams anyway, never pays for it. */
static int tgz_index(slz_archive *a) {
    tgzs *s;
    if (!a->lazy) return a->lazy_err;
    a->lazy = 0;
    if (!(s = (tgzs *)calloc(1, sizeof *s))) return a->lazy_err = SLZ_E_NOMEM;
    s->L.a = a;
    a->lazy_err = gz_pass(a, s, 1);
    if (!a->lazy_err && s->L.t.st != 4 && (s->L.t.st != 0 || s->L.t.hn))
        a->lazy_err = SLZ_E_CORRUPT; /* truncated */
    tar_free(&s->L.t);
    free(s);
    if (a->lazy_err)
        a->n = 0;
    else
        finish_index(a);
    return a->lazy_err;
}

static int open_gz(slz_archive *a, const uint8_t *head, size_t hn) {
    size_t hl;
    const char *fn;
    int r;
    tgzs *s;
    if ((r = gz_header(head, hn, &hl, &fn))) return r;
    if (a->size < hl + 8) return SLZ_E_CORRUPT;
    a->gz_off = hl;
    if (!(s = (tgzs *)calloc(1, sizeof *s))) return SLZ_E_NOMEM;
    r = gz_pass(a, s, 0);
    if (!r && s->got == 512 && is_tar(s->peek)) {
        a->fmt = SLZ_TGZ;
        a->lazy = 1;
    }
    free(s);
    if (r) return r;
    if (a->fmt != SLZ_TGZ) { /* single member: name from FNAME (basename only) or archive name minus .gz */
        uint8_t tr[8];
        const char *b = a->path, *q;
        size_t nn = 0, ncap = 0, cap = 0, L;
        char tmp[4096];
        if (os_pread(a->fd, tr, 8, a->size - 8) != 8) return SLZ_E_IO;
        a->fmt = SLZ_GZ;
        if (fn && memchr(fn, 0, hn - (size_t)(fn - (const char *)head))) b = fn;
        for (q = b; *q; q++)
            if (*q == '/' || *q == '\\') b = q + 1;
        L = strlen(b);
        if (b != fn && L > 3 && !strcmp(b + L - 3, ".gz")) L -= 3;
        snprintf(tmp, sizeof tmp, "%.*s", (int)MIN(L, 4000), b);
        if ((r = grow(a, &cap))) return r;
        memset(a->e, 0, sizeof *a->e);
        a->e[0].type = SLZ_FILE;
        a->e[0].method = 8;
        a->e[0].off = hl;
        a->e[0].csize = a->size - hl - 8;
        a->e[0].crc = rd32(tr);
        a->e[0].usize = rd32(tr + 4);
        a->e[0].mtime = rd32(head + 4);
        a->e[0].mode = 0644;
        if ((r = add_name(a, &nn, &ncap, tmp, strlen(tmp)))) return r;
        a->n = 1;
    }
    return 0;
}

/* Locate the central directory: EOCD record in the last 64 KiB, optional ZIP64 locator and
 * record, and the offset bias of self-extracting archives (data prepended to the zip). */
static int zip_locate(slz_archive *a, uint64_t *n, uint64_t *cdsz, uint64_t *cdoff) {
    size_t tn = (size_t)MIN(a->size, (uint64_t)(65557 + 20)), k;
    uint64_t base = a->size - tn, eocd, bias = 0;
    uint8_t *tail = (uint8_t *)malloc(tn + 1), *p; /* heap: keeps stacks small (WASM, threads) */
    int r = SLZ_E_FORMAT;
    if (!tail) return SLZ_E_NOMEM;
    if (tn < 22 || os_pread(a->fd, tail, tn, base) != (int64_t)tn) goto out;
    for (k = tn - 22 + 1; k-- > 0;)
        if (rd32(tail + k) == 0x06054b50 && k + 22 + rd16(tail + k + 20) <= tn) break;
    if (k == (size_t)-1) goto out;
    eocd = base + k;
    p = tail + k;
    *n = rd16(p + 10);
    *cdsz = rd32(p + 12);
    *cdoff = rd32(p + 16);
    r = SLZ_E_CORRUPT;
    if (k >= 20 && rd32(p - 20) == 0x07064b50) { /* ZIP64 locator */
        uint8_t z[56];
        uint64_t zo = rd64(p - 12);
        if (eocd < 76 || zo > eocd - 76 || os_pread(a->fd, z, 56, zo) != 56 || rd32(z) != 0x06064b50)
            goto out;
        *n = rd64(z + 32);
        *cdsz = rd64(z + 40);
        *cdoff = rd64(z + 48);
        eocd = zo;
    } else if (*cdoff + *cdsz < eocd) {
        bias = eocd - (*cdoff + *cdsz);
    }
    if (*cdoff > eocd || *cdsz > eocd - *cdoff - bias || *n > *cdsz / 46) goto out;
    r = *cdsz >= SIZE_MAX / 2 ? SLZ_E_LIMIT : 0; /* 32-bit targets: never truncate sizes */
    *cdoff += bias;
    a->zip_bias = bias;
out:
    free(tail);
    return r;
}

static int open_zip(slz_archive *a) {
    uint8_t *cd, *p, *end;
    size_t i, o = 0;
    uint64_t n = 0, cdsz = 0, cdoff = 0, bias;
    int r = zip_locate(a, &n, &cdsz, &cdoff);
    if (r) return r;
    bias = a->zip_bias;
    a->cd_off = cdoff;
    if (!(cd = (uint8_t *)malloc((size_t)cdsz + 1)) ||
        !(a->e = (slz_entry *)calloc((size_t)n + 1, sizeof *a->e))) {
        free(cd);
        return SLZ_E_NOMEM;
    }
    a->names = (char *)cd;
    if (os_pread(a->fd, cd, (size_t)cdsz, cdoff) != (int64_t)cdsz) return SLZ_E_IO;
    for (p = cd, end = cd + cdsz, i = 0; i < n; i++) {
        slz_entry *e = &a->e[i];
        size_t nl, xl, cl;
        uint32_t vm, attr;
        const uint8_t *x, *xe;
        if (end - p < 46 || rd32(p) != 0x02014b50) return SLZ_E_CORRUPT;
        nl = rd16(p + 28);
        xl = rd16(p + 30);
        cl = rd16(p + 32);
        if ((size_t)(end - p) - 46 < nl + xl + cl) return SLZ_E_CORRUPT;
        vm = rd16(p + 4) >> 8;
        attr = rd32(p + 38);
        e->enc = rd16(p + 8) & 1;
        e->method = rd16(p + 10);
        e->crc = rd32(p + 16);
        e->csize = rd32(p + 20);
        e->usize = rd32(p + 24);
        e->off = rd32(p + 42);
        {
            unsigned t = rd16(p + 12), d = rd16(p + 14); /* DOS time as UTC (days-from-civil) */
            int y = (int)(d >> 9) + 1980, m = (int)(d >> 5 & 15), dd = (int)(d & 31), era;
            unsigned yoe, doy;
            if (m < 1 || m > 12) m = 1;
            y -= m <= 2;
            era = y / 400;
            yoe = (unsigned)(y - era * 400);
            doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1);
            e->mtime = ((int64_t)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468) * 86400 +
                       (t >> 11) * 3600 + (t >> 5 & 63) * 60 + (t & 31) * 2;
        }
        for (x = p + 46 + nl, xe = x + xl; xe - x >= 4; x += 4 + rd16(x + 2)) {
            unsigned id = rd16(x), sz = rd16(x + 2);
            const uint8_t *q = x + 4, *qe = q + sz;
            if (sz > (size_t)(xe - x) - 4) return SLZ_E_CORRUPT;
            if (id == 1) {
                if (e->usize == 0xffffffffu) {
                    if (qe - q < 8) return SLZ_E_CORRUPT;
                    e->usize = rd64(q);
                    q += 8;
                }
                if (e->csize == 0xffffffffu) {
                    if (qe - q < 8) return SLZ_E_CORRUPT;
                    e->csize = rd64(q);
                    q += 8;
                }
                if (e->off == 0xffffffffu) {
                    if (qe - q < 8) return SLZ_E_CORRUPT;
                    e->off = rd64(q);
                }
            } else if (id == 0x5455 && sz >= 5 && (q[0] & 1))
                e->mtime = (int32_t)rd32(q + 1);
        }
        e->type = (nl && (p[46 + nl - 1] == '/' || p[46 + nl - 1] == '\\')) ? SLZ_DIR : SLZ_FILE;
        if (vm == 3) { /* unix */
            unsigned ft = attr >> 16 & 0170000;
            e->mode = attr >> 16 & 07777;
            if (ft == 0120000)
                e->type = SLZ_LINK;
            else if (ft == 0040000)
                e->type = SLZ_DIR;
            else if (ft && ft != 0100000)
                e->type = SLZ_OTHER;
        } else if (attr & 0x10)
            e->type = SLZ_DIR;
        e->off += bias;
        memmove(cd + o, p + 46, nl);
        cd[o + nl] = 0; /* compact names in place: o < p always */
        e->name = (const char *)(uintptr_t)o;
        e->nlen = (uint32_t)nl;
        o += nl + 1;
        p += 46 + nl + xl + cl;
    }
    a->n = (size_t)n;
    return 0;
}

/* ---------- index snapshot ----------
 * Destructive extraction punches the archive's own headers (tar headers, zip local headers), so
 * the entry index is saved in the journal before the first punch and reloaded on resume. */
typedef struct {
    uint64_t magic, fmt, n, nlen, size, cd_off, gz_off, fp;
} snap_hdr;
typedef struct {
    uint64_t usize, csize, off, mtime, name;
    uint32_t crc, mode, nlen, kind;
} snap_ent;
#define SNAP_MAGIC 0x31706e735a4c53ull

int snap_write(const slz_archive *a, os_fd fd, uint64_t at) {
    snap_hdr h;
    snap_ent *v;
    size_t i;
    uint64_t blob = 0;
    int r;
    if (!(v = (snap_ent *)calloc(a->n + 1, sizeof *v))) return SLZ_E_NOMEM;
    for (i = 0; i < a->n; i++) {
        const slz_entry *e = &a->e[i];
        v[i].usize = e->usize;
        v[i].csize = e->csize;
        v[i].off = e->off;
        v[i].mtime = (uint64_t)e->mtime;
        v[i].name = (uint64_t)(e->name - a->names);
        v[i].crc = e->crc;
        v[i].mode = e->mode;
        v[i].nlen = e->nlen;
        v[i].kind = (uint32_t)e->method | (uint32_t)e->type << 16 | (uint32_t)e->enc << 24;
        if (v[i].name + e->nlen + 1 > blob) blob = v[i].name + e->nlen + 1;
    }
    if (a->fmt == SLZ_7Z || a->fmt == SLZ_SLZ) blob = 0; /* re-parsed on resume: index never punched */
    h.magic = SNAP_MAGIC;
    h.fmt = (uint64_t)a->fmt;
    h.n = a->fmt == SLZ_7Z || a->fmt == SLZ_SLZ ? 0 : a->n;
    h.nlen = blob;
    h.size = a->size;
    h.cd_off = a->cd_off;
    h.gz_off = a->gz_off;
    h.fp = a->fp;
    r = os_pwrite(fd, &h, sizeof h, at);
    if (!r && h.n) r = os_pwrite(fd, v, (size_t)h.n * sizeof *v, at + sizeof h);
    if (!r && blob) r = os_pwrite(fd, a->names, (size_t)blob, at + sizeof h + (size_t)h.n * sizeof *v);
    free(v);
    return r;
}

static int snap_read(slz_archive *a, os_fd fd, uint64_t at) {
    snap_hdr h;
    snap_ent *v = NULL;
    size_t i;
    int64_t fs = os_size(fd);
    int r = SLZ_E_STATE;
    if (os_pread(fd, &h, sizeof h, at) != (int64_t)sizeof h || h.magic != SNAP_MAGIC || h.size != a->size ||
        (h.fmt != SLZ_ZIP && h.fmt != SLZ_TAR && h.fmt != SLZ_GZ && h.fmt != SLZ_7Z && h.fmt != SLZ_SLZ) || fs < 0 ||
        (uint64_t)fs >= SIZE_MAX / 2 || h.n > (uint64_t)fs / sizeof *v || h.nlen > (uint64_t)fs ||
        at + sizeof h + h.n * sizeof *v + h.nlen > (uint64_t)fs)
        return SLZ_E_STATE;
    if (h.fmt == SLZ_7Z || h.fmt == SLZ_SLZ) { /* identity only: the caller parses the archive itself */
        a->fp = h.fp;
        return 1;
    }
    if (!(v = (snap_ent *)malloc((size_t)h.n * sizeof *v + 1)) ||
        !(a->e = (slz_entry *)calloc((size_t)h.n + 1, sizeof *a->e)) ||
        !(a->names = (char *)malloc((size_t)h.nlen + 1))) {
        free(v);
        return SLZ_E_NOMEM;
    }
    if (os_pread(fd, v, (size_t)h.n * sizeof *v, at + sizeof h) != (int64_t)(h.n * sizeof *v) ||
        os_pread(fd, a->names, (size_t)h.nlen, at + sizeof h + h.n * sizeof *v) != (int64_t)h.nlen)
        goto out;
    for (i = 0; i < h.n; i++) {
        slz_entry *e = &a->e[i];
        if (v[i].name >= h.nlen || v[i].nlen >= h.nlen - v[i].name || a->names[v[i].name + v[i].nlen])
            goto out;
        e->usize = v[i].usize;
        e->csize = v[i].csize;
        e->off = v[i].off;
        e->mtime = (int64_t)v[i].mtime;
        e->name = (const char *)(uintptr_t)v[i].name;
        e->nlen = v[i].nlen;
        e->crc = v[i].crc;
        e->mode = v[i].mode;
        e->method = (uint16_t)v[i].kind;
        e->type = (uint8_t)(v[i].kind >> 16);
        e->enc = (uint8_t)(v[i].kind >> 24);
    }
    a->n = (size_t)h.n;
    a->fmt = (int)h.fmt;
    a->cd_off = h.cd_off;
    a->gz_off = h.gz_off;
    a->fp = h.fp;
    r = 0;
out:
    free(v);
    return r;
}

static uint64_t fnv(uint64_t h, uint64_t v);

/* entry names were stored as offsets into a->names (the buffer may move while growing):
 * turn them into pointers, and fingerprint the index for the destructive journal */
static void finish_index(slz_archive *a) {
    size_t i;
    for (i = 0, a->fp = 0xcbf29ce484222325ull; i < a->n; i++) {
        a->e[i].name = a->names + (uintptr_t)a->e[i].name;
        a->fp = fnv(fnv(fnv(a->fp, a->e[i].off), a->e[i].csize), a->e[i].crc);
    }
    a->fp = fnv(fnv(a->fp, a->size), os_fileid(a->fd)); /* same file, not just same content/size */
}

static uint64_t fnv(uint64_t h, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++)
        h = (h ^ (v >> (i * 8) & 255)) * 0x100000001b3ull;
    return h;
}

slz_archive *slz_open(const char *path, int *err) {
    slz_archive *a = (slz_archive *)calloc(1, sizeof *a);
    uint8_t head[8192];
    int64_t hn;
    int r = SLZ_E_NOMEM;
    size_t L = strlen(path);
    uint64_t snap_fp = 0;
    int reparse = 0;
    if (!a || !(a->path = (char *)malloc(L + 16))) goto fail;
    memcpy(a->path, path, L + 1);
    a->fd = OS_BAD;
    if ((r = os_open(path, 0, 0, &a->fd))) goto fail;
    if (os_size(a->fd) < 0) {
        r = SLZ_E_IO;
        goto fail;
    }
    a->size = (uint64_t)os_size(a->fd);
    memcpy(a->path + L, ".slzstate", 10);
    r = SLZ_E_STATE;
    if (os_exists(a->path)) { /* mid destructive extraction: the index comes from the journal */
        os_fd jf;
        if (!os_open(a->path, 0, 0, &jf)) {
            r = snap_read(a, jf, SNAP_AT);
            os_close(jf);
        }
        if (r == SLZ_E_NOMEM) goto fail;
        /* no valid snapshot => crashed before the first punch: the archive is intact, parse it */
        if (r == 1) { /* 7z: in progress, but parsed from the archive */
            reparse = 1;
            snap_fp = a->fp;
            a->partial = 1;
        } else if (!r) {
            a->partial = 1;
            snap_fp = a->fp;
        } else {
            free(a->e);
            free(a->names);
            a->e = NULL;
            a->names = NULL;
            a->n = 0;
        }
    }
    if (!a->partial || reparse) {
        if ((hn = os_pread(a->fd, head, sizeof head, 0)) < 0) {
            r = (int)hn;
            goto fail;
        }
        if (hn >= 4 && !memcmp(head, "SLZ\x1a", 4)) {
            a->fmt = SLZ_SLZ;
            r = open_slz(a, head, (size_t)hn);
        } else if (hn >= 6 && !memcmp(head, "7z\xbc\xaf\x27\x1c", 6)) {
            a->fmt = SLZ_7Z;
            r = open_7z(a, head, (size_t)hn);
        } else if (hn >= 2 && head[0] == 0x1f && head[1] == 0x8b)
            r = open_gz(a, head, (size_t)hn);
        else {
            if (hn >= 512 && is_tar(head)) {
                a->fmt = SLZ_TAR;
                r = list_tar(a);
            } else {
                a->fmt = SLZ_ZIP;
                r = open_zip(a);
            }
        }
    }
    a->path[L] = 0;
    if (r) goto fail;
    finish_index(a);
    if (a->partial && a->fp != snap_fp) { /* the journal belongs to another file */
        r = SLZ_E_STATE;
        goto fail;
    }
    if (err) *err = 0;
    return a;
fail:
    if (err) *err = r;
    slz_close(a);
    return NULL;
}

void slz_close(slz_archive *a) {
    if (!a) return;
    if (a->fd != OS_BAD) os_close(a->fd);
    free(a->e);
    free(a->names);
    free(a->sorted);
    z7_free(a->z7);
    nat_free(a->nat);
    free(a->path);
    free(a);
}
int slz_format(const slz_archive *a) {
    return a->fmt;
}
size_t slz_count(slz_archive *a) {
    tgz_index(a);
    return a->n;
}
const slz_entry *slz_get(slz_archive *a, size_t i) {
    tgz_index(a);
    return i < a->n ? &a->e[i] : NULL;
}

/* ---------- tree: sorted index + binary search over byte-wise order ---------- */
static int cmp_name(const void *x, const void *y) {
    const slz_entry *p = *(const slz_entry *const *)x, *q = *(const slz_entry *const *)y;
    int c = memcmp(p->name, q->name, MIN(p->nlen, q->nlen));
    return c ? c : (p->nlen > q->nlen) - (p->nlen < q->nlen);
}
/* first sorted position whose name >= s[0..n-1] + last */
static size_t lb(const slz_archive *a, const char *s, size_t n, char last) {
    size_t lo = 0, hi = a->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const slz_entry *e = a->sorted[mid];
        int c = memcmp(e->name, s, MIN(e->nlen, n));
        if (!c) c = e->nlen > n ? (unsigned char)e->name[n] - (unsigned char)last : -1;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int slz_dir(slz_archive *a, const char *dir, slz_dir_cb cb, void *user) {
    char pre[4096 + 2];
    size_t pn = strlen(dir), i;
    int r;
    if (pn > 4096) return SLZ_E_LIMIT;
    if ((r = tgz_index(a))) return r;
    if (!a->sorted) { /* one-time O(n log n); not thread-safe on the same archive */
        const slz_entry **v = (const slz_entry **)malloc(a->n * sizeof *v + 1);
        if (!v) return SLZ_E_NOMEM;
        for (i = 0; i < a->n; i++)
            v[i] = &a->e[i];
        qsort(v, a->n, sizeof *v, cmp_name);
        a->sorted = v;
    }
    memcpy(pre, dir, pn);
    if (pn) pre[pn++] = '/';
    for (i = pn ? lb(a, pre, pn - 1, '/') : 0; i < a->n;) {
        const slz_entry *e = a->sorted[i];
        const char *rest, *sl;
        size_t rn;
        if (e->nlen < pn || memcmp(e->name, pre, pn)) break;
        rest = e->name + pn;
        rn = e->nlen - pn;
        sl = (const char *)memchr(rest, '/', rn);
        if (!sl) {
            if (rn && cb(user, rest, rn, e->type == SLZ_DIR, (long long)(e - a->e))) return SLZ_E_CANCEL;
            i++;
            continue;
        }
        if (sl == rest) {
            i++;
            continue;
        }
        {
            size_t cn = (size_t)(sl - rest), n2 = pn + cn;
            if (n2 > 4096) return SLZ_E_LIMIT;
            if (cb(user, rest, cn, 1, (sl + 1 == e->name + e->nlen) ? (long long)(e - a->e) : -1))
                return SLZ_E_CANCEL;
            memcpy(pre + pn, rest, cn);
            i = lb(a, pre, n2, '/' + 1); /* skip the whole subtree */
        }
    }
    return 0;
}

unsigned slz_version_number(void) {
    return SLZ_VERSION_NUMBER;
}
const char *slz_version_string(void) {
    return SLZ_VERSION_STRING;
}

const char *slz_strerror(int e) {
    static const char *m[] = {"ok",
                              "I/O error",
                              "out of memory",
                              "unknown format",
                              "corrupt archive",
                              "unsafe path",
                              "unsupported method",
                              "encrypted entry",
                              "file exists",
                              "not supported by filesystem/format",
                              "limit exceeded",
                              "cancelled",
                              "archive is mid destructive extraction (resume with SLZ_DESTRUCTIVE)",
                              "cannot open file"};
    return (e <= 0 && e >= -13) ? m[-e] : "unknown error";
}
