#include "elf.h"
#include <string.h>
#include <stdint.h>

int elf_load_user(const uint8_t *file, uint32_t *entry_out) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr*)file;

    if (eh->e_ident[0] != 0x7F ||
        eh->e_ident[1] != 'E'  ||
        eh->e_ident[2] != 'L'  ||
        eh->e_ident[3] != 'F')
        return -1;

    if (eh->e_machine != 3)   /* EM_386 */
        return -2;

    const Elf32_Phdr *ph = (const Elf32_Phdr*)(file + eh->e_phoff);

    for (int i=0; i<eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;

        uint8_t *dest = (uint8_t*)ph[i].p_vaddr;
        const uint8_t *src = file + ph[i].p_offset;

        memcpy(dest, src, ph[i].p_filesz);

        if (ph[i].p_memsz > ph[i].p_filesz) {
            memset(dest + ph[i].p_filesz, 0,
                   ph[i].p_memsz - ph[i].p_filesz);
        }
    }

    *entry_out = eh->e_entry;
    return 0;
}
