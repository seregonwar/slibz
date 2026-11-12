#!/usr/bin/env python3
# slz - Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
"""End-to-end tests for slz: python run_tests.py <slz-cli> <slz-test> [--fuzz N]

Builds a corpus, packs it with Python's zipfile/tarfile/gzip (independent reference
implementation), extracts with slz and compares byte for byte. Then: hostile archives
(path traversal, symlinks, overlapping entries, bombs), truncation / bit-flip fuzzing
(must fail cleanly, never crash or write outside the destination) and destructive
extraction interrupted and resumed many times."""
import gzip, hashlib, io, os, random, shutil, struct, subprocess, sys, tarfile, tempfile, zipfile

CLI, TESTER = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
FUZZ = int(sys.argv[sys.argv.index("--fuzz") + 1]) if "--fuzz" in sys.argv else 300
WIN = os.name == "nt"
failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print("FAIL:", msg)


def run(*args, expect=None):
    p = subprocess.run([CLI, *args], capture_output=True, text=True, errors="replace")
    if p.returncode < 0 or p.returncode > 2 or "Sanitizer" in p.stderr or "runtime error" in p.stderr:
        failures.append(f"CRASH {args}: rc={p.returncode} {p.stderr[-2000:]}")
        print("CRASH:", args, p.stderr[-2000:])
    if expect is not None and (p.returncode == 0) != expect:
        check(False, f"{args}: rc={p.returncode} expected {'ok' if expect else 'error'} {p.stderr.strip()}")
    return p


def tree(root):
    out = {}
    for d, dirs, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            out[os.path.relpath(p, root).replace("\\", "/")] = hashlib.sha256(open(p, "rb").read()).hexdigest()
        for x in dirs:
            out[os.path.relpath(os.path.join(d, x), root).replace("\\", "/") + "/"] = "dir"
    return out


def make_corpus(root):
    rnd = random.Random(42)
    files = {
        "empty.txt": b"",
        "one.bin": b"\x00",
        "text/lorem.txt": b"lorem ipsum dolor sit amet " * 40000,
        "text/utf8-\u00e8\u00e0\u4e2d.txt": "unicode name \u00e8".encode(),
        "rand/r1.bin": rnd.randbytes(3 << 20),
        "rand/r2.bin": rnd.randbytes(70000),
        "zero/z.bin": bytes(5 << 20),
        "mixed/m.bin": b"".join(rnd.randbytes(rnd.randint(1, 300)) * rnd.randint(1, 50) for _ in range(3000)),
        "deep/" + "/".join(f"d{i}" for i in range(40)) + "/leaf.txt": b"deep",
    }
    for i in range(300):
        files[f"many/f{i:04d}.txt"] = (f"file {i} " * rnd.randint(0, 200)).encode()
    for name, data in files.items():
        p = os.path.join(root, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data)
    os.makedirs(os.path.join(root, "emptydir"), exist_ok=True)
    return files


def pack_all(src, out):
    paths = sorted(tree(src))
    arcs = {}
    for name, comp, lvl, z64 in [("store.zip", zipfile.ZIP_STORED, None, False), ("def1.zip", zipfile.ZIP_DEFLATED, 1, False),
                                 ("def9.zip", zipfile.ZIP_DEFLATED, 9, False), ("zip64.zip", zipfile.ZIP_DEFLATED, 6, True)]:
        p = os.path.join(out, name)
        with zipfile.ZipFile(p, "w", comp, compresslevel=lvl) as z:
            for rel in paths:
                if rel.endswith("/"):
                    z.writestr(zipfile.ZipInfo(rel), b"")
                else:
                    with open(os.path.join(src, rel), "rb") as f, z.open(rel, "w", force_zip64=z64) as w:
                        shutil.copyfileobj(f, w)
        arcs[name] = p
    for name, mode, fmt in [("ustar.tar", "w", tarfile.USTAR_FORMAT), ("gnu.tar", "w", tarfile.GNU_FORMAT),
                            ("pax.tar", "w", tarfile.PAX_FORMAT), ("pax.tar.gz", "w:gz", tarfile.PAX_FORMAT)]:
        p = os.path.join(out, name)
        with tarfile.open(p, mode, format=fmt) as t:
            for rel in paths:
                if fmt == tarfile.USTAR_FORMAT and (len(rel.encode()) > 255 or any(ord(c) > 127 for c in rel)):
                    continue
                t.add(os.path.join(src, rel.rstrip("/")), arcname=rel.rstrip("/"), recursive=False)
        arcs[name] = p
    p = os.path.join(out, "lorem.txt.gz")
    with gzip.open(p, "wb") as g:
        g.write(open(os.path.join(src, "text/lorem.txt"), "rb").read())
    arcs["gz"] = p
    sz = shutil.which("7z") or shutil.which("7zz") or shutil.which("7za")
    if sz:  # 7z archives made by the reference implementation, one per coder configuration
        for name, opts in [("lzma2.7z", ["-m0=lzma2", "-mx=5"]), ("lzma.7z", ["-m0=lzma", "-ms=on"]),
                           ("nonsolid.7z", ["-m0=lzma2", "-ms=off"]), ("copy.7z", ["-mx=0"]),
                           ("deflate.7z", ["-m0=deflate"]), ("bcj.7z", ["-mf=BCJ", "-m0=lzma2"]),
                           ("plainhdr.7z", ["-mhc=off", "-m0=lzma"]), ("ultra.7z", ["-mx=9", "-mqs=on"])]:
            p = os.path.join(out, name)
            subprocess.run([sz, "a", "-bd", "-y", *opts, p, "."], cwd=src, check=True, stdout=subprocess.DEVNULL)
            arcs[name] = p
    return arcs


