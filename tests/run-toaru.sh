#!/usr/bin/env bash
# ------------------------------------------------------------
# "One-click" run of the STOCK ToaruOS 2.3.2 ISO + TinyCC + mini-commander:
#
#   ./tests/run-toaru.sh
#
# Unlike tests/build-toaru.sh this script NEVER builds ToaruOS and does not
# repack the ramdisk: it downloads the official ToaruOS 2.3.2 release ISO
# and boots it as-is; the payload (tcc, mc, examples, sources) travels on a
# separate ext2 disk that the guest mounts as /share.
#
# What the script does (everything can be deleted and run again at any time):
#   1. downloads the ToaruOS 2.3.2 ISO into _import/download/image.iso
#      (the same place tests/build-toaru.sh and toaru-vm.sh look, so the
#      download is shared; nothing is compiled from the ToaruOS sources),
#   2. downloads the ToaruOS sources (tag v2.3.2, just for browsing in the
#      guest) and the Nuklear header (browsing only),
#   3. downloads TinyCC and builds three things:
#        - a cross compiler on the host (_build/tcc-toaru/bin/x86_64-toaru-tcc),
#        - a tcc that runs INSIDE ToaruOS with the default path /share/tcc
#          (_build/tcc-toaru/tcc-guest-share; the /extras/tcc variant
#          tcc-guest for build-toaru.sh is built as well),
#   4. downloads mini-commander and compiles it with the cross tcc (_output/mc),
#   5. prepares the ext2 image _build/share.img with:
#        /mc, /tcc_test, /tcc_test.c, /tcc/{bin/tcc,include,lib},
#        /tests/examples/*.c, /toaruos-src, /compile_examples.sh,
#   6. starts a HEADLESS (no window) control VM and verifies:
#        - /share/tcc_test (program compiled by the cross tcc),
#        - tcc inside ToaruOS compiles and runs /share/tcc_hello.c,
#        - mini-commander starts, draws its panels and quits with F10,
#   7. starts a VISIBLE ToaruOS in QEMU (the stock ISO, unmodified) and
#      leaves it running.
#
# Inside the visible ToaruOS open the Terminal and run:
#
#   mkdir -p /share
#   insmod /mod/ext2.ko
#   mount ext2 /dev/hda,rw /share
#   /share/tcc/bin/tcc -o /tmp/hello /share/tcc_hello.c ; /tmp/hello
#   /share/mc /share
#
# Shutdown:  ./toaru-vm.sh stop
#
# Optional environment variables:
#   VISIBLE=0     no QEMU window (serial line only; automatic check still runs)
#   HEADLESS=1    force headless boot even for the visible VM
#   DEMO=0        skip the automatic control VM
#   MB=64         share.img size in MiB
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
TCC_GUEST_SHARE="${BUILD_DIR}/tcc-toaru/tcc-guest-share"
TEST_BIN="${BUILD_DIR}/tcc-toaru/test"
SYSROOT="${BUILD_DIR}/tcc-toaru/lib"
MC_SRC="${BUILD_DIR}/minicmd/mini-commander"
MC_BIN="${REPO_ROOT}/_output/mc"
NUKLEAR="${DOWNLOAD}/nuklear.h"
PUSH="${BUILD_DIR}/push"
SHARE_IMG="${BUILD_DIR}/share.img"
SHARE_TEST="${BUILD_DIR}/share-test.img"
VM="${REPO_ROOT}/toaru-vm.sh"

VISIBLE=${VISIBLE:-1}
DEMO=${DEMO:-1}
MB=${MB:-64}

die() { echo "error: $*" >&2; exit 1; }
. "$REPO_ROOT/need.sh"   # need(): dependency check with package-install hints

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

# nuklear-ide/ and netsurf-toaru/ expect the sources also in _build/download —
# seed it the same way build-toaru.sh does (never over an existing tree).
if [[ ! -d "${BUILD_DIR}/download/toaruos-src" ]]; then
    mkdir -p "${BUILD_DIR}/download"
    cp -a "${TOARU_SRC}" "${BUILD_DIR}/download/toaruos-src"
fi

# ------------------------------------- 3) TinyCC: cross + tcc for ToaruOS
if [[ ! -x "${TCC}" || ! -f "${TCC_GUEST}" || ! -f "${TCC_GUEST_SHARE}" \
      || ! -f "${SYSROOT}/libc.a" ]]; then
    echo "== building TinyCC (cross and for running inside ToaruOS) ..."
    "${REPO_ROOT}/mktcc.sh"
fi
[[ -x "${TCC}" ]]             || die "missing ${TCC} (mktcc.sh failed)"
[[ -f "${TCC_GUEST}" ]]       || die "missing ${TCC_GUEST} (mktcc.sh failed)"
[[ -f "${TCC_GUEST_SHARE}" ]] || die "missing ${TCC_GUEST_SHARE} (mktcc.sh failed)"
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
    echo "== downloading Nuklear ..."
    curl -L --fail -o "${NUKLEAR}.part" \
        "https://raw.githubusercontent.com/Immediate-Mode-UI/Nuklear/master/nuklear.h"
    mv "${NUKLEAR}.part" "${NUKLEAR}"
fi

# -------------------------------------------- 6) shared disk contents
echo "== preparing contents of ${SHARE_IMG} ..."
rm -rf "${PUSH}"
mkdir -p "${PUSH}/tcc/bin" "${PUSH}/tcc/lib" "${PUSH}/toaruos-src"

