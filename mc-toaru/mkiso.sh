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
# patched mc from _build/output/mc as /bin/mc, copies everything from _build/push
# into /extras inside the ramdisk (never into / — files on / would clutter
# the desktop icons), repacks the ramdisk, rebuilds fat.img (kernel +
# ramdisk + EFI loader) and finally creates the ISO.
#
# Besides the ISO it also writes two artifacts for booting WITHOUT the ISO:
#
#   _build/ramdisk.tgz   the repacked ramdisk (same bytes as ramdisk.igz,
#                        friendly name: it is an ordinary gzipped tar, so
#                        "tar tzf" and file managers can open it directly)
#   _build/kernel.mb     the stock kernel with the multiboot1 "request
#                        graphics mode" flag cleared.  QEMU's built-in
#                        multiboot loader refuses to load a kernel whose
#                        header asks for VBE ("multiboot knows VBE. we
#                        don't"), so without the patch "qemu -kernel"
#                        silently drops to the BIOS and hangs.
#
# Boot straight from the files (no ISO, no genisoimage needed):
#   INITRD=_build/ramdisk.tgz ./scripts/toaru-vm.sh start
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
# The ISO is downloaded into _import/download by tests/run-toaru.sh or
# tests/build-toaru.sh; the download is shared with everything else.
if [[ -z ${ISO_IN:-} ]]; then
    ISO_IN=$TOP/_import/download/image.iso
fi
MC_BIN=${MC_BIN:-$TOP/_build/output/mc}
SRC_EXTRA=${SRC_EXTRA:-$TOP/_build/push}
EXTRA_DIR=${EXTRA_DIR:-extras}   # payload subdirectory in the ramdisk; files must
                                 # NOT go to / — they would clutter the desktop icons
OUT=${1:-$TOP/_build/custom.iso}
mkdir -p "$TOP/_tmp" "$TOP/_build"
WORK=$(mktemp -d "$TOP/_tmp/mkiso.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

die() { echo "error: $*" >&2; exit 1; }
. "$TOP/scripts/need.sh"   # need(): dependency check with package-install hints

need isoinfo
need genisoimage
need mkfs.fat
need mmd
need mcopy
need tar
need gzip
need du
need fallocate
need python3

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

# mc on PATH: a symlink to the payload binary in /extras (saves a 500 kB
# copy; argv[0] is only used for the usage message).
echo "== linking mc as /bin/mc"
ln -s /extras/mc "$WORK/ramdisk/bin/mc"

# tcc on PATH: a symlink to the sysroot-carrying binary in /extras/tcc (the
# compiled-in CONFIG_TCCDIR is /extras/tcc, so the symlink needs no -B).
echo "== linking tcc as /bin/tcc"
ln -s /extras/tcc/bin/tcc "$WORK/ramdisk/bin/tcc"

# tiny-ide on PATH: same trick as mc (the payload binary stays in /extras).
# Only when the payload carries it — a missing binary would leave a dangling
# /bin/tiny-ide symlink behind.
if [[ -f "$SRC_EXTRA/tiny-ide" ]]; then
    echo "== linking tiny-ide as /bin/tiny-ide"
    ln -s /extras/tiny-ide "$WORK/ramdisk/bin/tiny-ide"
fi

if [[ -d "$SRC_EXTRA" ]]; then
    echo "== copying payload from $SRC_EXTRA into /$EXTRA_DIR"
    mkdir -p "$WORK/ramdisk/$EXTRA_DIR"
    cp -a "$SRC_EXTRA"/. "$WORK/ramdisk/$EXTRA_DIR/"
fi

echo "== repacking ramdisk.igz"
(
    cd "$WORK/ramdisk"
    # The live image belongs to the "local" user (uid/gid 1000); only a few
    # symlinks are root-owned in the stock ramdisk.  Forcing owner 0 here
    # made /home/local (mode 750) inaccessible to the session, so the desktop
    # could not chdir to ~/Desktop and fell back to showing "/" as icons.
    find . -mindepth 1 -maxdepth 1 -printf '%P\0' |
        tar --null --owner=1000 --group=1000 -cf - -T -
) | gzip -9 -n > "$WORK/base/ramdisk.igz"

echo "== rebuilding fat.img"
mcopy -i "$WORK/base/fat.img" ::/efi/boot/bootx64.efi "$WORK/fatbase/efi/boot/bootx64.efi"
cp "$WORK/base/kernel" "$WORK/fatbase/kernel"
cp "$WORK/base/ramdisk.igz" "$WORK/fatbase/ramdisk.igz"

SPACE_REQ=$(du -s -B 2048 "$WORK/fatbase" | cut -f 1)
# FAT12 tops out around 4084 clusters (the original 7 MB image), so force
# FAT16 — but mkfs.fat refuses FAT16 below 4085 clusters, which a small
# payload (no toaruos-src) drops below.  Clamp to a safe minimum, keep a
# margin for the two FAT copies and the root directory, and switch to FAT32
# if the payload ever outgrows FAT16.
CLUSTERS=$(( SPACE_REQ + 64 ))
(( CLUSTERS < 4200 )) && CLUSTERS=4200
FAT_BITS=16
(( CLUSTERS > 65000 )) && FAT_BITS=32
SIZE=$(( CLUSTERS * 2048 ))
fallocate -l "$SIZE" "$WORK/fat.img"
mkfs.fat -F "$FAT_BITS" -s 1 -S 2048 "$WORK/fat.img" >/dev/null
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

# Artifacts for the ISO-less boot path: qemu -kernel kernel.mb -initrd ramdisk.tgz.
echo "== writing $TOP/_build/ramdisk.tgz and $TOP/_build/kernel.mb"
cp "$WORK/base/ramdisk.igz" "$TOP/_build/ramdisk.tgz"
python3 -I - "$WORK/base/kernel" "$TOP/_build/kernel.mb" <<'PY'
import struct, sys
data = bytearray(open(sys.argv[1], "rb").read())
idx = data.find(struct.pack("<I", 0x1BADB002))
if idx < 0:
    sys.exit("multiboot1 header not found in the kernel")
magic, flags, checksum = struct.unpack_from("<III", data, idx)
# bit 2 = "request graphics mode (VBE)"; QEMU's multiboot loader refuses
# such a kernel, the stock boot.sys handles graphics itself and does not
# need the request either.
flags &= ~0x4
checksum = (-(magic + flags)) & 0xFFFFFFFF
struct.pack_into("<III", data, idx, magic, flags, checksum)
open(sys.argv[2], "wb").write(bytes(data))
PY

echo "done: $OUT ($(stat -c%s "$OUT") B)"
echo "boot in QEMU:"
echo "  VISIBLE=0 HEADLESS=1 ISO=$OUT $TOP/toaru-vm.sh start"
echo "then:"
echo "  $TOP/toaru-vm.sh send 'mc /'"