def zip_with(entries):
    """entries: list of (ZipInfo or name, data)"""
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", zipfile.ZIP_DEFLATED) as z:
        for info, data in entries:
            z.writestr(info, data)
    return b.getvalue()


def tar_with(members):
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode="w") as t:
        for info, data in members:
            t.addfile(info, io.BytesIO(data) if data is not None else None)
    return b.getvalue()


def ti(name, size=0, type=tarfile.REGTYPE, link=""):
    t = tarfile.TarInfo(name)
    t.size, t.type, t.linkname = size, type, link
    return t


def symlink_zip(name, target):
    i = zipfile.ZipInfo(name)
    i.create_system, i.external_attr = 3, (0o120777 << 16)
    return (i, target)


def overlapping_zip():
    """two central-directory entries pointing at the same local data (zip-bomb style overlap)"""
    raw = bytearray(zip_with([("a.txt", b"A" * 1000), ("b.txt", b"B" * 1000)]))
    cd = raw.rfind(b"PK\x01\x02")  # second central entry
    struct.pack_into("<I", raw, cd + 42, 0)
    return bytes(raw)


def main():
    work = tempfile.mkdtemp(prefix="slz-test-")
    try:
        src, arcdir = os.path.join(work, "src"), os.path.join(work, "arc")
        os.makedirs(arcdir)
        make_corpus(src)
        want = tree(src)
        arcs = pack_all(src, arcdir)

        # 1. round trip, every format, single and multi-threaded
        for name, p in arcs.items():
            for threads in ("1", "8"):
                dst = os.path.join(work, f"out-{name}-{threads}")
                run("x", p, dst, "-q", "-j", threads, expect=True)
                got = tree(dst)
                ref = {"lorem.txt": want["text/lorem.txt"]} if name == "gz" else want
                if name == "ustar.tar":
                    ref = {k: v for k, v in want.items() if k in got}
                    check(len(ref) > 300, "ustar subset too small")
                check(got == ref, f"round trip mismatch for {name} (threads={threads}): "
                      f"missing={sorted(set(ref) - set(got))[:5]} extra={sorted(set(got) - set(ref))[:5]}")
                shutil.rmtree(dst, ignore_errors=True)
            run("l", p, expect=True)
        p = run("l", arcs["def9.zip"], "text")
        check("lorem.txt" in p.stdout and "utf8-" in p.stdout, "tree listing of text/")

        # 2. existing files are not overwritten unless -o
        dst = os.path.join(work, "exist")
        run("x", arcs["store.zip"], dst, "-q", expect=True)
        run("x", arcs["store.zip"], dst, "-q", expect=False)
        run("x", arcs["store.zip"], dst, "-q", "-o", expect=True)
        shutil.rmtree(dst)

        # 3. hostile archives: must error (or skip links) and never write outside dest
        hostile = {
            "slip.zip": (zip_with([("../evil.txt", b"x")]), False),
            "slip2.zip": (zip_with([("a/../../evil.txt", b"x")]), False),
            "abs.zip": (zip_with([("/tmp/slz-evil.txt", b"x")]), False),
            "backslash.zip": (zip_with([("..\\evil.txt", b"x")]), False),
            "nul.zip": (zip_with([("aQb", b"x")]).replace(b"aQb", b"a\x00b"), False),  # zipfile cuts names at NUL
            "overlap.zip": (overlapping_zip(), False),
            "symlink.zip": (zip_with([symlink_zip("link", "/etc"), ("link/passwd", b"x")]), True),
            "slip.tar": (tar_with([(ti("../evil.txt", 1), b"x")]), False),
            "abs.tar": (tar_with([(ti("/tmp/slz-evil.txt", 1), b"x")]), False),
            "symlink.tar": (tar_with([(ti("l", 0, tarfile.SYMTYPE, "/etc"), None), (ti("l/x", 1), b"x")]), True),
            "hardlink.tar": (tar_with([(ti("h", 0, tarfile.LNKTYPE, "/etc/passwd"), None)]), True),
            "dev.tar": (tar_with([(ti("dev", 0, tarfile.CHRTYPE), None)]), True),
        }
        if WIN:
            hostile["drive.zip"] = (zip_with([("C:/evil.txt", b"x")]), False)
            hostile["device.zip"] = (zip_with([("CON", b"x")]), False)
            hostile["ads.zip"] = (zip_with([("a.txt:stream", b"x")]), False)
        for name, (data, ok) in hostile.items():
            p = os.path.join(arcdir, name)
            open(p, "wb").write(data)
            dst = os.path.join(work, "h", "dest")
            os.makedirs(os.path.join(work, "h"))
            run("x", p, dst, "-q", expect=ok)
            outside = set(os.listdir(os.path.join(work, "h"))) - {"dest"} if os.path.isdir(os.path.join(work, "h")) else set()
            check(not outside and not os.path.exists("/tmp/slz-evil.txt"), f"{name} wrote outside dest: {outside}")
            for d, dirs, files in os.walk(dst):
                for f in dirs + files:
                    check(not os.path.islink(os.path.join(d, f)), f"{name} created a symlink")
            shutil.rmtree(os.path.join(work, "h"), ignore_errors=True)

        # encrypted 7z (AES data, AES header): refused with a clear error, nothing written
        sz = shutil.which("7z") or shutil.which("7zz") or shutil.which("7za")
        if sz:
            for name, extra in (("aes.7z", ["-mhe=off"]), ("aeshdr.7z", ["-mhe=on"])):
                p = os.path.join(arcdir, name)
                subprocess.run([sz, "a", "-bd", "-y", "-psecret", *extra, p, "text"], cwd=src, check=True,
                               stdout=subprocess.DEVNULL)
                dst = os.path.join(work, "enc")
                r = run("x", p, dst, "-q", expect=False)
                check("encrypted" in r.stderr, f"{name}: expected an 'encrypted' error, got {r.stderr.strip()}")
                check(not os.path.exists(dst) or not any(f for _, _, f in os.walk(dst)), f"{name}: wrote files")
                shutil.rmtree(dst, ignore_errors=True)

        # pre-existing symlink inside dest must not be followed (POSIX)
        if not WIN:
            dst = os.path.join(work, "pre")
            os.makedirs(dst)
            os.makedirs(os.path.join(work, "outside"))
            os.symlink(os.path.join(work, "outside"), os.path.join(dst, "a"))
            p = os.path.join(arcdir, "through.zip")
            open(p, "wb").write(zip_with([("a/x.txt", b"x")]))
            run("x", p, dst, "-q", expect=False)
            check(not os.listdir(os.path.join(work, "outside")), "followed pre-existing symlink")

        # 4. fuzz: truncations and bit flips must never crash / escape
        rnd = random.Random(7)
        seeds = [arcs[k] for k in ("def1.zip", "pax.tar", "pax.tar.gz", "gz", "zip64.zip", "lzma.7z", "nonsolid.7z",
                                    "plainhdr.7z", "bcj.7z") if k in arcs]
        small = os.path.join(arcdir, "small.zip")
        open(small, "wb").write(zip_with([(f"d/{i}.txt", (b"abc%d" % i) * (i * 30)) for i in range(20)]))
        seeds.append(small)
        blobs = [open(s, "rb").read() for s in seeds]
        for i in range(FUZZ):
            b = bytearray(rnd.choice(blobs))
            if rnd.random() < 0.3:
                b = b[: rnd.randrange(len(b))]
            for _ in range(rnd.randint(1, 8)):
                if b:
                    pos = rnd.randrange(len(b)) if rnd.random() < 0.5 else max(0, len(b) - 1 - rnd.randrange(min(len(b), 2048)))
                    b[pos] = rnd.randrange(256)
            p = os.path.join(arcdir, "fuzz.bin")
            open(p, "wb").write(b)
            dst = os.path.join(work, "f", "dest")
            os.makedirs(os.path.join(work, "f"))
            run("l", p)
            run("x", p, dst, "-q")
            outside = set(os.listdir(os.path.join(work, "f"))) - {"dest"} if os.path.isdir(os.path.join(work, "f")) else set()
            check(not outside, f"fuzz case {i} wrote outside dest")
            shutil.rmtree(os.path.join(work, "f"), ignore_errors=True)

        # 5. destructive extraction: interrupted repeatedly, resumed, result identical, archive gone
        for name in ("def9.zip", "store.zip", "pax.tar", "gz", "nonsolid.7z", "lzma2.7z"):
            if name not in arcs:
                continue
            p = os.path.join(work, "d-" + os.path.basename(arcs[name]))
            shutil.copy(arcs[name], p)
            dst = os.path.join(work, "dest-" + name)
            r = subprocess.run([TESTER, "resume", p, dst, "700"], capture_output=True, text=True, errors="replace")
            ref = {"lorem.txt": want["text/lorem.txt"]} if name == "gz" else want
            check(r.returncode == 0, f"destructive {name}: {r.stdout} {r.stderr}")
            check(tree(dst) == ref, f"destructive {name}: output mismatch")
            check(not os.path.exists(p) and not os.path.exists(p + ".slzstate"), f"destructive {name}: archive/journal left")
            print(f"destructive {name}: {r.stdout.strip()}")
            shutil.rmtree(dst, ignore_errors=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"{len(failures)} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
