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
# Requires the sysroot made by ../scripts/mktcc.sh and a ToaruOS source checkout
# (../tests/run-toaru.sh creates both).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
IDE_DIR=$(cd "$HERE/.." && pwd)
SYS=${SYSROOT:-$TOP/_build/tcc-toaru/lib}
TOARU_SRC=${TOARU_SRC:-$TOP/_build/download/toaruos-src}
OUT=${1:-$TOP/_output/nuklear-ide-toaru}
ELFPAD=$TOP/scripts/toaru-elfpad.py
CC=${CC:-gcc}
LD=${LD:-ld}
AR=${AR:-ar}

die() { echo "error: $*" >&2; exit 1; }

# The Nuklear headers are a download, not a tracked file: fetch them when they
# are missing (same pinned upstream commit as the Linux build).
NK_HEADER=$IDE_DIR/../_import/nuklear-ide/nuklear.h
if [[ ! -f $NK_HEADER ]]; then
    echo "== Nuklear headers missing — running fetch-nuklear.sh"
    "$IDE_DIR/fetch-nuklear.sh" || die "fetch-nuklear.sh failed"
fi

[[ -f $SYS/crt0.o && -f $SYS/libc.a && -d $SYS/include ]] ||
    die "missing sysroot $SYS — run ./scripts/mktcc.sh"
[[ -f $TOARU_SRC/lib/yutani.c ]] ||
    die "missing ToaruOS sources in $TOARU_SRC — run ./tests/run-toaru.sh"
[[ -f $ELFPAD ]] || die "missing $ELFPAD"
command -v "$CC" >/dev/null || die "missing $CC"
command -v "$LD" >/dev/null || die "missing GNU $LD"
command -v python3 >/dev/null || die "missing python3"

mkdir -p "$TOP/_tmp"
WORK=$(mktemp -d "$TOP/_tmp/toaru-ide.XXXXXX") || die "mktemp"
trap 'rm -rf "$WORK"' EXIT

# The menu/decorations code loads PNG icons through the sprite loader.  A
# static binary cannot dlopen libtoaru_png/libtoaru_jpeg, so register the
# built-in decoders; this is the same patch the NetSurf build uses.
SPRITE_PATCH=$TOP/netsurf-toaru/patches/toaruos-static-sprites.patch
if [[ -f $SPRITE_PATCH ]] && ! grep -q 'add_lib("jpeg"' "$TOARU_SRC/lib/graphics.c"; then
    echo "== enabling static sprite decoders in lib/graphics.c"
    (cd "$TOARU_SRC" && patch -p1 --batch < "$SPRITE_PATCH")
fi

# -fplan9-extensions: the ToaruOS headers use anonymous struct embedding
# (e.g. struct MenuEntry_Normal { struct MenuEntry; ... }).
COMMON="-O2 -g -std=gnu11 -ffreestanding -nostdinc -fplan9-extensions
        -isystem $SYS/include -I$IDE_DIR -isystem $TOARU_SRC
        -fno-pie -fno-pic -fno-stack-protector -mno-red-zone -DNO_SSE
        -include $HERE/compat.h -Wno-error=implicit-function-declaration"

echo "== compiling the IDE"
"$CC" $COMMON -DIDE_TOARU -Wall -Wextra -Wno-unused-parameter \
    -Wno-builtin-macro-redefined \
    -c "$IDE_DIR/main.c" -o "$WORK/main.o"

# The Yutani client and the standard decorations depend on these library
# sources.  They are taken from the ToaruOS tree and compiled in so the image
# stays static (the decorations/menu code needs graphics, text, markup and the
# built-in PNG/JPEG sprite decoders).
TOARU_LIBS="yutani hashmap list pex kbd decorations menu graphics text
            icon_cache markup markup_text inflate png jpeg"
for src in $TOARU_LIBS; do
    echo "== compiling ToaruOS lib/$src.c"
    "$CC" $COMMON -w -c "$TOARU_SRC/lib/$src.c" -o "$WORK/$src.o"
done

echo "== linking"
# The library constructors (menu, icon cache) must run after libc's own
# initialisation, which is an init_array entry in libc's main.o.  Pull that
# object out and place it before the constructors, as the NetSurf build does.
"$AR" p "$SYS/libc.a" main.o > "$WORK/libc-main.o"
# shellcheck disable=SC2086
"$LD" -m elf_x86_64 -static -Ttext-segment=0x3F000000 \
    -o "$WORK/nuklear-ide" \
    "$SYS/crt0.o" "$SYS/crti.o" "$WORK/libc-main.o" \
    "$WORK/main.o" \
    $(for src in $TOARU_LIBS; do printf '%s ' "$WORK/$src.o"; done) \
    "$SYS/crtn.o" -L"$SYS" -lc

echo "== padding ELF segments"
mkdir -p "$(dirname "$OUT")"
python3 -I "$ELFPAD" "$WORK/nuklear-ide" "$OUT"
python3 -I "$ELFPAD" --check "$OUT"
chmod 755 "$OUT"
echo "done: $OUT ($(stat -c%s "$OUT") B)"
