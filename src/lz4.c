/* slz - LZ4 block codec (raw block format, no frame) and xxHash64.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * LZ4 is the fast, dependency-free codec of the .slz format: a greedy single-hash encoder and a
 * decoder that checks every length and offset against both buffers. xxHash64 is the block and
 * file integrity hash of .slz. Both are byte-compatible with the reference implementations
 * (verified by tests/fuzz_codec.c against liblz4 and xxhsum). */
#include "internal.h"

/* ---------------- xxHash64 ---------------- */

#define P1 0x9E3779B185EBCA87ull
#define P2 0xC2B2AE3D27D4EB4Full
#define P3 0x165667B19E3779F9ull
#define P4 0x85EBCA77C2B2AE63ull
#define P5 0x27D4EB2F165667C5ull

static inline uint64_t rotl64(uint64_t x, int r) {
    return x << r | x >> (64 - r);
}
static inline uint64_t round64(uint64_t acc, uint64_t in) {
    return rotl64(acc + in * P2, 31) * P1;
}
static inline uint64_t merge64(uint64_t acc, uint64_t v) {
    return (acc ^ round64(0, v)) * P1 + P4;
}

void xxh64_init(xxh64_t *s, uint64_t seed) {
    memset(s, 0, sizeof *s);
    s->v[0] = seed + P1 + P2;
    s->v[1] = seed + P2;
    s->v[2] = seed;
    s->v[3] = seed - P1;
    s->seed = seed;
}

static const uint8_t *stripes(xxh64_t *s, const uint8_t *p, const uint8_t *end) {
    while (end - p >= 32) {
        s->v[0] = round64(s->v[0], rd64(p));
        s->v[1] = round64(s->v[1], rd64(p + 8));
        s->v[2] = round64(s->v[2], rd64(p + 16));
        s->v[3] = round64(s->v[3], rd64(p + 24));
        p += 32;
    }
    return p;
}

void xxh64_update(xxh64_t *s, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data, *end = p + n;
    s->total += n;
    if (s->mn + n < 32) { /* not enough for a stripe: buffer it */
        if (n) memcpy(s->mem + s->mn, p, n);
        s->mn += (unsigned)n;
        return;
    }
    if (s->mn) { /* complete the buffered stripe */
        size_t k = 32 - s->mn;
        memcpy(s->mem + s->mn, p, k);
        stripes(s, s->mem, s->mem + 32);
        p += k;
        s->mn = 0;
    }
    p = stripes(s, p, end);
    s->mn = (unsigned)(end - p);
    if (s->mn) memcpy(s->mem, p, s->mn);
}

uint64_t xxh64_digest(const xxh64_t *s) {
    const uint8_t *p = s->mem, *end = p + s->mn;
    uint64_t h;
    if (s->total >= 32) {
        h = rotl64(s->v[0], 1) + rotl64(s->v[1], 7) + rotl64(s->v[2], 12) + rotl64(s->v[3], 18);
        h = merge64(h, s->v[0]);
        h = merge64(h, s->v[1]);
        h = merge64(h, s->v[2]);
        h = merge64(h, s->v[3]);
    } else {
        h = s->seed + P5;
    }
    h += s->total;
    for (; end - p >= 8; p += 8)
        h = rotl64(h ^ round64(0, rd64(p)), 27) * P1 + P4;
    if (end - p >= 4) {
        h = rotl64(h ^ (uint64_t)rd32(p) * P1, 23) * P2 + P3;
        p += 4;
    }
    for (; p < end; p++)
        h = rotl64(h ^ (uint64_t)*p * P5, 11) * P1;
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    return h ^ h >> 32;
}

uint64_t xxh64(const void *data, size_t n, uint64_t seed) {
    xxh64_t s;
    xxh64_init(&s, seed);
    xxh64_update(&s, data, n);
    return xxh64_digest(&s);
}

/* ---------------- LZ4 block format ----------------
 * sequence = token (literal length << 4 | match length - 4), extra literal length bytes,
 *            literals, 16-bit offset, extra match length bytes; the last sequence has literals
 *            only. Lengths of 15 continue with bytes added until one is below 255. */

#define LZ4_MINMATCH 4
#define LZ4_LASTLIT  5  /* the last 5 bytes are always literals */
#define LZ4_MFLIMIT  12 /* no match may start within the last 12 bytes */
#define LZ4_HBITS    14

size_t lz4_bound(size_t n) {
    return n + n / 255 + 16;
}

