#!/usr/bin/env bash
# build-toaru-arm64.sh — one-click start of ToaruOS ARM64 in QEMU with a visible window.
#
#   ./tests/build-toaru-arm64.sh
#
# What the script does:
#   1. Builds the ARM64 ToaruOS images if they are missing (native build).
#   2. Starts QEMU-system-aarch64 with the built bootstub, kernel and initrd.
#   3. Opens a window (GTK) so the user can see the OS running.
#
# The script is intended for use in CI or local testing. It mirrors the
# functionality of tests/build-toaru.sh for x86_64, but uses the aarch64 artefacts
# produced by build-toaruos.sh -a.

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${REPO_ROOT}/_build"
# Prefer the native build artefacts; fall back to the docker one if present.
if [[ -d "${BUILD_DIR}/toaruos-native-aarch64/src" ]]; then
    SRC="${BUILD_DIR}/toaruos-native-aarch64/src"
elif [[ -d "${BUILD_DIR}/toaruos-docker-aarch64/src" ]]; then
    SRC="${BUILD_DIR}/toaruos-docker-aarch64/src"
else
    SRC=""
fi

# Ensure the required artefacts exist or build them.
ensure_artifacts() {
    if [[ -z "$SRC" || ! -f "${SRC}/bootstub" || ! -f "${SRC}/misaka-kernel" || ! -f "${SRC}/ramdisk.igz" ]]; then
        echo "== ARM64 artefacts missing — building with ./build-toaruos.sh -a native"
        "${REPO_ROOT}/build-toaruos.sh" -a native || { echo "Failed to build ARM64 ToaruOS" >&2; exit 1; }
        # After build, re-detect the directory.
        if [[ -d "${BUILD_DIR}/toaruos-native-aarch64/src" ]]; then
            SRC="${BUILD_DIR}/toaruos-native-aarch64/src"
        elif [[ -d "${BUILD_DIR}/toaruos-docker-aarch64/src" ]]; then
            SRC="${BUILD_DIR}/toaruos-docker-aarch64/src"
        else
            echo "Unable to locate ARM64 build output after building" >&2
            exit 1
        fi
    fi
}

ensure_artifacts

# Verify the artefacts are present.
for f in bootstub misaka-kernel ramdisk.igz; do
    if [[ ! -f "${SRC}/${f}" ]]; then
        echo "Missing ${f} in ${SRC}" >&2
        exit 1
    fi
done

# Run QEMU with a visible window.
echo "== starting ToaruOS ARM64 in QEMU (window will appear)..."
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 2G -smp 2 \
    -display gtk \
    -kernel "${SRC}/bootstub" \
    -append "root=/dev/ram0 start=--headless" \
    -fw_cfg name=opt/org.toaruos.kernel,file="${SRC}/misaka-kernel" \
    -fw_cfg name=opt.org.toaruos.initrd,file="${SRC}/ramdisk.igz"
