#!/usr/bin/env python3
# slz - Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
"""Benchmark report of .github/workflows/bench.yml (Python 3.8+, stdlib only).

    report.py stage RESULT --out DIR [--history FILE]
    report.py publish RESULT --site DIR [--preview]

RESULT is the result.json of .github/bench/run.py. FILE and DIR/history.json hold the runs on main
(history.json of the bench-data branch).

stage writes DIR/summary.md (the job summary: this run, slz's speed relative to the competitors
measured in the same job, and the change against the last run on main) and prints a ::warning::
annotation for each change beyond the noise threshold. Hosted runners are noisy: it never fails.

publish adds RESULT to DIR/history.json and renders the newest run (RESULT itself with --preview):
DIR/svg/{hero,trend}-{light,dark}.svg (README banners), DIR/badges/*.json (shields.io endpoint
badges), DIR/latest.json and DIR/README.md (dashboard). The workflow commits DIR to the bench-data
branch on pushes to main only; elsewhere it uploads it as a preview artifact."""
import argparse, html, json, math, os, re, statistics, sys

HISTORY_MAX, TREND_RUNS, RECENT = 1000, 60, 20
FONT = "-apple-system,BlinkMacSystemFont,'Segoe UI','Noto Sans',Helvetica,Arial,sans-serif"
THEME = {  # GitHub Primer colors: the banners sit on github.com's light and dark backgrounds
    "light": {"fg": "#1f2328", "muted": "#59636e", "accent": "#0969da", "bar": "#afb8c1", "win": "#1a7f37",
              "loss": "#cf222e", "line": "#d1d9e0", "card": "#f6f8fa"},
    "dark": {"fg": "#f0f6fc", "muted": "#9198a1", "accent": "#4493f8", "bar": "#656c76", "win": "#3fb950",
             "loss": "#f85149", "line": "#3d444d", "card": "#151b23"},
}


def workload(key, title, name, src, unit, ref, rows):
    return {"key": key, "title": title, "name": name, "src": src, "unit": unit, "ref": ref, "rows": rows}


# rows: (column of the result, label), slz first. ref: the format's standard tool, used by the badges,
# the summary and the regression check (slz's speed relative to a tool measured in the same job).
SEVENZ = [("slz x", "slz"), ("slz x -j 1", "slz -j 1"), ("7z x", "7-Zip"), ("bsdtar -x", "bsdtar")]
WORKLOADS = [
    workload("deflate", "DEFLATE", "DEFLATE decode", ("decode", "deflate"), "MB/s", "zlib",
             [("slz", "slz"), ("libdeflate", "libdeflate"), ("zlib-ng", "zlib-ng"), ("zlib", "zlib")]),
    workload("lzma2", "LZMA2", "LZMA2 decode", ("decode", "lzma2"), "MB/s", "xz", [("slz", "slz"), ("xz", "xz")]),
    workload("list", "List 500k-entry zip", "list 500k zip entries", ("e2e", "list", "500k entries"), "ms",
             "unzip -Z1", [("slz", "slz"), ("unzip -Z1", "unzip"), ("bsdtar -t", "bsdtar"), ("7z l", "7-Zip"),
                           ("python zipfile", "Python")]),
    workload("zip", "Extract silesia.zip", "extract silesia.zip", ("e2e", "zip", "silesia.zip"), "s", "unzip",
             [("slz", "slz"), ("slz -j 1", "slz -j 1"), ("unzip", "unzip"), ("bsdtar", "bsdtar"), ("7z x", "7-Zip"),
              ("python zipfile", "Python")]),
    workload("targz", "Extract silesia.tar.gz", "extract silesia.tar.gz", ("e2e", "targz", "silesia.tar.gz"), "s",
             "GNU tar -xzf", [("slz", "slz"), ("GNU tar -xzf", "GNU tar"), ("pigz -d | tar -x", "pigz | tar"),
                              ("bsdtar", "bsdtar")]),
    workload("7z", "Extract 7z, non-solid", "extract silesia-nonsolid.7z", ("e2e", "7z", "silesia-nonsolid.7z"), "s",
             "7z x", SEVENZ),
    workload("7zsolid", "Extract 7z, solid", "extract silesia.7z (solid)", ("e2e", "7z", "silesia.7z"), "s", "7z x",
             SEVENZ),
]
WL = {w["key"]: w for w in WORKLOADS}


