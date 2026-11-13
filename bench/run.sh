#!/usr/bin/env bash
# CI-only LZMA2 A/B bench and differential fuzz (branch lzma2-decode-bench, never merged), driven
# by bench/ci.sh, which CMakeLists.txt runs from ci.yml's Linux ASan configure.
#   bench/run.sh info                       runner, CPU and compiler versions
#   bench/run.sh corpus                     Silesia into silesia/ (sha256-pinned download)
#   bench/run.sh deps <cc>                  pinned xz (liblzma, static) into deps/<cc>, 7-Zip C sources
#   bench/run.sh variants                   bench/variants.txt -> v/<name>/{src,include}, order in v/order
#   bench/run.sh bench <cc> <flags...>      build every variant + bench/abbench.c, run it on Silesia
#   bench/run.sh profile <cc> <flags...>    perf profile of variants $PROF (default: last) and xz
#   bench/run.sh fuzz <iters> <seeds...>    tests/fuzz_lzma.c (ASan + UBSan) per variant, same counters
set -euo pipefail
cd "$(dirname "$0")/.."
unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS # every build below spells out its flags
ROUNDS=${ROUNDS:-5}
CPU=${BENCH_CPU:-1}
XZ_URL=https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz
XZ_SHA=0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
SZ_SHA=d3296e39e1f1f92c7b41c0c3848e65640428d42d # 7-Zip 26.06 (ip7z/7zip tag)
SILESIA_SHA=0626e25f45c0ffb5dc801f13b7c82a3b75743ba07e3a71835a41e3d9f63c77af
FILES="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
WARN="-Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion"
SZC="src-deps/7zip/C"

info() {
    echo "runner: ${ImageOS:-?} image ${ImageVersion:-?}, $(uname -m), $(nproc) cpus"
    lscpu | grep -E '^(Vendor ID|Model name|CPU\(s\)|Thread|L1d|L2|L3)' || true
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

deps() {
    local cc=$1 pre=$PWD/deps/$1 b=$PWD/build-deps/$1
    if [ ! -d src-deps/xz ]; then
        mkdir -p src-deps/xz
        curl -fsSL --retry 5 -o src-deps/xz.tar.gz "$XZ_URL"
        echo "$XZ_SHA  src-deps/xz.tar.gz" | sha256sum -c -
        tar -xzf src-deps/xz.tar.gz -C src-deps/xz --strip-components=1
    fi
    if [ ! -d src-deps/7zip ]; then
        mkdir -p src-deps/7zip
        curl -fsSL --retry 5 "https://github.com/ip7z/7zip/archive/$SZ_SHA.tar.gz" |
            tar -xz -C src-deps/7zip --strip-components=1 "7zip-$SZ_SHA/C"
    fi
    if [ -f "$pre/ok" ]; then
        echo "deps/$cc: cached"
        return 0
    fi
    rm -rf "$b" "$pre" && mkdir -p "$b"
    cmake -S src-deps/xz -B "$b/xz" -DCMAKE_C_COMPILER="$cc" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DXZ_NLS=OFF -DXZ_DOC=OFF \
        -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF \
        -DXZ_TOOL_SCRIPTS=OFF -DXZ_TOOL_SYMLINKS=OFF -DXZ_TOOL_SYMLINKS_LZMA=OFF \
        -DCMAKE_INSTALL_PREFIX="$pre" -DCMAKE_INSTALL_LIBDIR=lib > /dev/null
    cmake --build "$b/xz" -j"$(nproc)" > /dev/null
    cmake --install "$b/xz" > /dev/null
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
            "lzma.c sha256 $(sha256sum "v/$name/src/lzma.c" | cut -c1-16)"
    done < bench/variants.txt
}

vh() { # variants.h for the given variant names
    local n
    echo '#include <stddef.h>'
    echo '#include <stdint.h>'
    echo 'typedef int64_t (*rd_fn)(void *u, uint8_t *b, size_t n);'
    echo 'typedef int (*wr_fn)(void *u, const uint8_t *p, size_t n);'
    for n in "$@"; do echo "int v_${n}_decode(uint64_t, uint64_t, rd_fn, wr_fn, void *);"; done
    echo "#define NV $#"
    echo "static const char *const vname[NV] = {$(for n in "$@"; do printf '"%s", ' "$n"; done)};"
    echo "static int (*const vdec[NV])(uint64_t, uint64_t, rd_fn, wr_fn, void *) = {$(for n in "$@"; do
        printf 'v_%s_decode, ' "$n"
    done)};"
}

objdir() { # cc flags... -> obj/<tag>
    echo "obj/$(echo "$*" | tr -s ' ' '_' | tr -c 'A-Za-z0-9_\n' '_')"
}

