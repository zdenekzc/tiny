global context_switch

; void context_switch(uint32_t *old_esp, uint32_t *old_ebp,
;                     uint32_t new_esp, uint32_t new_ebp, uint32_t new_eip);

context_switch:
    pusha

    mov eax, [esp + 36]    ; old_esp*
    mov edx, [esp + 40]    ; old_ebp*
    mov [eax], esp
    mov [edx], ebp

    mov eax, [esp + 44]    ; new_esp
    mov ebp, [esp + 48]    ; new_ebp
    mov edx, [esp + 52]    ; new_eip

    mov esp, eax
    mov eax, edx

    popa
    jmp eax