# ---------------------------------------------------------------- data

def values(e, w):
    """{column: value} of workload w in a run: seconds, or MB/s for the decoders"""
    d = e
    for k in w["src"]:
        d = (d.get(k) or {}) if isinstance(d, dict) else {}
    return {c: float(d[c]) for c, _ in w["rows"] if isinstance(d.get(c), (int, float)) and d[c] > 0}


def speedup(w, v, col):
    """how many times faster slz is than `col` (below 1: slower); None when either is missing"""
    s, o = v.get(w["rows"][0][0]), v.get(col)
    if not s or not o:
        return None
    return s / o if w["unit"] == "MB/s" else o / s


def label(w, col):
    return next(l for c, l in w["rows"] if c == col)


def ordered(w, v):
    """slz rows first, then the other tools from fastest to slowest"""
    rest = sorted((r for r in w["rows"] if r[0] in v and not r[1].startswith("slz")),
                  key=lambda r: -v[r[0]] if w["unit"] == "MB/s" else v[r[0]])
    return [r for r in w["rows"] if r[0] in v and r[1].startswith("slz")] + rest


def verdict(w, v):
    """(color, text): slz against the fastest other tool"""
    rest = [r for r in w["rows"] if r[0] in v and not r[1].startswith("slz")]
    if not rest or w["rows"][0][0] not in v:
        return None
    col, name = min(rest, key=lambda r: speedup(w, v, r[0]))
    r = speedup(w, v, col)
    if abs(r - 1) < 0.02:
        return "muted", f"on par with {name}"
    return ("win", f"{times(r)} faster than {name}") if r > 1 else ("loss", f"{times(1 / r)} slower than {name}")


def times(r):
    return f"{r:.0f}×" if r >= 10 else f"{r:.1f}×" if r >= 1.5 else f"{r:.2f}×"


def fmt(w, x, unit=True):
    if w["unit"] == "MB/s":
        s, u = f"{x:.0f}", "MB/s"
    elif w["unit"] == "ms":
        s, u = (f"{x * 1e3:.0f}" if x >= 0.01 else f"{x * 1e3:.1f}"), "ms"
    else:
        s, u = (f"{x:.2f}" if x < 10 else f"{x:.1f}"), "s"
    return f"{s} {u}" if unit else s


def cpu_name(s):
    s = re.sub(r"\((R|TM)\)|\s+CPU\b|\s*@.*$|\s+Processor\b|\s+\d+-Core\b", "", s or "?")
    return re.sub(r"\s+", " ", s).strip()


def machine(env):
    image = (env.get("image") or "").split(" ")[-1]
    osname = (env.get("os") or "?") + (f", image {image}" if image else "")
    return f"{env.get('runner') or '?'} ({osname}; {cpu_name(env.get('cpu'))}, {env.get('threads') or '?'} threads)"


def versions(e):
    v = e.get("versions") or {}
    out = []
    for key, name in (("zlib", "zlib"), ("zlib-ng", "zlib-ng"), ("libdeflate", "libdeflate"), ("liblzma", "xz"),
                      ("7-Zip", "7-Zip"), ("bsdtar", "bsdtar"), ("unzip", "unzip"), ("tar", "GNU tar"),
                      ("pigz", "pigz"), ("python", "Python")):
        m = re.search(r"\d+\.\d+(?:\.\d+)*", v.get(key) or "")
        if m:
            out.append(f"{name} {m.group(0)}")
    return ", ".join(out)


def describe(e):
    """every number of the hero, for <desc> and alt texts"""
    out = []
    for w in WORKLOADS:
        v = values(e, w)
        if v:
            better = "higher" if w["unit"] == "MB/s" else "lower"
            out.append(f"{w['name']} ({better} is better): " + ", ".join(f"{l} {fmt(w, v[c])}" for c, l in ordered(w, v)))
    return "; ".join(out) + "."


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def load_history(path):
    if not path or not os.path.exists(path):
        return []
    h = load_json(path)
    return h.get("entries", []) if isinstance(h, dict) else h


