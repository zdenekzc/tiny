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

# Mini‑commander not required for bare‑MC boot.

# Build the bare‑MC kernel and ISO.
make -C "${REPO_ROOT}/bare-mc"

ISO="${REPO_ROOT}/bare-mc/bare-mc.iso"
if [[ ! -f "$ISO" ]]; then
    echo "Error: ISO not found at $ISO"
    exit 1
fi
# Ensure initramfs.tar exists (empty if missing)
if [[ ! -f "${REPO_ROOT}/bare-mc/initramfs.tar" ]]; then
    echo "== Creating empty initramfs.tar"
    tar -cf "${REPO_ROOT}/bare-mc/initramfs.tar" --files-from /dev/null
fi

# Build a temporary ISO using GRUB
ISO_DIR=$(mktemp -d)
mkdir -p "$ISO_DIR/boot/grub"
cp "${REPO_ROOT}/bare-mc/kernel.bin" "$ISO_DIR/boot/kernel.bin"
cp "${REPO_ROOT}/bare-mc/initramfs.tar" "$ISO_DIR/boot/initramfs.tar"
cat > "$ISO_DIR/boot/grub/grub.cfg" <<'EOF'
# Ensure GRUB menu is displayed
timeout_style=menu
set timeout=3
menuentry "Bare MC" {
    multiboot2 /boot/kernel.bin
    module2 /boot/initramfs.tar type=tar
}
EOF
# Ensure required GRUB BIOS tools are present (grub2-install, grub2-mkrescue)
if ! command -v grub2-install >/dev/null 2>&1; then
    echo "grub2-install not found, attempting to install grub2-pc..."
    if command -v sudo >/dev/null 2>&1; then
        sudo dnf install -y grub2-pc || { echo "Failed to install grub2-pc"; exit 1; }
    else
        dnf install -y grub2-pc || { echo "Failed to install grub2-pc"; exit 1; }
    fi
fi
# Generate the ISO
$GRUB_MKRESCUE_CMD -o "$ISO" "$ISO_DIR"
# Cleanup temporary ISO directory
rm -rf "$ISO_DIR"

echo "== Launching QEMU with $ISO"
# Launch QEMU, booting from the ISO (GRUB menu)
qemu-system-x86_64 -cdrom "$ISO" -boot d -m 512 -display gtk -net none
