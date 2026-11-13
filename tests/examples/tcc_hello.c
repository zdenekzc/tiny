/* tcc_hello.c -- a small plain-C program compiled by tcc inside ToaruOS.
 *
 * All files in this directory are plain C and TinyCC compiles them.  This one
 * verifies that tcc inside ToaruOS really compiles, links and runs a program:
 *
 *   /share/tcc/bin/tcc -o /share/tcc_hello /share/tcc_hello.c
 *   /share/tcc_hello
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    int i, sum = 0;
    char *p = malloc(32);

    for (i = 1; i <= 10; ++i)
        sum += i;

    strcpy(p, "TinyCC inside ToaruOS");
    printf("%s\n", p);
    printf("1+2+...+10 = %d, argc = %d\n", sum, argc);
    if (argc > 1)
        printf("argv[1] = %s\n", argv[1]);
    free(p);
    return 0;
}