def write(path, text):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def baseline(res, hist):
    """runs on main to compare with: other commits, on the same CPU model when there are any"""
    other = [h for h in hist if h.get("commit") != res.get("commit")]
    cpu = (res.get("env") or {}).get("cpu")
    same = [h for h in other if (h.get("env") or {}).get("cpu") == cpu]
    return (same, True) if same else (other, False)


def compare(res, hist):
    """{key: (ratio, ratio on main, change, threshold)} with ratio = slz's speed / the reference's speed"""
    pool, same_cpu = baseline(res, hist)
    if not pool:
        return {}, None, same_cpu
    base, out = pool[-1], {}
    for w in WORKLOADS:
        r, rb = speedup(w, values(res, w), w["ref"]), speedup(w, values(base, w), w["ref"])
        if not r or not rb:
            continue
        # noise threshold: 3 robust sigmas of the recent same-CPU ratios, at least 5%; 10% without them
        recent = [x for x in (speedup(w, values(h, w), w["ref"]) for h in pool[-RECENT:]) if x]
        thr = 0.10
        if same_cpu and len(recent) >= 5:
            logs = [math.log(x) for x in recent]
            m = statistics.median(logs)
            thr = max(0.05, math.exp(3 * 1.4826 * statistics.median([abs(x - m) for x in logs])) - 1)
        out[w["key"]] = (r, rb, r / rb - 1, thr)
    return out, base, same_cpu


# ---------------------------------------------------------------- SVG

def esc(s):
    return html.escape(str(s), quote=True)


def tw(s, size, bold=False):
    """rough text width: the system fonts are unknown, so the layout leaves slack"""
    return len(s) * size * (0.6 if bold else 0.56)


def wrap(text, size, width):
    lines, cur = [], ""
    for word in text.split(" "):
        t = f"{cur} {word}".strip()
        if cur and len(t) * size * 0.51 > width:  # prose averages about half an em per character
            lines.append(cur)
            cur = word
        else:
            cur = t
    return lines + ([cur] if cur else [])


class Svg:
    def __init__(self, theme):
        self.c, self.el = THEME[theme], []

    def text(self, x, y, s, size=12, fill="fg", weight=None, anchor=None):
        a = (f' font-weight="{weight}"' if weight else "") + (f' text-anchor="{anchor}"' if anchor else "")
        self.el.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{self.c[fill]}"{a}>{esc(s)}</text>')

    def spans(self, x, y, size, parts):
        """one line of differently styled runs: [(text, color, weight)]"""
        parts = [p for p in parts if p[0]]
        t = "".join(f'<tspan fill="{self.c[f]}"' + (' dx="6"' if i else "") + (f' font-weight="{wt}"' if wt else "")
                    + f">{esc(s)}</tspan>" for i, (s, f, wt) in enumerate(parts))
        self.el.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}">{t}</text>')

    def rect(self, x, y, w, h, fill, rx=0, stroke=None, opacity=None, tip=None):
        a = (f' rx="{rx}"' if rx else "") + (f' stroke="{self.c[stroke]}"' if stroke else "")
        a += f' fill-opacity="{opacity}"' if opacity is not None else ""
        r = f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" fill="{self.c[fill]}"{a}'
        self.el.append(r + (f"><title>{esc(tip)}</title></rect>" if tip else "/>"))

    def line(self, x1, y1, x2, y2, stroke, width=1, dash=None):
        a = f' stroke-dasharray="{dash}"' if dash else ""
        self.el.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{self.c[stroke]}" '
                       f'stroke-width="{width}"{a}/>')

    def polyline(self, pts, stroke, width):
        p = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
        self.el.append(f'<polyline points="{p}" fill="none" stroke="{self.c[stroke]}" stroke-width="{width}" '
                       f'stroke-linejoin="round" stroke-linecap="round"/>')

    def dot(self, x, y, r, fill, tip=None):
        c = f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="{self.c[fill]}"'
        self.el.append(c + (f"><title>{esc(tip)}</title></circle>" if tip else "/>"))

    def render(self, w, h, title, desc):
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h:.0f}" viewBox="0 0 {w} {h:.0f}" '
                f'role="img" aria-labelledby="title desc">\n<title id="title">{esc(title)}</title>\n'
                f'<desc id="desc">{esc(desc)}</desc>\n'
                f'<style>text{{font-family:{FONT};font-variant-numeric:tabular-nums}}</style>\n'
                + "\n".join(self.el) + "\n</svg>\n")


