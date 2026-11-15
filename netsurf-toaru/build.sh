#!/bin/bash
# Build a static NetSurf 3.11 for ToaruOS 2.3.2.
#
#   ./build.sh
#
# Output: _build/download/netsurf-3.11/netsurf
#
# Prerequisites:
#   - ToaruOS libc sysroot made by ../mktcc.sh   (_build/tcc-toaru/lib)
#   - ToaruOS source checkout                    (_build/download/toaruos-src,
#                                                 created by ../tests/run-toaru.sh)
#   - host gcc, GNU ld, python3, curl, make, perl
#
# The script downloads NetSurf 3.11, the toaruos/netsurf-frontend-toaru
# frontend and the NetSurf support libraries into _build/download/netsurf-deps
# unless they are already cached there.
#
# Environment overrides:
#   NETSURF_DL       download directory      (default _build/download)
#   NETSURF_DEPS     dependency build dir    (default $NETSURF_DL/netsurf-deps)
#   NETSURF_PREFIX   cross install prefix    (default _build/netsurf-prefix)
#   TOARU_SYSROOT    ToaruOS sysroot         (default _build/tcc-toaru/lib)
#   TOARU_SRC        ToaruOS source tree     (default $NETSURF_DL/toaruos-src)
#   SKIP_DEPS=1      reuse an already built $NETSURF_PREFIX
#   JOBS             parallel make jobs      (default nproc)
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$HERE/.." && pwd)

DL=${NETSURF_DL:-$TOP/_build/download}
DEPS=${NETSURF_DEPS:-$DL/netsurf-deps}
SRC=${NETSURF_SRC:-$DL/netsurf-3.11}
PREFIX=${NETSURF_PREFIX:-$TOP/_build/netsurf-prefix}
SYS=${TOARU_SYSROOT:-$TOP/_build/tcc-toaru/lib}
TOARU_SRC=${TOARU_SRC:-$DL/toaruos-src}
TOOL=$HERE/toolchain
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

NS_VER=3.11
FE_REV=f50f75909bc65020246e405f7972bebee5e7b022

die() { echo "error: $*" >&2; exit 1; }

[[ -f $SYS/crt0.o && -f $SYS/libc.a && -d $SYS/include ]] ||
    die "missing ToaruOS sysroot $SYS — run ../mktcc.sh"
[[ -f $TOARU_SRC/lib/yutani.c ]] ||
    die "missing ToaruOS sources in $TOARU_SRC — run ../tests/run-toaru.sh"
command -v gcc >/dev/null || die "missing host gcc"
command -v ld >/dev/null || die "missing GNU ld"
command -v curl >/dev/null || die "missing curl"
command -v python3 >/dev/null || die "missing python3"
command -v make >/dev/null || die "missing make"
command -v perl >/dev/null || die "missing perl"

fetch() {
    local url=$1 out=$2
    [[ -f $out ]] && return 0
    echo "== downloading $url"
    mkdir -p "$(dirname "$out")"
    curl -L --fail --silent --show-error -o "$out" "$url"
}

export TOARU_SYSROOT=$SYS
export NETSURF_PREFIX=$PREFIX
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig
export PATH=$PREFIX/bin:$PATH

mkdir -p "$DEPS" "$PREFIX/lib" "$PREFIX/include" "$PREFIX/lib/pkgconfig"

###############################################################################
# sources
###############################################################################

echo "== unpacking NetSurf $NS_VER"
rm -rf "$SRC"
fetch "https://download.netsurf-browser.org/netsurf/releases/source/netsurf-$NS_VER-src.tar.gz" \
    "$DL/netsurf-$NS_VER-src.tar.gz"
tar xz -C "$DL" -f "$DL/netsurf-$NS_VER-src.tar.gz"
[[ -d $SRC ]] || die "NetSurf source did not unpack into $SRC"
(cd "$SRC" && patch -p1 --batch < "$HERE/patches/netsurf-$NS_VER.patch")

