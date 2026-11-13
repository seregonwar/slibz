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
