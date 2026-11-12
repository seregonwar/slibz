/* slz - LZMA / LZMA2 decoder and the BCJ x86 filter (7z, zip method 14).
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * Streaming: compressed bytes come from an rd_fn, output goes to a wr_fn through a circular
 * dictionary that is never larger than the stream's total output. Every distance is checked
 * against the bytes produced since the last dictionary reset, and input exhaustion is an error,
 * so corrupt input can neither read outside the dictionary nor loop forever. */
#include "internal.h"

#define LZ_IBUF   (1u << 16) /* input buffer */
#define LZ_FLUSH  (1u << 20) /* hand output to wr at least every 1 MiB */
#define PROB_INIT 1024

typedef struct {
    uint16_t choice, choice2, low[16][8], mid[16][8], high[256];
} lenc;

struct lzd {
    /* input */
    rd_fn rd;
    void *ru;
    uint8_t ib[LZ_IBUF];
    size_t ip, ie;
    uint64_t consumed; /* bytes taken from the input so far */
    /* range decoder */
    uint32_t range, code;
    /* model */
    unsigned lc, lp, pb, state;
    uint32_t rep[4]; /* 0-based distances */
    struct {         /* adaptive bit probabilities (all uint16_t, reset together) */
        uint16_t is_match[12][16], is_rep[12], is_rep0[12], is_rep1[12], is_rep2[12], is_rep0_long[12][16];
        uint16_t slot[4][64], special[114], align[16];
        lenc mlen, rlen;
    } m;
    uint16_t lit[0x300 << 8]; /* literal coder, 0x300 << (lc + lp) used; lc + lp <= 8 */
    /* dictionary = output window */
    uint8_t *dict;
    size_t dsize, dpos, dflushed;
    uint64_t pos; /* bytes since the last dictionary reset */
    wr_fn wr;
    void *wu;
    int err;
};

lzd *lzd_new(uint64_t dict_size, uint64_t total_out) {
    lzd *s;
    uint64_t n = MIN(dict_size, total_out); /* a window larger than the whole output is never used */
    if (n < 4096) n = 4096;
    if (n > SIZE_MAX / 2) return NULL;
    if (!(s = (lzd *)calloc(1, sizeof *s))) return NULL;
    if (!(s->dict = (uint8_t *)malloc((size_t)n))) {
        free(s);
        return NULL;
    }
    s->dsize = (size_t)n;
    return s;
}

void lzd_free(lzd *s) {
    if (s) free(s->dict);
    free(s);
}

/* ---------------- input and range decoder ---------------- */

static uint8_t in_byte(lzd *s) {
    if (s->ip == s->ie) {
        int64_t k = s->err ? 0 : s->rd(s->ru, s->ib, LZ_IBUF);
        if (k <= 0) { /* running out of input inside the stream is corruption */
            if (!s->err) s->err = k < 0 ? (int)k : SLZ_E_CORRUPT;
            return 0;
        }
        s->ip = 0;
        s->ie = (size_t)k;
    }
    s->consumed++;
    return s->ib[s->ip++];
}

static void rc_init(lzd *s) {
    int i;
    s->range = 0xffffffffu;
    s->code = 0;
    if (in_byte(s) != 0 && !s->err) s->err = SLZ_E_CORRUPT;
    for (i = 0; i < 4; i++)
        s->code = s->code << 8 | in_byte(s);
}

static inline void rc_norm(lzd *s) {
    if (s->range < (1u << 24)) {
        s->range <<= 8;
        s->code = s->code << 8 | in_byte(s);
    }
}

static inline unsigned bit(lzd *s, uint16_t *p) {
    uint32_t bound;
    rc_norm(s);
    bound = (s->range >> 11) * *p;
    if (s->code < bound) {
        s->range = bound;
        *p = (uint16_t)(*p + ((2048 - *p) >> 5));
        return 0;
    }
    s->range -= bound;
    s->code -= bound;
    *p = (uint16_t)(*p - (*p >> 5));
    return 1;
}

static unsigned tree(lzd *s, uint16_t *p, unsigned bits) { /* MSB-first bit tree */
    unsigned m = 1;
    while (m < (1u << bits))
        m = m << 1 | bit(s, &p[m]);
    return m - (1u << bits);
}

static unsigned rtree(lzd *s, uint16_t *p, unsigned bits) { /* LSB-first bit tree */
    unsigned m = 1, r = 0, i;
    for (i = 0; i < bits; i++) {
        unsigned b = bit(s, &p[m]);
        m = m << 1 | b;
        r |= b << i;
    }
    return r;
}

