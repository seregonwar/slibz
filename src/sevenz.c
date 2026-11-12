/* slz - 7z archive reader: headers (plain or LZMA-encoded), folders, substreams, files.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * Every header field goes through a bounded reader (br): reads past the end set an error flag
 * and return 0, so a truncated or hostile header can only fail, never read out of bounds.
 * Counts are capped by the header length before anything is allocated. Supported decoding:
 * Copy, LZMA, LZMA2, Deflate, optionally behind BCJ x86; other methods are listed but refused
 * at extraction (SLZ_E_METHOD), AES-encrypted folders are refused (SLZ_E_ENCRYPTED). */
#include "internal.h"

enum { /* property ids (7zFormat.txt) */
       K_END,
       K_HEADER,
       K_ARCHIVE_PROPS,
       K_ADD_STREAMS,
       K_MAIN_STREAMS,
       K_FILES,
       K_PACK_INFO,
       K_UNPACK_INFO,
       K_SUBSTREAMS,
       K_SIZE,
       K_CRC,
       K_FOLDER,
       K_UNPACK_SIZE,
       K_NUM_UNPACK,
       K_EMPTY_STREAM,
       K_EMPTY_FILE,
       K_ANTI,
       K_NAME,
       K_CTIME,
       K_ATIME,
       K_MTIME,
       K_ATTRIBUTES,
       K_COMMENT,
       K_ENCODED_HEADER,
       K_START_POS,
       K_DUMMY
};

#define MAX_HEADER (256u << 20) /* decoded header size limit */

typedef struct {
    const uint8_t *p, *e;
    int err;
} br;

static uint8_t b8(br *b) {
    if (b->p >= b->e) {
        b->err = 1;
        return 0;
    }
    return *b->p++;
}
static uint32_t b32(br *b) {
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++)
        v |= (uint32_t)b8(b) << (8 * i);
    return v;
}
static uint64_t b64(br *b) {
    uint64_t v = b32(b);
    return v | (uint64_t)b32(b) << 32;
}
/* 7z NUMBER: the count of leading 1 bits of the first byte = number of extra bytes */
static uint64_t num(br *b) {
    unsigned first = b8(b), mask = 0x80;
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++, mask >>= 1) {
        if (!(first & mask)) return v | (uint64_t)(first & (mask - 1)) << (8 * i);
        v |= (uint64_t)b8(b) << (8 * i);
    }
    return v;
}
static size_t left(const br *b) {
    return (size_t)(b->e - b->p);
}
static void skip(br *b, uint64_t n) {
    if (n > left(b))
        b->err = 1;
    else
        b->p += n;
}
/* a count that must fit in the remaining header (each item needs >= 1 byte or bit) */
static size_t count(br *b, size_t per8) {
    uint64_t n = num(b);
    if (n > (uint64_t)left(b) * per8 + 8 || n > (1u << 28)) b->err = 1;
    return b->err ? 0 : (size_t)n;
}
/* bit vector, MSB first; NULL if allocation fails (the caller checks b->err too) */
static uint8_t *bits(br *b, size_t n) {
    uint8_t *v = (uint8_t *)calloc(n + 1, 1), cur = 0;
    size_t i;
    if (!v) return NULL;
    for (i = 0; i < n; i++) {
        if (!(i & 7)) cur = b8(b);
        v[i] = cur >> (7 - (i & 7)) & 1;
    }
    return v;
}
/* "AllAreDefined" byte, else a bit vector */
static uint8_t *defined(br *b, size_t n) {
    uint8_t *v;
    if (!b8(b)) return bits(b, n);
    if ((v = (uint8_t *)malloc(n + 1))) memset(v, 1, n + 1);
    return v;
}

/* ---------------- streams info ---------------- */

typedef struct {
    uint64_t pack_pos;
    size_t npack;
    uint64_t *pack; /* packed stream sizes */
    size_t nf;
    folder7 *f;
    uint64_t *sub_size; /* substream sizes, folder by folder */
    uint32_t *sub_crc;
    uint8_t *sub_hascrc;
    size_t nsub;
} streams;

