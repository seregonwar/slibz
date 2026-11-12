/* slz - DEFLATE (RFC 1951) decoder.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * Streaming in and out through callbacks, 64-bit bit buffer, two-level Huffman tables with
 * pre-decoded length/distance bases, a register-resident hot loop, and a checkpoint hook at
 * every block boundary (destructive extraction resumes from there). Acceptance rules match
 * zlib exactly (verified by tests/fuzz_inflate.c); every table, window and buffer access is
 * bounded by construction. */
#include "internal.h"

static const uint16_t lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dbase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                   33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static inline uint64_t ld64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
}

inf *inf_new(void) {
    inf *s = (inf *)malloc(sizeof(inf) + IBUF + 8 + OBUF + 16);
    if (s) {
        s->ib = (uint8_t *)(s + 1);
        s->ob = s->ib + IBUF + 8;
    }
    return s;
}

void inf_reset(inf *s, uint64_t in, unsigned skip, const uint8_t *hist, size_t hn, uint64_t out) {
    s->bb = 0;
    s->bn = s->pad = 0;
    s->eof = 0;
    s->fixed = skip ? -1 - (int)skip : 0; /* pending bit skip, consumed by inf_run */
    s->ibase = in;
    s->p = s->e = s->ib;
    if (hn) memcpy(s->ob, hist, hn);
    s->op = s->of = hn;
    s->obase = out - hn;
}

/* move unread bytes to the front of ib and read until >= 8 are buffered or input ends */
static int more_input(inf *s) {
    size_t r = (size_t)(s->e - s->p);
    if (r >= 8 || s->eof) return 0;
    memmove(s->ib, s->p, r);
    s->ibase += (uint64_t)(s->p - s->ib);
    s->p = s->ib;
    while (r < 8 && !s->eof) {
        int64_t k = s->rd(s->ru, s->ib + r, IBUF - r);
        if (k < 0) return (int)k;
        if (k == 0) s->eof = 1;
        r += (size_t)k;
    }
    s->e = s->ib + r;
    return 0;
}

/* ensure >= 56 valid bits (bits above bn always mirror the next input bytes) */
static int refill(inf *s) {
    int r = more_input(s);
    if (r) return r;
    if (s->e - s->p >= 8) {
        s->bb |= ld64(s->p) << s->bn;
        s->p += (63 - s->bn) >> 3;
        s->bn |= 56;
        return 0;
    }
    while (s->bn <= 56) { /* near end of input: byte at a time, zero padding past the end */
        uint64_t b = 0;
        if (s->p < s->e)
            b = *s->p++;
        else
            s->pad++;
        s->bb |= b << s->bn;
        s->bn += 8;
    }
    return s->pad > 16 ? SLZ_E_CORRUPT : 0;
}

static inline int bits(inf *s, unsigned n, unsigned *v) {
    int r;
    if (s->bn < n && (r = refill(s))) return r;
    *v = (unsigned)(s->bb & ((1ull << n) - 1));
    s->bb >>= n;
    s->bn -= n;
    return 0;
}

/* Room kept free at the end of ob before each symbol: 2 literals, or one match of
 * <= 258 bytes plus the <= 15-byte overshoot of the 16-byte copy loop. */
#define OSLACK (258 + 16 + 2)

/* Two-level decode tables. Entry: bits 0-3 code length (0 = invalid code), bit 4 SUB,
 * bit 5 LIT (literal), bit 6 EOB, bits 8-11 extra bits, bits 16-31 value: the literal byte,
 * the length/distance base, or the symbol (code-length alphabet). Base + extra live in the
 * entry so the hot loop needs one lookup and one shift per length or distance.
 * A SUB entry in the primary table points to a subtable: bits 16-31 = subtable offset,
 * bits 0-3 = subtable index bits. Primary tables stay small (4 KiB / 1 KiB) and cache-hot;
 * long, rare codes cost one extra lookup. */
