#pragma once
#include <stdint.h>

typedef struct {
    /* ... your existing fields ... */
    uint16_t s_magic;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    /* ... */
} ext2_superblock_t;

int  ext2_mount(uint32_t part_lba);
void ext2_print_features(void);
