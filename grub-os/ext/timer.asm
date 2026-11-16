global isr_timer_stub
extern timer_handler

isr_timer_stub:
    pusha
    call timer_handler
    popa
    mov al, 0x20
    out 0x20, al
    iretd