#define SUB 16u
#define LIT 32u
#define EOB 64u
#define BAD 0xffffffffu /* symbol that must never be decoded */
enum { K_PLAIN, K_LITLEN, K_DIST };

/* decoded meaning of symbol `sym` of alphabet `kind`, without the code length */
static uint32_t entry(int kind, unsigned sym) {
    if (kind == K_LITLEN) {
        if (sym < 256) return sym << 16 | LIT;
        if (sym == 256) return EOB;
        if (sym < 286) return (uint32_t)lbase[sym - 257] << 16 | (uint32_t)lext[sym - 257] << 8;
        return BAD; /* 286, 287: exist in the fixed code but are invalid */
    }
    if (kind == K_DIST) return sym < 30 ? (uint32_t)dbase[sym] << 16 | (uint32_t)dext[sym] << 8 : BAD;
    return sym << 16;
}

static inline uint32_t lookup(const uint32_t *t, uint64_t bb, unsigned bits) {
    uint32_t e = t[bb & ((1u << bits) - 1)];
    if (e & SUB) e = t[(e >> 16) + ((unsigned)(bb >> bits) & ((1u << (e & 15)) - 1))];
    return e;
}

/* Canonical Huffman code lengths -> table. Returns the primary table bits, 0 on error.
 * Same acceptance rules as zlib: over-subscribed codes are invalid; incomplete codes are invalid
 * except an empty code or a single code of length 1 (lit/len and distance trees only).
 * Capacity: 2^bits primary entries + at most 2^15 subtable entries (see LT_SIZE / DT_SIZE). */
static unsigned build(uint32_t *t, const uint8_t *len, unsigned n, int kind, unsigned bits) {
    unsigned cnt[16] = {0}, next[16], max = 0, i, code = 0, used;
    int left = 1;
    for (i = 0; i < n; i++) {
        cnt[len[i]]++;
        if (len[i] > max) max = len[i];
    }
    for (i = 1; i < 16; i++) {
        left = (left << 1) - (int)cnt[i];
        if (left < 0) return 0;
    }
    if (left > 0 && (kind == K_PLAIN || max > 1)) return 0;
    cnt[0] = 0;
    for (i = 1; i < 16; i++) {
        code = (code + cnt[i - 1]) << 1;
        next[i] = code;
    }
    if (max < bits) bits = max ? max : 1;
    used = 1u << bits;
    memset(t, 0, sizeof *t << bits);
    for (i = 0; i < n; i++) {
        unsigned l = len[i], c, rev = 0, k;
        uint32_t v;
        if (!l) continue;
        v = entry(kind, i);
        v = v == BAD ? 0 : v | l; /* invalid symbols decode to an invalid (length 0) entry */
        for (c = next[l]++, k = 0; k < l; k++)
            rev = rev << 1 | (c >> k & 1); /* deflate is LSB-first */
        if (l <= bits) {
            for (k = rev; k < 1u << bits; k += 1u << l)
                t[k] = v;
        } else {
            uint32_t *p = &t[rev & ((1u << bits) - 1)];
            unsigned sb = max - bits;
            if (!(*p & SUB)) { /* first long code with this prefix: allocate its subtable */
                *p = used << 16 | SUB | sb;
                memset(t + used, 0, sizeof *t << sb);
                used += 1u << sb;
            }
            for (k = rev >> bits; k < 1u << sb; k += 1u << (l - bits))
                t[(*p >> 16) + k] = v;
        }
    }
    return bits;
}

int inf_flush(inf *s) {
    int r = s->op > s->of ? s->wr(s->wu, s->ob + s->of, s->op - s->of) : 0;
    if (r) return r;
    if (s->op > WIN) {
        memmove(s->ob, s->ob + s->op - WIN, WIN);
        s->obase += s->op - WIN;
        s->op = WIN;
    }
    s->of = s->op;
    return 0;
}

