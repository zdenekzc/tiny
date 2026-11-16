#include "ext2.h"
#include "ata.h"
#include "vga.h"

static ext2_superblock_t sb;

#define EXT2_SUPER_MAGIC 0xEF53

/* EXT3/EXT2 feature flags we’re OK with ignoring for now */
#define EXT2_FEATURE_INCOMPAT_FILETYPE  0x0002
#define EXT2_FEATURE_INCOMPAT_RECOVER   0x0004
#define EXT2_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008

int ext2_mount(uint32_t part_lba) {
    uint8_t buf[1024];

    /* superblock is at offset 1024 bytes from start of FS */
    ata_read28(part_lba + 2, buf); /* adjust if your read is by sector */
    memcpy(&sb, buf, sizeof(sb));

    if (sb.s_magic != EXT2_SUPER_MAGIC)
        return 0;

    /* Level 1: accept EXT2/EXT3, just reject truly unknown incompatible bits */
    uint32_t allowed_incompat =
        EXT2_FEATURE_INCOMPAT_FILETYPE |
        EXT2_FEATURE_INCOMPAT_RECOVER  |
        EXT2_FEATURE_INCOMPAT_JOURNAL_DEV;

    if (sb.s_feature_incompat & ~allowed_incompat)
        return 0;

    return 1;
}

void ext2_print_features(void) {
    vga_puts_at("EXT2/3 features:", 0, 3, 0x0F);

    char line[80];

    snprintf(line, sizeof(line), "compat:    0x%08x", sb.s_feature_compat);
    vga_puts_at(line, 0, 4, 0x0F);

    snprintf(line, sizeof(line), "ro_compat: 0x%08x", sb.s_feature_ro_compat);
    vga_puts_at(line, 0, 5, 0x0F);

    snprintf(line, sizeof(line), "incompat:  0x%08x", sb.s_feature_incompat);
    vga_puts_at(line, 0, 6, 0x0F);
}
