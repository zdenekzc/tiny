#!/bin/bash
# mkshare.sh — builds _build/share.img (ext2) from the contents of the _build/push/ directory.
#
#   ./mkshare.sh                 # image from _build/push/
#   SRC=/somewhere/else ./mkshare.sh
#
# The mke2fs options are not random — ToaruOS modules/ext2.c only handles 32-bit ext2
# (no 64bit / metadata_csum) and has no htree, hence no dir_index either. The inode
# size of 256 does not matter, the driver reads it from the superblock.
#
# NOTE: mke2fs -d writes files as sparse (it does not allocate zero blocks) and
# ToaruOS 2.3.2 cannot work with such a file — see fallocate below.
# That is why debugfs is mandatory, not optional.
set -u

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=${SRC:-${DIR}/_build/push}
IMG=${IMG:-${DIR}/_build/share.img}
MB=${MB:-64}
BS=2048    # block size; the same value goes to mke2fs -b and to fallocate

die() { echo "error: $*" >&2; exit 1; }

# e2fsprogs live in /sbin, which a non-root PATH usually does not contain (verified
# 2026-11-12: mke2fs is /usr/sbin/mke2fs, "command -v mke2fs" fails and the script
# refuses to work, even though it is installed). No sudo needed — the image is a file.
find_tool() {
    local t=$1 c
    c=$(command -v "$t" 2> /dev/null) && { echo "$c"; return 0; }
    for c in "/sbin/$t" "/usr/sbin/$t" "/usr/local/sbin/$t"; do
        [[ -x $c ]] && { echo "$c"; return 0; }
    done
    return 1
}

[[ -d "$SRC" ]] || die "not a directory with files: $SRC"
MKE2FS=$(find_tool mke2fs) || die "missing mke2fs (dnf install e2fsprogs / apt install e2fsprogs)"
DEBUGFS=$(find_tool debugfs) || die "missing debugfs (part of e2fsprogs)"
[[ $(find "$SRC" -type f | wc -l) -gt 0 ]] || die "$SRC is empty — nothing to transfer"

mkdir -p "$(dirname "$IMG")"
rm -f "$IMG"
truncate -s "${MB}M" "$IMG"
"$MKE2FS" -q -F -L tiny -t ext2 -O ^64bit,^metadata_csum,^dir_index \
    -b "$BS" -m 0 -d "$SRC" "$IMG" || die "mke2fs"

# ---------------------------------------------------------------------------
# Fill in the allocation of all blocks (against sparse files from mke2fs -d).
#
# ToaruOS 2.3.2 modules/ext2.c has two bugs that only show up with sparse
# files (measured 2026-11-13 on _output/mc):
#
#   * inode_read_block() checks the block index against inode->blocks/(BS/512).
#     But i_blocks only counts ALLOCATED blocks, whereas the block index goes by
#     the file size. In mc 118 blocks are allocated, yet the file has 242, so
#     everything from block 118 onwards comes back as zeros. Exactly this killed
#     miniCommander: the global `active_panel` lies in block 177, i.e. in the
#     "impossible" part of .data, was NULL after loading, and libc printf("%s")
#     then read from address 0xc (the offset of the path field in PanelProp) and
#     crashed with SIGSEGV.
#   * a hole in the file (block number 0) is read as block 0 (the boot block) instead
#     of zeros, so places that should be zero contain foreign data.
#
# debugfs fallocate physically allocates all blocks to every file; holes then
# do not exist and i_blocks covers the whole file size. Fallocate exists in
# e2fsprogs >= 1.45 (Fedora 42: 1.47).
# ---------------------------------------------------------------------------
CMDS=$(mktemp) || die "mktemp"
trap 'rm -f "$CMDS"' EXIT
: > "$CMDS"
while IFS= read -r -d '' f; do
    size=$(stat -c %s "$f" 2> /dev/null) || continue
    (( size > 0 )) || continue
    rel=${f#"$SRC"}
    printf 'fallocate "%s" 0 %d\n' "$rel" $(( (size + BS - 1) / BS )) >> "$CMDS"
done < <(find "$SRC" -type f -print0)

if ! "$DEBUGFS" -w -f "$CMDS" "$IMG" > /dev/null 2> "${CMDS}.err"; then
    cat "${CMDS}.err" >&2
    rm -f "${CMDS}.err"
    die "debugfs fallocate failed (old e2fsprogs? You need >= 1.45)"
fi
rm -f "${CMDS}.err"
echo "fallocate: $(wc -l < "$CMDS") files, all blocks allocated"

# Check on the largest file: i_blocks must cover its whole size.
big=$(find "$SRC" -type f -printf '%s %p\n' | sort -n | tail -1)
size=${big%% *}; path=${big#* }
if [[ -n $path && ${size:-0} -gt 0 ]]; then
    want=$(( (size + BS - 1) / BS * (BS / 512) ))          # data sectors
    got=$("$DEBUGFS" -R "stat \"${path#"$SRC"}\"" "$IMG" 2> /dev/null \
          | sed -n 's/.*Blockcount: *\([0-9]*\).*/\1/p')
    if [[ -z $got ]] || (( got < want )); then
        die "sparse check: ${path#"$SRC"} has ${got:-?} sectors, expected >= $want"
    fi
    echo "check: ${path#"$SRC"} has $got sectors (>= $want)"
fi

echo "image: $IMG ($(wc -c < "$IMG") B)"
"$DEBUGFS" -R 'ls -l /' "$IMG" 2>/dev/null | tail -n +2
