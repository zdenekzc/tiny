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
#        /gcc_hello (built on the host by gcc-toaru.sh, when gcc is present),
#   7. starts a HEADLESS (no window) control VM and verifies:
#        - /share/tcc_test (program compiled by the cross tcc),
#        - tcc inside ToaruOS compiles and runs /share/tcc_hello.c,
#        - every example in /share compiles and runs,
#        - /share/gcc_hello (built by the HOST gcc, see gcc-toaru.sh) runs,
#        - mini-commander starts, draws its panels with a highlighted current
#          item, executes its command line on Enter and quits with F10 or a
#          mouse click on the F10 footer,
#   8. starts a VISIBLE ToaruOS in QEMU and mounts the shared disk at /share:
#        - visible boot: closes the welcome tutorial, double-clicks the
#          Terminal icon (QMP absolute pointer) and types the mount commands
#          with QEMU sendkey (the graphical session is "local"),
#        - headless boot: uses the getty on the serial line as root.
#      The VM is then left running.
#
# Inside the visible ToaruOS open the Terminal and run:
#
#   /share/tcc/bin/tcc -o /tmp/hello /share/tcc_hello.c ; /tmp/hello
#   /share/mc /share
#
# (When the VM is started by hand with ./toaru-vm.sh start, mount the disk
#  first: mkdir -p /share; insmod /mod/ext2.ko; mount ext2 /dev/hda,rw /share)
#
# Shutdown:  ./toaru-vm.sh stop
#
# Optional environment variables:
#   VISIBLE=0     no QEMU window (serial line only; automatic check still runs)
#   HEADLESS=1    force headless boot even for the visible VM
#   DEMO=0        skip the automatic control VM
#   AUTOMOUNT=0   do not mount the shared disk automatically
#   MB=128        share.img size in MiB (default 64, enough even with sources)
# ------------------------------------------------------------

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${REPO_ROOT}/_build"
IMPORT_DIR="${REPO_ROOT}/_import"
IMPORT_DOWNLOAD="${IMPORT_DIR}/download"
BUILD_DOWNLOAD="${BUILD_DIR}/download"
DOWNLOAD="${IMPORT_DOWNLOAD}"
ISO="${DOWNLOAD}/image.iso"
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

# If cached imports exist, copy them into the build tree before building
if [[ -d "${IMPORT_DOWNLOAD}/tinycc" ]]; then
    mkdir -p "${BUILD_DIR}/download"
    cp -a "${IMPORT_DOWNLOAD}/tinycc" "${BUILD_DIR}/download/"
fi
if [[ -d "${IMPORT_DIR}/mini-commander" ]]; then
    mkdir -p "${BUILD_DIR}/minicmd"
    cp -a "${IMPORT_DIR}/mini-commander" "${BUILD_DIR}/minicmd/"
fi

VISIBLE=${VISIBLE:-1}
HEADLESS=${HEADLESS:-0}   # 1 = headless boot even when a QEMU window is open
DEMO=${DEMO:-1}
AUTOMOUNT=${AUTOMOUNT:-1}
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
        "https://github.com/klange/toaruos/releases/download/v2.3.2/image.iso"
    mv "${ISO}.part" "${ISO}"
# Cache ISO in build dir
mkdir -p "${BUILD_DIR}/download"
cp -a "${ISO}" "${BUILD_DIR}/download/"
# Also copy to name expected by mktcc.sh
cp -a "${ISO}" "${BUILD_DIR}/download/toaruos-2.3.2.iso"
fi
# Ensure ISO is available in build directory for tools expecting the old name
mkdir -p "${BUILD_DIR}/download"
# Copy the image (may already exist) to the generic name used by mklibc.sh
cp -a "${ISO}" "${BUILD_DIR}/download/"
# Also copy to the historic toaruos-2.3.2.iso name expected by older scripts
cp -a "${ISO}" "${BUILD_DIR}/download/toaruos-2.3.2.iso"

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

# A program built on the HOST by the host gcc — the route a developer would
# use for their own C (gcc + GNU ld + toaru-elfpad.py, see gcc-toaru.sh).
# TinyCC is not involved; the control VM below checks that it really runs.
GCC_TEST="${PUSH}/gcc_hello"
if command -v gcc > /dev/null 2>&1 && command -v ld > /dev/null 2>&1; then
    "${REPO_ROOT}/gcc-toaru.sh" -o "${GCC_TEST}" "${REPO_ROOT}/tests/gcc/hello.c" \
        || die "gcc-toaru.sh failed"
