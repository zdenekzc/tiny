#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

typedef void (*entry_t)(uint32_t magic, void *mb_info);

void chainload_multiboot_elf(void *image, uint32_t size, uint32_t mb_magic, void *mb_info) {
    (void)size;
    Elf32_Ehdr *eh = (Elf32_Ehdr*)image;
    Elf32_Phdr *ph = (Elf32_Phdr*)((uint8_t*)image + eh->e_phoff);

    for (int i=0;i<eh->e_phnum;i++) {
        if (ph[i].p_type != 1) continue; // PT_LOAD
        memcpy((void*)ph[i].p_paddr, (uint8_t*)image + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz) {
            memset((uint8_t*)ph[i].p_paddr + ph[i].p_filesz, 0, ph[i].p_memsz - ph[i].p_filesz);
        }
    }

    entry_t entry = (entry_t)eh->e_entry;
    entry(mb_magic, mb_info);
}
