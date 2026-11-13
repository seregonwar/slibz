#!/usr/bin/env bash
# CI-only inflate A/B bench and differential fuzz (branch inflate-fast-bench, never merged), driven
# by bench/ci.sh, which CMakeLists.txt runs from ci.yml's Linux ASan configure.
#   bench/run.sh info                       runner, CPU and compiler versions
#   bench/run.sh corpus                     Silesia into silesia/ (sha256-pinned download)
#   bench/run.sh deps <cc>                  pinned libdeflate, zlib-ng (native API), zlib into deps/<cc>
#   bench/run.sh variants                   bench/variants.txt -> v/<name>/{src,include}, order in v/order
#   bench/run.sh bench <cc> <flags...>      build every variant + bench/abbench.c, run it on Silesia
#   bench/run.sh profile <cc> <flags...>    perf profile of variants $PROF (default: first) and libdeflate
#   bench/run.sh harness <cc> [variant]     tests/bench_inflate.c as is, on a variant (default: first)
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
    # default code generation target (no -march given below unless a bench config says so)
    echo "gcc default: $(gcc -Q --help=target 2> /dev/null | grep -E '^ +-march=' | tr -s ' \t' ' ' || true)," \
        "predefines: $(echo | gcc -dM -E - | grep -oE '__(SSE4_2|AVX2|BMI2|AVX512F)__' | tr '\n' ' ' || true)"
    echo "clang default: $(clang -### -x c -c /dev/null 2>&1 | grep -oE '"-target-cpu" "[^"]*"' || true)," \
        "predefines: $(echo | clang -dM -E - | grep -oE '__(SSE4_2|AVX2|BMI2|AVX512F)__' | tr '\n' ' ' || true)"
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

hasdirect() { grep -q slz_inflate "v/$1/include/slz.h"; }

vh() { # variants.h for the given variant names
    local n
    echo '#include <stddef.h>'
    echo '#include <stdint.h>'
    echo 'typedef int64_t (*rd_fn)(void *u, uint8_t *b, size_t n);'
    echo 'typedef int (*wr_fn)(void *u, const uint8_t *p, size_t n);'
    echo 'typedef int (*dir_fn)(const uint8_t *c, size_t cn, uint8_t *o, size_t cap, size_t *got);'
    for n in "$@"; do
        echo "void *v_${n}_new(void); int v_${n}_decode(void *, rd_fn, wr_fn, void *);"
        if hasdirect "$n"; then echo "int v_${n}_direct(const uint8_t *, size_t, uint8_t *, size_t, size_t *);"; fi
    done
    echo "#define NV $#"
    echo "static const char *const vname[NV] = {$(for n in "$@"; do printf '"%s", ' "$n"; done)};"
    echo "static void *(*const vnew[NV])(void) = {$(for n in "$@"; do printf 'v_%s_new, ' "$n"; done)};"
    echo "static int (*const vdec[NV])(void *, rd_fn, wr_fn, void *) = {$(for n in "$@"; do
        printf 'v_%s_decode, ' "$n"
    done)};"
    echo "static const dir_fn vdir[NV] = {$(for n in "$@"; do
        if hasdirect "$n"; then printf 'v_%s_direct, ' "$n"; else printf '0, '; fi
    done)};"
}

objdir() { # cc flags... -> obj/<tag>
    echo "obj/$(echo "$*" | tr -s ' ' '_' | tr -c 'A-Za-z0-9_\n' '_')"
}

