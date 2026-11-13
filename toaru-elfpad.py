#!/usr/bin/env python3
"""toaru-elfpad.py — aligns PT_LOAD so that the ToaruOS loadsric does not skip them.

The ToaruOS 2.3.2 kernel (kernel/misc/elf64.c:339) maps a segment page by page like this:

    for (uintptr_t i = phdr.p_vaddr; i < phdr.p_vaddr + phdr.p_memsz; i += 0x1000)
            mmu_frame_allocate(mmu_get_page(i, MMU_GET_MAKE), MMU_FLAG_WRITABLE);

The 0x1000 step is counted FROM p_vaddr, not from the lower page boundary. So if
p_vaddr is NOT page-aligned, the last page of the segment is mapped only
when room is left for it in the counter: the number of iterations is ceil(p_memsz/0x1000),
while what is needed is floor((off + memsz - 1)/0x1000) + 1, where off = p_vaddr & 0xfff.
E.g. p_vaddr = 0x3f009df0, p_memsz = 0x394 (file 0x1b0, the rest .bss):
the loop only walks i = 0x3f009df0, the next i = 0x3f00adf0 is already past the end
0x3f00a184, page 0x3f00a000 stays unmapped and the zero-fill loop
zeroes .bss into an unmapped address => "Panic! Page fault in kernel ...
elf_exec+0x3ef" and arch_fatal() stops both CPUs.

Fix without changing the file contents: each PT_LOAD "leans" into the page space earlier,
i.e. it claims the free gap before itself too (it belongs to nobody, because tcc moves
the address by a whole page for a new segment, see tccelf.c:2534):

    m = p_vaddr & 0xfff;  p_vaddr -= m;  p_offset -= m;  p_filesz += m;  p_memsz += m

This makes p_vaddr aligned, the number of iterations = ceil((memsz + m)/0x1000) and that always
covers the real range. Writing (read_fs + zeroing) stays at the SAME
addresses as before, it only additionally writes into the unused gap before the segment.

 Usage:
    ./toaru-elfpad.py --check image.bin    # 0 = headers OK, 1 = the kernel would fail
    ./toaru-elfpad.py         image.bin    # align in place
    ./toaru-elfpad.py in.bin  out.bin      # align into another file

Return code: 0 ok, 1 cannot align safely (segment overlap / the headers
alone do not hold), 2 input error.
"""
import argparse
import struct
import sys

PT_LOAD = 1
PHDR_FMT = "<IIQQQQQQ"


class Elf:
    def __init__(self, blob, name):
        if blob[:4] != b"\x7fELF" or blob[4] != 2:
            sys.exit("%s: not ELF64" % name)
        (self.e_type, self.e_machine, self.e_version, self.e_entry, self.e_phoff,
         self.e_shoff, self.e_flags, self.e_ehsize, self.e_phentsize,
         self.e_phnum) = struct.unpack_from("<HHIQQQIHHH", blob, 16)
        if self.e_phentsize < struct.calcsize(PHDR_FMT):
            sys.exit("%s: unexpected program header size (%d)"
                     % (name, self.e_phentsize))
        self.blob = bytearray(blob)

    def phdr(self, i):
        return struct.unpack_from(PHDR_FMT, self.blob, self.e_phoff + i * self.e_phentsize)

    def set_phdr(self, i, v):
        struct.pack_into(PHDR_FMT, self.blob, self.e_phoff + i * self.e_phentsize, *v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--check", action="store_true",
                    help="only evaluate whether the headers satisfy the loadsric rule")
    a = ap.parse_args()

    e = Elf(open(a.image, "rb").read(), a.image)
    loads = [i for i in range(e.e_phnum) if e.phdr(i)[0] == PT_LOAD]
    if not loads:
        sys.exit("%s: no PT_LOAD" % a.image)

    # how many pages the kernel maps vs. how many are needed
    need = []
    for i in loads:
        _t, _fl, _off, va, _pa, _fsz, msz, _al = e.phdr(i)
        got = (msz + 0xFFF) // 0x1000                    # loop iterations
        want = ((va & 0xFFF) + msz + 0xFFF) // 0x1000    # pages the segment touches
        if got < want:
            need.append((i, va, msz, got, want))

    if a.check:
        for i, va, msz, got, want in need:
            print("  PT_LOAD %d: vaddr=0x%x memsz=0x%x — the kernel maps %d pages, "
                  "needs %d => last page missing" % (i, va, msz, got, want))
        if need:
            print("%s: UNSAFE for the ToaruOS loadsric (%d segments)" % (a.image, len(need)))
            return 1
        print("%s: headers OK (each PT_LOAD has as many pages as the kernel maps)" % a.image)
        return 0

    # fix: shift down to the page boundary, only when it does not overlap another segment
    spans = [(e.phdr(i)[3], e.phdr(i)[3] + e.phdr(i)[6]) for i in loads]
    changed = 0
    for i in loads:
        t, fl, off, va, pa, fsz, msz, al = e.phdr(i)
        m = va & 0xFFF
        if not m:
            continue
        lo, hi = va - m, va
        for j, (s, e2) in enumerate(spans):
            if j != i and s < hi and lo < e2:
                print("  PT_LOAD %d: gap 0x%x..0x%x overlaps PT_LOAD %d (0x%x..0x%x)"
                      " — not aligning" % (i, lo, hi, j, s, e2), file=sys.stderr)
                return 1
        e.set_phdr(i, (t, fl, off - m, va - m, pa - m, fsz + m, msz + m, al))
        changed += 1

    dst = a.out or a.image
    open(dst, "wb").write(bytes(e.blob))
    print("%s: aligned %d PT_LOAD -> %s" % (a.image, changed, dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
