# tcc/toaru — programs and a patch for TinyCC targeting ToaruOS

Comments in the code are in English.

## Files

| file | meaning |
|---|---|
| `tcc-toaru.patch` | modifications of the tcc tree for the ToaruOS target (applied by `mktcc.sh`) |
| `hello.c` | the smallest program that verifies the whole chain (printf via the ToaruOS libc) |
| `test.c` | constructor, argc/argv, `snprintf`, `%g`, `malloc` — on 2026-11-12 this was the program that killed the VM (see below) |
| `repro/` | small programs for measurement in the guest, run by `../../guest-tcc-repro.sh` |

Tools around it (from the repository root): `../../toaru-elfpad.py` (checking and
alignment of `PT_LOAD` according to the loadsric rule), `../../toaru-ksym.py` (kernel
symbols from a binary without `.symtab`), `../../guest-tcc-repro.sh` (measurement
and capture of the dead state).

`mktcc.sh` compiles both programs (`_build/tcc-toaru/{hello,test}`) and
`guest-tcc.sh` runs them in the guest and compares their output with gcc.

## Fixed bug: a program with a larger `.bss` kills the whole machine

**The cause is not in TinyCC nor in the order of constructors, but in the loadsric of ToaruOS 2.3.2.**
TinyCC places the `p_vaddr` of further `PT_LOAD`s in the middle of a page (so that the
file contains no padding); the loadsric counts pages stepping `0x1000` *from `p_vaddr`*
and stops mapping the last page of the segment. The kernel then zeroes `.bss` into an
unmapped address and dies with a `panic()` that stops all CPUs. Since 2026-11-12 the
patch `tcc-toaru.patch` solves this by aligning the headers, `check_image` in
`mktcc.sh` guards it.

### How it manifests and how to measure it

The program does not even print the first line, getty stops responding, QEMU keeps
running. The kernel output **is not on the serial line**: `kernel/misc/kprintf.c`
sends its output via `printf_output`, and as soon as `main.c:478` calls
`fbterm_initialize()`, everything travels to the framebuffer (only SeaBIOS remains on
`ttyS0`; `debug` on the kernel parameters would return it to COM1, but the kernel
parameters in the ISO cannot be changed by hand). Hence:

```console
$ ./toaru-vm.sh mon 'screendump /tmp/screen.ppm'
# then convert the PPM to PNG with any image tool, e.g. ImageMagick:
$ convert /tmp/screen.ppm /tmp/screen.png
```

```
[117.017508] Panic! Page fault in kernel pid=84 (/tmp/test.bin) at 0x3f00a000
[117.017509] Registers at interrupt: $rax=0x210 $rdx=0x3f009df0 int=0x0e err=0x02
[117.018218] 0x1ba2bf  elf_exec+0x3ef
[117.018283] 0x1b4877  exec+0x167   [117.018352] 0x1c1b01  sys_execve+0x301
```

`err=0x02` = a write to a page that is not present; `$rdx + $rax` is `p_vaddr` plus
the index in `.bss`, i.e. the first address beyond the last page the kernel mapped.
In the tables of the dead CPU0 (physically via `xp` from `CR3`) `PT[0..9]` are valid and
`PT[10] = 0`, `gva2gpa 0x3f00a000` = "Unmapped". The second CPU stands in `_isr2`
(`cli; hlt`) — that is the handling of the IPI `0x447D` from `arch_fatal_prepare()`,
i.e. a consequence, not the cause. The addresses are named by `toaru-ksym.py` (the
kernel from the ISO has no `.symtab`, but carries its own symbol table; it is PIE, so
in this boot `--base 0xdf000`).

### What breaks

```c
/* kernel/misc/elf64.c:339 — map PT_LOAD page by page */
for (uintptr_t i = phdr.p_vaddr; i < phdr.p_vaddr + phdr.p_memsz; i += 0x1000)
        mmu_frame_allocate(mmu_get_page(i, MMU_GET_MAKE), MMU_FLAG_WRITABLE);
...
for (size_t i = phdr.p_filesz; i < phdr.p_memsz; ++i)      /* elf_exec+0x3ef */
        *(char*)(phdr.p_vaddr + i) = 0;                     /* zeroes .bss  */
```

