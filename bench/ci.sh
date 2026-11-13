#!/usr/bin/env bash
# lzma2-decode-bench only (never merged): called by CMakeLists.txt from the Linux ASan configure of
# ci.yml (linux-gcc-asan, linux-clang-asan, linux-arm64-asan), argument = that job's compiler.
# Every job benches the variants of bench/variants.txt (same-job ratios; -O3 is the CMake Release
# default) and runs its share of the tests/fuzz_lzma.c seeds on every variant; the gcc x86 job also
# sweeps gcc -O3 with single passes disabled (gcc -O3 decodes slower than gcc -O2).
set -euo pipefail
cd "$(dirname "$0")/.."
case "$(basename "${1:-gcc}")" in *clang*) job=clang ;; *) job=gcc ;; esac
arch=$(uname -m)
ITERS=${FUZZ_ITERS:-300}
r() {
    echo "== bench/run.sh $* ($((SECONDS / 60))m$((SECONDS % 60))s)"
    bash bench/run.sh "$@"
}
if ! command -v clang > /dev/null; then
    sudo apt-get update -qq && sudo apt-get install -y -qq clang > /dev/null
fi
r info
r corpus
r variants
r deps gcc
r deps clang
if [ "$arch" = x86_64 ] && [ "$job" = gcc ]; then
    r bench gcc -O3
    r bench gcc -O2
    for f in -fno-tree-vectorize -fno-unswitch-loops -fno-peel-loops "-fno-split-loops -fno-split-paths" \
        "-fno-predictive-commoning -fno-tree-partial-pre -fno-gcse-after-reload" \
        "-fno-tree-loop-distribution -fno-tree-loop-distribute-patterns" -fno-ipa-cp-clone \
        -fno-version-loops-for-strides; do
        # shellcheck disable=SC2086
        ONLY="c16 rg8 cm9" r bench gcc -O3 $f
    done
    CC=gcc r fuzz "$ITERS" 1 2
elif [ "$arch" = x86_64 ]; then
    r bench clang -O3
    r bench gcc -O3
    r bench gcc -O2
    CC=clang r fuzz "$ITERS" 3 4
else
    r bench gcc -O3
    r bench clang -O3
    r bench gcc -O2
    CC=gcc r fuzz "$ITERS" 5 6
fi
echo "== bench/ci.sh done ($((SECONDS / 60))m$((SECONDS % 60))s)"