global isr_syscall
extern syscall_handler

isr_syscall:
    pusha
    push ds
    push es
    push fs
    push gs

    mov eax, [esp + 44]   ; syscall number
    mov ebx, [esp + 48]   ; arg1
    mov ecx, [esp + 52]   ; arg2
    mov edx, [esp + 56]   ; arg3

    call syscall_handler

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd
