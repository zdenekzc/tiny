/* Press F5 in Tiny IDE to compile and run this demo. */
#include <stdio.h>

static int square(int x)
{
    return x * x;
}

int main(void)
{
    int sum = 0;
    /* C/C++ syntax: comments, strings, types and numbers are colored. */
    for (int i = 1; i <= 10; ++i) {
        sum += square(i);
        printf("%2d^2 = %3d\n", i, square(i));
    }
    printf("\nsum of squares 1..10 = %d\n", sum);
    return 0;
}
