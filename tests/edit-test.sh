#!/usr/bin/env bash
# Build and run the portable editor core self-test on Linux.
# The same editor_core.c is later compiled into the grub-os kernel.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p _build
gcc tests/edit-linux.c grub-os/editor_core.c -I grub-os -o _build/edit-test
exec _build/edit-test "$@"
