#!/usr/bin/env bash
# slz - Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
#
# Pinned inputs of the benchmark workflow (.github/workflows/bench.yml), Linux x86-64.
# Every download is checked against its SHA-256. To bump a competitor, edit its line here:
# the workflow's cache keys hash this file, so the next run rebuilds everything.
#
#   deps.sh tools <prefix>   build the competitors from source:
#     <prefix>/zlib        zlib 1.4, static                      bench_inflate "zlib" column
#     <prefix>/zng         zlib-ng 2.3.3, native API, shared      bench_inflate "zlib-ng" column
#     <prefix>/zngc        zlib-ng 2.3.3, zlib-compat, static     the zlib under bsdtar and pigz
#     <prefix>/libdeflate  libdeflate 1.26, static                bench_inflate "libdeflate" column
#     <prefix>/xz          xz 5.8.4 liblzma, static               bench_lzma, bsdtar's 7z/xz support
#     <prefix>/bin         bsdtar (libarchive 3.8.11), pigz 2.8, 7zz (7-Zip 26.06, official build)
#   deps.sh corpus <dir>     download the Silesia corpus to <dir>/silesia.zip
set -euo pipefail

get() { # <url> <sha256> <file>: download unless present, then verify
  if [ ! -f "$3" ]; then
    curl -fsSL --retry 5 --retry-all-errors --connect-timeout 20 -o "$3.part" "$1"
    mv "$3.part" "$3"
  fi
  echo "$2  $3" | sha256sum --check --quiet --strict - || { rm -f "$3"; exit 1; }
}

case "${1:-}" in
  corpus)
    D=$(realpath -m "${2:?usage: deps.sh corpus <dir>}")
    mkdir -p "$D"
    get https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip \
      0626e25f45c0ffb5dc801f13b7c82a3b75743ba07e3a71835a41e3d9f63c77af "$D/silesia.zip"
    exit 0 ;;
  tools) ;;
  *) echo "usage: deps.sh tools <prefix> | corpus <dir>" >&2; exit 2 ;;
esac

P=$(realpath -m "${2:?usage: deps.sh tools <prefix>}")
S=$P/.src
J=$(nproc)
mkdir -p "$S" "$P/bin"

src() { # <name> <url> <sha256>: fetch and unpack into $S/<name>
  local f="$S/$1.tar.${2##*.tar.}"
  get "$2" "$3" "$f"
  rm -rf "${S:?}/$1" && mkdir -p "$S/$1"
  tar -xf "$f" -C "$S/$1" --strip-components=1
}

quiet() { # run a build step, show its log only on failure
  "$@" > "$S/step.log" 2>&1 || { tail -n 80 "$S/step.log"; exit 1; }
}

cm() { # <name> <install prefix> [cmake args]: Release build + install
  local n=$1 pre=$2
  shift 2
  rm -rf "$S/$n/_b"
  quiet cmake -S "$S/$n" -B "$S/$n/_b" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$pre" \
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@"
  quiet cmake --build "$S/$n/_b" -j "$J"
  quiet cmake --install "$S/$n/_b"
}

src zlib https://github.com/madler/zlib/releases/download/v1.4/zlib-1.4.tar.gz \
  02dd159783677a749c04adbdd02b55b8a2bcc7e054f51b76b549199a916ad584
(cd "$S/zlib" && quiet ./configure --static --prefix="$P/zlib" && quiet make -j "$J" && quiet make install)

src zlib-ng https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz \
  f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1
cm zlib-ng "$P/zng" -DZLIB_COMPAT=OFF -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF
cm zlib-ng "$P/zngc" -DZLIB_COMPAT=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF

src libdeflate https://github.com/ebiggers/libdeflate/releases/download/v1.26/libdeflate-1.26.tar.gz \
  125856d4656e0feab660f94842f835923410c9281fedbcee64598c918da42b5a
cm libdeflate "$P/libdeflate" -DLIBDEFLATE_BUILD_SHARED_LIB=OFF -DLIBDEFLATE_BUILD_GZIP=OFF

src xz https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz \
  0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
cm xz "$P/xz" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DXZ_NLS=OFF -DXZ_DOC=OFF \
  -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF \
  -DXZ_TOOL_SCRIPTS=OFF -DXZ_TOOL_SYMLINKS=OFF -DXZ_TOOL_SYMLINKS_LZMA=OFF

# bsdtar: static; zip, tar, gzip (zlib-ng) and 7z / xz (liblzma) only
src libarchive https://github.com/libarchive/libarchive/releases/download/v3.8.11/libarchive-3.8.11.tar.gz \
  900fe9fc7237c2b04f090f3c3648de8fe597525771f5035016537ce2d667d12b
B=$S/libarchive/_b
quiet cmake -S "$S/libarchive" -B "$B" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$P/zngc;$P/xz" \
  -DBUILD_SHARED_LIBS=OFF -DENABLE_INSTALL=OFF -DENABLE_TEST=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF \
  -DENABLE_UNZIP=OFF -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_MD=OFF \
  -DENABLE_LIBB2=OFF -DENABLE_LZ4=OFF -DENABLE_LZO=OFF -DENABLE_ZSTD=OFF -DENABLE_BZip2=OFF \
  -DENABLE_LIBXML2=OFF -DENABLE_BSDXML=OFF -DENABLE_EXPAT=OFF -DENABLE_PCREPOSIX=OFF \
  -DENABLE_PCRE2POSIX=OFF -DENABLE_LIBGCC=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_WERROR=OFF
grep -Eq "^ZLIB_LIBRARY(_RELEASE)?:FILEPATH=$P/zngc/" "$B/CMakeCache.txt" ||
  { echo "libarchive did not pick zlib-ng:" >&2; grep -E '^ZLIB_' "$B/CMakeCache.txt" >&2; exit 1; }
grep -Eq "^LIBLZMA_LIBRARY(_RELEASE)?:FILEPATH=$P/xz/" "$B/CMakeCache.txt" ||
  { echo "libarchive did not pick xz 5.8.4:" >&2; grep -E '^LIBLZMA_' "$B/CMakeCache.txt" >&2; exit 1; }
quiet cmake --build "$B" -j "$J" --target bsdtar
cp "$B/bin/bsdtar" "$P/bin/bsdtar"

src pigz https://github.com/madler/pigz/archive/refs/tags/v2.8.tar.gz \
  2f7f6a6986996d21cb8658535fff95f1c7107ddce22b5324f4b41890e2904706
quiet make -C "$S/pigz" -j "$J" pigz \
  CFLAGS="-O3 -Wall -Wextra -Wno-unknown-pragmas -Wcast-qual -I$P/zngc/include" \
  LIBS="$P/zngc/lib/libz.a -lm -lpthread"
cp "$S/pigz/pigz" "$P/bin/pigz"

get https://github.com/ip7z/7zip/releases/download/26.06/7z2606-linux-x64.tar.xz \
  2b539cf067b2ad48e1baa5093dbaa8818b73542d1222a0ffde9dc5f2922ea62e "$S/7zip.tar.xz"
tar -xJf "$S/7zip.tar.xz" -C "$P/bin" 7zz

rm -rf "$S"
v=$("$P/bin/7zz" 2>&1 || true)
grep -m1 7-Zip <<< "$v"
"$P/bin/bsdtar" --version
"$P/bin/pigz" --version 2>&1