static void streams_free(streams *s) {
    free(s->pack);
    free(s->f);
    free(s->sub_size);
    free(s->sub_crc);
    free(s->sub_hascrc);
    memset(s, 0, sizeof *s);
}

static void read_folder(br *b, folder7 *f) {
    size_t nc = (size_t)num(b), i, tin = 0, tout = 0, nbind, npacked;
    uint32_t bind_in = 0, bind_out = 0, packed = 0, bound = 0;
    if (nc < 1 || nc > 4) {
        b->err = 1;
        return;
    }
    f->nc = (unsigned)nc;
    for (i = 0; i < nc; i++) {
        unsigned flags = b8(b), idn = flags & 15, k;
        uint64_t nin = 1, nout = 1;
        coder7 *c = &f->c[i];
        if (flags & 0x80) b->err = 1; /* "alternative methods": never written by 7-Zip */
        for (k = 0; k < idn; k++) {
            uint8_t v = b8(b);
            if (k < sizeof c->id) c->id[k] = v;
        }
        c->idn = idn;
        if (idn > sizeof c->id) f->unsupported = 1;
        if (idn == 4 && !memcmp(c->id, "\x06\xf1\x07\x01", 4)) f->enc = 1; /* 7zAES */
        if (flags & 0x10) {
            nin = num(b);
            nout = num(b);
            f->unsupported = 1; /* complex coders (BCJ2) */
        }
        if (flags & 0x20) {
            uint64_t np = num(b);
            c->props = b->p;
            c->np = np > 255 ? 255 : (unsigned)np;
            skip(b, np);
        }
        if (nin > 8 || nout > 8) b->err = 1;
        tin += (size_t)nin;
        tout += (size_t)nout;
    }
    if (tout < 1 || tout > 8 || tin > 8 || b->err) {
        b->err = 1;
        return;
    }
    nbind = tout - 1;
    for (i = 0; i < nbind; i++) {
        bind_in = (uint32_t)num(b);
        bind_out = (uint32_t)num(b);
        if (bind_out < 32) bound |= 1u << bind_out;
    }
    if (tin < nbind) {
        b->err = 1;
        return;
    }
    npacked = tin - nbind;
    if (npacked == 1) {
        for (packed = 0; packed < tin; packed++)
            if (!nbind || packed != bind_in) break;
    } else {
        for (i = 0; i < npacked; i++)
            num(b);
        f->unsupported = 1;
    }
    f->npacked = (unsigned)npacked;
    f->nout = (unsigned)tout;
    /* the folder's output is the one out stream no bind pair consumes (any coder graph) */
    for (f->main_out = 0; f->main_out < tout && (bound >> f->main_out & 1); f->main_out++)
        ;
    if (f->main_out >= tout) {
        b->err = 1;
        return;
    }
    /* supported shapes: [decoder]  or  [BCJ x86 <- decoder] (coder 0 consumes coder 1's output) */
    if (f->unsupported) return;
    if (nc == 1) {
        f->dec = 0;
        f->main_out = 0;
    } else if (nc == 2) {
        /* coder i owns in/out stream i: the BCJ coder's input must be the decoder's output,
         * the packed stream feeds the decoder, the BCJ output is the folder output */
        unsigned flt = f->c[0].idn == 4 && !memcmp(f->c[0].id, "\x03\x03\x01\x03", 4) ? 0 : 1, dec = 1 - flt;
        if (f->c[flt].idn == 4 && !memcmp(f->c[flt].id, "\x03\x03\x01\x03", 4) && !f->c[flt].np &&
            bind_in == flt && bind_out == dec && packed == dec) {
            f->dec = dec;
            f->filter = 1;
            f->main_out = flt;
        } else {
            f->unsupported = 1;
        }
    } else {
        f->unsupported = 1;
    }
}

