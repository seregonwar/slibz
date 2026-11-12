# slz

**Fast, hardened archive extraction in C99 + assembly — zip/zip64, 7z, tar, gzip, tar.gz.**

Created by **Seregon** · Copyright (c) 2026 Seregon · [MIT License](LICENSE)

Windows · macOS · Linux · FreeBSD · WebAssembly (WASI) — no dependencies.

---

## Why

- **Listing in milliseconds.** A zip's central directory is read with one `pread` and parsed in place
  (names are compacted inside the same buffer, no per-entry allocation). 500 000 entries list in
  ~75 ms. `slz_dir` browses the tree (implicit directories included) with binary search.
- **Extraction at SSD speed.** Entries are processed in on-disk order by a pool of workers, each with its
  own file handle, decoder and cached directory handle; DEFLATE decodes ~1.6× faster than zlib 1.2.11.
- **In-place ("destructive") extraction.** With `SLZ_DESTRUCTIVE` the archive's space is released
  *while* it is extracted, with a crash-safe journal: a 30 GB zip extracts into ~30 GB of output
  without ever needing 60 GB. See [Destructive extraction](#destructive-extraction).
- **Hostile input is the default assumption.** Every parser is bounds-checked by construction,
  the decoders are differentially fuzzed against zlib and liblzma, and the filesystem layer never
  follows a link. See [Security](#security).
- **Small.** ~4 200 lines of C for five formats, three codecs and a filter, plus ~130 lines of assembly — one shared
  path sanitizer, one tar parser for tar and tar.gz, one range reader and one extraction engine for every format.

## Formats

| format | listing | extraction | destructive |
|---|---|---|---|
| zip, zip64 (store, deflate, LZMA), self-extracting zips | instant (central directory) | parallel per entry | yes, resumable inside an entry (deflate, store) |
| 7z (LZMA, LZMA2, Deflate, Copy, BCJ x86; plain or packed headers) | instant (header) | parallel per folder (solid block) | yes, folder granularity |
| tar (ustar, GNU, pax) | fast (headers only, data skipped) | parallel per entry | yes |
| gzip (single member) | instant | streaming | yes, resumable inside the stream |
| tar.gz | needs a full decompression pass (lazy, on first use) | single streaming pass | no |

Refused with a clear error: encryption (zip, 7z AES), other methods (bzip2, PPMd, BCJ2, …).

## Build

```bash
cmake -S . -B build && cmake --build build --config Release
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

- **GCC / Clang** (Linux, macOS, FreeBSD, MinGW, clang on Windows): the CRC-32 assembly
  (`PCLMULQDQ` on x86-64, ARMv8 `crc32` on arm64) is enabled automatically (`-DSLZ_ASM=OFF` to disable).
- **MSVC** (`cl.exe`): builds warning-free at `/W4`; uses the portable slice-by-8 CRC.
- **Sanitizers:** `-DSLZ_SANITIZE=ON` (ASan + UBSan).
- **WebAssembly** with [wasi-sdk](https://github.com/WebAssembly/wasi-sdk):

```bash
/opt/wasi-sdk/bin/clang --target=wasm32-wasip1 -O2 -Iinclude -Isrc src/*.c tools/slz.c -o slz.wasm
```

  WASI has no threads and no hole punching: extraction is single-threaded and `SLZ_DESTRUCTIVE`
  reports `SLZ_E_NOTSUP` without touching the archive.

## API

```c
#include "slz.h"

int err;
slz_archive *a = slz_open("big.zip", &err);           /* zip / 7z / tar / gz / tar.gz, auto-detected */
if (!a) { fprintf(stderr, "%s\n", slz_strerror(err)); return 1; }

for (size_t i = 0; i < slz_count(a); i++) {             /* flat listing */
    const slz_entry *e = slz_get(a, i);
    printf("%s %llu\n", e->name, (unsigned long long)e->usize);
}

slz_dir(a, "docs/images", on_child, user);              /* direct children of a directory */

slz_opts o = {0};
o.flags = SLZ_DESTRUCTIVE;                              /* optional: free the archive while extracting */
err = slz_extract(a, "out", &o);                        /* threads = CPU count by default */
slz_close(a);
```

Options: `SLZ_OVERWRITE` (replace existing files — links are removed, never followed),
`SLZ_DESTRUCTIVE`, `SLZ_NO_MTIME`; `threads`, `max_total` (cap on output bytes), `checkpoint`
(destructive interval, default 64 MiB), and a `progress` callback that can cancel.

Command line: `slz l <archive> [dir]`, `slz x <archive> <dest> [-o] [-d] [-j N] [-c MiB] [-n] [-q]`.

## Destructive extraction

Goal: extracting a 30 GB archive must not need 30 GB of free space on top of it.

1. The filesystem's **hole punching** releases the blocks of archive data that has already been
   extracted, without moving anything: `fallocate(PUNCH_HOLE)` (Linux), `F_PUNCHHOLE` (macOS/APFS),
   `fspacectl` (FreeBSD 14+), `FSCTL_SET_ZERO_DATA` on a sparse file (Windows/NTFS, ReFS).
2. Order at every checkpoint: **output files durable** (`fsync`, `syncfs` on Linux, `F_FULLFSYNC`
   for the journal on macOS) **→ journal record durable → punch**. A journal record is never ahead of
   the data, and nothing is freed before the record that makes it unnecessary.
3. The journal (`<archive>.slzstate`, a few KiB) holds two checksummed records written
   alternately, plus a snapshot of the index (the archive's own headers get punched).
4. **Resume** after a crash, power loss or cancel: open and extract again with `SLZ_DESTRUCTIVE`.
   Zip/gzip DEFLATE entries resume *inside* the entry, at the last block boundary (bit position +
   the 32 KiB window re-read from the partly written output file); stored data at any byte;
   7z at the start of the interrupted folder.
5. When done, the archive and the journal are deleted.

**Bound:** peak disk use ≤ max(archive, output) + about one checkpoint interval (zip, tar, gzip),
or + the packed size of one 7z folder (7z).

Before anything is changed, a probe on the same filesystem checks that hole punching works; on
FAT/exFAT and similar the call fails with `SLZ_E_NOTSUP` and the archive is untouched. The journal is
bound to the archive by a fingerprint of its index, size and file identity (inode / file index), so
a different file with the same name is never punched.

Measured (tests below): a 21 MB stored zip interrupted 19 times peaks at **+289 KB** over
max(archive, output) on NTFS and **+295 KB** on ext4.

## Security

Threat model: the archive is hostile; the destination directory may contain links planted by
someone else.

- **Paths**: one sanitizer for every format. Rejects absolute paths, `..`, NUL and control
  characters, and on Windows drive letters, `:` (alternate streams), reserved device names
  (`CON`, `COM1.txt`, …) and trailing dots/spaces. `\` is treated as a separator everywhere.
- **No link following, ever**: POSIX walks the destination with `openat(O_NOFOLLOW | O_DIRECTORY)`
  component by component (a planted symlink anywhere is refused) and creates files with
  `O_EXCL | O_NOFOLLOW`; Windows verifies every directory is not a reparse point and opens with
  `FILE_FLAG_OPEN_REPARSE_POINT`. Symlinks, hard links and devices in archives are skipped.
- **Zip bombs / overlaps**: entries are sorted by offset and each one's data must end before the
  next local header (overlapping or shared data is corrupt); output may never exceed the declared
  size; `max_total` caps everything; CRC-32 is verified for every zip, gzip and 7z entry.
- **Parsers**: every read is bounds-checked (7z uses a bounded reader that can only fail), counts are
  capped by the header size before allocating, 64-bit sizes are never truncated on 32-bit targets
  (WASM), metadata records are size-limited (pax 1 MiB, 7z header 256 MiB).
- **Decoders**: DEFLATE accepts exactly what zlib accepts; LZMA/LZMA2 check every distance against
  the produced output and fail on truncated input; buffers have explicit slack for wide copies.
- **Hygiene**: internal symbols are namespaced (`slz_i_*`), no global mutable state except the
  CRC table, sources are pure ASCII.

### Verification

| what | how | result |
|---|---|---|
| unit + end-to-end | 13 archive variants from Python's `zipfile`/`tarfile`/`gzip` and 8 from 7-Zip, 1 and 8 threads, byte-for-byte | pass (Windows MSVC + MinGW, Linux GCC) |
| hostile archives | path traversal, absolute/UNC/drive paths, device names, NUL names, symlink chains, planted symlinks, overlapping entries, encrypted 7z | refused, nothing written outside the destination |
| sanitizers | full suite under ASan + UBSan (`-fno-sanitize-recover`) | clean |
| DEFLATE differential fuzzing vs zlib | 16 000 streams (all levels/strategies/windows, flushes) + 32 000 mutations, random input chunking | identical output and identical accept/reject |
| LZMA/LZMA2 differential fuzzing vs liblzma | 1 200 streams (random lc/lp/pb/dict/mode) + 4 800 mutations | identical output, no crash |
| whole-pipeline fuzzing | 2 800 mutated archives from 11 seeds (zip deflate/store/LZMA, 7z LZMA/LZMA2/BCJ/Copy/Deflate, tar, tar.gz, gz) built from the public Canterbury corpus; open, listing, tree, extraction in-process under ASan + UBSan | clean; no link created, nothing outside the destination |
| interruption | destructive extraction cancelled at irregular points and resumed, every format | identical output, archive and journal removed |
| scale | zip64 with a 5 GiB member, cancelled 5× mid-entry and resumed | CRC-exact |
| WASM | wasm32-wasip1 build run under Node's WASI | zip and tar.gz byte-identical |

Bugs found and fixed by this process include: stale bits in the DEFLATE bit buffer after a stored
block, a stored block at the end of the input rejected with tiny reads, an LZMA2 chunk-end
normalization off by one, a pax record without `=` causing a length underflow, a 64 KiB stack buffer
overflowing the WASM stack, and a journal identity that did not include the file's identity, a corrupted AES method id, and the
wrong output stream chosen for 7z coder chains that cannot be decoded.

## Performance

Machine: 24 threads, Windows 11 + WSL2 Ubuntu 22.04, ext4 on a virtual disk, warm page cache,
GCC 11 `-O2`. Corpora: [Silesia](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) (12 files,
212 MB), "small" = 17 414 files / 193 MB copied from `/usr/include`, `/usr/share/doc`, `/usr/lib/python3`,
and a 500 000-entry zip. Archives made by Info-ZIP `zip -6`, GNU `tar` + gzip, and 7-Zip 16.02 defaults.
Competitors: Info-ZIP unzip 6.0, libarchive 3.6 (bsdtar), p7zip 16.02, GNU tar 1.34, pigz, Python 3.10.
Times are whole processes, best of 3. Reproduce with `tests/bench.py` and `tests/bench_inflate.c`.

#### DEFLATE decoder alone (in memory, Silesia compressed by zlib -6, MB/s of output)

| | slz | zlib 1.2.11 | libdeflate 1.10 |
|---|---|---|---|
| Silesia total | **725** | 461 | 976 |

libdeflate only works on whole buffers; slz streams (bounded memory, resumable) and is 1.6x zlib.

#### Listing (ms, whole process)

| archive | entries | slz | unzip -Z1 | bsdtar -t | 7z l | python zipfile |
|---|---|---|---|---|---|---|
| silesia.zip | 12 | 1 | 2 | 2 | 3 | 25 |
| small.zip | 17414 | 5 | 11 | 71 | 53 | 89 |
| 500k entries | 500000 | 75 | 200 | 1338 | 1602 | 1722 |

#### Extraction, zip (s)

| archive | files / MB | slz (-j 24) | slz -j 1 | unzip | bsdtar | 7z x | python zipfile |
|---|---|---|---|---|---|---|---|
| silesia.zip | 12 / 212 | 0.12 | 0.38 | 0.95 | 0.66 | 0.79 | 0.74 |
| small.zip | 17414 / 193 | 0.54 | 0.88 | 1.37 | 1.09 | 1.37 | 1.45 |

#### Extraction, tar.gz (s)

| archive | slz | GNU tar -xzf | pigz -d \| tar -x | bsdtar |
|---|---|---|---|---|
| silesia.tar.gz | 0.38 | 0.90 | 0.67 | 0.56 |
| small.tar.gz | 0.60 | 0.91 | 0.71 | 0.88 |

#### 7z (s; listing in ms)

| archive | slz list | 7z l | slz x | slz x -j 1 | 7z x | bsdtar -x |
|---|---|---|---|---|---|---|
| silesia.7z | 1 | 3 | 2.66 | 2.73 | 2.02 | 2.34 |
| silesia-nonsolid.7z | 2 | 3 | 0.84 | 2.73 | 2.00 | 2.23 |
| small.7z | 18 | 50 | 2.16 | 2.16 | 1.97 | 1.96 |
| small-nonsolid.7z | 21 | 53 | 0.47 | 2.92 | 2.65 | 2.56 |

Notes:
- zip and tar.gz: slz is the fastest tool measured in every row; listing 500 000 entries takes 75 ms
  (2.7x faster than unzip, ~18x faster than libarchive and 7-Zip).
- 7z: non-solid archives (one folder per file) are extracted in parallel and are 2.4-5.6x faster
  than 7-Zip. Solid archives are one LZMA stream that no tool can parallelize; there the LZMA decoder
  is ~25% slower than 7-Zip's hand-optimized one (see Roadmap).
- Large single-archive test (1.5 GB zip of low-redundancy data, 2 003 entries, 1.96 GB out):
  slz 1.58 s, bsdtar 6.15 s, unzip 8.02 s. In destructive mode the same extraction takes 18.4 s
  because every checkpoint makes the output durable (fsync) before freeing archive space; the peak
  footprint was +28.5 MB over max(archive, output) with a 32 MiB checkpoint interval.

## Roadmap

- LZMA hot loop with register-resident range coder state (as already done for DEFLATE).
- Destructive 7z checkpoints inside a solid folder (persist the LZMA model + window).
- Overlap fsync with decoding in destructive mode (async durability, one extra interval of footprint).
- `.xz` / `tar.xz` (LZMA2 is already there), zstd.

## Layout

| file | lines | content |
|---|---|---|
| `include/slz.h` | 84 | public API |
| `src/inflate.c` | 415 | DEFLATE decoder |
| `src/lzma.c` | 466 | LZMA, LZMA2, BCJ x86 |
| `src/sevenz.c` | 781 | 7z headers and folder decoding |
| `src/formats.c` | 930 | zip, tar, gzip, tar.gz, index, tree, path sanitizer |
| `src/extract.c` | 594 | parallel, streaming and destructive extraction |
| `src/internal.h` | 264 | internal interfaces, documented structures |
| `src/os.c` | 481 | the only platform-specific file |
| `src/crc32.c`, `src/crc32_*.S` | 201 | CRC-32: slice-by-8, PCLMULQDQ, ARMv8 |
| `tools/slz.c` | 99 | command line tool |
| `tests/` | 1216 | unit, end-to-end, fuzzers, benchmarks |

## License

MIT — see [LICENSE](LICENSE). Created by Seregon.
