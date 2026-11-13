#!/bin/bash
# build.sh — builds miniCommander (and test programs) FOR TOARUOS with the cross
# compiler from ../mktcc.sh, i.e. without QEMU and without compiling inside the guest.
#
#   ./build.sh probe            ttyprobe (termios / terminal size)
#   ./build.sh mc               miniCommander  => _output/mc
#   ./build.sh clean            deletes _output
#
# Sources: _build/minicmd/mini-commander is an **unmodified** git clone of
# https://github.com/Tomas-M/mini-commander.  It is built from a copy in _output/src, to
# which patches/mc-toaru.patch is applied (generated from that same tree:
#   cd _output/src && git diff --no-index ... — see patches/README).
# The ToaruOS-specific layer is toaru/ (ncurses shim + compat + missing
# headers); it is not copied into the sources, it is only given precedence over the
# system headers via -I.
#
# Compiler: _build/tcc-toaru/bin/x86_64-toaru-tcc (../mktcc.sh), sysroot with
# libc.a, crt*.o and ToaruOS headers from the ISO.  Every image is verified with
# toaru-elfpad.py — the ToaruOS loader maps PT_LOAD page by page from p_vaddr,
# so an unaligned segment loses its last page (README §5i).
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
TCC=${TCC:-$TOP/_build/tcc-toaru/bin/x86_64-toaru-tcc}
SYSROOT=${SYSROOT:-$TOP/_build/tcc-toaru/lib}
SRC=${SRC:-$TOP/_build/minicmd/mini-commander}   # unmodified miniCommander sources
OUT=${OUT:-$TOP/_output}
WORK=$OUT/src
PATCH=$HERE/patches/mc-toaru.patch
ELFPAD=$TOP/toaru-elfpad.py

die() { echo "error: $*" >&2; exit 1; }
[[ -x $TCC ]] || die "missing cross compiler $TCC — run ../mktcc.sh"
[[ -d $SYSROOT/include ]] || die "missing sysroot $SYSROOT — run ../mktcc.sh"

CC="$TCC -B$SYSROOT -I$HERE/toaru"
mkdir -p "$OUT"

check() {  # image format per the ToaruOS loader requirements
    file "$1" | sed 's/,.*//' | sed "s|^|  image: |"
    python3 -I "$ELFPAD" --check "$1" || die "bad image format $1"
}

stage() {  # _output/src = clean sources + our patch, from scratch every time
    [[ -d $SRC ]] || die "missing miniCommander sources: $SRC (SRC=/path)"
    rm -rf "$WORK"; mkdir -p "$WORK"
    # Always from HEAD, not from the working tree: it may have changes already applied
    # (and git apply would then fail). The patch in patches/ is the only carrier of the port.
    if git -C "$SRC" rev-parse --git-dir > /dev/null 2>&1; then
        git -C "$SRC" archive HEAD | tar x -C "$WORK" || die "git archive $SRC"
    else
        (cd "$SRC" && tar cf - --exclude=.git .) | (cd "$WORK" && tar xf -) || die "copy of sources"
    fi
    if [[ -f $PATCH ]]; then
        # patch(1), not "git apply": _output/ is inside the project repository
        # and git apply silently skips files that .gitignore matches.
        (cd "$WORK" && patch -p1 -s < "$PATCH") || die "patch $PATCH failed to apply"
        echo "applied patch: $(basename "$PATCH")"
    else
        echo "warning: without patch $PATCH — original sources"
    fi
}

case "${1:-mc}" in
    clean) rm -rf "$OUT"; echo "deleted $OUT"; exit 0 ;;

    probe)
        $CC -o "$OUT/ttyprobe" "$HERE/probe/ttyprobe.c" || die "ttyprobe"
        check "$OUT/ttyprobe"
        echo "done: $OUT/ttyprobe ($(stat -c%s "$OUT/ttyprobe") B)"
        ;;

    stage) stage; echo "sources prepared in $WORK" ;;

    mc)
        stage
        # header order: toaru/ before the system sysroot — ncurses.h,
        # wctype.h, grp.h, sys/syscall.h, linux/fs.h are our own
        $CC -o "$OUT/mc" \
            "$WORK/mc.c" "$WORK/cmd.c" "$WORK/operations.c" "$WORK/dialog.c" \
            "$WORK/filelist.c" "$WORK/init.c" "$WORK/panel.c" "$WORK/ui.c" \
            "$WORK/view_edit.c" "$WORK/progress.c" \
            "$HERE/toaru/compat.c" "$HERE/toaru/curses.c" \
            -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 \
            || die "miniCommander"
        check "$OUT/mc"
        echo "done: $OUT/mc ($(stat -c%s "$OUT/mc") B)"
        ;;

    *) die "don't know what to build: $1 (probe | stage | mc | clean)" ;;
esac
