/* 7-Zip's portable C LZMA2 decoder (C/Lzma2Dec.c + C/LzmaDec.c), one call decoding straight into
 * the output buffer. The official x86-64 7-Zip binaries use an assembly LzmaDec instead. */
#include "Precomp.h"
#include "Lzma2Dec.h"
#include <stdlib.h>

static void *a_alloc(ISzAllocPtr p, size_t n) {
    (void)p;
    return malloc(n);
}
static void a_free(ISzAllocPtr p, void *a) {
    (void)p;
    free(a);
}
static const ISzAlloc a_ = {a_alloc, a_free};

int sdk_decode(const unsigned char *c, size_t cn, unsigned char *o, size_t n, unsigned prop, size_t *got);
int sdk_decode(const unsigned char *c, size_t cn, unsigned char *o, size_t n, unsigned prop, size_t *got) {
    SizeT dl = n, sl = cn;
    ELzmaStatus st;
    SRes r = Lzma2Decode(o, &dl, c, &sl, (Byte)prop, LZMA_FINISH_END, &st, &a_);
    *got = dl;
    return r != SZ_OK || sl != cn;
}
