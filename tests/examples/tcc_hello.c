/* tcc_hello.c -- the only example in this directory that is really C.
 *
 * The other files (avl.c, btree.c, queens.c, ...) are C++: they use
 * constructors, references and "const int N" as an array size.  TinyCC has
 * only a C frontend, so it will not compile them - in ToaruOS they serve for
 * viewing and editing in mini-commander.  This file verifies that tcc inside
 * ToaruOS really compiles, links and runs a program:
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
