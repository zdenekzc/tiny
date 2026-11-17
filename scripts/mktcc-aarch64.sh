#!/bin/bash
# mktcc-aarch64.sh — TinyCC for the aarch64 port of ToaruOS 2.3.2.
#
#   ./scripts/mktcc-aarch64.sh
#
# The aarch64 counterpart of mktcc.sh.  Everything needed comes from the
# official aarch64-virt.zip release (the same artifacts tests/run-arm64.sh
# boots): its ramdisk.igz contains the complete sysroot (lib/libc.a,
# lib/crt0.o, lib/crti.o, lib/crtn.o, usr/include/) — ToaruOS does not have
# to be built.
#
# Produces:
#   _build/tcc-toaru-aarch64/bin/aarch64-toaru-tcc
#       cross compiler: runs on the x86_64 host, generates static aarch64
#       ToaruOS executables (ET_EXEC at the arm64 default 0x400000, the
#       aarch64 kernel loads PT_LOAD as found, no PIE/ld.so involved)
#   _build/tcc-toaru-aarch64/lib/{crt0.o,crti.o,crtn.o,libc.a,libtcc1.a,include/}
#       the aarch64 sysroot + libtcc1 compiled by the cross tcc itself
#   _build/tcc-toaru-aarch64/tcc-guest-aarch64
#       a tcc that runs INSIDE the aarch64 ToaruOS (same recipe as the x86
#       tcc-guest: compiled by the cross compiler, no -run support)
#
# The guest side is expected under /extras/tcc (CONFIG_TCCDIR), see the
# payload packing in the test scripts.
set -euo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
B=$DIR/_build
IMPORT=$DIR/_import/download
SRC=$IMPORT/tinycc           # the patched TinyCC clone (shared with mktcc.sh)
GEN=$B/gen-aarch64
OUT=$B/tcc-toaru-aarch64
LIB=$OUT/lib
REL=$IMPORT/aarch64-virt
REL_ZIP=$IMPORT/aarch64-virt.zip
REL_URL="https://github.com/klange/toaruos/releases/download/v2.3.2/aarch64-virt.zip"

die() { echo "error: $*" >&2; exit 1; }
. "$DIR/scripts/need.sh"

need gcc
need unzip
need python3

[[ -d $SRC ]] || die "missing TinyCC clone $SRC — run ./scripts/mktcc.sh first"
git -C "$SRC" rev-parse -q --verify HEAD > /dev/null || die "$SRC is not a git clone"

