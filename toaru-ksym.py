#!/usr/bin/env python3
"""toaru-ksym.py — ToaruOS kernel symbols from a binary that has no .symtab.

The released (ISO) kernel is stripped, but ToaruOS installs its own symbol
table: kernel/arch/x86_64/main.c::symbols_install()

    kernel_symbol_t * k = (kernel_symbol_t *)&kernel_symbols_start;
    while ((uintptr_t)k < (uintptr_t)&kernel_symbols_end) {
        ksym_bind(k->name, (void*)(k->addr + base));
        k = (kernel_symbol_t *)((uintptr_t)k + sizeof *k + strlen(k->name) + 1);
    }

An entry is therefore [8 bytes address][NUL-terminated name], without alignment, one
after another. This script finds the table in the binary (it tries a start at every string and
takes the longest continuous run) and can compute names from it:

    ./toaru-ksym.py _build/kx/kernel --find          # where the table is, how many entries
    ./toaru-ksym.py _build/kx/kernel --addr 0x204dd2 # name + offset after it
    ./toaru-ksym.py _build/kx/kernel --stack dump.txt

--stack reads the HMP "x/...xg $rsp" dump (lines "0x...: 0x... 0x... ...") and converts every
value that sits in the kernel text into "name+0x...". The return address on any line
can thus be turned into a backtrace, even though the kernel has no symbols in the ELF.
"""
import argparse
import re
import struct
import sys

NAME_OK = re.compile(rb"^[A-Za-z_.$][A-Za-z_0-9.$]*$")


def find_table(blob):
    """(offset, [(addr, name), ...]) of the longest run through the table."""
    best = (0, [])
    # a start can only be at 8-byte multiples where the following string looks like a symbol
    for m in re.finditer(rb"[A-Za-z_.$][A-Za-z_0-9.$]{2,63}\x00", blob):
        start = m.start() - 8
        if start < 0 or start <= best[0]:
            continue
        out, off, ok = [], start, True
        while ok and off + 9 < len(blob):
            addr = struct.unpack_from("<Q", blob, off)[0]
            end = blob.find(b"\x00", off + 8)
            if end < 0 or end - (off + 8) > 64:
                break
            name = blob[off + 8:end]
            if not NAME_OK.match(name):
                break
            out.append((addr, name.decode()))
            off = end + 1
            if len(out) > 20000:          # absurd length = we ran into data
                ok = False
        if len(out) > len(best[1]):
            best = (start, out)
    return best


def load(path):
    blob = open(path, "rb").read()
    off, table = find_table(blob)
    if len(table) < 100:
        sys.exit("%s: I did not find a symbol table (only %d entries) — is this even "
                 "a release kernel?" % (path, len(table)))
    table.sort()
    return blob, off, table


def lookup(table, addr):
    """(name, offset) of the nearest symbol up to addr, or (None, None)."""
    lo, hi = 0, len(table) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if table[mid][0] <= addr:
            lo = mid
        else:
            hi = mid - 1
    if lo < 0 or table[lo][0] > addr:
        return None, None
    return table[lo][1], addr - table[lo][0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kernel")
    ap.add_argument("--addr", action="append", default=[], metavar="HEX")
    ap.add_argument("--symbol", action="append", default=[], metavar="NAME",
                    help="find the symbol in the table and print its link address "
                         "(and optionally + base, i.e. the address where it currently is)")
    ap.add_argument("--stack", metavar="FILE")
    ap.add_argument("--find", action="store_true")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0,
                    help="kernel offset in memory against the addresses in the table. "
                         "The kernel is PIE and the loader places it elsewhere on every "
                         "boot, so it must be computed from what we have seen running - "
                         "most reliably from the GDT: base = (GDT from info registers) - "
                         "0x131220, where 0x131220 is the link address of the `gdt` "
                         "symbol. (Measured: it differs by 0x1e000 between boots.)")
    ap.add_argument("--text-max", type=int, default=0x400000,
                    help="upper bound of the kernel text for --stack (an address above it "
                         "is usually a value, not a return address)")
    a = ap.parse_args()

    blob, off, table = load(a.kernel)
    print("# %s: table at offset 0x%x, %d entries, range 0x%x..0x%x"
          % (a.kernel, off, len(table), table[0][0], table[-1][0]), file=sys.stderr)

    if a.find:
        for addr, name in table[:5]:
            print("  0x%016x  %s" % (addr, name))
        print("  ...")
        for addr, name in table[-3:]:
            print("  0x%016x  %s" % (addr, name))
        return

    def show(raw, note=""):
        v = int(raw, 0) - a.base
        name, d = lookup(table, v)
        print("  %-18s -> %-40s %s" % ("0x%x" % int(raw, 0),
                                       "%s+0x%x" % (name, d) if name else "not in text",
                                       note))

    for h in a.addr:
        show(h)

    for s in a.symbol:
        hits = [(ad, n) for ad, n in table if n == s]
        for ad, n in hits:
            print("  %-24s 0x%x (+ base = 0x%x)" % (n, ad, ad + a.base))
        if not hits:
            print("  symbol %s not found" % s)

    if a.stack:
        for h in re.findall(r"0x[0-9a-f]{4,16}", open(a.stack).read()):
            v = int(h, 16) - a.base
            if int(h, 16) > a.text_max:
                continue
            name, d = lookup(table, v)
            if name is not None and d < 0x4000:
                show(h)


if __name__ == "__main__":
    main()
