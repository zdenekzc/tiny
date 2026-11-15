#!/bin/bash
# Build the Nuklear IDE for ToaruOS 2.3.2.
#
#   ./build.sh                        # -> _output/nuklear-ide-toaru
#   ./build.sh /path/to/nuklear-ide   # explicit output
#
# The IDE is a static ToaruOS executable: Toaru's libc and the Yutani/graphics
# client sources are linked in, so the only runtime dependency is the
# compositor.  The in-guest compiler used by the IDE's Build/Run commands is
# TinyCC from the ramdisk (/extras/tcc), not this build.
#
# Requires the sysroot made by ../mktcc.sh and a ToaruOS source checkout
# (../tests/run-toaru.sh creates both).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
IDE_DIR=$(cd "$HERE/.." && pwd)
SYS=${SYSROOT:-$TOP/_build/tcc-toaru/lib}
TOARU_SRC=${TOARU_SRC:-$TOP/_build/download/toaruos-src}
OUT=${1:-$TOP/_output/nuklear-ide-toaru}
ELFPAD=$TOP/toaru-elfpad.py
CC=${CC:-gcc}
LD=${LD:-ld}

die() { echo "error: $*" >&2; exit 1; }
[[ -f $SYS/crt0.o && -f $SYS/libc.a && -d $SYS/include ]] ||
    die "missing sysroot $SYS — run ./mktcc.sh"
[[ -f $TOARU_SRC/lib/yutani.c ]] ||
    die "missing ToaruOS sources in $TOARU_SRC — run ./tests/run-toaru.sh"
[[ -f $ELFPAD ]] || die "missing $ELFPAD"
command -v "$CC" >/dev/null || die "missing $CC"
command -v "$LD" >/dev/null || die "missing GNU $LD"
command -v python3 >/dev/null || die "missing python3"

mkdir -p "$TOP/_tmp"
WORK=$(mktemp -d "$TOP/_tmp/toaru-ide.XXXXXX") || die "mktemp"
trap 'rm -rf "$WORK"' EXIT

COMMON="-O2 -g -std=gnu11 -ffreestanding -nostdinc
        -isystem $SYS/include -I$IDE_DIR -isystem $TOARU_SRC
        -fno-pie -fno-pic -fno-stack-protector -mno-red-zone"

echo "== compiling the IDE"
"$CC" $COMMON -DIDE_TOARU -Wall -Wextra -Wno-unused-parameter \
    -Wno-builtin-macro-redefined \
    -c "$IDE_DIR/main.c" -o "$WORK/main.o"

# The Yutani client depends on these small utility libraries.  Their sources
# are taken from the ToaruOS tree; compiling them here keeps the image static.
for src in yutani hashmap list pex kbd; do
    echo "== compiling ToaruOS lib/$src.c"
    "$CC" $COMMON -w -c "$TOARU_SRC/lib/$src.c" -o "$WORK/$src.o"
done

echo "== linking"
"$LD" -m elf_x86_64 -static -Ttext-segment=0x3F000000 \
    -o "$WORK/nuklear-ide" \
    "$SYS/crt0.o" "$SYS/crti.o" \
    "$WORK/main.o" "$WORK/yutani.o" "$WORK/hashmap.o" "$WORK/list.o" \
    "$WORK/pex.o" "$WORK/kbd.o" \
    "$SYS/crtn.o" -L"$SYS" -lc

echo "== padding ELF segments"
mkdir -p "$(dirname "$OUT")"
python3 -I "$ELFPAD" "$WORK/nuklear-ide" "$OUT"
python3 -I "$ELFPAD" --check "$OUT"
chmod 755 "$OUT"
echo "done: $OUT ($(stat -c%s "$OUT") B)"
