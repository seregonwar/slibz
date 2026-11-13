# Project layout

[slz](../README.md) > [docs](README.md) > Project layout

| file | lines | content |
|---|---|---|
| `include/slz.h` | 84 | public API |
| `src/internal.h` | 286 | internal interfaces and documented structures |
| `src/inflate.c` | 415 | DEFLATE decoder |
| `src/lzma.c` | 567 | LZMA, LZMA2, BCJ x86 |
| `src/sevenz.c` | 781 | 7z headers and folder decoding |
| `src/formats.c` | 930 | zip, tar, gzip, tar.gz, index, tree, path sanitizer |
| `src/extract.c` | 602 | parallel, streaming and in-place extraction |
| `src/os.c` | 692 | the only platform-specific file |
| `src/crc32.c`, `src/crc32_*.S` | 201 | CRC-32: slice-by-8, PCLMULQDQ, ARMv8 |
| `tools/slz.c` | 99 | command line tool |
| `tests/` | 1480 | unit, end-to-end, fuzzers, benchmarks |

Code is shared rather than duplicated: one path sanitizer, one tar parser (driven by the indexer for
tar and by the decompressor for tar.gz), one range reader behind every compressed stream, and one
extraction engine whose unit of work is an entry or a 7z folder.
