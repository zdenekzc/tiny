#include <stddef.h>
#include <stdint.h>

void *malloc(size_t n) {
    extern uint8_t __heap_start;
    static uint8_t *p = &__heap_start;
    void *r = p;
    p += n;
    return r;
}

void free(void *p) { (void)p; }

void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *D=d; const uint8_t *S=s;
    while(n--) *D++ = *S++;
    return d;
}

void *memset(void *d, int c, size_t n) {
    uint8_t *D=d;
    while(n--) *D++ = (uint8_t)c;
    return d;
}

size_t strlen(const char *s) {
    size_t n=0; while(*s++) n++; return n;
}
