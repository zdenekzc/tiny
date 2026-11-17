#!/usr/bin/env bash
# ------------------------------------------------------------
# "One-click" run of ToaruOS 2.3.2 + TinyCC + mini-commander (light variant):
#
#   ./tests/run-toaru.sh
#
# Like tests/build-toaru.sh this script never builds ToaruOS itself, and it
# uses the same delivery mechanism: ALL payload files (tcc, mc, examples,
# sources) travel INSIDE the ramdisk (ramdisk.igz) under /extras — no shared
# disk, no mounting, and / stays clean for the desktop icons (mc is /bin/mc).
# The kernel and the repacked ramdisk of the stock release ISO are packed into
# a custom ISO (mc-toaru/mkiso.sh), which also emits _build/ramdisk.tgz and
# _build/kernel.mb for booting without the ISO:
#
#   INITRD=_build/ramdisk.tgz ./scripts/toaru-vm.sh start
#
# The difference from tests/build-toaru.sh: no Nuklear IDE and no NetSurf.
#
# What the script does (everything can be deleted and run again at any time):
#   1. downloads the ToaruOS 2.3.2 ISO into _import/download/image.iso
#      (the same place tests/build-toaru.sh and toaru-vm.sh look, so the
#      download is shared; nothing is compiled from the ToaruOS sources),
#   2. downloads the ToaruOS sources (tag v2.3.2, just for browsing in the
#      guest) and the Nuklear header (browsing only),
#   3. downloads TinyCC and builds three things:
#        - a cross compiler on the host (_build/tcc-toaru/bin/x86_64-toaru-tcc),
#        - a tcc that runs INSIDE ToaruOS with the default path /extras/tcc
#          (_build/tcc-toaru/tcc-guest),
#   4. downloads mini-commander and compiles it with the cross tcc (_output/mc),
#   5. prepares the ramdisk payload with:
#        /extras/mc, /extras/tcc/{bin/tcc,include,lib}, /extras/tcc_test,
#        /extras/tests/examples/*.c, /extras/toaruos-src, compile_examples.sh
#        and installs mc additionally as /bin/mc inside the ramdisk,
#   6. makes _build/custom.iso (ramdisk embedded) plus _build/ramdisk.tgz
#      and _build/kernel.mb,
#   7. starts a HEADLESS (no window) control VM and verifies:
#        - /extras/tcc_test (program compiled by the cross tcc),
#        - tcc inside ToaruOS compiles and runs /extras/tcc_hello.c,
#        - every example compiles and runs (/extras/compile_examples.sh),
#        - mini-commander starts, draws its panels and quits with F10,
#   8. starts a VISIBLE ToaruOS in QEMU and leaves it running.
#
# Inside the visible ToaruOS open the Terminal and run:
#
#   /extras/tcc/bin/tcc -o /tmp/hello /extras/tcc_hello.c ; /tmp/hello
#   mc /extras     (also /bin/mc)
#
# (When the VM is started by hand, pass the custom ISO or the initrd:
#  ISO=_build/custom.iso ./scripts/toaru-vm.sh start
#  INITRD=_build/ramdisk.tgz ./scripts/toaru-vm.sh start)
#
# Shutdown:  ./scripts/toaru-vm.sh stop
#
# Optional environment variables:
#   VISIBLE=0     no QEMU window (serial line only; automatic check still runs)
#   HEADLESS=1    force headless boot even for the visible VM
#   DEMO=0        skip the automatic control VM
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
MC_BIN="${REPO_ROOT}/_output/mc"
NUKLEAR="${REPO_ROOT}/_import/nuklear-ide/nuklear.h"
PUSH="${BUILD_DIR}/push"
VM="${REPO_ROOT}/scripts/toaru-vm.sh"

