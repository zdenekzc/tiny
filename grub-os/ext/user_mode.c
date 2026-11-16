#include <stdint.h>
#include "gdt.h"

#define USER_CS 0x1B
#define USER_DS 0x23

void enter_user_mode(uint32_t entry, uint32_t user_stack_top) {
    __asm__ volatile (
        "cli\n"
        "mov %0, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"

        "pushl %0\n"              // SS = USER_DS
        "pushl %1\n"              // ESP = user_stack_top
        "pushfl\n"                // EFLAGS
        "pushl %2\n"              // CS = USER_CS
        "pushl %3\n"              // EIP = entry
        "iret\n"
        :
        : "i"(USER_DS), "r"(user_stack_top), "i"(USER_CS), "r"(entry)
        : "ax"
    );
}
