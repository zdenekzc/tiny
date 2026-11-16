#!/usr/bin/env bash
# Test script for the bare‑MC kernel built with GRUB.
# Finds GRUB modules, builds the bare‑MC ISO, and boots it with QEMU.

# Auto‑detect GRUB i386‑pc modules (common locations).
GRUB_MODULES_DIR=""
for dir in /usr/lib/grub/i386-pc /usr/lib/grub2/i386-pc /usr/share/grub/i386-pc; do
    [[ -d "$dir" ]] && GRUB_MODULES_DIR="$dir" && break
done
if [[ -n "$GRUB_MODULES_DIR" ]]; then
    GRUB_MKRESCUE_CMD="grub2-mkrescue -d \"$GRUB_MODULES_DIR\""
else
    GRUB_MKRESCUE_CMD="grub2-mkrescue"
fi

set -euo pipefail
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# Ensure mini‑commander binary exists.
if [[ ! -x "${REPO_ROOT}/_output/mc" ]]; then
    echo "== Building mini‑commander ..."
    "${REPO_ROOT}/mc-toaru/build.sh" mc
fi

# Build the bare‑MC kernel and ISO.
make -C "${REPO_ROOT}/bare-mc"

ISO="${REPO_ROOT}/bare-mc/bare-mc.iso"
if [[ ! -f "$ISO" ]]; then
    echo "Error: ISO not found at $ISO"
    exit 1
fi

echo "== Launching QEMU with $ISO"
# Create a virtual hard disk (FAT32) if it does not exist
VHD="${REPO_ROOT}/bare-mc/hd.img"
if [[ ! -f "$VHD" ]]; then
    echo "== Creating virtual FAT32 disk $VHD"
    qemu-img create -f raw "$VHD" 128M
    mkfs.fat -F 32 "$VHD"
fi
qemu-system-x86_64 -cdrom "$ISO" -boot d -m 512 -display gtk -net none -drive file="$VHD",format=raw,if=virtio
