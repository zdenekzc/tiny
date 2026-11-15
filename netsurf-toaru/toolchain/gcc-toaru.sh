#!/bin/bash
# Compiler driver: host gcc -> static ToaruOS x86_64 objects and ELF.
#
# NetSurf and its support libraries are configured as a cross build with
# CC pointing at this script.  Compiles go to the host gcc with the ToaruOS
# sysroot; the final link is done by GNU ld with the ToaruOS startup files so
# the result is a static executable for ToaruOS 2.3.2.
#
# Environment:
#   TOARU_SYSROOT    ToaruOS libc sysroot (default: _build/tcc-toaru/lib)
#   NETSURF_PREFIX   cross prefix for NetSurf dependencies
#                    (default: _build/netsurf-prefix)
set -e

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)

SYS=${TOARU_SYSROOT:-$TOP/_build/tcc-toaru/lib}
PREFIX=${NETSURF_PREFIX:-$TOP/_build/netsurf-prefix}
GCCINC=$(gcc -print-file-name=include)

case "$1" in
  -dumpmachine) echo x86_64-pc-toaru; exit 0 ;;
  -dumpfullversion|-dumpversion) echo 10.3.0; exit 0 ;;
esac

have_c=0
for a in "$@"; do [ "$a" = "-c" ] && have_c=1; done

if [ "$have_c" = 1 ]; then
  exec gcc -O2 -g -ffreestanding -nostdinc \
    -isystem "$HERE/include" -isystem "$SYS/include" -isystem "$GCCINC" \
    -fplan9-extensions -include "$HERE/compat.h" -include errno.h \
    -DTOARUOS -fno-pie -fno-pic -fno-stack-protector -mno-red-zone "$@"
fi

# Final link: produce a static ToaruOS ELF.  Strip the host-only trace flag and
# the -o pair, then put the Toaru startup objects around the application objects.
out=a.out
args=()
i=1
while [ "$i" -le "$#" ]; do
  a=${!i}
  if [ "$a" = "-o" ]; then
    i=$((i+1))
    out=${!i}
  elif [ "$a" = "-Wl,--trace" ] || [ "${a#-Wl,}" != "$a" ] ||
       [ "${a#-O}" != "$a" ] || [ "$a" = "-g" ]; then
    :
  else
    args+=("$a")
  fi
  i=$((i+1))
done

ld -m elf_x86_64 -static -Ttext-segment=0x3F000000 -o "$out" \
  "$SYS/crt0.o" "$SYS/crti.o" "$PREFIX/lib/libc-main.o" "${args[@]}" \
  "$SYS/crtn.o" -L"$PREFIX/lib" -L"$SYS" -lc || exit $?

python3 -I "$TOP/toaru-elfpad.py" "$out" "$out" >/dev/null || exit $?
