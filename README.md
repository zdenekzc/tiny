# tiny — ToaruOS + TinyCC + mini-commander

One command downloads, builds and runs **ToaruOS 2.3.2** in QEMU with a working
**TinyCC** (a compiler running *inside* the guest) and **mini-commander** (`mc`),
a file manager with viewer and editor built on top of a custom ncurses shim
for ToaruOS.

```bash
./tests/run-toaru.sh
```

The first run downloads the ISO, the ToaruOS sources and TinyCC and builds
everything (a few minutes); later runs only fill in what is missing. A QEMU
window with ToaruOS then opens — just open Terminal inside and mount the shared
disk (see "Inside ToaruOS" below). You are logged in automatically
(`root@livecd`).

Nothing under `_build/` or `_output/` is irreplaceable: both can be deleted at
any time and `run-toaru.sh` will download and build everything again.

---

## What the script does

`tests/run-toaru.sh` performs these steps (only when needed — anything already
built in `_build/` or `_output/` is not downloaded or rebuilt):

1. downloads `toaruos-2.3.2.iso` into `_build/download/`,
2. clones the ToaruOS sources (tag `v2.3.2`) into `_build/download/toaruos-src`
   (copied to the guest only so they can be browsed in `mc`),
3. runs `mktcc.sh`, which builds two things:
   - **the host cross compiler**
     `_build/tcc-toaru/bin/x86_64-toaru-tcc` + sysroot `_build/tcc-toaru/lib`
     (`libc.a`, `crt0.o`, `crti.o`, `crtn.o`, `libtcc1.a`, `include/`),
   - **tcc that runs inside ToaruOS**
     `_build/tcc-toaru/tcc-guest` (ToaruOS ELF, `CONFIG_TCCDIR=/share/tcc`),
   - verification programs `_build/tcc-toaru/hello` and `.../test`,
4. clones mini-commander (`_build/minicmd/mini-commander`) and builds it with
   the cross compiler → `_output/mc` (`mc-toaru/build.sh mc`),
5. downloads `nuklear.h` (for now only to be browsed in the guest; the
   terminal backend is not part of the image),
6. builds `_build/push/` and packs it into the ext2 image
   **`_build/share.img`** (`mkshare.sh`): `/mc`, `/tcc_test`, `/tcc_test.c`,
   `/tcc/{bin/tcc,include,lib}`, the sources from `tests/examples/*.c`,
   `/toaruos-src`, `/nuklear.h`, `/compile_examples.sh`, `/README.txt`,
7. boots a **headless verification VM** and checks that `/share/tcc_test`
   runs, that tcc inside ToaruOS compiles and runs `/share/tcc_hello.c` and
   every example, and that mini-commander draws itself and can be quit,
8. starts the **visible ToaruOS** in a QEMU window and leaves it running.

The verification VM (step 7) can be skipped with `DEMO=0`, the QEMU window with
`VISIBLE=0`.

## Inside ToaruOS

The shared disk is not mounted automatically. In Terminal (or on the serial
line):

```sh
mkdir -p /share
insmod /mod/ext2.ko
mount ext2 /dev/hda,rw /share
```

TinyCC:

```sh
/share/tcc/bin/tcc -o /tmp/hello /share/tcc_hello.c ; /tmp/hello
/share/compile_examples.sh        # compiles and runs every *.c in /share
```

`tcc` has the sysroot built in (`/share/tcc`), so no `-B` or `-I` is needed.
Write outputs to `/tmp` — writes into the ext2 image are not reliably flushed
from the host (ata.ko does not use write-back caching).

mini-commander:

```sh
/share/mc /share
```

Quit with **F10** (same as Midnight Commander); the usual mouse and keyboard
bindings apply (`Tab` switches panels, `F3` viewer, `F4` editor, …).

## Controlling the VM

```bash
./toaru-vm.sh start            # starts the VM in the background (log _build/work/out.log)
./toaru-vm.sh send 'ls /'      # sends a command to the serial line (getty on ttyS1)
./toaru-vm.sh log 200          # last 200 log lines without ANSI sequences
./toaru-vm.sh status           # running? log size
./toaru-vm.sh mon 'info reg'   # QEMU monitor command (QMP socket)
./toaru-vm.sh stop             # stops QEMU
```

## Optional environment variables

| Variable | Meaning |
|---|---|
| `VISIBLE=0` | no QEMU window (serial line only); the automated check still runs |
| `HEADLESS=0` | graphical boot even for a windowless VM (getty on ttyS1 stays) |
| `DEMO=0` | skip the verification VM (step 7) |
| `MB=128` | size of `share.img` in MiB (default 64; enough even with the ToaruOS sources) |
| `ACCEL=tcg` | no KVM (slower, but `-d int` logs exceptions under TCG) |
| `QEMU_EXTRA=...` | extra QEMU arguments (e.g. `-s` for the gdb stub) |

The same variables can be passed to `toaru-vm.sh start`.

## Directory layout

