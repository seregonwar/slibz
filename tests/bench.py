#!/usr/bin/env python3
# slz - Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
"""End-to-end benchmark: slz vs Info-ZIP unzip, libarchive (bsdtar), 7-Zip, GNU tar, Python.

    python3 bench.py <slz-binary> <workdir> [--runs N]

Builds datasets in <workdir> (Silesia corpus from /opt/corpora, a many-small-files tree from
/usr, a 500k-entry listing stress zip), then times listing and extraction, best of N runs with
a warm page cache. Prints Markdown tables (copied into the README)."""
import os, shutil, subprocess, sys, time, zipfile

SLZ, WORK = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
RUNS = int(sys.argv[sys.argv.index("--runs") + 1]) if "--runs" in sys.argv else 3
os.makedirs(WORK, exist_ok=True)


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **kw)


def have(tool):
    return shutil.which(tool) is not None


def best(cmd, out=None):
    t = []
    for _ in range(RUNS):
        if out:
            shutil.rmtree(out, ignore_errors=True)
        t0 = time.perf_counter()
        sh(cmd)
        t.append(time.perf_counter() - t0)
    if out:
        shutil.rmtree(out, ignore_errors=True)
    return min(t)


def dataset():
    sets = {}
    sil = "/opt/corpora/silesia"
    if not os.path.isdir(f"{sil}/files"):
        sh(f"mkdir -p {sil}/files && cd {sil}/files && unzip -q ../silesia.zip")
    small = os.path.join(WORK, "small")
    if not os.path.isdir(small):
        os.makedirs(small)
        for d in ("/usr/include", "/usr/share/doc", "/usr/lib/python3"):
            if os.path.isdir(d):
                sh(f"cp -r --no-preserve=mode {d} {small}/ 2>/dev/null || true")
    for name, src in (("silesia", f"{sil}/files"), ("small", small)):
        z, tgz = os.path.join(WORK, f"{name}.zip"), os.path.join(WORK, f"{name}.tar.gz")
        if not os.path.exists(z):
            sh(f"cd {src} && zip -q -r -6 -y {z} .")
        if not os.path.exists(tgz):
            sh(f"tar -C {src} -czf {tgz} .")
        sz = os.path.join(WORK, f"{name}.7z")
        if have("7z") and not os.path.exists(sz):
            sh(f"cd {src} && 7z a -bd -y {sz} .")  # 7-Zip defaults: LZMA2 -mx5, solid
        szn = os.path.join(WORK, f"{name}-nonsolid.7z")
        if have("7z") and not os.path.exists(szn):
            sh(f"cd {src} && 7z a -bd -y -ms=off {szn} .")
        files = sum(len(f) for _, _, f in os.walk(src))
        size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(src) for f in fs if not os.path.islink(os.path.join(d, f)))
        sets[name] = (z, tgz, files, size)
    big = os.path.join(WORK, "entries500k.zip")
    if not os.path.exists(big):
        with zipfile.ZipFile(big, "w", zipfile.ZIP_STORED) as zf:
            for i in range(500_000):
                zf.writestr(f"d{i % 1000:03d}/sub{i % 7}/file{i:06d}.txt", b"")
    return sets, big


def main():
    sets, big = dataset()
    out = os.path.join(WORK, "out")
    ncpu = os.cpu_count()
    print(f"\nCPU threads: {ncpu}, runs: best of {RUNS}, warm cache, output on {WORK}\n")

    print("### Listing (ms, whole process)\n")
    print("| archive | entries | slz | unzip -Z1 | bsdtar -t | 7z l | python zipfile |")
    print("|---|---|---|---|---|---|---|")
    for label, z, n in (("silesia.zip", sets["silesia"][0], 12), ("small.zip", sets["small"][0], sets["small"][2]), ("500k entries", big, 500_000)):
        row = [best(f"{SLZ} l {z}"), best(f"unzip -Z1 {z}"), best(f"bsdtar -tf {z}") if have("bsdtar") else None,
               best(f"7z l {z}") if have("7z") else None, best(f"python3 -c \"import zipfile;zipfile.ZipFile('{z}').namelist()\"")]
        print(f"| {label} | {n} | " + " | ".join("-" if v is None else f"{v * 1e3:.0f}" for v in row) + " |")

    print("\n### Extraction, zip (s)\n")
    print(f"| archive | files / MB | slz (-j {ncpu}) | slz -j 1 | unzip | bsdtar | 7z x | python zipfile |")
    print("|---|---|---|---|---|---|---|---|")
    for name in ("silesia", "small"):
        z, _, files, size = sets[name]
        row = [best(f"{SLZ} x {z} {out} -q", out), best(f"{SLZ} x {z} {out} -q -j 1", out), best(f"unzip -q {z} -d {out}", out),
               best(f"mkdir -p {out} && bsdtar -xf {z} -C {out}", out) if have("bsdtar") else None,
               best(f"7z x -y -o{out} {z}", out) if have("7z") else None,
               best(f"python3 -c \"import zipfile;zipfile.ZipFile('{z}').extractall('{out}')\"", out)]
        print(f"| {name}.zip | {files} / {size / 1e6:.0f} | " + " | ".join("-" if v is None else f"{v:.2f}" for v in row) + " |")

    print("\n### Extraction, tar.gz (s)\n")
    print("| archive | slz | GNU tar -xzf | pigz -d \\| tar -x | bsdtar |")
    print("|---|---|---|---|---|")
    for name in ("silesia", "small"):
        tgz = sets[name][1]
        row = [best(f"{SLZ} x {tgz} {out} -q", out), best(f"mkdir -p {out} && tar -xzf {tgz} -C {out}", out),
               best(f"mkdir -p {out} && pigz -dc {tgz} | tar -x -C {out}", out) if have("pigz") else None,
               best(f"mkdir -p {out} && bsdtar -xf {tgz} -C {out}", out) if have("bsdtar") else None]
        print(f"| {name}.tar.gz | " + " | ".join("-" if v is None else f"{v:.2f}" for v in row) + " |")

    if have("7z"):
        print("\n### 7z (s; listing in ms)\n")
        print("| archive | slz list | 7z l | slz x | slz x -j 1 | 7z x | bsdtar -x |")
        print("|---|---|---|---|---|---|---|")
        for label in ("silesia.7z", "silesia-nonsolid.7z", "small.7z", "small-nonsolid.7z"):
            a = os.path.join(WORK, label)
            row = [best(f"{SLZ} l {a}") * 1e3, best(f"7z l {a}") * 1e3, best(f"{SLZ} x {a} {out} -q", out),
                   best(f"{SLZ} x {a} {out} -q -j 1", out), best(f"7z x -y -o{out} {a}", out),
                   best(f"mkdir -p {out} && bsdtar -xf {a} -C {out}", out) if have("bsdtar") else None]
            print(f"| {label} | " + " | ".join("-" if v is None else (f"{v:.0f}" if i < 2 else f"{v:.2f}")
                                            for i, v in enumerate(row)) + " |")


if __name__ == "__main__":
    main()
