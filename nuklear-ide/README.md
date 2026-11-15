# Nuklear IDE — malé IDE pro Linux

Minimalistický příklad IDE postavený na [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear)
(immediate-mode GUI) a backendu GLFW + OpenGL 2.1. Cílem není konkurovat
VS Code, ale ukázat kompletní, čitelný základ: strom souborů, taby, editor se
zvýrazňováním syntaxe, překlad TinyCC, klikatelné chyby a spuštění programu.

```
┌──────────────────────────────────────────────────────────────┐
│ File  Edit  Build  View  Help                                │
│ New  Save  Save as…  Build  Run  Stop  Hide output   /projekt│
├───────────────┬──────────────────────────────────────────────┤
│ EXPLORER      │ hello.c                                      │
│ Refresh       │                                              │
│ ▶ src/        │   /* editor — C/C++ syntax highlighting */   │
│   hello.c     │                                              │
├───────────────┴──────────────────────────────────────────────┤
│ OUTPUT                                     Copy   Clear     │
│ $ tcc -B… -std=c11 -Wall -Wextra -O2 -g -o … hello.c         │
│ hello.c:11: error: 're' undeclared          ← klik = skok     │
│ [exit 1]                                                     │
├──────────────────────────────────────────────────────────────┤
│ /projekt/hello.c          Jumped to line 11   Ln 11, Col 1   │
└──────────────────────────────────────────────────────────────┘
```

## Sestavení a spuštění

Potřeby: `gcc` (nebo `clang`), vývojové hlavičky GLFW 3, OpenGL,
`pkg-config` a **TinyCC** pro překlad programů z IDE.

Na Fedoře:

```sh
sudo dnf install gcc glfw-devel mesa-libGL-devel pkgconf-pkg-config tcc
```

Na Debianu/Ubuntu:

```sh
sudo apt install gcc libglfw3-dev libgl1-mesa-dev pkg-config tcc
```

Sestavení a demo:

```sh
cd nuklear-ide
make
make tcc                 # volitelné: najde nebo sestaví hostitelský TinyCC
make run                 # otevře demo/hello.c; F5 přeloží a spustí
```

`make run` závisí na cíli `tcc`. Ten použije `tcc` z `PATH`, nebo sestaví
hostitelský TinyCC ze zdrojů, které už má tento repozitář v
`../_build/download/tinycc` (typicky po `./tests/run-toaru.sh`). Vlastní
cestu lze předat přes `TCC=/cesta/k/tcc`; pokud TinyCC leží ve stromu zdrojů
a potřebuje `-B`, lze nastavit i `TCC_B=/cesta/k/tinycc`.

Nebo ručně:

```sh
./nuklear-ide.bin                    # aktuální adresář jako projekt
./nuklear-ide.bin /cesta/k/projektu  # jiný projekt
./nuklear-ide.bin demo/hello.c       # otevře soubor; projekt = jeho adresář
```

Editor používá font z `IDE_FONT`, jinak zkusí běžné systémové mono fonty
(Liberation Mono, DejaVu Sans Mono, Adwaita Mono) a nakonec vestavěný
Nuklear font.

### Aktualizace Nuklearu

Obě hlavičky jsou uložené v `_import/`, takže projekt jde sestavit offline.
Pro jejich znovustažení slouží:

```sh
./fetch-nuklear.sh              # pevný commit z GitHubu
NK_REF=master ./fetch-nuklear.sh  # poslední master
NK_DEST=/tmp/nk ./fetch-nuklear.sh  # kamkoli jinam
```

Skript stáhne `nuklear.h` a `demo/glfw_opengl2/nuklear_glfw_gl2.h` z
`Immediate-Mode-UI/Nuklear`, ověří základní markery a teprve pak soubory
přepíše. Výchozí pin odpovídá aktuálně commitnutým hlavičkám.

## Ovládání

