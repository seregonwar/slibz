/* one inflate.c variant with prefixed entry points (bench/run.sh compiles it once per variant
 * with -DVPFX=v_<name>_ and the slz_i_inf_* entry points renamed to v_<name>_inf_*) */
#include "inflate.c"
#define CAT2(a, b) a##b
#define CAT(a, b)  CAT2(a, b)
void *CAT(VPFX, new)(void);
int CAT(VPFX, decode)(void *z, rd_fn rdf, wr_fn wrf, void *u);
void *CAT(VPFX, new)(void) {
    return inf_new();
}
int CAT(VPFX, decode)(void *z, rd_fn rdf, wr_fn wrf, void *u) {
    inf *s = (inf *)z;
    inf_reset(s, 0, 0, NULL, 0, 0);
    s->rd = rdf;
    s->wr = wrf;
    s->ck = NULL;
    s->ru = s->wu = u;
    return inf_run(s);
}
#ifdef VDIRECT /* the variant has slz_inflate (in memory, zero-copy; renamed v_<name>_inflate) */
int CAT(VPFX, direct)(const uint8_t *c, size_t cn, uint8_t *o, size_t cap, size_t *got);
int CAT(VPFX, direct)(const uint8_t *c, size_t cn, uint8_t *o, size_t cap, size_t *got) {
    return slz_inflate(c, cn, o, cap, NULL, got);
}
#endif