static int method_of(const coder7 *c) {
    if (c->idn == 1 && c->id[0] == 0x00) return SLZ_M_STORE;
    if (c->idn == 1 && c->id[0] == 0x21) return SLZ_M_LZMA2;
    if (c->idn == 3 && !memcmp(c->id, "\x03\x01\x01", 3)) return SLZ_M_LZMA;
    if (c->idn == 3 && !memcmp(c->id, "\x04\x01\x08", 3)) return SLZ_M_DEFLATE;
    return -1;
}

static int read_streams(br *b, streams *s) {
    unsigned id = (unsigned)num(b);
    size_t i, j;
    if (id == K_PACK_INFO) {
        s->pack_pos = num(b);
        s->npack = count(b, 1);
        if (!(s->pack = (uint64_t *)calloc(s->npack + 1, sizeof *s->pack))) return SLZ_E_NOMEM;
        while (!b->err && (id = (unsigned)num(b)) != K_END) {
            if (id == K_SIZE)
                for (i = 0; i < s->npack; i++)
                    s->pack[i] = num(b);
            else if (id == K_CRC) {
                uint8_t *d = defined(b, s->npack);
                if (!d) return SLZ_E_NOMEM;
                for (i = 0; i < s->npack; i++)
                    if (d[i]) b32(b);
                free(d);
            } else
                b->err = 1;
        }
        id = (unsigned)num(b);
    }
    if (id == K_UNPACK_INFO) {
        size_t pk = 0;
        if (num(b) != K_FOLDER) return SLZ_E_CORRUPT;
        s->nf = count(b, 1);
        if (b8(b) != 0) return SLZ_E_NOTSUP; /* "external" data */
        if (!(s->f = (folder7 *)calloc(s->nf + 1, sizeof *s->f))) return SLZ_E_NOMEM;
        for (i = 0; i < s->nf && !b->err; i++) {
            read_folder(b, &s->f[i]);
            s->f[i].pack_index = pk;
            pk += s->f[i].npacked;
            s->f[i].nsub = 1;
        }
        if (pk > s->npack || num(b) != K_UNPACK_SIZE) return SLZ_E_CORRUPT;
        for (i = 0; i < s->nf; i++)
            for (j = 0; j < s->f[i].nout; j++) {
                uint64_t v = num(b);
                if (j == s->f[i].main_out) s->f[i].usize = v;
            }
        while (!b->err && (id = (unsigned)num(b)) != K_END) {
            if (id == K_CRC) {
                uint8_t *d = defined(b, s->nf);
                if (!d) return SLZ_E_NOMEM;
                for (i = 0; i < s->nf; i++)
                    if ((s->f[i].has_crc = d[i])) s->f[i].crc = b32(b);
                free(d);
            } else
                b->err = 1;
        }
        id = (unsigned)num(b);
    }
    if (id == K_SUBSTREAMS) {
        while (!b->err && (id = (unsigned)num(b)) == K_NUM_UNPACK)
            for (i = 0; i < s->nf; i++)
                s->f[i].nsub = count(b, 8);
        for (i = 0, s->nsub = 0; i < s->nf; i++) {
            s->f[i].first = s->nsub;
            s->nsub += s->f[i].nsub;
            if (s->nsub > (1u << 28)) return SLZ_E_CORRUPT;
        }
        s->sub_size = (uint64_t *)calloc(s->nsub + 1, sizeof *s->sub_size);
        s->sub_crc = (uint32_t *)calloc(s->nsub + 1, sizeof *s->sub_crc);
        s->sub_hascrc = (uint8_t *)calloc(s->nsub + 1, 1);
        if (!s->sub_size || !s->sub_crc || !s->sub_hascrc) return SLZ_E_NOMEM;
        for (i = 0; i < s->nf; i++) { /* sizes: all but the last substream, which takes the rest */
            uint64_t sum = 0;
            for (j = 0; j < s->f[i].nsub; j++) {
                uint64_t v;
                if (j + 1 < s->f[i].nsub && id != K_SIZE) return SLZ_E_CORRUPT; /* sizes are required */
                v = j + 1 < s->f[i].nsub ? num(b) : s->f[i].usize - sum;
                if (v > s->f[i].usize - sum) return SLZ_E_CORRUPT;
                s->sub_size[s->f[i].first + j] = v;
                sum += v;
            }
        }
        if (id == K_SIZE) id = (unsigned)num(b);
        for (i = 0; i < s->nf; i++) /* a single substream inherits the folder CRC */
            if (s->f[i].nsub == 1 && s->f[i].has_crc) {
                s->sub_hascrc[s->f[i].first] = 1;
                s->sub_crc[s->f[i].first] = s->f[i].crc;
            }
        while (!b->err && id != K_END) {
            if (id == K_CRC) { /* digests for the substreams whose CRC is still unknown */
                size_t n = 0, k = 0;
                uint8_t *d;
                for (i = 0; i < s->nsub; i++)
                    n += !s->sub_hascrc[i];
                if (!(d = defined(b, n))) return SLZ_E_NOMEM;
                for (i = 0; i < s->nsub; i++)
                    if (!s->sub_hascrc[i] && d[k++]) {
                        s->sub_hascrc[i] = 1;
                        s->sub_crc[i] = b32(b);
                    }
                free(d);
            } else
                b->err = 1;
            id = (unsigned)num(b);
        }
        id = (unsigned)num(b);
    } else if (s->nf) { /* no substreams info: one substream per folder */
        s->nsub = s->nf;
        s->sub_size = (uint64_t *)calloc(s->nsub + 1, sizeof *s->sub_size);
        s->sub_crc = (uint32_t *)calloc(s->nsub + 1, sizeof *s->sub_crc);
        s->sub_hascrc = (uint8_t *)calloc(s->nsub + 1, 1);
        if (!s->sub_size || !s->sub_crc || !s->sub_hascrc) return SLZ_E_NOMEM;
        for (i = 0; i < s->nf; i++) {
            s->f[i].first = i;
            s->sub_size[i] = s->f[i].usize;
            s->sub_hascrc[i] = (uint8_t)s->f[i].has_crc;
            s->sub_crc[i] = s->f[i].crc;
        }
    }
    if (id != K_END || b->err) return SLZ_E_CORRUPT;
    return 0;
}