# user files (the user/ directory in the project root) have precedence
if [[ -d "${REPO_ROOT}/user" ]]; then
    cp -a "${REPO_ROOT}/user/." "${PUSH}/"
fi

install -m 755 "${MC_BIN}"     "${PUSH}/mc"
# the /share/tcc variant of tcc: its compiled-in default path is /share/tcc,
# so crt0.o/libc.a/include are found without -B (tcc-guest expects /extras/tcc)
install -m 755 "${TCC_GUEST_SHARE}" "${PUSH}/tcc/bin/tcc"
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
cat > "${PUSH}/compile_examples.sh" <<'GUEST_EOF'
#!/bin/sh
# Compiles and runs all *.c in /share.  Output goes to /tmp so that
# nothing is written to the disk image (ata.ko flushes writes only partially).
#
#   /share/compile_examples.sh
#
# Note: avl.c, btree.c, queens.c, heapsort.c, knight.c, quicksort.c and towers.c
# are C++ (constructors, references, "const int N" as array size).
# TinyCC has only a C frontend, so it will not compile them - they serve for
# browsing in mc.
# tcc_hello.c and tcc_test.c are C and will be compiled.
if [ ! -d /share/tcc ]; then
    echo "Disk is not mounted. First run:"
    echo "  mkdir -p /share; insmod /mod/ext2.ko; mount ext2 /dev/hda,rw /share"
    exit 1
fi

for src in /share/*.c; do
    name=$(basename "$src" .c)
    exe="/tmp/$name"
    echo "== $src"
    if /share/tcc/bin/tcc -o "$exe" "$src"; then
        "$exe"
    else
        echo "   (tcc failed to compile; tcc cannot handle the C++ examples)"
    fi
done
GUEST_EOF
chmod +x "${PUSH}/compile_examples.sh"

cat > "${PUSH}/README.txt" <<'GUEST_EOF'
ToaruOS + TinyCC + mini-commander
=================================

Mount the disk (in the Terminal or on the serial line):

    mkdir -p /share
    insmod /mod/ext2.ko
    mount ext2 /dev/hda,rw /share

TinyCC inside ToaruOS:

    /share/tcc/bin/tcc -o /tmp/hello /share/tcc_hello.c ; /tmp/hello
    /share/compile_examples.sh

mini-commander:

    /share/mc /share

/share/tcc contains the whole sysroot (include/, lib/crt0.o, libc.a, libtcc1.a),
/share/toaruos-src contains the sources of ToaruOS 2.3.2.
GUEST_EOF

SRC="${PUSH}" IMG="${SHARE_IMG}" MB="${MB}" "${REPO_ROOT}/mkshare.sh"

# ------------------------------------- 7) automatic check in a headless VM
demo() {
    cp "${SHARE_IMG}" "${SHARE_TEST}"
    local rc=0 text
    echo "== starting the control (headless) ToaruOS ..."
    VISIBLE=0 HEADLESS=1 SHARE="${SHARE_TEST}" "${VM}" start > /dev/null
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

    "${VM}" send 'mkdir -p /share; insmod /mod/ext2.ko; mount ext2 /dev/hda /share; echo __MNT__'
    wait_for __MNT__ || { echo "  *** /share did not mount"; rc=1; return $rc; }

    "${VM}" send '/share/tcc_test; echo __T1__'
    wait_for __T1__ || { echo "  *** /share/tcc_test did not finish"; rc=1; return $rc; }

    # esh does not handle && (only ;), hence two commands in a row
    "${VM}" send '/share/tcc/bin/tcc -o /tmp/tcc_hello /share/tcc_hello.c ; /tmp/tcc_hello; echo __T2__'
    wait_for __T2__ || { echo "  *** tcc in ToaruOS did not compile tcc_hello.c"; rc=1; return $rc; }

    # mc exits with F10; we send its escape sequence directly (ESC[21~)
    "${VM}" send '/share/mc /share'
    sleep 3
    "${VM}" send "$(printf '\033[21~')"
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
    if grep -aq 'Segmentation fault' <<< "$text"; then
        echo "  *** something in the guest crashed with Segmentation fault"; rc=1
    fi
    if ! grep -aq 'Quit' <<< "$text"; then
        echo "  *** mini-commander did not draw (the Quit hint is missing)"; rc=1
    fi

    "${VM}" stop > /dev/null 2>&1 || true
    VM_TEST_STARTED=0

    if [[ $rc == 0 ]]; then
        echo "== check OK: tcc_test, tcc inside ToaruOS and mini-commander all work"
    else
        echo "== check FOUND A PROBLEM (see above)"
    fi
    return $rc
}

if [[ "${DEMO}" == 1 ]]; then
    if ! demo; then
        echo "Automatic check failed; the image ${SHARE_IMG} is ready anyway." >&2
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
    VISIBLE=1 HEADLESS=0 "${VM}" start
else
    echo "== starting ToaruOS without a window (serial line) ..."
    VISIBLE=0 HEADLESS=1 "${VM}" start
fi

cat <<EOF

Done. ToaruOS is running (log: ${BUILD_DIR}/work/out.log).

In the ToaruOS window open the Terminal and run:

    mkdir -p /share
    insmod /mod/ext2.ko
    mount ext2 /dev/hda,rw /share
    /share/tcc/bin/tcc -o /tmp/hello /share/tcc_hello.c ; /tmp/hello
    /share/mc /share

Shutting down the VM:  ./toaru-vm.sh stop
Serial line (only with HEADLESS=1):  ./toaru-vm.sh send 'command'
EOF
