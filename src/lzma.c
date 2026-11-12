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
    uint64_t ibase; /* input bytes before ib[0] */
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

/* slow path: refill the input buffer; at end of input / error it records the error and supplies
 * a zero byte so the current symbol can finish (the decode loop stops at its next check) */
static const uint8_t *more(lzd *s) {
    int64_t k = s->err ? 0 : s->rd(s->ru, s->ib, LZ_IBUF);
    s->ibase += s->ie;
    s->ip = 0;
    if (k > 0) {
        s->ie = (size_t)k;
        return s->ib;
    }
    if (!s->err) s->err = k < 0 ? (int)k : SLZ_E_CORRUPT; /* input ended inside the stream */
    s->ib[0] = 0;
    s->ie = 1;
    return s->ib;
}

static uint8_t in_byte(lzd *s) {
    if (s->ip == s->ie) more(s);
    return s->ib[s->ip++];
}

static uint64_t consumed(const lzd *s) {
    return s->ibase + s->ip;
}

static void rc_init(lzd *s) {
    int i;
    s->range = 0xffffffffu;
    s->code = 0;
    if (in_byte(s) != 0 && !s->err) s->err = SLZ_E_CORRUPT;
    for (i = 0; i < 4; i++)
        s->code = s->code << 8 | in_byte(s);
}

/* Range decoder state used by the hot loop. It lives in a local variable whose address only
 * reaches inline helpers, so the compiler keeps it in registers (dictionary byte stores would
 * otherwise force reloads of everything reachable from the lzd pointer). */
typedef struct {
    uint32_t range, code;
    const uint8_t *ip, *ie;
} rcs;

static inline void rc_norm(lzd *s, rcs *rc) {
    if (rc->range < (1u << 24)) {
        if (rc->ip == rc->ie) {
            s->ip = s->ie;
            rc->ip = more(s);
            rc->ie = s->ib + s->ie;
        }
        rc->range <<= 8;
        rc->code = rc->code << 8 | *rc->ip++;
    }
}

static inline unsigned bit(lzd *s, rcs *rc, uint16_t *p) {
    uint32_t bound;
    rc_norm(s, rc);
    bound = (rc->range >> 11) * *p;
    if (rc->code < bound) {
        rc->range = bound;
        *p = (uint16_t)(*p + ((2048 - *p) >> 5));
        return 0;
    }
    rc->range -= bound;
    rc->code -= bound;
    *p = (uint16_t)(*p - (*p >> 5));
    return 1;
}

/* Branch-free variant for literal bits, which are close to random: the update is computed with a
 * mask instead of a mispredicted branch. Same arithmetic as bit(). */
static inline unsigned bit_nb(lzd *s, rcs *rc, uint16_t *p) {
    uint32_t pr = *p, bound, one;
    rc_norm(s, rc);
    bound = (rc->range >> 11) * pr;
    one = 0u - (uint32_t)(rc->code >= bound); /* all ones when the bit is 1 */
    rc->range = (bound & ~one) | ((rc->range - bound) & one);
    rc->code -= bound & one;
    *p = (uint16_t)(pr + (((2048 - pr) >> 5) & ~one) - ((pr >> 5) & one));
    return one & 1;
}

static inline unsigned tree(lzd *s, rcs *rc, uint16_t *p, unsigned bits) { /* MSB-first bit tree */
    unsigned m = 1;
    while (m < (1u << bits))
        m = m << 1 | bit(s, rc, &p[m]);
    return m - (1u << bits);
}

static inline unsigned rtree(lzd *s, rcs *rc, uint16_t *p, unsigned bits) { /* LSB-first bit tree */
    unsigned m = 1, r = 0, i;
    for (i = 0; i < bits; i++) {
        unsigned b = bit(s, rc, &p[m]);
        m = m << 1 | b;
        r |= b << i;
    }
    return r;
}

