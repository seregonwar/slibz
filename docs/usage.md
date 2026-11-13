# Usage

[slz](../README.md) > [docs](README.md) > Usage

## Command line

```bash
slz l archive.7z                 # list everything
slz l archive.zip docs/images    # direct children of a directory
slz x archive.zip out            # extract (all CPU cores)
slz x big.zip out -d             # in-place: frees big.zip while extracting, resumable
```

Options for `x`: `-o` overwrite, `-d` destructive, `-j N` threads, `-c MiB` checkpoint interval,
`-n` no mtime, `-q` quiet.

## Library API

The public API is declared in [`include/slz.h`](../include/slz.h).

```c
#include <slz.h>

int err;
slz_archive *a = slz_open("big.zip", &err);           /* zip / 7z / tar / gz / tar.gz, auto-detected */
if (!a) { fprintf(stderr, "%s\n", slz_strerror(err)); return 1; }

for (size_t i = 0; i < slz_count(a); i++) {             /* flat listing */
    const slz_entry *e = slz_get(a, i);
    printf("%s %llu\n", e->name, (unsigned long long)e->usize);
}
slz_dir(a, "docs/images", on_child, user);              /* tree browsing, implicit dirs included */

slz_opts o = {0};
o.flags = SLZ_DESTRUCTIVE;                              /* optional */
err = slz_extract(a, "out", &o);
slz_close(a);
```

`slz_opts`: `flags` (`SLZ_OVERWRITE`, `SLZ_DESTRUCTIVE`, `SLZ_NO_MTIME`), `threads` (0 = CPU count),
`max_total` (cap on output bytes), `checkpoint` (destructive interval, default 64 MiB), `progress`
(callback, may cancel); `files` and `skipped` are filled in on return.

In-place mode is described in [In-place (destructive) extraction](in-place-extraction.md), building and
linking in [Building and integration](building.md).

### Reading one entry into memory

`slz_read_mem` and `slz_read` decode a single `SLZ_FILE` entry without writing anything to disk:
`slz_read_mem` into a buffer you own, `slz_read` to a callback.

```c
int slz_read_mem(slz_archive *a, size_t i, void *buf, size_t cap, size_t *n);
typedef int (*slz_write_cb)(void *user, const void *p, size_t n);
int slz_read(slz_archive *a, size_t i, slz_write_cb cb, void *user, slz_opts *opts);
```

Into a buffer sized from `usize`, but never above a limit of your own:

```c
#define MAX_ENTRY ((size_t)64 << 20)

const slz_entry *e = slz_get(a, i);  /* usize is declared by the archive, not trusted */
size_t n, cap = e->usize < MAX_ENTRY ? (size_t)e->usize : MAX_ENTRY;
void *buf = cap ? malloc(cap) : NULL;
int err = cap && !buf ? SLZ_E_NOMEM : slz_read_mem(a, i, buf, cap, &n);
if (err)  /* SLZ_E_LIMIT: larger than MAX_ENTRY; discard any bytes stored */
    fprintf(stderr, "entry %zu: %s\n", i, slz_strerror(err));
else
    use(buf, n);  /* n verified bytes */
free(buf);
```

Streamed to a callback, with a bound on the total work:

```c
static int on_data(void *user, const void *p, size_t len) {
    return fwrite(p, 1, len, (FILE *)user) != len;  /* non-zero cancels: SLZ_E_CANCEL */
}

slz_opts o = {0};
o.max_total = (uint64_t)256 << 20;  /* every byte decoded, those before the entry included */
err = slz_read(a, i, on_data, stdout, &o);
```

`slz_read_mem`:

- Stores at most `cap` bytes in `buf` and never writes past `cap`, whatever the archive declares. `buf`
  may be `NULL` when `cap` is 0 (empty entries).
- `n` may be `NULL`; otherwise it receives the number of bytes stored, on error too. On success it is
  the entry's exact size.
- `SLZ_E_LIMIT` means the entry does not fit: before anything is decoded if `usize > cap` (`n` = 0), or
  once `cap` bytes are stored if a gzip trailer understates the size (`n` = `cap`).
- It has no `max_total`: for an untrusted tar.gz or solid 7z, prefer `slz_read` with one (see the cost
  below).

`slz_read`:

- Passes the entry's data to `cb(user, p, len)` in order, in chunks with `len > 0`; a non-zero return
  cancels with `SLZ_E_CANCEL`.
- `opts` may be `NULL`. Only `max_total`, `progress` and `opts->user` (the progress argument) are used;
  `flags`, `threads` and `checkpoint` are ignored, and `files` / `skipped` are not written.
- `max_total` counts every byte decoded, including the ones before the entry in a tar.gz or solid 7z.
  It is checked against the declared sizes before anything is decoded (`SLZ_E_LIMIT`, no callback
  made), then again while decoding.
- `progress(user, done, total)` counts decoded bytes against the bytes to decode.

