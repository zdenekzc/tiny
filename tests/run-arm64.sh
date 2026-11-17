#!/usr/bin/env bash
# ------------------------------------------------------------
# "One-click" ToaruOS ARM64 in QEMU — the aarch64 counterpart of
# tests/run-toaru.sh:
#
#   ./tests/run-arm64.sh
#
# What the script does (everything can be deleted and run again at any time):
#   1. builds the ARM64 ToaruOS images when they are missing
#      (./build-toaruos.sh -a native; a cached build is reused as is —
#      there is no official aarch64 release, so the OS has to be built),
#   2. starts a HEADLESS (no window) control VM on the serial line and
#      verifies that the guest reaches the login prompt and answers commands,
#   3. starts a VISIBLE ToaruOS ARM64 in QEMU and leaves it running.
#
# Differences from the x86_64 run-toaru.sh, dictated by the aarch64 port:
#   - no ISO exists for aarch64: bootstub + misaka-kernel + ramdisk.igz are
#     handed to QEMU through fw_cfg (kernel8.img is for a Raspberry Pi 400),
#   - the aarch64 kernel has no block device driver (kernel/arch/aarch64/
#     virtio.c implements input devices only), so there is no /share disk
#     and no prebuilt payload (mc, tcc) — the guest userland is the one
#     built into ramdisk.igz,
#   - toaru-vm.sh is x86_64-only, so this script talks to QEMU through a
#     serial pipe directly.
#
# Shutdown:  ./toaru-vm.sh stop   (works: kills the last-started QEMU)  or
#            pkill -f qemu-system-aarch64
#
# Optional environment variables:
#   VISIBLE=0     no QEMU window (serial line only; automatic check still runs)
#   DEMO=0        skip the automatic control VM
# ------------------------------------------------------------

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${REPO_ROOT}/_build"
WORK="${BUILD_DIR}/work-arm64"

VISIBLE=${VISIBLE:-1}
DEMO=${DEMO:-1}

die() { echo "error: $*" >&2; exit 1; }
. "$REPO_ROOT/need.sh"   # need(): dependency check with package-install hints

need qemu-system-aarch64

# ------------------------------------------------------- 1) artefacts
# Prefer the native build artefacts; fall back to the docker one if present.
SRC=""
if [[ -d "${BUILD_DIR}/toaruos-native-aarch64/src" ]]; then
    SRC="${BUILD_DIR}/toaruos-native-aarch64/src"
elif [[ -d "${BUILD_DIR}/toaruos-docker-aarch64/src" ]]; then
    SRC="${BUILD_DIR}/toaruos-docker-aarch64/src"
fi

if [[ -z "${SRC}" || ! -f "${SRC}/bootstub" || ! -f "${SRC}/misaka-kernel" ||
      ! -f "${SRC}/ramdisk.igz" ]]; then
    echo "== ARM64 artefacts missing — building with ./build-toaruos.sh -a native ..."
    "${REPO_ROOT}/build-toaruos.sh" -a native || die "build-toaruos.sh -a native"
    if [[ -d "${BUILD_DIR}/toaruos-native-aarch64/src" ]]; then
        SRC="${BUILD_DIR}/toaruos-native-aarch64/src"
    elif [[ -d "${BUILD_DIR}/toaruos-docker-aarch64/src" ]]; then
        SRC="${BUILD_DIR}/toaruos-docker-aarch64/src"
    else
        die "unable to locate the ARM64 build output after building"
    fi
fi
for f in bootstub misaka-kernel ramdisk.igz; do
    [[ -f "${SRC}/${f}" ]] || die "missing ${f} in ${SRC}"
done
echo "== artefacts: ${SRC}"

