/* slz - CRC-32 (IEEE, reflected 0xEDB88320): asm fast paths (x86-64 PCLMULQDQ, ARMv8 CRC32),
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * portable slice-by-8 fallback (also used for WASM / MSVC). */
#include "internal.h"

static uint32_t T[8][256];
static volatile int ready; /* benign init race: every thread writes identical values */

static void init(void) {
    unsigned i, k;
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (k = 0; k < 8; k++)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
        T[0][i] = c;
    }
    for (i = 0; i < 256; i++)
        for (k = 1; k < 8; k++)
            T[k][i] = T[k - 1][i] >> 8 ^ T[0][T[k - 1][i] & 255];
    ready = 1;
}

static uint32_t sb8(uint32_t c, const uint8_t *p, size_t n) {
    for (; n >= 8; p += 8, n -= 8) {
        uint32_t a = c ^ rd32(p), b = rd32(p + 4);
        c = T[7][a & 255] ^ T[6][a >> 8 & 255] ^ T[5][a >> 16 & 255] ^ T[4][a >> 24] ^ T[3][b & 255] ^
            T[2][b >> 8 & 255] ^ T[1][b >> 16 & 255] ^ T[0][b >> 24];
    }
    while (n--)
        c = c >> 8 ^ T[0][(c ^ *p++) & 255];
    return c;
}

#if defined(SLZ_ASM) && (defined(__x86_64__) || defined(_M_X64))
#include <cpuid.h>
uint32_t slz_crc32_pclmul(uint32_t c, const uint8_t *p, size_t n); /* n % 16 == 0, n >= 64 */
static int has_hw(void) {
    static int v = -1;
    unsigned a, b, c, d;
    if (v < 0) v = __get_cpuid(1, &a, &b, &c, &d) && (c & 1u << 1) && (c & 1u << 19);
    return v;
}
#define HW(c, p, m) slz_crc32_pclmul(c, p, m)
#define HW_MIN      64
#elif defined(SLZ_ASM) && (defined(__aarch64__) || defined(_M_ARM64)) &&                                     \
    (defined(__ARM_FEATURE_CRC32) || defined(__APPLE__))
uint32_t slz_crc32_armv8(uint32_t c, const uint8_t *p, size_t n);
#define has_hw()    1
#define HW(c, p, m) slz_crc32_armv8(c, p, m)
#define HW_MIN      16
#endif

uint32_t slz_crc32(uint32_t crc, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t c = ~crc;
    if (!ready) init();
#ifdef HW
    if (n >= HW_MIN && has_hw()) {
        size_t m = n & ~(size_t)15;
        c = HW(c, p, m);
        p += m;
        n -= m;
    }
#endif
    return ~sb8(c, p, n);
}
