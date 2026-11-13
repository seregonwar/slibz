# Limits and roadmap

[slz](../README.md) > [docs](README.md) > Limits and roadmap

Not supported yet:

- encryption (zip ZipCrypto/AES, 7z AES), which needs a password API and crypto primitives;
- bzip2, xz / tar.xz, zstd, PPMd, BCJ2;
- in-place checkpoints *inside* a solid 7z folder (today a folder restarts on resume, so a solid 7z
  needs extra space equal to its packed size).

Planned:

1. Overlapping the in-order write of a solid 7z folder's decoded segments with the decoding of the
   next ones (today a batch is decoded, then written).
2. A faster single-thread LZMA decoder.
3. `.xz` and `tar.xz`, reusing the LZMA2 decoder.
4. zstd and bzip2 decoders, then encryption (zip AES, 7z AES).
5. 7z checkpoints inside solid folders (persist the LZMA model and window).
6. Overlapping fsync with decoding in in-place mode.
