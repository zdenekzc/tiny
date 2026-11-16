#include "ext2.h"
#include <string.h>

extern void ata_read28(uint32_t lba, uint8_t *buf);
extern void ata_write28(uint32_t lba, const uint8_t *buf);

static uint32_t part_lba;
static Ext2Super super;
static uint32_t block_size;
static Ext2GroupDesc group0;

static void read_block(uint32_t blk, uint8_t *buf) {
    uint32_t lba = part_lba + (blk * (block_size / 512));
    for (uint32_t i = 0; i < block_size / 512; i++)
        ata_read28(lba + i, buf + i*512);
}

static void write_block(uint32_t blk, const uint8_t *buf) {
    uint32_t lba = part_lba + (blk * (block_size / 512));
    for (uint32_t i = 0; i < block_size / 512; i++)
        ata_write28(lba + i, buf + i*512);
}

int ext2_mount(uint32_t plba) {
    part_lba = plba;

    uint8_t buf[1024];
    ata_read28(part_lba + 2, buf); /* superblock at 1024 */
    memcpy(&super, buf, sizeof(Ext2Super));
    if (super.magic != 0xEF53) return 0;

    block_size = 1024U << super.log_block_size;

    uint8_t gbuf[4096];
    read_block(2, gbuf);
    memcpy(&group0, gbuf, sizeof(Ext2GroupDesc));
    return 1;
}

int ext2_read_inode(uint32_t ino, Ext2Inode *out) {
    if (!ino) return 0;
    ino--;

    uint32_t inodes_per_block = block_size / super.inode_size;
    uint32_t group = ino / super.inodes_per_group;
    uint32_t index = ino % super.inodes_per_group;
    if (group != 0) return 0;

    uint32_t table_block = group0.inode_table;
    uint32_t blk = table_block + (index / inodes_per_block);
    uint32_t off = (index % inodes_per_block) * super.inode_size;

    uint8_t buf[4096];
    read_block(blk, buf);
    memcpy(out, buf + off, sizeof(Ext2Inode));
    return 1;
}

static int read_block_from_inode(const Ext2Inode *ino, uint32_t file_block_index, uint8_t *buf) {
    if (file_block_index < 12) {
        uint32_t blk = ino->block[file_block_index];
        if (!blk) return 0;
        read_block(blk, buf);
        return 1;
    }
    file_block_index -= 12;
    uint32_t indir_blk = ino->block[12];
    if (!indir_blk) return 0;

    uint32_t ptrs_per_block = block_size / 4;
    if (file_block_index >= ptrs_per_block) return 0;

    uint8_t ibuf[4096];
    read_block(indir_blk, ibuf);
    uint32_t *ptrs = (uint32_t*)ibuf;
    uint32_t blk = ptrs[file_block_index];
    if (!blk) return 0;

    read_block(blk, buf);
    return 1;
}

int ext2_read_file(uint32_t ino_num, uint8_t *buf, uint32_t maxlen, uint32_t *out_size) {
    Ext2Inode ino;
    if (!ext2_read_inode(ino_num, &ino)) return 0;

    uint32_t size = ino.size;
    if (size > maxlen) size = maxlen;
    if (out_size) *out_size = size;

    uint32_t block_count = (size + block_size - 1) / block_size;
    uint8_t blkbuf[4096];
    uint32_t copied = 0;

    for (uint32_t i = 0; i < block_count; i++) {
        if (!read_block_from_inode(&ino, i, blkbuf)) break;
        uint32_t chunk = block_size;
        if (copied + chunk > size) chunk = size - copied;
        memcpy(buf + copied, blkbuf, chunk);
        copied += chunk;
    }
    return 1;
}

int ext2_write_file_overwrite(uint32_t ino_num, const uint8_t *buf, uint32_t size) {
    Ext2Inode ino;
    if (!ext2_read_inode(ino_num, &ino)) return 0;

    if (size > ino.size) size = ino.size;

    uint32_t block_count = (size + block_size - 1) / block_size;
    uint8_t blkbuf[4096];
    uint32_t written = 0;

    for (uint32_t i = 0; i < block_count; i++) {
        uint32_t chunk = block_size;
        if (written + chunk > size) chunk = size - written;

        if (!read_block_from_inode(&ino, i, blkbuf)) return 0;
        memcpy(blkbuf, buf + written, chunk);

        uint32_t blk;
        if (i < 12) {
            blk = ino.block[i];
        } else {
            uint32_t idx = i - 12;
            uint8_t ibuf[4096];
            read_block(ino.block[12], ibuf);
            uint32_t *ptrs = (uint32_t*)ibuf;
            blk = ptrs[idx];
        }
        write_block(blk, blkbuf);
        written += chunk;
    }
    return 1;
}

int ext2_list_dir(uint32_t ino_num,
                  void (*cb)(const char *name, uint8_t type, uint32_t ino, void *user),
                  void *user) {
    Ext2Inode ino;
    if (!ext2_read_inode(ino_num, &ino)) return 0;
    if ((ino.mode & 0xF000) != 0x4000) return 0;

    uint8_t blkbuf[4096];
    uint32_t size = ino.size;
    uint32_t block_count = (size + block_size - 1) / block_size;

    for (uint32_t i = 0; i < block_count; i++) {
        if (!read_block_from_inode(&ino, i, blkbuf)) break;

        uint32_t off = 0;
        while (off < block_size) {
            Ext2DirEnt *de = (Ext2DirEnt*)(blkbuf + off);
            if (!de->inode || !de->rec_len) break;

            if (de->name_len > 0) {
                char name[256];
                uint8_t n = de->name_len;
                if (n >= sizeof(name)) n = sizeof(name)-1;
                memcpy(name, de->name, n);
                name[n] = 0;
                cb(name, de->file_type, de->inode, user);
            }

            off += de->rec_len;
        }
    }
    return 1;
}