| Zkratka | Akce |
|---|---|
| `Ctrl+N`, `F4` | nový soubor |
| `Ctrl+O` | otevřít cestu (absolutní nebo relativní k projektu) |
| `Ctrl+S`, `F2` | uložit |
| `Ctrl+Shift+S`, `F3` | uložit jako |
| `Ctrl+W`, `F8` | zavřít tab (u neuložených změn se zeptá) |
| `F5` | přeložit a spustit |
| `F7` | pouze přeložit |
| `Esc` | zastavit běžící program (SIGTERM, po 1 s SIGKILL) |
| `F9` | skrýt/zobrazit panel Output |
| `Ctrl+Q` | ukončit |

Běžné editování: `Ctrl+Z`/`Ctrl+Y` (undo/redo), `Ctrl+A`, `Ctrl+C`/`Ctrl+X`/
`Ctrl+V`, myš a výběr, `Tab` vkládá čtyři mezery. Strom souborů jde rozbalovat
klikáním; kliknutí na soubor ho otevře v novém tabu.

**View ▸ Light mode** přepíná tmavý/světlý vzhled. **View ▸ Hide output**
(`F9`) skrývá panel Output.

### Chyby a skok na řádek

TinyCC píše diagnostiku ve formátu `cesta:řádek: error: …`. Panel Output
takovým řádkům rozumí: jsou obarvené a kliknutím na ně se otevře příslušný
soubor, kurzor se přesune na daný řádek a editor získá fokus, takže se dá hned
psát. Funguje to i pro řádky `In file included from …:N:` a pro warningy.
Nelze kliknout na chyby linkeru, které žádné číslo řádku neobsahují.

### Syntax highlighting

Pro `.c`, `.h`, `.cc`, `.cpp`, `.cxx`, `.hpp` a `.hh` se v editoru zvýrazňují
komentáře, řetězce, čísla, preprocesor, klíčová slova a vestavěné typy.
Highlighting je implementovaný jako overlay nad Nuklear edit widgetem:
`nk_text_edit` dál řeší vstup, výběr, undo a clipboard, ale jeho text se
kreslí průhledně a barevné tokeny kreslí IDE samo nad ním. Díky tomu zůstává
stejné chování editoru a nemusí se kopírovat celý `nk_do_edit`.

## Co je uvnitř

Všechno je v jediném `main.c` (plus stažené hlavičky Nuklearu v `_import/`):

- **UI** — Nuklear kreslí menu, toolbar, strom, taby, editor, output a status
  bar. Vykresluje se přes GLFW/OpenGL 2.1 backend
  `_import/nuklear_glfw_gl2.h`.
- **Editor** — `struct nk_text_edit` z Nuklearu pro každý tab. Uložená verze
  textu slouží jako baseline pro hvězdičku „změněno“. Pro syntaxi se navíc
  staví pole offsetů začátků řádků a stav blokových komentářů.
- **Syntaxe** — jednoduchý C/C++ lexer po řádcích (komentáře, stringy,
  čísla, preprocesor, klíčová slova, typy) a vlastní kreslení textu do
  Nuklear command bufferu.
- **Strom souborů** — rekurzivní `nk_tree_push_hashed`; každý adresář má
  malou cache (1,5 s), filtrují se binárky a `build/`, `.git/`, `_build/` apod.
- **Překlad/spuštění** — TinyCC pro C; `fork`, `setpgid`, `execvp`; rodič čte
  pipe neblokujícím `read` a `waitpid(WNOHANG)` v každém framu.
- **Ukládání** — přímé `fopen`/`fwrite`, bez atomického přejmenování. Pro
  ukázku stačí; pro produkci by se hodilo psát do dočasného souboru a pak
  `rename()`.

TinyCC se hledá v `IDE_TCC` / `TCC`, v `PATH`, a potom v typických místech
tohoto repozitáře (`_build/download/tinycc/tcc`). Pokud je to build ze zdrojů,
IDE automaticky přidá `-B<cesta>`.

**C++:** TinyCC je C kompilátor a nemá C++ frontend. Soubory `.cc`, `.cpp` a
`.cxx` se proto zvýrazňují jako C++, ale pro překlad se použije `CXX` (výchozí
`c++`); v Outputu je na to upozornění. C a hlavičky jdou přes TinyCC.

