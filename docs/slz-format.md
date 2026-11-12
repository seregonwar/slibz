# The `.slz` archive format, version 1

Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT.

## Goals

1. **Ratio close to a solid archive.** File contents are concatenated into one logical *stream*
   (small files packed together), so the compressor sees context across files, as in 7z solid mode.
2. **Parallel decompression.** The stream is cut into fixed-size *blocks*, and each block is
   compressed independently, so every core can decode a block. Solid 7z decodes on a single core.
3. **Instant listing and random access.** One compressed index at the end of the file describes
   every block and every entry. Extracting one file reads only the blocks it overlaps.
4. **In-place extraction.** Blocks are stored in stream order, so the archive can be freed block by
   block while it is extracted. Extra disk use is at most one block.
5. **Integrity.** Every block carries an xxHash64 of its uncompressed bytes. The index and the
   trailer carry their own hashes.
6. **Deduplication.** Identical files are stored once; later copies point at the same stream range.

## Layout

All integers are little-endian. Offsets are absolute file offsets.

```
+----------------+  0
| header (32 B)  |
+----------------+  32
| block 0        |  compressed bytes, back to back, in stream order
| block 1        |
| ...            |
+----------------+  index_offset
| index          |  compressed with index_codec
+----------------+  file size - 64
| trailer (64 B) |
+----------------+  file size
```

### Header (32 bytes, offset 0)

| offset | size | field | value |
|---|---|---|---|
| 0 | 4 | magic | `53 4C 5A 1A` ("SLZ" + SUB, which stops `type`/`cat` from printing binary) |
| 4 | 2 | version_major | 1 (incompatible changes) |
| 6 | 2 | version_minor | 0 (compatible additions) |
| 8 | 4 | required_features | bits a reader must understand, otherwise it refuses the archive |
| 12 | 4 | optional_features | bits a reader may ignore |
| 16 | 12 | reserved | 0 |
| 28 | 4 | header_hash | low 32 bits of xxHash64(bytes 0..27, seed 0) |

Feature bits in version 1: no required bits are defined, so a v1 reader refuses any non-zero
`required_features`. Optional bit 0 (`DEDUP`) means that some entries share stream ranges.

### Blocks

Block *b* occupies `[offset_b, offset_b + csize_b)` in the file and decodes to `usize_b` bytes,
which are the stream bytes `[S_b, S_b + usize_b)`, where `S_b = usize_0 + ... + usize_(b-1)`.

| codec | id | meaning |
|---|---|---|
| store | 0 | `csize = usize`, raw bytes |
| lz4 | 1 | one LZ4 *block* (raw block format, no frame) |
| zstd | 2 | one zstd frame without checksum (the block hash replaces it) |

The writer stores a block raw when compression does not shrink it, or when the incompressibility
test (known compressed-format magic bytes, or byte entropy above 7.2 bits per byte) says it would
not.

### Index (at `index_offset`, compressed with `index_codec`)

The uncompressed index is:

```
u32 nblocks
u32 nentries
u64 names_size
u32 block_size          nominal block size (informative)
u32 reserved            0
block  blocks[nblocks]    32 bytes each
entry  entries[nentries]  48 bytes each
u8     names[names_size]
```

Block record (32 bytes):

| offset | size | field |
|---|---|---|
| 0 | 8 | offset (file offset of the compressed block) |
| 8 | 4 | csize |
| 12 | 4 | usize (1 to 256 MiB) |
| 16 | 1 | codec |
| 17 | 7 | reserved, 0 |
| 24 | 8 | hash: xxHash64 of the uncompressed block, seed 0 |

Entry record (48 bytes):

| offset | size | field |
|---|---|---|
| 0 | 8 | stream_offset (files only) |
| 8 | 8 | size (bytes of content; 0 for directories) |
| 16 | 8 | mtime (Unix seconds, signed) |
| 24 | 4 | name_offset into `names` |
| 28 | 4 | name_length (bytes, excluding the NUL) |
| 32 | 4 | mode (Unix permission bits, `0` when unknown) |
| 36 | 1 | type: 0 file, 1 directory |
| 37 | 1 | flags: bit 0 = this file shares the stream range of an earlier one (dedup) |
| 38 | 2 | reserved, 0 |
| 40 | 8 | content_hash: xxHash64 of the file's content (0 for directories) |

Names are UTF-8 relative paths with `/` separators, each followed by a NUL byte. They go through
the same sanitizer as every other format at extraction time, so the format itself carries no trust.

### Trailer (64 bytes, at file size - 64)

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `53 4C 5A 49` ("SLZI") |
| 4 | 4 | index_codec (0 store, 2 zstd) |
| 8 | 8 | index_offset |
| 16 | 8 | index_csize |
| 24 | 8 | index_usize |
| 32 | 8 | index_hash: xxHash64 of the uncompressed index |
| 40 | 4 | required_features (copy of the header's) |
| 44 | 12 | reserved, 0 |
| 56 | 8 | trailer_hash: xxHash64 of bytes 0..55 |

## Validity rules (a reader must refuse anything else)

- Header and trailer magic, hashes and `version_major = 1` match, and `required_features = 0`.
- `32 <= index_offset`, `index_offset + index_csize = file size - 64`, `index_usize <= 1 GiB`, and
  `index_usize = 24 + 32*nblocks + 48*nentries + names_size` exactly.
- Blocks are in increasing offset order and do not overlap: `offset_0 >= 32`,
  `offset_b + csize_b <= offset_(b+1)`, and the last block ends at or before `index_offset`;
  `1 <= usize_b <= 256 MiB`; a stored block has `csize = usize`.
- For every file: `stream_offset + size <= total stream size`. A directory has `size = 0`.
- `name_offset + name_length < names_size`, the byte at `name_offset + name_length` is NUL, and the
  name contains no other NUL.
- Every decoded block must have exactly `usize` bytes and match its hash.

## Writer behaviour (slz 0.2)

- Inputs are walked recursively. Links and special files are skipped, never followed.
- Files are ordered by extension and then by name, so similar data shares blocks (better ratio).
- Deduplication only hashes files whose size equals another file's size, and confirms every hash
  match byte for byte before sharing a range.
- Blocks are compressed in parallel and written in order. The archive is written to `<name>.part`
  and renamed only when complete, so a crash never leaves a truncated archive under the final name.
- Defaults: zstd level 3, 8 MiB blocks. With zstd, blocks of 128 KiB or more use long-distance
  matching and a window of at least the block size.

## Design notes

The block layer (independent blocks with a block index, a raw-store path for incompressible blocks
chosen by a magic-byte and fixed-point entropy test, zstd without frame checksum or redundant
metadata and with long-distance matching on large blocks) and the container conventions (a 0x1A
magic, required versus optional feature bits) come from Seregon's earlier block codec and container
libraries, adapted here.

What differs from per-file compression: file contents form one solid stream cut into blocks, which
gives most of the ratio of a solid archive while every block still decodes in parallel. Integrity
uses xxHash64 per block rather than a SHA-256 tree, because SHA-256 at ~0.5 GB/s per core would
limit decompression at SSD speed. Threading, in-place extraction and resume come from the slz
extraction engine, so the format itself needs no journal or cache.