W, GAP, PAD, ROW = 840, 12, 12, 16


def footer(d, y, lines):
    for text in lines:
        for ln in wrap(text, 11, W - 4):
            y += 15
            d.text(1, y, ln, 11, "muted")
    return y + 6


def hero(e, theme):
    """small multiples: listing and extraction wall times, then the in-memory decoders"""
    d = Svg(theme)
    pw = (W - 2 * GAP) / 3
    panels = [(WL[k]["title"], "wall time · lower is better", [("", WL[k], values(e, WL[k]))])
              for k in ("list", "zip", "targz", "7z", "7zsolid")]
    panels.append(("Decode in memory", "MB/s of output · higher is better",
                   [(WL[k]["title"], WL[k], values(e, WL[k])) for k in ("deflate", "lzma2")]))
    panels = [(t, s, [g for g in gs if g[1]["rows"][0][0] in g[2]]) for t, s, gs in panels]
    panels = [p for p in panels if p[2]]
    y = 0.5
    for i in range(0, len(panels), 3):
        row = panels[i:i + 3]
        h = max(50 + sum(18 + ROW * len(g[2]) for g in p[2]) + 6 * (len(p[2]) - 1) + 8 for p in row)
        for j, (title, sub, groups) in enumerate(row):
            x = 0.5 + j * (pw + GAP)
            d.rect(x, y, pw - 1, h, "card", rx=6, stroke="line")
            d.text(x + PAD, y + 22, title, 13, weight=600)
            d.text(x + PAD, y + 38, sub, 11, "muted")
            lw = max(tw(l, 12, l.startswith("slz")) for _, w, v in groups for c, l in w["rows"] if c in v) + 10
            vw = max(tw(fmt(w, val), 11, True) for _, w, v in groups for val in v.values()) + 6
            bx, bmax, cy = x + PAD + lw, pw - 1 - 2 * PAD - lw - vw, y + 50
            for name, w, v in groups:
                color, msg = verdict(w, v) or ("muted", "")
                d.spans(x + PAD, cy + 11, 11, [(name, "fg", 600), (msg, color, None if color == "muted" else 600)])
                cy += 18
                top = max(v.values())
                for c, l in ordered(w, v):
                    slz = l.startswith("slz")
                    bw = max(2.0, v[c] / top * bmax)
                    d.text(bx - 6, cy + 11, l, 12, "fg", 600 if slz else None, "end")
                    d.rect(bx, cy + 3, bw, 10, "accent" if slz else "bar", rx=2,
                           opacity=0.55 if slz and l != "slz" else None, tip=f"{l}: {fmt(w, v[c])}")
                    d.text(bx + bw + 5, cy + 11.5, fmt(w, v[c]), 11, "fg" if slz else "muted", 600 if slz else None)
                    cy += ROW
                cy += 6
        y += h + GAP
    env, e2e, dec = e.get("env") or {}, e.get("e2e") or {}, e.get("decode") or {}
    lines = [f"Measured {(e.get('measured_at') or '?')[:10]} by .github/workflows/bench.yml at slz "
             f"{(e.get('commit') or '?')[:7]} on {machine(env)}."]
    method = []
    if e2e:
        method.append(f"Archives: whole-process wall time, best of {e2e.get('runs')} runs, warm page cache; slz runs "
                      f"{e2e.get('threads')} threads, slz -j 1 one.")
    if dec:
        method.append(f"Decoders: in memory over the Silesia corpus ({(dec.get('bytes') or 0) / 1e6:.0f} MB), "
                      "best of several runs.")
    if method:
        lines.append(" ".join(method))
    vs = versions(e)
    if vs:
        lines.append(f"Versions: {vs}.")
    y = footer(d, y - GAP + 2, lines)
    return d.render(W, y, "slz benchmarks", describe(e) + " " + " ".join(lines))


