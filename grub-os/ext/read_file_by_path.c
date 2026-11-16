int fs_read_file_by_path(const char *path, uint8_t *buf, uint32_t max, uint32_t *out_size) {
    uint32_t ino = ext2_inode_by_path(path);
    if (!ino)
        return 0;

    return ext2_read_file(ino, buf, max, out_size);
}
