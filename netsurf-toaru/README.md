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

- builds as a static ToaruOS ELF (`netsurf-3.11/netsurf`, ~13 MB)
- starts, opens a Yutani window, renders the menu bar, navigation buttons and
  icons, URL bar, status bar and window decorations
- fetches local files (`file:///…`) and **plain HTTP** pages, parses HTML with
  the full libdom/libhubbub stack, builds the box tree, lays the page out and
  renders text with the default stylesheet, list markers, links and colours;
  the status bar ends in “Done”
- the HTTP fetcher is a static libcurl 8.4.0 built against the ToaruOS
  sysroot; ToaruOS network setup (DHCP, DNS) and `http://` therefore work,
  e.g. `http://info.cern.ch/`

Not supported in this configuration:

- `https://` — ToaruOS has no TLS stack (even its own `fetch` reports “no tls
  backend”), so libcurl is built without any SSL backend and only the HTTP
  protocol.  HTTPS would need mbedTLS/BearSSL/OpenSSL ported first.
- images — the PNG/JPEG/GIF/BMP decoders are disabled in `Makefile.config`
  (the ToaruOS sprite loader is patched for static builds, but no image
  handler is compiled in)
- JavaScript (Duktape) and SVG

Non-obvious pieces that were needed to get this far; all are handled by
`build.sh` or the patches below:

- libparserutils defaults to an **iconv**-based input filter.  ToaruOS ships
  iconv stubs whose `iconv_open()` always fails, so every
  `parserutils_inputstream_create()` (and thus every HTML document) failed and
  the page stayed blank.  parserutils is now built with
  `-DWITHOUT_ICONV_FILTER` and uses its built-in codecs.
- the frontend’s local `tt_*stringn*` helpers passed a signed `char` to
  ToaruOS’s `decode()` (which takes `uint32_t`).  A non-ASCII byte such as the
  list bullet (U+2022) became `0xFFFFFFE2`, indexed the state table out of
  bounds and crashed the browser during layout.  The helpers now cast to
  `unsigned char`, matching ToaruOS’s own `tt_string_width()`.
- libcurl is configured with `curl_disallow_getaddrinfo=yes`.  ToaruOS’s
  `getaddrinfo()` ignores the service argument and does not set the port in
  `sockaddr_in`, so libcurl uses its `gethostbyname()` resolver instead.  Its
  bundled `inet_pton()`/`inet_ntop()` implementations are used for the same
  reason (ToaruOS lacks them).
- ToaruOS has the `fd_set` type but no `select()` and no `FD_*` macros; they
  are provided by the `sys/select.h` shim, which the compiler driver
  force-includes.  `AF_UNIX` (used only as an ignored argument to curl’s
  socketpair fallback) comes from the `sys/socket.h` shim.

## Files

```
build.sh                 end-to-end build
toolchain/gcc-toaru.sh   host gcc -> static ToaruOS ELF linker driver
toolchain/ar-toaru.sh    host ar wrapper
toolchain/compat.h       limits/PRI* fallbacks for the freestanding build
toolchain/include/       small header shims (sys/select.h with FD_* macros,
                         sys/socket.h with AF_UNIX, netinet/in.h, curl/curl.h
                         wrapping the real header)
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
- casts bytes to `unsigned char` before UTF-8 decoding in the width,
  position, split and draw helpers

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

Pack the binary and the NetSurf resources into `/extras/netsurf` (the binary,
`default.css`, `internal.css`, `quirks.css`, `adblock.css`, the `Messages`
file and the `en/`… resource directories), start the compositor and set
`DISPLAY` **before** starting NetSurf:

```sh
compositor &
export DISPLAY=compositor
/extras/netsurf/netsurf file:///extras/test.html
/extras/netsurf/netsurf http://info.cern.ch/
```

`DISPLAY` is essential: ToaruOS fonts are requested from the compositor’s
shared-memory font service as `sys.$DISPLAY.fonts.sans-serif`.  Without it
`tt_font_from_shm()` returns NULL while the menu/decorations libraries are
initialised and NetSurf crashes before the window appears.

For `http://` the guest needs working DHCP/DNS.  With the QEMU user-mode
network used by `toaru-vm.sh`, startup DHCP gives the guest `10.0.2.15` and
name resolution through `10.0.2.3`, so no manual network setup is needed.
`https://` URLs fail with `Protocol "https" not supported` — that is the
missing TLS stack, not a network problem.

## Notes

- TinyCC is not used here; this is a host-cross build.  The unrelated Nuklear
  IDE port uses the in-guest TinyCC instead.
- `NETSURF_LOG_LEVEL` can be raised in the build to get more detail; the
  frontend accepts `-v` for verbose logging to stderr.
- The frontend sources and the local ToaruOS checkout contain a few stray
  `fprintf` debug prints (upstream in `main.c`/`gui.c`, and in
  `button.c`/`decorations.c` of the checkout); they do not come from the
  patches.