def nice_step(x):
    e = 10 ** math.floor(math.log10(x))
    return next(m * e for m in (1, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10) if m * e >= x)


def trend(entries, theme):
    """MB/s of every decoder over the last runs on main: the competitors are fixed, so their lines
    show the runner's noise and slz's line shows the code's progress"""
    d, H = Svg(theme), 232
    pw, n = (W - GAP) / 2, len(entries)
    cpus = [(e.get("env") or {}).get("cpu") for e in entries]
    changes = [j for j in range(1, n) if cpus[j] != cpus[j - 1]]
    for i, w in enumerate((WL["deflate"], WL["lzma2"])):
        x0, y0 = 0.5 + i * (pw + GAP), 0.5
        d.rect(x0, y0, pw - 1, H, "card", rx=6, stroke="line")
        d.text(x0 + PAD, y0 + 22, f"{w['title']} decode, MB/s", 13, weight=600)
        d.text(x0 + PAD, y0 + 38, f"last {n} run{'s' if n != 1 else ''} on main · higher is better", 11, "muted")
        L, R, T, B = x0 + 44, x0 + pw - 112, y0 + 56, y0 + H - 30
        series = [(c, l, [values(e, w).get(c) for e in entries]) for c, l in w["rows"]]
        flat = [x for _, _, s in series for x in s if x]
        if not flat:
            d.text(L, (T + B) / 2, "no data yet", 12, "muted")
            continue
        step = nice_step(max(flat) * 1.04 / 4)
        for k in range(5):
            gy = B - (B - T) * k / 4
            d.line(L, gy, R, gy, "line", 1)
            d.text(L - 6, gy + 4, f"{step * k:.0f}", 10, "muted", anchor="end")

        def X(j):
            return L + (R - L) * (j / (n - 1) if n > 1 else 0.5)

        def Y(val):
            return B - (B - T) * val / (4 * step)

        for j in changes:
            d.line((X(j) + X(j - 1)) / 2, T - 4, (X(j) + X(j - 1)) / 2, B, "muted", 1, "3 3")
        ends = []
        for c, l, s in sorted(series, key=lambda t: t[1] == "slz"):  # slz drawn last, on top
            slz = l == "slz"
            pts = [(X(j), Y(val)) for j, val in enumerate(s) if val]
            if not pts:
                continue
            color = "accent" if slz else "bar"
            if len(pts) > 1:
                d.polyline(pts, color, 2 if slz else 1.25)
            for j, val in enumerate(s):
                if val and (n <= 20 or j == n - 1):
                    d.dot(X(j), Y(val), 2.5 if slz else 2, color,
                          f"{l} {val:.0f} MB/s at {(entries[j].get('commit') or '?')[:7]}")
            ends.append([pts[-1][1], f"{l} {next(v for v in reversed(s) if v):.0f}", slz])
        ends.sort(key=lambda t: t[0])
        for k in range(1, len(ends)):
            ends[k][0] = max(ends[k][0], ends[k - 1][0] + 13)
        shift = max(0.0, ends[-1][0] - B - 4)
        for yy, text, slz in ends:
            d.text(R + 8, yy - shift + 4, text, 11, "fg" if slz else "muted", 600 if slz else None)
        first, last = entries[0], entries[-1]
        d.text(L if n > 1 else X(0), B + 16, f"{(first.get('commit') or '?')[:7]} · {(first.get('commit_time') or '')[:10]}",
               10, "muted", anchor=None if n > 1 else "middle")
        if n > 1:
            d.text(R, B + 16, f"{(last.get('commit') or '?')[:7]} · {(last.get('commit_time') or '')[:10]}", 10, "muted",
                   anchor="end")
    note = ("Each point is one run on main (a push or a manual run), measured on a GitHub-hosted runner. The "
            "competitors run in the same job, so a shift shared by every line comes from the runner, not from slz.")
    if changes:
        note += " Dashed lines: the runner's CPU model changed."
    y = footer(d, H + 2, [note])
    return d.render(W, y, "slz decoder throughput over time", note)


