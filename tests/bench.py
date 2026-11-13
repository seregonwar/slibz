#!/usr/bin/env python3
# slz - Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
"""End-to-end benchmark: slz vs Info-ZIP unzip, libarchive (bsdtar), 7-Zip, GNU tar, Python.

    python3 bench.py <slz-binary> <workdir> [--runs N] [--sets silesia,small,500k] [--json FILE]

Builds datasets in <workdir> (Silesia corpus from /opt/corpora, a many-small-files tree from
/usr, a 500k-entry listing stress zip), then times listing and extraction, best of N runs with
a warm page cache. Prints Markdown tables (copied into docs/benchmarks.md). --sets picks the datasets,
--json also writes the times in seconds (used by .github/workflows/bench.yml), and $SLZ_CORPUS
moves the Silesia directory (silesia.zip, unpacked into files/)."""
import argparse, json, os, shutil, subprocess, sys, time, zipfile

SET_NAMES = ("silesia", "small", "500k")


def sets_arg(s):
    names = [n for n in s.split(",") if n]
    bad = [n for n in names if n not in SET_NAMES]
    if bad or not names:
        raise argparse.ArgumentTypeError(f"unknown dataset {','.join(bad) or repr(s)} (choose from {','.join(SET_NAMES)})")
    return names


def runs_arg(s):
    n = int(s)
    if n < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return n


ap = argparse.ArgumentParser(description="End-to-end listing and extraction benchmark (see the module docstring).")
ap.add_argument("slz", help="slz binary")
ap.add_argument("workdir", help="directory for the datasets and the extraction output")
ap.add_argument("--runs", type=runs_arg, default=3, help="runs per command, best is kept (default 3)")
ap.add_argument("--sets", type=sets_arg, default=list(SET_NAMES), help="comma-separated datasets (default silesia,small,500k)")
ap.add_argument("--json", help="also write the times in seconds to this file")
ARGS = ap.parse_args()
SLZ, WORK = os.path.abspath(ARGS.slz), os.path.abspath(ARGS.workdir)
# competitor binaries: override with SZ_BIN, BSDTAR_BIN, UNZIP_BIN, TAR_BIN, PIGZ_BIN, PY_BIN
# (not PIGZ / GZIP: those variables are read by the tools themselves as default options)
SZ = os.environ.get("SZ_BIN", shutil.which("7zz") or shutil.which("7z") or "7z")
BSDTAR = os.environ.get("BSDTAR_BIN", "bsdtar")
UNZIP = os.environ.get("UNZIP_BIN", "unzip")
TAR = os.environ.get("TAR_BIN", "tar")
PIGZ = os.environ.get("PIGZ_BIN", "pigz")
PY = os.environ.get("PY_BIN", sys.executable)
RUNS, SETS, JSON = ARGS.runs, ARGS.sets, ARGS.json
RES = {"runs": RUNS, "threads": os.cpu_count(), "versions": {}}
TREE = {}  # dataset -> (regular files, bytes) of its source tree
os.makedirs(WORK, exist_ok=True)


def rec(table, row, cols, vals):  # --json: one table row, times in seconds
    RES.setdefault(table, {})[row] = {c: round(v, 6) for c, v in zip(cols, vals) if v is not None}


def sh(cmd, ok=(0,), **kw):
    r = subprocess.run(cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **kw)
    if r.returncode not in ok:
        raise subprocess.CalledProcessError(r.returncode, cmd)
    return r


def have(tool):
    return shutil.which(tool) is not None or os.path.exists(tool)


def version(cmd):
    try:
        out = subprocess.run(cmd, shell=True, capture_output=True, text=True, errors="replace").stdout
        return next((l.strip() for l in out.splitlines() if l.strip()), "?")
    except Exception:
        return "?"


def tree(d):  # (regular files, bytes) under d; symlinks are not counted
    n = size = 0
    for r, _, fs in os.walk(d):
        for f in fs:
            p = os.path.join(r, f)
            if not os.path.islink(p):
                n, size = n + 1, size + os.path.getsize(p)
    return n, size


