/* slz - DEFLATE (RFC 1951) decoder.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * Streaming in and out through callbacks (slz_inflate: straight from and into the caller's
 * buffers, with its own instance of the hot loop), 64-bit bit buffer, two-level Huffman tables
 * whose entries carry everything the hot loop needs (bits to consume including extra bits,
 * codeword length, base value, flags), a fast loop that runs while the input and output buffers
 * have margin for a whole symbol, and a careful one-symbol path near the end of the input (or of
 * slz_inflate's output buffer). A checkpoint hook runs at every block boundary (destructive
 * extraction resumes from there).
 * Acceptance rules match zlib exactly (verified by tests/fuzz_inflate.c); every table, window
 * and buffer access is bounded by construction. */
#include "internal.h"

/* x86-64 GCC/Clang: the hot loop is also compiled for BMI2 (shrx/bzhi for the variable shifts
 * and masks) and picked at run time */
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(_MSC_VER) && !defined(__BMI2__) && !defined(SLZ_NO_BMI2)
#define INF_BMI2 1
#endif
#if defined(__GNUC__) || defined(__clang__)
#define UNLIKELY(x)  __builtin_expect(!!(x), 0)
#define INF_INLINE static inline __attribute__((always_inline))
#define INF_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define UNLIKELY(x)  (x)
#define INF_INLINE static __forceinline
#define INF_NOINLINE __declspec(noinline)
#else
#define UNLIKELY(x)  (x)
#define INF_INLINE static inline
#define INF_NOINLINE
#endif

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

static int has_bmi2(void) {
#ifdef INF_BMI2
    __builtin_cpu_init();
    return __builtin_cpu_supports("bmi2") != 0;
#else
    return 0;
#endif
}

