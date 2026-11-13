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