static uint32_t direct(lzd *s, unsigned bits) {
    uint32_t r = 0;
    while (bits--) {
        rc_norm(s);
        s->range >>= 1;
        if (s->code >= s->range) {
            s->code -= s->range;
            r = r << 1 | 1;
        } else {
            r <<= 1;
        }
    }
    return r;
}

/* ---------------- dictionary ---------------- */

static void flush(lzd *s) {
    if (s->dpos > s->dflushed && !s->err) s->err = s->wr(s->wu, s->dict + s->dflushed, s->dpos - s->dflushed);
    s->dflushed = s->dpos;
}

static inline void put(lzd *s, uint8_t b) {
    s->dict[s->dpos++] = b;
    s->pos++;
    if (s->dpos == s->dsize) {
        flush(s);
        s->dpos = s->dflushed = 0;
    }
}

static inline uint8_t back(const lzd *s, uint32_t d) { /* byte at 0-based distance d (d < pos, d < dsize) */
    size_t i = s->dpos > d ? s->dpos - d - 1 : s->dpos + s->dsize - d - 1;
    return s->dict[i];
}

/* ---------------- LZMA ---------------- */

static void init_probs(lzd *s) {
    uint16_t *p = (uint16_t *)&s->m;
    size_t i, n;
    for (i = 0; i < sizeof s->m / sizeof *p; i++) /* every probability starts at 1/2 */
        p[i] = PROB_INIT;
    for (i = 0, n = (size_t)0x300 << (s->lc + s->lp); i < n; i++)
        s->lit[i] = PROB_INIT;
    s->state = 0;
    s->rep[0] = s->rep[1] = s->rep[2] = s->rep[3] = 0;
}

static int set_props(lzd *s, unsigned d, int lzma2) { /* d = (pb * 5 + lp) * 9 + lc */
    if (d >= 9 * 5 * 5) return SLZ_E_CORRUPT;
    s->lc = d % 9;
    s->lp = d / 9 % 5;
    s->pb = d / 45;
    if (lzma2 && s->lc + s->lp > 4) return SLZ_E_CORRUPT;
    return s->lc + s->lp > 8 ? SLZ_E_METHOD : 0;
}

static unsigned length(lzd *s, lenc *l, unsigned ps) {
    if (!bit(s, &l->choice)) return tree(s, l->low[ps], 3);
    if (!bit(s, &l->choice2)) return 8 + tree(s, l->mid[ps], 3);
    return 16 + tree(s, l->high, 8);
}

/* Decode `n` bytes (or up to an end marker when `eos_ok`). The struct fields are the decoder
 * state; only the input/output buffers are touched outside it. */
