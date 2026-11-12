/* slz - platform layer: positional I/O, durable sync, hole punching, link-safe file creation,
 * directory durability, journal locking, threads and atomics.
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * Windows (Win32), POSIX (Linux, macOS, FreeBSD) and WASI (single-threaded). */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "internal.h"

/* Test builds only (-DSLZ_FAULT_INJECTION): SLZ_FAULT_AT=N makes the N-th I/O operation (read,
 * write, sync, truncate, punch) fail, to exercise every error and resume path. */
#ifdef SLZ_FAULT_INJECTION
#include <stdio.h>
static volatile int64_t fault_left, fault_init;
static int fault(void) {
    if (os_cas(&fault_init, 0, 1)) {
        const char *e = getenv("SLZ_FAULT_AT");
        os_add(&fault_left, e ? (int64_t)atoll(e) : 0);
    }
    if (os_add(&fault_left, 0) <= 0 || os_add(&fault_left, -1) != 0) return 0;
    fputs("[slz fault injected]\n", stderr); /* lets the test know the fault point was reached */
    return 1;
}
#define FAULT(ret)                                                                                                 \
    do {                                                                                                           \
        if (fault()) return ret;                                                                                   \
    } while (0)
#else
#define FAULT(ret) ((void)0)
#endif

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>

