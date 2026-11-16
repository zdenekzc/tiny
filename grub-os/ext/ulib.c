#include <stdint.h>
#include <stdarg.h>

#define SYS_WRITE 1
#define SYS_READ  2
#define SYS_SBRK  3
#define SYS_EXIT  4
#define SYS_EXEC 5

static inline int syscall1(int n, int a) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "b"(a));
    return r;
}

int exec(const char *path) {
    return syscall1(SYS_EXEC, (int)path);
}

static inline int syscall3(int n, int a, int b, int c) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "b"(a), "c"(b), "d"(c));
    return r;
}

int write(int fd, const void *buf, int len) {
    (void)fd; // only stdout for now
    return syscall3(SYS_WRITE, (int)buf, len, 0);
}

int read(int fd, void *buf, int len) {
    (void)fd; // only stdin for now
    return syscall3(SYS_READ, (int)buf, len, 0);
}

void exit(int code) {
    syscall3(SYS_EXIT, code, 0, 0);
    while (1) {}
}

/* simple strlen */

int strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

int puts(const char *s) {
    int n = strlen(s);
    write(1, s, n);
    write(1, "\n", 1);
    return n+1;
}

/* very small printf: supports %s and %d only */

static void print_dec(int v) {
    char buf[16];
    int i = 0;
    if (v == 0) {
        write(1, "0", 1);
        return;
    }
    if (v < 0) {
        write(1, "-", 1);
        v = -v;
    }
    while (v > 0 && i < 15) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }
    while (i--) write(1, &buf[i], 1);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            write(1, fmt, 1);
            count++;
            continue;
        }
        fmt++;
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char*);
            int n = strlen(s);
            write(1, s, n);
            count += n;
        } else if (*fmt == 'd') {
            int v = va_arg(ap, int);
            print_dec(v);
            /* rough count, not exact */
        } else {
            write(1, fmt, 1);
            count++;
        }
    }

    va_end(ap);
    return count;
}

/* sbrk + malloc */

static char *brk_end = 0;

void *sbrk(int inc) {
    int old = syscall3(SYS_SBRK, inc, 0, 0);
    if (old == -1) return (void*)-1;
    if (!brk_end) brk_end = (char*)old;
    char *prev = brk_end;
    brk_end += inc;
    return prev;
}

void *malloc(int size) {
    if (size <= 0) return 0;
    return sbrk((size + 7) & ~7);
}

void free(void *p) {
    (void)p; /* no free in this tiny libc */
}

/* simple line input */

int readline(char *buf, int maxlen) {
    int n = 0;
    while (n < maxlen-1) {
        char c;
        int r = read(0, &c, 1);
        if (r <= 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            buf[n] = 0;
            write(1, "\n", 1);
            return n;
        }
        if (c == '\b' || c == 127) {
            if (n > 0) {
                n--;
                write(1, "\b \b", 3);
            }
            continue;
        }
        buf[n++] = c;
        write(1, &c, 1);
    }
    buf[n] = 0;
    return n;
}
