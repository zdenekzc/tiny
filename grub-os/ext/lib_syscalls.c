#include <stdint.h>

static inline int syscall(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c)
    );
    return ret;
}

int puts(const char *s) {
    return syscall(1, (int)s, 0, 0);
}

int putc(char c) {
    return syscall(2, c, 0, 0);
}
