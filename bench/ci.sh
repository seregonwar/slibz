#!/usr/bin/env bash
# inflate-faster-bench only (never merged): run by .github/workflows/abbench.yml, argument = the
# job's compiler (x86: gcc or clang job; arm64: one job).
# Every job benches the variants of bench/variants.txt with both compilers (same-job ratios; -O3 is
# the CMake Release default) and runs
# its share of the fuzz seeds. x86: the compilers' default target (printed by info) decides whether
# the BMI2 build is static or dispatched at run time; -march=x86-64 forces the dispatch.
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
    if grep -qm1 GenuineIntel /proc/cpuinfo; then # the gcc gap is largest on Intel: probe gcc passes
        for x in "-O2" "-O3 -fno-tree-vectorize" "-O3 -falign-loops=64 -falign-jumps=32" \
            "-O3 -freorder-blocks-algorithm=simple" "-O3 -fno-schedule-insns2" "-O3 -mtune=native" \
            "-O3 -fno-tree-pre -fno-gcse"; do
            # shellcheck disable=SC2086
            r bench gcc $x
        done
        PROF="zc_d" r profile gcc -O3
        PROF="zc_d" r profile clang -O3
    fi
    if [ "${2:-1}" = 1 ]; then
        CC=gcc FUZZ_V=pc r fuzz "$ITERS" 1 2
        CC=gcc FUZZ_V=pc FUZZ_CFLAGS="-march=x86-64 -DSLZ_NO_BMI2" r fuzz "$ITERS" 5
    fi
elif [ "$arch" = x86_64 ]; then
    r bench clang -O3
    r bench gcc -O3
    r bench clang -O3 -march=x86-64 -DSLZ_NO_BMI2
    CC=clang FUZZ_V=pc r fuzz "$ITERS" 3 4
else
    r bench gcc -O3
    r bench clang -O3
    CC=gcc FUZZ_V=pc r fuzz "$ITERS" 7 8
fi
echo "== bench/ci.sh done ($((SECONDS / 60))m$((SECONDS % 60))s)"