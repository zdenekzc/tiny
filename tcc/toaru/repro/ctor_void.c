/* constructor + main(void): probehne (srovnavaci priklad, 2026-11-12). */
#include <stdio.h>

static int ran;

__attribute__((constructor)) static void ctor(void) { ran = 1; }

int main(void) {
    printf("ctor_void: ran=%d\n", ran);
    fflush(stdout);
    return 0;
}
