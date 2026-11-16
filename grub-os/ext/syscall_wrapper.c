static inline int syscall(int n, int a, int b, int c) {
    int r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c));
    return r;
}