else
    echo "note: gcc/ld not found — /share/gcc_hello will not be built"
fi

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
/share/gcc_hello was built on the host by the host gcc (gcc-toaru.sh), not by
TinyCC: same ToaruOS libc, different compiler.
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

    at_shell() {   # at_shell <marker> - did a shell prompt follow the marker?
        awk -v m="$1" '$0 == m { seen = 1; next }
            seen && /root@livecd/ { found = 1 }
            END { exit !found }'
    }

    # send_raw writes bytes without the trailing NL that "send" adds; needed for
    # CR (Enter) and for escape sequences whose trailing NL would be read as Enter.
    send_raw() { printf '%s' "$1" > "${BUILD_DIR}/work/in"; }

    "${VM}" send 'mkdir -p /share; insmod /mod/ext2.ko; mount ext2 /dev/hda /share; echo __MNT__'
    wait_for __MNT__ || { echo "  *** /share did not mount"; rc=1; return $rc; }

    "${VM}" send '/share/tcc_test; echo __T1__'
    wait_for __T1__ || { echo "  *** /share/tcc_test did not finish"; rc=1; return $rc; }

    # esh does not handle && (only ;), hence two commands in a row
    "${VM}" send '/share/tcc/bin/tcc -o /tmp/tcc_hello /share/tcc_hello.c ; /tmp/tcc_hello; echo __T2__'
    wait_for __T2__ || { echo "  *** tcc in ToaruOS did not compile tcc_hello.c"; rc=1; return $rc; }

    # The same C library, but the program was compiled by the HOST gcc.
    if [[ -f "${GCC_TEST}" ]]; then
        "${VM}" send '/share/gcc_hello; echo __T4__'
        wait_for __T4__ || { echo "  *** /share/gcc_hello (host gcc) did not finish"; rc=1; return $rc; }
    fi

    "${VM}" send '/share/compile_examples.sh; echo __T3__'
    wait_for __T3__ || { echo "  *** compile_examples.sh did not finish"; rc=1; return $rc; }

    # mc exits with F10; we send the escape sequences directly (F9 = ESC[20~,
    # F10 = ESC[21~).  If the parser loses the CSI prefix, "20~"/"21~" ends up
    # on the terminal as text (that was a real bug), so check for both.
    "${VM}" send '/share/mc /share'
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

    # F3 (viewer) and F4 (editor) used to crash: view_edit.c called mmap(),
    # which ToaruOS does not implement, and then parsed the NULL pointer.
    # Move down to README.txt, open both, and quit each with F10.
    local dn
    for dn in 1 2 3 4 5; do
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
    "${VM}" send '/share/mc /share'
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
    btree_out=$(sed -n '/^== \/share\/btree\.c$/,/^== \/share\/heapsort\.c$/p' <<< "$text")
    if ! grep -aqx '800' <<< "$btree_out"; then
        echo "  *** btree did not print its last key (800)"; rc=1
    fi
    # The viewer draws its status bar as "<file> ... <last>/<lines> <pct>%"
    # (the editor instead has "L:[...]" and "U+...").  Do not look for the file
    # text itself: the screen is updated cell by cell, so a space that is
    # already on the screen is not sent to the terminal again and the log would
    # show "TinyCC+ mini-commander".
    if ! grep -aqE '/share/README\.txt.*[0-9]+/ *[0-9]+ +[0-9]+%' <<< "$text"; then
        echo "  *** the F3 viewer did not open README.txt"; rc=1
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
    if ! grep -aq -- '-- /share --' <<< "$text"; then
        echo "  *** mini-commander did not draw the file panels"; rc=1
    fi
    # The current item is drawn black on cyan (SGR 30;46).  If wattron() does
    # not store the color pair, every row keeps the plain panel background.
    if ! grep -aqF $'\033[0;30;46m' "${BUILD_DIR}/work/out.log" 2>/dev/null; then
        echo "  *** the current panel item is not highlighted"; rc=1
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
    VISIBLE=1 HEADLESS="${HEADLESS}" "${VM}" start
