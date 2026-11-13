# slz

[![ci](https://github.com/seregonwar/slibz/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/seregonwar/slibz/actions/workflows/ci.yml)
[![bench](https://github.com/seregonwar/slibz/actions/workflows/bench.yml/badge.svg?branch=main)](https://github.com/seregonwar/slibz/actions/workflows/bench.yml)
[![DEFLATE decode](https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2Fseregonwar%2Fslibz%2Fbench-data%2Fbadges%2Fdeflate.json)](docs/benchmarks.md#continuous-benchmarks)
[![LZMA2 decode](https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2Fseregonwar%2Fslibz%2Fbench-data%2Fbadges%2Flzma2.json)](docs/benchmarks.md#continuous-benchmarks)
[![list 500k zip entries](https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2Fseregonwar%2Fslibz%2Fbench-data%2Fbadges%2Flist.json)](docs/benchmarks.md#continuous-benchmarks)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/seregonwar/slibz?style=flat&logo=github)](https://github.com/seregonwar/slibz/stargazers)

**Fast, hardened archive extraction in C99 + assembly: zip/zip64, 7z, tar, gzip, tar.gz.**
Lists huge archives in milliseconds, extracts at SSD speed, and can extract *in place*, freeing the
archive's space as it goes, without needing extra free disk.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/seregonwar/slibz/bench-data/svg/hero-dark.svg">
  <img alt="Wall time of slz, unzip, bsdtar, 7-Zip, GNU tar, pigz and Python listing and extracting zip, tar.gz and 7z archives, and in-memory DEFLATE and LZMA2 decoding speed of slz, zlib, zlib-ng, libdeflate and xz, measured by CI on the latest commit of main" src="https://raw.githubusercontent.com/seregonwar/slibz/bench-data/svg/hero-light.svg">
</picture>

<sub>Redrawn by CI after every push to `main`, on a GitHub-hosted runner: [how it is measured](docs/benchmarks.md#continuous-benchmarks),
[detailed tables](docs/benchmarks.md#detailed-results).</sub>

Created by **Seregon**. Copyright (c) 2026 Seregon. Released under the [MIT License](LICENSE).

[Windows, macOS, Linux, FreeBSD, WebAssembly (WASI)](docs/platforms.md). No dependencies.

| | |
|---|---|
| **Listing** | 500 000 zip entries in 75 ms (one read of the central directory, parsed in place) |
| **Extraction** | parallel, in on-disk order; fastest measured on zip, tar.gz and non-solid 7z vs the latest 7-Zip and libarchive (see [Benchmarks](docs/benchmarks.md)) |
| **In-place extraction** | a 30 GB archive extracts into ~30 GB of output: peak = max(archive, output) + one checkpoint (see [In-place extraction](docs/in-place-extraction.md)) |
| **Crash safety** | journaled; survives kill -9 and power loss at any point, resumes inside an entry (see [Verification](docs/verification.md)) |
| **Hardening** | bounded parsers, no link following, overlap/bomb checks; differential fuzzing vs zlib and xz (see [Security model](docs/security-model.md)) |
| **Size** | ~4 400 lines of library C and ~130 lines of assembly (see [Project layout](docs/project-layout.md)) |

## Contents

1. [Quick start](#quick-start)
2. [Formats](#formats)
3. [Performance](#performance)
4. [Documentation](#documentation)
5. [Star history](#star-history)
6. [License](#license)

## Quick start

```bash
cmake -S . -B build && cmake --build build --config Release
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

Command line:

```bash
slz l archive.7z                 # list everything
slz l archive.zip docs/images    # direct children of a directory
slz x archive.zip out            # extract (all CPU cores)
slz x big.zip out -d             # in-place: frees big.zip while extracting, resumable
```

Library:

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

Every command-line option and the `slz_opts` fields are described in [docs/usage.md](docs/usage.md);
installing and `find_package(slz)` in [docs/building.md](docs/building.md).

## Formats

| format | methods | listing | extraction | in-place |
|---|---|---|---|---|
| zip, zip64, self-extracting zip | store, deflate, LZMA | instant (central directory) | parallel per entry | yes, resumes inside an entry |
| 7z | LZMA, LZMA2, Deflate, Copy, BCJ x86; plain or packed headers | instant (header) | parallel per folder (solid block) | yes, folder granularity |
| tar | ustar, GNU, pax | fast (headers only, data skipped) | parallel per entry | yes |
| gzip | deflate, single member | instant | streaming | yes, resumes inside the stream |
| tar.gz | deflate | on first use (needs a full pass) | single streaming pass | no |

Refused with a clear error: encrypted entries (zip, 7z AES), other methods (bzip2, PPMd, BCJ2, zstd...).

## Performance

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/seregonwar/slibz/bench-data/svg/trend-dark.svg">
  <img alt="In-memory DEFLATE and LZMA2 decoding speed of slz, zlib, zlib-ng, libdeflate and xz over the last runs on main" src="https://raw.githubusercontent.com/seregonwar/slibz/bench-data/svg/trend-light.svg">
</picture>

The banner at the top shows the newest run on `main` and this chart follows the decoders over the last
runs; CI redraws both. Every push that changes code is benchmarked on a GitHub-hosted runner, in one job,
against zlib, zlib-ng, libdeflate, xz, libarchive (bsdtar) and pigz built from pinned sources, the official
7-Zip build, and the runner's unzip, GNU tar and Python. On solid 7z archives 7-Zip is faster than slz:
parallel decoding of solid blocks is first on the [roadmap](docs/roadmap.md).

[docs/benchmarks.md](docs/benchmarks.md) explains how runner noise and regressions are handled and how to run
the same benchmark, and has the full tables measured on a 24-thread desktop.

## Documentation

- [Usage](docs/usage.md): every command-line option and the C library API
- [In-place (destructive) extraction](docs/in-place-extraction.md): hole punching, checkpoint order, journal, resume, disk bound
- [Security model](docs/security-model.md): threat model, paths, links, overlaps and bombs, parsers, decoders
- [Verification](docs/verification.md): round trips, hostile archives, sanitizers, fuzzing, forced kills, injected I/O failures, CI
- [Benchmarks](docs/benchmarks.md): continuous benchmarks on every push, full performance tables, methodology, how to reproduce
- [Platforms](docs/platforms.md): what is tested where, assembly CRC-32, the WebAssembly build
- [Building and integration](docs/building.md): CMake build and tests, install, `find_package`, vendoring, options
- [Project layout](docs/project-layout.md): source files and how the code is shared
- [Limits and roadmap](docs/roadmap.md): what is not supported yet and what is planned

## Star history

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=seregonwar/slibz&type=Date&theme=dark">
  <img alt="Star history of seregonwar/slibz" src="https://api.star-history.com/svg?repos=seregonwar/slibz&type=Date">
</picture>

<sub>Chart by [star-history.com](https://www.star-history.com/#seregonwar/slibz&Date).</sub>

## License

MIT, see [LICENSE](LICENSE). Created by Seregon.
