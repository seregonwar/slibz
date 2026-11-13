#!/usr/bin/env bash
# CI-only inflate A/B bench and differential fuzz (branch inflate-fast-bench, never merged), driven
# by bench/ci.sh, which CMakeLists.txt runs from ci.yml's Linux ASan configure.
#   bench/run.sh info                       runner, CPU and compiler versions
#   bench/run.sh corpus                     Silesia into silesia/ (sha256-pinned download)
#   bench/run.sh deps <cc>                  pinned libdeflate, zlib-ng (native API), zlib into deps/<cc>
#   bench/run.sh variants                   bench/variants.txt -> v/<name>/{src,include}, order in v/order
#   bench/run.sh bench <cc> <flags...>      build every variant + bench/abbench.c, run it on Silesia
#   bench/run.sh harness <cc>               tests/bench_inflate.c as is, on the last variant
#   bench/run.sh fuzz <iters> <seeds...>    tests/fuzz_inflate.c (ASan + UBSan) per variant, same counters
set -euo pipefail
cd "$(dirname "$0")/.."
unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS # every build below spells out its flags
ROUNDS=${ROUNDS:-15}
CPU=${BENCH_CPU:-1}
LD_SHA=92e6a0db9fa848d742f9eb286c92afc60f2c3dda  # libdeflate v1.26
ZNG_SHA=12731092979c6d07f42da27da673a9f6c7b13586 # zlib-ng 2.3.3
Z_SHA=99623ef7097352da7993ba614161fd6a311e046e   # zlib v1.4
SILESIA_SHA=0626e25f45c0ffb5dc801f13b7c82a3b75743ba07e3a71835a41e3d9f63c77af
FILES="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
WARN="-Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion"

info() {
    echo "runner: ${ImageOS:-?} image ${ImageVersion:-?}, $(uname -m), $(nproc) cpus"
    lscpu | grep -E '^(Vendor ID|Model name|CPU\(s\)|Thread|L1d|L2|L3)' || true
    echo "bmi2: $(grep -m1 -ow bmi2 /proc/cpuinfo || echo no)"
    gcc --version | sed -n 1p
    clang --version | sed -n 1p
    cmake --version | sed -n 1p
}

corpus() {
    if [ ! -f silesia.zip ]; then
        curl -fsSL --retry 5 --retry-delay 10 -o silesia.zip https://sun.aei.polsl.pl//~sdeor/corpus/silesia.zip
    fi
    echo "$SILESIA_SHA  silesia.zip" | sha256sum -c -
    rm -rf silesia && mkdir silesia
    if command -v unzip > /dev/null; then
        unzip -q silesia.zip -d silesia
    else
        python3 -m zipfile -e silesia.zip silesia
    fi
    for f in $FILES; do [ -f "silesia/$f" ] || { echo "missing silesia/$f"; exit 1; }; done
}

fetch() { # dir repo sha
    [ -d "src-deps/$1" ] && return 0
    mkdir -p "src-deps/$1"
    curl -fsSL --retry 5 "https://github.com/$2/archive/$3.tar.gz" | tar -xz -C "src-deps/$1" --strip-components=1
}

