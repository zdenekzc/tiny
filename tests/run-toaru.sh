#!/usr/bin/env bash
# ------------------------------------------------------------
# "One-click" ToaruOS + TinyCC + mini-commander:
#
#   ./tests/run-toaru.sh
#
# What the script does (everything can be deleted and run again at any time):
#   1. downloads the ToaruOS 2.3.2 ISO (if missing),
#   2. downloads the ToaruOS sources (tag v2.3.2, just for browsing in the guest),
#   3. downloads TinyCC and builds two things:
#        - a cross compiler on the host (_build/tcc-toaru/bin/x86_64-toaru-tcc),
#        - a tcc that runs INSIDE ToaruOS (_build/tcc-toaru/tcc-guest),
#   4. downloads mini-commander and compiles it with the cross tcc (_output/mc),
#   5. downloads the Nuklear header (only into /share, not used yet),
#   6. prepares the ext2 image _build/share.img with:
#        /mc, /tcc_test, /tcc_test.c, /tcc/{bin/tcc,include,lib},
#        /tests/examples/*.c, /toaruos-src, /compile_examples.sh,
#   7. starts a HEADLESS (no window) control VM and verifies:
#        - /share/tcc_test (program compiled by the cross tcc),
#        - tcc inside ToaruOS compiles and runs /share/tcc_hello.c,
#        - mini-commander starts and can be exited with F10,
#   8. starts a VISIBLE ToaruOS in QEMU and leaves it running.
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
#   MB=128        share.img size in MiB (default 64, enough even with sources)
# ------------------------------------------------------------

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${REPO_ROOT}/_build"
DOWNLOAD="${BUILD_DIR}/download"
ISO="${DOWNLOAD}/toaruos-2.3.2.iso"
TOARU_SRC="${DOWNLOAD}/toaruos-src"
TCC="${BUILD_DIR}/tcc-toaru/bin/x86_64-toaru-tcc"
TCC_GUEST="${BUILD_DIR}/tcc-toaru/tcc-guest"
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
need() { command -v "$1" > /dev/null 2>&1 || die "missing program '$1'"; }

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
    curl -L --fail -o "${ISO}.part" \
        "https://github.com/klange/toaruos/releases/download/v2.3.2/toaruos-2.3.2.iso"
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
    "${REPO_ROOT}/mktcc.sh"
fi
[[ -x "${TCC}" ]]     || die "missing ${TCC} (mktcc.sh failed)"
[[ -f "${TCC_GUEST}" ]] || die "missing ${TCC_GUEST} (mktcc.sh failed)"
[[ -f "${TEST_BIN}" ]]  || die "missing ${TEST_BIN} (mktcc.sh failed)"

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
# Compiles and runs all *.c in /share.  Output goes to /tmp so that
# nothing is written to the disk image (ata.ko flushes writes only partially).
#
#   /share/compile_examples.sh
#
# All sources in /share are plain C and TinyCC compiles and runs them;
# tcc_test.c is the test program from tcc/toaru.
if not stat -q /share/tcc/bin/tcc then echo "Disk is not mounted. First run:"
if not stat -q /share/tcc/bin/tcc then echo "  mkdir -p /share; insmod /mod/ext2.ko; mount ext2 /dev/hda,rw /share"
if not stat -q /share/tcc/bin/tcc then exit 1

GUEST_EOF
    for src in "${PUSH}"/*.c; do
        name=$(basename "$src" .c)
        printf 'echo "== /share/%s.c"\n' "$name"
        printf 'if stat -q "/tmp/%s" then rm "/tmp/%s"\n' "$name" "$name"
        printf '/share/tcc/bin/tcc -o "/tmp/%s" "/share/%s.c"\n' "$name" "$name"
        printf 'if stat -q "/tmp/%s" then "/tmp/%s"\n' "$name" "$name"
        printf 'if not stat -q "/tmp/%s" then echo "   tcc failed on /share/%s.c"\n\n' "$name" "$name"
    done
} > "${PUSH}/compile_examples.sh"
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

    "${VM}" send '/share/compile_examples.sh; echo __T3__'
    wait_for __T3__ || { echo "  *** compile_examples.sh did not finish"; rc=1; return $rc; }

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
    if grep -aq 'tcc failed on' <<< "$text"; then
        echo "  *** tcc inside ToaruOS failed to compile an example"; rc=1
    fi
    # six examples end with O.K.; btree just prints its keys (last one is 800)
    local ok_count btree_out
    ok_count=$(grep -ac '^O\.K\.$' <<< "$text" || true)
    if [[ ${ok_count:-0} -lt 6 ]]; then
        echo "  *** only ${ok_count:-0} of the 6 examples printed O.K."; rc=1
    fi
    btree_out=$(sed -n '/^== \/share\/btree\.c$/,/^== \/share\/heapsort\.c$/p' <<< "$text")
    if ! grep -aqx '800' <<< "$btree_out"; then
        echo "  *** btree did not print its last key (800)"; rc=1
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
        echo "== check OK: tcc_test, tcc inside ToaruOS, all examples and mini-commander work"
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