bench() {
    local cc=$1
    shift
    local flags="$*" tag o pre=$PWD/deps/$cc n p bad names dflags
    names=$(cat v/order)
    tag=$(echo "$cc $flags" | tr -s ' ' '_')
    o=$(objdir "$cc" "$flags")
    rm -rf "$o" && mkdir -p "$o"
    # shellcheck disable=SC2086
    vh $names > "$o/variants.h"
    echo "== $cc $flags: $($cc --version | sed -n 1p)"
    for n in $names; do
        p=v_${n}_
        dflags=
        if hasdirect "$n"; then dflags="-DVDIRECT -Dslz_inflate=${p}inflate"; fi
        # shellcheck disable=SC2086
        $cc $flags $WARN -DNDEBUG -DVPFX="$p" -Dslz_i_inf_new="${p}inf_new" -Dslz_i_inf_reset="${p}inf_reset" \
            -Dslz_i_inf_run="${p}inf_run" -Dslz_i_inf_flush="${p}inf_flush" $dflags -I"v/$n/src" -I"v/$n/include" \
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
    n=${2:-$(sed -n 1p v/order)}
    mkdir -p "$o"
    $cc -O2 -I"v/$n/include" -I"v/$n/src" -I"$pre/include" tests/bench_inflate.c "v/$n/src/inflate.c" \
        "$pre/lib/libdeflate.a" "$pre/lib/libz.a" -o "$o/bench_inflate"
    echo "== tests/bench_inflate.c as is (its slz timing includes a memcmp), slz = variant $n, $cc -O2"
    for k in 1 2 3; do
        # shellcheck disable=SC2046
        taskset -c "$CPU" "$o/bench_inflate" $(for f in $FILES; do echo "silesia/$f"; done)
    done
}

perfbin() { # path of a working perf, installing it if needed (GitHub runners: passwordless sudo)
    local p
    p=$(command -v perf || true)
    if [ -z "$p" ] || ! "$p" --version > /dev/null 2>&1; then
        sudo apt-get update -qq > /dev/null 2>&1 || true
        sudo apt-get install -y -qq linux-tools-common "linux-tools-$(uname -r)" > /dev/null 2>&1 ||
            sudo apt-get install -y -qq linux-tools-common linux-tools-generic > /dev/null 2>&1 || true
        p=$(ls /usr/lib/linux-tools/*/perf /usr/lib/linux-tools-*/perf 2> /dev/null | sed -n 1p)
    fi
    [ -n "$p" ] && "$p" --version > /dev/null 2>&1 || return 1
    sudo sysctl -q -w kernel.perf_event_paranoid=-1 > /dev/null 2>&1 || true
    sudo sysctl -q -w kernel.kptr_restrict=0 > /dev/null 2>&1 || true
    echo "$p"
}

profile() { # cpu-clock sampling of the variants in $PROF (default: the first) and of libdeflate
    local cc=$1
    shift
    local flags="$*" o pre=$PWD/deps/$cc vs perf m sym files bin=
    o=$(objdir "$cc" "$flags")
    vs=${PROF:-$(sed -n 1p v/order)} # modes: <variant> (streaming) or <variant>_d (slz_inflate)
    files=$(for f in $FILES; do echo "silesia/$f"; done)
    for m in $(for m in $vs; do echo "${m%_d}"; done | sort -u); do
        [ -f "$o/$m.o" ] || { echo "profile: no $o/$m.o, run bench $cc $flags first"; return 1; }
    done
    perf=$(perfbin) || { echo "profile: perf unavailable, skipped"; return 0; }
    echo "perf: $("$perf" --version)"
    for m in $(for m in $vs; do echo "${m%_d}"; done | sort -u); do # one slz variant per binary: its static functions have unique names
        mkdir -p "$o/prof/$m"
        vh "$m" > "$o/prof/$m/variants.h"
        # shellcheck disable=SC2086
        $cc $flags -I"$o/prof/$m" -I"$pre/include" bench/abbench.c "$o/$m.o" "$o/zng.o" "$pre/lib/libdeflate.a" \
            "$pre/lib/libz-ng.a" "$pre/lib/libz.a" -o "$o/prof/$m/abbench"
    done
    for m in $vs libdeflate; do
        [ "$m" = libdeflate ] || bin=$o/prof/${m%_d}/abbench # libdeflate: the last variant's binary
        echo "== perf $cc $flags: $m"
        # shellcheck disable=SC2086
        "$perf" stat -e task-clock,cycles,instructions,branches,branch-misses -- taskset -c "$CPU" \
            "$bin" -r 3 -m "$m" $files 2>&1 | grep -E 'TOTAL|task-clock|cycles|instructions|branch' || true
        # shellcheck disable=SC2086
        "$perf" record -q -e cpu-clock -F 10000 -o "$o/prof/$m.data" -- taskset -c "$CPU" "$bin" -r 3 \
            -m "$m" $files > /dev/null 2>&1 || { echo "perf record failed"; continue; }
        "$perf" report -i "$o/prof/$m.data" --stdio -q --sort sym --percent-limit 0.3 2> /dev/null | sed -n '1,20p' || true
    done
    for m in $vs libdeflate; do
        [ -f "$o/prof/$m.data" ] || continue
        sym=$("$perf" report -i "$o/prof/$m.data" --stdio -q --sort sym 2> /dev/null |
            awk '$2 == "[.]" && $3 ~ /^(codes|deflate_decompress|libdeflate_deflate_decompress)/ { print $3; exit }' ||
            true)
        [ -n "$sym" ] || continue
        echo "== perf annotate $cc $flags: $m $sym (instructions >= 0.1% of samples)"
        "$perf" annotate -i "$o/prof/$m.data" --stdio --no-source -s "$sym" > "$o/prof/$m.ann" 2> "$o/prof/$m.err" ||
            true
        echo "annotate: $(wc -l < "$o/prof/$m.ann") lines; stderr: $(head -c 400 "$o/prof/$m.err" | tr '\n' ' ')"
        sed -n '1,4p' "$o/prof/$m.ann"
        awk '{ x = $1; sub(/%$/, "", x) } x ~ /^[0-9.]+$/ && x + 0 >= 0.1' "$o/prof/$m.ann" | sed -n '1,260p' || true
    done
}

