/* slz - small, fast, hardened archive library (zip/zip64, 7z, tar, gzip, tar.gz).
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * C99 + optional asm (CRC32). Windows, macOS, Linux, FreeBSD, WASI. */
#ifndef SLZ_H
#define SLZ_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    SLZ_OK = 0,
    SLZ_E_IO = -1,
    SLZ_E_NOMEM = -2,
    SLZ_E_FORMAT = -3,
    SLZ_E_CORRUPT = -4,
    SLZ_E_UNSAFE = -5,
    SLZ_E_METHOD = -6,
    SLZ_E_ENCRYPTED = -7,
    SLZ_E_EXISTS = -8,
    SLZ_E_NOTSUP = -9,
    SLZ_E_LIMIT = -10,
    SLZ_E_CANCEL = -11,
    SLZ_E_STATE = -12,
    SLZ_E_OPEN = -13
};
enum { SLZ_ZIP = 1, SLZ_TAR, SLZ_GZ, SLZ_TGZ, SLZ_7Z, SLZ_SLZ /* native format, docs/slz-format.md */ };
/* compression methods (zip numbering where it exists) */
enum { SLZ_M_STORE = 0, SLZ_M_DEFLATE = 8, SLZ_M_LZMA = 14, SLZ_M_LZMA2 = 33, SLZ_M_ZSTD = 93, SLZ_M_LZ4 = 132 };
enum { SLZ_FILE = 0, SLZ_DIR, SLZ_LINK, SLZ_OTHER };

typedef struct {
    const char *name; /* NUL-terminated, raw archive name (not sanitized) */
    uint32_t nlen;
    uint8_t type, enc;          /* SLZ_FILE.., enc: entry is encrypted */
    uint16_t method;            /* 0 store, 8 deflate */
    uint32_t mode, crc;         /* unix permission bits (0 if unknown) */
    int64_t mtime;              /* unix seconds */
    uint64_t usize, csize, off; /* off: zip local header / tar,gz data / 7z packed stream offset */
} slz_entry;

typedef struct slz_archive slz_archive;

/* progress: return non-zero to cancel (state stays resumable in destructive mode) */
typedef int (*slz_progress)(void *user, uint64_t in_done, uint64_t in_total);

enum {
    SLZ_OVERWRITE = 1,   /* replace existing files (never follows links) */
    SLZ_DESTRUCTIVE = 2, /* free archive space while extracting, resumable, deletes archive */
    SLZ_NO_MTIME = 4
};

typedef struct {
    unsigned flags, threads; /* threads: 0 = auto (ignored in destructive mode) */
    uint64_t max_total;      /* anti-bomb cap on total output bytes, 0 = unlimited */
    uint64_t checkpoint;     /* destructive: bytes of input between checkpoints (0 = 64 MiB) */
    slz_progress progress;
    void *user;
    uint64_t files, skipped; /* out: files written, entries skipped (links, devices) */
} slz_opts;

slz_archive *slz_open(const char *path, int *err);
void slz_close(slz_archive *a);
int slz_format(const slz_archive *a);
/* tar.gz has no index: it is built by a full decompression pass on the first call to
 * slz_count / slz_get / slz_dir (0 / NULL / error code if that pass fails). */
size_t slz_count(slz_archive *a);
const slz_entry *slz_get(slz_archive *a, size_t i);

/* List direct children of dir ("" = root, no trailing '/'), implicit dirs included.
 * idx = entry index, or -1 for an implicit directory. O(k log n) after a one-time sort. */
typedef int (*slz_dir_cb)(void *user, const char *name, size_t len, int is_dir, long long idx);
int slz_dir(slz_archive *a, const char *dir, slz_dir_cb cb, void *user);

int slz_extract(slz_archive *a, const char *dest, slz_opts *opts);

/* ---- creating .slz archives ---- */
enum { SLZ_CODEC_DEFAULT = 0, SLZ_CODEC_STORE, SLZ_CODEC_LZ4, SLZ_CODEC_ZSTD };
enum { SLZ_NO_DEDUP = 1 };
typedef struct {
    int codec;            /* SLZ_CODEC_*: default = zstd when built in (slz_has_zstd), else LZ4 */
    int level;            /* zstd level 1..22, 0 = 3 */
    unsigned threads;     /* 0 = CPU count */
    uint32_t block_size;  /* bytes per block, 0 = 8 MiB; 64 KiB .. 256 MiB */
    unsigned flags;       /* SLZ_NO_DEDUP */
    slz_progress progress; void *user; /* progress over input bytes; non-zero cancels */
    uint64_t files, in_bytes, out_bytes; /* out */
} slz_create_opts;

/* Pack files and directories (recursively; links are skipped) into a new .slz archive. The
 * archive is written to "<path>.part" and renamed into place only when complete. */
int slz_create(const char *path, const char *const *inputs, size_t ninputs, slz_create_opts *opts);
int slz_has_zstd(void);
uint32_t slz_crc32(uint32_t crc, const void *p, size_t n);
const char *slz_strerror(int err);

#ifdef __cplusplus
}
#endif
#endif
