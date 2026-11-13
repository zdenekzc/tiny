# Reproduction: which programs compiled by tcc survive a ToaruOS boot

Each file is a standalone program that `x86_64-toaru-tcc` compiles and
`../../../guest-tcc-repro.sh` runs in the guest. Measurement is **always one
program per one boot** – after a kernel crash the guest is a write-off and there is
nothing left to measure (`arch_fatal()` stops all CPUs).

```console
$ OUT=_build/repro/r1 ./guest-tcc-repro.sh            # all programs here
$ OUT=_build/repro/r2 ./guest-tcc-repro.sh test       # and test.c from ../
```

| program | constructor | `main` reads `argc` | `.bss` past page boundary | result |
|---|---|---|---|---|
| `ctor_void.c`       | yes | no  | no  | OK – prints |
| `noctor_argc.c`     | no  | yes | no  | OK – prints |
| `ctor_argc.c`       | yes | yes | no  | OK – prints |
| `ctor_argc_quiet.c` | yes | yes | no  | OK – intentionally prints nothing |
| `bss_page.c`        | no  | no  | **yes** (12 kB `.bss`) | OK – `zeroed=ok` |
| `../test.c`         | yes | yes | **yes** | OK |
| `exit3.c`           | no  | no  | no  | OK – returns 3, see the note below |

The first four rows already held *before* the fix, which is what tempted one to
blame the constructor and `argc`. The last column decides: the kernel maps `PT_LOAD`
in steps of `0x1000` from `p_vaddr`; if `p_vaddr` is unaligned, the last page of the
segment is not mapped. But this only becomes apparent when `.bss` reaches past the
page boundary. Analyzed in
`../README.md`; the headers are checked by `toaru-elfpad.py --check` and `check_image`
in `mktcc.sh`, and fixed by a hunk in `tcc-toaru.patch`.

Measurement 2026-11-12: `test.bin` killed the boot 4 out of 4 times, `ctor_argc`
passed 6 out of 6. After the header fix, 10 boots in a row, 0 deaths, `test.bin`
prints all 6 lines.

`exit3.c` does not really belong in this table – it does not measure the fatal bug.
But it holds a measurement worth recording for when someone wonders about an empty
`RC`: `esh` does **not** have the exit code of an external program in `$?`. A program
that returned 3 announced itself in the guest as `==RC-exit3-…== 0` (measured
2026-11-12, `_build/repro/rc/exit3.log`); the value does reach the kernel (`pre_main()`
ends with `exit(main(argc, argv))`, `libc/main.c`), the shell loses it. That is why
`guest-tcc-repro.sh` prints `RC` only as "the shell program finished" and the
tcc ↔ gcc comparison in `guest-tcc.sh` happens **by the text of the output**, not by
exit codes.
