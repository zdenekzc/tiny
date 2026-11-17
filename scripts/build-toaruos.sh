#!/bin/bash
# build-toaruos.sh — builds ToaruOS from its sources, two ways (and two ISAs):
#
#   ./scripts/build-toaruos.sh docker         upstream method: util/build-in-docker.sh inside
#                                     docker.io/toaruos/build-tools:1.99.x
#   ./scripts/build-toaruos.sh native         on this host with _import/gcc_local.tgz
#   ./scripts/build-toaruos.sh -a <mode>      arm64: image :aarch64, toolchain
#                                     _import/gcc_arm64.tgz
#
# x86_64 writes a bootable ISO to _build/toaruos-<mode>/src/image.iso.
# aarch64 has no ISO; it writes _build/toaruos-<mode>-aarch64/src/
# {bootstub,misaka-kernel,ramdisk.igz,kernel8.img} (bootstub+kernel+initrd are
# handed to QEMU via fw_cfg, kernel8.img is for a Raspberry Pi 400).
#
#   -a      build the aarch64 target
#   -j N    parallel make jobs for the native build (default: nproc; the docker
#           variant uses the -j4 fixed in upstream util/build-in-docker*.sh)
#   -c      remove the build tree first (fresh source copy, toolchain, objects)
#   -r      run docker/podman through sudo (see the podman note below)
#   -h      this help
#
# Environment:
#   TOARU_REF   git tag or branch to build (default: v2.3.2)
#   IMAGE       image for the docker variant
#               (default: docker.io/toaruos/build-tools:1.99.x, -a: :aarch64)
#   GCC_TGZ     native toolchain archive
#               (default: _import/gcc_local.tgz, -a: _import/gcc_arm64.tgz)
#   JOBS        same as -j
#   ENGINE      docker|podman forces one for the docker variant
#
# The source tree is _build/toaruos-<mode>[-aarch64]/src.  It is seeded from
# the cached _import/download/toaruos-src when it is the requested ref
# (tests/build-toaru.sh or tests/run-toaru.sh already keep v2.3.2 there) and cloned otherwise; the kuroko
# and bim submodules are initialized in either case.  Nothing in _import/ is
# modified, so the cached clone stays pristine.
#
# docker variant: util/build-in-docker*.sh in the image creates a user with
# the UID that owns the source tree and runs make as that user.  That needs a
# rootful engine.  Docker is rootful by default; rootless Podman cannot run
# the script (it lacks the privilege for useradd and the workspace ownership
# does not match), so with Podman pass -r to use the root podman storage.
#
# native variant: extracts GCC_TGZ (fetch it with ./scripts/fetch-gcc-local.sh), makes
# util/local point at it, and runs the same make steps as the image:
#   make [ARCH=aarch64] util/local/bin/kuroko; ... base/lib/libc.so; make -jN
# The toolchain is gcc 10.3.0 (x86_64-pc-toaru or aarch64-unknown-toaru, a
# normal Linux binary either way).  Adjustments needed outside the image:
#   * it was configured with --with-sysroot=/root/misaka/util/../base, a path
#     that only exists under the container's bind mount, so CC is overridden
#     with --sysroot=<source>/base;
#   * as/ld are dynamically linked against libopcodes/libbfd, which are not on
#     the default library path, so LD_LIBRARY_PATH points into the toolchain;
#   * kuroko is built by this script with -lm: the Makefile rule invokes cc
#     without it and current gcc/glibc no longer resolves __builtin_floor.
# x86_64 additionally needs xorriso, mtools, dosfstools and gnu-efi
# (Fedora: dnf install xorriso mtools dosfstools gnu-efi gnu-efi-devel);
# aarch64 builds no ISO and needs none of those.
#
# The aarch64 result runs headless with (from the src directory):
#   qemu-system-aarch64 -M virt -cpu cortex-a72 -m 4G -smp 4 \
#     -display none -serial mon:stdio -no-reboot -kernel bootstub \
#     -append "root=/dev/ram0 migrate start=--headless" \
#     -fw_cfg name=opt/org.toaruos.kernel,file=misaka-kernel \
#     -fw_cfg name=opt/org.toaruos.initrd,file=ramdisk.igz
# (the same thing as `make ARCH=aarch64 shell`, which needs util/local back).
set -euo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=$DIR/_build
CACHE=$DIR/_import/download/toaruos-src
PLATFORM=${PLATFORM:-linux/amd64}
TOARU_REF=${TOARU_REF:-v2.3.2}

jobs=${JOBS:-$(nproc)}
clean=0
use_sudo=0
arch=x86_64