static inline uint32_t direct(lzd *s, rcs *rc, unsigned bits) {
    uint32_t r = 0;
    while (bits--) {
        rc_norm(s, rc);
        rc->range >>= 1;
        if (rc->code >= rc->range) {
            rc->code -= rc->range;
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

static void wrap(lzd *s) { /* the window is full: hand it out and start over at its beginning */
    s->dpos = s->dsize;
    flush(s);
    s->dpos = s->dflushed = 0;
}

static void put(lzd *s, uint8_t b) { /* slow path (stored LZMA2 chunks) */
    s->dict[s->dpos++] = b;
    s->pos++;
    if (s->dpos == s->dsize) wrap(s);
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

static inline unsigned length(lzd *s, rcs *rc, lenc *l, unsigned ps) {
    if (!bit(s, rc, &l->choice)) return tree(s, rc, l->low[ps], 3);
    if (!bit(s, rc, &l->choice2)) return 8 + tree(s, rc, l->mid[ps], 3);
    return 16 + tree(s, rc, l->high, 8);
}

/* Decode `n` bytes (or up to an end marker when `eos_ok`). Hot state (range coder, window
 * position, state, rep distances) is held in locals and written back by SAVE() before any slow
 * path that reads it from *s, and on exit. */
static int lzma(lzd *s, uint64_t n, int eos_ok) {
    rcs rc;
    uint8_t *dict = s->dict;
    const size_t dsize = s->dsize;
    size_t dpos = s->dpos;
    uint64_t pos = s->pos;
    unsigned state = s->state;
    uint32_t rep0 = s->rep[0], rep1 = s->rep[1], rep2 = s->rep[2], rep3 = s->rep[3];
    const unsigned pbm = (1u << s->pb) - 1, lpm = (1u << s->lp) - 1, lc = s->lc;
    int r = 0;
#define SAVE()                                                                                               \
    (s->range = rc.range, s->code = rc.code, s->ip = (size_t)(rc.ip - s->ib), s->dpos = dpos, s->pos = pos,  \
     s->state = state, s->rep[0] = rep0, s->rep[1] = rep1, s->rep[2] = rep2, s->rep[3] = rep3)
#define PUT(b)                                                                                               \
    do {                                                                                                     \
        uint8_t byte_ = (b); /* evaluated before dpos changes */                                             \
        dict[dpos++] = byte_;                                                                                \
        if (dpos == dsize) {                                                                                 \
            s->dpos = dpos;                                                                                  \
            wrap(s);                                                                                         \
            dpos = 0;                                                                                        \
        }                                                                                                    \
    } while (0)
    rc.range = s->range;
    rc.code = s->code;
    rc.ip = s->ib + s->ip;
    rc.ie = s->ib + s->ie;
    while (n && !s->err) {
        unsigned ps = (unsigned)pos & pbm, len;
        uint32_t d;
        if (!bit(s, &rc, &s->m.is_match[state][ps])) { /* literal */
            unsigned prev = pos ? dict[dpos ? dpos - 1 : dsize - 1] : 0, sym = 1;
            uint16_t *p = s->lit + 0x300 * ((((unsigned)pos & lpm) << lc) + (prev >> (8 - lc)));
            if (state >= 7) { /* after a match: decode against the byte at rep0 */
                unsigned mb = rep0 < pos ? dict[dpos > rep0 ? dpos - rep0 - 1 : dpos + dsize - rep0 - 1] : 0;
                /* branch-free form (as in the LZMA SDK): offs is 0x100 while the decoded bits
                 * still equal the match byte's bits, 0 afterwards (plain literal probabilities) */
                unsigned offs = 0x100;
                do {
                    unsigned bitv = offs, b;
                    mb <<= 1;
                    offs &= mb;
                    b = bit_nb(s, &rc, &p[offs + bitv + sym]);
                    sym = sym << 1 | b;
                    offs ^= bitv & (b - 1); /* bit 0: keep following the match byte only if it was 0 too */
                } while (sym < 0x100);
            } else {
                while (sym < 0x100)
                    sym = sym << 1 | bit_nb(s, &rc, &p[sym]);
            }
            PUT((uint8_t)sym);
            pos++;
            n--;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
        } else {
            size_t from;
            if (!bit(s, &rc, &s->m.is_rep[state])) { /* match with a new distance */
                unsigned ls, sl;
                len = length(s, &rc, &s->m.mlen, ps);
                state = state < 7 ? 7 : 10;
                ls = len < 4 ? len : 3;
                sl = tree(s, &rc, s->m.slot[ls], 6);
                if (sl < 4) {
                    d = sl;
                } else {
                    unsigned nb = (sl >> 1) - 1;
                    d = (2 | (sl & 1)) << nb;
                    if (sl < 14) {
                        d += rtree(s, &rc, s->m.special + d - sl - 1, nb);
                    } else {
                        d += direct(s, &rc, nb - 4) << 4;
                        d += rtree(s, &rc, s->m.align, 4);
                    }
                }
                if (d == 0xffffffffu) { /* end marker */
                    if (!eos_ok) r = SLZ_E_CORRUPT;
                    break;
                }
                rep3 = rep2;
                rep2 = rep1;
                rep1 = rep0;
                rep0 = d;
            } else {
                if (!bit(s, &rc, &s->m.is_rep0[state])) {
                    if (!bit(s, &rc, &s->m.is_rep0_long[state][ps])) { /* short rep: one byte at rep0 */
                        if (rep0 >= pos || rep0 >= dsize) {
                            r = SLZ_E_CORRUPT;
                            break;
                        }
                        state = state < 7 ? 9 : 11;
                        PUT(dict[dpos > rep0 ? dpos - rep0 - 1 : dpos + dsize - rep0 - 1]);
                        pos++;
                        n--;
                        continue;
                    }
                } else {
                    if (!bit(s, &rc, &s->m.is_rep1[state])) {
                        d = rep1;
                    } else {
                        if (!bit(s, &rc, &s->m.is_rep2[state])) {
                            d = rep2;
                        } else {
                            d = rep3;
                            rep3 = rep2;
                        }
                        rep2 = rep1;
                    }
                    rep1 = rep0;
                    rep0 = d;
                }
                len = length(s, &rc, &s->m.rlen, ps);
                state = state < 7 ? 8 : 11;
            }
            len += 2;
            if (rep0 >= pos || rep0 >= dsize || len > n) {
                r = SLZ_E_CORRUPT;
                break;
            }
            n -= len;
            pos += len;
            /* copy in runs that are contiguous on both sides of the circular window; a forward
             * byte copy is correct for overlapping runs (distance < length) */
            from = dpos > rep0 ? dpos - rep0 - 1 : dpos + dsize - rep0 - 1;
            while (len) {
                size_t k = len, i;
                if (k > dsize - dpos) k = dsize - dpos;
                if (k > dsize - from) k = dsize - from;
                for (i = 0; i < k; i++)
                    dict[dpos + i] = dict[from + i];
                dpos += k;
                from += k;
                len -= (unsigned)k;
                if (from == dsize) from = 0;
                if (dpos == dsize) {
                    s->dpos = dpos;
                    wrap(s);
                    dpos = 0;
                }
            }
        }
        if (dpos - s->dflushed >= LZ_FLUSH) {
            s->dpos = dpos;
            flush(s);
        }
    }
    SAVE();
    return s->err ? s->err : r; /* the root cause (e.g. truncated input) wins */
#undef SAVE
#undef PUT
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
            uint64_t start = consumed(s);
            rc_init(s);
            if ((r = lzma(s, un, 0))) break;
            if (s->range <
                (1u << 24)) { /* final normalization: the encoder's last byte belongs to the chunk */
                s->range <<= 8;
                s->code = s->code << 8 | in_byte(s);
            }
            if (consumed(s) - start != pk || s->code != 0) { /* exact packed size, cleanly finished */
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
    s->ibase = 0;
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
