# Platforms

[slz](../README.md) > [docs](README.md) > Platforms

| platform | tested here | notes |
|---|---|---|
| Windows x64 | MSVC `/W4` (warning-free), MinGW GCC, clang-cl; full suite | NTFS sparse files for in-place extraction |
| Linux x86-64 | GCC 11 with ASan + UBSan; full suite, fuzzers, kill and fault tests | ext4, XFS, Btrfs support hole punching |
| macOS, FreeBSD, Linux arm64 | assembled and covered by CI (not run on this machine) | APFS `F_PUNCHHOLE`; FreeBSD 14+ `fspacectl` |
| WebAssembly (WASI) | built with wasi-sdk 34, run under Node 24 | single-threaded; no in-place mode (no hole punching in WASI) |

CRC-32 uses assembly on GCC/Clang: `PCLMULQDQ` on x86-64, the ARMv8 `crc32` instructions on arm64. MSVC
uses the portable slice-by-8 C version.

WASM build:

```bash
/opt/wasi-sdk/bin/clang --target=wasm32-wasip1 -O2 -Iinclude -Isrc src/*.c tools/slz.c -o slz.wasm
```