deps() {
    local cc=$1 pre=$PWD/deps/$1 b=$PWD/build-deps/$1
    if [ -f "$pre/ok" ]; then
        echo "deps/$cc: cached"
        return 0
    fi
    fetch libdeflate ebiggers/libdeflate $LD_SHA
    fetch zlib-ng zlib-ng/zlib-ng $ZNG_SHA
    fetch zlib madler/zlib $Z_SHA
    rm -rf "$b" "$pre" && mkdir -p "$b"
    cmake -S src-deps/libdeflate -B "$b/ld" -DCMAKE_C_COMPILER="$cc" -DCMAKE_BUILD_TYPE=Release \
        -DLIBDEFLATE_BUILD_SHARED_LIB=OFF -DLIBDEFLATE_BUILD_GZIP=OFF \
        -DCMAKE_INSTALL_PREFIX="$pre" -DCMAKE_INSTALL_LIBDIR=lib > /dev/null
    cmake --build "$b/ld" -j"$(nproc)" > /dev/null
    cmake --install "$b/ld" > /dev/null
    cmake -S src-deps/zlib-ng -B "$b/zng" -DCMAKE_C_COMPILER="$cc" -DCMAKE_BUILD_TYPE=Release -DZLIB_COMPAT=OFF \
        -DBUILD_SHARED_LIBS=OFF -DZLIB_ENABLE_TESTS=OFF -DZLIBNG_ENABLE_TESTS=OFF -DWITH_GTEST=OFF \
        -DCMAKE_INSTALL_PREFIX="$pre" -DCMAKE_INSTALL_LIBDIR=lib > /dev/null
    cmake --build "$b/zng" -j"$(nproc)" > /dev/null
    cmake --install "$b/zng" > /dev/null
    cp -r src-deps/zlib "$b/z"
    (cd "$b/z" && CC=$cc ./configure --static --prefix="$pre" > /dev/null && make -j"$(nproc)" libz.a > /dev/null &&
        make install > /dev/null)
    ls "$pre/lib"/*.a "$pre/include"
    touch "$pre/ok"
}

variants() {
    local name ref ovl
    rm -rf v && mkdir v
    : > v/order
    while read -r name ref ovl || [ -n "${name:-}" ]; do
        case "$name" in '' | '#'*) continue ;; esac
        [[ $name =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || { echo "bad variant name '$name'"; exit 1; }
        # ci.yml checks out shallow: fetch the variant's commit (full SHA) when it is missing
        git cat-file -e "$ref^{commit}" 2> /dev/null || git fetch -q --no-tags --depth=1 origin "$ref"
        mkdir "v/$name"
        git archive "$ref" src include | tar -x -C "v/$name"
        if [ -n "${ovl:-}" ]; then cp -r "$ovl"/. "v/$name/"; fi
        echo "$name" >> v/order
        echo "variant $name: $(git rev-parse --short "$ref")${ovl:+ + $ovl}," \
            "inflate.c sha256 $(sha256sum "v/$name/src/inflate.c" | cut -c1-16)"
    done < bench/variants.txt
}

bench() {
    local cc=$1
    shift
    local flags="$*" tag o pre=$PWD/deps/$cc n p bad names
    names=$(cat v/order)
    tag=$(echo "$cc $flags" | tr -s ' ' '_')
    o=obj/$(echo "$tag" | tr -c 'A-Za-z0-9_\n' '_')
    rm -rf "$o" && mkdir -p "$o"
    {
        echo '#include <stddef.h>'
        echo '#include <stdint.h>'
        echo 'typedef int64_t (*rd_fn)(void *u, uint8_t *b, size_t n);'
        echo 'typedef int (*wr_fn)(void *u, const uint8_t *p, size_t n);'
        for n in $names; do echo "void *v_${n}_new(void); int v_${n}_decode(void *, rd_fn, wr_fn, void *);"; done
        echo "#define NV $(echo $names | wc -w)"
        echo "static const char *const vname[NV] = {$(for n in $names; do printf '"%s", ' "$n"; done)};"
        echo "static void *(*const vnew[NV])(void) = {$(for n in $names; do printf 'v_%s_new, ' "$n"; done)};"
        echo "static int (*const vdec[NV])(void *, rd_fn, wr_fn, void *) = {$(for n in $names; do
            printf 'v_%s_decode, ' "$n"
        done)};"
    } > "$o/variants.h"
    echo "== $cc $flags: $($cc --version | sed -n 1p)"
    for n in $names; do
        p=v_${n}_
        # shellcheck disable=SC2086
        $cc $flags $WARN -DNDEBUG -DVPFX="$p" -Dslz_i_inf_new="${p}inf_new" -Dslz_i_inf_reset="${p}inf_reset" \
            -Dslz_i_inf_run="${p}inf_run" -Dslz_i_inf_flush="${p}inf_flush" -I"v/$n/src" -I"v/$n/include" \
            -c bench/variant.c -o "$o/$n.o"
        bad=$(nm -g --defined-only "$o/$n.o" | awk '{print $3}' | grep -v "^$p" || true)
        [ -z "$bad" ] || { echo "variant $n exports unprefixed symbols: $bad"; exit 1; }
    done
    # shellcheck disable=SC2086
    $cc $flags -I"$pre/include" -c bench/zng.c -o "$o/zng.o"
    # shellcheck disable=SC2086
    $cc $flags -I"$o" -I"$pre/include" bench/abbench.c "$o"/*.o "$pre/lib/libdeflate.a" "$pre/lib/libz-ng.a" \
        "$pre/lib/libz.a" -o "$o/abbench"
    # shellcheck disable=SC2046
    taskset -c "$CPU" "$o/abbench" -r "$ROUNDS" -t "$tag" $(for f in $FILES; do echo "silesia/$f"; done) |
        tee "$o/result.txt"
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
        { echo '```'; cat "$o/result.txt"; echo '```'; } >> "$GITHUB_STEP_SUMMARY"
    fi
}

harness() {
    local cc=$1 pre=$PWD/deps/$1 n o=obj/harness-$1 k
    n=$(tail -1 v/order)
    mkdir -p "$o"
    $cc -O2 -I"v/$n/include" -I"v/$n/src" -I"$pre/include" tests/bench_inflate.c "v/$n/src/inflate.c" \
        "$pre/lib/libdeflate.a" "$pre/lib/libz.a" -o "$o/bench_inflate"
    echo "== tests/bench_inflate.c as is (its slz timing includes a memcmp), slz = variant $n, $cc -O2"
    for k in 1 2 3; do
        # shellcheck disable=SC2046
        taskset -c "$CPU" "$o/bench_inflate" $(for f in $FILES; do echo "silesia/$f"; done)
    done
}

fuzz() {
    local iters=$1
    shift
    local cc=${CC:-gcc} o=obj/fuzz n s fail=0 first ref rc t0=$SECONDS
    local pre=$PWD/deps/${CC:-gcc}
    mkdir -p "$o" out
    echo "fuzz: $cc ${FUZZ_CFLAGS:-}, $iters iterations, seeds $*, reference zlib from deps/$cc"
    for n in $(cat v/order); do
        # shellcheck disable=SC2086
        $cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
            ${FUZZ_CFLAGS:-} -I"v/$n/include" -I"v/$n/src" -I"$pre/include" tests/fuzz_inflate.c \
            "v/$n/src/inflate.c" "$pre/lib/libz.a" -o "$o/fuzz_$n"
    done
    for s in "$@"; do
        for n in $(cat v/order); do
            (
                set +e
                "$o/fuzz_$n" "$iters" "$s" > "out/$n.$s.txt" 2> "out/$n.$s.err"
                echo $? > "out/$n.$s.rc"
            ) &
        done
    done
    wait
    first=$(head -1 v/order)
    for s in "$@"; do
        ref=$(cat "out/$first.$s.txt")
        for n in $(cat v/order); do
            rc=$(cat "out/$n.$s.rc")
            echo "seed $s $n (exit $rc): $(cat "out/$n.$s.txt")"
            if [ "$rc" != 0 ]; then
                tail -30 "out/$n.$s.err"
                fail=1
            fi
            if [ "$(cat "out/$n.$s.txt")" != "$ref" ]; then
                echo "  ^ counters differ from $first"
                fail=1
            fi
        done
    done
    echo "fuzz: $((SECONDS - t0))s"
    return $fail
}

cmd=${1:-}
[ $# -gt 0 ] && shift
case "$cmd" in
    info | corpus | deps | variants | bench | harness | fuzz) "$cmd" "$@" ;;
    *)
        sed -n '2,10p' "$0"
        exit 2
        ;;
esac