/* absolute pack offsets and sizes: packed streams are stored back to back after the
 * 32-byte signature header; everything must lie before the next header */
static int place_folders(streams *s, uint64_t limit) {
    uint64_t off = 32 + s->pack_pos;
    size_t i, k = 0;
    if (s->pack_pos > limit) return SLZ_E_CORRUPT;
    for (i = 0; i < s->nf; i++) {
        folder7 *f = &s->f[i];
        for (; k < f->pack_index; k++)
            if ((off += s->pack[k]) > limit || s->pack[k] > limit) return SLZ_E_CORRUPT;
        if (k >= s->npack && f->npacked) return SLZ_E_CORRUPT;
        f->pack_off = off;
        f->pack_size = f->npacked ? s->pack[k] : 0;
        if (off > limit || f->pack_size > limit - off) return SLZ_E_CORRUPT;
    }
    for (; k < s->npack; k++) /* remaining sizes must also fit (overflow check) */
        if ((off += s->pack[k]) > limit) return SLZ_E_CORRUPT;
    return 0;
}

/* ---------------- folder decoding (shared by header decoding and extraction) ---------------- */

typedef struct {
    wr_fn wr;
    void *wu;
    uint64_t n, max;
} counted;

static int count_wr(void *u, const uint8_t *p, size_t n) {
    counted *c = (counted *)u;
    if (n > c->max - c->n) return SLZ_E_CORRUPT; /* more output than the folder declares */
    c->n += n;
    return c->wr(c->wu, p, n);
}

