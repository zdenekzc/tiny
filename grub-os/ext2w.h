#pragma once
#include <stdint.h>
#include <stddef.h>

/* ext2 write layer: block/inode allocation, directory entry
 * manipulation, file creation/writing, mkdir, unlink.
 * Supports the on-disk layout produced by tests/test-grub.sh
 * (block size 1024, single block group, 128-byte inodes,
 * direct + single-indirect blocks, no extents). */

uint32_t ext2w_alloc_block(void);
void     ext2w_free_block(uint32_t blk);
uint32_t ext2w_alloc_inode(void);
void     ext2w_free_inode(uint32_t ino);

/* create a file (is_dir=0) or directory (is_dir=1) in parent,
 * returns new inode number or 0 on failure */
uint32_t ext2w_create(uint32_t parent_ino, const char *name,
                      int is_dir, uint16_t mode);

/* add / remove a directory entry in parent */
int ext2w_add_dirent(uint32_t parent_ino, const char *name,
                     uint32_t ino, uint8_t file_type);
int ext2w_remove_dirent(uint32_t parent_ino, const char *name);

/* write buf into inode, allocating/freeing blocks as needed
 * (direct blocks and single indirect). returns 1 on success */
int ext2w_write_file(uint32_t ino, const uint8_t *buf, uint32_t size);

/* look up name in parent; returns inode and type via out params */
int ext2w_lookup(uint32_t parent_ino, const char *name,
                 uint32_t *out_ino, uint8_t *out_type);

/* delete name from parent (file or empty directory) */
int ext2w_unlink(uint32_t parent_ino, const char *name);

/* does the directory contain any entries besides "." and ".."? */
int ext2w_dir_is_empty(uint32_t dir_ino);
