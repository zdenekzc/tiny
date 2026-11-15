# Nuklear IDE — malé IDE pro Linux

Minimalistický příklad IDE postavený na [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear)
(immediate-mode GUI) a backendu GLFW + OpenGL 2.1. Cílem není konkurovat
VS Code, ale ukázat kompletní, čitelný základ: strom souborů, taby, editor,
asynchronní překlad a spuštění programu.

```
┌──────────────────────────────────────────────────────────────┐
│ File  Edit  Build  View  Help                                │
│ New  Save  Save as…  Build  Run  Stop  Hide output   /projekt│
├───────────────┬──────────────────────────────────────────────┤
│ EXPLORER      │ hello.c                                      │
│ Refresh       │                                              │
│ ▶ src/        │   /* editor — Nuklear multiline edit */      │
│   hello.c     │                                              │
├───────────────┴──────────────────────────────────────────────┤
│ OUTPUT                                     Copy   Clear     │
│ $ cc -std=c11 -Wall -Wextra -O2 -g -o … hello.c             │
│ [exit 0]                                                     │
├──────────────────────────────────────────────────────────────┤
│ /projekt/hello.c          Build finished       Ln 11, Col 18 │
└──────────────────────────────────────────────────────────────┘
```

## Sestavení a spuštění

Potřeby: `gcc` (nebo `clang`), vývojové hlavičky GLFW 3, OpenGL a `pkg-config`.
Na Fedoře:

```sh
sudo dnf install gcc glfw-devel mesa-libGL-devel pkgconf-pkg-config
```

Na Debianu/Ubuntu:

```sh
sudo apt install gcc libglfw3-dev libgl1-mesa-dev pkg-config
```

Sestavení a demo:

```sh
cd nuklear-ide
make
make run                 # otevře demo/hello.c
```

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

Tlačítko **Run** vždy nejdřív uloží a přeloží aktuální soubor, potom spustí
výsledný binární soubor. Program běží asynchronně (fork/exec + neblokující
roura), takže UI nezamrzne. Standardní výstup a chyby se slévají do panelu
**Output**; běžící program dostane po 15 sekundách `SIGKILL`, aby se dalo
pracovat i s programem, který se zacyklí. Standardní vstup programu je
`/dev/null`.

Překladač se bere z `CC` (C) nebo `CXX` (C++), jinak `cc`/`c++`. Céčko se
překládá s `-std=c11 -Wall -Wextra -O2 -g`, C++ s `-std=c++17`. `CC`/`CXX`
musí být jediná cesta k executable (ne řetězec s dalšími argumenty).

## Co je uvnitř

Všechno je v jediném `main.c` (plus vestavěné hlavičky Nuklearu ve `vendor/`):

- **UI** — Nuklear kreslí menu, toolbar, strom, taby, editor, output a status
  bar. Vykresluje se přes GLFW/OpenGL 2.1 backend
  `vendor/nuklear_glfw_gl2.h`.
- **Editor** — `struct nk_text_edit` z Nuklearu pro každý tab. Uložená verze
  textu slouží jako baseline pro hvězdičku „změněno“.
- **Strom souborů** — rekurzivní `nk_tree_push_hashed`; každý adresář má
  malou cache (1,5 s), filtrují se binárky a `build/`, `.git/`, `_build/` apod.
- **Překlad/spuštění** — `fork`, `setpgid`, `execvp`; rodič čte pipe
  neblokujícím `read` a `waitpid(WNOHANG)` v každém framu.
- **Ukládání** — přímé `fopen`/`fwrite`, bez atomického přejmenování. Pro
  ukázku stačí; pro produkci by se hodilo psát do dočasného souboru a pak
  `rename()`.

## Poznámky k portování

Tohle je záměrně **linuxová první verze**. Backend je jediné místo, které ví
o GLFW/OpenGL; zbytek aplikace kreslí jen přes Nuklear command buffer. Pro
jiný operační systém (nebo pro ToaruOS) je potřeba dodat jiný Nuklear backend
a upravit:

1. vytvoření okna a vstupní události (myš/klávesy),
2. `fork/exec` + pipe na překlad a spuštění,
3. souborový strom (`dirent`/`stat`) a práci s cestami.

Samotné obrazovky, layout, editor a stavová logika jsou na platformě nezávislé.
Vestavěný font a OpenGL backend se na zařízení bez OpenGL nahradí software
rendererem; Nuklear má k dispozici i command-buffer/software cestu.

## Omezení ukázky

- žádné zvýrazňování syntaxe a žádná čísla řádků (Nuklear editor je čistý
  text; šlo by doplnit vlastní widget nad `nk_text_edit`),
- žádný debugger, terminál ani jazykový server,
- output panel je scrollovatelný seznam řádků, ale bez barev a bez
  automatického scrollování na konec,
- undo historie se v Nuklearu resetuje při opětovném kliknutí do editoru,
- je to ukázka a čitelný starting point, ne produkční IDE.
