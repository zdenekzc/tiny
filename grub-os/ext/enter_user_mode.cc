void enter_user_mode(uint32_t entry) {
    asm volatile(
        "cli;"
        "mov $0x23, %ax;"
        "mov %ax, %ds;"
        "mov %ax, %es;"
        "mov %ax, %fs;"
        "mov %ax, %gs;"

        "mov %esp, %eax;"
        "pushl $0x23;"
        "pushl %eax;"
        "pushf;"
        "pushl $0x1B;"
        "pushl %0;"
        "iret"
        :
        : "r"(entry)
    );
}
