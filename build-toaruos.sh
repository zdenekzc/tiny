#!/bin/bash
# build-toaruos.sh — builds ToaruOS from its sources, two ways:
#
#   ./build-toaruos.sh docker   upstream method: util/build-in-docker.sh inside
#                               docker.io/toaruos/build-tools:1.99.x
#   ./build-toaruos.sh native   on this host with _import/gcc_local.tgz
#
# Both write a bootable ISO to _build/toaruos-<mode>/src/image.iso.
#
#   -j N   parallel make jobs for the native build (default: nproc; the docker
#          variant uses the -j4 fixed in upstream util/build-in-docker.sh)
#   -c     remove the build tree first (fresh source copy, toolchain, objects)
#   -r     run docker/podman through sudo (see the podman note below)
#   -h     this help
#
# Environment:
#   TOARU_REF   git tag or branch to build (default: v2.3.2)
#   IMAGE       image for the docker variant
#               (default: docker.io/toaruos/build-tools:1.99.x)
#   GCC_TGZ     native toolchain archive (default: _import/gcc_local.tgz)
#   JOBS        same as -j
#   ENGINE      docker|podman forces one for the docker variant
#
# The source tree is _build/toaruos-<mode>/src.  It is seeded from the cached
# _import/download/toaruos-src when it is the requested ref (run-toaru.sh
# already keeps v2.3.2 there) and cloned otherwise; the kuroko and bim
# submodules are initialized in either case.  Nothing in _import/ is modified,
# so the cached clone stays pristine.
#
# docker variant: util/build-in-docker.sh in the image creates a user with the
# UID that owns the source tree and runs make as that user.  That needs a
# rootful engine.  Docker is rootful by default; rootless Podman cannot run
# the script (it lacks the privilege for useradd and the workspace ownership
# does not match), so with Podman pass -r to use the root podman storage.
#
# native variant: extracts GCC_TGZ (fetch it with ./fetch-gcc-local.sh), makes
# util/local point at it, and runs the same make steps as the image:
#   make util/local/bin/kuroko; make base/lib/libc.so; make -jN
# The toolchain is gcc 10.3.0 for x86_64-pc-toaru and runs on the host (it is
# a normal Linux binary).  Two adjustments are needed outside the image:
#   * it was configured with --with-sysroot=/root/misaka/util/../base, a path
#     that only exists under the container's bind mount, so CC is overridden
#     with --sysroot=<source>/base;
#   * as/ld are dynamically linked against libopcodes/libbfd, which are not on
#     the default library path, so LD_LIBRARY_PATH points into the toolchain.
# The host needs xorriso, mtools, dosfstools and gnu-efi/gnu-efi-devel (on
# Fedora: dnf install xorriso mtools dosfstools gnu-efi gnu-efi-devel).
set -euo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD=$DIR/_build
CACHE=$DIR/_import/download/toaruos-src
PLATFORM=${PLATFORM:-linux/amd64}
TOARU_REF=${TOARU_REF:-v2.3.2}
IMAGE=${IMAGE:-docker.io/toaruos/build-tools:1.99.x}
GCC_TGZ=${GCC_TGZ:-$DIR/_import/gcc_local.tgz}

jobs=${JOBS:-$(nproc)}
clean=0
use_sudo=0

usage() {
    awk 'NR > 1 && /^set -euo pipefail$/ { exit }
         NR > 1 { sub(/^# ?/, ""); print }' "$0"
}

die() { echo "error: $*" >&2; exit 1; }
need() { command -v "$1" > /dev/null 2>&1 || die "missing program '$1'"; }

mode=
while [[ $# -gt 0 ]]; do
    case $1 in
        -j) [[ $# -ge 2 ]] || die "-j needs a value"; jobs=$2; shift 2 ;;
        -j*) jobs=${1#-j}; shift ;;
        -c) clean=1; shift ;;
        -r) use_sudo=1; shift ;;
        -h) usage; exit 0 ;;
        -*) usage >&2; exit 2 ;;
        *)
            [[ -z $mode ]] || die "unexpected argument '$1'"
            mode=$1
            shift
            ;;
    esac
done

case $mode in
    docker|native) ;;
    *) usage >&2; die "choose 'docker' or 'native'" ;;
esac

if (( use_sudo )) && ! command -v sudo > /dev/null 2>&1; then
    die "-r needs sudo"
fi
[[ $jobs =~ ^[0-9]+$ ]] && (( jobs > 0 )) || die "-j needs a positive number"

MODE_DIR=$BUILD/toaruos-$mode
SRC=$MODE_DIR/src

if (( clean )); then
    echo "== removing $MODE_DIR ..."
    rm -rf "$MODE_DIR"
fi

# ------------------------------------------------------------------ sources
prepare_src() {
    if [[ ! -d $SRC ]]; then
        mkdir -p "$MODE_DIR"
        if [[ -d $CACHE/.git ]] && \
           [[ $(git -C "$CACHE" describe --tags --always 2> /dev/null) == "$TOARU_REF" ]]; then
            echo "== using the cached sources from $CACHE ..."
            cp -a "$CACHE" "$SRC"
        else
            echo "== cloning ToaruOS $TOARU_REF ..."
            git clone --depth 1 --branch "$TOARU_REF" \
                https://github.com/klange/toaruos.git "$SRC" || die "git clone"
        fi
    fi
    git -C "$SRC" rev-parse --verify HEAD > /dev/null 2>&1 \
        || die "$SRC is not a git checkout (try -c)"
    echo "== initializing kuroko and bim submodules ..."
    git -C "$SRC" submodule update --init kuroko bim || die "git submodule update"
}

need git
prepare_src

