int write(const char *s, int len) {
    return syscall(SYS_WRITE, (int)s, len, 0);
}

int puts(const char *s) {
    return write(s, strlen(s));
}

void exit(int code) {
    syscall(SYS_EXIT, code, 0, 0);
}
