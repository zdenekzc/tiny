extern uint32_t alloc_user_stack(void);
extern void enter_user_mode(uint32_t entry, uint32_t user_stack_top);

void run_user_elf(const uint8_t *elf, uint32_t size) {
    uint32_t entry;
    if (elf_load_user(elf, &entry) != 0)
        return;

    uint32_t ustk = alloc_user_stack();
    enter_user_mode(entry, ustk);
}
