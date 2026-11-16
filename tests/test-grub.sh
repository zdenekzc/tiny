#!/usr/bin/env bash
# Test script for the grub-os Mini Commander built with GRUB.
# Builds the OS using the makefile in grub-os/, creates an ISO, and boots with QEMU.

set -euo pipefail

# Repository root
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# Directory containing the grub-os sources
SRC_DIR="${REPO_ROOT}/grub-os"

# All build artifacts go here (outside the source tree)
BUILD_DIR="${REPO_ROOT}/_build/grub-os"

# Build the OS ISO using the provided makefile.
echo "== Building OS ISO with ${SRC_DIR}"
make -C "$SRC_DIR" iso

# The resulting ISO is placed in the source directory by the makefile.
ISO="${BUILD_DIR}/twopane_ext2.iso"  # produced by `make iso` in grub-os/

if [[ ! -f "$ISO" ]]; then
    echo "Error: ISO not found at $ISO"
    exit 1
fi

# Create a small ext2 disk with sample files for the mini commander (once).
DISK="${BUILD_DIR}/disk.img"
if [[ ! -f "$DISK" ]]; then
    mkdir -p "$BUILD_DIR"
    echo "== Creating ext2 test disk with sample files"
    CONTENT_DIR=$(mktemp -d)
    mkdir -p "$CONTENT_DIR/docs"
    echo "Mini Commander test disk" > "$CONTENT_DIR/README.TXT"
    echo "Hello from ext2!"       > "$CONTENT_DIR/hello.txt"
    printf 'line1\nline2\nline3\n' > "$CONTENT_DIR/notes.txt"
    echo "document in docs"        > "$CONTENT_DIR/docs/doc1.txt"
    mke2fs -q -F -t ext2 -b 1024 -I 128 \
        -O ^resize_inode,^extents,^has_journal,^64bit,^flex_bg,^metadata_csum \
        -d "$CONTENT_DIR" "$DISK" 8M
    rm -rf "$CONTENT_DIR"
fi

echo "== Launching QEMU with $ISO"
# Launch QEMU, booting from the ISO (GRUB menu). Display is disabled; serial output is shown.
qemu-system-i386 -cdrom "$ISO" -drive file="$DISK",format=raw,if=ide -boot d -serial stdio -monitor none &
QEMU_PID=$!
# Allow time for GRUB and the kernel to run.
sleep 30
kill $QEMU_PID 2>/dev/null || true

echo "=== QEMU session ended ==="
