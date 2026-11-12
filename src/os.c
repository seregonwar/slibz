/* slz - platform layer: positional I/O, durable sync, hole punching, symlink-safe file creation,
 * Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
 *
 * threads. Windows (Win32), POSIX (Linux, macOS, FreeBSD) and WASI (single-threaded). */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "internal.h"

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
    return FlushFileBuffers((HANDLE)fd) ? 0 : SLZ_E_IO;
}
int os_trunc(os_fd fd, uint64_t n) {
    FILE_END_OF_FILE_INFO i;
    i.EndOfFile.QuadPart = (LONGLONG)n;
    return SetFileInformationByHandle((HANDLE)fd, FileEndOfFileInfo, &i, sizeof i) ? 0 : SLZ_E_IO;
}
int os_punch(os_fd fd, uint64_t off, uint64_t len) {
    FILE_ZERO_DATA_INFORMATION z;
    DWORD r;
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
int os_dst_open(os_dst *d, const char *path) {
    wchar_t *w = wpath(path);
    DWORD a;
    memset(d, 0, sizeof *d);
    d->root = d->cfd = OS_BAD;
    if (!w) return SLZ_E_OPEN;
    CreateDirectoryW(w, NULL);
    a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        free(w);
        return SLZ_E_OPEN;
    }
    d->wroot = w;
    return 0;
}
void os_dst_close(os_dst *d) {
    free(d->wroot);
    d->wroot = NULL;
}
/* Every intermediate directory is created/verified to be a real directory, never a reparse
 * point (junction/symlink); the leaf is opened with FILE_FLAG_OPEN_REPARSE_POINT. */
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
    if (dn && !(d->cdn == dn && !memcmp(d->cdir, rel, dn))) {
        size_t we = mode == OC_MKDIR ? wcslen(w) : (size_t)(wcsrchr(w + rn + 1, L'\\') - w);
        for (i = rn + 1; i <= we && !ret; i++) {
            wchar_t c = w[i];
            DWORD a;
            if (i != we && c != L'\\') continue;
            w[i] = 0;
            if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ret = werr();
            a = GetFileAttributesW(w);
            if (!ret && (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) ||
                         (a & FILE_ATTRIBUTE_REPARSE_POINT)))
                ret = SLZ_E_UNSAFE;
            w[i] = c;
        }
        if (!ret && dn < sizeof d->cdir) {
            memcpy(d->cdir, rel, dn);
            d->cdn = dn;
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

#else /* ---------------- POSIX / WASI ---------------- */
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if !defined(__wasi__) && !defined(SLZ_NO_THREADS)
#include <pthread.h>
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
    while (t < n) {
        ssize_t k = pwrite((int)fd, (const char *)b + t, n - t, (off_t)(off + t));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return perr();
        t += (size_t)k;
    }
    return 0;
}
int os_sync(os_fd fd, int full) {
#ifdef F_FULLFSYNC /* macOS: fsync does not flush the drive cache */
    if (full && fcntl((int)fd, F_FULLFSYNC) == 0) return 0;
#endif
    (void)full;
    return fsync((int)fd) ? SLZ_E_IO : 0;
}
int os_trunc(os_fd fd, uint64_t n) {
    return ftruncate((int)fd, (off_t)n) ? SLZ_E_IO : 0;
}
int os_punch(os_fd fd, uint64_t off, uint64_t len) {
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
int os_dst_open(os_dst *d, const char *path) {
    int f;
    memset(d, 0, sizeof *d);
    d->root = d->cfd = OS_BAD;
    mkdir(path, 0755);
    if ((f = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) return SLZ_E_OPEN;
    d->root = f;
    return 0;
}
void os_dst_close(os_dst *d) {
    if (d->cfd != OS_BAD) close((int)d->cfd);
    if (d->root != OS_BAD) close((int)d->root);
    d->root = d->cfd = OS_BAD;
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
            close((int)d->cfd);
            d->cfd = OS_BAD;
        }
        for (;; c = e + 1) {
            int nf;
            if ((e = strchr(c, '/'))) *e = 0;
            if (mkdirat(dfd, c, 0755) && errno != EEXIST) {
                f = perr();
                goto bad;
            }
            nf = openat(dfd, c, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (nf < 0) {
                f = perr();
                goto bad;
            } /* ELOOP/ENOTDIR (symlink) -> SLZ_E_UNSAFE */
            if (dfd != (int)d->root) close(dfd);
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
#endif
#endif