static uint8_t *put_len(uint8_t *o, size_t n) { /* length continuation bytes after a 15 nibble */
    for (; n >= 255; n -= 255)
        *o++ = 255;
    *o++ = (uint8_t)n;
    return o;
}

static uint8_t *put_seq(uint8_t *o, const uint8_t *lit, size_t nlit, size_t off, size_t mlen) {
    uint8_t *tok = o++;
    *tok = (uint8_t)((nlit >= 15 ? 15 : nlit) << 4);
    if (nlit >= 15) o = put_len(o, nlit - 15);
    memcpy(o, lit, nlit);
    o += nlit;
    if (!mlen) return o; /* last sequence */
    *o++ = (uint8_t)off;
    *o++ = (uint8_t)(off >> 8);
    mlen -= LZ4_MINMATCH;
    *tok |= (uint8_t)(mlen >= 15 ? 15 : mlen);
    if (mlen >= 15) o = put_len(o, mlen - 15);
    return o;
}

static inline uint32_t hash4(uint32_t v) {
    return (v * 2654435761u) >> (32 - LZ4_HBITS);
}

/* Greedy encoder: one hash table of the last position seen for each 4-byte value; the step grows
 * on long runs without matches (as LZ4's acceleration does) so incompressible input stays fast.
 * Output fits in lz4_bound(n). */
size_t lz4_compress(const uint8_t *src, size_t n, uint8_t *dst) {
    uint32_t *table = (uint32_t *)calloc((size_t)1 << LZ4_HBITS, sizeof *table);
    const uint8_t *anchor = src;
    uint8_t *o = dst;
    size_t i = 0, misses = 0;
    if (!table) return 0;
    if (n > LZ4_MFLIMIT) {
        size_t limit = n - LZ4_MFLIMIT, mlimit = n - LZ4_LASTLIT;
        while (i < limit) {
            uint32_t v = rd32(src + i), h = hash4(v);
            size_t ref = table[h];
            table[h] = (uint32_t)i + 1; /* 0 = empty */
            if (ref && i + 1 - ref <= 65535 && rd32(src + ref - 1) == v) {
                size_t m = ref - 1, len = LZ4_MINMATCH;
                while (i > (size_t)(anchor - src) && m > 0 && src[i - 1] == src[m - 1]) { /* extend back */
                    i--;
                    m--;
                    len++;
                }
                while (i + len < mlimit && src[m + len] == src[i + len])
                    len++;
                o = put_seq(o, anchor, (size_t)(src + i - anchor), i - m, len);
                i += len;
                anchor = src + i;
                if (i >= 2 && i - 2 < limit) table[hash4(rd32(src + i - 2))] = (uint32_t)(i - 2) + 1;
                misses = 0;
            } else {
                i += 1 + (misses++ >> 6);
            }
        }
    }
    o = put_seq(o, anchor, (size_t)(src + n - anchor), 0, 0);
    free(table);
    return (size_t)(o - dst);
}

/* Decode exactly `n` bytes; 0 on success, SLZ_E_CORRUPT on any malformed input. */
int lz4_decompress(const uint8_t *src, size_t sn, uint8_t *dst, size_t n) {
    const uint8_t *ip = src, *iend = src + sn;
    uint8_t *op = dst, *oend = dst + n;
    for (;;) {
        size_t lit, ml, off;
        unsigned tok;
        if (ip >= iend) return SLZ_E_CORRUPT;
        tok = *ip++;
        lit = tok >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (ip >= iend) return SLZ_E_CORRUPT;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if (lit > (size_t)(iend - ip) || lit > (size_t)(oend - op)) return SLZ_E_CORRUPT;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (ip == iend) break; /* the last sequence has no match */
        if (iend - ip < 2) return SLZ_E_CORRUPT;
        off = (size_t)ip[0] | (size_t)ip[1] << 8;
        ip += 2;
        if (!off || off > (size_t)(op - dst)) return SLZ_E_CORRUPT;
        ml = tok & 15;
        if (ml == 15) {
            unsigned b;
            do {
                if (ip >= iend) return SLZ_E_CORRUPT;
                b = *ip++;
                ml += b;
            } while (b == 255);
        }
        ml += LZ4_MINMATCH;
        if (ml > (size_t)(oend - op)) return SLZ_E_CORRUPT;
        if (off >= ml) {
            memcpy(op, op - off, ml);
        } else { /* overlapping run */
            const uint8_t *m = op - off;
            size_t k;
            for (k = 0; k < ml; k++)
                op[k] = m[k];
        }
        op += ml;
    }
    return op == oend ? 0 : SLZ_E_CORRUPT;
}
