# Tiny IDE — malé ncurses IDE pro Linux

Tiny IDE je textový protějšek `nuklear-ide`. Má strom projektu, taby,
C/C++ editor se zvýrazňováním syntaxe, překlad TinyCC, asynchronní spuštění
programu a klikatelnou diagnostiku. Vzhled a ovládání navazují na
`mini-commander`: modré pozadí, azurové lišty, ASCII dialogy, myš a funkční
klávesy `F1`–`F10`.

Jde o malý a čitelný základ, ne o konkurenci VS Code nebo Midnight Commandera.
Program momentálně cílí na **Linux**; vrstvy jsou oddělené tak, aby mohl
následovat port pro ToaruOS.

```text
 Tiny IDE  ~/projekt
[hello.c *] [main.c] [Makefile]
 EXPLORER              |   1 #include <stdio.h>
 ~/projekt             |   2
 - src/                |   3 static int square(int x)
   main.c              |   4 {
 + include/            |   5     return x * x;
 hello.c               |   6 }
 README.md             |   7
----------------------- OUTPUT --------------------------------------------
$ tcc -std=c11 -Wall -Wextra -O2 -g -o /tmp/tiny-ide-…/hello hello.c
[exit 0]
 hello.c *            Saved    Ln 10, Col 28                  Editor
1Help  2Save  3SaveAs  4New  5Run  6Focus  7Build  8Close  9Output  10Quit
```

## Sestavení a spuštění

Potřeby: C překladač, Unicode `ncursesw` a `pkg-config`. Pro překlad programů
z IDE se používá TinyCC; ten nemusí být nainstalovaný, pokud jsou k dispozici
jeho zdroje v tomto repozitáři.

Fedora:

```sh
sudo dnf install gcc ncurses-devel pkgconf-pkg-config
```

Debian/Ubuntu:

```sh
sudo apt install build-essential libncursesw5-dev pkg-config
```

Sestavení a demo:

```sh
cd tiny-ide
make
make tcc                  # systémový tcc nebo _build/host-tcc/tcc
make run                  # otevře demo/hello.c; F5 přeloží a spustí
```

Nebo ručně:

```sh
./tiny-ide.bin                         # aktuální adresář jako projekt
./tiny-ide.bin /cesta/k/projektu       # jiný projekt
./tiny-ide.bin demo/hello.c            # soubor; projekt = jeho adresář
./tiny-ide.bin --nocolor demo/hello.c  # monochromatický režim
```

Doporučený je UTF-8 terminál alespoň 80×24, například `LC_ALL=C.UTF-8`.
Terminál menší než 40×8 program odmítne vykreslovat do doby, než ho zvětšíte.

### Hostitelský TinyCC

`make tcc` nejprve použije `tcc` z `PATH`. Jinak sestaví zdroje z
`../_import/download/tinycc` **mimo download strom** do
`../_build/host-tcc`. Nejsou potřeba žádné další stahování ani úpravy zdrojů.
Build vytvoří jen `tcc`, `libtcc1.a` a odkaz na potřebné include hlavičky.

TinyCC lze také zvolit prostředím:

```sh
TCC=/cesta/k/tcc ./tiny-ide.bin demo/hello.c
# IDE_TCC má přednost před TCC; nestandardní -B lze zadat takto:
TCC_B=/cesta/k/tcc-runtime ./tiny-ide.bin demo/hello.c
# IDE_CFLAGS nahradí výchozí sadu přepínačů (-std=c11 -Wall -Wextra -O2 -g),
# hodí se na pomalých emulovaných cílech, kde tcc -O2 trvá minut:
IDE_CFLAGS=-g0 ./tiny-ide.bin demo/hello.c
```

C soubory se překládají TinyCC. Pro `.cc`, `.cpp` a `.cxx` se místo něj použije
`CXX` (výchozí `c++`), protože TinyCC nemá C++ frontend.

## Ovládání

| Zkratka | Akce |
|---|---|
| `F1` | nápověda |
| `F2`, `Ctrl+S` | uložit |
| `F3` | uložit jako |
| `F4`, `Ctrl+N` | nový soubor |
| `F5` | uložit, přeložit a spustit |
| `F6`, `Shift+Tab` | předchozí/další panel |
| `F7` | pouze přeložit |
| `F8`, `Ctrl+W` | zavřít tab |
| `F9` | skrýt/zobrazit Output |
| `F10`, `Ctrl+Q` | ukončit |
| `Ctrl+O` | otevřít cestu |
| `Ctrl+R` | obnovit strom projektu |
| `Ctrl+L` | překreslit obrazovku |
| `Ctrl+PgUp/PgDn`, `Alt+Left/Right` | předchozí/další tab |
| `Alt+1/2/3` | Explorer / editor / Output |
| `Esc` | zastavit překlad nebo program; jinak zrušit výběr |

`F6` přepíná v pořadí Explorer → editor → Output. Pokud je Output skrytý,
přepíná jen první dva panely.

### Editor

- běžný text, UTF-8 včetně širokých znaků a kombinujících diakritických znamének;
- šipky, `Home`, `End`, `PgUp`, `PgDn`, `Ctrl+Home`, `Ctrl+End`;
- `Ctrl+Left/Right` pro pohyb po slovech;
- `Shift+šipky/Home/End` pro výběr a myš pro kurzor, označení a kolečko;
- `Ctrl+A/C/X/V` pro výběr všeho, kopírování, vyjmutí a vložení;
- `Ctrl+Z` / `Ctrl+Y` pro undo/redo;
- `Ctrl+F` pro přesné hledání s pokračováním od kurzoru přes konec souboru;
- `Ctrl+G` pro skok na řádek;
- `Tab` vloží čtyři mezery, `Enter` zachová odsazení a za `{` přidá čtyři mezery;
- `Backspace` a `Delete` umí spojovat řádky a mazat výběr.

