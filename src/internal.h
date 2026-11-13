/* slz - internal interfaces shared by the library's translation units.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT */
#ifndef SLZ_INTERNAL_H
#define SLZ_INTERNAL_H
#include "slz.h"
#include <stdlib.h>
#include <string.h>

/* Internal symbols are namespaced so that static linking never clashes with user code. */
#define os_open         slz_i_os_open
#define os_size         slz_i_os_size
#define os_pread        slz_i_os_pread
#define os_pwrite       slz_i_os_pwrite
#define os_sync         slz_i_os_sync
#define os_trunc        slz_i_os_trunc
#define os_punch        slz_i_os_punch
#define os_mtime        slz_i_os_mtime
#define os_close        slz_i_os_close
#define os_unlink       slz_i_os_unlink
#define os_exists       slz_i_os_exists
#define os_allocated    slz_i_os_allocated
#define os_dst_open     slz_i_os_dst_open
#define os_dst_close    slz_i_os_dst_close
#define os_create       slz_i_os_create
#define os_ncpu         slz_i_os_ncpu
#define os_run          slz_i_os_run
#define os_add          slz_i_os_add
#define os_fileid       slz_i_os_fileid
#define os_cas          slz_i_os_cas
#define os_open_journal slz_i_os_open_journal
#define os_sync_parent  slz_i_os_sync_parent
#define os_dst_sync     slz_i_os_dst_sync
#define inf_new         slz_i_inf_new
#define inf_reset       slz_i_inf_reset
#define inf_run         slz_i_inf_run
#define inf_flush       slz_i_inf_flush
#define tar_step        slz_i_tar_step
#define tar_free        slz_i_tar_free
#define sanitize        slz_i_sanitize
#define snap_write      slz_i_snap_write
#define rng_read        slz_i_rng_read
#define lzd_new         slz_i_lzd_new
#define lzd_free        slz_i_lzd_free
#define lzd_io          slz_i_lzd_io
#define lzd_lzma        slz_i_lzd_lzma
#define lzd_lzma2       slz_i_lzd_lzma2
#define bcj_wr          slz_i_bcj_wr
#define bcj_finish      slz_i_bcj_finish
#define open_7z         slz_i_open_7z
#define z7_decode       slz_i_z7_decode
#define z7_supported    slz_i_z7_supported
#define z7_free         slz_i_z7_free