# ---------------------------------------------------------------- Markdown and badges

def md(s):
    return (s or "").replace("|", "\\|").replace("<", "&lt;").replace("\n", " ")


def ratio_cell(w, v, col):
    r = speedup(w, v, col)
    return f"{md(label(w, col))} {fmt(w, v[col])} · {times(r)}" if r else "-"


def badges(e):
    out = {}
    for w in WORKLOADS:
        v = values(e, w)
        s, r = v.get(w["rows"][0][0]), v.get(w["ref"])
        if s:
            msg = fmt(w, s) + (f" · {label(w, w['ref'])} {fmt(w, r, unit=False)}" if r else "")
            out[w["key"]] = {"schemaVersion": 1, "label": w["name"], "message": msg, "color": "0969da",
                             "cacheSeconds": 600}
    out["measured"] = {"schemaVersion": 1, "label": "benchmarked", "color": "lightgrey", "cacheSeconds": 600,
                       "message": f"{(e.get('commit') or '?')[:7]} · {(e.get('measured_at') or '?')[:10]}"}
    return out


def results_table(e):
    out = ["| workload | slz | the others, same job |", "|---|---:|---|"]
    for w in WORKLOADS:
        v = values(e, w)
        rows = ordered(w, v)
        if not rows or not rows[0][1].startswith("slz"):
            continue
        better = "MB/s, higher is better" if w["unit"] == "MB/s" else "lower is better"
        slz = " · ".join(f"**{fmt(w, v[c])}**" if l == "slz" else f"{l}: {fmt(w, v[c])}"
                         for c, l in rows if l.startswith("slz"))
        rest = " · ".join(f"{md(l)} {fmt(w, v[c])}" for c, l in rows if not l.startswith("slz"))
        out.append(f"| {w['name']} ({better}) | {slz} | {rest} |")
    return out


def summary(res, hist, result_dir=None):
    cmp_, base, same_cpu = compare(res, hist)
    env, t = res.get("env") or {}, res.get("timing") or {}
    lines = [f"## slz benchmarks · `{(res.get('commit') or '?')[:7]}`", "",
             f"{md(res.get('subject'))} · `{res.get('ref', '')}` · {machine(env)} · {env.get('cc', '')} · "
             f"build {t.get('build', '-')} s, DEFLATE {t.get('deflate', '-')} s, LZMA2 {t.get('lzma2', '-')} s, "
             f"archives {t.get('e2e', '-')} s", ""]
    flagged = [k for k, (_, _, ch, thr) in cmp_.items() if ch < -thr]
    if base is None:
        lines += ["No run on main to compare with yet (bench-data branch).", ""]
    else:
        where = "same CPU model" if same_cpu else f"**different CPU** ({cpu_name((base.get('env') or {}).get('cpu'))})"
        state = (f"⚠️ {len(flagged)} possible regression{'s' if len(flagged) > 1 else ''}" if flagged
                 else "✅ no change beyond the noise threshold")
        lines += [f"{state} against main at `{(base.get('commit') or '?')[:7]}` ({where}).", ""]
    lines += ["| workload | slz | standard tool, slz speed | fastest other, slz speed | vs last main run |",
              "|---|---:|---|---|---|"]
    for w in WORKLOADS:
        v = values(res, w)
        if w["rows"][0][0] not in v:
            continue
        rest = [r for r in w["rows"] if r[0] in v and not r[1].startswith("slz")]
        fastest = min(rest, key=lambda r: speedup(w, v, r[0]))[0] if rest else None
        c = cmp_.get(w["key"])
        delta = "-"
        if c:
            r, rb, ch, thr = c
            mark = " ⚠️" if ch < -thr else " 🚀" if ch > thr else ""
            delta = f"{rb:.2f}× → {r:.2f}× ({ch:+.1%}, ±{thr:.0%}){mark}"
        lines.append(f"| {w['name']} | **{fmt(w, v[w['rows'][0][0]])}** | {ratio_cell(w, v, w['ref'])} | "
                     f"{ratio_cell(w, v, fastest) if fastest else '-'} | {delta} |")
    lines += ["", "slz speed: how many times faster than that tool (below 1× slz is slower), both measured in this "
              "job. The last column compares it with the last run on main; ⚠️ marks a drop beyond the noise "
              "threshold (3 robust sigmas of recent main runs on this CPU model, at least 5%; 10% without them). "
              "Hosted runners are noisy: re-run the job before trusting a single warning.", "",
              "<details><summary>All numbers</summary>", ""] + results_table(res)
    vs = versions(res)
    lines += ["", f"Versions: {vs}." if vs else ""]
    for name in ("decoders.txt", "e2e.md"):
        p = os.path.join(result_dir or "", name)
        if result_dir and os.path.exists(p):
            with open(p, encoding="utf-8", errors="replace") as f:
                body = f.read().strip()
            lines += ["", f"`{name}`", "", "```" if name.endswith(".txt") else "", body, "```" if name.endswith(".txt") else ""]
    lines += ["", "</details>", ""]
    return "\n".join(lines), [(WL[k], cmp_[k], base) for k in flagged], cmp_