## ToaruOS

Stejné IDE se dá sestavit i jako **nativní statický program pro ToaruOS**.
Zdrojáky jsou společné; `IDE_TOARU` přepne backend z GLFW/OpenGL na
Yutani + vlastní software rasterizer Nuklear command bufferu.

```sh
./toaru/build.sh                 # -> _output/nuklear-ide-toaru
# nebo
make toaru
```

Build potřebuje sysroot z `./mktcc.sh` a strom ToaruOS 2.3.2:
`_build/download/toaruos-src` (oba věci vytvoří `./tests/run-toaru.sh`).
Výsledek je statický ELF, do kterého jsou přilinkované potřebné klientské
knihovny ToaruOS (`lib/yutani.c`, `hashmap.c`, `list.c`, `pex.c`, `kbd.c`,
`decorations.c`, `menu.c`, `graphics.c`, `text.c`, `icon_cache.c`, `markup.c`,
`markup_text.c` a vestavěné `png.c`/`jpeg.c`/`inflate.c`) a ToaruOS libc;
za běhu tedy nepotřebuje žádné `.so` knihovny.

`tests/run-toaru.sh` integruje IDE do ramdisku:

```sh
./tests/run-toaru.sh
# v ToaruOS:
/extras/nuklear-ide
```

Bez argumentu si IDE vytvoří zapisovatelný projekt
`/tmp/nuklear-ide-demo` s `hello.c`. F5 ho přeloží pomocí
`/extras/tcc/bin/tcc` a hned spustí. Jiný projekt lze otevřít například
`/extras/nuklear-ide /extras` (soubory v `/extras` jsou z ramdisku jen pro
čtení, takže ukládat je třeba jinam).

Backend je v `toaru_backend.h`: vytvoří Yutani okno, přeloží Nuklear
draw commands na trojúhelníky a rasterizuje je do 32bitového framebufferu
okna. Nepoužívá OpenGL; font je vestavěný Nuklear ProggyClean.

Okno používá standardní klientské dekorace ToaruOS (`libtoaru_decorations`):
má titulek „Nuklear IDE“, zavírací tlačítko, jde táhnout za titulkovou lištu,
maximalizovat dvojklikem a měnit velikost tažením za okraj nebo roh. Po
resize se obsah přepočítá (Nuklear dostane nové rozměry a framebuffer se
posune pod titulkovou lištu). `File ▸ Quit` i křížek v titulku ukončí
aplikaci.

## Poznámky k portování

Backend je jediné místo, které ví o platformě; zbytek aplikace kreslí jen
přes Nuklear command buffer. Linux používá GLFW/OpenGL, ToaruOS Yutani a
software rasterizer. Pro další systém je potřeba dodat třetí backend a
upravit:

1. vytvoření okna a vstupní události (myš/klávesy),
2. `fork/exec` + pipe na překlad a spuštění,
3. souborový strom (`dirent`/`stat`) a práci s cestami,
4. hledání TinyCC a cestu k jeho include/runtime souborům.

Samotné obrazovky, layout, editor, syntax highlighting a stavová logika jsou
na platformě nezávislé.

## Omezení ukázky

- Linux i ToaruOS varianta jsou pouze grafické. Nuklear je immediate-mode GUI
  knihovna a nemá připravený terminálový backend (jako ncurses nebo slang),
  takže textový režim v terminálu by vyžadoval vlastní buňkový renderer.
- žádná čísla řádků a žádný jazyk server; highlighting je záměrně jednoduchý,
- žádný debugger ani terminál,
- output panel nemá automatické scrollování na konec a nelze v něm
  označovat text myší (Copy zkopíruje celý výstup),
- TinyCC neumí C++; C++ soubory se překládají přes `CXX` (v ToaruOS `CXX`
  není, takže C++ build tam selže),
- undo historie se v Nuklearu resetuje při opětovném kliknutí do editoru,
- je to ukázka a čitelný starting point, ne produkční IDE.