echo "== unpacking the ToaruOS frontend"
rm -rf "$DEPS/netsurf-frontend-toaru" "$DEPS/netsurf-frontend-toaru-$FE_REV"
fetch "https://github.com/toaruos/netsurf-frontend-toaru/archive/$FE_REV.tar.gz" \
    "$DEPS/netsurf-frontend-toaru-$FE_REV.tar.gz"
tar xz -C "$DEPS" -f "$DEPS/netsurf-frontend-toaru-$FE_REV.tar.gz"
mv "$DEPS/netsurf-frontend-toaru-$FE_REV" "$DEPS/netsurf-frontend-toaru"
(cd "$DEPS/netsurf-frontend-toaru" && \
    patch -p1 --batch < "$HERE/patches/netsurf-frontend-toaru.patch")
rm -rf "$SRC/frontends/toaru"
cp -a "$DEPS/netsurf-frontend-toaru" "$SRC/frontends/toaru"

echo "== patching ToaruOS graphics for static PNG/JPEG decoding"
if ! grep -q 'add_lib("jpeg"' "$TOARU_SRC/lib/graphics.c"; then
    (cd "$TOARU_SRC" && patch -p1 --batch < "$HERE/patches/toaruos-static-sprites.patch")
fi

###############################################################################
# dependencies
###############################################################################

if [[ ${SKIP_DEPS:-0} != 1 ]]; then
    fetch "https://download.netsurf-browser.org/libs/releases/buildsystem-1.10.tar.gz" \
        "$DEPS/buildsystem-1.10.tar.gz"
    fetch "https://ftp.gnu.org/gnu/gperf/gperf-3.1.tar.gz" "$DEPS/gperf-3.1.tar.gz"
    fetch "https://download.netsurf-browser.org/libs/releases/libwapcaplet-0.4.3-src.tar.gz" \
        "$DEPS/libwapcaplet-0.4.3.tar.gz"
    fetch "https://download.netsurf-browser.org/libs/releases/libparserutils-0.2.5-src.tar.gz" \
        "$DEPS/libparserutils-0.2.5.tar.gz"
    fetch "https://download.netsurf-browser.org/libs/releases/libhubbub-0.3.8-src.tar.gz" \
        "$DEPS/libhubbub-0.3.8.tar.gz"
    fetch "https://download.netsurf-browser.org/libs/releases/libcss-0.9.2-src.tar.gz" \
        "$DEPS/libcss-0.9.2.tar.gz"
    fetch "https://download.netsurf-browser.org/libs/releases/libdom-0.4.2-src.tar.gz" \
        "$DEPS/libdom-0.4.2.tar.gz"
    fetch "https://download.netsurf-browser.org/libs/releases/libnsutils-0.1.1-src.tar.gz" \
        "$DEPS/libnsutils-0.1.1.tar.gz"
    fetch "https://zlib.net/fossils/zlib-1.3.1.tar.gz" "$DEPS/zlib-1.3.1.tar.gz"

    for t in buildsystem-1.10 gperf-3.1 libwapcaplet-0.4.3 libparserutils-0.2.5 \
             libhubbub-0.3.8 libcss-0.9.2 libdom-0.4.2 libnsutils-0.1.1 \
             zlib-1.3.1; do
        [[ -d $DEPS/$t ]] || tar xz -C "$DEPS" -f "$DEPS/$t.tar.gz"
    done

    # libparserutils defaults to an iconv-based input filter.  ToaruOS only
    # has iconv stubs (iconv_open() always fails with EINVAL), which makes
    # every parserutils_inputstream_create() fail and, with it, HTML content
    # creation (dom_hubbub_parser_create()).  Use parserutils' built-in
    # codecs instead, as its Makefile.config suggests.
    pu_conf="$DEPS/libparserutils-0.2.5/Makefile.config"
    grep -q '^CFLAGS.*WITHOUT_ICONV_FILTER' "$pu_conf" ||
        echo 'CFLAGS := $(CFLAGS) -DWITHOUT_ICONV_FILTER' >> "$pu_conf"

    # libdom builds an optional expat-driven XML binding by default.  NetSurf
    # parses HTML through the hubbub binding and does not need it; expat is
    # not available in the ToaruOS sysroot.
    ld_conf="$DEPS/libdom-0.4.2/Makefile.config"
    sed -i 's/^WITH_EXPAT_BINDING.*/WITH_EXPAT_BINDING := no/' "$ld_conf"

    echo "== building gperf (host tool)"
    if [[ ! -x $PREFIX/bin/gperf ]]; then
        (cd "$DEPS/gperf-3.1" &&
            ./configure --prefix="$PREFIX" >/dev/null &&
            make -j"$JOBS" >/dev/null && make install >/dev/null)
    fi

    build_ns_lib() {
        local dir=$1
        echo "== building $(basename "$dir")"
        make -C "$dir" -j"$JOBS" \
            HOST=x86_64-pc-toaru BUILD=x86_64-linux-gnu \
            CC="$TOOL/gcc-toaru.sh" AR="$TOOL/ar-toaru.sh" \
            NSSHARED="$DEPS/buildsystem-1.10" PREFIX="$PREFIX" \
            install
    }

    build_ns_lib "$DEPS/libwapcaplet-0.4.3"
    build_ns_lib "$DEPS/libparserutils-0.2.5"
    build_ns_lib "$DEPS/libhubbub-0.3.8"
    build_ns_lib "$DEPS/libcss-0.9.2"
    build_ns_lib "$DEPS/libnsutils-0.1.1"
    build_ns_lib "$DEPS/libdom-0.4.2"

    echo "== building zlib"
    if [[ ! -f $PREFIX/lib/libz.a ]]; then
        mkdir -p "$DEPS/zlib-1.3.1/build-toaru"
        (cd "$DEPS/zlib-1.3.1/build-toaru" &&
            CC="$TOOL/gcc-toaru.sh" AR="$TOOL/ar-toaru.sh" \
                ../configure --prefix="$PREFIX" >/dev/null &&
            make -j"$JOBS" >/dev/null && make install >/dev/null)
    fi
