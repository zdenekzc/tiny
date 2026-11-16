extern int elf_load_image(const uint8_t *file, uint32_t *entry_out);

void run_elf_from_ext2(uint32_t ino) {
    static uint8_t elf_buf[4*1024*1024];
    uint32_t size;

    if (!ext2_read_file(ino, elf_buf, sizeof(elf_buf), &size))
        return;

    uint32_t entry;
    if (elf_load_image(elf_buf, &entry) == 0) {
        void (*entry_fn)(void) = (void(*)(void))entry;
        entry_fn();  /* jumps into loaded ELF */
    }
}
