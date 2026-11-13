#!/usr/bin/env bash
# inflate-fast-bench only (never merged): called by CMakeLists.txt from the Linux ASan configure of
# ci.yml (linux-gcc-asan, linux-clang-asan, linux-arm64-asan), argument = that job's compiler.
# Every job benches the variants of bench/variants.txt with both compilers (same-job ratios; -O3 is
# the CMake Release default, -O2 for continuity), profiles the first variant against libdeflate,
# and runs its share of the fuzz seeds.
set -euo pipefail
cd "$(dirname "$0")/.."
case "$(basename "${1:-gcc}")" in *clang*) job=clang ;; *) job=gcc ;; esac
arch=$(uname -m)
ITERS=${FUZZ_ITERS:-1000}
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
    r bench clang -O3
    r bench gcc -O3 -DSLZ_NO_BMI2
    r bench gcc -O2
    r profile gcc -O3
    CC=gcc r fuzz "$ITERS" 1 2
    CC=gcc FUZZ_CFLAGS=-DSLZ_NO_BMI2 r fuzz "$ITERS" 5
elif [ "$arch" = x86_64 ]; then
    r bench clang -O3
    r bench gcc -O3
    r bench clang -O2
    r profile clang -O3
    CC=clang r fuzz "$ITERS" 3 4
else
    r bench gcc -O3
    r bench clang -O3
    r bench gcc -O2
    r profile gcc -O3
    r profile clang -O3
    CC=gcc r fuzz "$ITERS" 7 8
fi
echo "== bench/ci.sh done ($((SECONDS / 60))m$((SECONDS % 60))s)"