def best(cmd, out=None, ok=(0,), expect=None):
    """Best time of RUNS. expect=(files, bytes): when a warning exit code is accepted, the output
    tree is checked against it after each timed run, so an incomplete extraction is an error."""
    t = []
    for _ in range(RUNS):
        if out:
            shutil.rmtree(out, ignore_errors=True)
        t0 = time.perf_counter()
        try:
            sh(cmd, ok=ok)
        except subprocess.CalledProcessError:  # one retry after a fresh cleanup (transient I/O errors)
            if out:
                shutil.rmtree(out, ignore_errors=True)
            t0 = time.perf_counter()
            sh(cmd, ok=ok)
        t.append(time.perf_counter() - t0)
        if expect and tree(out) != expect:
            raise RuntimeError(f"incomplete extraction: {cmd}: {tree(out)} (files, bytes), expected {expect}")
    if out:
        shutil.rmtree(out, ignore_errors=True)
    return min(t)


def dataset():
    sets = {}
    sil = os.environ.get("SLZ_CORPUS", "/opt/corpora/silesia")
    if "silesia" in SETS and not os.path.isdir(f"{sil}/files"):
        sh(f"mkdir -p {sil}/files && cd {sil}/files && unzip -q ../silesia.zip")
    small = os.path.join(WORK, "small")
    if "small" in SETS and not os.path.isdir(small):
        os.makedirs(small)
        for d in ("/usr/include", "/usr/share/doc", "/usr/lib/python3"):
            if os.path.isdir(d):
                sh(f"cp -r --no-preserve=mode {d} {small}/ 2>/dev/null || true")
    for name, src in (("silesia", f"{sil}/files"), ("small", small)):
        if name not in SETS:
            continue
        z, tgz = os.path.join(WORK, f"{name}.zip"), os.path.join(WORK, f"{name}.tar.gz")
        if not os.path.exists(z):
            sh(f"cd {src} && zip -q -r -6 -y {z} .")
        if not os.path.exists(tgz):
            sh(f"tar -C {src} -czf {tgz} .")
        sz = os.path.join(WORK, f"{name}.7z")
        if have(SZ) and not os.path.exists(sz):
            sh(f"cd {src} && {SZ} a -bd -y {sz} .", ok=(0, 1))  # 1 = warnings (e.g. symlinks)  # 7-Zip defaults: LZMA2 -mx5, solid
        szn = os.path.join(WORK, f"{name}-nonsolid.7z")
        if have(SZ) and not os.path.exists(szn):
            sh(f"cd {src} && {SZ} a -bd -y -ms=off {szn} .", ok=(0, 1))
        files = sum(len(f) for _, _, f in os.walk(src))
        TREE[name] = tree(src)
        sets[name] = (z, tgz, files, TREE[name][1])
    big = os.path.join(WORK, "entries500k.zip")
    if "500k" in SETS and not os.path.exists(big):
        with zipfile.ZipFile(big, "w", zipfile.ZIP_STORED) as zf:
            for i in range(500_000):
                zf.writestr(f"d{i % 1000:03d}/sub{i % 7}/file{i:06d}.txt", b"")
    return sets, big