Clipboard je **interní pro jedno spuštěné IDE**, není propojený se systémovým
`X11`/Wayland clipboardem. Historie undo používá jen změněné byty, nikoli
kompletní kopie souboru.

Soubory se ukládají atomicky přes dočasný soubor a `rename()`. U existujících
souborů se zachovají práva a vlastník; cíl symbolického odkazu se přepíše bez
nahrazení samotného odkazu. CRLF se po načtení normalizuje na LF.

### Explorer

Strom se ovládá šipkami, `Home`, `End`, `PgUp/PgDn` a `Enter`. `Right` adresář
rozbalí, `Left` ho sbalí nebo přejde na rodiče. Soubor se otevře klávesou
`Enter` nebo kliknutím. Skryté položky a obvyklé build adresáře (`.git`,
`build`, `_build`, `_tmp`, `target`, `node_modules` atd.) se nezobrazují.
Strom pojme nejvýše 2048 položek a 12 úrovní.

### Output, chyby a běžící program

`F5` a `F7` zobrazí Output a nechají překlad běžet na pozadí. Po úspěšném
překladu `F5` rovnou spustí výsledný program. Standardní výstup i chyby jsou v
jednom panelu; uchová se posledních 2000 řádků a nejvýše 8 KiB z jednoho řádku.

Po přepnutí `F6` do Outputu šipky vybírají řádky. `Enter` na řádce
`cesta.c:12: error: …` otevře soubor a přesune kurzor na uvedený řádek; totéž
udělá kliknutí. Funguje i `warning:` a `In file included from …:N:`. Chyby
linkeru bez čísla řádku samozřejmě cíl nemají.

Spuštěný program má stdin připojený na `/dev/null`, takže interaktivní vstup
z Output panelu nefunguje. `Esc` pošle procesní skupině `SIGTERM` a po jedné
sekundě `SIGKILL`. Běh delší než 15 sekund se automaticky ukončí. Dočasné
binárky jsou v `/tmp/tiny-ide-XXXXXX` a po skončení IDE se smažou.

## Testy

```sh
make check          # jednotkové testy + ncurses integrace
make check-unit     # textový model, bez terminálu
make check-smoke    # skutečný pseudoterminál, F5/F7 a dialogy
```

`tests/test_editor.c` ověřuje UTF-8, editace, výběry, undo/redo, hledání a
okrajové stavy. `tests/smoke.py` používá pouze standardní knihovnu Pythonu.
Spustí IDE na pseudoterminálu 100×32 a ověří:

1. editaci, uložení, `F5`, skutečný překlad a výstup `385` z dema,
2. potvrzení ukončení s neuloženým souborem a zahození změn,
3. chybný překlad přes `F7` a skok na `broken.c:4`.

## Struktura

```text
tiny-ide/
├── main.c             # argumenty, akce, klávesnice a myš
├── app.c              # soubory, taby, output a strom projektu
├── editor.c/.h        # na curses nezávislý UTF-8 textový model a undo/redo
├── runner.c           # TinyCC/CXX, procesní skupiny, diagnostika
├── ui.c               # ncurses panely, syntaxe a klávesové sekvence
├── dialogs.c          # vstupní a potvrzovací dialogy, nápověda
├── demo/hello.c
└── tests/
```

Mezi jednotlivými částmi nejsou grafické knihovny, threads ani externí UI
runtime. Linuxová verze používá pouze `ncursesw` a POSIX.

## ToaruOS port

Port pro ToaruOS (x86_64) staví `build-toaru.sh` se křížovým TinyCC ze
`scripts/mktcc.sh`. Zdrojové soubory Linuxu se používají nezměněné; rozdíl je
jen v kompatibilní vrstvě:

- `toaru/wctype.h` a `toaru/compat.c` — UTF-8 konverze a klasifikace
  (`mbrtowc`, `wcrtomb`, `mbstate_t`, `iswprint`, …), které libc ToaruOS
  nemá, a no-op `fsync`;
- `../mc-toaru/toaru/curses.c` — sdílený ncurses shim, na kterém běží
  `mini-commander`; Tiny IDE ho používá beze změny (jen s doplněným
  `KEY_SLEFT`).

Build:

```sh
./scripts/mktcc.sh          # křížový tcc + sysroot z release ISO (jednou)
tiny-ide/build-toaru.sh     # => _build/output/tiny-ide (ELF pro ToaruOS)
```

Do ramdisku obrazu se Tiny IDE dostává přes `tests/build-toaru.sh` jako
`/extras/tiny-ide` s demem v `/extras/tiny-ide-demo`. Headless kontrola
spustí IDE, klávesou F5 nechá přeložit a spustit demo hostovaným TinyCC
(„sum of squares 1..10 = 385") a F10 ho ukončí.

```sh
./tests/build-toaru.sh      # kompletní obraz; VM zůstane běžet
# v terminálu ToaruOS: /extras/tiny-ide /extras/tiny-ide-demo
```

## Omezení

- maximální velikost otevřeného souboru je 4 MiB;
- lexer zvýrazňuje C/C++, ne plnou gramatiku; ostatní typy souborů jsou bez barev;
- hledání je case-sensitive a neumí regulární výrazy;
- clipboard je pouze interní, stdin programu je `/dev/null`;
- žádný LSP, debugger, projektový Makefile runner ani vícesouborový build;
- při otevření se soubor nijak nesleduje, externí změny se samy nenačtou.
