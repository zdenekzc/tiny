# ext2: sparse files are read incorrectly (two independent defects)

> **Status (2026-11-13):** `klange/toaruos` was archived by its owner on
> 2026-11-09 and is read-only, so GitHub no longer accepts new issues there.
> This report is kept here as a reference until an upstream tracker exists again.

## Summary

`modules/ext2.c` reads **sparse files** (files with unallocated holes)
incorrectly. Two independent defects combine:

1. `inode_read_block()` (line 602) compares the logical block index against
   `inode->blocks / (block_size / 512)`. `i_blocks` counts **allocated 512-byte
   sectors** (data + indirect blocks), not logical file blocks. For a sparse
   file it is smaller than the file's block count, so every logical block past
   that bound is returned as zeros — even blocks that *are* allocated and
   contain data.
2. For a hole, `get_block_number()` returns 0, `read_block(…, 0, buf)` returns
   `E_BADBLOCK` **without writing to `buf`**, and `inode_read_block()` ignores
   the error. `read_ext2()` reuses one heap buffer for all blocks of the read,
   so a hole returns the **stale contents of the previously read block**
   (or uninitialized heap data if the first block of the range is a hole).

Ordinary files written by ToaruOS are dense (blocks are allocated on write),
so the bugs stay latent. They trigger on images produced on another system
with `mke2fs -d`, `tar --sparse`, `cp --sparse=always`, etc. — a common way
to prepare a share/disk image for a VM.

## Tested versions

- ToaruOS 2.3.2 ISO (`toaruos-2.3.2.iso`, x86_64) and source tag `v2.3.2`.
- The same code is still present on `master` as of 2026-11-13
  (`modules/ext2.c`, `inode_read_block()` at line 600, check at line 602).

## Reproduction

### 1. Host: create an image containing one sparse file

```sh
mkdir -p /tmp/repro/src && cd /tmp/repro

python3 - <<'PY'
data = bytearray(300000)                      # all zeroes = holes
data[0:64] = b"BEGIN_AT_0" + b" " * 54 + b"\n"
off = 280000                                  # block 136 at bs=2048
magic = b"MAGIC_BEYOND_ALLOCATION\n"
data[off:off+len(magic)] = magic
open("src/sparse.txt", "wb").write(data)
PY

truncate -s 8M sparse.img
mke2fs -q -F -t ext2 -O ^64bit,^metadata_csum,^dir_index \
    -b 2048 -L SPARSETEST -m 0 -d src sparse.img
```

Host-side facts about the file (`debugfs`):

```
Size: 300001                    -> 147 logical blocks (block size 2048)
Links: 1   Blockcount: 12       -> 12 sectors = 3 filesystem blocks allocated
                                   (2 data blocks + 1 indirect block)
bmap /sparse.txt   0 -> 273     (allocated)
bmap /sparse.txt   1 -> 0       (hole)
bmap /sparse.txt   2 -> 0       (hole)
bmap /sparse.txt 136 -> 275     (allocated, contains the magic string)
```

The host file itself is correct: `grep -a MAGIC_BEYOND_ALLOCATION src/sparse.txt`
matches.

### 2. Guest: read the file back

Boot ToaruOS 2.3.2 with `sparse.img` as an IDE disk, then:

```sh
mkdir -p /share
insmod /mod/ext2.ko
mount ext2 /dev/hda /share

wc -c /share/sparse.txt                                          # 300001
hexify /share/sparse.txt | grep BEGIN_AT_0
hexify /share/sparse.txt | grep MAGIC_BEYOND_ALLOCATION
```

(`grep` on the file directly cannot be used: ToaruOS `grep` treats lines as
C strings and stops at the first NUL. `hexify` is in the base system and its
output is plain text, so piping it into `grep` works.)

### 3. Observed result

```
$ hexify /share/sparse.txt | grep BEGIN_AT_0
00000000: ... BEGIN_AT_0 ...      <- correct
00000800: ... BEGIN_AT_0 ...      <- hole block 1: duplicate of block 0
00001000: ... BEGIN_AT_0 ...      <- hole block 2: duplicate of block 0

$ hexify /share/sparse.txt | grep MAGIC_BEYOND_ALLOCATION
(nothing)                          <- block 136 lost, reads as zeros
```

Expected: `BEGIN_AT_0` only at offset 0, zeros at 0x800 and 0x1000, and the
magic string at offset 0x445c0 (280000).

## Root cause

### Defect 1 — wrong bound check

`modules/ext2.c:600` (`v2.3.2`):

