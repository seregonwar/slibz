# Limits and roadmap

[slz](../README.md) > [docs](README.md) > Limits and roadmap

Not supported yet:

- encryption (zip ZipCrypto/AES, 7z AES), which needs a password API and crypto primitives;
- bzip2, xz / tar.xz, zstd, PPMd, BCJ2;
- in-place checkpoints *inside* a solid 7z folder (today a folder restarts on resume, so a solid 7z
  needs extra space equal to its packed size).

Planned:

1. Parallel decoding of solid 7z blocks: LZMA2 chunk headers give each chunk's sizes without decoding,
   so the segments that start with a dictionary reset can be decoded concurrently and written at
   known offsets (per-file CRCs combined with `crc32_combine`). This is how 7-Zip 26 beats slz on
   solid archives today.
2. A faster single-thread LZMA decoder (today ~61% of xz 5.8.4).
3. `.xz` and `tar.xz`, reusing the LZMA2 decoder.
4. zstd and bzip2 decoders, then encryption (zip AES, 7z AES).
5. 7z checkpoints inside solid folders (persist the LZMA model and window).
6. Overlapping fsync with decoding in in-place mode.
