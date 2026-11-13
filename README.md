# slz benchmarks

Written by [`.github/workflows/bench.yml`](https://github.com/seregonwar/slibz/blob/main/.github/workflows/bench.yml) after every push to `main` that touches the code; the [README](https://github.com/seregonwar/slibz#readme) embeds the banners and badges from this branch and [docs/benchmarks.md](https://github.com/seregonwar/slibz/blob/main/docs/benchmarks.md) explains them. Generated files: don't edit them by hand.

**Newest run:** [`6a39a2e`](https://github.com/seregonwar/slibz/commit/6a39a2e2a6751aa40db974ebdc94d05b5be21d62) Add native .slz archive format · measured 2026-11-13 16:54:36 UTC · [workflow run](https://github.com/seregonwar/slibz/actions/runs/43466750365)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/seregonwar/slibz/bench-data/svg/hero-dark.svg">
  <img alt="DEFLATE decode (higher is better): slz 799 MB/s, libdeflate 926 MB/s, zlib-ng 701 MB/s, zlib 398 MB/s; LZMA2 decode (higher is better): slz 94 MB/s, xz 130 MB/s; list 500k zip entries (lower is better): slz 117 ms, unzip 670 ms, Python 2277 ms, bsdtar 3114 ms, 7-Zip 4683 ms; extract silesia.zip (lower is better): slz 0.12 s, slz -j 1 0.33 s, bsdtar 0.40 s, Python 0.83 s, 7-Zip 0.98 s, unzip 1.33 s; extract silesia.tar.gz (lower is better): slz 0.33 s, bsdtar 0.44 s, pigz | tar 0.58 s, GNU tar 1.24 s; extract silesia-nonsolid.7z (lower is better): slz 1.06 s, slz -j 1 2.52 s, 7-Zip 1.89 s, bsdtar 1.91 s; extract silesia.7z (solid) (lower is better): slz 2.44 s, slz -j 1 2.43 s, 7-Zip 1.24 s, bsdtar 1.86 s." src="https://raw.githubusercontent.com/seregonwar/slibz/bench-data/svg/hero-light.svg">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/seregonwar/slibz/bench-data/svg/trend-dark.svg">
  <img alt="slz decoder throughput over time" src="https://raw.githubusercontent.com/seregonwar/slibz/bench-data/svg/trend-light.svg">
</picture>

## Newest run

GitHub Actions (Ubuntu 24.04.5 LTS, image 20261111.370.1; AMD EPYC 7763, 4 threads), gcc 13.3.0.

| workload | slz | the others, same job |
|---|---:|---|
| DEFLATE decode (MB/s, higher is better) | **799 MB/s** | libdeflate 926 MB/s · zlib-ng 701 MB/s · zlib 398 MB/s |
| LZMA2 decode (MB/s, higher is better) | **94 MB/s** | xz 130 MB/s |
| list 500k zip entries (lower is better) | **117 ms** | unzip 670 ms · Python 2277 ms · bsdtar 3114 ms · 7-Zip 4683 ms |
| extract silesia.zip (lower is better) | **0.12 s** · slz -j 1: 0.33 s | bsdtar 0.40 s · Python 0.83 s · 7-Zip 0.98 s · unzip 1.33 s |
| extract silesia.tar.gz (lower is better) | **0.33 s** | bsdtar 0.44 s · pigz \| tar 0.58 s · GNU tar 1.24 s |
| extract silesia-nonsolid.7z (lower is better) | **1.06 s** · slz -j 1: 2.52 s | 7-Zip 1.89 s · bsdtar 1.91 s |
| extract silesia.7z (solid) (lower is better) | **2.44 s** · slz -j 1: 2.43 s | 7-Zip 1.24 s · bsdtar 1.86 s |

Versions: zlib 1.4, zlib-ng 2.3.3, libdeflate 1.26, xz 5.8.4, 7-Zip 26.06, bsdtar 3.8.11, unzip 6.00, GNU tar 1.35, pigz 2.8, Python 3.12.3.

## History

slz's value, then how many times faster it is than the format's standard tool in the same job (zlib, xz, unzip, GNU tar, 7-Zip). Newest first.

| measured | commit | DEFLATE decode, vs zlib | LZMA2 decode, vs xz | list 500k zip entries, vs unzip | extract silesia.zip, vs unzip | extract silesia.tar.gz, vs GNU tar | extract silesia-nonsolid.7z, vs 7-Zip | extract silesia.7z (solid), vs 7-Zip | CPU |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| [2026-11-13](https://github.com/seregonwar/slibz/actions/runs/43466750365) | [`6a39a2e`](https://github.com/seregonwar/slibz/commit/6a39a2e2a6751aa40db974ebdc94d05b5be21d62) | 799 · 2.0× | 94 · 0.72× | 117 · 5.7× | 0.12 · 11× | 0.33 · 3.8× | 1.06 · 1.8× | 2.44 · 0.51× | AMD EPYC 7763 |
| [2026-11-13](https://github.com/seregonwar/slibz/actions/runs/43455055038) | [`0464858`](https://github.com/seregonwar/slibz/commit/0464858b09d82798e6bb976d38ff2def22d3a73f) | 1040 · 2.1× | 128 · 0.81× | 74 · 6.3× | 0.83 · 1.32× | 0.27 · 3.7× | 0.79 · 2.0× | 1.80 · 0.57× | AMD EPYC 9V45 |
| [2026-11-13](https://github.com/seregonwar/slibz/actions/runs/43455015153) | [`4d6057c`](https://github.com/seregonwar/slibz/commit/4d6057c3c432279909f7525c1b5bca53b30d6328) | 768 · 1.9× | 87 · 0.74× | 121 · 5.5× | 0.12 · 11× | 0.33 · 3.8× | 1.09 · 1.8× | 2.48 · 0.53× | AMD EPYC 7763 |
| [2026-11-13](https://github.com/seregonwar/slibz/actions/runs/43427918493) | [`2203d38`](https://github.com/seregonwar/slibz/commit/2203d38ce1b76889583d88f465c81754ad992bd0) | 973 · 2.1× | 89 · 0.58× | 96 · 8.5× | 0.86 · 1.35× | 0.27 · 4.0× | 0.89 · 1.8× | 2.46 · 0.43× | AMD EPYC 9V74 |
| [2026-11-13](https://github.com/seregonwar/slibz/actions/runs/43426348151) | [`a37bdda`](https://github.com/seregonwar/slibz/commit/a37bddad7d713586f5f2ecf1219ac06470ad8075) | 805 · 1.5× | 105 · 0.56× | 70 · 6.3× | 0.83 · 1.20× | 0.32 · 2.8× | 0.76 · 1.7× | 2.16 · 0.41× | AMD EPYC 9V45 |
| [2026-11-13](https://github.com/seregonwar/slibz/actions/runs/43421333345) | [`7ddf69c`](https://github.com/seregonwar/slibz/commit/7ddf69c0469028e556947bde30cc04511f2aba94) | 567 · 1.42× | 78 · 0.61× | 118 · 5.7× | 0.16 · 8.3× | 0.44 · 2.8× | 1.08 · 1.7× | 2.93 · 0.44× | AMD EPYC 7763 |
| [2026-11-13](https://github.com/seregonwar/slibz/actions/runs/43418578760) | [`df9dd34`](https://github.com/seregonwar/slibz/commit/df9dd34373176ae18f52f101332245471c9c9c52) | 657 · 1.42× | 89 · 0.59× | 96 · 9.0× | 0.83 · 1.39× | 0.37 · 3.0× | 0.89 · 1.8× | 2.48 · 0.42× | AMD EPYC 9V74 |

## Files

- `history.json`: every run on main, oldest first (the last 1000), as written by `.github/bench/run.py`
- `latest.json`: the newest run
- `svg/`: the banners, light and dark
- `badges/`: [shields.io endpoint](https://shields.io/badges/endpoint-badge) badges
