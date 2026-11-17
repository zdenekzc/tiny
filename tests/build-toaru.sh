#!/usr/bin/env bash
# ------------------------------------------------------------
# "One-click" ToaruOS + TinyCC + mini-commander:
#
#   ./tests/build-toaru.sh
#
# What the script does (everything can be deleted and run again at any time):
#   1. downloads the ToaruOS 2.3.2 ISO (if missing),
#   2. downloads the ToaruOS sources (tag v2.3.2, just for browsing in the guest),
#   3. downloads TinyCC and builds two things:
#        - a cross compiler on the host (_build/tcc-toaru/bin/x86_64-toaru-tcc),
#        - a tcc that runs INSIDE ToaruOS (_build/tcc-toaru/tcc-guest),
#   4. downloads mini-commander and compiles it with the cross tcc (_output/mc),
#   5. downloads the Nuklear header (browsing only),
#   6. builds the native Nuklear IDE for ToaruOS (Yutani + software renderer),
#      builds NetSurf (netsurf-toaru/build.sh; reused when it already exists)
#      and makes _build/custom.iso via mc-toaru/mkiso.sh: ALL files (mc, tcc,
#      examples, sources, the IDE, NetSurf, ...) are placed INSIDE the ramdisk
#      (ramdisk.igz) under /extras — no shared disk, no mounting, and / stays
#      clean so the desktop icons are not cluttered (mc itself is /bin/mc).
#   7. starts a HEADLESS (no window) control VM and verifies:
#        - /extras/tcc_test (program compiled by the cross tcc),
#        - tcc inside ToaruOS compiles and runs /extras/tcc_hello.c,
#        - every example in /extras compiles and runs,
#        - /extras/gcc_hello (built by the HOST gcc, see gcc-toaru.sh) runs,
#        - mini-commander starts, draws its panels with a highlighted current
#          item, executes its command line on Enter and quits with F10 or a
#          mouse click on the F10 footer,
#        - NetSurf starts under the compositor, parses and lays out
#          /extras/netsurf-test.html and survives the first redraw,
#   8. starts a VISIBLE ToaruOS in QEMU:
#        - visible boot: closes the welcome tutorial and opens the Terminal
#          (QMP absolute pointer + sendkey; the graphical session is "local"),
#        - headless boot: uses the getty on the serial line as root.
#      The VM is then left running.
#
# Inside the visible ToaruOS open the Terminal and run:
#
#   /extras/tcc/bin/tcc -o /tmp/hello /extras/tcc_hello.c ; /tmp/hello
#   mc /extras
#
# (When the VM is started by hand, pass the custom ISO:
#  ISO=_build/custom.iso ./scripts/toaru-vm.sh start)
#
# Shutdown:  ./scripts/toaru-vm.sh stop
#
# Optional environment variables:
#   VISIBLE=0     no QEMU window (serial line only; automatic check still runs)
#   HEADLESS=1    force headless boot even for the visible VM
#   DEMO=0        skip the automatic control VM
#   WITH_NETSURF=0  skip NetSurf (built on first use; on by default)
# ------------------------------------------------------------

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${REPO_ROOT}/_build"
IMPORT_DIR="${REPO_ROOT}/_import"
IMPORT_DOWNLOAD="${IMPORT_DIR}/download"
DOWNLOAD="${IMPORT_DOWNLOAD}"
ISO="${DOWNLOAD}/image.iso"
TOARU_SRC="${DOWNLOAD}/toaruos-src"
TCC="${BUILD_DIR}/tcc-toaru/bin/x86_64-toaru-tcc"
TCC_GUEST="${BUILD_DIR}/tcc-toaru/tcc-guest"
TEST_BIN="${BUILD_DIR}/tcc-toaru/test"
SYSROOT="${BUILD_DIR}/tcc-toaru/lib"
MC_SRC="${BUILD_DIR}/minicmd/mini-commander"
MC_IMPORT="${IMPORT_DIR}/mini-commander"
MC_BIN="${REPO_ROOT}/_output/mc"
NUKLEAR="${REPO_ROOT}/_import/nuklear-ide/nuklear.h"
NETSURF_BUILD="${REPO_ROOT}/netsurf-toaru/build.sh"
NETSURF_DL="${BUILD_DIR}/netsurf"
NETSURF_CACHE="${IMPORT_DOWNLOAD}"
NETSURF_BIN="${NETSURF_DL}/netsurf-3.11/netsurf"
PUSH="${BUILD_DIR}/push"
VM="${REPO_ROOT}/scripts/toaru-vm.sh"