```c
static unsigned int inode_read_block(ext2_fs_t * this, ext2_inodetable_t * inode,
                                     unsigned int block, uint8_t * buf) {
    if (block >= inode->blocks / (this->block_size / 512)) {
        memset(buf, 0x00, this->block_size);
        debug_print(WARNING, "Tried to read an invalid block. ...");
        return 0;
    }

    unsigned int real_block = get_block_number(this, inode, block);
    read_block(this, real_block, buf);
    return real_block;
}
```

`block` is a **logical** block index (0 … ceil(size / block_size) − 1).
`inode->blocks` is the ext2 `i_blocks` field: the number of **allocated
512-byte sectors**, including indirect blocks. For the file above the bound is
12 / 4 = 3, while the file has 147 logical blocks. As a result, blocks 3–146
are rejected as “invalid”, including allocated blocks such as 136.

The check should be against the file size, not against `i_blocks`.

### Defect 2 — holes are not zero-filled

`get_block_number()` (`modules/ext2.c:424`) simply returns
`inode->block[iblock]` for direct blocks, which is 0 for a hole.
`read_block()` (`modules/ext2.c:263`) rejects block 0:

```c
static int read_block(ext2_fs_t * this, unsigned int block_no, uint8_t * buf) {
    /* 0 is an invalid block number. ... */
    if (!block_no) {
        return E_BADBLOCK;
    }
    read_fs(this->block_device, block_no * this->block_size,
            this->block_size, (uint8_t *)buf);
    return E_SUCCESS;
}
```

It returns before touching `buf`; `inode_read_block()` ignores the return
value. `read_ext2()` (`modules/ext2.c:1216`) allocates **one** buffer and
reuses it across the block loop (`buf` passed to every `inode_read_block()`
call), so a hole yields whatever the previous block left in the buffer.
That is the duplicated `BEGIN_AT_0` at offsets 0x800 and 0x1000 above.

In our real case the first block of a multi-block read could also be a hole,
in which case uninitialized heap memory is copied to userspace.

## Suggested fix

```diff
 static unsigned int inode_read_block(ext2_fs_t * this, ext2_inodetable_t * inode,
                                      unsigned int block, uint8_t * buf) {
-    if (block >= inode->blocks / (this->block_size / 512)) {
+    unsigned int file_blocks = inode->size
+        ? (inode->size - 1) / this->block_size + 1 : 0;
+
+    if (block >= file_blocks) {
         memset(buf, 0x00, this->block_size);
         debug_print(WARNING, "Tried to read an invalid block. ...");
         return 0;
     }

     unsigned int real_block = get_block_number(this, inode, block);
+    if (real_block == 0) {
+        /* Hole: must read as zeroes. */
+        memset(buf, 0x00, this->block_size);
+        return 0;
+    }
+
     read_block(this, real_block, buf);
     return real_block;
 }
```

`inode_write_block()` (line 618) uses the same `i_blocks`-based expression in
its grow loop and should be reviewed as well. Alternatively, `read_ext2()`
could zero its buffer on a hole, but the central fix in `inode_read_block()`
covers all callers (file reads, directory reads, indirect-block walks).

## Impact (real-world case)

We hit this with a 64 MiB ext2 share image built by `mke2fs -d`:

- a 495 011-byte program (242 logical blocks) got `i_blocks = 472` sectors →
  bound 118; blocks 118–241 were all read as zeros even though some were
  allocated;
- logical blocks 114–117 were holes → duplicated block 113 (`.data` garbage);
- a global pointer stored in block 177 was read as NULL, and libc later
  dereferenced address 0xc in `snprintf("%s")` → SIGSEGV in `xvasprintf()`.

The crash looked like a compiler or libc bug; the binary was byte-identical on
the host. Any program whose image contains sparse files is at risk of reading
silently wrong `.text`/`.data`/`.rodata`.

## Workaround

For host-built images, allocate every block of every file after `mke2fs -d`
(`e2fsprogs >= 1.45`):

```sh
BS=2048
CMDS=$(mktemp)
find "$SRC" -type f -print0 | while IFS= read -r -d '' f; do
    size=$(stat -c %s "$f") || continue
    (( size )) || continue
    rel=${f#"$SRC"}
    printf 'fallocate "%s" 0 %d\n' "$rel" $(( (size + BS - 1) / BS )) >> "$CMDS"
done
debugfs -w -f "$CMDS" image.img
```

The newly allocated blocks read as zeroes, which is exactly the semantic value
of the holes, so the file contents do not change — only the holes disappear and
both defects stop triggering.