def annotate(kind, title, msg):
    def e(s, prop=False):
        s = str(s).replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        return s.replace(":", "%3A").replace(",", "%2C") if prop else s
    print(f"::{kind} title={e(title, True)}::{e(msg)}", flush=True)


def picture(path, alt, width=None):
    w = f' width="{width}"' if width else ""
    return (f'<picture>\n  <source media="(prefers-color-scheme: dark)" srcset="{path}-dark.svg">\n'
            f'  <img alt="{esc(alt)}" src="{path}-light.svg"{w}>\n</picture>')


def dashboard(hist, latest):
    repo = latest.get("repository") or "seregonwar/slibz"
    server = (latest.get("run_url") or "https://github.com/").split("/" + repo)[0].rstrip("/") or "https://github.com"
    base, sha = f"{server}/{repo}", latest.get("commit") or ""
    # absolute image URLs: GitHub rewrites a relative <img src> but not necessarily a <source srcset>
    raw = (f"https://raw.githubusercontent.com/{repo}/bench-data" if server == "https://github.com"
           else f"{base}/raw/bench-data")
    run = f" · [workflow run]({latest['run_url']})" if latest.get("run_url") else ""
    out = ["# slz benchmarks", "",
           f"Written by [`.github/workflows/bench.yml`]({base}/blob/main/.github/workflows/bench.yml) after every push "
           f"to `main` that touches the code; the [README]({base}#continuous-benchmarks) embeds the banners and "
           "badges from this branch. Generated files: don't edit them by hand.", "",
           f"**Newest run:** [`{sha[:7]}`]({base}/commit/{sha}) {md(latest.get('subject'))} · measured "
           f"{(latest.get('measured_at') or '?').replace('T', ' ').rstrip('Z')} UTC{run}", "",
           picture(f"{raw}/svg/hero", describe(latest)), "",
           picture(f"{raw}/svg/trend", "slz decoder throughput over time"), "",
           "## Newest run", "", f"{machine(latest.get('env') or {})}, {(latest.get('env') or {}).get('cc', '')}.", ""]
    out += results_table(latest)
    vs = versions(latest)
    out += ["", f"Versions: {vs}." if vs else "", "", "## History", "",
            "slz's value, then how many times faster it is than the format's standard tool in the same job "
            "(zlib, xz, unzip, GNU tar, 7-Zip). Newest first.", "",
            "| measured | commit | " + " | ".join(f"{w['name']}, vs {md(label(w, w['ref']))}" for w in WORKLOADS) + " | CPU |",
            "|---|---|" + "---:|" * len(WORKLOADS) + "---|"]
    for h in reversed(hist[-30:]):
        cells = []
        for w in WORKLOADS:
            v = values(h, w)
            r = speedup(w, v, w["ref"])
            cells.append(f"{fmt(w, v[w['rows'][0][0]], unit=False)} · {times(r)}" if r else "-")
        c = h.get("commit") or ""
        when = (h.get("measured_at") or "?")[:10]
        when = f"[{when}]({h['run_url']})" if h.get("run_url") else when
        out.append(f"| {when} | [`{c[:7]}`]({base}/commit/{c}) | " + " | ".join(cells)
                   + f" | {cpu_name((h.get('env') or {}).get('cpu'))} |")
    out += ["", "## Files", "",
            f"- `history.json`: every run on main, oldest first (the last {HISTORY_MAX}), as written by "
            "`.github/bench/run.py`",
            "- `latest.json`: the newest run", "- `svg/`: the banners, light and dark",
            "- `badges/`: [shields.io endpoint](https://shields.io/badges/endpoint-badge) badges", ""]
    return "\n".join(out)