# ------------------------------------------------------- 2) QEMU helpers
# toaru-vm.sh is x86_64-only; here QEMU runs directly and the serial line is
# a pipe: the guest writes to <p>.out, commands are written into <p>.in.
# QEMU does not create the FIFOs itself — mkfifo is mandatory.
QEMU_PID=0
CAT_PID=0
qemu_start() {   # qemu_start <visible 0|1> <logfile>
    local visible=$1 log=$2
    mkdir -p "${WORK}"
    rm -f "${WORK}/serial.in" "${WORK}/serial.out" "${WORK}/serial.log"
    mkfifo "${WORK}/serial.in" "${WORK}/serial.out"
    local -a display=() extra=() append=()
    if [[ "${visible}" == 1 ]]; then
        # graphical session (the upstream `make ARCH=aarch64 run` target)
        display=(-display gtk)
        extra=(-device ramfb -device virtio-tablet-pci -device virtio-keyboard-pci -device AC97)
        append=(-append "root=/dev/ram0 migrate start=live-session ramfb vid=preset")
    else
        # headless session with a getty on the serial line (upstream `shell`)
        display=(-display none)
        extra=(-fw_cfg name=opt/org.toaruos.gettyargs,string='-a local /dev/ttyS0 115200 xterm')
        append=(-append "root=/dev/ram0 migrate start=--headless")
    fi
    qemu-system-aarch64 -M virt -cpu cortex-a72 -m 4G -smp 4 \
        "${display[@]}" -no-reboot -d guest_errors \
        -kernel "${SRC}/bootstub" \
        "${append[@]}" \
        -fw_cfg name=opt/org.toaruos.initrd,file="${SRC}/ramdisk.igz" \
        -fw_cfg name=opt/org.toaruos.kernel,file="${SRC}/misaka-kernel" \
        "${extra[@]}" \
        -serial pipe:"${WORK}/serial" \
        > "${log}" 2>&1 &
    QEMU_PID=$!
    # A FIFO needs a permanent reader: drain the guest output into a plain
    # file so log polling never blocks and QEMU never stalls on a full pipe.
    # stderr goes to /dev/null: an inherited stderr would keep the caller's
    # pipe open after the script exits (the "hangs forever" trap that
    # toaru-vm.sh works around as well).
    cat "${WORK}/serial.out" > "${WORK}/serial.log" 2> /dev/null &
    CAT_PID=$!
}

qemu_stop() {
    [[ "${QEMU_PID}" != 0 ]] && kill "${QEMU_PID}" 2> /dev/null || true
    [[ "${CAT_PID}" != 0 ]] && kill "${CAT_PID}" 2> /dev/null || true
    QEMU_PID=0
    CAT_PID=0
}
trap qemu_stop EXIT

serial_send() {   # serial_send <text> - one command line to the guest
    printf '%s\n' "$1" > "${WORK}/serial.in"
}

serial_log() {    # serial_log [lines] - last lines of the guest serial output
    tail -n "${1:-40}" "${WORK}/serial.log" 2> /dev/null | tr -d '\r'
}

wait_for() {      # wait_for <regex> [timeout s] - watch the serial log
    local pattern=$1 timeout=${2:-120} i
    for i in $(seq 1 "${timeout}"); do
        if serial_log 2000 | grep -aq "${pattern}"; then return 0; fi
        kill -0 "${QEMU_PID}" 2> /dev/null || return 1
        sleep 1
    done
    return 1
}

# ------------------------------------------------------- 3) automatic check
demo() {
    local rc=0
    echo "== starting the control (headless) ToaruOS ARM64 ..."
    qemu_start 0 "${WORK}/qemu-check.log"

    # The guest takes a while to decompress the ramdisk and start init.
    # The serial getty auto-logs in as "local" (opt/org.toaruos.gettyargs).
    if ! wait_for 'local@livecd' 180; then
        echo "  *** guest did not reach a shell within 180 s; last serial output:"
        serial_log 30 | sed 's/^/      /'
        rc=1
        return $rc
    fi
    sleep 1
    serial_send 'echo __BOOT_OK__'
    if ! wait_for '__BOOT_OK__' 30; then
        echo "  *** guest did not answer on the serial line"
        rc=1
        return $rc
    fi

    qemu_stop

    if [[ $rc == 0 ]]; then
        echo "== check OK: ToaruOS ARM64 boots and answers on the serial line"
    else
        echo "== check FOUND A PROBLEM (see above)"
    fi
    return $rc
}

if [[ "${DEMO}" == 1 ]]; then
    if ! demo; then
        echo "Automatic check failed." >&2
        exit 1
    fi
fi

# --------------------------------------- 4) visible ToaruOS for the user
if [[ "${VISIBLE}" == 1 ]]; then
    echo "== starting visible ToaruOS ARM64 in QEMU ..."
    qemu_start 1 "${WORK}/qemu-visible.log"
    echo "QEMU pid=${QEMU_PID} log=${WORK}/qemu-visible.log"
else
    echo "== starting ToaruOS ARM64 without a window (serial line) ..."
    qemu_start 0 "${WORK}/qemu-visible.log"
    echo "QEMU pid=${QEMU_PID} log=${WORK}/qemu-visible.log"
    echo "serial output: ${WORK}/serial.log"
    echo "send commands: echo 'command' > ${WORK}/serial.in"
fi

# do not kill the user's VM on exit
trap - EXIT
QEMU_PID=0

cat <<EOF

Done. ToaruOS ARM64 is running.

Shutting down the VM:  ./toaru-vm.sh stop   (or: pkill -f qemu-system-aarch64)
Serial line:           ${WORK}/serial.log
EOF