inf *inf_new(void) {
    inf *s = (inf *)malloc(sizeof(inf) + IBUF + 8 + OBUF + 16);
    if (s) {
        s->ib = (uint8_t *)(s + 1);
        s->ob = s->ib + IBUF + 8;
        s->oe = OBUF;
        s->direct = 0;
        s->bmi2 = has_bmi2();
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

/* move unread bytes to the front of ib and read until >= need are buffered or input ends */
static int more_input(inf *s, size_t need) {
    size_t r = (size_t)(s->e - s->p);
    if (r >= need || s->eof) return 0;
    memmove(s->ib, s->p, r);
    s->ibase += (uint64_t)(s->p - s->ib);
    s->p = s->ib;
    while (r < need && !s->eof) {
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
    int r = more_input(s, 8);
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

/* Decode table entry (32 bits):
 *   bits 0-5   bits to consume: codeword length + extra bits (subtable pointer: primary bits);
 *              bits 6-7 are 0
 *   bits 8-11  codeword length (subtable pointer: subtable index bits); the extra bits of a
 *              length or distance are (bits & mask(bits 0-5)) >> (bits 8-11). Bits 12-15 are 0
 *              in those entries, so EXTRA masks with the low byte (what x86 bzhi reads) and
 *              shifts by entry >> 8 & 63 (what shift instructions read), with no extra masking
 *   bit 13 EOB, bit 14 SUB (subtable pointer), bit 15 EXC (EOB, SUB, or invalid code)
 *   bits 16-30 value: literal byte, length / distance base, code-length symbol, subtable offset
 *   bit 31 LIT (literal)
 * Subtable entries are relative to the primary bits already consumed. The most bits one entry
 * consumes: 11 + 5 (length), 8 + 13 (distance), 4 + 5 / 7 + 13 in subtables. */
#define LIT 0x80000000u
#define EXC 0x8000u
#define SUB 0x4000u
#define EOB 0x2000u
#define BAD EXC /* invalid code (or a symbol that must never be decoded) */
#define LTM ((1u << LT_BITS) - 1)
#define DTM ((1u << DT_BITS) - 1)
enum { K_PLAIN, K_LITLEN, K_DIST };

/* entry for symbol `sym` of alphabet `kind` without its codeword length l, which the table
 * builder adds as l | l << 8 (no carry: l + extra bits <= 28) */
static uint32_t entry(int kind, unsigned sym) {
    if (kind == K_LITLEN) {
        if (sym < 256) return LIT | sym << 16;
        if (sym == 256) return EOB | EXC;
        if (sym < 286) return (uint32_t)lbase[sym - 257] << 16 | lext[sym - 257];
        return BAD; /* 286, 287: exist in the fixed code but are invalid */
    }
    if (kind == K_DIST) return sym < 30 ? (uint32_t)dbase[sym] << 16 | dext[sym] : BAD;
    return sym << 16;
}

/* bit-reversed bytes: deflate sends codewords MSB-first into an LSB-first bit stream */
#define R2(n) n, n + 128, n + 64, n + 192
#define R4(n) R2(n), R2(n + 32), R2(n + 16), R2(n + 48)
#define R6(n) R4(n), R4(n + 8), R4(n + 4), R4(n + 12)
static const uint8_t rev8[256] = {R6(0), R6(2), R6(1), R6(3)};
#undef R2
#undef R4
#undef R6
#define REV(c, l) ((unsigned)(rev8[(c) & 255] << 8 | rev8[(c) >> 8]) >> (16 - (l))) /* c < 2^l, l <= 15 */

/* Canonical Huffman code lengths -> table with 2^bits primary entries (codes shorter than
 * bits are replicated; longer ones go to subtables of 2^(max - bits) entries, one per distinct
 * primary prefix, so at most one per long code: see LT_SIZE / DT_SIZE). Returns 0 on error.
 * Same acceptance rules as zlib: over-subscribed codes are invalid; incomplete codes are invalid
 * except an empty code or a single code of length 1 (lit/len and distance trees only). */
static int build(uint32_t *t, const uint8_t *len, unsigned n, int kind, unsigned bits) {
    unsigned cnt[16] = {0}, off[16], max, i, l, k, code, used, sb;
    uint32_t ent[320];
    int left = 1;
    for (i = 0; i < n; i++)
        cnt[len[i]]++;
    for (max = 15; max && !cnt[max]; max--)
        ;
    for (i = 1; i < 16; i++) {
        left = (left << 1) - (int)cnt[i];
        if (left < 0) return 0;
    }
    if (left > 0 && (kind == K_PLAIN || max > 1)) return 0;
    for (off[0] = 0, i = 0; i < 15; i++)
        off[i + 1] = off[i] + cnt[i];
    for (i = 0; i < n; i++) /* entries sorted by (length, value) = canonical code order, unused first */
        ent[off[len[i]]++] = entry(kind, i);
    /* Primary table, shortest codes first: after placing the codes of length l in the first
     * 2^l entries, double the table (the next index bit is "don't care" for them). Codewords
     * are consecutive within a length; a longer length appends a 0 bit. */
    t[0] = t[1] = BAD;
    for (i = cnt[0], code = 0, l = 1; l <= bits; l++, code <<= 1) {
        uint32_t c = l | l << 8;
        for (k = 0; k < cnt[l]; k++, code++)
            t[REV(code, l)] = ent[i++] + c;
        if (l < bits) memcpy(t + ((size_t)1 << l), t, sizeof *t * ((size_t)1 << l));
    }
    if (max <= bits) return 1;
    sb = max - bits;
    used = 1u << bits;
    for (; l <= max; l++, code <<= 1) {
        uint32_t c = (l - bits) | (l - bits) << 8;
        for (k = 0; k < cnt[l]; k++, code++) {
            unsigned rev = REV(code, l), j;
            uint32_t *p = &t[rev & ((1u << bits) - 1)], v = ent[i++] + c;
            if (!(*p & SUB)) { /* first long code with this prefix: allocate its subtable */
                *p = used << 16 | EXC | SUB | sb << 8 | bits;
                for (j = 0; j < 1u << sb; j++)
                    t[used + j] = BAD;
                used += 1u << sb;
            }
            for (j = rev >> bits; j < 1u << sb; j += 1u << (l - bits))
                t[(*p >> 16) + j] = v;
        }
    }
    return 1;
}
#undef REV

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
static int dynamic(inf *s) {
    static const uint8_t ord[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t L[320 + 138]; /* slack for the fixed-size stores of the repeat codes */
    uint64_t bb = s->bb;
    unsigned bn = s->bn, hlit, hdist, hclen, i, n;
    const uint8_t *p = s->p;
    int r = SLZ_E_CORRUPT;
/* the bit buffer lives in locals; make >= k bits valid like refill(), inline while 8 input
 * bytes are buffered */
#define NEED(k)                                                                                              \
    if (bn < (k)) {                                                                                          \
        if (s->e - p >= 8) {                                                                                 \
            bb |= ld64(p) << bn;                                                                             \
            p += (63 - bn) >> 3;                                                                             \
            bn |= 56;                                                                                        \
        } else {                                                                                             \
            s->bb = bb, s->bn = bn, s->p = p;                                                                \
            if ((r = refill(s))) return r;                                                                   \
            bb = s->bb, bn = s->bn, p = s->p;                                                                \
            r = SLZ_E_CORRUPT;                                                                               \
        }                                                                                                    \
    }
#define DROP(k) (bb >>= (k), bn -= (k))
    NEED(14);
    hlit = (unsigned)(bb & 31) + 257;
    hdist = (unsigned)(bb >> 5 & 31) + 1;
    hclen = (unsigned)(bb >> 10 & 15) + 4;
    DROP(14);
    if (hlit > 286 || hdist > 30) goto out;
    memset(L, 0, 19);
    for (i = 0; i < hclen; i++) {
        NEED(3);
        L[ord[i]] = (uint8_t)(bb & 7);
        DROP(3);
    }
    if (!build(s->dt, L, 19, K_PLAIN, 7)) goto out;
    for (i = 0, n = hlit + hdist; i < n;) {
        uint32_t e;
        unsigned sym;
        NEED(7 + 7);
        e = s->dt[bb & 127];
        if (e & EXC) goto out;
        DROP(e & 63);
        sym = e >> 16;
        if (sym < 16) {
            L[i++] = (uint8_t)sym;
            continue;
        }
        /* repeat codes store their maximum count, then advance by the actual one */
        if (sym == 16) {
            if (!i) goto out;
            memset(L + i, L[i - 1], 6);
            i += 3 + (unsigned)(bb & 3);
            DROP(2);
        } else if (sym == 17) {
            memset(L + i, 0, 10);
            i += 3 + (unsigned)(bb & 7);
            DROP(3);
        } else {
            memset(L + i, 0, 138);
            i += 11 + (unsigned)(bb & 127);
            DROP(7);
        }
        if (i > n) goto out;
    }
    if (L[256] && build(s->lt, L, hlit, K_LITLEN, LT_BITS) && build(s->dt, L + hlit, hdist, K_DIST, DT_BITS))
        r = 0;
out:
    s->bb = bb, s->bn = bn, s->p = p;
    return r;
#undef NEED
#undef DROP
}

/* Margins for the fast loop. Input: from a position checked against FAST_IN, at most 4 refills
 * run before the next check (each reads 8 bytes and advances <= 7), reading <= 29 bytes.
 * Output: one iteration writes <= 3 literals, or <= 2 literals and a match of <= 258 bytes plus
 * the <= 31-byte overshoot of copy_match. Bytes past op inside the margin are scratch. */
#define FAST_IN  32
#define FAST_OUT (258 + 32 + 2)

/* copy a match of len (3..258) bytes from distance d (1..op) ending inside the output margin */
static inline void copy_match(uint8_t *o, size_t d, size_t len) {
    const uint8_t *m = o - d;
    uint8_t *end = o + len;
    if (d >= 16) {
        do {
            memcpy(o, m, 16);
            memcpy(o + 16, m + 16, 16);
            o += 32;
            m += 32;
        } while (o < end); /* may overshoot <= 31 bytes */
    } else if (d >= 8) {
        do {
            memcpy(o, m, 8);
            memcpy(o + 8, m + 8, 8);
            o += 16;
            m += 16;
        } while (o < end);
    } else if (d == 1) {
        uint64_t v = 0x0101010101010101ull * *m;
        do {
            memcpy(o, &v, 8);
            memcpy(o + 8, &v, 8);
            o += 16;
        } while (o < end);
    } else {
        /* 8-byte stores advancing by d: each load reads bytes already final (the tail of the
         * previous store is overwritten by the next one) */
        do {
            uint64_t v;
            memcpy(&v, m, 8);
            memcpy(o, &v, 8);
            o += d;
            m += d;
        } while (o < end);
    }
}

/* Decode one Huffman block until end-of-block. The bit buffer, input and output cursors live
 * in locals (registers); state is synced back around the rare slow paths. direct (a constant
 * per instance): the output is the caller's buffer ob[0, oe), which is never flushed; the fast
 * loop runs while a whole iteration fits, the careful path checks every symbol against oe. */
INF_INLINE int codes(inf *s, const int direct) {
    uint8_t *ob = s->ob;
    uint64_t bb = s->bb, sv;
    unsigned bn = s->bn;
    size_t len;
    const uint8_t *p = s->p, *pe = s->e;
    size_t op = s->op, d;
    const size_t oe = direct ? s->oe : OBUF;
    const size_t ol = oe - FAST_OUT; /* the fast loop runs while op <= ol (direct: and oe >= FAST_OUT) */
    uint32_t e;
    int r;
#if (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__)
    /* opaque copy: keeps the distance table base in a register (its offset from s is beyond
     * the arm64 load immediate range, so it would otherwise be added on every lookup) */
    const uint32_t *dt = s->dt;
    __asm__("" : "+r"(dt));
#define DT(i) dt[i]
#else
    /* x86 and others: index the table from s (base + index * 4 + offset); with a local copy of
     * the pointer gcc runs out of registers and reloads it from the stack on every lookup */
#define DT(i) s->dt[i]
#endif
#define SAVE()     (s->bb = bb, s->bn = bn, s->p = p, s->op = op)
#define LOAD()     (bb = s->bb, bn = s->bn, p = s->p, pe = s->e, op = s->op)
#define CONSUME(x) (bb >>= (x) & 63, bn -= (x)) /* fast loop: only (u8)bn is valid */
#define REFILL()   (bb |= ld64(p) << (bn & 63), p += 7 - (bn >> 3 & 7), bn |= 56)
#define CONSUMEX(x) (bb >>= (x) & 63, bn -= (x) & 63)
#define EXTRA(x)   ((size_t)((sv & ((1ull << (uint8_t)(x)) - 1)) >> ((x) >> 8 & 63)))
    for (;;) {
        if (!direct && op > ol) {
            SAVE();
            if ((r = inf_flush(s))) return r;
            LOAD();
        }
        if (!direct && pe - p < FAST_IN && !s->eof) {
            SAVE();
            if ((r = more_input(s, FAST_IN))) return r;
            LOAD();
        }
        if (pe - p >= FAST_IN && (!direct || (oe >= FAST_OUT && op <= ol))) {
            /* Fast loop. Invariant at the top: (u8)bn >= 56 and e = primary entry for bb.
             * Upper bits of bn are garbage: whole entries are subtracted and only the low 8
             * bits count. Bit budget per refill: 2 literals (<= 22) + a length (<= 20, with
             * subtable) leave >= 14 bits to preload the distance entry; before the distance
             * (<= 8 + 13 bits from the primary table, <= 28 with a subtable) the loop refills
             * if needed so >= 11 bits remain for the next lookup. */
            const uint8_t *pf = pe - FAST_IN;
            REFILL();
            e = s->lt[bb & LTM];
            do {
                sv = bb;
                CONSUME(e);
                if (e & LIT) {
                    unsigned lit = e >> 16 & 255;
                    e = s->lt[bb & LTM];
                    sv = bb;
                    CONSUME(e);
                    ob[op++] = (uint8_t)lit;
                    if (e & LIT) {
                        lit = e >> 16 & 255;
                        e = s->lt[bb & LTM];
                        sv = bb;
                        CONSUME(e);
                        ob[op++] = (uint8_t)lit;
                        if (e & LIT) { /* third literal: next lookup, refill, next iteration */
                            lit = e >> 16 & 255;
                            e = s->lt[bb & LTM];
                            REFILL();
                            ob[op++] = (uint8_t)lit;
                            continue;
                        }
                    }
                }
                if (UNLIKELY(e & EXC)) {
                    if (UNLIKELY(e & EOB)) {
                        bn &= 255;
                        SAVE();
                        return 0;
                    }
                    if (!(e & SUB)) return SLZ_E_CORRUPT;
                    /* long code: primary bits consumed, look up the rest */
                    e = s->lt[(e >> 16) + ((unsigned)bb & ((1u << (e >> 8 & 15)) - 1))];
                    sv = bb;
                    CONSUME(e);
                    if ((uint8_t)bn < 20 + 8) REFILL();
                    if (e & LIT) {
                        ob[op++] = (uint8_t)(e >> 16);
                        e = s->lt[bb & LTM];
                        REFILL();
                        continue;
                    }
                    if (UNLIKELY(e & EXC)) {
                        if (!(e & EOB)) return SLZ_E_CORRUPT;
                        bn &= 255;
                        SAVE();
                        return 0;
                    }
                }
                len = (size_t)(e >> 16) + EXTRA(e);
                e = DT(bb & DTM);
                if (UNLIKELY(e & EXC)) {
                    if ((uint8_t)bn < 28 + LT_BITS) REFILL();
                    if (!(e & SUB)) return SLZ_E_CORRUPT;
                    CONSUME(e);
                    e = DT((e >> 16) + ((unsigned)bb & ((1u << (e >> 8 & 15)) - 1)));
                    if (e & EXC) return SLZ_E_CORRUPT;
                } else if ((uint8_t)bn < DT_BITS + 13 + LT_BITS) { /* primary-table distance */
                    REFILL();
                }
                sv = bb;
                CONSUME(e);
                d = (size_t)(e >> 16) + EXTRA(e);
                if (UNLIKELY(d > op)) return SLZ_E_CORRUPT;
                e = s->lt[bb & LTM];
                REFILL();
                copy_match(ob + op, d, len);
                op += len;
            } while (p <= pf && op <= ol);
            bn &= 255;
            continue;
        }
        /* careful path near the end of the input (direct: or of the output): one symbol, refills
         * through the slow refill (zero padding past the end is caught by inf_run's final check) */
        if (bn < 48) {
            SAVE();
            if ((r = refill(s))) return r;
            LOAD();
        }
        e = s->lt[bb & LTM];
        if (e & SUB) {
            CONSUMEX(e);
            e = s->lt[(e >> 16) + ((unsigned)bb & ((1u << (e >> 8 & 15)) - 1))];
        }
        sv = bb;
        CONSUMEX(e);
        if (e & LIT) {
            if (direct && op == oe) return SLZ_E_LIMIT;
            ob[op++] = (uint8_t)(e >> 16);
            continue;
        }
        if (e & EXC) {
            if (e & EOB) break;
            return SLZ_E_CORRUPT;
        }
        len = (size_t)(e >> 16) + EXTRA(e);
        e = DT(bb & DTM);
        if (e & SUB) {
            CONSUMEX(e);
            e = DT((e >> 16) + ((unsigned)bb & ((1u << (e >> 8 & 15)) - 1)));
        }
        if (e & EXC) return SLZ_E_CORRUPT;
        sv = bb;
        CONSUMEX(e);
        d = (size_t)(e >> 16) + EXTRA(e);
        if (d > op) return SLZ_E_CORRUPT;
        if (direct && oe - op < len + 31u) { /* no room for copy_match's overshoot: exact copy */
            uint8_t *o = ob + op;
            const uint8_t *m = o - d;
            size_t k;
            if (len > oe - op) return SLZ_E_LIMIT;
            for (k = 0; k < len; k++)
                o[k] = m[k];
        } else
            copy_match(ob + op, d, len);
        op += len;
    }
    SAVE();
    return 0;
#undef SAVE
#undef LOAD
#undef CONSUME
#undef CONSUMEX
#undef REFILL
#undef EXTRA
#undef DT
}

static INF_NOINLINE int codes_generic(inf *s) {
    return codes(s, 0);
}
static INF_NOINLINE int codes_direct(inf *s) {
    return codes(s, 1);
}
#ifdef INF_BMI2
__attribute__((target("bmi2"))) static INF_NOINLINE int codes_bmi2(inf *s) {
    return codes(s, 0);
}
__attribute__((target("bmi2"))) static INF_NOINLINE int codes_direct_bmi2(inf *s) {
    return codes(s, 1);
}
#endif

int inf_run(inf *s) {
    unsigned hdr, v;
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
                if (s->op + 8 >= s->oe) {
                    if (!s->direct) {
                        if ((r = inf_flush(s))) return r;
                    } else if (s->op == s->oe)
                        return SLZ_E_LIMIT;
                }
                if (s->bn) {
                    s->ob[s->op++] = (uint8_t)s->bb;
                    s->bb >>= 8;
                    s->bn -= 8;
                    len--;
                    continue;
                }
                if (s->pad) return SLZ_E_CORRUPT; /* stored data would come from padding */
                if (s->p == s->e) {
                    if ((r = more_input(s, 8))) return r;
                    if (s->p == s->e) return SLZ_E_CORRUPT;
                    continue;
                }
                s->bb = 0; /* bit buffer empty: drop prefetched copies of bytes we now copy directly */
                k = MIN((size_t)len, MIN((size_t)(s->e - s->p), s->oe - s->op));
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
                build(s->lt, L, 288, K_LITLEN, LT_BITS);
                build(s->dt, L + 288, 32, K_DIST, DT_BITS);
                s->fixed = 1;
            }
        } else if (hdr == 2) {
            if ((r = dynamic(s))) return r;
            s->fixed = 0;
        } else
            return SLZ_E_CORRUPT;
#ifdef INF_BMI2
        if (s->direct)
            r = s->bmi2 ? codes_direct_bmi2(s) : codes_direct(s);
        else
            r = s->bmi2 ? codes_bmi2(s) : codes_generic(s);
#else
        r = s->direct ? codes_direct(s) : codes_generic(s);
#endif
        if (r) return r;
    }
    if ((r = bits(s, s->bn & 7, &v))) return r;   /* byte-align end of stream */
    if (s->pad > s->bn / 8) return SLZ_E_CORRUPT; /* consumed bits past real input */
    return s->direct ? 0 : inf_flush(s);
}

int slz_inflate(const void *in, size_t in_len, void *out, size_t out_cap, size_t *in_used, size_t *out_len) {
    static const uint8_t none[1] = {0};
    inf *s = (inf *)malloc(sizeof *s); /* decode tables only: no buffers */
    int r;
    if (in_used) *in_used = 0;
    if (out_len) *out_len = 0;
    if (!s) return SLZ_E_NOMEM;
    /* the input is never written: eof is set from the start, so more_input() never moves it */
    s->ib = (uint8_t *)(uintptr_t)(in ? in : none);
    s->ob = out ? (uint8_t *)out : (uint8_t *)(uintptr_t)none; /* out_cap 0: never written */
    s->oe = out ? out_cap : 0;
    s->direct = 1;
    s->bmi2 = has_bmi2();
    inf_reset(s, 0, 0, NULL, 0, 0);
    s->e = s->ib + (in ? in_len : 0);
    s->eof = 1;
    s->rd = NULL;
    s->wr = NULL;
    s->ck = NULL;
    s->ru = s->wu = NULL;
    r = inf_run(s);
    if (!r) {
        if (in_used) *in_used = (size_t)(inf_bitpos(s) / 8);
        if (out_len) *out_len = s->op;
    }
    free(s);
    return r;
}
