/* test.c -- what a tcc program has to get right on ToaruOS 2.3.2.
 *
 * Compiled by the cross compiler of mktcc.sh, run inside the guest (README
 * §5i).  Everything here goes through libc of ToaruOS only - no hand written
 * syscalls - so the test covers crt0.o, the constructor array and stdio:
 *
 *   ctor=1                 __attribute__((constructor)) reached .ctors and
 *                          pre_main() ran it (see the init_array hunk in
 *                          tcc-toaru.patch; without it nothing is printed)
 *   argv[0]=...            argv comes from the stack like on Linux
 *   snprintf=tcc/42        vsnprintf in libc
 *   float=7.5              %g with a double
 *   heap=malloc ok         malloc/free from libc, string above e_entry
 *
 * Exit code is 0 only when the constructor really ran.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ctor_ran;

__attribute__((constructor))
static void my_ctor(void)
{
    ++ctor_ran;
}

int main(int argc, char **argv)
{
    char buf[64];
    char *heap;
    int i;

    printf("ctor=%d argc=%d\n", ctor_ran, argc);
    for (i = 0; i < argc; ++i)
        printf("argv[%d]=%s\n", i, argv[i]);

    snprintf(buf, sizeof buf, "%s/%d", "tcc", 42);
    printf("snprintf=%s\n", buf);
    printf("float=%g\n", 2.5 * 3);

    heap = malloc(24);
    if (!heap)
        return 2;
    strcpy(heap, "malloc ok");
    printf("heap=%s\n", heap);
    free(heap);

    return ctor_ran == 1 ? 0 : 1;
}
