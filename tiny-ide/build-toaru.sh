#!/bin/bash
# build-toaru.sh — builds Tiny IDE FOR TOARUOS with the cross compiler from
# ../scripts/mktcc.sh, i.e. without QEMU and without compiling inside the guest.
#
#   ./build-toaru.sh            tiny-ide for ToaruOS x86_64 => _build/output/tiny-ide
#   ./build-toaru.sh clean      deletes _build/output
#
# Sources: this directory (the Linux sources are used unchanged).  The
# ToaruOS-specific layer is:
#   - toaru/ (this directory): wctype.h + compat.c — UTF-8 multibyte helpers
#     missing from the guest libc; found before the sysroot via -I,
#   - ../mc-toaru/toaru/: the shared ncurses shim (curses.c, ncurses.h).
#
# Compiler: _build/tcc-toaru/bin/x86_64-toaru-tcc, sysroot with libc.a, crt*.o
# and ToaruOS headers from the ISO.  Every image is verified with
# toaru-elfpad.py — the ToaruOS loader maps PT_LOAD page by page from p_vaddr,
# so an unaligned segment loses its last page (README §5i).
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
TCC=${TCC:-$TOP/_build/tcc-toaru/bin/x86_64-toaru-tcc}
SYSROOT=${SYSROOT:-$TOP/_build/tcc-toaru/lib}
OUT=${OUT:-$TOP/_build/output}
SHIM=$TOP/mc-toaru/toaru
ELFPAD=$TOP/scripts/toaru-elfpad.py

die() { echo "error: $*" >&2; exit 1; }
[[ -x $TCC ]] || die "missing cross compiler $TCC — run ../scripts/mktcc.sh"
[[ -d $SYSROOT/include ]] || die "missing sysroot $SYSROOT — run ../scripts/mktcc.sh"
[[ -f $SHIM/curses.c ]] || die "missing the shared curses shim $SHIM"

CC="$TCC -B$SYSROOT -I$HERE/toaru -I$SHIM"

case "${1:-tiny-ide}" in
    clean) rm -rf "$OUT"; echo "deleted $OUT"; exit 0 ;;
esac

check() {  # image format per the ToaruOS loader requirements
    file "$1" | sed 's/,.*//' | sed "s|^|  image: |"
    python3 -I "$ELFPAD" --check "$1" || die "bad image format $1"
}

mkdir -p "$OUT"
$CC -o "$OUT/tiny-ide" \
    "$HERE/main.c" "$HERE/app.c" "$HERE/editor.c" "$HERE/runner.c" \
    "$HERE/ui.c" "$HERE/dialogs.c" \
    "$HERE/toaru/compat.c" "$SHIM/compat.c" "$SHIM/curses.c" \
    || die "tiny-ide"
check "$OUT/tiny-ide"
echo "done: $OUT/tiny-ide ($(stat -c%s "$OUT/tiny-ide") B)"