# ------------------------------------------------------------------- docker
if [[ $mode == docker ]]; then
    prefix=()
    (( use_sudo )) && prefix=(sudo)

    if [[ -n ${ENGINE:-} ]]; then
        candidates=("$ENGINE")
    else
        candidates=(docker podman)
    fi
    engine=()
    for e in "${candidates[@]}"; do
        command -v "$e" > /dev/null 2>&1 || continue
        "${prefix[@]}" "$e" info > /dev/null 2>&1 || continue
        engine=("${prefix[@]}" "$e")
        break
    done
    [[ ${#engine[@]} -gt 0 ]] || die "no usable container engine (tried: ${candidates[*]})"

    if [[ ${engine[0]} == podman && ${#prefix[@]} -eq 0 ]] && \
       [[ $("${engine[@]}" info --format '{{.Host.Security.Rootless}}' 2> /dev/null) == true ]]; then
        die "rootless podman cannot run util/build-in-docker.sh (useradd + workspace ownership);
       re-run with -r to use the root podman storage"
    fi

    if ! "${engine[@]}" image inspect "$IMAGE" > /dev/null 2>&1; then
        echo "== pulling $IMAGE ..."
        "${engine[@]}" pull --platform "$PLATFORM" "$IMAGE" || die "pull failed"
    fi

    vol=$SRC:/root/misaka
    if command -v getenforce > /dev/null 2>&1 && [[ $(getenforce) != Disabled ]]; then
        vol=$vol:z
    fi

    echo "== building in $IMAGE ..."
    "${engine[@]}" run --rm --platform "$PLATFORM" -v "$vol" -w /root/misaka \
        -e LANG=C.UTF-8 "$IMAGE" util/build-in-docker.sh || die "build failed"
fi

# ------------------------------------------------------------------- native
TOOLCHAIN=$MODE_DIR/gcc_local
created_link=0
cleanup() {
    if (( created_link )); then
        rm -f "$SRC/util/local"
    fi
}
trap cleanup EXIT

if [[ $mode == native ]]; then
    missing=()
    for t in make cc python3 tar xorriso mkfs.fat mformat mcopy mmd; do
        command -v "$t" > /dev/null 2>&1 || missing+=("$t")
    done
    [[ -f /usr/lib/crt0-efi-x86_64.o && -f /usr/lib/elf_x86_64_efi.lds ]] || missing+=("gnu-efi")
    [[ -d /usr/include/efi && -f /usr/lib/libefi.a && -f /usr/lib/libgnuefi.a ]] || missing+=("gnu-efi-devel")
    if [[ ${#missing[@]} -gt 0 ]]; then
        die "missing native build dependencies: ${missing[*]}
       Fedora: sudo dnf install xorriso mtools dosfstools gnu-efi gnu-efi-devel
       Debian: sudo apt install xorriso mtools dosfstools gnu-efi libgnuefi-dev"
    fi

    if [[ ! -d $TOOLCHAIN ]]; then
        [[ -f $GCC_TGZ ]] || die "missing toolchain $GCC_TGZ — run ./fetch-gcc-local.sh first"
        echo "== extracting $GCC_TGZ ..."
        mkdir -p "$MODE_DIR"
        tar -xzf "$GCC_TGZ" -C "$MODE_DIR" || die "tar"
    fi
    [[ -x $TOOLCHAIN/bin/x86_64-pc-toaru-gcc ]] \
        || die "unexpected toolchain layout: $TOOLCHAIN/bin/x86_64-pc-toaru-gcc is missing"

    # util/activate.sh (called by the Makefile) puts util/local/bin on PATH.
    # A real directory here would swallow the symlink, so clear a stale one
    # (this is our own _build tree, not the cached clone).
    [[ -L $SRC/util/local ]] || rm -rf "$SRC/util/local"
    ln -sfn "$TOOLCHAIN" "$SRC/util/local"
    created_link=1

    # Build the host kuroko ourselves instead of using the Makefile's rule:
    # that rule invokes cc without -lm, and with current gcc/glibc the calls
    # to __builtin_floor in obj_numeric.c no longer resolve ("undefined
    # reference to floor").  The Makefile's order-only prerequisite
    # util/local/bin/kuroko then finds this binary up to date.
    echo "== building kuroko for the host ..."
    mkdir -p "$TOOLCHAIN/bin"
    cc -I"$SRC/kuroko/src" -DKRK_BUNDLE_LIBS="BUNDLED(os);BUNDLED(fileio);" \
        -DNO_RLINE -DKRK_STATIC_ONLY -DKRK_DISABLE_THREADS \
        -o "$TOOLCHAIN/bin/kuroko" "$SRC"/kuroko/src/*.c \
        "$SRC"/kuroko/src/modules/module_os.c \
        "$SRC"/kuroko/src/modules/module_fileio.c -lm \
        || die "kuroko build failed"

    # binutils (as, ld) need their shared libraries; gcc's configured
    # sysroot points at /root/misaka, which does not exist here.
    libdir=$TOOLCHAIN/x86_64-pc-linux-gnu/x86_64-pc-toaru/lib
    [[ -d $libdir ]] || die "missing $libdir"
    export LD_LIBRARY_PATH=$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    cross_cc="x86_64-pc-toaru-gcc --sysroot=$SRC/base"

    echo "== building libc ..."
    make -C "$SRC" CC="$cross_cc" base/lib/libc.so || die "libc build failed"
    echo "== building ToaruOS (make -j$jobs) ..."
    make -C "$SRC" CC="$cross_cc" -j"$jobs" || die "build failed"
fi

# ------------------------------------------------------------------- result
ISO=$SRC/image.iso
[[ -f $ISO ]] || die "build finished but $ISO is missing"
echo "== done: $ISO ($(du -h "$ISO" | cut -f1))"
