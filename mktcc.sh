#!/bin/bash
# mktcc.sh — TinyCC FOR TOARUOS, built ON THE HOST (cross compilation):
#   _build/tcc-toaru/bin/x86_64-toaru-tcc   compiler running on Linux
#   _build/tcc-toaru/lib/                   {-B path}: libtcc1.a, crt0.o
#                                           crti.o crtn.o libc.a, include/
#   _build/tcc-toaru/hello                  verification program (ELF for ToaruOS)
#   _build/tcc-toaru/test                   tcc/toaru/test.c: ctor, argv, %g, malloc
#   _build/tcc-toaru/tcc-guest              tcc itself as an ELF for running in ToaruOS
#
#   ./mktcc.sh              patches the clone, compiles, prepares the sysroot, verifies both images
#   ./mktcc.sh -p           before that deletes _build/download/tinycc and does a git clone
#
# Why a custom build instead of configure/make:
#   * there is no make nor configure in the guest (that also applies to building tcc inside
#     ToaruOS, guest-tcc.sh), so a single "gcc tcc.c" is enough for both directions
#   * with ONE_SOURCE (the default in tcc.c) tcc is compiled as a single translation unit
#   * we write config.h ourselves (tcc/config-toaru.h), CONFIG_TCC_PREDEFS=0 =>
#     no need to generate tccdefs_.h using c2str.exe
#
# What tcc/toaru/tcc-toaru.patch changes (without it the images produced are ones
# the ToaruOS kernel will not run or that crash in the guest):
#   x86_64-link.c  ELF_START_ADDR 0x400000 -> 0x3F000000 (the address of our own link.ld;
#                  at 0x400000 the program SIGSEGVs before the first syscall, README §3b)
#   tccelf.c       startup object crt0.o instead of the Linux crt1.o
#   tccelf.c       ".text" sits BEFORE ".data.ro" — the kernel only allows syscalls with a
#                  pointer to addresses greater than e_entry (PTR_INRANGE, kernel/syscall.h),
#                  so string literals must not lie below the program entry
#   libtcc.c       the default is static linking (the kernel would hand PT_DYNAMIC to ld.so)
#   tcc.h          -run only for the compiler built inside ToaruOS
#   tccpp.c        predefined macros: __toaru__/__toaru/toaru, no __linux__ and no
#                  __unix__ (note, "toaru" then cannot be used as an identifier)
#   tccdefs.h      __GNUC__ 10.3 like the local gcc, int64_t = long, __ELF__
#   tccelf.c       PT_LOAD: every segment starts on a page boundary. The loadsric of
#                  ToaruOS 2.3.2 maps page by page so that it counts the 0x1000 step
#                  FROM p_vaddr (kernel/misc/elf64.c:339), thus an unaligned p_vaddr
#                  costs the segment its last page; the kernel then zeroes .bss into
#                  an unmapped address and crashes with "Page fault in kernel" in
#                  elf_exec+0x3ef, which arch_fatal() turns into taking down the whole VM.
#                  Only the header changes (p_vaddr down, p_offset down, filesz/memsz up),
#                  no section moves. check_image below guards this.
#   tccelf.c       Constructors: ToaruOS libc keeps them in ".ctors" but
#                  pre_main() runs __init_array_start.._end (libc/main.c), so
#                  tccelf.c brackets ".ctors" with those symbols and tccgen.c
#                  puts tcc's own ctors there too.  Without it _libc_init() is
#                  never called: the program links, runs, exits 0 and prints
#                  nothing, because stdio buffers were never initialised.
#                  Those two sections must stay SHT_PROGBITS, otherwise tcc
#                  refuses to merge the ".ctors" of libc.a into them.
#
# config-toaru.h: CONFIG_TCC_PIE / CONFIG_TCC_PIC must not be defined at all.
#   tcc tests them with #ifdef, so "#define CONFIG_TCC_PIE 0" switches PIE ON
#   and the result is ET_DYN at address 0, which the kernel refuses (README §5g).
set -euo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=$DIR/_build
SRC=$B/download/tinycc
GEN=$B/gen
OUT=$B/tcc-toaru
TOARULIB=$B/toaru-lib            # crt0.o crti.o crtn.o libc.a (from the ISO, mklibc.sh)
TOARUINC=$B/toaru-include        # /usr/include from the ISO (mklibc.sh)
TCCINCL=$SRC/include             # stdarg.h, stddef.h, float.h, tccdefs.h ...

