# TinyCC pro aarch64 ToaruOS — kompletní postup

Tento dokument popisuje, jak jsme k x86_64 variantě (viz `README.md` v tomto
adresáři) přidali TinyCC pro **aarch64 port ToaruOS 2.3.2**: cross-kompilátor
běžící na x86_64 Linuxu, který generuje spustitelné aarch64 binárky, a
kompilátor, který sám **běží uvnitř aarch64 ToaruOS** v QEMU.

Vše bylo odzkoušeno end-to-end: host cross-kompiluje `hello`, ten se spustí
v guestu, a guest tcc si v guestu sám zkompiluje a spustí další program.

---

## 1. Výchozí stav a klíčové poznatky

x86_64 tok (`scripts/mktcc.sh`) staví tcc na třech sloupcích:

1. **sysroot** — `libc.a`, `crt0.o`, `crti.o`, `crtn.o` a hlavičky vytahané
   z oficiálního x86 ISO,
2. **cross tcc** — `gcc` zkompiluje `tcc.c` s vlastním `config.h`
   (`tcc/config-toaru.h`: `TCC_TARGET_X86_64`, `TARGETOS_Toaru`),
3. **guest tcc** — cross tcc zkompiluje sám sebe pro ToaruOS (self-hosting),
   s `-DTOARU_TCC_NO_NATIVE` a `guest-compat.c`.

Pro aarch64 se scénář lišil ve třech podstatných bodech:

| téma | x86_64 | aarch64 |
|---|---|---|
| zdroj sysrootu | oficiální ISO | **neexistuje žádné ISO** — sysroot je ale v `ramdisk.igz` z oficiálního [aarch64-virt.zip](https://github.com/klange/toaruos/releases/download/v2.3.2/aarch64-virt.zip) |
| layout binárek | statické ET_EXEC na `0x3F000000` (patch v `x86_64-link.c`) | oficiální userland je **dynamický** (ET_EXEC na `0x400000`, `PT_INTERP /lib/ld.so`, `libc.so`) — tcc přesto emituje statické ET_EXEC a aarch64 kernel je spustí, **patch adresy netřeba** (0x400000 je default `arm64-link.c`) |
| závislosti libc.a | žádné extra | `libc.a` odkazuje **libatomic helper** `__aarch64_swp4_acq` (viz §3) |

Zásadní zjištění: oficiální `ramdisk.igz` obsahuje kompletní sysroot
(`lib/libc.a` — 167 objektů, `lib/crt0.o/crti.o/crtn.o`, `usr/include/` — 48
hlaviček). **ToaruOS pro ARM64 proto nebylo potřeba buildovat vůbec** —
`scripts/build-toaruos.sh -a` (docker + gcc toolchain) není pro tcc potřeba.

---

## 2. Co nový skript dělá: `scripts/mktcc-aarch64.sh`

```
_build/tcc-toaru-aarch64/
├─ bin/aarch64-toaru-tcc      cross: host x86_64 → aarch64 ToaruOS
├─ lib/{crt0.o,crti.o,crtn.o,libc.a,libtcc1.a,include/}
├─ obj-libtcc1/               mezivýstup libtcc1 (může se smazat)
└─ tcc-guest-aarch64          tcc, který běží v aarch64 guestu
```

Postup krok za krokem:

1. **Sysroot z release zipu.** Skript stáhne `aarch64-virt.zip` do
   `_import/download/` (pokud chybí), rozbalí a z `ramdisk.igz` vytáhne
   `lib/{crt0,crti,crtn}.o`, `lib/libc.a` a `usr/include/`. Do `include/`
   dá **nejdřív tcc vlastní hlavičky** (`$SRC/include/*.h`, tedy
   `tccdefs.h`, `stdarg.h`, …) a **na ně** ToaruOS hlavičky — stejně jako
   x86 tok. Bez `tccdefs.h` nezkompiluje ani vlastní `lib/` runtime.

2. **`config.h` pro arm64.** Vygeneruje se z `tcc/config-toaru.h` jediným
   `sed`: `TCC_TARGET_X86_64` → `TCC_TARGET_ARM64`. Vše ostatní zůstává:
   `TARGETOS_Toaru` (patch), `CONFIG_TCC_*_PIE` nedefinováno (kernel chce
   ET_EXEC), BCHECK/BACKTRACE/SEMLOCK 0, `CONFIG_TCC_ELFINTERP "-"`.

3. **Cross kompilátor.** Jedno gcc volání, žádný configure/make:
   ```sh
   gcc -O1 -w -DTOARU_HOST_LINUX -DCONFIG_TCCDIR="<sysroot>" \
       -I gen-aarch64 -I tinycc tinycc/tcc.c -o .../aarch64-toaru-tcc
   ```
   tcc obsahuje arm64 backend (`arm64-gen.c`, `arm64-link.c`,
   `arm64-asm.c`) — při `TCC_TARGET_ARM64` se zkompilují místo x86.

4. **`libtcc1.a`** — objektová sada pro arm64 z tcc `lib/Makefile`
   (`OBJ-arm64`): `lib-arm64.c` (dělení apod.; **žádný `libtcc1.o`** — ten
   je x86-only), `stdatomic.c`, `builtin.c`, `dsohandle.c`, `armflush.c`,
   `atomic.S`, `alloca.S`, `alloca-bt.S` (všechny `.S` mají `__aarch64__`
   větve) **+ náš `aarch64-atomic-helpers.S`** (§3). Každý objekt zkompiluje
   samotný cross tcc (`-B sysroot -c`), archiv přes `tcc -ar`.

5. **Guest tcc** — cross tcc zkompiluje `tcc.c` znovu, tentokrát jako
   aarch64 binárku pro ToaruOS:
   ```sh
   aarch64-toaru-tcc -B sysroot -DTOARU_TCC_NO_NATIVE \
       -DCONFIG_TCCDIR='"/extras/tcc"' \
       -DCONFIG_TCC_CRTPREFIX='"{B}/lib"' -DCONFIG_TCC_LIBPATHS='"{B}/lib"' \
       -o tcc-guest-aarch64 tinycc/tcc.c tcc/toaru/guest-compat.c \
                            tcc/toaru/aarch64-atomic-helpers.S
   ```
   Výsledek: 523 kB statická aarch64 binárka. `guest-compat.c` doplňuje
   `strtold`/`ldexpl` (ToaruOS libc je nemá) — na aarch64 funguje stejně
   jako na x86.

6. **Smoke test** — cross tcc zkompiluje `tcc_hello.c` a skript zkontroluje
   stejnými pravidly jako x86: ET_EXEC, žádné PT_INTERP/PT_DYNAMIC.

---

## 3. Dvě opravdu nové věci (bug-fixy portů)

### 3a. Relokace `R_AARCH64_ADR_PREL_LO21` (274) v tcc linkeru

**Symptom:** link čehokoli s release `crt0.o` padlo na
`Unknown relocation type for got: 274`.

**Diagnóza:** aarch64 `crt0.S` z release začíná
```asm
_start:
    adr x0, main        ; small code model → R_AARCH64_ADR_PREL_LO21 (274)
    bl  pre_main
```
tcc uměl jen `R_AARCH64_ADR_PREL_PG_HI21` (275, `adrp`, page-based) —
`adr` s plným PC-relative ofsetem (21 bitů, ±1 MiB) ve `arm64-link.c`
chyběl. Jde o tři místa: `code_reloc`, `gotplt_entry_type`
(`NO_GOTPLT_ENTRY`) a `relocate`.

**Fix** (foldnut do `tcc/toaru/tcc-toaru.patch`, guard list v `mktcc.sh`
rozšířen o `arm64-link.c`, takže čerstvý klon dostane opravu také):
kódování imm je stejné jako u ADRP (`immlo` bity 30:29, `immhi` 23:5),
jen se nezshiftuje po stránkách:

```c
case R_AARCH64_ADR_PREL_LO21: {
    int64_t off = val - addr;                 /* plný ofset, ±1 MiB */
    if ((uint64_t)(off + ((uint64_t)1 << 20)) >> 21)
        tcc_error_noabort("R_AARCH64_ADR_PREL_LO21 relocation failed");
    write32le(ptr, ((read32le(ptr) & 0x9f00001f) |
                    (off & 0x1ffffc) << 3 | (off & 3) << 29));
    return;
}
```
Maska `0x9f00001f` zachovává i opcode (bit 31: ADRP=1, ADR=0), takže jeden
kód posluží oběma variantám.

### 3b. Chybějící libatomic helper `__aarch64_swp4_acq`

**Symptom:** po opravě §3a padl self-compile na
`unresolved reference to '__aarch64_swp4_acq'`.

**Diagnóza:** `libc.a` (atexit.o, malloc.o, pthread.o) má symbol jako
nedefinovaný; definovaný je jen v **dynamickém** `libc.so` (a v libatomic
z gcc toolchainu, který nemáme). Upstream prostě nikdy nelinkuje
staticky. Zdrojově jde o `__sync_lock_test_and_set(lock, 1)` v
`malloc.c`/`spinlock.h` — gcc pro aarch64 bez LSE nechá vzniknout volání
helperu.

**Konvence volání** jsem vyčetl z disassemblace release `atexit.o`
(`binutils-aarch64-linux-gnu-objdump`):
```asm
mov  x1, <ptr>          ; ukazatel na proměnnou
mov  w0, #1             ; nová hodnota
bl   __aarch64_swp4_acq ; stará hodnota ve w0
```

**Fix** — `tcc/toaru/aarch64-atomic-helpers.S`, klasický LL/SC loop:
```asm
__aarch64_swp4_acq:
    mov   w3, w0           /* hodnota k uložení */
1:  ldaxr w0, [x1]         /* stará hodnota, acquire */
    stlxr w4, w3, [x1]     /* store-release, w4 = selhání */
    cbnz  w4, 1b
    ret
```
Soubor se linkuje do guest tcc i do `libtcc1.a`, takže každý cross-kompilovaný
program, který natáhne `malloc()`/`atexit()` z libc.a, má helper k dispozici.
Pokud by budoucí libc přidala další varianty (`__aarch64_cas*`,
`__aarch64_ldadd*`, `__aarch64_swp{1,2,8}_*`), přidají se stejným vzorem.

---

## 4. Ověření v guestu

Payload se nabalí do kopie release ramdisku (přesně jako `mc-toaru/mkiso.sh`
na x86 — jen bez ISO, aarch64 bootuje přímo z fw_cfg):

```
/extras/tcc/{bin/tcc, lib/{crt*.o,libc.a,libtcc1.a}, include/}
/extras/examples/*.c          # zdrojové texty příkladů
/extras/hello                 # cross-kompilovaný ukazový program
/bin/tcc -> /extras/tcc/bin/tcc   # symlink, tcc na PATH
```

Boot (stejné invokace jako `tests/run-arm64.sh`, jen s vlastním ramdiskem):
```sh
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 4G -smp 4 -display none \
    -kernel _import/download/aarch64-virt/bootstub \
    -append "root=/dev/ram0 migrate start=--headless" \
    -fw_cfg name=opt/org.toaruos.initrd,file=_build/ramdisk-a64.tgz \
    -fw_cfg name=opt/org.toaruos.kernel,file=_import/download/aarch64-virt/misaka-kernel \
    -fw_cfg name=opt/org.toaruos.gettyargs,string='-a local /dev/ttyS0 115200 xterm' \
    -serial pipe:$WORK/serial
```

Skutečný transcript ze sériové linky (login `local@livecd` po ~31 s):

```
/extras/hello
TinyCC inside ToaruOS
1+2+...+10 = 55, argc = 1          ← cross-kompilovaná binárka z hosta

tcc -o /tmp/hello /extras/examples/tcc_hello.c
In file included from /extras/examples/tcc_hello.c:10:
/extras/tcc/include/stdlib.h:52: warning: NULL redefined   (známé, viz x86 README)

/tmp/hello
TinyCC inside ToaruOS
1+2+...+10 = 55, argc = 1          ← guest tcc zkompiloval a program běžel
```

---

## 5. Reprodukce od nuly

```sh
./scripts/mktcc.sh              # klon TinyCC + patch (sdílený s aarch64)
./scripts/mktcc-aarch64.sh      # cross tcc + libtcc1 + guest tcc
# pak: nabalit payload do kopie ramdisku (viz §4) a bootnout přes fw_cfg
```

Poznámky pro budoucí práci:

- **mini-commander pro aarch64** zatím není (`mc-toaru` ncurses shim je
  x86-only) — payload ARM64 guestu je tcc + příklady.
- **`tcc -run`** funguje ani na x86 (žádné mmap v kernelu), na aarch64 platí
  totéž (`-DTOARU_TCC_NO_NATIVE`).
- gcc aarch64 toolchain (`build-toaruos.sh -a`, `gcc_arm64.tgz`) není pro tcc
  potřeba — potřeboval by se jen pro build dalšího guest userlandu, ne pro
  kompilátor.
- Dynamické linkování (ld.so, `libc.so`) tcc nepodporuje a nepodporuje ho
  ani x86 varianta; statické ET_EXEC je na obou architekturách jediná cesta.