bench() {
    local cc=$1
    shift
    local flags="$*" tag o pre=$PWD/deps/$cc n p bad names
    names=$(cat v/order)
    tag=$(echo "$cc $flags" | tr -s ' ' '_')
    o=$(objdir "$cc" "$flags")
    rm -rf "$o" && mkdir -p "$o"
    # shellcheck disable=SC2086
    vh $names > "$o/variants.h"
    echo "== $cc $flags: $($cc --version | sed -n 1p)"
    for n in $names; do
        p=v_${n}_
        # shellcheck disable=SC2086
        $cc $flags $WARN -DNDEBUG -DVPFX="$p" -Dslz_i_lzd_new="${p}lzd_new" -Dslz_i_lzd_free="${p}lzd_free" \
            -Dslz_i_lzd_io="${p}lzd_io" -Dslz_i_lzd_lzma="${p}lzd_lzma" -Dslz_i_lzd_lzma2="${p}lzd_lzma2" \
            -Dslz_i_bcj_wr="${p}bcj_wr" -Dslz_i_bcj_finish="${p}bcj_finish" -I"v/$n/src" -I"v/$n/include" \
            -c bench/variant.c -o "$o/$n.o"
        bad=$(nm -g --defined-only "$o/$n.o" | awk '{print $3}' | grep -v "^$p" || true)
        [ -z "$bad" ] || { echo "variant $n exports unprefixed symbols: $bad"; exit 1; }
    done
    # shellcheck disable=SC2086
    $cc $flags -DNDEBUG -I"$SZC" -c "$SZC/LzmaDec.c" -o "$o/LzmaDec.o"
    # shellcheck disable=SC2086
    $cc $flags -DNDEBUG -I"$SZC" -c "$SZC/Lzma2Dec.c" -o "$o/Lzma2Dec.o"
    # shellcheck disable=SC2086
    $cc $flags -DNDEBUG -I"$SZC" -c bench/sdk.c -o "$o/sdk.o"
    # shellcheck disable=SC2086
    $cc $flags -I"$o" -I"$pre/include" bench/abbench.c "$o"/*.o "$pre/lib/liblzma.a" -pthread -o "$o/abbench"
    # shellcheck disable=SC2046
    taskset -c "$CPU" "$o/abbench" -r "$ROUNDS" -t "$tag" $(for f in $FILES; do echo "silesia/$f"; done) |
        tee "$o/result.txt"
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
        { echo '```'; cat "$o/result.txt"; echo '```'; } >> "$GITHUB_STEP_SUMMARY"
    fi
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

profile() { # cpu-clock sampling of the variants in $PROF (default: the last) and of xz
    local cc=$1
    shift
    local flags="$*" o pre=$PWD/deps/$cc vs perf m sym files bin=
    o=$(objdir "$cc" "$flags")
    vs=${PROF:-$(tail -1 v/order)}
    files=$(for f in $FILES; do echo "silesia/$f"; done)
    for m in $vs; do
        [ -f "$o/$m.o" ] || { echo "profile: no $o/$m.o, run bench $cc $flags first"; return 1; }
    done
    perf=$(perfbin) || { echo "profile: perf unavailable, skipped"; return 0; }
    echo "perf: $("$perf" --version)"
    for m in $vs; do # one slz variant per binary: its static functions have unique names
        mkdir -p "$o/prof/$m"
        vh "$m" > "$o/prof/$m/variants.h"
        # shellcheck disable=SC2086
        $cc $flags -I"$o/prof/$m" -I"$pre/include" bench/abbench.c "$o/$m.o" "$o/LzmaDec.o" "$o/Lzma2Dec.o" \
            "$o/sdk.o" "$pre/lib/liblzma.a" -pthread -o "$o/prof/$m/abbench"
    done
    for m in $vs xz sdk; do
        case "$m" in xz | sdk) ;; *) bin=$o/prof/$m/abbench ;; esac # xz, sdk: the last variant's binary
        echo "== perf $cc $flags: $m"
        # shellcheck disable=SC2086
        "$perf" stat -e task-clock,cycles,instructions,branches,branch-misses -- taskset -c "$CPU" \
            "$bin" -r 1 -m "$m" $files 2>&1 | grep -E 'TOTAL|task-clock|cycles|instructions|branch' || true
        # shellcheck disable=SC2086
        "$perf" record -q -e cpu-clock -F 5000 -o "$o/prof/$m.data" -- taskset -c "$CPU" "$bin" -r 1 \
            -m "$m" $files > /dev/null 2>&1 || { echo "perf record failed"; continue; }
        "$perf" report -i "$o/prof/$m.data" --stdio -q --sort sym --percent-limit 0.3 2> /dev/null | sed -n '1,20p' || true
    done
    for m in $vs; do
        [ -f "$o/prof/$m.data" ] || continue
        sym=$("$perf" report -i "$o/prof/$m.data" --stdio -q --sort sym 2> /dev/null |
            awk '$2 == "[.]" && $3 ~ /^(lzma|v_.*lzd_lzma2)/ { print $3; exit }' || true)
        [ -n "$sym" ] || continue
        echo "== perf annotate $cc $flags: $m $sym (instructions >= 0.15% of samples)"
        "$perf" annotate -i "$o/prof/$m.data" --stdio --no-source "$sym" 2> /dev/null |
            awk '$1 + 0 >= 0.15' | sed -n '1,260p' || true
    done
}

fuzz() {
    local iters=$1
    shift
    local cc=${CC:-gcc} o=obj/fuzz n s fail=0 first ref rc t0=$SECONDS
    local pre=$PWD/deps/${CC:-gcc}
    mkdir -p "$o" out
    echo "fuzz: $cc ${FUZZ_CFLAGS:-}, $iters iterations, seeds $*, reference liblzma from deps/$cc"
    for n in $(cat v/order); do
        # shellcheck disable=SC2086
        $cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
            ${FUZZ_CFLAGS:-} -I"v/$n/include" -I"v/$n/src" -I"$pre/include" tests/fuzz_lzma.c \
            "v/$n/src/lzma.c" "$pre/lib/liblzma.a" -pthread -o "$o/fuzz_$n"
    done
    for s in "$@"; do
        for n in $(cat v/order); do
            (
                set +e
                "$o/fuzz_$n" "$iters" "$s" > "out/$n.$s.txt" 2> "out/$n.$s.err"
                echo $? > "out/$n.$s.rc"
            ) &
        done
        wait
    done
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
    info | corpus | deps | variants | bench | profile | fuzz) "$cmd" "$@" ;;
    *)
        sed -n '2,10p' "$0"
        exit 2
        ;;
esac
