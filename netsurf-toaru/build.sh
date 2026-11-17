#!/bin/bash
# Build a static NetSurf 3.11 for ToaruOS 2.3.2.
#
#   ./build.sh
#
# Output: _build/netsurf/netsurf-3.11/netsurf
#
# Prerequisites:
#   - ToaruOS libc sysroot made by ../scripts/mktcc.sh   (_build/tcc-toaru/lib)
#   - ToaruOS source checkout                    (_build/toaruos-src,
#                                                 created by ../tests/run-toaru.sh)
#   - host gcc, GNU ld, python3, curl, make, perl
#
# The script downloads NetSurf 3.11, the toaruos/netsurf-frontend-toaru
# frontend and the NetSurf support libraries.  The downloaded tarballs are
# cached in _import/download (so they survive deleting _build); the sources
# are unpacked and built under _build/netsurf.
#
# Environment overrides:
#   NETSURF_DL       netsurf build dir       (default _build/netsurf)
#   NETSURF_CACHE    tarball cache           (default _import/download)
#   NETSURF_DEPS     dependency build dir    (default $NETSURF_DL/netsurf-deps)
#   NETSURF_PREFIX   cross install prefix    (default _build/netsurf-prefix)
#   TOARU_SYSROOT    ToaruOS sysroot         (default _build/tcc-toaru/lib)
#   TOARU_SRC        ToaruOS source tree     (default _build/toaruos-src)
#   SKIP_DEPS=1      reuse an already built $NETSURF_PREFIX
#   JOBS             parallel make jobs      (default nproc)
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$HERE/.." && pwd)

DL=${NETSURF_DL:-$TOP/_build/netsurf}
CACHE=${NETSURF_CACHE:-$TOP/_import/download}
DEPS=${NETSURF_DEPS:-$DL/netsurf-deps}
DEPS_CACHE=$CACHE/netsurf-deps
SRC=${NETSURF_SRC:-$DL/netsurf-3.11}
PREFIX=${NETSURF_PREFIX:-$TOP/_build/netsurf-prefix}
SYS=${TOARU_SYSROOT:-$TOP/_build/tcc-toaru/lib}
TOARU_SRC=${TOARU_SRC:-$TOP/_build/toaruos-src}
TOOL=$HERE/toolchain
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

NS_VER=3.11
FE_REV=f50f75909bc65020246e405f7972bebee5e7b022

die() { echo "error: $*" >&2; exit 1; }

[[ -f $SYS/crt0.o && -f $SYS/libc.a && -d $SYS/include ]] ||
    die "missing ToaruOS sysroot $SYS — run ../scripts/mktcc.sh"
[[ -f $TOARU_SRC/lib/yutani.c ]] ||
    die "missing ToaruOS sources in $TOARU_SRC — run ../tests/run-toaru.sh"
command -v gcc >/dev/null || die "missing host gcc"
command -v ld >/dev/null || die "missing GNU ld"
command -v curl >/dev/null || die "missing curl"
command -v python3 >/dev/null || die "missing python3"
command -v make >/dev/null || die "missing make"
command -v perl >/dev/null || die "missing perl"

fetch() {
    local url=$1 out=$2 legacy=${3:-}
    [[ -f $out ]] && return 0
    # An older version of this script kept the tarballs next to the build
    # trees; move such a file into the cache instead of downloading it again.
    if [[ -n $legacy && -f $legacy ]]; then
        echo "== moving $legacy to $out"
        mkdir -p "$(dirname "$out")"
        mv "$legacy" "$out"
        return 0
    fi
    echo "== downloading $url"
    mkdir -p "$(dirname "$out")"
    curl -L --fail --silent --show-error -o "$out" "$url"
}

export TOARU_SYSROOT=$SYS
export NETSURF_PREFIX=$PREFIX
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig
export PATH=$PREFIX/bin:$PATH

mkdir -p "$DL" "$DEPS" "$CACHE" "$DEPS_CACHE" \
    "$PREFIX/lib" "$PREFIX/include" "$PREFIX/lib/pkgconfig"

# NetSurf is linked with explicit startup objects; libc's main.o (which runs
# _libc_init) must precede the Toaru library constructors.  It is also needed
# by the configure probes of the dependencies below (zlib, libcurl), so create
# it before anything is built.
if [[ ! -f $PREFIX/lib/libc-main.o ]]; then
    tmp=$(mktemp -d)
    (cd "$tmp" && ar x "$SYS/libc.a" main.o && cp main.o "$PREFIX/lib/libc-main.o")
    rm -rf "$tmp"
fi

###############################################################################
# sources
###############################################################################

echo "== unpacking NetSurf $NS_VER"
rm -rf "$SRC"
fetch "https://download.netsurf-browser.org/netsurf/releases/source/netsurf-$NS_VER-src.tar.gz" \
    "$CACHE/netsurf-$NS_VER-src.tar.gz" "$DL/netsurf-$NS_VER-src.tar.gz"
tar xz -C "$DL" -f "$CACHE/netsurf-$NS_VER-src.tar.gz"
[[ -d $SRC ]] || die "NetSurf source did not unpack into $SRC"
(cd "$SRC" && patch -p1 --batch < "$HERE/patches/netsurf-$NS_VER.patch")

echo "== unpacking the ToaruOS frontend"
rm -rf "$DEPS/netsurf-frontend-toaru" "$DEPS/netsurf-frontend-toaru-$FE_REV"
FETAR=netsurf-frontend-toaru-$FE_REV.tar.gz
fetch "https://github.com/toaruos/netsurf-frontend-toaru/archive/$FE_REV.tar.gz" \
    "$DEPS_CACHE/$FETAR" "$DEPS/$FETAR"