Verification: the data received before the call returns is unverified, and only `SLZ_OK` means the size
and the CRC-32 (zip, gzip, 7z) or the xxHash64 (.slz: the entry's, and each block's) matched; on any
error, discard what was received. tar has no checksum. A tar.gz read stops at the end of the entry, so
the gzip trailer is not checked; a solid 7z read stops there too, so the folder CRC is checked only for
the last entry of a folder (the entry's own CRC is always checked when the archive stores one).

#### Errors

| code | when |
|---|---|
| `SLZ_E_ARG` | `a` or `cb` is `NULL`, `buf` is `NULL` with `cap > 0`, or `i` is out of range |
| `SLZ_E_STATE` | the archive was left mid destructive extraction (finish it with `slz_extract` and `SLZ_DESTRUCTIVE`), or a destructive extraction has already consumed it |
| `SLZ_E_NOTSUP` | the entry is a directory, a link or another non-file |
| `SLZ_E_ENCRYPTED`, `SLZ_E_METHOD` | encrypted entry, unsupported compression method |
| `SLZ_E_LIMIT` | larger than `cap`, or more than `max_total` bytes to decode |
| `SLZ_E_CORRUPT` | the data does not match its declared size or CRC, is out of bounds, or shares bytes with another entry |
| `SLZ_E_NOMEM`, `SLZ_E_IO` | out of memory, read error |
| `SLZ_E_CANCEL` | `cb` or `progress` returned non-zero |

If the tar.gz index could not be built, `slz_count` is 0 and the read returns the index error instead
of `SLZ_E_ARG`.

#### Limits and untrusted archives

`slz_get(a, i)->usize` is the size the archive declares. How far it can be trusted:

- zip, 7z: any value up to 2^64-1, checked only by decoding.
- tar: at most the archive's size.
- tar.gz: taken from the tar headers, whose data the index pass found complete.
- gzip: the trailer's ISIZE, the size modulo 2^32: a lower bound, or anything at all if forged.
- .slz: within the archive's stream, whose blocks declare their sizes (up to 256 MiB each).

Never allocate `usize` bytes without a limit of your own. Neither function does:

- The declared sizes are checked against `cap` / `max_total` before any allocation.
- Except for gzip, decoding stops with `SLZ_E_CORRUPT` as soon as an entry produces more than its
  declared size. A gzip member is checked against ISIZE (modulo 2^32) only at the end, and
  `slz_read_mem` stops it at `cap`.
- Memory, freed on return: about 5 MiB for DEFLATE, 1 MiB for stored data, and for LZMA / LZMA2 a
  window no larger than the dictionary or the bytes to decode (+64 KiB with BCJ), and for .slz one block
  (8 MiB by default, up to 256 MiB) and its compressed form (no larger than the archive). With
  `slz_read` and a `max_total`, that is at most `max_total` plus the margin.

#### Cost and threads

- zip, tar, gzip and non-solid 7z read only the entry's own data; .slz decodes the blocks the entry
  covers, whole (`max_total` counts all of their bytes).
- tar.gz and solid 7z decode, and drop, everything before the entry in the stream or folder: reading
  every entry this way is quadratic, so use `slz_extract` for bulk extraction. For tar.gz, the first
  `slz_count` / `slz_get` also builds the index with one full decompression pass.
- Concurrent reads of one archive are safe once `slz_count` has returned (a tar.gz builds its index on
  that first call).
### In-memory DEFLATE (zero-copy)

`slz_inflate` decodes a raw DEFLATE stream (RFC 1951, no zlib or gzip wrapper) from one buffer
straight into another: the input is read in place and the output is written once, directly into
the caller's buffer, with no intermediate window or copy. It is the fastest way to inflate when
the uncompressed size is known (a zip entry's `usize`, a gzip trailer's ISIZE) or bounded.

```c
size_t used, got;
int r = slz_inflate(comp, comp_len, buf, buf_cap, &used, &got);
if (r == SLZ_E_LIMIT) { /* buf_cap too small: retry with a larger buffer */ }
else if (r) { fprintf(stderr, "%s\n", slz_strerror(r)); return 1; }
/* buf[0, got) = the data; comp[used, comp_len) = whatever follows the stream (e.g. a gzip trailer) */
```

- Returns 0, `SLZ_E_CORRUPT` (invalid or truncated stream), `SLZ_E_LIMIT` (the output does not fit
  in `out_cap`) or `SLZ_E_NOMEM`. `in_used` and `out_len` may be `NULL`; both are 0 on error.
- `out[*out_len, out_cap)` may be overwritten (the decoder copies matches in whole words), and so
  may all of `out` on error. Giving a few dozen bytes of slack beyond the exact size keeps the
  fast path running to the very end, but is not required.
- Thread-safe, no global state; each call allocates ~43 KiB of decode tables.
- Acceptance rules are identical to the streaming decoder used by `slz_extract` (and to zlib's).