# ---------------------------------------------------------------- commands

def stage(a):
    res = load_json(a.result)
    text, flagged, cmp_ = summary(res, load_history(a.history), os.path.dirname(os.path.abspath(a.result)))
    write(os.path.join(a.out, "summary.md"), text)
    for w, (r, rb, ch, thr), base in flagged:
        annotate("warning", f"Possible regression: {w['name']}",
                 f"slz vs {label(w, w['ref'])}: {times(rb)} on main ({(base.get('commit') or '?')[:7]}) -> {times(r)} "
                 f"here, {ch:+.1%} (noise threshold {thr:.0%}). Hosted runners are noisy: re-run to confirm.")
    for k, (r, rb, ch, thr) in cmp_.items():
        if ch > thr:
            annotate("notice", f"Faster: {WL[k]['name']}",
                     f"slz vs {label(WL[k], WL[k]['ref'])}: {times(rb)} on main -> {times(r)} here, {ch:+.1%}.")


def publish(a):
    res = load_json(a.result)
    path = os.path.join(a.site, "history.json")
    key = (res.get("commit"), res.get("measured_at"))
    hist = [h for h in load_history(path) if (h.get("commit"), h.get("measured_at")) != key] + [res]
    hist.sort(key=lambda h: (h.get("commit_time") or "", h.get("measured_at") or ""))
    hist = hist[-HISTORY_MAX:]
    write(path, '{"schema": 1, "entries": [\n'
          + ",\n".join(json.dumps(h, sort_keys=True, separators=(",", ":"), ensure_ascii=False) for h in hist)
          + "\n]}\n")
    latest = res if a.preview else hist[-1]
    write(os.path.join(a.site, "latest.json"), json.dumps(latest, indent=1, sort_keys=True, ensure_ascii=False) + "\n")
    for theme in THEME:
        write(os.path.join(a.site, "svg", f"hero-{theme}.svg"), hero(latest, theme))
        write(os.path.join(a.site, "svg", f"trend-{theme}.svg"), trend(hist[-TREND_RUNS:], theme))
    for name, b in badges(latest).items():
        write(os.path.join(a.site, "badges", f"{name}.json"), json.dumps(b) + "\n")
    write(os.path.join(a.site, "README.md"), dashboard(hist, latest))
    print(f"{a.site}: {len(hist)} runs, newest {(latest.get('commit') or '?')[:7]}")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stage", help="job summary and regression warnings")
    s.add_argument("result")
    s.add_argument("--out", required=True)
    s.add_argument("--history", help="history.json of the bench-data branch (runs on main)")
    q = sub.add_parser("publish", help="history, banners, badges and dashboard")
    q.add_argument("result")
    q.add_argument("--site", required=True, help="bench-data checkout")
    q.add_argument("--preview", action="store_true", help="render RESULT even when older than the newest run")
    a = p.parse_args()
    stage(a) if a.cmd == "stage" else publish(a)


if __name__ == "__main__":
    sys.exit(main())
