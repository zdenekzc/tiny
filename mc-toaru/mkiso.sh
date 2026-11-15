#!/bin/bash
# mkiso.sh — build a bootable ToaruOS ISO whose payload travels INSIDE the
# ramdisk (ramdisk.igz), not in the ISO root and not on a mounted disk.
#
#   ./mc-toaru/mkiso.sh                 # -> _build/custom.iso
#   ./mc-toaru/mkiso.sh /tmp/test.iso   # explicit output
#   SRC_EXTRA=/path ./mc-toaru/mkiso.sh # extra files copied into the ramdisk root
#
# The source ISO is the unmodified ToaruOS 2.3.2 release ISO.  The script
# extracts it, unpacks ramdisk.igz (a gzip-compressed tar), installs the
# patched mc from _output/mc as /bin/mc, copies everything from _build/push
# into /extras inside the ramdisk (never into / — files on / would clutter
# the desktop icons), repacks the ramdisk, rebuilds fat.img (kernel +
# ramdisk + EFI loader) and finally creates the ISO.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
ISO_IN=${ISO_IN:-$TOP/_build/download/toaruos-2.3.2.iso}
MC_BIN=${MC_BIN:-$TOP/_output/mc}
SRC_EXTRA=${SRC_EXTRA:-$TOP/_build/push}
EXTRA_DIR=${EXTRA_DIR:-extras}   # payload subdirectory in the ramdisk; files must
                                 # NOT go to / — they would clutter the desktop icons
OUT=${1:-$TOP/_build/custom.iso}
WORK=$(mktemp -d "$TOP/_tmp/mkiso.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

die() { echo "error: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing program '$1'"; }

need isoinfo
need genisoimage
need mkfs.fat
need mmd
need mcopy
need tar
need gzip
need du
need fallocate

[[ -f "$ISO_IN" ]] || die "missing source ISO: $ISO_IN"
[[ -x "$MC_BIN" ]] || die "missing patched mc: $MC_BIN (run ./mc-toaru/build.sh mc)"

mkdir -p "$WORK/base" "$WORK/ramdisk" "$WORK/fatbase/efi/boot"

echo "== extracting $ISO_IN"
# 7z (p7zip 16.02) reads kernel and ramdisk.igz as zero-length files from this
# ISO; isoinfo with Rock Ridge returns the real data.
while IFS= read -r path; do
    [[ -z "$path" || "$path" == "/" ]] && continue
    rel=${path#/}
    mkdir -p "$WORK/base/$(dirname "$rel")"
    if isoinfo -R -i "$ISO_IN" -x "$path" > "$WORK/.extract.tmp" 2>/dev/null && [[ -s "$WORK/.extract.tmp" ]]; then
        mv "$WORK/.extract.tmp" "$WORK/base/$rel"
    else
        rm -f "$WORK/.extract.tmp"
        mkdir -p "$WORK/base/$rel"
    fi
done < <(isoinfo -R -f -i "$ISO_IN")

echo "== unpacking ramdisk.igz"
gzip -dc "$WORK/base/ramdisk.igz" | tar -xf - -C "$WORK/ramdisk"

echo "== installing mc as /bin/mc"
install -m 755 "$MC_BIN" "$WORK/ramdisk/bin/mc"

if [[ -d "$SRC_EXTRA" ]]; then
    echo "== copying payload from $SRC_EXTRA into /$EXTRA_DIR"
    mkdir -p "$WORK/ramdisk/$EXTRA_DIR"
    cp -a "$SRC_EXTRA"/. "$WORK/ramdisk/$EXTRA_DIR/"
fi

echo "== repacking ramdisk.igz"
(
    cd "$WORK/ramdisk"
    find . -mindepth 1 -maxdepth 1 -printf '%P\0' |
        tar --null --owner=0 --group=0 -cf - -T -
) | gzip -9 -n > "$WORK/base/ramdisk.igz"

echo "== rebuilding fat.img"
mcopy -i "$WORK/base/fat.img" ::/efi/boot/bootx64.efi "$WORK/fatbase/efi/boot/bootx64.efi"
cp "$WORK/base/kernel" "$WORK/fatbase/kernel"
cp "$WORK/base/ramdisk.igz" "$WORK/fatbase/ramdisk.igz"

SPACE_REQ=$(du -s -B 2048 "$WORK/fatbase" | cut -f 1)
# +25 sectors was enough for the original 7 MB FAT12 image; with a bigger
# ramdisk the image crosses the FAT12 limit, so force FAT16 and leave a
# larger margin for the two FAT copies and the root directory.
SIZE=$(( (SPACE_REQ + 64) * 2048 ))
fallocate -l "$SIZE" "$WORK/fat.img"
mkfs.fat -F 16 -s 1 -S 2048 "$WORK/fat.img" >/dev/null
mmd -i "$WORK/fat.img" ::/efi
mmd -i "$WORK/fat.img" ::/efi/boot
mcopy -i "$WORK/fat.img" "$WORK/fatbase/kernel" ::/kernel
mcopy -i "$WORK/fat.img" "$WORK/fatbase/ramdisk.igz" ::/ramdisk.igz
mcopy -i "$WORK/fat.img" "$WORK/fatbase/efi/boot/bootx64.efi" ::/efi/boot/bootx64.efi
cp "$WORK/fat.img" "$WORK/base/fat.img"

echo "== creating $OUT"
genisoimage -V TOARUOS -R -J \
    -b boot.sys -c bootcat -no-emul-boot -boot-load-size 44 \
    -eltorito-alt-boot -e fat.img -no-emul-boot \
    -o "$OUT" "$WORK/base" >/dev/null

echo "done: $OUT ($(stat -c%s "$OUT") B)"
echo "boot in QEMU:"
echo "  VISIBLE=0 HEADLESS=1 ISO=$OUT $TOP/toaru-vm.sh start"
echo "then:"
echo "  $TOP/toaru-vm.sh send 'mc /'"