VISIBLE=${VISIBLE:-1}
DEMO=${DEMO:-1}

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
need 7z
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
    # The primary URL serves the release asset; the fallback covers the
    # "latest" redirect (verified 2026-11-17).
    ISO_URL="https://github.com/klange/toaruos/releases/download/v2.3.2/image.iso"
    FALLBACK_URL="https://github.com/klange/toaruos/releases/latest/download/image.iso"
    ok=0
    for url in "${ISO_URL}" "${FALLBACK_URL}"; do
        if curl -L --fail -o "${ISO}.part" "${url}"; then ok=1; break; fi
        echo "  ... ${url} failed, trying the next one"
    done
    [[ ${ok} == 1 ]] || die "failed to download the ToaruOS ISO from all URLs"
    mv "${ISO}.part" "${ISO}"
fi
# mktcc.sh / mklibc.sh take libc, crt*.o and the headers from this ISO.
[[ -f "${ISO}" ]] || die "missing ISO: ${ISO}"

# ------------------------------------------------------- 2) ToaruOS sources
if [[ ! -d "${TOARU_SRC}" ]]; then
    echo "== downloading ToaruOS sources (v2.3.2) ..."
    git clone --depth 1 --branch v2.3.2 \
        https://github.com/klange/toaruos.git "${TOARU_SRC}" \
        || die "git clone ToaruOS"
fi

# nuklear-ide/toaru/ and netsurf-toaru/ build against a working copy of the
# sources in _build/toaruos-src (they patch lib/graphics.c, so the pristine
# _import clone must stay untouched); seed it like build-toaru.sh does.
if [[ ! -d "${BUILD_DIR}/toaruos-src" ]]; then
    cp -a "${TOARU_SRC}" "${BUILD_DIR}/toaruos-src"
fi

# ------------------------------------- 3) TinyCC: cross + tcc for ToaruOS
if [[ ! -x "${TCC}" || ! -f "${TCC_GUEST}" \
      || ! -f "${SYSROOT}/libc.a" ]]; then
    echo "== building TinyCC (cross and for running inside ToaruOS) ..."
    "${REPO_ROOT}/scripts/mktcc.sh"
fi
[[ -x "${TCC}" ]]             || die "missing ${TCC} (mktcc.sh failed)"
[[ -f "${TCC_GUEST}" ]]       || die "missing ${TCC_GUEST} (mktcc.sh failed)"
[[ -f "${TEST_BIN}" ]]        || die "missing ${TEST_BIN} (mktcc.sh failed)"

# ------------------------------------------------------ 4) mini-commander
if [[ ! -d "${MC_SRC}" ]]; then
    echo "== downloading mini-commander ..."
    mkdir -p "$(dirname "${MC_SRC}")"
    git clone --depth 1 https://github.com/Tomas-M/mini-commander.git "${MC_SRC}" \
        || die "git clone mini-commander"
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

# -------------------------------------------- 6) ramdisk payload + custom ISO
# Every file from PUSH travels inside the ramdisk (ramdisk.igz), see the note
# above; nothing goes to / except mc as /bin/mc.
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

# Nuklear (for browsing only for now; the term backend is not part of the image)
cp "${NUKLEAR}" "${PUSH}/nuklear.h"

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

/extras/tcc contains the whole sysroot (include/, lib/crt0.o, libc.a,
libtcc1.a), /extras/toaruos-src contains the sources of ToaruOS 2.3.2.
GUEST_EOF

# a tiny directory with a predictable layout, used by the automatic mc check
mkdir -p "${PUSH}/demo"
cp "${PUSH}/README.txt" "${PUSH}/demo/README.txt"

# Build the bootable ISO (payload inside the ramdisk); it also emits
# _build/ramdisk.tgz and _build/kernel.mb for the ISO-less boot.
echo "== building ramdisk ISO ..."
BOOT_ISO="${BUILD_DIR}/custom.iso"
"${REPO_ROOT}/mc-toaru/mkiso.sh" "${BOOT_ISO}" || die "mkiso.sh failed"