die() { echo "error: $*" >&2; exit 1; }
OK=0; NOK=0
ok()  { echo "  OK   $*"; OK=$((OK+1)); }
bad() { echo "  ***  $*"; NOK=$((NOK+1)); }

if [[ ${1:-} == -p ]]; then
    echo "refreshing the clone from repo.or.cz ..."
    rm -rf "$SRC"
fi

if [[ ! -d $SRC ]]; then
    echo "downloading TinyCC (repo.or.cz/tinycc) ..."
    mkdir -p "$B/download"
    git clone --depth 50 https://repo.or.cz/tinycc.git "$SRC" > /dev/null || die "git clone"
fi
git -C "$SRC" rev-parse -q --verify HEAD > /dev/null || die "$SRC is not a git clone"

# ---------------------------------------------------------------- 1. patch
PATCH=$DIR/tcc/toaru/tcc-toaru.patch
[[ -f $PATCH ]] || die "missing target patch: $PATCH"
if git -C "$SRC" diff --quiet -- tcc.h tccpp.c tccelf.c tccgen.c libtcc.c x86_64-link.c include/tccdefs.h; then
    echo "applying $PATCH"
    git -C "$SRC" apply --whitespace=nowarn "$PATCH" || die "git apply $PATCH"
else
    echo "the patch is already applied (or the sources are hand-edited), leaving as is"
fi

# ------------------------------------------------------- 2. target compiler
mkdir -p "$GEN" "$OUT/bin"
cp "$DIR/tcc/config-toaru.h" "$GEN/config.h"
echo "building x86_64-toaru-tcc ..."
gcc -O1 -w -DTOARU_HOST_LINUX -I"$GEN" -I"$SRC" "$SRC/tcc.c" \
    -o "$OUT/bin/x86_64-toaru-tcc" || die "tcc build"
chmod 755 "$OUT/bin/x86_64-toaru-tcc"
"$OUT/bin/x86_64-toaru-tcc" -v 2>&1 | tail -1

# ------------------------------------------------------------- 3. sysroot
if [[ ! -f $TOARULIB/libc.a || ! -f $TOARUINC/stdio.h ]]; then
    echo "extracting libc and headers of ToaruOS from the ISO ..."
    "$DIR/mklibc.sh" > /dev/null || die "mklibc.sh"