fi

###############################################################################
# ToaruOS client libraries and libc startup object
###############################################################################

echo "== building ToaruOS client libraries"
OBJ=$PREFIX/obj-toaru
mkdir -p "$OBJ"
TOARU_LIBS="auth button confreader decorations graphics hashmap icon_cache
            inflate jpeg json kbd list markup markup_text menu pex png procfs
            rline_exp text tree yutani"
for src in $TOARU_LIBS; do
    # toaru/spinlock.h calls syscall_yield() without a declaration; newer GCC
    # rejects that by default, so downgrade it to a warning.
    "$TOOL/gcc-toaru.sh" -c -std=gnu11 -DNO_SSE \
        -Wno-error=implicit-function-declaration \
        "$TOARU_SRC/lib/$src.c" -o "$OBJ/$src.o"
done
rm -f "$PREFIX/lib/libtoaru_all.a"
"$TOOL/ar-toaru.sh" rcs "$PREFIX/lib/libtoaru_all.a" "$OBJ"/*.o
for lib in menu yutani decorations button text graphics hashmap list; do
    ln -sf libtoaru_all.a "$PREFIX/lib/libtoaru_$lib.a"
done

# NetSurf is linked with explicit startup objects; libc's main.o (which runs
# _libc_init) must precede the Toaru library constructors.
if [[ ! -f $PREFIX/lib/libc-main.o ]]; then
    tmp=$(mktemp -d)
    (cd "$tmp" && ar x "$SYS/libc.a" main.o && cp main.o "$PREFIX/lib/libc-main.o")
    rm -rf "$tmp"
fi

###############################################################################
# NetSurf
###############################################################################

echo "== building NetSurf"
make -C "$SRC" -j"$JOBS" \
    TARGET=toaru HOST=x86_64-pc-toaru BUILD=x86_64-linux-gnu \
    CC="$TOOL/gcc-toaru.sh" AR="$TOOL/ar-toaru.sh" \
    PREFIX="$PREFIX" PKG_CONFIG=pkg-config

echo "== done: $SRC/netsurf"
