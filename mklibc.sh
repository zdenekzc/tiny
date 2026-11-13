#!/bin/bash
# mklibc.sh — ToaruOS libc for static linking on the host machine:
#   _build/toaru-lib/       crt0.o crti.o crtn.o libc.a (from /lib of the ToaruOS base system)
#   _build/toaru-include/   ToaruOS /usr/include headers
#
#   ./mklibc.sh                 from _build/download/toaruos-2.3.2.iso
#   ISO=/path ./mklibc.sh
#
# The files are directly in the ISO (no package from msk): ISO 9660 => fat.img (FAT12) =>
# ramdisk.igz (gzip) => root filesystem tar => lib/.  Needs 7z (p7zip).
set -euo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ISO=${ISO:-${DIR}/_build/download/toaruos-2.3.2.iso}
OUT=${OUT:-${DIR}/_build/toaru-lib}
INC=${INC:-${DIR}/_build/toaru-include}

die() { echo "error: $*" >&2; exit 1; }
[[ -f $ISO ]] || die "missing ISO: $ISO"
command -v 7z > /dev/null || die "missing 7z (dnf install p7zip p7zip-plugins)"

TMP=$(mktemp -d) || die "mktemp"
trap 'rm -rf "$TMP"' EXIT
7z x -y -o"$TMP" "$ISO" fat.img > /dev/null || die "7z: fat.img from $ISO"
7z x -y -o"$TMP" "$TMP/fat.img" ramdisk.igz > /dev/null || die "7z: ramdisk.igz from fat.img"
mkdir -p "$TMP/root"
gzip -dc < "$TMP/ramdisk.igz" | tar xf - -C "$TMP/root" lib/crt0.o lib/crti.o lib/crtn.o lib/libc.a usr/include \
   || die "ramdisk: lib/crt0.o crti.o crtn.o libc.a, usr/include"
mkdir -p "$OUT"
cp "$TMP"/root/lib/crt0.o "$TMP"/root/lib/crti.o "$TMP"/root/lib/crtn.o "$TMP"/root/lib/libc.a "$OUT/"
rm -rf "$INC"
cp -a "$TMP/root/usr/include" "$INC"
echo "toaru-lib: $OUT ($(cd "$OUT" && ls | tr '\n' ' '))"
echo "toaru-include: $INC ($(find "$INC" -name '*.h' | wc -l) headers)"