tar xz -C "$DEPS" -f "$DEPS_CACHE/$FETAR"
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
        "$DEPS_CACHE/buildsystem-1.10.tar.gz" "$DEPS/buildsystem-1.10.tar.gz"
    fetch "https://ftp.gnu.org/gnu/gperf/gperf-3.1.tar.gz" "$DEPS_CACHE/gperf-3.1.tar.gz" "$DEPS/gperf-3.1.tar.gz"
    fetch "https://download.netsurf-browser.org/libs/releases/libwapcaplet-0.4.3-src.tar.gz" \
        "$DEPS_CACHE/libwapcaplet-0.4.3.tar.gz" "$DEPS/libwapcaplet-0.4.3.tar.gz"
    fetch "https://download.netsurf-browser.org/libs/releases/libparserutils-0.2.5-src.tar.gz" \
        "$DEPS_CACHE/libparserutils-0.2.5.tar.gz" "$DEPS/libparserutils-0.2.5.tar.gz"
    fetch "https://download.netsurf-browser.org/libs/releases/libhubbub-0.3.8-src.tar.gz" \
        "$DEPS_CACHE/libhubbub-0.3.8.tar.gz" "$DEPS/libhubbub-0.3.8.tar.gz"
    fetch "https://download.netsurf-browser.org/libs/releases/libcss-0.9.2-src.tar.gz" \
        "$DEPS_CACHE/libcss-0.9.2.tar.gz" "$DEPS/libcss-0.9.2.tar.gz"
    fetch "https://download.netsurf-browser.org/libs/releases/libdom-0.4.2-src.tar.gz" \
        "$DEPS_CACHE/libdom-0.4.2.tar.gz" "$DEPS/libdom-0.4.2.tar.gz"
    fetch "https://download.netsurf-browser.org/libs/releases/libnsutils-0.1.1-src.tar.gz" \
        "$DEPS_CACHE/libnsutils-0.1.1.tar.gz" "$DEPS/libnsutils-0.1.1.tar.gz"
    fetch "https://zlib.net/fossils/zlib-1.3.1.tar.gz" "$DEPS_CACHE/zlib-1.3.1.tar.gz" "$DEPS/zlib-1.3.1.tar.gz"
    fetch "https://curl.se/download/curl-8.4.0.tar.gz" "$DEPS_CACHE/curl-8.4.0.tar.gz" "$DEPS/curl-8.4.0.tar.gz"

    for t in buildsystem-1.10 gperf-3.1 libwapcaplet-0.4.3 libparserutils-0.2.5 \
             libhubbub-0.3.8 libcss-0.9.2 libdom-0.4.2 libnsutils-0.1.1 \
             zlib-1.3.1 curl-8.4.0; do
        [[ -d $DEPS/$t ]] || tar xz -C "$DEPS" -f "$DEPS_CACHE/$t.tar.gz"
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

    # libcurl for NetSurf's HTTP fetcher.  Plain HTTP only: there is no TLS
    # stack on ToaruOS.  The build is reduced to the HTTP protocol and uses
    # libcurl's own gethostbyname() resolver because ToaruOS's getaddrinfo()
    # ignores the service argument (the port would stay 0).
    echo "== building curl (HTTP only, no TLS)"
    if [[ ! -f $PREFIX/lib/libcurl.a ]]; then
        rm -rf "$DEPS/curl-8.4.0/build-toaru"
        mkdir -p "$DEPS/curl-8.4.0/build-toaru"
        (cd "$DEPS/curl-8.4.0/build-toaru" &&
            CC="$TOOL/gcc-toaru.sh" AR="$TOOL/ar-toaru.sh" RANLIB=ranlib \
            ../configure \
                --host=x86_64-unknown-linux-gnu --build=x86_64-linux-gnu \
                --prefix="$PREFIX" \
                --disable-shared --enable-static \
                --disable-threaded-resolver --disable-pthreads --disable-ipv6 \
                --without-ssl --without-gnutls --without-mbedtls \
                --without-wolfssl --without-bearssl --without-rustls \
                --without-libpsl --without-libidn2 --without-brotli \
                --without-zstd --without-zlib --without-nghttp2 \
                --without-nghttp3 --without-libssh2 --without-gssapi \
                --disable-ldap --disable-ldaps --disable-ftp --disable-file \
                --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
                --disable-pop3 --disable-imap --disable-smtp --disable-gopher \
                --disable-mqtt --disable-smb --disable-aws --disable-alt-svc \
                --disable-hsts --disable-websockets --disable-ntlm \
                --disable-ntlm-wb \
                ac_cv_func_inet_pton=no ac_cv_func_inet_ntop=no \
                ac_cv_func_gai_strerror=no ac_cv_func_gethostbyaddr=no \
                ac_cv_func_getservbyname=no ac_cv_func_select=no \
                ac_cv_func_pthread_create=no \
                curl_disallow_getaddrinfo=yes \
                >/dev/null &&
            make -j"$JOBS" -C include install >/dev/null &&
            make -j"$JOBS" -C lib >/dev/null &&
            make -C lib install >/dev/null &&
            make install-pkgconfigDATA >/dev/null)
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

###############################################################################
# NetSurf
###############################################################################

echo "== building NetSurf"
make -C "$SRC" -j"$JOBS" \
    TARGET=toaru HOST=x86_64-pc-toaru BUILD=x86_64-linux-gnu \
    CC="$TOOL/gcc-toaru.sh" AR="$TOOL/ar-toaru.sh" \
    PREFIX="$PREFIX" PKG_CONFIG=pkg-config

echo "== done: $SRC/netsurf"