static int lzma(lzd *s, uint64_t n, int eos_ok) {
    while (n && !s->err) {
        unsigned ps = (unsigned)(s->pos & ((1u << s->pb) - 1)), len;
        uint32_t d;
        if (!bit(s, &s->m.is_match[s->state][ps])) { /* literal */
            unsigned prev = s->pos ? back(s, 0) : 0, sym = 1;
            uint16_t *p = s->lit + 0x300 * (((unsigned)(s->pos & ((1u << s->lp) - 1)) << s->lc) +
                                            (prev >> (8 - s->lc)));
            if (s->state >= 7) { /* after a match: decode against the byte at rep0 */
                unsigned mb = s->rep[0] < s->pos ? back(s, s->rep[0]) : 0;
                do {
                    unsigned m = mb >> 7 & 1, b;
                    mb <<= 1;
                    b = bit(s, &p[((1 + m) << 8) + sym]);
                    sym = sym << 1 | b;
                    if (m != b) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100)
                sym = sym << 1 | bit(s, &p[sym]);
            put(s, (uint8_t)sym);
            n--;
            s->state = s->state < 4 ? 0 : s->state < 10 ? s->state - 3 : s->state - 6;
        } else {
            if (!bit(s, &s->m.is_rep[s->state])) { /* match with a new distance */
                unsigned ls, sl;
                len = length(s, &s->m.mlen, ps);
                s->state = s->state < 7 ? 7 : 10;
                ls = len < 4 ? len : 3;
                sl = tree(s, s->m.slot[ls], 6);
                if (sl < 4) {
                    d = sl;
                } else {
                    unsigned nb = (sl >> 1) - 1;
                    d = (2 | (sl & 1)) << nb;
                    if (sl < 14) {
                        d += rtree(s, s->m.special + d - sl - 1, nb);
                    } else {
                        d += direct(s, nb - 4) << 4;
                        d += rtree(s, s->m.align, 4);
                    }
                }
                if (d == 0xffffffffu) { /* end marker */
                    if (!eos_ok && !s->err) s->err = SLZ_E_CORRUPT;
                    break;
                }
                s->rep[3] = s->rep[2];
                s->rep[2] = s->rep[1];
                s->rep[1] = s->rep[0];
                s->rep[0] = d;
            } else {
                if (!bit(s, &s->m.is_rep0[s->state])) {
                    if (!bit(s, &s->m.is_rep0_long[s->state][ps])) { /* short rep: one byte at rep0 */
                        if (s->rep[0] >= s->pos || s->rep[0] >= s->dsize)
                            return s->err ? s->err : SLZ_E_CORRUPT;
                        s->state = s->state < 7 ? 9 : 11;
                        put(s, back(s, s->rep[0]));
                        n--;
                        continue;
                    }
                } else {
                    if (!bit(s, &s->m.is_rep1[s->state])) {
                        d = s->rep[1];
                    } else {
                        if (!bit(s, &s->m.is_rep2[s->state])) {
                            d = s->rep[2];
                        } else {
                            d = s->rep[3];
                            s->rep[3] = s->rep[2];
                        }
                        s->rep[2] = s->rep[1];
                    }
                    s->rep[1] = s->rep[0];
                    s->rep[0] = d;
                }
                len = length(s, &s->m.rlen, ps);
                s->state = s->state < 7 ? 8 : 11;
            }
            len += 2;
            d = s->rep[0];
            if (d >= s->pos || d >= s->dsize || len > n) return s->err ? s->err : SLZ_E_CORRUPT;
            n -= len;
            while (len--)
                put(s, back(s, d));
        }
        if (s->dpos - s->dflushed >= LZ_FLUSH) flush(s);
    }
    return s->err;
}

static void dict_reset(lzd *s) {
    s->pos = 0;
}

/* .lzma-style stream as used by 7z and zip: 5 property bytes given by the container */
int lzd_lzma(lzd *s, const uint8_t *props, uint64_t n, int eos_ok) {
    int r = set_props(s, props[0], 0);
    if (r) return r;
    init_probs(s);
    dict_reset(s);
    rc_init(s);
    r = lzma(s, n, eos_ok);
    flush(s);
    return r ? r : s->err;
}

/* LZMA2: a sequence of chunks, each LZMA-compressed (with optional state / property /
 * dictionary resets) or stored. */
int lzd_lzma2(lzd *s, uint64_t n) {
    int need_dict = 1, need_props = 1, r = 0;
    for (;;) {
        unsigned c = in_byte(s), un, pk;
        if (s->err) break;
        if (c == 0) break;      /* end of stream */
        if (c == 1 || c == 2) { /* stored chunk: 1 resets the dictionary */
            if (c == 1) {
                dict_reset(s);
                need_dict = 0;
            } else if (need_dict) {
                r = SLZ_E_CORRUPT;
                break;
            }
            un = (unsigned)in_byte(s) << 8;
            un = (un | in_byte(s)) + 1;
            if (un > n) {
                r = SLZ_E_CORRUPT;
                break;
            }
            n -= un;
            while (un-- && !s->err)
                put(s, in_byte(s));
            continue;
        }
        if (c < 0x80) {
            r = SLZ_E_CORRUPT;
            break;
        }
        un = (c & 0x1f) << 16;
        un |= (unsigned)in_byte(s) << 8;
        un = (un | in_byte(s)) + 1;
        pk = (unsigned)in_byte(s) << 8;
        pk = (pk | in_byte(s)) + 1;
        if ((c >> 5 & 3) == 3) {
            dict_reset(s);
            need_dict = 0;
        } else if (need_dict) {
            r = SLZ_E_CORRUPT;
            break;
        }
        if ((c >> 5 & 3) >= 2) {
            if ((r = set_props(s, in_byte(s), 1))) break;
            need_props = 0;
        } else if (need_props) {
            r = SLZ_E_CORRUPT;
            break;
        }
        if ((c >> 5 & 3) >= 1) init_probs(s);
        if (un > n) {
            r = SLZ_E_CORRUPT;
            break;
        }
        n -= un;
        {
            uint64_t start = s->consumed;
            rc_init(s);
            if ((r = lzma(s, un, 0))) break;
            rc_norm(s); /* the encoder's final flush byte belongs to this chunk */
            if (s->consumed - start != pk || s->code != 0) { /* exact packed size, cleanly finished */
                r = SLZ_E_CORRUPT;
                break;
            }
        }
        if (s->dpos - s->dflushed >= LZ_FLUSH) flush(s);
    }
    flush(s);
    if (!r) r = s->err;
    return r ? r : n ? SLZ_E_CORRUPT : 0;
}

void lzd_io(lzd *s, rd_fn rd, void *ru, wr_fn wr, void *wu) {
    s->rd = rd;
    s->ru = ru;
    s->wr = wr;
    s->wu = wu;
    s->ip = s->ie = 0;
    s->consumed = 0;
    s->dpos = s->dflushed = 0;
    s->err = 0;
}

/* ---------------- BCJ x86 filter (decoder) ----------------
 * Converts the absolute addresses that the encoder wrote into E8/E9 (call/jmp) operands back
 * to relative ones. Same algorithm as 7-Zip / xz. Streaming: the last <= 4 bytes of each
 * chunk are held back until the next chunk (an operand may span the boundary). */

static int msbyte_ok(uint8_t b) {
    return b == 0x00 || b == 0xff;
}

static size_t bcj_x86(bcj *f, uint8_t *buf, size_t size) {
    static const uint8_t allowed[8] = {1, 1, 1, 0, 1, 0, 0, 0};
    static const uint8_t bitnum[8] = {0, 1, 2, 2, 3, 3, 3, 3};
    size_t i, prev_pos = (size_t)-1;
    uint32_t prev_mask = f->mask;
    if (size <= 4) return 0;
    size -= 4;
    for (i = 0; i < size; i++) {
        uint32_t src, dest;
        if ((buf[i] & 0xfe) != 0xe8) continue;
        prev_pos = i - prev_pos;
        if (prev_pos > 3) {
            prev_mask = 0;
        } else {
            prev_mask = (prev_mask << (prev_pos - 1)) & 7;
            if (prev_mask && (!allowed[prev_mask] || msbyte_ok(buf[i + 4 - bitnum[prev_mask]]))) {
                prev_pos = i;
                prev_mask = prev_mask << 1 | 1;
                continue;
            }
        }
        prev_pos = i;
        if (!msbyte_ok(buf[i + 4])) {
            prev_mask = prev_mask << 1 | 1;
            continue;
        }
        src = rd32(buf + i + 1);
        for (;;) {
            unsigned j;
            dest = src - (f->pos + (uint32_t)i + 5);
            if (!prev_mask) break;
            j = bitnum[prev_mask] * 8u;
            if (!msbyte_ok((uint8_t)(dest >> (24 - j)))) break;
            src = dest ^ ((1u << (32 - j)) - 1);
        }
        dest &= 0x01ffffff;
        dest |= 0u - (dest & 0x01000000);
        buf[i + 1] = (uint8_t)dest;
        buf[i + 2] = (uint8_t)(dest >> 8);
        buf[i + 3] = (uint8_t)(dest >> 16);
        buf[i + 4] = (uint8_t)(dest >> 24);
        i += 4;
    }
    prev_pos = i - prev_pos;
    f->mask = prev_pos > 3 ? 0 : prev_mask << (prev_pos - 1);
    return i;
}

int bcj_wr(void *u, const uint8_t *p, size_t n) {
    bcj *f = (bcj *)u;
    while (n) {
        size_t k = MIN(n, sizeof f->buf - f->n), done;
        int r;
        memcpy(f->buf + f->n, p, k);
        f->n += k;
        p += k;
        n -= k;
        if (f->n < sizeof f->buf) break;
        done = bcj_x86(f, f->buf, f->n);
        if ((r = f->wr(f->wu, f->buf, done))) return r;
        f->pos += (uint32_t)done;
        memmove(f->buf, f->buf + done, f->n - done);
        f->n -= done;
    }
    return 0;
}

int bcj_finish(bcj *f) { /* end of stream: filter what can be filtered, pass the rest through */
    size_t done = bcj_x86(f, f->buf, f->n);
    int r = f->wr(f->wu, f->buf, f->n);
    f->pos += (uint32_t)done;
    f->n = 0;
    return r;
}
