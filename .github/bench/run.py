#!/usr/bin/env python3
# slz - Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
"""Benchmark driver of .github/workflows/bench.yml (Linux, Python 3.8+, stdlib only).

    python3 .github/bench/run.py --deps DIR --corpus DIR --work DIR --out DIR

Builds slz (CMake Release) and the in-memory harnesses against the competitors built by
.github/bench/deps.sh, then runs tests/bench_inflate.c (DEFLATE: slz, zlib, libdeflate, zlib-ng),
tests/bench_lzma.c (LZMA2: slz, liblzma) and tests/bench.py (whole-process listing and extraction
vs unzip, bsdtar, 7-Zip, GNU tar, pigz and Python). Writes OUT/result.json plus the raw outputs.
The decoders run in separate processes (--rounds) and keep each decoder's best MB/s."""
import argparse, datetime, json, os, platform, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def sh(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def first_line(cmd):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, errors="replace").stdout
        return next((l.strip() for l in out.splitlines() if l.strip()), "?")
    except OSError:
        return "?"


def build(a, b):
    sh(["cmake", "-S", ROOT, "-B", b, "-DCMAKE_BUILD_TYPE=Release", "-DSLZ_TESTS=OFF", "-DSLZ_INSTALL=OFF"],
       stdout=subprocess.DEVNULL)
    sh(["cmake", "--build", b, "-j", str(os.cpu_count() or 2)], stdout=subprocess.DEVNULL)
    cc, d, t = os.environ.get("CC", "gcc"), a.deps, os.path.join(ROOT, "tests")
    c = [cc, "-O2", "-std=c99", "-Wall", "-I" + os.path.join(ROOT, "include"), "-I" + os.path.join(ROOT, "src")]
    zng = os.path.join(b, "bench_inflate_zng.o")
    sh(c + ["-I" + d + "/zng/include", "-c", t + "/bench_inflate_zng.c", "-o", zng])
    sh(c + ["-DSLZ_BENCH_ZLIBNG", "-I" + d + "/zlib/include", "-I" + d + "/libdeflate/include",
            t + "/bench_inflate.c", zng, b + "/libslz.a", d + "/zlib/lib/libz.a", d + "/libdeflate/lib/libdeflate.a",
            "-L" + d + "/zng/lib", "-lz-ng", "-Wl,-rpath," + d + "/zng/lib", "-pthread", "-lm", "-o", b + "/bench_inflate"])
    sh(c + ["-I" + d + "/xz/include", t + "/bench_lzma.c", b + "/libslz.a", d + "/xz/lib/liblzma.a",
            "-pthread", "-lm", "-o", b + "/bench_lzma"])


def harness(exe, files, rounds, cols, log):
    """Runs a decoder harness `rounds` times; returns (version line, best MB/s per column, all rounds)."""
    best, runs, version = {}, [], "?"
    for i in range(rounds):
        print(f"+ {os.path.basename(exe)} <{len(files)} files>  (round {i + 1}/{rounds})", flush=True)
        out = subprocess.run([exe] + files, stdout=subprocess.PIPE, text=True, check=True).stdout
        log.write(out + "\n")
        version = out.splitlines()[0].strip()
        tot = next(l for l in out.splitlines() if l.startswith("TOTAL")).split()[2:]
        if len(tot) != len(cols):
            sys.exit(f"{exe}: expected {len(cols)} columns, got TOTAL {tot}")
        runs.append({c: float(v) for c, v in zip(cols, tot)})
        for c, v in runs[-1].items():
            best[c] = max(best.get(c, 0.0), v)
    return version, best, runs


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True).stdout.strip()