disasm() { # cc flags...: disassembly of the first variant's hot loops (to compare compilers)
    local cc=$1
    shift
    local o n f syn=
    o=$(objdir "$cc" "$*")
    n=${DIS_V:-$(sed -n 1p v/order)}
    [ "$(uname -m)" = x86_64 ] && syn="-M intel"
    for f in codes_bmi2 codes_generic; do # the BMI2 build when there is one
        nm "$o/$n.o" | grep -qw "$f" || continue
        echo "== disasm $cc $*: $n $f"
        # shellcheck disable=SC2086
        objdump -d --no-show-raw-insn $syn --disassemble="$f" "$o/$n.o" | sed -n '7,$p' | cut -c1-90 || true
        break
    done
    if [ -n "${DIS_LD:-}" ]; then # libdeflate's biggest decompress function, same compiler
        local t=$o/ldobj s a=$PWD/deps/$cc/lib/libdeflate.a
        rm -rf "$t" && mkdir -p "$t"
        (cd "$t" && ar x "$a") || true
        s=$(nm -S --size-sort "$t"/deflate_decompress*.o | awk '$3 ~ /[tT]/ && $4 ~ /decompress/ { s = $4 } END { print s }' || true)
        echo "== disasm $cc $*: libdeflate $s"
        # shellcheck disable=SC2086
        objdump -d --no-show-raw-insn $syn --disassemble="$s" "$t"/deflate_decompress*.o | sed -n '7,$p' | cut -c1-90 || true
    fi
}

fuzz() {
    local iters=$1
    shift
    local cc=${CC:-gcc} o=obj/fuzz n s fail=0 first ref rc t0=$SECONDS vs
    local pre=$PWD/deps/${CC:-gcc}
    vs=${FUZZ_V:-$(cat v/order)} # variants to fuzz (tests/fuzz_inflate.c must build against each)
    first=${vs%%[[:space:]]*}
    mkdir -p "$o" out
    echo "fuzz: $cc ${FUZZ_CFLAGS:-}, $iters iterations, seeds $*, variants $(echo $vs), reference zlib from deps/$cc"
    for n in $vs; do
        # shellcheck disable=SC2086
        $cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
            ${FUZZ_CFLAGS:-} -I"v/$n/include" -I"v/$n/src" -I"$pre/include" tests/fuzz_inflate.c \
            "v/$n/src/inflate.c" "$pre/lib/libz.a" -o "$o/fuzz_$n"
    done
    for s in "$@"; do
        for n in $vs; do
            (
                set +e
                "$o/fuzz_$n" "$iters" "$s" > "out/$n.$s.txt" 2> "out/$n.$s.err"
                echo $? > "out/$n.$s.rc"
            ) &
        done
    done
    wait
    for s in "$@"; do
        ref=$(cat "out/$first.$s.txt")
        for n in $vs; do
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
    info | corpus | deps | variants | bench | profile | disasm | harness | fuzz) "$cmd" "$@" ;;
    *)
        sed -n '2,11p' "$0"
        exit 2
        ;;
esac
