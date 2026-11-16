.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot
    .long MAGIC
    .long FLAGS
    .long CHECKSUM

.section .text
.global _start
.extern kernel_main
.extern stack_top

_start:
    cli
    mov $stack_top, %esp
    push %ebx
    push %eax
    call kernel_main

halt:
    hlt
    jmp halt
