# Security model

[slz](../README.md) > [docs](README.md) > Security model

Threat model: the archive is hostile, and the destination directory may contain links planted by
someone else. The destination root itself is chosen by the caller and is trusted, and may be a link
(macOS `/tmp` is one). Everything below it is not trusted.

- **Paths.** A single sanitizer is shared by every format. It rejects absolute paths, `..`, NUL and
  control characters, and on Windows drive letters, `:` (alternate data streams), reserved device
  names (`CON`, `COM1.txt`, ...) and trailing dots or spaces. `\` is a separator everywhere.
- **No link following.** On POSIX the destination is walked with `openat(O_NOFOLLOW | O_DIRECTORY)`
  one component at a time, and files are created with `O_EXCL | O_NOFOLLOW`. On Windows every
  directory below the root is opened as a handle that refuses reparse points and is held *without*
  `FILE_SHARE_DELETE`, so it cannot be renamed or replaced by a junction between check and use. Files
  are opened with `FILE_FLAG_OPEN_REPARSE_POINT` and verified on the handle. Symlinks, hard links and
  devices stored in archives are skipped.
- **Overlaps and bombs.** In offset order, zip/tar/gzip entry *i* owns exactly
  `[offset_i, offset_i+1)`. A local header or compressed data that crosses that limit is refused
  before decoding, so entries can never share bytes. Output may not exceed the declared size,
  `max_total` caps everything, and CRC-32 is verified for every zip, gzip and 7z entry.
- **Parsers.** Every read is bounds-checked; 7z goes through a bounded reader that can only fail.
  Counts are capped by the header size before allocating. 64-bit sizes are never truncated on 32-bit
  targets (WASM). Metadata is size-limited (pax 1 MiB, 7z header 256 MiB).
- **Decoders.** DEFLATE accepts exactly what zlib accepts. LZMA/LZMA2 check every distance against the
  produced output and fail on truncated input. Buffers have explicit slack for wide copies.
- **Concurrency and hygiene.** Shared counters and the first error use atomic operations (add, CAS).
  Internal symbols are namespaced (`slz_i_*`). There is no global mutable state except the CRC table.
  Sources are pure ASCII.
