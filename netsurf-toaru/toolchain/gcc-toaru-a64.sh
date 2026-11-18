#!/bin/bash
# Compiler driver: the aarch64-unknown-toaru gcc -> static aarch64 ToaruOS ELF.
# (aarch64 counterpart of gcc-toaru.sh; TOARU_SYSROOT/NETSURF_PREFIX have the
# same meaning, but point at the aarch64 sysroot and prefix.)
#
# The aarch64 toolchain lives in the native build tree and needs its own
# lib directories on the loader path:
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

# The aarch64-unknown-toaru gcc from build-toaruos.sh -a native; its binutils
# need the toolchain's own shared libs on the loader path.
TC=${A64_TOOLCHAIN:-$TOP/_build/toaruos-native-aarch64/gcc_local}
export PATH="$TC/bin:$PATH"
export LD_LIBRARY_PATH="$TC/x86_64-pc-linux-gnu/aarch64-unknown-toaru/lib:$TC/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

SYS=${TOARU_SYSROOT:-$TOP/_build/tcc-toaru/lib}
PREFIX=${NETSURF_PREFIX:-$TOP/_build/netsurf-prefix}
GCCINC=$("$TC/bin/aarch64-unknown-toaru-gcc" -print-file-name=include)

case "$1" in
  -dumpmachine) echo aarch64-pc-toaru; exit 0 ;;
  -dumpfullversion|-dumpversion) echo 10.3.0; exit 0 ;;
  # The NetSurf buildsystem identifies the compiler with these two queries;
  # answer them from the host gcc so it sees a GCC toolchain.
  -dumpspecs|--version) exec "$TC/bin/aarch64-unknown-toaru-gcc" "$@" ;;
esac

# Anything that is not a final link: -c, preprocess-only (-E), assembly
# (-S) and dependency generation (-M/-MM).  autoconf's probes use -E.
have_c=0
for a in "$@"; do
  case "$a" in -c|-E|-S|-M|-MM) have_c=1 ;; esac
done

if [ "$have_c" = 1 ]; then
  exec "$TC/bin/aarch64-unknown-toaru-gcc" -O2 -g -ffreestanding -nostdinc \
    -isystem "$HERE/include" -isystem "$SYS/include" -isystem "$GCCINC" \
    -fplan9-extensions -include "$HERE/compat.h" -include errno.h \
    -include "$HERE/include/sys/select.h" -DTOARUOS -fno-pie -fno-pic -fno-stack-protector "$@"
fi

# Final link: produce a static ToaruOS ELF.  Unlike a normal compiler driver,
# this also has to cope with autoconf probes that compile and link in one step
# (cc -o conftest conftest.c): any source files on the command line are first
# compiled to temporary objects.  Compile-only options are kept for that step,
# link inputs and libraries are passed to ld in their original order.
CFLAGS_TOARU=(-O2 -g -ffreestanding -nostdinc
    -isystem "$HERE/include" -isystem "$SYS/include" -isystem "$GCCINC"
    -fplan9-extensions -include "$HERE/compat.h" -include errno.h
    -include "$HERE/include/sys/select.h" -DTOARUOS -fno-pie -fno-pic -fno-stack-protector)

orig=("$@")
out=a.out
compile_flags=()
sources=()
i=0
while [ "$i" -lt "${#orig[@]}" ]; do
  a=${orig[$i]}
  case "$a" in
    -o) i=$((i+1)); out=${orig[$i]} ;;
    -Wl,*) ;;
    -include|-isystem|-idirafter|-iquote) i=$((i+1)) ;;  # wrapper supplies these
    -I*|-D*|-U*|-f*|-m*|-W*|-O*|-g*|-std=*|-pthread)
        compile_flags+=("$a") ;;
    *.c|*.cc|*.cpp|*.s|*.S) sources+=("$a") ;;
  esac
  i=$((i+1))
done

tmpdir=
cleanup() { [ -z "$tmpdir" ] || rm -rf "$tmpdir"; }
trap cleanup EXIT

declare -A objmap=()
if [ ${#sources[@]} -gt 0 ]; then
  tmpdir=$(mktemp -d) || exit 1
  n=0
  for src in "${sources[@]}"; do
    n=$((n+1))
    obj=$tmpdir/cc$n.o
    "$TC/bin/aarch64-unknown-toaru-gcc" "${CFLAGS_TOARU[@]}" "${compile_flags[@]}" -c "$src" -o "$obj" || exit $?
    objmap[$src]=$obj
  done
fi

# Rebuild the command in order, replacing sources with their objects.
link_args=()
i=0
while [ "$i" -lt "${#orig[@]}" ]; do
  a=${orig[$i]}
  case "$a" in
    -o) i=$((i+1)) ;;
    -Wl,*) ;;                                  # host-only linker flags
    -static) ;;                                # ld gets -static anyway
    -include|-isystem|-idirafter|-iquote) i=$((i+1)) ;;
    -I*|-D*|-U*|-f*|-m*|-W*|-O*|-g*|-std=*|-pthread) ;;
    -shared|-dynamiclib|-rpath*) ;;            # static only
    *.c|*.cc|*.cpp|*.s|*.S) link_args+=("${objmap[$a]}") ;;
    *) link_args+=("$a") ;;                    # objects and other ld inputs
  esac
  i=$((i+1))
done

"$TC/bin/aarch64-unknown-toaru-ld" -m elf64toaru_aarch64 -static -Ttext-segment=0x400000 -o "$out" \
  "$SYS/crt0.o" "$SYS/crti.o" "$PREFIX/lib/libc-main.o" "${link_args[@]}" \
  "$SYS/crtn.o" -L"$PREFIX/lib" -L"$SYS" -lc -la64lite || exit $?

python3 -I "$TOP/scripts/toaru-elfpad.py" "$out" "$out" >/dev/null || exit $?