/* dynamic block header: code-length code, then the literal/length and distance codes */
static int dynamic(inf *s, unsigned *lm, unsigned *dm) {
    static const uint8_t ord[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t L[320] = {0};
    unsigned hlit, hdist, hclen, i, v, cm;
    int r;
    if ((r = bits(s, 5, &hlit)) || (r = bits(s, 5, &hdist)) || (r = bits(s, 4, &hclen))) return r;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30) return SLZ_E_CORRUPT;
    for (i = 0; i < hclen; i++) {
        if ((r = bits(s, 3, &v))) return r;
        L[ord[i]] = (uint8_t)v;
    }
    if (!(cm = build(s->dt, L, 19, K_PLAIN, 7))) return SLZ_E_CORRUPT;
    memset(L, 0, 19);
    for (i = 0; i < hlit + hdist;) {
        unsigned e, l, sym, rep, val = 0;
        if (s->bn < 16 && (r = refill(s))) return r;
        e = lookup(s->dt, s->bb, cm);
        l = e & 15;
        if (!l) return SLZ_E_CORRUPT;
        s->bb >>= l;
        s->bn -= l;
        sym = e >> 16;
        if (sym < 16) {
            L[i++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (!i) return SLZ_E_CORRUPT;
            val = L[i - 1];
            r = bits(s, 2, &rep);
            rep += 3;
        } else if (sym == 17) {
            r = bits(s, 3, &rep);
            rep += 3;
        } else {
            r = bits(s, 7, &rep);
            rep += 11;
        }
        if (r) return r;
        if (i + rep > hlit + hdist) return SLZ_E_CORRUPT;
        while (rep--)
            L[i++] = (uint8_t)val;
    }
    if (!L[256] || !(*lm = build(s->lt, L, hlit, K_LITLEN, LT_BITS)) ||
        !(*dm = build(s->dt, L + hlit, hdist, K_DIST, DT_BITS)))
        return SLZ_E_CORRUPT;
    return 0;
}

/* Decode one Huffman block until end-of-block. Hot loop: the bit buffer, input and output
 * cursors live in locals (registers) because byte stores to ob would otherwise force the
 * compiler to reload them from *s after every write. State is synced back around the rare
 * slow paths (refill near the end of the input buffer, output flush). */
static int codes(inf *s, unsigned lm, unsigned dm) {
    const uint32_t *lt = s->lt, *dt = s->dt;
    uint8_t *ob = s->ob;
    uint64_t bb = s->bb;
    unsigned bn = s->bn;
    const uint8_t *p = s->p, *pe = s->e;
    size_t op = s->op;
    int r = 0;
#define SAVE()     (s->bb = bb, s->bn = bn, s->p = p, s->op = op)
#define LOAD()     (bb = s->bb, bn = s->bn, p = s->p, pe = s->e, op = s->op)
#define CONSUME(n) (bb >>= (n), bn -= (n))
    for (;;) {
        unsigned e, l, x, len, d;
        /* one refill covers litlen + extra + dist + extra (<= 48 bits) */
        if (bn < 48) {
            if (pe - p >= 8) {
                bb |= ld64(p) << bn;
                p += (63 - bn) >> 3;
                bn |= 56;
            } else {
                SAVE();
                if ((r = refill(s))) return r;
                LOAD();
            }
        }
        e = lookup(lt, bb, lm);
        l = e & 15;
        if (!l) {
            r = SLZ_E_CORRUPT;
            break;
        }
        CONSUME(l);
        if (e & LIT) {
            ob[op++] = (uint8_t)(e >> 16);
            /* literals are common: after the refill bn >= 48 - 15 = 33 >= 15, so a second
             * literal can be taken without refilling */
            e = lookup(lt, bb, lm);
            if (e & LIT) {
                CONSUME(e & 15);
                ob[op++] = (uint8_t)(e >> 16);
            }
        } else if (e & EOB) {
            break;
        } else {
            uint8_t *o, *m, *end;
            x = e >> 8 & 15;
            len = (e >> 16) + (unsigned)(bb & ((1u << x) - 1));
            CONSUME(x);
            e = lookup(dt, bb, dm);
            l = e & 15;
            if (!l) {
                r = SLZ_E_CORRUPT;
                break;
            }
            x = e >> 8 & 15;
            d = (e >> 16) + (unsigned)(bb >> l & ((1u << x) - 1));
            CONSUME(l + x);
            if (d > op) {
                r = SLZ_E_CORRUPT;
                break;
            }
            o = ob + op;
            m = o - d;
            end = o + len;
            op += len;
            if (d >= 16) {
                do {
                    memcpy(o, m, 16);
                    o += 16;
                    m += 16;
                } while (o < end); /* may overshoot <= 15 bytes */
            } else if (d >= 8) {
                do {
                    memcpy(o, m, 8);
                    o += 8;
                    m += 8;
                } while (o < end);
            } else if (d == 1) {
                memset(o, *m, len);
            } else {
                while (o < end)
                    *o++ = *m++;
            }
        }
        if (op >= OBUF - OSLACK) {
            SAVE();
            if ((r = inf_flush(s))) return r;
            LOAD();
        }
    }
    SAVE();
    return r;
#undef SAVE
#undef LOAD
#undef CONSUME
}

int inf_run(inf *s) {
    unsigned lm = 0, dm = 0, hdr, v;
    int r, final = 0;
    if (s->fixed < 0) {
        unsigned skip = (unsigned)(-1 - s->fixed);
        s->fixed = 0;
        if ((r = bits(s, skip, &v))) return r;
    }
    while (!final) {
        if (s->ck && (r = s->ck(s->wu, s))) return r; /* block boundary: checkpoint opportunity */
        if ((r = bits(s, 3, &hdr))) return r;
        final = hdr & 1;
        if ((hdr >>= 1) == 0) { /* stored block: LEN, NLEN, then LEN raw bytes */
            unsigned len, nlen;
            if ((r = bits(s, s->bn & 7, &v)) || (r = bits(s, 16, &len)) || (r = bits(s, 16, &nlen))) return r;
            if (len != (~nlen & 0xffff)) return SLZ_E_CORRUPT;
            while (len) {
                size_t k;
                if (s->op >= OBUF - 8 && (r = inf_flush(s))) return r;
                if (s->bn) {
                    s->ob[s->op++] = (uint8_t)s->bb;
                    s->bb >>= 8;
                    s->bn -= 8;
                    len--;
                    continue;
                }
                if (s->pad) return SLZ_E_CORRUPT; /* stored data would come from padding */
                if (s->p == s->e) {
                    if ((r = more_input(s))) return r;
                    if (s->p == s->e) return SLZ_E_CORRUPT;
                    continue;
                }
                s->bb = 0; /* bit buffer empty: drop prefetched copies of bytes we now copy directly */
                k = MIN((size_t)len, MIN((size_t)(s->e - s->p), OBUF - s->op));
                memcpy(s->ob + s->op, s->p, k);
                s->p += k;
                s->op += k;
                len -= (unsigned)k;
            }
            continue;
        }
        if (hdr == 1) { /* fixed Huffman codes (RFC 1951 3.2.6), built once and cached */
            if (s->fixed != 1) {
                uint8_t L[320];
                unsigned i;
                for (i = 0; i < 288; i++)
                    L[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                for (; i < 320; i++)
                    L[i] = 5;
                s->flm = build(s->lt, L, 288, K_LITLEN, LT_BITS);
                s->fdm = build(s->dt, L + 288, 32, K_DIST, DT_BITS);
                s->fixed = 1;
            }
            lm = s->flm;
            dm = s->fdm;
        } else if (hdr == 2) {
            if ((r = dynamic(s, &lm, &dm))) return r;
            s->fixed = 0;
        } else
            return SLZ_E_CORRUPT;
        if ((r = codes(s, lm, dm))) return r;
    }
    if ((r = bits(s, s->bn & 7, &v))) return r;   /* byte-align end of stream */
    if (s->pad > s->bn / 8) return SLZ_E_CORRUPT; /* consumed bits past real input */
    return inf_flush(s);
}
