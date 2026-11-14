#!/bin/bash
# gcc-toaru.sh — builds a program for ToaruOS 2.3.2 with the HOST gcc.
#
#   ./gcc-toaru.sh tests/gcc/hello.c              # -> ./hello
#   ./gcc-toaru.sh -o _tmp/hello a.c b.c          # several sources
#   SYSROOT=/path ./gcc-toaru.sh prog.c
#
# No cross compiler is needed: the x86_64 SysV ABI is the same as on Linux,
# so what differs is only the C library, the startup code and the ELF layout.
# The sysroot from mktcc.sh (_build/tcc-toaru/lib) supplies the ToaruOS
# headers, crt0.o/crti.o/crtn.o and libc.a; GNU ld puts the image at
# 0x3F000000 and toaru-elfpad.py aligns the PT_LOAD segments (the kernel maps
# a segment page by page from p_vaddr — see toaru-elfpad.py).
#
# Flags that matter:
#   -ffreestanding -nostdinc -I$SYS/include  ToaruOS headers, never glibc
#   -fno-pie -fno-pic                        the loader takes only ET_EXEC
#   -fno-stack-protector                     ToaruOS libc has no __stack_chk_fail
#   -mno-red-zone                            not required (the kernel subtracts
#                                            128 bytes before a signal frame),
#                                            but cheap insurance
#
# Do NOT link with ld.lld: its default layout gives the ELF headers their own
# PT_LOAD at 0x200000, and such a binary crashes in the guest (measured
# 2026-11-14: SIGSEGV or a hang).  GNU ld's -Ttext-segment=0x3F000000 produces
# the same layout tcc uses, which the ToaruOS loader handles.
#
# Extra compiler flags: CFLAGS='-g -O0' ./gcc-toaru.sh ...
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SYS=${SYSROOT:-$HERE/_build/tcc-toaru/lib}
ELFPAD=$HERE/toaru-elfpad.py
CC=${CC:-gcc}
LD=${LD:-ld}
CFLAGS=${CFLAGS:--O2 -Wall}

die() { echo "error: $*" >&2; exit 1; }

usage() {
    awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
}

OUT=
SRCS=()
while [[ $# -gt 0 ]]; do
    case $1 in
        -o)   [[ $# -ge 2 ]] || die "-o needs a file name"; OUT=$2; shift 2 ;;
        -o*)  OUT=${1#-o}; shift ;;
        -h|--help) usage; exit 0 ;;
        -*)   die "unknown option: $1 (C compiler flags go through CFLAGS)" ;;
        *)    SRCS+=("$1"); shift ;;
    esac
done
[[ ${#SRCS[@]} -gt 0 ]] || { usage >&2; exit 1; }
[[ -n $OUT ]] || OUT=${SRCS[0]%.c}
[[ $OUT != *.c ]] || die "output would be a C file: $OUT"

command -v "$CC" > /dev/null || die "missing $CC"
command -v "$LD" > /dev/null || die "missing $LD (GNU ld; do not use ld.lld)"
[[ -f $SYS/crt0.o && -f $SYS/libc.a && -d $SYS/include ]] \
    || die "missing sysroot $SYS — run ./mktcc.sh (it builds _build/tcc-toaru/lib)"
[[ -f $ELFPAD ]] || die "missing $ELFPAD"
command -v python3 > /dev/null || die "missing python3 (toaru-elfpad.py)"

for src in "${SRCS[@]}"; do
    [[ -f $src ]] || die "no such source: $src"
done

mkdir -p "$HERE/_tmp"
TMP=$(mktemp -d "$HERE/_tmp/gcc-toaru.XXXXXX") || die "mktemp"
trap 'rm -rf "$TMP"' EXIT

# The mandatory flags come after the user's, so they cannot be overridden by
# accident (a PIE or stack-protector build simply does not run on ToaruOS).
objs=()
n=0
for src in "${SRCS[@]}"; do
    n=$((n + 1))
    obj=$TMP/$n-$(basename "${src%.c}").o
    "$CC" $CFLAGS -ffreestanding -nostdinc -I"$SYS/include" \
        -fno-pie -fno-pic -fno-stack-protector -mno-red-zone \
        -c "$src" -o "$obj" || die "$CC failed on $src"
    objs+=("$obj")
done

"$LD" -m elf_x86_64 -static -Ttext-segment=0x3F000000 -o "$TMP/prog" \
    "$SYS/crt0.o" "$SYS/crti.o" "${objs[@]}" "$SYS/crtn.o" \
    -L"$SYS" -lc || die "$LD failed"

# ld leaves the last PT_LOAD a few bytes short of a page boundary; the kernel
# would not map its last page (toaru-elfpad.py pads the segment backwards).
python3 -I "$ELFPAD" "$TMP/prog" "$OUT" || die "toaru-elfpad.py could not align $OUT"
python3 -I "$ELFPAD" --check "$OUT" || die "bad image for the ToaruOS loader: $OUT"
chmod 755 "$OUT"

entry=$(python3 -I -c 'import struct,sys
b = open(sys.argv[1], "rb").read()
print("0x%x" % struct.unpack_from("<Q", b, 24)[0])' "$OUT") || entry="?"

echo "built $OUT ($(stat -c%s "$OUT") B), entry $entry"