def main():
    sets, big = dataset()
    out = os.path.join(WORK, "out")
    ncpu = os.cpu_count()
    print(f"\nCPU threads: {ncpu}, runs: best of {RUNS}, warm cache, output on {WORK}\n")
    print("Versions:")
    for label, cmd in (("7-Zip", f"{SZ} | grep -m1 7-Zip"), ("bsdtar", f"{BSDTAR} --version"),
                       ("unzip", f"{UNZIP} -v | head -1"), ("tar", f"{TAR} --version | head -1"),
                       ("pigz", f"{PIGZ} --version 2>&1"), ("python", f"{PY} --version")):
        RES["versions"][label] = version(cmd)
        print(f"- {label}: {RES['versions'][label]}")
    print()

    print("### Listing (ms, whole process)\n")
    print("| archive | entries | slz | unzip -Z1 | bsdtar -t | 7z l | python zipfile |")
    print("|---|---|---|---|---|---|---|")
    lists = [(f"{k}.zip", sets[k][0], 12 if k == "silesia" else sets[k][2]) for k in ("silesia", "small") if k in sets]
    for label, z, n in lists + ([("500k entries", big, 500_000)] if "500k" in SETS else []):
        row = [best(f"{SLZ} l {z}"), best(f"{UNZIP} -Z1 {z}"), best(f"{BSDTAR} -tf {z}") if have(BSDTAR) else None,
               best(f"{SZ} l {z}") if have(SZ) else None, best(f"{PY} -c \"import zipfile;zipfile.ZipFile('{z}').namelist()\"")]
        rec("list", label, ("slz", "unzip -Z1", "bsdtar -t", "7z l", "python zipfile"), row)
        print(f"| {label} | {n} | " + " | ".join("-" if v is None else f"{v * 1e3:.0f}" for v in row) + " |")

    print("\n### Extraction, zip (s)\n")
    print(f"| archive | files / MB | slz (-j {ncpu}) | slz -j 1 | unzip | bsdtar | 7z x | python zipfile |")
    print("|---|---|---|---|---|---|---|---|")
    for name in [k for k in ("silesia", "small") if k in sets]:
        z, _, files, size = sets[name]
        row = [best(f"{SLZ} x {z} {out} -q", out), best(f"{SLZ} x {z} {out} -q -j 1", out), best(f"{UNZIP} -q {z} -d {out}", out),
               best(f"mkdir -p {out} && {BSDTAR} -xf {z} -C {out}", out) if have(BSDTAR) else None,
               best(f"{SZ} x -y -o{out} {z}", out, ok=(0, 1), expect=TREE[name]) if have(SZ) else None,  # 1 = warnings, 2 = fatal
               best(f"{PY} -c \"import zipfile;zipfile.ZipFile('{z}').extractall('{out}')\"", out)]
        rec("zip", f"{name}.zip", ("slz", "slz -j 1", "unzip", "bsdtar", "7z x", "python zipfile"), row)
        print(f"| {name}.zip | {files} / {size / 1e6:.0f} | " + " | ".join("-" if v is None else f"{v:.2f}" for v in row) + " |")

    print("\n### Extraction, tar.gz (s)\n")
    print("| archive | slz | GNU tar -xzf | pigz -d \\| tar -x | bsdtar |")
    print("|---|---|---|---|---|")
    for name in [k for k in ("silesia", "small") if k in sets]:
        tgz = sets[name][1]
        row = [best(f"{SLZ} x {tgz} {out} -q", out), best(f"mkdir -p {out} && {TAR} -xzf {tgz} -C {out}", out),
               best(f"mkdir -p {out} && {PIGZ} -dc {tgz} | {TAR} -x -C {out}", out) if have(PIGZ) else None,
               best(f"mkdir -p {out} && {BSDTAR} -xf {tgz} -C {out}", out) if have(BSDTAR) else None]
        rec("targz", f"{name}.tar.gz", ("slz", "GNU tar -xzf", "pigz -d | tar -x", "bsdtar"), row)
        print(f"| {name}.tar.gz | " + " | ".join("-" if v is None else f"{v:.2f}" for v in row) + " |")

    if have(SZ):
        print("\n### 7z (s; listing in ms)\n")
        print("| archive | slz list | 7z l | slz x | slz x -j 1 | 7z x | bsdtar -x |")
        print("|---|---|---|---|---|---|---|")
        for label in ("silesia.7z", "silesia-nonsolid.7z", "small.7z", "small-nonsolid.7z"):
            name = label.split(".")[0].split("-")[0]
            if name not in sets:
                continue
            a = os.path.join(WORK, label)
            row = [best(f"{SLZ} l {a}") * 1e3, best(f"{SZ} l {a}") * 1e3, best(f"{SLZ} x {a} {out} -q", out),
                   best(f"{SLZ} x {a} {out} -q -j 1", out), best(f"{SZ} x -y -o{out} {a}", out, ok=(0, 1), expect=TREE[name]),
                   best(f"mkdir -p {out} && {BSDTAR} -xf {a} -C {out}", out, ok=(0, 1), expect=TREE[name]) if have(BSDTAR) else None]
            rec("7z", label, ("slz list", "7z l", "slz x", "slz x -j 1", "7z x", "bsdtar -x"),
                [v / 1e3 if i < 2 else v for i, v in enumerate(row)])
            print(f"| {label} | " + " | ".join("-" if v is None else (f"{v:.0f}" if i < 2 else f"{v:.2f}")
                                            for i, v in enumerate(row)) + " |")

    if JSON:
        with open(JSON, "w") as f:
            json.dump(RES, f, indent=1)


if __name__ == "__main__":
    main()
