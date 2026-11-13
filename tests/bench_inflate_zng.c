/* slz - zlib-ng (native API) column for bench_inflate, enabled with -DSLZ_BENCH_ZLIBNG.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * A separate translation unit because zlib.h and zlib-ng.h cannot be included together. Link
 * libz-ng as a shared library: a static zlib-ng and a static zlib in one binary can clash. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <zlib-ng.h>

const char *bench_zng_version(void);
int bench_zng_inflate(const uint8_t *c, size_t cn, uint8_t *o, size_t n);

const char *bench_zng_version(void) { return zlibng_version(); }

/* raw DEFLATE, whole buffer in one call (like the zlib column); 0 when exactly n bytes came out */
int bench_zng_inflate(const uint8_t *c, size_t cn, uint8_t *o, size_t n) {
    zng_stream zs;
    int32_t r;
    size_t got;
    memset(&zs, 0, sizeof zs);
    if (zng_inflateInit2(&zs, -15) != Z_OK) return -1;
    zs.next_in = c;
    zs.avail_in = (uint32_t)cn;
    zs.next_out = o;
    zs.avail_out = (uint32_t)n;
    r = zng_inflate(&zs, Z_FINISH);
    got = (size_t)zs.total_out;
    zng_inflateEnd(&zs);
    return r == Z_STREAM_END && got == n ? 0 : -1;
}
