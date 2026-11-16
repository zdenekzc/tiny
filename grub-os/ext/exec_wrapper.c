#define SYS_EXEC 5

int exec(const char *path) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_EXEC), "b"(path));
    return r;
}