fi
LIB=$OUT/lib
rm -rf "$LIB"; mkdir -p "$LIB/include"
cp "$TOARULIB"/crt0.o "$TOARULIB"/crti.o "$TOARULIB"/crtn.o "$TOARULIB"/libc.a "$LIB/"
# headers: first tcc's (stdarg.h, stddef.h, float.h, tccdefs.h), then ToaruOS's
cp "$TCCINCL"/*.h "$LIB/include/"
cp -a "$TOARUINC"/. "$LIB/include/"
echo "sysroot: $LIB ($(ls "$LIB" | tr '\n' ' ') + $(find "$LIB/include" -name '*.h' | wc -l) headers)"

# --------------------------------------------------------- 4. libtcc1.a for Toaru
# tcc compiles this itself (note: lib/Makefile uses the same procedure),
# the objects are the same as for the x86_64 target without backtrace and bound checks
TCC=$OUT/bin/x86_64-toaru-tcc
O=$OUT/obj-libtcc1
rm -rf "$O"; mkdir -p "$O"
for f in libtcc1.c stdatomic.c builtin.c va_list.c dsohandle.c; do
    "$TCC" -B"$LIB" -c "$SRC/lib/$f" -o "$O/${f%.c}.o" || die "tcc -c lib/$f"
done
for f in atomic.S alloca.S alloca-bt.S; do
    "$TCC" -B"$LIB" -c "$SRC/lib/$f" -o "$O/${f%.S}.o" || die "tcc -c lib/$f"
done
"$TCC" -B"$LIB" -ar rcs "$LIB/libtcc1.a" "$O"/*.o || die "ar libtcc1.a"
echo "libtcc1.a: $(stat -c%s "$LIB/libtcc1.a") B"

# ------------------------------------------------------- 5. test program
# tcc/toaru/hello.c is the smallest program that proves the whole chain works,
# tcc/toaru/test.c needs a bit more of libc and checks that the constructor
# array reached pre_main(); see the header of each file for its expected output.
# guest-tcc.sh compiles the very same sources with host gcc and diffs the
# result against the guest, so the sources stay real files in tcc/toaru/.
for p in hello test; do
    cp "$DIR/tcc/toaru/$p.c" "$OUT/$p.c"
done
for p in hello test; do
    "$TCC" -B"$LIB" -o "$OUT/$p" "$OUT/$p.c" || die "compile and link $p.c"
done

# ------------------------------------------- 5b. tcc that runs INSIDE ToaruOS
# The cross compiler is not portable: it needs Linux.  That is why tcc.c is
# compiled once more, this time as an image for ToaruOS.  The result is shipped in
# /share/tcc (run-toaru.sh) and is run in the guest:
#
#   /share/tcc/bin/tcc -o /share/hello /share/hello.c
#
# -DCONFIG_TCCDIR="/share/tcc" is the default path, so inside the guest there is no
# need for -B; files are looked up in /share/tcc (include/, lib/).  The cross sysroot
# has the libraries directly in the -B directory, but for the guest we use the same
# layout as a normal sysroot (/share/tcc/lib), which is why CRT and LIBPATHS are overridden.
# -DTOARU_TCC_NO_NATIVE disables -run: the ToaruOS libc has no mmap/mprotect, so
# tcc would have no way to map code (see tcc.h).  guest-compat.c supplies strtold
# and ldexpl, which are missing from the ToaruOS libc.
"$TCC" -B"$LIB" -DTOARU_TCC_NO_NATIVE \
    -DCONFIG_TCCDIR='"/share/tcc"' \
    -DCONFIG_TCC_CRTPREFIX='"{B}/lib"' \
    -DCONFIG_TCC_LIBPATHS='"{B}/lib"' \
    -I"$GEN" -I"$SRC" -o "$OUT/tcc-guest" \
    "$SRC/tcc.c" "$DIR/tcc/toaru/guest-compat.c" \
    || die "compile tcc for ToaruOS (tcc-guest)"
chmod 755 "$OUT/tcc-guest"
echo "tcc-guest: $(stat -c%s "$OUT/tcc-guest") B"

# Requirements an image has to meet before the ToaruOS kernel even tries to run
# it (measured with our own probe images, README §3b and §5g).
check_image() {
    local f=$1 ehdr phdr type entry base under w addr flg
    echo "image: $f"
    file "$f" 2> /dev/null | sed 's/^/  /'
    ehdr=$(readelf -hW "$f" 2> /dev/null)
    phdr=$(readelf -lW "$f" 2> /dev/null)
    # readelf prints "EXEC (Executable file)" / "DYN (Shared object file)", so
    # match the keyword, not the string "ET_EXEC"
    type=$(sed -n 's/^ *Type:  *\([A-Z]*\).*/\1/p' <<<"$ehdr")
    [[ $type == EXEC ]] && ok "ET_EXEC (the kernel rejects ET_DYN)" \
                       || bad "$f: ELF type is '$type', the kernel only runs ET_EXEC"
    if grep -qE '^[[:space:]]+(INTERP|DYNAMIC)[[:space:]]' <<<"$phdr"; then
        bad "$f: PT_INTERP/PT_DYNAMIC -> the kernel would start ld.so instead"
    else
        ok "no PT_INTERP, no PT_DYNAMIC"
    fi
    entry=$(sed -n 's/^ *Entry point address: *//p' <<<"$ehdr")
    # in `readelf -lW` column 2 is the file offset, column 3 the virtual address;
    # readelf pads the two addresses differently, so normalise before comparing
    base=$(awk '$1=="LOAD"{print $3; exit}' <<<"$phdr")
    entry=$(printf '0x%x' $((16#${entry#0x})))
    base=$(printf '0x%x' $((16#${base#0x})))
    [[ $entry == 0x3f000* ]] && ok "entry $entry is in an image based at 0x3F000000" \
                            || bad "$f: entry $entry is outside 0x3F000000"
    [[ $base == 0x3f000* ]] && ok "first PT_LOAD at $base" \
                           || bad "$f: first PT_LOAD at $base"
    # Data - and with it the string literals handed to open() - has to sit ABOVE
    # the entry point: the kernel only accepts pointer arguments greater than
    # e_entry (PTR_INRANGE in kernel/syscall.h), everything below it comes back
    # as -EFAULT.  Executable sections below e_entry (.init, .fini,
    # .text.startup) are fine, they are never passed to a syscall, so only
    # allocated non-executable sections are checked.
    # Watch out for portability: strtonum() was a gawk extension; on machines
    # where awk is aliased to mawk (Debian) it ends with "function strtonum
    # never defined" and with pipefail that brings the whole script down - the
    # check then does not run at all and contributes to the script not even
    # finishing its summary. Bash reads hex by itself, hence no awk at all here.
    #
    # readelf -SW: [Nr] Name Type Address Off Size ES Flg Lk Inf Al
    # "  [ 0]" and "  [13]" merge into a single field; the header line and the NULL
    # section (without a name) shift the columns, so I only take lines starting with a dot.
    under=""
    while read -r -a w
    do
        [[ ${w[0]:-} == .* ]] || continue
        addr=${w[2]:-} flg=${w[6]:-}
        [[ $addr =~ ^[0-9a-f]+$ ]] || continue      # the listing header is not an address
        [[ $flg == *A* && $flg != *X* ]] || continue
        (( 16#$addr == 0 )) && continue             # we do not check the section at address 0
        if (( 16#$addr < 16#${entry#0x} ))
        then
            under+="${w[0]}($addr) "
        fi
    done < <(readelf -SW "$f" 2> /dev/null | sed -e 's/^ *\[[^]]*\] *//')
    [[ -z $under ]] && ok "no allocated data section below the entry point" \
                   || bad "$f: sections below e_entry (open()/read() would give -EFAULT): $(echo "$under" | tr '\n' ' ')"
    # ToaruOS maps one page per iteration of every PT_LOAD and counts the 0x1000
    # step FROM p_vaddr (kernel/misc/elf64.c:339). If p_vaddr is not aligned, the
    # loop runs ceil(memsz/0x1000) times and skips the last page of the segment;
    # right afterwards the kernel zeroes .bss into an unmapped address and crashes
    # with "Page fault in kernel" — arch_fatal() stops all CPUs, i.e. the whole VM.
    # The patch solves this by aligning the headers; this check merely makes it
    # possible to detect on the host instead of only by booting (and following the
    # readelf column order: Type Offset VirtAddr PhysAddr FileSiz MemSiz Flg Align).
    short=""
    while read -r t off va pa fsz msz flg al
    do
        [[ $t == LOAD ]] || continue
        v=$((16#${va#0x})); m=$((16#${msz#0x}))
        got=$(( (m + 0xFFF) / 0x1000 ))                       # how many pages the kernel maps
        want=$(( ((v & 0xFFF) + m + 0xFFF) / 0x1000 ))        # how many the segment needs
        (( got < want )) && short+="vaddr=$va memsz=$msz: maps $got<need $want  "
    done <<<"$phdr"
    [[ -z $short ]] && ok "every PT_LOAD covers all pages the kernel maps" \
                   || bad "$f: PT_LOAD pages the kernel would not map: $short"
}
for p in hello test tcc-guest; do check_image "$OUT/$p"; done

echo
echo "done: $OK OK, $NOK problems"
echo "in the guest:  fetch http://10.0.2.2:8010/hello -o /tmp/hello; chmod 755 /tmp/hello; /tmp/hello"
echo "comparison with gcc in the guest:  ./toaru-vm.sh start ; PORT=8010 ./guest-tcc.sh"
echo "kernel crash in elf_exec (fixed, reason in): tcc/toaru/README.md"
[[ $NOK == 0 ]]