/* 0 if this folder can be decoded, else why not (checked before any output file is created) */
int z7_supported(const folder7 *f) {
    if (f->enc) return SLZ_E_ENCRYPTED;
    return f->unsupported || method_of(&f->c[f->dec]) < 0 ? SLZ_E_METHOD : 0;
}

int z7_decode(os_fd fd, const folder7 *f, wr_fn wr, void *wu) {
    rng in;
    counted cnt;
    bcj *filt = NULL;
    wr_fn w = count_wr;
    void *u = &cnt;
    int m = method_of(&f->c[f->dec]), r = z7_supported(f);
    if (r) return r;
    in.fd = fd;
    in.pos = f->pack_off;
    in.end = f->pack_off + f->pack_size;
    cnt.wr = wr;
    cnt.wu = wu;
    cnt.n = 0;
    cnt.max = f->usize;
    if (f->filter) { /* decoder -> BCJ -> counter -> sink */
        if (!(filt = (bcj *)calloc(1, sizeof *filt))) return SLZ_E_NOMEM;
        filt->wr = count_wr;
        filt->wu = &cnt;
        w = bcj_wr;
        u = filt;
    }
    if (m == SLZ_M_STORE) {
        uint8_t *buf = (uint8_t *)malloc(1u << 20);
        int64_t k;
        if (!buf) r = SLZ_E_NOMEM;
        while (!r && (k = rng_read(&in, buf, 1u << 20)) != 0)
            r = k < 0 ? (int)k : w(u, buf, (size_t)k);
        free(buf);
    } else if (m == SLZ_M_DEFLATE) {
        inf *z = inf_new();
        if (!z)
            r = SLZ_E_NOMEM;
        else {
            inf_reset(z, in.pos, 0, NULL, 0, 0);
            z->rd = rng_read;
            z->wr = w;
            z->ck = NULL;
            z->ru = &in;
            z->wu = u;
            r = inf_run(z);
            free(z);
        }
    } else {
        const coder7 *c = &f->c[f->dec];
        uint64_t dict;
        lzd *z;
        if (m == SLZ_M_LZMA) {
            if (c->np < 5) return free(filt), SLZ_E_CORRUPT;
            dict = rd32(c->props + 1);
        } else {
            if (c->np < 1 || c->props[0] > 40) return free(filt), SLZ_E_CORRUPT;
            dict =
                c->props[0] == 40 ? 0xffffffffu : (uint64_t)(2 | (c->props[0] & 1)) << (c->props[0] / 2 + 11);
        }
        if (!(z = lzd_new(dict, f->usize)))
            r = SLZ_E_NOMEM;
        else {
            lzd_io(z, rng_read, &in, w, u);
            r = m == SLZ_M_LZMA ? lzd_lzma(z, c->props, f->usize, 1) : lzd_lzma2(z, f->usize);
            lzd_free(z);
        }
    }
    if (!r && filt) r = bcj_finish(filt);
    free(filt);
    if (!r && cnt.n != f->usize) r = SLZ_E_CORRUPT;
    return r;
}

/* ---------------- header ---------------- */

typedef struct {
    uint8_t *p;
    size_t n, cap;
} membuf;
static int mem_wr(void *u, const uint8_t *p, size_t n) {
    membuf *m = (membuf *)u;
    if (n > m->cap - m->n) return SLZ_E_CORRUPT;
    memcpy(m->p + m->n, p, n);
    m->n += n;
    return 0;
}

