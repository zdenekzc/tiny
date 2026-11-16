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
# Ensure required GRUB BIOS tools are present (grub2-install, grub2-mkrescue)
if ! command -v grub2-install >/dev/null 2>&1; then
    echo "grub2-install not found, attempting to install grub2-pc..."
    if command -v sudo >/dev/null 2>&1; then
        sudo dnf install -y grub2-pc || { echo "Failed to install grub2-pc"; exit 1; }
    else
        dnf install -y grub2-pc || { echo "Failed to install grub2-pc"; exit 1; }
    fi
fi

# Create a bootable virtual hard disk (ext3) if it does not exist
VHD_BOOT="${REPO_ROOT}/bare-mc/vhd_boot.img"
if [[ ! -f "$VHD_BOOT" ]]; then
    echo "== Creating bootable virtual disk $VHD_BOOT"
    # 256 MiB raw image
    qemu-img create -f raw "$VHD_BOOT" 256M
    # Partition it with an ext3 partition starting at 1 MiB
    parted -s "$VHD_BOOT" mklabel msdos
    parted -s "$VHD_BOOT" mkpart primary ext3 1MiB 100%
    # Mark the partition as bootable so BIOS will attempt to boot it
    parted -s "$VHD_BOOT" set 1 boot on
    # Set up loop device with partition scanning
    LOOPDEV=$(losetup -f --show -P "$VHD_BOOT")
    # Format the primary partition as ext3
    mkfs.ext3 "${LOOPDEV}p1"
    # Mount the partition
    MNT_DIR=$(mktemp -d)
    mount "${LOOPDEV}p1" "$MNT_DIR"
    # Install kernel and initramfs into /boot
    mkdir -p "$MNT_DIR/boot/grub"
    cp "${REPO_ROOT}/bare-mc/kernel.bin" "$MNT_DIR/boot/kernel.bin"
    cp "${REPO_ROOT}/bare-mc/initramfs.tar" "$MNT_DIR/boot/initramfs.tar"
    # Write a minimal GRUB config that loads the kernel and initramfs
    cat > "$MNT_DIR/boot/grub/grub.cfg" <<'EOF'
# Ensure GRUB menu is displayed
timeout_style=menu
set timeout=3
menuentry "Bare MC" {
    multiboot2 /boot/kernel.bin
    module2 /boot/initramfs.tar type=tar
}
EOF
    # Install GRUB onto the disk (BIOS mode)
    grub2-install --target=i386-pc --boot-directory="$MNT_DIR/boot" "$LOOPDEV"
    # Cleanup
    umount "$MNT_DIR"
    losetup -d "$LOOPDEV"
    rm -rf "$MNT_DIR"
fi
# Launch QEMU, booting from the virtual hard disk
qemu-system-x86_64 -hda "$VHD_BOOT" -boot c -m 512 -display gtk -net none
