/* hello.c -- smallest program that proves the whole chain works: tcc compiles
 * and links against ToaruOS libc, the kernel loads the image and stdio works
 * (which it only does when the constructor array reached pre_main(), see
 * tcc-toaru.patch).  Expected output, in the guest and on Linux:
 *
 *   tcc v ToaruOS: 2 + 3 = 5, OK
 */
#include <stdio.h>
#include <string.h>

int main(void) {
    char buf[64];
    strcpy(buf, "tcc v ToaruOS");
    printf("%s: %d + %d = %d, %s\n", buf, 2, 3, 2 + 3, "OK");
    return 0;
}
