# In-place (destructive) extraction

[slz](../README.md) > [docs](README.md) > In-place (destructive) extraction

With `SLZ_DESTRUCTIVE` (CLI `-d`) the archive's blocks are released as soon as their content is safely
on disk, so extraction needs no free space beyond the size of the output.

**Mechanism.** Filesystem hole punching deallocates already-extracted ranges of the archive in place:
`fallocate(PUNCH_HOLE)` on Linux, `F_PUNCHHOLE` on macOS/APFS, `fspacectl` on FreeBSD 14+, and
`FSCTL_SET_ZERO_DATA` on a sparse file on Windows (NTFS, ReFS). Before anything is modified, a probe on
the same filesystem checks that this works. If it does not (FAT, exFAT, WASI), the call returns
`SLZ_E_NOTSUP` and the archive is untouched.

**Order at every checkpoint.** Nothing is freed before it is provably unnecessary:

1. output files are made durable (`fsync`);
2. the directories that received new entries are made durable (`fsync` of each directory, or
   `FlushFileBuffers` on its handle on Windows), so a crash cannot lose a file's name;
3. the journal record is written and made durable (`F_FULLFSYNC` on macOS);
4. only then is the consumed archive range punched.

**Journal.** `<archive>.slzstate`, a few KiB: two checksummed records written alternately, plus a
snapshot of the archive index (the archive's own headers get punched). It is opened without
following links, must be a regular file, and is locked exclusively (`flock` / share mode 0): a second
extraction of the same archive is refused. Its directory entry is made durable before the first punch.
It is bound to the archive by a fingerprint of the index, the size and the file identity
(inode / volume + file index), so a different file with the same name is never punched.

**Resume.** After a crash, power loss, kill or cancel, run the same extraction again. Zip and gzip
DEFLATE entries resume *inside* the entry, at the last block boundary (bit position plus the 32 KiB
window re-read from the partly written output); stored data resumes at any byte; 7z resumes at the
start of the interrupted folder. When everything is done, the archive and then the journal are
deleted. A run killed exactly between these two steps leaves only the journal, which is harmless.

**Bound.** Peak disk use is at most max(archive, output) plus about one checkpoint interval for zip,
tar and gzip, or plus the packed size of one folder for 7z. Measured examples are in
[Verification](verification.md).
