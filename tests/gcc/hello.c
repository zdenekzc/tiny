/* hello.c — built on the HOST by gcc-toaru.sh, not by TinyCC.
 *
 * tests/run-toaru.sh builds it into /share/gcc_hello and the control VM runs
 * it: that verifies the host gcc -> GNU ld (-Ttext-segment) -> toaru-elfpad.py
 * route, whose only ToaruOS-specific parts are the headers, crt0.o/libc.a
 * from the ISO and the image layout.  It also means the guest can tell the
 * two builds apart by the text below.
 *
 *   ./gcc-toaru.sh -o /tmp/gcc_hello tests/gcc/hello.c
 *   /tmp/gcc_hello
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char *who = malloc(16);
    int i, sum = 0;

    if (!who)
        return 1;

    strcpy(who, "host gcc");
    for (i = 1; i <= 5; ++i)
        sum += i;

    printf("gcc_toaru: %s\n", who);
    printf("gcc_sum: %d\n", sum);

    free(who);
    return 0;
}