# If cached imports exist and the build tree is gone, seed it from _import.
# (Never copy over an existing tree: git's pack files are read-only, so a
# repeated cp would fail.)
if [[ -d "${IMPORT_DIR}/mini-commander" && ! -d "${BUILD_DIR}/minicmd/mini-commander" ]]; then
    mkdir -p "${BUILD_DIR}/minicmd"
    cp -a "${IMPORT_DIR}/mini-commander" "${BUILD_DIR}/minicmd/"
fi

VISIBLE=${VISIBLE:-1}
HEADLESS=${HEADLESS:-0}   # 1 = headless boot even when a QEMU window is open
DEMO=${DEMO:-1}
WITH_NETSURF=${WITH_NETSURF:-1}
BOOT_ISO=""

die() { echo "error: $*" >&2; exit 1; }
. "$REPO_ROOT/scripts/need.sh"   # need(): dependency check with package-install hints

# the control VM must not stay running if the script crashes
VM_TEST_STARTED=0
cleanup() {
    if [[ ${VM_TEST_STARTED} == 1 ]]; then
        "${VM}" stop > /dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

need curl
need git
need qemu-system-x86_64
need python3
need gcc
need isoinfo
need genisoimage
need mkfs.fat
need mmd
need mcopy

# ---------------------------------------------------------------- 1) ISO
mkdir -p "${DOWNLOAD}"
if [[ ! -f "${ISO}" ]]; then
    echo "== downloading ToaruOS 2.3.2 ISO ..."
    # Try primary URL, fallback if it fails
    ISO_URL="https://github.com/klange/toaruos/releases/download/v2.3.2/image.iso"
    FALLBACK_URL="https://github.com/klange/toaruos/releases/latest/download/image.iso"
    if ! curl -L --fail -o "${ISO}.part" "$ISO_URL"; then
        echo "Primary ISO URL failed, trying fallback..."
        if ! curl -L --fail -o "${ISO}.part" "$FALLBACK_URL"; then
            die "Failed to download ToaruOS ISO from both URLs"
        fi
    fi
    mv "${ISO}.part" "${ISO}"
fi

# ------------------------------------------------------- 2) ToaruOS sources
if [[ ! -d "${TOARU_SRC}" ]]; then
    echo "== downloading ToaruOS sources (v2.3.2) ..."
    git clone --depth 1 --branch v2.3.2 \
        https://github.com/klange/toaruos.git "${TOARU_SRC}" \
        || die "git clone ToaruOS"
fi

# ------------------------------------- 3) TinyCC: cross + tcc for ToaruOS
if [[ ! -x "${TCC}" || ! -f "${TCC_GUEST}" || ! -f "${SYSROOT}/libc.a" ]]; then
    echo "== building TinyCC (cross and for running inside ToaruOS) ..."
    "${REPO_ROOT}/scripts/mktcc.sh"
fi
[[ -x "${TCC}" ]]     || die "missing ${TCC} (mktcc.sh failed)"
[[ -f "${TCC_GUEST}" ]] || die "missing ${TCC_GUEST} (mktcc.sh failed)"
[[ -f "${TEST_BIN}" ]]  || die "missing ${TEST_BIN} (mktcc.sh failed)"

# ------------------------------------------------------ 4) mini-commander
if [[ ! -d "${MC_SRC}" ]]; then
    if [[ ! -d "${MC_IMPORT}" ]]; then
        echo "== downloading mini-commander ..."
        mkdir -p "${IMPORT_DIR}"
        git clone --depth 1 https://github.com/Tomas-M/mini-commander.git "${MC_IMPORT}" \
            || die "git clone mini-commander"
    fi
    mkdir -p "$(dirname "${MC_SRC}")"
    cp -a "${MC_IMPORT}" "${MC_SRC}"
fi
if [[ ! -x "${MC_BIN}" ]]; then
    echo "== building mini-commander ..."
    "${REPO_ROOT}/mc-toaru/build.sh" mc
fi

# ---------------------------------------------------------------- 5) Nuklear
if [[ ! -s "${NUKLEAR}" ]]; then
    echo "== fetching the Nuklear headers (pinned commit, same as the IDE) ..."
    "${REPO_ROOT}/nuklear-ide/fetch-nuklear.sh" || die "fetch-nuklear.sh"
fi

# -------------------------------------------- 6) ramdisk contents
# Everything the guest needs is collected in _build/push/ and copied INTO the
# ramdisk (ramdisk.igz) by mc-toaru/mkiso.sh — the guest sees the files
# directly on /; there is no shared disk and nothing is mounted.
echo "== preparing the ramdisk payload ..."
rm -rf "${PUSH}"
mkdir -p "${PUSH}/tcc/bin" "${PUSH}/tcc/lib" "${PUSH}/toaruos-src"

# user files (the user/ directory in the project root) have precedence
if [[ -d "${REPO_ROOT}/user" ]]; then
    cp -a "${REPO_ROOT}/user/." "${PUSH}/"
fi

install -m 755 "${MC_BIN}"     "${PUSH}/mc"
install -m 755 "${TCC_GUEST}"  "${PUSH}/tcc/bin/tcc"
install -m 755 "${TEST_BIN}"   "${PUSH}/tcc_test"
install -m 644 "${SYSROOT}/crt0.o" "${SYSROOT}/crti.o" "${SYSROOT}/crtn.o" \
               "${SYSROOT}/libc.a" "${SYSROOT}/libtcc1.a" "${PUSH}/tcc/lib/"
cp -a "${SYSROOT}/include" "${PUSH}/tcc/include"

# example source texts + the tcc_test source text
cp "${REPO_ROOT}/tests/examples/"*.c "${PUSH}/"
cp "${REPO_ROOT}/tcc/toaru/test.c" "${PUSH}/tcc_test.c"

# A program built on the HOST by the host gcc — the route a developer would
# use for their own C (gcc + GNU ld + toaru-elfpad.py, see gcc-toaru.sh).
# TinyCC is not involved; the control VM below checks that it really runs.
GCC_TEST="${PUSH}/gcc_hello"
if command -v gcc > /dev/null 2>&1 && command -v ld > /dev/null 2>&1; then
    "${REPO_ROOT}/scripts/gcc-toaru.sh" -o "${GCC_TEST}" "${REPO_ROOT}/tests/gcc/hello.c" \
        || die "gcc-toaru.sh failed"
else
    echo "note: gcc/ld not found — /gcc_hello will not be built"
fi

# Nuklear (for browsing; the old terminal backend is not part of the image)
cp "${NUKLEAR}" "${PUSH}/nuklear.h"

# Working copy of the ToaruOS sources: the IDE and NetSurf builds patch
# lib/graphics.c for static sprite decoding, so they must not touch the
# pristine clone in _import/download — they build against this copy instead.
WORK_SRC="${BUILD_DIR}/toaruos-src"
if [[ -f "${TOARU_SRC}/lib/yutani.c" && ! -f "${WORK_SRC}/lib/yutani.c" ]]; then
    echo "== copying ToaruOS sources for the IDE/NetSurf build ..."
    mkdir -p "${WORK_SRC}"
    cp -a "${TOARU_SRC}/." "${WORK_SRC}/"
fi

# Native Nuklear IDE for ToaruOS (Yutani + software renderer, statically linked)
IDE_TOARU_BUILD="${REPO_ROOT}/nuklear-ide/toaru/build.sh"
if [[ -x "${IDE_TOARU_BUILD}" && -f "${WORK_SRC}/lib/yutani.c" && -f "${SYSROOT}/libc.a" ]]; then
    echo "== building Nuklear IDE for ToaruOS ..."
    TOARU_SRC="${WORK_SRC}" "${IDE_TOARU_BUILD}" "${PUSH}/nuklear-ide" \
        || die "Nuklear IDE for ToaruOS"
else
    echo "note: Nuklear IDE for ToaruOS not built (missing sysroot or ToaruOS sources)"
fi

# NetSurf for ToaruOS (static Yutani browser).  The build cross-compiles
# NetSurf and its support libraries and is expensive, so reuse an existing
# binary; delete it (or run netsurf-toaru/build.sh) to force a rebuild.
if [[ "${WITH_NETSURF}" == 1 ]]; then
    if [[ ! -x "${NETSURF_BIN}" && -x "${NETSURF_BUILD}" &&
          -f "${WORK_SRC}/lib/yutani.c" && -f "${SYSROOT}/libc.a" ]]; then
        echo "== building NetSurf for ToaruOS (this takes a while) ..."
        NETSURF_DL="${NETSURF_DL}" NETSURF_CACHE="${NETSURF_CACHE}" \
            TOARU_SRC="${WORK_SRC}" \
            "${NETSURF_BUILD}" || die "NetSurf for ToaruOS"
    fi
    if [[ -x "${NETSURF_BIN}" ]]; then
        NETSURF_SRC="${NETSURF_DL}/netsurf-3.11"
        echo "== preparing NetSurf for the ramdisk ..."
        mkdir -p "${PUSH}/netsurf"
        install -m 755 "${NETSURF_BIN}" "${PUSH}/netsurf/netsurf"
        cp -a "${NETSURF_SRC}/resources/." "${PUSH}/netsurf/"
        install -m 644 "${NETSURF_SRC}/frontends/toaru/res/en/Messages" \
            "${PUSH}/netsurf/Messages"
        cat > "${PUSH}/netsurf-test.html" <<'GUEST_EOF'
<!DOCTYPE html>
<html><head><title>NetSurf on ToaruOS</title></head>
<body style="background:#ffffff;color:#102040;font-family:sans-serif">
<h1>Hello from NetSurf!</h1>
<p>This page is rendered by NetSurf 3.11 as a static ToaruOS executable.</p>
<ul><li>HTML parsing</li><li>CSS layout</li><li>Yutani window</ul>
<p style="color:#a00000">If you can read this, the port works.</p>
</body></html>
GUEST_EOF
    else
        echo "note: NetSurf for ToaruOS not built — /extras/netsurf will be missing"
    fi
fi

# ToaruOS sources without .git, so they can be browsed in mc in the guest
( cd "${TOARU_SRC}" && tar cf - --exclude=.git . ) \
    | ( cd "${PUSH}/toaruos-src" && tar xf - )

# script for compiling the examples directly in ToaruOS
# ToaruOS's esh has no for loop and splits commands at ';', so the file list
# is generated here instead of using a loop in the guest script.
{
    cat <<'GUEST_EOF'
#!/bin/sh
# Compiles and runs all *.c in /extras.  Output goes to /tmp so that
# nothing is written to the ramdisk.
#
#   /extras/compile_examples.sh
#
# All sources in /extras are plain C and TinyCC compiles and runs them;
# tcc_test.c is the test program from tcc/toaru.
if not stat -q /extras/tcc/bin/tcc then echo "tcc is missing from the ramdisk"
if not stat -q /extras/tcc/bin/tcc then exit 1

GUEST_EOF
    for src in "${PUSH}"/*.c; do
        name=$(basename "$src" .c)
        printf 'echo "== /extras/%s.c"\n' "$name"
        printf 'if stat -q "/tmp/%s" then rm "/tmp/%s"\n' "$name" "$name"
        printf '/extras/tcc/bin/tcc -o "/tmp/%s" "/extras/%s.c"\n' "$name" "$name"
        printf 'if stat -q "/tmp/%s" then "/tmp/%s"\n' "$name" "$name"
        printf 'if not stat -q "/tmp/%s" then echo "   tcc failed on /extras/%s.c"\n\n' "$name" "$name"
    done
} > "${PUSH}/compile_examples.sh"
chmod +x "${PUSH}/compile_examples.sh"

cat > "${PUSH}/README.txt" <<'GUEST_EOF'
ToaruOS + TinyCC + mini-commander
=================================

All files are part of the ramdisk under /extras — no disk needs to be
mounted and / stays clean for the desktop icons.

TinyCC inside ToaruOS:

    /extras/tcc/bin/tcc -o /tmp/hello /extras/tcc_hello.c ; /tmp/hello
    /extras/compile_examples.sh

mini-commander:

    mc /            (also /bin/mc /)

Nuklear IDE for ToaruOS (Yutani window, static binary):

    /extras/nuklear-ide

The IDE starts with a writable demo in /tmp/nuklear-ide-demo; F5 compiles
and runs it with /extras/tcc.  Use File > Open or pass another directory,
for example: /extras/nuklear-ide /extras

NetSurf (Yutani window, static binary; file:// and plain http:// pages):

    /extras/netsurf/netsurf file:///extras/netsurf-test.html
    /extras/netsurf/netsurf http://info.cern.ch/

In the graphical session DISPLAY is already set.  On the serial getty
(headless boot) start the compositor first:

    compositor &
    export DISPLAY=compositor

DISPLAY must be set before NetSurf starts — the fonts are fetched from the
compositor's shared-memory font service.  Resources are in /extras/netsurf.
The QEMU user-mode network gives the guest DHCP and DNS, so http:// works;
https:// does not, because ToaruOS has no TLS stack.

/extras/tcc contains the whole sysroot (include/, lib/crt0.o, libc.a,
libtcc1.a), /extras/toaruos-src contains the sources of ToaruOS 2.3.2.
/extras/gcc_hello was built on the host by the host gcc (gcc-toaru.sh),
not by TinyCC: same ToaruOS libc, different compiler.
GUEST_EOF

# a tiny directory with a predictable layout, used by the automatic mc
# viewer/editor check (one Down arrow lands on README.txt)
mkdir -p "${PUSH}/demo"
cp "${PUSH}/README.txt" "${PUSH}/demo/README.txt"

# Build the bootable ISO: every file from PUSH goes into the ramdisk, and the
# patched mc is additionally installed as /bin/mc so it is on PATH.
echo "== building ramdisk ISO ..."
BOOT_ISO="${BUILD_DIR}/custom.iso"
"${REPO_ROOT}/mc-toaru/mkiso.sh" "${BOOT_ISO}" || die "mkiso.sh failed"

# ------------------------------------- 7) automatic check in a headless VM
demo() {
    local rc=0 text
    echo "== starting the control ToaruOS ..."
    VISIBLE=0 HEADLESS=1 ISO="${BOOT_ISO}" "${VM}" start > /dev/null
    VM_TEST_STARTED=1

    local i
    for i in $(seq 1 60); do
        if "${VM}" log 2000 2>/dev/null | grep -aq 'root@livecd'; then break; fi
        sleep 1
    done
    "${VM}" log 2000 2>/dev/null | grep -aq 'root@livecd' \
        || { echo "  *** guest did not boot within 60 s"; rc=1; return $rc; }

    wait_for() {   # wait_for <marker> - an exact line in the log
        local j
        for j in $(seq 1 40); do
            "${VM}" log 2000 2>/dev/null | grep -qx "$1" && return 0
            sleep 1
        done
        return 1
    }

    at_shell() {   # at_shell <marker> - did a shell prompt follow the marker?
        awk -v m="$1" '$0 == m { seen = 1; next }
            seen && /root@livecd/ { found = 1 }
            END { exit !found }'
    }

    # send_raw writes bytes without the trailing NL that "send" adds; needed for
    # CR (Enter) and for escape sequences whose trailing NL would be read as Enter.
    send_raw() { printf '%s' "$1" > "${BUILD_DIR}/work/in"; }

    "${VM}" send '/extras/tcc_test; echo __T1__'
    wait_for __T1__ || { echo "  *** /extras/tcc_test did not finish"; rc=1; return $rc; }

    # esh does not handle && (only ;), hence two commands in a row
    "${VM}" send '/extras/tcc/bin/tcc -o /tmp/tcc_hello /extras/tcc_hello.c ; /tmp/tcc_hello; echo __T2__'
    wait_for __T2__ || { echo "  *** tcc in ToaruOS did not compile tcc_hello.c"; rc=1; return $rc; }

    # The same C library, but the program was compiled by the HOST gcc.
    if [[ -f "${GCC_TEST}" ]]; then
        "${VM}" send '/extras/gcc_hello; echo __T4__'
        wait_for __T4__ || { echo "  *** /extras/gcc_hello (host gcc) did not finish"; rc=1; return $rc; }
    fi

    "${VM}" send '/extras/compile_examples.sh; echo __T3__'
    wait_for __T3__ || { echo "  *** compile_examples.sh did not finish"; rc=1; return $rc; }

    # mc exits with F10; we send the escape sequences directly (F9 = ESC[20~,
    # F10 = ESC[21~).  If the parser loses the CSI prefix, "20~"/"21~" ends up
    # on the terminal as text (that was a real bug), so check for both.
    # Everything lives in /extras; /extras/demo has a predictable layout so the
    # one Down arrow below always lands on README.txt.
    local mc_cmd='/bin/mc /extras/demo'
    "${VM}" send "$mc_cmd"
    sleep 3

    # Enter arrives from the terminal as CR; the ncurses shim has to translate
    # it to NL, otherwise mc ignores it (the command line does nothing).
    send_raw 'echo __MC_ENTER__'
    sleep 1
    send_raw "$(printf '\r')"
    local en
    for en in $(seq 1 10); do
        "${VM}" log 2000 2>/dev/null | grep -qx '__MC_ENTER__' && break
        sleep 1
    done
    if ! "${VM}" log 2000 2>/dev/null | grep -qx '__MC_ENTER__'; then
        echo "  *** Enter did not execute the mini-commander command line"; rc=1
    fi
    # The output stays on the normal screen (not the panels) until F9.
    if ! "${VM}" log 4000 2>/dev/null | grep -aq '\[F9\] return to mini-commander'; then
        echo "  *** the command output did not wait for F9"; rc=1
    fi
    send_raw "$(printf '\033[20~')"   # F9 - back to the panels
    sleep 1

    # F3 (viewer) and F4 (editor) used to crash: view_edit.c called mmap(),
    # which ToaruOS does not implement, and then parsed the NULL pointer.
    # Move down to README.txt, open both, and quit each with F10.
    local dn
    for dn in 1; do
        send_raw "$(printf '\033[B')"
        sleep 0.3
    done
    send_raw "$(printf '\033OR')"   # F3 - viewer
    sleep 2
    send_raw "$(printf '\033[21~')" # F10 - quit the viewer
    sleep 1
    send_raw "$(printf '\033OS')"   # F4 - editor
    sleep 2
    send_raw "$(printf '\033[21~')" # F10 - quit the (unmodified) editor
    sleep 1

    send_raw "$(printf '\033[20~')"
    sleep 1
    send_raw "$(printf '\033[21~')"
    sleep 1
    "${VM}" send 'echo __MC_QUIT__'
    wait_for __MC_QUIT__ || { echo "  *** mini-commander did not exit on F10"; rc=1; return $rc; }
    local t
    t=$("${VM}" log 100000 2>/dev/null || true)
    if ! at_shell __MC_QUIT__ <<< "$t"; then
        echo "  *** F10 did not return mini-commander to the shell"; rc=1; return $rc
    fi

    # A mouse click on the F10 footer must work too.  The getty terminal is
    # 80x24 and SGR coordinates are 1-based, so (75,24) is the last footer slot.
    "${VM}" send "$mc_cmd"
    sleep 3
    send_raw "$(printf '\033[<0;75;24M\033[<0;75;24m')"
    sleep 2
    "${VM}" send 'echo __MC_MOUSE__'
    local j
    for j in $(seq 1 10); do
        "${VM}" log 2000 2>/dev/null | grep -qx '__MC_MOUSE__' && break
        sleep 1
    done

    text=$("${VM}" log 100000 2>/dev/null || true)
    if ! at_shell __MC_MOUSE__ <<< "$text"; then
        echo "  *** mouse click on the F10 footer did not quit mini-commander"; rc=1
        send_raw "$(printf '\033[21~')"   # do not leave mc running
        sleep 1
    fi
    if grep -aqE '(20|21)~|<[0-9]+;[0-9]+;[0-9]+[Mm]' <<< "$text"; then
        echo "  *** a key or mouse sequence leaked into the terminal as text"; rc=1
    fi
    if ! grep -aq 'ctor=1' <<< "$text"; then
        echo "  *** tcc_test did not print ctor=1"; rc=1
    fi
    if ! grep -aq 'TinyCC inside ToaruOS' <<< "$text"; then
        echo "  *** program compiled by tcc in ToaruOS did not print the expected text"; rc=1
    fi
    if ! grep -aq '1+2+...+10 = 55' <<< "$text"; then
        echo "  *** tcc_hello did not print the correct sum"; rc=1
    fi
    if grep -aq 'tcc failed on' <<< "$text"; then
        echo "  *** tcc inside ToaruOS failed to compile an example"; rc=1
    fi
    if [[ -f "${GCC_TEST}" ]] && ! grep -aq 'gcc_sum: 15' <<< "$text"; then
        echo "  *** the program built by the host gcc did not print its sum"; rc=1
    fi
    # six examples end with O.K.; btree just prints its keys (last one is 800)
    local ok_count btree_out
    ok_count=$(grep -ac '^O\.K\.$' <<< "$text" || true)
    if [[ ${ok_count:-0} -lt 6 ]]; then
        echo "  *** only ${ok_count:-0} of the 6 examples printed O.K."; rc=1
    fi
    btree_out=$(sed -n '/^== \/extras\/btree\.c$/,/^== \/extras\/heapsort\.c$/p' <<< "$text")
    if ! grep -aqx '800' <<< "$btree_out"; then
        echo "  *** btree did not print its last key (800)"; rc=1
    fi
    # The viewer draws its status bar as "<file> ... <last>/<lines> <pct>%"
    # (the editor instead has "L:[...]" and "U+...").  Do not look for the file
    # text itself: the screen is updated cell by cell, so a space that is
    # already on the screen is not sent to the terminal again and the log would
    # show "TinyCC+ mini-commander".
    if ! grep -aqE '/demo/README\.txt.*[0-9]+/ *[0-9]+ +[0-9]+%' <<< "$text"; then
        echo "  *** the F3 viewer did not open README.txt"; rc=1
    fi
    if ! grep -aq -- '-- /extras/demo --' <<< "$text"; then
        echo "  *** mini-commander did not draw the file panels"; rc=1
    fi
    if ! grep -aq 'U+0054' <<< "$text"; then
        echo "  *** the F4 editor did not open README.txt"; rc=1
    fi
    if grep -aq 'Segmentation fault' <<< "$text"; then
        echo "  *** something in the guest crashed with Segmentation fault"; rc=1
    fi
    if ! grep -aq 'Quit' <<< "$text"; then
        echo "  *** mini-commander did not draw (the Quit hint is missing)"; rc=1
    fi
    # The current item keeps the original Tomas-M colors: black on cyan
    # (SGR 30;46).  If wattron() does not store the color pair, every row
    # keeps the plain panel background.
    if ! grep -aqF $'\033[0;30;46m' "${BUILD_DIR}/work/out.log" 2>/dev/null; then
        echo "  *** the current panel item is not highlighted"; rc=1
    fi
    # The F9 hint is printed with a blue background (SGR 0;97;44).
    if ! grep -aqF $'\033[0;97;44m [F9] return to mini-commander' "${BUILD_DIR}/work/out.log" 2>/dev/null; then
        echo "  *** the F9 hint does not have the blue background"; rc=1
    fi

    # NetSurf: start the compositor (the serial getty has no graphical
    # session), then load the test page with verbose logging.  Parsing, DOM
    # and box-tree conversion show up in the log; the process still being
    # alive afterwards proves that layout and the first redraw did not crash
    # (the layout itself has no log message after it finishes).
    if [[ -x "${PUSH}/netsurf/netsurf" ]]; then
        "${VM}" send 'compositor &'
        local ns_ready=0
        for ns_i in $(seq 1 20); do
            if "${VM}" log 4000 2>/dev/null | grep -aq 'Loading wallpaper'; then
                ns_ready=1; break
            fi
            sleep 1
        done
        if [[ ${ns_ready} != 1 ]]; then
            echo "  *** the compositor did not start"; rc=1
        else
            "${VM}" send 'export DISPLAY=compositor'
            "${VM}" send '/extras/netsurf/netsurf -v file:///extras/netsurf-test.html &'
            local ns_parsed=0
            for ns_i in $(seq 1 30); do
                if "${VM}" log 300000 2>/dev/null |
                        grep -aq 'DOM to box conversion complete'; then
                    ns_parsed=1; break
                fi
                sleep 1
            done
            if [[ ${ns_parsed} != 1 ]]; then
                echo "  *** NetSurf did not parse the test page"; rc=1
            else
                "${VM}" send 'ps'
                local ns_alive=0
                for ns_i in $(seq 1 10); do
                    if "${VM}" log 300000 2>/dev/null |
                            grep -aqE '[0-9]+ +netsurf'; then
                        ns_alive=1; break
                    fi
                    sleep 1
                done
                if [[ ${ns_alive} != 1 ]]; then
                    echo "  *** NetSurf crashed during layout or redraw"; rc=1
                fi
            fi
        fi
    fi

    "${VM}" stop > /dev/null 2>&1 || true
    VM_TEST_STARTED=0

    if [[ $rc == 0 ]]; then
        local extra=
        [[ -f "${GCC_TEST}" ]] && extra=", the program built by the host gcc"
        echo "== check OK: tcc_test, tcc inside ToaruOS, all examples${extra} and mini-commander work"
    else
        echo "== check FOUND A PROBLEM (see above)"
    fi
    return $rc
}

if [[ "${DEMO}" == 1 ]]; then
    if ! demo; then
        echo "Automatic check failed; the ISO ${BOOT_ISO} is ready anyway." >&2
        exit 1
    fi
fi

# --------------------------------------- 8) visible ToaruOS for the user
if [[ "${VISIBLE}" == 1 && -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "warning: no DISPLAY - cannot open a QEMU window, using the serial line" >&2
    VISIBLE=0
fi

if [[ "${VISIBLE}" == 1 ]]; then
    echo "== starting visible ToaruOS in QEMU (the window opens in ~15 s) ..."
    VISIBLE=1 HEADLESS="${HEADLESS}" ISO="${BOOT_ISO}" "${VM}" start
else
    echo "== starting ToaruOS without a window (serial line) ..."
    VISIBLE=0 HEADLESS=1 ISO="${BOOT_ISO}" "${VM}" start
fi

# A graphical boot does not run the serial getty: 99_runstart.sh starts yutani
# and a session for the user "local".  The only way in is to type into the
# Terminal window.  QEMU's sendkey injects keys into the guest keyboard and the
# compositor gives them to its focused window; bringing the QEMU window to the
# front with wmctrl/xdotool is best effort and keeps the host from stealing
# focus.
focus_qemu() {
    local qpid wid i
    qpid=$(cat "${BUILD_DIR}/work/qemu.pid" 2>/dev/null || true)
    for i in $(seq 1 20); do
        if command -v wmctrl > /dev/null 2>&1; then
            wid=$(wmctrl -lp 2>/dev/null | awk -v p="${qpid:-0}" '$3 == p {print $1; exit}')
            if [[ -n ${wid} ]]; then
                wmctrl -i -a "${wid}" > /dev/null 2>&1 && return 0
            fi
            # fall back to the window title; useful if _NET_WM_PID is missing
            wmctrl -a QEMU > /dev/null 2>&1 && return 0
        elif command -v xdotool > /dev/null 2>&1 && [[ -n ${qpid} ]]; then
            xdotool search --pid "${qpid}" windowactivate > /dev/null 2>&1 && return 0
        fi
        sleep 0.5
    done
    return 1
}

type_in_guest() {   # type_in_guest <text> [delay] — uses the QEMU monitor's sendkey
    local text=$1 delay=${2:-0.1} i ch key
    for ((i = 0; i < ${#text}; i++)); do
        ch=${text:i:1}
        case ${ch} in
            ' ') key=spc ;;
            '/') key=slash ;;
            '-') key=minus ;;
            '.') key=dot ;;
            ',') key=comma ;;
            '_') key=shift-minus ;;
            ';') key=semicolon ;;
            '=') key=equal ;;
            '&') key=shift-7 ;;
            '|') key=shift-backslash ;;
            '>') key=shift-dot ;;
            '<') key=shift-comma ;;
            '*') key=shift-8 ;;
            "'") key=apostrophe ;;
            [A-Z]) key="shift-${ch,,}" ;;   # HMP key names are lower case
            *)   key=${ch} ;;
        esac
        "${VM}" mon "sendkey ${key}" > /dev/null 2>&1 || return 1
        sleep "${delay}"
    done
    "${VM}" mon 'sendkey ret' > /dev/null 2>&1 || return 1
    sleep 0.5
    return 0
}

press_key() {       # press_key <QEMU key name>
    "${VM}" mon "sendkey $1" > /dev/null 2>&1
}

# The graphical session has no serial getty, so the only way in is the Terminal
# window.  QEMU's HMP mouse_move does nothing with the absolute vmmouse device;
# QMP input-send-event works.  Its absolute axes are always 0..32767, so guest
# pixel coordinates from a screendump have to be scaled.
guest_screen_size() {   # prints "WIDTH HEIGHT" of the guest display
    local ppm="${BUILD_DIR}/work/screen.ppm" i
    rm -f "$ppm"
    "${VM}" mon "screendump ${ppm}" > /dev/null 2>&1 || return 1
    for i in $(seq 1 20); do
        [[ -s "$ppm" ]] && break
        sleep 0.1
    done
    [[ -s "$ppm" ]] || return 1
    python3 -I - "$ppm" <<'PY'
import sys
with open(sys.argv[1], "rb") as f:
    if f.readline().strip() != b"P6":
        sys.exit(1)
    w, h = map(int, f.readline().split())
print(w, h)
PY
}

qmp_abs() {   # qmp_abs <x> <y> <width> <height> — move the absolute pointer
    local x=$1 y=$2 w=$3 h=$4 ax ay
    (( w > 0 && h > 0 )) || return 1
    ax=$(( (x * 32767 + w / 2) / w ))
    ay=$(( (y * 32767 + h / 2) / h ))
    "${VM}" qmp "$(printf '{"execute":"input-send-event","arguments":{"events":[{"type":"abs","data":{"axis":"x","value":%d}},{"type":"abs","data":{"axis":"y","value":%d}}]}}' "${ax}" "${ay}")" > /dev/null
}

qmp_btn() {   # qmp_btn <down|up>
    local down=false
    [[ $1 == down ]] && down=true
    "${VM}" qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":${down},\"button\":\"left\"}}]}}" > /dev/null
}

qmp_click() {   # qmp_click <x> <y> <width> <height>
    qmp_abs "$1" "$2" "$3" "$4" || return 1
    sleep 0.05
    qmp_btn down || return 1
    sleep 0.05
    qmp_btn up || return 1
}

qmp_double_click() {
    qmp_click "$@" || return 1
    sleep 0.12
    qmp_click "$@" || return 1
}

# There is nothing to mount: every file travels inside ramdisk.igz and the
# guest sees it directly on /.

MC_NOTE="All payload files live in /extras inside the ramdisk (mc is /bin/mc) — no disk is mounted and / stays clean for the desktop icons."
START_HINT="ISO=${BOOT_ISO} ./scripts/toaru-vm.sh start"

cat <<EOF

Done. ToaruOS is running (log: ${BUILD_DIR}/work/out.log).
${MC_NOTE}

In the ToaruOS window open the Terminal and run:

    /extras/nuklear-ide
    /extras/tcc/bin/tcc -o /tmp/hello /extras/tcc_hello.c ; /tmp/hello
    mc /extras     (mc is also installed as /bin/mc)

If you start the VM later by hand, pass the custom ISO:

    ${START_HINT}

Shutting down the VM:  ./scripts/toaru-vm.sh stop
Serial line (only with HEADLESS=1):  ./scripts/toaru-vm.sh send 'command'
EOF
