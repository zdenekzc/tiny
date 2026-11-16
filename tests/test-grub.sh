#!/usr/bin/env bash
# Updated test script for Fedora: uses grub2-mkrescue with explicit module path
# and ensures a bootable ISO with a visible text GRUB menu.

# Try to locate the i386-pc GRUB modules automatically.
# Common locations on Fedora/RHEL based systems include:
#   /usr/lib/grub/i386-pc
#   /usr/lib/grub2/i386-pc
#   /usr/share/grub/i386-pc
# If none are found, grub2-mkrescue will fall back to its defaults.
GRUB_MODULES_DIR=""
# Search a list of possible directories.
for dir in /usr/lib/grub/i386-pc /usr/lib/grub2/i386-pc /usr/share/grub/i386-pc; do
    if [[ -d "$dir" ]]; then
        GRUB_MODULES_DIR="$dir"
        break
    fi
done
# Build the command – include -d only when a directory was found.
if [[ -n "$GRUB_MODULES_DIR" ]]; then
    GRUB_MKRESCUE_CMD="grub2-mkrescue -d \"$GRUB_MODULES_DIR\""
else
    GRUB_MKRESCUE_CMD="grub2-mkrescue"
fi
# test-grub.sh — Boot ToaruOS via GRUB and auto-start mini-commander.
#
# This script builds ToaruOS (if not already built), extracts the kernel and
# initrd, creates a GRUB bootable ISO that loads them, and runs QEMU with a
# visible window.

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${REPO_ROOT}/_build"
TOARU_SRC_DIR="${BUILD_DIR}/toaruos-native/src"
ISO_PATH="${TOARU_SRC_DIR}/image.iso"

# Ensure ToaruOS native build exists (with our modified startup script).
if [[ ! -f "${ISO_PATH}" ]]; then
    echo "== ToaruOS ISO not found, building native version..."
    "${REPO_ROOT}/build-toaruos.sh" native
fi

# Extract kernel and ramdisk from the ISO.
KERNEL_OUT="${BUILD_DIR}/toaruos-native/kernel"
RAMDISK_OUT="${BUILD_DIR}/toaruos-native/ramdisk.igz"
mkdir -p "${KERNEL_OUT}" "${RAMDISK_OUT}"

# Use isoinfo to extract raw files.
isoinfo -R -i "${ISO_PATH}" -x /kernel > "${KERNEL_OUT}/kernel"
isoinfo -R -i "${ISO_PATH}" -x /ramdisk.igz > "${RAMDISK_OUT}/ramdisk.igz"

# Prepare GRUB directory structure.
GRUB_DIR="${BUILD_DIR}/grub_toaruos"
mkdir -p "${GRUB_DIR}/boot/grub"
cp "${KERNEL_OUT}/kernel" "${GRUB_DIR}/boot/kernel"
cp "${RAMDISK_OUT}/ramdisk.igz" "${GRUB_DIR}/boot/ramdisk.igz"

# Create a simple GRUB configuration that loads the kernel and initrd.
cat > "${GRUB_DIR}/boot/grub/grub.cfg" <<'EOF'
set timeout=5
menuentry "ToaruOS (mini-commander auto start)" {
    multiboot /boot/kernel
    module /boot/ramdisk.igz
}
EOF

# Build the GRUB ISO.
GRUB_ISO="${BUILD_DIR}/toaruos_grub.iso"
# grub2-mkrescue must be on PATH.
$GRUB_MKRESCUE_CMD -o "${GRUB_ISO}" "${GRUB_DIR}" > /dev/null

echo "== GRUB ISO created at ${GRUB_ISO}"

# Launch QEMU with a visible window.
# Adjust RAM/CPU as needed.
qemu-system-x86_64 -cdrom "${GRUB_ISO}" -m 1024 -smp 2 -display gtk
