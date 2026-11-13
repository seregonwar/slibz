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
