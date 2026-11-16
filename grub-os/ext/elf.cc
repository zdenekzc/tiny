uint32_t load_elf(const uint8_t *data) {
    Elf32_Ehdr *eh = (Elf32_Ehdr*)data;
    Elf32_Phdr *ph = (Elf32_Phdr*)(data + eh->e_phoff);

    for (int i=0;i<eh->e_phnum;i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        memcpy((void*)ph[i].p_vaddr, data + ph[i].p_offset, ph[i].p_filesz);
        memset((void*)(ph[i].p_vaddr + ph[i].p_filesz), 0,
               ph[i].p_memsz - ph[i].p_filesz);
    }

    return eh->e_entry;
}
