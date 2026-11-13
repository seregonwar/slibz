/* zlib-ng native API in its own translation unit: zlib-ng.h and zlib.h do not mix */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <zlib-ng.h>

int zng_decode(const uint8_t *c, size_t cn, uint8_t *o, size_t n, size_t *got);
const char *zng_ver(void);

int zng_decode(const uint8_t *c, size_t cn, uint8_t *o, size_t n, size_t *got) {
    zng_stream zs;
    int r;
    memset(&zs, 0, sizeof zs);
    if (zng_inflateInit2(&zs, -15) != Z_OK) return -1;
    zs.next_in = c;
    zs.avail_in = (uint32_t)cn;
    zs.next_out = o;
    zs.avail_out = (uint32_t)n;
    r = zng_inflate(&zs, Z_FINISH);
    *got = zs.total_out;
    zng_inflateEnd(&zs);
    return r != Z_STREAM_END;
}

const char *zng_ver(void) {
    return ZLIBNG_VERSION;
}
