#include <stdint.h>

extern int  elf_load_user(const uint8_t *file, uint32_t *entry_out);
extern void enter_user_mode(uint32_t entry, uint32_t user_stack_top);
extern uint32_t alloc_user_stack(void);
extern int fs_read_file_by_path(const char *path, uint8_t *buf, uint32_t max, uint32_t *out_size);

int k_exec(const char *path) {
    static uint8_t elf_buf[4*1024*1024];
    uint32_t size;

    if (!fs_read_file_by_path(path, elf_buf, sizeof(elf_buf), &size))
        return -1;

    uint32_t entry;
    if (elf_load_user(elf_buf, &entry) != 0)
        return -2;

    uint32_t ustk = alloc_user_stack();

    enter_user_mode(entry, ustk);

    return 0;  // never reached if exec succeeds
}