```
tiny/
├─ tests/run-toaru.sh      # one-click: download, build, run
├─ tests/examples/*.c      # sample programs (copied to /share)
├─ user/                   # YOUR files — copied into the root of /share
├─ mc-toaru/
│   ├─ build.sh            # mc | probe | stage | clean (cross tcc)
│   ├─ patches/mc-toaru.patch   # the single carrier of the mini-commander port
│   └─ toaru/              # ncurses shim, compat layer, missing headers
├─ mklibc.sh               # libc.a, crt*.o and headers from the ISO (used by mktcc.sh)
├─ mktcc.sh                # TinyCC: cross compiler and in-guest compiler
├─ mkshare.sh              # ext2 image from _build/push
├─ toaru-vm.sh             # start/stop/send/log/status/mon
├─ toaru-elfpad.py         # ELF format check for the ToaruOS loader
├─ toaruos-ext2-sparse-bug.md  # ext2 sparse-file bug report (upstream is archived)
├─ _build/                 # downloaded material and artifacts (safe to delete)
│   ├─ download/           # ISO, ToaruOS sources, tinycc, nuklear.h
│   ├─ tcc-toaru/          # cross tcc, sysroot, tcc-guest, test programs
│   ├─ push/               # shared-disk contents before packing
│   ├─ share.img           # finished ext2 disk (mounted as /dev/hda in the guest)
│   └─ work/               # FIFO, logs and pid files of the running VM
├─ _output/                # build results worth keeping
│   ├─ mc                  # mini-commander for ToaruOS
│   ├─ ttyprobe            # termios / terminal-size diagnostics
│   └─ src/                # pristine mc sources + applied patch (regenerated)
├─ _tmp/                   # scratch binaries (safe to empty)
└─ README.md
```

`_build/` and `_output/` can be deleted at any time; `user/` **cannot** — its
contents are copied into the root of the shared disk on every run (and
override built-in files of the same name). Directories starting with an
underscore are treated as disposable, which is why user data belongs in
`user/`.

## Requirements

`bash`, `curl`, `git`, `qemu-system-x86_64`, `python3`, `7z` (p7zip; unpacks
the ISO when building the sysroot), host `gcc` (for the cross tcc) and
`e2fsprogs` (`mke2fs`, `debugfs`). No `sudo` is needed — `share.img` is a plain
file and the guest mounts it as an IDE disk.

Fedora: `dnf install qemu-system-x86 p7zip gcc e2fsprogs python3 curl git`
Debian/Ubuntu: `apt install qemu-system-x86 p7zip-full gcc e2fsprogs python3 curl git`

## Cleaning and rebuilding

| Command | Effect |
|---|---|
| `rm -rf _build` | deletes downloads and images; the next run fetches and builds everything again |
| `rm -rf _output` | deletes `mc`, `ttyprobe` and the staging tree; `run-toaru.sh` rebuilds them |
| `./mc-toaru/build.sh mc` | rebuilds only mini-commander (after a change in `toaru/` or the patch) |
| `./mc-toaru/build.sh clean` | deletes `_output` |
| `rm -rf _tmp/*` | empties the scratch directory |

After changing `mc-toaru/toaru/*.c` you must run `./mc-toaru/build.sh mc` —
`run-toaru.sh` only builds `mc` when the binary is missing.

## Port notes (what had to be solved)

- **ELF for ToaruOS**: base `0x3F000000`, `ET_EXEC`, every `PT_LOAD` starting on
  a page boundary. The loader maps segments page by page from `p_vaddr`
  (`kernel/misc/elf64.c`), so an unaligned segment loses its last page and the
  kernel crashes. Checked by `toaru-elfpad.py`.
- **Heap**: starts at the aligned end of the last `PT_LOAD`; programs must not
  assume `brk` starts at zero.
- **ncurses shim** (`mc-toaru/toaru/curses.c`) implements only the subset of
  ncurses that mc uses and draws directly with ANSI sequences. Beware of SGR:
  every sequence must end with `m` — without it the terminal takes the next
  character as the sequence terminator and swallows it (this is exactly what
  made the first letter of every button label disappear, `F2 Sort` → `ort`).
- **ext2 and sparse files** — the trickiest thing in the project: `mke2fs -d`
  writes files with holes (zero blocks are not allocated), but `modules/ext2.c`
  in ToaruOS 2.3.2 has two bugs:
  `inode_read_block()` compares the block index against `i_blocks` (the number
  of *allocated* sectors), so blocks past the allocated region read as zeros
  even when they are physically allocated and contain data; and for a hole
  `read_block(0)` returns `E_BADBLOCK` without touching the buffer — while
  `read_ext2()` reuses one buffer, so a hole returns **the contents of the
  previous block**. Result: `mc` was loaded corrupted (`active_panel` in
  `.data` became NULL) and crashed with SIGSEGV in `printf("%s")`.
  `mkshare.sh` therefore runs `debugfs fallocate` after `mke2fs`, physically
  allocating every block of every file, and verifies the result at the end.
  Without `debugfs` the script refuses to continue. Details, a verified
  reproduction and a proposed patch are in `toaruos-ext2-sparse-bug.md`.
- **Known ToaruOS 2.3.2 libc bugs** (found while porting): `fflush(NULL)` crashes with SIGSEGV,
  `printf("%-7s")` truncates, `dprintf` is missing, `.fini_array` is not
  called, `%f` has no default precision, `sscanf("%hd")` is broken,
  `dlsym(RTLD_DEFAULT, …)` crashes. The shim and mc avoid all of these.
- **Examples**: all `tests/examples/*.c` files are plain C (C99/C11) and
  TinyCC compiles and runs them. `queens.c`, `knight.c` and `towers.c` are the
  chess/toy examples; the rest are data structures and sorting.
  `/share/compile_examples.sh` compiles every `*.c` in `/share`.

## License

tiny is licensed under the **GNU General Public License v3.0** — see `LICENSE`.
Third-party components keep their own licenses (see `NOTICE`):

- mini-commander — GPLv3,
- TinyCC — LGPL-2.1,
- ToaruOS — BSD-2-Clause,
- Nuklear — public domain / MIT.
