/* user_stack.c */

#include <stdint.h>

#define USER_STACK_REGION_TOP   0xBFFF0000
#define USER_STACK_SIZE         0x4000   /* 16 KB per stack */

static uint32_t next_stack_top = USER_STACK_REGION_TOP;

uint32_t alloc_user_stack(void) {
    uint32_t top = next_stack_top;
    next_stack_top -= USER_STACK_SIZE;
    return top;
}