else
    echo "== starting ToaruOS without a window (serial line) ..."
    VISIBLE=0 HEADLESS=1 "${VM}" start
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

mount_share_serial() {
    local i
    for i in $(seq 1 60); do
        "${VM}" log 2000 2>/dev/null | grep -aq 'root@livecd' && break
        sleep 1
    done
    "${VM}" send 'mkdir -p /share; insmod /mod/ext2.ko; mount ext2 /dev/hda,rw /share; echo __SHARE_MOUNTED__'
    for i in $(seq 1 30); do
        "${VM}" log 2000 2>/dev/null | grep -qx '__SHARE_MOUNTED__' && return 0
        sleep 1
    done
    return 1
}

mount_share_gui() {
    # The graphical boot shows the "Welcome to ToaruOS!" tutorial first and has
    # no Terminal open; checking the serial log tells us nothing here.
    echo "== waiting for the graphical session ..."
    sleep 25
    focus_qemu || echo "warning: cannot focus the QEMU window (wmctrl/xdotool); continuing" >&2
    sleep 1

    # Close the first-boot tutorial (harmless when it is already gone).
    press_key esc
    sleep 2

    local w h tx ty i text
    if ! read -r w h < <(guest_screen_size); then
        echo "warning: could not read the guest screen size" >&2
        return 1
    fi
    [[ -n ${w:-} && -n ${h:-} ]] || {
        echo "warning: could not read the guest screen size" >&2; return 1; }
    # Terminal icon in the top-left corner of the desktop (measured at
    # 1920x1080; scale in case the guest picks another mode).
    tx=$(( 64 * w / 1920 ))
    ty=$(( 190 * h / 1080 ))
    qmp_double_click "${tx}" "${ty}" "${w}" "${h}" || return 1
    sleep 3

    # The first sudo asks for the live CD user's password (local/local).  The
    # commands are typed slowly: sendkey holds each key for ~100 ms and the
    # Terminal drops keys when they arrive too fast.
    type_in_guest 'sudo mkdir -p /share' 0.12 || return 1
    sleep 2
    type_in_guest 'local' 0.12 || return 1
    sleep 2
    type_in_guest 'sudo insmod /mod/ext2.ko' 0.1 || return 1
    sleep 1
    type_in_guest 'sudo mount ext2 /dev/hda,rw /share' 0.1 || return 1
    sleep 1

    # Verify from the host.  local cannot redirect into /dev/ttyS1 (the live
    # session does not set supplementary groups), but root can, so copy a file
    # from the mounted disk there: if the README text reaches the serial log,
    # /share really is mounted.
    type_in_guest 'sudo cp /share/README.txt /dev/ttyS1' 0.1 || return 1
    for i in $(seq 1 20); do
        text=$("${VM}" log 200000 2>/dev/null || true)
        if grep -aq 'ToaruOS + TinyCC + mini-commander' <<< "$text"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

MOUNTED_TEXT="Mount the shared disk in the Terminal first (see below)."
if [[ "${AUTOMOUNT}" == 1 ]]; then
    if [[ "${VISIBLE}" == 1 && "${HEADLESS}" == 0 ]]; then
        if mount_share_gui; then
            echo "== /share is mounted in the graphical session"
            MOUNTED_TEXT="The shared disk was mounted at /share automatically."
        else
            echo "warning: /share was not mounted automatically; mount it in the Terminal" >&2
        fi
    elif mount_share_serial; then
        echo "== /share is mounted in the guest"
        MOUNTED_TEXT="The shared disk was mounted at /share automatically."
    else
        echo "warning: /share was not mounted automatically; mount it manually (see below)" >&2
    fi
fi

cat <<EOF

Done. ToaruOS is running (log: ${BUILD_DIR}/work/out.log).
${MOUNTED_TEXT}

In the ToaruOS window open the Terminal and run:

    /share/tcc/bin/tcc -o /tmp/hello /share/tcc_hello.c ; /tmp/hello
    /share/mc /share

If you start the VM later with ./toaru-vm.sh start, mount the disk first:

    mkdir -p /share
    insmod /mod/ext2.ko
    mount ext2 /dev/hda,rw /share

Shutting down the VM:  ./toaru-vm.sh stop
Serial line (only with HEADLESS=1):  ./toaru-vm.sh send 'command'
EOF