# ------------------------------------------------- 1) sysroot from the release
if [[ ! -f $LIB/libc.a ]]; then
    if [[ ! -f $REL/ramdisk.igz ]]; then
        if [[ ! -f $REL_ZIP ]]; then
            echo "== downloading the official aarch64-virt.zip ..."
            mkdir -p "$IMPORT"
            curl -L --fail -o "${REL_ZIP}.part" "${REL_URL}" || die "curl aarch64-virt.zip"
            mv "${REL_ZIP}.part" "${REL_ZIP}"
        fi
        mkdir -p "$REL"
        unzip -o "${REL_ZIP}" -d "${REL}" > /dev/null || die "unzip aarch64-virt.zip"
    fi
    echo "== extracting the aarch64 sysroot from the release ramdisk ..."
    W=$(mktemp -d "$B/a64sysroot.XXXXXX")
    gzip -dc "$REL/ramdisk.igz" | tar -x -C "$W" \
        lib/crt0.o lib/crti.o lib/crtn.o lib/libc.a usr/include \
        || die "extracting the sysroot from the release ramdisk"
    mkdir -p "$LIB/include"
    cp "$W"/lib/crt0.o "$W"/lib/crti.o "$W"/lib/crtn.o "$W"/lib/libc.a "$LIB/"
    # tcc's own headers first (tccdefs.h, stdarg.h, ...), the ToaruOS ones on top
    cp "$SRC"/include/*.h "$LIB/include/"
    cp -a "$W/usr/include/." "$LIB/include/"
    rm -rf "$W"
fi

# --------------------------------------------------------- 2) config (arm64)
# Same file as the x86_64 target, only the code generator differs.  The aarch64
# userland is compiled by gcc with the standard AAPCS64 ABI, which is also what
# tcc's arm64 backend emits — no arm64 counterpart of the ELF_START_ADDR patch
# is needed: 0x400000 (the arm64 default) is where every stock aarch64 image
# lives and the kernel loads PT_LOAD as found.
mkdir -p "$GEN"
sed 's/#define TCC_TARGET_X86_64 1/#define TCC_TARGET_ARM64 1/' \
    "$DIR/tcc/config-toaru.h" > "$GEN/config.h"

# ------------------------------------------------------- 3) cross compiler
mkdir -p "$OUT/bin"
echo "building aarch64-toaru-tcc (cross: host x86_64 -> aarch64 ToaruOS) ..."
gcc -O1 -w -DTOARU_HOST_LINUX \
    -DCONFIG_TCCDIR="\"$LIB\"" \
    -I"$GEN" -I"$SRC" "$SRC/tcc.c" \
    -o "$OUT/bin/aarch64-toaru-tcc" || die "cross tcc build"
chmod 755 "$OUT/bin/aarch64-toaru-tcc"

# ------------------------------------------------------------ 4) libtcc1.a
# Object list for arm64 from tcc's lib/Makefile (OBJ-arm64): there is no
# libtcc1.o on arm64 — lib-arm64.c provides the division helpers; va_list.c is
# x86_64-only.  The cross tcc compiles its own runtime (it generates aarch64
# objects while running on the host).
TCC=$OUT/bin/aarch64-toaru-tcc
O=$OUT/obj-libtcc1
rm -rf "$O"; mkdir -p "$O"
for f in lib-arm64.c stdatomic.c builtin.c dsohandle.c armflush.c; do
    "$TCC" -B"$LIB" -c "$SRC/lib/$f" -o "$O/${f%.c}.o" || die "tcc -c lib/$f"
done
for f in atomic.S alloca.S alloca-bt.S; do
    "$TCC" -B"$LIB" -c "$SRC/lib/$f" -o "$O/${f%.S}.o" || die "tcc -c lib/$f"
done
# the libatomic helper the static aarch64 libc.a expects (see the file)
"$TCC" -B"$LIB" -c "$DIR/tcc/toaru/aarch64-atomic-helpers.S" \
    -o "$O/aarch64-atomic-helpers.o" || die "tcc -c aarch64-atomic-helpers.S"
"$TCC" -B"$LIB" -ar rcs "$LIB/libtcc1.a" "$O"/*.o || die "ar libtcc1.a"
echo "libtcc1.a: $(stat -c%s "$LIB/libtcc1.a") B"

# ------------------------------------------------------- 5) tcc in the guest
# Same recipe as the x86_64 tcc-guest: the cross compiler compiles tcc itself
# for aarch64, linked against the ToaruOS aarch64 libc.  -DTOARU_TCC_NO_NATIVE
# disables -run (no mmap/mprotect in the kernel); guest-compat.c supplies
# strtold and ldexpl, which the ToaruOS libc does not have.
"$TCC" -B"$LIB" -DTOARU_TCC_NO_NATIVE \
    -DCONFIG_TCCDIR='"/extras/tcc"' \
    -DCONFIG_TCC_CRTPREFIX='"{B}/lib"' \
    -DCONFIG_TCC_LIBPATHS='"{B}/lib"' \
    -I"$GEN" -I"$SRC" -o "$OUT/tcc-guest-aarch64" \
    "$SRC/tcc.c" "$DIR/tcc/toaru/guest-compat.c" \
    "$DIR/tcc/toaru/aarch64-atomic-helpers.S" \
    || die "compile tcc for aarch64 ToaruOS (tcc-guest-aarch64)"
chmod 755 "$OUT/tcc-guest-aarch64"
echo "tcc-guest-aarch64: $(stat -c%s "$OUT/tcc-guest-aarch64") B"

# ------------------------------------------------------------ 6) smoke test
# Build one example with the cross compiler and run the same image checks the
# x86_64 flow uses (the aarch64 kernel only runs ET_EXEC without PT_INTERP
# either — measured on the stock images, which are dynamic and rely on ld.so).
TESTDIR=$(mktemp -d "$B/a64test.XXXXXX")
"$TCC" -o "$TESTDIR/hello" "$DIR/tests/examples/tcc_hello.c" || die "tcc hello"
file "$TESTDIR/hello" 2>/dev/null | sed 's/^/  /'

die2() { rm -rf "$TESTDIR"; die "$1"; }
ehdr=$(readelf -hW "$TESTDIR/hello" 2>/dev/null)
phdr=$(readelf -lW "$TESTDIR/hello" 2>/dev/null)
type=$(sed -n 's/^ *Type:  *\([A-Z]*\).*/\1/p' <<<"$ehdr")
[[ $type == EXEC ]] || die2 "hello: ELF type is '$type', the kernel only runs ET_EXEC"
if grep -qE '^[[:space:]]+(INTERP|DYNAMIC)[[:space:]]' <<<"$phdr"; then
    die2 "hello: PT_INTERP/PT_DYNAMIC found — the kernel would start ld.so"
fi
echo "hello: $(stat -c%s "$TESTDIR/hello") B, static aarch64 ET_EXEC — OK"
rm -rf "$TESTDIR"

echo "done. cross tcc: $OUT/bin/aarch64-toaru-tcc"
echo "guest tcc:      $OUT/tcc-guest-aarch64  (install under /extras/tcc/bin/tcc)"
