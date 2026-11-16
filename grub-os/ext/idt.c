extern void isr_syscall();  // assembly stub

void idt_init(void) {
    idt_set_gate(0x80, (uint32_t)isr_syscall, 0x08, 0xEE);
}