/* UTF-16LE -> UTF-8, strict (unpaired surrogates are corruption) */
static int utf16(br *b, char *out, size_t cap, size_t *len) {
    size_t o = 0;
    for (;;) {
        uint32_t c = b8(b);
        c |= (uint32_t)b8(b) << 8;
        if (b->err) return SLZ_E_CORRUPT;
        if (!c) break;
        if (c >= 0xd800 && c < 0xdc00) {
            uint32_t lo = b8(b);
            lo |= (uint32_t)b8(b) << 8;
            if (lo < 0xdc00 || lo >= 0xe000) return SLZ_E_CORRUPT;
            c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
        } else if (c >= 0xdc00 && c < 0xe000)
            return SLZ_E_CORRUPT;
        if (o + 4 >= cap) return SLZ_E_LIMIT;
        if (c < 0x80)
            out[o++] = (char)c;
        else if (c < 0x800) {
            out[o++] = (char)(0xc0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xe0 | c >> 12);
            out[o++] = (char)(0x80 | (c >> 6 & 63));
            out[o++] = (char)(0x80 | (c & 63));
        } else {
            out[o++] = (char)(0xf0 | c >> 18);
            out[o++] = (char)(0x80 | (c >> 12 & 63));
            out[o++] = (char)(0x80 | (c >> 6 & 63));
            out[o++] = (char)(0x80 | (c & 63));
        }
    }
    out[o] = 0;
    *len = o;
    return 0;
}

