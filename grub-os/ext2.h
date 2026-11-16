#pragma once
#include <stdint.h>

typedef struct {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t reserved_blocks;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mount_count;
    uint16_t max_mount_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t  uuid[16];
    char     volume_name[16];
    char     last_mounted[64];
    uint32_t algo_bitmap;
} __attribute__((packed)) Ext2Super;

typedef struct {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint8_t  reserved[12];
} __attribute__((packed)) Ext2GroupDesc;

typedef struct {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15];
    uint32_t gen;
    uint32_t file_acl;
    uint32_t dir_acl;
    uint32_t faddr;
    uint8_t  osd2[12];
} __attribute__((packed)) Ext2Inode;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed)) Ext2DirEnt;

int  ext2_mount(uint32_t part_lba);
int  ext2_read_inode(uint32_t ino, Ext2Inode *out);
int  ext2_read_file(uint32_t ino, uint8_t *buf, uint32_t maxlen, uint32_t *out_size);
int  ext2_write_file_overwrite(uint32_t ino, const uint8_t *buf, uint32_t size);
int  ext2_list_dir(uint32_t ino,
                   void (*cb)(const char *name, uint8_t type, uint32_t ino, void *user),
                   void *user);
