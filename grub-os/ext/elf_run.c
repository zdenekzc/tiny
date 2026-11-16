#include "elf.h"

extern int  elf_load_user(const uint8_t *file, uint32_t *entry_out);
extern void enter_user_mode(uint32_t entry, uint32_t user_stack_top);

void run_user_elf_from_ext2(uint32_t ino) {
    static uint8_t elf_buf[4*1024*1024];
    uint32_t size;

    if (!ext2_read_file(ino, elf_buf, sizeof(elf_buf), &size))
        return;

    uint32_t entry;
    if (elf_load_user(elf_buf, &entry) != 0)
        return;

    uint32_t user_stack_top = 0xBFFFF000;  // pick a high virtual address
    enter_user_mode(entry, user_stack_top);
}
