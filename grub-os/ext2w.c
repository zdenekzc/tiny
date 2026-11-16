#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ext2.h"
#include "ext2w.h"

extern void serial_puts(const char *s);
extern void serial_put_dec(uint32_t v);

/* on-disk directory entry header (duplicate of the one in ext2.c internals) */
typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed)) WDirEnt;

static uint32_t bs;

static int ready(void) {
    if (!bs) bs = ext2_block_size();
    return bs != 0;
}

/* ---- bitmap helpers ---- */

static int bm_test(const uint8_t *bm, uint32_t idx) {
    return (bm[idx >> 3] >> (idx & 7)) & 1;
}

static void bm_set(uint8_t *bm, uint32_t idx, int v) {
    if (v) bm[idx >> 3] |= (uint8_t)(1u << (idx & 7));
    else   bm[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
}

uint32_t ext2w_alloc_block(void) {
    if (!ready()) return 0;
    Ext2Super *sb = ext2_super_ptr();
    Ext2GroupDesc *g = ext2_group0_ptr();
    if (!sb->free_blocks) return 0;

    static uint8_t bm[4096];
    ext2_read_block(g->block_bitmap, bm);
    for (uint32_t i = 0; i < sb->blocks_count; i++) {
        if (!bm_test(bm, i)) {
            bm_set(bm, i, 1);
            ext2_write_block(g->block_bitmap, bm);
            sb->free_blocks--;
            g->free_blocks_count--;
            ext2_flush_meta();
            /* bitmap bit i corresponds to block i + first_data_block */
            return i + sb->first_data_block;
        }
    }
    return 0;
}

void ext2w_free_block(uint32_t blk) {
    if (!ready() || !blk) return;
    Ext2Super *sb = ext2_super_ptr();
    Ext2GroupDesc *g = ext2_group0_ptr();
    if (blk < sb->first_data_block) return;

    static uint8_t bm[4096];
    ext2_read_block(g->block_bitmap, bm);
    uint32_t bit = blk - sb->first_data_block;
    if (!bm_test(bm, bit)) return;   /* already free */
    bm_set(bm, bit, 0);
    ext2_write_block(g->block_bitmap, bm);
    sb->free_blocks++;
    g->free_blocks_count++;
    ext2_flush_meta();
}

uint32_t ext2w_alloc_inode(void) {
    if (!ready()) return 0;
    Ext2Super *sb = ext2_super_ptr();
    Ext2GroupDesc *g = ext2_group0_ptr();
    if (!sb->free_inodes) return 0;

    static uint8_t bm[4096];
    ext2_read_block(g->inode_bitmap, bm);
    /* bitmap bit i corresponds to inode i+1 */
    for (uint32_t i = sb->first_ino - 1; i < sb->inodes_count; i++) {
        if (!bm_test(bm, i)) {
            bm_set(bm, i, 1);
            ext2_write_block(g->inode_bitmap, bm);
            sb->free_inodes--;
            g->free_inodes_count--;
            ext2_flush_meta();
            return i + 1;
        }
    }
    return 0;
}

void ext2w_free_inode(uint32_t ino) {
    if (!ready() || !ino) return;
    Ext2Super *sb = ext2_super_ptr();
    Ext2GroupDesc *g = ext2_group0_ptr();

    static uint8_t bm[4096];
    ext2_read_block(g->inode_bitmap, bm);
    uint32_t idx = ino - 1;
    if (!bm_test(bm, idx)) return;
    bm_set(bm, idx, 0);
    ext2_write_block(g->inode_bitmap, bm);
    sb->free_inodes++;
    g->free_inodes_count++;
    ext2_flush_meta();
}

/* ---- inode helpers ---- */

static int read_inode(uint32_t ino, Ext2Inode *out) {
    return ext2_read_inode(ino, out);
}

/* ---- directory entries ---- */

static uint32_t dirent_len(uint32_t namelen) {
    uint32_t l = 8 + namelen;
    return (l + 3) & ~3u;
}

int ext2w_add_dirent(uint32_t parent_ino, const char *name,
                     uint32_t ino, uint8_t file_type) {
    if (!ready()) return 0;
    uint32_t namelen = (uint32_t)strlen(name);
    if (namelen > 255) return 0;
    uint32_t needed = dirent_len(namelen);

    Ext2Inode parent;
    if (!read_inode(parent_ino, &parent)) return 0;
    if ((parent.mode & 0xF000) != 0x4000) return 0;

    uint32_t nb = (parent.size + bs - 1) / bs;
    uint8_t buf[4096];

    for (uint32_t b = 0; b < nb; b++) {
        /* direct blocks only for directory blocks (dirs stay small) */
        uint32_t blk = parent.block[b];
        if (!blk) continue;
        ext2_read_block(blk, buf);

        uint32_t off = 0;
        while (off < bs) {
            WDirEnt *de = (WDirEnt*)(buf + off);
            if (!de->rec_len) break;
            uint32_t rl = de->rec_len;
            uint32_t ideal = de->inode ? dirent_len(de->name_len) : 0;

            /* deleted entry with enough room */
            if (!de->inode && rl >= needed) {
                uint32_t rest = rl - needed;
                de->inode = ino;
                de->name_len = (uint8_t)namelen;
                de->file_type = file_type;
                memcpy(de->name, name, namelen);
                if (rest >= 8) {
                    de->rec_len = (uint16_t)needed;
                    WDirEnt *hole = (WDirEnt*)(buf + off + needed);
                    memset(hole, 0, 8);
                    hole->rec_len = (uint16_t)rest;
                }
                ext2_write_block(blk, buf);
                return 1;
            }

            /* slack at the end of a live entry */
            if (de->inode && rl - ideal >= needed) {
                uint32_t rest = rl - ideal;
                de->rec_len = (uint16_t)ideal;
                WDirEnt *ne = (WDirEnt*)(buf + off + ideal);
                ne->inode = ino;
                ne->rec_len = (uint16_t)rest;
                ne->name_len = (uint8_t)namelen;
                ne->file_type = file_type;
                memcpy(ne->name, name, namelen);
                ext2_write_block(blk, buf);
                return 1;
            }

            off += rl;
        }
    }

    /* need a new directory block */
    uint32_t nblk = ext2w_alloc_block();
    if (!nblk) return 0;
    memset(buf, 0, bs);
    WDirEnt *de = (WDirEnt*)buf;
    de->inode = ino;
    de->rec_len = (uint16_t)bs;
    de->name_len = (uint8_t)namelen;
    de->file_type = file_type;
    memcpy(de->name, name, namelen);
    ext2_write_block(nblk, buf);

    /* append block pointer to parent (direct only) */
    if (nb >= 12) { ext2w_free_block(nblk); return 0; }
    parent.block[nb] = nblk;
    parent.size += bs;
    parent.mtime = 0;  /* caller updates time if desired */
    if (!ext2_write_inode(parent_ino, &parent)) return 0;
    return 1;
}

int ext2w_remove_dirent(uint32_t parent_ino, const char *name) {
    if (!ready()) return 0;
    Ext2Inode parent;
    if (!read_inode(parent_ino, &parent)) return 0;

    uint32_t nb = (parent.size + bs - 1) / bs;
    uint8_t buf[4096];

    for (uint32_t b = 0; b < nb && b < 12; b++) {
        uint32_t blk = parent.block[b];
        if (!blk) continue;
        ext2_read_block(blk, buf);

        uint32_t off = 0, prev_off = (uint32_t)-1;
        while (off < bs) {
            WDirEnt *de = (WDirEnt*)(buf + off);
            if (!de->rec_len) break;
            if (de->inode && de->name_len == strlen(name) &&
                !memcmp(de->name, name, de->name_len)) {
                if (prev_off != (uint32_t)-1) {
                    WDirEnt *prev = (WDirEnt*)(buf + prev_off);
                    prev->rec_len = (uint16_t)(prev->rec_len + de->rec_len);
                } else {
                    de->inode = 0;  /* first entry: mark deleted */
                }
                ext2_write_block(blk, buf);
                parent.mtime = 0;
                ext2_write_inode(parent_ino, &parent);
                return 1;
            }
            prev_off = off;
            off += de->rec_len;
        }
    }
    return 0;
}

int ext2w_lookup(uint32_t parent_ino, const char *name,
                 uint32_t *out_ino, uint8_t *out_type) {
    if (!ready()) return 0;
    Ext2Inode parent;
    if (!ext2_read_inode(parent_ino, &parent)) return 0;
    if ((parent.mode & 0xF000) != 0x4000) return 0;

    uint8_t buf[4096];
    uint32_t size = parent.size;
    uint32_t nb = (size + bs - 1) / bs;

    for (uint32_t b = 0; b < nb && b < 12; b++) {
        if (!parent.block[b]) continue;
        ext2_read_block(parent.block[b], buf);
        uint32_t off = 0;
        while (off < bs) {
            WDirEnt *de = (WDirEnt*)(buf + off);
            if (!de->inode || !de->rec_len) break;
            if (de->name_len == strlen(name) &&
                !memcmp(de->name, name, de->name_len)) {
                if (out_ino) *out_ino = de->inode;
                if (out_type) *out_type = de->file_type;
                return 1;
            }
            off += de->rec_len;
        }
    }
    return 0;
}

/* ---- file create / write ---- */

uint32_t ext2w_create(uint32_t parent_ino, const char *name,
                      int is_dir, uint16_t mode) {
    if (!ready()) return 0;
    uint32_t ino = ext2w_alloc_inode();
    if (!ino) return 0;

    Ext2Inode node;
    memset(&node, 0, sizeof(node));
    node.mode = mode;
    node.size = 0;
    node.links_count = is_dir ? 2u : 1u;
    node.mtime = 0;
    node.ctime = 0;

    if (is_dir) {
        uint32_t blk = ext2w_alloc_block();
        if (!blk) { ext2w_free_inode(ino); return 0; }
        uint8_t buf[4096];
        memset(buf, 0, bs);
        WDirEnt *dot = (WDirEnt*)buf;
        dot->inode = ino; dot->rec_len = 12; dot->name_len = 1;
        dot->file_type = 2; dot->name[0] = '.';
        WDirEnt *dotdot = (WDirEnt*)(buf + 12);
        dotdot->inode = parent_ino; dotdot->rec_len = (uint16_t)(bs - 12);
        dotdot->name_len = 2; dotdot->file_type = 2;
        dotdot->name[0] = '.'; dotdot->name[1] = '.';
        ext2_write_block(blk, buf);

        node.size = bs;
        node.block[0] = blk;
        node.blocks = bs / 512;
    }

    if (!ext2_write_inode(ino, &node) ||
        !ext2w_add_dirent(parent_ino, name, ino, is_dir ? 2 : 1)) {
        /* rollback */
        if (is_dir && node.block[0]) ext2w_free_block(node.block[0]);
        ext2w_free_inode(ino);
        return 0;
    }

    Ext2GroupDesc *g = ext2_group0_ptr();
    if (is_dir) g->used_dirs_count++;
    ext2_flush_meta();
    return ino;
}

static void free_inode_blocks(Ext2Inode *node) {
    for (int i = 0; i < 12; i++) {
        if (node->block[i]) { ext2w_free_block(node->block[i]); node->block[i] = 0; }
    }
    if (node->block[12]) {
        uint32_t ptrs_per_block = bs / 4;
        static uint8_t ib[4096];
        ext2_read_block(node->block[12], ib);
        uint32_t *ptrs = (uint32_t*)ib;
        for (uint32_t i = 0; i < ptrs_per_block && i * bs < node->size; i++) {
            if (ptrs[i]) ext2w_free_block(ptrs[i]);
        }
        ext2w_free_block(node->block[12]);
        node->block[12] = 0;
    }
    node->blocks = 0;
}

int ext2w_write_file(uint32_t ino, const uint8_t *buf, uint32_t size) {
    if (!ready()) return 0;
    Ext2Inode node;
    if (!read_inode(ino, &node)) return 0;

    uint32_t ptrs_per_block = bs / 4;
    uint32_t need_blocks = (size + bs - 1) / bs;
    static uint8_t zbuf[4096];

    for (uint32_t b = 0; b < need_blocks; b++) {
        const uint8_t *src = buf + b * bs;
        uint32_t chunk = bs;
        if (b * bs + chunk > size) chunk = size - b * bs;

        uint32_t blk = 0;
        uint8_t ib[4096];
        uint32_t iblk = 0;
        uint32_t idx = 0;

        if (b < 12) {
            blk = node.block[b];
        } else {
            idx = b - 12;
            if (idx >= ptrs_per_block) return 0;
            iblk = node.block[12];
            if (!iblk) {
                iblk = ext2w_alloc_block();
                if (!iblk) return 0;
                memset(zbuf, 0, bs);
                ext2_write_block(iblk, zbuf);
                node.block[12] = iblk;
            }
            ext2_read_block(iblk, ib);
            blk = ((uint32_t*)ib)[idx];
        }

        if (!blk) {
            blk = ext2w_alloc_block();
            if (!blk) return 0;
            if (b < 12) node.block[b] = blk;
            else {
                ext2_read_block(iblk, ib);
                ((uint32_t*)ib)[idx] = blk;
                ext2_write_block(iblk, ib);
            }
        }

        uint8_t wb[4096];
        memcpy(wb, src, chunk);
        if (chunk < bs) memset(wb + chunk, 0, bs - chunk);
        ext2_write_block(blk, wb);
    }

    /* free blocks beyond the new size */
    uint32_t old_blocks = (node.size + bs - 1) / bs;
    for (uint32_t b = need_blocks; b < old_blocks && b < 12; b++) {
        if (node.block[b]) { ext2w_free_block(node.block[b]); node.block[b] = 0; }
    }
    if (need_blocks <= 12 && old_blocks > 12 && node.block[12]) {
        free_inode_blocks(&node);  /* drops indirect chain */
    }

    node.size = size;
    node.blocks = (size + 511) / 512;
    node.mtime = 0;
    return ext2_write_inode(ino, &node);
}

/* ---- mkdir / unlink ---- */

int ext2w_dir_is_empty(uint32_t dir_ino) {
    uint8_t names = 0;
    Ext2Inode d;
    if (!ext2_read_inode(dir_ino, &d)) return 0;
    if ((d.mode & 0xF000) != 0x4000) return 0;

    uint8_t buf[4096];
    uint32_t nb = (d.size + bs - 1) / bs;
    for (uint32_t b = 0; b < nb && b < 12; b++) {
        if (!d.block[b]) continue;
        ext2_read_block(d.block[b], buf);
        uint32_t off = 0;
        while (off < bs) {
            WDirEnt *de = (WDirEnt*)(buf + off);
            if (!de->rec_len) break;
            if (de->inode && de->name_len > 0) {
                if (!(de->name_len == 1 && de->name[0] == '.') &&
                    !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.'))
                    names++;
            }
            off += de->rec_len;
        }
    }
    return names == 0;
}

int ext2w_unlink(uint32_t parent_ino, const char *name) {
    if (!ready()) return 0;
    uint32_t ino;
    uint8_t type;
    if (!ext2w_lookup(parent_ino, name, &ino, &type)) return 0;

    Ext2Inode node;
    if (!ext2_read_inode(ino, &node)) return 0;
    int is_dir = (node.mode & 0xF000) == 0x4000;
    if (is_dir && !ext2w_dir_is_empty(ino)) return 0;

    if (!ext2w_remove_dirent(parent_ino, name)) return 0;

    free_inode_blocks(&node);
    if (node.links_count > 1) node.links_count--;
    if (node.links_count <= 1) {
        ext2w_free_inode(ino);
        if (is_dir) {
            Ext2GroupDesc *g = ext2_group0_ptr();
            if (g->used_dirs_count) g->used_dirs_count--;
        }
    } else {
        ext2_write_inode(ino, &node);
    }
    ext2_flush_meta();
    return 1;
}
