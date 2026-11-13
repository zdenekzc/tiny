/* constructor + main(argc): passes.  The measurement of 2026-11-12 made a fatal
 * combination out of this and described it in ../README.md — it had nothing to do
 * with the constructor or with argc. What was killing was test.c, whose .bss reaches
 * past the page boundary; that crash is analyzed in ../README.md and measured here
 * by bss_page.c. */
#include <stdio.h>

static int ran;

__attribute__((constructor)) static void ctor(void) { ran = 1; }

int main(int argc, char **argv) {
    printf("ctor_argc: argc=%d\n", argc);
    fflush(stdout);
    return 0;
}