static int read_files(slz_archive *a, br *b, const streams *s) {
    size_t nfiles = count(b, 1), nempty = 0, i, k, sub = 0, f = 0, nn = 0;
    uint8_t *empty = NULL, *emptyfile = NULL, *anti = NULL, *mdef = NULL, *adef = NULL;
    const uint8_t *names = NULL, *mtimes = NULL, *mtimes_end = NULL, *attrs = NULL, *attrs_end = NULL;
    size_t names_len = 0;
    z7 *z = a->z7;
    int r = SLZ_E_NOMEM;
    if (b->err) return SLZ_E_CORRUPT;
    for (;;) { /* properties: type, size, payload (unknown ones are skipped) */
        unsigned id = (unsigned)num(b);
        uint64_t sz;
        br sb;
        if (b->err) return SLZ_E_CORRUPT;
        if (id == K_END) break;
        sz = num(b);
        if (sz > left(b)) return SLZ_E_CORRUPT;
        sb.p = b->p;
        sb.e = b->p + sz;
        sb.err = 0;
        skip(b, sz);
        if (id == K_EMPTY_STREAM && !empty) {
            if (!(empty = bits(&sb, nfiles))) goto out;
            for (i = 0, nempty = 0; i < nfiles; i++)
                nempty += empty[i];
        } else if (id == K_EMPTY_FILE && !emptyfile) {
            if (!(emptyfile = bits(&sb, nempty))) goto out;
        } else if (id == K_ANTI && !anti) {
            if (!(anti = bits(&sb, nempty))) goto out;
        } else if (id == K_NAME) {
            if (b8(&sb) != 0) {
                r = SLZ_E_NOTSUP;
                goto out;
            }
            names = sb.p;
            names_len = left(&sb);
        } else if (id == K_MTIME && !mdef) {
            if (!(mdef = defined(&sb, nfiles))) goto out;
            if (b8(&sb) != 0) {
                r = SLZ_E_NOTSUP;
                goto out;
            }
            mtimes = sb.p;
            for (i = 0; i < nfiles; i++)
                if (mdef[i]) skip(&sb, 8);
            mtimes_end = sb.p;
        } else if (id == K_ATTRIBUTES && !adef) {
            if (!(adef = defined(&sb, nfiles))) goto out;
            if (b8(&sb) != 0) {
                r = SLZ_E_NOTSUP;
                goto out;
            }
            attrs = sb.p;
            for (i = 0; i < nfiles; i++)
                if (adef[i]) skip(&sb, 4);
            attrs_end = sb.p;
        }
        if (sb.err) {
            r = SLZ_E_CORRUPT;
            goto out;
        }
    }
    if (!(a->e = (slz_entry *)calloc(nfiles + 1, sizeof *a->e)) ||
        !(a->names = (char *)malloc(names_len * 2 + 1)) ||
        !(z->entry_folder = (uint32_t *)malloc((nfiles + 1) * sizeof *z->entry_folder)) ||
        !(z->entry_hascrc = (uint8_t *)calloc(nfiles + 1, 1)) ||
        !(z->sub_entry = (uint32_t *)calloc(s->nsub + 1, sizeof *z->sub_entry)))
        goto out;
    {
        br nb, mb, ab;
        size_t ei = 0;
        nb.p = names;
        nb.e = names ? names + names_len : names;
        nb.err = 0;
        mb.p = mtimes;
        mb.e = mtimes_end;
        mb.err = 0;
        ab.p = attrs;
        ab.e = attrs_end;
        ab.err = 0;
        for (i = 0; i < nfiles; i++) {
            slz_entry *e = &a->e[i];
            int has_stream = !empty || !empty[i];
            size_t len = 0;
            uint32_t attr = 0;
            if (names) {
                if ((r = utf16(&nb, a->names + nn, names_len * 2 + 1 - nn, &len))) goto out;
            } else
                a->names[nn] = 0;
            e->name = (const char *)(uintptr_t)nn; /* offset; finish_index makes it a pointer */
            e->nlen = (uint32_t)len;
            nn += len + 1;
            if (mdef && mdef[i]) {
                uint64_t ft = b64(&mb);
                e->mtime = (int64_t)(ft / 10000000u) - 11644473600LL;
            }
            if (adef && adef[i]) attr = b32(&ab);
            e->type = SLZ_FILE;
            if (!has_stream) {
                int is_file = emptyfile && emptyfile[ei];
                if (anti && anti[ei])
                    e->type = SLZ_OTHER;
                else if (!is_file)
                    e->type = SLZ_DIR;
                ei++;
            }
            if (attr & 0x10) e->type = SLZ_DIR;
            if (attr & 0x8000) { /* unix extension: st_mode in the high 16 bits */
                unsigned ft = attr >> 16 & 0170000;
                e->mode = attr >> 16 & 07777;
                if (ft == 0120000)
                    e->type = SLZ_LINK;
                else if (ft == 0040000)
                    e->type = SLZ_DIR;
                else if (ft && ft != 0100000)
                    e->type = SLZ_OTHER;
            }
            z->entry_folder[i] = UINT32_MAX;
            if (has_stream) { /* next substream, skipping folders without any */
                const folder7 *fo;
                while (f < s->nf && sub >= s->f[f].first + s->f[f].nsub)
                    f++;
                if (f >= s->nf || sub >= s->nsub) {
                    r = SLZ_E_CORRUPT;
                    goto out;
                }
                fo = &s->f[f];
                z->entry_folder[i] = (uint32_t)f;
                z->sub_entry[sub] = (uint32_t)i;
                e->usize = s->sub_size[sub];
                e->crc = s->sub_crc[sub];
                z->entry_hascrc[i] = s->sub_hascrc[sub];
                e->csize = fo->nsub == 1 ? fo->pack_size : 0;
                e->off = fo->pack_off;
                e->enc = (uint8_t)fo->enc;
                k = (size_t)method_of(&fo->c[fo->dec]);
                e->method = (uint16_t)(fo->unsupported ? 0xffff : k);
                sub++;
            }
        }
        if (nb.err || mb.err || ab.err || sub != s->nsub) {
            r = SLZ_E_CORRUPT;
            goto out;
        }
    }
    a->n = nfiles;
    r = 0;
out:
    free(empty);
    free(emptyfile);
    free(anti);
    free(mdef);
    free(adef);
    return r;
}