usage() {
    awk 'NR > 1 && /^set -euo pipefail$/ { exit }
         NR > 1 { sub(/^# ?/, ""); print }' "$0"
}

die() { echo "error: $*" >&2; exit 1; }
. "$DIR/scripts/need.sh"   # need(): dependency check with package-install hints

mode=
while [[ $# -gt 0 ]]; do
    case $1 in
        -j) [[ $# -ge 2 ]] || die "-j needs a value"; jobs=$2; shift 2 ;;
        -j*) jobs=${1#-j}; shift ;;
        -a) arch=aarch64; shift ;;
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

# Per-arch defaults.  Both images are linux/amd64 (they are cross toolchains),
# so PLATFORM does not change with -a.
case $arch in
    x86_64)
        IMAGE=${IMAGE:-docker.io/toaruos/build-tools:1.99.x}
        GCC_TGZ=${GCC_TGZ:-$DIR/_import/gcc_local.tgz}
        cc=x86_64-pc-toaru
        libsub=x86_64-pc-toaru
        docker_script=util/build-in-docker.sh
        dir_suffix=
        ;;
    aarch64)
        IMAGE=${IMAGE:-docker.io/toaruos/build-tools:aarch64}
        GCC_TGZ=${GCC_TGZ:-$DIR/_import/gcc_arm64.tgz}
        cc=aarch64-unknown-toaru
        libsub=aarch64-unknown-toaru
        docker_script=util/build-in-docker-aarch64.sh
        dir_suffix=-aarch64
        ;;
    *) die "unsupported arch '$arch'" ;;
esac

MODE_DIR=$BUILD/toaruos-$mode$dir_suffix
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

# ------------------------------------------------------------------ patches
# Repository-local fixes for ToaruOS itself (applied to the working copy, the
# pristine _import clone stays untouched).  Idempotent: a marker check skips
# already-applied patches, so re-runs and cached builds are safe.
if [[ $arch == aarch64 ]]; then
    for p in "$DIR"/toaruos-patches/*.patch; do
        [[ -e $p ]] || continue
        # idempotence: the first line the patch adds is the marker — if the
        # target file already contains it, the patch is applied.
        marker=$(grep '^+[^+]' "$p" | head -1 | sed 's/^+//')
        if grep -qsF "$marker" "$SRC/kernel/arch/aarch64/virtio.c" 2>/dev/null; then
            echo "== patch already applied: $(basename "$p")"
            continue
        fi
        echo "== applying $(basename "$p") ..."
        (cd "$SRC" && patch -p1 -s < "$p") || die "patch $p"
    done
fi

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
        die "rootless podman cannot run $docker_script (useradd + workspace ownership);
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
        -e LANG=C.UTF-8 "$IMAGE" "$docker_script" || die "build failed"
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
    for t in make cc python3 tar; do
        command -v "$t" > /dev/null 2>&1 || missing+=("$t")
    done
    if [[ $arch == x86_64 ]]; then
        # only the ISO build needs these
        for t in xorriso mkfs.fat mformat mcopy mmd; do
            command -v "$t" > /dev/null 2>&1 || missing+=("$t")
        done
        [[ -f /usr/lib/crt0-efi-x86_64.o && -f /usr/lib/elf_x86_64_efi.lds ]] || missing+=("gnu-efi")
        [[ -d /usr/include/efi && -f /usr/lib/libefi.a && -f /usr/lib/libgnuefi.a ]] || missing+=("gnu-efi-devel")
        if [[ ${#missing[@]} -gt 0 ]]; then
            die "missing native build dependencies: ${missing[*]}
       Fedora: sudo dnf install xorriso mtools dosfstools gnu-efi gnu-efi-devel
       Debian: sudo apt install xorriso mtools dosfstools gnu-efi libgnuefi-dev"
        fi
    fi
    if [[ ${#missing[@]} -gt 0 ]]; then
        die "missing native build dependencies: ${missing[*]}"
    fi

    if [[ ! -d $TOOLCHAIN ]]; then
        [[ -f $GCC_TGZ ]] || die "missing toolchain $GCC_TGZ — run ./scripts/fetch-gcc-local.sh first"
        echo "== extracting $GCC_TGZ ..."
        mkdir -p "$MODE_DIR"
        tar -xzf "$GCC_TGZ" -C "$MODE_DIR" || die "tar"
    fi
    [[ -x $TOOLCHAIN/bin/$cc-gcc ]] \
        || die "unexpected toolchain layout: $TOOLCHAIN/bin/$cc-gcc is missing"

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
    libdir=$TOOLCHAIN/x86_64-pc-linux-gnu/$libsub/lib
    [[ -d $libdir ]] || die "missing $libdir"
    export LD_LIBRARY_PATH=$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    cross_cc="$cc-gcc --sysroot=$SRC/base"

    echo "== building libc ..."
    make -C "$SRC" ARCH=$arch CC="$cross_cc" base/lib/libc.so || die "libc build failed"
    echo "== building ToaruOS (make -j$jobs) ..."
    make -C "$SRC" ARCH=$arch CC="$cross_cc" -j"$jobs" || die "build failed"
fi

# ------------------------------------------------------------------- result
if [[ $arch == x86_64 ]]; then
    ISO=$SRC/image.iso
    [[ -f $ISO ]] || die "build finished but $ISO is missing"
    echo "== done: $ISO ($(du -h "$ISO" | cut -f1))"
else
    artifacts=(bootstub misaka-kernel ramdisk.igz kernel8.img)
    for f in "${artifacts[@]}"; do
        [[ -f $SRC/$f ]] || die "build finished but $SRC/$f is missing"
    done
    echo "== done (in $SRC):"
    (cd "$SRC" && du -h "${artifacts[@]}")
fi