/* MIN evaluates its arguments twice: never pass expressions with side effects. */
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* little-endian loads (archive formats are little-endian; works on any host) */
static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}
static inline uint32_t rd32(const uint8_t *p) {
    return rd16(p) | (uint32_t)rd16(p + 2) << 16;
}
static inline uint64_t rd64(const uint8_t *p) {
    return rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

/* ===================== os.c: the only platform-specific file ===================== */

typedef intptr_t os_fd; /* POSIX file descriptor or Win32 HANDLE */
#define OS_BAD ((os_fd) - 1)

/* os_create modes */
enum {
    OC_EXCL,      /* create, fail if it exists */
    OC_OVERWRITE, /* remove what is there (never following links), then create */
    OC_RESUME,    /* open existing or create, no truncation (destructive resume) */
    OC_MKDIR      /* create the directory path only */
};

/* Extraction destination. Each worker thread owns one.
 * - The handle of the last directory is cached so that sibling files cost a single open.
 * - Windows: the whole chain of directory handles below the root is held open without
 *   FILE_SHARE_DELETE, so none of them can be renamed or replaced (e.g. by a junction) between
 *   the check and the use.
 * - With `track` set (destructive mode), every directory that receives a new entry is recorded
 *   (a duplicated handle) so os_dst_sync can make the new names durable. */
#define DST_DIRTY 64
typedef struct {
    os_fd root;      /* fd / handle of the destination directory */
    void *wroot;     /* Windows: absolute \\?\ path of the destination */
    char cdir[4096]; /* cached relative directory ... */
    size_t cdn;      /* ... its length ... */
    os_fd cfd;       /* ... and its handle */
    os_fd *chain;    /* Windows: held handles of every component of cdir */
    size_t nchain;
    int track;
    os_fd dirty[DST_DIRTY]; /* directories with new entries since the last os_dst_sync */
    os_fd dirty_src;        /* handle the last dirty entry was duplicated from */
    int ndirty;
} os_dst;

int os_open(const char *path, int rw, int create, os_fd *fd);
int64_t os_size(os_fd fd);
int64_t os_pread(os_fd fd, void *b, size_t n, uint64_t off); /* short only at EOF */
int os_pwrite(os_fd fd, const void *b, size_t n, uint64_t off);
int os_sync(os_fd fd, int full); /* full: also flush the drive cache where that is separate (macOS) */
int os_trunc(os_fd fd, uint64_t n);
int os_punch(os_fd fd, uint64_t off, uint64_t len); /* deallocate blocks, keep the size */
int os_mtime(os_fd fd, int64_t t);
void os_close(os_fd fd);
int os_unlink(const char *path);
int os_exists(const char *path);
uint64_t os_allocated(const char *path); /* bytes allocated on disk (sparse aware) */
uint64_t os_fileid(os_fd fd);            /* identity of the open file (volume + inode / file index) */
int os_dst_open(os_dst *d, const char *path);
void os_dst_close(os_dst *d);
int os_dst_sync(os_dst *d); /* make every directory entry created since the last call durable */
/* destructive-mode journal: never follows a link, regular file only, exclusive lock (a second
 * process extracting the same archive gets SLZ_E_STATE) */
int os_open_journal(const char *path, os_fd *fd);
int os_sync_parent(const char *path); /* make the directory entry of `path` durable */
/* create `rel` (sanitized, '/'-separated) below the destination without ever following a link */
int os_create(os_dst *d, const char *rel, int mode, unsigned perm, os_fd *fd);
unsigned os_ncpu(void);
int os_run(unsigned n, void (*fn)(void *), void *arg); /* run fn(arg) on n threads (incl. caller), join */
int64_t os_add(volatile int64_t *p, int64_t v);        /* atomic add, returns the new value */
int os_cas(volatile int64_t *p, int64_t expect, int64_t v); /* atomic compare-and-swap, 1 on success */

/* ===================== inflate.c: DEFLATE decoder ===================== */

#define WIN     32768u                         /* DEFLATE window */
#define IBUF    (1u << 20)                     /* input buffer */
#define OBUF    (WIN + (4u << 20))             /* output buffer: window history + 4 MiB of new output */
#define LT_BITS 10                             /* primary table bits, literal/length code */
#define DT_BITS 8                              /* primary table bits, distance code */
#define LT_SIZE ((1u << LT_BITS) + (1u << 15)) /* primary + worst-case subtables */
#define DT_SIZE ((1u << DT_BITS) + (1u << 15))

typedef int64_t (*rd_fn)(void *u, uint8_t *b, size_t n);   /* bytes read, 0 = end, < 0 = error */
typedef int (*wr_fn)(void *u, const uint8_t *p, size_t n); /* 0 or error (stops decoding) */
typedef struct inf inf;
typedef int (*ck_fn)(void *u, inf *s); /* called at every block boundary; non-zero stops */

struct inf {
    uint64_t bb; /* bit buffer: bn valid bits; bits above bn mirror the next input bytes */
    unsigned bn;
    unsigned pad; /* zero bytes appended past the end of input (must not be consumed) */
    int eof;
    int fixed;            /* 1: fixed tables built; < 0: -1-n bits to skip at start (resume) */
    uint64_t ibase;       /* absolute input offset of ib[0] */
    const uint8_t *p, *e; /* unread input: [p, e) inside ib */
    uint8_t *ib;
    uint8_t *ob; /* output: [0, op) = history + new output; [of, op) not yet written */
    size_t op, of;
    uint64_t obase; /* total output index of ob[0] */
    rd_fn rd;
    wr_fn wr;
    ck_fn ck;
    void *ru, *wu;     /* reader context; writer + checkpoint context */
    unsigned flm, fdm; /* primary bits of the cached fixed tables */
    uint32_t lt[LT_SIZE], dt[DT_SIZE];
};

inf *inf_new(void); /* one allocation: state + buffers; free() it */
/* Start decoding at absolute input byte `in`, after skipping `skip` bits. hist/hn = the last
 * output bytes (<= 32 KiB, for resume), out = total bytes produced so far. */
void inf_reset(inf *s, uint64_t in, unsigned skip, const uint8_t *hist, size_t hn, uint64_t out);
int inf_run(inf *s);   /* decode to the end of the stream */
int inf_flush(inf *s); /* hand pending output to wr (checkpoints call it at block boundaries) */
static inline uint64_t inf_bitpos(const inf *s) { /* absolute input position of the next bit */
    return (s->ibase + (uint64_t)(s->p - s->ib) + s->pad) * 8 - s->bn;
}

/* ===================== lzma.c: LZMA, LZMA2, BCJ x86 ===================== */

typedef struct lzd lzd;
lzd *lzd_new(uint64_t dict_size, uint64_t total_out); /* window = min(dict_size, total_out) */
void lzd_free(lzd *s);
void lzd_io(lzd *s, rd_fn rd, void *ru, wr_fn wr, void *wu);
int lzd_lzma(lzd *s, const uint8_t *props5, uint64_t n, int eos_ok); /* n = exact output size */
int lzd_lzma2(lzd *s, uint64_t n);
typedef struct { /* BCJ x86 decoder in front of a writer: wr_fn = bcj_wr, context = the bcj */
    wr_fn wr;
    void *wu;
    uint32_t pos, mask;
    size_t n;
    uint8_t buf[1 << 16];
} bcj;
int bcj_wr(void *u, const uint8_t *p, size_t n);
int bcj_finish(bcj *f);

/* ===================== sevenz.c: 7z ===================== */

typedef struct {
    uint8_t id[4]; /* method id (longer ids are unsupported) */
    unsigned idn;
    const uint8_t *props; /* points into the archive's header buffer */
    unsigned np;
} coder7;

typedef struct { /* a folder = one coder chain over one packed stream = 1..n files */
    unsigned nc, nout, npacked;
    coder7 c[4];
    int unsupported;   /* method or coder graph we cannot decode (still listed) */
    int enc;           /* AES */
    int filter;        /* BCJ x86 after the decoder */
    unsigned dec;      /* coder index of the decompressor */
    unsigned main_out; /* the folder's final output stream */
    size_t pack_index;
    uint64_t pack_off, pack_size, usize;
    uint32_t crc;
    int has_crc;
    size_t first, nsub; /* its substreams (files), in archive order */
} folder7;

typedef struct {
    folder7 *f;
    size_t nf;
    uint8_t *hdr;           /* decoded header (owns coder properties) */
    uint32_t *entry_folder; /* per entry: folder index, UINT32_MAX = no data (dir, empty file) */
    uint8_t *entry_hascrc;
    uint32_t *sub_entry; /* substream (folder order) -> entry index */
} z7;

int open_7z(slz_archive *a, const uint8_t *head, size_t hn);
int z7_supported(const folder7 *f);
/* exactly f->usize bytes to wr; wr stops it once it has the first need of them (bounds the window) */
int z7_decode(os_fd fd, const folder7 *f, uint64_t need, wr_fn wr, void *wu);
void z7_free(z7 *z);

/* ===================== formats.c: archive formats ===================== */

/* sequential reader over [pos, end) of a file: the rd_fn behind every compressed stream */
typedef struct {
    os_fd fd;
    uint64_t pos, end;
} rng;
int64_t rng_read(void *r, uint8_t *b, size_t n);

struct slz_archive {
    char *path;
    os_fd fd;
    uint64_t size;
    int fmt;
    int partial;        /* a destructive extraction is in progress (journal present) */
    int lazy, lazy_err; /* tar.gz: index not built yet / result of building it */
    uint64_t cd_off;    /* zip: central directory offset (bound for entry data) */
    uint64_t zip_bias;  /* zip: bytes prepended to the archive (self-extractors) */
    uint64_t gz_off;    /* gzip: offset of the deflate stream */
    slz_entry *e;       /* the index */
    size_t n;
    char *names;              /* all entry names, NUL-terminated */
    const slz_entry **sorted; /* by name, built on the first slz_dir */
    z7 *z7;                   /* 7z: folders and entry -> folder map */
    uint64_t fp;              /* fingerprint of the index (journal identity check) */
};

/* tar push parser, shared by tar (driven by the indexer, data skipped) and tar.gz (driven
 * by the decompressor). States: 0 header, 1 meta data (pax / GNU long name), 2 entry data,
 * 3 padding, 4 end of archive. */
typedef struct {
    uint8_t h[512];
    size_t hn; /* header bytes collected */
    int st;
    uint64_t left, pad; /* data and padding bytes left in the current member */
    char *meta;         /* pax / GNU long name payload being collected */
    size_t mn;
    int mtype;
    char *lname;    /* pending path override for the next entry */
    uint64_t psize; /* pending pax size override */
    int haspsize;
    slz_entry cur; /* the entry just parsed (T_ENTRY) */
    char name[4096 + 256];
} tarp;
enum { T_MORE, T_ENTRY, T_DATA, T_END, T_ERR };
/* consume input; returns at each event: T_ENTRY (t->cur valid), T_DATA (*dp, *dn = entry bytes) */
int tar_step(tarp *t, const uint8_t *p, size_t n, size_t *used, const uint8_t **dp, size_t *dn);
void tar_free(tarp *t);

/* archive name -> safe relative path ('/'-separated, no '.', '..', absolute or device parts).
 * Returns the length, 0 for "nothing" (e.g. "./"), or SLZ_E_UNSAFE / SLZ_E_LIMIT. */
int sanitize(const char *in, size_t n, char *out, size_t cap);

#define SNAP_AT 4096 /* journal offset of the index snapshot */
int snap_write(const slz_archive *a, os_fd fd, uint64_t at);
#endif