def utc(ts):
    return datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def environment():
    cpu = "?"
    try:
        with open("/proc/cpuinfo") as f:
            cpu = next((l.split(":", 1)[1].strip() for l in f if l.startswith("model name")), "?")
    except OSError:
        pass
    osname = "?"
    try:
        with open("/etc/os-release") as f:
            osname = next((l.split("=", 1)[1].strip().strip('"') for l in f if l.startswith("PRETTY_NAME=")), "?")
    except OSError:
        pass
    cc = first_line([os.environ.get("CC", "gcc"), "--version"])
    m = re.search(r"(\S+) \(.*\) (\d+\.\d+\.\d+)", cc)
    e = os.environ
    return {
        "runner": "GitHub Actions" if e.get("GITHUB_ACTIONS") == "true" else platform.node(),
        "os": osname,
        "image": f"{e['ImageOS']} {e.get('ImageVersion', '')}".strip() if e.get("ImageOS") else "",
        "cpu": re.sub(r"\s+", " ", cpu),
        "threads": os.cpu_count(),
        "cc": f"{m.group(1)} {m.group(2)}" if m else cc,
        "kernel": platform.release(),
    }


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--deps", required=True, help="prefix of `deps.sh tools`")
    p.add_argument("--corpus", required=True, help="directory of `deps.sh corpus` (silesia.zip)")
    p.add_argument("--work", required=True, help="build and dataset directory (reusable)")
    p.add_argument("--out", required=True, help="where result.json and the raw outputs go")
    p.add_argument("--rounds", type=int, default=3, help="bench_inflate processes (default 3)")
    p.add_argument("--lzma-rounds", type=int, default=1, help="bench_lzma processes (default 1)")
    p.add_argument("--runs", type=int, default=3, help="tests/bench.py runs per command (default 3)")
    p.add_argument("--no-e2e", action="store_true", help="decoders only")
    a = p.parse_args()
    for k in ("deps", "corpus", "work", "out"):
        setattr(a, k, os.path.abspath(getattr(a, k)))
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(a.work, exist_ok=True)
    timing, t0 = {}, time.time()

    b = os.path.join(a.work, "build")
    build(a, b)
    timing["build"] = round(time.time() - t0, 1)

    files_dir = os.path.join(a.corpus, "files")
    if not os.path.isdir(files_dir):
        sh(["unzip", "-q", os.path.join(a.corpus, "silesia.zip"), "-d", files_dir])
    files = sorted(os.path.join(files_dir, f) for f in os.listdir(files_dir))
    size = sum(os.path.getsize(f) for f in files)

    res = {"schema": 1, "versions": {}, "timing": timing}
    with open(os.path.join(a.out, "decoders.txt"), "w") as log:
        t = time.time()
        v, best, runs = harness(b + "/bench_inflate", files, a.rounds, ("slz", "zlib", "libdeflate", "zlib-ng"), log)
        timing["deflate"] = round(time.time() - t, 1)
        for part in v.split(","):  # "zlib 1.4, libdeflate 1.26, zlib-ng 2.3.3"
            name, _, ver = part.strip().partition(" ")
            res["versions"][name] = ver
        t = time.time()
        v, lbest, lruns = harness(b + "/bench_lzma", files, a.lzma_rounds, ("slz", "xz"), log)
        timing["lzma2"] = round(time.time() - t, 1)
        res["versions"]["liblzma"] = v.partition(" ")[2]
    res["decode"] = {"corpus": "Silesia", "files": len(files), "bytes": size,
                     "deflate": best, "lzma2": lbest, "rounds": {"deflate": runs, "lzma2": lruns},
                     "best_of": {"deflate": f"5 per process, {a.rounds} processes",
                                 "lzma2": f"3 per process, {a.lzma_rounds} process" + ("es" if a.lzma_rounds > 1 else "")}}

    if not a.no_e2e:
        t = time.time()
        env = dict(os.environ, SLZ_CORPUS=a.corpus, SZ_BIN=a.deps + "/bin/7zz",
                   BSDTAR_BIN=a.deps + "/bin/bsdtar", PIGZ_BIN=a.deps + "/bin/pigz")
        e2e_json = os.path.join(a.out, "e2e.json")
        cmd = [sys.executable, os.path.join(ROOT, "tests", "bench.py"), b + "/slz", a.work,
               "--sets", "silesia,500k", "--runs", str(a.runs), "--json", e2e_json]
        md = sh(cmd, env=env, stdout=subprocess.PIPE, text=True).stdout
        print(md, flush=True)
        with open(os.path.join(a.out, "e2e.md"), "w") as f:
            f.write(md)
        with open(e2e_json) as f:
            res["e2e"] = json.load(f)
        res["versions"].update(res["e2e"].pop("versions", {}))
        timing["e2e"] = round(time.time() - t, 1)

    e = os.environ
    commit = e.get("GITHUB_SHA") or git("rev-parse", "HEAD")
    ct = git("show", "-s", "--format=%ct", commit)
    res.update({
        "commit": commit,
        "commit_time": utc(int(ct)) if ct.isdigit() else utc(time.time()),
        "subject": git("show", "-s", "--format=%s", commit),
        "ref": e.get("GITHUB_REF") or "refs/heads/" + git("rev-parse", "--abbrev-ref", "HEAD"),
        "event": e.get("GITHUB_EVENT_NAME", "local"),
        "run_id": e.get("GITHUB_RUN_ID", ""),
        "run_attempt": e.get("GITHUB_RUN_ATTEMPT", ""),
        "run_url": f"{e['GITHUB_SERVER_URL']}/{e['GITHUB_REPOSITORY']}/actions/runs/{e['GITHUB_RUN_ID']}"
                   if e.get("GITHUB_RUN_ID") else "",
        "repository": e.get("GITHUB_REPOSITORY", ""),
        "measured_at": utc(time.time()),
        "env": environment(),
    })
    timing["total"] = round(time.time() - t0, 1)
    with open(os.path.join(a.out, "result.json"), "w") as f:
        json.dump(res, f, indent=1, sort_keys=True)
    print(json.dumps({"decode": {"deflate": best, "lzma2": lbest}, "timing": timing}, indent=1))


if __name__ == "__main__":
    main()