static wchar_t *wide(const char *s, size_t extra) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc(((size_t)n + extra) * sizeof(wchar_t)) : NULL;
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        w = NULL;
    }
    return w;
}
/* absolute \\?\ path (no MAX_PATH limit), UNC aware */
static wchar_t *wpath(const char *s) {
    wchar_t *w = wide(s, 0), *o = NULL, *f = NULL;
    DWORD n;
    if (w && (n = GetFullPathNameW(w, 0, NULL, NULL)) && (o = (wchar_t *)malloc(n * sizeof *o)) &&
        GetFullPathNameW(w, n, o, NULL)) {
        /* C:\x -> \\?\C:\x ; \\srv\share -> \\?\UNC\srv\share ; \\?\... unchanged */
        int unc = !wcsncmp(o, L"\\\\", 2) && wcsncmp(o, L"\\\\?\\", 4);
        const wchar_t *pre = !wcsncmp(o, L"\\\\?\\", 4) ? L"" : unc ? L"\\\\?\\UNC" : L"\\\\?\\";
        const wchar_t *rest = unc ? o + 1 : o;
        size_t pl = wcslen(pre), rl = wcslen(rest);
        if ((f = (wchar_t *)malloc((pl + rl + 1) * sizeof *f))) {
            memcpy(f, pre, pl * sizeof *f);
            memcpy(f + pl, rest, (rl + 1) * sizeof *f);
        }
    }
    free(w);
    free(o);
    return f;
}
static int werr(void) {
    DWORD e = GetLastError();
    return e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS      ? SLZ_E_EXISTS
           : e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? SLZ_E_OPEN
           : e == ERROR_DISK_FULL                                   ? SLZ_E_LIMIT
                                                                    : SLZ_E_IO;
}
int os_open(const char *path, int rw, int create, os_fd *fd) {
    wchar_t *w = wpath(path);
    HANDLE h;
    if (!w) return SLZ_E_OPEN;
    h = CreateFileW(w, GENERIC_READ | (rw ? GENERIC_WRITE : 0),
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return werr();
    *fd = (os_fd)h;
    return 0;
}
int64_t os_size(os_fd fd) {
    LARGE_INTEGER s;
    return GetFileSizeEx((HANDLE)fd, &s) ? s.QuadPart : -1;
}
int64_t os_pread(os_fd fd, void *b, size_t n, uint64_t off) {
    size_t t = 0;
    FAULT(SLZ_E_IO);
    while (t < n) {
        OVERLAPPED o;
        DWORD got = 0, want = (DWORD)MIN(n - t, (size_t)1 << 30);
        memset(&o, 0, sizeof o);
        o.Offset = (DWORD)(off + t);
        o.OffsetHigh = (DWORD)((off + t) >> 32);
        if (!ReadFile((HANDLE)fd, (char *)b + t, want, &got, &o)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return SLZ_E_IO;
        }
        if (!got) break;
        t += got;
    }
    return (int64_t)t;
}
int os_pwrite(os_fd fd, const void *b, size_t n, uint64_t off) {
    size_t t = 0;
    FAULT(SLZ_E_IO);
    while (t < n) {
        OVERLAPPED o;
        DWORD put = 0, want = (DWORD)MIN(n - t, (size_t)1 << 30);
        memset(&o, 0, sizeof o);
        o.Offset = (DWORD)(off + t);
        o.OffsetHigh = (DWORD)((off + t) >> 32);
        if (!WriteFile((HANDLE)fd, (const char *)b + t, want, &put, &o) || !put) return werr();
        t += put;
    }
    return 0;
}
int os_sync(os_fd fd, int full) {
    (void)full;
    FAULT(SLZ_E_IO);
    return FlushFileBuffers((HANDLE)fd) ? 0 : SLZ_E_IO;
}
int os_trunc(os_fd fd, uint64_t n) {
    FILE_END_OF_FILE_INFO i;
    FAULT(SLZ_E_IO);
    i.EndOfFile.QuadPart = (LONGLONG)n;
    return SetFileInformationByHandle((HANDLE)fd, FileEndOfFileInfo, &i, sizeof i) ? 0 : SLZ_E_IO;
}
int os_punch(os_fd fd, uint64_t off, uint64_t len) {
    FILE_ZERO_DATA_INFORMATION z;
    DWORD r;
    FAULT(SLZ_E_IO);
    if (!DeviceIoControl((HANDLE)fd, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &r, NULL)) return SLZ_E_NOTSUP;
    z.FileOffset.QuadPart = (LONGLONG)off;
    z.BeyondFinalZero.QuadPart = (LONGLONG)(off + len);
    return DeviceIoControl((HANDLE)fd, FSCTL_SET_ZERO_DATA, &z, sizeof z, NULL, 0, &r, NULL) ? 0
                                                                                             : SLZ_E_NOTSUP;
}
int os_mtime(os_fd fd, int64_t t) {
    ULARGE_INTEGER u;
    FILETIME ft;
    if (t < -11644473600LL) t = 0;
    u.QuadPart = (ULONGLONG)(t + 11644473600LL) * 10000000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return SetFileTime((HANDLE)fd, NULL, NULL, &ft) ? 0 : SLZ_E_IO;
}
void os_close(os_fd fd) {
    CloseHandle((HANDLE)fd);
}
int os_unlink(const char *path) {
    wchar_t *w = wpath(path);
    int r = w && DeleteFileW(w) ? 0 : SLZ_E_IO;
    free(w);
    return r;
}
uint64_t os_fileid(os_fd fd) {
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle((HANDLE)fd, &i)) return 0;
    return ((uint64_t)i.nFileIndexHigh << 32 | i.nFileIndexLow) ^ (uint64_t)i.dwVolumeSerialNumber << 40;
}
int os_exists(const char *path) {
    wchar_t *w = wpath(path);
    int r = w && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
    free(w);
    return r;
}
uint64_t os_allocated(const char *path) {
    wchar_t *w = wpath(path);
    DWORD hi = 0, lo = w ? GetCompressedFileSizeW(w, &hi) : INVALID_FILE_SIZE;
    free(w);
    return lo == INVALID_FILE_SIZE && GetLastError() != NO_ERROR ? 0 : ((uint64_t)hi << 32 | lo);
}
/* open a directory without following a reparse point and without letting anyone rename or delete
 * it while the handle is held; verified on the handle itself */