int open_7z(slz_archive *a, const uint8_t *head, size_t hn) {
    uint64_t next_off, next_size, hdr_at;
    uint8_t *hdr = NULL;
    int r, depth;
    streams s;
    br b;
    memset(&s, 0, sizeof s);
    if (hn < 32 || head[6] != 0) return SLZ_E_NOTSUP; /* major version 0 */
    if (slz_crc32(0, head + 12, 20) != rd32(head + 8)) return SLZ_E_CORRUPT;
    next_off = rd64(head + 12);
    next_size = rd64(head + 20);
    if (next_off > a->size - 32 || next_size > a->size - 32 - next_off || next_size > MAX_HEADER)
        return SLZ_E_CORRUPT;
    hdr_at = 32 + next_off;
    if (!(a->z7 = (z7 *)calloc(1, sizeof *a->z7)) || !(hdr = (uint8_t *)malloc((size_t)next_size + 1))) {
        free(hdr);
        return SLZ_E_NOMEM;
    }
    if (os_pread(a->fd, hdr, (size_t)next_size, hdr_at) != (int64_t)next_size) {
        free(hdr);
        return SLZ_E_IO;
    }
    if (slz_crc32(0, hdr, (size_t)next_size) != rd32(head + 28)) {
        free(hdr);
        return SLZ_E_CORRUPT;
    }
    b.p = hdr;
    b.e = hdr + next_size;
    b.err = 0;
    if (next_size == 0) { /* empty archive */
        a->z7->hdr = hdr;
        return 0;
    }
    for (depth = 0;; depth++) { /* the header may itself be packed (7-Zip's default) */
        membuf m;
        br t = b;
        if (num(&t) != K_ENCODED_HEADER) break;
        if (depth == 4) {
            r = SLZ_E_CORRUPT;
            goto fail;
        }
        b = t;
        if ((r = read_streams(&b, &s)) || (r = place_folders(&s, hdr_at)) || !s.nf) goto fail;
        if (s.f[0].usize > MAX_HEADER) {
            r = SLZ_E_LIMIT;
            goto fail;
        }
        m.cap = (size_t)s.f[0].usize;
        m.n = 0;
        if (!(m.p = (uint8_t *)malloc(m.cap + 1))) {
            r = SLZ_E_NOMEM;
            goto fail;
        }
        if ((r = z7_decode(a->fd, &s.f[0], mem_wr, &m)) ||
            (s.f[0].has_crc && slz_crc32(0, m.p, m.n) != s.f[0].crc)) {
            free(m.p);
            r = r ? r : SLZ_E_CORRUPT;
            goto fail;
        }
        streams_free(&s);
        free(hdr);
        hdr = m.p;
        b.p = hdr;
        b.e = hdr + m.n;
        b.err = 0;
    }
    if (num(&b) != K_HEADER || b.err) {
        r = SLZ_E_CORRUPT;
        goto fail;
    }
    {
        unsigned id = (unsigned)num(&b);
        if (id == K_ARCHIVE_PROPS) {
            while (!b.err && num(&b) != K_END)
                skip(&b, num(&b));
            id = (unsigned)num(&b);
        }
        if (id == K_ADD_STREAMS) {
            r = SLZ_E_NOTSUP;
            goto fail;
        }
        if (id == K_MAIN_STREAMS) {
            if ((r = read_streams(&b, &s)) || (r = place_folders(&s, hdr_at))) goto fail;
            id = (unsigned)num(&b);
        }
        if (id == K_FILES) {
            if ((r = read_files(a, &b, &s))) goto fail;
            id = (unsigned)num(&b);
        } else if (s.nsub) {
            r = SLZ_E_CORRUPT; /* streams without files */
            goto fail;
        }
        if (id != K_END || b.err) {
            r = SLZ_E_CORRUPT;
            goto fail;
        }
    }
    a->z7->f = s.f; /* keep the folders; coder properties point into hdr */
    a->z7->nf = s.nf;
    s.f = NULL;
    a->z7->hdr = hdr;
    hdr = NULL;
    a->cd_off = hdr_at;
    r = 0;
fail:
    streams_free(&s);
    free(hdr);
    return r;
}

void z7_free(z7 *z) {
    if (!z) return;
    free(z->f);
    free(z->hdr);
    free(z->entry_folder);
    free(z->entry_hascrc);
    free(z->sub_entry);
    free(z);
}
