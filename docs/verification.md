# Verification

[slz](../README.md) > [docs](README.md) > Verification

| what | how | result |
|---|---|---|
| round trips | 13 archive variants from Python's `zipfile`/`tarfile`/`gzip` and 8 from 7-Zip (every coder configuration), 1 and 8 threads, compared byte for byte | pass on Windows (MSVC, MinGW), Linux (GCC) |
| hostile archives | traversal, absolute/UNC/drive paths, device names, NUL names, symlink chains, planted symlinks and junctions, four kinds of data overlap, encrypted 7z | refused; nothing written outside the destination |
| sanitizers | whole suite under ASan + UBSan, `-fno-sanitize-recover` | clean |
| DEFLATE differential fuzzing | vs zlib: 16 000 streams (all levels/strategies/windows/flushes) + 32 000 mutations, random input chunking | identical output and identical accept/reject |
| LZMA differential fuzzing | vs xz 5.8.4: random lc/lp/pb/dict/mode streams + mutations | identical output, no crash |
| whole-pipeline fuzzing | 4 800 mutated archives from 11 seeds built from the public Canterbury corpus (zip deflate/store/LZMA, 7z LZMA/LZMA2/BCJ/Copy/Deflate, tar, tar.gz, gz); open, list, tree, extract in-process under ASan + UBSan | clean; no link created, nothing outside the destination |
| forced kills | in-place extraction of zip, tar and 7z killed with SIGKILL / TerminateProcess at random moments (30 times each), then resumed | identical output; archive and journal removed |
| injected I/O failures | build with `SLZ_FAULT_INJECTION`; every read/write/sync/truncate/punch of an in-place run fails in turn (158 points on zip and 7z), then a clean resume | identical output every time |
| disk footprint | 21 MB stored zip interrupted 19 times: +289 KB (NTFS) / +295 KB (ext4) over max(archive, output). 1.5 GB zip, 32 MiB checkpoints: +28.5 MB | within the bound |
| scale | zip64 with a 5 GiB member, cancelled 5 times mid-entry and resumed | CRC-exact |
| WASM | wasm32-wasip1 build run under Node's WASI | zip and tar.gz byte-identical |
| integration | `cmake --install`, then a separate project with `find_package(slz)` | builds and runs |

Bugs found and fixed by this process: stale bits in the DEFLATE bit buffer after a stored block; a
stored block at the end of input rejected with tiny reads; an LZMA2 chunk end off by one byte; a pax
record without `=` causing a length underflow; a 64 KiB stack buffer overflowing the WASM stack; a
journal identity that did not include the file's identity; a corrupted 7z AES method id; the wrong
output stream chosen for undecodable 7z coder chains; and an undefined sequence point in the LZMA
output macro.

The CI workflow ([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)) builds with `SLZ_FAULT_INJECTION` and runs the
`ctest` suite on Linux x86-64 (GCC, Clang), Linux arm64 (GCC), macOS arm64 (Clang), Windows (MSVC,
clang-cl) and FreeBSD 14.2. Only the Linux and macOS jobs build with ASan + UBSan. The WASI job
builds for wasm32-wasip1 and runs one round-trip smoke test under Node (a small zip, extracted and
compared), not the suite.