static HANDLE open_dir(const wchar_t *w, int nofollow) {
    BY_HANDLE_FILE_INFORMATION bi;
    HANDLE h =
        CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | (nofollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0), NULL);
    if (h == INVALID_HANDLE_VALUE) return h;
    if (!GetFileInformationByHandle(h, &bi) || !(bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (nofollow && (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(h);
        SetLastError(ERROR_CANT_ACCESS_FILE);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}
static void drop_chain(os_dst *d) {
    while (d->nchain)
        CloseHandle((HANDLE)d->chain[--d->nchain]);
    d->cfd = OS_BAD;
    d->cdn = 0;
}
static void mark_dirty(os_dst *d, os_fd dir) { /* remember a directory that got a new entry */
    HANDLE dup;
    if (!d->track || (d->ndirty && d->dirty_src == dir)) return;
    if (d->ndirty == DST_DIRTY) os_dst_sync(d);
    if (DuplicateHandle(GetCurrentProcess(), (HANDLE)dir, GetCurrentProcess(), &dup, 0, FALSE,
                        DUPLICATE_SAME_ACCESS)) {
        d->dirty[d->ndirty++] = (os_fd)dup;
        d->dirty_src = dir;
    }
}
int os_dst_sync(os_dst *d) {
    int i, r = 0;
    for (i = 0; i < d->ndirty; i++) { /* NTFS / ReFS: flushes the directory's metadata */
        if (!FlushFileBuffers((HANDLE)d->dirty[i]) && !r) r = SLZ_E_IO;
        CloseHandle((HANDLE)d->dirty[i]);
    }
    d->ndirty = 0;
    d->dirty_src = OS_BAD;
    return r;
}
int os_dst_open(os_dst *d, const char *path) {
    wchar_t *w = wpath(path);
    HANDLE h;
    memset(d, 0, sizeof *d);
    d->root = d->cfd = d->dirty_src = OS_BAD;
    if (!w) return SLZ_E_OPEN;
    CreateDirectoryW(w, NULL);
    /* the root is chosen by the caller and may itself be a link; everything below it is not trusted */
    if ((h = open_dir(w, 0)) == INVALID_HANDLE_VALUE ||
        !(d->chain = (os_fd *)malloc(2048 * sizeof *d->chain))) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        free(w);
        return SLZ_E_OPEN;
    }
    d->root = (os_fd)h;
    d->wroot = w;
    return 0;
}
void os_dst_close(os_dst *d) {
    os_dst_sync(d);
    drop_chain(d);
    if (d->root != OS_BAD) CloseHandle((HANDLE)d->root);
    free(d->chain);
    free(d->wroot);
    d->chain = NULL;
    d->wroot = NULL;
    d->root = OS_BAD;
}
/* Every directory below the root is created, then opened as a handle that refuses reparse points
 * and blocks renames/deletes while held (see os_dst); the leaf is opened with
 * FILE_FLAG_OPEN_REPARSE_POINT and verified on its handle. */
int os_create(os_dst *d, const char *rel, int mode, unsigned perm, os_fd *fd) {
    size_t rn = wcslen((wchar_t *)d->wroot), i, dn;
    wchar_t *r = wide(rel, 0), *w;
    HANDLE h;
    int ret = 0;
    const char *sl = strrchr(rel, '/');
    (void)perm;
    if (!r) return SLZ_E_UNSAFE;
    w = (wchar_t *)malloc((rn + wcslen(r) + 2) * sizeof(wchar_t));
    if (!w) {
        free(r);
        return SLZ_E_NOMEM;
    }
    memcpy(w, d->wroot, rn * sizeof(wchar_t));
    w[rn] = L'\\';
    for (i = 0; r[i]; i++)
        w[rn + 1 + i] = r[i] == L'/' ? L'\\' : r[i];
    w[rn + 1 + i] = 0;
    free(r);
    dn = mode == OC_MKDIR ? strlen(rel) : sl ? (size_t)(sl - rel) : 0;
    if (dn && !(d->cdn == dn && d->cfd != OS_BAD && !memcmp(d->cdir, rel, dn))) {
        size_t we = mode == OC_MKDIR ? wcslen(w) : (size_t)(wcsrchr(w + rn + 1, L'\\') - w);
        os_fd parent = d->root;
        drop_chain(d);
        for (i = rn + 1; i <= we && !ret; i++) {
            wchar_t c = w[i];
            HANDLE dh;
            if (i != we && c != L'\\') continue;
            w[i] = 0;
            if (CreateDirectoryW(w, NULL))
                mark_dirty(d, parent);
            else if (GetLastError() != ERROR_ALREADY_EXISTS)
                ret = werr();
            if (!ret) {
                if ((dh = open_dir(w, 1)) == INVALID_HANDLE_VALUE || d->nchain == 2048) {
                    if (dh != INVALID_HANDLE_VALUE) CloseHandle(dh);
                    ret = SLZ_E_UNSAFE; /* a reparse point (junction / symlink) or not a directory */
                } else {
                    d->chain[d->nchain++] = (os_fd)dh;
                    parent = (os_fd)dh;
                }
            }
            w[i] = c;
        }
        if (!ret && dn < sizeof d->cdir) {
            memcpy(d->cdir, rel, dn);
            d->cdn = dn;
            d->cfd = parent;
        } else {
            drop_chain(d);
        }
    }
    if (!ret && mode != OC_MKDIR) {
        if (mode == OC_OVERWRITE) DeleteFileW(w);
        h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        mode == OC_RESUME ? OPEN_ALWAYS : CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (h == INVALID_HANDLE_VALUE)
            ret = werr();
        else {
            if (GetLastError() != ERROR_ALREADY_EXISTS) mark_dirty(d, dn ? d->cfd : d->root); /* new name */
            BY_HANDLE_FILE_INFORMATION bi;
            if (!GetFileInformationByHandle(h, &bi) ||
                (bi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
                CloseHandle(h);
                ret = SLZ_E_UNSAFE;
            } else
                *fd = (os_fd)h;
        }
    }
    free(w);
    return ret;
}
unsigned os_ncpu(void) {
    SYSTEM_INFO s;
    GetSystemInfo(&s);
    return s.dwNumberOfProcessors ? s.dwNumberOfProcessors : 1;
}
typedef struct {
    void (*fn)(void *);
    void *arg;
} th_arg;
static DWORD WINAPI th_main(LPVOID p) {
    th_arg *t = (th_arg *)p;
    t->fn(t->arg);
    return 0;
}
int os_run(unsigned n, void (*fn)(void *), void *arg) {
    HANDLE h[64];
    th_arg t = {fn, arg};
    unsigned i, k = 0;
    for (i = 1; i < n && i < 64; i++)
        if ((h[k] = CreateThread(NULL, 0, th_main, &t, 0, NULL))) k++;
    fn(arg);
    for (i = 0; i < k; i++) {
        WaitForSingleObject(h[i], INFINITE);
        CloseHandle(h[i]);
    }
    return 0;
}
int64_t os_add(volatile int64_t *p, int64_t v) {
    return InterlockedExchangeAdd64((volatile LONG64 *)p, v) + v;
}
int os_cas(volatile int64_t *p, int64_t expect, int64_t v) {
    return InterlockedCompareExchange64((volatile LONG64 *)p, v, expect) == expect;
}
int os_open_journal(const char *path, os_fd *fd) {
    wchar_t *w = wpath(path);
    BY_HANDLE_FILE_INFORMATION bi;
    HANDLE h;
    if (!w) return SLZ_E_OPEN;
    /* share mode 0: exclusive while open; never opens through a reparse point */
    h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION ? SLZ_E_STATE : werr();
    if (!GetFileInformationByHandle(h, &bi) ||
        (bi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        CloseHandle(h);
        return SLZ_E_UNSAFE;
    }
    *fd = (os_fd)h;
    return 0;
}
int os_sync_parent(const char *path) {
    wchar_t *w = wpath(path), *sl;
    HANDLE h;
    int r = 0;
    if (!w) return SLZ_E_OPEN;
    if ((sl = wcsrchr(w, L'\\'))) *sl = 0;
    if ((h = open_dir(w, 0)) == INVALID_HANDLE_VALUE)
        r = SLZ_E_IO;
    else {
        if (!FlushFileBuffers(h)) r = SLZ_E_IO;
        CloseHandle(h);
    }
    free(w);
    return r;
}

#else /* ---------------- POSIX / WASI ---------------- */
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if !defined(__wasi__) && !defined(SLZ_NO_THREADS)
#include <pthread.h>
#endif
#if !defined(__wasi__)
#include <sys/file.h> /* flock */
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static int perr(void) {
    return errno == EEXIST                                           ? SLZ_E_EXISTS
           : errno == ENOENT                                         ? SLZ_E_OPEN
           : (errno == ENOSPC || errno == ENAMETOOLONG)              ? SLZ_E_LIMIT
           : (errno == ELOOP || errno == ENOTDIR || errno == EMLINK) ? SLZ_E_UNSAFE
                                                                     : SLZ_E_IO;
}
int os_open(const char *path, int rw, int create, os_fd *fd) {
    int f = open(path, (rw ? O_RDWR : O_RDONLY) | (create ? O_CREAT : 0) | O_CLOEXEC, 0600);
    if (f < 0) return perr();
    *fd = f;
    return 0;
}
int64_t os_size(os_fd fd) {
    struct stat s;
    return fstat((int)fd, &s) ? -1 : (int64_t)s.st_size;
}
int64_t os_pread(os_fd fd, void *b, size_t n, uint64_t off) {
    size_t t = 0;
    FAULT(SLZ_E_IO);
    while (t < n) {
        ssize_t k = pread((int)fd, (char *)b + t, n - t, (off_t)(off + t));
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) return SLZ_E_IO;
        if (!k) break;
        t += (size_t)k;
    }
    return (int64_t)t;
}
int os_pwrite(os_fd fd, const void *b, size_t n, uint64_t off) {
    size_t t = 0;
    FAULT(SLZ_E_IO);
    while (t < n) {
        ssize_t k = pwrite((int)fd, (const char *)b + t, n - t, (off_t)(off + t));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return perr();
        t += (size_t)k;
    }
    return 0;
}
int os_sync(os_fd fd, int full) {
    FAULT(SLZ_E_IO);
#ifdef F_FULLFSYNC /* macOS: fsync does not flush the drive cache */
    if (full && fcntl((int)fd, F_FULLFSYNC) == 0) return 0;
#endif
    (void)full;
    return fsync((int)fd) ? SLZ_E_IO : 0;
}
int os_trunc(os_fd fd, uint64_t n) {
    FAULT(SLZ_E_IO);
    return ftruncate((int)fd, (off_t)n) ? SLZ_E_IO : 0;
}
int os_punch(os_fd fd, uint64_t off, uint64_t len) {
    FAULT(SLZ_E_IO);
#if defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE)
    return fallocate((int)fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)off, (off_t)len)
               ? SLZ_E_NOTSUP
               : 0;
#elif defined(F_PUNCHHOLE)
    fpunchhole_t p;
    memset(&p, 0, sizeof p);
    p.fp_offset = (off_t)off;
    p.fp_length = (off_t)len;
    return fcntl((int)fd, F_PUNCHHOLE, &p) ? SLZ_E_NOTSUP : 0;
#elif defined(SPACECTL_DEALLOC)
    struct spacectl_range r;
    r.r_offset = (off_t)off;
    r.r_len = (off_t)len;
    return fspacectl((int)fd, SPACECTL_DEALLOC, &r, 0, NULL) ? SLZ_E_NOTSUP : 0;
#else
    (void)fd;
    (void)off;
    (void)len;
    return SLZ_E_NOTSUP;
#endif
}
int os_mtime(os_fd fd, int64_t t) {
    struct timespec ts[2];
    ts[0].tv_sec = ts[1].tv_sec = (time_t)t;
    ts[0].tv_nsec = ts[1].tv_nsec = 0;
    return futimens((int)fd, ts) ? SLZ_E_IO : 0;
}
void os_close(os_fd fd) {
    close((int)fd);
}
int os_unlink(const char *path) {
    return unlink(path) ? perr() : 0;
}
uint64_t os_fileid(os_fd fd) {
    struct stat s;
    if (fstat((int)fd, &s)) return 0;
    return (uint64_t)s.st_ino ^ (uint64_t)s.st_dev << 40;
}
int os_exists(const char *path) {
    struct stat s;
    return lstat(path, &s) == 0;
}
uint64_t os_allocated(const char *path) {
    struct stat s;
    return stat(path, &s) ? 0 : (uint64_t)s.st_blocks * 512;
}
int os_dst_sync(os_dst *d) {
    int i, r = 0;
    for (i = 0; i < d->ndirty; i++) { /* fsync of a directory makes its new entries durable */
        if (fsync((int)d->dirty[i]) && !r) r = SLZ_E_IO;
        close((int)d->dirty[i]);
    }
    d->ndirty = 0;
    d->dirty_src = OS_BAD;
    return r;
}
static void mark_dirty(os_dst *d, int dir) { /* remember a directory that got a new entry */
#ifndef __wasi__
    int k;
    if (!d->track || (d->ndirty && d->dirty_src == dir)) return;
    if (d->ndirty == DST_DIRTY) os_dst_sync(d);
    if ((k = fcntl(dir, F_DUPFD_CLOEXEC, 0)) >= 0) {
        d->dirty[d->ndirty++] = k;
        d->dirty_src = dir;
    }
#else
    (void)d;
    (void)dir;
#endif
}
int os_dst_open(os_dst *d, const char *path) {
    int f;
    memset(d, 0, sizeof *d);
    d->root = d->cfd = d->dirty_src = OS_BAD;
    mkdir(path, 0755);
    /* the root is chosen by the caller and may itself be a link (macOS /tmp is one); everything
     * below it is opened with O_NOFOLLOW */
    if ((f = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) return SLZ_E_OPEN;
    d->root = f;
    return 0;
}
void os_dst_close(os_dst *d) {
    os_dst_sync(d);
    if (d->cfd != OS_BAD) close((int)d->cfd);
    if (d->root != OS_BAD) close((int)d->root);
    d->root = d->cfd = OS_BAD;
}
int os_open_journal(const char *path, os_fd *fd) {
    struct stat s;
    int f = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (f < 0) return perr();
#ifndef __wasi__
    if (flock(f, LOCK_EX | LOCK_NB)) { /* another process is extracting this archive */
        close(f);
        return SLZ_E_STATE;
    }
#endif
    if (fstat(f, &s) || !S_ISREG(s.st_mode)) {
        close(f);
        return SLZ_E_UNSAFE;
    }
    *fd = f;
    return 0;
}
int os_sync_parent(const char *path) {
    char buf[4096];
    const char *sl = strrchr(path, '/');
    size_t n = sl ? (size_t)(sl - path) : 0;
    int f, r;
    if (n >= sizeof buf) return SLZ_E_LIMIT;
    memcpy(buf, path, n);
    strcpy(buf + n, n ? "" : ".");
    if (sl == path) strcpy(buf, "/");
    if ((f = open(buf, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) return SLZ_E_IO;
    r = fsync(f) ? SLZ_E_IO : 0;
    close(f);
    return r;
}
/* Walk with openat(O_NOFOLLOW|O_DIRECTORY): a symlink anywhere (pre-existing or raced) is refused.
 * The last directory fd is cached so siblings cost a single openat. */
int os_create(os_dst *d, const char *rel, int mode, unsigned perm, os_fd *fd) {
    const char *sl = strrchr(rel, '/'), *name = sl ? sl + 1 : rel;
    size_t dn = mode == OC_MKDIR ? strlen(rel) : sl ? (size_t)(sl - rel) : 0;
    int dfd = (int)d->root, f;
    if (dn >= sizeof d->cdir) return SLZ_E_LIMIT;
    if (dn && d->cfd != OS_BAD && d->cdn == dn && !memcmp(d->cdir, rel, dn))
        dfd = (int)d->cfd;
    else if (dn) {
        char buf[4096];
        char *c = buf, *e;
        memcpy(buf, rel, dn);
        buf[dn] = 0;
        if (d->cfd != OS_BAD) {
            if (d->dirty_src == d->cfd) d->dirty_src = OS_BAD;
            close((int)d->cfd);
            d->cfd = OS_BAD;
        }
        for (;; c = e + 1) {
            int nf;
            if ((e = strchr(c, '/'))) *e = 0;
            if (!mkdirat(dfd, c, 0755))
                mark_dirty(d, dfd);
            else if (errno != EEXIST) {
                f = perr();
                goto bad;
            }
            nf = openat(dfd, c, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (nf < 0) {
                f = perr();
                goto bad;
            } /* ELOOP/ENOTDIR (symlink) -> SLZ_E_UNSAFE */
            if (dfd != (int)d->root) {
                if (d->dirty_src == dfd) d->dirty_src = OS_BAD; /* fd number may be reused */
                close(dfd);
            }
            dfd = nf;
            if (!e) break;
        }
        memcpy(d->cdir, rel, dn);
        d->cdn = dn;
        d->cfd = dfd;
    }
    if (mode == OC_MKDIR) return 0;
    if (mode == OC_OVERWRITE) unlinkat(dfd, name, 0);
    f = openat(dfd, name, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | (mode == OC_RESUME ? 0 : O_EXCL),
               (perm & 0111) ? 0755 : 0644);
    if (f < 0) return perr();
    mark_dirty(d, dfd); /* new name (or recreated after a crash) in this directory */
    {
        struct stat s;
        if (fstat(f, &s) || !S_ISREG(s.st_mode)) {
            close(f);
            return SLZ_E_UNSAFE;
        }
    }
    *fd = f;
    return 0;
bad:
    if (dfd != (int)d->root) close(dfd);
    return f;
}
#if defined(__wasi__) || defined(SLZ_NO_THREADS)
unsigned os_ncpu(void) {
    return 1;
}
int os_run(unsigned n, void (*fn)(void *), void *arg) {
    (void)n;
    fn(arg);
    return 0;
}
int64_t os_add(volatile int64_t *p, int64_t v) {
    return *p += v;
}
int os_cas(volatile int64_t *p, int64_t expect, int64_t v) {
    if (*p != expect) return 0;
    *p = v;
    return 1;
}
#else
unsigned os_ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1;
}
typedef struct {
    void (*fn)(void *);
    void *arg;
} th_arg;
static void *th_main(void *p) {
    th_arg *t = (th_arg *)p;
    t->fn(t->arg);
    return NULL;
}
int os_run(unsigned n, void (*fn)(void *), void *arg) {
    pthread_t h[64];
    th_arg t;
    unsigned i, k = 0;
    t.fn = fn;
    t.arg = arg;
    for (i = 1; i < n && i < 64; i++)
        if (!pthread_create(&h[k], NULL, th_main, &t)) k++;
    fn(arg);
    for (i = 0; i < k; i++)
        pthread_join(h[i], NULL);
    return 0;
}
int64_t os_add(volatile int64_t *p, int64_t v) {
    return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST);
}
int os_cas(volatile int64_t *p, int64_t expect, int64_t v) {
    return __atomic_compare_exchange_n(p, &expect, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
#endif
#endif