The loop runs `ceil(p_memsz / 0x1000)` times, while the number of pages needed is
`floor(((p_vaddr & 0xfff) + p_memsz - 1) / 0x1000) + 1`. The last `PT_LOAD` of the
file `test.bin` has `p_vaddr = 0x3f009df0`, `p_filesz = 0x1b0`, `p_memsz = 0x394`
(ending at `0x3f00a184`): the next `i` is `0x3f00adf0`, which is already *past* the
end, so the page `0x3f00a000` stays unmapped and zeroing `.bss` reaches into it. That
is why `hello` and `ctor_argc` (whose `.bss` fits in a single page) passed and `test.c`
(two `printf` lines more, i.e. a few bytes longer in `.rodata`/`.bss`) kept dying —
it had nothing to do with the constructor or with `argc`. The only thing that decides
is whether `(p_vaddr & 0xfff) + p_memsz` crosses the page boundary:

```console
$ ./toaru-elfpad.py --check obraz.bin        # 0 = OK, 1 = the kernel would crash
```

### The fix

The `PT_LOAD`s are moved down to the page boundary — only in the header, no section
moves, so not a single address in the image nor a single byte of the file changes:

```python
m = p_vaddr & 0xfff     # p_offset -= m; p_vaddr -= m; p_filesz += m; p_memsz += m
```

The bytes gained before the segment belong to nobody: tcc (nor `ld`) moves the new
segment up by a whole page (`tccelf.c`, branch `SHFX_NEWPH`), so the segments never
share a page, and `read_fs` still ends at the same file offset. This is exactly what
a hand-written assembler did as well, which worked on ToaruOS: a single `PT_LOAD` with
`p_vaddr = 0x3F000000` and `p_offset = 0`.

The same procedure is incorporated in `tcc-toaru.patch` (`layout_sections`), so the
compiler does it itself; `toaru-elfpad.py` remains for finished images and as `--check`.

### Remaining (a second, harmless curiosity)

The program's constructor runs *before* `_libc_init()` (both are in `.ctors` and
`pre_main()` runs them in the order the linker sorted them), so `printf` called
inside the constructor is lost — the stdio buffers do not exist yet. The program
then continues normally and exits. `repro/ctor_argc_quiet.c` keeps this variant as
a measured "output 0 lines".

### History of the measurements that led to a mistake

The original text (2026-11-12, the same day) blamed the combination constructor +
`main(int argc)` for the crash and relied on the fact that `ctor` without `argc` and
`argc` without a ctor both pass. That was a consequence, not the cause: both programs
were a few bytes smaller than `test.c`, so `.bss` fit in the page. After aligning the
headers `ctor_argc.bin` and `test.bin` both run — 10 boots in a row, 10 programs, no
hesitation:

| run | before the fix | after the fix |
|---|---|---|
| `test.bin` | **fatal** 4/4 boots | OK, 6 lines, RC 0 |
| `ctor_argc` | OK (6/6) — so it was not that boundary | OK |
| `bss_page` (12 kB `.bss`) | — | OK, `zeroed=ok` |

Measured by `guest-tcc-repro.sh` (one program per boot, `alive[]` verifies the shell;
on death it captures registers, disassembly, `screendump` and the load of the QEMU
threads).

During the search the following was reasonably ruled out and still holds (useful for
next time):

* **file transfer** — `fetch` delivers an image of the exact size; the behaviour is
  the same from an ext2 image and from `fetch`. (Note, `cp /mnt/share/* /tmp/` in
  ToaruOS itself fails with "cp: failed to copy ...: Bad address" and leaves the files
  missing — that is why `guest-*.sh` bring images over `fetch`, not over the shared disk.)
* **data below `e_entry`** — the `PTR_INRANGE` rule (`kernel/syscall.h`) holds:
  `open()`/`read()` with a pointer below `e_entry` returns `-EFAULT`. This is guarded
  by `check_image` in `mktcc.sh`; for all our images the rule is satisfied.
* **boot flakiness** — it does exist (ToaruOS sometimes fails to come up, which is why
  `guest-tcc-repro.sh` repeats the boot `BOOTS` times), but it differs measurably:
  no shell is heard, whereas with this bug the shell responds even afterwards, only it
  prints nothing itself — and after a while it stops responding at all, because the
  kernel has crashed in the meantime.
