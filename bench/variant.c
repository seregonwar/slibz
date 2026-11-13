/* one lzma.c variant with prefixed entry points (bench/run.sh compiles it once per variant with
 * -DVPFX=v_<name>_ and the slz_i_lzd_* / slz_i_bcj_* entry points renamed to v_<name>_*) */
#include "lzma.c"
#define CAT2(a, b) a##b
#define CAT(a, b)  CAT2(a, b)
int CAT(VPFX, decode)(uint64_t dict, uint64_t n, rd_fn rdf, wr_fn wrf, void *u);
int CAT(VPFX, decode)(uint64_t dict, uint64_t n, rd_fn rdf, wr_fn wrf, void *u) {
    lzd *z = lzd_new(dict, n); /* a new decoder per stream, as tests/bench_lzma.c */
    int r;
    if (!z) return -1000;
    lzd_io(z, rdf, u, wrf, u);
    r = lzd_lzma2(z, n);
    lzd_free(z);
    return r;
}
