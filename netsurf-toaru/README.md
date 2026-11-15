# NetSurf for ToaruOS

An experiment in porting [NetSurf](https://www.netsurf-browser.org/) 3.11 to
ToaruOS 2.3.2, using the
[toaruos/netsurf-frontend-toaru](https://github.com/toaruos/netsurf-frontend-toaru)
frontend.

The result is a **static** ToaruOS executable: NetSurf, the NetSurf support
libraries, zlib and the ToaruOS client libraries (Yutani, `libtoaru_graphics`,
decorations, menus, …) are all linked into a single binary.  The only runtime
dependencies are the compositor and the shared font/menu services.

## Status

Working:

- builds as a static ToaruOS ELF (`netsurf-3.11/netsurf`, ~12 MB)
- starts, opens a Yutani window, renders the menu bar, navigation buttons and
  icons, URL bar, status bar and window decorations
- fetches local files (`file:///test.html`) and reaches content creation

Known problem:

- the page area stays blank and the status line says “Loading”.  NetSurf
  fetches the file, calls `content__init`, then `html_create()` fails almost
  immediately and the browser loops on `about:query/fetcherror`.  The suspect
  is `dom_hubbub_parser_create()` in `html_create_html_data()`.  This is where
  the port currently stops.

The frontend is unmodified apart from the patches below; the missing piece is
in the NetSurf/libdom/libhubbub layer, not in the Yutani window code.

## Files

```
build.sh                 end-to-end build
toolchain/gcc-toaru.sh   host gcc -> static ToaruOS ELF linker driver
toolchain/ar-toaru.sh    host ar wrapper
toolchain/compat.h       limits/PRI* fallbacks for the freestanding build
toolchain/include/       small header shims (sys/select.h, netinet/in.h,
                         curl/curl.h for the disabled cURL fetcher)
patches/
  netsurf-3.11.patch             core build config and ToaruOS portability
  netsurf-frontend-toaru.patch   frontend resource path and redraw fixes
  toaruos-static-sprites.patch   register PNG/JPEG decoders without dlopen
```

The core patch:

- adds `toaru` to the frontend targets and a `Makefile.config` that disables
  the features which need libraries ToaruOS does not provide (cURL, OpenSSL,
  image decoders, Duktape, …)
- teaches `utils/config.h` about `TOARUOS` (no `mmap`, `inet_aton`, regex;
  `strchrnul` exists)
- renames NetSurf’s hashmap symbols so they do not collide with the ToaruOS
  hashmap at link time

The frontend patch:

- searches for resources in `/extras/netsurf` first
- uses the cross prefix’s `pkg-config` database
- recomputes the page viewport on redraw and preserves an invalidation that
  arrives while `browser_window_create()` is still on the stack

The ToaruOS patch adds the built-in PNG/JPEG decoders to the sprite loader for
static builds; the stock code only finds them by `dlopen()`ing
`libtoaru_png.so` / `libtoaru_jpeg.so`, which a static executable cannot do.

## Building

Prerequisites:

- the ToaruOS sysroot produced by `../mktcc.sh` (`_build/tcc-toaru/lib`)
- a ToaruOS source checkout (`_build/download/toaruos-src`, produced by
  `../tests/run-toaru.sh`)
- host `gcc`, GNU `ld`, `make`, `perl`, `curl`, `python3`

```sh
./build.sh
```

The script downloads NetSurf and the support libraries on first use
(`_build/download/netsurf-deps`) and installs the cross-built dependencies into
`_build/netsurf-prefix`.  Output: `_build/download/netsurf-3.11/netsurf`.

Set `SKIP_DEPS=1` to reuse an already built prefix.

## Running under ToaruOS

Pack the binary and the NetSurf resources, copy them into the guest and start
the compositor first:

```sh
compositor &
export DISPLAY=compositor
/extras/netsurf/netsurf file:///test.html
```

The `file://` fetcher needs no network support.  HTML rendering for `http://`
would additionally require the cURL-based fetcher, which is not enabled in
this configuration.

## Notes

- TinyCC is not used here; this is a host-cross build.  The unrelated Nuklear
  IDE port uses the in-guest TinyCC instead.
- `NETSURF_LOG_LEVEL` can be raised in the build to get more detail; the
  frontend accepts `-v` for verbose logging to stderr.
- The current test binaries also carried temporary `fprintf` tracing in
  `gui.c`/`main.c` and in ToaruOS `button.c`/`decorations.c`; those prints are
  not part of the patches.
