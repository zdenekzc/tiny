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
./nuklear-ide                    # aktuální adresář jako projekt
./nuklear-ide /cesta/k/projektu  # jiný projekt
./nuklear-ide demo/hello.c       # otevře soubor; projekt = jeho adresář
```

Editor používá font z `IDE_FONT`, jinak zkusí běžné systémové mono fonty
(Liberation Mono, DejaVu Sans Mono, Adwaita Mono) a nakonec vestavěný
Nuklear font.

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
soubor a kurzor se přesune na daný řádek. Funguje to i pro řádky
`In file included from …:N:` a pro warningy. Nelze kliknout na chyby linkeru,
které žádné číslo řádku neobsahují.

### Syntax highlighting

Pro `.c`, `.h`, `.cc`, `.cpp`, `.cxx`, `.hpp` a `.hh` se v editoru zvýrazňují
komentáře, řetězce, čísla, preprocesor, klíčová slova a vestavěné typy.
Highlighting je implementovaný jako overlay nad Nuklear edit widgetem:
`nk_text_edit` dál řeší vstup, výběr, undo a clipboard, ale jeho text se
kreslí průhledně a barevné tokeny kreslí IDE samo nad ním. Díky tomu zůstává
stejné chování editoru a nemusí se kopírovat celý `nk_do_edit`.

## Co je uvnitř

Všechno je v jediném `main.c` (plus vestavěné hlavičky Nuklearu ve `vendor/`):

- **UI** — Nuklear kreslí menu, toolbar, strom, taby, editor, output a status
  bar. Vykresluje se přes GLFW/OpenGL 2.1 backend
  `vendor/nuklear_glfw_gl2.h`.
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

## Poznámky k portování

Tohle je záměrně **linuxová první verze**. Backend je jediné místo, které ví
o GLFW/OpenGL; zbytek aplikace kreslí jen přes Nuklear command buffer. Pro
jiný operační systém (nebo pro ToaruOS) je potřeba dodat jiný Nuklear backend
a upravit:

1. vytvoření okna a vstupní události (myš/klávesy),
2. `fork/exec` + pipe na překlad a spuštění,
3. souborový strom (`dirent`/`stat`) a práci s cestami,
4. hledání TinyCC a cestu k jeho include/runtime souborům.

Samotné obrazovky, layout, editor, syntax highlighting a stavová logika jsou
na platformě nezávislé. Vestavěný font a OpenGL backend se na zařízení bez
OpenGL nahradí software rendererem; Nuklear má k dispozici i
command-buffer/software cestu.

## Omezení ukázky

- žádná čísla řádků a žádný jazyk server; highlighting je záměrně jednoduchý,
- žádný debugger ani terminál,
- output panel nemá automatické scrollování na konec a nelze v něm
  označovat text myší (Copy zkopíruje celý výstup),
- TinyCC neumí C++; C++ soubory se překládají přes `CXX`,
- undo historie se v Nuklearu resetuje při opětovném kliknutí do editoru,
- je to ukázka a čitelný starting point, ne produkční IDE.