# ------------------------------------- 7) automatic check in a headless VM
demo() {
    local rc=0 text
    echo "== starting the control (headless) ToaruOS ..."
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

    "${VM}" send '/extras/tcc_test; echo __T1__'
    wait_for __T1__ || { echo "  *** /extras/tcc_test did not finish"; rc=1; return $rc; }

    # esh does not handle && (only ;), hence two commands in a row
    "${VM}" send '/extras/tcc/bin/tcc -o /tmp/tcc_hello /extras/tcc_hello.c ; /tmp/tcc_hello; echo __T2__'
    wait_for __T2__ || { echo "  *** tcc in ToaruOS did not compile tcc_hello.c"; rc=1; return $rc; }

    "${VM}" send '/extras/compile_examples.sh; echo __T3__'
    wait_for __T3__ || { echo "  *** compile_examples.sh did not finish"; rc=1; return $rc; }

    # mc exits with F10; we send its escape sequences directly.  Before
    # quitting, run one command on mc's command line: its output stays on the
    # normal screen until F9 — that is where the [F9] hint is printed.
    send_raw() { printf '%s' "$1" > "${BUILD_DIR}/work/in"; }
    "${VM}" send '/bin/mc /extras/demo'
    sleep 3
    send_raw 'echo __MC_ENTER__'
    sleep 1
    # Enter arrives from the terminal as CR; the ncurses shim translates it
    send_raw "$(printf '\r')"
    local en
    for en in $(seq 1 10); do
        "${VM}" log 2000 2>/dev/null | grep -qx '__MC_ENTER__' && break
        sleep 1
    done
    if ! "${VM}" log 4000 2>/dev/null | grep -aq '\[F9\] return to mini-commander'; then
        echo "  *** the command output did not wait for F9"; rc=1
    fi
    send_raw "$(printf '\033[20~')"   # F9 — back to the panels
    sleep 1
    send_raw "$(printf '\033[21~')"   # F10 — quit mc
    sleep 2

    text=$("${VM}" log 100000 2>/dev/null || true)
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
    if grep -aq 'Segmentation fault' <<< "$text"; then
        echo "  *** something in the guest crashed with Segmentation fault"; rc=1
    fi
    if ! grep -aq 'Quit' <<< "$text"; then
        echo "  *** mini-commander did not draw (the Quit hint is missing)"; rc=1
    fi
    # The F9 hint is printed with a blue background (SGR 0;97;44) so it stands
    # out; the raw log (out.log) carries the escape sequences, `log` strips them.
    if ! grep -aqF $'\033[0;97;44m [F9] return to mini-commander' "${BUILD_DIR}/work/out.log" 2>/dev/null; then
        echo "  *** the F9 hint does not have the blue background"; rc=1
    fi

    "${VM}" stop > /dev/null 2>&1 || true
    VM_TEST_STARTED=0

    if [[ $rc == 0 ]]; then
        echo "== check OK: tcc_test, tcc inside ToaruOS, all examples and mini-commander work"
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
    VISIBLE=1 HEADLESS=0 ISO="${BOOT_ISO}" "${VM}" start
else
    echo "== starting ToaruOS without a window (serial line) ..."
    VISIBLE=0 HEADLESS=1 ISO="${BOOT_ISO}" "${VM}" start
fi

cat <<EOF

Done. ToaruOS is running (log: ${BUILD_DIR}/work/out.log).

All payload files live in /extras inside the ramdisk (mc is /bin/mc) — no
disk is mounted and / stays clean for the desktop icons.

In the ToaruOS window open the Terminal and run:

    /extras/tcc/bin/tcc -o /tmp/hello /extras/tcc_hello.c ; /tmp/hello
    mc /extras     (mc is also installed as /bin/mc)

If you start the VM later by hand, pass the custom ISO or boot without it:

    ISO=${BOOT_ISO} ./scripts/toaru-vm.sh start
    INITRD=${BUILD_DIR}/ramdisk.tgz ./scripts/toaru-vm.sh start

Shutting down the VM:  ./scripts/toaru-vm.sh stop
Serial line (only with HEADLESS=1):  ./scripts/toaru-vm.sh send 'command'
EOF
