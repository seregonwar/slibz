# slz benchmarks

Written by [`.github/workflows/bench.yml`](https://github.com/seregonwar/slibz/blob/main/.github/workflows/bench.yml) after every push to `main` that touches the code; the [README](https://github.com/seregonwar/slibz#readme) embeds the banners and badges from this branch and [docs/benchmarks.md](https://github.com/seregonwar/slibz/blob/main/docs/benchmarks.md) explains them. Generated files: don't edit them by hand.

**Newest run:** [`df9dd34`](https://github.com/seregonwar/slibz/commit/df9dd34373176ae18f52f101332245471c9c9c52) docs: move the details out of the README into docs/ · measured 2026-11-13 11:44:21 UTC · [workflow run](https://github.com/seregonwar/slibz/actions/runs/43418578760)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/seregonwar/slibz/bench-data/svg/hero-dark.svg">
  <img alt="DEFLATE decode (higher is better): slz 657 MB/s, libdeflate 1103 MB/s, zlib-ng 796 MB/s, zlib 462 MB/s; LZMA2 decode (higher is better): slz 89 MB/s, xz 152 MB/s; list 500k zip entries (lower is better): slz 96 ms, unzip 861 ms, Python 1953 ms, bsdtar 2813 ms, 7-Zip 4053 ms; extract silesia.zip (lower is better): slz 0.83 s, slz -j 1 0.82 s, bsdtar 0.35 s, Python 0.69 s, 7-Zip 0.82 s, unzip 1.15 s; extract silesia.tar.gz (lower is better): slz 0.37 s, bsdtar 0.38 s, pigz | tar 0.42 s, GNU tar 1.09 s; extract silesia-nonsolid.7z (lower is better): slz 0.89 s, slz -j 1 2.53 s, 7-Zip 1.57 s, bsdtar 1.59 s; extract silesia.7z (solid) (lower is better): slz 2.48 s, slz -j 1 2.47 s, 7-Zip 1.05 s, bsdtar 1.52 s." src="https://raw.githubusercontent.com/seregonwar/slibz/bench-data/svg/hero-light.svg">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/seregonwar/slibz/bench-data/svg/trend-dark.svg">
  <img alt="slz decoder throughput over time" src="https://raw.githubusercontent.com/seregonwar/slibz/bench-data/svg/trend-light.svg">
</picture>

## Newest run

GitHub Actions (Ubuntu 24.04.5 LTS, image 20261111.370.1; AMD EPYC 9V74, 4 threads), gcc 13.3.0.

| workload | slz | the others, same job |
|---|---:|---|
| DEFLATE decode (MB/s, higher is better) | **657 MB/s** | libdeflate 1103 MB/s · zlib-ng 796 MB/s · zlib 462 MB/s |
| LZMA2 decode (MB/s, higher is better) | **89 MB/s** | xz 152 MB/s |
| list 500k zip entries (lower is better) | **96 ms** | unzip 861 ms · Python 1953 ms · bsdtar 2813 ms · 7-Zip 4053 ms |
| extract silesia.zip (lower is better) | **0.83 s** · slz -j 1: 0.82 s | bsdtar 0.35 s · Python 0.69 s · 7-Zip 0.82 s · unzip 1.15 s |
| extract silesia.tar.gz (lower is better) | **0.37 s** | bsdtar 0.38 s · pigz \| tar 0.42 s · GNU tar 1.09 s |
| extract silesia-nonsolid.7z (lower is better) | **0.89 s** · slz -j 1: 2.53 s | 7-Zip 1.57 s · bsdtar 1.59 s |
| extract silesia.7z (solid) (lower is better) | **2.48 s** · slz -j 1: 2.47 s | 7-Zip 1.05 s · bsdtar 1.52 s |

Versions: zlib 1.4, zlib-ng 2.3.3, libdeflate 1.26, xz 5.8.4, 7-Zip 26.06, bsdtar 3.8.11, unzip 6.00, GNU tar 1.35, pigz 2.8, Python 3.12.3.

## History

slz's value, then how many times faster it is than the format's standard tool in the same job (zlib, xz, unzip, GNU tar, 7-Zip). Newest first.

| measured | commit | DEFLATE decode, vs zlib | LZMA2 decode, vs xz | list 500k zip entries, vs unzip | extract silesia.zip, vs unzip | extract silesia.tar.gz, vs GNU tar | extract silesia-nonsolid.7z, vs 7-Zip | extract silesia.7z (solid), vs 7-Zip | CPU |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| [2026-11-13](https://github.com/seregonwar/slibz/actions/runs/43418578760) | [`df9dd34`](https://github.com/seregonwar/slibz/commit/df9dd34373176ae18f52f101332245471c9c9c52) | 657 · 1.42× | 89 · 0.59× | 96 · 9.0× | 0.83 · 1.39× | 0.37 · 3.0× | 0.89 · 1.8× | 2.48 · 0.42× | AMD EPYC 9V74 |

## Files

- `history.json`: every run on main, oldest first (the last 1000), as written by `.github/bench/run.py`
- `latest.json`: the newest run
- `svg/`: the banners, light and dark
- `badges/`: [shields.io endpoint](https://shields.io/badges/endpoint-badge) badges
